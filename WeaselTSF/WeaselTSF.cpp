#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <shellapi.h>
#include <tlhelp32.h>
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "LanguageBar.h"
#include "Compartment.h"
#include "ResponseParser.h"

static std::wstring ModuleFileName(HMODULE module) {
  WCHAR path[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameW(module, path, _countof(path));
  if (length == 0)
    return L"";
  return path;
}

static void error_message(const WCHAR* msg) {
  static DWORD next_tick = 0;
  DWORD now = GetTickCount();
  if (now > next_tick) {
    next_tick = now + 10000;  // (ms)
    MessageBox(NULL, msg, get_weasel_ime_name().c_str(), MB_ICONERROR | MB_OK);
  }
}

WeaselTSF::WeaselTSF() {
  _cRef = 1;

  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;

  _dwTextEditSinkCookie = TF_INVALID_COOKIE;
  _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
  _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;
  _fTestKeyDownPending = FALSE;
  _fTestKeyUpPending = FALSE;

  _fCUASWorkaroundTested = _fCUASWorkaroundEnabled = FALSE;

  _cand = new CCandidateList(this);

  DllAddRef();
  WeaselDebugLog(L"WeaselTSF", L"constructed module=" +
                                   ModuleFileName(g_hInst) + L" host=" +
                                   ModuleFileName(NULL));
}

WeaselTSF::~WeaselTSF() {
  WeaselDebugLog(L"WeaselTSF", L"destroyed");
  DllRelease();
}

STDMETHODIMP WeaselTSF::QueryInterface(REFIID riid, void** ppvObject) {
  if (ppvObject == NULL)
    return E_INVALIDARG;

  *ppvObject = NULL;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppvObject = (ITfTextInputProcessor*)this;
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppvObject = (ITfTextInputProcessorEx*)this;
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppvObject = (ITfThreadMgrEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextEditSink))
    *ppvObject = (ITfTextEditSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    *ppvObject = (ITfTextLayoutSink*)this;
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppvObject = (ITfKeyEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppvObject = (ITfCompositionSink*)this;
  else if (IsEqualIID(riid, IID_ITfEditSession))
    *ppvObject = (ITfEditSession*)this;
  else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    *ppvObject = (ITfThreadFocusSink*)this;
  else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    *ppvObject = (ITfDisplayAttributeProvider*)this;

  if (*ppvObject) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) WeaselTSF::AddRef() {
  return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) WeaselTSF::Release() {
  LONG cr = InterlockedDecrement(&_cRef);

  assert(cr >= 0);

  if (cr == 0)
    delete this;

  return cr;
}

STDMETHODIMP WeaselTSF::Activate(ITfThreadMgr* pThreadMgr,
                                 TfClientId tfClientId) {
  WeaselDebugLog(L"WeaselTSF",
                 L"Activate client_id=" + std::to_wstring(tfClientId));
  return ActivateEx(pThreadMgr, tfClientId, 0U);
}

STDMETHODIMP WeaselTSF::Deactivate() {
  WeaselDebugLog(L"WeaselTSF", L"Deactivate start");
  m_client.EndSession();

  _InitTextEditSink(com_ptr<ITfDocumentMgr>());

  _UninitThreadMgrEventSink();

  _UninitKeyEventSink();
  _UninitPreservedKey();

  _UninitLanguageBar();

  _UninitCompartment();

  _UninitThreadMgrEventSink();

  _pThreadMgr = NULL;

  _tfClientId = TF_CLIENTID_NULL;

  _cand->DestroyAll();

  WeaselDebugLog(L"WeaselTSF", L"Deactivate done");
  return S_OK;
}

static void fake_key() {
  INPUT inputs[2];
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki = {VK_SELECT, 0, 0, 0, 0};
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki = {VK_SELECT, 0, KEYEVENTF_KEYUP, 0, 0};
  ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
}

STDMETHODIMP WeaselTSF::ActivateEx(ITfThreadMgr* pThreadMgr,
                                   TfClientId tfClientId,
                                   DWORD dwFlags) {
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  _activateFlags = dwFlags;

  WeaselDebugLog(L"WeaselTSF", L"ActivateEx start client_id=" +
                                   std::to_wstring(tfClientId) + L" flags=" +
                                   std::to_wstring(dwFlags) + L" module=" +
                                   ModuleFileName(g_hInst) + L" host=" +
                                   ModuleFileName(NULL));

  _pThreadMgr = pThreadMgr;
  _tfClientId = tfClientId;

  if (!_InitThreadMgrEventSink()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitThreadMgrEventSink");
    goto ExitError;
  }

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    _InitTextEditSink(pDocMgrFocus);
  }

  if (!_InitKeyEventSink()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitKeyEventSink");
    goto ExitError;
  }

  // if (!_InitDisplayAttributeGuidAtom())
  //	goto ExitError;
  //	some app might init failed because it not provide DisplayAttributeInfo,
  // like some opengl stuff
  _InitDisplayAttributeGuidAtom();

  if (!_InitPreservedKey()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitPreservedKey");
    goto ExitError;
  }

  fake_key();
  if (!_InitLanguageBar()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitLanguageBar");
    goto ExitError;
  }

  if (!_IsKeyboardOpen())
    _SetKeyboardOpen(TRUE);

  if (!_InitCompartment()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitCompartment");
    goto ExitError;
  }
  if (!_InitThreadFocusSink()) {
    WeaselDebugLog(L"WeaselTSF", L"ActivateEx failed: _InitThreadFocusSink");
    goto ExitError;
  }

  _EnsureServerConnected();

  WeaselDebugLog(L"WeaselTSF", L"ActivateEx succeeded");
  return S_OK;

ExitError:
  WeaselDebugLog(L"WeaselTSF", L"ActivateEx returning E_FAIL");
  Deactivate();
  return E_FAIL;
}

STDMETHODIMP WeaselTSF::OnSetThreadFocus() {
  std::wstring _ToggleImeOnOpenClose{};
  RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                    L"ToggleImeOnOpenClose", _ToggleImeOnOpenClose);
  _isToOpenClose = (_ToggleImeOnOpenClose == L"yes");
  if (_EnsureServerConnected()) {
    bool eaten = false;
    m_client.ProcessKeyEvent(0, &eaten);
    weasel::ResponseParser parser(NULL, NULL, &_status, &_config,
                                  &_cand->style());
    bool ok = m_client.GetResponseData(std::ref(parser));
    if (ok)
      _UpdateLanguageBar(_status);
    _ShowLanguageBar(!_config.hide_ime_mode_icon);
  }
  return S_OK;
}
STDMETHODIMP WeaselTSF::OnKillThreadFocus() {
  _AbortComposition();
  return S_OK;
}
BOOL WeaselTSF::_InitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return FALSE;
  if (FAILED(pSource->AdviseSink(IID_ITfThreadFocusSink,
                                 (ITfThreadFocusSink*)this,
                                 &_dwThreadFocusSinkCookie)))
    return FALSE;
  return TRUE;
}
void WeaselTSF::_UninitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return;
  if (FAILED(pSource->UnadviseSink(_dwThreadFocusSinkCookie)))
    return;
}

