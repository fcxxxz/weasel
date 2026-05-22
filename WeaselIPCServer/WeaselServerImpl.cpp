#include "stdafx.h"
#include "WeaselServerImpl.h"
#include <mutex>
#include <Windows.h>
#include <resource.h>
#include <WeaselUtility.h>

namespace weasel {
class PipeServer : public PipeChannel<DWORD, PipeMessage> {
 public:
  using ServerRunner = std::function<void()>;
  using Respond = std::function<void(Msg)>;
  using ServerHandler = std::function<void(PipeMessage, Respond)>;

  PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s);

 public:
  void Listen(ServerHandler const& handler);
  /* Get a server runner */
  ServerRunner GetServerRunner(ServerHandler const& handler);

 private:
  void _ProcessPipeThread(HANDLE pipe, ServerHandler const& handler);
};
}  // namespace weasel

using namespace weasel;

extern CAppModule _Module;
static std::mutex g_api_mutex;

static bool PipeMessageUsesTryOuterApiLock(PipeMessage pipe_msg) {
  return pipe_msg.Msg == WEASEL_IPC_PROCESS_KEY_EVENT ||
         pipe_msg.Msg == WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS;
}

ServerImpl::ServerImpl()
    : m_pRequestHandler(NULL),
      m_darkMode(IsUserDarkMode()),
      channel(std::make_unique<PipeServer>(GetPipeName(), sa.get_attr())) {
  m_hUser32Module = GetModuleHandle(_T("user32.dll"));
}

ServerImpl::~ServerImpl() {
  _Finailize();
}

void ServerImpl::_Finailize() {
  if (pipeThread != nullptr) {
    pipeThread->interrupt();
    pipeThread = nullptr;
  } else {
    // avoid finalize again
    return;
  }

  if (IsWindow()) {
    DestroyWindow();
  }
}

LRESULT ServerImpl::OnColorChange(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  if (IsUserDarkMode() != m_darkMode) {
    m_darkMode = IsUserDarkMode();
    std::lock_guard guard(g_api_mutex);
    if (m_pRequestHandler)
      m_pRequestHandler->UpdateColorTheme(m_darkMode);
  }
  return 0;
}

LRESULT ServerImpl::OnCreate(UINT uMsg,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL& bHandled) {
  // not neccessary...
  ::SetWindowText(m_hWnd, WEASEL_IPC_WINDOW);
  return 0;
}

LRESULT ServerImpl::OnClose(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled) {
  Stop();
  return 0;
}

LRESULT ServerImpl::OnDestroy(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  bHandled = FALSE;
  return 1;
}

LRESULT ServerImpl::OnQueryEndSystemSession(UINT uMsg,
                                            WPARAM wParam,
                                            LPARAM lParam,
                                            BOOL& bHandled) {
  return TRUE;
}

LRESULT ServerImpl::OnEndSystemSession(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  std::lock_guard guard(g_api_mutex);
  if (m_pRequestHandler) {
    m_pRequestHandler->Finalize();
    m_pRequestHandler = nullptr;
  }
  return 0;
}

LRESULT ServerImpl::OnServiceNotifyMessage(UINT uMsg,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           BOOL& bHandled) {
  std::lock_guard guard(g_api_mutex);
  if (m_pRequestHandler)
    m_pRequestHandler->NotifyService(static_cast<DWORD>(wParam));
  return 0;
}

LRESULT ServerImpl::OnCommand(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  UINT uID = LOWORD(wParam);
  switch (uID) {
    case ID_WEASELTRAY_ENABLE_ASCII: {
      std::lock_guard guard(g_api_mutex);
      if (m_pRequestHandler)
        m_pRequestHandler->SetOption(lParam, "ascii_mode", true);
      return 0;
    }
    case ID_WEASELTRAY_DISABLE_ASCII: {
      std::lock_guard guard(g_api_mutex);
      if (m_pRequestHandler)
        m_pRequestHandler->SetOption(lParam, "ascii_mode", false);
      return 0;
    }
    default:;
  }

  std::map<UINT, CommandHandler>::iterator it = m_MenuHandlers.find(uID);
  if (it == m_MenuHandlers.end()) {
    bHandled = FALSE;
    return 0;
  }
  it->second();  // execute command
  return 0;
}

DWORD ServerImpl::OnCommand(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  BOOL handled = TRUE;
  OnCommand(uMsg, wParam, lParam, handled);
  return handled;
}

