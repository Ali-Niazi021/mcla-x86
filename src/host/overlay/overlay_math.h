// mcla-x86 host overlay: the arithmetic of the statistics screen. Pure C++ (no SDK, no ImGui, no Windows), so
// mcla/tests/test_overlay.cpp compiles and runs it alone.
//
//   Summarize()        a list of frame times -> fps, average, percentiles, 1 % / 0.1 % low, frames over 20 / 25 / 34 ms.
//                      The percentile is bench.py's (linear interpolation between order statistics), and the "1 % low" is the
//                      fps of the 99th percentile frame time (the project's gate "p99 <= 20 ms" at 60 fps reads the same).
//   WorldSpeed()       how fast the game's world ran against real time over a stretch (the "world/real" of bench.py).
//   RatePerSecond()    a cumulative counter's change over a stretch, per second (wraps of a 32-bit counter included).
//   Downsample()       a long list of frame times -> N bars for a graph (the maximum of each bucket, so a hitch is never
//                      averaged away).
//   FontIndexFor()     which baked font size to use for a window height and the player's size factor.
//   ClassifyHitch()    one short phrase for what a long frame was doing (from the numbers mcla_hitch.cpp already has).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mcla_overlay::math {

struct FrameTimeStats {
  uint32_t count = 0;
  double span_s = 0.0;  // the time the frames cover (sum of their times)
  double fps = 0.0;     // count / span
  double avg_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double p999_ms = 0.0;
  double worst_ms = 0.0;
  double low1_fps = 0.0;   // 1000 / p99
  double low01_fps = 0.0;  // 1000 / p99.9
  uint32_t over20 = 0, over25 = 0, over34 = 0;  // frames longer than 20 / 25 / 34 ms
};

// p in 0..1 of an ASCENDING list, linear interpolation between order statistics (tools/bench.py percentile()).
inline double Percentile(const std::vector<float>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return double(sorted[0]);
  p = std::clamp(p, 0.0, 1.0);
  const double rank = p * double(sorted.size() - 1);
  const size_t lo = size_t(std::floor(rank));
  const size_t hi = std::min(sorted.size() - 1, lo + 1);
  const double frac = rank - double(lo);
  return double(sorted[lo]) * (1.0 - frac) + double(sorted[hi]) * frac;
}

// `ms` = real time between engine frames, any order. Frames of zero or less (or not a number) are ignored.
inline FrameTimeStats Summarize(const std::vector<float>& ms) {
  FrameTimeStats s;
  std::vector<float> v;
  v.reserve(ms.size());
  double sum = 0.0;
  for (float f : ms) {
    if (!(f > 0.0f) || !std::isfinite(f)) continue;
    v.push_back(f);
    sum += double(f);
    if (f > 20.0f) ++s.over20;
    if (f > 25.0f) ++s.over25;
    if (f > 34.0f) ++s.over34;
  }
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.count = uint32_t(v.size());
  s.span_s = sum / 1000.0;
  s.avg_ms = sum / double(v.size());
  s.fps = s.avg_ms > 0.0 ? 1000.0 / s.avg_ms : 0.0;
  s.p50_ms = Percentile(v, 0.50);
  s.p99_ms = Percentile(v, 0.99);
  s.p999_ms = Percentile(v, 0.999);
  s.worst_ms = double(v.back());
  s.low1_fps = s.p99_ms > 0.0 ? 1000.0 / s.p99_ms : 0.0;
  s.low01_fps = s.p999_ms > 0.0 ? 1000.0 / s.p999_ms : 0.0;
  return s;
}

// World seconds that passed over `real_s` real seconds. A frozen world (a pause, a menu: the timer's total does not move)
// reads 0; a stretch that is too short to say reads -1.
inline double WorldSpeed(double world_s, double real_s, double min_real_s = 1.0) {
  if (!(real_s >= min_real_s) || !std::isfinite(world_s)) return -1.0;
  if (world_s < 0.0) return -1.0;  // the timer was reset (a load)
  return world_s / real_s;
}

// Change of a cumulative counter per second. `bits` = 32 for a 32-bit counter that may wrap.
inline double RatePerSecond(uint64_t before, uint64_t after, double seconds, int bits = 64) {
  if (!(seconds > 0.0)) return 0.0;
  uint64_t delta = after - before;
  if (bits == 32) delta = uint64_t(uint32_t(uint32_t(after) - uint32_t(before)));
  if (bits == 64 && after < before) return 0.0;  // a counter that went backwards was reset
  return double(delta) / seconds;
}

