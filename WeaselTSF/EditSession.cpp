#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"

STDMETHODIMP WeaselTSF::DoEditSession(TfEditCookie ec) {
  bool compositionEnded = false;
  bool compositionEndAttempted = false;
  bool pendingEndRetryFailed = false;
  if (_compositionEndRetryState.HasPending(_pCompositionGeneration)) {
    compositionEndAttempted = true;
    compositionEnded = _RetryPendingCompositionEnd();
    if (compositionEnded) {
      // The old generation is detached. This response may legitimately need
      // to end a newly-created composition of its own.
      compositionEndAttempted = false;
    } else {
      pendingEndRetryFailed = true;
      _positionRequestGate.Invalidate();
      _cand->Show(FALSE);
    }
  }

  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = false;
  try {
    ok = m_client.GetResponseData(std::ref(parser));
  } catch (...) {
    // A malformed response must degrade to pass-through input. Do not let a
    // third-party schema or missing runtime DLL take down the host process.
    ok = false;
  }

  if (!ok) {
    _AbortComposition(true, compositionEndAttempted);
    return TRUE;
  }

  if (config.hide_ime_mode_icon != _config.hide_ime_mode_icon) {
    _config = config;
    _UninitLanguageBar();
    _InitLanguageBar();
  }
  _UpdateLanguageBar(_status);

  bool compositionStarted = false;
  if (!pendingEndRetryFailed && !commit.empty()) {
    // For auto-selecting, commit and preedit can both exist.
    // Commit the old TSF composition. If Rime immediately has a new
    // preedit (top-word input), _EndComposition() drops the local pointer
    // synchronously, so the following state check starts a new TSF
    // composition instead of observing the old one.
    if (!_IsComposing()) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
      compositionStarted = true;
    }
    _InsertText(_pEditSessionContext, commit);
    // Keep the candidate UI alive while the replacement composition is
    // being created; otherwise the key-down path destroys the old window
    // and the new one cannot be positioned until key-up.
    compositionEndAttempted = true;
    compositionEnded =
        _EndComposition(_pEditSessionContext, false, !_status.composing);
    if (!compositionEnded) {
      pendingEndRetryFailed = true;
      _positionRequestGate.Invalidate();
      _cand->Show(FALSE);
    }
    _committed = TRUE;
  } else {
    _committed = !commit.empty();
  }
  if (!pendingEndRetryFailed) {
    if (_status.composing && (compositionEnded || !_IsComposing())) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
      compositionStarted = true;
    } else if (!_status.composing && _IsComposing() &&
               !compositionEndAttempted) {
      compositionEndAttempted = true;
      compositionEnded = _EndComposition(_pEditSessionContext, true);
      if (!compositionEnded) {
        pendingEndRetryFailed = true;
        _positionRequestGate.Invalidate();
        _cand->Show(FALSE);
      }
    }
  }
  if (!pendingEndRetryFailed && _IsComposing() && config.inline_preedit) {
    _ShowInlinePreedit(_pEditSessionContext, context);
  }

  if (!pendingEndRetryFailed && !compositionEnded && !compositionStarted)
    _UpdateCompositionWindow(_pEditSessionContext);
  // Keep the existing candidate window alive during top-word input, but
  // publish the new candidates in this key-down edit session. Positioning is
  // still updated by the queued read session after the new composition is
  // created.
  _UpdateUI(*context, _status, pendingEndRetryFailed);

  return TRUE;
}
