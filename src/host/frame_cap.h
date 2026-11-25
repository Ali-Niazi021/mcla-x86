// mcla-x86 host: the frame cap. A frame limiter that holds the guest's swap to an even 30, 60 or 120 fps grid, with a
// high-resolution waitable timer for most of the wait and a QPC spin for the tail (win32_time.h). PLAN 3.6, W3.3.
//
// WHERE IT RUNS. Nothing in the SDK (ReXGlue v0.10.0 + the patch series) lets a host act at the swap: VdSwap, the
// command processor's IssueSwap and the presenter have no callback, and the driver limiter of the low-latency patch
// (LowLatency::SetFrameLimit) needs NVIDIA Reflex or AMD Anti-Lag 2 and an engine-timer hook that nobody here calls.
// What there is, is the game layer's own mid-asm hook in the guest's D3DDevice_Present, `mcla_hook_present_interval`
// (0x82419AA0, mcla/hooks/mcla_t_present_interval.cpp). It runs on the thread that presents, once per swap, before the
// swap packet is written. That is where Renan Queiroz's mcla-recomp holds its frame limiter too (GPL-3: src/
// frame_timing.cpp:848-863, config/frame_timing.toml:41-47). The hook calls mcla::sim::PaceAtSwap()
// (mcla/hooks/mcla_t_common.cpp), which decides the rate and calls PaceAtSwap(fps) below.
//
// THE CVARS (defined in mcla_app.cpp):
//   mcla_fps_cap          0 (default: no host limiter, the shipped behaviour), 30, 60 or 120. Live.
//   mcla_fps_cap_spin     wait by spinning only (mode=spin): a whole core, for A/B of the timer's precision. Live.
//   mcla_fps_cap_tail_us  the part of each wait that is spun after the timer (default 250 us, R7), for the limiter and
//                         for the sim60 game-thread pacer (mcla_sim_pace). The high-resolution timer wakes late by
//                         0.1 to 0.8 ms in one wait of ten (measured, scratch W3.3: lateness p99 0.45-0.85 ms with a
//                         300-400 us tail, 0.04-0.11 ms with 1000 us). Live.
//
// HOW IT INTERACTS WITH THE GAME LAYER (mcla/hooks/mcla_t_common.h has the full table):
//   * mcla_sim_cap (0, 30, 60, 120) is the cap the WORLD assumes with mcla_sim60: the quantiser's grid, the game-thread
//     pacer, the present interval. 0 means "the host's cap" (mcla_fps_cap), and when that is 0 too, 60.
//   * The limiter's rate is mcla_fps_cap, lowered to the world's effective cap while mcla_sim60 is on (a 60 -> 30 hold
//     lowers it to 30, a 120 -> 60 tier to 60, an explicit mcla_sim_cap too). It is never raised.
//   * Without mcla_sim60 the game keeps its fixed 1/30 s step and its present interval of 2: only 30 is a valid cap
//     (M4); 60 and 120 then never wait, because the guest's own swaps are already 33 ms apart.
//   * The guest vblank sets the highest rate the guest can present at: 60 Hz by default, 120 Hz with
//     --video_mode_refresh_rate=120 (needed for a cap of 120).
//
// THE GRID (frame_pacer.h): slots one period apart from the first paced swap; a swap is released at its slot or at its
// arrival if that is later, so the average rate is exactly the cap and a late frame is paid back by a short one. The
// phase of the grid is never moved by a late frame, and the period is 25 ppm short of nominal: under the guest's
// vblank the swaps arrive right after a vblank, a limiter that anchored at a late frame would hold every later swap
// until its vblank is gone, and one that is a few ppm slower than the vblank would do so after some minutes. The
// limiter is a ceiling: with a vblank-locked guest it hardly ever waits ("waited" in the second log line), it acts
// when the swaps come faster than the cap.
//
// THE LOG (tag [mcla-cap]):
//   [mcla-cap] cap=<n> mode=<timer|spin>   at start (cap=0 mode=off without a cap), and when the rate in force changes
//   [mcla-cap] period p50=<ms> p99=<ms> n=<count>   every 10 s of paced swaps: the time between two released swaps
//   [mcla-cap]   cap <n> mode <m>: max, waited <w> of <f> swaps (slept <ms>), grid skips, gaps over 1 s, window
//
// Everything is inline: the cvars are defined in mcla_app.cpp (a cvar needs a translation unit of its own), the hook
// side (mcla_t_common.cpp) includes this header from the same executable.
#pragma once

