// Unit test of the GPU frame governor's pure logic (mcla/hooks/mcla_gov_math.h, LOCK60 A5): the level table parser (syntax,
// aliases, VRS unsupported / clamped, equal levels skipped) and the control law (shed latency and spacing, the missed-frame
// rules and the no-signal fallback, the restore delay and its p90, probe bounces and their decay, no restore while not
// driving, the cap, the pin, the rich levels, HoldAllowed, the summary window) plus a closed-loop run against a toy GPU with
// a DVFS-like slack response. Standalone: no SDK, no game, no clock.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_gov_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I mcla/hooks mcla/tests/test_gov.cpp -o <scratch>/test_gov && <scratch>/test_gov
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// A controller, its parameters and a clock; one call = one engine frame.
struct Rig {
  m::Controller c;
  m::Params p;
  int64_t now = 5'000'000'000;
  int frame = 0;
  bool driving = true;
  uint32_t seq_delta = 1;  // GPU frames published per sample
  struct Event {
    int frame;
    int64_t ns;
    m::Change change;
  };
  std::vector<Event> events;

  Rig() { p.max_level = 3; }

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
    const m::Change ch = c.Step(in, p);
    if (ch.changed) events.push_back({frame, now, ch});
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
  // Level 1 with the cap there: three samples over the line shed, the cap holds it.
  void SetAtLevel1() {
    p.max_level = 1;
    Run(3, 16.0);
  }
  int Changes() const { return int(events.size()); }
  const Event& Last() const {
    static const Event none{};
    return events.empty() ? none : events.back();
  }
};

// ---- the table ---------------------------------------------------------------------------------------------------

const char* kDefaultTable =
    "vrs=1;vrs=2;vrs=3;vrs=3,mcla_gfx_env_interval=4,mcla_gfx_far_cascades_interval=3;"
    "vrs=4,mcla_gfx_env_interval=4,mcla_gfx_far_cascades_interval=3,mcla_gfx_shadow_dist=0.45,mcla_gfx_city_lod=0.55";

void TestTableDefault() {
  const m::ParsedTable t = m::ParseLevelTable(kDefaultTable, 4);
  CHECK(t.levels.size() == 5 && t.skipped.empty() && t.source_levels == 5 && t.warnings.empty(), "levels %zu skipped %zu", t.levels.size(),
        t.skipped.size());
  CHECK(t.levels[0].vrs == 1 && t.levels[0].cvars.empty(), "level 1 is vrs=1");
  CHECK(t.levels[3].vrs == 3 && t.levels[3].cvars.size() == 2 && t.levels[3].cvars[0].name == "mcla_gfx_env_interval" &&
            t.levels[3].cvars[0].value == "4" && t.levels[3].cvars[1].name == "mcla_gfx_far_cascades_interval",
        "level 4");
  CHECK(t.levels[4].vrs == 4 && t.levels[4].cvars.size() == 4 && t.levels[4].Find("mcla_gfx_city_lod") &&
            t.levels[4].Find("mcla_gfx_city_lod")->value == "0.55",
        "level 5");
  CHECK(m::Describe(t.levels[4]) ==
            "vrs=4, mcla_gfx_env_interval=4, mcla_gfx_far_cascades_interval=3, mcla_gfx_shadow_dist=0.45, mcla_gfx_city_lod=0.55",
        "describe: %s", m::Describe(t.levels[4]).c_str());
  CHECK(m::Describe(m::LevelSettings()) == "(base)", "describe of nothing");
}

void TestTableVrsUnsupported() {
  // No VRS on this GPU: the three vrs-only levels are copies of level 0 and are skipped; the later levels lose the vrs entry.
  const m::ParsedTable t = m::ParseLevelTable(kDefaultTable, 0);
  CHECK(t.levels.size() == 2, "levels %zu", t.levels.size());
  CHECK(t.skipped.size() == 3 && t.skipped[0] == 1 && t.skipped[1] == 2 && t.skipped[2] == 3, "skipped 1 2 3");
  CHECK(t.vrs_ignored == 5, "vrs entries dropped %d", t.vrs_ignored);
  CHECK(t.levels[0].vrs == -1 && t.levels[0].cvars.size() == 2 && t.levels[1].cvars.size() == 4, "kept levels");
  CHECK(m::Describe(t.levels[0]) == "mcla_gfx_env_interval=4, mcla_gfx_far_cascades_interval=3", "describe %s", m::Describe(t.levels[0]).c_str());
}

