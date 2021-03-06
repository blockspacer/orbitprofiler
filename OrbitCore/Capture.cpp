//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "Capture.h"

#include <fstream>
#include <ostream>

#include "Core.h"
#include "CoreApp.h"
#include "EventBuffer.h"
#include "Injection.h"
#include "Log.h"
#include "OrbitRule.h"
#include "OrbitSession.h"
#include "OrbitUnreal.h"
#include "Params.h"
#include "Path.h"
#include "Pdb.h"
#include "SamplingProfiler.h"
#include "Serialization.h"
#include "TcpClient.h"
#include "TcpForward.h"
#include "TcpServer.h"
#include "TestRemoteMessages.h"
#include "TimerManager.h"
#include "absl/strings/str_format.h"

#ifdef _WIN32
#include "EventTracer.h"
#else
std::shared_ptr<Pdb> GPdbDbg;
#endif

bool Capture::GInjected = false;
bool Capture::GIsConnected = false;
std::string Capture::GInjectedProcess;
double Capture::GOpenCaptureTime;
bool Capture::GIsSampling = false;
bool Capture::GIsTesting = false;
uint32_t Capture::GNumSamples = 0;
uint32_t Capture::GNumSamplingTicks = 0;
uint32_t Capture::GFunctionIndex = -1;
uint32_t Capture::GNumInstalledHooks;
bool Capture::GHasContextSwitches;
Timer Capture::GTestTimer;
ULONG64 Capture::GMainFrameFunction;
ULONG64 Capture::GNumContextSwitches;
ULONG64 Capture::GNumLinuxEvents;
ULONG64 Capture::GNumProfileEvents;
int Capture::GCapturePort = 0;
std::string Capture::GCaptureHost = "localhost";
std::string Capture::GPresetToLoad = "";
std::string Capture::GProcessToInject = "";

std::map<ULONG64, Function*> Capture::GSelectedFunctionsMap;
std::map<ULONG64, Function*> Capture::GVisibleFunctionsMap;
std::unordered_map<ULONG64, ULONG64> Capture::GFunctionCountMap;
std::shared_ptr<CallStack> Capture::GSelectedCallstack;
std::vector<ULONG64> Capture::GSelectedAddressesByType[Function::NUM_TYPES];
std::unordered_map<DWORD64, std::shared_ptr<CallStack> > Capture::GCallstacks;
Mutex Capture::GCallstackMutex;
std::unordered_map<DWORD64, std::string> Capture::GZoneNames;
TextBox* Capture::GSelectedTextBox;
ThreadID Capture::GSelectedThreadId;
Timer Capture::GCaptureTimer;
std::chrono::system_clock::time_point Capture::GCaptureTimePoint;
Capture::LoadPdbAsyncFunc Capture::GLoadPdbAsync;

std::shared_ptr<SamplingProfiler> Capture::GSamplingProfiler = nullptr;
std::shared_ptr<Process> Capture::GTargetProcess = nullptr;
std::shared_ptr<Session> Capture::GSessionPresets = nullptr;

void (*Capture::GClearCaptureDataFunc)();
std::vector<std::shared_ptr<SamplingProfiler> > GOldSamplingProfilers;
bool Capture::GUnrealSupported = false;

// The user_data pointer is provided by caller when registering capture
// callback. It is then passed to the callback and can be used to store context
// such as a pointer to a class.
Capture::SamplingDoneCallback Capture::sampling_done_callback_ = nullptr;
void* Capture::sampling_done_callback_user_data_ = nullptr;

//-----------------------------------------------------------------------------
void Capture::Init() {
  GTargetProcess = std::make_shared<Process>();
  Capture::GCapturePort = GParams.m_Port;
}

//-----------------------------------------------------------------------------
bool Capture::Inject(bool a_WaitForConnection) {
  Injection inject;
  std::string dllName = Path::GetDllPath(GTargetProcess->GetIs64Bit());

  GTcpServer->Disconnect();

  GInjected = inject.Inject(dllName, *GTargetProcess, "OrbitInit");
  if (GInjected) {
    ORBIT_LOG(
        absl::StrFormat("Injected in %s", GTargetProcess->GetName().c_str()));
    GInjectedProcess = GTargetProcess->GetName();
  }

  if (a_WaitForConnection) {
    int numTries = 50;
    while (!GTcpServer->HasConnection() && numTries-- > 0) {
      ORBIT_LOG(absl::StrFormat("Waiting for connection on port %i",
                                Capture::GCapturePort));
      Sleep(100);
    }

    GInjected = GInjected && GTcpServer->HasConnection();
  }

  return GInjected;
}

