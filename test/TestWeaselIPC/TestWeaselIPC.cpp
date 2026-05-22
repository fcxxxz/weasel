// TestWeaselIPC.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <WeaselIPC.h>
#include <RimeWithWeasel.h>
#include <resource.h>
#include "../../WeaselIPC/WeaselClientImpl.h"
#include "../../WeaselIPCServer/WeaselServerImpl.h"
#include "../../WeaselServer/WeaselTrayIcon.h"

#include <boost/detail/lightweight_test.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>
using namespace boost::interprocess;

#include <iostream>
#include <memory>

CAppModule _Module;

int console_main();
int client_main();
int server_main();
int unit_main();

// usage: TestWeaselIPC.exe [/start | /stop | /console]

int _tmain(int argc, _TCHAR* argv[]) {
  if (argc == 1)  // no args
  {
    return client_main();
  } else if (argc > 1 && !wcscmp(L"/start", argv[1])) {
    return server_main();
  } else if (argc > 1 && !wcscmp(L"/stop", argv[1])) {
    weasel::Client client;
    if (!client.TryConnect()) {
      std::cerr << "server not running." << std::endl;
      return 0;
    }
    client.ShutdownServer(weasel::WEASEL_IPC_SHUTDOWN_REASON_STOP);
    return 0;
  } else if (argc > 1 && !wcscmp(L"/console", argv[1])) {
    return console_main();
    return 0;
  } else if (argc > 1 && !wcscmp(L"/unit", argv[1])) {
    return unit_main();
  }

  return -1;
}

