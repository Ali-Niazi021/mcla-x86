// Unit test of the present cadence maths (mcla/tests/present_cadence.h, a verbatim copy of the iOS port's
// ios/src/present_cadence.h): the on-screen interval histogram and the display link slot choice.
// Standalone: no SDK, no game, no Apple headers.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_present_cadence_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra mcla/tests/mcla_present_cadence_test.cpp \
//       -o <scratch>/mcla_present_cadence_test && <scratch>/mcla_present_cadence_test
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "present_cadence.h"

namespace c = nfsmw::cadence;

static int g_checks = 0, g_failed = 0;
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

struct Rng {
  uint32_t s = 12345;
  double next() {  // [-1, 1]
    s = s * 1664525u + 1013904223u;
    return double(s >> 8) / double(1 << 23) - 1.0;
  }
};

// A panel: refreshes at origin + k * period. A drawable presented with atTime is shown at the first
// refresh at or after both atTime and the moment its GPU work is done, and never before the frame shown
// before it (Metal shows drawables in order).
struct Panel {
  double origin, period;
  double last_shown = 0;
  double Show(double at_time, double gpu_done) {
    const double t = std::max(at_time, gpu_done);
    double k = std::ceil((t - origin) / period - 1e-9);
    double shown = origin + k * period;
    if (shown <= last_shown) shown = last_shown + period;
    last_shown = shown;
    return shown;
  }
};

static void TestBuckets() {
  CHECK(c::BucketOf(8.3) == 0, "8.3");
  CHECK(c::BucketOf(16.7) == 1, "16.7");
  CHECK(c::BucketOf(25.0) == 2, "25");
  CHECK(c::BucketOf(33.3) == 3, "33.3");
  CHECK(c::BucketOf(41.7) == 4, "41.7");
  CHECK(c::BucketOf(50.0) == 5, "50");
  CHECK(c::BucketOf(66.7) == 6, "66.7");
  CHECK(c::BucketOf(100.0) == 7, "100");
  CHECK(c::BucketOf(300.0) == 8, "300");
  // Edges: 37.5 ms is the first of the 41.7 bucket, 37.4 the last of the 33.3.
  CHECK(c::BucketOf(37.4) == 3 && c::BucketOf(37.6) == 4, "edge 37.5");
}

static void TestLogWindow() {
  c::OnScreenLog log;
  std::string line;
  int lines = 0;
  // 30 fps on a 60 Hz screen, 15 s: three windows.
  double t = 1000.0;
  for (int i = 0; i < 500; ++i, t += 1.0 / 30.0) {
    if (log.OnShown(t, 1000.0 / 30.0, "test", &line)) {
      ++lines;
      CHECK(line.find("[ios-pacing] test") == 0, "label: %s", line.c_str());
      CHECK(line.find("33.3:") != std::string::npos, "bucket names: %s", line.c_str());
      CHECK(line.find("100.0 % within") != std::string::npos, "all on the cap: %s", line.c_str());
    }
  }
  CHECK(lines == 3, "three 5 s windows, got %d", lines);

  // Pattern 25 / 41.7 (the judder): lands in its own buckets.
  c::OnScreenLog log2;
  t = 2000.0;
  std::string got;
  for (int i = 0; i < 400 && got.empty(); ++i) {
    t += (i % 2 ? 0.0417 : 0.025);
    log2.OnShown(t, 1000.0 / 30.0, "j", &got);
  }
  CHECK(!got.empty() && got.find(" 25:") != std::string::npos && got.find(" 41.7:") != std::string::npos, "%s",
        got.c_str());
  CHECK(got.find(" 33.3:0") != std::string::npos, "no 33.3 in the judder pattern: %s", got.c_str());
  CHECK(got.find("0.0 % within") != std::string::npos, "%s", got.c_str());

  // Non-monotonic and repeated times are ignored (no negative interval, no zero interval).
  c::OnScreenLog log3;
  std::string l3;
  CHECK(!log3.OnShown(10.0, 33.3, "x", &l3), "first");
  CHECK(!log3.OnShown(10.0, 33.3, "x", &l3), "repeat ignored");
  CHECK(!log3.OnShown(9.9, 33.3, "x", &l3), "backwards ignored");
  CHECK(!log3.OnShown(0.0, 33.3, "x", &l3), "zero ignored");
}

