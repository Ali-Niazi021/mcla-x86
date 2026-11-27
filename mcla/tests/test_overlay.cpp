// Unit test of the overlay's pure parts (src/host/overlay): the statistics arithmetic (overlay_math.h), the launcher.json merge
// (overlay_json.h) and the game-thread feed (overlay_feed.cpp, which needs nothing but the standard library).
// Standalone: no SDK, no ImGui, no Windows, no game.
//
// Built and run by CTest (mcla/tests/CMakeLists.txt, test mcla_overlay_test). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I src/host/overlay mcla/tests/test_overlay.cpp src/host/overlay/overlay_feed.cpp \
//       -o <scratch>/test_overlay && <scratch>/test_overlay
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "overlay_feed.h"
#include "overlay_json.h"
#include "overlay_math.h"

namespace m = mcla_overlay::math;
namespace j = mcla_overlay::json;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

#define CHECK_NEAR(got, want, tol)                                                                              \
  do {                                                                                                          \
    const double got_ = double(got), want_ = double(want);                                                      \
    ++g_checks;                                                                                                 \
    if (!(std::fabs(got_ - want_) <= (tol))) {                                                                  \
      ++g_failures;                                                                                             \
      std::printf("FAIL %s:%d: %s = %.6f, expected %.6f +- %g\n", __FILE__, __LINE__, #got, got_, want_, double(tol)); \
    }                                                                                                           \
  } while (0)

#define CHECK_STR(got, want)                                                                                    \
  do {                                                                                                          \
    const std::string got_ = (got), want_ = (want);                                                             \
    ++g_checks;                                                                                                 \
    if (got_ != want_) {                                                                                        \
      ++g_failures;                                                                                             \
      std::printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #got, got_.c_str(), want_.c_str()); \
    }                                                                                                           \
  } while (0)

// ---- overlay_math.h -------------------------------------------------------------------------------------------------

void TestSummarize() {
  // 99 frames of 16.7 ms and one of 50 ms.
  std::vector<float> ms(99, 16.7f);
  ms.push_back(50.0f);
  const m::FrameTimeStats s = m::Summarize(ms);
  CHECK(s.count == 100);
  CHECK_NEAR(s.avg_ms, (99 * 16.7 + 50.0) / 100.0, 1e-3);
  CHECK_NEAR(s.fps, 1000.0 / s.avg_ms, 1e-6);
  CHECK_NEAR(s.p50_ms, 16.7, 1e-4);
  CHECK_NEAR(s.worst_ms, 50.0, 1e-9);
  // p99 of 100 values: rank 0.99 * 99 = 98.01 -> between the 99th (16.7) and the 100th (50): 16.7 + 0.01 * 33.3.
  CHECK_NEAR(s.p99_ms, 16.7 + 0.01 * 33.3, 1e-3);
  CHECK_NEAR(s.low1_fps, 1000.0 / s.p99_ms, 1e-6);
  CHECK(s.over20 == 1 && s.over25 == 1 && s.over34 == 1);
  CHECK_NEAR(s.span_s, (99 * 16.7 + 50.0) / 1000.0, 1e-4);

  // Junk is ignored, an empty list is all zero.
  const m::FrameTimeStats junk = m::Summarize({0.0f, -3.0f, std::nanf(""), 20.0f});
  CHECK(junk.count == 1);
  CHECK_NEAR(junk.avg_ms, 20.0, 1e-9);
  CHECK(junk.over20 == 0);  // "longer than 20": 20 is not
  const m::FrameTimeStats none = m::Summarize({});
  CHECK(none.count == 0 && none.fps == 0.0 && none.low1_fps == 0.0);

  // One frame.
  const m::FrameTimeStats one = m::Summarize({33.3f});
  CHECK_NEAR(one.p99_ms, 33.3, 1e-4);
  CHECK(one.over25 == 1 && one.over34 == 0);
}

void TestPercentile() {
  const std::vector<float> v = {1, 2, 3, 4, 5};
  CHECK_NEAR(m::Percentile(v, 0.0), 1.0, 1e-9);
  CHECK_NEAR(m::Percentile(v, 1.0), 5.0, 1e-9);
  CHECK_NEAR(m::Percentile(v, 0.5), 3.0, 1e-9);
  CHECK_NEAR(m::Percentile(v, 0.125), 1.5, 1e-9);  // rank 0.5
  CHECK_NEAR(m::Percentile({}, 0.5), 0.0, 1e-9);
  CHECK_NEAR(m::Percentile(v, 7.0), 5.0, 1e-9);  // clamped
}

void TestWorldSpeed() {
  CHECK_NEAR(m::WorldSpeed(2.0, 2.0), 1.0, 1e-9);
  CHECK_NEAR(m::WorldSpeed(1.0, 2.0), 0.5, 1e-9);
  CHECK_NEAR(m::WorldSpeed(0.0, 2.0), 0.0, 1e-9);   // paused
  CHECK_NEAR(m::WorldSpeed(-5.0, 2.0), -1.0, 1e-9);  // the timer was reset
  CHECK_NEAR(m::WorldSpeed(0.4, 0.5), -1.0, 1e-9);   // too short a stretch to say
  CHECK_NEAR(m::WorldSpeed(std::nan(""), 2.0), -1.0, 1e-9);
}

void TestRate() {
  CHECK_NEAR(m::RatePerSecond(100, 160, 2.0), 30.0, 1e-9);
  CHECK_NEAR(m::RatePerSecond(100, 100, 2.0), 0.0, 1e-9);
  CHECK_NEAR(m::RatePerSecond(5, 3, 1.0), 0.0, 1e-9);  // a 64-bit counter that went backwards was reset
  // A 32-bit counter that wrapped: 0xFFFFFFF0 -> 0x10 is 32 steps.
  CHECK_NEAR(m::RatePerSecond(0xFFFFFFF0ull, 0x10ull, 1.0, 32), 32.0, 1e-9);
  CHECK_NEAR(m::RatePerSecond(1, 2, 0.0), 0.0, 1e-9);
}

void TestDownsample() {
  const std::vector<float> in = {1, 1, 9, 1, 1, 1, 1, 1};
  const std::vector<float> out = m::Downsample(in, 4);
  CHECK(out.size() == 4);
  CHECK_NEAR(out[0], 1.0, 1e-9);
  CHECK_NEAR(out[1], 9.0, 1e-9);  // the hitch survives
  CHECK_NEAR(out[2], 1.0, 1e-9);
  const std::vector<float> few = m::Downsample({3, 4}, 5);
  CHECK(few.size() == 5);
  CHECK_NEAR(few[0], 3.0, 1e-9);
  CHECK_NEAR(few[1], 4.0, 1e-9);
  CHECK_NEAR(few[2], 0.0, 1e-9);
  CHECK(m::Downsample({}, 3).size() == 3);
  CHECK(m::Downsample({1, 2}, 0).empty());
}

void TestGraphCeiling() {
  CHECK_NEAR(m::GraphCeilingMs(10.0), 34.0, 1e-9);   // never below the floor
  CHECK_NEAR(m::GraphCeilingMs(33.0), 50.0, 1e-9);   // 33 * 1.05 = 34.65 > 34
  CHECK_NEAR(m::GraphCeilingMs(60.0), 100.0, 1e-9);
  CHECK_NEAR(m::GraphCeilingMs(120.0), 150.0, 1e-9);
  CHECK_NEAR(m::GraphCeilingMs(15.0, 20.0), 20.0, 1e-9);
}

void TestFontIndex() {
  const float sizes[] = {14.0f, 17.0f, 21.0f, 26.0f, 33.0f};
  CHECK(m::FontIndexFor(720, 1.0, sizes, 5) == 0);   // 11.6 px wanted
  CHECK(m::FontIndexFor(900, 1.0, sizes, 5) == 0);   // 14.5
  CHECK(m::FontIndexFor(1080, 1.0, sizes, 5) == 1);  // 17.4
  CHECK(m::FontIndexFor(1333, 1.0, sizes, 5) == 2);  // 21.5: the 3200x2000 window at 150 % scaling
  CHECK(m::FontIndexFor(1440, 1.0, sizes, 5) == 2);  // 23.2
  CHECK(m::FontIndexFor(2000, 1.0, sizes, 5) == 4);  // 32.3
  CHECK(m::FontIndexFor(1080, 2.0, sizes, 5) == 4);  // 34.8
  CHECK(m::FontIndexFor(1080, 0.5, sizes, 5) == 0);  // 8.7
  CHECK(m::FontIndexFor(0, 1.0, sizes, 5) == 0);     // unknown height: the 720 rule
  CHECK(m::FontIndexFor(1080, 1.0, sizes, 0) == -1);
}

void TestHitchClass() {
  m::HitchFacts f;
  f.frame_ms = 120.0;
  f.on_cpu_ms = 10.0;
  f.pipelines = 3;
  CHECK_STR(m::ClassifyHitch(f), "shader compile (+3 pipelines, +0 shaders)");
  f.pipelines = 0;
  f.read_mib = 12.0;
  CHECK_STR(m::ClassifyHitch(f), "disk read (+12 MiB)");
  f.read_mib = 0.0;
  f.on_cpu_ms = 100.0;
  CHECK_STR(m::ClassifyHitch(f), "game thread busy (100 ms of CPU)");
  f.on_cpu_ms = 10.0;
  f.gpu_ms = 110.0;
  CHECK_STR(m::ClassifyHitch(f), "GPU (110 ms)");
  f.gpu_ms = 5.0;
  f.page_faults = 5000;
  CHECK_STR(m::ClassifyHitch(f), "paging (+5000 page faults)");
  f.page_faults = 0;
  f.slept_ms = 4.0;
  CHECK_STR(m::ClassifyHitch(f), "game thread waited (106 ms)");
  f.on_cpu_ms = -1.0;
  CHECK_STR(m::ClassifyHitch(f), "unknown");
}

// ---- overlay_json.h -------------------------------------------------------------------------------------------------

void TestJsonParse() {
  j::Value v;
  std::string err;
  CHECK(j::Parse(R"({"a": 1, "b": [true, false, null, "x\n\u00e9\ud83d\ude00"], "c": {"d": -2.50e+3}})", &v, &err));
  CHECK(v.type == j::Value::Type::kObject);
  CHECK(v.Find("a") && v.Find("a")->type == j::Value::Type::kNumber && v.Find("a")->text == "1");
  const j::Value* b = v.Find("b");
  CHECK(b && b->array.size() == 4);
  CHECK(b && b->array[0].boolean && !b->array[1].boolean && b->array[2].type == j::Value::Type::kNull);
  CHECK(b && b->array[3].text == "x\n\xC3\xA9\xF0\x9F\x98\x80");  // newline, e-acute, U+1F600
  CHECK(v.Find("c") && v.Find("c")->Find("d") && v.Find("c")->Find("d")->text == "-2.50e+3");  // numbers keep their text

  // Malformed input is refused, with a reason.
  const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "{\"a\": 01x}", "{\"a\": tru}", "\"abc", "{\"a\":1} x", "{\"a\": \"\\q\"}",
                       "{\"a\": \"\\ud800\"}", "{\"a\": 1.}", "{\"a\": -}", "{\"a\": \"\x01\"}"};
  for (const char* text : bad) {
    j::Value out;
    std::string e;
    CHECK(!j::Parse(text, &out, &e));
    CHECK(!e.empty());
  }
  // Depth is limited, not recursed without end.
  std::string deep(200, '[');
  deep += std::string(200, ']');
  j::Value out;
  CHECK(!j::Parse(deep, &out, &err));
  // A byte order mark is tolerated.
  CHECK(j::Parse("\xEF\xBB\xBF{\"k\":1}", &out, &err));
}

