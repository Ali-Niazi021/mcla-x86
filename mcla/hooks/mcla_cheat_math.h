// Midnight Club: Los Angeles: the pure logic of the classic cheats (mcla_cheat.cpp). No SDK, no guest memory, no cvars, no clock:
// everything here is a function of numbers handed in (std and mcla_game_state.h only), so a unit test compiles it standalone
// (mcla/tests/test_cheat.cpp). The glue (addresses, cvars, log lines) is mcla_cheat.cpp; the evidence is docs/CHEATS.md.
//
//   * which screens a cheat may act on (continuous ones: only while the player steers; the money button and the unlock
//     predicates: the stable screens of a career; the no-save switch: every screen),
//   * the hold of one of the game's own flag bytes: remember the byte the game had, hold it, put it back only if it is still ours,
//   * the time scale: the game stores its own scale into the timer every frame, the cheat multiplies THAT value (so a pause, which
//     is a scale of 0, stays a pause), and never compounds on its own earlier product,
//   * the money arithmetic with the game's own limits, and the plausibility checks of the pointers the cheats would write through.
#pragma once

#include <bit>
#include <cstdint>

#include "mcla_game_state.h"

namespace mcla::cheat::math {

using mcla::gamestate::Screen;

// ---- screens --------------------------------------------------------------------------------------------------------------

// The cheats that change how the game plays (nitro, special abilities, damage, police, slow motion) act only while the player
// steers: free roam or a race. In a menu, a load, a cutscene or the garage they are released (the game's own value written back).
constexpr bool DrivingOnly(Screen s) { return s == Screen::kFreeRoam || s == Screen::kRacing; }  // = gamestate::IsDriving

// The money button and the unlock predicates act on the screens where a career exists and nothing is being built or saved: a drive,
// the pause menu, the garage, the career map. Never on a loading screen, the front end (title, legals, results, lobby), a cutscene,
// or before the tracker knows the screen.
constexpr bool CareerScreen(Screen s) {
  return DrivingOnly(s) || s == Screen::kPause || s == Screen::kGarage || s == Screen::kMap;
}

// ---- holding one of the game's own flag words ---------------------------------------------------------------------------------
// A flag byte of the game's cheat block, or the value word of a command-line parameter: the cheat writes `on` into it while it is
// wanted and gives the game's value back afterwards, but only if the word still holds what the cheat wrote (a word the game changed
// itself is not ours to put back). A word the game already had "on" (its own cheat menu, a command-line parameter) is never written
// and never taken back.

template <class T>
struct HoldT {
  bool held = false;  // the cheat has taken the word over
  T original{};       // what the game had when it was taken over
  T written{};        // what the cheat stored (the value the word has to still hold for the give-back)
};

enum class Act : uint8_t {
  kNone,         // nothing to do
  kTake,         // taken over now (write `value` when `write` is set: the word was not already on)
  kRewrite,      // the game cleared or changed it while held: put it back (the game resets its cheats at some screen changes)
  kRelease,      // given back: write the original
  kReleaseLeft,  // given back, but the word is not ours to restore (the game had it on, or changed it): left as it is
};

template <class T>
struct StepT {
  Act act = Act::kNone;
  bool write = false;
  T value{};
};

// One frame of a held word. `want`: the cheat is on and allowed to act now. `current`: the word as it is in guest memory now.
// `on`: what the cheat stores. `is_on(v)`: the game's reader sees the cheat as on for the value v (every reader here tests != 0).
template <class T, class IsOn>
constexpr StepT<T> StepHoldT(HoldT<T>& h, bool want, T current, T on, IsOn is_on) {
  StepT<T> s;
  if (want && !h.held) {
    h.held = true;
    h.original = current;
    s.act = Act::kTake;
    if (is_on(current)) {
      h.written = current;  // the game has it on already: nothing is written, nothing will be taken back
    } else {
      s.write = true;
      s.value = on;
      h.written = on;
    }
  } else if (want && h.held) {
    if (!is_on(current)) {
      s.act = Act::kRewrite;
      s.write = true;
      s.value = on;
      h.written = on;
    }
  } else if (!want && h.held) {
    h.held = false;
    if (current == h.written && !is_on(h.original)) {
      s.act = Act::kRelease;
      s.write = true;
      s.value = h.original;
    } else {
      s.act = Act::kReleaseLeft;
    }
  }
  return s;
}

constexpr bool NonZero8(uint8_t v) { return v != 0; }
constexpr bool NonZero32(uint32_t v) { return v != 0; }

using Hold = HoldT<uint8_t>;      // a flag byte
using Step = StepT<uint8_t>;
using Hold32 = HoldT<uint32_t>;   // a parameter's value word
using Step32 = StepT<uint32_t>;

constexpr Step StepFlag(Hold& h, bool want, uint8_t current) { return StepHoldT<uint8_t>(h, want, current, uint8_t(1), NonZero8); }
constexpr Step32 StepParam(Hold32& h, bool want, uint32_t current, uint32_t on) {
  return StepHoldT<uint32_t>(h, want, current, on, NonZero32);
}

// ---- the time scale ---------------------------------------------------------------------------------------------------------

inline constexpr double kScaleMin = 0.05;  // 1/20 of the speed: slower than that is a freeze, not a slow motion
inline constexpr double kScaleMax = 1.0;   // never faster: the timer clamps a step above its maximum anyway

// The factor the cvar asks for, as the timer will see it: NaN and anything above 1 mean 1 (off), below the minimum the minimum.
constexpr double ClampFactor(double v) {
  if (!(v == v)) return 1.0;
  if (v >= kScaleMax) return 1.0;
  return v < kScaleMin ? kScaleMin : v;
}
constexpr bool FactorIsOn(double v) { return ClampFactor(v) < kScaleMax; }

struct ScaleState {
  bool active = false;       // the cheat has written the timer's scale and not given it back yet
  float last_written = 0.f;  // the bits we stored last frame
  float last_base = 1.f;     // the game's own scale that product was made from
};

struct ScaleStep {
  bool write = false;  // store `value` into the timer's scale word
  float value = 0.f;
  float base = 1.f;    // the game's own scale this frame (for the log)
};

constexpr bool SameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
constexpr bool IsFinite(float x) { return x == x && x - x == 0.0f; }

// One frame, called before the timer reads its scale. `current` is the word in the timer. The game stores its own scale there every
// frame before the update (sub_822C1FA8, 0x822C2124 / 0x822C22D0), so `current` is normally fresh; when the update ran without that
// store (current is still our own product) the base is the one remembered, never the product (no compounding).
// A base that is not finite, not above 0 (the game's pause is a scale of 0) or above 4 is left alone: the cheat never touches what it
// does not understand.
constexpr ScaleStep StepScale(ScaleState& s, bool want, double factor, float current) {
  ScaleStep r;
  float base = current;
  if (s.active && SameBits(current, s.last_written)) base = s.last_base;
  r.base = base;
  const bool sane = IsFinite(base) && base > 0.0f && base <= 4.0f;
  if (want && sane) {
    const float value = float(double(base) * ClampFactor(factor));
    r.value = value;
    r.write = !SameBits(value, current);
    s.active = true;
    s.last_written = value;
    s.last_base = base;
  } else if (want) {
    // paused (0) or a value we do not understand: write nothing, and forget the product so a later 0.5 is not taken for ours
    s.last_written = -1.f;
  } else if (s.active) {
    s.active = false;
    if (SameBits(current, s.last_written)) {  // the word still holds our product: give the game's scale back for this frame
      r.write = true;
      r.value = s.last_base;
      r.base = s.last_base;
    }
  }
  return r;
}

// ---- money ------------------------------------------------------------------------------------------------------------------

inline constexpr int64_t kMoneyMax = 2'140'000'000;  // the game's own ceiling in AddMoney (sub_826B93E0: 0x7F8DCF00)
inline constexpr int64_t kMoneyFloor = -2'140'000'000;  // the game's own floor in SpendMoney (0x80723100)
inline constexpr int64_t kMoneyStep = 1'000'000'000;    // the largest amount one press moves

enum class MoneyOp : int32_t { kNone = 0, kGive = 1, kTake = 2, kSet = 3 };

struct MoneyResult {
  bool ok = false;
  int32_t value = 0;           // the new amount
  const char* why = nullptr;   // when !ok: one phrase for the log and the menu
};

constexpr MoneyOp MoneyOpOf(int32_t v) { return v >= 1 && v <= 3 ? MoneyOp(v) : MoneyOp::kNone; }
constexpr const char* MoneyOpName(MoneyOp op) {
  switch (op) {
    case MoneyOp::kGive: return "give";
    case MoneyOp::kTake: return "take";
    case MoneyOp::kSet: return "set";
    default: return "none";
  }
}

// The amount after one press. A stored amount outside the game's own limits means the field is not what we think it is: refused.
// Give and set stop at the game's ceiling; take stops at 0 (the game would go negative, the cheat does not).
constexpr MoneyResult MoneyApply(int64_t current, MoneyOp op, int64_t amount) {
  MoneyResult r;
  if (current < kMoneyFloor || current > kMoneyMax) {
    r.why = "the stored amount is not plausible (not the career's money)";
    return r;
  }
  if (amount < 0 || amount > kMoneyStep) {
    r.why = "the amount is out of range";
    return r;
  }
  int64_t v = current;
  switch (op) {
    case MoneyOp::kGive: v = current + amount; break;
    case MoneyOp::kTake: v = current - amount; break;
    case MoneyOp::kSet: v = amount; break;
    default: r.why = "no request"; return r;
  }
  if (v > kMoneyMax) v = kMoneyMax;
  if (v < 0) v = 0;
  r.ok = true;
  r.value = int32_t(v);
  return r;
}

// ---- guest pointers ---------------------------------------------------------------------------------------------------------

// An object the game allocated on its heap: 4-aligned, in the range the guest heaps live in. The cheats also check a known vtable or
// name before they write through a pointer; this only keeps a garbage word from being used as an address at all.
constexpr bool PlausibleHeapPointer(uint32_t p) { return (p & 3u) == 0 && p >= 0x20000000u && p < 0xF0000000u; }

// The word a command-line parameter holds when it is given: the game only tests it for non-zero, the stock parser stores a pointer
// to the argument text. The cheat stores the node's own name pointer (a valid, non-empty string); 1 if the node has none.
constexpr uint32_t ParamArgument(uint32_t name_pointer) { return name_pointer != 0 ? name_pointer : 1u; }

}  // namespace mcla::cheat::math
