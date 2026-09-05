#include "WeaselPanel.h"
#include <WeaselUI.h>

namespace weasel {
// ----------------------------------------------------------------------------
class UIImpl {
 public:
  WeaselPanel panel;
  UIImpl(UI& ui) : panel(ui) {}
  ~UIImpl() {}
  bool IsShown() const {
    return panel.IsWindow() && ::IsWindowVisible(panel.hwnd());
  }
  // While the panel is hidden nobody can see it, so per-keystroke UI
  // work (DWrite layout/measure) is pure waste - it showed up as the
  // dominant p95 tail (~145us) of every key. Defer the refresh; the
  // show paths catch up with exactly one refresh before painting.
  void RefreshIfVisible() {
    if (panel.IsWindow() && ::IsWindowVisible(panel.hwnd()))
      panel.Refresh();
    else
      dirty_on_show = true;
  }
  void RefreshDirty() {
    if (dirty_on_show) {
      dirty_on_show = false;
      panel.Refresh();
    }
  }
  bool dirty_on_show = true;
  void Refresh() {
    if (!panel.IsWindow())
      return;
    panel.Refresh();
  }
  void Prewarm() {
    if (!panel.IsWindow())
      return;
    panel.Prewarm();
    panel.ShowWindow(SW_HIDE);
  }
  void RepositionPreview() {
    if (panel.IsWindow()) {
      RefreshDirty();
      panel.RepositionPreview();
    }
  }
  void Show() {
    if (!panel.IsWindow())
      return;
    RefreshDirty();
    panel.ShowWindow(SW_SHOWNA);
  }
  void Hide() {
    if (!panel.IsWindow())
      return;
    panel.ShowWindow(SW_HIDE);
  }
  void ShowWithTimeout(size_t millisec) {
    RefreshDirty();
    panel.ShowWithTimeout(millisec);
  }
  bool IsCountingDown() const { return panel.IsCountingDown(); }
};
// ----------------------------------------------------------------------------
BOOL UI::IsCountingDown() const {
  return pimpl_ && pimpl_->panel.IsCountingDown();
};

UI::UI() : pimpl_(nullptr), in_server_(false) {}

UI::~UI() {
  if (pimpl_)
    Destroy(true);
}
BOOL UI::IsShown() const {
  return pimpl_ && pimpl_->IsShown();
}
void UI::UpdateInputPosition(RECT const& rc) {
  if (pimpl_ && pimpl_->panel.IsWindow()) {
    const bool hidden = !::IsWindowVisible(pimpl_->panel.hwnd());
    pimpl_->panel.MoveTo(rc);
    if (hidden)
      pimpl_->dirty_on_show = true;
  }
}
void UI::Update(const Context& ctx, const Status& status) {
  if (ctx_ == ctx && status_ == status)
    return;
  ctx_ = ctx;
  status_ = status;
  if (style_.candidate_abbreviate_length > 0) {
    for (auto& c : ctx_.cinfo.candies) {
      if (c.str.length() > (size_t)style_.candidate_abbreviate_length) {
        c.str =
            c.str.substr(0, (size_t)style_.candidate_abbreviate_length - 1) +
            L"..." + c.str.substr(c.str.length() - 1);
      }
    }
  }
  if (pimpl_ && pimpl_->panel.IsWindow() &&
      !::IsWindowVisible(pimpl_->panel.hwnd())) {
    // hidden: keep the fresh (abbreviated) context for the catch-up refresh
    // on show; skip per-keystroke layout work while invisible
    pimpl_->dirty_on_show = true;
    return;
  }
  Refresh();
}
void UI::Refresh() {
  if (pimpl_) {
    pimpl_->Refresh();
  }
}
void UI::Prewarm() {
  if (pimpl_) {
    pimpl_->Prewarm();
  }
}
void UI::RepositionPreview() {
  if (pimpl_)
    pimpl_->RepositionPreview();
}
void UI::ShowWithTimeout(size_t millisec) {
  if (pimpl_) {
    pimpl_->ShowWithTimeout(millisec);
  }
}
void UI::Show() {
  if (pimpl_) {
    pimpl_->Show();
  }
}
void UI::Hide() {
  if (pimpl_) {
    pimpl_->Hide();
  }
}
void UI::Destroy(bool full) {
  if (pimpl_) {
    if (pimpl_->panel.IsWindow())
      pimpl_->panel.DestroyWindow();
    if (full) {
      // ensure window resources and shared devices are released
      pimpl_->panel.ReleaseAllResources();
      pimpl_.reset();
    }
  }
}
bool UI::Create(HWND parent, bool preview_mode) {
  if (pimpl_) {
    pimpl_->panel.SetPreviewMode(preview_mode);
    pimpl_->panel.Create(parent, preview_mode);
    return true;
  }
  pimpl_ = std::make_unique<UIImpl>(*this);
  if (!pimpl_)
    return false;
  return pimpl_->panel.Create(parent, preview_mode);
}
bool UI::GetIsReposition() {
  return pimpl_ && pimpl_->panel.GetIsReposition();
}

HWND UI::hwnd() {
  if (pimpl_ && pimpl_->panel.IsWindow())
    return pimpl_->panel.hwnd();
  return nullptr;
}
}  // namespace weasel
