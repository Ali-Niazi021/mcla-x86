// Unit test of the classic cheats' pure logic (mcla/hooks/mcla_cheat_math.h, mcla_cheat.cpp): which screens a cheat may act on, the hold
// of one of the game's flag words (take, hold, put back, never take back what is not ours), the time scale (the game's own scale is
// multiplied, never compounded, a pause stays a pause, the base is the game's value of THIS frame), the money arithmetic with the
// game's own limits, and the pointer / argument plausibility rules.
// constexpr tests: the compiler runs them (`clang++ -fsyntax-only -std=c++23 -I mcla/hooks mcla/tests/test_cheat.cpp` is a test run),
// and main() runs them again at run time. Standalone: no SDK, no game, no clock.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_cheat_test).
#include <cstdint>
#include <cstdio>
#include <limits>

#include "mcla_cheat_math.h"

namespace m = mcla::cheat::math;
using mcla::gamestate::Screen;

namespace {

// ---- screens ---------------------------------------------------------------------------------------------------------------------

constexpr bool TestScreens() {
  // the continuous cheats: a drive only
  if (!m::DrivingOnly(Screen::kFreeRoam) || !m::DrivingOnly(Screen::kRacing)) return false;
  for (Screen s : {Screen::kUnknown, Screen::kFrontEnd, Screen::kLoading, Screen::kCutscene, Screen::kGarage, Screen::kPause, Screen::kMap}) {
    if (m::DrivingOnly(s)) return false;
  }
  // the money button and the unlock parameters: a drive, the pause menu, the garage, the map
  for (Screen s : {Screen::kFreeRoam, Screen::kRacing, Screen::kPause, Screen::kGarage, Screen::kMap}) {
    if (!m::CareerScreen(s)) return false;
  }
  for (Screen s : {Screen::kUnknown, Screen::kFrontEnd, Screen::kLoading, Screen::kCutscene}) {
    if (m::CareerScreen(s)) return false;
  }
  return true;
}
static_assert(TestScreens());

// It is the screen tracker's own notion of a drive (IsDriving is not constexpr: a run-time check).
bool TestScreensMatchTheTracker() {
  for (Screen s : {Screen::kUnknown, Screen::kFrontEnd, Screen::kLoading, Screen::kCutscene, Screen::kGarage, Screen::kFreeRoam,
                   Screen::kRacing, Screen::kPause, Screen::kMap}) {
    if (m::DrivingOnly(s) != mcla::gamestate::IsDriving(s)) return false;
  }
  return true;
}

// ---- the flag bytes ----------------------------------------------------------------------------------------------------------------

constexpr bool TestFlagLifecycle() {
  m::Hold h;
  // nothing wanted, nothing held: nothing happens
  if (m::StepFlag(h, false, 0).act != m::Act::kNone || h.held) return false;
  // taken over: the byte was 0, 1 is written, the original is remembered
  m::Step s = m::StepFlag(h, true, 0);
  if (s.act != m::Act::kTake || !s.write || s.value != 1 || !h.held || h.original != 0) return false;
  // held: the byte reads 1, nothing to do
  s = m::StepFlag(h, true, 1);
  if (s.act != m::Act::kNone || s.write) return false;
  // the game resets its cheats at a screen change: put back
  s = m::StepFlag(h, true, 0);
  if (s.act != m::Act::kRewrite || !s.write || s.value != 1) return false;
  // given back: the byte still holds our 1, the original 0 is written
  s = m::StepFlag(h, false, 1);
  if (s.act != m::Act::kRelease || !s.write || s.value != 0 || h.held) return false;
  // and again: nothing held, nothing to do
  return m::StepFlag(h, false, 0).act == m::Act::kNone;
}
static_assert(TestFlagLifecycle());

constexpr bool TestFlagTheGameHadItOn() {
  m::Hold h;
  // the game's own cheat menu had the byte on: taken without a write, and never taken back
  m::Step s = m::StepFlag(h, true, 1);
  if (s.act != m::Act::kTake || s.write || !h.held || h.original != 1) return false;
  s = m::StepFlag(h, true, 1);
  if (s.act != m::Act::kNone) return false;
  s = m::StepFlag(h, false, 1);
  return s.act == m::Act::kReleaseLeft && !s.write && !h.held;
}
static_assert(TestFlagTheGameHadItOn());

constexpr bool TestFlagChangedByTheGame() {
  m::Hold h;
  m::StepFlag(h, true, 0);  // taken, 1 written
  // at the moment of the release the byte is 0: the game (or the player in its cheat page) turned it off itself: nothing to write
  m::Step s = m::StepFlag(h, false, 0);
  if (s.act != m::Act::kReleaseLeft || s.write) return false;
  // a value the game wrote that is neither our 1 nor the original: left alone
  m::Hold g;
  m::StepFlag(g, true, 0);
  m::Step t = m::StepFlag(g, false, 2);
  return t.act == m::Act::kReleaseLeft && !t.write;
}
static_assert(TestFlagChangedByTheGame());

constexpr bool TestFlagNeverWritesWhenNotWanted() {
  // every state of the byte, nothing wanted and nothing held: no write
  for (int v = 0; v < 256; ++v) {
    m::Hold h;
    const m::Step s = m::StepFlag(h, false, uint8_t(v));
    if (s.write || h.held) return false;
  }
  return true;
}
static_assert(TestFlagNeverWritesWhenNotWanted());

constexpr bool TestFlagToggleFlap() {
  // the cheat switched on and off many times: after every release the byte is the game's original again
  m::Hold h;
  uint8_t byte = 0;
  for (int i = 0; i < 20; ++i) {
    m::Step s = m::StepFlag(h, true, byte);
    if (s.write) byte = s.value;
    if (byte != 1) return false;
    s = m::StepFlag(h, false, byte);
    if (s.write) byte = s.value;
    if (byte != 0) return false;
  }
  return true;
}
static_assert(TestFlagToggleFlap());

// ---- the parameter words ------------------------------------------------------------------------------------------------------------

constexpr bool TestParam() {
  m::Hold32 h;
  const uint32_t name = 0x82040123;
  // taken: the word is 0, the node's name pointer is written (a non-zero word is all the game tests)
  m::Step32 s = m::StepParam(h, true, 0, m::ParamArgument(name));
  if (s.act != m::Act::kTake || !s.write || s.value != name) return false;
  // held
  if (m::StepParam(h, true, name, name).write) return false;
  // given back
  s = m::StepParam(h, false, name, name);
  if (s.act != m::Act::kRelease || !s.write || s.value != 0) return false;
  // the command line had given the parameter: never written, never taken back
  m::Hold32 g;
  const uint32_t from_command_line = 0x40001000;
  s = m::StepParam(g, true, from_command_line, name);
  if (s.write || !g.held) return false;
  s = m::StepParam(g, false, from_command_line, name);
  if (s.write || s.act != m::Act::kReleaseLeft) return false;
  // a node without a name pointer still gets a non-zero word
  return m::ParamArgument(0) == 1u && m::ParamArgument(name) == name;
}
static_assert(TestParam());

// ---- the time scale ---------------------------------------------------------------------------------------------------------------

constexpr bool TestFactor() {
  if (m::ClampFactor(1.0) != 1.0 || m::ClampFactor(2.0) != 1.0 || m::ClampFactor(0.5) != 0.5) return false;
  if (m::ClampFactor(0.0) != m::kScaleMin || m::ClampFactor(-3.0) != m::kScaleMin || m::ClampFactor(0.01) != m::kScaleMin) return false;
  if (m::ClampFactor(std::numeric_limits<double>::quiet_NaN()) != 1.0) return false;  // garbage means off
  if (m::FactorIsOn(std::numeric_limits<double>::quiet_NaN())) return false;
  if (m::FactorIsOn(1.0) || m::FactorIsOn(5.0) || !m::FactorIsOn(0.99) || !m::FactorIsOn(0.05) || !m::FactorIsOn(0.0)) return false;
  return true;
}
static_assert(TestFactor());

constexpr bool TestScaleBasic() {
  m::ScaleState st;
  // the game stores its scale 1.0, the cheat asks 0.5: 0.5 is written, from the game's own value
  m::ScaleStep s = m::StepScale(st, true, 0.5, 1.0f);
  if (!s.write || s.value != 0.5f || s.base != 1.0f || !st.active) return false;
  // the next frame the game stores 1.0 again before the update: the base is that fresh 1.0, not our 0.5 (no compounding)
  s = m::StepScale(st, true, 0.5, 1.0f);
  if (!s.write || s.value != 0.5f) return false;
  // a frame where the game did not store (the word still holds our 0.5): the remembered base is used, the product is not halved again
  s = m::StepScale(st, true, 0.5, 0.5f);
  if (s.write || s.value != 0.5f || s.base != 1.0f) return false;
  // the factor changes live: 0.25 of the game's scale
  s = m::StepScale(st, true, 0.25, 0.5f);
  if (!s.write || s.value != 0.25f || s.base != 1.0f) return false;
  return true;
}
static_assert(TestScaleBasic());

constexpr bool TestScaleComposesWithTheGamesOwnSlowMotion() {
  // the game's own slow motion (a wipe-out) is a scale of 0.4: the cheat multiplies it
  m::ScaleState st;
  const m::ScaleStep s = m::StepScale(st, true, 0.5, 0.4f);
  return s.write && s.value == 0.2f && s.base == 0.4f;
}
static_assert(TestScaleComposesWithTheGamesOwnSlowMotion());

constexpr bool TestScalePauseStaysPause() {
  m::ScaleState st;
  // the game's pause: a scale of 0. Nothing is written, nothing is claimed
  m::ScaleStep s = m::StepScale(st, true, 0.5, 0.0f);
  if (s.write) return false;
  // not finite / absurd values are not touched
  const float huge = 1.0e30f;
  s = m::StepScale(st, true, 0.5, huge);
  if (s.write) return false;
  s = m::StepScale(st, true, 0.5, -1.0f);
  if (s.write) return false;
  // and a later real scale is taken as the game's own, not as ours
  s = m::StepScale(st, true, 0.5, 1.0f);
  return s.write && s.value == 0.5f && s.base == 1.0f;
}
static_assert(TestScalePauseStaysPause());

constexpr bool TestScaleRelease() {
  m::ScaleState st;
  m::StepScale(st, true, 0.5, 1.0f);
  // switched off while the word still holds our product: the game's scale is put back for this frame
  m::ScaleStep s = m::StepScale(st, false, 0.5, 0.5f);
  if (!s.write || s.value != 1.0f || st.active) return false;
  // switched off after the game stored its own value: nothing to put back
  m::ScaleState su;
  m::StepScale(su, true, 0.5, 1.0f);
  s = m::StepScale(su, false, 0.5, 1.0f);
  if (s.write || su.active) return false;
  // never active: never writes
  m::ScaleState sn;
  return !m::StepScale(sn, false, 0.5, 0.7f).write;
}
static_assert(TestScaleRelease());

constexpr bool TestScaleOnlyOneMultiplicationPerFrame() {
  // a long run: the game stores 1.0 every frame, the product never decays
  m::ScaleState st;
  float word = 1.0f;
  for (int i = 0; i < 1000; ++i) {
    word = 1.0f;  // the game's store
    const m::ScaleStep s = m::StepScale(st, true, 0.5, word);
    if (s.write) word = s.value;
    if (word != 0.5f) return false;
  }
  // and when the game skips its store for a while, the product still holds
  for (int i = 0; i < 1000; ++i) {
    const m::ScaleStep s = m::StepScale(st, true, 0.5, word);
    if (s.write) word = s.value;
    if (word != 0.5f) return false;
  }
  return true;
}
static_assert(TestScaleOnlyOneMultiplicationPerFrame());

// ---- money -------------------------------------------------------------------------------------------------------------------------

constexpr bool TestMoney() {
  using Op = m::MoneyOp;
  // give
  m::MoneyResult r = m::MoneyApply(5000, Op::kGive, 100000);
  if (!r.ok || r.value != 105000) return false;
  // give stops at the game's own ceiling (AddMoney: 0x7F8DCF00 = 2,140,000,000)
  r = m::MoneyApply(2'000'000'000, Op::kGive, 1'000'000'000);
  if (!r.ok || r.value != 2'140'000'000) return false;
  // take stops at 0: the cheat does not take the career negative
  r = m::MoneyApply(5000, Op::kTake, 100000);
  if (!r.ok || r.value != 0) return false;
  r = m::MoneyApply(150000, Op::kTake, 50000);
  if (!r.ok || r.value != 100000) return false;
  // set
  r = m::MoneyApply(5000, Op::kSet, 1'000'000);
  if (!r.ok || r.value != 1'000'000) return false;
  r = m::MoneyApply(5000, Op::kSet, 0);
  if (!r.ok || r.value != 0) return false;
  // a negative balance (the game allows it) can be given to: 0 is the floor of a take, a give works from where it is
  r = m::MoneyApply(-500, Op::kGive, 1000);
  if (!r.ok || r.value != 500) return false;
  // refusals: an amount out of range, no request, a stored value that is not the money
  if (m::MoneyApply(5000, Op::kGive, -1).ok || m::MoneyApply(5000, Op::kGive, 1'000'000'001).ok) return false;
  if (m::MoneyApply(5000, Op::kNone, 100).ok) return false;
  if (m::MoneyApply(2'200'000'000, Op::kGive, 1).ok || m::MoneyApply(-2'200'000'000, Op::kSet, 1).ok) return false;
  if (m::MoneyApply(5000, Op::kGive, 1).why != nullptr) return false;  // an accepted press has no reason
  if (m::MoneyApply(5000, Op::kGive, -1).why == nullptr) return false;
  // the request numbers
  if (m::MoneyOpOf(0) != Op::kNone || m::MoneyOpOf(1) != Op::kGive || m::MoneyOpOf(2) != Op::kTake || m::MoneyOpOf(3) != Op::kSet ||
      m::MoneyOpOf(4) != Op::kNone || m::MoneyOpOf(-1) != Op::kNone) {
    return false;
  }
  return true;
}
static_assert(TestMoney());

constexpr bool TestMoneyExtremes() {
  using Op = m::MoneyOp;
  // the result always fits the game's own range, whatever the inputs
  for (int64_t cur : {int64_t(-2'140'000'000), int64_t(-1), int64_t(0), int64_t(1), int64_t(5000), int64_t(2'139'999'999), int64_t(2'140'000'000)}) {
    for (Op op : {Op::kGive, Op::kTake, Op::kSet}) {
      for (int64_t amount : {int64_t(0), int64_t(1), int64_t(100000), int64_t(1'000'000'000)}) {
        const m::MoneyResult r = m::MoneyApply(cur, op, amount);
        if (!r.ok) return false;
        if (r.value < 0 || int64_t(r.value) > m::kMoneyMax) return false;
      }
    }
  }
  return true;
}
static_assert(TestMoneyExtremes());

// ---- pointers ----------------------------------------------------------------------------------------------------------------------

constexpr bool TestPointers() {
  if (!m::PlausibleHeapPointer(0x40123450) || !m::PlausibleHeapPointer(0xA0000010)) return false;
  if (m::PlausibleHeapPointer(0) || m::PlausibleHeapPointer(0x40123451) || m::PlausibleHeapPointer(0x00010000)) return false;
  if (m::PlausibleHeapPointer(0xFFFFFFF0) || m::PlausibleHeapPointer(0x12345678)) return false;
  return true;
}
static_assert(TestPointers());

}  // namespace

int main() {
  struct T {
    const char* name;
    bool ok;
  } tests[] = {
      {"screens", TestScreens()},
      {"screens match the tracker", TestScreensMatchTheTracker()},
      {"flag lifecycle", TestFlagLifecycle()},
      {"flag the game had on", TestFlagTheGameHadItOn()},
      {"flag changed by the game", TestFlagChangedByTheGame()},
      {"flag never written when not wanted", TestFlagNeverWritesWhenNotWanted()},
      {"flag toggled many times", TestFlagToggleFlap()},
      {"parameter words", TestParam()},
      {"factor", TestFactor()},
      {"time scale", TestScaleBasic()},
      {"time scale composes with the game's own", TestScaleComposesWithTheGamesOwnSlowMotion()},
      {"time scale: a pause stays a pause", TestScalePauseStaysPause()},
      {"time scale release", TestScaleRelease()},
      {"time scale never compounds", TestScaleOnlyOneMultiplicationPerFrame()},
      {"money", TestMoney()},
      {"money extremes", TestMoneyExtremes()},
      {"pointers", TestPointers()},
  };
  int failed = 0;
  for (const T& t : tests) {
    std::printf("%-44s %s\n", t.name, t.ok ? "ok" : "FAIL");
    failed += t.ok ? 0 : 1;
  }
  std::printf("%d failed\n", failed);
  return failed == 0 ? 0 : 1;
}
