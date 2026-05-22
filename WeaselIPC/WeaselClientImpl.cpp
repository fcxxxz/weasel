#include "stdafx.h"
#include "WeaselClientImpl.h"
#include <StringAlgorithm.hpp>
#include <utility>

using namespace weasel;

ClientImpl::ClientImpl(std::wstring pipe_name)
    : session_id(0),
      channel(std::move(pipe_name)),
      is_ime(false),
      input_position_cache() {
  _InitializeClientInfo();
}

ClientImpl::~ClientImpl() {
  if (channel.Connected())
    Disconnect();
}

// http://stackoverflow.com/questions/557081/how-do-i-get-the-hmodule-for-the-currently-executing-code
HMODULE GetCurrentModule() {  // NB: XP+ solution!
  HMODULE hModule = NULL;
  GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    (LPCTSTR)GetCurrentModule, &hModule);

  return hModule;
}

void ClientImpl::_InitializeClientInfo() {
  // get app name
  WCHAR exe_path[MAX_PATH] = {0};
  GetModuleFileName(NULL, exe_path, MAX_PATH);
  std::wstring path = exe_path;
  size_t separator_pos = path.find_last_of(L"\\/");
  if (separator_pos < path.size())
    app_name = path.substr(separator_pos + 1);
  else
    app_name = path;
  to_lower(app_name);
  // determine client type
  GetModuleFileName(GetCurrentModule(), exe_path, MAX_PATH);
  path = exe_path;
  to_lower(path);
  is_ime = ends_with(path, L".ime");
}

bool ClientImpl::Connect(ServerLauncher const& launcher) {
  std::lock_guard<std::recursive_mutex> lock(client_mutex);
  if (channel.Connected())
    return true;
  if (channel.TryConnect())
    return true;
  if (launcher)
    launcher();
  return channel.Connect();
}

bool ClientImpl::TryConnect() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (channel.Connected())
    return true;
  return channel.TryConnect();
}

bool ClientImpl::Reconnect(ServerLauncher const& launcher, bool wait_for_pipe) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;

  if (_Active()) {
    DWORD ignored = 0;
    _TrySendMessage(WEASEL_IPC_END_SESSION, 0, session_id, &ignored);
  }
  session_id = 0;
  input_position_cache.Reset();
  channel.Disconnect();

  if (wait_for_pipe) {
    if (!channel.TryConnect() && launcher)
      launcher();
    if (!channel.Connect())
      return false;
  } else if (!channel.TryConnect()) {
    return false;
  }

  return _StartSessionLocked(wait_for_pipe);
}

void ClientImpl::Disconnect() {
  std::lock_guard<std::recursive_mutex> lock(client_mutex);
  if (_Active())
    EndSession();
  channel.Disconnect();
  input_position_cache.Reset();
}

void ClientImpl::ShutdownServer(DWORD reason) {
  std::lock_guard<std::recursive_mutex> lock(client_mutex);
  _SendMessage(WEASEL_IPC_SHUTDOWN_SERVER, reason, 0);
}

void ClientImpl::NotifyService(DWORD notification) {
  std::lock_guard<std::recursive_mutex> lock(client_mutex);
  _SendMessage(WEASEL_IPC_NOTIFY_SERVICE, notification, 0);
}

bool ClientImpl::ProcessKeyEvent(KeyEvent const& keyEvent) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;

  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_PROCESS_KEY_EVENT, keyEvent, session_id,
                         &result) &&
         result != 0;
}

bool ClientImpl::ProcessKeyEvent(KeyEvent const& keyEvent, bool* eaten) {
  if (eaten)
    *eaten = false;
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (session_id == 0)
    return false;

  DWORD result = 0;
  if (!_TrySendMessage(WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS, keyEvent,
                       session_id, &result))
    return false;
  if (!IsKeyEventResultProcessed(result))
    return false;
  if (eaten)
    *eaten = IsKeyEventResultEaten(result);
  return true;
}

bool ClientImpl::CommitComposition() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;

  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_COMMIT_COMPOSITION, 0, session_id,
                         &result) &&
         result != 0;
}

bool ClientImpl::ClearComposition() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;

  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_CLEAR_COMPOSITION, 0, session_id,
                         &result) &&
         result != 0;
}

bool ClientImpl::SelectCandidateOnCurrentPage(size_t index) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;
  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
                         static_cast<DWORD>(index), session_id, &result) &&
         result != 0;
}

bool ClientImpl::HighlightCandidateOnCurrentPage(size_t index) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;
  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                         static_cast<DWORD>(index), session_id, &result) &&
         result != 0;
}

