// MCLA 60 fps timing: the pure arithmetic of the sim60 hooks (docs/mcla/TIMING.md). No SDK, no guest
// memory, no cvars: everything here is a function of numbers, so games/mcla/tests/test_sim_math.cpp
// compiles it standalone and checks it (the hooks only fetch the numbers and store the answers).
//
// Units: the quantiser works in milliseconds (frame_clock_policy.h's AdvanceWorldClock does), the
// smoothing and UI functions in seconds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "frame_clock_policy.h"

namespace mcla::sim::math {

// ---- the quantised world step ------------------------------------------------------------------

constexpr double kQuantumS = 1.0 / 60.0;     // the game's physics step; the unit of the world step
constexpr double kQuantumMs = 1000.0 / 60.0;
constexpr int kMaxN = 4;                     // at most 4/60 s a frame (= AdvanceWorldClock's 1/15 s)

// How many quanta one frame lasts at a frame cap: 2 at a 30 cap (the shipped 1/30 s step), else 1.
// Caps above 60 (the host's 120, 40) are paced at 60: the world has no finer step than 1/60.
inline int PeriodQuanta(int cap) { return cap > 0 && cap <= 30 ? 2 : 1; }

struct Step {
  int n;         // the frame's step is n / 60 s, n = 1..4
  bool resync;   // the debt model gave up and started over (a hitch or a clock jump)
};

// One frame. `debt_ms` carries what the world is owed (+) or has been given ahead (-) between
// frames; `elapsed_ms` is the real time since the previous frame; `period_quanta` from PeriodQuanta().
// Reuses the MW/Carbon world clock: whole periods, debt carried, at most 4 quanta, resync on a gap.
inline Step QuantiseStep(double& debt_ms, double elapsed_ms, int period_quanta) {
  const double period = kQuantumMs * period_quanta;
  const nfsmw::WorldClockStep s = nfsmw::AdvanceWorldClock(debt_ms, elapsed_ms, period);
  const int n = int(std::llround(s.given / kQuantumMs));
  return {std::clamp(n, 1, kMaxN), s.resync};
}

// The step as the single-precision float the timer's path 2 publishes (n = 2 is the shipped 1/30).
inline double StepSeconds(int n) { return double(float(n / 60.0)); }

// ---- P1 begin: 120 fps (docs/mcla/TIMING.md section 15) ----------------------------------------
// A cap above 60 (the launcher's 120 on a ProMotion screen) runs the world on a FINE grid: the quantum is
// 1/120 s and n = 1..8 (at most 8/120 = 1/15 s, the same ceiling as 4/60). At caps of 60 and below nothing
// changes (the coarse 1/60 grid above, bit for bit). Physics substeps on the fine grid: ceil(n/2), so a
// substep is never longer than the shipped 1/60 s (n = 1: one substep of 1/120 s, the step MCLA-4iOS
// runs at 60 with 2 substeps; n = 2: one of 1/60 s = the shipped one; never 0 = no wheels).
constexpr int kFineHz = 120;
constexpr double kFineQuantumMs = 1000.0 / kFineHz;
constexpr int kFineMaxN = 8;
inline bool FineGrid(int cap) { return cap > 60; }
inline int QuantumHz(int cap) { return FineGrid(cap) ? kFineHz : 60; }
inline double QuantumMs(int cap) { return FineGrid(cap) ? kFineQuantumMs : kQuantumMs; }
inline int MaxN(int cap) { return FineGrid(cap) ? kFineMaxN : kMaxN; }

// One frame on the cap's grid (fine above 60, else exactly QuantiseStep). n counts the cap's quanta.
inline Step QuantiseStepFor(double& debt_ms, double elapsed_ms, int cap) {
  if (!FineGrid(cap)) return QuantiseStep(debt_ms, elapsed_ms, PeriodQuanta(cap));
  const nfsmw::WorldClockStep s = nfsmw::AdvanceWorldClock(debt_ms, elapsed_ms, kFineQuantumMs);
  const int n = int(std::llround(s.given / kFineQuantumMs));
  return {std::clamp(n, 1, kFineMaxN), s.resync};
}
// The step in seconds as the timer's float, for n quanta of the cap's grid.
inline double StepSecondsFor(int n, int cap) {
  return FineGrid(cap) ? double(float(n / double(kFineHz))) : StepSeconds(n);
}
// Physics substeps for n quanta of the cap's grid (the coarse grid: n, as before).
inline int SubstepsFor(int n, int cap) { return FineGrid(cap) ? std::max(1, (n + 1) / 2) : n; }
// The game-thread pacer's period (ms) for a cap: 1/120, 1/60 or 1/30 s.
inline double PacePeriodMs(int cap) { return QuantumMs(cap) * (FineGrid(cap) ? 1 : PeriodQuanta(cap)); }
// The frame rate the guest's presentation interval aims at: 120, 60 or 30.
inline double WantedFps(int cap) { return FineGrid(cap) ? 120.0 : PeriodQuanta(cap) == 1 ? 60.0 : 30.0; }
// The guest's presentation interval (vblanks a swap) for a vblank rate and a cap, from the shipped 2. On a
// 60 Hz guest vblank the hook only ever LOWERS it (60 cap -> 1, as before); on a 120 Hz one it is exact
// (120 -> 1, 60 -> 2, 30 -> 4: a 30 hold under a 120 Hz grid would otherwise run at 60).
inline int PresentInterval(double vblank_hz, int cap, int shipped) {
  const int interval = std::clamp<int>(int(std::llround(vblank_hz / WantedFps(cap))), 1, 4);
  return vblank_hz > 90.0 ? interval : std::min(shipped, interval);
}

// Frame-time stutter counters (the [mcla-sim] check line, bench.py, read_device_log.py): the real time
// between engine frames in 1 ms buckets up to 250 ms.
struct FrameTimes {
  static constexpr int kBuckets = 251;
  uint32_t hist[kBuckets] = {};
  uint32_t count = 0;
  double worst_ms = 0.0;
  void Add(double ms) {
    if (!(ms >= 0.0)) return;
    ++hist[std::min(kBuckets - 1, int(ms))];
    ++count;
    worst_ms = std::max(worst_ms, ms);
  }
  // Frames of `ms` or longer (1 ms buckets: 20 / 25 / 34 never cut through a 16.7 or 33.3 ms frame).
  uint32_t Over(int ms) const {
    uint32_t n = 0;
    for (int i = std::max(0, ms); i < kBuckets; ++i) n += hist[i];
    return n;
  }
  // The p-th percentile (0..1) in ms (upper edge of its bucket).
  double Percentile(double p) const {
    if (!count) return 0.0;
    const uint32_t want = uint32_t(std::ceil(p * count));
    uint32_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += hist[i];
      if (seen >= want) return double(i + 1);
    }
    return worst_ms;
  }
};
// ---- P1 end ---------------------------------------------------------------------------------------