HWND ServerImpl::Start() {
  std::wstring instanceName = ServiceInstanceMutexName();
  HANDLE hMutexOneInstance = ::CreateMutex(NULL, FALSE, instanceName.c_str());
  DWORD mutex_error = ::GetLastError();
  bool areYouOK =
      (mutex_error == ERROR_ALREADY_EXISTS || mutex_error == ERROR_ACCESS_DENIED);
  WeaselDebugLog(L"WeaselIPCServer",
                 L"Start mutex=" + instanceName + L" handle=" +
                     std::to_wstring(reinterpret_cast<uintptr_t>(
                         hMutexOneInstance)) +
                     L" error=" + std::to_wstring(mutex_error) +
                     L" already_running=" + std::to_wstring(areYouOK));

  if (areYouOK) {
    WeaselDebugLog(L"WeaselIPCServer",
                   L"Start failed: single instance guard");
    return 0;  // assure single instance
  }

  HWND hwnd = Create(NULL);
  WeaselDebugLog(L"WeaselIPCServer",
                 L"Create hidden window hwnd=" +
                     std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)) +
                     L" last_error=" + std::to_wstring(GetLastError()));

  return hwnd;
}

int ServerImpl::Stop() {
  WeaselDebugLog(L"WeaselIPCServer", L"Stop requested");
  // DO NOT exit process or finalize here
  // Let WeaselServer handle this
  PostMessage(WM_QUIT);
  return 0;
}

bool ServerImpl::PipeMessageNeedsOuterApiLock(PipeMessage pipe_msg) {
  switch (pipe_msg.Msg) {
    case WEASEL_IPC_PROCESS_KEY_EVENT:
    case WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS:
    case WEASEL_IPC_SHUTDOWN_SERVER:
    case WEASEL_IPC_NOTIFY_SERVICE:
    case WEASEL_IPC_UPDATE_INPUT_POS:
    case WEASEL_IPC_TRAY_COMMAND:
      return false;
    default:
      return true;
  }
}

void ServerImpl::PostServiceNotification(DWORD notification) {
  if (!notification)
    return;
  PostMessage(WM_WEASEL_SERVICE_NOTIFY, static_cast<WPARAM>(notification), 0);
}

int ServerImpl::Run() {
  // This workaround causes a VC internal error:
  // void PipeServer::Listen(ServerHandler handler);
  //
  // auto handler = boost::bind(&ServerImpl::HandlePipeMessage, this);
  // auto listener = boost::bind(&PipeServer::Listen, channel.get(), handler);
  //
  auto listener = [this](PipeMessage msg, PipeServer::Respond resp) -> void {
    HandlePipeMessage(msg, resp);
  };
  pipeThread = std::make_unique<boost::thread>(
      [this, &listener]() { channel->Listen(listener); });
  WeaselDebugLog(L"WeaselIPCServer", L"pipe listener thread started");

  CMessageLoop theLoop;
  _Module.AddMessageLoop(&theLoop);
  int nRet = theLoop.Run();
  _Module.RemoveMessageLoop();
  return nRet;
}

DWORD ServerImpl::OnEcho(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->FindSession(lParam);
}

DWORD ServerImpl::OnStartSession(WEASEL_IPC_COMMAND uMsg,
                                 DWORD wParam,
                                 DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->AddSession(
      reinterpret_cast<LPWSTR>(channel->ReceiveBuffer()),
      [this](std::wstring& msg) -> bool {
        *channel << msg;
        return true;
      });
}

DWORD ServerImpl::OnEndSession(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->RemoveSession(lParam);
}

DWORD ServerImpl::OnKeyEvent(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler || !m_pRequestHandler->FindSession(lParam))
    return 0;

  auto eat = [this](std::wstring& msg) -> bool {
    *channel << msg;
    return true;
  };
  return m_pRequestHandler->ProcessKeyEvent(KeyEvent(wParam), lParam, eat);
}

DWORD ServerImpl::OnKeyEventWithStatus(WEASEL_IPC_COMMAND uMsg,
                                       DWORD wParam,
                                       DWORD lParam) {
  if (!m_pRequestHandler || !m_pRequestHandler->FindSession(lParam))
    return 0;

  auto eat = [this](std::wstring& msg) -> bool {
    *channel << msg;
    return true;
  };
  BOOL eaten = m_pRequestHandler->ProcessKeyEvent(KeyEvent(wParam), lParam, eat);
  return MakeKeyEventResult(eaten);
}