bool ClientImpl::ChangePage(bool backward) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;
  DWORD result = 0;
  return _TrySendMessage(WEASEL_IPC_CHANGE_PAGE, backward, session_id,
                         &result) &&
         result != 0;
}

void ClientImpl::UpdateInputPosition(RECT const& rc) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (!_Active())
    return;
  /*
  移位标志 = 1bit == 0
  height:0~127 = 7bit
  top:-2048~2047 = 12bit（有符号）
  left:-2048~2047 = 12bit（有符号）

  高解析度下：
  移位标志 = 1bit == 1
  height:0~254 = 7bit（舍弃低1位）
  top:-4096~4094 = 12bit（有符号，舍弃低1位）
  left:-4096~4094 = 12bit（有符号，舍弃低1位）
  */
  int hi_res =
      static_cast<int>(rc.bottom - rc.top >= 128 || rc.left < -2048 ||
                       rc.left >= 2048 || rc.top < -2048 || rc.top >= 2048);
  int left = max(-2048, min(2047, rc.left >> hi_res));
  int top = max(-2048, min(2047, rc.top >> hi_res));
  int height = max(0, min(127, (rc.bottom - rc.top) >> hi_res));
  DWORD compressed_rect = ((hi_res & 0x01) << 31) | ((height & 0x7f) << 24) |
                          ((top & 0xfff) << 12) | (left & 0xfff);
  if (!input_position_cache.ShouldSend(compressed_rect))
    return;

  DWORD result = 0;
  if (_TrySendMessage(WEASEL_IPC_UPDATE_INPUT_POS, compressed_rect, session_id,
                      &result)) {
    input_position_cache.MarkSent(compressed_rect);
  }
}

void ClientImpl::FocusIn() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  DWORD client_caps = 0; /* TODO */
  DWORD result = 0;
  _TrySendMessage(WEASEL_IPC_FOCUS_IN, client_caps, session_id, &result);
}

void ClientImpl::FocusOut() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  DWORD result = 0;
  _TrySendMessage(WEASEL_IPC_FOCUS_OUT, 0, session_id, &result);
}

void ClientImpl::TrayCommand(UINT menuId) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    WeaselDebugLog(L"WeaselClient", L"TrayCommand skipped: busy menuId=" +
                                        std::to_wstring(menuId));
    return;
  }
  DWORD result = 0;
  bool ok =
      _TrySendMessage(WEASEL_IPC_TRAY_COMMAND, menuId, session_id, &result);
  WeaselDebugLog(L"WeaselClient",
                 L"TrayCommand menuId=" + std::to_wstring(menuId) +
                     L" session_id=" + std::to_wstring(session_id) +
                     L" connected=" + std::to_wstring(channel.Connected()) +
                     L" ok=" + std::to_wstring(ok) + L" result=" +
                     std::to_wstring(result));
}

bool ClientImpl::TrayCommandSync(UINT menuId) {
  std::lock_guard<std::recursive_mutex> lock(client_mutex);
  DWORD result = 0;
  bool ok =
      _TrySendMessage(WEASEL_IPC_TRAY_COMMAND, menuId, session_id, &result);
  if (!ok) {
    channel.Disconnect();
    if (channel.TryConnect())
      ok =
          _TrySendMessage(WEASEL_IPC_TRAY_COMMAND, menuId, session_id, &result);
  }
  WeaselDebugLog(L"WeaselClient",
                 L"TrayCommandSync menuId=" + std::to_wstring(menuId) +
                     L" session_id=" + std::to_wstring(session_id) +
                     L" connected=" + std::to_wstring(channel.Connected()) +
                     L" ok=" + std::to_wstring(ok) + L" result=" +
                     std::to_wstring(result));
  return ok;
}

void ClientImpl::StartSession() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (_Active() && Echo())
    return;

  _StartSessionLocked(false);
}

void ClientImpl::EndSession() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  DWORD result = 0;
  _TrySendMessage(WEASEL_IPC_END_SESSION, 0, session_id, &result);
  session_id = 0;
  input_position_cache.Reset();
}

void ClientImpl::StartMaintenance() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  _SendMessage(WEASEL_IPC_START_MAINTENANCE, 0, 0);
  session_id = 0;
  input_position_cache.Reset();
}

void ClientImpl::EndMaintenance(DWORD result) {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  _SendMessage(WEASEL_IPC_END_MAINTENANCE, result, 0);
  session_id = 0;
  input_position_cache.Reset();
}

