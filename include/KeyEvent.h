#pragma once
#include <atomic>

struct KeyInfo {
  UINT repeatCount : 16;
  UINT scanCode : 8;
  UINT isExtended : 1;
  UINT reserved : 4;
  UINT contextCode : 1;
  UINT prevKeyState : 1;
  UINT isKeyUp : 1;

  KeyInfo(LPARAM lparam) { *this = *reinterpret_cast<KeyInfo*>(&lparam); }

  operator UINT32() { return *reinterpret_cast<UINT32*>(this); }
};

namespace weasel {
struct KeyEvent {
  UINT keycode : 16;
  UINT mask : 16;
  KeyEvent() : keycode(0), mask(0) {}
  KeyEvent(UINT _keycode, UINT _mask) : keycode(_keycode), mask(_mask) {}
  KeyEvent(UINT x) { *reinterpret_cast<UINT*>(this) = x; }
  operator UINT32 const() const {
    return *reinterpret_cast<UINT32 const*>(this);
  }
};

inline bool IsTextVirtualKey(WPARAM wParam) {
  return (wParam >= L'0' && wParam <= L'9') ||
         (wParam >= L'A' && wParam <= L'Z') ||
         (wParam >= VK_NUMPAD0 && wParam <= VK_DIVIDE) ||
         (wParam >= VK_OEM_1 && wParam <= VK_OEM_102) ||
         wParam == VK_PACKET || wParam == VK_SPACE;
}

inline bool IsModeSwitchVirtualKey(WPARAM wParam) {
  return wParam == VK_SHIFT || wParam == VK_LSHIFT ||
         wParam == VK_RSHIFT || wParam == VK_CONTROL ||
         wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
         wParam == VK_CAPITAL;
}

inline bool ShouldEatTestKeyEvent(bool service_available,
                                  bool ascii_mode,
                                  bool capslock_simulation,
                                  bool composing,
                                  bool shortcut_modifier,
                                  bool known_key,
                                  WPARAM wParam) {
  if (!service_available)
    return false;
  if (!known_key)
    return false;
  if (capslock_simulation)
    return true;
  if (IsModeSwitchVirtualKey(wParam))
    return true;
  if (shortcut_modifier && !composing)
    return false;
  if (composing)
    return true;
  return !ascii_mode && IsTextVirtualKey(wParam);
}

inline bool ShouldEatTestKeyEvent(bool ascii_mode,
                                  bool capslock_simulation,
                                  bool composing,
                                  bool shortcut_modifier,
                                  bool known_key,
                                  WPARAM wParam) {
  return ShouldEatTestKeyEvent(true, ascii_mode, capslock_simulation, composing,
                               shortcut_modifier, known_key, wParam);
}

struct KeyEventTestCache {
  KeyEventTestCache()
      : entries(), next_entry(0), matched_entry(-1) {}

  bool Matches(bool is_key_up, WPARAM wParam, LPARAM lParam) const {
    matched_entry = -1;
    UINT32 key_identity = ComparableLParam(lParam);
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (entries[i].valid && entries[i].key_up == is_key_up &&
          entries[i].w_param == wParam &&
          ComparableLParam(entries[i].l_param) == key_identity) {
        matched_entry = static_cast<int>(i);
        return true;
      }
    }
    return false;
  }

