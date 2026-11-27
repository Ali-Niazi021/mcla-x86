// Unit test of the GPU frame governor's pure logic (mcla/hooks/mcla_gov_math.h, LOCK60 A5 / F1): the two-way ladder of tiers and VRS
// (rung map, shed order, restore order, floor / ceiling / VRS count, live changes), the shed rules (median of 4, the tail rule, missed
// frames, spacing, the cap, the pin), the restore delay and its p90, probe bounces and their decay, enrichment (only with headroom, after
// its window, the predicted cost from the table and from learning, the learning itself, the per-tier back-off and its decay, no
// oscillation against a toy GPU), pop hiding (waits, hiding moments, the shed time-out, the missed-frame override), the camera
// cut / speed tracker, HoldAllowed, the summary window, the screen-change grace (the previous screen's samples thrown away at a change of
// the driving flag, 60 frames of nothing recorded or decided), and closed loops against a toy GPU (a DVFS-like slack response among them).
// Also: the floor on auto (FloorForPreset / AutoFloor: perf = 2, perf+ / lowest = 1, quality / shipped = 4, ultra = 5) and the shed order from
// floor 2 (VRS before tier 1, no AA flicker, a no-op 2 -> 1 step when AA is pinned), the compile-burst watch (CompileWatch), the excuse
// budget (ExcuseBudget) and the compile excuse of the shed rules with its bounds (a 30 s burst of real overload still sheds).
// And sustained overload below the floor (a scene tour of single 18-19 ms snaps never takes the tier under the floor 2, an 18 ms load
// takes the whole ladder in about a second, 2 missed frames of 30 still shed there, the 30 s bounce window of a restore from a rung under the
// floor) and the learning windows with a spike (discarded, the EMA only fed by clean ones).
// And the budget following the frame-rate target (BudgetScale / ScaleParamsForCap: the 60 fps thresholds halve on the 1/120 grid).
// Standalone: no SDK, no game, no clock.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_gov_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/test_gov.cpp -o <scratch>/test_gov && <scratch>/test_gov
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "mcla_gov_math.h"

namespace m = mcla::gov::math;

namespace {

int g_checks = 0, g_failed = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failed;                                                 \
      std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                   \
      std::printf("\n");                                          \
    }                                                             \
  } while (0)

constexpr int64_t kFrameNs = 16'666'667;  // 60 fps
constexpr double kSec = 1e9;

// A small deterministic generator: uniform and roughly normal (sum of 12 uniforms) numbers.
struct Rng {
  uint32_t s = 7;
  double U() {
    s = s * 1664525u + 1013904223u;
    return double(s >> 8) / double(1 << 24);
  }
  double N() {
    double t = 0;
    for (int i = 0; i < 12; ++i) t += U();
    return t - 6.0;
  }
};

// A controller, its parameters and a clock; one call = one engine frame. The default ladder is the v1 one: tier 1 only (floor =
// ceiling = 1, no enrichment) with 3 VRS levels, so rung r = VRS level r, and every tier is pop-free.
struct Rig {
  m::Controller c;
  m::Params p;
  int64_t now = 5'000'000'000;
  int frame = 0;
  bool driving = true;
  uint32_t seq_delta = 1;  // GPU frames published per sample
  bool cut = false;
  double speed = -1.0;
  bool compile = false;  // a compile burst is on (Input::compile_burst)
  struct Event {
    int frame;
    int64_t ns;
    m::Change change;
  };
  std::vector<Event> events;  // steps
  std::vector<Event> waits;   // steps that started to wait for a hiding moment
  std::vector<Event> learns;  // finished measurements
  std::vector<Event> discards;  // measurements thrown away (a spike in a window)

  Rig() {
    p.tier_floor = 1;
    p.tier_ceiling = 1;
    p.vrs_levels = 3;
    p.max_level = 3;
    p.tier_pop_free.fill(true);
  }

  // gpu_ms <= 0: no new sample this frame.
  m::Change Frame(double gpu_ms, bool missed = false, int lag = 3) {
    m::Input in;
    in.now_ns = now;
    in.driving = driving;
    in.missed = missed;
    if (gpu_ms > 0) {
      in.new_sample = true;
      in.gpu_ms = gpu_ms;
      in.seq_delta = seq_delta;
    }
    in.lag = lag;
    in.cut = cut;
    in.speed_kmh = speed;
    in.compile_burst = compile;
    const m::Change ch = c.Step(in, p);
    if (ch.changed) events.push_back({frame, now, ch});
    if (ch.wait_started) waits.push_back({frame, now, ch});
    if (ch.learn_done) learns.push_back({frame, now, ch});
    if (ch.learn_discarded) discards.push_back({frame, now, ch});
    ++frame;
    now += kFrameNs;
    return ch;
  }
  // `n` frames of a constant GPU time; the index (0-based) of the first change, or -1.
  int Run(int n, double gpu_ms, bool missed = false) {
    int first = -1;
    for (int i = 0; i < n; ++i) {
      if (Frame(gpu_ms, missed).changed && first < 0) first = i;
    }
    return first;
  }
  int Seconds(double s, double gpu_ms, bool missed = false) { return Run(int(std::lround(s * kSec / double(kFrameNs))), gpu_ms, missed); }
  // Rung 1 with the cap there: three samples over the line shed, the cap holds it.
  void SetAtLevel1() {
    p.max_level = 1;
    Run(3, 16.0);
  }
  int Changes() const { return int(events.size()); }
  const Event& Last() const {
    static const Event none{};
    return events.empty() ? none : events.back();
  }
  m::Ladder Lad() const { return m::Ladder::Of(p); }
  int D(int level) const { return c.Doublings(Lad().At(level), now); }
  double Delay(int level) const { return c.RestoreDelayS(Lad().At(level), now, p); }
  double Seconds0() const { return double(now) / kSec; }
};

// The production ladder: the cvar defaults (shed 15.6, restore 15.0), floor 1, ceiling 4, two VRS levels, the real tier table
// (tier 3 and 5 pop). `pop_free` all true unless a test is about popping.
Rig ProdRig(int floor = 1, int ceiling = 4, bool pop_free = true) {
  Rig r;
  r.p.shed_ms = 15.6;
  r.p.restore_ms = 15.0;
  r.p.tier_floor = floor;
  r.p.tier_ceiling = ceiling;
  r.p.vrs_levels = 2;
  r.p.max_level = -1;
  if (!pop_free) r.p.tier_pop_free = m::Params().tier_pop_free;
  return r;
}

struct Step {
  int tier, vrs;
  m::Kind kind;
};
std::vector<Step> StepsOf(const Rig& r, size_t from = 0) {
  std::vector<Step> out;
  for (size_t i = from; i < r.events.size(); ++i) out.push_back({r.events[i].change.to_tier, r.events[i].change.to_vrs, r.events[i].change.kind});
  return out;
}
bool Is(const Step& s, int tier, int vrs, m::Kind kind) { return s.tier == tier && s.vrs == vrs && s.kind == kind; }

// The toy GPU of the tier ladder: the busy time of a state is `base` plus the real cost of every tier above 1 up to the state's, minus
// what VRS saves, published 3 frames late; `noise` is uniform +-.
struct TierPlant {
  double base = 9.0;
  double cost[m::kTiers] = {0, 0, 0.3, 0.4, 0.8, 1.0};  // the real ms of tier k over k-1
  double vrs_save[3] = {0.0, 0.4, 0.6};
  double noise = 0.0;
  double lag_line[3] = {0, 0, 0};
  Rng rng;

  double Busy(m::State s) const {
    double t = base;
    for (int k = 2; k <= s.tier && k < m::kTiers; ++k) t += cost[k];
    return t - vrs_save[std::min(s.vrs, 2)];
  }
  double Next(m::State s) {
    const double now_ms = Busy(s) + (rng.U() * 2.0 - 1.0) * noise;
    const double published = lag_line[2];
    lag_line[2] = lag_line[1];
    lag_line[1] = lag_line[0];
    lag_line[0] = now_ms;
    return published > 0 ? published : now_ms;
  }
};
// `frames` frames against the plant (the sample of a frame is of the state the controller held before it).
void Drive(Rig& r, TierPlant& plant, int frames, bool missed = false) {
  for (int i = 0; i < frames; ++i) r.Frame(plant.Next(r.c.Current()), missed);
}
void DriveSeconds(Rig& r, TierPlant& plant, double s) { Drive(r, plant, int(std::lround(s * kSec / double(kFrameNs)))); }

// ---- statistics -----------------------------------------------------------------------------------------------------

void TestHistogram() {
  m::Histogram h;
  CHECK(h.Percentile(0.5) < 0 && h.Count() == 0, "empty");
  for (int i = 0; i < 100; ++i) h.Add(m::Histogram::BucketOf(10.0));
  for (int i = 0; i < 5; ++i) h.Add(m::Histogram::BucketOf(20.0));
  CHECK(std::fabs(h.Percentile(0.50) - 10.0) < 0.05 && std::fabs(h.Percentile(0.95) - 10.0) < 0.05 && std::fabs(h.Percentile(0.99) - 20.0) < 0.05,
        "p50 %.3f p95 %.3f p99 %.3f", h.Percentile(0.5), h.Percentile(0.95), h.Percentile(0.99));
  h.Remove(m::Histogram::BucketOf(20.0));
  CHECK(h.Count() == 104, "count");
  h.Clear();
  CHECK(h.Count() == 0 && h.Percentile(0.5) < 0, "cleared");
  // Everything above 80 ms lands in the last bucket; zero and NaN in the first.
  CHECK(m::Histogram::BucketOf(500.0) == m::Histogram::kBuckets - 1 && m::Histogram::BucketOf(0.0) == 0 && m::Histogram::BucketOf(std::nan("")) == 0, "ends");
  // The restore line must be exact at the cvar's own value: 12.8 is in a bucket above 12.775 and below 12.825.
  CHECK(m::Histogram::BucketMs(m::Histogram::BucketOf(12.79)) < 12.8 && m::Histogram::BucketMs(m::Histogram::BucketOf(12.80)) >= 12.8, "12.8 boundary");
}

