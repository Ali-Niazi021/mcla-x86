// Unit test of the 60 fps timing arithmetic (mcla/hooks/mcla_t_sim_math.h): the n/60 quantiser
// on top of mcla/hooks/frame_clock_policy.h, camera/chassis smoothing, the race clock carry, the Flash
// movie 30 Hz stepper and the countdown accumulator. Standalone: no SDK, no game.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_sim_math_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks \
//       mcla/tests/test_sim_math.cpp -o <scratch>/test_sim_math && <scratch>/test_sim_math
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mcla_t_sim_math.h"

namespace m = mcla::sim::math;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failed;                                                     \
      std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);     \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

// Deterministic jitter in [-1, 1].
struct Rng {
  uint32_t s = 12345;
  double next() {
    s = s * 1664525u + 1013904223u;
    return double(s >> 8) / double(1 << 23) - 1.0;
  }
};

struct Run {
  double real_s = 0, world_s = 0;
  uint64_t frames = 0, n_hist[5] = {}, resyncs = 0, max_n = 0;
};

// Frames of `elapsed_ms` (+- jitter_ms) at `cap`, for `seconds` of real time.
static Run Simulate(int cap, double elapsed_ms, double jitter_ms, double seconds, int force_n = 0) {
  Run r;
  double debt = 0;
  Rng rng;
  const int pq = m::PeriodQuanta(cap);
  while (r.real_s < seconds) {
    const double e = elapsed_ms + jitter_ms * rng.next();
    int n;
    if (force_n) {
      n = force_n;
    } else {
      const m::Step s = m::QuantiseStep(debt, e, pq);
      n = s.n;
      r.resyncs += s.resync;
    }
    r.real_s += e / 1000.0;
    r.world_s += n * m::kQuantumS;
    ++r.frames;
    ++r.n_hist[n];
    if (uint64_t(n) > r.max_n) r.max_n = uint64_t(n);
  }
  return r;
}

// The game thread sleeps to deadlines (the pacer): frame k starts at k x period + j_k, so the elapsed
// time is period + j_k - j_(k-1) and the jitter does not accumulate.
static Run SimulatePaced(int cap, double jitter_ms, double seconds) {
  Run r;
  double debt = 0;
  Rng rng;
  const int pq = m::PeriodQuanta(cap);
  const double period = m::kQuantumMs * pq;
  double prev_j = 0;
  while (r.real_s < seconds) {
    const double j = jitter_ms * rng.next();
    const double e = period + j - prev_j;
    prev_j = j;
    const m::Step s = m::QuantiseStep(debt, e, pq);
    r.resyncs += s.resync;
    r.real_s += e / 1000.0;
    r.world_s += s.n * m::kQuantumS;
    ++r.frames;
    ++r.n_hist[s.n];
  }
  return r;
}