STDMETHODIMP WeaselTSF::OnActivated(REFCLSID clsid,
                                    REFGUID guidProfile,
                                    BOOL isActivated) {
  if (!IsEqualCLSID(clsid, c_clsidTextService)) {
    return S_OK;
  }

  WeaselDebugLog(L"WeaselTSF",
                 L"OnActivated isActivated=" + std::to_wstring(isActivated));
  if (isActivated) {
    _ShowLanguageBar(!_config.hide_ime_mode_icon);
    _UpdateLanguageBar(_status);
  } else {
    _DeleteCandidateList();
    _ShowLanguageBar(FALSE);
  }
  return S_OK;
}

bool WeaselTSF::_Reconnect(bool update_tsf_status, bool wait_for_pipe) {
  if (!m_client.Reconnect(NULL, wait_for_pipe))
    return false;
  _keyEventTestCacheReset.Mark();
  if (!update_tsf_status)
    return true;
  weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
  bool ok = m_client.GetResponseData(std::ref(parser));
  if (ok) {
    _UpdateLanguageBar(_status);
  }
  return true;
}

void WeaselTSF::_RecoverServerAsync() {
  if (!weasel::ShouldAutoRecoverService()) {
    WeaselDebugLog(L"WeaselTSF", L"skip async recovery: manual exit marked");
    return;
  }

  if (InterlockedCompareExchange(&_recoveringServer, 1, 0) != 0)
    return;

  AddRef();
  try {
    std::thread th([this]() {
      try {
        _EnsureServerConnected(false);
      } catch (...) {
      }
      InterlockedExchange(&_recoveringServer, 0);
      Release();
    });
    th.detach();
  } catch (...) {
    InterlockedExchange(&_recoveringServer, 0);
    Release();
  }
}

bool WeaselTSF::_EnsureServerConnected(bool update_tsf_status) {
  if (!weasel::ShouldAutoRecoverService()) {
    WeaselDebugLog(L"WeaselTSF", L"skip recovery: manual exit marked");
    return false;
  }

  if (!m_client.Echo()) {
    _Reconnect(update_tsf_status, false);
    if (++_serverRecoveryRetry >= 6) {
      HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerExclusiveMutex");
      DWORD mutex_error = GetLastError();
      const auto count_server_process = []() -> int {
        int count = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
          return 0;
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
          do {
            if (_wcsicmp(pe.szExeFile, L"WeaselServer.exe") == 0)
              count++;
          } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return count;
      };
      if (!m_client.Echo() && mutex_error != ERROR_ALREADY_EXISTS &&
          !count_server_process()) {
        if (!weasel::ShouldAutoRecoverService()) {
          WeaselDebugLog(L"WeaselTSF",
                         L"skip launch during recovery: manual exit marked");
          if (hMutex) {
            CloseHandle(hMutex);
          }
          _serverRecoveryRetry.store(0);
          return false;
        }
        std::wstring dir = _GetRootDir();
        auto restart_server = [dir, this]() {
          if (!weasel::ShouldAutoRecoverService()) {
            WeaselDebugLog(L"WeaselTSF",
                           L"skip recovery thread launch: manual exit marked");
            return;
          }
          if (dir.empty())
            return;
          std::wstring server = weasel::ServiceExecutablePath(dir);
          ShellExecuteW(NULL, L"open", server.c_str(),
                        weasel::ServiceRecoveryArgument(), dir.c_str(),
                        SW_HIDE);
          for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (_Reconnect(false, false) && m_client.Echo()) {
              m_client.NotifyService(
                  weasel::WEASEL_IPC_SERVICE_NOTIFICATION_RESTART_SUCCESS);
              break;
            }
          }
        };
        if (update_tsf_status) {
          AddRef();
          try {
            std::thread th([restart_server, this]() {
              try {
                restart_server();
              } catch (...) {
              }
              Release();
            });
            th.detach();
          } catch (...) {
            Release();
          }
        } else {
          restart_server();
        }
      }
      if (hMutex) {
        CloseHandle(hMutex);
      }
      _serverRecoveryRetry.store(0);
    }
    return (m_client.Echo() != 0);
  } else {
    _serverRecoveryRetry.store(0);
    return true;
  }
}
