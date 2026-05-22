#pragma once
#include <WeaselUI.h>
#include <WeaselIPC.h>
#include "SystemTraySDK.h"
#include "resource.h"

#define WM_WEASEL_TRAY_NOTIFY (WEASEL_IPC_LAST_COMMAND + 100)

inline UINT ServiceNotificationStringId(DWORD notification) {
  if (notification == weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_SUCCESS)
    return IDS_STR_DEPLOY_SUCCESS;
  if (notification == weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE)
    return IDS_STR_DEPLOY_FAILURE;
  if (notification == weasel::WEASEL_IPC_SERVICE_NOTIFICATION_EXITING)
    return IDS_STR_SERVICE_EXITING;
  if (notification == weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTARTING)
    return IDS_STR_SERVICE_RESTARTING;
  if (notification == weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS)
    return IDS_STR_SERVICE_RESTART_SUCCESS;
  if (notification == weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE)
    return IDS_STR_SERVICE_RESTART_FAILURE;
  return 0;
}

inline DWORD ServiceNotificationBalloonIcon(DWORD notification) {
  return notification == weasel::WEASEL_IPC_MAINTENANCE_DEPLOY_FAILURE ||
                 notification ==
                     weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_FAILURE
             ? NIIF_ERROR
             : NIIF_INFO;
}

inline UINT MaintenanceDeployResultStringId(DWORD result) {
  return ServiceNotificationStringId(result);
}

inline DWORD MaintenanceDeployResultBalloonIcon(DWORD result) {
  return ServiceNotificationBalloonIcon(result);
}

class WeaselTrayIcon : public CSystemTray {
 public:
  enum WeaselTrayMode {
    INITIAL,
    ZHUNG,
    ASCII,
    DISABLED,
  };

  WeaselTrayIcon(weasel::UI& ui);

  BOOL Create(HWND hTargetWnd);
  void Refresh();
  void ShowMaintenanceResult(DWORD result);
  void ShowServiceNotification(DWORD notification);

 protected:
  virtual void CustomizeMenu(HMENU hMenu);

  weasel::UIStyle& m_style;
  weasel::Status& m_status;
  WeaselTrayMode m_mode;
  std::wstring m_schema_zhung_icon;
  std::wstring m_schema_ascii_icon;
  bool m_disabled;
};