static void TestQuantiser() {
  // Steady 60 at a 60 cap: n = 1 every frame, world = real.
  Run a = Simulate(60, 1000.0 / 60, 0, 60);
  CHECK(a.n_hist[1] == a.frames, "n1 %llu of %llu", (unsigned long long)a.n_hist[1], (unsigned long long)a.frames);
  CHECK(std::fabs(a.world_s / a.real_s - 1) < 0.0005, "x%.5f", a.world_s / a.real_s);

  // Steady 30 at a 30 cap: n = 2 every frame (the shipped 1/30 step).
  Run b = Simulate(30, 1000.0 / 30, 0, 60);
  CHECK(b.n_hist[2] == b.frames, "n2 %llu of %llu", (unsigned long long)b.n_hist[2], (unsigned long long)b.frames);
  CHECK(std::fabs(b.world_s / b.real_s - 1) < 0.0005, "x%.5f", b.world_s / b.real_s);

  // A paced game thread (start-time jitter +-3 ms at 60, +-8 ms at 30): n never moves off the cap's step.
  Run c = SimulatePaced(60, 3.0, 60);
  CHECK(c.n_hist[1] == c.frames, "paced 60: n1 %llu of %llu", (unsigned long long)c.n_hist[1], (unsigned long long)c.frames);
  CHECK(std::fabs(c.world_s / c.real_s - 1) < 0.005, "x%.5f", c.world_s / c.real_s);
  Run d = SimulatePaced(30, 8.0, 60);
  CHECK(d.n_hist[2] == d.frames, "paced 30: n1 %llu n2 %llu n3 %llu n4 %llu", (unsigned long long)d.n_hist[1],
        (unsigned long long)d.n_hist[2], (unsigned long long)d.n_hist[3], (unsigned long long)d.n_hist[4]);

  // (Independent noise on every frame TIME, as opposed to on the frame START, is not a model of any
  // real loop: it random-walks the debt into the clock's resync and the world drifts fast. Not tested.)

  // A machine that holds 45 fps at a 60 cap: n alternates 1 and 2, world = real within 0.5 % over 60 s.
  Run e = Simulate(60, 1000.0 / 45, 0, 60);
  CHECK(std::fabs(e.world_s / e.real_s - 1) < 0.005, "x%.5f", e.world_s / e.real_s);
  CHECK(e.n_hist[1] > 0 && e.n_hist[2] > 0 && e.n_hist[3] == 0, "mix n1 %llu n2 %llu n3 %llu", (unsigned long long)e.n_hist[1],
        (unsigned long long)e.n_hist[2], (unsigned long long)e.n_hist[3]);

  // 30 fps at a 60 cap: n = 2. 20 fps: 3. 15 fps: 4. Real time kept to 0.5 %.
  const double rates[] = {30, 20, 15};
  for (int i = 0; i < 3; ++i) {
    Run r = Simulate(60, 1000.0 / rates[i], 0, 60);
    const int want = 2 + i;
    CHECK(r.n_hist[want] > r.frames * 98 / 100, "%g fps wants n%d: n%d %llu of %llu", rates[i], want, want,
          (unsigned long long)r.n_hist[want], (unsigned long long)r.frames);
    CHECK(std::fabs(r.world_s / r.real_s - 1) < 0.005, "%g fps x%.5f", rates[i], r.world_s / r.real_s);
  }

  // The Mac's 10 fps: capped at 4/60 per frame (slow motion x0.667, as TIMING.md says), n never above 4.
  Run f = Simulate(60, 100.0, 0, 30);
  CHECK(f.max_n == 4, "max n %llu", (unsigned long long)f.max_n);
  CHECK(std::fabs(f.world_s / f.real_s - 4.0 / 60 / 0.1) < 0.01, "x%.4f", f.world_s / f.real_s);

  // One 125 ms hitch (the A2 clamp) in a steady 60: the world catches up within a few frames and never
  // gets more than n = 4 a frame; afterwards debt is back to within 0.75 period.
  {
    double debt = 0;
    double real = 0, world = 0;
    int max_n = 0;
    for (int i = 0; i < 600; ++i) {
      const double e = i == 100 ? 125.0 : 1000.0 / 60;
      const m::Step s = m::QuantiseStep(debt, e, 1);
      real += e;
      world += s.n * m::kQuantumMs;
      max_n = std::max(max_n, s.n);
    }
    CHECK(max_n == 4, "hitch n %d", max_n);
    CHECK(std::fabs(world - real) <= 0.75 * m::kQuantumMs + 1e-6, "world - real = %.3f ms", world - real);
  }

  // Forced n: the world runs n/60 a frame whatever the time (the Mac test mode).
  Run g = Simulate(60, 80.0, 0, 20, /*force_n=*/1);
  CHECK(g.n_hist[1] == g.frames, "forced n1");

  // Caps: only 30 (and below) is a two-quanta period; 40, 60, 120 and "no cap" are paced at 60.
  CHECK(m::PeriodQuanta(30) == 2 && m::PeriodQuanta(60) == 1 && m::PeriodQuanta(120) == 1 &&
            m::PeriodQuanta(40) == 1 && m::PeriodQuanta(0) == 1,
        "period quanta");

  // The step the timer publishes: n = 2 is the shipped single-precision 1/30, bit for bit.
  CHECK(float(m::StepSeconds(2)) == float(1.0f / 30.0f), "n2 step is the shipped float 1/30");
  CHECK(float(m::StepSeconds(1)) == float(1.0f / 60.0f), "n1 step");
}

