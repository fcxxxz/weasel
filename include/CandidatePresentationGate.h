#pragma once

#include <cstdint>

namespace weasel {

using CompositionGeneration = std::uint64_t;

enum class CandidatePresentationAction {
  kNone,
  kMove,
  kMoveAndShow,
};

class CandidatePresentationGate {
 public:
  CompositionGeneration BeginComposition() noexcept {
    ++generation_;
    if (generation_ == 0)
      ++generation_;
    active_ = true;
    show_requested_ = false;
    position_ready_ = false;
    return generation_;
  }

  CompositionGeneration CurrentGeneration() const noexcept {
    return generation_;
  }

  bool IsCurrent(CompositionGeneration generation) const noexcept {
    return active_ && generation == generation_;
  }

  bool RequestShow() noexcept {
    if (!active_)
      return false;
    show_requested_ = true;
    return position_ready_;
  }

  void RequestHide() noexcept { show_requested_ = false; }

  CandidatePresentationAction OnPositionReady(
      CompositionGeneration generation) noexcept {
    if (!IsCurrent(generation))
      return CandidatePresentationAction::kNone;
    position_ready_ = true;
    return show_requested_ ? CandidatePresentationAction::kMoveAndShow
                           : CandidatePresentationAction::kMove;
  }

  CandidatePresentationAction OnPositionUnavailable(
      CompositionGeneration generation) noexcept {
    (void)generation;
    return CandidatePresentationAction::kNone;
  }

  CandidatePresentationAction CancelComposition(
      CompositionGeneration generation) noexcept {
    if (!IsCurrent(generation))
      return CandidatePresentationAction::kNone;
    active_ = false;
    show_requested_ = false;
    position_ready_ = false;
    return CandidatePresentationAction::kNone;
  }

 private:
  CompositionGeneration generation_ = 0;
  bool active_ = false;
  bool show_requested_ = false;
  bool position_ready_ = false;
};

struct CuasCandidatePositionProbe {
  bool accept_position;
  bool tested;
  bool workaround_enabled;
};

inline CuasCandidatePositionProbe ProbeCuasCandidatePosition(
    bool tested,
    bool workaround_enabled,
    std::int32_t top,
    std::int32_t bottom) noexcept {
  if (!tested) {
    tested = true;
    if (top == bottom) {
      workaround_enabled = true;
      return {false, tested, workaround_enabled};
    }
  }
  return {true, tested, workaround_enabled};
}

using PositionRequestGeneration = std::uint64_t;

class CandidatePositionRequestGate {
 public:
  PositionRequestGeneration BeginRequest() noexcept {
    Advance();
    return generation_;
  }

  bool IsCurrent(PositionRequestGeneration generation) const noexcept {
    return generation != 0 && generation == generation_;
  }

  void Invalidate() noexcept { Advance(); }

 private:
  void Advance() noexcept {
    ++generation_;
    if (generation_ == 0)
      ++generation_;
  }

  PositionRequestGeneration generation_ = 0;
};

struct CompositionEndParameters {
  bool clear_text;
  bool end_ui;
};

class CompositionEndRetryState {
 public:
  CompositionEndParameters ParametersForAttempt(
      CompositionGeneration generation,
      bool requestedClearText,
      bool requestedEndUi) const noexcept {
    if (HasPending(generation))
      return {clear_text_, end_ui_};
    return {requestedClearText, requestedEndUi};
  }

  void RecordFailure(CompositionGeneration generation,
                     bool clearText,
                     bool endUi) noexcept {
    if (generation == 0 || pending_)
      return;
    generation_ = generation;
    clear_text_ = clearText;
    end_ui_ = endUi;
    pending_ = true;
  }

  void Complete(CompositionGeneration generation) noexcept {
    if (!HasPending(generation))
      return;
    generation_ = 0;
    clear_text_ = false;
    end_ui_ = false;
    pending_ = false;
  }

  void RequireEndUI(CompositionGeneration generation) noexcept {
    if (HasPending(generation))
      end_ui_ = true;
  }

  bool HasPending(CompositionGeneration generation) const noexcept {
    return pending_ && generation != 0 && generation_ == generation;
  }

 private:
  CompositionGeneration generation_ = 0;
  bool clear_text_ = false;
  bool end_ui_ = false;
  bool pending_ = false;
};

inline bool CanProcessKeyAfterCompositionEndRetry(bool hasPending,
                                                  bool retryAccepted) noexcept {
  return !hasPending || retryAccepted;
}

inline bool ShouldRequestCompositionUpdateAfterKey(bool hadPending,
                                                   bool processed,
                                                   bool pendingAfter) noexcept {
  return !pendingAfter && (!hadPending || processed);
}

}  // namespace weasel