//-----------------------------------------------------------------------------
bool Capture::InjectRemote() {
  Injection inject;
  std::string dllName = Path::GetDllPath(GTargetProcess->GetIs64Bit());
  GTcpServer->Disconnect();

  GInjected = inject.Inject(dllName, *GTargetProcess, "OrbitInitRemote");

  if (GInjected) {
    ORBIT_LOG(
        absl::StrFormat("Injected in %s", GTargetProcess->GetName().c_str()));
    GInjectedProcess = GTargetProcess->GetName();
  }

  return GInjected;
}

//-----------------------------------------------------------------------------
void Capture::SetTargetProcess(const std::shared_ptr<Process>& a_Process) {
  if (a_Process != GTargetProcess) {
    GInjected = false;
    GInjectedProcess = "";

    GTargetProcess = a_Process;
    GSamplingProfiler = std::make_shared<SamplingProfiler>(a_Process);
    GSelectedFunctionsMap.clear();
    GSessionPresets = nullptr;
    GOrbitUnreal.Clear();
    GTargetProcess->LoadDebugInfo();
    GTargetProcess->ClearWatchedVariables();
  }
}

//-----------------------------------------------------------------------------
bool Capture::Connect() {
  if (!GInjected) {
    Inject();
  }

  return GInjected;
}

//-----------------------------------------------------------------------------
bool Capture::StartCapture() {
  SCOPE_TIMER_LOG("Capture::StartCapture");

  if (GTargetProcess->GetName().size() == 0) return false;

  GCaptureTimer.Start();
  GCaptureTimePoint = std::chrono::system_clock::now();

#ifdef WIN32
  if (!IsRemote()) {
    if (!Connect()) {
      return false;
    }
  }
#endif

  GInjected = true;
  ++Message::GSessionID;
  GTcpServer->Send(Msg_NewSession);
  GTimerManager->StartRecording();

  ClearCaptureData();
  SendFunctionHooks();

  if (Capture::IsTrackingEvents()) {
#ifdef WIN32
    GEventTracer.Start();
#else
    GEventTracer.Start(GTargetProcess->GetID());
#endif
  } else if (Capture::IsRemote()) {
    Capture::NewSamplingProfiler();
    Capture::GSamplingProfiler->SetIsLinuxPerf(true);
    Capture::GSamplingProfiler->StartCapture();
  }

  GCoreApp->SendToUiNow(L"startcapture");

  if (GSelectedFunctionsMap.size() > 0) {
    GCoreApp->SendToUiNow(L"gotolive");
  }

  return true;
}

//-----------------------------------------------------------------------------
void Capture::StopCapture() {
  if (IsTrackingEvents()) {
    GEventTracer.Stop();
  } else if (Capture::IsRemote()) {
    Capture::GSamplingProfiler->StopCapture();
    Capture::GSamplingProfiler->ProcessSamples();
    GCoreApp->RefreshCaptureView();
  }

  if (!GInjected) {
    return;
  }

  TcpEntity* tcpEntity = Capture::GetMainTcpEntity();
  tcpEntity->Send(Msg_StopCapture);
  GTimerManager->StopRecording();
}

//-----------------------------------------------------------------------------
void Capture::ToggleRecording() {
  if (GTimerManager) {
    if (GTimerManager->m_IsRecording)
      StopCapture();
    else
      StartCapture();
  }
}

//-----------------------------------------------------------------------------
void Capture::ClearCaptureData() {
  GSelectedFunctionsMap.clear();
  GFunctionCountMap.clear();
  GZoneNames.clear();
  GSelectedTextBox = nullptr;
  GSelectedThreadId = 0;
  GNumProfileEvents = 0;
  GTcpServer->ResetStats();
  GOrbitUnreal.NewSession();
  GHasContextSwitches = false;
  GNumLinuxEvents = 0;
  GNumContextSwitches = 0;
}

