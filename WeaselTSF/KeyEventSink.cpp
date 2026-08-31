#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"
#include "ResponseParser.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;
static const DWORD kManualExitKeyCheckIntervalMs = 100;

ibus::Keycode TranslateKeycode(UINT vkey, KeyInfo kinfo);

static std::wstring CurrentHostPath() {
  WCHAR path[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameW(NULL, path, _countof(path));
  if (length == 0)
    return L"";
  return path;
}

static std::wstring KeyEventTracePrefix(const wchar_t* callback,
                                        WPARAM wParam,
                                        LPARAM lParam) {
  KeyInfo info(lParam);
  return std::wstring(callback) + L" vk=" + std::to_wstring(wParam) +
         L" lparam=" + std::to_wstring(static_cast<UINT32>(lParam)) +
         L" identity=" +
         std::to_wstring(weasel::KeyEventTestCache::ComparableLParam(lParam)) +
         L" repeat=" + std::to_wstring(info.repeatCount) + L" scan=" +
         std::to_wstring(info.scanCode) + L" ext=" +
         std::to_wstring(info.isExtended) + L" context=" +
         std::to_wstring(info.contextCode) + L" prev=" +
         std::to_wstring(info.prevKeyState) + L" up=" +
         std::to_wstring(info.isKeyUp);
}

static void TraceKeyEvent(const wchar_t* callback,
                          WPARAM wParam,
                          LPARAM lParam,
                          const std::wstring& message) {
  if (!ShouldTraceKeyEvents())
    return;
  WeaselDebugLog(L"KeyEvent", KeyEventTracePrefix(callback, wParam, lParam) +
                                  L" " + message);
}

#define TRACE_KEY_EVENT(callback, wParam, lParam, message) \
  do {                                                     \
    if (ShouldTraceKeyEvents())                            \
      TraceKeyEvent(callback, wParam, lParam, message);    \
  } while (0)

bool WeaselTSF::_TestKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  *pfEaten = FALSE;
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    TRACE_KEY_EVENT(L"Test", wParam, lParam,
                    L"skip disabled_or_keyboard_closed=1");
    return false;
  }
  bool service_available = _CanHandleKeyEvent();
  if (!service_available) {
    TRACE_KEY_EVENT(L"Test", wParam, lParam,
                    L"skip manual_exit_or_recovery_disabled=1");
    return false;
  }

  KeyInfo key_info(lParam);
  bool known_key =
      TranslateKeycode(static_cast<UINT>(wParam), key_info) != ibus::Null ||
      weasel::IsTextVirtualKey(wParam);
  if (!known_key) {
    TRACE_KEY_EVENT(L"Test", wParam, lParam, L"known_key=0");
    return false;
  }

  BYTE key_state[256] = {0};
  GetKeyboardState(key_state);
  bool shortcut_modifier =
      (key_state[VK_CONTROL] & 0x80) != 0 || (key_state[VK_MENU] & 0x80) != 0;
  bool predicted_eaten = weasel::ShouldEatTestKeyEvent(
      service_available, _status.ascii_mode, keyCountToSimulate != 0,
      _status.composing, shortcut_modifier, known_key, wParam);
  *pfEaten = predicted_eaten ? TRUE : FALSE;
  TRACE_KEY_EVENT(
      L"Test", wParam, lParam,
      L"known_key=1 ascii_mode=" + std::to_wstring(_status.ascii_mode) +
          L" composing=" + std::to_wstring(_status.composing) +
          L" capslock_simulation=" + std::to_wstring(keyCountToSimulate != 0) +
          L" shortcut_modifier=" + std::to_wstring(shortcut_modifier) +
          L" predicted_eaten=" + std::to_wstring(*pfEaten));
  return predicted_eaten;
}

bool WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  *pfEaten = FALSE;
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    TRACE_KEY_EVENT(L"Process", wParam, lParam,
                    L"skip disabled_or_keyboard_closed=1");
    return false;
  }
  if (!_CanHandleKeyEvent()) {
    TRACE_KEY_EVENT(L"Process", wParam, lParam,
                    L"skip manual_exit_or_recovery_disabled=1");
    return false;
  }

  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    TRACE_KEY_EVENT(L"Process", wParam, lParam, L"convert=0");
    return false;
  } else {
    bool processed = false;
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate) {
      bool eaten = false;
      if (m_client.ProcessKeyEvent(ke, &eaten)) {
        *pfEaten = (BOOL)eaten;
        processed = true;
        weasel::ResponseParser parser(NULL, NULL, &_status, NULL,
                                      &_cand->style());
        m_client.GetResponseData(std::ref(parser));
      } else {
        *pfEaten = FALSE;
        _RecoverServerAsync();
      }
      TRACE_KEY_EVENT(L"Process", wParam, lParam,
                      L"convert=1 keycode=" + std::to_wstring(ke.keycode) +
                          L" mask=" + std::to_wstring(ke.mask) +
                          L" processed=" + std::to_wstring(processed) +
                          L" eaten=" + std::to_wstring(*pfEaten));
    } else {
      TRACE_KEY_EVENT(L"Process", wParam, lParam,
                      L"skip capslock_simulation=1");
    }

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
    return processed;
  }
}

bool WeaselTSF::_CanHandleKeyEvent() {
  DWORD now = GetTickCount();
  if (_manualExitCheckTick == 0 ||
      now - _manualExitCheckTick >= kManualExitKeyCheckIntervalMs) {
    _manualExitMarkedForKeyEvents = weasel::IsServiceManualExitMarked();
    _manualExitCheckTick = now;
  }
  return !_manualExitMarkedForKeyEvents;
}

