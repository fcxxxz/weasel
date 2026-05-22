// WeaselServer.cpp : main source file for WeaselServer.exe
//
//	WTL MessageLoop 封装了消息循环. 实现了 getmessage/dispatchmessage....

#include "stdafx.h"
#include "resource.h"
#include "WeaselService.h"
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <RimeWithWeasel.h>
#include <WeaselUtility.h>
#include <winsparkle.h>
#include <functional>
#include <ShellScalingApi.h>
#include <WinUser.h>
#include <tlhelp32.h>
#include <memory>
#include <atlstr.h>
#pragma comment(lib, "Shcore.lib")
CAppModule _Module;

struct ParentProcessInfo {
  DWORD pid = 0;
  std::wstring name;
  std::wstring path;

  std::wstring Description() const {
    std::wstring description = L"parent_pid=" + std::to_wstring(pid);
    if (!name.empty())
      description += L" parent_name=" + name;
    if (!path.empty())
      description += L" parent_path=" + path;
    return description;
  }
};

static DWORD GetParentProcessId(DWORD pid,
                                std::wstring* parent_name = nullptr) {
  DWORD parent_pid = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;

  PROCESSENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) {
        parent_pid = entry.th32ParentProcessID;
        break;
      }
    } while (Process32Next(snapshot, &entry));
  }
  if (parent_pid && parent_name && Process32First(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == parent_pid) {
        *parent_name = entry.szExeFile;
        break;
      }
    } while (Process32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return parent_pid;
}

static std::wstring GetProcessImagePath(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return L"";

  WCHAR path[MAX_PATH] = {0};
  DWORD length = _countof(path);
  std::wstring result;
  if (QueryFullProcessImageNameW(process, 0, path, &length))
    result = path;
  CloseHandle(process);
  return result;
}

static ParentProcessInfo CurrentParentProcessInfo() {
  ParentProcessInfo info;
  info.pid = GetParentProcessId(GetCurrentProcessId(), &info.name);
  if (info.pid)
    info.path = GetProcessImagePath(info.pid);
  return info;
}

