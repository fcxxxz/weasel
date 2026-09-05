#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"
#include <new>

namespace {

bool EditSessionRequestSucceeded(HRESULT requestResult, HRESULT sessionResult) {
  return SUCCEEDED(requestResult) &&
         (sessionResult == TF_S_ASYNC || SUCCEEDED(sessionResult));
}

}  // namespace

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled,
                               weasel::CompositionGeneration generation)
      : CEditSession(pTextService, pContext),
        _fCUASWorkaroundEnabled(fCUASWorkaroundEnabled),
        _generation(generation) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
  weasel::CompositionGeneration _generation;
};

STDMETHODIMP CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  if (!_pTextService ||
      !_pTextService->_IsCurrentCompositionGeneration(_generation))
    return S_OK;

  const auto fail = [this]() {
    _pTextService->_CancelCompositionGeneration(_generation);
    return E_FAIL;
  };

  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return fail();
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return fail();

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return fail();
  const HRESULT startResult = pContextComposition->StartComposition(
      ec, pRangeComposition, _pTextService, &pComposition);
  if (FAILED(startResult) || pComposition == nullptr)
    return fail();

  if (!_pTextService->_SetComposition(pComposition, _generation)) {
    pComposition->EndComposition(ec);
    return S_OK;
  }

  /* WORKAROUND:
   *   CUAS does not provide a correct GetTextExt() position unless the
   * composition is filled with characters. So we insert a zero width space
   * here. The workaround is only needed when inline preedit is not enabled.
   *   See https://github.com/rime/weasel/pull/883#issuecomment-1567625762
   */
  if (_fCUASWorkaroundEnabled) {
    pRangeComposition->SetText(ec, TF_ST_CORRECTION, L"\u2060", 1);
  }

  /* set selection */
  TF_SELECTION tfSelection;
  pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  // Position only after the new composition has actually been created.
  _pTextService->_UpdateCompositionWindow(_pContext, pComposition, _generation);
  return S_OK;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  _positionRequestGate.Invalidate();
  const auto generation = _cand->BeginComposition();
  _pComposition = nullptr;
  _pCompositionGeneration = generation;

  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(new CStartCompositionEditSession(
      this, pContext, fCUASWorkaroundEnabled, generation));
  _cand->StartUI();
  if (pContext == nullptr || pStartCompositionEditSession == nullptr) {
    _CancelCompositionGeneration(generation);
    return;
  }

  HRESULT sessionResult = E_FAIL;
  const HRESULT requestResult = pContext->RequestEditSession(
      _tfClientId, pStartCompositionEditSession,
      TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &sessionResult);
  if (!EditSessionRequestSucceeded(requestResult, sessionResult))
    _CancelCompositionGeneration(generation);
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             weasel::CompositionGeneration generation,
                             BOOL clear,
                             BOOL endUI)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _generation(generation),
        _clear(clear),
        _endUI(endUI) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  weasel::CompositionGeneration _generation;
  BOOL _clear;
  BOOL _endUI;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext,
                                                    _pComposition);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  // Drop ownership before EndComposition(). Some applications notify
  // OnCompositionTerminated synchronously while the old composition ends.
  // Keeping it as the current composition makes that normal notification
  // look like an external abort and can clear a new Rime composition during
  // auto-commit.
  const BOOL finalized =
      _pTextService->_FinalizeComposition(_pComposition, _generation);
  if (finalized && _endUI)
    _pTextService->_EndUI();
  _pComposition->EndComposition(ec);
  return S_OK;
}

