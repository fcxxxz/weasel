#pragma once

#include <windows.h>

namespace weasel {

struct CandidateWindowPlacementStyle {
  LONG anchor_gap_y = 0;
  LONG above_gap_y = 6;
  LONG shadow_radius = 0;
  LONG shadow_offset_x = 0;
  LONG shadow_offset_y = 0;
  LONG layout_offset_x = 0;
  LONG layout_offset_y = 0;
  bool shadow_transparent = false;
  bool vertical_text = false;
  bool vertical_text_left_to_right = false;
  bool vertical_layout = false;
  bool vertical_auto_reverse = false;
};

struct CandidateWindowPlacement {
  POINT origin{0, 0};
  bool sticky = false;
  bool reverse_vertical_candidates = false;
};

inline CandidateWindowPlacement ComputeCandidateWindowPlacement(
    const RECT& raw_input,
    const RECT& work_area,
    SIZE effective_size,
    const CandidateWindowPlacementStyle& style,
    bool was_sticky) {
  const LONGLONG max_x =
      static_cast<LONGLONG>(work_area.right) - effective_size.cx;
  const LONGLONG max_y =
      static_cast<LONGLONG>(work_area.bottom) - effective_size.cy;
  LONGLONG x = raw_input.left;
  LONGLONG y = static_cast<LONGLONG>(raw_input.bottom) + style.anchor_gap_y;

  if (style.shadow_radius != 0) {
    x -= (style.shadow_offset_x >= 0 || style.shadow_transparent)
             ? style.layout_offset_x
             : (style.layout_offset_x / 2);
    y -= (style.shadow_offset_y > 0 || style.shadow_transparent)
             ? style.layout_offset_y
             : (style.layout_offset_y / 2);
  }

  if (style.vertical_text && !style.vertical_text_left_to_right) {
    x += static_cast<LONGLONG>(style.layout_offset_x) - effective_size.cx;
    if (style.shadow_offset_x < 0)
      x += style.layout_offset_x;
  }

  if (x > max_x)
    x = max_x;
  if (x < work_area.left)
    x = work_area.left;

  const bool sticky = was_sticky || y > max_y;
  bool reverse_vertical_candidates = false;
  if (sticky) {
    y = static_cast<LONGLONG>(raw_input.top) - effective_size.cy -
        style.above_gap_y;
    if (style.shadow_radius != 0 && style.shadow_offset_y > 0)
      y -= style.shadow_offset_y;
    reverse_vertical_candidates =
        style.vertical_auto_reverse && style.vertical_layout;
    if (style.shadow_radius > 0) {
      y += (style.shadow_offset_y < 0 || style.shadow_transparent)
               ? style.layout_offset_y
               : (style.layout_offset_y / 2);
    }
  }

  if (y < work_area.top)
    y = work_area.top;

  constexpr LONGLONG kLongMin = -2147483647LL - 1;
  constexpr LONGLONG kLongMax = 2147483647LL;
  if (x < kLongMin)
    x = kLongMin;
  else if (x > kLongMax)
    x = kLongMax;
  if (y < kLongMin)
    y = kLongMin;
  else if (y > kLongMax)
    y = kLongMax;

  return {{static_cast<LONG>(x), static_cast<LONG>(y)},
          sticky,
          reverse_vertical_candidates};
}

inline bool IsCandidateInputPositionJitter(const RECT& previous_raw_input,
                                           const RECT& raw_input) {
  const LONGLONG bottom_delta =
      static_cast<LONGLONG>(raw_input.bottom) - previous_raw_input.bottom;
  const LONGLONG absolute_delta =
      bottom_delta < 0 ? -bottom_delta : bottom_delta;
  return previous_raw_input.left == raw_input.left && bottom_delta != 0 &&
         absolute_delta < 6;
}

}  // namespace weasel