// Shrinks `in` to `out_count` bars; each bar is the largest value of its bucket. Fewer values than bars: the values, left
// aligned, the rest 0.
inline std::vector<float> Downsample(const std::vector<float>& in, size_t out_count) {
  std::vector<float> out(out_count, 0.0f);
  if (in.empty() || out_count == 0) return out;
  if (in.size() <= out_count) {
    std::copy(in.begin(), in.end(), out.begin());
    return out;
  }
  for (size_t i = 0; i < out_count; ++i) {
    const size_t b = i * in.size() / out_count;
    const size_t e = std::max(b + 1, (i + 1) * in.size() / out_count);
    float peak = 0.0f;
    for (size_t k = b; k < e && k < in.size(); ++k) peak = std::max(peak, in[k]);
    out[i] = peak;
  }
  return out;
}

// The top of a frame-time graph: the smallest of 20, 34, 50, 100 ms, then a multiple of 50, that holds `worst_ms`... but never
// below `floor_ms` (so a calm 60 fps does not fill the box with noise). Steady steps so the picture does not breathe.
inline double GraphCeilingMs(double worst_ms, double floor_ms = 34.0) {
  static const double kSteps[] = {20.0, 34.0, 50.0, 100.0};
  const double need = std::max(worst_ms * 1.05, floor_ms);
  for (double s : kSteps) {
    if (need <= s) return s;
  }
  return std::ceil(need / 50.0) * 50.0;
}

// The baked font sizes, in pixels, smallest first (overlay_ui.cpp bakes them in OnConfigureFonts, kFontPixels).
// The wanted size is the window height / 62 (17 px at 1080, 32 at 2000, 12 at 720) times the player's factor; the
// nearest baked size wins (in the log scale, so 1.5x and 0.67x are equally far).
inline int FontIndexFor(double display_height, double user_scale, const float* sizes, int count) {
  if (count <= 0) return -1;
  const double wanted = std::max(8.0, (display_height > 0.0 ? display_height : 720.0) / 62.0) * std::clamp(user_scale, 0.25, 4.0);
  int best = 0;
  double best_distance = 1e30;
  for (int i = 0; i < count; ++i) {
    const double distance = std::fabs(std::log(double(sizes[i]) / wanted));
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

// What was going on in a long engine frame, in one phrase. The numbers are the ones mcla_hitch.cpp already measures; -1 = not
// measured. `on_cpu_ms` is the game thread's own CPU time in the frame, `slept_ms` what the pacer slept in it.
struct HitchFacts {
  double frame_ms = 0.0;
  double on_cpu_ms = -1.0;
  double slept_ms = 0.0;
  int32_t pipelines = 0;  // run-time pipeline creations in the frame
  int32_t shaders = 0;    // run-time shader translations in the frame
  double gpu_ms = -1.0;   // the GPU frame time last read back
  int32_t page_faults = 0;
  double read_mib = 0.0;
  bool menu = false;      // a menu, pause or load (a long frame there is usually a load step)
};

inline std::string ClassifyHitch(const HitchFacts& f) {
  char buf[160];
  if (f.pipelines > 0 || f.shaders > 0) {
    std::snprintf(buf, sizeof(buf), "shader compile (+%d pipelines, +%d shaders)", int(f.pipelines), int(f.shaders));
    return buf;
  }
  if (f.read_mib >= 4.0) {
    std::snprintf(buf, sizeof(buf), "disk read (+%.0f MiB)", f.read_mib);
    return buf;
  }
  if (f.on_cpu_ms >= 0.0 && f.on_cpu_ms >= 0.6 * f.frame_ms) {
    std::snprintf(buf, sizeof(buf), "game thread busy (%.0f ms of CPU)", f.on_cpu_ms);
    return buf;
  }
  if (f.gpu_ms >= 0.0 && f.gpu_ms >= 0.8 * f.frame_ms) {
    std::snprintf(buf, sizeof(buf), "GPU (%.0f ms)", f.gpu_ms);
    return buf;
  }
  if (f.page_faults >= 2000) {
    std::snprintf(buf, sizeof(buf), "paging (+%d page faults)", int(f.page_faults));
    return buf;
  }
  if (f.on_cpu_ms >= 0.0) {
    std::snprintf(buf, sizeof(buf), "game thread waited (%.0f ms)", std::max(0.0, f.frame_ms - f.on_cpu_ms - f.slept_ms));
    return buf;
  }
  return "unknown";
}

}  // namespace mcla_overlay::math
