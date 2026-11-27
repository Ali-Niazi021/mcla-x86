// mcla-x86 host overlay: controller handling, the pure parts. No SDK, no ImGui, no Windows: mcla/tests/test_overlay_pad.cpp
// compiles and runs it alone.
//
// What is here and who uses it (overlay_ui.cpp has the glue):
//
//   Raw / Digitize()      a pad's state (XInput layout) -> one bit per logical input: the 14 buttons, the triggers and the four
//                         directions of each stick past a threshold. Sticks and triggers have a hysteresis (on at about 52 % / 39 %,
//                         off at about 37 % / 24 %), so a stick resting near a threshold does not chatter.
//   ChordDetector         the pad's way to open and close the menu: a set of buttons held together for a time, with nothing else
//                         pressed. Fires once; fires again only after every chord button was let go.
//   HoldOff               "a button held across a change must not act until it is released": the inputs held when it began stay
//                         masked until each of them comes up. Atomic: the menu's close (UI thread) begins it and the game thread's
//                         poll ends it. Used for the game (nothing reaches it until the buttons that were held at the close are up) and
//                         for the menu (what was held at the open is not a press).
//   Repeater              press, wait, then repeat faster the longer the button is held (the left / right of a slider); the step size
//                         grows with the hold too (HoldMultiplier).
//   MenuPad               the menu's mapping: which inputs ImGui sees (up / down / left / right, A, B), and the actions it keeps for
//                         itself (LB / RB tabs, left / right changes a value, X reset, Y save, B back).
//   DeviceTracker         the last input device (pad, or keyboard and mouse), which decides the legend at the bottom of the menu.
//
// Time is milliseconds on any monotonic clock the caller picks (the overlay uses the steady clock).
//
// Everything is constexpr (HoldOff through BasicHoldOff<PlainCell>): the unit test evaluates it at compile time with static_assert,
// so `clang++ -fsyntax-only` on mcla/tests/test_overlay_pad.cpp is a test run.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