void TestTableVrsClamped() {
  // Two VRS levels: vrs=3 is clamped to 2, which equals level 2: skipped; the vrs=4 level clamps to 2 as well.
  const m::ParsedTable t = m::ParseLevelTable(kDefaultTable, 2);
  CHECK(t.levels.size() == 4 && t.skipped.size() == 1 && t.skipped[0] == 3, "levels %zu skipped %zu", t.levels.size(), t.skipped.size());
  CHECK(t.levels[0].vrs == 1 && t.levels[1].vrs == 2 && t.levels[2].vrs == 2 && t.levels[2].cvars.size() == 2 && t.levels[3].vrs == 2,
        "clamped vrs");
  CHECK(t.vrs_ignored == 0, "nothing dropped");
}

void TestTableSyntax() {
  // Whitespace, a trailing ';', the aliases of the harness ('|' '+' ':'), explicit vrs=0 and a negative vrs.
  m::ParsedTable t = m::ParseLevelTable("  vrs = 1 ; mcla_a = 2 , mcla_b=0.5 ;  ", 3);
  CHECK(t.levels.size() == 2 && t.levels[0].vrs == 1 && t.levels[1].cvars.size() == 2 && t.levels[1].cvars[1].value == "0.5" &&
            t.warnings.empty(),
        "spaces: %zu levels", t.levels.size());
  t = m::ParseLevelTable("vrs:1|vrs:2+mcla_a:7", 3);
  CHECK(t.levels.size() == 2 && t.levels[1].vrs == 2 && t.levels[1].cvars.size() == 1 && t.levels[1].cvars[0].name == "mcla_a" &&
            t.levels[1].cvars[0].value == "7",
        "aliases");
  t = m::ParseLevelTable("vrs=0;vrs=-1,mcla_a=1", 3);
  CHECK(t.levels.size() == 2 && t.levels[0].vrs == 0 && t.levels[1].vrs == -1, "vrs=0 is a forced full rate (kept), vrs=-1 is not governed");
  // Empty text: no levels. Garbage: ignored with a warning, never a crash.
  CHECK(m::ParseLevelTable("", 3).levels.empty() && m::ParseLevelTable("   ", 3).levels.empty() && m::ParseLevelTable(";", 3).levels.empty() &&
            m::ParseLevelTable("", 3).source_levels == 0 && m::ParseLevelTable(";", 3).skipped.size() == 1,
        "empty text");
  t = m::ParseLevelTable("vrs=x;mcla_a;=3;mcla b=1;mcla_c=;mcla_ok=1", 3);
  CHECK(t.levels.size() == 1 && t.levels[0].cvars.size() == 1 && t.levels[0].cvars[0].name == "mcla_ok" && t.warnings.size() == 5,
        "garbage: %zu levels %zu warnings", t.levels.size(), t.warnings.size());
  CHECK(t.source_levels == 6 && t.skipped.size() == 5, "five garbage levels were empty = equal to level 0: skipped (%zu)", t.skipped.size());
  // Duplicate name inside a level: the last value counts.
  t = m::ParseLevelTable("mcla_a=1,mcla_a=2", 0);
  CHECK(t.levels.size() == 1 && t.levels[0].cvars.size() == 1 && t.levels[0].cvars[0].value == "2" && t.warnings.size() == 1, "duplicate");
}

void TestTableEqualLevels() {
  // Equal to the previous kept level: skipped, whatever the spelling of the number or the order of the entries.
  const m::ParsedTable t = m::ParseLevelTable("mcla_a=0.45,mcla_b=4;mcla_b=4.0,mcla_a=0.450;mcla_a=0.5,mcla_b=4;mcla_a=0.5,mcla_b=4", 0);
  CHECK(t.levels.size() == 2 && t.skipped.size() == 2 && t.skipped[0] == 2 && t.skipped[1] == 4, "levels %zu skipped %zu", t.levels.size(), t.skipped.size());
  // A level that goes back to a former one is not equal to its PREDECESSOR: kept.
  const m::ParsedTable u = m::ParseLevelTable("mcla_a=1;mcla_a=2;mcla_a=1", 0);
  CHECK(u.levels.size() == 3 && u.skipped.empty(), "back to a former value");
  // The first level equal to level 0 (nothing): skipped.
  const m::ParsedTable v = m::ParseLevelTable(";mcla_a=1", 0);
  CHECK(v.levels.size() == 1 && v.skipped.size() == 1 && v.skipped[0] == 1, "an empty first level is level 0 again");
  // Case and text values.
  CHECK(m::ValuesEqual("true", "True") && !m::ValuesEqual("true", "false") && m::ValuesEqual("2", "2.0") && !m::ValuesEqual("2", "3"), "values");
}

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