// ---- camera / chassis smoothing ----------------------------------------------------------------
//
// The game computes round(1/dt) and picks a factor: below 60 the 30 fps one, from 60 up another. The
// hooks run after that choice. k30 is what the game uses at 1/30 s (the tuning value halved in its
// own 60+ branch, untouched in the below-60 one); a per-step filter that behaves the same at any dt
// is k(dt) = 1 - (1 - k30)^(30 dt) (MCLA-4iOS's formula).

// A step below 1/240 s is a (nearly) frozen world (pause: the time scale is 0 and the timer's floor is
// 0.0001 s): the game keeps easing there with its 60+ constant and a camera that is still settling must
// finish, so the filters are left as the game computed them, as shipped.
constexpr double kSmoothMinDt = 1.0 / 240.0;

// `k_game` = the factor the game picked, `ge60` = its branch (round(1/dt) >= 60), `dt` = the timer's
// step (seconds). Factors outside (0, 1) and dt <= 0 are left alone. At dt = 1/30 in the below-60
// branch the game's own value is already exact: returned unchanged (bit for bit the shipped run).
inline double SmoothK(double k_game, bool ge60, double dt) {
  const double k30 = ge60 ? k_game * 0.5 : k_game;
  if (!(k30 > 0.0 && k30 < 1.0) || !(dt >= kSmoothMinDt)) {
    return k_game;
  }
  const double r = 30.0 * dt;
  if (!ge60 && std::fabs(r - 1.0) < 1e-3) {
    return k_game;
  }
  return 1.0 - std::pow(1.0 - k30, r);
}

// Chassis ground depth: alpha = 1 - 0.9^(30 dt) (0.10 at 1/30, 0.0513 at 1/60). The game's two values
// (0.10 below 60, 0.05 from 60) are within 3 % of that at their own step, so they are kept there:
// only a step that is neither 1/30 nor 1/60 (a slow frame, slow motion) gets the formula.
inline double ChassisAlpha(double alpha_game, bool ge60, double dt) {
  if (!(dt >= kSmoothMinDt)) {
    return alpha_game;
  }
  const double r = 30.0 * dt;
  if (!ge60 && std::fabs(r - 1.0) < 1e-3) {
    return alpha_game;
  }
  if (ge60 && std::fabs(2.0 * r - 1.0) < 1e-3) {
    return alpha_game;
  }
  return 1.0 - std::pow(0.9, r);
}

