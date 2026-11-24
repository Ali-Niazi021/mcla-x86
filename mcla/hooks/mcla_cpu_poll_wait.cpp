// mcla_cpu_poll_wait: the main thread's ring-space wait sleeps until the release instead of looping
// around Sleep(0) (HOOKS_SPEC E1; Renan's `poll_wait`: 61-75 % less main-thread CPU on a PC).
//
// Site 0x82148AB0 in sub_82148A10(ctx = r3 -> r31, r4), the producer of the frame pipeline's rings
// (see mcla_cpu_common.h). Walking the function:
//
//     ring    = 0x82836814 + ((byte[ctx+199] >> 2) & 0x38)     r30 = 0x82836814, r8 = the offset
//     r28     = BE32[ring+0]  write index        r29 = BE32[ring+4]  read index
//     r27     = (r28 + 1) % 3  the slot it wants (0x82148A78)
//     if (r27 == r29 && byte[0x82836810] == 0) {                // ring full, waiting not given up yet
//         r26 = clock_us() + 1000                               // sub_82144FC8, 32-bit microseconds
//         while (r3 = clock_us(), r3 < r26) {                   // 0x82148AA8
//             0x82148AB0  li r3,0 ; bl Sleep(0)                <- hooked BEFORE li
//             0x82148AB8  r29 = BE32[ring+4] (ctx byte re-read), r3 = clock_us()
//             if (r27 != r29) break;
//         }
//         if (still full) byte[0x82836810] = 1                  // gives up for good
//     }
//
// The hook replaces `li r3,0 ; bl Sleep(0)` (it returns true: jump to 0x82148AB8) by a real wait:
//   * re-reads the read index; if the slot is already free it just returns (the guest re-checks);
//   * otherwise waits on RingEvent (bumped by mcla_cpu_poll_release.cpp right after the game stored
//     the read index, HOOKS_SPEC E2) for the rest of the game's own 1 ms window, r26 - r3 us.
// The game's loop is untouched: it re-reads the index and the clock after every return, so a missed
// wake costs at most the remaining window and the "give up" rule at the deadline is the shipped one.
//
// Behaviour: the same decisions as the shipped loop (wait, leave when freed, give up at 1 ms), only
// the waiting changes from repeated Sleep(0) laps (a 100 us host sleep or a bare yield, SDK
// xthread.cpp) to one sleep ended by the release. Observable results are unchanged, so the default
// is ON. It needs E2 (without the release nobody wakes it: the hook then does nothing).
//
// Wake mode of the pump (mcla_cpu_pump.cpp, default off): before it waits, the hook asks the pump
// thread to run its next round now (the pump frees the slot the main thread is waiting for).
//
// Stats: the "[mcla-cpu] 10 s" line (mcla_cpu_common.cpp): waits, woken, timeouts, time waited,
// Sleep(0) laps replaced, release-latency estimate. Off: --mcla_cpu_poll_wait=false, or
// MCLA_HOOK_CPU_POLL_WAIT=0 at build time (hooks.toml keeps linking).
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_cpu_common.h"
#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_cpu_poll_wait, true, "MCLA",
                    "MCLA: the main thread's ring-space wait (sub_82148A10) sleeps until the pump "
                    "releases a slot instead of looping around Sleep(0); same decisions, only the "
                    "waiting differs (needs mcla_cpu_poll_release; false: the game's own loop)");

namespace {

mcla::hooks::Registration g_registration("cpu", "mcla_cpu_poll_wait", MCLA_HOOK_CPU_POLL_WAIT != 0,
                                         [] { return REXCVAR_GET(mcla_cpu_poll_wait); });

}  // namespace

#if MCLA_HOOK_CPU_POLL_WAIT

bool mcla_hook_cpu_poll_wait(PPCRegister& r31, PPCRegister& r27, PPCRegister& r26, PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_cpu_poll_wait) || !mcla::cpu::PollReleaseOn()) {
    return false;  // the game's own Sleep(0) lap
  }
  using namespace mcla::cpu;
  Counters& stats = Stats();
  stats.ring_calls.fetch_add(1, std::memory_order_relaxed);

  // The rest of the game's 1 ms window. The guest only gets here while r3 < r26 (unsigned compare at
  // 0x82148AA8), so the difference is 1..1000; clamp anything else (a corrupt register) to the window.
  uint32_t remaining_us = r26.u32 - r3.u32;
  if (remaining_us == 0 || remaining_us > kRingDeadlineUs) {
    remaining_us = kRingDeadlineUs;
  }

  // The ring the guest is looking at: the ctx object is the game's own (the guest read the same byte
  // at 0x82148A30), the ring words are static image data.
  uint8_t select = 0;
  if (!mcla::guest::U8(r31.u32 + kRingSelectByte, &select)) {
    return false;
  }
  const uint32_t ring = kRingBase + ((uint32_t(select) >> 2) & 0x38);

  // Register as a waiter, THEN sample the event word and the ring (see the header: no lost wake).
  RingWaiters().fetch_add(1, std::memory_order_seq_cst);
  const uint32_t seen = RingEvent().load(std::memory_order_seq_cst);
  uint32_t read_index = 0;
  if (!mcla::guest::U32(ring + 4, &read_index)) {
    RingWaiters().fetch_sub(1, std::memory_order_seq_cst);
    return false;
  }
  if (read_index != r27.u32) {
    RingWaiters().fetch_sub(1, std::memory_order_seq_cst);
    stats.ring_free.fetch_add(1, std::memory_order_relaxed);
    return true;  // freed in between: the guest loop re-reads and leaves
  }

  if (PumpWakeOn()) {
    // Wake-on-demand: the pump frees this slot; do not wait for its 7 ms beat.
    PumpRequest().store(true, std::memory_order_seq_cst);
    Notify(PumpEvent(), PumpWaiters());
    stats.ring_pump_asks.fetch_add(1, std::memory_order_relaxed);
  }

  const int64_t start = NowNs();
  const bool woken = WaitWord(RingEvent(), seen, uint64_t(remaining_us) * 1000);
  const int64_t end = NowNs();
  RingWaiters().fetch_sub(1, std::memory_order_seq_cst);

  const uint64_t waited_us = uint64_t((end - start) / 1000);
  stats.ring_waits.fetch_add(1, std::memory_order_relaxed);
  (woken ? stats.ring_woken : stats.ring_timeouts).fetch_add(1, std::memory_order_relaxed);
  stats.ring_waited_us.fetch_add(waited_us, std::memory_order_relaxed);
  AtomicMax(stats.ring_max_us, waited_us);
  stats.ring_laps_replaced.fetch_add(
      waited_us < kGameSleepLapUs ? 1 : (waited_us + kGameSleepLapUs - 1) / kGameSleepLapUs,
      std::memory_order_relaxed);
  MaybeReport(end);
  return true;  // skip `li r3,0 ; bl Sleep(0)`: the loop re-reads the index and the clock itself
}

#else  // MCLA_HOOK_CPU_POLL_WAIT == 0: keeps hooks.toml linking, does nothing.

bool mcla_hook_cpu_poll_wait(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) { return false; }

#endif
