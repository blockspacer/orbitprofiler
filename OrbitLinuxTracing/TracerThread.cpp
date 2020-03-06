#include "TracerThread.h"

#include "Logging.h"
#include "PerfEventProcessor.h"
#include "PerfEventProcessor2.h"
#include "PerfEventRingBuffer.h"
#include "ScopeTimer.h"
#include "UprobesUnwindingVisitor.h"
#include "Utils.h"
#include "absl/container/flat_hash_map.h"

namespace LinuxTracing {

struct TracerThreadData {
  absl::flat_hash_map<int32_t, PerfEventRingBuffer> fds_to_ring_buffer;
  absl::flat_hash_map<pid_t, int32_t> threads_to_fd;
  absl::flat_hash_map<int32_t, const Function*> uprobe_fds_to_function;
  absl::flat_hash_map<int32_t, const Function*> uretprobe_fds_to_function;
};


// TODO: Refactor this huge method.
void TracerThread::Run(
    const std::shared_ptr<std::atomic<bool>>& exit_requested) {

  data_ = std::make_shared<TracerThreadData>();

  // perf_event_open refers to cores as "CPUs".
  int32_t num_cpus = GetNumCores();

  if (trace_context_switches_) {
    // Record context switches from all cores for all processes.
    for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
      int32_t context_switch_fd = cpu_context_switch_event_open(cpu);
      data_->fds_to_ring_buffer.emplace(context_switch_fd,
                                 PerfEventRingBuffer{context_switch_fd});
    }
  }

  auto uprobes_unwinding_visitor =
      std::make_unique<UprobesUnwindingVisitor>(ReadMaps(pid_));
  uprobes_unwinding_visitor->SetListener(listener_);
  // Switch between PerfEventProcessor and PerfEventProcessor2 here.
  // PerfEventProcessor2 is supposedly faster but assumes that events from
  // the same perf_event_open ring buffer are already sorted.
  PerfEventProcessor uprobes_event_processor{
      std::move(uprobes_unwinding_visitor)};

