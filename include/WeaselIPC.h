#pragma once
#include <WeaselIPCData.h>
#include <WeaselUtility.h>
#include <windows.h>
#include <functional>
#include <memory>
#include <KeyEvent.h>

#define WEASEL_IPC_WINDOW L"WeaselIPCWindow_1.0"
#define WEASEL_IPC_PIPE_NAME L"WeaselNamedPipe"

#define WEASEL_IPC_METADATA_SIZE 1024
#define WEASEL_IPC_BUFFER_SIZE (4 * 1024)
#define WEASEL_IPC_BUFFER_LENGTH (WEASEL_IPC_BUFFER_SIZE / sizeof(WCHAR))
#define WEASEL_IPC_SHARED_MEMORY_SIZE \
  (sizeof(PipeMessage) + WEASEL_IPC_BUFFER_SIZE)

enum WEASEL_IPC_COMMAND {
  WEASEL_IPC_ECHO = (WM_APP + 1),
  WEASEL_IPC_START_SESSION,
  WEASEL_IPC_END_SESSION,
  WEASEL_IPC_PROCESS_KEY_EVENT,
  WEASEL_IPC_SHUTDOWN_SERVER,
  WEASEL_IPC_FOCUS_IN,
  WEASEL_IPC_FOCUS_OUT,
  WEASEL_IPC_UPDATE_INPUT_POS,
  WEASEL_IPC_START_MAINTENANCE,
  WEASEL_IPC_END_MAINTENANCE,
  WEASEL_IPC_COMMIT_COMPOSITION,
  WEASEL_IPC_CLEAR_COMPOSITION,
  WEASEL_IPC_TRAY_COMMAND,
  WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
  WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
  WEASEL_IPC_CHANGE_PAGE,
  WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS,
  WEASEL_IPC_NOTIFY_SERVICE,
  WEASEL_IPC_LAST_COMMAND
};

namespace weasel {
enum WEASEL_IPC_KEY_EVENT_RESULT {
  WEASEL_IPC_KEY_EVENT_PROCESSED = 0x01,
  WEASEL_IPC_KEY_EVENT_EATEN = 0x02
};

enum WEASEL_IPC_MAINTENANCE_RESULT {
  WEASEL_IPC_MAINTENANCE_RESULT_NONE = 0,
  WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS = 1,
  WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE = 2
};

enum WEASEL_IPC_SERVICE_NOTIFICATION {
  WEASEL_IPC_SERVICE_NOTIFICATION_NONE = 0,
  WEASEL_IPC_SERVICE_NOTIFICATION_EXITING = 3,
  WEASEL_IPC_SERVICE_NOTIFICATION_RESTARTING = 4,
  WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS = 5,
  WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE = 6
};

enum WEASEL_IPC_SHUTDOWN_REASON {
  WEASEL_IPC_SHUTDOWN_REASON_NONE = 0,
  WEASEL_IPC_SHUTDOWN_REASON_EXIT = 1,
  WEASEL_IPC_SHUTDOWN_REASON_RESTART = 2,
  WEASEL_IPC_SHUTDOWN_REASON_STOP = 3
};

inline DWORD MakeKeyEventResult(BOOL eaten) {
  return WEASEL_IPC_KEY_EVENT_PROCESSED |
         (eaten ? WEASEL_IPC_KEY_EVENT_EATEN : 0);
}

inline bool IsKeyEventResultProcessed(DWORD result) {
  return (result & WEASEL_IPC_KEY_EVENT_PROCESSED) != 0;
}

inline bool IsKeyEventResultEaten(DWORD result) {
  return (result & WEASEL_IPC_KEY_EVENT_EATEN) != 0;
}

inline bool ShouldEatKeyEvent(BOOL handled, BOOL has_commit) {
  return handled || has_commit;
}

inline bool ShouldEatKeyEvent(BOOL handled,
                              BOOL has_commit,
                              BOOL was_composing,
                              BOOL is_composing) {
  return ShouldEatKeyEvent(handled, has_commit) ||
         (was_composing && !is_composing);
}

inline bool IsMaintenanceDeployResult(DWORD result) {
  return result == WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS ||
         result == WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE;
}

inline const char* MaintenanceDeployMessageValue(DWORD result) {
  return result == WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE ? "failure"
                                                         : "success";
}

inline DWORD ServiceStartupNotification(bool restart_requested,
                                        bool restarted_existing_server) {
  return restart_requested || restarted_existing_server
             ? WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS
             : WEASEL_IPC_SERVICE_NOTIFICATION_NONE;
}

inline fs::path ServiceManualExitFlagPath() {
  return WeaselLogPath() / L"weasel-service-manual-exit.flag";
}

inline void MarkServiceManualExit() {
  try {
    fs::path path = ServiceManualExitFlagPath();
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      WeaselDebugLog(L"WeaselIPC",
                     L"manual exit marked path=" + path.wstring());
    } else {
      WeaselDebugLog(L"WeaselIPC", L"manual exit mark failed path=" +
                                       path.wstring() + L" error=" +
                                       std::to_wstring(GetLastError()));
    }
  } catch (...) {
  }
}