void TestJsonRoundTrip() {
  const std::string text = R"({"version":1,"values":{"fullscreen":true,"user_data_root":"C:\\Users\\Ali \"x\"\\caf\u00e9","scale":0.50},"other":[1,2]})";
  j::Value v;
  std::string err;
  CHECK(j::Parse(text, &v, &err));
  const std::string again = j::Serialize(v);
  j::Value v2;
  CHECK(j::Parse(again, &v2, &err));
  CHECK(j::Serialize(v2) == again);                             // stable
  CHECK(v2.Find("values")->Find("scale")->text == "0.50");        // a number is printed as it was written
  CHECK(v2.Find("values")->Find("user_data_root")->text == "C:\\Users\\Ali \"x\"\\caf\xC3\xA9");
  CHECK(again.find("caf\xC3\xA9") != std::string::npos);        // UTF-8 passes through
  j::Value control = j::Value::String(std::string("a\x01") + "b");
  CHECK(j::Serialize(control).find("\\u0001") != std::string::npos);
}

void TestMerge() {
  const std::string existing = R"({
  "version": 1,
  "values": {
    "fullscreen": true,
    "mcla_gfx_preset": "perf",
    "unknown_future_id": {"keep": [1, 2, 3]}
  },
  "carried": "yes"
})";
  std::vector<std::pair<std::string, j::Value>> updates;
  updates.emplace_back("mcla_gfx_preset", j::Value::String("quality"));      // replaced in place
  updates.emplace_back("texture_lod_bias", j::Value::Number("-0.5"));         // added
  updates.emplace_back("swap_post_effect", j::Value::String("fxaa"));
  updates.emplace_back("aa_auto", j::Value::Bool(false));
  std::string merged, err;
  CHECK(j::MergeLauncherValues(existing, updates, &merged, &err));
  j::Value v;
  CHECK(j::Parse(merged, &v, &err));
  CHECK(v.Find("version") && v.Find("version")->text == "1");
  CHECK(v.Find("carried") && v.Find("carried")->text == "yes");              // other top-level keys survive
  const j::Value* values = v.Find("values");
  CHECK(values && values->type == j::Value::Type::kObject);
  CHECK(values && values->Find("fullscreen") && values->Find("fullscreen")->boolean);  // untouched
  CHECK(values && values->Find("mcla_gfx_preset") && values->Find("mcla_gfx_preset")->text == "quality");
  CHECK(values && values->Find("unknown_future_id") && values->Find("unknown_future_id")->Find("keep") &&
        values->Find("unknown_future_id")->Find("keep")->array.size() == 3);   // an id nobody here knows survives
  CHECK(values && values->Find("texture_lod_bias") && values->Find("texture_lod_bias")->text == "-0.5");
  CHECK(values && values->Find("aa_auto") && !values->Find("aa_auto")->boolean);
  // Order: the replaced id keeps its place, the new ones come last, in the order given.
  CHECK(values && values->object.size() == 6);
  CHECK(values && values->object[1].first == "mcla_gfx_preset");
  CHECK(values && values->object[3].first == "texture_lod_bias" && values->object[5].first == "aa_auto");

  // No "values" yet: made. "values" of the wrong type, a root that is not an object, junk: refused, nothing produced.
  merged.clear();
  CHECK(j::MergeLauncherValues(R"({"version":1})", updates, &merged, &err));
  CHECK(merged.find("\"values\"") != std::string::npos);
  merged = "untouched";
  CHECK(!j::MergeLauncherValues(R"({"values": 3})", updates, &merged, &err) && merged == "untouched" && !err.empty());
  CHECK(!j::MergeLauncherValues("[1]", updates, &merged, &err) && merged == "untouched");
  CHECK(!j::MergeLauncherValues("{ not json", updates, &merged, &err) && merged == "untouched");
  CHECK(!j::MergeLauncherValues("", updates, &merged, &err) && merged == "untouched");
}

