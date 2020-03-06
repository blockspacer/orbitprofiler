#include "PerfEventProcessor.h"

#include <queue>

#include "Logging.h"
#include "Utils.h"
#include "ScopeTimer.h"

namespace LinuxTracing {

void PerfEventProcessor::AddEvent(int origin_fd,
                                  std::unique_ptr<PerfEvent> event) {
  SCOPE_TIMER_INTROSPECTION_FUNC;
  if (last_processed_timestamp_ > 0 &&
      event->Timestamp() <
          last_processed_timestamp_ - PROCESSING_DELAY_MS * 1'000'000) {
    ERROR("Processed an event out of order");
  }

  event_queue_.push(std::move(event));
}

void PerfEventProcessor::ProcessAllEvents() {
  SCOPE_TIMER_INTROSPECTION_FUNC;
  while (!event_queue_.empty()) {
    PerfEvent* event = event_queue_.top().get();
    event->accept(visitor_.get());

    last_processed_timestamp_ = event->Timestamp();

    event_queue_.pop();
  }
}

void PerfEventProcessor::ProcessOldEvents() {
  SCOPE_TIMER_INTROSPECTION_FUNC;
  uint64_t max_timestamp = MonotonicTimestampNs();

  while (!event_queue_.empty()) {
    PerfEvent* event = event_queue_.top().get();

    // Do not read the most recent events are out-of-order events could arrive.
    if (event->Timestamp() + PROCESSING_DELAY_MS * 1'000'000 >= max_timestamp) {
      break;
    }
    event->accept(visitor_.get());

    last_processed_timestamp_ = event->Timestamp();

    event_queue_.pop();
  }
}

}  // namespace LinuxTracing
