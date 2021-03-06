//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "ConnectionManager.h"

#include "Capture.h"
#include "ContextSwitch.h"
#include "CoreApp.h"
#include "EventBuffer.h"
#include "LinuxCallstackEvent.h"
#include "LinuxSymbol.h"
#include "OrbitFunction.h"
#include "OrbitModule.h"
#include "Params.h"
#include "ProcessUtils.h"
#include "SamplingProfiler.h"
#include "Serialization.h"
#include "TcpClient.h"
#include "TcpServer.h"
#include "TestRemoteMessages.h"
#include "TimerManager.h"

#if __linux__
#include "LinuxUtils.h"
#endif

#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>

//-----------------------------------------------------------------------------
ConnectionManager::ConnectionManager()
    : m_ExitRequested(false), m_IsService(false) {}

//-----------------------------------------------------------------------------
ConnectionManager::~ConnectionManager() { TerminateThread(); }

//-----------------------------------------------------------------------------
void ConnectionManager::TerminateThread() {
  if (m_Thread) {
    m_ExitRequested = true;
    m_Thread->join();
    m_Thread = nullptr;
  }
}

//-----------------------------------------------------------------------------
ConnectionManager& ConnectionManager::Get() {
  static ConnectionManager instance;
  return instance;
}

//-----------------------------------------------------------------------------
void ConnectionManager::ConnectToRemote(std::string a_RemoteAddress) {
  m_RemoteAddress = a_RemoteAddress;
  TerminateThread();
  SetupClientCallbacks();
  m_Thread =
      std::make_unique<std::thread>(&ConnectionManager::ConnectionThread, this);
}

//-----------------------------------------------------------------------------
void ConnectionManager::InitAsService() {
#if __linux__
  GParams.m_TrackContextSwitches = true;
#endif

  m_IsService = true;
  SetupServerCallbacks();
  m_Thread =
      std::make_unique<std::thread>(&ConnectionManager::RemoteThread, this);
}

//-----------------------------------------------------------------------------
void ConnectionManager::SetSelectedFunctionsOnRemote(const Message& a_Msg) {
  PRINT_FUNC;
  const char* a_Data = a_Msg.GetData();
  size_t a_Size = a_Msg.m_Size;
  std::istringstream buffer(std::string(a_Data, a_Size));
  cereal::JSONInputArchive inputAr(buffer);
  std::vector<std::string> selectedFunctions;
  inputAr(selectedFunctions);

  // Unselect the all currently selected functions:
  std::vector<Function*> prevSelectedFuncs;
  for (auto& pair : Capture::GSelectedFunctionsMap) {
    prevSelectedFuncs.push_back(pair.second);
  }

  Capture::GSelectedFunctionsMap.clear();
  for (Function* function : prevSelectedFuncs) {
    function->UnSelect();
  }

  // Select the received functions:
  for (const std::string& address_str : selectedFunctions) {
    uint64_t address = std::stoll(address_str);
    PRINT("Select address %x\n", address);
    Function* function =
        Capture::GTargetProcess->GetFunctionFromAddress(address);
    if (!function)
      PRINT("Received invalid address %x\n", address);
    else {
      PRINT("Received Selected Function: %s\n", function->PrettyName().c_str());
      // this also adds the function to the map.
      function->Select();
    }
  }
}

//-----------------------------------------------------------------------------
void ConnectionManager::StartCaptureAsRemote(uint32_t pid) {
  PRINT_FUNC;
  std::shared_ptr<Process> process = process_list_.GetProcess(pid);
  if (!process) {
    PRINT("Process not found (pid=%d)\n", pid);
    return;
  }
  Capture::SetTargetProcess(process);
  Capture::StartCapture();
  GCoreApp->StartRemoteCaptureBufferingThread();
}

//-----------------------------------------------------------------------------
void ConnectionManager::StopCaptureAsRemote() {
  PRINT_FUNC;
  Capture::StopCapture();
  GCoreApp->StopRemoteCaptureBufferingThread();
}