// ---- overlay_feed.cpp -----------------------------------------------------------------------------------------------

void TestFeed() {
  using namespace mcla_overlay;
  ResetForTest();
  SetDemand(false);
  NoteEngineFrame(1'000'000'000);
  NoteEngineFrame(1'016'000'000);
  CHECK(FramesRecorded() == 0);  // nobody looks: nothing is recorded

  SetDemand(true);
  CHECK(Demand());
  int64_t t = 2'000'000'000;
  NoteEngineFrame(t);  // the first frame only starts the measurement
  CHECK(FramesRecorded() == 0);
  for (int i = 0; i < 60; ++i) {
    t += 16'666'667;
    NoteEngineFrame(t);
  }
  CHECK(FramesRecorded() == 60);
  t += 120'000'000;  // a 120 ms hitch
  NoteEngineFrame(t);
  CHECK(FramesRecorded() == 61);
  t += 2'500'000'000;  // a 2.5 s gap (a load): not a frame time
  NoteEngineFrame(t);
  CHECK(FramesRecorded() == 61);
  t += 16'666'667;
  NoteEngineFrame(t);  // measured from the instant after the gap
  CHECK(FramesRecorded() == 62);

  double span = 0.0;
  std::vector<float> recent = RecentFrameTimes(t, 600.0, 4000, &span);
  CHECK(recent.size() == 62);
  CHECK_NEAR(recent.front(), 16.666667, 1e-3);
  CHECK_NEAR(recent[60], 120.0, 1e-3);
  CHECK_NEAR(span, (61 * 16.666667 + 120.0) / 1000.0, 1e-2);
  // A window by age: the last 0.2 s holds the final frame only (the frame before it ended 2.5 s earlier).
  recent = RecentFrameTimes(t, 0.2, 4000);
  CHECK(recent.size() == 1);
  // A cap on the count.
  CHECK(RecentFrameTimes(t, 600.0, 10).size() == 10);
  // The graph: the newest `count` frames, zeros in front when there are fewer.
  const std::vector<float> graph = LastFrameTimes(100);
  CHECK(graph.size() == 100);
  CHECK_NEAR(graph[99], 16.666667, 1e-3);
  CHECK_NEAR(graph[37], 0.0, 1e-9);  // 62 recorded: slots 0..37 are the zeros
  CHECK(graph[38] > 0.0f);

  // The ring wraps: more than 4096 frames, the newest are what is read.
  ResetForTest();
  SetDemand(true);
  t = 10'000'000'000;
  NoteEngineFrame(t);
  for (int i = 0; i < 5000; ++i) {
    t += 8'333'333;
    NoteEngineFrame(t);
  }
  CHECK(FramesRecorded() == 5000);
  recent = RecentFrameTimes(t, 1000.0, 100000);
  CHECK(recent.size() == 4080);  // the ring is 4096 deep, 16 slots are kept clear of the writer
  CHECK_NEAR(recent.back(), 8.333333, 1e-3);

  // Hitches.
  ResetForTest();
  CHECK(Hitches(0, 60.0).total == 0 && Hitches(0, 60.0).last_ns == 0);
  NoteHitch(10'000'000'000, 80.0, 0, "shader compile (+2 pipelines, +1 shaders)");
  NoteHitch(40'000'000'000, 150.0, 1, "disk read (+9 MiB)");
  const HitchSummary h = Hitches(50'000'000'000, 30.0);
  CHECK(h.total == 2);
  CHECK(h.recent == 1);  // the first is 40 s old: outside 30 s
  CHECK_NEAR(h.worst_recent_ms, 150.0, 1e-9);
  CHECK_NEAR(h.last_ms, 150.0, 1e-9);
  CHECK(h.last_screen_mode == 1);
  CHECK_STR(h.last_cause, "disk read (+9 MiB)");
  const HitchSummary all = Hitches(50'000'000'000, 600.0);
  CHECK(all.recent == 2 && std::fabs(all.worst_recent_ms - 150.0) < 1e-9);
  SetDemand(false);
}

void TestWorldClock() {
  using namespace mcla_overlay;
  ResetForTest();
  SetDemand(true);
  int64_t t = 5'000'000'000;
  NoteEngineFrame(t);
  NoteWorldTime(t, 100.0f);  // the first call only starts the measurement: no frame, nothing stored
  double world_s = 0.0, real_s = 0.0;
  CHECK(!WorldClock(3.0, &world_s, &real_s));

  // 300 frames at 60 fps with the world running at 1.000 (6 s).
  float total = 100.0f;
  for (int i = 0; i < 300; ++i) {
    t += 16'666'667;
    total += 1.0f / 60.0f;
    NoteEngineFrame(t);
    NoteWorldTime(t, total);
  }
  CHECK(WorldClock(3.0, &world_s, &real_s));
  CHECK(real_s >= 3.0 && real_s < 3.03);
  CHECK_NEAR(world_s / real_s, 1.0, 1e-3);
  CHECK(!WorldClock(100.0, &world_s, &real_s));  // more than the feed holds

  // A time noted for an instant that is not the newest frame's is ignored.
  NoteWorldTime(t + 5, 5000.0f);
  CHECK(WorldClock(3.0, &world_s, &real_s));
  CHECK_NEAR(world_s / real_s, 1.0, 1e-3);

  // A pause: 240 frames with the timer standing still. The last 3 s hold no world time at all.
  for (int i = 0; i < 240; ++i) {
    t += 16'666'667;
    NoteEngineFrame(t);
    NoteWorldTime(t, total);
  }
  CHECK(WorldClock(3.0, &world_s, &real_s));
  CHECK_NEAR(world_s, 0.0, 1e-9);
  CHECK_NEAR(mcla_overlay::math::WorldSpeed(world_s, real_s, 2.9), 0.0, 1e-9);

  // Frames recorded without a world time (the hook did not note one) give no answer instead of a wrong one.
  ResetForTest();
  SetDemand(true);
  t = 9'000'000'000;
  NoteEngineFrame(t);
  for (int i = 0; i < 300; ++i) {
    t += 16'666'667;
    NoteEngineFrame(t);
  }
  CHECK(!WorldClock(3.0, &world_s, &real_s));
  SetDemand(false);
}

}  // namespace

int main() {
  TestSummarize();
  TestPercentile();
  TestWorldSpeed();
  TestRate();
  TestDownsample();
  TestGraphCeiling();
  TestFontIndex();
  TestHitchClass();
  TestJsonParse();
  TestJsonRoundTrip();
  TestMerge();
  TestFeed();
  TestWorldClock();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
