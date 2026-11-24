// mcla_t_present_interval (A5): the game's own presentation interval, lowered to 1 for a 60 fps run
// (docs/mcla/TIMING.md section 11; HOOKS_SPEC A5). Only with --mcla_sim60=true (or an explicit
// --mcla_present_interval=1..4); off = the game's own code.
//
// Why. The shipped game presents every SECOND vblank (interval 2, render-thread mode; TIMING 1.3).
// The guest D3D enforces it itself, in the command stream, not in the host:
//   * D3DDevice_Present (sub_82419E90 = `li r4,0 ; b sub_824199B0`, called right before every
//     D3DDevice_Swap, sub_82419E98) emits SCRATCH_REG1 = 1 (the CP writes it back to [shared+4]), a
//     WAIT_REG_MEM "memory [shared+4] == 1", an INTERRUPT carrying the callback sub_82419818 with the
//     interval in bits 8..11 of its argument, and a WAIT_REG_MEM "memory [shared+4] == 0";
//   * the guest interrupt handler (sub_82411478, VdSetGraphicsInterruptCallback) runs the callback on
//     the command-processor thread: target vblank = max(current, last target + interval) (+1 when the
//     frame is more than the swap-early percentage into the vblank); a target in the future is queued
//     ([dev+16572..], [dev+16704] entries), the vblank handler (sub_82419718, source 0) pops it when
//     the vblank counter [dev+16548] reaches the target and writes [shared+4] = 0;
//   * so the CP sits in the second WAIT_REG_MEM until the target vblank (the [cp-wait] census line:
//     `mem <shared+4>|2 func 3 ref 0`, one a frame, thousands of laps); the fence the guest then waits
//     for (Swap's BlockOnFence on [dev+14940]) is behind it, hence the ~28 ms "fence block" of every
//     frame. At the SDK's 60 Hz guest vblank an interval of 2 is exactly 30.0 frames a second with
//     zero host pacer sleeps (dev004: 150 frames in 5.00 s).
//
// Site 0x82419AA0 in sub_824199B0, AFTER `li r11,2` (reached only when the device's presentation
// interval [dev+0x351C] == 2; the next instruction is `b 0x82419AB4`, which builds the callback
// argument from r11). The hook only ever LOWERS the interval:
//   interval = min(2, round(guest vblank Hz / wanted fps)), wanted fps = 60 when the frame cap is
//   60 or more (or unknown), 30 at a cap of 30 or less.
//   60 Hz vblank, cap 60 -> 1;  60 Hz, cap 30 -> 2 (shipped).
// P1 (TIMING.md section 15): on a 120 Hz guest vblank the interval is exact, wanted fps 120 above a 60 cap:
//   120 Hz, cap 120 -> 1 (120 fps);  120 Hz, cap 60 (the 120 -> 60 tier) -> 2;  120 Hz, cap 30 -> 4.
// Swaps with no engine-timer frame since the previous swap (legals, logos, blocking loads: TIMING 4.5)
// keep the shipped 2, otherwise those loops would run at double speed.
//
// Frame cap (W3.3): the same site is where the host holds the frame rate, because it runs on the thread that presents,
// once per swap, before the swap packet is written, and the SDK has no swap callback of its own. The first thing the
// hook does, whatever the interval mode, is mcla::sim::PaceAtSwap() (mcla_t_common.cpp): with --mcla_fps_cap=30|60|120
// it holds the swap to that grid (src/host/frame_cap.h), with the default 0 it returns at once. The idea and the site
// are Renan Queiroz's mcla-recomp (GPL-3, src/frame_timing.cpp:848-863, config/frame_timing.toml:41-47).
//
// Guest vblank rate: the SDK's video_mode_refresh_rate cvar (60 Hz unless set).
//
// Off: --mcla_present_interval=0 (always the game's 2), --mcla_sim60=false (default, auto mode), or
// MCLA_HOOK_SIM_PRESENT_INTERVAL=0 at build time (which also removes the frame cap: no swap hook, no limiter).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

REXCVAR_DEFINE_INT32(mcla_present_interval, -1, "MCLA",
                     "MCLA: the game's presentation interval (vblanks per swap; shipped 2). -1 = auto: "
                     "lowered to 1 for a 60 fps cap when mcla_sim60 is on and an engine-timer frame ran "
                     "since the last swap; 0 = never touched; 1..4 = force that value on every swap "
                     "(diagnostics: without mcla_sim60 the world then runs at double speed)");