//-----------------------------------------------------------------------------
void ConnectionManager::Stop() { m_ExitRequested = true; }

//-----------------------------------------------------------------------------
void ConnectionManager::SetupServerCallbacks() {
  GTcpServer->AddMainThreadCallback(
      Msg_RemoteSelectedFunctionsMap,
      [this](const Message& a_Msg) { SetSelectedFunctionsOnRemote(a_Msg); });

  GTcpServer->AddMainThreadCallback(
      Msg_StartCapture, [this](const Message& msg) {
        uint32_t pid = static_cast<uint32_t>(
            msg.m_Header.m_GenericHeader.m_Address);
        StartCaptureAsRemote(pid);
      });

  GTcpServer->AddMainThreadCallback(
      Msg_StopCapture, [this](const Message&) { StopCaptureAsRemote(); });

  GTcpServer->AddMainThreadCallback(
      Msg_RemoteProcessRequest, [this](const Message& msg) {
        uint32_t pid = static_cast<uint32_t>(
            msg.m_Header.m_GenericHeader.m_Address);

        SendRemoteProcess(GTcpServer, pid);
      });

  GTcpServer->AddMainThreadCallback(
      Msg_RemoteModuleDebugInfo, [=](const Message& msg) {
        uint32_t pid = static_cast<uint32_t>(
            msg.m_Header.m_GenericHeader.m_Address);

        PRINT("RemoteModuleDebugInfo pid=%d\n", pid);
        std::shared_ptr<Process> process = process_list_.GetProcess(pid);
        if (!process) {
          PRINT("Process not found (pid=%d)\n", pid);
          return;
        }

        Capture::SetTargetProcess(process);

        std::vector<ModuleDebugInfo> remoteModuleDebugInfo;
        std::vector<std::string> modules;

        std::istringstream buffer(std::string(msg.m_Data, msg.m_Size));
        cereal::BinaryInputArchive inputAr(buffer);
        inputAr(modules);

        for (std::string& module : modules) {
          ModuleDebugInfo moduleDebugInfo;
          moduleDebugInfo.m_Name = module;
          process->FillModuleDebugInfo(moduleDebugInfo);
          remoteModuleDebugInfo.push_back(moduleDebugInfo);
        }

        // Send data back
        std::string messageData = SerializeObjectBinary(remoteModuleDebugInfo);
        GTcpServer->Send(Msg_RemoteModuleDebugInfo, (void*)messageData.data(),
                         messageData.size());
      });
}

