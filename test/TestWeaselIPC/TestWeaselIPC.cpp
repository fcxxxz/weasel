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

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>

CAppModule _Module;

int console_main();
int client_main();
int bench_main(int iterations, int threads);
int server_main();
int unit_main();
int realseat_main(int iterations, int churn);
int stress_main();
void candidate_window_logic_unit_tests();

// usage: TestWeaselIPC.exe [/start | /stop | /console | /unit | /bench |
//                          /realseat <iterations> <churn_sessions> |
//                          /stress]

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
  } else if (argc > 1 && !wcscmp(L"/bench", argv[1])) {
    int iterations = argc > 2 ? _wtoi(argv[2]) : 2000;
    int threads = argc > 3 ? _wtoi(argv[3]) : 4;
    return bench_main(iterations, threads);
  } else if (argc > 1 && !wcscmp(L"/realseat", argv[1])) {
    int iterations = argc > 2 ? _wtoi(argv[2]) : 300;
    int churn = argc > 3 ? _wtoi(argv[3]) : 10;
    return realseat_main(iterations, churn);
  } else if (argc > 1 && !wcscmp(L"/stress", argv[1])) {
    return stress_main();
  }

  return -1;
}

namespace {
double percentile(std::vector<double> v, double p) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  size_t idx = (size_t)(p * (v.size() - 1) + 0.5);
  return v[(std::min)(idx, v.size() - 1)];
}
}  // namespace