// ---- the race clock ----------------------------------------------------------------------------

enum class RaceMode { kOff, kRetail, kExact };

// The console's race clock: int(dt x 1000) ms a step is 33 per 1/30 s = 0.99 of the real time.
constexpr double kRetailFactor = 0.99;

struct RaceCarry {
  double frac = 0.0;  // milliseconds not handed out yet
};

// Whole milliseconds to add for a step of `dt_ms`, keeping the fraction for the next step. The 1e-3
// guards float noise (33.333332 x 0.99 is 32.999998, not 33).
inline int RaceMs(RaceCarry& carry, double dt_ms, RaceMode mode) {
  const double value = dt_ms * (mode == RaceMode::kRetail ? kRetailFactor : 1.0);
  const double total = carry.frac + value;
  const double whole = std::floor(total + 1e-3);
  carry.frac = total - whole;
  return int(whole);
}

// ---- Flash movies held at 30 steps a second ----------------------------------------------------

constexpr double kUiPeriodS = 1.0 / 30.0;
constexpr double kUiToleranceS = 0.001;

struct UiRecord {
  double since = kUiPeriodS;          // world time since the last step (a new movie steps at once)
  uint64_t frame_serial = ~uint64_t(0);
  uint32_t step_seq = 0;              // steps let through so far
  double countdown_acc = 0.0;         // real time since the last step, for the countdown variable
  uint32_t countdown_seen = 0;        // step_seq the accumulator was last cleared at
};

// A movie update call. `frame_serial` = the timer's frame counter, `step_s` = this frame's unscaled
// step. True = hold (skip this update), false = let the game step one movie frame. Time is added
// once per frame, a step takes 1/30 s of it, and what is left is capped at half a period so a hitch
// does not become a burst of catch-up steps (the shipped game steps once a frame at any rate).
inline bool UiHold(UiRecord& r, uint64_t frame_serial, double step_s) {
  if (r.frame_serial != frame_serial) {
    r.frame_serial = frame_serial;
    r.since += step_s;
  }
  if (r.since < kUiPeriodS - kUiToleranceS) {
    return true;
  }
  r.since = std::clamp(r.since - kUiPeriodS, -kUiToleranceS, 0.5 * kUiPeriodS);
  ++r.step_seq;
  return false;
}

// The script variable `countdownUpdate` (the script adds it once per movie step): the game passes
// the real frame time every frame; with held steps the script must get the time since its LAST step.
// Order-agnostic: the accumulator restarts when a step happened since the last call. A 0 stays 0.
inline double UiCountdown(UiRecord& r, double game_dt) {
  if (!(game_dt > 0.0)) {
    return game_dt;
  }
  if (r.countdown_seen != r.step_seq) {
    r.countdown_acc = 0.0;
    r.countdown_seen = r.step_seq;
  }
  r.countdown_acc += game_dt;
  return r.countdown_acc;
}

// ---- the 60 -> 30 hold (docs/mcla/TIMING.md section 13) ----------------------------------------

// What the renderer's dynamic-resolution governor tells the pacer (mcla_pacer_renderer_room): can it still make
// the frames cheaper? 0 = unknown (no governor, or no report for a while), 1 = room (the scale is above its floor),
// 2 = floor (nothing left to give: a frame that still misses can only be fixed by dropping to 30).
enum Room { kRoomUnknown = 0, kRoomLeft = 1, kRoomNone = 2 };

// One decision window at a cap of 60: `missed` of `frames` took two or more quanta. `high_s` = how long the
// windows before this one were high in a row. True = drop to 30. A window of less than 30 frames decides nothing
// (a load, a menu); while the governor has room the pacer lets it work for room_wait_s first.
inline bool HoldDrop(uint64_t missed, uint64_t frames, int miss_pct, int room, double high_s, double room_wait_s) {
  if (frames < 30 || missed * 100 <= frames * uint64_t(miss_pct < 0 ? 0 : miss_pct)) return false;
  if (room == kRoomLeft) return high_s >= room_wait_s;
  return true;
}

