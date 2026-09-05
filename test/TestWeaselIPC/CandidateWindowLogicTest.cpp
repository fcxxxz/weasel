#include "stdafx.h"

#include <CandidatePresentationGate.h>
#include <CandidateWindowPosition.h>

#include <boost/detail/lightweight_test.hpp>

void candidate_window_logic_unit_tests() {
  using weasel::CandidatePresentationAction;
  using weasel::CandidatePresentationGate;
  using weasel::CandidateWindowPlacementStyle;

  const RECT input_anchor{100, 100, 102, 120};
  const RECT work_area{0, 0, 1920, 1080};
  const SIZE panel_size{300, 100};
  CandidateWindowPlacementStyle shadow_style;
  shadow_style.anchor_gap_y = 6;
  shadow_style.shadow_radius = 5;
  shadow_style.shadow_offset_y = 4;
  shadow_style.layout_offset_y = 8;

  const auto moved = weasel::ComputeCandidateWindowPlacement(
      input_anchor, work_area, panel_size, shadow_style, false);
  const auto refreshed = weasel::ComputeCandidateWindowPlacement(
      input_anchor, work_area, panel_size, shadow_style, moved.sticky);
  BOOST_TEST(moved.origin.x == 100);
  BOOST_TEST(moved.origin.y == 118);
  BOOST_TEST(refreshed.origin.x == moved.origin.x);
  BOOST_TEST(refreshed.origin.y == moved.origin.y);
  BOOST_TEST(input_anchor.bottom == 120);
  BOOST_TEST(!moved.sticky);
  BOOST_TEST(!moved.reverse_vertical_candidates);

  CandidateWindowPlacementStyle plain_style;
  plain_style.anchor_gap_y = 6;
  const RECT bottom_anchor{100, 1000, 102, 1020};
  const SIZE tall_panel{300, 200};
  const auto placed_above = weasel::ComputeCandidateWindowPlacement(
      bottom_anchor, work_area, tall_panel, plain_style, false);
  BOOST_TEST(placed_above.origin.y == 794);
  BOOST_TEST(placed_above.sticky);

  const SIZE short_panel{300, 40};
  const auto kept_above = weasel::ComputeCandidateWindowPlacement(
      bottom_anchor, work_area, short_panel, plain_style, placed_above.sticky);
  BOOST_TEST(kept_above.origin.y == 954);
  BOOST_TEST(kept_above.sticky);

  plain_style.vertical_layout = true;
  plain_style.vertical_auto_reverse = true;
  const auto reversed = weasel::ComputeCandidateWindowPlacement(
      bottom_anchor, work_area, tall_panel, plain_style, false);
  BOOST_TEST(reversed.reverse_vertical_candidates);

  const RECT jittered_anchor{100, 100, 102, 122};
  const RECT moved_anchor{100, 100, 102, 127};
  BOOST_TEST(
      weasel::IsCandidateInputPositionJitter(input_anchor, jittered_anchor));
  BOOST_TEST(
      !weasel::IsCandidateInputPositionJitter(input_anchor, moved_anchor));

  CandidatePresentationGate gate;
  const auto first_generation = gate.BeginComposition();
  BOOST_TEST(gate.CurrentGeneration() == first_generation);
  BOOST_TEST(gate.IsCurrent(first_generation));
  BOOST_TEST(!gate.RequestShow());

  const auto current_generation = gate.BeginComposition();
  BOOST_TEST(!gate.IsCurrent(first_generation));
  BOOST_TEST(gate.IsCurrent(current_generation));
  BOOST_TEST(!gate.RequestShow());
  BOOST_TEST(gate.OnPositionReady(first_generation) ==
             CandidatePresentationAction::kNone);
  BOOST_TEST(gate.OnPositionReady(current_generation) ==
             CandidatePresentationAction::kMoveAndShow);
  BOOST_TEST(gate.RequestShow());

  gate.RequestHide();
  BOOST_TEST(gate.OnPositionReady(current_generation) ==
             CandidatePresentationAction::kMove);

  const auto fallback_generation = gate.BeginComposition();
  BOOST_TEST(!gate.RequestShow());
  BOOST_TEST(gate.OnPositionUnavailable(first_generation) ==
             CandidatePresentationAction::kNone);
  BOOST_TEST(gate.OnPositionUnavailable(fallback_generation) ==
             CandidatePresentationAction::kNone);
  BOOST_TEST(!gate.RequestShow());
  BOOST_TEST(gate.OnPositionReady(fallback_generation) ==
             CandidatePresentationAction::kMoveAndShow);
  BOOST_TEST(gate.RequestShow());

  const auto position_first_generation = gate.BeginComposition();
  BOOST_TEST(gate.OnPositionReady(position_first_generation) ==
             CandidatePresentationAction::kMove);
  BOOST_TEST(gate.RequestShow());

  const auto replacement_generation = gate.BeginComposition();
  BOOST_TEST(gate.CancelComposition(position_first_generation) ==
             CandidatePresentationAction::kNone);
  BOOST_TEST(gate.IsCurrent(replacement_generation));
  BOOST_TEST(gate.CancelComposition(replacement_generation) ==
             CandidatePresentationAction::kNone);
  BOOST_TEST(!gate.IsCurrent(replacement_generation));
  BOOST_TEST(!gate.RequestShow());

  weasel::CandidatePositionRequestGate position_requests;
  const auto old_request = position_requests.BeginRequest();
  BOOST_TEST(position_requests.IsCurrent(old_request));
  const auto current_request = position_requests.BeginRequest();
  BOOST_TEST(!position_requests.IsCurrent(old_request));
  BOOST_TEST(position_requests.IsCurrent(current_request));
  position_requests.Invalidate();
  BOOST_TEST(!position_requests.IsCurrent(current_request));

  auto cuas_probe = weasel::ProbeCuasCandidatePosition(false, false, 200, 200);
  BOOST_TEST(!cuas_probe.accept_position);
  BOOST_TEST(cuas_probe.tested);
  BOOST_TEST(cuas_probe.workaround_enabled);
  cuas_probe = weasel::ProbeCuasCandidatePosition(
      cuas_probe.tested, cuas_probe.workaround_enabled, 200, 200);
  BOOST_TEST(cuas_probe.accept_position);

  const auto regular_probe =
      weasel::ProbeCuasCandidatePosition(false, false, 200, 220);
  BOOST_TEST(regular_probe.accept_position);
  BOOST_TEST(regular_probe.tested);
  BOOST_TEST(!regular_probe.workaround_enabled);

  weasel::CompositionEndRetryState end_retry;
  auto end_parameters = end_retry.ParametersForAttempt(7, true, true);
  BOOST_TEST(end_parameters.clear_text);
  BOOST_TEST(end_parameters.end_ui);
  end_retry.RecordFailure(7, false, false);
  BOOST_TEST(end_retry.HasPending(7));
  end_parameters = end_retry.ParametersForAttempt(7, true, true);
  BOOST_TEST(!end_parameters.clear_text);
  BOOST_TEST(!end_parameters.end_ui);
  end_retry.RequireEndUI(7);
  end_parameters = end_retry.ParametersForAttempt(7, true, true);
  BOOST_TEST(!end_parameters.clear_text);
  BOOST_TEST(end_parameters.end_ui);
  end_retry.Complete(6);
  BOOST_TEST(end_retry.HasPending(7));
  end_retry.Complete(7);
  BOOST_TEST(!end_retry.HasPending(7));

  BOOST_TEST(weasel::CanProcessKeyAfterCompositionEndRetry(false, false));
  BOOST_TEST(weasel::CanProcessKeyAfterCompositionEndRetry(true, true));
  BOOST_TEST(!weasel::CanProcessKeyAfterCompositionEndRetry(true, false));
  BOOST_TEST(
      weasel::ShouldRequestCompositionUpdateAfterKey(false, false, false));
  BOOST_TEST(weasel::ShouldRequestCompositionUpdateAfterKey(true, true, false));
  BOOST_TEST(
      !weasel::ShouldRequestCompositionUpdateAfterKey(true, false, false));
  BOOST_TEST(
      !weasel::ShouldRequestCompositionUpdateAfterKey(true, false, true));
}

#ifdef WEASEL_CANDIDATE_WINDOW_STANDALONE_TEST
int main() {
  candidate_window_logic_unit_tests();
  return boost::report_errors();
}
#endif