// ---- the control law ---------------------------------------------------------------------------------------------

void TestShedLatency() {
  Rig r;
  // Under the line: nothing, however long.
  CHECK(r.Run(600, 14.0) < 0 && r.c.Level() == 0, "14.0 ms is under 14.8");
  // Over it: the median of the last 4 needs at least 3 samples of the burst (a median of 4 with the older 14.0 ones: the
  // third 16 ms sample makes [14, 16, 16, 16] -> 16).
  Rig s;
  const int first = s.Run(100, 16.0);
  CHECK(first == 2 && s.c.Level() >= 1, "first shed on the 3rd sample (the median needs 3), got %d", first);
  CHECK(s.events[0].change.reason == m::Reason::kGpuMedian && s.events[0].change.from == 0 && s.events[0].change.to == 1 &&
            std::fabs(s.events[0].change.stat_ms - 16.0) < 1e-9 && s.events[0].change.stat_n == 3 && std::string(s.events[0].change.stat_name) == "p50",
        "reason and statistic of the first shed");
  // A rise from a calm run: 12 ms then 16 ms: [12,12,12,16] median 12, [12,12,16,16] median 14 < 14.8, [12,16,16,16] median 16: the 3rd sample.
  Rig t;
  t.Run(100, 12.0);
  const int rise = t.Run(10, 16.0);
  CHECK(rise == 2, "rise from 12 to 16 ms sheds on the 3rd high sample, got %d", rise);
  // One spike among calm frames does not shed (the median of 4 = the mean of the middle two); two 30 ms frames of the last
  // four do: [12, 12, 30, 30] -> 21 ms.
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

void TestMissedFrames() {
  // With a GPU signal: a missed frame sheds when the last sample is within 1 ms of the line, not otherwise.
  Rig a;
  a.Run(60, 14.0);
  const m::Change hit = a.Frame(14.0, true);
  CHECK(hit.changed && a.c.Level() == 1 && hit.reason == m::Reason::kMissedAtBudget && std::string(hit.stat_name) == "last" && hit.missed == 1,
        "14.0 ms (> 13.8) with a missed frame sheds");
  Rig b;
  b.Run(60, 13.0);
  b.Frame(13.0, true);
  b.Frame(13.0, true);
  CHECK(b.c.Level() == 0, "13.0 ms (under 13.8) with missed frames: no shed while the GPU is fine");
  // Without a GPU signal (no sample for 1 s): 2 missed frames within 8 engine frames shed; spread further apart they do not.
  Rig c;
  c.Run(30, 0.0);
  CHECK(!c.c.SignalLive(), "no signal");
  CHECK(!c.Frame(0, true).changed && c.c.Level() == 0, "one missed frame");
  c.Run(7, 0.0);  // 7 on-time frames between
  CHECK(!c.Frame(0, true).changed && c.c.Level() == 0, "two missed frames 8 frames apart: the first fell out of the window");
  const m::Change ch = c.Frame(0, true);  // the very next frame: 2 of the last 2
  CHECK(ch.changed && ch.reason == m::Reason::kMissedFrames && ch.to == 1 && !ch.signal_gpu && ch.missed == 2, "two missed frames in a row sheds");
  // 7 frames apart is within the 8: sheds.
  Rig d;
  d.Run(30, 0.0);
  d.Frame(0, true);
  d.Run(6, 0.0);
  CHECK(d.Frame(0, true).changed, "two missed frames 7 apart (inside the last 8) shed");
  // The signal lapse: after 1 s without a sample the fallback is in force and a live signal returns with the next sample.
  Rig e;
  e.Run(30, 12.0);
  CHECK(e.c.SignalLive(), "live");
  e.Seconds(1.2, 0.0);
  CHECK(!e.c.SignalLive(), "lapsed after 1 s");
  e.Frame(12.0);
  CHECK(e.c.SignalLive(), "back");
}

void TestRestoreDelay() {
  Rig r;
  r.SetAtLevel1();
  CHECK(r.c.Level() == 1, "level 1");
  const int64_t t0 = r.events.back().ns;
  // 11 ms from here on: the restore comes after restore_s = 4 s.
  const int first = r.Seconds(8.0, 11.0);
  CHECK(first >= 0 && r.c.Level() == 0, "restored");
  const double waited = double(r.Last().ns - t0) / kSec;
  CHECK(r.Last().change.reason == m::Reason::kRestore && r.Last().change.from == 1 && r.Last().change.to == 0 && waited >= 4.0 && waited < 4.05 &&
            std::string(r.Last().change.stat_name) == "p90" && r.Last().change.stat_ms < 11.1,
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
  CHECK(r.c.Level() == 1 && r.c.Doublings(1, r.now) == 0, "level 1");
  CHECK(std::fabs(r.c.RestoreDelayS(1, r.now, r.p) - 4.0) < 1e-9, "delay 4 s");
  // Restore (4 s of 11 ms), then 16 ms: the shed back within 2 s is a bounce of level 1.
  const double expected[] = {8.0, 16.0, 32.0, 60.0, 60.0};
  for (double want : expected) {
    const double delay_before = r.c.RestoreDelayS(1, r.now, r.p);
    const double since = double(r.now - r.Last().ns) / kSec;  // time since the level change
    r.Seconds(delay_before - since - 0.1, 11.0);
    CHECK(r.c.Level() == 1, "no restore 0.1 s before the delay of %.1f s", delay_before);
    r.Seconds(0.3, 11.0);
    CHECK(r.c.Level() == 0, "restored after the delay of %.1f s", delay_before);
    const int64_t restored_ns = r.Last().ns;
    r.Run(30, 16.0);
    CHECK(r.c.Level() == 1 && r.Last().change.bounce && double(r.Last().ns - restored_ns) / kSec < 2.0, "shed back within 2 s: a bounce");
    CHECK(std::fabs(r.c.RestoreDelayS(1, r.now, r.p) - want) < 1e-9, "the next delay is %.1f s, is %.1f", want, r.c.RestoreDelayS(1, r.now, r.p));
  }
  // Another level's delay is untouched.
  CHECK(std::fabs(r.c.RestoreDelayS(2, r.now, r.p) - 4.0) < 1e-9 && r.c.Doublings(1, r.now) == 5, "per level (%d doublings)", r.c.Doublings(1, r.now));
}

void TestBounceWindowAndDecay() {
  // A shed back to the level after MORE than 2 s is not a bounce.
  Rig r;
  r.SetAtLevel1();
  r.Seconds(4.2, 11.0);  // restored
  CHECK(r.c.Level() == 0, "restored");
  r.Seconds(2.5, 11.0);
  r.Run(30, 16.0);
  CHECK(r.c.Level() == 1 && !r.Last().change.bounce && r.c.Doublings(1, r.now) == 0, "shed after 2.5 s: no bounce");
  // A bounce doubles the delay (8 s); it decays one step per 30 s without another: 8 s -> 4 s after 30 s.
  Rig s;
  s.SetAtLevel1();
  s.Seconds(4.2, 11.0);
  s.Run(30, 16.0);
  CHECK(s.c.Level() == 1 && s.c.Doublings(1, s.now) == 1, "one bounce");
  // The delay is 8 s now; the doubling decays 30 s after those 8 s, so at 38 s, not at 30 s (a 32 s wait must not eat its own memory).
  const int64_t t = s.Last().ns + 8'000'000'000;
  CHECK(s.c.Doublings(1, t + 29'000'000'000) == 1 && s.c.Doublings(1, t + 31'000'000'000) == 0, "decays 30 s after the delay");
  CHECK(std::fabs(s.c.RestoreDelayS(1, t + 31'000'000'000, s.p) - 4.0) < 1e-9 && std::fabs(s.c.RestoreDelayS(1, t + 29'000'000'000, s.p) - 8.0) < 1e-9,
        "delay back to 4 s");
  // Two bounces: 16 s; 30 s after that delay 8 s; 60 s after it 4 s.
  s.Seconds(8.2, 11.0);
  s.Run(30, 16.0);
  CHECK(s.c.Doublings(1, s.now) == 2, "second bounce: %d", s.c.Doublings(1, s.now));
  const int64_t u = s.Last().ns + 16'000'000'000;
  CHECK(s.c.Doublings(1, u + 31'000'000'000) == 1 && s.c.Doublings(1, u + 61'000'000'000) == 0, "two steps of decay");
  // A restore delay never exceeds 60 s (but never undercuts restore_s itself).
  m::Params big = s.p;
  big.restore_s = 100.0;
  CHECK(std::fabs(s.c.RestoreDelayS(1, s.now, big) - 100.0) < 1e-9, "restore_s above the cap stays");
}

void TestNotDriving() {
  // A menu, a pause or a load: the level is held (no shed, no restore), and the timer starts over when driving resumes.
  Rig r;
  r.driving = false;
  r.Run(30, 30.0);
  CHECK(r.c.Level() == 0, "no shed in a menu, even at 30 ms");
  r.driving = true;
  r.Run(10, 16.0);
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
}

void TestRichLevels() {
  // Rich levels: the restore step goes on below 0 only while min_level allows it.
  Rig r;
  r.p.min_level = 0;
  r.Run(60, 9.0);
  CHECK(r.c.Level() == 0, "min_level 0: no rich level");
  r.p.min_level = -2;
  r.Seconds(4.3, 9.0);
  CHECK(r.c.Level() == -1 && r.Last().change.reason == m::Reason::kRestore, "level -1");
  r.Seconds(4.3, 9.0);
  CHECK(r.c.Level() == -2, "level -2");
  CHECK(r.Seconds(10.0, 9.0) < 0 && r.c.Level() == -2, "the floor");
  // Over budget on the rich level: the shed is level + 1, through 0 and on.
  r.Run(3, 16.0);
  CHECK(r.Last().change.from == -2 && r.Last().change.to == -1 && r.Last().change.reason == m::Reason::kGpuMedian, "shed from a rich level");
  r.Run(60, 16.0);
  CHECK(r.c.Level() == 3, "and up to the cap");
  // Rich levels switched off live: the level goes to 0 at once.
  Rig s;
  s.p.min_level = -1;
  s.Seconds(4.3, 9.0);
  CHECK(s.c.Level() == -1, "at -1");
  s.p.min_level = 0;
  const m::Change off = s.Frame(9.0);
  CHECK(off.changed && off.reason == m::Reason::kRichOff && off.to == 0, "rich off");
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
  CHECK(w.sheds == 2 && w.restores == 1 && w.bounces == 1 && w.missed == 1, "sheds %d restores %d bounces %d missed %u", w.sheds, w.restores, w.bounces, w.missed);
  CHECK(w.min_level == 0 && w.max_level == 1 && w.samples > 300 && w.signal_gpu && w.lag == 3, "levels %d..%d samples %u lag %d", w.min_level, w.max_level,
        w.samples, w.lag);
  int64_t total = 0;
  for (const auto& [level, ns] : w.time_ns) total += ns;
  CHECK(std::llabs(total - w.length_ns) <= 2 * kFrameNs && w.time_ns.size() == 2 && w.time_ns.at(1) > 4'000'000'000 && w.time_ns.at(0) > 0,
        "time at levels %.2f of %.2f s", double(total) / kSec, double(w.length_ns) / kSec);
  CHECK(w.p50_ms > 10.9 && w.p50_ms < 11.2 && w.p95_ms > 15.9 && w.p95_ms < 16.2, "p50 %.2f p95 %.2f", w.p50_ms, w.p95_ms);
  // The next window starts at the current level.
  const m::Window next = r.c.TakeWindow(r.now + 1'000'000'000);
  CHECK(next.min_level == next.max_level && next.min_level == r.c.Level() && next.sheds == 0 && next.samples == 0 && next.p50_ms < 0, "next window");
}

// ---- a closed loop against a toy GPU ---------------------------------------------------------------------------------

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
  r.p.max_level = 5;
  Plant plant;
  uint32_t rng = 7;
  for (int i = 0; i < 60 * 120; ++i) {
    rng = rng * 1664525u + 1013904223u;
    const double noise = (double(rng >> 8) / double(1 << 23) - 1.0) * 0.4;
    r.Frame(plant.Next(r.c.Level(), noise));
  }
  CHECK(r.c.Level() == 2 && r.Changes() == 2, "converged at level 2 (14.0 ms) after 2 sheds: level %d changes %d", r.c.Level(), r.Changes());
  // A calm scene afterwards: the loop restores all the way down.
  Rig q;
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
  CHECK(r.c.Doublings(1, r.now) >= 3, "doublings %d", r.c.Doublings(1, r.now));
  std::printf("dvfs test: %d sheds, %zu restores in 130 s, restores at", sheds, restores_at.size());
  for (double t : restores_at) std::printf(" %.1f", t);
  std::printf(" s\n");
}

}  // namespace

int main() {
  TestTableDefault();
  TestTableVrsUnsupported();
  TestTableVrsClamped();
  TestTableSyntax();
  TestTableEqualLevels();
  TestHistogram();
  TestShedLatency();
  TestShedSpacing();
  TestMissedFrames();
  TestRestoreDelay();
  TestRestoreNeedsP90();
  TestMissedFrameResetsRestore();
  TestRestoreWithoutGpuSignal();
  TestBounce();
  TestBounceWindowAndDecay();
  TestNotDriving();
  TestCapAndForce();
  TestRichLevels();
  TestHoldAllowed();
  TestWindow();
  TestClosedLoopConverges();
  TestClosedLoopDvfs();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