  void Store(bool is_key_up, WPARAM wParam, LPARAM lParam, BOOL is_eaten) {
    size_t entry = next_entry;
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (!entries[i].valid ||
          (entries[i].key_up == is_key_up && entries[i].w_param == wParam &&
           entries[i].l_param == lParam)) {
        entry = i;
        break;
      }
    }
    entries[entry].valid = true;
    entries[entry].key_up = is_key_up;
    entries[entry].w_param = wParam;
    entries[entry].l_param = lParam;
    entries[entry].eaten = is_eaten ? TRUE : FALSE;
    matched_entry = static_cast<int>(entry);
    next_entry = (entry + 1) % ENTRY_COUNT;
  }

  BOOL Eaten() const {
    if (matched_entry < 0)
      return FALSE;
    return entries[matched_entry].eaten;
  }

  void Clear() {
    for (size_t i = 0; i < ENTRY_COUNT; ++i)
      entries[i] = Entry();
    next_entry = 0;
    matched_entry = -1;
  }

  void RemoveMatched() {
    if (matched_entry >= 0)
      entries[matched_entry].valid = false;
    matched_entry = -1;
  }

  static UINT32 ComparableLParam(LPARAM lParam) {
    // Repeat count and previous-key-state may differ between TestKey and Key
    // callbacks for the same physical key in some hosts.
    return static_cast<UINT32>(lParam) & 0xA1FF0000;
  }

  static UINT32 PhysicalKeyIdentity(LPARAM lParam) {
    return ComparableLParam(lParam) & ~0x80000000u;
  }

  struct Entry {
    Entry()
        : valid(false), key_up(false), w_param(0), l_param(0), eaten(FALSE) {}

    bool valid;
    bool key_up;
    WPARAM w_param;
    LPARAM l_param;
    BOOL eaten;
  };

  enum { ENTRY_COUNT = 4 };
  Entry entries[ENTRY_COUNT];
  size_t next_entry;
  mutable int matched_entry;
};

struct ActiveKeyDownGuard {
  ActiveKeyDownGuard() : entries(), next_entry(0) {}

  bool ShouldSuppress(WPARAM wParam, LPARAM lParam) {
    KeyInfo info(lParam);
    if (info.isKeyUp || info.prevKeyState)
      return false;

    UINT32 identity = KeyEventTestCache::PhysicalKeyIdentity(lParam);
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (entries[i].valid && entries[i].w_param == wParam &&
          entries[i].key_identity == identity) {
        return true;
      }
    }

    return false;
  }

  void Remember(WPARAM wParam, LPARAM lParam) {
    KeyInfo info(lParam);
    if (info.isKeyUp || info.prevKeyState)
      return;

    UINT32 identity = KeyEventTestCache::PhysicalKeyIdentity(lParam);
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (entries[i].valid && entries[i].w_param == wParam &&
          entries[i].key_identity == identity) {
        return;
      }
    }

    size_t entry = next_entry;
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (!entries[i].valid) {
        entry = i;
        break;
      }
    }
    entries[entry].valid = true;
    entries[entry].w_param = wParam;
    entries[entry].key_identity = identity;
    next_entry = (entry + 1) % ENTRY_COUNT;
  }

  void Release(WPARAM wParam, LPARAM lParam) {
    UINT32 identity = KeyEventTestCache::PhysicalKeyIdentity(lParam);
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      if (entries[i].valid && entries[i].w_param == wParam &&
          entries[i].key_identity == identity) {
        entries[i].valid = false;
      }
    }
  }

  void Reset() {
    for (size_t i = 0; i < ENTRY_COUNT; ++i)
      entries[i] = Entry();
    next_entry = 0;
  }

  struct Entry {
    Entry() : valid(false), w_param(0), key_identity(0) {}

    bool valid;
    WPARAM w_param;
    UINT32 key_identity;
  };

  enum { ENTRY_COUNT = 8 };
  Entry entries[ENTRY_COUNT];
  size_t next_entry;
};

struct KeyEventTestCacheReset {
  KeyEventTestCacheReset() : pending(false) {}

  void Mark() { pending.store(true); }

  bool Pending() const { return pending.load(); }

  bool Apply(KeyEventTestCache& cache) {
    if (!pending.exchange(false))
      return false;
    cache.Clear();
    return true;
  }

  std::atomic_bool pending;
};

}  // namespace weasel

bool ConvertKeyEvent(UINT vkey,
                     KeyInfo kinfo,
                     const LPBYTE keyState,
                     weasel::KeyEvent& result);

