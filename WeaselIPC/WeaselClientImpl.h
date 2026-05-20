#pragma once
#include <WeaselIPC.h>
#include <PipeChannel.h>

namespace weasel {

class ClientImpl {
 public:
  ClientImpl();
  ~ClientImpl();

  bool Connect(ServerLauncher const& launcher);
  void Disconnect();
  void ShutdownServer();
  void StartSession();
  void EndSession();
  void StartMaintenance();
  void EndMaintenance();
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
  bool GetResponseData(ResponseHandler const& handler);

 protected:
  void _InitializeClientInfo();
  bool _WriteClientInfo();

  LRESULT _SendMessage(WEASEL_IPC_COMMAND Msg, DWORD wParam, DWORD lParam);
  bool _TrySendMessage(WEASEL_IPC_COMMAND Msg,
                       DWORD wParam,
                       DWORD lParam,
                       DWORD* result);

  bool _Connected() const { return channel.Connected(); }
  bool _Active() const { return channel.Connected() && session_id != 0; }

 private:
  UINT session_id;
  std::wstring app_name;
  bool is_ime;
  bool has_input_position;
  DWORD last_input_position;

  PipeChannel<PipeMessage> channel;
};
}  // namespace weasel