// How long this hold at 30 lasts: min_s for the first drop (or after calm_s seconds at 60 since the last hold),
// double the previous hold when the misses came back soon after it ended, never more than max_s.
inline int HoldSeconds(int prev_hold_s, double since_end_s, int min_s, int max_s, int calm_s) {
  if (max_s < 1) return 0;
  min_s = std::clamp(min_s, 1, max_s);
  if (prev_hold_s <= 0 || since_end_s >= double(calm_s)) return min_s;
  return std::min(max_s, std::max(min_s, prev_hold_s * 2));
}

// ---- the governed hold (mcla_sim_pacer 2, LOCK60 A3) -------------------------------------------------
//
// Why. Every hold of the 4 bad 1080p bench runs began within ~1 s of `free-roam -> pause` (the pause menu is the heaviest
// screen of the game for the GPU: 7 tiles at 4x MSAA, it misses ~100 % of its frames) or inside `loading`, and the 12 s
// re-hold followed because the menu itself kept missing. A hold is for a drive the GPU cannot sustain at 60, so this
// judge looks at driving only and wants the overload to last:
//   * a menu (pause, loading, front end: ProfileMode() == 1) is never judged and resets the window; -1 (unknown) counts
//     as driving, as before the screen was known;
//   * grace_s seconds after a hold ends, and after a menu closes, nothing is judged (GPU clocks and queues settle);
//   * a hold needs `windows` high windows IN A ROW (a window that is not high resets the count);
//   * and only when the governor has nothing left to shed (`allowed`); otherwise the verdict is `deferred`;
//   * a high window that a SHADER-COMPILE BURST overlapped (`compile` of Tick, mcla::gov::CompileBurstActive) is not judged: a stretch
//     of run-time pipeline creations stalls the frames for the compiler's sake, and no hold helps that. It neither advances nor
//     resets the high-window count (it is as if the window had not happened), and it is counted as `not judged: compile`. Bounded, so
//     a real overload still holds: within one run of consecutive high windows at most rule.compile_windows are excused (the third
//     high window of the default two is judged); a window that is not high, a menu, a grace and a hold start the allowance over.
// Pure: time comes in as a number, so the tests drive it with a made-up clock. The caller owns the cvars (HoldRule),
// the hold in force and its length memory (prev_hold_s / since_end_s), the log lines and the rate limit of `deferred`.

struct HoldRule {
  int window_s = 2;   // mcla_sim_miss_window_s: a decision window
  int miss_pct = 10;  // mcla_sim_miss_pct: a window is high above this share of missed frames
  int windows = 2;    // mcla_sim_hold_windows: consecutive high windows before a hold
  int grace_s = 4;    // mcla_sim_hold_grace_s: nothing is judged this long after a hold or a menu
  int min_s = 6;      // mcla_sim_hold30_min_s: the first hold
  int max_s = 30;     // mcla_sim_hold30_s: the longest hold; 0 = never hold
  int calm_s = 30;    // mcla_sim_calm_s: seconds after a hold before the next one starts short again
  int compile_windows = 2;  // mcla_compile_excuse_s / window_s (4.0 s / 2 s): high windows of a run a compile burst may excuse; 0 = none
};

struct HoldVerdict {
  bool window_ended = false;  // a decision window was judged by this call
  bool high = false;          // ... and it was high
  bool start_hold = false;    // start a hold of hold_s seconds now
  int hold_s = 0;
  bool deferred = false;      // a hold is due but the governor still has room: do not start
  bool excused = false;       // a high window not judged: a compile burst overlapped it (high is still true, streak unchanged)
  int excused_in_run = 0;     // high windows of the current run excused so far, this one included
  int streak = 0;             // consecutive judged high windows including this one
  uint64_t frames = 0, missed = 0;  // the judged window
  double window_s = 0.0;
};

struct HoldJudge {
  // The decision window: the frames of the 60 step noted since window_start_ns.
  bool started = false;
  int64_t window_start_ns = 0;
  uint64_t frames = 0, missed = 0;
  int streak = 0;
  int64_t grace_until_ns = 0;
  bool was_menu = false;
  bool window_compile = false;  // a compile burst was active during the window in progress
  int excused_in_run = 0;       // high windows of the current run of them excused for a compile burst
  // Engine frames not judged (one count a Tick; compile: the frames of the excused windows), for the 5 s log line (TakeSkipped).
  uint64_t skipped_menu = 0, skipped_grace = 0, skipped_hold = 0, skipped_compile = 0;