static void TestSmoothing() {
  // At 1/30 in the below-60 branch the game's value is returned bit for bit.
  for (double k : {0.05, 0.1, 0.25, 0.5, 0.9}) {
    CHECK(m::SmoothK(k, false, double(1.0f / 30.0f)) == k, "k30 at 1/30 unchanged (%g)", k);
  }
  // At 1/60 the game took its 60+ branch (k = F, 2 x k30): the hook gives k60 with two steps = one k30 step.
  for (double f : {0.2, 0.5, 0.8}) {
    const double k30 = f * 0.5;
    const double k60 = m::SmoothK(f, true, 1.0 / 60);
    CHECK(std::fabs((1 - k60) * (1 - k60) - (1 - k30)) < 1e-12, "two 1/60 steps = one 1/30 step (F %g)", f);
    CHECK(k60 < f && k60 < 0.5 * k30 * 1.5, "k60 %g is far below the game's stiff F %g", k60, f);
  }
  // A slow frame (1/20) smooths more than 1/30, k stays < 1; 1/15 equals two 1/30 steps.
  const double k20 = m::SmoothK(0.1, false, 1.0 / 20);
  CHECK(k20 > 0.1 && k20 < 1, "k at 1/20 %g", k20);
  CHECK(std::fabs((1 - m::SmoothK(0.1, false, 1.0 / 15)) - 0.9 * 0.9) < 1e-12, "1/15 = two 1/30 steps");
  // A frozen world (pause: dt at the timer's floor 0.0001) keeps the game's own easing.
  CHECK(m::SmoothK(0.3, true, 0.0001) == 0.3 && m::SmoothK(0.3, false, 0.0001) == 0.3, "pause: camera unchanged");
  CHECK(m::ChassisAlpha(0.05, true, 0.0001) == 0.05, "pause: chassis unchanged");
  // Out of range factors and a bad dt are left alone.
  CHECK(m::SmoothK(0.0, false, 1.0 / 60) == 0.0 && m::SmoothK(1.5, false, 1.0 / 60) == 1.5 &&
            m::SmoothK(0.3, false, 0.0) == 0.3,
        "left alone");

  // Chassis: the game's 0.10 at 1/30 and 0.05 at 1/60 stay; others follow 1 - 0.9^(30 dt).
  CHECK(m::ChassisAlpha(0.10, false, double(1.0f / 30.0f)) == 0.10, "0.10 at 1/30");
  CHECK(m::ChassisAlpha(0.05, true, double(1.0f / 60.0f)) == 0.05, "0.05 at 1/60");
  CHECK(std::fabs(m::ChassisAlpha(0.10, false, 1.0 / 20) - (1 - std::pow(0.9, 1.5))) < 1e-12, "1/20");
  CHECK(std::fabs(m::ChassisAlpha(0.05, true, 0.5 / 60) - (1 - std::pow(0.9, 0.25))) < 1e-12, "slow motion at 1/120");
  // 60 steps of the 0.05 at 1/60 vs 30 of 0.10 at 1/30: close (the 3 % the game's constants carry).
  CHECK(std::fabs(std::pow(1 - 0.05, 2) - (1 - 0.10)) < 0.0105, "constants agree within 1 %%");
}