int unit_main() {
  BOOST_TEST(!weasel::IsKeyEventResultProcessed(0));
  BOOST_TEST(!weasel::IsKeyEventResultEaten(0));

  DWORD not_eaten = weasel::MakeKeyEventResult(FALSE);
  BOOST_TEST(weasel::IsKeyEventResultProcessed(not_eaten));
  BOOST_TEST(!weasel::IsKeyEventResultEaten(not_eaten));

  DWORD eaten = weasel::MakeKeyEventResult(TRUE);
  BOOST_TEST(weasel::IsKeyEventResultProcessed(eaten));
  BOOST_TEST(weasel::IsKeyEventResultEaten(eaten));
  BOOST_TEST(!weasel::ShouldEatKeyEvent(FALSE, FALSE));
  BOOST_TEST(weasel::ShouldEatKeyEvent(TRUE, FALSE));
  BOOST_TEST(weasel::ShouldEatKeyEvent(FALSE, TRUE));
  BOOST_TEST(weasel::ShouldEatKeyEvent(TRUE, TRUE));
  BOOST_TEST(weasel::ShouldEatKeyEvent(FALSE, FALSE, TRUE, FALSE));
  BOOST_TEST(!weasel::ShouldEatKeyEvent(FALSE, FALSE, FALSE, FALSE));
  BOOST_TEST(!weasel::ShouldEatKeyEvent(FALSE, FALSE, TRUE, TRUE));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, false, false,
                                            'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, false, false,
                                            true, 'A'));
  BOOST_TEST(weasel::ShouldEatTestKeyEvent(false, false, false, false, true,
                                           'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(true, false, false, false, true,
                                            'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(true, false, false, false, true,
                                            VK_SPACE));
  BOOST_TEST(weasel::ShouldEatTestKeyEvent(true, false, false, false, true,
                                           VK_SHIFT));
  BOOST_TEST(weasel::ShouldEatTestKeyEvent(true, true, false, false, true,
                                           'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, false, true,
                                            VK_BACK));
  BOOST_TEST(weasel::ShouldEatTestKeyEvent(false, false, true, false, true,
                                           VK_BACK));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, true, true,
                                            'C'));
  BOOST_TEST(weasel::ShouldEatTestKeyEvent(false, false, true, true, true,
                                           'C'));
  BOOST_TEST(weasel::IsTextVirtualKey('A'));
  BOOST_TEST(weasel::IsTextVirtualKey(VK_SPACE));
  BOOST_TEST(!weasel::IsTextVirtualKey(VK_SHIFT));
  BOOST_TEST(weasel::IsModeSwitchVirtualKey(VK_SHIFT));
  BOOST_TEST(weasel::IsModeSwitchVirtualKey(VK_CONTROL));
  BOOST_TEST(weasel::IsModeSwitchVirtualKey(VK_CAPITAL));
  BOOST_TEST(!weasel::IsModeSwitchVirtualKey(VK_BACK));

  BOOST_TEST(weasel::IsMaintenanceDeployResult(
      weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS));
  BOOST_TEST(weasel::IsMaintenanceDeployResult(
      weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE));
  BOOST_TEST(!weasel::IsMaintenanceDeployResult(
      weasel::WEASEL_IPC_MAINTENANCE_RESULT_NONE));
  BOOST_TEST(!strcmp(weasel::MaintenanceDeployMessageValue(
                         weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS),
                     "success"));
  BOOST_TEST(!strcmp(weasel::MaintenanceDeployMessageValue(
                         weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE),
                     "failure"));
  BOOST_TEST(MaintenanceDeployResultStringId(
                 weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS) ==
             IDS_STR_DEPLOY_SUCCESS);
  BOOST_TEST(MaintenanceDeployResultStringId(
                 weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE) ==
             IDS_STR_DEPLOY_FAILURE);
  BOOST_TEST(MaintenanceDeployResultBalloonIcon(
                 weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS) == NIIF_INFO);
  BOOST_TEST(MaintenanceDeployResultBalloonIcon(
                 weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE) == NIIF_ERROR);
  BOOST_TEST(ServiceNotificationStringId(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_EXITING) ==
             IDS_STR_SERVICE_EXITING);
  BOOST_TEST(ServiceNotificationStringId(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTARTING) ==
             IDS_STR_SERVICE_RESTARTING);
  BOOST_TEST(ServiceNotificationStringId(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS) ==
             IDS_STR_SERVICE_RESTART_SUCCESS);
  BOOST_TEST(ServiceNotificationStringId(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE) ==
             IDS_STR_SERVICE_RESTART_FAILURE);
  BOOST_TEST(ServiceNotificationBalloonIcon(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE) ==
             NIIF_ERROR);
  BOOST_TEST(ServiceNotificationBalloonIcon(
                 weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS) ==
             NIIF_INFO);
  BOOST_TEST(weasel::ServiceStartupNotification(false, false) ==
             weasel::WEASEL_IPC_SERVICE_NOTIFICATION_NONE);
  BOOST_TEST(weasel::ServiceStartupNotification(true, false) ==
             weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS);
  BOOST_TEST(weasel::ServiceStartupNotification(false, true) ==
             weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS);
  BOOST_TEST(!wcscmp(weasel::ServiceExecutableName(), L"WeaselServer.exe"));
  BOOST_TEST(!wcscmp(weasel::ServiceRestartArgument(), L"/restart"));
  BOOST_TEST(!wcscmp(weasel::ServiceManualRestartArgument(),
                     L"/restart-manual"));
  BOOST_TEST(!wcscmp(weasel::ServiceRecoveryArgument(), L"/recover"));
  BOOST_TEST(!wcscmp(weasel::ServiceStartupArgument(), L"/startup"));
  BOOST_TEST(!wcscmp(weasel::ServiceStopArgument(), L"/stop"));
  BOOST_TEST(weasel::IsServiceRestartCommandLine(L"/restart"));
  BOOST_TEST(weasel::IsServiceRestartCommandLine(L"/restart-manual"));
  BOOST_TEST(weasel::IsServiceLegacyRestartCommandLine(L"/restart"));
  BOOST_TEST(!weasel::IsServiceLegacyRestartCommandLine(L"/restart-manual"));
  BOOST_TEST(weasel::IsServiceManualRestartCommandLine(L"/restart-manual"));
  BOOST_TEST(!weasel::IsServiceManualRestartCommandLine(L"/restart"));
  BOOST_TEST(weasel::IsServiceRestartCommandLine(L"/rerun"));
  BOOST_TEST(weasel::IsServiceRestartCommandLine(L" /ReStart "));
  BOOST_TEST(weasel::IsServiceManualRestartCommandLine(L" /ReStart-Manual "));
  BOOST_TEST(weasel::IsServiceRestartCommandLine(L"/RERUN "));
  BOOST_TEST(!weasel::IsServiceRestartCommandLine(L"/recover"));
  BOOST_TEST(weasel::IsServiceRecoveryCommandLine(L"/recover"));
  BOOST_TEST(weasel::IsServiceRecoveryCommandLine(L" /Recover "));
  BOOST_TEST(!weasel::IsServiceRecoveryCommandLine(L"/restart"));
  BOOST_TEST(weasel::IsServiceStartupCommandLine(L"/startup"));
  BOOST_TEST(weasel::IsServiceStartupCommandLine(L" /START "));
  BOOST_TEST(!weasel::IsServiceStartupCommandLine(L"/restart"));
  BOOST_TEST(weasel::IsImplicitServiceStartCommandLine(L""));
  BOOST_TEST(weasel::IsImplicitServiceStartCommandLine(NULL));
  BOOST_TEST(!weasel::IsImplicitServiceStartCommandLine(L"/startup"));
  BOOST_TEST(weasel::IsServiceQuitCommandLine(L"/q"));
  BOOST_TEST(weasel::IsServiceQuitCommandLine(L" /QUIT "));
  BOOST_TEST(!weasel::IsServiceQuitCommandLine(L"/restart"));
  BOOST_TEST(weasel::IsServiceStopCommandLine(L"/stop"));
  BOOST_TEST(weasel::IsServiceStopCommandLine(L" /STOP-SERVICE "));
  BOOST_TEST(!weasel::IsServiceStopCommandLine(L"/q"));
  BOOST_TEST(!weasel::IsServiceRestartCommandLine(L""));
  BOOST_TEST(!weasel::IsServiceRestartCommandLine(NULL));
  std::wstring service_root = L"C:\\Program Files\\Rime\\weasel";
  BOOST_TEST(weasel::ServiceExecutablePath(service_root) ==
             L"C:\\Program Files\\Rime\\weasel\\WeaselServer.exe");
  std::wstring parsed_root;
  BOOST_TEST(weasel::ServiceRootFromCommandLine(
      L"C:\\Program Files\\Rime\\weasel\\WeaselServer.exe", parsed_root));
  BOOST_TEST(parsed_root == service_root);
  parsed_root.clear();
  BOOST_TEST(weasel::ServiceRootFromCommandLine(
      L"\"C:\\Program Files\\Rime\\weasel\\WeaselServer.exe\" /restart",
      parsed_root));
  BOOST_TEST(parsed_root == service_root);
  BOOST_TEST(!weasel::ServiceRootFromCommandLine(L"", parsed_root));
  BOOST_TEST(!weasel::ShouldTreatLegacyRestartAsManual(
      L"/restart", L"notepad.exe", L"C:\\Windows\\System32\\notepad.exe"));
  BOOST_TEST(weasel::ShouldTreatLegacyRestartAsManual(
      L"/rerun", L"explorer.exe", L"C:\\Windows\\explorer.exe"));
  BOOST_TEST(weasel::ShouldTreatLegacyRestartAsManual(
      L"/restart", L"WeaselDeployer.exe",
      L"C:\\Program Files\\Rime\\weasel\\WeaselDeployer.exe"));
  BOOST_TEST(weasel::ShouldTreatLegacyRestartAsManual(
      L"/restart", L"WeaselSetup.exe",
      L"C:\\Program Files\\Rime\\weasel\\WeaselSetup.exe"));
  BOOST_TEST(!weasel::ShouldTreatLegacyRestartAsManual(
      L"/restart", L"cmd.exe", L"C:\\Windows\\System32\\cmd.exe"));
  BOOST_TEST(!weasel::ShouldTreatLegacyRestartAsManual(L"/restart", L"", L""));
  BOOST_TEST(!weasel::ShouldTreatLegacyRestartAsManual(
      L"/restart-manual", L"notepad.exe",
      L"C:\\Windows\\System32\\notepad.exe"));

  struct ManualExitFlagGuard {
    ManualExitFlagGuard() : was_marked(weasel::IsServiceManualExitMarked()) {}
    ~ManualExitFlagGuard() {
      if (was_marked)
        weasel::MarkServiceManualExit();
      else
        weasel::ClearServiceManualExit();
    }
    bool was_marked;
  } manual_exit_guard;

  {
    weasel::ClearServiceManualExit();
    BOOST_TEST(!weasel::IsServiceManualExitMarked());
    BOOST_TEST(weasel::ShouldAutoRecoverService());
    weasel::MarkServiceManualExit();
    BOOST_TEST(weasel::IsServiceManualExitMarked());
    BOOST_TEST(!weasel::ShouldAutoRecoverService());
    BOOST_TEST(!weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/restart"));
    BOOST_TEST(weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/recover"));
    BOOST_TEST(!weasel::ShouldSuppressServiceRecoveryAfterManualExit(L""));
    BOOST_TEST(!weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/q"));
    BOOST_TEST(weasel::ShouldSuppressServiceStartAfterManualExit(L""));
    BOOST_TEST(weasel::ShouldSuppressServiceStartAfterManualExit(NULL));
    BOOST_TEST(weasel::ShouldSuppressServiceStartAfterManualExit(L"/recover"));
    BOOST_TEST(weasel::ShouldSuppressServiceStartAfterManualExit(L"/restart"));
    BOOST_TEST(
        !weasel::ShouldSuppressServiceStartAfterManualExit(L"/restart-manual"));
    BOOST_TEST(!weasel::ShouldSuppressServiceStartAfterManualExit(L"/startup"));
    BOOST_TEST(!weasel::ShouldSuppressServiceStartAfterManualExit(L"/q"));
    BOOST_TEST(!weasel::ShouldSuppressServiceStartAfterManualExit(L"/stop"));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(L""));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(NULL));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(L"/recover"));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(L"/update"));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(L"/unknown"));
    BOOST_TEST(!weasel::ShouldClearServiceManualExit(L"/restart"));
    BOOST_TEST(weasel::ShouldClearServiceManualExit(L"/restart-manual"));
    BOOST_TEST(weasel::ShouldClearServiceManualExit(L"/startup"));
    BOOST_TEST(weasel::ShouldShutdownExistingService(L"/q"));
    BOOST_TEST(weasel::ShouldShutdownExistingService(L"/stop"));
    BOOST_TEST(weasel::ShouldShutdownExistingService(L"/restart"));
    BOOST_TEST(!weasel::ShouldShutdownExistingService(L""));
    BOOST_TEST(!weasel::ShouldShutdownExistingService(NULL));
    BOOST_TEST(!weasel::ShouldShutdownExistingService(L"/startup"));
    BOOST_TEST(!weasel::ShouldShutdownExistingService(L"/recover"));
    BOOST_TEST(!weasel::ShouldShutdownExistingService(L"/update"));
    weasel::ClearServiceManualExit();
    BOOST_TEST(!weasel::IsServiceManualExitMarked());
    BOOST_TEST(weasel::ShouldAutoRecoverService());
    BOOST_TEST(
        !weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/recover"));
    BOOST_TEST(!weasel::ShouldSuppressServiceStartAfterManualExit(L""));
  }

  BOOST_TEST(weasel::IsTrayMenuSelectionCancelled(0));
  BOOST_TEST(!weasel::IsTrayMenuSelectionCancelled(ID_WEASELTRAY_QUIT));
  BOOST_TEST(!weasel::IsTrayMenuSelectionCancelled(ID_WEASELTRAY_RERUN_SERVICE));

  weasel::UIStyle style;
  weasel::Status status;
  status.composing = true;
  weasel::MarkLocalCompositionAborted(status);
  BOOST_TEST(!status.composing);
  status.reset();

  auto signature = RimeTrayIconSignature::From(style, status);
  BOOST_TEST(signature == RimeTrayIconSignature::From(style, status));

  weasel::Status composing_status = status;
  composing_status.composing = true;
  BOOST_TEST(signature == RimeTrayIconSignature::From(style, composing_status));

  weasel::Status ascii_status = status;
  ascii_status.ascii_mode = true;
  BOOST_TEST(signature != RimeTrayIconSignature::From(style, ascii_status));

  weasel::UIStyle icon_style = style;
  icon_style.current_zhung_icon = L"zh.ico";
  BOOST_TEST(signature != RimeTrayIconSignature::From(icon_style, status));

  RIME_STRUCT(RimeStatus, rime_status);
  rime_status.schema_id = "luna_pinyin";
  rime_status.schema_name = "Luna Pinyin";
  rime_status.is_ascii_mode = True;
  rime_status.is_composing = True;
  rime_status.is_disabled = False;
  rime_status.is_full_shape = True;

  auto status_snapshot = RimeUiStatusSnapshot::From(rime_status);
  BOOST_TEST(status_snapshot.has_status);
  BOOST_TEST(status_snapshot.schema_id == "luna_pinyin");
  BOOST_TEST(status_snapshot.status.schema_id == L"luna_pinyin");
  BOOST_TEST(status_snapshot.status.schema_name == L"Luna Pinyin");
  BOOST_TEST(status_snapshot.status.ascii_mode);
  BOOST_TEST(status_snapshot.status.composing);
  BOOST_TEST(!status_snapshot.status.disabled);
  BOOST_TEST(status_snapshot.status.full_shape);

  weasel::KeyEventTestCache key_cache;
  BOOST_TEST(!key_cache.Matches(false, 'A', 0x1e0001));
  key_cache.Store(false, 'A', 0x1e0001, FALSE);
  BOOST_TEST(key_cache.Matches(false, 'A', 0x1e0001));
  BOOST_TEST(!key_cache.Eaten());
  BOOST_TEST(key_cache.Matches(false, 'A', 0x401e0002));
  BOOST_TEST(!key_cache.Eaten());
  BOOST_TEST(!key_cache.Matches(true, 'A', 0x1e0001));
  BOOST_TEST(!key_cache.Matches(false, 'B', 0x1e0001));
  BOOST_TEST(!key_cache.Matches(false, 'A', 0x1f0001));
  key_cache.Store(true, 'A', 0x9e0001, TRUE);
  BOOST_TEST(key_cache.Matches(true, 'A', 0x9e0001));
  BOOST_TEST(key_cache.Eaten());
  key_cache.Clear();
  BOOST_TEST(!key_cache.Matches(true, 'A', 0x9e0001));

  key_cache.Store(false, VK_SHIFT, 0x002a0001, FALSE);
  key_cache.Remove(false, VK_SHIFT, 0x802a0001);
  BOOST_TEST(!key_cache.Matches(false, VK_SHIFT, 0x002a0001));
  key_cache.Store(true, VK_SHIFT, 0x802a0001, TRUE);
  key_cache.Remove(true, VK_SHIFT, 0x002a0001);
  BOOST_TEST(!key_cache.Matches(true, VK_SHIFT, 0x802a0001));

  key_cache.Store(false, 'A', 0x1e0001, FALSE);
  key_cache.Store(false, 'B', 0x300001, TRUE);
  BOOST_TEST(key_cache.Matches(false, 'A', 0x1e0001));
  BOOST_TEST(!key_cache.Eaten());
  BOOST_TEST(key_cache.Matches(false, 'B', 0x300001));
  BOOST_TEST(key_cache.Eaten());
  BOOST_TEST(key_cache.Matches(false, 'A', 0x1e0001));
  key_cache.RemoveMatched();
  BOOST_TEST(!key_cache.Matches(false, 'A', 0x1e0001));
  BOOST_TEST(key_cache.Matches(false, 'B', 0x300001));
  key_cache.RemoveMatched();
  BOOST_TEST(!key_cache.Matches(false, 'B', 0x300001));

  weasel::KeyEventTestCacheReset key_cache_reset;
  key_cache.Store(false, 'C', 0x2e0001, TRUE);
  key_cache_reset.Mark();
  BOOST_TEST(key_cache_reset.Pending());
  BOOST_TEST(key_cache_reset.Apply(key_cache));
  BOOST_TEST(!key_cache_reset.Pending());
  BOOST_TEST(!key_cache.Matches(false, 'C', 0x2e0001));
  BOOST_TEST(!key_cache_reset.Apply(key_cache));

  weasel::ActiveKeyDownGuard active_key_guard;
  BOOST_TEST(!active_key_guard.ShouldSuppress('A', 0x001e0001));
  active_key_guard.Remember('A', 0x001e0001);
  BOOST_TEST(active_key_guard.ShouldSuppress('A', 0x001e0001));
  BOOST_TEST(!active_key_guard.ShouldSuppress('A', 0x401e0001));
  active_key_guard.Release('A', 0x801e0001);
  BOOST_TEST(!active_key_guard.ShouldSuppress('A', 0x001e0001));
  BOOST_TEST(!active_key_guard.ShouldSuppress('C', 0x002e0001));
  active_key_guard.Remember('C', 0x402e0001);
  BOOST_TEST(!active_key_guard.ShouldSuppress('C', 0x402e0001));
  active_key_guard.Remember('C', 0x002e0001);
  BOOST_TEST(active_key_guard.ShouldSuppress('C', 0x002e0001));
  active_key_guard.Release('C', 0x802e0001);
  BOOST_TEST(!active_key_guard.ShouldSuppress('C', 0x002e0001));
  BOOST_TEST(!active_key_guard.ShouldSuppress('B', 0x00300001));
  active_key_guard.Remember('B', 0x00300001);
  BOOST_TEST(active_key_guard.ShouldSuppress('B', 0x00300001));
  active_key_guard.Reset();
  BOOST_TEST(!active_key_guard.ShouldSuppress('B', 0x00300001));

  weasel::InputPositionCache position_cache;
  BOOST_TEST(position_cache.ShouldSend(0x12345678));
  position_cache.MarkSent(0x12345678);
  BOOST_TEST(!position_cache.ShouldSend(0x12345678));
  BOOST_TEST(position_cache.ShouldSend(0x12345679));
  position_cache.Reset();
  BOOST_TEST(position_cache.ShouldSend(0x12345678));

  weasel::Context current_context;
  weasel::Status current_status;
  BOOST_TEST(!RimeUiNeedsUpdate(current_context, current_status,
                                current_context, current_status));

  weasel::Context next_context = current_context;
  next_context.aux.str = L"tip";
  BOOST_TEST(RimeUiNeedsUpdate(current_context, current_status, next_context,
                               current_status));

  weasel::Status next_status = current_status;
  next_status.ascii_mode = true;
  BOOST_TEST(RimeUiNeedsUpdate(current_context, current_status,
                               current_context, next_status));

  BOOST_TEST(ShouldSuppressInlineOptionNotification("option", "ascii_mode"));
  BOOST_TEST(ShouldSuppressInlineOptionNotification("option", "!ascii_mode"));
  BOOST_TEST(
      !ShouldSuppressInlineOptionNotification("option", "full_shape"));
  BOOST_TEST(
      !ShouldSuppressInlineOptionNotification("option", "!full_shape"));
  BOOST_TEST(!ShouldSuppressInlineOptionNotification("deploy", "success"));
  BOOST_TEST(!ShouldSuppressInlineOptionNotification("schema", "luna_pinyin"));

  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_PROCESS_KEY_EVENT, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_SHUTDOWN_SERVER, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_UPDATE_INPUT_POS, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_TRAY_COMMAND, ID_WEASELTRAY_DEPLOY, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_TRAY_COMMAND, ID_WEASELTRAY_ENABLE_ASCII, 0}));

  weasel::ClientImpl missing_server_client(
      L"\\\\.\\pipe\\WeaselTestMissingServerPipe");
  BOOST_TEST(!missing_server_client.TryConnect());

  return boost::report_errors();
}