//-----------------------------------------------------------------------------
void ConnectionManager::SetupClientCallbacks() {
  GTcpClient->AddMainThreadCallback(Msg_RemotePerf, [=](const Message& a_Msg) {
    PRINT_VAR(a_Msg.m_Size);
    std::string msgStr(a_Msg.m_Data, a_Msg.m_Size);
    std::istringstream buffer(msgStr);

    Capture::NewSamplingProfiler();
    Capture::GSamplingProfiler->StartCapture();
    Capture::GSamplingProfiler->SetIsLinuxPerf(true);
    Capture::GSamplingProfiler->StopCapture();
    Capture::GSamplingProfiler->ProcessSamples();
    GCoreApp->RefreshCaptureView();
  });

  GTcpClient->AddCallback(Msg_SamplingCallstack, [=](const Message& a_Msg) {
    // TODO: Send buffered callstacks.
    LinuxCallstackEvent data;

    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::JSONInputArchive inputAr(buffer);
    inputAr(data);

    GCoreApp->ProcessSamplingCallStack(data);
  });

  GTcpClient->AddCallback(Msg_RemoteTimers, [=](const Message& a_Msg) {
    uint32_t numTimers = (uint32_t)a_Msg.m_Size / sizeof(Timer);
    Timer* timers = (Timer*)a_Msg.GetData();
    for (uint32_t i = 0; i < numTimers; ++i) {
      GTimerManager->Add(timers[i]);
    }
  });

  GTcpClient->AddCallback(Msg_RemoteCallStack, [=](const Message& a_Msg) {
    CallStack stack;
    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::JSONInputArchive inputAr(buffer);
    inputAr(stack);

    GCoreApp->ProcessCallStack(stack);
  });

  GTcpClient->AddCallback(Msg_RemoteSymbol, [=](const Message& a_Msg) {
    LinuxSymbol symbol;
    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::BinaryInputArchive inputAr(buffer);
    inputAr(symbol);

    GCoreApp->AddSymbol(symbol.m_Address, symbol.m_Module, symbol.m_Name);
  });

  GTcpClient->AddCallback(Msg_RemoteContextSwitches, [=](const Message& a_Msg) {
    uint32_t num_context_switches =
        (uint32_t)a_Msg.m_Size / sizeof(ContextSwitch);
    ContextSwitch* context_switches = (ContextSwitch*)a_Msg.GetData();
    for (uint32_t i = 0; i < num_context_switches; i++) {
      GCoreApp->ProcessContextSwitch(context_switches[i]);
    }
  });

  GTcpClient->AddCallback(Msg_SamplingCallstacks, [=](const Message& a_Msg) {
    const char* a_Data = a_Msg.GetData();
    size_t a_Size = a_Msg.m_Size;
    std::istringstream buffer(std::string(a_Data, a_Size));
    cereal::BinaryInputArchive inputAr(buffer);
    std::vector<LinuxCallstackEvent> call_stacks;
    inputAr(call_stacks);

    for (auto& cs : call_stacks) {
      GCoreApp->ProcessSamplingCallStack(cs);
    }
  });

  GTcpClient->AddCallback(
      Msg_SamplingHashedCallstacks, [=](const Message& a_Msg) {
        const char* a_Data = a_Msg.GetData();
        size_t a_Size = a_Msg.m_Size;
        std::istringstream buffer(std::string(a_Data, a_Size));
        cereal::BinaryInputArchive inputAr(buffer);
        std::vector<CallstackEvent> call_stacks;
        inputAr(call_stacks);

        for (auto& cs : call_stacks) {
          GCoreApp->ProcessHashedSamplingCallStack(cs);
        }
      });
}

//-----------------------------------------------------------------------------
void ConnectionManager::SendProcesses(TcpEntity* tcp_entity) {
  process_list_.Refresh();
  process_list_.UpdateCpuTimes();
  std::string process_data = SerializeObjectHumanReadable(process_list_);
  tcp_entity->Send(Msg_RemoteProcessList, process_data.data(),
                   process_data.size());
}

void ConnectionManager::SendRemoteProcess(TcpEntity* tcp_entity, uint32_t pid) {
  std::shared_ptr<Process> process = process_list_.GetProcess(pid);
  // TODO: remove this - pid should be part of every message,
  // and all the messages should to be as stateless as possible.
  Capture::SetTargetProcess(process);
  process->ListModules();
  if (process) {
    std::string process_data = SerializeObjectHumanReadable(*process);
    tcp_entity->Send(Msg_RemoteProcess, process_data.data(),
                     process_data.size());
  }
}

//-----------------------------------------------------------------------------
void ConnectionManager::ConnectionThread() {
  while (!m_ExitRequested) {
    if (!GTcpClient->IsValid()) {
      GTcpClient->Connect(m_RemoteAddress);
      GTcpClient->Start();
    } else {
      // std::string msg("Hello from dev machine");
      // GTcpClient->Send(msg);
    }

    Sleep(2000);
  }
}

//-----------------------------------------------------------------------------
void ConnectionManager::RemoteThread() {
  while (!m_ExitRequested) {
    if (GTcpServer && GTcpServer->HasConnection()) {
      SendProcesses(GTcpServer);
    }

    Sleep(2000);
  }
}