BOOL WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                BOOL endUI) {
  com_ptr<ITfComposition> pComposition = _pComposition;
  const auto generation = _pCompositionGeneration;
  if (endUI && _compositionEndRetryState.HasPending(generation))
    _compositionEndRetryState.RequireEndUI(generation);
  const auto parameters = _compositionEndRetryState.ParametersForAttempt(
      generation, !!clear, !!endUI);
  const BOOL effectiveClear = parameters.clear_text;
  const BOOL effectiveEndUI = parameters.end_ui;
  const bool retrying = _compositionEndRetryState.HasPending(generation);
  com_ptr<ITfContext> pEndContext =
      retrying ? _pPendingCompositionEndContext : pContext;

  if (pComposition == nullptr) {
    if (generation == 0)
      _positionRequestGate.Invalidate();
    else
      _CancelCompositionGeneration(generation);
    _CompletePendingCompositionEnd(generation);
    if (effectiveEndUI)
      _cand->EndUI();
    return TRUE;
  }

  const auto fail = [this, pComposition, generation, pEndContext,
                     effectiveClear, effectiveEndUI]() {
    if (_IsCurrentComposition(pComposition, generation)) {
      _positionRequestGate.Invalidate();
      _cand->CancelComposition(generation);
    }
    _RecordPendingCompositionEnd(generation, pEndContext, effectiveClear,
                                 effectiveEndUI);
    return FALSE;
  };

  if (pEndContext == nullptr)
    return fail();

  CEndCompositionEditSession* pEditSession = new (std::nothrow)
      CEndCompositionEditSession(this, pEndContext, pComposition, generation,
                                 effectiveClear, effectiveEndUI);
  if (pEditSession == nullptr)
    return fail();

  HRESULT sessionResult = E_FAIL;
  const HRESULT requestResult = pEndContext->RequestEditSession(
      _tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
      &sessionResult);
  pEditSession->Release();
  if (!EditSessionRequestSucceeded(requestResult, sessionResult))
    return fail();

  const BOOL finalized = _FinalizeComposition(pComposition, generation);
  if (finalized && effectiveEndUI)
    _cand->EndUI();
  _CompletePendingCompositionEnd(generation);
  return TRUE;
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            weasel::CompositionGeneration generation,
                            weasel::PositionRequestGeneration requestGeneration,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext),
        _pContextView(pContextView),
        _pComposition(pComposition),
        _generation(generation),
        _requestGeneration(requestGeneration),
        _enhancedPosition(enhancedPosition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  weasel::CompositionGeneration _generation;
  weasel::PositionRequestGeneration _requestGeneration;
  bool _enhancedPosition;
};

STDMETHODIMP CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  if (!_pTextService ||
      !_pTextService->_IsCurrentPositionRequest(_requestGeneration))
    return S_OK;

  const bool trackedComposition = _pComposition != nullptr;
  if (trackedComposition) {
    if (!_pTextService->_IsCurrentCompositionGeneration(_generation) ||
        !_pTextService->_IsCurrentComposition(_pComposition, _generation))
      return S_OK;
  } else if (!_pTextService->_CanApplySelectionPosition(_pContext,
                                                        _requestGeneration)) {
    return S_OK;
  }

  const auto unavailable = [this, trackedComposition]() {
    if (trackedComposition)
      _pTextService->_OnCompositionPositionUnavailable(_generation);
  };

  com_ptr<ITfRange> pRange;
  if (trackedComposition) {
    if (FAILED(_pComposition->GetRange(&pRange))) {
      unavailable();
      return E_FAIL;
    }
  } else {
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    const HRESULT selectionResult = _pContext->GetSelection(
        ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (selection.range != nullptr)
      pRange.Attach(selection.range);
    if (FAILED(selectionResult) || fetched != 1) {
      unavailable();
      return E_FAIL;
    }
  }

  if (pRange == nullptr || FAILED(pRange->Collapse(ec, TF_ANCHOR_START))) {
    unavailable();
    return E_FAIL;
  }

  RECT rc = {};
  BOOL fClipped = FALSE;
  if (_pContextView->GetTextExt(ec, pRange, &rc, &fClipped) != S_OK) {
    unavailable();
    return S_OK;
  }
  if (rc.left == 0 && rc.top == 0 && rc.right == 0 && rc.bottom == 0) {
    unavailable();
    return S_OK;
  }

  // Get the foreground window position and check whether GetTextExt returned
  // a rectangle outside it.
  if (_enhancedPosition) {
    const HWND hwnd = GetForegroundWindow();
    RECT rcForegroundWindow = {};
    if (hwnd != nullptr && ::GetWindowRect(hwnd, &rcForegroundWindow) &&
        (rc.left < rcForegroundWindow.left ||
         rc.left > rcForegroundWindow.right ||
         rc.top < rcForegroundWindow.top ||
         rc.top > rcForegroundWindow.bottom)) {
      POINT pt = {};
      const bool hasCaret = ::GetCaretPos(&pt);
      const int offsetx =
          rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
      const int offsety =
          rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
      rc.left += offsetx;
      rc.right += offsetx;
      rc.top += offsety;
      rc.bottom += offsety;
    }
  }

  if (trackedComposition) {
    _pTextService->_SetCompositionPosition(rc, _pComposition, _generation,
                                           _requestGeneration);
  } else {
    _pTextService->_SetSelectionPosition(rc, _pContext, _requestGeneration);
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  const auto generation = _pCompositionGeneration;
  if (_compositionEndRetryState.HasPending(generation))
    return FALSE;
  if (_IsCurrentCompositionGeneration(generation)) {
    if (_IsCurrentComposition(_pComposition, generation)) {
      return _UpdateCompositionWindow(pContext, _pComposition, generation);
    }
    // A presentation generation exists, but CStart has not supplied its TSF
    // composition yet. Its successful edit session queues the tracked read.
    return FALSE;
  }
  return _UpdateCompositionWindow(pContext, nullptr, 0);
}

BOOL WeaselTSF::_UpdateCompositionWindow(
    com_ptr<ITfContext> pContext,
    com_ptr<ITfComposition> pComposition,
    weasel::CompositionGeneration generation) {
  const bool trackedComposition = pComposition != nullptr;
  const auto unavailable = [this, trackedComposition, generation]() {
    if (trackedComposition)
      _cand->OnPositionUnavailable(generation);
  };

  if (pContext == nullptr) {
    unavailable();
    return FALSE;
  }
  if (trackedComposition &&
      (!_IsCurrentCompositionGeneration(generation) ||
       !_IsCurrentComposition(pComposition, generation))) {
    unavailable();
    return FALSE;
  }

  const auto requestGeneration = _positionRequestGate.BeginRequest();

  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK ||
      pContextView == nullptr) {
    unavailable();
    return FALSE;
  }
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(new CGetTextExtentEditSession(
      this, pContext, pContextView, pComposition, generation, requestGeneration,
      _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    unavailable();
    return FALSE;
  }

  HRESULT sessionResult = E_FAIL;
  const HRESULT requestResult = pContext->RequestEditSession(
      _tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READ,
      &sessionResult);
  if (!EditSessionRequestSucceeded(requestResult, sessionResult)) {
    unavailable();
    return FALSE;
  }
  return TRUE;
}

void WeaselTSF::_SetCompositionPosition(
    const RECT& rc,
    ITfComposition* pComposition,
    weasel::CompositionGeneration generation,
    weasel::PositionRequestGeneration requestGeneration) {
  if (!_positionRequestGate.IsCurrent(requestGeneration) ||
      !_cand->IsCurrentComposition(generation) ||
      _pCompositionGeneration != generation || pComposition == nullptr ||
      _pComposition != pComposition)
    return;

  if (!_AcceptPositionAfterCuasProbe(rc)) {
    _cand->OnPositionUnavailable(generation);
    return;
  }

  if (rc.right < rc.left || rc.bottom < rc.top) {
    _cand->OnPositionUnavailable(generation);
    return;
  }

  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc, generation);
}

