#pragma once
#include <WeaselIPC.h>
#include <PipeChannel.h>
#include <mutex>

namespace weasel {

struct InputPositionCache {
  InputPositionCache() : has_position(false), last_position(0) {}

  bool ShouldSend(DWORD position) const {
    return !has_position || last_position != position;
  }

  void MarkSent(DWORD position) {
    has_position = true;
    last_position = position;
  }

  void Reset() {
    has_position = false;
    last_position = 0;
  }

  bool has_position;
  DWORD last_position;
};

class ClientImpl {
 public:
  explicit ClientImpl(std::wstring pipe_name = GetPipeName());
  ~ClientImpl();

  bool Connect(ServerLauncher const& launcher);
  bool TryConnect();
  bool Reconnect(ServerLauncher const& launcher, bool wait_for_pipe);
  void Disconnect();
  void ShutdownServer(DWORD reason);
  void NotifyService(DWORD notification);
  void StartSession();
  void EndSession();
  void StartMaintenance();
  void EndMaintenance(DWORD result = WEASEL_IPC_MAINTENANCE_RESULT_NONE);
  bool Echo();
  bool ProcessKeyEvent(KeyEvent const& keyEvent);
  bool ProcessKeyEvent(KeyEvent const& keyEvent, bool* eaten);
  bool CommitComposition();
  bool ClearComposition();
  bool SelectCandidateOnCurrentPage(size_t index);
  bool HighlightCandidateOnCurrentPage(size_t index);
  bool ChangePage(bool backward);
  void UpdateInputPosition(RECT const& rc);
  void FocusIn();
  void FocusOut();
  void TrayCommand(UINT menuId);
  bool TrayCommandSync(UINT menuId);
  bool GetResponseData(ResponseHandler const& handler);

 protected:
  void _InitializeClientInfo();
  bool _WriteClientInfo();

  LRESULT _SendMessage(WEASEL_IPC_COMMAND Msg, DWORD wParam, DWORD lParam);
  bool _TrySendMessage(WEASEL_IPC_COMMAND Msg,
                       DWORD wParam,
                       DWORD lParam,
                       DWORD* result);
  bool _StartSessionLocked(bool wait_for_pipe);

  bool _Connected() const { return channel.Connected(); }
  bool _Active() const { return channel.Connected() && session_id != 0; }

 private:
  UINT session_id;
  std::wstring app_name;
  bool is_ime;
  InputPositionCache input_position_cache;
  std::recursive_mutex client_mutex;

  PipeChannel<PipeMessage> channel;
};
}  // namespace weasel