void TestTimedWindow() {
  m::TimedWindow w;
  for (int i = 0; i < 100; ++i) w.Add(int64_t(i) * 100'000'000, 10.0);        // 10 s of 10 ms
  for (int i = 100; i < 150; ++i) w.Add(int64_t(i) * 100'000'000, 20.0);      // 5 s of 20 ms
  w.Trim(int64_t(149) * 100'000'000, 10'000'000'000);                         // the last 10 s
  CHECK(w.Count() == 101 && std::fabs(w.Percentile(0.5) - 10.0) < 0.05 && std::fabs(w.Percentile(0.99) - 20.0) < 0.05, "count %u", w.Count());
  w.Trim(int64_t(149) * 100'000'000, 2'000'000'000);  // the last 2 s: all 20 ms
  CHECK(w.Count() == 21 && std::fabs(w.Percentile(0.5) - 20.0) < 0.05, "short window %u", w.Count());
  w.Reset();
  CHECK(w.Count() == 0 && w.Percentile(0.5) < 0, "reset");
}

// ---- the ladder -----------------------------------------------------------------------------------------------------

void TestLadderMap() {
  m::Params p;
  p.tier_floor = 1;
  p.tier_ceiling = 4;
  p.vrs_levels = 2;
  m::Ladder l = m::Ladder::Of(p);  // the default: rungs -3 .. 2
  CHECK(l.enrich == 3 && l.shed == 2 && l.TopTier() == 4, "F1 C4 V2: %d / %d", l.enrich, l.shed);
  CHECK(l.At(-3) == (m::State{4, 0}) && l.At(-1) == (m::State{2, 0}) && l.At(0) == (m::State{1, 0}) && l.At(1) == (m::State{1, 1}) && l.At(2) == (m::State{1, 2}),
        "states");
  CHECK(l.At(-9) == l.At(-3) && l.At(9) == l.At(2), "At clamps");
  for (int r = -3; r <= 2; ++r) CHECK(l.Rung(l.At(r)) == r && l.On(l.At(r)), "round trip %d", r);
  CHECK(!l.On(m::State{2, 1}) && !l.On(m::State{5, 0}) && !l.On(m::State{1, 3}) && !l.On(m::State{0, 0}), "off the ladder");

  p.tier_floor = 3;  // F3 C4 V2: 4 and 3 above, VRS 1 2, tiers 2 1 below: rungs -1 .. 4
  l = m::Ladder::Of(p);
  CHECK(l.enrich == 1 && l.shed == 4, "F3: %d / %d", l.enrich, l.shed);
  CHECK(l.At(-1) == (m::State{4, 0}) && l.At(0) == (m::State{3, 0}) && l.At(1) == (m::State{3, 1}) && l.At(2) == (m::State{3, 2}) && l.At(3) == (m::State{2, 2}) &&
            l.At(4) == (m::State{1, 2}),
        "F3 states");
  for (int r = -1; r <= 4; ++r) CHECK(l.Rung(l.At(r)) == r && l.On(l.At(r)), "F3 round trip %d", r);
  CHECK(!l.On(m::State{2, 0}) && !l.On(m::State{2, 1}) && l.On(m::State{2, 2}), "below the floor VRS is at its maximum");
  CHECK(l.Clamp(m::State{2, 0}) == (m::State{2, 2}) && l.Clamp(m::State{5, 1}) == (m::State{4, 0}) && l.Clamp(m::State{3, 9}) == (m::State{3, 2}), "clamp");

  p.tier_ceiling = 2;  // C < F: no enrichment
  l = m::Ladder::Of(p);
  CHECK(l.enrich == 0 && l.TopTier() == 3 && l.At(-5) == (m::State{3, 0}), "C below F: no rungs above the base");
  p.vrs_levels = 0;    // no VRS: the base's next rung is the tier below
  l = m::Ladder::Of(p);
  CHECK(l.shed == 2 && l.At(1) == (m::State{2, 0}) && l.At(2) == (m::State{1, 0}), "no VRS");
  p.tier_floor = 99;   // clamped
  p.tier_ceiling = -4;
  l = m::Ladder::Of(p);
  CHECK(l.floor == 5 && l.ceiling == 1 && l.enrich == 0, "floor and ceiling clamp");
  // Popping steps: between k-1 and k with tier k not pop-free (the table: 3 and 5).
  const m::Params table;
  CHECK(m::Ladder::Popping({2, 0}, {3, 0}, table) && m::Ladder::Popping({3, 0}, {2, 0}, table) && !m::Ladder::Popping({3, 0}, {4, 0}, table) &&
            m::Ladder::Popping({4, 0}, {5, 0}, table) && !m::Ladder::Popping({1, 0}, {2, 0}, table) && !m::Ladder::Popping({3, 1}, {3, 2}, table),
        "popping steps");
}

void TestCameraMotion() {
  m::CameraMotion cam;
  auto r = cam.Update(0, 0, 0, 0, 20.0);
  CHECK(!r.cut && r.speed_kmh < 0, "the first position says nothing");
  // 0.5 m a frame at 60 fps = 30 m/s = 108 km/h.
  int64_t t = 0;
  double x = 0;
  for (int i = 0; i < 30; ++i) {
    t += kFrameNs;
    x += 0.5;
    r = cam.Update(t, x, 0, 0, 20.0);
  }
  CHECK(!r.cut && std::fabs(r.speed_kmh - 108.0) < 0.5, "steady speed %.2f km/h", r.speed_kmh);
  // A jump of 25 m in one frame is a cut; the speed before it stays.
  t += kFrameNs;
  r = cam.Update(t, x + 25.0, 0, 0, 20.0);
  CHECK(r.cut && std::fabs(r.speed_kmh - 108.0) < 0.5, "cut: %d, speed %.2f", int(r.cut), r.speed_kmh);
  x += 25.0;
  // 19 m is no cut (and an absurd speed: the smoothing takes it in at its own pace).
  t += kFrameNs;
  r = cam.Update(t, x + 19.0, 0, 0, 20.0);
  CHECK(!r.cut && r.speed_kmh > 108.0, "19 m: not a cut");
  x += 19.0;
  // The speed is smoothed: a step from 108 to 36 km/h (10 m/s) comes down over a few tenths of a second.
  cam = m::CameraMotion();
  t = 0;
  x = 0;
  cam.Update(t, x, 0, 0, 20.0);
  for (int i = 0; i < 60; ++i) {
    t += kFrameNs;
    x += 0.5;
    r = cam.Update(t, x, 0, 0, 20.0);
  }
  t += kFrameNs;
  x += 10.0 / 60.0;
  r = cam.Update(t, x, 0, 0, 20.0);
  CHECK(r.speed_kmh > 90.0 && r.speed_kmh < 108.0, "one frame after the step: %.1f km/h", r.speed_kmh);
  for (int i = 0; i < 60; ++i) {
    t += kFrameNs;
    x += 10.0 / 60.0;
    r = cam.Update(t, x, 0, 0, 20.0);
  }
  CHECK(std::fabs(r.speed_kmh - 36.0) < 1.0, "a second later: %.1f km/h", r.speed_kmh);
  // A gap of more than 0.5 s (a load) leaves the speed unknown; 3D distance counts; a cut limit of 0 disables the cut.
  t += 800'000'000;
  r = cam.Update(t, x + 1, 0, 0, 20.0);
  CHECK(!r.cut && r.speed_kmh < 0, "gap: unknown");
  t += kFrameNs;
  r = cam.Update(t, x + 1, 30.0, 40.0, 0.0);
  CHECK(!r.cut, "cut limit 0");
  cam = m::CameraMotion();
  cam.Update(0, 5.0, 30.0, 40.0, 20.0);
  r = cam.Update(kFrameNs, 5.0, 30.0, 40.0, 20.0);
  CHECK(!r.cut && r.speed_kmh == 0.0, "standing still: speed 0 (%.2f)", r.speed_kmh);
  cam = m::CameraMotion();
  cam.Update(0, 0, 0, 0, 20.0);
  r = cam.Update(kFrameNs, 12.0, 0.0, 17.0, 20.0);  // sqrt(12^2 + 17^2) = 20.8
  CHECK(r.cut, "the distance is 3D");
}

// ---- the shed rules -------------------------------------------------------------------------------------------------

void TestShedLatency() {
  Rig r;
  // Under the line: nothing, however long.
  CHECK(r.Run(600, 14.0) < 0 && r.c.Level() == 0, "14.0 ms is under 14.8");
  // Over it: the median of the last 4 needs at least 3 samples of the burst.
  Rig s;
  const int first = s.Run(100, 16.0);
  CHECK(first == 2 && s.c.Level() >= 1, "first shed on the 3rd sample (the median needs 3), got %d", first);
  CHECK(s.events[0].change.reason == m::Reason::kGpuMedian && s.events[0].change.from == 0 && s.events[0].change.to == 1 &&
            std::fabs(s.events[0].change.stat_ms - 16.0) < 1e-9 && s.events[0].change.stat_n == 3 && std::string(s.events[0].change.stat_name) == "p50",
        "reason and statistic of the first shed");
  CHECK(s.events[0].change.from_tier == 1 && s.events[0].change.to_tier == 1 && s.events[0].change.from_vrs == 0 && s.events[0].change.to_vrs == 1 &&
            s.events[0].change.kind == m::Kind::kShed,
        "the first shed is VRS 0 -> 1");
  // A rise from a calm run: 12 ms then 16 ms: the 3rd high sample.
  Rig t;
  t.Run(100, 12.0);
  CHECK(t.Run(10, 16.0) == 2, "rise from 12 to 16 ms sheds on the 3rd high sample");
  // One spike among calm frames does not shed; two 30 ms frames of the last four do: [12, 12, 30, 30] -> 21 ms.
  Rig u;
  u.Run(100, 12.0);
  u.Frame(30.0);
  CHECK(u.Run(100, 12.0) < 0 && u.c.Level() == 0, "a single 30 ms spike is no shed");
  Rig w;
  w.Run(100, 12.0);
  CHECK(!w.Frame(30.0).changed, "first spike");
  const m::Change two = w.Frame(30.0);
  CHECK(two.changed && w.c.Level() == 1 && std::fabs(two.stat_ms - 21.0) < 1e-9 && two.stat_n == 4, "two 30 ms frames in the last four: median 21 ms");
}

void TestShedSpacing() {
  Rig r;
  r.p.max_level = 3;
  r.Run(300, 20.0);
  CHECK(r.Changes() == 3 && r.c.Level() == 3, "sheds up to the cap: %d changes, level %d", r.Changes(), r.c.Level());
  CHECK(r.events[1].frame - r.events[0].frame == 6 && r.events[2].frame - r.events[1].frame == 6, "6 GPU frames between two sheds: %d, %d",
        r.events[1].frame - r.events[0].frame, r.events[2].frame - r.events[1].frame);
  // A larger shed_frames spaces them more; a burst of published frames counts all of them.
  Rig s;
  s.p.shed_frames = 10;
  s.Run(300, 20.0);
  CHECK(s.events[1].frame - s.events[0].frame == 10, "shed_frames 10");
  Rig t;
  t.seq_delta = 3;  // 3 GPU frames published per call
  t.Run(100, 20.0);
  CHECK(t.events[1].frame - t.events[0].frame == 2, "6 GPU frames = 2 calls of 3: %d", t.events[1].frame - t.events[0].frame);
  // The cap: no shed beyond max_level; none at all when it is 0.
  Rig u;
  u.p.max_level = 0;
  CHECK(u.Run(300, 30.0) < 0, "max level 0: no actuator, never a shed");
}

// A stream that repeats `pattern` (ms values): the tail rule alone (the median of 4 stays under the line).
void TestTailRule() {
  const double pattern[5] = {12.0, 12.0, 12.0, 12.0, 20.0};  // every 5th sample over the 14.8 line: median 12, 6 over in 30
  auto run = [&](Rig& r, int frames) {
    int first = -1;
    for (int i = 0; i < frames; ++i) {
      if (r.Frame(pattern[i % 5]).changed && first < 0) first = i;
    }
    return first;
  };
  Rig a;
  const int first = run(a, 120);
  CHECK(first == 14 && a.events[0].change.reason == m::Reason::kGpuTail && std::string(a.events[0].change.stat_name) == "tail" &&
            a.events[0].change.stat_n == 3 && a.events[0].change.stat_window == 3 * 5 && std::fabs(a.events[0].change.stat_ms - 20.0) < 1e-9,
        "the 3rd sample over the line sheds: frame %d", first);
  // After a step the old samples do not count: the next shed needs 3 new overs (and the first 4 published frames are skipped).
  CHECK(a.Changes() >= 2 && a.events[1].frame - a.events[0].frame >= 15, "the next shed needs three more over-samples: %d frames later",
        a.Changes() >= 2 ? a.events[1].frame - a.events[0].frame : -1);
  // A larger count waits for it; 0 switches the rule off.
  Rig b;
  b.p.shed_over_count = 5;
  CHECK(run(b, 120) == 24, "5 overs: the 5th over-sample");
  Rig c;
  c.p.shed_over_count = 0;
  CHECK(run(c, 600) < 0, "tail rule off");
  // The window is 30 samples: overs 10 apart (3 per 30) shed, overs 15 apart (2 per 30) never do; a lone spike never does.
  auto spaced = [&](int every) {
    Rig r;
    int first = -1;
    for (int i = 0; i < 1200; ++i) {
      if (r.Frame(i % every == every - 1 ? 20.0 : 12.0).changed && first < 0) first = i;
    }
    return first;
  };
  CHECK(spaced(10) >= 0 && spaced(15) < 0 && spaced(300) < 0, "overs 10 apart shed (%d), 15 apart do not (%d), 300 apart do not", spaced(10), spaced(15));
  // Samples of a menu or a load are not the drive's: heavy samples while not driving, calm after: no shed.
  Rig d;
  d.driving = false;
  run(d, 120);
  d.driving = true;
  CHECK(d.Run(600, 12.0) < 0 && d.Changes() == 0, "no shed on the strength of a menu's frames");
  // The threshold is the live shed_ms.
  Rig e;
  e.p.shed_ms = 25.0;
  CHECK(run(e, 300) < 0, "20 ms is under shed_ms 25");
}

void TestMissedFrames() {
  // One missed frame sheds nothing, not even with the GPU at the line.
  Rig a;
  a.Run(60, 14.0);
  CHECK(!a.Frame(14.0, true).changed && a.c.Level() == 0, "one missed frame at 14.0 ms: no shed");
  a.Run(60, 14.0);
  CHECK(a.c.Level() == 0, "still none");
  // Two within the last 30 engine frames shed (the GPU fine: the engine frames are what is late).
  Rig b;
  b.Run(60, 13.0);
  b.Frame(13.0, true);
  b.Run(14, 13.0);
  const m::Change two = b.Frame(13.0, true);  // 15 frames after the first
  CHECK(two.changed && two.reason == m::Reason::kMissedFrames && two.to == 1 && std::string(two.stat_name) == "missed" && two.stat_n == 2 &&
            two.stat_window == 30 && two.missed == 2 && two.signal_gpu,
        "two missed frames within 30 shed with a GPU signal too");
  // The window is 30 frames: 29 frames apart sheds, 30 apart does not.
  Rig c;
  c.Run(60, 13.0);
  c.Frame(13.0, true);
  c.Run(28, 13.0);
  CHECK(c.Frame(13.0, true).changed, "missed frames 29 apart (both inside the last 30)");
  Rig d;
  d.Run(60, 13.0);
  d.Frame(13.0, true);
  d.Run(29, 13.0);
  CHECK(!d.Frame(13.0, true).changed && d.c.Level() == 0, "missed frames 30 apart: the first fell out");
  // After a step the earlier missed frames are not counted again (no second shed from the same two).
  Rig e;
  e.Run(60, 13.0);
  e.Frame(13.0, true);
  e.Frame(13.0, true);
  CHECK(e.c.Level() == 1, "shed by two in a row");
  CHECK(e.Run(120, 13.0) < 0 && e.c.Level() == 1, "the same two do not shed again");
  // No GPU signal (no sample for 1 s): the same rule.
  Rig f;
  f.Run(90, 0.0);
  CHECK(!f.c.SignalLive(), "no signal");
  CHECK(!f.Frame(0, true).changed && f.c.Level() == 0, "one missed frame");
  f.Run(10, 0.0);
  const m::Change ch = f.Frame(0, true);
  CHECK(ch.changed && ch.reason == m::Reason::kMissedFrames && ch.to == 1 && !ch.signal_gpu && ch.missed == 2, "two missed frames sheds without a GPU signal");
  // Missed frames while not driving (a pause, a load) count for nothing, and the count starts over when driving resumes.
  Rig g;
  g.p.screen_grace_frames = 0;  // the grace would swallow the frame this test is about; the rule under test is the history's reset
  g.Run(60, 13.0);
  g.Frame(13.0, true);
  g.driving = false;
  g.Run(10, 13.0, true);
  g.driving = true;
  CHECK(!g.Frame(13.0, true).changed && g.c.Level() == 0, "only one missed frame counts after a pause");
  // The signal lapse: after 1 s without a sample the fallback is in force and a live signal returns with the next sample.
  Rig h;
  h.Run(30, 12.0);
  CHECK(h.c.SignalLive(), "live");
  h.Seconds(1.2, 0.0);
  CHECK(!h.c.SignalLive(), "lapsed after 1 s");
  h.Frame(12.0);
  CHECK(h.c.SignalLive(), "back");
}

// The commander's field pattern (run int1-base, a busy machine): busy ~ N(15.2, 0.4) with every ~10th sample at 24-26 ms, and 12-18
// missed frames per 5 s. The median of 4 sits under 15.6 and never sheds; the tail rule and the missed-frames rule must, within ~0.5 s.
void TestFieldBimodal() {
  auto stream = [](Rng& rng, int i) { return i % 10 == 9 ? 24.0 + 2.0 * rng.U() : 15.2 + 0.4 * rng.N(); };
  auto base_rig = [] {
    Rig r = ProdRig(1, 1);  // no enrichment: the shed order is what is under test
    r.p.vrs_levels = 2;
    return r;
  };
  // Both signals: first shed within 33 frames (0.55 s).
  {
    Rig r = base_rig();
    Rng rng;
    int first = -1;
    for (int i = 0; i < 90; ++i) {
      const bool missed = i % 20 == 10;  // 15 per 5 s
      if (r.Frame(stream(rng, i), missed).changed && first < 0) first = i;
    }
    CHECK(first >= 0 && first <= 33, "bimodal busy with missed frames: first shed at frame %d", first);
    std::printf("field pattern (p50 15.2, every 10th at 24-26 ms, 15 missed per 5 s): first shed at frame %d (%.2f s), by %s\n", first,
                double(first) / 60.0, first >= 0 ? m::ReasonText(r.events[0].change.reason) : "-");
    CHECK(r.Changes() >= 2, "and it keeps shedding while the pattern lasts (%d steps in 1.5 s)", r.Changes());
  }
  // Busy alone (no missed frame flags at all): the tail rule, within 0.5 s.
  {
    Rig r = base_rig();
    Rng rng;
    int first = -1;
    for (int i = 0; i < 90; ++i) {
      if (r.Frame(stream(rng, i)).changed && first < 0) first = i;
    }
    CHECK(first >= 0 && first <= 33, "bimodal busy alone: first shed at frame %d", first);
    std::printf("field pattern, busy alone: first shed at frame %d (%.2f s), by %s\n", first, double(first) / 60.0,
                first >= 0 ? m::ReasonText(r.events[0].change.reason) : "-");
    CHECK(r.events[0].change.reason == m::Reason::kGpuTail || r.events[0].change.reason == m::Reason::kGpuMedian, "by the busy statistics");
  }
  // The tail rule on its own: a steadier base (15.0, sd 0.2: the median of 4 cannot reach 15.6 even with a spike among the four) and the
  // same every-10th spikes: the third spike in the window sheds, at frame 29, by the tail.
  {
    Rig r = base_rig();
    Rng rng;
    int first = -1;
    for (int i = 0; i < 90; ++i) {
      if (r.Frame(i % 10 == 9 ? 24.0 + 2.0 * rng.U() : 15.0 + 0.2 * rng.N()).changed && first < 0) first = i;
    }
    CHECK(first >= 0 && first <= 33 && r.events[0].change.reason == m::Reason::kGpuTail && r.events[0].change.stat_n >= 3,
          "median under the line, spikes every 10th sample: the tail sheds at frame %d", first);
    std::printf("field pattern, tail rule alone (median stays under the line): first shed at frame %d (%.2f s)\n", first, double(first) / 60.0);
  }
  // Missed frames alone (the GPU at 12 ms): the missed-frames rule; 15 per 5 s = every 20th frame: the 2nd within 20 frames.
  {
    Rig r = base_rig();
    Rng rng;
    int first = -1;
    for (int i = 0; i < 90; ++i) {
      if (r.Frame(12.0 + 0.4 * rng.N(), i % 20 == 10).changed && first < 0) first = i;
    }
    CHECK(first >= 0 && first <= 33 && r.events[0].change.reason == m::Reason::kMissedFrames, "missed frames alone: first shed at frame %d", first);
    std::printf("field pattern, missed frames alone: first shed at frame %d (%.2f s)\n", first, double(first) / 60.0);
  }
  // The same pattern from a steady state of a minute: still the first shed within the window, and the ladder is climbed.
  {
    Rig r = base_rig();
    Rng rng;
    for (int i = 0; i < 3600; ++i) r.Frame(11.0 + 0.4 * rng.N());  // a calm minute
    const int before = r.Changes();
    int first = -1;
    for (int i = 0; i < 90; ++i) {
      if (r.Frame(stream(rng, i), i % 20 == 10).changed && first < 0) first = i;
    }
    CHECK(first >= 0 && first <= 33, "after a minute of other load: first shed at frame %d (%d earlier steps)", first, before);
  }
}

// A clean busy stream ~ N(12, 0.5) with one 25 ms spike every 5 s must not shed (the spike frame missed, or not).
void TestCleanSpike() {
  for (const bool flagged : {false, true}) {
    for (const int ceiling : {1, 4}) {
      Rig r = ProdRig(1, ceiling);
      Rng rng;
      for (int i = 0; i < 60 * 600; ++i) {
        const bool spike = i % 300 == 150;
        r.Frame(spike ? 25.0 : 12.0 + 0.5 * rng.N(), spike && flagged);
      }
      int sheds = 0;
      for (const auto& e : r.events) sheds += e.change.kind == m::Kind::kShed ? 1 : 0;
      CHECK(sheds == 0, "a lone 25 ms spike every 5 s (missed flag %d, ceiling %d): %d sheds in 10 minutes", int(flagged), ceiling, sheds);
      if (ceiling == 1) CHECK(r.Changes() == 0, "no change at all without enrichment (%d)", r.Changes());
      std::printf("clean stream with a lone 25 ms spike every 5 s (missed flag %d, ceiling %d): %d sheds, %d steps in 10 minutes\n", int(flagged), ceiling, sheds,
                  r.Changes());
    }
  }
  // Two spikes close together (2 of 30 frames missed) is a different matter: that sheds.
  Rig r = ProdRig(1, 1);
  Rng rng;
  for (int i = 0; i < 600; ++i) r.Frame(12.0 + 0.5 * rng.N());
  r.Frame(25.0, true);
  r.Run(5, 12.0);
  CHECK(r.Frame(25.0, true).changed, "two missed spikes 6 frames apart shed");
}

// ---- restore ----------------------------------------------------------------------------------------------------------

void TestRestoreDelay() {
  Rig r;
  r.SetAtLevel1();
  CHECK(r.c.Level() == 1, "level 1");
  const int64_t t0 = r.events.back().ns;
  // 11 ms from here on: the restore comes after restore_s = 4 s.
  const int first = r.Seconds(8.0, 11.0);
  CHECK(first >= 0 && r.c.Level() == 0, "restored");
  const double waited = double(r.Last().ns - t0) / kSec;
  CHECK(r.Last().change.reason == m::Reason::kRestore && r.Last().change.kind == m::Kind::kRestore && r.Last().change.from == 1 && r.Last().change.to == 0 &&
            waited >= 4.0 && waited < 4.05 && std::string(r.Last().change.stat_name) == "p90" && r.Last().change.stat_ms < 11.1,
        "restore after %.3f s", waited);
  // Not a frame earlier: 3.9 s of calm is not enough.
  Rig s;
  s.SetAtLevel1();
  CHECK(s.Seconds(3.9, 11.0) < 0 && s.c.Level() == 1, "no restore before 4 s");
  // A longer restore_s (live) waits longer.
  Rig t;
  t.SetAtLevel1();
  t.p.restore_s = 10.0;
  CHECK(t.Seconds(9.9, 11.0) < 0 && t.Seconds(0.3, 11.0) >= 0, "restore_s 10");
}

void TestRestoreNeedsP90() {
  // 15 % of the samples at 14 ms (clustered, so that the median of 4 stays under the shed line): p90 = 14 >= 12.8, no restore.
  Rig r;
  r.SetAtLevel1();
  int restored = -1;
  for (int i = 0; i < 20 * 20 && restored < 0; ++i) {
    for (int k = 0; k < 20; ++k) {
      if (r.Frame(k >= 17 ? 14.0 : 11.0).changed) restored = i;
    }
  }
  CHECK(restored < 0 && r.c.Level() == 1, "15 %% of the frames at 14 ms: p90 14 ms, not restored in 2 minutes");
  // 5 % at 14 ms: p90 = 11 ms, restored.
  Rig s;
  s.SetAtLevel1();
  int when = -1;
  for (int i = 0; i < 20 * 20 && when < 0; ++i) {
    for (int k = 0; k < 20; ++k) {
      if (s.Frame(k == 19 ? 14.0 : 11.0).changed) when = i;
    }
  }
  CHECK(when >= 0 && s.c.Level() == 0, "5 %% at 14 ms: restored");
  // Just over / under the line.
  Rig u;
  u.SetAtLevel1();
  CHECK(u.Seconds(8.0, 12.9) < 0 && u.c.Level() == 1, "12.9 ms is over restore_ms 12.8");
  Rig v;
  v.SetAtLevel1();
  CHECK(v.Seconds(8.0, 12.7) >= 0 && v.c.Level() == 0, "12.7 ms is under it");
}

void TestMissedFrameResetsRestore() {
  Rig r;
  r.SetAtLevel1();
  r.Seconds(3.0, 11.0);
  r.Frame(11.0, true);  // a missed frame at 3 s: the quiet time starts over
  const int64_t t_miss = r.now - kFrameNs;
  CHECK(r.c.Level() == 1, "11 ms with a missed frame: no shed, no restore");
  CHECK(r.Seconds(3.9, 11.0) < 0, "3.9 s after the missed frame: still waiting");
  r.Seconds(0.3, 11.0);
  CHECK(r.c.Level() == 0, "restored 4 s after the missed frame");
  CHECK(double(r.Last().ns - t_miss) / kSec >= 3.99, "after %.3f s", double(r.Last().ns - t_miss) / kSec);
}

void TestRestoreWithoutGpuSignal() {
  // No GPU sample at all: missed frames shed, and 4 s without one restore.
  Rig r;
  r.p.max_level = 1;
  r.Run(10, 0.0);
  r.Frame(0, true);
  r.Frame(0, true);
  CHECK(r.c.Level() == 1, "shed by two missed frames");
  const int64_t t0 = r.Last().ns;
  CHECK(r.Seconds(3.9, 0.0) < 0, "quiet 3.9 s");
  r.Seconds(0.3, 0.0);
  CHECK(r.c.Level() == 0 && double(r.Last().ns - t0) / kSec >= 4.0 && r.Last().change.reason == m::Reason::kRestore, "restored after 4 s of quiet");
}

void TestBounce() {
  Rig r;
  r.SetAtLevel1();
  CHECK(r.c.Level() == 1 && r.D(1) == 0, "level 1");
  CHECK(std::fabs(r.Delay(1) - 4.0) < 1e-9, "delay 4 s");
  // Restore (4 s of 11 ms), then 16 ms: the shed back within 2 s is a bounce of level 1.
  const double expected[] = {8.0, 16.0, 32.0, 60.0, 60.0};
  for (double want : expected) {
    const double delay_before = r.Delay(1);
    const double since = double(r.now - r.Last().ns) / kSec;  // time since the level change
    r.Seconds(delay_before - since - 0.1, 11.0);
    CHECK(r.c.Level() == 1, "no restore 0.1 s before the delay of %.1f s", delay_before);
    r.Seconds(0.3, 11.0);
    CHECK(r.c.Level() == 0, "restored after the delay of %.1f s", delay_before);
    const int64_t restored_ns = r.Last().ns;
    r.Run(30, 16.0);
    CHECK(r.c.Level() == 1 && r.Last().change.bounce && double(r.Last().ns - restored_ns) / kSec < 2.0, "shed back within 2 s: a bounce");
    CHECK(std::fabs(r.Delay(1) - want) < 1e-9, "the next delay is %.1f s, is %.1f", want, r.Delay(1));
  }
  // Another level's delay is untouched.
  CHECK(std::fabs(r.Delay(2) - 4.0) < 1e-9 && r.D(1) == 5, "per level (%d doublings)", r.D(1));
}

void TestBounceWindowAndDecay() {
  // A shed back to the level after MORE than 2 s is not a bounce.
  Rig r;
  r.SetAtLevel1();
  r.Seconds(4.2, 11.0);  // restored
  CHECK(r.c.Level() == 0, "restored");
  r.Seconds(2.5, 11.0);
  r.Run(30, 16.0);
  CHECK(r.c.Level() == 1 && !r.Last().change.bounce && r.D(1) == 0, "shed after 2.5 s: no bounce");
  // A bounce doubles the delay (8 s); it decays one step per 30 s without another: 8 s -> 4 s after 30 s.
  Rig s;
  s.SetAtLevel1();
  s.Seconds(4.2, 11.0);
  s.Run(30, 16.0);
  CHECK(s.c.Level() == 1 && s.D(1) == 1, "one bounce");
  // The delay is 8 s now; the doubling decays 30 s after those 8 s, so at 38 s, not at 30 s (a 32 s wait must not eat its own memory).
  const int64_t t = s.Last().ns + 8'000'000'000;
  const m::State at1 = s.Lad().At(1);
  CHECK(s.c.Doublings(at1, t + 29'000'000'000) == 1 && s.c.Doublings(at1, t + 31'000'000'000) == 0, "decays 30 s after the delay");
  CHECK(std::fabs(s.c.RestoreDelayS(at1, t + 31'000'000'000, s.p) - 4.0) < 1e-9 && std::fabs(s.c.RestoreDelayS(at1, t + 29'000'000'000, s.p) - 8.0) < 1e-9,
        "delay back to 4 s");
  // Two bounces: 16 s; 30 s after that delay 8 s; 60 s after it 4 s.
  s.Seconds(8.2, 11.0);
  s.Run(30, 16.0);
  CHECK(s.D(1) == 2, "second bounce: %d", s.D(1));
  const int64_t u = s.Last().ns + 16'000'000'000;
  CHECK(s.c.Doublings(at1, u + 31'000'000'000) == 1 && s.c.Doublings(at1, u + 61'000'000'000) == 0, "two steps of decay");
  // A restore delay never exceeds 60 s (but never undercuts restore_s itself).
  m::Params big = s.p;
  big.restore_s = 100.0;
  CHECK(std::fabs(s.c.RestoreDelayS(at1, s.now, big) - 100.0) < 1e-9, "restore_s above the cap stays");
}

void TestNotDriving() {
  // A menu, a pause or a load: the level is held (no shed, no restore), and the timer starts over when driving resumes.
  Rig r;
  r.driving = false;
  r.Run(30, 30.0);
  CHECK(r.c.Level() == 0, "no shed in a menu, even at 30 ms");
  r.driving = true;
  r.Run(70, 16.0);  // the 60 frames of screen-change grace first (TestScreenGrace), then the shed on the 3rd sample
  CHECK(r.c.Level() >= 1, "sheds once driving");
  r.p.max_level = r.c.Level();
  const int shed_to = r.c.Level();
  r.driving = false;
  CHECK(r.Seconds(20.0, 9.0) < 0 && r.c.Level() == shed_to, "20 s of calm in a menu: no restore");
  r.driving = true;
  CHECK(r.Seconds(3.9, 9.0) < 0 && r.c.Level() == shed_to, "3.9 s after driving resumes: still holding");
  r.Seconds(0.3, 9.0);
  CHECK(r.c.Level() == shed_to - 1, "restored 4 s after driving resumed");
  // Pausing in the middle of the wait resets it.
  Rig s;
  s.SetAtLevel1();
  s.Seconds(3.5, 9.0);
  s.driving = false;
  s.Seconds(1.0, 9.0);
  s.driving = true;
  CHECK(s.Seconds(3.9, 9.0) < 0 && s.c.Level() == 1, "a pause reset the timer");
  s.Seconds(0.3, 9.0);
  CHECK(s.c.Level() == 0, "then restored");
}

// ---- the screen-change grace ----------------------------------------------------------------------------------------------

// A calm drive, a pause at 30 ms busy (1.5 s), then driving again. The export lags 3 frames (the first 3 samples after the resume are
// still the menu's) and the first frames of the new screen hitch (35 ms); then `scene_frames` frames of `scene_ms`. Returns the index of
// the first change counted from the frame of the resume (0 = that frame), or -1.
int ResumeFromPause(Rig& r, double scene_ms, int scene_frames) {
  r.Run(120, 12.0);
  r.driving = false;
  r.Run(90, 30.0);
  r.driving = true;
  int first = -1, i = 0;
  auto feed = [&](double ms, int n) {
    for (int k = 0; k < n; ++k, ++i) {
      if (r.Frame(ms).changed && first < 0) first = i;
    }
  };
  feed(30.0, 3);
  feed(35.0, 10);
  feed(scene_ms, scene_frames);
  return first;
}

// The screen-change grace (mcla_gov_screen_grace_frames, default 60): a change of the driving flag throws away what the previous screen
// recorded, and for the first 60 engine frames of the new one nothing is recorded or decided. Field: entering driving from a load or the
// pause menu shed 0 -> 4 within 0.3-0.4 s, judging the previous screen's samples and the new screen's first, hitchy frames.
void TestScreenGrace() {
  // (1) A pause at 30 ms, then a light scene at 12 ms: no shed with the grace; without it (the old behaviour) the menu's samples shed at once.
  {
    Rig r;
    const int first = ResumeFromPause(r, 12.0, 400);
    CHECK(first < 0 && r.Changes() == 0 && r.c.Level() == 0, "grace 60: pause at 30 ms then driving at 12 ms: no shed (first %d)", first);
    Rig c;
    c.p.screen_grace_frames = 0;
    const int cfirst = ResumeFromPause(c, 12.0, 400);
    CHECK(cfirst >= 0 && cfirst <= 3 && c.c.Level() >= 1, "grace 0 (the old behaviour) sheds on the previous screen's samples: frame %d", cfirst);
    std::printf("screen grace: resume from a 30 ms pause into 12 ms: first shed %d (grace 60) / frame %d (grace 0)\n", first, cfirst);
  }
  // (2) Resuming into a genuinely heavy scene (18 ms): the shed comes only after the grace (the change frame is its first of 60) and the
  // usual latency, the 3rd fresh sample: frame 62. The statistic is over the new screen's samples only; the next shed has its usual spacing.
  {
    Rig r;
    const int first = ResumeFromPause(r, 18.0, 400);
    CHECK(first == 62, "heavy scene: first shed at frame %d after the resume (grace 60 + the 3rd sample)", first);
    CHECK(r.Changes() >= 2 && r.events[0].change.reason == m::Reason::kGpuMedian && std::fabs(r.events[0].change.stat_ms - 18.0) < 1e-9 &&
              r.events[0].change.stat_n == 3 && r.events[1].frame - r.events[0].frame == 6,
          "the shed is judged on 3 fresh 18 ms samples, the next one 6 frames later");
    Rig c;
    c.p.screen_grace_frames = 0;
    const int cfirst = ResumeFromPause(c, 18.0, 400);
    CHECK(cfirst >= 0 && cfirst <= 3, "grace 0: the same resume sheds at frame %d", cfirst);
    std::printf("screen grace: resume from a 30 ms pause into 18 ms: first shed at frame %d (grace 60) / %d (grace 0)\n", first, cfirst);
  }
  // (3) A missed frame inside the grace does not count: one at the last frame of the grace and one 3 frames later are ONE missed frame.
  {
    auto drive_to_resume = [](Rig& r) {
      r.Run(120, 12.0);
      r.driving = false;
      r.Run(30, 12.0);
      r.driving = true;
    };
    Rig r;
    drive_to_resume(r);
    r.Run(59, 12.0);  // 0..58
    CHECK(!r.Frame(12.0, true).changed && r.c.InGrace(), "a missed frame at frame 59, the last of the grace");
    r.Run(2, 12.0);   // 60, 61
    CHECK(!r.c.InGrace(), "frame 61 is past the grace");
    CHECK(!r.Frame(12.0, true).changed && r.c.Level() == 0, "a missed frame at 62: the one in the grace does not count, no shed");
    CHECK(r.Run(120, 12.0) < 0, "and nothing after");
    Rig c;  // the same without the grace: the pair 3 frames apart sheds
    c.p.screen_grace_frames = 0;
    drive_to_resume(c);
    c.Run(59, 12.0);
    c.Frame(12.0, true);
    c.Run(2, 12.0);
    const m::Change pair = c.Frame(12.0, true);
    CHECK(pair.changed && pair.reason == m::Reason::kMissedFrames, "grace 0: the same pair sheds");
    // Two missed frames both inside the grace: nothing, ever.
    Rig d;
    drive_to_resume(d);
    d.Run(10, 12.0);
    d.Frame(12.0, true);
    d.Run(3, 12.0);
    d.Frame(12.0, true);
    CHECK(d.Run(300, 12.0) < 0 && d.Changes() == 0, "two missed frames inside the grace: no shed");
    // Two real ones right after it shed as usual (the rule is not dead).
    Rig e;
    drive_to_resume(e);
    e.Run(60, 12.0);
    e.Frame(12.0, true);
    e.Run(2, 12.0);
    const m::Change real = e.Frame(12.0, true);
    CHECK(real.changed && real.reason == m::Reason::kMissedFrames, "two missed frames after the grace shed");
  }
  // (4) Nothing is restored or enriched inside the grace, even when everything else is ready (a tiny restore delay / enrichment window).
  {
    Rig a;
    a.SetAtLevel1();
    a.p.restore_s = 0.1;
    a.driving = false;
    a.Seconds(1.0, 9.0);
    a.driving = true;
    const int restored = a.Run(100, 9.0);
    CHECK(restored == 60 && a.Last().change.kind == m::Kind::kRestore, "restore ready at 0.1 s waits for the end of the grace: frame %d", restored);
    Rig b;
    b.SetAtLevel1();
    b.p.restore_s = 0.1;
    b.p.screen_grace_frames = 0;
    b.driving = false;
    b.Seconds(1.0, 9.0);
    b.driving = true;
    const int bfirst = b.Run(100, 9.0);
    CHECK(bfirst >= 6 && bfirst <= 8, "grace 0: the same restore at frame %d", bfirst);
    Rig e = ProdRig(1, 2);
    e.p.enrich_s = 0.1;
    e.driving = false;
    e.Seconds(1.0, 9.0);
    e.driving = true;
    const int enriched = e.Run(100, 9.0);
    CHECK(enriched == 60 && e.Last().change.kind == m::Kind::kEnrich, "enrichment ready at 0.1 s waits for the end of the grace: frame %d", enriched);
    Rig f = ProdRig(1, 2);
    f.p.enrich_s = 0.1;
    f.p.screen_grace_frames = 0;
    f.driving = false;
    f.Seconds(1.0, 9.0);
    f.driving = true;
    const int ffirst = f.Run(100, 9.0);
    CHECK(ffirst >= 6 && ffirst <= 8, "grace 0: the same enrichment at frame %d", ffirst);
  }
  // (5) A missed frame inside the grace does not start the restore timer over: 4 s after the resume, not 4 s after the missed frame.
  {
    Rig r;
    r.SetAtLevel1();
    r.driving = false;
    r.Seconds(1.0, 9.0);
    r.driving = true;
    r.Run(30, 9.0);
    r.Frame(9.0, true);  // frame 30, inside the grace
    int restored = -1;
    for (int i = 31; i < 400 && restored < 0; ++i) {
      if (r.Frame(9.0).changed) restored = i;
    }
    CHECK(restored >= 240 && restored <= 242 && r.Last().change.kind == m::Kind::kRestore, "restore at frame %d (4 s after the resume)", restored);
  }
  // (6) Both directions, the count, and live changes.
  {
    Rig r;
    r.Run(10, 12.0);
    CHECK(!r.c.InGrace() && r.c.GraceFramesLeft() == 0, "no grace before any change");
    r.driving = false;
    r.Frame(12.0);
    CHECK(r.c.InGrace() && r.c.GraceFramesLeft() == 59, "a change to not driving opens the grace (left %d)", r.c.GraceFramesLeft());
    r.Run(10, 12.0);
    CHECK(r.c.InGrace() && r.c.GraceFramesLeft() == 49, "left %d", r.c.GraceFramesLeft());
    r.driving = true;
    r.Frame(12.0);
    CHECK(r.c.InGrace() && r.c.GraceFramesLeft() == 59, "the next change opens it again (left %d)", r.c.GraceFramesLeft());
    r.Run(59, 12.0);
    CHECK(r.c.InGrace() && r.c.GraceFramesLeft() == 0, "the 60th frame is the last of it");
    r.Frame(12.0);
    CHECK(!r.c.InGrace(), "then it is over");
    // Shortened to 0 live, in the middle of one: over at once.
    r.driving = false;
    r.Run(5, 12.0);
    CHECK(r.c.InGrace(), "open again");
    r.p.screen_grace_frames = 0;
    r.Frame(12.0);
    CHECK(!r.c.InGrace(), "grace set to 0 live: over");
    // A pin goes through the grace (only the closed loop is held back).
    Rig f;
    f.Run(30, 12.0);
    f.driving = false;
    f.Run(5, 12.0);
    f.driving = true;
    f.p.force_level = 2;
    const m::Change pin = f.Frame(12.0);
    CHECK(f.c.InGrace() && pin.changed && pin.reason == m::Reason::kForced && pin.to == 2, "a pin goes at once inside the grace");
  }
  // (7) The summary counts the grace frames.
  {
    Rig r;
    r.Run(10, 12.0);
    r.c.TakeWindow(r.now);
    r.driving = false;
    r.Run(70, 12.0);
    r.driving = true;
    r.Run(70, 12.0);
    const m::Window w = r.c.TakeWindow(r.now);
    CHECK(w.grace_frames == 120, "grace frames in the window: %u", w.grace_frames);
  }
  // (8) The learning: the previous screen's samples do not make the 'before' of a tier step taken just after the grace. Floor 2, ceiling
  // 2: at (2, 2) (the cap) 14 ms for 3 s, a short pause, then a heavy scene sheds (2, 2) -> (1, 2). With the grace the step has too few
  // samples of its own state to be measured; without it the pre-pause samples serve and a (bogus) cost is learned.
  for (const int grace : {60, 0}) {
    Rig r = ProdRig(2, 2);
    r.p.screen_grace_frames = grace;
    r.p.max_level = 2;
    r.Run(10, 20.0);
    CHECK(r.c.Current() == (m::State{2, 2}), "at (2, 2)");
    r.Run(180, 14.0);
    r.driving = false;
    r.Run(20, 14.0);
    r.driving = true;
    r.p.max_level = -1;
    r.Run(400, 20.0);
    CHECK(r.c.Current() == (m::State{1, 2}), "shed to tier 1");
    if (grace > 0) {
      CHECK(r.learns.empty(), "grace %d: nothing learned from the previous screen's samples (%zu)", grace, r.learns.size());
    } else {
      CHECK(r.learns.size() == 1, "grace 0: the pre-pause samples were used (%zu learned)", r.learns.size());
    }
  }
}

void TestCapAndForce() {
  // The cap, live: lowering it moves the level at once.
  Rig r;
  r.p.max_level = 3;
  r.Run(100, 20.0);
  CHECK(r.c.Level() == 3, "at 3");
  r.p.max_level = 1;
  const m::Change ch = r.Frame(20.0);
  CHECK(ch.changed && ch.reason == m::Reason::kCap && ch.from == 3 && ch.to == 1 && r.c.Level() == 1, "cap lowered to 1");
  CHECK(r.Run(100, 20.0) < 0 && r.c.Level() == 1, "stays at the cap");
  // The pin: the level is exactly the pinned one whatever the samples say, with no shed or restore counted; released, the loop resumes.
  Rig f;
  f.p.force_level = 2;
  const m::Change pin = f.Frame(5.0);
  CHECK(pin.changed && pin.reason == m::Reason::kForced && pin.to == 2, "pinned to 2");
  CHECK(f.Seconds(30.0, 5.0) < 0 && f.c.Level() == 2, "pinned through 30 s of calm");
  f.p.force_level = 0;
  f.Frame(20.0);
  CHECK(f.c.Level() == 0 && f.Seconds(2.0, 20.0) < 0, "pinned to 0 through a heavy 2 s");
  f.p.force_level = -1;
  f.Run(30, 20.0);
  CHECK(f.c.Level() >= 1, "closed loop again");
  const m::Window w = f.c.TakeWindow(f.now);
  CHECK(w.sheds >= 1 && w.restores == 0, "forced changes are not sheds or restores: %d / %d", w.sheds, w.restores);
  // A pin beyond the ladder is the last rung; the tier of the base pins with the floor.
  Rig g = ProdRig(3, 4);
  g.p.force_level = 99;
  g.Frame(9.0);
  CHECK(g.c.Current() == (m::State{1, 2}), "pin 99: the last rung (1, 2)");
  g.p.force_level = 0;
  g.Frame(9.0);
  CHECK(g.c.Current() == (m::State{3, 0}), "pin 0: the base = the floor");
}

void TestHoldAllowed() {
  Rig r;
  r.p.max_level = 2;
  r.Run(30, 12.0);
  CHECK(!r.c.HoldAllowed(r.p), "level 0 of 2: the hold is not allowed");
  r.Run(30, 20.0);
  CHECK(r.c.Level() == 2 && r.c.HoldAllowed(r.p), "at the last level and over the line: allowed");
  r.Frame(13.0);
  CHECK(r.c.Level() == 2 && !r.c.HoldAllowed(r.p), "at the last level but the last sample is under the line: not allowed");
  // No GPU signal: allowed at the last level whatever the (stale) sample.
  r.Seconds(1.5, 0.0);
  CHECK(!r.c.SignalLive() && r.c.HoldAllowed(r.p), "no signal at the last level: allowed");
  Rig s;
  s.p.max_level = 2;
  s.Run(30, 0.0);
  CHECK(!s.c.HoldAllowed(s.p), "no signal, level 0 of 2: not allowed");
  // Nothing to shed (cap 0): the hold is the only lever once the GPU is over the line.
  Rig t;
  t.p.max_level = 0;
  t.Run(30, 12.0);
  CHECK(!t.c.HoldAllowed(t.p), "level 0 = last level, GPU fine: not allowed");
  t.Run(10, 16.0);
  CHECK(t.c.HoldAllowed(t.p), "level 0 = last level, GPU over the line: allowed");
}

// The last level of the ladder is (1, V) whatever the floor, and only there is the hold allowed.
void TestHoldAllowedLadder() {
  Rig r = ProdRig(3, 4);
  r.Run(30, 12.0);
  CHECK(!r.c.HoldAllowed(r.p), "at the base: no");
  r.Run(400, 20.0);
  CHECK(r.c.Current() == (m::State{1, 2}) && r.c.HoldAllowed(r.p), "at (1, 2) and over the line: the hold may start");
  r.Run(30, 13.0);
  CHECK(!r.c.HoldAllowed(r.p), "(1, 2) with the GPU under the line: no");
  // One rung short of it (a cap): no.
  Rig s = ProdRig(3, 4);
  s.p.max_level = 3;
  s.Run(400, 20.0);
  CHECK(s.c.Current() == (m::State{2, 2}) && s.c.HoldAllowed(s.p), "the cap's rung is the last level");
  s.p.max_level = -1;
  CHECK(!s.c.HoldAllowed(s.p), "with the cap lifted (2, 2) is not the last level");
  // With enrichment (tier above the floor) the hold is never allowed.
  Rig t = ProdRig(1, 4);
  t.Run(700, 9.0);
  CHECK(t.c.Tier() >= 2 && !t.c.HoldAllowed(t.p), "enriched: no hold");
}

void TestWindow() {
  Rig r;
  r.p.max_level = 1;
  r.Run(60, 12.0);
  r.c.TakeWindow(r.now);  // start clean
  r.Run(60, 16.0);        // shed to 1
  r.Seconds(5.5, 11.0);   // restore (the 16 ms samples of the first second leave the 4 s window first)
  r.Run(30, 16.0);        // bounce
  r.Frame(16.0, true);
  const m::Window w = r.c.TakeWindow(r.now);
  CHECK(w.sheds == 2 && w.restores == 1 && w.bounces == 1 && w.missed == 1 && w.enriches == 0, "sheds %d restores %d bounces %d missed %u", w.sheds, w.restores,
        w.bounces, w.missed);
  CHECK(w.min_level == 0 && w.max_level == 1 && w.samples > 300 && w.signal_gpu && w.lag == 3, "levels %d..%d samples %u lag %d", w.min_level, w.max_level,
        w.samples, w.lag);
  int64_t total = 0;
  for (const auto& [key, ns] : w.time_ns) total += ns;
  CHECK(std::llabs(total - w.length_ns) <= 2 * kFrameNs && w.time_ns.size() == 2 && w.time_ns.at(m::Key(m::State{1, 1})) > 4'000'000'000 &&
            w.time_ns.at(m::Key(m::State{1, 0})) > 0,
        "time at levels %.2f of %.2f s", double(total) / kSec, double(w.length_ns) / kSec);
  CHECK(w.p50_ms > 10.9 && w.p50_ms < 11.2 && w.p95_ms > 15.9 && w.p95_ms < 16.2, "p50 %.2f p95 %.2f", w.p50_ms, w.p95_ms);
  // The next window starts at the current level.
  const m::Window next = r.c.TakeWindow(r.now + 1'000'000'000);
  CHECK(next.min_level == next.max_level && next.min_level == r.c.Level() && next.sheds == 0 && next.samples == 0 && next.p50_ms < 0, "next window");
}

// ---- the ladder in motion ------------------------------------------------------------------------------------------------

// Floor 2, ceiling 4, two VRS levels: enrich to (4, 0) in the calm, then a load: enriched tiers first, then VRS, then the tiers below
// the floor; then calm: the reverse (tiers up to the floor, VRS back to 0) and the enrichment again.
void TestShedAndRestoreOrder() {
  Rig r = ProdRig(2, 4);
  r.Seconds(25.0, 9.0);
  CHECK(r.c.Current() == (m::State{4, 0}) && r.Changes() == 2, "enriched to (4, 0): %d, %d", r.c.Tier(), r.c.Vrs());
  CHECK(r.c.Level() == -2, "rung -2");
  const size_t before = r.events.size();
  r.Seconds(8.0, 20.0);
  const std::vector<Step> shed = StepsOf(r, before);
  CHECK(shed.size() == 5 && Is(shed[0], 3, 0, m::Kind::kShed) && Is(shed[1], 2, 0, m::Kind::kShed) && Is(shed[2], 2, 1, m::Kind::kShed) &&
            Is(shed[3], 2, 2, m::Kind::kShed) && Is(shed[4], 1, 2, m::Kind::kShed),
        "shed order: enriched tiers, VRS 1 2, tiers below the floor (%zu steps)", shed.size());
  CHECK(r.c.Level() == 3 && r.c.Current() == (m::State{1, 2}), "the last level");
  r.Run(60, 20.0);
  CHECK(r.Changes() == int(before) + 5, "nothing beyond the last level");
  // Calm: the reverse.
  const size_t mid = r.events.size();
  r.Seconds(40.0, 9.0);
  const std::vector<Step> back = StepsOf(r, mid);
  CHECK(back.size() >= 4 && Is(back[0], 2, 2, m::Kind::kRestore) && Is(back[1], 2, 1, m::Kind::kRestore) && Is(back[2], 2, 0, m::Kind::kRestore) &&
            Is(back[3], 3, 0, m::Kind::kEnrich),
        "restore order: tier up to the floor first, then VRS back to 0, then enrichment (%zu steps)", back.size());
  // The restore steps are 4 s apart, the enrichment 10 s after the last restore.
  CHECK(std::fabs(double(r.events[mid + 1].ns - r.events[mid].ns) / kSec - 4.0) < 0.1 && double(r.events[mid + 3].ns - r.events[mid + 2].ns) / kSec >= 10.0,
        "restore every 4 s, enrich after 10 s");
}

void TestEnrichConditions() {
  // The window: not before 10 s, then at once.
  Rig a = ProdRig(1, 2);
  CHECK(a.Seconds(9.9, 9.0) < 0, "no enrichment before 10 s");
  CHECK(a.Seconds(0.3, 9.0) >= 0 && a.c.Current() == (m::State{2, 0}) && a.Last().change.kind == m::Kind::kEnrich && a.Last().change.reason == m::Reason::kEnrich,
        "enriched at 10 s");
  const m::Change& e = a.Last().change;
  CHECK(std::string(e.stat_name) == "p90" && std::fabs(e.stat_ms - 9.0) < 0.1 && std::fabs(e.est_ms - 0.3) < 1e-9 && e.learned_ms < 0 &&
            std::fabs(e.predicted_ms - 0.3) < 1e-9 && e.from == 0 && e.to == -1,
        "the log fields: p90 %.2f est %.2f learned %.2f predicted %.2f", e.stat_ms, e.est_ms, e.learned_ms, e.predicted_ms);
  CHECK(a.Seconds(60.0, 9.0) < 0 && a.c.Level() == -1, "the ceiling 2 is the top");
  // Headroom: p90 + predicted cost of tier 2 (0.3) + margin 1.5 must fit under 15.0, i.e. p90 <= 13.2.
  Rig b = ProdRig(1, 2);
  CHECK(b.Seconds(30.0, 13.1) >= 0 && b.c.Tier() == 2, "13.1 ms: 13.1 + 0.3 + 1.5 fits");
  Rig c = ProdRig(1, 2);
  CHECK(c.Seconds(60.0, 13.3) < 0 && c.c.Tier() == 1, "13.3 ms: it does not");
  // The p90, not the median: 15 % of the samples at 14 ms (the median 11) keeps it out; 5 % lets it in.
  Rig d = ProdRig(1, 2);
  int got = -1;
  for (int i = 0; i < 20 * 5 && got < 0; ++i) {
    for (int k = 0; k < 20; ++k) {
      if (d.Frame(k >= 17 ? 14.0 : 11.0).changed) got = i;
    }
  }
  CHECK(got < 0, "p90 14 ms: no enrichment");
  Rig e2 = ProdRig(1, 2);
  got = -1;
  for (int i = 0; i < 20 * 5 && got < 0; ++i) {
    for (int k = 0; k < 20; ++k) {
      if (e2.Frame(k == 19 ? 14.0 : 11.0).changed) got = i;
    }
  }
  CHECK(got >= 0 && e2.c.Tier() == 2, "p90 11 ms: enriched");
  // It needs the GPU signal.
  Rig f = ProdRig(1, 2);
  CHECK(f.Seconds(60.0, 0.0) < 0, "no GPU signal: no enrichment");
  // A missed frame, a pause: the window starts over.
  Rig g = ProdRig(1, 2);
  g.Seconds(8.0, 9.0);
  g.Frame(9.0, true);
  CHECK(g.Seconds(9.9, 9.0) < 0, "a missed frame at 8 s: 9.9 s after it, not yet");
  CHECK(g.Seconds(0.3, 9.0) >= 0, "10 s after it, enriched");
  Rig h = ProdRig(1, 2);
  h.Seconds(8.0, 9.0);
  h.driving = false;
  h.Seconds(5.0, 9.0);
  h.driving = true;
  CHECK(h.Seconds(9.9, 9.0) < 0 && h.Seconds(0.3, 9.0) >= 0, "a pause at 8 s: the window starts over");
  // Never while not driving.
  Rig i = ProdRig(1, 2);
  i.driving = false;
  CHECK(i.Seconds(60.0, 9.0) < 0, "not driving: no enrichment");
  // The live enrich_s and margin.
  Rig j = ProdRig(1, 2);
  j.p.enrich_s = 20.0;
  CHECK(j.Seconds(19.9, 9.0) < 0 && j.Seconds(0.3, 9.0) >= 0, "enrich_s 20");
  Rig k = ProdRig(1, 2);
  k.p.enrich_margin_ms = 0.0;
  CHECK(k.Seconds(30.0, 14.6) >= 0, "margin 0: 14.6 + 0.3 fits under 15.0");
  // Not from a shed rung: VRS first comes back (4 s), then the enrichment 10 s after that.
  Rig l = ProdRig(1, 2);
  l.p.max_level = 1;
  l.Run(3, 16.0);
  CHECK(l.c.Current() == (m::State{1, 1}), "shed to (1, 1)");
  l.Seconds(5.0, 9.0);
  CHECK(l.c.Current() == (m::State{1, 0}), "restored first");
  const int64_t restored = l.Last().ns;
  l.Seconds(8.5, 9.0);
  CHECK(l.c.Tier() == 1, "and 10 s after the restore, not before");
  l.Seconds(1.2, 9.0);
  CHECK(l.c.Tier() == 2 && double(l.Last().ns - restored) / kSec >= 10.0, "enriched %.1f s after the restore", double(l.Last().ns - restored) / kSec);
}

void TestFloorCeiling() {
  // The floor is where it starts, and the shed order from there.
  Rig a = ProdRig(3, 4);
  a.Run(1, 9.0);
  CHECK(a.c.Current() == (m::State{3, 0}) && a.c.Level() == 0, "starts at the floor");
  // C < F: nothing above the floor, ever.
  Rig b = ProdRig(3, 2);
  CHECK(b.Seconds(120.0, 9.0) < 0 && b.c.Current() == (m::State{3, 0}), "ceiling below the floor: no enrichment");
  Rig c = ProdRig(3, 3);
  CHECK(c.Seconds(120.0, 9.0) < 0, "ceiling = floor: none either");
  Rig d = ProdRig(2, 5);
  d.Seconds(60.0, 9.0);
  CHECK(d.c.Tier() == 5 && d.Changes() == 3, "floor 2, ceiling 5: up to tier 5 (%d changes)", d.Changes());
  // Live: the ceiling lowered under the tier in force moves it at once; the floor raised above the tier in force; VRS levels dropped.
  Rig e = ProdRig(1, 4);
  e.Seconds(35.0, 9.0);
  CHECK(e.c.Tier() == 4, "enriched to 4");
  e.p.tier_ceiling = 2;
  const m::Change ce = e.Frame(9.0);
  CHECK(ce.changed && ce.reason == m::Reason::kRange && ce.to_tier == 2 && ce.to_vrs == 0, "ceiling lowered to 2: tier 2");
  Rig f = ProdRig(1, 1);
  f.p.max_level = -1;
  f.Run(60, 20.0);
  CHECK(f.c.Current() == (m::State{1, 2}), "(1, 2)");
  f.p.tier_floor = 3;
  f.p.tier_ceiling = 3;
  const m::Change cf = f.Frame(20.0);
  CHECK(!cf.changed && f.c.Current() == (m::State{1, 2}), "floor raised: (1, 2) is still on the ladder (tier below the floor, VRS at its maximum)");
  Rig g = ProdRig(1, 1);
  g.Run(3, 9.0);
  g.p.tier_floor = 3;
  const m::Change cg = g.Frame(9.0);
  CHECK(cg.changed && cg.reason == m::Reason::kRange && cg.to_tier == 1 && cg.to_vrs == 2, "floor raised over a (1, 0) state: VRS to its maximum, never richer: (%d, %d)",
        cg.to_tier, cg.to_vrs);
  Rig h = ProdRig(1, 1);
  h.Run(60, 20.0);
  h.p.vrs_levels = 0;
  const m::Change ch = h.Frame(20.0);
  CHECK(ch.changed && ch.reason == m::Reason::kRange && h.c.Current() == (m::State{1, 0}), "VRS unsupported / dropped: (1, 0)");
  // The floor lowered under the tier in force: that tier is now an enriched one and stays.
  Rig i = ProdRig(3, 4);
  i.Run(3, 9.0);
  i.p.tier_floor = 1;
  CHECK(!i.Frame(9.0).changed && i.c.Current() == (m::State{3, 0}) && i.c.Level() == -2, "floor lowered: the tier is now rung -2");
}

// ---- pop hiding ---------------------------------------------------------------------------------------------------------

void TestPopEnrichWaits() {
  // Floor 2, ceiling 4, the real table: tier 3 pops. Enrichment into 3 is ready at 10 s and waits for a hiding moment, without a limit.
  auto ready = [] {
    Rig r = ProdRig(2, 4, /*pop_free=*/false);
    r.Seconds(11.0, 9.0);  // 2 -> 3: ready at 10 s
    return r;
  };
  Rig a = ready();
  CHECK(a.Changes() == 0 && a.waits.size() == 1 && a.c.Waiting() && a.waits[0].change.kind == m::Kind::kEnrich && a.waits[0].change.to_tier == 3 &&
            !a.waits[0].change.changed,
        "waits for a hiding moment (%zu waits, %d changes)", a.waits.size(), a.Changes());
  CHECK(a.Seconds(300.0, 9.0) < 0 && a.waits.size() == 1 && a.c.Tier() == 2 && a.c.Waiting(), "no time-out: 5 minutes later it still waits, one log line");
  // A pause: it goes at once.
  a.driving = false;
  const m::Change go = a.Frame(9.0);
  CHECK(go.changed && go.kind == m::Kind::kEnrich && go.to_tier == 3 && go.hide == m::Hide::kNotDriving && go.waited_s > 300.0 && !a.c.Waiting(),
        "a pause releases it (waited %.0f s)", go.waited_s);
  // A camera cut, one frame.
  Rig b = ready();
  b.Seconds(20.0, 9.0);
  b.cut = true;
  const m::Change cut = b.Frame(9.0);
  CHECK(cut.changed && cut.to_tier == 3 && cut.hide == m::Hide::kCut, "a camera cut releases it");
  // Speed over 90 km/h; 80 km/h does not.
  Rig c = ready();
  c.speed = 80.0;
  CHECK(c.Seconds(10.0, 9.0) < 0 && c.c.Tier() == 2, "80 km/h hides nothing");
  c.speed = 95.0;
  const m::Change fast = c.Frame(9.0);
  CHECK(fast.changed && fast.hide == m::Hide::kSpeed, "95 km/h hides it");
  // The first second after a change of the driving flag hides too (a pause that began and ended: it was released at the start).
  Rig d = ready();
  d.driving = false;
  d.Frame(9.0);  // goes here
  CHECK(d.c.Tier() == 3, "went at the pause");
  // A cut with speed unknown and no feed at all: only the flag hides. The conditions must still hold when it goes (a missed frame cancels).
  Rig e = ready();
  e.Frame(9.0, true);
  CHECK(!e.c.Waiting() && e.c.Tier() == 2, "a missed frame voids the waiting step");
  e.driving = false;
  CHECK(!e.Frame(9.0).changed, "and so a pause finds nothing to do");
  // Busy rising while it waits voids it too: the p90 no longer fits.
  Rig f = ready();
  f.Seconds(12.0, 14.5);  // 14.5 + 0.4 + 1.5 > 15
  CHECK(!f.c.Waiting(), "the headroom went: the plan is dropped");
}

void TestPopShedWaits() {
  // Floor 3, ceiling 3, V 2: the third shed (3 -> 2, across the popping tier 3) waits at most 1.5 s.
  Rig r = ProdRig(3, 3, /*pop_free=*/false);
  r.Run(20, 20.0);
  CHECK(r.c.Current() == (m::State{3, 2}), "VRS first: (3, 2)");
  const size_t before = r.events.size();
  int wait_frame = -1;
  for (int i = 0; i < 200 && r.events.size() == before; ++i) {
    r.Frame(20.0);
    if (wait_frame < 0 && !r.waits.empty()) wait_frame = r.frame;
  }
  CHECK(r.waits.size() == 1 && r.waits[0].change.kind == m::Kind::kShed && r.waits[0].change.to_tier == 2, "the shed across tier 3 waits");
  CHECK(r.events.size() == before + 1 && r.Last().change.hide == m::Hide::kTimeout && r.Last().change.to_tier == 2 &&
            r.Last().change.waited_s >= 1.5 && r.Last().change.waited_s < 1.6,
        "and goes after 1.5 s: waited %.2f s", r.Last().change.waited_s);
  // The next shed (2 -> 1) crosses tier 2, which does not pop: at once, 6 frames later.
  const size_t mid = r.events.size();
  r.Run(30, 20.0);
  CHECK(r.events.size() == mid + 1 && r.Last().change.to_tier == 1 && r.Last().change.hide == m::Hide::kNone && r.waits.size() == 1 &&
            r.Last().frame - r.events[mid - 1].frame <= 7,
        "2 -> 1 goes at once");
  // A missed frame sends the waiting shed at once.
  // (A shed across tier 3 from the floor 3 goes to a tier under the floor: the median must hold 30 GPU frames, so it starts waiting at
  // about frame 33 and the missed frame comes at frame 60.)
  Rig m1 = ProdRig(3, 3, false);
  m1.Run(20, 20.0);
  for (int i = 0; i < 40; ++i) m1.Frame(20.0);
  CHECK(m1.c.Waiting() && m1.c.Tier() == 3, "waiting");
  const m::Change miss = m1.Frame(20.0, true);
  CHECK(miss.changed && miss.hide == m::Hide::kMissed && miss.to_tier == 2 && miss.waited_s > 0.3 && miss.waited_s < 1.0, "a missed frame sends it (waited %.2f s)", miss.waited_s);
  // A pause sends it, and so does a camera cut; a calm frame drops it.
  Rig p1 = ProdRig(3, 3, false);
  p1.Run(20, 20.0);
  for (int i = 0; i < 20; ++i) p1.Frame(20.0);
  p1.driving = false;
  const m::Change pause = p1.Frame(20.0);
  CHECK(pause.changed && pause.hide == m::Hide::kNotDriving && pause.to_tier == 2, "a pause sends it");
  Rig p2 = ProdRig(3, 3, false);
  p2.Run(20, 20.0);
  for (int i = 0; i < 20; ++i) p2.Frame(20.0);
  p2.cut = true;
  CHECK(p2.Frame(20.0).hide == m::Hide::kCut, "a camera cut sends it");
  Rig p3 = ProdRig(3, 3, false);
  p3.Run(20, 20.0);
  for (int i = 0; i < 20; ++i) p3.Frame(20.0);
  CHECK(p3.c.Waiting(), "waiting");
  p3.Run(8, 9.0);
  CHECK(!p3.c.Waiting() && p3.c.Tier() == 3,
        "the load went: the plan is dropped at once (below the floor the tail window of the 20 ms samples does not hold it: only the held median counts), no shed");
  p3.Run(32, 9.0);
  CHECK(!p3.c.Waiting() && p3.c.Tier() == 3, "and it stays dropped, no shed");
  // A shed in the first second after driving resumes (a load just ended) hides: no wait. (With a short grace: the shed waits for it, the
  // 3rd fresh sample after 10 frames of grace; the default 60 frames of grace use the second up, see below.)
  Rig q = ProdRig(3, 3, false);
  q.p.max_level = 2;
  q.p.screen_grace_frames = 10;
  q.Run(30, 20.0);
  CHECK(q.c.Current() == (m::State{3, 2}), "at the cap (3, 2)");
  q.driving = false;
  q.Seconds(3.0, 20.0);
  q.p.max_level = -1;
  q.driving = true;
  m::Change resume;
  int resume_frame = -1;
  for (int i = 0; i < 80 && resume_frame < 0; ++i) {
    resume = q.Frame(20.0);
    if (resume.changed) resume_frame = i;
  }
  // (3 -> 2 goes under the floor 3: the median is over the line from the 3rd fresh sample, frame 12, and must hold 30 GPU frames: frame 42,
  // 0.7 s after the change of the flag, still inside its hiding second.)
  CHECK(resume_frame == 42 && resume.to_tier == 2 && resume.hide == m::Hide::kDrivingChange && resume.waited_s == 0.0,
        "the first second after a pause: goes at once (frame %d, after the grace, 3 samples and the 30 frames the median must hold)", resume_frame);
  // With the default 60 frames of grace the 1 s hiding moment after the change of the flag is over by the time a shed may go: it waits.
  Rig q2 = ProdRig(3, 3, false);
  q2.p.max_level = 2;
  q2.Run(30, 20.0);
  q2.driving = false;
  q2.Seconds(3.0, 20.0);
  q2.p.max_level = -1;
  q2.driving = true;
  for (int i = 0; i < 110; ++i) q2.Frame(20.0);  // the grace 60 + 3 samples + the 30 frames the median must hold (below the floor): ~frame 92
  CHECK(q2.c.Tier() == 3 && q2.c.Waiting() && q2.waits.size() == 1,
        "grace 60: the shed across tier 3 waits for a hiding moment (waits %zu)", q2.waits.size());
}

void TestPopRestoreWaits() {
  // Restoring from (2, 2) to (3, 2) crosses tier 3: it waits without a limit, and goes at a pause.
  Rig r = ProdRig(3, 3, false);
  r.Run(20, 20.0);
  for (int i = 0; i < 200 && r.c.Tier() == 3; ++i) r.Frame(20.0);  // the timed-out shed
  CHECK(r.c.Current() == (m::State{2, 2}), "at (2, 2)");
  const size_t waits = r.waits.size();
  r.Seconds(30.0, 9.0);
  CHECK(r.c.Current() == (m::State{2, 2}) && r.waits.size() == waits + 1 && r.waits.back().change.kind == m::Kind::kRestore && r.c.Waiting(),
        "the restore waits for a hiding moment");
  r.Seconds(120.0, 9.0);
  CHECK(r.c.Current() == (m::State{2, 2}), "no time-out for a restore");
  r.driving = false;
  const m::Change go = r.Frame(9.0);
  CHECK(go.changed && go.kind == m::Kind::kRestore && go.to_tier == 3 && go.hide == m::Hide::kNotDriving, "a pause releases the restore");
}

// ---- learning and the predicted cost ---------------------------------------------------------------------------------------

void TestPredictedCost() {
  Rig r = ProdRig(1, 4);
  // No measurement: the table's estimate; out of range: 0.
  CHECK(std::fabs(r.c.PredictedCostMs(2, r.p) - 0.3) < 1e-9 && std::fabs(r.c.PredictedCostMs(4, r.p) - 0.8) < 1e-9 && std::fabs(r.c.PredictedCostMs(5, r.p) - 1.0) < 1e-9 &&
            r.c.PredictedCostMs(9, r.p) == 0.0 && r.c.PredictedCostMs(-1, r.p) == 0.0 && !r.c.HasLearned(2) && r.c.LearnedMs(2) < 0,
        "the estimate");
  // A learned cost above the estimate wins, one below does not.
  TierPlant plant;
  plant.cost[2] = 1.2;  // the table says 0.3
  plant.cost[3] = 0.1;  // the table says 0.4
  DriveSeconds(r, plant, 40.0);
  CHECK(r.c.Tier() == 4, "enriched through 3 tiers: %d", r.c.Tier());
  CHECK(r.c.HasLearned(2) && std::fabs(r.c.LearnedMs(2) - 1.2) < 0.1 && std::fabs(r.c.PredictedCostMs(2, r.p) - r.c.LearnedMs(2)) < 1e-9,
        "tier 2: learned %.2f, predicted %.2f", r.c.LearnedMs(2), r.c.PredictedCostMs(2, r.p));
  CHECK(r.c.HasLearned(3) && std::fabs(r.c.LearnedMs(3) - 0.1) < 0.1 && std::fabs(r.c.PredictedCostMs(3, r.p) - 0.4) < 1e-9,
        "tier 3: learned %.2f, predicted %.2f (the estimate)", r.c.LearnedMs(3), r.c.PredictedCostMs(3, r.p));
  CHECK(r.c.LearnedCount(2) == 1 && r.c.LearnedCount(3) == 1, "counts");
}

void TestLearning() {
  // The cost of a tier is the difference of the busy p50 over 3 s after the step (the first 0.5 s skipped) and before it.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.7;
    plant.noise = 0.15;
    DriveSeconds(r, plant, 20.0);
    CHECK(r.learns.size() == 1 && r.c.Tier() == 2, "one measurement (%zu)", r.learns.size());
    if (!r.learns.empty()) {
      const m::Change& l = r.learns[0].change;
      const double after_step = double(r.learns[0].ns - r.events[0].ns) / kSec;
      CHECK(l.learn_tier == 2 && std::fabs(l.learn_delta_ms - 0.7) < 0.12 && after_step >= 3.5 && after_step < 3.6 && l.learn_n_before >= 170 && l.learn_n_after >= 170 &&
                std::fabs(l.learn_after_ms - l.learn_before_ms - 0.7) < 0.12,
            "tier 2: +%.2f ms (%.2f -> %.2f), %d / %d samples, %.2f s after the step", l.learn_delta_ms, l.learn_before_ms, l.learn_after_ms, l.learn_n_before,
            l.learn_n_after, after_step);
      CHECK(std::fabs(r.c.LearnedMs(2) - l.learn_delta_ms) < 1e-9, "the first sample is the value");
    }
  }
  // A cost below the estimate (0): learned 0, predicted stays the estimate; a negative one clamps to 0; one above 5 clamps to 5.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = -0.6;
    DriveSeconds(r, plant, 20.0);
    CHECK(r.c.HasLearned(2) && r.c.LearnedMs(2) == 0.0 && std::fabs(r.c.PredictedCostMs(2, r.p) - 0.3) < 1e-9, "negative cost: learned %.2f, predicted %.2f", r.c.LearnedMs(2),
          r.c.PredictedCostMs(2, r.p));
    Rig s = ProdRig(1, 2);
    TierPlant big;
    big.base = 6.0;
    big.cost[2] = 9.0;
    DriveSeconds(s, big, 20.0);
    CHECK(std::fabs(s.c.LearnedMs(2) - 5.0) < 1e-9 && std::fabs(s.c.PredictedCostMs(2, s.p) - 5.0) < 1e-9, "clamped to 5: %.2f", s.c.LearnedMs(2));
  }
  // EMA: a second measurement of the same tier (here by a pin that steps down) folds in with weight 0.4.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.7;
    DriveSeconds(r, plant, 20.0);
    CHECK(std::fabs(r.c.LearnedMs(2) - 0.7) < 0.1, "first: %.2f", r.c.LearnedMs(2));
    const double first = r.c.LearnedMs(2);
    plant.cost[2] = 0.3;  // the tier got cheaper
    r.p.force_level = 0;  // step down by a pin: a tier step, measured too (before - after)
    DriveSeconds(r, plant, 1.0);
    CHECK(r.c.Tier() == 1, "stepped down");
    // The state left was measured over samples taken at 0.7 (the lag line still carries the old state a few frames); wait it out.
    DriveSeconds(r, plant, 5.0);
    r.p.force_level = -1;
    CHECK(r.learns.size() == 2 && r.learns[1].change.learn_tier == 2, "second measurement");
    if (r.learns.size() == 2) {
      const double sample = r.learns[1].change.learn_delta_ms;
      CHECK(std::fabs(r.c.LearnedMs(2) - (first + 0.4 * (sample - first))) < 1e-9 && r.c.LearnedCount(2) == 2, "EMA 0.4: %.3f from %.3f and %.3f", r.c.LearnedMs(2), first, sample);
    }
  }
  // Samples of missed frames are left out: the after window holds 3 s of samples minus the missed ones.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.5;
    DriveSeconds(r, plant, 10.2);
    CHECK(r.c.Tier() == 2, "stepped");
    for (int i = 0; i < 300; ++i) {
      const bool miss = i == 40 || i == 100 || i == 160;  // far apart: no shed
      r.Frame(plant.Next(r.c.Current()), miss);
    }
    // Each missed frame also restarts nothing that matters here (no shed, no restore at this rung).
    CHECK(r.learns.size() == 1 && r.learns[0].change.learn_n_after <= 181 - 3 + 1 && r.learns[0].change.learn_n_after >= 170 - 3,
          "after-window samples %d with 3 missed frames out of ~180", r.learns.empty() ? -1 : r.learns[0].change.learn_n_after);
  }
  // A pause in the after window voids the measurement.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.5;
    DriveSeconds(r, plant, 10.0);
    DriveSeconds(r, plant, 1.0);
    r.driving = false;
    DriveSeconds(r, plant, 1.0);
    r.driving = true;
    DriveSeconds(r, plant, 10.0);
    CHECK(r.learns.empty() && !r.c.HasLearned(2), "a pause: nothing learned");
  }
  // Too few samples (15 a second): nothing learned, but the enrichment still runs (150 samples in its 10 s).
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.5;
    for (int i = 0; i < 60 * 20; ++i) {
      const double v = plant.Next(r.c.Current());
      r.Frame(i % 4 == 0 ? v : 0.0);
    }
    CHECK(r.c.Tier() == 2 && r.learns.empty(), "sparse samples: stepped (%d), nothing learned (%zu)", r.c.Tier(), r.learns.size());
  }
  // A step in the middle of a measurement voids it (the first step's cost is confounded).
  {
    Rig r = ProdRig(1, 3);
    TierPlant plant;
    r.p.enrich_s = 2.0;  // two steps 2.5 s apart: the second comes inside the first one's 3.5 s
    DriveSeconds(r, plant, 8.0);
    CHECK(r.c.Tier() == 3, "two quick steps");
    CHECK(r.learns.size() <= 2 && !r.c.HasLearned(2) , "the first step's measurement was cut short (tier 2 learned: %d)", int(r.c.HasLearned(2)));
  }
  // learn_s = 0 switches it off.
  {
    Rig r = ProdRig(1, 2);
    r.p.learn_s = 0.0;
    TierPlant plant;
    DriveSeconds(r, plant, 20.0);
    CHECK(r.c.Tier() == 2 && r.learns.empty(), "no learning with learn_s 0");
  }
  // A learned cost that is higher than the estimate keeps the enrichment out: p90 + learned + margin must fit.
  {
    Rig r = ProdRig(1, 3);
    TierPlant plant;
    plant.base = 11.5;
    plant.cost[2] = 0.3;
    plant.cost[3] = 0.4;
    DriveSeconds(r, plant, 60.0);
    CHECK(r.c.Tier() == 3, "11.5 base: tier 3 fits");
    Rig s = ProdRig(1, 3);
    TierPlant dear;
    dear.base = 11.5;
    dear.cost[2] = 1.3;  // learned 1.3 > est: busy at tier 2 = 12.8, tier 3 then needs 12.8 + 0.4 + 1.5 = 14.7: fits; raise the base
    dear.base = 12.0;
    DriveSeconds(s, dear, 60.0);
    // base 12.0: tier 2 needs 12.0 + 0.3 + 1.5 = 13.8 (fits), real 1.3 -> 13.3; tier 3: 13.3 + 0.4 + 1.5 = 15.2 > 15.0: stays at 2.
    CHECK(s.c.Tier() == 2, "at tier 2 (%d)", s.c.Tier());
  }
}