static void TestRaceClock() {
  const float dt30 = float(1.0f / 30.0f) * 1000.0f;  // what the game's fmuls produces: 33.333336
  const float dt60 = float(1.0f / 60.0f) * 1000.0f;

  {  // retail at 30: 33 every step, like the shipped truncation
    m::RaceCarry c;
    int bad = 0;
    long total = 0;
    for (int i = 0; i < 1500; ++i) {
      const int ms = m::RaceMs(c, dt30, m::RaceMode::kRetail);
      total += ms;
      bad += ms != 33;
    }
    CHECK(bad == 0, "retail at 30: %d steps not 33 (total %ld)", bad, total);
  }
  {  // retail at 60 for 5 s: 4950 ms (16.5 a step)
    m::RaceCarry c;
    long total = 0;
    for (int i = 0; i < 300; ++i) total += m::RaceMs(c, dt60, m::RaceMode::kRetail);
    CHECK(std::labs(total - 4950) <= 1, "retail at 60: %ld ms in 5 s", total);
  }
  {  // exact at 60: 5000 ms
    m::RaceCarry c;
    long total = 0;
    for (int i = 0; i < 300; ++i) total += m::RaceMs(c, dt60, m::RaceMode::kExact);
    CHECK(std::labs(total - 5000) <= 1, "exact at 60: %ld ms in 5 s", total);
  }
  {  // the game's own truncation at 60 is 4 % slow: 16 ms a step (what mcla_race_clock=off leaves)
    long total = 0;
    for (int i = 0; i < 300; ++i) total += long(dt60);
    CHECK(total == 4800, "off at 60: %ld", total);
  }
  {  // mixed n = 1 and 2 steps: the clock follows world time (0.99)
    m::RaceCarry c;
    long total = 0;
    double world_ms = 0;
    Rng rng;
    for (int i = 0; i < 3000; ++i) {
      const int n = rng.next() > 0.3 ? 1 : 2;
      const float step_ms = float(float(n) / 60.0f) * 1000.0f;
      total += m::RaceMs(c, step_ms, m::RaceMode::kRetail);
      world_ms += n * m::kQuantumMs;
    }
    CHECK(std::fabs(double(total) / (world_ms * 0.99) - 1) < 1e-4, "mixed: %ld vs %.1f", total, world_ms * 0.99);
  }
  {  // a pause (0 ms) adds nothing and loses nothing
    m::RaceCarry c;
    m::RaceMs(c, dt60, m::RaceMode::kRetail);
    int added = 0;
    for (int i = 0; i < 100; ++i) added += m::RaceMs(c, 0.0, m::RaceMode::kRetail);
    CHECK(added == 0, "paused clock added %d", added);
  }
}

// One scene: frames from the quantiser at `real_ms` a frame at `cap`; each frame the movie stepper
// (and the countdown) is called in the given order. Returns steps and the time the script would add.
struct UiResult {
  uint64_t steps = 0;
  double script_s = 0, real_s = 0;
  int max_run_without_step = 0;
};
static UiResult SimulateUi(int cap, double real_ms, double seconds, bool countdown_first, int calls_per_frame = 1) {
  UiResult r;
  m::UiRecord rec;
  double debt = 0;
  double var = 0;  // the script variable countdownUpdate as last set
  uint64_t serial = 0;
  int run = 0;
  while (r.real_s < seconds) {
    const m::Step s = m::QuantiseStep(debt, real_ms, m::PeriodQuanta(cap));
    r.real_s += real_ms / 1000.0;
    ++serial;
    auto countdown = [&] { var = double(float(m::UiCountdown(rec, real_ms / 1000.0))); };
    if (countdown_first) countdown();
    bool stepped = false;
    for (int c = 0; c < calls_per_frame; ++c) {
      if (!m::UiHold(rec, serial, s.n * m::kQuantumS)) {
        ++r.steps;
        r.script_s += var;  // the script adds countdownUpdate once per movie step
        stepped = true;
      }
    }
    if (!countdown_first) countdown();
    run = stepped ? 0 : run + 1;
    r.max_run_without_step = std::max(r.max_run_without_step, run);
  }
  return r;
}