static void TestLinkSlot() {
  const double p = 1.0 / 60.0, iv = 1.0 / 30.0;
  // Not usable: no link yet, a stopped link, a 48 Hz link at 30 fps.
  CHECK(!c::ChooseLinkSlot(0, p, 10, 10.02, 0, iv).ok, "no vsync");
  CHECK(!c::ChooseLinkSlot(9.0, p, 10, 10.02, 0, iv).ok, "link stopped for 1 s");
  CHECK(!c::ChooseLinkSlot(10.0, 1.0 / 48.0, 10, 10.02, 0, iv).ok, "48 Hz: 1.6 refreshes per frame");
  CHECK(c::ChooseLinkSlot(10.0, 1.0 / 120.0, 10, 10.02, 0, iv).refreshes == 4, "120 Hz: 4 refreshes");
  CHECK(c::ChooseLinkSlot(10.0, p, 10, 10.02, 0, iv).refreshes == 2, "60 Hz: 2 refreshes");
  CHECK(c::ChooseLinkSlot(10.0, p, 10, 10.02, 0, 1.0 / 60.0).refreshes == 1, "60 Hz at 60 fps: 1");

  // A slot is a refresh of the link's grid, and the target lies a few ms before it.
  const double v = 10.0003;
  const auto s = c::ChooseLinkSlot(v, p, 10.0100, 10.0200, 0, iv);
  CHECK(s.ok, "ok");
  const double k = (s.slot_s - v) / p;
  CHECK(std::fabs(k - std::round(k)) < 1e-9, "slot on the grid (k=%f)", k);
  CHECK(s.slot_s >= 10.0200 && s.slot_s < 10.0200 + p, "first refresh at/after ready: %f", s.slot_s);
  CHECK(s.at_time_s < s.slot_s && s.slot_s - s.at_time_s <= 0.004 + 1e-12, "target just before the slot");

  // The cap's spacing: the next frame is exactly 2 refreshes after, even when it is ready early.
  const auto a = c::ChooseLinkSlot(v, p, 10.01, 10.02, 0, iv);
  const auto b = c::ChooseLinkSlot(v, p, 10.02, 10.025, a.slot_s, iv);
  CHECK(std::fabs((b.slot_s - a.slot_s) - 2 * p) < 1e-9, "2 refreshes later: %f", (b.slot_s - a.slot_s) / p);
  CHECK(b.late_by == 0, "on time");
  // Late by 5 ms: one refresh more (50 ms), flagged as slipping by 1.
  const auto late = c::ChooseLinkSlot(v, p, 10.05, a.slot_s + 2 * p + 0.005, a.slot_s, iv);
  CHECK(std::fabs((late.slot_s - a.slot_s) - 3 * p) < 1e-9 && late.late_by == 1, "slips one refresh: %f / %d",
        (late.slot_s - a.slot_s) / p, late.late_by);
  // Jitter of the link's timestamps (+-0.3 ms) does not move a slot to another refresh.
  for (double j : {-0.0003, 0.0003}) {
    const auto jt = c::ChooseLinkSlot(v + j, p, 10.02, 10.025, a.slot_s, iv);
    CHECK(std::fabs((jt.slot_s - a.slot_s) - 2 * p) < 0.0004, "jitter %f: %f", j, (jt.slot_s - a.slot_s) / p);
  }
}

// The whole loop against a simulated panel, 60 s: frames are ready at an uneven time (GPU 12-24 ms,
// swap time jittering), the link's timestamps jitter +-0.2 ms, the clock of the game runs 80 ppm fast
// against the panel's (what made the open loop grid drift across a refresh every few seconds).
struct SimResult {
  uint64_t shown = 0, intervals[9] = {}, late_slots = 0;
  double worst_ms = 0;
};