//-----------------------------------------------------------------------------
MessageType GetMessageType(Function::OrbitType a_type) {
  static std::map<Function::OrbitType, MessageType> typeMap;
  if (typeMap.size() == 0) {
    typeMap[Function::NONE] = Msg_FunctionHook;
    typeMap[Function::ORBIT_TIMER_START] = Msg_FunctionHookZoneStart;
    typeMap[Function::ORBIT_TIMER_STOP] = Msg_FunctionHookZoneStop;
    typeMap[Function::ORBIT_LOG] = Msg_FunctionHook;
    typeMap[Function::ORBIT_OUTPUT_DEBUG_STRING] =
        Msg_FunctionHookOutputDebugString;
    typeMap[Function::UNREAL_ACTOR] = Msg_FunctionHookUnrealActor;
    typeMap[Function::ALLOC] = Msg_FunctionHookAlloc;
    typeMap[Function::FREE] = Msg_FunctionHookFree;
    typeMap[Function::REALLOC] = Msg_FunctionHookRealloc;
    typeMap[Function::ORBIT_DATA] = Msg_FunctionHookOrbitData;
  }

  assert(typeMap.size() == Function::OrbitType::NUM_TYPES);

  return typeMap[a_type];
}

//-----------------------------------------------------------------------------
void Capture::PreFunctionHooks() {
  // Clear selected functions
  for (int i = 0; i < Function::NUM_TYPES; ++i) {
    GSelectedAddressesByType[i].clear();
  }

  // Clear current argument tracking data
  GTcpServer->Send(Msg_ClearArgTracking);

  // Find OutputDebugStringA
  if (GParams.m_HookOutputDebugString) {
    if (DWORD64 outputAddr = GTargetProcess->GetOutputDebugStringAddress()) {
      GSelectedAddressesByType[Function::ORBIT_OUTPUT_DEBUG_STRING].push_back(
          outputAddr);
    }
  }

  // Find alloc/free functions
  GTargetProcess->FindCoreFunctions();

  // Unreal
  CheckForUnrealSupport();
}

//-----------------------------------------------------------------------------
void Capture::SendFunctionHooks() {
  PreFunctionHooks();

  for (Function* func : GTargetProcess->GetFunctions()) {
    if (func->IsSelected() || func->IsOrbitFunc()) {
      func->PreHook();
      uint64_t address = func->GetVirtualAddress();
      GSelectedAddressesByType[func->GetOrbitType()].push_back(address);
      GSelectedFunctionsMap[address] = func;
      func->ResetStats();
      GFunctionCountMap[address] = 0;
    }
  }

  GVisibleFunctionsMap = GSelectedFunctionsMap;

  if (GClearCaptureDataFunc) {
    GClearCaptureDataFunc();
  }

  if (Capture::IsRemote()) {
    std::vector<std::string> selectedFunctions;
    for (auto& pair : GSelectedFunctionsMap) {
      PRINT("Send Selected Function: %s\n", pair.second->PrettyName().c_str());
      selectedFunctions.push_back(std::to_string(pair.first));
    }

    std::string selectedFunctionsData =
        SerializeObjectHumanReadable(selectedFunctions);
    PRINT_VAR(selectedFunctionsData);
    GTcpClient->Send(Msg_RemoteSelectedFunctionsMap,
                     (void*)selectedFunctionsData.data(),
                     selectedFunctionsData.size());

    Message msg(Msg_StartCapture);
    msg.m_Header.m_GenericHeader.m_Address = GTargetProcess->GetID();
    GTcpClient->Send(msg);
  }

  // Unreal
  if (Capture::GUnrealSupported) {
    const OrbitUnrealInfo& info = GOrbitUnreal.GetUnrealInfo();
    GTcpServer->Send(Msg_OrbitUnrealInfo, info);
  }

  // Send all hooks by type
  for (int i = 0; i < Function::NUM_TYPES; ++i) {
    std::vector<DWORD64>& addresses = GSelectedAddressesByType[i];
    if (addresses.size()) {
      MessageType msgType = GetMessageType((Function::OrbitType)i);
      GTcpServer->Send(msgType, addresses);
    }
  }
}