void WeaselTSF::_SetSelectionPosition(
    const RECT& rc,
    ITfContext* pContext,
    weasel::PositionRequestGeneration requestGeneration) {
  if (!_CanApplySelectionPosition(pContext, requestGeneration))
    return;
  if (!_AcceptPositionAfterCuasProbe(rc))
    return;
  if ((rc.left == 0 && rc.top == 0 && rc.right == 0 && rc.bottom == 0) ||
      rc.right < rc.left || rc.bottom < rc.top)
    return;

  m_client.UpdateInputPosition(rc);
  _cand->CacheInputPosition(rc);
}

BOOL WeaselTSF::_AcceptPositionAfterCuasProbe(const RECT& rc) {
  const auto result = weasel::ProbeCuasCandidatePosition(
      !!_fCUASWorkaroundTested, !!_fCUASWorkaroundEnabled, rc.top, rc.bottom);
  _fCUASWorkaroundTested = result.tested;
  _fCUASWorkaroundEnabled = result.workaround_enabled;
  return result.accept_position;
}

void WeaselTSF::_OnCompositionPositionUnavailable(
    weasel::CompositionGeneration generation) {
  _cand->OnPositionUnavailable(generation);
}

BOOL WeaselTSF::_IsCurrentPositionRequest(
    weasel::PositionRequestGeneration requestGeneration) {
  return _positionRequestGate.IsCurrent(requestGeneration);
}

BOOL WeaselTSF::_CanApplySelectionPosition(
    ITfContext* pContext,
    weasel::PositionRequestGeneration requestGeneration) {
  return _positionRequestGate.IsCurrent(requestGeneration) &&
         pContext != nullptr && _pEditSessionContext == pContext &&
         _pComposition == nullptr && _pCompositionGeneration == 0;
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDMETHODIMP CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
}