inline void ClearServiceManualExit() {
  try {
    fs::path path = ServiceManualExitFlagPath();
    BOOL deleted = DeleteFileW(path.c_str());
    DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
    WeaselDebugLog(L"WeaselIPC", L"manual exit cleared path=" + path.wstring() +
                                     L" deleted=" + std::to_wstring(deleted) +
                                     L" error=" + std::to_wstring(error));
  } catch (...) {
  }
}

inline bool IsServiceManualExitMarked() {
  try {
    return fs::exists(ServiceManualExitFlagPath());
  } catch (...) {
    return false;
  }
}

inline bool ShouldAutoRecoverService() {
  return !IsServiceManualExitMarked();
}

inline LPCWSTR ServiceExecutableName() {
  return L"WeaselServer.exe";
}

inline std::wstring ServiceInstanceMutexName() {
  std::wstring name = L"(WEASEL)Furandōru-Sukāretto-";
  name += getUsername();
  return name;
}

inline bool IsServiceInstanceMutexPresent() {
  std::wstring name = ServiceInstanceMutexName();
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
  if (!mutex)
    return GetLastError() == ERROR_ACCESS_DENIED;
  CloseHandle(mutex);
  return true;
}

inline LPCWSTR ServiceRestartArgument() {
  return L"/restart";
}

inline LPCWSTR ServiceManualRestartArgument() {
  return L"/restart-manual";
}

inline LPCWSTR ServiceRecoveryArgument() {
  return L"/recover";
}

inline LPCWSTR ServiceStartupArgument() {
  return L"/startup";
}

inline LPCWSTR ServiceStopArgument() {
  return L"/stop";
}

inline std::wstring NormalizeServiceCommandLineArgument(LPCWSTR command_line) {
  if (!command_line)
    return L"";

  std::wstring argument = command_line;
  while (!argument.empty() && iswspace(argument.front()))
    argument.erase(0, 1);
  while (!argument.empty() && iswspace(argument.back()))
    argument.pop_back();
  if (argument.size() >= 2 && argument.front() == L'"' &&
      argument.back() == L'"') {
    argument.erase(0, 1);
    argument.pop_back();
  }
  for (auto& ch : argument)
    ch = static_cast<wchar_t>(towlower(ch));
  return argument;
}

inline std::wstring ServiceExecutablePath(const std::wstring& root_dir) {
  if (root_dir.empty())
    return L"";
  std::wstring path = root_dir;
  wchar_t last = path[path.size() - 1];
  if (last != L'\\' && last != L'/')
    path += L"\\";
  path += ServiceExecutableName();
  return path;
}

inline bool IsServiceRestartCommandLine(LPCWSTR command_line) {
  std::wstring argument = NormalizeServiceCommandLineArgument(command_line);
  return argument == ServiceRestartArgument() ||
         argument == ServiceManualRestartArgument() || argument == L"/rerun";
}

inline bool IsServiceLegacyRestartCommandLine(LPCWSTR command_line) {
  std::wstring argument = NormalizeServiceCommandLineArgument(command_line);
  return argument == ServiceRestartArgument() || argument == L"/rerun";
}

inline bool IsServiceManualRestartCommandLine(LPCWSTR command_line) {
  return NormalizeServiceCommandLineArgument(command_line) ==
         ServiceManualRestartArgument();
}

inline bool IsServiceRecoveryCommandLine(LPCWSTR command_line) {
  return NormalizeServiceCommandLineArgument(command_line) ==
         ServiceRecoveryArgument();
}

inline bool IsServiceStartupCommandLine(LPCWSTR command_line) {
  std::wstring argument = NormalizeServiceCommandLineArgument(command_line);
  return argument == ServiceStartupArgument() || argument == L"/start";
}

inline bool IsImplicitServiceStartCommandLine(LPCWSTR command_line) {
  return NormalizeServiceCommandLineArgument(command_line).empty();
}