//-----------------------------------------------------------------------------
void Capture::SendDataTrackingInfo() {
  // Send information about arguments we want to track
  for (auto& pair : *GCoreApp->GetRules()) {
    const std::shared_ptr<Rule> rule = pair.second;
    Function* func = rule->m_Function;
    Message msg(Msg_ArgTracking);
    ArgTrackingHeader& header = msg.m_Header.m_ArgTrackingHeader;
    uint64_t address = func->GetVirtualAddress();
    header.m_Function = address;
    header.m_NumArgs = (int)rule->m_TrackedVariables.size();

    // TODO: Argument tracking was hijacked by data tracking
    //       We should separate both concepts and revive argument
    //       tracking.
    std::vector<Argument> args;
    for (const std::shared_ptr<Variable> var : rule->m_TrackedVariables) {
      Argument arg;
      arg.m_Offset = (DWORD)var->m_Address;
      arg.m_NumBytes = var->m_Size;
      args.push_back(arg);
    }

    msg.m_Size = (int)args.size() * sizeof(Argument);

    GTcpServer->Send(msg, (void*)args.data());
  }
}

//-----------------------------------------------------------------------------
void Capture::TestHooks() {
  if (!GIsTesting) {
    GIsTesting = true;
    GFunctionIndex = 0;
    GTestTimer.Start();
  } else {
    GIsTesting = false;
  }
}

//-----------------------------------------------------------------------------
void Capture::StartSampling() {
#ifdef WIN32
  if (!GIsSampling && Capture::IsTrackingEvents() &&
      GTargetProcess->GetName().size()) {
    SCOPE_TIMER_LOG("Capture::StartSampling");

    GCaptureTimer.Start();
    GCaptureTimePoint = std::chrono::system_clock::now();

    ClearCaptureData();
    GTimerManager->StartRecording();
    GEventTracer.Start();

    GIsSampling = true;
  }
#endif
}

//-----------------------------------------------------------------------------
void Capture::StopSampling() {
  if (GIsSampling) {
    if (IsTrackingEvents()) {
      GEventTracer.Stop();
    }

    GTimerManager->StopRecording();
  }
}

//-----------------------------------------------------------------------------
bool Capture::IsCapturing() {
  return GTimerManager && GTimerManager->m_IsRecording;
}

//-----------------------------------------------------------------------------
TcpEntity* Capture::GetMainTcpEntity() {
  return Capture::IsRemote() ? (TcpEntity*)GTcpClient.get()
                             : (TcpEntity*)GTcpServer;
}

//-----------------------------------------------------------------------------
void Capture::Update() {
  if (GIsSampling) {
#ifdef WIN32
    if (GSamplingProfiler->ShouldStop()) {
      GSamplingProfiler->StopCapture();
    }
#endif

    if (GSamplingProfiler->GetState() == SamplingProfiler::DoneProcessing) {
      if (sampling_done_callback_ != nullptr) {
        sampling_done_callback_(GSamplingProfiler,
                                sampling_done_callback_user_data_);
      }
      GIsSampling = false;
    }
  }

  if (GPdbDbg) {
    GPdbDbg->Update();
  }

#ifdef WIN32
  if (!Capture::IsRemote() && GInjected && !GTcpServer->HasConnection()) {
    StopCapture();
    GInjected = false;
  }
#endif
}

//-----------------------------------------------------------------------------
void Capture::DisplayStats() {
  if (GSamplingProfiler) {
    TRACE_VAR(GSamplingProfiler->GetNumSamples());
  }
}

//-----------------------------------------------------------------------------
void Capture::OpenCapture(const std::string& a_CaptureName) {
  LocalScopeTimer Timer(&GOpenCaptureTime);
  SCOPE_TIMER_LOG("OpenCapture");

  // TODO!
}

//-----------------------------------------------------------------------------
bool Capture::IsOtherInstanceRunning() {
#ifdef _WIN32
  DWORD procID = 0;
  HANDLE procHandle = Injection::GetTargetProcessHandle(ORBIT_EXE_NAME, procID);
  PRINT_FUNC;
  bool otherInstanceFound = procHandle != NULL;
  PRINT_VAR(otherInstanceFound);
  return otherInstanceFound;
#else
  return false;
#endif
}

