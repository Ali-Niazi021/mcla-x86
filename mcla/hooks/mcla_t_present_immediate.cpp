// mcla_t_present_immediate (W4.5; docs/PLAN.md section 6 M5): the guest D3D's own "present immediately when late",
// switched on, and the census that shows what it does. Two mid-asm hooks, one build flag
// (MCLA_HOOK_SIM_PRESENT_IMMEDIATE), the pure rule in mcla_t_swap_target.h.
//
// Why (scratch/W4.4/REPORT.md). The guest D3D's swap callback sub_82419818 holds a late frame back one vblank: it
// does so when the frame arrives more than the present-immediate threshold (the Xbox 360 render state
// D3DRS_PRESENTIMMEDIATETHRESHOLD, a percentage of the vblank period) into the vblank period, and this game leaves the
// threshold at its default 0. So at 60 Hz with interval 1 every late frame waits for the next vblank: 33.3 ms, and the
// render thread (Swap's fence) and the GPU idle behind it. With a threshold of 100 the callback releases a late frame
// at once (target = the current vblank) and an on-time frame still waits for its real vblank: the guest vblank stays a
// real 60 Hz, the cadence returns to the vblank grid on the next frame, logos and loads are untouched.
//
// Hook 1, mcla_hook_present_immediate: 0x82419AC0 in sub_824199B0 (the Present prelude, run before every
// D3DDevice_Swap), AFTER `rlwinm r10,r10,9,25,31`. r10 then holds only the 7-bit threshold from [dev+11852]
// (`lwz r10,11852(r31)` at 0x82419AB4); the next instructions build the callback argument
// `(interval << 8) | threshold` (`or r11,r10,r11` at 0x82419AC8) and hand it to the INTERRUPT. Only that copy changes:
// the device state [dev+11852] and its getter are left alone. Not a branch target; every interval path reaches it.
//   --mcla_present_immediate=0   (default) untouched: the game's own threshold
//   --mcla_present_immediate=-1  auto: 100 on timed swaps while mcla_sim60 is on
//   --mcla_present_immediate=1..100  that threshold on timed swaps (diagnostics)
// "Timed" is published by the present-interval hook (A5, 0x82419AA0, mcla_t_present_interval.cpp), which runs a few
// instructions earlier on the same thread: a swap with no engine-timer frame since the previous one (legals, logos,
// blocking loads) keeps the shipped threshold, and so does a swap that never reached A5 (its device interval is not 2).
// Without A5 (MCLA_HOOK_SIM_PRESENT_INTERVAL=0) no swap is ever timed and this hook never acts.
//
// Hook 2, mcla_hook_swap_census (read-only): 0x82419920 in sub_82419818, `stw r11,16556(r31)` (word 917F40AC), on the
// command-processor thread, under the guest's spinlock [dev+16712]. At that point r8 = the last target, r30 = the
// interval, r10 = the vblank counter ([dev+16548], reloaded at 0x8241991C), r11 = the target the callback chose, r7 =
// the percentage into the vblank period, r27 = the threshold of the argument. Counts only, never writes; it classifies
// each swap (mcla_t_swap_target.h: on time / late, released at once / late, held to the next vblank) and logs
//   [mcla-present] immediate: shipped threshold T, auto -> 100 on timed swaps          once, at the first swap
//   [mcla-swap] on time A | late, released at once B | late, held to the next vblank C | ...   every 10 s
// --mcla_swap_census=false switches the census off.
//
// Off: --mcla_present_immediate=0 (default; the census still counts), MCLA_HOOK_SIM_PRESENT_IMMEDIATE=0 at build time
// (both hooks do nothing, the build is as before).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"
#include "mcla_t_swap_target.h"

REXCVAR_DEFINE_INT32(mcla_present_immediate, 0, "MCLA",
                     "MCLA: the present-immediate threshold of the guest's swaps (D3DRS_PRESENTIMMEDIATETHRESHOLD, a "
                     "percentage of the vblank period; shipped 0: a frame that arrives late waits for the next "
                     "vblank, 33.3 ms at 60 Hz). 0 = untouched (the game's own); -1 = auto: 100 on timed swaps while "
                     "mcla_sim60 is on, so a late frame is released at once and an on-time frame still waits for "
                     "its vblank; 1..100 = force that threshold on timed swaps (diagnostics). Live");

REXCVAR_DEFINE_BOOL(mcla_swap_census, true, "MCLA",
                    "MCLA: the swap census: every 10 s one [mcla-swap] line with how many swaps were on time, late "
                    "and released at once, or late and held to the next vblank (read-only)");

namespace {

mcla::hooks::Registration g_registration_immediate(
    "present-immediate", "mcla_present_immediate", MCLA_HOOK_SIM_PRESENT_IMMEDIATE != 0,
    [] {
      const int32_t mode = REXCVAR_GET(mcla_present_immediate);
      return mode > 0 || (mode < 0 && mcla::sim::Enabled());
    },
    /*default_on=*/false);

mcla::hooks::Registration g_registration_census("swap-census", "mcla_swap_census", MCLA_HOOK_SIM_PRESENT_IMMEDIATE != 0,
                                                [] { return REXCVAR_GET(mcla_swap_census); });

}  // namespace

#if MCLA_HOOK_SIM_PRESENT_IMMEDIATE

