// Present cadence: what the screen really showed, and which refresh a frame is aimed at.
//
// Header only and free of Apple headers on purpose: the same text is the on-screen interval
// histogram of the native renderer (native_renderer.mm) and, copied verbatim into the SDK by
// games/mcla/sdk_patches/0206 (include/rex/ui/ios_present_cadence.h), the one of the MoltenVK
// presenter, and the cadence maths is checked by games/mcla/tests/mcla_present_cadence_test.cpp.
//
// Two things:
//   - OnScreenLog: every shown frame's time (CAMetalDrawable.presentedTime, or the
//     VK_GOOGLE_display_timing actualPresentTime) goes in; every 5 s one line comes out with the
//     interval between consecutive shown frames in buckets of 120 Hz refreshes (8.3 ms steps):
//     at a 30 cap on a 60 Hz screen everything should sit in 33.3, at 120 Hz too.
//   - ChooseLinkSlot: the refresh a frame is aimed at, taken from the display link's own refresh
//     times (not from a clock of ours that drifts from the panel's): n whole refreshes after the
//     previous frame's slot, or the first one after the frame is ready.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace nfsmw::cadence {

// Intervals between two shown frames, in ms. Bucket edges lie halfway between multiples of a
// 120 Hz refresh (8.333 ms), so an interval of k refreshes always lands in its own bucket.
inline constexpr int kBuckets = 9;
inline constexpr double kBucketUpperMs[kBuckets - 1] = {12.5,  20.833, 29.167, 37.5,
                                                         45.833, 58.333, 75.0,   120.0};
inline constexpr const char* kBucketNames[kBuckets] = {"8.3",  "16.7", "25",     "33.3", "41.7",
                                                        "50",   "66.7", "83-120", ">120"};

inline int BucketOf(double ms) {
  for (int i = 0; i < kBuckets - 1; ++i) {
    if (ms < kBucketUpperMs[i]) return i;
  }
  return kBuckets - 1;
}

// One 5 s window of shown frames. Thread safe (Metal calls its presented handlers from a queue).
class OnScreenLog {
 public:
  // Records a frame shown at `shown_s` (seconds, any steady clock; a time that does not move
  // forward is ignored) and, when this closes a 5 s window, fills *line and returns true.
  // expected_ms: the interval the cap asks for (33.3 at 30 fps), 0 = unknown.
  bool OnShown(double shown_s, double expected_ms, const char* label, std::string* line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shown_s <= 0) return false;
    if (last_ > 0 && shown_s > last_) {
      const double ms = (shown_s - last_) * 1000.0;
      ++bucket_[BucketOf(ms)];
      ++intervals_;
      sum_ += ms;
      sum_sq_ += ms * ms;
      max_ = std::max(max_, ms);
      min_ = intervals_ == 1 ? ms : std::min(min_, ms);
      if (expected_ms > 0 && std::fabs(ms - expected_ms) <= kOnCapToleranceMs) ++on_cap_;
    } else if (last_ > 0) {
      return false;  // out of order or a repeated time
    }
    last_ = shown_s;
    ++shown_;
    if (window_start_ <= 0) window_start_ = shown_s;
    if (shown_s - window_start_ < kWindowSeconds) return false;
    if (line) *line = Format(shown_s - window_start_, expected_ms, label);
    Reset(shown_s);
    return true;
  }

  static constexpr double kWindowSeconds = 5.0;
  static constexpr double kOnCapToleranceMs = 2.5;

 private:
  std::string Format(double seconds, double expected_ms, const char* label) const {
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), "[ios-pacing] %s %.1f s: %llu shown (%.1f/s) | on-screen interval ms:",
                          label ? label : "", seconds, static_cast<unsigned long long>(shown_),
                          seconds > 0 ? double(shown_) / seconds : 0.0);
    for (int i = 0; i < kBuckets && n > 0 && n < int(sizeof(buf)); ++i) {
      n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " %s:%llu", kBucketNames[i],
                         static_cast<unsigned long long>(bucket_[i]));
    }
    if (intervals_ > 0 && n > 0 && n < int(sizeof(buf))) {
      const double mean = sum_ / double(intervals_);
      const double var = std::max(0.0, sum_sq_ / double(intervals_) - mean * mean);
      n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " | mean %.1f sd %.1f min %.1f max %.1f",
                         mean, std::sqrt(var), min_, max_);
      if (expected_ms > 0 && n > 0 && n < int(sizeof(buf))) {
        n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " | %.1f %% within %.1f ms of the cap's %.1f ms",
                           100.0 * double(on_cap_) / double(intervals_), kOnCapToleranceMs, expected_ms);
      }
    }
    return std::string(buf);
  }

  void Reset(double start_s) {
    for (auto& b : bucket_) b = 0;
    shown_ = intervals_ = on_cap_ = 0;
    sum_ = sum_sq_ = max_ = min_ = 0;
    window_start_ = start_s;
  }

  std::mutex mutex_;
  double last_ = 0, window_start_ = 0;
  uint64_t bucket_[kBuckets] = {};
  uint64_t shown_ = 0, intervals_ = 0, on_cap_ = 0;
  double sum_ = 0, sum_sq_ = 0, max_ = 0, min_ = 0;
};