static bool TryConnectService(weasel::Client& client, bool wait_for_control) {
  int attempts = wait_for_control ? 20 : 1;
  for (int retry = 0; retry < attempts; ++retry) {
    if (client.TryConnect())
      return true;
    if (!wait_for_control)
      break;
    if (!weasel::IsServiceInstanceMutexPresent() && retry > 0)
      break;
    Sleep(100);
  }
  return false;
}

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPTSTR lpstrCmdLine,
                     int nCmdShow) {
  LANGID langId = get_language_id();
  SetThreadUILanguage(langId);
  SetThreadLocale(langId);
  ParentProcessInfo parent_process = CurrentParentProcessInfo();
  WeaselDebugLog(L"WeaselServer", L"entry cmdline=" +
                                      std::wstring(lpstrCmdLine) + L" " +
                                      parent_process.Description());

  if (!IsWindowsBlueOrLaterEx()) {
    WeaselDebugLog(L"WeaselServer", L"exit: unsupported Windows version");
    CString info, cap;
    info.LoadStringW(IDS_STR_SYSTEM_VERSION_WARNING);
    cap.LoadStringW(IDS_STR_SYSTEM_VERSION_WARNING_CAPTION);
    MessageBoxExW(NULL, info, cap, MB_ICONERROR, langId);
    return 0;
  }
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

  // 防止服务进程开启输入法
  ImmDisableIME(-1);

  WCHAR user_name[20] = {0};
  DWORD size = _countof(user_name);
  GetUserName(user_name, &size);
  if (!_wcsicmp(user_name, L"SYSTEM")) {
    WeaselDebugLog(L"WeaselServer", L"exit: running as SYSTEM");
    return 1;
  }

  HRESULT hRes = ::CoInitialize(NULL);
  // If you are running on NT 4.0 or higher you can use the following call
  // instead to make the EXE free threaded. This means that calls come in on a
  // random RPC thread.
  // HRESULT hRes = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
  ATLASSERT(SUCCEEDED(hRes));

  // this resolves ATL window thunking problem when Microsoft Layer for Unicode
  // (MSLU) is used
  ::DefWindowProc(NULL, 0, 0, 0L);

  AtlInitCommonControls(
      ICC_BAR_CLASSES);  // add flags to support other controls

  hRes = _Module.Init(NULL, hInstance);
  ATLASSERT(SUCCEEDED(hRes));

  if (!wcscmp(L"/userdir", lpstrCmdLine)) {
    WeaselDebugLog(L"WeaselServer", L"command /userdir");
    CreateDirectory(WeaselUserDataPath().c_str(), NULL);
    WeaselServerApp::explore(WeaselUserDataPath());
    return 0;
  }
  if (!wcscmp(L"/weaseldir", lpstrCmdLine)) {
    WeaselDebugLog(L"WeaselServer", L"command /weaseldir");
    WeaselServerApp::explore(WeaselServerApp::install_dir());
    return 0;
  }
  if (!wcscmp(L"/ascii", lpstrCmdLine) || !wcscmp(L"/nascii", lpstrCmdLine)) {
    weasel::Client client;
    bool ascii = !wcscmp(L"/ascii", lpstrCmdLine);
    bool connected = TryConnectService(client, false);
    WeaselDebugLog(L"WeaselServer",
                   L"command ascii connected=" + std::to_wstring(connected));
    if (connected)  // try to connect to running server
    {
      if (ascii)
        client.TrayCommand(ID_WEASELTRAY_ENABLE_ASCII);
      else
        client.TrayCommand(ID_WEASELTRAY_DISABLE_ASCII);
    }
    return 0;
  }

  // command line option /q stops the running server
  bool quit = weasel::IsServiceQuitCommandLine(lpstrCmdLine);
  bool stop_requested = weasel::IsServiceStopCommandLine(lpstrCmdLine);
  bool restart_requested = weasel::IsServiceRestartCommandLine(lpstrCmdLine);
  bool recovery_requested = weasel::IsServiceRecoveryCommandLine(lpstrCmdLine);
  bool startup_requested = weasel::IsServiceStartupCommandLine(lpstrCmdLine);
  bool implicit_start = weasel::IsImplicitServiceStartCommandLine(lpstrCmdLine);
  bool check_updates = weasel::IsServiceUpdateCommandLine(lpstrCmdLine);
  bool legacy_restart_from_interactive_parent =
      weasel::ShouldTreatLegacyRestartAsManual(
          lpstrCmdLine, parent_process.name, parent_process.path);
  bool restarted_existing_server = false;
  if (quit) {
    weasel::MarkServiceManualExit();
  } else if (legacy_restart_from_interactive_parent ||
             weasel::ShouldClearServiceManualExit(lpstrCmdLine)) {
    weasel::ClearServiceManualExit();
  }
  WeaselDebugLog(
      L"WeaselServer",
      L"service command quit=" + std::to_wstring(quit) + L" stop_requested=" +
          std::to_wstring(stop_requested) + L" restart_requested=" +
          std::to_wstring(restart_requested) + L" recovery_requested=" +
          std::to_wstring(recovery_requested) + L" startup_requested=" +
          std::to_wstring(startup_requested) + L" implicit_start=" +
          std::to_wstring(implicit_start) + L" check_updates=" +
          std::to_wstring(check_updates) + L" legacy_manual_restart=" +
          std::to_wstring(legacy_restart_from_interactive_parent) +
          L" manual_exit_marked=" +
          std::to_wstring(weasel::IsServiceManualExitMarked()));
  if (!quit && !legacy_restart_from_interactive_parent &&
      weasel::ShouldSuppressServiceStartAfterManualExit(lpstrCmdLine)) {
    WeaselDebugLog(
        L"WeaselServer",
        L"exit: service start suppressed because manual exit is marked");
    return 0;
  }
  // restart if already running
  {
    weasel::Client client;
    bool connected = client.TryConnect();
    WeaselDebugLog(L"WeaselServer", L"initial TryConnect connected=" +
                                        std::to_wstring(connected));
    if (connected)  // try to connect to running server
    {
      if (check_updates) {
        WeaselDebugLog(L"WeaselServer",
                       L"forwarding update check to existing server");
        client.TrayCommand(ID_WEASELTRAY_CHECKUPDATE);
        return 0;
      }
      if (!weasel::ShouldShutdownExistingService(lpstrCmdLine)) {
        WeaselDebugLog(
            L"WeaselServer",
            L"exit: service already running and command does not request "
            L"shutdown");
        return 0;
      }
      WeaselDebugLog(L"WeaselServer", L"sending shutdown to existing server");
      DWORD shutdown_reason = weasel::WEASEL_IPC_SHUTDOWN_REASON_RESTART;
      if (quit)
        shutdown_reason = weasel::WEASEL_IPC_SHUTDOWN_REASON_EXIT;
      else if (stop_requested)
        shutdown_reason = weasel::WEASEL_IPC_SHUTDOWN_REASON_STOP;
      client.ShutdownServer(shutdown_reason);
      if (quit || stop_requested)
        return 0;
      restarted_existing_server = true;
      client.Disconnect();
      int retry = 0;
      bool still_connected = true;
      bool mutex_present = true;
      for (; retry < 50; retry++) {
        Sleep(100);
        weasel::Client probe;
        still_connected = probe.TryConnect();
        mutex_present = weasel::IsServiceInstanceMutexPresent();
        WeaselDebugLog(L"WeaselServer",
                       L"restart wait retry=" + std::to_wstring(retry) +
                           L" still_connected=" +
                           std::to_wstring(still_connected) +
                           L" mutex_present=" + std::to_wstring(mutex_present));
        if (!still_connected && !mutex_present)
          break;
        if (still_connected && retry == 24)
          probe.ShutdownServer(weasel::WEASEL_IPC_SHUTDOWN_REASON_RESTART);
      }
      if (retry >= 50) {
        weasel::Client probe;
        if (probe.TryConnect())
          probe.NotifyService(
              weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE);
        WeaselDebugLog(L"WeaselServer",
                       L"exit: existing server did not stop in time");
        return 0;
      }
    } else if (quit || stop_requested) {
      WeaselDebugLog(L"WeaselServer", L"exit: stop requested but no server");
      return 0;
    }
  }

  if (recovery_requested && weasel::IsServiceManualExitMarked()) {
    WeaselDebugLog(
        L"WeaselServer",
        L"exit: recovery suppressed by manual exit before app start");
    return 0;
  }

  if (check_updates) {
    WeaselDebugLog(L"WeaselServer", L"command /update");
    WeaselServerApp::check_update();
  }

  CreateDirectory(WeaselUserDataPath().c_str(), NULL);

  int nRet = 0;
  try {
    WeaselDebugLog(L"WeaselServer",
                   L"starting app startup_notification=" +
                       std::to_wstring(weasel::ServiceStartupNotification(
                           restart_requested, restarted_existing_server)));
    WeaselServerApp app(weasel::ServiceStartupNotification(
        restart_requested, restarted_existing_server));
    RegisterApplicationRestart(weasel::ServiceRecoveryArgument(), 0);
    nRet = app.Run();
    WeaselDebugLog(L"WeaselServer",
                   L"app.Run returned " + std::to_wstring(nRet));
  } catch (...) {
    // bad luck...
    WeaselDebugLog(L"WeaselServer", L"exception while running app");
    nRet = -1;
  }

  _Module.Term();
  ::CoUninitialize();

  return nRet;
}