namespace ibus {
// keycodes

enum Keycode {
  VoidSymbol = 0xFFFFFF,
  space = 0x020,
  grave = 0x060,
  BackSpace = 0xFF08,
  Tab = 0xFF09,
  Linefeed = 0xFF0A,
  Clear = 0xFF0B,
  Return = 0xFF0D,
  Pause = 0xFF13,
  Scroll_Lock = 0xFF14,
  Sys_Req = 0xFF15,
  Escape = 0xFF1B,
  Delete = 0xFFFF,
  Multi_key = 0xFF20,
  Codeinput = 0xFF37,
  SingleCandidate = 0xFF3C,
  MultipleCandidate = 0xFF3D,
  PreviousCandidate = 0xFF3E,
  Kanji = 0xFF21,
  Muhenkan = 0xFF22,
  Henkan_Mode = 0xFF23,
  Henkan = 0xFF23,
  Romaji = 0xFF24,
  Hiragana = 0xFF25,
  Katakana = 0xFF26,
  Hiragana_Katakana = 0xFF27,
  Zenkaku = 0xFF28,
  Hankaku = 0xFF29,
  Zenkaku_Hankaku = 0xFF2A,
  Touroku = 0xFF2B,
  Massyo = 0xFF2C,
  Kana_Lock = 0xFF2D,
  Kana_Shift = 0xFF2E,
  Eisu_Shift = 0xFF2F,
  Eisu_toggle = 0xFF30,
  Kanji_Bangou = 0xFF37,
  Zen_Koho = 0xFF3D,
  Mae_Koho = 0xFF3E,
  Home = 0xFF50,
  Left = 0xFF51,
  Up = 0xFF52,
  Right = 0xFF53,
  Down = 0xFF54,
  Prior = 0xFF55,
  Page_Up = 0xFF55,
  Next = 0xFF56,
  Page_Down = 0xFF56,
  End = 0xFF57,
  Begin = 0xFF58,
  Select = 0xFF60,
  Print = 0xFF61,
  Execute = 0xFF62,
  Insert = 0xFF63,
  Undo = 0xFF65,
  Redo = 0xFF66,
  Menu = 0xFF67,
  Find = 0xFF68,
  Cancel = 0xFF69,
  Help = 0xFF6A,
  Break = 0xFF6B,
  Mode_switch = 0xFF7E,
  script_switch = 0xFF7E,
  Num_Lock = 0xFF7F,
  KP_Space = 0xFF80,
  KP_Tab = 0xFF89,
  KP_Enter = 0xFF8D,
  KP_F1 = 0xFF91,
  KP_F2 = 0xFF92,
  KP_F3 = 0xFF93,
  KP_F4 = 0xFF94,
  KP_Home = 0xFF95,
  KP_Left = 0xFF96,
  KP_Up = 0xFF97,
  KP_Right = 0xFF98,
  KP_Down = 0xFF99,
  KP_Prior = 0xFF9A,
  KP_Page_Up = 0xFF9A,
  KP_Next = 0xFF9B,
  KP_Page_Down = 0xFF9B,
  KP_End = 0xFF9C,
  KP_Begin = 0xFF9D,
  KP_Insert = 0xFF9E,
  KP_Delete = 0xFF9F,
  KP_Equal = 0xFFBD,
  KP_Multiply = 0xFFAA,
  KP_Add = 0xFFAB,
  KP_Separator = 0xFFAC,
  KP_Subtract = 0xFFAD,
  KP_Decimal = 0xFFAE,
  KP_Divide = 0xFFAF,
  KP_0 = 0xFFB0,
  KP_1 = 0xFFB1,
  KP_2 = 0xFFB2,
  KP_3 = 0xFFB3,
  KP_4 = 0xFFB4,
  KP_5 = 0xFFB5,
  KP_6 = 0xFFB6,
  KP_7 = 0xFFB7,
  KP_8 = 0xFFB8,
  KP_9 = 0xFFB9,
  F1 = 0xFFBE,
  F2 = 0xFFBF,
  F3 = 0xFFC0,
  F4 = 0xFFC1,
  F5 = 0xFFC2,
  F6 = 0xFFC3,
  F7 = 0xFFC4,
  F8 = 0xFFC5,
  F9 = 0xFFC6,
  F10 = 0xFFC7,
  F11 = 0xFFC8,
  L1 = 0xFFC8,
  F12 = 0xFFC9,
  L2 = 0xFFC9,
  F13 = 0xFFCA,
  L3 = 0xFFCA,
  F14 = 0xFFCB,
  L4 = 0xFFCB,
  F15 = 0xFFCC,
  L5 = 0xFFCC,
  F16 = 0xFFCD,
  L6 = 0xFFCD,
  F17 = 0xFFCE,
  L7 = 0xFFCE,
  F18 = 0xFFCF,
  L8 = 0xFFCF,
  F19 = 0xFFD0,
  L9 = 0xFFD0,
  F20 = 0xFFD1,
  L10 = 0xFFD1,
  F21 = 0xFFD2,
  R1 = 0xFFD2,
  F22 = 0xFFD3,
  R2 = 0xFFD3,
  F23 = 0xFFD4,
  R3 = 0xFFD4,
  F24 = 0xFFD5,
  R4 = 0xFFD5,
  F25 = 0xFFD6,
  R5 = 0xFFD6,
  F26 = 0xFFD7,
  R6 = 0xFFD7,
  F27 = 0xFFD8,
  R7 = 0xFFD8,
  F28 = 0xFFD9,
  R8 = 0xFFD9,
  F29 = 0xFFDA,
  R9 = 0xFFDA,
  F30 = 0xFFDB,
  R10 = 0xFFDB,
  F31 = 0xFFDC,
  R11 = 0xFFDC,
  F32 = 0xFFDD,
  R12 = 0xFFDD,
  F33 = 0xFFDE,
  R13 = 0xFFDE,
  F34 = 0xFFDF,
  R14 = 0xFFDF,
  F35 = 0xFFE0,
  R15 = 0xFFE0,
  Shift_L = 0xFFE1,
  Shift_R = 0xFFE2,
  Control_L = 0xFFE3,
  Control_R = 0xFFE4,
  Caps_Lock = 0xFFE5,
  Shift_Lock = 0xFFE6,
  Meta_L = 0xFFE7,
  Meta_R = 0xFFE8,
  Alt_L = 0xFFE9,
  Alt_R = 0xFFEA,
  Super_L = 0xFFEB,
  Super_R = 0xFFEC,
  Hyper_L = 0xFFED,
  Hyper_R = 0xFFEE,
  XK_bracketleft = 0x005b, /* U+005B LEFT SQUARE BRACKET */
  XK_c = 0x0063,           /* U+0063 LATIN SMALL LETTER C */
  XK_C = 0x0043,           /* U+0043 LATIN CAPITAL LETTER C */
  Null = 0
};

// modifiers, modified to fit a UINT16

enum Modifier {
  NULL_MASK = 0,

  SHIFT_MASK = 1 << 0,
  LOCK_MASK = 1 << 1,
  CONTROL_MASK = 1 << 2,
  ALT_MASK = 1 << 3,
  MOD1_MASK = 1 << 3,
  MOD2_MASK = 1 << 4,
  MOD3_MASK = 1 << 5,
  MOD4_MASK = 1 << 6,
  MOD5_MASK = 1 << 7,

  HANDLED_MASK = 1 << 8,  // 24
  IGNORED_MASK = 1 << 9,  // 25
  FORWARD_MASK = 1 << 9,  // 25

  SUPER_MASK = 1 << 10,  // 26
  HYPER_MASK = 1 << 11,  // 27
  META_MASK = 1 << 12,   // 28

  RELEASE_MASK = 1 << 14,  // 30

  MODIFIER_MASK = 0x2fff
};

}  // namespace ibus