/* Composition State */
STDMETHODIMP WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                                ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  // EndComposition() may generate this callback for the composition we just
  // closed. Only an active, matching composition is an external termination.
  const auto generation = _pCompositionGeneration;
  if (!_IsCurrentComposition(pComposition, generation))
    return S_OK;
  const bool endPendingUI =
      _compositionEndRetryState.HasPending(generation) &&
      _compositionEndRetryState.ParametersForAttempt(generation, false, false)
          .end_ui;

  // A host may terminate the empty TSF composition used for a non-inline
  // preedit. Keep Rime's composing state; the next key will create a fresh
  // TSF composition. Only an inactive Rime session should be aborted here.
  if (_status.composing) {
    if (_FinalizeComposition(pComposition, generation) && endPendingUI)
      _cand->EndUI();
    return S_OK;
  }

  if (_FinalizeComposition(pComposition, generation))
    _cand->EndUI();
  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear, bool compositionEndAttempted) {
  m_client.ClearComposition();
  weasel::MarkLocalCompositionAborted(_status);
  _keyEventTestCache.Clear();
  const auto generation = _pCompositionGeneration;
  if (compositionEndAttempted &&
      _compositionEndRetryState.HasPending(generation)) {
    _compositionEndRetryState.RequireEndUI(generation);
  }
  if (_IsComposing() && !compositionEndAttempted) {
    _EndComposition(_pEditSessionContext, clear);
  }
  if (_IsComposing()) {
    _positionRequestGate.Invalidate();
    _cand->Show(FALSE);
  } else {
    _CancelCompositionGeneration(generation);
    if (_pComposition == nullptr && _pCompositionGeneration == 0)
      _cand->EndUI();
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _FinalizeComposition(_pComposition, _pCompositionGeneration);
}

BOOL WeaselTSF::_FinalizeComposition(ITfComposition* pComposition,
                                     weasel::CompositionGeneration generation) {
  if (!_IsCurrentComposition(pComposition, generation))
    return FALSE;
  if (_cand->IsCurrentComposition(generation))
    _positionRequestGate.Invalidate();
  _pComposition = nullptr;
  _pCompositionGeneration = 0;
  _cand->CancelComposition(generation);
  _CompletePendingCompositionEnd(generation);
  return TRUE;
}

BOOL WeaselTSF::_RetryPendingCompositionEnd() {
  const auto generation = _pCompositionGeneration;
  if (!_compositionEndRetryState.HasPending(generation))
    return FALSE;
  return _EndComposition(_pPendingCompositionEndContext, FALSE, FALSE);
}

void WeaselTSF::_RecordPendingCompositionEnd(
    weasel::CompositionGeneration generation,
    com_ptr<ITfContext> pContext,
    BOOL clear,
    BOOL endUI) {
  const bool alreadyPending = _compositionEndRetryState.HasPending(generation);
  _compositionEndRetryState.RecordFailure(generation, !!clear, !!endUI);
  if (!alreadyPending && _compositionEndRetryState.HasPending(generation))
    _pPendingCompositionEndContext = pContext;
}

void WeaselTSF::_CompletePendingCompositionEnd(
    weasel::CompositionGeneration generation) {
  if (!_compositionEndRetryState.HasPending(generation))
    return;
  _compositionEndRetryState.Complete(generation);
  _pPendingCompositionEndContext = nullptr;
}

void WeaselTSF::_CancelCompositionGeneration(
    weasel::CompositionGeneration generation) {
  if (_pCompositionGeneration == generation) {
    _positionRequestGate.Invalidate();
    _pComposition = nullptr;
    _pCompositionGeneration = 0;
    _CompletePendingCompositionEnd(generation);
  }
  _cand->CancelComposition(generation);
}

BOOL WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition,
                                weasel::CompositionGeneration generation) {
  if (pComposition == nullptr || _pCompositionGeneration != generation ||
      !_cand->IsCurrentComposition(generation))
    return FALSE;
  _pComposition = pComposition;
  return TRUE;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}

BOOL WeaselTSF::_IsCurrentComposition(ITfComposition* pComposition) {
  return _IsCurrentComposition(pComposition, _pCompositionGeneration);
}

BOOL WeaselTSF::_IsCurrentComposition(
    ITfComposition* pComposition,
    weasel::CompositionGeneration generation) {
  return _pComposition != nullptr && _pComposition == pComposition &&
         _pCompositionGeneration == generation;
}

BOOL WeaselTSF::_IsCurrentCompositionGeneration(
    weasel::CompositionGeneration generation) {
  return _pCompositionGeneration == generation &&
         _cand->IsCurrentComposition(generation);
}
