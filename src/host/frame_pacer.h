// mcla-x86 host: the arithmetic of the frame limiter (frame_cap.h). No SDK, no Win32, no clock of its own: every
// function is a function of the numbers it is given, so a throwaway program can check it (and the unit tests could).
//
// Units: FrameGrid works in whatever tick the caller's clock has (QPC ticks in the host, nanoseconds in a test);
// PeriodStats works in milliseconds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mcla_host {

// One frame at `fps`, in ticks of a clock that runs `ticks_per_second`, rounded to the nearest tick (a 10 MHz QPC:
// 166'667 ticks at 60 fps, 0.3 ticks = 30 ns a frame off). 0 for fps <= 0.
inline int64_t PeriodTicks(int fps, int64_t ticks_per_second) {
  return fps > 0 ? (ticks_per_second + fps / 2) / fps : 0;
}

// The limiter's grid period: PeriodTicks less 25 ppm (1.5 us a frame at 60 fps). The cap is a ceiling. A guest vblank
// of the same nominal rate (interval 1 at 60 fps, interval 2 at 30) ticks on its own rounding of the period, and a
// grid even a few ppm SLOWER than it would see the frames arrive earlier and earlier against its slots and hold them
// longer and longer, until a held frame misses its vblank. A grid a few ppm FASTER sees them arrive later and later
// and never waits (it skips a slot about every 11 minutes at 60 fps, `skipped`): the limiter then only acts when the
// frames come faster than the cap, which is its job.
inline int64_t GridPeriodTicks(int fps, int64_t ticks_per_second) {
  const int64_t period = PeriodTicks(fps, ticks_per_second);
  return period - period / 40000;
}

// The limiter's deadline grid: slots one period apart, the first at the first frame's arrival. A frame is released at
// its slot, or at its arrival if that is later: a frame that comes early waits, a frame that comes late is released at
// once and the slots do not move, so one late frame is paid back by a short one and the average stays at the cap.
//
// The PHASE of the grid is never moved by a late frame. Under the guest's vblank (interval 1 or 2) the frames arrive
// right after a vblank, and a limiter that restarted its grid at a hitch frame's arrival would anchor every later slot
// at that frame's phase and delay the early frames to it, up to a whole vblank: the guest's present callback would
// then find its target vblank gone and add one. So a frame that is a whole period or more late (a hitch, a load) skips
// the slots it missed and is released at once, on the grid's phase (`skipped`). Only a slot more than two periods in
// the future (the clock went back) or a change of period restarts the grid, at the frame's arrival.
class FrameGrid {
 public:
  struct Release {
    int64_t at;      // the frame is released at this time (<= now: no wait)
    bool skipped;    // slots were skipped (the frame was a period or more late) or the grid restarted after a clock jump
  };

  // `now`: when the frame arrived. `period`: ticks per frame, > 0. A different period than the last call (the cap
  // changed) restarts the grid, and so does the first call.
  Release Next(int64_t now, int64_t period) {
    if (!have_ || period != period_) {
      have_ = true;
      period_ = period;
      deadline_ = now;
      return {now, false};
    }
    int64_t deadline = deadline_ + period;
    bool skipped = false;
    if (deadline - now > 2 * period) {
      deadline = now;
      skipped = true;
    } else if (now - deadline >= period) {
      deadline += ((now - deadline) / period) * period;  // the latest slot at or before `now`
      skipped = true;
    }
    deadline_ = deadline;
    return {deadline, skipped};
  }

  void Reset() { have_ = false; }

 private:
  bool have_ = false;
  int64_t period_ = 0;
  int64_t deadline_ = 0;
};

// The frame periods of one report window, in ms, kept exactly (a window is 10 s: at most a few thousand frames), so
// the percentiles are the real nearest-rank values and not a bucket edge. When the store is full a sample is counted
// but not kept, and the percentiles are over the kept ones.
class PeriodStats {
 public:
  static constexpr size_t kCapacity = 1u << 14;

  PeriodStats() { samples_.reserve(kCapacity); }

  void Reset() {
    samples_.clear();
    count_ = 0;
    max_ms_ = 0.0;
  }

  void Add(double ms) {
    if (!(ms >= 0.0)) {
      return;
    }
    ++count_;
    max_ms_ = std::max(max_ms_, ms);
    if (samples_.size() < kCapacity) {
      samples_.push_back(ms);
    }
  }

  uint64_t count() const { return count_; }
  double max() const { return max_ms_; }

  // Nearest rank: the ceil(p * n)-th smallest of the n kept samples. 0 when there is none. Reorders the store.
  double Percentile(double p) {
    if (samples_.empty()) {
      return 0.0;
    }
    const size_t n = samples_.size();
    const size_t rank = std::clamp<size_t>(static_cast<size_t>(std::ceil(p * static_cast<double>(n) - 1e-9)), 1, n);
    std::nth_element(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(rank - 1), samples_.end());
    return samples_[rank - 1];
  }

 private:
  std::vector<double> samples_;
  uint64_t count_ = 0;
  double max_ms_ = 0.0;
};

}  // namespace mcla_host
