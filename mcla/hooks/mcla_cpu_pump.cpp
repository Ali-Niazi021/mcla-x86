// mcla_cpu_pump: statistics of the pump thread's rounds, and an optional wake-on-demand
// (HOOKS_SPEC E3).
//
// Site 0x821449C8 in sub_82144888, hooked AFTER `80610054 lwz r3,84(r1)`: the end of one round of
// the pump (timer name `audioFrameTimer`, one round per at most 7 ms: the 7.0 at 0x820036EC). The
// round has just freed the ring slot (mcla_cpu_poll_release.cpp), run the audio/engine callbacks, and
// r3 = whole milliseconds left of the 7.0 ms budget (0..7, 0 = already over). Then the game does
// `bl 0x821C91B8` = Sleep(r3) and returns to its loop; a hook that returns true jumps to 0x821449D0,
// the epilogue, and so skips the Sleep. The code only runs while the byte 0x827D1998 is non-zero
// (static 1).
//
// Two modes, two cvars:
//   mcla_cpu_pump = true (default ON)      statistics only: rounds, period between rounds, sleep asked
//                                          (the "[mcla-cpu] 10 s" line). Returns false: the game's own
//                                          Sleep(r3) runs. Observable results unchanged.
//   mcla_cpu_pump_wake = true (default OFF) wake-on-demand: the Sleep(r3) becomes a wait on PumpEvent
//                                          for at most r3 ms, ended early when the main thread's ring
//                                          wait (mcla_cpu_poll_wait.cpp) asks for a round. The pump then
//                                          runs its next round at once and frees the slot. This changes
//                                          WHEN rounds run (not how often over a second at 30 fps: a ring
//                                          that is never full never asks), so it stays OFF until the
//                                          mac run shows no [xma-work]/[coreaudio] underruns (Renan: a
//                                          pump that runs its rounds too hard starved audio on 4 CPUs).
//
// Off: --mcla_cpu_pump=false, or MCLA_HOOK_CPU_PUMP=0 at build time (hooks.toml keeps linking).
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_cpu_common.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_cpu_pump, true, "MCLA",
                    "MCLA: count the pump thread's rounds (period, sleep asked) in the [mcla-cpu] "
                    "10 s line; the game's own Sleep runs (false: the hook does nothing)");
REXCVAR_DEFINE_BOOL(mcla_cpu_pump_wake, false, "MCLA",
                    "MCLA experiment: the pump's end-of-round sleep becomes a wait that the main "
                    "thread's full-ring wait cuts short (wake-on-demand). Needs mcla_cpu_poll_wait; "
                    "default off: it changes when audio rounds run");

namespace {

mcla::hooks::Registration g_registration("cpu", "mcla_cpu_pump", MCLA_HOOK_CPU_PUMP != 0,
                                         [] { return REXCVAR_GET(mcla_cpu_pump); });

}  // namespace

#if MCLA_HOOK_CPU_PUMP

namespace {

constexpr uint32_t kBudgetMs = 7;                 // 0x820036EC: the pump's beat
constexpr uint64_t kLateRoundUs = 10'000;         // a round more than this after the last one is late
constexpr uint32_t kMaxAskMs = 1000;              // a larger r3 is not the clamped 0..7: ignore it

std::atomic<int64_t> g_last_ns{0};

}  // namespace

bool mcla_hook_cpu_pump(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_cpu_pump)) {
    return false;
  }
  using namespace mcla::cpu;
  Counters& stats = Stats();
  const int64_t now = NowNs();
  const int64_t last = g_last_ns.exchange(now, std::memory_order_relaxed);
  stats.pump_rounds.fetch_add(1, std::memory_order_relaxed);
  if (last != 0) {
    const uint64_t period_us = uint64_t((now - last) / 1000);
    stats.pump_period_us.fetch_add(period_us, std::memory_order_relaxed);
    AtomicMax(stats.pump_period_max_us, period_us);
    if (period_us > kLateRoundUs) {
      stats.pump_late.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const uint32_t ask_ms = r3.u32;
  if (ask_ms <= kMaxAskMs) {
    stats.pump_ask_ms.fetch_add(ask_ms, std::memory_order_relaxed);
    if (ask_ms == 0) {
      stats.pump_ask_zero.fetch_add(1, std::memory_order_relaxed);
    }
  }
  MaybeReport(now);

  if (!PumpWakeOn() || ask_ms == 0 || ask_ms > kBudgetMs) {
    return false;  // the game's own Sleep(r3)
  }

  // Wake mode. Register, then look at the request and the event word (no lost ask; see the header).
  PumpWaiters().fetch_add(1, std::memory_order_seq_cst);
  const uint32_t seen = PumpEvent().load(std::memory_order_seq_cst);
  bool early = true;
  if (!PumpRequest().exchange(false, std::memory_order_seq_cst)) {
    early = WaitWord(PumpEvent(), seen, uint64_t(ask_ms) * 1'000'000);
    PumpRequest().store(false, std::memory_order_seq_cst);
  }
  PumpWaiters().fetch_sub(1, std::memory_order_seq_cst);

  const int64_t end = NowNs();
  const uint64_t slept_us = uint64_t((end - now) / 1000);
  stats.pump_waits.fetch_add(1, std::memory_order_relaxed);
  (early ? stats.pump_early : stats.pump_timeouts).fetch_add(1, std::memory_order_relaxed);
  if (uint64_t(ask_ms) * 1000 > slept_us) {
    stats.pump_cut_us.fetch_add(uint64_t(ask_ms) * 1000 - slept_us, std::memory_order_relaxed);
  }
  return true;  // skip `bl Sleep(r3)`: the epilogue at 0x821449D0
}

#else  // MCLA_HOOK_CPU_PUMP == 0: keeps hooks.toml linking, does nothing.

bool mcla_hook_cpu_pump(PPCRegister&) { return false; }

#endif