namespace mcla_overlay::pad {

// ---- logical inputs -------------------------------------------------------------------------------------------------------

// The low 16 bits are XInput's button bits (the guide button, 0x0400, is not used); the rest are made from the analog inputs.
enum Input : uint32_t {
  kDpadUp = 0x0001,
  kDpadDown = 0x0002,
  kDpadLeft = 0x0004,
  kDpadRight = 0x0008,
  kStart = 0x0010,
  kBack = 0x0020,
  kL3 = 0x0040,
  kR3 = 0x0080,
  kLB = 0x0100,
  kRB = 0x0200,
  kA = 0x1000,
  kB = 0x2000,
  kX = 0x4000,
  kY = 0x8000,
  kLT = 0x0001'0000,
  kRT = 0x0002'0000,
  kLsUp = 0x0004'0000,
  kLsDown = 0x0008'0000,
  kLsLeft = 0x0010'0000,
  kLsRight = 0x0020'0000,
  kRsUp = 0x0040'0000,
  kRsDown = 0x0080'0000,
  kRsLeft = 0x0100'0000,
  kRsRight = 0x0200'0000,
};
// The buttons proper (the 14 of the pad); a chord is made of these and nothing else may be down while it is held.
inline constexpr uint32_t kButtons = 0xF3FF;

// A pad's state as the SDK's input system reports it (XInput's layout: stick y is positive up).
struct Raw {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

inline constexpr int kStickOn = 17000;  // of 32767: 52 %
inline constexpr int kStickOff = 12000; // 37 %
inline constexpr int kTriggerOn = 100;  // of 255: 39 %
inline constexpr int kTriggerOff = 60;  // 24 %

namespace detail {
// `now` of an axis in one direction (value v, +1 / -1 = which way), with the previous state `was` for the hysteresis.
constexpr bool Past(int v, int sign, bool was) {
  const int m = sign > 0 ? v : -v;  // -(-32768) fits an int
  return m >= (was ? kStickOff : kStickOn);
}
}  // namespace detail

// `prev` = the bits of the previous call (0 the first time).
constexpr uint32_t Digitize(const Raw& r, uint32_t prev) {
  uint32_t out = uint32_t(r.buttons) & kButtons;
  if (int(r.lt) >= (prev & kLT ? kTriggerOff : kTriggerOn)) out |= kLT;
  if (int(r.rt) >= (prev & kRT ? kTriggerOff : kTriggerOn)) out |= kRT;
  if (detail::Past(r.ly, +1, (prev & kLsUp) != 0)) out |= kLsUp;
  if (detail::Past(r.ly, -1, (prev & kLsDown) != 0)) out |= kLsDown;
  if (detail::Past(r.lx, -1, (prev & kLsLeft) != 0)) out |= kLsLeft;
  if (detail::Past(r.lx, +1, (prev & kLsRight) != 0)) out |= kLsRight;
  if (detail::Past(r.ry, +1, (prev & kRsUp) != 0)) out |= kRsUp;
  if (detail::Past(r.ry, -1, (prev & kRsDown) != 0)) out |= kRsDown;
  if (detail::Past(r.rx, -1, (prev & kRsLeft) != 0)) out |= kRsLeft;
  if (detail::Past(r.rx, +1, (prev & kRsRight) != 0)) out |= kRsRight;
  return out;
}

// A stick axis as -1..+1 with a dead zone (the menu's scroll): 0 inside `dead` of full scale, then linear to 1.
constexpr float StickValue(int v, float dead = 0.25f) {
  const float f = std::clamp(float(v) / 32767.0f, -1.0f, 1.0f);
  const float a = f < 0.0f ? -f : f;
  if (a <= dead) return 0.0f;
  const float s = (a - dead) / (1.0f - dead);
  return f < 0.0f ? -s : s;
}

// ---- the chord --------------------------------------------------------------------------------------------------------------

// mcla_overlay_pad_chord: 0 off, 1 both stick clicks, 2 Back + Start, 3 LB + RB + Back.
inline constexpr int kChordCount = 4;
constexpr uint32_t ChordMask(int chord) {
  switch (chord) {
    case 1: return kL3 | kR3;
    case 2: return kBack | kStart;
    case 3: return kLB | kRB | kBack;
    default: return 0;
  }
}
constexpr const char* ChordText(int chord) {
  switch (chord) {
    case 1: return "L3 + R3";
    case 2: return "Back + Start";
    case 3: return "LB + RB + Back";
    default: return "off";
  }
}

class ChordDetector {
 public:
  // `down` = Digitize() bits now. True exactly once when every button of `mask` has been down for `hold_ms` with no other button
  // down (the triggers and the sticks do not count: a chord is made while driving). After it fired it needs all of the chord's
  // buttons to be released before it can fire again, whatever else is pressed meanwhile.
  constexpr bool Update(uint32_t down, uint32_t mask, int64_t now_ms, int64_t hold_ms) {
    if (mask == 0) {
      Reset();
      return false;
    }
    if (fired_) {
      if ((down & mask) == 0) Reset();
      return false;
    }
    const bool held = (down & mask) == mask && (down & kButtons & ~mask) == 0;
    if (!held) {
      since_ms_ = -1;
      return false;
    }
    if (since_ms_ < 0) since_ms_ = now_ms;
    if (now_ms - since_ms_ >= hold_ms) {
      fired_ = true;
      return true;
    }
    return false;
  }
  // How long the chord has been held (0 when it is not), for a log line.
  constexpr int64_t HeldMs(int64_t now_ms) const { return since_ms_ < 0 ? 0 : now_ms - since_ms_; }
  constexpr void Reset() {
    since_ms_ = -1;
    fired_ = false;
  }

 private:
  int64_t since_ms_ = -1;
  bool fired_ = false;
};

// ---- hold-off ---------------------------------------------------------------------------------------------------------------

// The storage of a HoldOff: std::atomic at run time, a plain value for the compile-time test.
template <class T>
struct PlainCell {
  T v{};
  constexpr T load(std::memory_order = std::memory_order_seq_cst) const { return v; }
  constexpr void store(T x, std::memory_order = std::memory_order_seq_cst) { v = x; }
  constexpr T fetch_and(T x, std::memory_order = std::memory_order_seq_cst) {
    const T old = v;
    v = T(v & x);
    return old;
  }
  constexpr T exchange(T x, std::memory_order = std::memory_order_seq_cst) {
    const T old = v;
    v = x;
    return old;
  }
};

template <template <class> class Cell>
class BasicHoldOff {
 public:
  static constexpr int64_t kTimeoutMs = 4000;  // a stuck button (a drifting stick) must not keep the game quiet for ever