namespace {

mcla::hooks::Registration g_registration(
    "present-interval", "mcla_present_interval", MCLA_HOOK_SIM_PRESENT_INTERVAL != 0,
    [] {
      const int32_t mode = REXCVAR_GET(mcla_present_interval);
      return mode > 0 || (mode < 0 && mcla::sim::Enabled()) || mcla::sim::HostCapSetting() > 0;
    },
    /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_PRESENT_INTERVAL

namespace {

constexpr int64_t kReportNs = 10'000'000'000;

std::atomic<uint64_t> g_last_timer_frames{0};
std::atomic<uint64_t> g_swaps{0}, g_lowered{0}, g_untimed{0}, g_capped30{0}, g_forced{0};
std::atomic<int64_t> g_report_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The guest vblank rate in Hz: the SDK's video_mode_refresh_rate cvar (default 60), which drives the vblank
// thread (graphics_system.cpp) and the rate the guest reads (xboxkrnl_video.cpp, clamped to 24..240 there too).
double GuestVblankHz() {
  const double rate = rex::cvar::Query<double>("video_mode_refresh_rate");
  return rate > 0.0 ? std::clamp(rate, 24.0, 240.0) : 60.0;
}

void Report(int64_t now_ns, double vblank_hz, int cap) {
  int64_t since = g_report_ns.load(std::memory_order_relaxed);
  if (since == 0) {
    g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed);
    return;
  }
  if (now_ns - since < kReportNs || !g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t swaps = g_swaps.exchange(0), lowered = g_lowered.exchange(0), untimed = g_untimed.exchange(0),
                 capped30 = g_capped30.exchange(0), forced = g_forced.exchange(0);
  REXLOG_INFO(
      "[mcla-present] {:.0f} s: {} swaps through the interval site | changed: {} | the game's 2 kept: "
      "{} (no timer frame since the last swap: {}, cap 30: {}) | forced: {} | guest vblank {:.0f} Hz, "
      "cap {}",
      double(now_ns - since) / 1e9, swaps, lowered, untimed + capped30, untimed, capped30, forced, vblank_hz,
      cap > 0 ? cap : 60);
}

}  // namespace

void mcla_hook_present_interval(PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  // The host frame cap first: it only waits (mcla_fps_cap > 0) and never touches r11.
  mcla::sim::PaceAtSwap();
  const int32_t mode = REXCVAR_GET(mcla_present_interval);
  if (mode == 0) {
    return;
  }
  const int shipped = int(r11.u32);  // 2: the only value that reaches this site
  g_swaps.fetch_add(1, std::memory_order_relaxed);
  if (mode > 0) {
    const int forced = std::clamp<int>(mode, 1, 4);
    g_forced.fetch_add(1, std::memory_order_relaxed);
    r11.u64 = uint64_t(forced);
    return;
  }
  if (!mcla::sim::Enabled()) {
    return;
  }
  // A swap with no engine-timer frame since the previous one (legals, logos, blocking loads) is paced
  // by the interval alone: keep the shipped value there.
  const uint64_t timer_frames = mcla::FrameCount();
  const uint64_t previous = g_last_timer_frames.exchange(timer_frames, std::memory_order_relaxed);
  const int cap = mcla::sim::EffectiveCap();
  const double vblank_hz = GuestVblankHz();
  int chosen = shipped;
  if (timer_frames == previous) {
    g_untimed.fetch_add(1, std::memory_order_relaxed);
  } else {
    // P1: 120 at a 120 Hz guest vblank (interval 1), 60 -> 2, 30 -> 4 there; at 60 Hz only ever lowered (as before).
    chosen = mcla::sim::math::PresentInterval(vblank_hz, cap, shipped);
    if (chosen != shipped) {
      g_lowered.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_capped30.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (chosen != shipped) {
    r11.u64 = uint64_t(chosen);
  }
  Report(NowNs(), vblank_hz, cap);
}

#else  // MCLA_HOOK_SIM_PRESENT_INTERVAL == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_present_interval(PPCRegister&) {}

#endif