bool ClientImpl::Echo() {
  std::unique_lock<std::recursive_mutex> lock(client_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (!_Active())
    return false;

  DWORD serverEcho = 0;
  if (!_TrySendMessage(WEASEL_IPC_ECHO, 0, session_id, &serverEcho))
    return false;
  return (serverEcho == session_id);
}

bool ClientImpl::GetResponseData(ResponseHandler const& handler) {
  if (!handler) {
    return false;
  }

  // The response buffer is thread-local in PipeChannel. Parsing it must not be
  // blocked by a background recovery thread that uses another thread's pipe.
  return channel.HandleResponseData(handler);
}

bool ClientImpl::_StartSessionLocked(bool wait_for_pipe) {
  input_position_cache.Reset();
  _WriteClientInfo();
  DWORD ret = 0;
  if (wait_for_pipe) {
    ret = static_cast<DWORD>(_SendMessage(WEASEL_IPC_START_SESSION, 0, 0));
  } else if (!_TrySendMessage(WEASEL_IPC_START_SESSION, 0, 0, &ret)) {
    session_id = 0;
    return false;
  }
  session_id = ret;
  return session_id != 0;
}

bool ClientImpl::_WriteClientInfo() {
  channel << L"action=session\n";
  channel << L"session.client_app=" << app_name.c_str() << L"\n";
  channel << L"session.client_type=" << (is_ime ? L"ime" : L"tsf") << L"\n";
  channel << L".\n";
  return true;
}

LRESULT ClientImpl::_SendMessage(WEASEL_IPC_COMMAND Msg,
                                 DWORD wParam,
                                 DWORD lParam) {
  try {
    PipeMessage req{Msg, wParam, lParam};
    return channel.Transact(req);
  } catch (DWORD /* ex */) {
    return 0;
  }
}

bool ClientImpl::_TrySendMessage(WEASEL_IPC_COMMAND Msg,
                                 DWORD wParam,
                                 DWORD lParam,
                                 DWORD* result) {
  if (!result)
    return false;
  PipeMessage req{Msg, wParam, lParam};
  return channel.TryTransact(req, result);
}

Client::Client() : m_pImpl(new ClientImpl()) {}

Client::~Client() {
  if (m_pImpl)
    delete m_pImpl;
}

bool Client::Connect(ServerLauncher launcher) {
  return m_pImpl->Connect(launcher);
}

bool Client::TryConnect() {
  return m_pImpl->TryConnect();
}

bool Client::Reconnect(ServerLauncher launcher, bool wait_for_pipe) {
  return m_pImpl->Reconnect(launcher, wait_for_pipe);
}

void Client::Disconnect() {
  m_pImpl->Disconnect();
}

void Client::ShutdownServer(DWORD reason) {
  m_pImpl->ShutdownServer(reason);
}

void Client::NotifyService(DWORD notification) {
  m_pImpl->NotifyService(notification);
}

bool Client::ProcessKeyEvent(KeyEvent const& keyEvent) {
  return m_pImpl->ProcessKeyEvent(keyEvent);
}

bool Client::ProcessKeyEvent(KeyEvent const& keyEvent, bool* eaten) {
  return m_pImpl->ProcessKeyEvent(keyEvent, eaten);
}

bool Client::CommitComposition() {
  return m_pImpl->CommitComposition();
}

bool Client::ClearComposition() {
  return m_pImpl->ClearComposition();
}

bool Client::SelectCandidateOnCurrentPage(size_t index) {
  return m_pImpl->SelectCandidateOnCurrentPage(index);
}

bool Client::HighlightCandidateOnCurrentPage(size_t index) {
  return m_pImpl->HighlightCandidateOnCurrentPage(index);
}

bool Client::ChangePage(bool backward) {
  return m_pImpl->ChangePage(backward);
}

void Client::UpdateInputPosition(RECT const& rc) {
  m_pImpl->UpdateInputPosition(rc);
}

void Client::FocusIn() {
  m_pImpl->FocusIn();
}

void Client::FocusOut() {
  m_pImpl->FocusOut();
}

void Client::StartSession() {
  m_pImpl->StartSession();
}

void Client::EndSession() {
  m_pImpl->EndSession();
}

void Client::StartMaintenance() {
  m_pImpl->StartMaintenance();
}

void Client::EndMaintenance(DWORD result) {
  m_pImpl->EndMaintenance(result);
}

void Client::TrayCommand(UINT menuId) {
  m_pImpl->TrayCommand(menuId);
}

bool Client::TrayCommandSync(UINT menuId) {
  return m_pImpl->TrayCommandSync(menuId);
}

bool Client::Echo() {
  return m_pImpl->Echo();
}

bool Client::GetResponseData(ResponseHandler handler) {
  return m_pImpl->GetResponseData(handler);
}