  // The inputs in `held` are masked from now until each one comes up (any thread).
  constexpr void Begin(uint32_t held, int64_t now_ms) {
    start_ms_.store(now_ms, std::memory_order_relaxed);
    expired_.store(false, std::memory_order_relaxed);
    mask_.store(held, std::memory_order_seq_cst);
  }
  // One look at the pad. Inputs that came up leave the mask for good (a second press is a new press). True while any input held at
  // Begin() is still down. After kTimeoutMs the rest is let go and TakeExpired() says so once.
  constexpr bool Update(uint32_t down, int64_t now_ms) {
    if (mask_.load(std::memory_order_seq_cst) == 0) return false;
    const uint32_t now_mask = mask_.fetch_and(down, std::memory_order_seq_cst) & down;
    if (now_mask == 0) return false;
    if (now_ms - start_ms_.load(std::memory_order_relaxed) > kTimeoutMs) {
      mask_.store(0, std::memory_order_seq_cst);
      expired_.store(true, std::memory_order_relaxed);
      return false;
    }
    return true;
  }
  constexpr bool Active() const { return mask_.load(std::memory_order_seq_cst) != 0; }
  constexpr uint32_t Mask() const { return mask_.load(std::memory_order_seq_cst); }
  // True once after the timeout let go of something that was still held.
  constexpr bool TakeExpired() { return expired_.exchange(false, std::memory_order_relaxed); }
  constexpr void Clear() { mask_.store(0, std::memory_order_seq_cst); }
  constexpr int64_t StartMs() const { return start_ms_.load(std::memory_order_relaxed); }

 private:
  Cell<uint32_t> mask_{};
  Cell<int64_t> start_ms_{};
  Cell<bool> expired_{};
};

using HoldOff = BasicHoldOff<std::atomic>;         // what the overlay uses
using HoldOffModel = BasicHoldOff<PlainCell>;      // the same code, single-threaded and constexpr (the unit test)

// ---- repeat with acceleration --------------------------------------------------------------------------------------------------

struct RepeatTiming {
  int delay_ms = 380;   // from the press to the first repeat
  int start_ms = 150;   // the time between repeats at first
  int fast_ms = 45;     // ... and once the ramp is over
  int ramp_ms = 2200;   // how long the speed-up takes
};

// How far a hold of `held_ms` multiplies the size of one step: x1 for a second, then x2, x3 and x5 (a slider is crossed in about four
// seconds, not in a quarter of one).
constexpr int HoldMultiplier(int64_t held_ms) {
  if (held_ms < 1000) return 1;
  if (held_ms < 2000) return 2;
  if (held_ms < 3500) return 3;
  return 5;
}

class Repeater {
 public:
  static constexpr int kMaxStepsPerCall = 4;  // a stalled caller gets a few steps, not the whole backlog

  // The time between two repeats after the key had been held `since_repeat_ms` past the delay.
  static constexpr int IntervalAt(int64_t since_repeat_ms, const RepeatTiming& t) {
    const int64_t x = std::clamp<int64_t>(since_repeat_ms, 0, t.ramp_ms);
    return int(t.start_ms + (int64_t(t.fast_ms) - t.start_ms) * x / std::max(1, t.ramp_ms));
  }

  // `held` now, at `now_ms`. Returns how many steps are due: 1 on the press, none until the delay has passed, then as many as the
  // schedule says (usually 0 or 1 per call).
  constexpr int Update(bool held, int64_t now_ms, const RepeatTiming& t = RepeatTiming()) {
    if (!held) {
      active_ = false;
      return 0;
    }
    if (!active_) {
      active_ = true;
      press_ms_ = now_ms;
      next_ms_ = now_ms + t.delay_ms;
      return 1;
    }
    int steps = 0;
    while (now_ms >= next_ms_ && steps < kMaxStepsPerCall) {
      ++steps;
      next_ms_ += IntervalAt(next_ms_ - press_ms_ - t.delay_ms, t);
    }
    if (steps == kMaxStepsPerCall && now_ms >= next_ms_) next_ms_ = now_ms + IntervalAt(now_ms - press_ms_ - t.delay_ms, t);
    return steps;
  }
  constexpr bool active() const { return active_; }
  constexpr int64_t HeldMs(int64_t now_ms) const { return active_ ? now_ms - press_ms_ : 0; }
  constexpr void Reset() { active_ = false; }