#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "frame_pacer.h"
#include "win32_time.h"

REXCVAR_DECLARE(int32_t, mcla_fps_cap);
REXCVAR_DECLARE(bool, mcla_fps_cap_spin);
REXCVAR_DECLARE(int32_t, mcla_fps_cap_tail_us);

namespace mcla_host {

// mcla_fps_cap as the host understands it: 30, 60 or 120; anything else (0, or a value that is not a cap) is 0 = no
// host limiter. One relaxed load.
inline int FrameCapSetting() {
  const int32_t cap = REXCVAR_GET(mcla_fps_cap);
  return cap == 30 || cap == 60 || cap == 120 ? cap : 0;
}

// The spin tail of the pacers' waits in seconds (mcla_fps_cap_tail_us).
inline double PacerSpinTailSeconds() {
  const int32_t us = REXCVAR_GET(mcla_fps_cap_tail_us);
  return (us > 0 ? us : 1) * 1e-6;
}

// The wait mode now: spin only (the cvar, or this thread has no waitable timer) or timer + spin tail.
inline bool FrameCapSpinOnly() { return REXCVAR_GET(mcla_fps_cap_spin) || !WaitableTimerAvailable(); }

namespace frame_cap_detail {

constexpr int64_t kReportSeconds = 10;
constexpr double kLongGapMs = 1000.0;  // a load or a suspension, not a frame: counted apart

class Limiter {
 public:
  // Holds the calling thread until its swap's slot on the `fps` grid (fps > 0), then returns. Thread-safe: the slots
  // are taken in order under a lock, and no thread sleeps while holding it.
  void Pace(int fps) {
    const bool spin = FrameCapSpinOnly();
    const int64_t freq = static_cast<int64_t>(QpcFrequency());
    const int64_t arrived = static_cast<int64_t>(QpcNow());
    int64_t release = arrived;
    bool first = false, changed = false;
    int was = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        started_ = first = true;
        fps_ = fps;
        spin_ = spin;
        ResetWindow(arrived);
      } else if (fps != fps_ || spin != spin_) {
        changed = true;
        was = fps_;
        fps_ = fps;
        spin_ = spin;
        last_release_ = 0;  // no period is measured across a change of rate
        ResetWindow(arrived);
      }
      const FrameGrid::Release slot = grid_.Next(arrived, GridPeriodTicks(fps, freq));
      release = slot.at;
      skips_ += slot.skipped ? 1 : 0;
      if (release > arrived) {
        ++waits_;
        slept_ticks_ += release - arrived;
      }
    }
    if (first) {
      REXLOG_INFO("[mcla-cap] swap limiter running (first paced swap; the grid starts now)");
    }
    if (changed) {
      REXLOG_INFO("[mcla-cap] cap={} mode={} (was {})", fps, spin ? "spin" : "timer", was);
    }

    if (release > arrived) {
      if (spin) {
        SpinUntilQpc(static_cast<uint64_t>(release));
      } else {
        SleepUntilQpc(static_cast<uint64_t>(release), PacerSpinTailSeconds());
      }
    }

    const int64_t released = static_cast<int64_t>(QpcNow());
    Report report;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++frames_;
      if (last_release_ != 0) {
        const double ms = static_cast<double>(released - last_release_) * 1000.0 / static_cast<double>(freq);
        if (ms > kLongGapMs) {
          ++long_gaps_;
        } else {
          periods_.Add(ms);
        }
      }
      last_release_ = released;
      if (released - window_start_ >= kReportSeconds * freq) {
        report.due = true;
        report.window_s = static_cast<double>(released - window_start_) / static_cast<double>(freq);
        report.n = periods_.count();
        report.p50 = periods_.Percentile(0.50);
        report.p99 = periods_.Percentile(0.99);
        report.max = periods_.max();
        report.fps = fps_;
        report.spin = spin_;
        report.frames = frames_;
        report.waits = waits_;
        report.slept_ms = static_cast<double>(slept_ticks_) * 1000.0 / static_cast<double>(freq);
        report.skips = skips_;
        report.long_gaps = long_gaps_;
        ResetWindow(released);
      }
    }
    if (report.due) {
      REXLOG_INFO("[mcla-cap] period p50={:.2f} p99={:.2f} n={}", report.p50, report.p99, report.n);
      REXLOG_INFO("[mcla-cap]   cap {} mode {}: max {:.2f} ms, waited {} of {} swaps (slept {:.0f} ms), grid skips {}, "
                  "gaps over 1 s {}, window {:.1f} s",
                  report.fps, report.spin ? "spin" : "timer", report.max, report.waits, report.frames,
                  report.slept_ms, report.skips, report.long_gaps, report.window_s);
    }
  }

 private:
  struct Report {
    bool due = false;
    bool spin = false;
    int fps = 0;
    double window_s = 0.0, p50 = 0.0, p99 = 0.0, max = 0.0, slept_ms = 0.0;
    uint64_t n = 0, frames = 0, waits = 0, skips = 0, long_gaps = 0;
  };

  // mutex_ held.
  void ResetWindow(int64_t now) {
    periods_.Reset();
    window_start_ = now;
    frames_ = waits_ = skips_ = long_gaps_ = 0;
    slept_ticks_ = 0;
  }

  std::mutex mutex_;
  FrameGrid grid_;
  PeriodStats periods_;
  bool started_ = false;
  int fps_ = 0;
  bool spin_ = false;
  int64_t last_release_ = 0;
  int64_t window_start_ = 0;
  uint64_t frames_ = 0, waits_ = 0, skips_ = 0, long_gaps_ = 0;
  int64_t slept_ticks_ = 0;
};