inline bool IsServiceQuitCommandLine(LPCWSTR command_line) {
  std::wstring argument = NormalizeServiceCommandLineArgument(command_line);
  return argument == L"/q" || argument == L"/quit";
}

inline bool IsServiceStopCommandLine(LPCWSTR command_line) {
  std::wstring argument = NormalizeServiceCommandLineArgument(command_line);
  return argument == ServiceStopArgument() || argument == L"/stop-service";
}

inline bool IsServiceUpdateCommandLine(LPCWSTR command_line) {
  return NormalizeServiceCommandLineArgument(command_line) == L"/update";
}

inline bool ShouldSuppressServiceRecoveryAfterManualExit(LPCWSTR command_line) {
  return IsServiceRecoveryCommandLine(command_line) &&
         IsServiceManualExitMarked();
}

inline bool ShouldClearServiceManualExit(LPCWSTR command_line) {
  return IsServiceManualRestartCommandLine(command_line) ||
         IsServiceStartupCommandLine(command_line);
}

inline bool ShouldShutdownExistingService(LPCWSTR command_line) {
  return IsServiceQuitCommandLine(command_line) ||
         IsServiceStopCommandLine(command_line) ||
         IsServiceRestartCommandLine(command_line);
}

inline bool ShouldSuppressServiceStartAfterManualExit(LPCWSTR command_line) {
  if (!IsServiceManualExitMarked())
    return false;
  if (IsServiceManualRestartCommandLine(command_line) ||
      IsServiceStartupCommandLine(command_line) ||
      IsServiceQuitCommandLine(command_line) ||
      IsServiceStopCommandLine(command_line))
    return false;
  if (IsServiceRestartCommandLine(command_line))
    return true;
  return IsImplicitServiceStartCommandLine(command_line) ||
         IsServiceRecoveryCommandLine(command_line);
}

inline bool IsTrayMenuSelectionCancelled(UINT menu_id) {
  return menu_id == 0;
}

inline std::wstring ProcessBaseName(std::wstring path) {
  size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos)
    path = path.substr(slash + 1);
  for (auto& ch : path)
    ch = static_cast<wchar_t>(towlower(ch));
  return path;
}

inline bool IsInteractiveLegacyRestartParent(const std::wstring& parent_name) {
  std::wstring name = ProcessBaseName(parent_name);
  return name == L"explorer.exe" || name == L"taskmgr.exe" ||
         name == L"weaseldeployer.exe" || name == L"weaselsetup.exe";
}

inline bool ShouldTreatLegacyRestartAsManual(LPCWSTR command_line,
                                             const std::wstring& parent_name,
                                             const std::wstring& parent_path) {
  if (!IsServiceLegacyRestartCommandLine(command_line))
    return false;
  if (parent_path.empty())
    return false;
  return IsInteractiveLegacyRestartParent(parent_name.empty() ? parent_path
                                                              : parent_name);
}

inline bool ServiceRootFromCommandLine(const std::wstring& command_line,
                                       std::wstring& root_dir) {
  root_dir.clear();
  if (command_line.empty())
    return false;

  std::wstring exe_name = ServiceExecutableName();
  size_t exe_pos = std::wstring::npos;
  for (size_t i = 0; i + exe_name.size() <= command_line.size(); ++i) {
    if (!_wcsnicmp(command_line.c_str() + i, exe_name.c_str(),
                   exe_name.size())) {
      exe_pos = i;
      break;
    }
  }
  if (exe_pos == std::wstring::npos)
    return false;

  std::wstring exe_path = command_line.substr(0, exe_pos + exe_name.size());
  while (!exe_path.empty() && iswspace(exe_path[0]))
    exe_path.erase(0, 1);
  if (!exe_path.empty() && exe_path[0] == L'"')
    exe_path.erase(0, 1);
  while (!exe_path.empty() && (iswspace(exe_path[exe_path.size() - 1]) ||
                               exe_path[exe_path.size() - 1] == L'"')) {
    exe_path.erase(exe_path.size() - 1);
  }

  size_t slash_pos = exe_path.find_last_of(L"\\/");
  if (slash_pos == std::wstring::npos)
    return false;
  root_dir = exe_path.substr(0, slash_pos);
  return !root_dir.empty();
}

struct PipeMessage {
  WEASEL_IPC_COMMAND Msg;
  DWORD wParam;
  DWORD lParam;
};

struct IPCMetadata {
  enum { WINDOW_CLASS_LENGTH = 64 };
  UINT32 server_hwnd;
  WCHAR server_window_class[WINDOW_CLASS_LENGTH];
};