  // One frame of the 60 step (the world step quantiser ran): `missed_step` = n >= 2 and not a resync.
  void NoteStep(bool missed_step) {
    ++frames;
    if (missed_step) ++missed;
  }
  // Start a new window now (the counts of the old one are dropped).
  void Restart(int64_t now_ns) {
    started = true;
    window_start_ns = now_ns;
    frames = missed = 0;
    window_compile = false;
  }
  // The hold in force ran out: grace_s seconds of nothing, then a fresh window.
  void EndHold(int64_t now_ns, int grace_s) {
    grace_until_ns = std::max(grace_until_ns, now_ns + int64_t(std::max(0, grace_s)) * 1'000'000'000);
    streak = 0;
    excused_in_run = 0;
    Restart(now_ns);
  }
  // The skipped-frame counts since the last call (and clears them).
  void TakeSkipped(uint64_t* menu, uint64_t* grace, uint64_t* hold, uint64_t* compile) {
    *menu = skipped_menu;
    *grace = skipped_grace;
    *hold = skipped_hold;
    *compile = skipped_compile;
    skipped_menu = skipped_grace = skipped_hold = skipped_compile = 0;
  }
  void TakeSkipped(uint64_t* menu, uint64_t* grace, uint64_t* hold) {
    uint64_t compile = 0;
    TakeSkipped(menu, grace, hold, &compile);
  }

  // Once per engine frame, before that frame's step is noted. `menu` = ProfileMode() == 1; `hold_active` = a hold (30 or
  // the 120 -> 60 tier) is in force; `allowed` = mcla::gov::HoldAllowed(); `prev_hold_s` / `since_end_s` = the length of
  // the previous hold and the seconds since it ended (1e9 = never), for HoldSeconds; `compile` = a shader-compile burst is active now
  // (mcla::gov::CompileBurstActive; false = no signal).
  HoldVerdict Tick(int64_t now_ns, bool menu, bool hold_active, bool allowed, int prev_hold_s, double since_end_s,
                   const HoldRule& rule, bool compile = false) {
    HoldVerdict v;
    if (hold_active) {  // it runs out normally; its frames are not judged
      ++skipped_hold;
      streak = 0;
      excused_in_run = 0;
      was_menu = menu;
      Restart(now_ns);
      return v;
    }
    if (menu) {  // a menu: never judged, and the window starts over when it closes
      ++skipped_menu;
      streak = 0;
      excused_in_run = 0;
      was_menu = true;
      Restart(now_ns);
      return v;
    }
    if (was_menu) {  // a menu just closed
      was_menu = false;
      grace_until_ns = std::max(grace_until_ns, now_ns + int64_t(std::max(0, rule.grace_s)) * 1'000'000'000);
    }
    if (now_ns < grace_until_ns) {
      ++skipped_grace;
      streak = 0;
      excused_in_run = 0;
      Restart(now_ns);
      return v;
    }
    if (!started) {
      Restart(now_ns);
      return v;
    }
    if (compile) window_compile = true;
    const int64_t window_ns = int64_t(std::max(1, rule.window_s)) * 1'000'000'000;
    if (now_ns - window_start_ns < window_ns) return v;
    v.window_ended = true;
    v.frames = frames;
    v.missed = missed;
    v.window_s = double(now_ns - window_start_ns) / 1e9;
    const bool compile_seen = window_compile;
    Restart(now_ns);
    // Fewer than 30 frames of the 60 step in a window decide nothing (and break a run of high windows).
    v.high = v.frames >= 30 && v.missed * 100 > v.frames * uint64_t(std::max(0, rule.miss_pct));
    if (!v.high) {
      streak = 0;
      excused_in_run = 0;  // the run of high windows is over: the allowance starts again
      return v;
    }
    // A compile burst overlapped it: not judged, within the allowance of the run (the streak stays as it was).
    if (compile_seen && excused_in_run < std::max(0, rule.compile_windows)) {
      ++excused_in_run;
      skipped_compile += v.frames;
      v.excused = true;
      v.excused_in_run = excused_in_run;
      v.streak = streak;
      return v;
    }
    v.excused_in_run = excused_in_run;
    const int need = std::max(1, rule.windows);
    streak = std::min(streak + 1, need);
    v.streak = streak;
    if (streak < need || rule.max_s <= 0) return v;
    if (!allowed) {
      v.deferred = true;  // the streak stays: the first high window after the governor runs out of levels holds
      return v;
    }
    v.hold_s = HoldSeconds(prev_hold_s, since_end_s, rule.min_s, rule.max_s, rule.calm_s);
    v.start_hold = v.hold_s > 0;
    if (v.start_hold) streak = excused_in_run = 0;
    return v;
  }
};

}  // namespace mcla::sim::math
