// Unit test of the overlay's controller logic (src/host/overlay/overlay_pad.h): the digitising of a pad's state, the chord that
// opens the menu (with its hold time), the hold-off that keeps a held button from reaching the game, the repeat with acceleration,
// the menu's mapping of the pad, and the last-input-device rule. Standalone: no SDK, no ImGui, no Windows, no game.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_overlay_pad_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I src/host/overlay mcla/tests/test_overlay_pad.cpp -o <scratch>/test_overlay_pad && <scratch>/test_overlay_pad
#include <cstdint>
#include <cstdio>

#include "overlay_pad.h"

namespace p = mcla_overlay::pad;

// Every test is a constexpr function that returns 0 when it passes and the line of the first failed check otherwise. The compiler
// evaluates each one in a static_assert (so `clang++ -fsyntax-only` runs the whole test), and main() calls them again at run time and
// adds a check of the atomic HoldOff, the one thing the constant evaluator cannot run.
namespace {

#define CHECK(cond)           do {                          if (!(cond)) return __LINE__;   } while (0)

constexpr double Abs(double v) { return v < 0.0 ? -v : v; }
#define CHECK_NEAR(got, want, tol) CHECK(Abs(double(got) - double(want)) <= (tol))

constexpr bool Eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

constexpr p::Raw Buttons(uint16_t b) {
  p::Raw r;
  r.buttons = b;
  return r;
}

// ---- Digitize ---------------------------------------------------------------------------------------------------------------

constexpr int TestDigitize() {
  // The 14 buttons pass through; the guide button (0x0400) and the unused bits (0x0800) do not.
  CHECK(p::Digitize(Buttons(0x0001), 0) == p::kDpadUp);
  CHECK(p::Digitize(Buttons(0x1000 | 0x2000 | 0x4000 | 0x8000), 0) == (p::kA | p::kB | p::kX | p::kY));
  CHECK(p::Digitize(Buttons(0x0400 | 0x0800), 0) == 0);
  CHECK(p::Digitize(Buttons(0x0040 | 0x0080), 0) == (p::kL3 | p::kR3));
  CHECK(p::Digitize(Buttons(0x0010 | 0x0020), 0) == (p::kStart | p::kBack));
  CHECK(p::Digitize(Buttons(0x0100 | 0x0200), 0) == (p::kLB | p::kRB));
  CHECK(p::Digitize(Buttons(0xFFFF), 0) == p::kButtons);

  // Sticks: XInput's y is positive up. On at 17000, off below 12000 (hysteresis).
  p::Raw r;
  r.ly = 16999;
  CHECK(p::Digitize(r, 0) == 0);
  r.ly = 17000;
  CHECK(p::Digitize(r, 0) == p::kLsUp);
  r.ly = 12500;  // between: stays on when it was on, stays off when it was off
  CHECK(p::Digitize(r, p::kLsUp) == p::kLsUp);
  CHECK(p::Digitize(r, 0) == 0);
  r.ly = 11999;
  CHECK(p::Digitize(r, p::kLsUp) == 0);
  r.ly = -20000;
  CHECK(p::Digitize(r, 0) == p::kLsDown);
  r.ly = 0;
  r.lx = -20000;
  CHECK(p::Digitize(r, 0) == p::kLsLeft);
  r.lx = 32767;
  CHECK(p::Digitize(r, 0) == p::kLsRight);
  r.lx = INT16_MIN;
  CHECK(p::Digitize(r, 0) == p::kLsLeft);  // -32768 does not overflow
  r = p::Raw();
  r.rx = 30000;
  r.ry = -30000;
  CHECK(p::Digitize(r, 0) == (p::kRsRight | p::kRsDown));
  r = p::Raw();
  r.rx = -30000;
  r.ry = 30000;
  CHECK(p::Digitize(r, 0) == (p::kRsLeft | p::kRsUp));

  // Triggers: on at 100, off below 60.
  r = p::Raw();
  r.lt = 99;
  r.rt = 100;
  CHECK(p::Digitize(r, 0) == p::kRT);
  r.lt = 80;
  r.rt = 80;
  CHECK(p::Digitize(r, p::kLT | p::kRT) == (p::kLT | p::kRT));
  CHECK(p::Digitize(r, 0) == 0);
  r.lt = 59;
  CHECK(p::Digitize(r, p::kLT) == 0);

  // A diagonal is two directions, a resting pad is nothing.
  r = p::Raw();
  r.lx = 25000;
  r.ly = 25000;
  CHECK(p::Digitize(r, 0) == (p::kLsRight | p::kLsUp));
  CHECK(p::Digitize(p::Raw(), 0) == 0);
  return 0;
}

constexpr int TestStickValue() {
  CHECK_NEAR(p::StickValue(0), 0.0, 1e-9);
  CHECK_NEAR(p::StickValue(8000), 0.0, 1e-9);  // inside the dead zone (0.25 of 32767 = 8191)
  CHECK_NEAR(p::StickValue(32767), 1.0, 1e-6);
  CHECK_NEAR(p::StickValue(-32768), -1.0, 1e-6);
  CHECK_NEAR(p::StickValue(20000, 0.25f), (20000.0 / 32767.0 - 0.25) / 0.75, 1e-5);
  CHECK(p::StickValue(-20000) < 0.0f);
  return 0;
}

// ---- the chord ----------------------------------------------------------------------------------------------------------------

constexpr int TestChordNames() {
  CHECK(p::ChordMask(0) == 0);
  CHECK(p::ChordMask(1) == (p::kL3 | p::kR3));
  CHECK(p::ChordMask(2) == (p::kBack | p::kStart));
  CHECK(p::ChordMask(3) == (p::kLB | p::kRB | p::kBack));
  CHECK(p::ChordMask(99) == 0);
  CHECK(p::ChordMask(-1) == 0);
  CHECK(Eq(p::ChordText(1), "L3 + R3"));
  CHECK(Eq(p::ChordText(0), "off"));
  return 0;
}

constexpr int TestChordHold() {
  p::ChordDetector c;
  const uint32_t mask = p::kL3 | p::kR3;
  // Held from t = 1000: nothing before 500 ms, fires exactly at 500.
  CHECK(!c.Update(mask, mask, 1000, 500));
  CHECK(!c.Update(mask, mask, 1100, 500));
  CHECK(!c.Update(mask, mask, 1499, 500));
  CHECK(c.Update(mask, mask, 1500, 500));
  CHECK(c.HeldMs(1500) == 500);
  // Once: it does not fire again while the chord stays down.
  CHECK(!c.Update(mask, mask, 1600, 500));
  CHECK(!c.Update(mask, mask, 5000, 500));
  // One button up does not re-arm it (the other is still down) ...
  CHECK(!c.Update(p::kL3, mask, 5100, 500));
  CHECK(!c.Update(mask, mask, 9000, 500));
  // ... all of them up does.
  CHECK(!c.Update(0, mask, 9100, 500));
  CHECK(!c.Update(mask, mask, 9200, 500));
  CHECK(!c.Update(mask, mask, 9699, 500));
  CHECK(c.Update(mask, mask, 9700, 500));
  return 0;
}

constexpr int TestChordBroken() {
  const uint32_t mask = p::kBack | p::kStart;
  p::ChordDetector c;
  // Released early: the timer starts again.
  CHECK(!c.Update(mask, mask, 0, 500));
  CHECK(!c.Update(mask, mask, 400, 500));
  CHECK(!c.Update(p::kBack, mask, 450, 500));
  CHECK(c.HeldMs(450) == 0);
  CHECK(!c.Update(mask, mask, 500, 500));
  CHECK(!c.Update(mask, mask, 900, 500));  // 400 ms since 500
  CHECK(c.Update(mask, mask, 1000, 500));

  // Another button down breaks it (a chord is a deliberate act): A pressed at 200 resets the timer, and the chord counts from the A's
  // release.
  p::ChordDetector d;
  CHECK(!d.Update(mask, mask, 0, 500));
  CHECK(!d.Update(mask | p::kA, mask, 200, 500));
  CHECK(!d.Update(mask, mask, 300, 500));
  CHECK(!d.Update(mask, mask, 700, 500));  // 400 ms since 300
  CHECK(d.Update(mask, mask, 800, 500));
  // After it fired, an extra button pressed and released does not arm a second fire while the chord is still down.
  CHECK(!d.Update(mask | p::kX, mask, 900, 500));
  CHECK(!d.Update(mask, mask, 1000, 500));
  CHECK(!d.Update(mask, mask, 3000, 500));

  // Triggers and sticks do not count as "another button": a chord is made while driving.
  p::ChordDetector e;
  const uint32_t analog = p::kRT | p::kLT | p::kLsUp | p::kRsLeft;
  CHECK(!e.Update(mask | analog, mask, 0, 300));
  CHECK(e.Update(mask | analog, mask, 300, 300));

  // The chord's own buttons only: a part of it is nothing, and with no chord (0) nothing ever fires.
  p::ChordDetector f;
  CHECK(!f.Update(p::kStart, mask, 0, 0));
  CHECK(!f.Update(p::kStart, mask, 10000, 0));
  CHECK(!f.Update(mask, 0, 0, 0));
  CHECK(!f.Update(mask, 0, 10000, 0));
  // A zero hold time fires on the first full look.
  CHECK(f.Update(mask, mask, 10001, 0));

  // The hold time is the cvar's: a longer one waits longer.
  p::ChordDetector g;
  CHECK(!g.Update(p::kL3 | p::kR3, p::kL3 | p::kR3, 0, 2000));
  CHECK(!g.Update(p::kL3 | p::kR3, p::kL3 | p::kR3, 1999, 2000));
  CHECK(g.Update(p::kL3 | p::kR3, p::kL3 | p::kR3, 2000, 2000));
  return 0;
}

constexpr int TestChordThreeButtons() {
  const uint32_t mask = p::ChordMask(3);  // LB + RB + Back
  p::ChordDetector c;
  CHECK(!c.Update(p::kLB | p::kRB, mask, 0, 500));
  CHECK(!c.Update(p::kLB | p::kRB, mask, 800, 500));  // Back never came
  CHECK(!c.Update(mask, mask, 900, 500));
  CHECK(c.Update(mask, mask, 1400, 500));
  return 0;
}

// ---- hold-off ---------------------------------------------------------------------------------------------------------------

constexpr int TestHoldOff() {
  p::HoldOffModel h;
  CHECK(!h.Active());
  CHECK(!h.Update(p::kA, 0));
  // B (the close) and the left stick held at the close.
  h.Begin(p::kB | p::kLsUp, 1000);
  CHECK(h.Active());
  CHECK(h.Mask() == (p::kB | p::kLsUp));
  CHECK(h.Update(p::kB | p::kLsUp, 1010));
  // The stick comes up: only B remains.
  CHECK(h.Update(p::kB, 1100));
  CHECK(h.Mask() == p::kB);
  // B comes back after having been released? It is a new press: not masked. First the release:
  CHECK(!h.Update(0, 1200));
  CHECK(!h.Active());
  CHECK(!h.Update(p::kB, 1300));  // pressed again: the game gets it
  CHECK(h.Mask() == 0);
  // A different button pressed while one is held is not held back.
  h.Begin(p::kB, 2000);
  CHECK(h.Update(p::kB | p::kA, 2010));
  CHECK(h.Mask() == p::kB);
  CHECK(!h.Update(p::kA, 2020));
  // Nothing held at the close: nothing is held off.
  h.Begin(0, 3000);
  CHECK(!h.Active());
  CHECK(!h.Update(p::kA | p::kB, 3010));
  // Clear lets go at once.
  h.Begin(p::kStart, 4000);
  h.Clear();
  CHECK(!h.Active());
  CHECK(!h.Update(p::kStart, 4001));
  return 0;
}

constexpr int TestHoldOffTimeout() {
  p::HoldOffModel h;
  h.Begin(p::kLsRight, 0);  // a drifting stick that never comes up
  CHECK(h.Update(p::kLsRight, 1000));
  CHECK(h.Update(p::kLsRight, p::HoldOff::kTimeoutMs));
  CHECK(!h.TakeExpired());
  CHECK(!h.Update(p::kLsRight, p::HoldOff::kTimeoutMs + 1));  // let go
  CHECK(!h.Active());
  CHECK(h.TakeExpired());
  CHECK(!h.TakeExpired());  // once
  // A normal release does not report an expiry.
  h.Begin(p::kA, 10000);
  CHECK(!h.Update(0, 10100));
  CHECK(!h.TakeExpired());
  return 0;
}

// The whole story of a close with a button held, as the game thread and the UI thread play it (one engine frame = one Update).
constexpr int TestCloseWithHeldButton() {
  p::HoldOffModel game;
  // Frame 1: B pressed. Frame 2: still down, and the UI thread closes the menu (it begins the hold-off with what is held).
  const uint32_t frames[8] = {p::kB, p::kB, p::kB, p::kB, 0, 0, p::kB, p::kB};
  bool quiet[8] = {};
  for (int i = 0; i < 8; ++i) {
    if (i == 2) game.Begin(frames[i], int64_t(i) * 33);  // the close
    quiet[i] = game.Update(frames[i], int64_t(i) * 33);
  }
  // Quiet at the close, while B is down, until it is released; a second press after that is the game's.
  CHECK(quiet[2] && quiet[3]);
  CHECK(!quiet[4]);
  CHECK(!quiet[5]);
  CHECK(!quiet[6] && !quiet[7]);
  return 0;
}

// ---- repeat -------------------------------------------------------------------------------------------------------------------

constexpr int TestRepeater() {
  p::Repeater r;
  const p::RepeatTiming t;
  // A press is one step at once; none until the delay; then repeats.
  CHECK(r.Update(true, 0, t) == 1);
  CHECK(r.Update(true, 100, t) == 0);
  CHECK(r.Update(true, t.delay_ms - 1, t) == 0);
  CHECK(r.Update(true, t.delay_ms, t) == 1);
  CHECK(r.Update(true, t.delay_ms + 10, t) == 0);
  CHECK(r.Update(true, t.delay_ms + t.start_ms, t) == 1);
  // Released: nothing, and a new press is a step again.
  CHECK(r.Update(false, 700, t) == 0);
  CHECK(!r.active());
  CHECK(r.Update(true, 710, t) == 1);
  return 0;
}

constexpr int TestRepeaterAccelerates() {
  // The interval shrinks from start_ms to fast_ms over ramp_ms.
  const p::RepeatTiming t;
  CHECK(p::Repeater::IntervalAt(0, t) == t.start_ms);
  CHECK(p::Repeater::IntervalAt(t.ramp_ms, t) == t.fast_ms);
  CHECK(p::Repeater::IntervalAt(t.ramp_ms * 10, t) == t.fast_ms);
  CHECK(p::Repeater::IntervalAt(-5, t) == t.start_ms);
  int prev = 1 << 30;
  for (int64_t x = 0; x <= t.ramp_ms; x += 100) {
    const int now = p::Repeater::IntervalAt(x, t);
    CHECK(now <= prev);
    prev = now;
  }

  // Held for 4 s, looked at every 8 ms: the steps per second go up with the hold.
  p::Repeater r;
  int steps_1s = 0, steps_2_to_3s = 0, steps_3_to_4s = 0, total = 0;
  for (int64_t now = 0; now <= 4000; now += 8) {
    const int n = r.Update(true, now, t);
    total += n;
    if (now < 1000) steps_1s += n;
    else if (now >= 2000 && now < 3000) steps_2_to_3s += n;
    else if (now >= 3000) steps_3_to_4s += n;
  }
  CHECK(steps_1s >= 5 && steps_1s <= 8);        // press, then 380 ms later every ~140 ms
  CHECK(steps_2_to_3s > steps_1s);              // faster
  CHECK(steps_3_to_4s >= steps_2_to_3s);        // and then at the top speed
  CHECK(steps_3_to_4s <= 1000 / t.fast_ms + 2);  // never faster than fast_ms
  CHECK(total > 25);
  return 0;
}

constexpr int TestRepeaterStall() {
  // The UI thread stalls for 2 s: the backlog is not played back as 20 steps.
  p::Repeater r;
  const p::RepeatTiming t;
  CHECK(r.Update(true, 0, t) == 1);
  const int n = r.Update(true, 2000, t);
  CHECK(n >= 1 && n <= p::Repeater::kMaxStepsPerCall);
  // And the schedule goes on from now, not from the past.
  CHECK(r.Update(true, 2001, t) == 0);
  CHECK(r.Update(true, 2001 + t.fast_ms + t.start_ms, t) >= 1);
  return 0;
}

constexpr int TestHoldMultiplier() {
  CHECK(p::HoldMultiplier(0) == 1);
  CHECK(p::HoldMultiplier(999) == 1);
  CHECK(p::HoldMultiplier(1000) == 2);
  CHECK(p::HoldMultiplier(1999) == 2);
  CHECK(p::HoldMultiplier(2000) == 3);
  CHECK(p::HoldMultiplier(3499) == 3);
  CHECK(p::HoldMultiplier(3500) == 5);
  CHECK(p::HoldMultiplier(60000) == 5);
  return 0;
}

constexpr int TestStepValue() {
  CHECK_NEAR(p::StepValue(0.5, 1, 1, 0.1, 0.0, 1.0), 0.6, 1e-12);
  CHECK_NEAR(p::StepValue(0.5, -2, 1, 0.1, 0.0, 1.0), 0.3, 1e-12);
  CHECK_NEAR(p::StepValue(0.5, 1, 3, 0.1, 0.0, 1.0), 0.8, 1e-12);
  CHECK_NEAR(p::StepValue(0.95, 1, 5, 0.1, 0.0, 1.0), 1.0, 1e-12);  // clamped
  CHECK_NEAR(p::StepValue(0.05, -1, 1, 0.1, 0.0, 1.0), 0.0, 1e-12);
  CHECK_NEAR(p::StepValue(7.0, 1, 0, 1.0, 0.0, 15.0), 8.0, 1e-12);  // a zero multiplier counts as one

  CHECK(p::StepChoice(2, 1, 5) == 3);
  CHECK(p::StepChoice(2, -1, 5) == 1);
  CHECK(p::StepChoice(4, 1, 5) == 4);  // clamped at the end, no wrap
  CHECK(p::StepChoice(0, -1, 5) == 0);
  CHECK(p::StepChoice(-1, 1, 5) == 0);  // a custom value steps to the first choice
  CHECK(p::StepChoice(-1, -1, 5) == 0);
  CHECK(p::StepChoice(3, 1, 0) == 0);
  CHECK(p::StepChoice(1, 9, 4) == 3);
  return 0;
}

// ---- the menu's mapping ---------------------------------------------------------------------------------------------------------

constexpr int TestMenuPadDirections() {
  p::MenuPad m;
  // On an item ImGui moves around (a button), all four directions go to ImGui.
  p::MenuOut o = m.Update(p::kDpadUp, 0, p::Focus::kOther);
  CHECK(o.dirs == p::kNavUp && o.adjust == 0);
  o = m.Update(p::kLsDown | p::kLsLeft, 10, p::Focus::kOther);
  CHECK(o.dirs == (p::kNavDown | p::kNavLeft));
  o = m.Update(p::kDpadRight, 20, p::Focus::kOther);
  CHECK(o.dirs == p::kNavRight);
  // Opposites cancel; the stick and the d-pad are one direction.
  o = m.Update(p::kDpadUp | p::kLsDown, 30, p::Focus::kOther);
  CHECK(o.dirs == 0);
  o = m.Update(p::kDpadUp | p::kLsUp, 40, p::Focus::kOther);
  CHECK(o.dirs == p::kNavUp);
  // Nothing held, nothing sent.
  o = m.Update(0, 50, p::Focus::kOther);
  CHECK(o.dirs == 0 && !o.activate && !o.cancel && o.tab_step == 0 && o.adjust == 0 && o.pressed == 0);
  return 0;
}

constexpr int TestMenuPadAdjust() {
  p::MenuPad m;
  // On a slider left / right do not go to ImGui: they are steps. One press = one step, none until the delay, then repeats.
  p::MenuOut o = m.Update(p::kDpadRight, 0, p::Focus::kSlider);
  CHECK(o.dirs == 0 && o.adjust == 1 && o.adjust_press && o.adjust_mult == 1);
  o = m.Update(p::kDpadRight, 100, p::Focus::kSlider);
  CHECK(o.adjust == 0 && !o.adjust_press);
  o = m.Update(p::kDpadRight, 400, p::Focus::kSlider);
  CHECK(o.adjust == 1 && !o.adjust_press);
  o = m.Update(0, 450, p::Focus::kSlider);
  CHECK(o.adjust == 0);
  o = m.Update(p::kLsLeft, 500, p::Focus::kSlider);  // the stick does the same as the d-pad
  CHECK(o.adjust == -1 && o.adjust_press);
  // Both at once: nothing.
  o = m.Update(p::kDpadLeft | p::kDpadRight, 520, p::Focus::kSlider);
  CHECK(o.adjust == 0);

  // The step grows with the hold (the multiplier is the one HoldMultiplier gives for the hold so far).
  p::MenuPad h;
  int64_t t = 0;
  int last_mult = 0;
  for (; t <= 4000; t += 8) {
    const p::MenuOut x = h.Update(p::kDpadRight, t, p::Focus::kSlider);
    CHECK(x.adjust_mult == p::HoldMultiplier(t));
    last_mult = x.adjust_mult;
  }
  CHECK(last_mult == 5);

  // The same on a combo and a tick box; A is the combo's / box's own (ImGui's), and not the slider's.
  p::MenuPad c;
  o = c.Update(p::kDpadLeft | p::kA, 0, p::Focus::kChoice);
  CHECK(o.adjust == -1 && o.activate);
  p::MenuPad tg;
  o = tg.Update(p::kDpadRight | p::kA, 0, p::Focus::kToggle);
  CHECK(o.adjust == 1 && o.activate);
  p::MenuPad sl;
  o = sl.Update(p::kA, 0, p::Focus::kSlider);
  CHECK(!o.activate);
  o = sl.Update(p::kA, 10, p::Focus::kOther);  // a button: A presses it (A was already down: the level is what ImGui needs)
  CHECK(o.activate);

  // Changing from an adjustable item to a button while left is held: the held direction now navigates, and the repeat starts clean.
  p::MenuPad sw;
  o = sw.Update(p::kDpadLeft, 0, p::Focus::kSlider);
  CHECK(o.adjust == -1);
  o = sw.Update(p::kDpadLeft, 8, p::Focus::kOther);
  CHECK(o.dirs == p::kNavLeft && o.adjust == 0);
  o = sw.Update(p::kDpadLeft, 16, p::Focus::kSlider);
  CHECK(o.adjust == -1 && o.adjust_press);
  return 0;
}

constexpr int TestMenuPadActions() {
  p::MenuPad m;
  // LB / RB: one tab per press, none while held.
  p::MenuOut o = m.Update(p::kRB, 0, p::Focus::kOther);
  CHECK(o.tab_step == 1);
  o = m.Update(p::kRB, 100, p::Focus::kOther);
  CHECK(o.tab_step == 0);
  o = m.Update(0, 200, p::Focus::kOther);
  o = m.Update(p::kLB, 300, p::Focus::kOther);
  CHECK(o.tab_step == -1);
  o = m.Update(p::kLB | p::kRB, 310, p::Focus::kOther);  // RB joins: only the new press counts
  CHECK(o.tab_step == 1);
  o = m.Update(0, 400, p::Focus::kOther);

  // X, Y and B are presses; B is also ImGui's cancel while it is down.
  o = m.Update(p::kX, 500, p::Focus::kOther);
  CHECK(o.reset && !o.save && !o.back);
  o = m.Update(p::kX, 510, p::Focus::kOther);
  CHECK(!o.reset);
  o = m.Update(p::kY, 520, p::Focus::kOther);
  CHECK(o.save && !o.reset);
  o = m.Update(0, 530, p::Focus::kOther);
  o = m.Update(p::kB, 540, p::Focus::kOther);
  CHECK(o.back && o.cancel && !o.activate);
  o = m.Update(p::kB, 550, p::Focus::kOther);
  CHECK(!o.back && o.cancel);
  o = m.Update(p::kA, 560, p::Focus::kOther);
  CHECK(o.activate && !o.cancel);

  // Start, Back, the sticks' clicks and the triggers are not the menu's.
  o = m.Update(0, 600, p::Focus::kOther);
  o = m.Update(p::kStart | p::kBack | p::kL3 | p::kR3 | p::kLT | p::kRT, 610, p::Focus::kOther);
  CHECK(o.dirs == 0 && !o.activate && !o.cancel && o.tab_step == 0 && !o.reset && !o.save && !o.back && o.adjust == 0);
  CHECK(o.pressed == (p::kStart | p::kBack | p::kL3 | p::kR3 | p::kLT | p::kRT));

  // Reset forgets what was down: a button held across a Reset is a press again.
  m.Reset();
  o = m.Update(p::kX, 700, p::Focus::kOther);
  CHECK(o.reset);
  return 0;
}

// What was held when the menu opened is not a press: the open's hold-off, then the mapping.
constexpr int TestMenuOpenHoldOff() {
  p::HoldOffModel gate;
  p::MenuPad m;
  // The chord L3 + R3 and the right bumper are down at the open.
  gate.Begin(p::kL3 | p::kR3 | p::kRB, 0);
  auto look = [&](uint32_t down, int64_t now) {
    gate.Update(down, now);
    return m.Update(down & ~gate.Mask(), now, p::Focus::kOther);
  };
  p::MenuOut o = look(p::kL3 | p::kR3 | p::kRB, 8);
  CHECK(o.tab_step == 0);  // the held RB did not switch the tab
  o = look(p::kRB, 16);    // the chord is let go
  CHECK(o.tab_step == 0);
  o = look(0, 24);         // RB let go
  o = look(p::kRB, 32);    // pressed again: a press
  CHECK(o.tab_step == 1);
  return 0;
}

// ---- the last input device ---------------------------------------------------------------------------------------------------------

constexpr int TestDeviceTracker() {
  p::DeviceTracker d;
  CHECK(d.last() == p::Device::kKeyboardMouse);

  // A pad input that goes down takes over; one that stays down does not keep taking over.
  d.NotePad(p::kA, 0);
  CHECK(d.last() == p::Device::kPad);
  d.NoteKey();
  CHECK(d.last() == p::Device::kKeyboardMouse);
  d.NotePad(p::kA, p::kA);  // A is still held: nothing new
  CHECK(d.last() == p::Device::kKeyboardMouse);
  d.NotePad(0, p::kA);  // a release is not a use
  CHECK(d.last() == p::Device::kKeyboardMouse);
  d.NotePad(p::kA | p::kLsUp, p::kA);  // the stick crossed its threshold: new
  CHECK(d.last() == p::Device::kPad);

  // The mouse: the first position only anchors; a bump of the desk (< 8 px) is not use; a real movement is.
  p::DeviceTracker m;
  m.NotePad(p::kB, 0);
  CHECK(m.last() == p::Device::kPad);
  m.NoteMouseMove(500.0f, 400.0f);
  CHECK(m.last() == p::Device::kPad);
  m.NoteMouseMove(503.0f, 402.0f);
  m.NoteMouseMove(501.0f, 399.0f);
  CHECK(m.last() == p::Device::kPad);
  m.NoteMouseMove(512.0f, 400.0f);  // 12 px from the anchor
  CHECK(m.last() == p::Device::kKeyboardMouse);
  // Back to the pad, then jitter around the new anchor does nothing; moving away does.
  m.NotePad(p::kDpadDown, 0);
  CHECK(m.last() == p::Device::kPad);
  m.NoteMouseMove(514.0f, 401.0f);
  CHECK(m.last() == p::Device::kPad);
  m.NoteMouseMove(512.0f, 420.0f);
  CHECK(m.last() == p::Device::kKeyboardMouse);

  // A click or a key is always use; Force sets it (the menu opened by the chord).
  p::DeviceTracker f;
  f.Force(p::Device::kPad);
  CHECK(f.last() == p::Device::kPad);
  f.NoteKey();
  CHECK(f.last() == p::Device::kKeyboardMouse);
  return 0;
}

// The compiler runs the tests.
static_assert(TestDigitize() == 0, "Digitize");
static_assert(TestStickValue() == 0, "StickValue");
static_assert(TestChordNames() == 0, "chord names");
static_assert(TestChordHold() == 0, "chord hold time");
static_assert(TestChordBroken() == 0, "chord broken");
static_assert(TestChordThreeButtons() == 0, "three-button chord");
static_assert(TestHoldOff() == 0, "hold-off");
static_assert(TestHoldOffTimeout() == 0, "hold-off timeout");
static_assert(TestCloseWithHeldButton() == 0, "close with a held button");
static_assert(TestRepeater() == 0, "repeater");
static_assert(TestRepeaterAccelerates() == 0, "repeater acceleration");
static_assert(TestRepeaterStall() == 0, "repeater stall");
static_assert(TestHoldMultiplier() == 0, "hold multiplier");
static_assert(TestStepValue() == 0, "step value / choice");
static_assert(TestMenuPadDirections() == 0, "menu pad directions");
static_assert(TestMenuPadAdjust() == 0, "menu pad adjust");
static_assert(TestMenuPadActions() == 0, "menu pad actions");
static_assert(TestMenuOpenHoldOff() == 0, "menu open hold-off");
static_assert(TestDeviceTracker() == 0, "device tracker");

// The atomic HoldOff (the one the overlay uses) against the same story as TestCloseWithHeldButton.
int TestAtomicHoldOff() {
  p::HoldOff h;
  CHECK(!h.Active());
  h.Begin(p::kB | p::kLsUp, 100);
  CHECK(h.Active() && h.Mask() == (p::kB | p::kLsUp));
  CHECK(h.Update(p::kB | p::kLsUp, 110));
  CHECK(h.Update(p::kB, 120) && h.Mask() == p::kB);
  CHECK(!h.Update(0, 130) && !h.Active());
  CHECK(!h.Update(p::kB, 140));
  h.Begin(p::kA, 1000);
  CHECK(h.Update(p::kA, 1000 + p::HoldOff::kTimeoutMs));
  CHECK(!h.Update(p::kA, 1001 + p::HoldOff::kTimeoutMs) && h.TakeExpired() && !h.TakeExpired());
  h.Begin(p::kX, 5000);
  h.Clear();
  CHECK(!h.Active());
  return 0;
}

}  // namespace