 private:
  bool active_ = false;
  int64_t press_ms_ = 0;
  int64_t next_ms_ = 0;
};

// ---- the menu's mapping -----------------------------------------------------------------------------------------------------

// What the focused item of the menu is, which decides what left / right and A do.
enum class Focus : uint8_t {
  kOther,   // a button, a tab, anything ImGui moves around: left / right navigate, A presses
  kSlider,  // left / right change the value; A does nothing (ImGui's own "tweak" mode would fight the steps)
  kChoice,  // a combo: left / right pick the previous / next choice; A opens the list
  kToggle,  // a tick box: left switches it off, right on; A switches it
};

enum NavDir : uint8_t { kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8 };

struct MenuOut {
  uint8_t dirs = 0;           // ImGui's directional keys that are down (NavDir bits)
  bool activate = false;      // ImGui's activate key (A) is down
  bool cancel = false;        // ImGui's cancel key (B) is down
  int tab_step = 0;           // -1 (LB) / +1 (RB): one press
  int adjust = 0;             // steps due for the focused item: negative = left (Repeater: one on the press, more while held)
  int adjust_mult = 1;        // how big a step is after this long a hold (HoldMultiplier)
  bool adjust_press = false;  // the adjust is the first step of a new press
  bool reset = false;         // X pressed
  bool save = false;          // Y pressed
  bool back = false;          // B pressed
  uint32_t pressed = 0;       // every input that went down since the last call
};

class MenuPad {
 public:
  // `down`: Digitize() bits with the hold-off applied.
  constexpr MenuOut Update(uint32_t down, int64_t now_ms, Focus focus) {
    MenuOut o;
    o.pressed = down & ~prev_;
    prev_ = down;
    bool up = (down & (kDpadUp | kLsUp)) != 0;
    bool dn = (down & (kDpadDown | kLsDown)) != 0;
    bool lf = (down & (kDpadLeft | kLsLeft)) != 0;
    bool rt = (down & (kDpadRight | kLsRight)) != 0;
    if (up && dn) up = dn = false;  // opposite directions cancel
    if (lf && rt) lf = rt = false;
    if (up) o.dirs |= kNavUp;
    if (dn) o.dirs |= kNavDown;
    if (focus == Focus::kOther) {
      if (lf) o.dirs |= kNavLeft;
      if (rt) o.dirs |= kNavRight;
      left_.Reset();
      right_.Reset();
    } else {
      const bool left_new = lf && !left_.active();
      const bool right_new = rt && !right_.active();
      const int l = left_.Update(lf, now_ms);
      const int r = right_.Update(rt, now_ms);
      o.adjust = r - l;
      o.adjust_press = left_new || right_new;
      const int64_t held = lf ? left_.HeldMs(now_ms) : rt ? right_.HeldMs(now_ms) : 0;
      o.adjust_mult = HoldMultiplier(held);
    }
    o.activate = (down & kA) != 0 && focus != Focus::kSlider;
    o.cancel = (down & kB) != 0;
    o.tab_step = ((o.pressed & kRB) ? 1 : 0) - ((o.pressed & kLB) ? 1 : 0);
    o.reset = (o.pressed & kX) != 0;
    o.save = (o.pressed & kY) != 0;
    o.back = (o.pressed & kB) != 0;
    return o;
  }
  constexpr void Reset() {
    prev_ = 0;
    left_.Reset();
    right_.Reset();
  }

 private:
  uint32_t prev_ = 0;
  Repeater left_, right_;
};

// A step of a slider: `steps` presses of `mult` times `unit`, from `value`, kept inside [lo, hi]. An integer slider has unit 1; a
// float one a fiftieth of its range.
constexpr double StepValue(double value, int steps, int mult, double unit, double lo, double hi) {
  return std::clamp(value + double(steps) * double(std::max(1, mult)) * unit, lo, hi);
}

// The index of the next choice of a list of `count`, `steps` away from `current` (clamped at both ends: 16x does not wrap to off).
constexpr int StepChoice(int current, int steps, int count) {
  if (count <= 0) return 0;
  return std::clamp(current + steps, 0, count - 1);
}

// ---- the last input device -----------------------------------------------------------------------------------------------------

enum class Device : uint8_t { kKeyboardMouse, kPad };

class DeviceTracker {
 public:
  static constexpr float kMouseSlopPx = 8.0f;  // a mouse that moved less than this since it last counted is a bump of the desk

  constexpr Device last() const { return last_; }
  constexpr void Force(Device d) { last_ = d; }

  // A key went down (not an auto-repeat), a mouse button went down, the wheel turned: the player is at the keyboard or the mouse.
  constexpr void NoteKey() { last_ = Device::kKeyboardMouse; }
  // The mouse is at (x, y): it counts once it is `kMouseSlopPx` away from where it last counted (the first position only anchors).
  constexpr void NoteMouseMove(float x, float y) {
    if (!anchored_) {
      anchored_ = true;
      ax_ = x;
      ay_ = y;
      return;
    }
    const float dx = x - ax_, dy = y - ay_;
    if (dx * dx + dy * dy >= kMouseSlopPx * kMouseSlopPx) {
      ax_ = x;
      ay_ = y;
      last_ = Device::kKeyboardMouse;
    }
  }
  // The pad: an input went down that was not down in the previous look (a stick or trigger counts when it crosses its threshold, so
  // drift inside it never takes over).
  constexpr void NotePad(uint32_t down, uint32_t prev) {
    if (down & ~prev) last_ = Device::kPad;
  }

 private:
  Device last_ = Device::kKeyboardMouse;
  bool anchored_ = false;
  float ax_ = 0.0f, ay_ = 0.0f;
};

}  // namespace mcla_overlay::pad