//-----------------------------------------------------------------------------
void Capture::LoadSession(const std::shared_ptr<Session>& a_Session) {
  GSessionPresets = a_Session;

  std::vector<std::string> modulesToLoad;
  for (auto& it : a_Session->m_Modules) {
    SessionModule& module = it.second;
    ORBIT_LOG_DEBUG(module.m_Name);
    modulesToLoad.push_back(it.first);
  }

  if (GLoadPdbAsync) {
    GLoadPdbAsync(modulesToLoad);
  }

  GParams.m_ProcessPath = a_Session->m_ProcessFullPath;
  GParams.m_Arguments = a_Session->m_Arguments;
  GParams.m_WorkingDirectory = a_Session->m_WorkingDirectory;

  GCoreApp->SendToUiNow(L"SetProcessParams");
}

//-----------------------------------------------------------------------------
void Capture::SaveSession(const std::string& a_FileName) {
  Session session;
  session.m_ProcessFullPath = GTargetProcess->GetFullName();

  GCoreApp->SendToUiNow(L"UpdateProcessParams");

  session.m_ProcessFullPath = GParams.m_ProcessPath;
  session.m_Arguments = GParams.m_Arguments;
  session.m_WorkingDirectory = GParams.m_WorkingDirectory;

  for (Function* func : GTargetProcess->GetFunctions()) {
    if (func->IsSelected()) {
      session.m_Modules[func->GetPdb()->GetName()].m_FunctionHashes.push_back(
          func->Hash());
    }
  }

  std::string saveFileName = a_FileName;
  if (!EndsWith(a_FileName, ".opr")) {
    saveFileName += ".opr";
  }

  SCOPE_TIMER_LOG(
      absl::StrFormat("Saving Orbit session in %s", saveFileName.c_str()));
  std::ofstream file(saveFileName, std::ios::binary);
  cereal::BinaryOutputArchive archive(file);
  archive(cereal::make_nvp("Session", session));
}

//-----------------------------------------------------------------------------
void Capture::NewSamplingProfiler() {
  if (GSamplingProfiler) {
    // To prevent destruction while processing data...
    GOldSamplingProfilers.push_back(GSamplingProfiler);
  }

  Capture::GSamplingProfiler =
      std::make_shared<SamplingProfiler>(Capture::GTargetProcess, true);
}

//-----------------------------------------------------------------------------
bool Capture::IsTrackingEvents() {
#ifdef __linux
  return !IsRemote();
#else
  static bool yieldEvents = false;
  if (yieldEvents && IsOtherInstanceRunning() && GTargetProcess) {
    if (Contains(GTargetProcess->GetName(), "Orbit.exe")) {
      return false;
    }
  }

  if (GTargetProcess->GetIsRemote() && !GTcpServer->IsLocalConnection()) {
    return false;
  }

  return GParams.m_TrackContextSwitches || GParams.m_TrackSamplingEvents;
#endif
}

//-----------------------------------------------------------------------------
bool Capture::IsRemote() {
  return GTargetProcess && GTargetProcess->GetIsRemote();
}

//-----------------------------------------------------------------------------
bool Capture::IsLinuxData() {
  bool isLinux = false;
#if __linux__
  isLinux = true;
#endif
  return IsRemote() || isLinux;
}

//-----------------------------------------------------------------------------
void Capture::RegisterZoneName(DWORD64 a_ID, char* a_Name) {
  GZoneNames[a_ID] = a_Name;
}

//-----------------------------------------------------------------------------
void Capture::AddCallstack(CallStack& a_CallStack) {
  ScopeLock lock(GCallstackMutex);
  Capture::GCallstacks[a_CallStack.m_Hash] =
      std::make_shared<CallStack>(a_CallStack);
}

//-----------------------------------------------------------------------------
std::shared_ptr<CallStack> Capture::GetCallstack(CallstackID a_ID) {
  ScopeLock lock(GCallstackMutex);

  auto it = Capture::GCallstacks.find(a_ID);
  if (it != Capture::GCallstacks.end()) {
    return it->second;
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
void Capture::CheckForUnrealSupport() {
  GUnrealSupported =
      GCoreApp->GetUnrealSupportEnabled() && GOrbitUnreal.HasFnameInfo();
}

//-----------------------------------------------------------------------------
void Capture::PreSave() {
  // Add selected functions' exact address to sampling profiler
  for (auto& pair : GSelectedFunctionsMap) {
    GSamplingProfiler->AddAddress(pair.first);
  }
}

//-----------------------------------------------------------------------------
void Capture::TestRemoteMessages() { TestRemoteMessages::Get().Run(); }