DWORD ServerImpl::OnShutdownServer(WEASEL_IPC_COMMAND uMsg,
                                    DWORD wParam,
                                    DWORD lParam) {
  DWORD reason = WEASEL_IPC_SHUTDOWN_REASON_EXIT;
  if (wParam == WEASEL_IPC_SHUTDOWN_REASON_RESTART)
    reason = WEASEL_IPC_SHUTDOWN_REASON_RESTART;
  else if (wParam == WEASEL_IPC_SHUTDOWN_REASON_STOP)
    reason = WEASEL_IPC_SHUTDOWN_REASON_STOP;
  if (reason == WEASEL_IPC_SHUTDOWN_REASON_EXIT)
    MarkServiceManualExit();
  else if (reason == WEASEL_IPC_SHUTDOWN_REASON_RESTART)
    ClearServiceManualExit();
  if (reason == WEASEL_IPC_SHUTDOWN_REASON_EXIT)
    UnregisterApplicationRestart();

  DWORD notification = WEASEL_IPC_SERVICE_NOTIFICATION_EXITING;
  if (reason == WEASEL_IPC_SHUTDOWN_REASON_RESTART)
    notification = WEASEL_IPC_SERVICE_NOTIFICATION_RESTARTING;
  PostServiceNotification(notification);
  boost::thread([this] {
    Sleep(1200);
    Stop();
  }).detach();
  return 0;
}

DWORD ServerImpl::OnServiceNotification(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  PostServiceNotification(wParam);
  return 0;
}