// /realseat <iterations> <churn>: 连接默认命名空间的真实服务器（引擎+UI 全
// 走真实路径），测会话建立、首键、持续按键延迟与会话churn
int realseat_main(int iterations, int churn) {
  weasel::Client client;
  if (!client.TryConnect()) {
    std::wcerr << L"RESULT server=unreachable" << std::endl;
    return 1;
  }
  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);

  QueryPerformanceCounter(&t0);
  client.StartSession();
  QueryPerformanceCounter(&t1);
  double session_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
  client.FocusIn();

  const UINT seq[] = {'N', 'I', 'H', 'A', 'O', VK_SPACE};
  double first_key_us = -1.0;
  std::vector<double> us;
  us.reserve((size_t)iterations * _countof(seq));
  for (int i = 0; i < iterations; ++i) {
    for (UINT key : seq) {
      weasel::KeyEvent ev(key, 0);
      QueryPerformanceCounter(&t0);
      client.ProcessKeyEvent(ev);
      QueryPerformanceCounter(&t1);
      double d = (t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart;
      if (first_key_us < 0.0)
        first_key_us = d;
      else
        us.push_back(d);
    }
  }
  client.EndSession();

  std::vector<double> churn_ms;
  for (int i = 0; i < churn; ++i) {
    QueryPerformanceCounter(&t0);
    client.StartSession();
    QueryPerformanceCounter(&t1);
    churn_ms.push_back((t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
    client.FocusIn();
    weasel::KeyEvent ev((UINT)L'n', 0);
    client.ProcessKeyEvent(ev);
    client.EndSession();
  }

  printf(
      "RESULT session_first_ms=%.3f first_key_ms=%.3f keys=%zu p50_us=%.1f "
      "p95_us=%.1f p99_us=%.1f max_us=%.1f churn=%d churn_p50_ms=%.3f "
      "churn_max_ms=%.3f\n",
      session_ms, first_key_us / 1000.0, us.size(), percentile(us, 0.50),
      percentile(us, 0.95), percentile(us, 0.99),
      us.empty() ? 0.0 : *std::max_element(us.begin(), us.end()), churn,
      percentile(churn_ms, 0.50),
      churn_ms.empty() ? 0.0
                       : *std::max_element(churn_ms.begin(), churn_ms.end()));
  return 0;
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
  BOOST_TEST(
      !weasel::ShouldEatTestKeyEvent(false, false, false, false, false, 'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, false, false,
                                            true, 'A'));
  BOOST_TEST(
      weasel::ShouldEatTestKeyEvent(false, false, false, false, true, 'A'));
  BOOST_TEST(
      !weasel::ShouldEatTestKeyEvent(true, false, false, false, true, 'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(true, false, false, false, true,
                                            VK_SPACE));
  BOOST_TEST(
      weasel::ShouldEatTestKeyEvent(true, false, false, false, true, VK_SHIFT));
  BOOST_TEST(
      weasel::ShouldEatTestKeyEvent(true, true, false, false, true, 'A'));
  BOOST_TEST(!weasel::ShouldEatTestKeyEvent(false, false, false, false, true,
                                            VK_BACK));
  BOOST_TEST(
      weasel::ShouldEatTestKeyEvent(false, false, true, false, true, VK_BACK));
  BOOST_TEST(
      !weasel::ShouldEatTestKeyEvent(false, false, false, true, true, 'C'));
  BOOST_TEST(
      weasel::ShouldEatTestKeyEvent(false, false, true, true, true, 'C'));
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
  BOOST_TEST(
      !wcscmp(weasel::ServiceManualRestartArgument(), L"/restart-manual"));
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
    BOOST_TEST(
        !weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/restart"));
    BOOST_TEST(
        weasel::ShouldSuppressServiceRecoveryAfterManualExit(L"/recover"));
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
  BOOST_TEST(
      !weasel::IsTrayMenuSelectionCancelled(ID_WEASELTRAY_RERUN_SERVICE));

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
  BOOST_TEST(RimeUiNeedsUpdate(current_context, current_status, current_context,
                               next_status));

  BOOST_TEST(ShouldSuppressInlineOptionNotification("option", "ascii_mode"));
  BOOST_TEST(ShouldSuppressInlineOptionNotification("option", "!ascii_mode"));
  BOOST_TEST(!ShouldSuppressInlineOptionNotification("option", "full_shape"));
  BOOST_TEST(!ShouldSuppressInlineOptionNotification("option", "!full_shape"));
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

  candidate_window_logic_unit_tests();

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

// ---------------------------------------------------------------------------
// /bench [iterations] [threads]: measure client-side IPC round-trip latency
// and per-thread pipe-context memory growth against a running /start server.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <psapi.h>
#include <vector>

struct BenchStats {
  double min_us = 0, p50 = 0, p95 = 0, p99 = 0, max_us = 0, mean = 0;
};

static BenchStats Summarize(std::vector<double>& samples) {
  BenchStats s;
  if (samples.empty())
    return s;
  std::sort(samples.begin(), samples.end());
  const auto pick = [&](double q) {
    size_t i = static_cast<size_t>(q * (samples.size() - 1) + 0.5);
    return samples[(std::min)(i, samples.size() - 1)];
  };
  s.min_us = samples.front();
  s.p50 = pick(0.50);
  s.p95 = pick(0.95);
  s.p99 = pick(0.99);
  s.max_us = samples.back();
  double sum = 0;
  for (double v : samples)
    sum += v;
  s.mean = sum / samples.size();
  return s;
}

static void Report(const char* name, const BenchStats& s) {
  std::cout << "BENCH " << name << " min_us=" << (long long)s.min_us
            << " p50_us=" << (long long)s.p50 << " p95_us=" << (long long)s.p95
            << " p99_us=" << (long long)s.p99
            << " max_us=" << (long long)s.max_us
            << " mean_us=" << (long long)s.mean << std::endl;
}

static size_t WorkingSetKB() {
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
  return pmc.WorkingSetSize / 1024;
}

static bool read_buffer_bench(LPWSTR buffer, UINT length) {
  // Touch the response body so parsing cost is not optimized away.
  volatile wchar_t sink = buffer[0];
  (void)sink;
  return length > 0;
}

int bench_main(int iterations, int threads) {
  if (iterations <= 0)
    iterations = 2000;
  if (threads <= 0)
    threads = 1;

  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "bench: failed to connect." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "bench: failed to login." << std::endl;
    return -3;
  }

  using clock = std::chrono::steady_clock;

  // warmup both paths
  for (int i = 0; i < 100; ++i) {
    client.Echo();
    client.ProcessKeyEvent(weasel::KeyEvent(L'a', 0));
    client.GetResponseData(std::bind<bool>(
        read_buffer_bench, std::placeholders::_1, std::placeholders::_2));
  }

  std::vector<double> echo_us, key_us;
  echo_us.reserve(iterations);
  key_us.reserve(iterations);
  weasel::KeyEvent key(L'a', 0);

  for (int i = 0; i < iterations; ++i) {
    auto t0 = clock::now();
    bool ok = client.Echo();
    auto t1 = clock::now();
    if (ok)
      echo_us.push_back(
          std::chrono::duration<double, std::micro>(t1 - t0).count());

    t0 = clock::now();
    bool eaten = client.ProcessKeyEvent(key);
    bool got = false;
    if (eaten) {
      got = client.GetResponseData(std::bind<bool>(
          read_buffer_bench, std::placeholders::_1, std::placeholders::_2));
    }
    t1 = clock::now();
    if (got)
      key_us.push_back(
          std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  Report("echo", Summarize(echo_us));
  Report("key_roundtrip", Summarize(key_us));

  // Memory growth per additional transacting thread (thread-local pipe
  // handle + 64KB context each).
  const size_t ws_before = WorkingSetKB();
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&client]() {
      for (int i = 0; i < 50; ++i) {
        client.Echo();
      }
    });
  }
  for (auto& w : workers)
    w.join();
  const size_t ws_after = WorkingSetKB();
  std::cout << "BENCH threads=" << threads << " ws_before_kb=" << ws_before
            << " ws_after_kb=" << ws_after
            << " delta_kb=" << (ws_after - ws_before) << std::endl;

  client.EndSession();
  return 0;
}

// ---------------------------------------------------------------------------
// /stress: concurrent-clients regression test. A sandboxed /start server
// (WEASEL_IPC_NAMESPACE isolates its pipe and instance mutex from the user's
// live service) simulates heavy logins that hold the engine api lock while
// several clients type concurrently. Keystrokes must NEVER be dropped (the
// old try-lock dispatch silently answered 0 here and real apps received raw
// letters), and the Echo probe must keep answering while the lock is held
// (a stalled probe used to make clients mistake a busy server for a dead
// one and tear down healthy sessions).
// ---------------------------------------------------------------------------

int stress_main() {
  const wchar_t* kNamespace = L"stress";
  const wchar_t* kSlowMs = L"25";
  SetEnvironmentVariableW(L"WEASEL_IPC_NAMESPACE", kNamespace);
  SetEnvironmentVariableW(L"WEASEL_TEST_SLOW_COMMAND_MS", kSlowMs);

  // Spawn ourselves as the sandboxed server child.
  WCHAR self[MAX_PATH] = {0};
  GetModuleFileNameW(NULL, self, MAX_PATH);
  std::wstring cmdline = std::wstring(L"\"") + self + L"\" /start";
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {0};
  if (!CreateProcessW(self, &cmdline[0], NULL, NULL, FALSE,
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    std::cerr << "stress: failed to spawn server child." << std::endl;
    return 1;
  }
  CloseHandle(pi.hThread);

  // Wait for the child's pipe to come up.
  weasel::Client probe;
  bool up = false;
  for (int i = 0; i < 100 && !up; ++i) {
    up = probe.TryConnect();
    if (!up)
      Sleep(100);
  }
  if (!up) {
    std::cerr << "stress: server child did not come up." << std::endl;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    return 1;
  }

  using clock = std::chrono::steady_clock;

  // The server recreates its listening pipe instance after every accept;
  // a client that lands in that gap gets ERROR_PIPE_BUSY and Connect()
  // fails without retrying (real TSF clients ride _EnsureServerConnected's
  // retry loop over the same behavior). Mirror that tolerance here.
  auto connect_with_retry = [](weasel::Client& client) {
    for (int i = 0; i < 50; ++i) {
      if (client.Connect())
        return true;
      Sleep(10);
    }
    return false;
  };

  // Load generator: continuous logins, each holding the api lock ~25ms.
  std::atomic<bool> loading(true);
  std::atomic<int> logins(0);
  std::thread load([&]() {
    weasel::Client client;
    if (!connect_with_retry(client))
      return;
    while (loading.load(std::memory_order_relaxed)) {
      client.EndSession();
      client.StartSession();
      logins.fetch_add(1, std::memory_order_relaxed);
      Sleep(5);
    }
  });

  // Typing clients: every key must be processed, none may be dropped.
  const int kThreads = 4;
  const int kKeysPerThread = 40;
  std::atomic<int> dropped(0);
  std::atomic<int> answered(0);
  std::vector<std::thread> typers;
  for (int t = 0; t < kThreads; ++t) {
    typers.emplace_back([&]() {
      weasel::Client client;
      if (!connect_with_retry(client))
        return;
      client.StartSession();
      for (int i = 0; i < kKeysPerThread; ++i) {
        bool eaten = false;
        if (client.ProcessKeyEvent(weasel::KeyEvent(L'a', 0), &eaten)) {
          answered.fetch_add(1, std::memory_order_relaxed);
        } else {
          dropped.fetch_add(1, std::memory_order_relaxed);
        }
        Sleep(3);
      }
    });
  }

  // Echo must stay responsive while the api lock is held by slow logins.
  double worst_echo_ms = 0.0;
  std::thread prober([&]() {
    weasel::Client client;
    if (!connect_with_retry(client))
      return;
    client.StartSession();
    for (int i = 0; i < 40; ++i) {
      auto t0 = clock::now();
      bool ok = client.Echo();
      auto t1 = clock::now();
      if (ok) {
        double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        worst_echo_ms = (std::max)(worst_echo_ms, ms);
      }
      Sleep(10);
    }
  });

  for (auto& t : typers)
    t.join();
  prober.join();
  loading.store(false, std::memory_order_relaxed);
  load.join();

  const int expected = kThreads * kKeysPerThread;
  std::cout << "STRESS logins=" << logins.load() << " keys_answered="
            << answered.load() << "/" << expected
            << " keys_dropped=" << dropped.load()
            << " worst_echo_ms=" << worst_echo_ms << std::endl;

  int errors = 0;
  if (answered.load() != expected || dropped.load() != 0) {
    std::cerr << "stress: keystrokes were dropped under api-lock contention"
              << std::endl;
    ++errors;
  }
  if (worst_echo_ms > 100.0) {
    std::cerr << "stress: echo stalled behind api-lock holders ("
              << worst_echo_ms << "ms)" << std::endl;
    ++errors;
  }

  weasel::Client shutdown;
  if (shutdown.TryConnect()) {
    shutdown.ShutdownServer(weasel::WEASEL_IPC_SHUTDOWN_REASON_STOP);
    if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0)
      TerminateProcess(pi.hProcess, 1);
  }
  CloseHandle(pi.hProcess);
  return errors == 0 ? 0 : 1;
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
  // Signatures must match RequestHandler exactly; UINT != WeaselSessionId
  // (unsigned long) on MSVC, and a silent non-override makes the base no-ops
  // answer every request with 0. `override` keeps this from regressing again.
  // Per-request logging is compiled out by default: line-buffered cerr writes
  // showed up as millisecond outliers in /bench. Enable WEASEL_TEST_TRACE to
  // get them back while debugging the harness.
  DWORD FindSession(WeaselSessionId ipc_id) override {
    if (trace_enabled_)
      std::cerr << "FindSession: " << ipc_id << std::endl;
    return (ipc_id <= m_counter ? ipc_id : 0);
  }
  DWORD AddSession(LPWSTR buffer, EatLine eat = 0) override {
    if (trace_enabled_)
      std::cerr << "AddSession: " << m_counter + 1 << std::endl;
    // /stress: simulate a heavy login (schema load) that holds the engine
    // api lock, exactly like a real AddSession does while dispatching.
    const int slow_ms = slow_command_ms_.load(std::memory_order_relaxed);
    if (slow_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
    return ++m_counter;
  }
  DWORD RemoveSession(WeaselSessionId ipc_id) override {
    if (trace_enabled_)
      std::cerr << "RemoveClient: " << ipc_id << std::endl;
    return 0;
  }
  BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                       WeaselSessionId ipc_id,
                       EatLine eat) override {
    if (trace_enabled_)
      std::cerr << "ProcessKeyEvent: " << ipc_id
                << " keycode: " << keyEvent.keycode
                << " mask: " << keyEvent.mask << std::endl;
    processed_keys_.fetch_add(1, std::memory_order_relaxed);
    eat(std::wstring(L"Greeting=Hello, 小狼毫.\n"));
    return TRUE;
  }

  static std::atomic<int> slow_command_ms_;
  static std::atomic<int> processed_keys_;

 private:
  static bool trace_enabled_;
  unsigned int m_counter;
};

bool TestRequestHandler::trace_enabled_ = []() {
  WCHAR value[4] = {0};
  DWORD length = GetEnvironmentVariableW(L"WEASEL_TEST_TRACE", value,
                                         static_cast<DWORD>(_countof(value)));
  return length > 0 && value[0] != L'0';
}();

std::atomic<int> TestRequestHandler::slow_command_ms_([]() {
  WCHAR value[8] = {0};
  DWORD length = GetEnvironmentVariableW(L"WEASEL_TEST_SLOW_COMMAND_MS", value,
                                         static_cast<DWORD>(_countof(value)));
  return (length > 0 && length < _countof(value)) ? _wtoi(value) : 0;
}());

std::atomic<int> TestRequestHandler::processed_keys_(0);

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
