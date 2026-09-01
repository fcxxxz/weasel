#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <map>
#include <string>
#include <mutex>

#include <rime_api.h>

struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

struct SessionStatus {
  SessionStatus() : style(weasel::UIStyle()), __synced(false), session_id(0) {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
  // Last candidate payload handed to this session; unchanged lists are not
  // re-serialized into responses.
  weasel::CandidateInfo last_cinfo;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;

struct RimeTrayIconSignature {
  RimeTrayIconSignature()
      : valid(false),
        display_tray_icon(false),
        disabled(false),
        ascii_mode(false) {}

  static RimeTrayIconSignature From(const weasel::UIStyle& style,
                                    const weasel::Status& status) {
    RimeTrayIconSignature signature;
    signature.valid = true;
    signature.display_tray_icon = style.display_tray_icon;
    signature.disabled = status.disabled;
    signature.ascii_mode = status.ascii_mode;
    signature.current_zhung_icon = style.current_zhung_icon;
    signature.current_ascii_icon = style.current_ascii_icon;
    return signature;
  }

  bool operator==(const RimeTrayIconSignature& rhs) const {
    return valid == rhs.valid && display_tray_icon == rhs.display_tray_icon &&
           disabled == rhs.disabled && ascii_mode == rhs.ascii_mode &&
           current_zhung_icon == rhs.current_zhung_icon &&
           current_ascii_icon == rhs.current_ascii_icon;
  }

  bool operator!=(const RimeTrayIconSignature& rhs) const {
    return !(*this == rhs);
  }

  bool valid;
  bool display_tray_icon;
  bool disabled;
  bool ascii_mode;
  std::wstring current_zhung_icon;
  std::wstring current_ascii_icon;
};

struct RimeUiStatusSnapshot {
  RimeUiStatusSnapshot() : has_status(false) {}

  static RimeUiStatusSnapshot From(const RimeStatus& rime_status) {
    RimeUiStatusSnapshot snapshot;
    const char* schema_id = rime_status.schema_id ? rime_status.schema_id : "";
    const char* schema_name =
        rime_status.schema_name ? rime_status.schema_name : "";
    snapshot.has_status = true;
    snapshot.schema_id = schema_id;
    snapshot.status.schema_name = u8tow(schema_name);
    snapshot.status.schema_id = u8tow(schema_id);
    snapshot.status.ascii_mode = !!rime_status.is_ascii_mode;
    snapshot.status.composing = !!rime_status.is_composing;
    snapshot.status.disabled = !!rime_status.is_disabled;
    snapshot.status.full_shape = !!rime_status.is_full_shape;
    return snapshot;
  }

  bool has_status;
  std::string schema_id;
  weasel::Status status;
};

inline bool RimeUiNeedsUpdate(const weasel::Context& current_context,
                              const weasel::Status& current_status,
                              const weasel::Context& next_context,
                              const weasel::Status& next_status) {
  return current_context != next_context || !(current_status == next_status);
}

inline bool ShouldSuppressInlineOptionNotification(
    const std::string& message_type,
    const std::string& message_value) {
  return message_type == "option" &&
         (message_value == "ascii_mode" || message_value == "!ascii_mode");
}

void LoadWeaselUIStyle(RimeConfig* config,
                       weasel::UIStyle& style,
                       bool initialize = true,
                       const std::string& color_scheme = std::string());

class RimeWithWeaselHandler : public weasel::RequestHandler {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance(
      DWORD result = weasel::WEASEL_IPC_MAINTENANCE_RESULT_NONE);
  virtual void NotifyService(DWORD notification);
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);

  void OnUpdateUI(std::function<void()> const& cb);
  void OnMaintenanceResult(std::function<void(DWORD)> const& cb);
  void OnServiceNotification(std::function<void(DWORD)> const& cb);

 private:
  void _Setup();
  bool _IsDeployerRunning();
  void _UpdateUI(WeaselSessionId ipc_id,
                 const RimeUiStatusSnapshot* status_snapshot = nullptr);
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx, weasel::Status& status);
  bool _Respond(WeaselSessionId ipc_id,
                EatLine eat,
                RimeUiStatusSnapshot* status_snapshot = nullptr,
                bool* has_commit = nullptr);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo, RimeContext& ctx);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _ApplyStatusSnapshot(weasel::Status& stat,
                            WeaselSessionId ipc_id,
                            weasel::Context& ctx,
                            const RimeUiStatusSnapshot& snapshot);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);

  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);
  void _RefreshTrayIconIfNeeded(RimeSessionId session_id);
  void _InvalidateTrayIconSignature();
  void _SetDeployMessage(DWORD result);

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id].session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id];
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id] = SessionStatus();
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  bool m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;
  std::function<void(DWORD)> _ServiceNotificationCallback;

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  static std::mutex m_notifier_mutex;
  SessionStatusMap m_session_status_map;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  DWORD m_pid;
  RimeTrayIconSignature m_tray_icon_signature;
};