namespace {

namespace math = mcla::sim::math;

constexpr int64_t kReportNs = 10'000'000'000;
constexpr uint32_t kUnseen = ~uint32_t(0);

// Hook 1 -> census: the threshold the game ships ([dev+11852] bits), whatever hook 1 then did to the argument.
std::atomic<uint32_t> g_shipped_threshold{kUnseen};
std::atomic<uint64_t> g_raised{0};  // swaps whose threshold hook 1 changed since the last report

// The census (one writer: the command-processor thread; relaxed atomics so a second caller cannot corrupt it).
std::atomic<uint64_t> g_on_time{0}, g_late_at_once{0}, g_late_held{0}, g_other{0};
std::array<std::atomic<uint32_t>, math::kSwapPctMax + 1> g_late_pct{};  // r7 of the late swaps
std::atomic<uint32_t> g_interval{0};
std::atomic<int64_t> g_report_ns{0};
std::atomic<bool> g_logged_threshold{false};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The first swap's line: the threshold the game ships, and what the auto mode does to it.
void LogShippedThreshold(uint32_t arg_threshold) {
  if (g_logged_threshold.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  uint32_t shipped = g_shipped_threshold.load(std::memory_order_relaxed);
  if (shipped == kUnseen) {
    shipped = arg_threshold;  // hook 1 has not run (cannot happen: it runs before the swap's packet is written)
  }
  REXLOG_INFO("[mcla-present] immediate: shipped threshold {}, auto -> {} on timed swaps (mcla_present_immediate={})",
              shipped, math::kImmediateThreshold, REXCVAR_GET(mcla_present_immediate));
}

// A percentile (0..1) of the late swaps' r7, from the histogram; 0 when there is none.
uint32_t PctPercentile(const std::array<uint32_t, math::kSwapPctMax + 1>& hist, uint64_t total, double p) {
  if (total == 0) {
    return 0;
  }
  const uint64_t want = std::max<uint64_t>(1, uint64_t(std::ceil(p * double(total))));
  uint64_t seen = 0;
  for (uint32_t i = 0; i < hist.size(); ++i) {
    seen += hist[i];
    if (seen >= want) {
      return i;
    }
  }
  return math::kSwapPctMax;
}

void Report(int64_t now_ns) {
  int64_t since = g_report_ns.load(std::memory_order_relaxed);
  if (since == 0) {
    g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed);
    return;
  }
  if (now_ns - since < kReportNs || !g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t on_time = g_on_time.exchange(0), at_once = g_late_at_once.exchange(0), held = g_late_held.exchange(0),
                 other = g_other.exchange(0), raised = g_raised.exchange(0);
  std::array<uint32_t, math::kSwapPctMax + 1> hist{};
  uint64_t late = 0;
  for (size_t i = 0; i < hist.size(); ++i) {
    hist[i] = g_late_pct[i].exchange(0);
    late += hist[i];
  }
  const uint32_t shipped = g_shipped_threshold.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[mcla-swap] on time {} | late, released at once {} | late, held to the next vblank {} | other {} | {} swaps "
      "in {:.0f} s | late swaps at p50 {} % p90 {} % into the vblank period | interval {} | shipped threshold {} | "
      "threshold raised on {} swaps",
      on_time, at_once, held, other, on_time + at_once + held + other, double(now_ns - since) / 1e9,
      PctPercentile(hist, late, 0.5), PctPercentile(hist, late, 0.9), g_interval.load(std::memory_order_relaxed),
      shipped == kUnseen ? 0u : shipped, raised);
}

}  // namespace

// Hook 1: r10 = the 7-bit threshold of the swap about to be written (after `rlwinm r10,r10,9,25,31`).
void mcla_hook_present_immediate(PPCRegister& r10) {
  MCLA_HOOK_ENTER();
  const uint32_t shipped = r10.u32 & 0x7F;
  g_shipped_threshold.store(shipped, std::memory_order_relaxed);
  // Always consumed: a flag published for this swap must never reach the next one.
  const bool timed = mcla::sim::TakeSwapTimed();
  const int32_t mode = REXCVAR_GET(mcla_present_immediate);
  if (mode == 0 || !timed) {
    return;
  }
  uint32_t threshold = math::kImmediateThreshold;
  if (mode > 0) {
    threshold = std::min<uint32_t>(uint32_t(mode), math::kImmediateThreshold);
  } else if (!mcla::sim::Enabled()) {
    return;  // auto mode follows mcla_sim60
  }
  r10.u64 = (r10.u64 & ~uint64_t(0x7F)) | threshold;
  g_raised.fetch_add(1, std::memory_order_relaxed);
}

// Hook 2: the classification of one swap; counts and logs only.
void mcla_hook_swap_census(PPCRegister& r7, PPCRegister& r8, PPCRegister& r10, PPCRegister& r11, PPCRegister& r27,
                           PPCRegister& r30) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_swap_census)) {
    return;
  }
  const uint32_t interval = r30.u32;
  switch (math::ClassifySwap(r8.u32, interval, r10.u32, r11.u32)) {
    case math::SwapClass::kOnTime:
      g_on_time.fetch_add(1, std::memory_order_relaxed);
      break;
    case math::SwapClass::kLateAtOnce:
      g_late_at_once.fetch_add(1, std::memory_order_relaxed);
      g_late_pct[std::min(r7.u32, math::kSwapPctMax)].fetch_add(1, std::memory_order_relaxed);
      break;
    case math::SwapClass::kLateHeld:
      g_late_held.fetch_add(1, std::memory_order_relaxed);
      g_late_pct[std::min(r7.u32, math::kSwapPctMax)].fetch_add(1, std::memory_order_relaxed);
      break;
    case math::SwapClass::kOther:
      g_other.fetch_add(1, std::memory_order_relaxed);
      break;
  }
  g_interval.store(interval, std::memory_order_relaxed);
  LogShippedThreshold(r27.u32);
  Report(NowNs());
}

#else  // MCLA_HOOK_SIM_PRESENT_IMMEDIATE == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_present_immediate(PPCRegister&) {}

void mcla_hook_swap_census(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