static void TestUi() {
  // 60 fps at a 60 cap: 30 steps a second (one every other frame); the countdown gets 1/30 a step.
  for (bool first : {true, false}) {
    UiResult a = SimulateUi(60, 1000.0 / 60, 60, first);
    CHECK(std::llabs(int64_t(a.steps) - 1800) <= 1, "60 fps: %llu steps in 60 s", (unsigned long long)a.steps);
    CHECK(a.max_run_without_step <= 1, "60 fps: held %d frames in a row", a.max_run_without_step);
    CHECK(std::fabs(a.script_s - a.real_s) < 0.1, "60 fps countdown %.3f s vs real %.3f s (order %d)", a.script_s, a.real_s, first);

    // 30 fps at a 30 cap: every frame steps, the countdown value is the game's frame time, exactly.
    UiResult b = SimulateUi(30, 1000.0 / 30, 60, first);
    CHECK(std::llabs(int64_t(b.steps) - 1800) <= 1, "30 fps: %llu steps", (unsigned long long)b.steps);
    CHECK(b.max_run_without_step == 0, "30 fps never holds (%d)", b.max_run_without_step);
    CHECK(std::fabs(b.script_s - b.real_s) < 0.1, "30 fps countdown %.3f vs %.3f", b.script_s, b.real_s);

    // 45 fps at a 60 cap and 20 fps: 30 steps a second as far as the machine allows, countdown = real time.
    UiResult c = SimulateUi(60, 1000.0 / 45, 60, first);
    CHECK(std::llabs(int64_t(c.steps) - 1800) <= 2, "45 fps: %llu steps", (unsigned long long)c.steps);
    CHECK(std::fabs(c.script_s - c.real_s) < 0.1, "45 fps countdown %.3f vs %.3f", c.script_s, c.real_s);
    UiResult d = SimulateUi(60, 50.0, 60, first);
    CHECK(d.max_run_without_step == 0 && std::llabs(int64_t(d.steps) - 1200) <= 2, "20 fps: %llu steps, max hold %d",
          (unsigned long long)d.steps, d.max_run_without_step);
    CHECK(std::fabs(d.script_s - d.real_s) < 0.1, "20 fps countdown %.3f vs %.3f", d.script_s, d.real_s);
  }
  // Two calls for the same movie in one frame: still one step per frame at most.
  UiResult e = SimulateUi(30, 1000.0 / 30, 10, true, 2);
  CHECK(std::llabs(int64_t(e.steps) - 300) <= 1, "double call: %llu steps in 10 s", (unsigned long long)e.steps);

  // A paused world (time scale 0) still steps the menus: the stepper uses the UNSCALED step, so the
  // test feeds the same step regardless of any scale. (Nothing to scale here by construction.)

  // A hitch (n = 4) does not become a burst: one step a frame at most, then back to normal.
  {
    m::UiRecord rec;
    uint64_t serial = 0;
    int burst = 0, steps = 0;
    for (int i = 0; i < 300; ++i) {
      const double step = i == 100 ? 4 * m::kQuantumS : m::kQuantumS;
      ++serial;
      steps += !m::UiHold(rec, serial, step);
      if (i >= 101 && i < 104) burst += !m::UiHold(rec, serial, step);  // a second call this frame
    }
    CHECK(burst <= 1, "burst %d", burst);
    CHECK(steps >= 148 && steps <= 152, "300 frames at 60 fps = ~150 steps, got %d", steps);
  }
  // The countdown passes a 0 through, and leaves the accumulator alone.
  {
    m::UiRecord rec;
    CHECK(m::UiCountdown(rec, 0.0) == 0.0, "zero stays zero");
    CHECK(std::fabs(m::UiCountdown(rec, 1.0 / 60) - 1.0 / 60) < 1e-12, "first value");
  }
}

