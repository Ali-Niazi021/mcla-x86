// mcla_cpu_poll_release: tell a waiting main thread that a ring slot was freed (HOOKS_SPEC E2).
//
// Site 0x82160140 in sub_82160088, hooked AFTER `91660004 stw r11,4(r6)`: r6 = 0x82836814 (ring 0)
// and the store has just moved the ring's READ index to (write index + 2) % 3, that is freed every
// slot but the newest. The function runs once per round of the pump thread (sub_82144888, one round
// of at most 7 ms, timer `audioFrameTimer`); the producer that may be waiting for exactly this store
// is the main thread's loop at 0x82148AB0 (mcla_cpu_poll_wait.cpp).
//
// The hook bumps the shared RingEvent word and wakes its waiters (WakeByAddressAll, only
// when one is registered: the common case is one atomic increment and one load). The store itself is
// the game's; nothing here changes what the game does, so the default is ON. Without a waiter (E1
// off) the hook is a counter. It also drops a pending wake-on-demand ask of the pump (E3's wake
// mode): this release answers it.
//
// Off: --mcla_cpu_poll_release=false, or MCLA_HOOK_CPU_POLL_RELEASE=0 at build time (hooks.toml keeps
// linking). E1 then falls back to the game's own Sleep(0) loop.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_cpu_common.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_cpu_poll_release, true, "MCLA",
                    "MCLA: the pump's ring-slot release (sub_82160088) wakes the main thread's "
                    "ring-space wait (false: no wake; mcla_cpu_poll_wait then runs the game's own "
                    "Sleep(0) loop)");

namespace {

mcla::hooks::Registration g_registration("cpu", "mcla_cpu_poll_release",
                                         MCLA_HOOK_CPU_POLL_RELEASE != 0,
                                         [] { return REXCVAR_GET(mcla_cpu_poll_release); });

}  // namespace

#if MCLA_HOOK_CPU_POLL_RELEASE

void mcla_hook_cpu_poll_release(PPCRegister& r6) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_cpu_poll_release) || r6.u32 != mcla::cpu::kRingBase) {
    return;
  }
  using namespace mcla::cpu;
  Counters& stats = Stats();
  stats.rel_calls.fetch_add(1, std::memory_order_relaxed);
  // A pending "run a round now" ask of the main thread (pump wake mode) is answered by this very
  // release: drop it so the pump does not skip its next sleep for nothing.
  PumpRequest().store(false, std::memory_order_relaxed);
  if (Notify(RingEvent(), RingWaiters())) {
    stats.rel_wakes.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeReport(NowNs());
}

#else  // MCLA_HOOK_CPU_POLL_RELEASE == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_cpu_poll_release(PPCRegister&) {}

#endif