// The refresh a frame is aimed at, on the display link's grid.
struct LinkSlot {
  bool ok = false;        // false: the link's grid is unusable, keep the older logic
  double slot_s = 0;      // the refresh the frame should appear on
  double at_time_s = 0;   // presentDrawable:atTime: argument (a little before the refresh)
  int refreshes = 0;      // refreshes in one frame interval at this link rate (2 at 30 fps / 60 Hz)
  int late_by = 0;        // refreshes this frame slips beyond the cap's own spacing (0 = on time)
};

// vsync_s, period_s: a refresh time and the link's period (nfsmw::display::LatestVblank, in
//   seconds). now_s: the same clock now. ready_s: when the frame can be shown at the earliest
//   (now + the GPU's time + some slack). last_slot_s: the slot of the previous frame (0 = none).
// interval_s: the cap's frame interval (1/30). The slot is a whole number of refreshes after
//   last_slot_s (the cap's spacing) or later when the frame is not ready by then; a frame that
//   is early waits for its slot, one that is late takes the first refresh after it is ready.
inline LinkSlot ChooseLinkSlot(double vsync_s, double period_s, double now_s, double ready_s,
                               double last_slot_s, double interval_s) {
  LinkSlot r;
  if (!(vsync_s > 0) || !(period_s > 0.003) || !(period_s < 0.040) || !(interval_s > 0)) return r;
  if (now_s - vsync_s > 0.25 || vsync_s - now_s > 0.25) return r;  // the link stopped, or a bad clock
  const double per_frame = interval_s / period_s;
  const double whole = std::round(per_frame);
  if (whole < 1.0 || std::fabs(per_frame - whole) > 0.08) return r;  // 33.3 ms on a 48 Hz link: no grid
  r.refreshes = int(whole);
  double earliest = ready_s;
  const double cap_slot = last_slot_s > 0 && last_slot_s < now_s + 4.0 * interval_s
                              ? last_slot_s + (whole - 0.5) * period_s
                              : 0.0;
  earliest = std::max(earliest, cap_slot);
  // The first refresh at or after `earliest` (a 1 us tolerance for the float rounding).
  const double j = std::ceil((earliest - vsync_s) / period_s - 1e-6);
  r.slot_s = vsync_s + j * period_s;
  const double margin = std::min(0.004, 0.25 * period_s);
  r.at_time_s = r.slot_s - margin;
  if (cap_slot > 0) {
    r.late_by = std::max(0, int(std::lround((r.slot_s - (last_slot_s + whole * period_s)) / period_s)));
  }
  r.ok = true;
  return r;
}

}  // namespace nfsmw::cadence