// ---- the back-off -----------------------------------------------------------------------------------------------------------

// A tier whose real cost the table underestimates: enrichment into it, shed back at once. Learning off, so only the back-off keeps the
// governor from trying again.
void TestBackoff() {
  Rig r = ProdRig(1, 2);
  r.p.learn_s = 0.0;
  TierPlant plant;
  plant.base = 11.0;
  plant.cost[2] = 6.0;  // est 0.3: 11 + 0.3 + 1.5 fits; real busy 17 > 15.6
  const double expected[] = {30, 60, 120, 240, 480, 600, 600};
  std::vector<double> enrich_at, shed_at, backoffs;
  size_t seen = 0;
  while (r.Seconds0() - 5.0 < 3200.0 && backoffs.size() < 7) {
    Drive(r, plant, 1);
    for (; seen < r.events.size(); ++seen) {
      const auto& ev = r.events[seen];
      if (ev.change.kind == m::Kind::kEnrich) enrich_at.push_back(double(ev.ns) / kSec);
      if (ev.change.kind == m::Kind::kShed) {
        shed_at.push_back(double(ev.ns) / kSec);
        backoffs.push_back(ev.change.backoff_s);
      }
    }
  }
  CHECK(backoffs.size() == 7, "seven bounces (%zu)", backoffs.size());
  for (size_t i = 0; i < backoffs.size() && i < 7; ++i) CHECK(std::fabs(backoffs[i] - expected[i]) < 1e-9, "back-off %zu: %.0f s, want %.0f", i, backoffs[i], expected[i]);
  // The next enrichment comes when the back-off ends, not before.
  for (size_t i = 1; i < enrich_at.size() && i <= shed_at.size() && i < 7; ++i) {
    const double gap = enrich_at[i] - shed_at[i - 1];
    CHECK(gap >= expected[i - 1] - 1e-6 && gap < expected[i - 1] + 1.0, "enrichment %zu came %.1f s after the shed (back-off %.0f s)", i, gap, expected[i - 1]);
  }
  // Entered by enrichment, shed within 30 s: bounce. Shed later: not.
  Rig s = ProdRig(1, 2);
  s.p.learn_s = 0.0;
  TierPlant light;
  light.base = 11.0;
  light.cost[2] = 0.3;
  DriveSeconds(s, light, 15.0);
  CHECK(s.c.Tier() == 2, "tier 2");
  DriveSeconds(s, light, 40.0);  // held for 40 s: no longer a fresh entry
  light.base = 16.5;             // the scene got heavy
  DriveSeconds(s, light, 3.0);
  CHECK(s.c.Tier() == 1 && s.Last().change.kind == m::Kind::kShed && s.Last().change.backoff_s == 0.0 && !s.c.BackedOff(2, s.now),
        "shed after 40 s at the tier: no back-off");
  // A tier entered by RESTORE (not enrichment) and shed again is the restore's business (probe bounce), not the back-off's.
  Rig t = ProdRig(2, 2);
  t.Run(60, 20.0);
  CHECK(t.c.Current() == (m::State{1, 2}), "(1, 2)");
  t.Seconds(4.2, 9.0);
  CHECK(t.c.Current() == (m::State{2, 2}) && t.Last().change.kind == m::Kind::kRestore, "restored to tier 2");
  t.Run(40, 20.0);  // 2 -> 1 is below the floor 2: the median must hold 30 GPU frames over the line
  CHECK(t.c.Tier() == 1 && t.Last().change.backoff_s == 0.0 && t.Last().change.bounce, "shed back: a probe bounce, no tier back-off");
}