bool launch_server() {
  int ret = (int)ShellExecute(NULL, L"open", L"TestWeaselIPC.exe", L"/start",
                              NULL, SW_NORMAL);
  if (ret <= 32) {
    std::cerr << "failed to launch server." << std::endl;
    return false;
  }
  return true;
}

bool read_buffer(LPWSTR buffer, UINT length, LPWSTR dest) {
  wbufferstream bs(buffer, length);
  bs.read(dest, WEASEL_IPC_BUFFER_LENGTH);
  return bs.good();
}

const char* wcstomb(const wchar_t* wcs) {
  const int buffer_len = 8192;
  static char buffer[buffer_len];
  WideCharToMultiByte(CP_OEMCP, NULL, wcs, -1, buffer, buffer_len, NULL, FALSE);
  return buffer;
}

int console_main() {
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to start session." << std::endl;
    return -3;
  }

  while (std::cin.good()) {
    int ch = std::cin.get();
    if (!std::cin.good())
      break;
    bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(ch, 0));
    std::cout << "server replies: " << eaten << std::endl;
    if (eaten) {
      WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
      bool ret = client.GetResponseData(
          std::bind<bool>(read_buffer, std::placeholders::_1,
                          std::placeholders::_2, std::ref(response)));
      std::cout << "get response data: " << ret << std::endl;
      std::cout << "buffer reads: " << std::endl
                << wcstomb(response) << std::endl;
    }
  }

  client.EndSession();

  return 0;
}

