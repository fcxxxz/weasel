#include "stdafx.h"
#include "WeaselServerApp.h"
#include <chrono>
#include <filesystem>
#include <thread>

WeaselServerApp::WeaselServerApp(DWORD startup_notification)
    : m_handler(std::make_unique<RimeWithWeaselHandler>(&m_ui)),
      tray_icon(m_ui),
      m_startup_notification(startup_notification) {
  // m_handler.reset(new RimeWithWeaselHandler(&m_ui));
  m_server.SetRequestHandler(m_handler.get());
  SetupMenuHandlers();
}

WeaselServerApp::~WeaselServerApp() {}

int WeaselServerApp::Run() {
  WeaselDebugLog(L"WeaselServerApp",
                 L"Run start startup_notification=" +
                     std::to_wstring(m_startup_notification));
  if (!m_server.Start()) {
    WeaselDebugLog(L"WeaselServerApp", L"m_server.Start failed");
    return -1;
  }
  WeaselDebugLog(L"WeaselServerApp", L"m_server.Start ok");

  // win_sparkle_set_appcast_url("http://localhost:8000/weasel/update/appcast.xml");
  win_sparkle_set_registry_path("Software\\Rime\\Weasel\\Updates");
  if (GetThreadUILanguage() ==
      MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
    win_sparkle_set_lang("zh-TW");
  else if (GetThreadUILanguage() ==
           MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
    win_sparkle_set_lang("zh-CN");
  else
    win_sparkle_set_lang("en");
  win_sparkle_init();
  m_ui.Create(m_server.GetHWnd());

  m_handler->Initialize();
  m_handler->OnUpdateUI([this]() { tray_icon.Refresh(); });

  tray_icon.Create(m_server.GetHWnd());
  m_handler->OnServiceNotification(
      [this](DWORD notification) {
        tray_icon.ShowServiceNotification(notification);
      });
  tray_icon.Refresh();
  tray_icon.ShowServiceNotification(m_startup_notification);
  WeaselDebugLog(L"WeaselServerApp", L"tray initialized");

  int ret = m_server.Run();
  WeaselDebugLog(L"WeaselServerApp", L"m_server.Run returned " +
                                         std::to_wstring(ret));

  m_handler->Finalize();
  m_ui.Destroy();
  tray_icon.RemoveIcon();
  win_sparkle_cleanup();

  return ret;
}

void WeaselServerApp::SetupMenuHandlers() {
  std::filesystem::path dir = install_dir();
  m_server.AddMenuHandler(ID_WEASELTRAY_QUIT, [this] {
    WeaselDebugLog(L"WeaselServerApp", L"tray quit requested");
    weasel::MarkServiceManualExit();
    UnregisterApplicationRestart();
    tray_icon.ShowServiceNotification(
        weasel::WEASEL_IPC_SERVICE_NOTIFICATION_EXITING);
    std::thread([this] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      m_server.Stop();
    }).detach();
    return true;
  });
  m_server.AddMenuHandler(ID_WEASELTRAY_DEPLOY,
                          std::bind(execute, dir / L"WeaselDeployer.exe",
                                    std::wstring(L"/deploy")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SETTINGS,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring()));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_DICT_MANAGEMENT,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/dict")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SYNC,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/sync")));
  m_server.AddMenuHandler(ID_WEASELTRAY_WIKI,
                          std::bind(open, L"https://rime.im/docs/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_HOMEPAGE,
                          std::bind(open, L"https://rime.im/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_FORUM,
                          std::bind(open, L"https://rime.im/discuss/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_CHECKUPDATE, check_update);
  m_server.AddMenuHandler(ID_WEASELTRAY_INSTALLDIR, std::bind(explore, dir));
  m_server.AddMenuHandler(ID_WEASELTRAY_USERCONFIG,
                          std::bind(explore, WeaselUserDataPath()));
  m_server.AddMenuHandler(ID_WEASELTRAY_LOGDIR,
                          std::bind(explore, WeaselLogPath()));
}