static SimResult Simulate(double panel_hz, double fps, double drift_ppm, bool use_link) {
  SimResult out;
  Rng rng;
  const double period = 1.0 / panel_hz;
  Panel panel{0.0031, period};
  const double interval = 1.0 / fps;
  const double link_period = period > 1.0 / 90.0 ? period : 2.0 * period;  // the 60 Hz link of the 30 cap
  // The first seconds find the grid's latency (the slowest GPU time it sees): not counted.
  const int warmup_frames = int(3.0 * fps);
  double now = 5.0, last_slot = 0, last_target = 0, last_shown = 0;
  const double swap0 = now;
  for (int frame = 0; frame < int(60.0 * fps); ++frame) {
    // The guest swaps every `interval` of its own clock (fast against the panel's) +- 1.5 ms.
    now = swap0 + frame * interval / (1.0 + drift_ppm * 1e-6) + rng.next() * 0.0015;
    const double gpu = 0.012 + 0.012 * (rng.next() * 0.5 + 0.5);  // 12-24 ms
    const double ready = now + gpu + 0.004;
    double at;
    if (use_link) {
      // The newest refresh of the link at `now` (+- 0.2 ms timestamp noise).
      const double ref0 = panel.origin;
      const double k = std::floor((now - ref0) / link_period);
      const double vsync = ref0 + k * link_period + rng.next() * 0.0002;
      const auto s = c::ChooseLinkSlot(vsync, link_period, now, ready, last_slot, interval);
      if (!s.ok) return out;
      last_slot = s.slot_s;
      if (frame >= warmup_frames) out.late_slots += s.late_by > 0;
      at = s.at_time_s;
    } else {
      // The old open loop: the previous target + one interval, 2 ms early.
      double target = last_target > 0 && last_target > now - 4 * interval && last_target < now + 4 * interval
                          ? last_target + interval
                          : ready;
      while (target < ready) target += interval;
      last_target = target;
      at = target - 0.002;
    }
    const double shown = panel.Show(at, now + gpu);
    if (last_shown > 0 && frame >= warmup_frames) {
      const double ms = (shown - last_shown) * 1000.0;
      ++out.intervals[c::BucketOf(ms)];
      out.worst_ms = std::max(out.worst_ms, std::fabs(ms - 1000.0 / fps));
    }
    last_shown = shown;
    ++out.shown;
  }
  return out;
}

static void TestSimulation() {
  // 30 fps on a 60 Hz and a 120 Hz screen: with the link grid every interval is 33.3 ms, with the
  // drifting open loop some are not.
  for (double hz : {60.0, 120.0}) {
    const auto link = Simulate(hz, 30.0, 80.0, true);
    CHECK(link.shown > 1700, "frames %llu", (unsigned long long)link.shown);
    uint64_t total = 0;
    for (auto n : link.intervals) total += n;
    CHECK(total > 0 && link.intervals[3] == total, "link grid at %.0f Hz: every interval 33.3 ms (%llu of %llu, worst %.1f ms off)",
          hz, (unsigned long long)link.intervals[3], (unsigned long long)total, link.worst_ms);
    CHECK(link.late_slots == 0, "no late slot at %.0f Hz: %llu", hz, (unsigned long long)link.late_slots);
  }
  // 60 fps on 60 Hz and 120 Hz (a 60 Hz link: 1 or 2 panel refreshes): every interval 16.7 ms.
  {
    const auto link = Simulate(60.0, 60.0, 80.0, true);
    uint64_t total = 0;
    for (auto n : link.intervals) total += n;
    CHECK(total > 0 && link.intervals[1] == total, "60 fps at 60 Hz: %llu of %llu at 16.7", (unsigned long long)link.intervals[1],
          (unsigned long long)total);
  }
  // The drifting open loop is what the link grid replaces: it must be the one that shows off-grid intervals.
  const auto open = Simulate(60.0, 30.0, 80.0, false);
  uint64_t total = 0;
  for (auto n : open.intervals) total += n;
  std::printf("  (old open loop, 80 ppm drift, 60 Hz: %llu of %llu intervals at 33.3 ms, worst %.1f ms off)\n",
              (unsigned long long)open.intervals[3], (unsigned long long)total, open.worst_ms);
}

int main() {
  TestBuckets();
  TestLogWindow();
  TestLinkSlot();
  TestSimulation();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