// ---- the 60 -> 30 hold (TIMING.md 13) ----
static void TestHold() {
  // calm: under the threshold, nothing; a short window decides nothing
  CHECK(!m::HoldDrop(10, 120, 25, m::kRoomNone, 0, 8), "8 pct missed is calm");
  CHECK(!m::HoldDrop(30, 120, 25, m::kRoomNone, 0, 8), "exactly 25 pct is not above it");
  CHECK(m::HoldDrop(31, 120, 25, m::kRoomNone, 0, 8), "26 pct at the floor drops at once");
  CHECK(m::HoldDrop(60, 120, 25, m::kRoomUnknown, 0, 8), "no governor drops at once");
  CHECK(!m::HoldDrop(20, 29, 25, m::kRoomNone, 0, 8), "fewer than 30 frames decide nothing");
  // room left: wait room_wait_s of consecutive high windows
  CHECK(!m::HoldDrop(100, 120, 25, m::kRoomLeft, 0.0, 8), "room: first window waits");
  CHECK(!m::HoldDrop(100, 120, 25, m::kRoomLeft, 6.0, 8), "room: 6 s of 8 waits");
  CHECK(m::HoldDrop(100, 120, 25, m::kRoomLeft, 8.0, 8), "room: 8 s of high misses drops anyway");
  // hold length: short first, doubling when the misses return soon, reset after calm
  CHECK(m::HoldSeconds(0, 1e9, 6, 30, 30) == 6, "first hold 6 s");
  CHECK(m::HoldSeconds(6, 5.0, 6, 30, 30) == 12, "back after 5 s: 12");
  CHECK(m::HoldSeconds(12, 5.0, 6, 30, 30) == 24, "24");
  CHECK(m::HoldSeconds(24, 5.0, 6, 30, 30) == 30, "capped at the longest hold 30");
  CHECK(m::HoldSeconds(30, 5.0, 6, 30, 30) == 30, "stays 30");
  CHECK(m::HoldSeconds(24, 31.0, 6, 30, 30) == 6, "calm for 30 s at 60: short again");
  CHECK(m::HoldSeconds(6, 5.0, 40, 30, 30) == 30, "min above max clamps to max");
  CHECK(m::HoldSeconds(0, 0, 6, 0, 30) == 0, "max 0 = never");
  // a simulated thermal spike: 60 fps, GPU 18 ms for 8 s then fine; the shipped rule (5 s window > 10 %, 30 s hold)
  // against this one with no governor (min 6 s, 2 s windows): seconds spent at 30 after the spike ends at t = 8
  auto run = [](bool adaptive, double spike_s, double total_s) {
    double t = 0, high = 0, hold_until = -1, at30 = 0, since_end = 1e9, last_end = -1e9;
    int prev = 0;
    const double win = adaptive ? 2.0 : 5.0;
    for (t = win; t <= total_s + 1e-9; t += win) {
      const bool on30 = t - win < hold_until;
      if (on30) { at30 += std::min(win, hold_until - (t - win)); continue; }
      const bool spiking = (t - win) < spike_s;
      const uint64_t frames = uint64_t(win * 60), missed = spiking ? uint64_t(0.6 * frames) : 0;
      const bool drop = adaptive ? m::HoldDrop(missed, frames, 10, m::kRoomNone, high, 8)
                                 : (missed * 10 > frames);
      if (!drop) { high = 0; continue; }
      since_end = t - last_end;
      const int hold = adaptive ? m::HoldSeconds(prev, since_end, 6, 30, 30) : 30;
      prev = hold; hold_until = t + hold; last_end = hold_until;
    }
    return at30;
  };
  const double shipped = run(false, 8.0, 120.0), adaptive = run(true, 8.0, 120.0);
  CHECK(adaptive < shipped, "an 8 s spike: adaptive %.0f s at 30 against shipped %.0f s", adaptive, shipped);
  CHECK(adaptive <= 8.0 && shipped >= 30.0, "adaptive %.0f s, shipped %.0f s", adaptive, shipped);
  std::printf("hold test: 8 s spike -> %.0f s at 30 (adaptive, no governor) against %.0f s (shipped)\n", adaptive, shipped);
}