// 處理請求之物件
struct RequestHandler {
  using EatLine = std::function<bool(std::wstring&)>;
  RequestHandler() {}
  virtual ~RequestHandler() {}
  virtual void Initialize() {}
  virtual void Finalize() {}
  virtual DWORD FindSession(DWORD session_id) { return 0; }
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0) { return 0; }
  virtual DWORD RemoveSession(DWORD session_id) { return 0; }
  virtual BOOL ProcessKeyEvent(KeyEvent keyEvent,
                               DWORD session_id,
                               EatLine eat) {
    return FALSE;
  }
  virtual void CommitComposition(DWORD session_id) {}
  virtual void ClearComposition(DWORD session_id) {}
  virtual void SelectCandidateOnCurrentPage(size_t index, DWORD session_id) {}
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               DWORD session_id,
                                               EatLine eat) {
    return false;
  }
  virtual bool ChangePage(bool backward, DWORD session_id, EatLine eat) {
    return false;
  }
  virtual void FocusIn(DWORD param, DWORD session_id) {}
  virtual void FocusOut(DWORD param, DWORD session_id) {}
  virtual void UpdateInputPosition(RECT const& rc, DWORD session_id) {}
  virtual void StartMaintenance() {}
  virtual void EndMaintenance(
      DWORD result = WEASEL_IPC_MAINTENANCE_RESULT_NONE) {}
  virtual void NotifyService(DWORD notification) {}
  virtual void SetOption(DWORD session_id, const std::string& opt, bool val) {}
  virtual void UpdateColorTheme(BOOL darkMode) {}
};

// 處理server端回應之物件
typedef std::function<bool(LPWSTR buffer, DWORD length)> ResponseHandler;

// 事件處理函數
typedef std::function<bool()> CommandHandler;

// 啟動服務進程之物件
typedef CommandHandler ServerLauncher;

// IPC實現類聲明

class ClientImpl;
class ServerImpl;

// IPC接口類

class Client {
 public:
  Client();
  virtual ~Client();

  // 连接到服务，必要时启动服务进程
  bool Connect(ServerLauncher launcher = 0);
  // 仅尝试连接已运行的服务；服务不存在时立即返回
  bool TryConnect();
  // 重新建立会话
  bool Reconnect(ServerLauncher launcher = 0, bool wait_for_pipe = true);
  // 断开连接
  void Disconnect();
  // 终止服务
  void ShutdownServer(DWORD reason);
  // 显示服务状态通知
  void NotifyService(DWORD notification);
  // 發起會話
  void StartSession();
  // 結束會話
  void EndSession();
  // 進入維護模式
  void StartMaintenance();
  // 退出維護模式
  void EndMaintenance(DWORD result = WEASEL_IPC_MAINTENANCE_RESULT_NONE);
  // 测试连接
  bool Echo();
  // 请求服务处理按键消息
  bool ProcessKeyEvent(KeyEvent const& keyEvent);
  bool ProcessKeyEvent(KeyEvent const& keyEvent, bool* eaten);
  // 上屏正在編輯的文字
  bool CommitComposition();
  // 清除正在編輯的文字
  bool ClearComposition();
  // 选择当前页面编号为index的候选
  bool SelectCandidateOnCurrentPage(size_t index);
  // 高亮当前页面编号为index的候选
  bool HighlightCandidateOnCurrentPage(size_t index);
  // 翻页，backward = true 向前翻，false向后翻
  bool ChangePage(bool backward);
  // 更新输入位置
  void UpdateInputPosition(RECT const& rc);
  // 输入窗口获得焦点
  void FocusIn();
  // 输入窗口失去焦点
  void FocusOut();
  // 托盤菜單
  void TrayCommand(UINT menuId);
  bool TrayCommandSync(UINT menuId);
  // 读取server返回的数据
  bool GetResponseData(ResponseHandler handler);

 private:
  ClientImpl* m_pImpl;
};

class Server {
 public:
  Server();
  virtual ~Server();

  // 初始化服务
  HWND Start();
  // 结束服务
  int Stop();
  // 消息循环
  int Run();

  void SetRequestHandler(RequestHandler* pHandler);
  void AddMenuHandler(UINT uID, CommandHandler handler);
  HWND GetHWnd();

 private:
  ServerImpl* m_pImpl;
};

inline std::wstring GetPipeName() {
  std::wstring pipe_name;
  pipe_name += L"\\\\.\\pipe\\";
  pipe_name += getUsername();
  pipe_name += L"\\";
  pipe_name += WEASEL_IPC_PIPE_NAME;
  return pipe_name;
}
}  // namespace weasel