inline Limiter& TheLimiter() {
  static Limiter limiter;
  return limiter;
}

}  // namespace frame_cap_detail

// Holds the calling thread to the `fps` grid; the first call after a pause or a change of rate starts a new grid and
// returns at once. For the thread that presents, once per swap, before the swap is issued. fps <= 0 does nothing.
inline void PaceAtSwap(int fps) {
  if (fps > 0) {
    frame_cap_detail::TheLimiter().Pace(fps);
  }
}

// The start-up line: the cap in force and the wait mode, plus what is wrong with the setup (a value that is not a cap,
// no high-resolution timer, a cap the guest vblank cannot reach). Once, from OnPostInitLogging.
inline void LogFrameCapStartup() {
  const int32_t raw = REXCVAR_GET(mcla_fps_cap);
  const int cap = FrameCapSetting();
  // Creates this thread's timer: whether a high-resolution one exists is a property of the system, not of the thread.
  const bool high_resolution = HighResolutionTimerAvailable();
  const bool spin = FrameCapSpinOnly();
  const bool sim60 = REXCVAR_QUERY(bool, mcla_sim60);
  const double vblank_hz = REXCVAR_QUERY(double, video_mode_refresh_rate);

  if (raw != 0 && cap == 0) {
    REXLOG_WARN("[mcla-cap] mcla_fps_cap={} is not 0, 30, 60 or 120: ignored", raw);
  }
  if (cap == 0) {
    REXLOG_INFO("[mcla-cap] cap=0 mode=off (no host limiter: the guest's present interval and vblank pace the swap)");
  } else if (spin) {
    REXLOG_INFO("[mcla-cap] cap={} mode=spin (QPC spin for the whole wait{}; limiter at the guest swap)", cap,
                REXCVAR_GET(mcla_fps_cap_spin) ? ", mcla_fps_cap_spin" : ": no waitable timer");
  } else {
    REXLOG_INFO("[mcla-cap] cap={} mode=timer ({}waitable timer + {} us QPC spin tail; limiter at the guest swap)", cap,
                high_resolution ? "high-resolution " : "", REXCVAR_GET(mcla_fps_cap_tail_us));
  }
  if (!high_resolution) {
    // The sim60 pacer (mcla_sim_pace) waits the same way, so this matters without a cap too.
    REXLOG_WARN("[mcla-cap] no high-resolution waitable timer: the pacers' waits end on the system timer grid, with a "
                "2 ms spin tail");
  }
  if (cap > 30 && !sim60) {
    REXLOG_INFO("[mcla-cap] mcla_sim60 is off: the game keeps its present interval of 2 and its 1/30 s step, so the "
                "guest swaps at 30 fps and a cap of {} never waits (use cap 30, or --mcla_sim60=true)", cap);
  }
  if (cap > 60 && vblank_hz > 0.0 && vblank_hz < 100.0) {
    REXLOG_WARN("[mcla-cap] cap={} needs a 120 Hz guest vblank (--video_mode_refresh_rate=120, now {:.0f} Hz): the "
                "guest cannot swap faster than its vblank",
                cap, vblank_hz);
  }
}

}  // namespace mcla_host