// P1: the 120 fps grid (TIMING.md section 15). A cap above 60 quantises in 1/120 s; caps of 60 and below are
// unchanged (the coarse functions are the same calls).
static void Test120() {
  CHECK(m::FineGrid(120) && !m::FineGrid(60) && !m::FineGrid(40) && !m::FineGrid(30) && !m::FineGrid(0), "fine grid");
  // Steady 120 fps with jitter: n1 only, world = real within 0.5 %, substeps 1 a frame (1/120 s physics).
  struct R { double real = 0, world = 0; uint64_t frames = 0, hist[9] = {}, subs = 0; };
  auto sim = [](int cap, double ms, double jitter, double seconds) {
    R r;
    double debt = 0;
    Rng rng;
    while (r.real < seconds * 1000.0) {
      const double e = ms + jitter * rng.next();
      r.real += e;
      const m::Step s = m::QuantiseStepFor(debt, e, cap);
      r.world += s.n * m::QuantumMs(cap);
      r.subs += m::SubstepsFor(s.n, cap);
      ++r.hist[s.n];
      ++r.frames;
    }
    return r;
  };
  const R a = sim(120, 1000.0 / 120, 0.6, 60);
  CHECK(a.hist[1] >= a.frames * 99 / 100, "120: n1 %llu of %llu", (unsigned long long)a.hist[1],
        (unsigned long long)a.frames);
  CHECK(std::fabs(a.world / a.real - 1.0) < 0.005, "120: world/real %.4f", a.world / a.real);
  CHECK(a.subs == a.frames, "120: one substep a frame");
  // 60 fps frames under a 120 cap (missed every other refresh): n2 = 1/60 s, one substep of the shipped 1/60.
  const R b = sim(120, 1000.0 / 60, 0.6, 60);
  // (0.6 ms jitter against a 8.33 ms quantum: a few frames take n1 / n3 and the debt evens them out)
  CHECK(b.hist[2] >= b.frames * 95 / 100, "60 under 120: n2 %llu of %llu", (unsigned long long)b.hist[2],
        (unsigned long long)b.frames);
  CHECK(std::fabs(b.world / b.real - 1.0) < 0.005, "60 under 120: world/real %.4f", b.world / b.real);
  CHECK(b.subs <= b.frames * 103 / 100, "60 under 120: about one substep a frame (%llu for %llu)",
        (unsigned long long)b.subs, (unsigned long long)b.frames);
  // 45 fps under 120 (between grids): n alternates 2/3, world = real.
  const R c = sim(120, 1000.0 / 45, 0.3, 60);
  CHECK(std::fabs(c.world / c.real - 1.0) < 0.005, "45 under 120: world/real %.4f", c.world / c.real);
  // Slow frames: never more than 8/120 = 1/15 s a frame (the 60 grid's ceiling).
  const R d = sim(120, 100.0, 0, 10);
  CHECK(d.hist[8] == d.frames, "10 fps under 120: n8 every frame");
  // The coarse grid is unchanged: the 60 and 30 caps give what QuantiseStep gives.
  double d1 = 0, d2 = 0;
  for (int i = 0; i < 1000; ++i) {
    const double e = 16.0 + (i % 7);
    const m::Step x = m::QuantiseStepFor(d1, e, 60), y = m::QuantiseStep(d2, e, m::PeriodQuanta(60));
    CHECK(x.n == y.n && x.resync == y.resync && d1 == d2, "coarse grid unchanged at frame %d", i);
  }
  // Substeps: ceil(n/2) on the fine grid (never 0, never longer than 1/60 s), n on the coarse one.
  CHECK(m::SubstepsFor(1, 120) == 1 && m::SubstepsFor(2, 120) == 1 && m::SubstepsFor(3, 120) == 2 &&
            m::SubstepsFor(8, 120) == 4 && m::SubstepsFor(2, 60) == 2 && m::SubstepsFor(1, 60) == 1,
        "substeps");
  CHECK(float(m::StepSecondsFor(1, 120)) == float(1.0 / 120.0) && m::StepSecondsFor(2, 60) == m::StepSeconds(2),
        "step seconds");
  CHECK(std::fabs(m::PacePeriodMs(120) - 8.3333) < 0.001 && std::fabs(m::PacePeriodMs(60) - 16.6667) < 0.001 &&
            std::fabs(m::PacePeriodMs(30) - 33.3333) < 0.001,
        "pace periods");
  // Presentation interval: 120 Hz guest vblank exact (120 -> 1, 60 -> 2, 30 -> 4); 60 Hz only lowers.
  CHECK(m::PresentInterval(120, 120, 2) == 1 && m::PresentInterval(120, 60, 2) == 2 &&
            m::PresentInterval(120, 30, 2) == 4,
        "interval at 120 Hz");
  CHECK(m::PresentInterval(60, 60, 2) == 1 && m::PresentInterval(60, 30, 2) == 2 && m::PresentInterval(60, 120, 2) == 1 &&
            m::PresentInterval(60, 0, 2) == 1,
        "interval at 60 Hz");
  // The race clock at 120 steps a second: retail 990 ms and exact 1000 ms a second, as at 30 and 60.
  {
    m::RaceCarry retail, exact;
    int r_ms = 0, e_ms = 0;
    for (int i = 0; i < 120 * 60; ++i) {
      const double dt_ms = double(float(1.0 / 120.0)) * 1000.0;  // the float step the timer publishes
      r_ms += m::RaceMs(retail, dt_ms, m::RaceMode::kRetail);
      e_ms += m::RaceMs(exact, dt_ms, m::RaceMode::kExact);
    }
    CHECK(std::abs(r_ms - 59400) <= 1 && std::abs(e_ms - 60000) <= 1, "race clock at 120: retail %d exact %d ms in 60 s",
          r_ms, e_ms);
    // The UI movies stay at 30 steps a second of world time at 120 frames a second.
    m::UiRecord rec;
    int steps = 0;
    for (uint64_t f = 0; f < 1200; ++f) steps += m::UiHold(rec, f, 1.0 / 120.0) ? 0 : 1;
    CHECK(std::abs(steps - 300) <= 1, "UI steps at 120: %d in 10 s", steps);
    // Smoothing at 1/120: k = 1 - (1 - k30)^(1/4), chassis alpha 1 - 0.9^(1/4).
    CHECK(std::fabs(m::SmoothK(0.4, true, 1.0 / 120) - (1.0 - std::pow(0.8, 0.25))) < 1e-9, "camera k at 120");
    CHECK(std::fabs(m::ChassisAlpha(0.05, true, 1.0 / 120) - (1.0 - std::pow(0.9, 0.25))) < 1e-9, "chassis at 120");
  }
  // Frame-time counters.
  m::FrameTimes ft;
  for (int i = 0; i < 990; ++i) ft.Add(16.7);
  for (int i = 0; i < 8; ++i) ft.Add(33.3);
  ft.Add(50.0);
  ft.Add(400.0);
  CHECK(ft.count == 1000 && ft.Over(20) == 10 && ft.Over(25) == 10 && ft.Over(34) == 2 && ft.worst_ms == 400.0,
        "frame times over: %u %u %u", ft.Over(20), ft.Over(25), ft.Over(34));
  CHECK(ft.Percentile(0.5) == 17.0 && ft.Percentile(0.99) == 17.0 && ft.Percentile(0.995) == 34.0, "percentiles %.0f %.0f",
        ft.Percentile(0.99), ft.Percentile(0.995));
  std::printf("120 test: steady 120 n1 %.1f %% x%.4f; 60 under 120 n2 %.1f %% x%.4f; 45 under 120 x%.4f\n",
              100.0 * a.hist[1] / a.frames, a.world / a.real, 100.0 * b.hist[2] / b.frames, b.world / b.real,
              c.world / c.real);
}

int main() {
  Test120();
  TestQuantiser();
  TestSmoothing();
  TestRaceClock();
  TestUi();
  TestHold();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