int main() {
  struct Run {
    const char* name;
    int (*fn)();
  };
  const Run runs[] = {{"Digitize", TestDigitize},
                      {"StickValue", TestStickValue},
                      {"chord names", TestChordNames},
                      {"chord hold time", TestChordHold},
                      {"chord broken", TestChordBroken},
                      {"three-button chord", TestChordThreeButtons},
                      {"hold-off", TestHoldOff},
                      {"hold-off timeout", TestHoldOffTimeout},
                      {"close with a held button", TestCloseWithHeldButton},
                      {"repeater", TestRepeater},
                      {"repeater acceleration", TestRepeaterAccelerates},
                      {"repeater stall", TestRepeaterStall},
                      {"hold multiplier", TestHoldMultiplier},
                      {"step value / choice", TestStepValue},
                      {"menu pad directions", TestMenuPadDirections},
                      {"menu pad adjust", TestMenuPadAdjust},
                      {"menu pad actions", TestMenuPadActions},
                      {"menu open hold-off", TestMenuOpenHoldOff},
                      {"device tracker", TestDeviceTracker},
                      {"atomic hold-off", TestAtomicHoldOff}};
  int failures = 0;
  for (const Run& r : runs) {
    const int line = r.fn();
    if (line != 0) {
      ++failures;
      std::printf("FAIL %s: check at line %d\n", r.name, line);
    }
  }
  std::printf("overlay pad: %zu tests, %d failure(s)\n", sizeof(runs) / sizeof(runs[0]), failures);
  return failures == 0 ? 0 : 1;
}