void WeaselTSF::_ResetKeyEventTestCacheIfNeeded() {
  _keyEventTestCacheReset.Apply(_keyEventTestCache);
}

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
    _activeKeyDownGuard.Reset();
    m_client.FocusOut();
    _AbortComposition();
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * Test-key callbacks may be the only callback a host reliably sends for
 * composition-editing keys. Process them once and cache the result so the
 * following OnKey* callback, when present, does not send the same key to Rime
 * twice.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  _ResetKeyEventTestCacheIfNeeded();
  _fTestKeyUpPending = FALSE;
  _keyEventTestCache.Remove(true, wParam, lParam);
  if (_keyEventTestCache.Matches(false, wParam, lParam)) {
    *pfEaten = _keyEventTestCache.Eaten();
    TRACE_KEY_EVENT(L"OnTestKeyDown", wParam, lParam,
                    L"cache=1 eaten=" + std::to_wstring(*pfEaten));
    return S_OK;
  }
  bool processed = _ProcessKeyEvent(wParam, lParam, pfEaten);
  if (processed) {
    _keyEventTestCache.Store(false, wParam, lParam, *pfEaten);
    _UpdateComposition(pContext);
  } else {
    _keyEventTestCache.Clear();
  }
  _fTestKeyDownPending = (processed && *pfEaten) ? TRUE : FALSE;
  TRACE_KEY_EVENT(L"OnTestKeyDown", wParam, lParam,
                  L"processed=" + std::to_wstring(processed) + L" eaten=" +
                      std::to_wstring(*pfEaten) + L" pending=" +
                      std::to_wstring(_fTestKeyDownPending));
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  _ResetKeyEventTestCacheIfNeeded();
  _fTestKeyUpPending = FALSE;
  _fTestKeyDownPending = FALSE;
  if (_keyEventTestCache.Matches(false, wParam, lParam)) {
    *pfEaten = _keyEventTestCache.Eaten();
    _keyEventTestCache.RemoveMatched();
    TRACE_KEY_EVENT(L"OnKeyDown", wParam, lParam,
                    L"cache=1 eaten=" + std::to_wstring(*pfEaten));
    return S_OK;
  }
  _keyEventTestCache.Clear();
  if (_activeKeyDownGuard.ShouldSuppress(wParam, lParam)) {
    *pfEaten = TRUE;
    TRACE_KEY_EVENT(L"OnKeyDown", wParam, lParam,
                    L"active_duplicate=1 suppress=1");
    return S_OK;
  }
  bool processed = _ProcessKeyEvent(wParam, lParam, pfEaten);
  if (processed && *pfEaten)
    _activeKeyDownGuard.Remember(wParam, lParam);
  _UpdateComposition(pContext);
  TRACE_KEY_EVENT(L"OnKeyDown", wParam, lParam,
                  L"processed=" + std::to_wstring(processed) + L" eaten=" +
                      std::to_wstring(*pfEaten));
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  _ResetKeyEventTestCacheIfNeeded();
  _fTestKeyDownPending = FALSE;
  _keyEventTestCache.Remove(false, wParam, lParam);
  if (_keyEventTestCache.Matches(true, wParam, lParam)) {
    *pfEaten = _keyEventTestCache.Eaten();
    TRACE_KEY_EVENT(L"OnTestKeyUp", wParam, lParam,
                    L"cache=1 eaten=" + std::to_wstring(*pfEaten));
    return S_OK;
  }
  bool processed = _ProcessKeyEvent(wParam, lParam, pfEaten);
  if (processed) {
    _keyEventTestCache.Store(true, wParam, lParam, *pfEaten);
    _UpdateComposition(pContext);
  } else {
    _keyEventTestCache.Clear();
  }
  _fTestKeyUpPending = (processed && *pfEaten) ? TRUE : FALSE;
  TRACE_KEY_EVENT(L"OnTestKeyUp", wParam, lParam,
                  L"processed=" + std::to_wstring(processed) + L" eaten=" +
                      std::to_wstring(*pfEaten) + L" pending=" +
                      std::to_wstring(_fTestKeyUpPending));
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _ResetKeyEventTestCacheIfNeeded();
  _fTestKeyDownPending = FALSE;
  _fTestKeyUpPending = FALSE;
  _activeKeyDownGuard.Release(wParam, lParam);
  if (_keyEventTestCache.Matches(true, wParam, lParam)) {
    *pfEaten = _keyEventTestCache.Eaten();
    _keyEventTestCache.RemoveMatched();
    TRACE_KEY_EVENT(L"OnKeyUp", wParam, lParam,
                    L"cache=1 eaten=" + std::to_wstring(*pfEaten));
    return S_OK;
  }
  _keyEventTestCache.Clear();
  bool processed = _ProcessKeyEvent(wParam, lParam, pfEaten);
  if (!_async_edit)
    _UpdateComposition(pContext);
  TRACE_KEY_EVENT(L"OnKeyUp", wParam, lParam,
                  L"processed=" + std::to_wstring(processed) + L" eaten=" +
                      std::to_wstring(*pfEaten));
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);
  WeaselDebugLog(L"KeyEvent", L"InitKeyEventSink hr=" + std::to_wstring(hr) +
                                  L" host=" + CurrentHostPath());

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