int client_main() {
  // launch_server();
  Sleep(1000);
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to login." << std::endl;
    return -3;
  }
  bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(L'a', 0));
  std::cout << "server replies: " << eaten << std::endl;
  if (eaten) {
    WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
    bool ret = client.GetResponseData(
        std::bind<bool>(read_buffer, std::placeholders::_1,
                        std::placeholders::_2, std::ref(response)));
    std::cout << "get response data: " << ret << std::endl;
    std::cout << "buffer reads: " << std::endl
              << wcstomb(response) << std::endl;
  }
  client.EndSession();

  system("pause");
  return 0;
}

class TestRequestHandler : public weasel::RequestHandler {
 public:
  TestRequestHandler() : m_counter(0) {
    std::cerr << "handler ctor." << std::endl;
  }
  virtual ~TestRequestHandler() {
    std::cerr << "handler dtor: " << m_counter << std::endl;
  }
  virtual UINT FindSession(UINT session_id) {
    std::cerr << "FindSession: " << session_id << std::endl;
    return (session_id <= m_counter ? session_id : 0);
  }
  virtual UINT AddSession(LPWSTR buffer) {
    std::cerr << "AddSession: " << m_counter + 1 << std::endl;
    return ++m_counter;
  }
  virtual UINT RemoveSession(UINT session_id) {
    std::cerr << "RemoveClient: " << session_id << std::endl;
    return 0;
  }
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               UINT session_id,
                               EatLine eat) {
    std::cerr << "ProcessKeyEvent: " << session_id
              << " keycode: " << keyEvent.keycode << " mask: " << keyEvent.mask
              << std::endl;
    eat(std::wstring(L"Greeting=Hello, 小狼毫.\n"));
    return TRUE;
  }

 private:
  unsigned int m_counter;
};

int server_main() {
  HRESULT hRes = _Module.Init(NULL, GetModuleHandle(NULL));
  ATLASSERT(SUCCEEDED(hRes));

  weasel::Server server;
  // weasel::UI ui;
  // const std::unique_ptr<weasel::RequestHandler> handler(new
  // RimeWithWeaselHandler(&ui));
  const std::unique_ptr<weasel::RequestHandler> handler(new TestRequestHandler);

  server.SetRequestHandler(handler.get());
  if (!server.Start())
    return -4;
  std::cerr << "server running." << std::endl;
  int ret = server.Run();
  std::cerr << "server quitting." << std::endl;
  return ret;
}