void TestBackoffDecay() {
  Rig r = ProdRig(1, 2);
  r.p.learn_s = 0.0;
  TierPlant plant;
  plant.base = 11.0;
  plant.cost[2] = 6.0;
  // Three bounces: 30, 60, 120 s.
  size_t bounces = 0;
  while (bounces < 3 && r.Seconds0() < 1000.0) {
    const size_t n = r.events.size();
    Drive(r, plant, 1);
    if (r.events.size() > n && r.Last().change.kind == m::Kind::kShed && r.Last().change.backoff_s > 0) ++bounces;
  }
  CHECK(bounces == 3 && std::fabs(r.c.BackoffPeriodS(2, r.now) - 120.0) < 1e-9, "three bounces: period %.0f s", r.c.BackoffPeriodS(2, r.now));
  const int64_t until = r.Last().ns + 120'000'000'000;
  CHECK(r.c.BackedOff(2, until - 1'000'000) && !r.c.BackedOff(2, until + 1'000'000) && std::fabs(r.c.BackoffLeftS(2, r.Last().ns) - 120.0) < 1e-6, "barred for 120 s");
  // The doubling decays one step per period spent without a bounce: 120 -> 60 after 120 s, -> 30 after 60 s more, gone after 30 s more.
  CHECK(std::fabs(r.c.BackoffPeriodS(2, until + 119'000'000'000) - 120.0) < 1e-9 && std::fabs(r.c.BackoffPeriodS(2, until + 121'000'000'000) - 60.0) < 1e-9 &&
            std::fabs(r.c.BackoffPeriodS(2, until + 181'000'000'000) - 30.0) < 1e-9 && r.c.BackoffPeriodS(2, until + 211'000'000'000) == 0.0,
        "decay: %.0f %.0f %.0f %.0f", r.c.BackoffPeriodS(2, until + 119'000'000'000), r.c.BackoffPeriodS(2, until + 121'000'000'000),
        r.c.BackoffPeriodS(2, until + 181'000'000'000), r.c.BackoffPeriodS(2, until + 211'000'000'000));
  // Other tiers are untouched.
  CHECK(r.c.BackoffPeriodS(3, until) == 0.0 && !r.c.BackedOff(3, until - 1'000'000) && r.c.BackoffPeriodS(9, until) == 0.0 && !r.c.BackedOff(-1, until), "per tier");
  // After the decay a new bounce starts from 30 s again: the plant stays cheap meanwhile (the tier is entered and held), then gets dear.
  TierPlant cheap = plant;
  cheap.cost[2] = 0.3;
  while (r.c.Tier() == 1 && r.Seconds0() < 3000.0) Drive(r, cheap, 1);
  // The tier is entered when the back-off ends, 120 s after the last bounce; it then stays. Shed it within 30 s of entering: decayed? No:
  // at entry only 0 s of the decay have run, so the next bounce doubles the 120 s period (to 240 s).
  const double entered = double(r.Last().ns) / kSec;
  CHECK(r.Last().change.kind == m::Kind::kEnrich && r.c.Tier() == 2, "re-entered at %.0f s", entered);
  DriveSeconds(r, cheap, 5.0);
  plant.cost[2] = 6.0;
  Drive(r, plant, 90);
  CHECK(r.Last().change.kind == m::Kind::kShed && std::fabs(r.Last().change.backoff_s - 240.0) < 1e-9, "a bounce right after re-entry doubles: %.0f s", r.Last().change.backoff_s);
}

// ---- no oscillation ----------------------------------------------------------------------------------------------------

int TierSteps(const Rig& r) {
  int n = 0;
  for (const auto& e : r.events) n += e.change.from_tier != e.change.to_tier ? 1 : 0;
  return n;
}
int EntriesInto(const Rig& r, int tier) {
  int n = 0;
  for (const auto& e : r.events) n += e.change.to_tier == tier && e.change.from_tier < tier ? 1 : 0;
  return n;
}

void TestNoOscillation() {
  // The tier above the ceiling's neighbour is just over budget: floor 1, ceiling 5; the busy base 10 ms, the real costs 0.3 / 0.4 / 0.8 and
  // 5.0 for tier 5 (the table says 1.0), so that tier 5 lands at ~16.5 ms, over the 15.6 line: it is tried, shed, and (learning) never again.
  auto run = [](bool learn, int* tier5_entries, int* steps, std::vector<double>* tier5_times) {
    Rig r = ProdRig(1, 5);
    r.p.learn_s = learn ? 3.0 : 0.0;
    TierPlant plant;
    plant.base = 10.0;
    plant.cost[5] = 5.0;
    plant.noise = 0.3;
    DriveSeconds(r, plant, 600.0);
    *tier5_entries = EntriesInto(r, 5);
    *steps = TierSteps(r);
    for (const auto& e : r.events) {
      if (e.change.to_tier == 5 && e.change.from_tier < 5) tier5_times->push_back(double(e.ns) / kSec);
    }
    return r.c.Tier();
  };
  int entries = 0, steps = 0;
  std::vector<double> times;
  int tier = run(true, &entries, &steps, &times);
  CHECK(entries == 1 && steps <= 6 && tier == 4, "learning on: tier 5 tried %d time(s), %d tier steps in 10 minutes, ends at tier %d", entries, steps, tier);
  std::printf("no-oscillation, learning on: tier 5 entries %d, tier steps %d, ends at %d\n", entries, steps, tier);
  times.clear();
  tier = run(false, &entries, &steps, &times);
  CHECK(entries <= 5 && steps <= 14 && tier <= 5, "learning off, back-off only: tier 5 tried %d time(s), %d tier steps in 10 minutes", entries, steps);
  bool growing = times.size() >= 3;
  for (size_t i = 2; i < times.size(); ++i) growing = growing && (times[i] - times[i - 1]) > 1.5 * (times[i - 1] - times[i - 2]) - 1.0;
  CHECK(growing, "the gaps between the attempts keep growing");
  std::printf("no-oscillation, learning off: tier 5 entries %d, tier steps %d, attempts at", entries, steps);
  for (double t : times) std::printf(" %.0f", t);
  std::printf(" s\n");

  // An honest plant (real = estimated cost) just at the edge: base 12.0 fits tiers 2 and 3 and not 4 (p90 + 0.8 + 1.5 > 15.0): two steps,
  // then nothing for ten minutes.
  {
    Rig r = ProdRig(1, 4);
    TierPlant plant;
    plant.base = 12.0;
    plant.noise = 0.2;
    DriveSeconds(r, plant, 600.0);
    CHECK(r.c.Tier() == 3 && TierSteps(r) == 2 && r.Changes() == 2, "honest edge: tier %d after %d steps", r.c.Tier(), TierSteps(r));
  }
  // The same with the scene getting heavier and lighter every minute (10.5 .. 14.5 ms): the ladder follows without thrashing. Count the
  // steps in any 60 s stretch.
  {
    Rig r = ProdRig(1, 4);
    TierPlant plant;
    plant.noise = 0.3;
    const int frames = 60 * 60 * 10;
    for (int i = 0; i < frames; ++i) {
      plant.base = 12.5 + 2.0 * std::sin(double(i) / 3600.0 * 2.0 * 3.14159265358979);  // a 60 s period
      r.Frame(plant.Next(r.c.Current()));
    }
    int worst = 0;
    for (size_t i = 0; i < r.events.size(); ++i) {
      int n = 0;
      for (size_t j = i; j < r.events.size() && r.events[j].ns - r.events[i].ns <= 60'000'000'000; ++j) ++n;
      worst = std::max(worst, n);
    }
    std::printf("varying scene: %d steps in 10 minutes, at most %d within 60 s\n", r.Changes(), worst);
    CHECK(worst <= 8 && r.Changes() <= 60, "the ladder does not thrash on a varying scene: %d steps, %d in the worst minute", r.Changes(), worst);
  }
}

// ---- the floor on auto: the perf preset's floor is tier 2 -----------------------------------------------------------------

void TestAutoFloor() {
  // The preset names (mcla_gfx_preset): perf = tier 2 (perf + FXAA), perf+ and lowest = 1, quality and as shipped = 4, ultra = 5.
  CHECK(m::FloorForPreset("perf") == 2, "perf: %d", m::FloorForPreset("perf"));
  CHECK(m::FloorForPreset("Perf") == 2 && m::FloorForPreset("  PERF ") == 2, "case and blanks ignored");
  CHECK(m::FloorForPreset("perf+") == 1 && m::FloorForPreset("perfplus") == 1 && m::FloorForPreset("perf_plus") == 1 && m::FloorForPreset("perf-plus") == 1,
        "perf+ stays 1 (no far cascade, no impostors: a different picture, not perf)");
  CHECK(m::FloorForPreset("lowest") == 1, "lowest stays 1");
  CHECK(m::FloorForPreset("quality") == 4 && m::FloorForPreset("") == 4 && m::FloorForPreset("default") == 4 && m::FloorForPreset("none") == 4 &&
            m::FloorForPreset("shipped") == 4,
        "quality / as shipped: 4, unchanged");
  CHECK(m::FloorForPreset("ultra") == 5 && m::FloorForPreset("ULTRA") == 5, "ultra: 5, unchanged");
  CHECK(m::FloorForPreset("turbo") == 1, "an unknown name: 1, as before");
  // The tier set before the governor first ran wins over the preset (and is clamped); 0 / -1 = not set.
  CHECK(m::AutoFloor(3, "perf") == 3 && m::AutoFloor(1, "ultra") == 1 && m::AutoFloor(5, "") == 5 && m::AutoFloor(9, "perf") == 5, "a configured tier is the floor");
  CHECK(m::AutoFloor(-1, "perf") == 2 && m::AutoFloor(0, "perf") == 2 && m::AutoFloor(-1, "perf+") == 1 && m::AutoFloor(-1, "quality") == 4, "else the preset's");
}

// Floor 2 (the perf preset), ceiling 2 so that nothing enriches: the base is (2, 0), then VRS 1 and 2, then tier 1, then the hold.
void TestFloor2ShedOrder() {
  for (const bool pop_free : {true, false}) {
    Rig r = ProdRig(2, 2, pop_free);
    r.Run(1, 9.0);
    CHECK(r.c.Current() == (m::State{2, 0}) && r.c.Level() == 0 && m::Ladder::Of(r.p).shed == 3 && m::Ladder::Of(r.p).enrich == 0,
          "the base rung is (2, 0), three shed rungs: (2, 1) (2, 2) (1, 2)");
    CHECK(!r.c.HoldAllowed(r.p), "no hold at the base");
    // A load nothing fixes: every rung in turn, none skipped, the hold allowed only at the last (tier 1, VRS at its maximum).
    std::vector<bool> allowed_at;
    size_t seen = 0;
    for (int i = 0; i < 120; ++i) {
      r.Frame(20.0);
      if (r.events.size() != seen) {
        seen = r.events.size();
        allowed_at.push_back(r.c.HoldAllowed(r.p));
      }
    }
    const std::vector<Step> down = StepsOf(r);
    CHECK(down.size() == 3 && Is(down[0], 2, 1, m::Kind::kShed) && Is(down[1], 2, 2, m::Kind::kShed) && Is(down[2], 1, 2, m::Kind::kShed),
          "shed order from floor 2: VRS 1, VRS 2, then tier 1 (%zu steps, pop_free %d)", down.size(), pop_free);
    CHECK(r.c.Level() == 3 && r.c.Current() == (m::State{1, 2}) && allowed_at.size() == 3 && !allowed_at[0] && !allowed_at[1] && allowed_at[2],
          "the hold becomes allowed only at tier 1");
    CHECK(r.c.HoldAllowed(r.p) && r.Changes() == 3, "and nothing beyond it");
    // Calm: the reverse, tier 1 -> 2 first, then the VRS levels, 4 s apart.
    const size_t mid = r.events.size();
    r.Seconds(20.0, 9.0);
    const std::vector<Step> up = StepsOf(r, mid);
    CHECK(up.size() == 3 && Is(up[0], 2, 2, m::Kind::kRestore) && Is(up[1], 2, 1, m::Kind::kRestore) && Is(up[2], 2, 0, m::Kind::kRestore),
          "restore order: tier 2 back first, then VRS (%zu steps)", up.size());
    CHECK(r.c.Current() == (m::State{2, 0}) && r.Changes() == 6, "back at the base, and it stays (no enrichment: ceiling 2)");
    CHECK(r.Seconds(120.0, 9.0) < 0, "no step in two quiet minutes");
  }
  // With the default ceiling 4 the enriched tiers shed first: 4, 3, then VRS, then tier 1.
  Rig e = ProdRig(2, 4);
  e.Seconds(25.0, 9.0);
  CHECK(e.c.Current() == (m::State{4, 0}) && e.c.Level() == -2, "enriched to (4, 0) from the floor 2");
  CHECK(EntriesInto(e, 2) == 0, "tier 2 is never entered by enrichment: it is the base");
  const size_t before = e.events.size();
  e.Seconds(8.0, 20.0);
  const std::vector<Step> shed = StepsOf(e, before);
  CHECK(shed.size() == 5 && Is(shed[0], 3, 0, m::Kind::kShed) && Is(shed[1], 2, 0, m::Kind::kShed) && Is(shed[2], 2, 1, m::Kind::kShed) && Is(shed[3], 2, 2, m::Kind::kShed) &&
            Is(shed[4], 1, 2, m::Kind::kShed),
        "shed order from the enriched tiers: 4 -> 3 -> 2 (the base) -> VRS 1 -> VRS 2 -> tier 1 (%zu steps)", shed.size());
  // Tier 2 -> 1 is the step that costs the table's estimate, and the step the learning measures when it happens (a plant that really
  // takes 0.3 ms for it): the learned cost of tier 2 is then ~0.3, never negative, and the estimate stays the floor of the prediction.
  {
    Rig r = ProdRig(2, 2);
    TierPlant plant;
    plant.base = 16.5;  // over the line at (2, 0); VRS 2 takes 0.6; tier 1 takes 0.3 (tier 2's cost)
    DriveSeconds(r, plant, 8.0);
    CHECK(r.c.Current() == (m::State{1, 2}), "shed to tier 1");
    plant.base = 9.0;  // calm: tier 1 is restored after the 4 s, and that step is measured
    DriveSeconds(r, plant, 12.0);
    CHECK(r.c.Tier() == 2 && r.c.HasLearned(2) && std::fabs(r.c.LearnedMs(2) - 0.3) < 0.15, "learned cost of tier 2 over 1: %.2f ms (the plant's: 0.3)", r.c.LearnedMs(2));
    CHECK(r.c.PredictedCostMs(2, r.p) >= 0.3 - 1e-9, "the prediction never goes under the table's estimate");
  }
}

// The field problem (gate run): from a floor of 1 the governor enriched to tier 2 (FXAA on), shed it on a burst, barred it for 30 s and
// enriched again: the anti-aliasing flickered on and off. From floor 2 it is the base: a burst is absorbed by the enriched tiers and VRS.
void TestFloor2NoAaFlicker() {
  auto run = [](int floor, int* aa_flips, int* tier2_entries, int* backoffs, int* min_tier) {
    Rig r = ProdRig(floor, 4);
    TierPlant plant;
    plant.base = 9.0;
    plant.noise = 0.2;
    for (int i = 0; i < 60 * 60 * 6; ++i) {  // six minutes; every 40 s, 3 s of a heavier scene (+6.5 ms)
      const double t = double(i) / 60.0;
      plant.base = 9.0 + (std::fmod(t, 40.0) >= 27.0 && std::fmod(t, 40.0) < 30.0 ? 6.5 : 0.0);
      r.Frame(plant.Next(r.c.Current()));
    }
    *aa_flips = 0;
    *tier2_entries = 0;
    *backoffs = 0;
    *min_tier = 9;
    for (const auto& e : r.events) {
      const bool from_aa = e.change.from_tier >= 2, to_aa = e.change.to_tier >= 2;
      if (from_aa != to_aa) ++*aa_flips;
      if (e.change.to_tier == 2 && e.change.from_tier == 1) ++*tier2_entries;
      if (e.change.backoff_s > 0.0 && e.change.from_tier == 2) ++*backoffs;
      *min_tier = std::min(*min_tier, e.change.to_tier);
    }
  };
  int flips1 = 0, entries1 = 0, backoffs1 = 0, min1 = 0, flips2 = 0, entries2 = 0, backoffs2 = 0, min2 = 0;
  run(1, &flips1, &entries1, &backoffs1, &min1);
  run(2, &flips2, &entries2, &backoffs2, &min2);
  std::printf("AA flicker over 6 min of 3 s bursts every 40 s: floor 1: %d flips, %d entries into tier 2, %d back-offs of tier 2; floor 2: %d flips, min tier %d\n", flips1,
              entries1, backoffs1, flips2, min2);
  CHECK(flips1 >= 6 && entries1 >= 3, "the old floor 1 flickers the AA: %d flips, %d entries into tier 2", flips1, entries1);
  CHECK(flips2 == 0 && min2 >= 2 && entries2 == 0 && backoffs2 == 0, "floor 2: the AA never goes off in play (%d flips, lowest tier %d)", flips2, min2);
}

// The player pinned anti-aliasing (swap_post_effect by hand): tiers 1 and 2 are then the same picture and the 2 -> 1 step buys nothing.
// It must be harmless: the rung is the last one (nothing is shed after it), the learned cost comes out ~0 and does not lower the
// prediction under the table's estimate, the restore costs only its delay, and repeated loads never loop.
void TestFloor2PinnedAa() {
  Rig r = ProdRig(2, 2);
  TierPlant plant;
  plant.cost[2] = 0.0;  // tier 2 over tier 1 costs nothing
  plant.base = 16.7;
  DriveSeconds(r, plant, 30.0);
  CHECK(r.c.Current() == (m::State{1, 2}) && r.Changes() == 3 && r.c.HoldAllowed(r.p), "overload: down to the last rung, and 30 s later nothing more (%d changes)", r.Changes());
  plant.base = 9.0;
  DriveSeconds(r, plant, 30.0);
  CHECK(r.c.Current() == (m::State{2, 0}) && r.Changes() == 6, "calm: back to the base in 3 restores (%d changes)", r.Changes());
  CHECK(r.c.HasLearned(2) && r.c.LearnedMs(2) < 0.05, "the no-op step measured ~0 (%.3f ms)", r.c.LearnedMs(2));
  CHECK(std::fabs(r.c.PredictedCostMs(2, r.p) - r.p.tier_est_ms[2]) < 1e-9, "the prediction is still the table's estimate: %.2f ms", r.c.PredictedCostMs(2, r.p));
  // Five cycles of 10 s of overload and 30 s of calm: each cycle takes 3 steps down and 3 up, no more.
  Rig q = ProdRig(2, 2);
  TierPlant plant2;
  plant2.cost[2] = 0.0;
  for (int cycle = 0; cycle < 5; ++cycle) {
    plant2.base = 16.7;
    DriveSeconds(q, plant2, 10.0);
    plant2.base = 9.0;
    DriveSeconds(q, plant2, 30.0);
  }
  int bounces = 0;
  for (const auto& e : q.events) bounces += e.change.bounce ? 1 : 0;
  // (The 40 s period puts every shed to tier 1 within 30 s of the previous cycle's restore from it: since the below-the-floor window of a
  // probe bounce is 30 s, not 2 s, cycles 2..5 are bounces, the restore delay out of tier 1 doubles - and decays again 30 s after the
  // delay it lengthened, so it does not grow, and every cycle still ends at the base.)
  CHECK(q.Changes() == 30 && TierSteps(q) == 10 && bounces == 4 && q.c.Current() == (m::State{2, 0}),
        "five load cycles: %d changes (%d of them tier steps), %d bounces (cycles 2..5): nothing loops", q.Changes(), TierSteps(q), bounces);
  // And the same load with a floor of 1 and ceiling 4 (the plant's tier 2 free): the enrichment into tier 2 is tried once and stays.
  Rig f = ProdRig(1, 2);
  TierPlant plant3;
  plant3.cost[2] = 0.0;
  DriveSeconds(f, plant3, 120.0);
  CHECK(f.c.Tier() == 2 && f.Changes() == 1, "a free tier 2 enriched into at floor 1 stays: %d changes", f.Changes());
}

// ---- sustained overload below the floor, and spiky learning windows ----------------------------------------------------------

// The scene tour of the VISUALS runs (field run lk-w3cold2): the busy time is ~N(10, 0.6) ms and every ~6 s the time of day / weather snaps:
// one 18-19 ms frame; every 3rd time three of them within 24 frames, every 6th time a storm of twelve three frames apart. The tail rule fires
// on the clusters (and sheds VRS, which is invisible); the median of 4 never goes over the line. With the floor 2 (perf + FXAA) and VRS at
// its maximum the old governor then took tier 2 -> 1 (FXAA off) and the restore brought it back 4 s later: six times a minute.
struct TourResult {
  int min_tier = 9, tier_sheds = 0, vrs_sheds = 0, tail_sheds = 0, tier_steps = 0, snaps = 0;
  bool hold_ever = false;
};
TourResult RunSceneTour(int floor, int ceiling, int floor_shed_frames, double minutes) {
  Rig r = ProdRig(floor, ceiling);
  r.p.floor_shed_frames = floor_shed_frames;
  Rng rng;
  rng.s = 11;
  TourResult out;
  std::set<int> snap_at;
  int next_event = 300, event = 0;
  const int frames = int(minutes * 3600.0);
  for (int i = 0; i < frames; ++i) {
    if (i == next_event) {
      ++event;
      if (event % 6 == 0) {
        for (int k = 0; k < 12; ++k) snap_at.insert(i + 3 * k);
      } else if (event % 3 == 0) {
        for (int k = 0; k < 3; ++k) snap_at.insert(i + 8 * k);
      } else {
        snap_at.insert(i);
      }
      next_event = i + 330 + int(rng.U() * 60.0);  // 5.5 .. 6.5 s
    }
    double v = std::max(5.0, 10.0 + 0.6 * rng.N());
    if (snap_at.erase(i) > 0) {
      v = 18.0 + rng.U();
      ++out.snaps;
    }
    r.Frame(v);
    if (r.c.HoldAllowed(r.p)) out.hold_ever = true;
  }
  for (const auto& e : r.events) {
    const m::Change& c = e.change;
    out.min_tier = std::min(out.min_tier, c.to_tier);
    if (c.kind == m::Kind::kShed && c.to_tier < floor) ++out.tier_sheds;
    if (c.kind == m::Kind::kShed && c.to_tier == c.from_tier && c.to_vrs > c.from_vrs) ++out.vrs_sheds;
    if (c.kind == m::Kind::kShed && c.reason == m::Reason::kGpuTail) ++out.tail_sheds;
    if (c.from_tier != c.to_tier) ++out.tier_steps;
  }
  return out;
}

// A governor at the last VRS rung of the floor 2 (the next shed is tier 2 -> 1, below the floor), calm.
Rig AtVrsMaxFloor2() {
  Rig r = ProdRig(2, 2);
  r.p.force_level = 2;
  r.Run(10, 10.0);
  r.p.force_level = -1;
  r.Run(10, 10.0);
  return r;
}

void TestSceneTourBelowFloor() {
  // (a) the tour: the tail rule sheds VRS (it may), the tier never goes below the floor in 10 minutes, so the hold is never allowed either.
  for (const int ceiling : {2, 4}) {
    const TourResult t = RunSceneTour(2, ceiling, 30, 10.0);
    std::printf("scene tour, floor 2, ceiling %d, 10 min: %d snaps, %d VRS sheds (%d by the tail), %d tier sheds under the floor, lowest tier %d, %d tier steps\n", ceiling,
                t.snaps, t.vrs_sheds, t.tail_sheds, t.tier_sheds, t.min_tier, t.tier_steps);
    CHECK(t.snaps > 100, "the tour has its snaps (%d)", t.snaps);
    CHECK(t.tail_sheds >= 3 && (ceiling > 2 || t.vrs_sheds >= 3), "the tail rule still sheds on the clusters (%d tail sheds, %d VRS sheds)", t.tail_sheds, t.vrs_sheds);
    CHECK(t.tier_sheds == 0 && t.min_tier >= 2, "the tier never goes under the floor 2: %d sheds under it, lowest tier %d", t.tier_sheds, t.min_tier);
    CHECK(!t.hold_ever, "so the 60 -> 30 hold is never allowed on it");
  }
  // The pattern is a real one: with the rule off (floor_shed_frames 0, the old governor) the same tour takes the tier below the floor.
  const TourResult old = RunSceneTour(2, 2, 0, 10.0);
  std::printf("scene tour with the below-the-floor rule off: %d tier sheds under the floor, lowest tier %d\n", old.tier_sheds, old.min_tier);
  CHECK(old.tier_sheds >= 3 && old.min_tier == 1, "without the rule the storms switch FXAA off (%d sheds, lowest tier %d)", old.tier_sheds, old.min_tier);
  // Floor 1 has no rung below it: nothing changes there.
  const TourResult f1 = RunSceneTour(1, 1, 30, 5.0);
  CHECK(f1.min_tier >= 1 && f1.vrs_sheds >= 1, "floor 1: VRS sheds as before (%d)", f1.vrs_sheds);
}

void TestSustainedOverloadBelowFloor() {
  // (b) 18 ms for good: the whole ladder in about a second; the hold becomes allowed at the last rung only.
  for (const bool noisy : {false, true}) {
    for (const int floor : {2, 3, 4}) {
      Rig r = ProdRig(floor, floor);
      Rng rng;
      r.Run(5, 10.0);
      const int start = r.frame;
      std::vector<bool> allowed_at;
      size_t seen = 0;
      for (int i = 0; i < 180; ++i) {
        r.Frame(noisy ? std::max(16.5, 18.0 + 0.6 * rng.N()) : 18.0);
        if (r.events.size() != seen) {
          seen = r.events.size();
          allowed_at.push_back(r.c.HoldAllowed(r.p));
        }
      }
      const int rungs = 2 + floor - 1;
      const int last_frame = r.events.empty() ? -1 : r.Last().frame - start;
      CHECK(int(r.events.size()) == rungs && r.c.Current() == (m::State{1, 2}), "floor %d: %d sheds down to (1, 2) (%zu, now %d/%d)", floor, rungs, r.events.size(),
            r.c.Tier(), r.c.Vrs());
      CHECK(last_frame >= 30 && last_frame <= 60, "floor %d: tier 1 after %d frames (the median must hold 30 GPU frames first, then ~1 s at most)", floor, last_frame);
      CHECK(!allowed_at.empty() && allowed_at.back() && std::count(allowed_at.begin(), allowed_at.end(), true) == 1, "the hold is allowed only at the last rung");
      CHECK(r.c.HoldAllowed(r.p), "and stays allowed at 18 ms");
      // The first shed under the floor says so: the median held for >= 30 GPU frames.
      for (const auto& e : r.events) {
        if (e.change.from_vrs == 2 && e.change.to_tier < floor && e.change.from_tier == floor) {
          CHECK(e.change.below_floor && e.change.reason == m::Reason::kGpuMedian && e.change.sustained_frames >= 30 && e.change.sustained_frames < 45,
                "floor %d: the first tier shed was judged as below the floor: sustained %d GPU frames", floor, e.change.sustained_frames);
        }
        if (e.change.from_vrs < 2 && e.change.kind == m::Kind::kShed && e.change.from_tier == floor && e.change.to_tier == floor) {
          CHECK(!e.change.below_floor && e.change.sustained_frames == 0, "a VRS shed is not below the floor");
        }
      }
    }
  }
  // The sustained-overload rule is the median's: 20 frames over the line (a long snap, a short hitch) shed nothing below the floor.
  {
    Rig r = AtVrsMaxFloor2();
    const size_t before = r.events.size();
    r.Run(20, 18.0);
    CHECK(r.events.size() == before && r.c.Current() == (m::State{2, 2}) && r.c.OverloadFrames() > 10 && r.c.OverloadFrames() < 30, "20 frames over the line: no shed under the floor (%u frames held)",
          r.c.OverloadFrames());
    r.Run(60, 10.0);
    CHECK(r.events.size() == before && r.c.OverloadFrames() == 0, "and the calm ends the run");
  }
  // A sample whose median is back under the line in between ends the run: the 30 frames count from the first check after it.
  {
    Rig r = AtVrsMaxFloor2();
    const size_t before = r.events.size();
    r.Run(20, 18.0);
    r.Run(4, 10.0);
    r.Run(20, 18.0);
    CHECK(r.events.size() == before, "20 + break + 20 frames: not sustained (the break restarted the run)");
    const int first = r.Run(20, 18.0);
    CHECK(first >= 0 && r.c.Current() == (m::State{1, 2}) && r.Last().change.below_floor && r.Last().change.reason == m::Reason::kGpuMedian,
          "the run goes on: the shed comes %d frames later", first);
  }
  // The same overload on the first rungs is the old fast shed (above the floor nothing changed): VRS in 3 samples.
  {
    Rig r = ProdRig(2, 2);
    r.Run(5, 10.0);
    const int first = r.Run(10, 18.0);
    CHECK(first == 2 && !r.events.empty() && r.events[0].change.to_vrs == 1 && !r.events[0].change.below_floor, "VRS 1 after 3 samples (frame %d)", first);
  }
}

void TestBelowFloorMissedAndTail() {
  // (c) two missed frames in the last 30 still shed under the floor, with or without a GPU signal; one does not.
  {
    Rig r = AtVrsMaxFloor2();
    const size_t before = r.events.size();
    for (int i = 0; i < 60; ++i) r.Frame(10.0, i == 20);
    CHECK(r.events.size() == before, "one missed frame: nothing");
    m::Change c;
    for (int i = 0; i < 12; ++i) {
      const m::Change f = r.Frame(10.0, i == 0 || i == 10);
      if (f.changed) c = f;
    }
    CHECK(c.changed && c.to_tier == 1 && c.to_vrs == 2 && c.reason == m::Reason::kMissedFrames && c.below_floor && c.stat_n == 2 && c.sustained_frames == 0,
          "two missed frames in 30 (calm GPU): tier 2 -> 1 (reason %d, %d missed)", int(c.reason), c.stat_n);
  }
  {
    Rig r = ProdRig(2, 2);
    r.p.force_level = 2;
    r.Run(10, 0.0);  // no GPU samples at all
    r.p.force_level = -1;
    r.Run(10, 0.0);
    CHECK(r.c.Current() == (m::State{2, 2}) && !r.c.SignalLive(), "no GPU signal, at (2, 2)");
    r.Frame(0.0, true);
    r.Run(5, 0.0);
    const m::Change c = r.Frame(0.0, true);
    CHECK(c.changed && c.to_tier == 1 && c.reason == m::Reason::kMissedFrames && c.below_floor, "no GPU signal: two missed frames shed under the floor too");
  }
  // The tail rule alone never takes the ladder below the floor: 3, 5, 12 samples of 20 ms in 30 never shed tier 2 -> 1.
  for (const int spikes : {3, 5, 15}) {
    Rig r = AtVrsMaxFloor2();
    const size_t before = r.events.size();
    for (int i = 0; i < 120; ++i) r.Frame(i < 30 && i % (30 / spikes) == 0 ? 20.0 : 10.0);
    CHECK(r.events.size() == before && r.waits.empty(), "%d spikes in 30 samples: the tail rule does not shed under the floor", spikes);
  }
  // ... but it sheds above the floor (VRS) as ever.
  {
    Rig r = ProdRig(2, 2);
    r.Run(10, 10.0);
    for (int i = 0; i < 30; ++i) r.Frame(i % 8 == 0 ? 20.0 : 10.0);
    CHECK(r.Changes() == 1 && r.Last().change.reason == m::Reason::kGpuTail && r.Last().change.to_vrs == 1 && !r.Last().change.below_floor,
          "the tail rule sheds VRS 1 (%d changes)", r.Changes());
  }
  // floor_shed_frames = 0: the old rules everywhere (the tail below the floor, the median at once).
  {
    Rig r = AtVrsMaxFloor2();
    r.p.floor_shed_frames = 0;
    for (int i = 0; i < 30; ++i) r.Frame(i % 8 == 0 ? 20.0 : 10.0);
    CHECK(r.Last().change.reason == m::Reason::kGpuTail && r.c.Current() == (m::State{1, 2}) && !r.Last().change.below_floor, "off: the tail rule takes tier 1");
    Rig q = AtVrsMaxFloor2();
    q.p.floor_shed_frames = 0;
    const int first = q.Run(10, 18.0);
    CHECK(first == 2 && q.c.Current() == (m::State{1, 2}), "off: the median at once (frame %d)", first);
  }
  // The pin and the cap are not judged by any of it.
  {
    Rig r = AtVrsMaxFloor2();
    r.p.force_level = 3;
    r.Frame(10.0);
    CHECK(r.c.Current() == (m::State{1, 2}), "a pin goes below the floor at once");
    r.p.force_level = -1;
    r.p.max_level = 2;
    r.Frame(10.0);
    CHECK(r.c.Current() == (m::State{2, 2}), "the cap takes it back");
  }
}

void TestBelowFloorBounce() {
  // (2) A restore from tier 1 back up to the floor is the normal one (4 s); a shed to tier 1 again within 30 s of it, whatever came in
  // between (the VRS restores), is a bounce: the restore delay out of tier 1 doubles. A shed after 30 s is not.
  auto overload = [](Rig& r) {
    r.Run(5, 10.0);
    r.Run(60, 18.0);
  };
  auto calm_until_tier = [](Rig& r, int tier, double max_s) {
    const int64_t t0 = r.now;
    while (r.c.Tier() != tier && double(r.now - t0) / kSec < max_s) r.Frame(10.0);
  };
  for (const double calm_s : {20.0, 40.0}) {
    Rig r = ProdRig(2, 2);
    overload(r);
    CHECK(r.c.Current() == (m::State{1, 2}) && !r.Last().change.bounce && r.D(3) == 0, "the first shed to tier 1 is no bounce");
    const size_t shed1 = r.events.size() - 1;
    calm_until_tier(r, 2, 20.0);
    CHECK(r.Last().change.kind == m::Kind::kRestore && r.Last().change.from_tier == 1 && r.Last().change.to_tier == 2, "restored to tier 2");
    const double restore_gap = double(r.Last().ns - r.events[shed1].ns) / kSec;
    CHECK(restore_gap >= 4.0 && restore_gap < 6.0, "the first restore is the normal one: %.1f s after the shed", restore_gap);
    const int64_t restored_at = r.Last().ns;
    r.Seconds(calm_s, 10.0);  // the VRS restores come in between
    CHECK(r.c.Current() == (m::State{2, 0}), "back at the base");
    overload(r);
    const bool within = double(r.Last().ns - restored_at) / kSec <= 30.0;
    CHECK(r.c.Current() == (m::State{1, 2}) && r.Last().change.to_tier == 1, "tier 1 again");
    CHECK(within == (calm_s < 30.0), "the shed came %.1f s after the restore", double(r.Last().ns - restored_at) / kSec);
    if (within) {
      CHECK(r.Last().change.bounce && r.D(3) == 1 && std::fabs(r.Delay(3) - 8.0) < 1e-9, "within 30 s: a bounce, the delay out of tier 1 doubles (%d, %.1f s)", r.D(3), r.Delay(3));
      const size_t shed2 = r.events.size() - 1;
      // 6 s of calm are not enough now, the restore comes after the 8 s.
      r.Seconds(6.0, 10.0);
      CHECK(r.c.Tier() == 1, "no restore after 6 s");
      calm_until_tier(r, 2, 10.0);
      const double gap = double(r.Last().ns - r.events[shed2].ns) / kSec;
      CHECK(r.c.Tier() == 2 && gap >= 8.0 && gap < 10.0, "restored %.1f s after the shed (8 s)", gap);
    } else {
      CHECK(!r.Last().change.bounce && r.D(3) == 0 && std::fabs(r.Delay(3) - 4.0) < 1e-9, "after 30 s: no bounce, the delay stays 4 s");
    }
  }
  // The 2 s window above the floor is as it was: a VRS restore shed again after 10 s is no bounce.
  {
    Rig r = ProdRig(2, 2);
    r.p.force_level = 1;
    r.Run(5, 10.0);
    r.p.force_level = -1;
    r.Seconds(5.0, 10.0);
    CHECK(r.c.Current() == (m::State{2, 0}), "VRS restored");
    r.Seconds(10.0, 10.0);
    r.Run(5, 18.0);
    CHECK(r.c.Vrs() == 1 && !r.Last().change.bounce, "10 s later: no bounce above the floor");
  }
  // The restores of the rungs below a deeper floor are told apart: floor 3, (2, 2) and (1, 2) each have their own window.
  {
    Rig r = ProdRig(3, 3);
    overload(r);
    CHECK(r.c.Current() == (m::State{1, 2}), "floor 3, down to tier 1");
    calm_until_tier(r, 2, 20.0);
    calm_until_tier(r, 3, 20.0);
    CHECK(r.c.Tier() == 3, "back at the floor tier");
    r.Seconds(2.0, 10.0);
    overload(r);  // sheds (3,1) (3,2), then (2,2) and (1,2)
    int bounces = 0;
    for (const auto& e : r.events) bounces += e.change.bounce && e.change.kind == m::Kind::kShed ? 1 : 0;
    CHECK(r.c.Current() == (m::State{1, 2}) && bounces == 2, "both below-the-floor rungs bounced (%d)", bounces);
  }
}

// ---- learning windows with a spike -----------------------------------------------------------------------------------------------

struct LearnRun {
  int step_frame = -1;
  size_t learns = 0;
  int discards = 0;
  bool learned = false;
  double learned_ms = -1.0;
  bool discard_seen = false, discard_before = false;
  double discard_p50 = 0.0, discard_max = 0.0;
  int discard_tier = 0;
  double after_pin_ms = -1.0;
  int after_pin_n = 0;
};
// Floor 1, ceiling 2: the enrichment into tier 2 (a plant that really takes 0.7 ms for it) at ~10 s and its measurement; sample `spike_at`
// (a frame index; -1 = none) is `spike_ms` instead. Then a pin steps tier 2 -> 1, a second, clean measurement.
LearnRun RunLearn(int spike_at, double spike_ms) {
  Rig r = ProdRig(1, 2);
  TierPlant plant;
  plant.cost[2] = 0.7;
  plant.noise = 0.15;
  LearnRun out;
  for (int i = 0; i < 1500; ++i) {  // 25 s
    double v = plant.Next(r.c.Current());
    if (i == spike_at) v = spike_ms;
    r.Frame(v);
  }
  out.step_frame = r.events.empty() ? -1 : r.events[0].frame;
  out.learns = r.learns.size();
  out.discards = r.c.LearnDiscarded();
  out.learned = r.c.HasLearned(2);
  out.learned_ms = r.c.LearnedMs(2);
  for (const auto& e : r.discards) {
    if (!out.discard_seen) {
      out.discard_seen = true;
      out.discard_before = e.change.learn_discard_before;
      out.discard_p50 = e.change.learn_discard_p50_ms;
      out.discard_max = e.change.learn_discard_max_ms;
      out.discard_tier = e.change.learn_discard_tier;
    }
  }
  r.p.force_level = 0;  // tier 2 -> 1 by a pin: a clean measurement, folded in
  for (int i = 0; i < 480; ++i) r.Frame(plant.Next(r.c.Current()));
  out.after_pin_n = r.c.LearnedCount(2);
  out.after_pin_ms = r.c.LearnedMs(2);
  return out;
}

void TestLearningIgnoresSpikes() {
  // (d) a one-off spike (a scene snap, a hitch) in either window throws the measurement away; the EMA is only fed by clean ones.
  const LearnRun base = RunLearn(-1, 0.0);
  CHECK(base.step_frame > 0 && base.learns == 1 && base.discards == 0 && base.learned && std::fabs(base.learned_ms - 0.7) < 0.12,
        "no spike: learned +%.2f ms (%zu measurements, %d discarded)", base.learned_ms, base.learns, base.discards);
  CHECK(base.after_pin_n == 2 && std::fabs(base.after_pin_ms - 0.7) < 0.15, "and the pin's measurement folds in: %d samples, %.2f ms", base.after_pin_n, base.after_pin_ms);
  const LearnRun after = RunLearn(base.step_frame + 90, 19.0);  // 1.5 s after the step: in the window of 0.5 .. 3.5 s
  CHECK(after.step_frame == base.step_frame, "the spike does not move the step");
  CHECK(after.learns == 0 && after.discards == 1 && !after.learned && after.discard_seen && !after.discard_before && after.discard_tier == 2 && after.discard_max > 18.5 &&
            after.discard_p50 < 11.0,
        "a spike after the step: nothing learned (%zu), %d discarded (the window after: %.1f ms vs p50 %.1f)", after.learns, after.discards, after.discard_max, after.discard_p50);
  CHECK(after.after_pin_n == 1 && std::fabs(after.after_pin_ms - 0.7) < 0.15, "the next clean measurement is the first one folded: %d, %.2f ms", after.after_pin_n, after.after_pin_ms);
  const LearnRun before = RunLearn(base.step_frame - 90, 19.0);  // 1.5 s before: in the window of 3.1 .. 0.1 s before
  CHECK(before.step_frame == base.step_frame, "the spike does not move the step");
  CHECK(before.learns == 0 && before.discards == 1 && !before.learned && before.discard_seen && before.discard_before && before.discard_max > 18.5,
        "a spike before the step: nothing learned (%zu), %d discarded (the window before)", before.learns, before.discards);
  CHECK(before.after_pin_n == 1 && std::fabs(before.after_pin_ms - 0.7) < 0.15, "and the next clean one folds: %d, %.2f ms", before.after_pin_n, before.after_pin_ms);
  // A bump below 1.5x the median (13 ms on 9.7) is no spike: the measurement stands.
  const LearnRun mild = RunLearn(base.step_frame + 90, 13.0);
  CHECK(mild.learns == 1 && mild.discards == 0 && mild.learned && std::fabs(mild.learned_ms - 0.7) < 0.12, "a bump under 1.5x: measured (%zu, %d discarded, %.2f ms)", mild.learns,
        mild.discards, mild.learned_ms);
  // A spike outside both windows (the first 0.5 s after the step, which are skipped anyway, or later than 3.5 s) is nothing.
  const LearnRun outside = RunLearn(base.step_frame + 240, 19.0);
  CHECK(outside.learns == 1 && outside.discards == 0 && outside.learned, "a spike 4 s after the step: outside the windows (%zu, %d)", outside.learns, outside.discards);
  // The spike does not inflate what a level change would: a pre-snap window at 9.7 ms and a post-snap one at 11.1 (scene +1.4 ms) with the
  // snap's own spike in it: discarded, so the tier does not learn +1.4 ms for a change that was the scene's.
  {
    Rig r = ProdRig(1, 2);
    TierPlant plant;
    plant.cost[2] = 0.3;
    plant.noise = 0.15;
    for (int i = 0; i < 1500; ++i) {
      if (!r.events.empty()) {  // the scene snaps 1.5 s after the step: +1.4 ms for good, and one 19 ms frame
        const int since = i - r.events[0].frame;
        plant.base = since >= 90 ? 10.4 : 9.0;
      }
      double v = plant.Next(r.c.Current());
      if (!r.events.empty() && i == r.events[0].frame + 90) v = 19.0;
      r.Frame(v);
    }
    CHECK(r.c.LearnDiscarded() == 1 && !r.c.HasLearned(2) && r.c.PredictedCostMs(2, r.p) == r.p.tier_est_ms[2],
          "a snap in the after window: the tier does not learn the scene's +1.4 ms (discarded %d, learned %d)", r.c.LearnDiscarded(), int(r.c.HasLearned(2)));
  }
}

// ---- compile bursts ------------------------------------------------------------------------------------------------------------

constexpr int64_t kMs = 1'000'000;

m::CompileCounters Counters(uint32_t pipelines, uint32_t shaders, int32_t pending = 0) {
  m::CompileCounters c;
  c.valid = true;
  c.pipelines = pipelines;
  c.shaders = shaders;
  c.pending = pending;
  return c;
}

void TestCompileWatch() {
  const int64_t quiet = 500 * kMs;
  // No symbols: never a burst.
  {
    m::CompileWatch w;
    bool any = false;
    for (int i = 0; i < 100; ++i) {
      const auto r = w.Update(int64_t(i) * 16 * kMs, m::CompileCounters{}, quiet);
      any = any || r.active || r.started || r.ended;
    }
    CHECK(!any && !w.Active(), "no symbols: no signal");
  }
  // The first look only takes the values; growth starts a burst; it ends `quiet` after the last growth.
  {
    m::CompileWatch w;
    auto r = w.Update(0, Counters(120, 300), quiet);
    CHECK(!r.active && !r.started, "the counters' first value is not a burst");
    r = w.Update(100 * kMs, Counters(120, 300), quiet);
    CHECK(!r.active, "still");
    r = w.Update(1000 * kMs, Counters(123, 300), quiet);
    CHECK(r.active && r.started && !r.ended && r.pipelines == 3 && r.shaders == 0, "3 pipelines: a burst starts (%u)", r.pipelines);
    r = w.Update(1200 * kMs, Counters(124, 305), quiet);
    CHECK(r.active && !r.started && r.pipelines == 4 && r.shaders == 5, "growth inside it adds up: %u pipelines %u shaders", r.pipelines, r.shaders);
    r = w.Update(1600 * kMs, Counters(124, 305), quiet);
    CHECK(r.active, "400 ms after the last growth: still active");
    r = w.Update(1700 * kMs, Counters(124, 305), quiet);
    CHECK(r.active && !r.ended, "exactly 500 ms: still active");
    r = w.Update(1716 * kMs, Counters(124, 305), quiet);
    CHECK(!r.active && r.ended && r.pipelines == 4 && r.shaders == 5 && r.duration_ns == 700 * kMs, "516 ms: over; %u pipelines %u shaders over %.0f ms", r.pipelines,
          r.shaders, double(r.duration_ns) / 1e6);
    r = w.Update(1732 * kMs, Counters(124, 305), quiet);
    CHECK(!r.active && !r.ended && !w.Active(), "and it does not end twice");
    // Shaders alone start one too.
    r = w.Update(5000 * kMs, Counters(124, 306), quiet);
    CHECK(r.active && r.started && r.pipelines == 0 && r.shaders == 1, "a shader translation alone");
  }
  // An asynchronous creation in flight keeps it active past the quiet time, whatever the counters do.
  {
    m::CompileWatch w;
    w.Update(0, Counters(10, 10), quiet);
    auto r = w.Update(1000 * kMs, Counters(10, 10, 2), quiet);
    CHECK(r.active && r.started && r.pipelines == 0, "pending > 0 starts one with no growth");
    r = w.Update(6000 * kMs, Counters(10, 10, 1), quiet);
    CHECK(r.active && !r.ended, "5 s later, one still in flight");
    r = w.Update(6016 * kMs, Counters(10, 10, 0), quiet);
    CHECK(!r.active && r.ended && r.duration_ns == 5000 * kMs, "it ends at the first call with none in flight (%.0f ms)", double(r.duration_ns) / 1e6);
    // growth at 10 s, one in flight until 11 s: the quiet time is over at 10.5 s, the pending keeps it
    r = w.Update(10000 * kMs, Counters(11, 10, 1), quiet);
    CHECK(r.active && r.started, "growth with one in flight");
    r = w.Update(10900 * kMs, Counters(11, 10, 1), quiet);
    CHECK(r.active && !r.ended, "past the quiet time with one in flight: active");
    r = w.Update(11000 * kMs, Counters(11, 10, 0), quiet);
    CHECK(!r.active && r.ended, "none in flight and the quiet time over: ended");
  }
  // quiet 0: only the call that sees the growth (and pending) counts. A wrapped counter is growth. A negative quiet reads as 0.
  {
    m::CompileWatch w;
    w.Update(0, Counters(5, 5), 0);
    auto r = w.Update(100 * kMs, Counters(6, 5), 0);
    CHECK(r.active && r.started, "quiet 0: the call that sees the growth");
    r = w.Update(116 * kMs, Counters(6, 5), -50 * kMs);
    CHECK(!r.active && r.ended, "and not the next (a negative quiet is 0)");
    m::CompileWatch x;
    x.Update(0, Counters(0xFFFFFFFFu, 0), quiet);
    r = x.Update(16 * kMs, Counters(1, 0), quiet);
    CHECK(r.active && r.started && r.pipelines == 2, "a counter that wraps: growth of 2 (%u)", r.pipelines);
  }
  // The symbols vanish during a burst (the SDK was swapped): it ends.
  {
    m::CompileWatch w;
    w.Update(0, Counters(1, 1), quiet);
    w.Update(100 * kMs, Counters(2, 1), quiet);
    const auto r = w.Update(200 * kMs, m::CompileCounters{}, quiet);
    CHECK(!r.active && r.ended && !w.Active(), "no signal any more: over");
  }
}

void TestExcuseBudget() {
  const int64_t cap = 4'000 * kMs, window = 20'000 * kMs;
  const int64_t t0 = 100'000 * kMs;
  m::ExcuseBudget b;
  int excused = 0;
  int64_t charged = 0, total = 0;
  for (int i = 1; i <= 100; ++i) {  // 100 ms frames for 10 s
    if (b.Excuse(t0 + int64_t(i) * 100 * kMs, 100 * kMs, cap, window, &charged)) ++excused;
    total += charged;
  }
  CHECK(excused == 40 && total == cap && b.Used(t0 + 10'000 * kMs, window) == cap, "4 s of 100 ms frames: %d excused, %.2f s charged", excused, double(total) / 1e9);
  CHECK(!b.Excuse(t0 + 10'100 * kMs, 100 * kMs, cap, window, &charged) && charged == 0, "the next one is not");
  // The window slides: at 20.2 s the first frame (it ended at 0.1 s) is out: 100 ms of budget, one frame.
  CHECK(b.Used(t0 + 20'050 * kMs, window) == cap, "at 20.05 s the first charge (ended 0.1 s) is still inside");
  CHECK(b.Used(t0 + 20'150 * kMs, window) == cap - 100 * kMs, "at 20.15 s it is out: %.1f s", double(b.Used(t0 + 20'150 * kMs, window)) / 1e9);
  CHECK(b.Excuse(t0 + 20'150 * kMs, 100 * kMs, cap, window, &charged) && charged == 100 * kMs && !b.Excuse(t0 + 20'160 * kMs, 100 * kMs, cap, window), "one frame's worth is back");
  // Well after the window all of it is back.
  CHECK(b.Used(t0 + 60'000 * kMs, window) == 0 && b.Excuse(t0 + 60'000 * kMs, 100 * kMs, cap, window), "a minute later: empty");
  // A frame that does not fit whole is charged what is left (the cap is exact); 0 cap never excuses; a zero-length frame is excused free.
  m::ExcuseBudget c;
  CHECK(c.Excuse(1000 * kMs, 100 * kMs, 250 * kMs, window, &charged) && charged == 100 * kMs, "1st");
  CHECK(c.Excuse(1100 * kMs, 100 * kMs, 250 * kMs, window, &charged) && charged == 100 * kMs, "2nd");
  CHECK(c.Excuse(1200 * kMs, 100 * kMs, 250 * kMs, window, &charged) && charged == 50 * kMs, "3rd: what is left");
  CHECK(!c.Excuse(1300 * kMs, 100 * kMs, 250 * kMs, window, &charged) && charged == 0 && c.Used(1300 * kMs, window) == 250 * kMs, "then none: the cap is exact");
  m::ExcuseBudget z;
  CHECK(!z.Excuse(0, 100 * kMs, 0, window), "cap 0: never");
  CHECK(z.Excuse(0, 0, cap, window, &charged) && charged == 0, "a zero-length frame is excused and charged nothing");
  c.Reset();
  CHECK(c.Used(1300 * kMs, window) == 0, "reset");
}

// The governor's own rules: an excused frame's sample and missed flag are not fed into the shed rules.
void TestCompileExcuse() {
  // (1) A burst of samples over the line is excused; the same without the flag (or with the excuse off) sheds at once.
  {
    Rig r;
    r.Seconds(2.0, 10.0);
    r.compile = true;
    r.Seconds(2.0, 17.0);
    CHECK(r.Changes() == 0 && r.c.CompileExcused(), "2 s of 17 ms inside a burst: no shed");
    r.compile = false;
    r.Seconds(3.0, 10.0);
    CHECK(r.Changes() == 0 && r.c.Level() == 0 && !r.c.CompileExcused(), "and none after it");
    Rig a;
    a.Seconds(2.0, 10.0);
    CHECK(a.Seconds(2.0, 17.0) == 2, "the control, no burst: the 3rd sample sheds");
    Rig b;
    b.p.compile_excuse_s = 0.0;
    b.Seconds(2.0, 10.0);
    b.compile = true;
    CHECK(b.Seconds(2.0, 17.0) == 2 && !b.c.CompileExcused(), "excuse 0 (off): a burst is judged like any time");
  }
  // (2) Missed frames inside a burst: two of them do not shed (and the history does not fill up from them).
  {
    Rig r;
    r.Seconds(1.0, 10.0);
    r.compile = true;
    r.Seconds(2.0, 10.0, /*missed=*/true);
    CHECK(r.Changes() == 0, "every frame missed inside a burst: no shed");
    r.compile = false;
    r.Seconds(1.0, 10.0);
    CHECK(r.Changes() == 0, "nor after it");
    Rig a;
    a.Seconds(1.0, 10.0);
    CHECK(a.Seconds(2.0, 10.0, true) == 1 && a.Last().change.reason == m::Reason::kMissedFrames, "the control: the 2nd missed frame sheds");
  }
  // (3) What was recorded BEFORE the burst stays: two samples over the line, a burst of 17 ms (ignored), the third real one sheds.
  {
    Rig r;
    r.Seconds(1.0, 10.0);
    r.Run(2, 17.0);
    r.compile = true;
    r.Seconds(2.0, 17.0);
    r.compile = false;
    CHECK(r.Changes() == 0, "the burst's samples were not counted");
    const m::Change ch = r.Frame(17.0);
    CHECK(ch.changed && ch.reason == m::Reason::kGpuMedian && std::fabs(ch.stat_ms - 17.0) < 1e-9, "the third sample sheds, with the two from before the burst");
  }
  // (4) The bound: a 30 s burst of real overload still sheds, once the 4 s are used; the excused time never goes over 4 s in 20 s.
  {
    Rig r;
    r.Seconds(1.0, 10.0);
    r.compile = true;
    const int64_t t0 = r.now;
    int64_t worst = 0;
    int excused_after_use = 0;
    int first = -1;
    for (int i = 0; i < 30 * 60; ++i) {
      const m::Change ch = r.Frame(17.0);
      worst = std::max(worst, r.c.CompileExcusedNs(r.now));
      if (ch.changed && first < 0) first = i;
      const double t = double(r.now - t0) / kSec;
      if (t > 4.5 && t < 19.9 && r.c.CompileExcused()) ++excused_after_use;
    }
    const double first_s = double(first) * double(kFrameNs) / kSec;
    CHECK(first >= 0 && first_s >= 4.0 && first_s <= 4.3, "the first shed comes after the 4 s of excuse and 3 samples: %.2f s into the burst", first_s);
    CHECK(worst <= 4'000'000'000 && worst >= 3'900'000'000, "the excused time in any 20 s peaks at %.3f s (cap 4.0)", double(worst) / 1e9);
    CHECK(excused_after_use == 0, "no frame is excused between 4.5 s and 19.9 s of a continuous burst (%d)", excused_after_use);
    CHECK(r.c.Level() == 3 && r.c.HoldAllowed(r.p), "after 30 s of it the ladder is at its last rung and the hold is allowed");
    std::printf("compile burst, 30 s of 17 ms: first shed at %.2f s, level %d, excused peak %.3f s\n", first_s, r.c.Level(), double(worst) / 1e9);
  }
  // (5) The budget is a sliding 20 s: a second burst 10 s after the first has nothing, one 25 s after it has its 4 s again.
  for (const double gap : {5.0, 25.0}) {
    Rig r;
    r.Seconds(1.0, 10.0);
    r.compile = true;
    r.Seconds(6.0, 10.0);  // calm samples: the 4 s are used (up to ~4.0 s of the 6)
    r.compile = false;
    r.Seconds(gap, 10.0);
    r.compile = true;
    const int first = r.Seconds(8.0, 17.0);
    if (gap < 10.0) {
      CHECK(first >= 0 && first <= 3, "a burst 5 s after the first: no budget, the 3rd sample sheds (frame %d)", first);
    } else {
      const double s = double(first) * double(kFrameNs) / kSec;
      CHECK(first >= 0 && s >= 3.9 && s <= 4.3, "a burst 25 s after: the budget is back: first shed %.2f s into it", s);
    }
  }
  // (6) Only while driving, outside a grace: a burst during a pause or a load is neither excused nor charged.
  {
    Rig r;
    r.Seconds(1.0, 10.0);
    r.driving = false;
    r.compile = true;
    r.Seconds(10.0, 10.0);
    CHECK(r.c.CompileExcusedTotalNs() == 0 && !r.c.CompileExcused(), "a 10 s burst while not driving: nothing excused, nothing charged");
    r.driving = true;
    const int first = r.Seconds(10.0, 17.0);  // the grace (60 frames), then 4 s of excuse, then 3 samples
    CHECK(first >= 295 && first <= 310, "driving again: the grace, then the full 4 s of excuse, then the shed (frame %d)", first);
    CHECK(r.c.CompileExcusedTotalNs() <= 4'000'000'000 && r.c.CompileExcusedTotalNs() >= 3'900'000'000, "charged %.3f s", double(r.c.CompileExcusedTotalNs()) / 1e9);
  }
  // (7) A burst is not calm: it starts the quiet timer over, so the restore comes a full delay after it, not after the shed.
  {
    Rig r;
    r.SetAtLevel1();
    r.Seconds(3.0, 9.0);  // 1 s from a restore
    r.compile = true;
    r.Seconds(0.5, 9.0);
    r.compile = false;
    const int64_t burst_end = r.now;
    CHECK(r.Changes() == 1, "no restore inside the burst");
    r.Seconds(8.0, 9.0);
    CHECK(r.Changes() == 2 && r.Last().change.kind == m::Kind::kRestore, "restored");
    const double after = double(r.Last().ns - burst_end) / kSec;
    CHECK(after >= 3.9 && after <= 4.2, "the restore comes 4 s after the burst, not 1 s: %.2f s", after);
    Rig e = ProdRig(1, 2);
    e.Seconds(8.0, 9.0);
    e.compile = true;
    e.Seconds(0.5, 9.0);
    e.compile = false;
    CHECK(e.Seconds(9.8, 9.0) < 0 && e.Seconds(0.8, 9.0) >= 0 && e.c.Tier() == 2, "an enrichment waits its 10 s from the end of a burst too");
  }
  // (8) The grace is not charged, a pin and the cap are not held back, the summary counts it.
  {
    Rig r;
    r.Run(10, 10.0);
    r.c.TakeWindow(r.now);
    r.compile = true;
    r.Seconds(6.0, 10.0);
    const m::Window w = r.c.TakeWindow(r.now);
    CHECK(std::abs(int(w.compile_frames) - 240) <= 2 && std::fabs(double(w.compile_excused_ns) / 1e9 - 4.0) < 0.02 && std::abs(int(w.compile_denied) - 120) <= 2,
          "the summary: %u frames excused (%.2f s), %u judged anyway", w.compile_frames, double(w.compile_excused_ns) / 1e9, w.compile_denied);
    Rig f;
    f.Run(10, 10.0);
    f.compile = true;
    f.Frame(10.0);
    f.p.force_level = 2;
    const m::Change pin = f.Frame(10.0);
    CHECK(f.c.CompileExcused() && pin.changed && pin.reason == m::Reason::kForced && pin.to == 2, "a pin goes through a burst");
  }
}

// ---- closed loops against a toy GPU -----------------------------------------------------------------------------------------

// The GPU frame time of a level, published 3 frames late. `slack_clock`: when the nominal time is under 12 ms the GPU clocks
// down (x1.35 within ~7 s), and it needs ~0.2 s of busy frames to clock back up.
struct Plant {
  double load[6] = {19.0, 16.5, 14.0, 11.5, 10.0, 9.0};
  bool slack_clock = false;
  double clk = 1.0;
  double ms[3] = {0, 0, 0};  // the lag line

  double Next(int level, double noise) {
    const double nominal = load[std::min(5, std::max(0, level))];
    if (slack_clock) {
      const double target = nominal < 12.0 ? 1.35 : 1.0;
      const double rate = target > clk ? 0.35 / 7.0 : 0.35 / 0.2;  // per second
      clk += std::clamp(target - clk, -rate / 60.0, rate / 60.0);
    }
    const double now_ms = nominal * clk + noise;
    const double published = ms[2];
    ms[2] = ms[1];
    ms[1] = ms[0];
    ms[0] = now_ms;
    return published > 0 ? published : now_ms;
  }
};

void TestClosedLoopConverges() {
  // 19 ms at level 0: the loop climbs until the frame is under 14.8 and stays there (no restore: the next level up is not < 12.8).
  Rig r;
  r.p.vrs_levels = 5;
  r.p.max_level = 5;
  Plant plant;
  Rng rng;
  for (int i = 0; i < 60 * 120; ++i) {
    const double noise = (rng.U() * 2.0 - 1.0) * 0.4;
    r.Frame(plant.Next(r.c.Level(), noise));
  }
  CHECK(r.c.Level() == 2 && r.Changes() == 2, "converged at level 2 (14.0 ms) after 2 sheds: level %d changes %d", r.c.Level(), r.Changes());
  // A calm scene afterwards: the loop restores all the way down.
  Rig q;
  q.p.vrs_levels = 5;
  q.p.max_level = 5;
  Plant light;
  light.load[0] = 11.0;
  light.load[1] = 9.5;
  light.load[2] = 8.0;
  q.Run(20, 20.0);  // a heavy start pushed it up
  for (int i = 0; i < 60 * 40; ++i) q.Frame(light.Next(q.c.Level(), 0.0));
  CHECK(q.c.Level() == 0, "restored to 0 on a light scene: %d", q.c.Level());
  std::printf("closed loop: heavy scene settles at level 2 after %d changes; light scene back to level %d after %d changes\n", r.Changes(), q.c.Level(),
              q.Changes());
}

void TestClosedLoopDvfs() {
  // Level 1 (10 ms) lets the GPU clock down; the restore to level 0 (17 ms nominal, 23 ms with the slow clock) then fails and
  // sheds back within 2 s. Without the probe rule that would repeat every ~4 s (about 30 times in 130 s); with it the retries
  // double: 4, 8, 16, 32, 60 s.
  Rig r;
  r.p.max_level = 1;
  Plant plant;
  plant.slack_clock = true;
  plant.load[0] = 17.0;
  plant.load[1] = 9.0;  // 12.15 ms with the slow clock: restorable; level 0 at the slow clock is 23 ms
  r.Run(3, 20.0);  // start at level 1
  CHECK(r.c.Level() == 1, "start at 1");
  const int before = r.Changes();
  std::vector<double> restores_at;
  const int64_t t0 = r.now;
  for (int i = 0; i < 60 * 130; ++i) {
    const m::Change ch = r.Frame(plant.Next(r.c.Level(), 0.0));
    if (ch.changed && ch.reason == m::Reason::kRestore) restores_at.push_back(double(r.now - t0) / kSec);
  }
  int sheds = 0;
  for (size_t i = size_t(before); i < r.events.size(); ++i) sheds += r.events[i].change.to > r.events[i].change.from ? 1 : 0;
  CHECK(sheds >= 3 && sheds <= 6, "sheds in 130 s with the slack clock: %d (restores %zu)", sheds, restores_at.size());
  bool growing = restores_at.size() >= 4;
  for (size_t i = 2; i < restores_at.size(); ++i) {
    growing = growing && (restores_at[i] - restores_at[i - 1]) > 1.6 * (restores_at[i - 1] - restores_at[i - 2]);
  }
  CHECK(growing, "the gaps between restores keep growing");
  CHECK(r.D(1) >= 3, "doublings %d", r.D(1));
  std::printf("dvfs test: %d sheds, %zu restores in 130 s, restores at", sheds, restores_at.size());
  for (double t : restores_at) std::printf(" %.1f", t);
  std::printf(" s\n");
}

// T6: the budget follows the frame-rate target. The cvars hold the 60 fps thresholds (shed 15.6, restore 15.0, margin 1.5 ms); on
// the 1/120 grid (a cap above 60) they are scaled so that a 9.5 ms GPU frame is over budget at 120 and fine at 60.
void TestBudgetFollowsCap() {
  CHECK(m::BudgetScale(0) == 1.0 && m::BudgetScale(30) == 1.0 && m::BudgetScale(60) == 1.0, "no scale at 60 and below");
  CHECK(std::fabs(m::BudgetScale(120) - 0.5) < 1e-12, "0.5 at 120: %.6f", m::BudgetScale(120));
  CHECK(std::fabs(m::BudgetScale(90) - 2.0 / 3.0) < 1e-12, "2/3 at 90");
  CHECK(std::fabs(m::BudgetScale(240) - 0.25) < 1e-12 && std::fabs(m::BudgetScale(1000) - 0.25) < 1e-12, "0.25 floor");

  // The thresholds: the frame budget scales, the 0.7 ms paint outside the measured frame stays.
  CHECK(m::ScaleThresholdMs(15.6, 60) == 15.6 && m::ScaleThresholdMs(15.6, 0) == 15.6 && m::ScaleThresholdMs(15.6, 30) == 15.6,
        "bit for bit at 60 and below");
  CHECK(std::fabs(m::ScaleThresholdMs(15.6, 120) - 7.45) < 1e-9, "shed at 120: %.4f", m::ScaleThresholdMs(15.6, 120));
  CHECK(std::fabs(m::ScaleThresholdMs(15.0, 120) - 7.15) < 1e-9, "restore at 120: %.4f", m::ScaleThresholdMs(15.0, 120));
  CHECK(std::fabs(m::ScaleMarginMs(1.5, 120) - 0.75) < 1e-12, "margin at 120");
  CHECK(m::ScaleThresholdMs(15.6, 120) < 8.33 && m::ScaleThresholdMs(15.6, 120) > 8.33 - 1.5, "shed line just under the 120 fps frame");
  CHECK(m::ScaleThresholdMs(1.2, 120) >= 1.0, "never under 1 ms");
  // Shed stays above restore (the slack between them is the hysteresis).
  CHECK(m::ScaleThresholdMs(15.6, 120) > m::ScaleThresholdMs(15.0, 120), "shed above restore at 120");

  m::Params base;
  base.shed_ms = 15.6;
  base.restore_ms = 15.0;
  base.enrich_margin_ms = 1.5;
  m::Params at60 = base, at120 = base;
  m::ScaleParamsForCap(at60, 60);
  m::ScaleParamsForCap(at120, 120);
  CHECK(at60.shed_ms == 15.6 && at60.restore_ms == 15.0 && at60.enrich_margin_ms == 1.5, "Params unchanged at 60");
  CHECK(std::fabs(at120.shed_ms - 7.45) < 1e-9 && std::fabs(at120.restore_ms - 7.15) < 1e-9 &&
            std::fabs(at120.enrich_margin_ms - 0.75) < 1e-12,
        "Params at 120: %.3f %.3f %.3f", at120.shed_ms, at120.restore_ms, at120.enrich_margin_ms);

  // Closed loop, a GPU that needs 9.5 ms a frame: at 60 it is at 57 % of the budget and nothing happens; at 120 it is 14 % over
  // and the governor sheds within a second.
  {
    Rig r60 = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::ScaleParamsForCap(r60.p, 60);
    r60.Seconds(20, 9.5);
    CHECK(r60.Changes() == 0, "9.5 ms at a cap of 60 changes nothing: %d changes", r60.Changes());
    Rig r120 = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::ScaleParamsForCap(r120.p, 120);
    const int first = r120.Seconds(5, 9.5);
    CHECK(first >= 0 && first < 120, "9.5 ms at a cap of 120 sheds within 2 s: first change at frame %d", first);
    CHECK(r120.Changes() >= 1 && r120.events[0].change.reason != m::Reason::kRestore, "the first change is a shed");
  }
  // A GPU that needs 7.0 ms is inside the 120 budget and above the restore line 7.15?  No: 7.0 < 7.15 (restore) < 7.45 (shed).
  // 7.0 ms: no shed; 7.3 ms: no shed and nothing restored (the dead band); 6.0 ms after a shed: a restore.
  {
    Rig r = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::ScaleParamsForCap(r.p, 120);
    r.Seconds(10, 7.0);
    CHECK(r.Changes() == 0, "7.0 ms at 120: nothing to do, %d changes", r.Changes());
    Rig dead = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::ScaleParamsForCap(dead.p, 120);
    dead.Seconds(10, 7.3);
    CHECK(dead.Changes() == 0, "7.3 ms at 120 sits in the dead band: %d changes", dead.Changes());
    Rig up = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::ScaleParamsForCap(up.p, 120);
    up.Seconds(3, 9.5);  // sheds
    const int shed_changes = up.Changes();
    CHECK(shed_changes >= 1, "shed first");
    up.Seconds(40, 6.0);  // headroom: restores
    int restores = 0;
    for (const Rig::Event& e : up.events) restores += e.change.reason == m::Reason::kRestore ? 1 : 0;
    CHECK(restores >= 1, "6.0 ms at 120 restores a rung after a shed: %d restores", restores);
  }
  // The same 9.5 ms stream judged by the 60 fps thresholds while the world is held at 60 (the 120 -> 60 tier): back to nothing.
  {
    Rig tier = ProdRig(1, 1)  /* no enrichment: ceiling = floor */;
    m::Params p = tier.p;
    m::ScaleParamsForCap(p, 60);
    CHECK(p.shed_ms == tier.p.shed_ms, "a world at 60 uses the 60 fps thresholds");
  }
}

}  // namespace

int main() {
  TestHistogram();
  TestTimedWindow();
  TestLadderMap();
  TestCameraMotion();
  TestShedLatency();
  TestShedSpacing();
  TestTailRule();
  TestMissedFrames();
  TestFieldBimodal();
  TestCleanSpike();
  TestRestoreDelay();
  TestRestoreNeedsP90();
  TestMissedFrameResetsRestore();
  TestRestoreWithoutGpuSignal();
  TestBounce();
  TestBounceWindowAndDecay();
  TestNotDriving();
  TestScreenGrace();
  TestCapAndForce();
  TestHoldAllowed();
  TestHoldAllowedLadder();
  TestWindow();
  TestShedAndRestoreOrder();
  TestEnrichConditions();
  TestFloorCeiling();
  TestPopEnrichWaits();
  TestPopShedWaits();
  TestPopRestoreWaits();
  TestPredictedCost();
  TestLearning();
  TestBackoff();
  TestBackoffDecay();
  TestNoOscillation();
  TestAutoFloor();
  TestFloor2ShedOrder();
  TestFloor2NoAaFlicker();
  TestFloor2PinnedAa();
  TestSceneTourBelowFloor();
  TestSustainedOverloadBelowFloor();
  TestBelowFloorMissedAndTail();
  TestBelowFloorBounce();
  TestLearningIgnoresSpikes();
  TestCompileWatch();
  TestExcuseBudget();
  TestCompileExcuse();
  TestClosedLoopConverges();
  TestClosedLoopDvfs();
  TestBudgetFollowsCap();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