DWORD ServerImpl::OnFocusIn(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusIn(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnFocusOut(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusOut(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnUpdateInputPosition(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  /*
   * 移位标志 = 1bit == 0
   * height: 0~127 = 7bit
   * top:-2048~2047 = 12bit（有符号）
   * left:-2048~2047 = 12bit（有符号）
   *
   * 高解析度下：
   * 移位标志 = 1bit == 1
   * height: 0~254 = 7bit（舍弃低1位）
   * top: -4096~4094 = 12bit（有符号，舍弃低1位）
   * left: -4096~4094 = 12bit（有符号，舍弃低1位）
   */
  RECT rc;
  int hi_res = (wParam >> 31) & 0x01;
  rc.left = ((wParam & 0x7ff) - (wParam & 0x800)) << hi_res;
  rc.top = (((wParam >> 12) & 0x7ff) - ((wParam >> 12) & 0x800)) << hi_res;
  const int width = 6;
  int height = ((wParam >> 24) & 0x7f) << hi_res;
  rc.right = rc.left + width;
  rc.bottom = rc.top + height;

  {
    using PPTLPFPMDPI = BOOL(WINAPI*)(HWND, LPPOINT);
    PPTLPFPMDPI PhysicalToLogicalPointForPerMonitorDPI =
        (PPTLPFPMDPI)::GetProcAddress(m_hUser32Module,
                                      "PhysicalToLogicalPointForPerMonitorDPI");
    POINT lt = {rc.left, rc.top};
    POINT rb = {rc.right, rc.bottom};
    PhysicalToLogicalPointForPerMonitorDPI(NULL, &lt);
    PhysicalToLogicalPointForPerMonitorDPI(NULL, &rb);
    rc = {lt.x, lt.y, rb.x, rb.y};
  }

  std::lock_guard guard(g_api_mutex);
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->UpdateInputPosition(rc, lParam);
  return 0;
}

DWORD ServerImpl::OnStartMaintenance(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->StartMaintenance();
  return 0;
}

DWORD ServerImpl::OnEndMaintenance(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->EndMaintenance(wParam);
  return 0;
}

DWORD ServerImpl::OnCommitComposition(WEASEL_IPC_COMMAND uMsg,
                                      DWORD wParam,
                                      DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->CommitComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnClearComposition(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->ClearComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnSelectCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                 DWORD wParam,
                                                 DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->SelectCandidateOnCurrentPage(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnHighlightCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                    DWORD wParam,
                                                    DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->HighlightCandidateOnCurrentPage(wParam, lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnChangePage(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->ChangePage(wParam, lParam, eat);
  }
  return 0;
}

#define MAP_PIPE_MSG_HANDLE(__msg, __wParam, __lParam) \
  {                                                    \
    auto lParam = __lParam;                            \
    auto wParam = __wParam;                            \
    LRESULT _result = 0;                               \
    switch (__msg) {
#define PIPE_MSG_HANDLE(__msg, __func)       \
  case __msg:                                \
    _result = __func(__msg, wParam, lParam); \
    break;

#define END_MAP_PIPE_MSG_HANDLE(__result) \
  }                                       \
  __result = _result;                     \
  }

DWORD ServerImpl::DispatchPipeMessage(PipeMessage pipe_msg) {
  DWORD result;

  MAP_PIPE_MSG_HANDLE(pipe_msg.Msg, pipe_msg.wParam, pipe_msg.lParam)
  PIPE_MSG_HANDLE(WEASEL_IPC_ECHO, OnEcho)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_SESSION, OnStartSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_SESSION, OnEndSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_PROCESS_KEY_EVENT, OnKeyEvent)
  PIPE_MSG_HANDLE(WEASEL_IPC_SHUTDOWN_SERVER, OnShutdownServer)
  PIPE_MSG_HANDLE(WEASEL_IPC_NOTIFY_SERVICE, OnServiceNotification)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_IN, OnFocusIn)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_OUT, OnFocusOut)
  PIPE_MSG_HANDLE(WEASEL_IPC_UPDATE_INPUT_POS, OnUpdateInputPosition)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_MAINTENANCE, OnStartMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_MAINTENANCE, OnEndMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_COMMIT_COMPOSITION, OnCommitComposition)
  PIPE_MSG_HANDLE(WEASEL_IPC_CLEAR_COMPOSITION, OnClearComposition);
  PIPE_MSG_HANDLE(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
                  OnSelectCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                  OnHighlightCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_CHANGE_PAGE, OnChangePage);
  PIPE_MSG_HANDLE(WEASEL_IPC_TRAY_COMMAND, OnCommand);
  PIPE_MSG_HANDLE(WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS,
                  OnKeyEventWithStatus);
  END_MAP_PIPE_MSG_HANDLE(result);

  return result;
}

template <typename _Resp>
void ServerImpl::HandlePipeMessage(PipeMessage pipe_msg, _Resp resp) {
  DWORD result;
  if (PipeMessageUsesTryOuterApiLock(pipe_msg)) {
    std::unique_lock<std::mutex> guard(g_api_mutex, std::try_to_lock);
    if (!guard.owns_lock()) {
      result = 0;
    } else {
      result = DispatchPipeMessage(pipe_msg);
    }
  } else if (PipeMessageNeedsOuterApiLock(pipe_msg)) {
    std::lock_guard guard(g_api_mutex);
    result = DispatchPipeMessage(pipe_msg);
  } else {
    result = DispatchPipeMessage(pipe_msg);
  }

  resp(result);
}

PipeServer::PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s)
    : PipeChannel(std::move(pn_cmd), s) {}

void PipeServer::Listen(ServerHandler const& handler) {
  for (;;) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    try {
      boost::this_thread::interruption_point();
      WeaselDebugLog(L"WeaselIPCServer", L"waiting for pipe connection");
      pipe = _ConnectServerPipe(pname);
      WeaselDebugLog(L"WeaselIPCServer",
                     L"pipe connected handle=" +
                         std::to_wstring(reinterpret_cast<uintptr_t>(pipe)));
      boost::thread th(
          [&handler, pipe, this] { _ProcessPipeThread(pipe, handler); });
      th.detach();
    } catch (DWORD ex) {
      WeaselDebugLog(L"WeaselIPCServer",
                     L"pipe listen exception=" + std::to_wstring(ex));
      _FinalizePipe(pipe);
    }
    boost::this_thread::interruption_point();
  }
}

PipeServer::ServerRunner PipeServer::GetServerRunner(
    ServerHandler const& handler) {
  return [&handler, this]() { Listen(handler); };
}

void PipeServer::_ProcessPipeThread(HANDLE pipe, ServerHandler const& handler) {
  try {
    for (;;) {
      Res msg;
      _Receive(pipe, &msg, sizeof(msg));
      handler(msg, [this, pipe](Msg resp) { _Send(pipe, resp); });
    }
  } catch (...) {
    _FinalizePipe(pipe);
  }
}

// weasel::Server

Server::Server() : m_pImpl(new ServerImpl) {}

Server::~Server() {
  if (m_pImpl)
    delete m_pImpl;
}

HWND Server::Start() {
  return m_pImpl->Start();
}

int Server::Stop() {
  return m_pImpl->Stop();
}

int Server::Run() {
  return m_pImpl->Run();
}

void Server::SetRequestHandler(RequestHandler* pHandler) {
  m_pImpl->SetRequestHandler(pHandler);
}

void Server::AddMenuHandler(UINT uID, CommandHandler handler) {
  m_pImpl->AddMenuHandler(uID, handler);
}

HWND Server::GetHWnd() {
  return m_pImpl->m_hWnd;
}