  if (trace_instrumented_functions_) {
    for (const auto& function : instrumented_functions_) {
      for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
        int uprobe_fd = uprobe_stack_event_open(function.BinaryPath().c_str(),
                                                function.FileOffset(), -1, cpu);
        data_->fds_to_ring_buffer.emplace(uprobe_fd, PerfEventRingBuffer{uprobe_fd});
        data_->uprobe_fds_to_function.emplace(uprobe_fd, &function);

        int uretprobe_fd = uretprobe_stack_event_open(
            function.BinaryPath().c_str(), function.FileOffset(), -1, cpu);
        data_->fds_to_ring_buffer.emplace(uretprobe_fd,
                                   PerfEventRingBuffer{uretprobe_fd});
        data_->uretprobe_fds_to_function.emplace(uretprobe_fd, &function);
      }
    }
  }

  // TODO(b/148209993): Consider sampling based on CPU and filter by pid.
  for (pid_t tid : ListThreads(pid_)) {
    // Keep threads in sync.
    if (listener_ != nullptr) {
      listener_->OnTid(tid);
    }

    if (trace_callstacks_) {
      int sampling_fd = sample_mmap_task_event_open(tid, sampling_period_ns_);
      data_->fds_to_ring_buffer.emplace(sampling_fd, PerfEventRingBuffer{sampling_fd});
      data_->threads_to_fd.emplace(tid, sampling_fd);
    }
  }

  // TODO: New threads might spawn here before forks are started to be recorded.
  //  Consider also polling threads regularly.

  // Start recording events.
  for (const auto& fd_to_ring_buffer : data_->fds_to_ring_buffer) {
    perf_event_enable(fd_to_ring_buffer.first);
  }

  // Record and periodically print basic statistics on the number events.
  constexpr uint64_t EVENT_COUNT_WINDOW_S = 5;
  uint64_t event_count_window_begin_ns = 0;
  uint64_t sched_switch_count = 0;
  uint64_t sample_count = 0;
  uint64_t uprobes_count = 0;

  bool last_iteration_saw_events = false;

  std::thread stats_thread(&TracerThread::RunStats, this, exit_requested);

  while (!(*exit_requested)) {
    // Wait if there was no new event in the last iteration, so that we are not
    // constantly polling the buffers. 10 ms are still small enough to not have
    // our buffers overflow and therefore lose events.
    // TODO: Refine this sleeping pattern, possibly using exponential backoff.
    if (!last_iteration_saw_events) {
      //SCOPE_TIMER_INTROSPECTION("SLEEP");
      usleep(1'000);
    }

    last_iteration_saw_events = false;

    std::vector<std::pair<int32_t, PerfEventRingBuffer>>
        fds_to_ring_buffer_to_add;
    std::vector<int32_t> fds_to_remove;

    // Read and process events from all ring buffers. In order to ensure that no
    // buffer is read constantly while others overflow, we schedule the reading
    // using round-robin like scheduling.
    for (auto& fd_to_ring_buffer : data_->fds_to_ring_buffer) {
      if (*exit_requested) {
        break;
      }

      const int32_t& fd = fd_to_ring_buffer.first;
      PerfEventRingBuffer& ring_buffer = fd_to_ring_buffer.second;

      bool is_uprobes = data_->uprobe_fds_to_function.count(fd) > 0;
      bool is_uretprobes = data_->uretprobe_fds_to_function.count(fd) > 0;

      int32_t read_from_this_buffer = 0;
      // Read up to ROUND_ROBIN_BATCH_SIZE (5) new events.
      // TODO: Some event types (e.g., stack samples) have a much longer
      //  processing time but are less frequent than others (e.g., context
      //  switches). Take this into account in our scheduling algorithm.
      while (ring_buffer.HasNewData() &&
             read_from_this_buffer < ROUND_ROBIN_BATCH_SIZE) {
        if (*exit_requested) {
          break;
        }

        read_from_this_buffer++;
        last_iteration_saw_events = true;
        perf_event_header header{};
        ring_buffer.ReadHeader(&header);

        // perf_event_header::type contains the type of record, e.g.,
        // PERF_RECORD_SAMPLE, PERF_RECORD_MMAP, etc., defined in enum
        // perf_event_type in perf_event.h.
        switch (header.type) {
          // non system-wide profiling:
          case PERF_RECORD_SWITCH: {
            auto event =
                ring_buffer.ConsumeRecord<ContextSwitchPerfEvent>(header);
            if (event.IsSwitchOut()) {
              ContextSwitchOut context_switch_out{
                  event.TID(), static_cast<uint16_t>(event.CPU()),
                  event.Timestamp()};
              if (listener_ != nullptr) {
                listener_->OnContextSwitchOut(context_switch_out);
              }
            } else {
              ContextSwitchIn context_switch_in{
                  event.TID(), static_cast<uint16_t>(event.CPU()),
                  event.Timestamp()};
              if (listener_ != nullptr) {
                listener_->OnContextSwitchIn(context_switch_in);
              }
            }
            ++sched_switch_count;
          } break;

          // system-wide profiling
          case PERF_RECORD_SWITCH_CPU_WIDE: {
            auto event =
                ring_buffer.ConsumeRecord<SystemWideContextSwitchPerfEvent>(
                    header);
            if (event.PrevTID() != 0) {
              ContextSwitchOut context_switch_out{
                  event.PrevTID(), static_cast<uint16_t>(event.CPU()),
                  event.Timestamp()};
              if (listener_ != nullptr) {
                listener_->OnContextSwitchOut(context_switch_out);
              }
            }
            if (event.NextTID() != 0) {
              ContextSwitchIn context_switch_in{
                  event.NextTID(), static_cast<uint16_t>(event.CPU()),
                  event.Timestamp()};
              if (listener_ != nullptr) {
                listener_->OnContextSwitchIn(context_switch_in);
              }
            }

            ++sched_switch_count;
          } break;

          case PERF_RECORD_FORK: {
            auto fork = ring_buffer.ConsumeRecord<ForkPerfEvent>(header);

            if (fork.PID() == pid_) {
              // A new thread of the sampled process was spawned.
              int32_t sample_fd =
                  sample_mmap_task_event_open(fork.TID(), sampling_period_ns_);
              perf_event_enable(sample_fd);
              // Do not add a new ring buffer to fds_to_ring_buffer here as we
              // are already iterating over fds_to_ring_buffer.
              fds_to_ring_buffer_to_add.emplace_back(
                  sample_fd, PerfEventRingBuffer{sample_fd});
              data_->threads_to_fd.emplace(fork.TID(), sample_fd);
            }
          } break;

          case PERF_RECORD_EXIT: {
            auto exit = ring_buffer.ConsumeRecord<ForkPerfEvent>(header);
            if (exit.PID() == pid_) {
              if (data_->threads_to_fd.count(exit.TID()) > 0) {
                int32_t sample_fd = data_->threads_to_fd.at(exit.TID());
                perf_event_disable(sample_fd);
                close(sample_fd);
                // Do not remove the ring buffer from fds_to_ring_buffer here as
                // we are already iterating over fds_to_ring_buffer.
                fds_to_remove.push_back(sample_fd);
                data_->threads_to_fd.erase(sample_fd);
              }
            }
          } break;

          case PERF_RECORD_MMAP: {
            // There was a call to mmap with PROT_EXEC, hence refresh the maps.
            // This should happen rarely.
            ring_buffer.SkipRecord(header);
            uprobes_event_processor.AddEvent(
                fd, std::make_unique<MapsPerfEvent>(MonotonicTimestampNs(),
                                                    ReadMaps(pid_)));
          } break;

          case PERF_RECORD_SAMPLE: {
            if (is_uprobes) {
              SCOPE_TIMER_INTROSPECTION("uprobe consume");
              auto sample =
                  ring_buffer.ConsumeRecord<UprobePerfEventWithStack>(header);
              sample.SetFunction(data_->uprobe_fds_to_function.at(fd));
              uprobes_event_processor.AddEvent(
                  fd, std::make_unique<UprobePerfEventWithStack>(
                          std::move(sample)));

              ++uprobes_count;

            } else if (is_uretprobes) {
              SCOPE_TIMER_INTROSPECTION("uretprobe consume");
              auto sample =
                  ring_buffer.ConsumeRecord<UretprobePerfEventWithStack>(
                      header);
              sample.SetFunction(data_->uretprobe_fds_to_function.at(fd));
              uprobes_event_processor.AddEvent(
                  fd, std::make_unique<UretprobePerfEventWithStack>(
                          std::move(sample)));

              ++uprobes_count;

            } else {
              SCOPE_TIMER_INTROSPECTION("StackSamplePerfEvent");
              StackSamplePerfEvent sample = ring_buffer.ConsumeRecord<StackSamplePerfEvent>(header);
              uprobes_event_processor.AddEvent(
                  fd,
                  std::make_unique<StackSamplePerfEvent>(std::move(sample)));

              ++sample_count;
            }
          } break;

          case PERF_RECORD_LOST: {
            auto lost = ring_buffer.ConsumeRecord<LostPerfEvent>(header);
            LOG("Lost %lu events", lost.Lost());
          } break;

          default: {
            ERROR("Unexpected perf_event_header::type: %u", header.type);
            ring_buffer.SkipRecord(header);
          } break;
        }

        if (event_count_window_begin_ns == 0) {
          event_count_window_begin_ns = MonotonicTimestampNs();
        } else if (event_count_window_begin_ns +
                       EVENT_COUNT_WINDOW_S * 1'000'000'000 <
                   MonotonicTimestampNs()) {
          LOG("Events per second (last %lu s): "
              "sched switches: %lu; "
              "samples: %lu; "
              "u(ret)probes: %lu",
              EVENT_COUNT_WINDOW_S, sched_switch_count / EVENT_COUNT_WINDOW_S,
              sample_count / EVENT_COUNT_WINDOW_S,
              uprobes_count / EVENT_COUNT_WINDOW_S);
          sched_switch_count = 0;
          sample_count = 0;
          uprobes_count = 0;
          event_count_window_begin_ns = MonotonicTimestampNs();
        }
      }
    }

    uprobes_event_processor.ProcessOldEvents();

    for (auto& fd_to_ring_buffer_to_add : fds_to_ring_buffer_to_add) {
      data_->fds_to_ring_buffer.emplace(std::move(fd_to_ring_buffer_to_add));
    }
    for (int32_t fd_to_remove : fds_to_remove) {
      data_->fds_to_ring_buffer.erase(fd_to_remove);
    }
  }

  uprobes_event_processor.ProcessAllEvents();

  stats_thread.join();
  // Stop recording and close the file descriptors.
  for (auto& fd_to_ring_buffer : data_->fds_to_ring_buffer) {
    const int32_t& fd = fd_to_ring_buffer.first;
    perf_event_disable(fd);
    close(fd);
  }
  data_->fds_to_ring_buffer.clear();
}

void TracerThread::RunStats(const std::shared_ptr<std::atomic<bool>>& exit_requested) {
  while (!(*exit_requested)) {
    usleep(5000);

    // uprobe_fds_to_function;
    // uretprobe_fds_to_function;

    for( const auto& pair : data_->uprobe_fds_to_function ) {
      auto fd = pair.first;
      const Function* function = pair.second;
      const PerfEventRingBuffer& ring_buffer = data_->fds_to_ring_buffer.at(fd);
      auto size = ring_buffer.GetCurrentSize();
      if(size == 0) continue;
      std::cout << "Ring buffer uprobe    " << fd << " size: "
                << size /*<< function->BinaryPath()*/ << std::endl;
    }

    for( const auto& pair : data_->uretprobe_fds_to_function ) {
      auto fd = pair.first;
      const Function* function = pair.second;
      const PerfEventRingBuffer& ring_buffer = data_->fds_to_ring_buffer.at(fd);
      auto size = ring_buffer.GetCurrentSize();
      if(size == 0) continue;
      std::cout << "Ring buffer uretprobe " << fd << " size: "
                << size /*<< function->BinaryPath()*/ << std::endl;
    }
  }
}

}  // namespace LinuxTracing
