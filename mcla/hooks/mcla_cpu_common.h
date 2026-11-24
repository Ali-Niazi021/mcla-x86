// Midnight Club: Los Angeles: guest-CPU levers E1-E3 (docs/mcla/HOOKS_SPEC.md section E): the state
// the three hooks share. Not a hook itself: mcla_cpu_common.cpp is always compiled.
//
//   E1 mcla_cpu_poll_wait     0x82148AB0  main thread, ring full: wait for the release (mcla_cpu_poll_wait.cpp)
//   E2 mcla_cpu_poll_release  0x82160140  pump thread frees a ring slot: wake it     (mcla_cpu_poll_release.cpp)
//   E3 mcla_cpu_pump          0x821449C8  pump round end: statistics, optional wake  (mcla_cpu_pump.cpp)
//
// The game's frame pipeline hands work between threads through 8 rings of three slots at
// 0x82836814 (per ring: +0 the producer's write index, +4 the consumer's read index; modulo 3). The
// producer (sub_82148A10, main thread) finds its next slot equal to the read index when it is two
// slots ahead of the consumer, and loops up to 1000 us of the guest microsecond clock around
// `Sleep(0)`; the consumer (sub_82160088, run by the audio/engine pump thread sub_82144888 once per
// round of at most 7 ms) moves the read index. On a PC that loop is a busy yield (Renan measured
// 61-75 % less main-thread CPU with a real wait). The shipped Sleep(0) is a 100 us host sleep for
// low-priority threads and a bare yield for the others (SDK xthread.cpp).
//
// Event words. Two process-wide 32-bit words are waited on with WaitOnAddress and woken with
// WakeByAddressAll (Synchronization.lib), the Win32 futex:
//   RingEvent  bumped by E2 right after the game stored the read index. E1 waits on it.
//   PumpEvent  bumped by E1 when the ring is full ("wake-on-demand"). E3 waits on it in its wake
//              mode only (--mcla_cpu_pump_wake=true, default OFF).
// A waiter registers in the matching waiter count BEFORE it reads the event word and the ring, and a
// notifier bumps the word BEFORE it reads the count (both sequentially consistent), so no wake is
// lost; a missed one costs the rest of the wait, which is bounded by the guest's own deadline.
//
// One 10 s line "[mcla-cpu] 10 s: ..." (MaybeReport) summarises all three hooks.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "mcla_hooks.h"

#ifndef MCLA_HOOK_CPU_POLL_WAIT
#define MCLA_HOOK_CPU_POLL_WAIT 0
#endif
#ifndef MCLA_HOOK_CPU_POLL_RELEASE
#define MCLA_HOOK_CPU_POLL_RELEASE 0
#endif
#ifndef MCLA_HOOK_CPU_PUMP
#define MCLA_HOOK_CPU_PUMP 0
#endif

REXCVAR_DECLARE(bool, mcla_cpu_poll_wait);
REXCVAR_DECLARE(bool, mcla_cpu_poll_release);
REXCVAR_DECLARE(bool, mcla_cpu_pump);
REXCVAR_DECLARE(bool, mcla_cpu_pump_wake);

namespace mcla::cpu {

constexpr uint32_t kRingBase = 0x82836814;     // ring 0: write index (+0), read index (+4); 8 B per ring
constexpr uint32_t kRingSelectByte = 199;      // byte [ctx+199]: ring = base + ((byte >> 2) & 0x38)
constexpr uint32_t kRingDeadlineUs = 1000;     // the game's own bound of the wait
constexpr uint32_t kGameSleepLapUs = 100;      // what one shipped Sleep(0) lap costs (the stats' lap estimate)

// One 10 s window of counters (exchanged to 0 by the report). All relaxed atomics: several game
// threads may call.
struct Counters {
  // E1 (mcla_cpu_poll_wait)
  std::atomic<uint64_t> ring_calls{0};         // hook calls (one per Sleep(0) lap the game would run)
  std::atomic<uint64_t> ring_waits{0};         // calls that really waited
  std::atomic<uint64_t> ring_woken{0};         // ... ended by a release (before the deadline)
  std::atomic<uint64_t> ring_timeouts{0};      // ... ran to the end of the wait (nobody released)
  std::atomic<uint64_t> ring_free{0};          // calls where the slot was already free (no wait)
  std::atomic<uint64_t> ring_waited_us{0};     // total time waited
  std::atomic<uint64_t> ring_max_us{0};        // longest single wait
  std::atomic<uint64_t> ring_laps_replaced{0}; // shipped Sleep(0) laps those waits stand for (100 us each)
  std::atomic<uint64_t> ring_pump_asks{0};     // wake-on-demand requests sent to the pump (wake mode)
  // E2 (mcla_cpu_poll_release)
  std::atomic<uint64_t> rel_calls{0};          // ring slot releases seen
  std::atomic<uint64_t> rel_wakes{0};          // ... that woke a waiting main thread
  // E3 (mcla_cpu_pump)
  std::atomic<uint64_t> pump_rounds{0};        // pump rounds that reached the end-of-round sleep
  std::atomic<uint64_t> pump_period_us{0};     // sum of the time between two such points
  std::atomic<uint64_t> pump_period_max_us{0};
  std::atomic<uint64_t> pump_late{0};          // rounds over 10 ms apart (7 ms is the pump's own beat)
  std::atomic<uint64_t> pump_ask_ms{0};        // sum of the sleep the game asked for (r3)
  std::atomic<uint64_t> pump_ask_zero{0};      // rounds that asked for 0 ms (already over budget)
  std::atomic<uint64_t> pump_waits{0};         // wake mode: sleeps replaced by a wait
  std::atomic<uint64_t> pump_early{0};         // ... ended early by the main thread's request
  std::atomic<uint64_t> pump_timeouts{0};      // ... ran the whole sleep
  std::atomic<uint64_t> pump_cut_us{0};        // wake mode: time cut from the shipped sleeps
};
Counters& Stats();

int64_t NowNs();

void AtomicMax(std::atomic<uint64_t>& target, uint64_t value);

std::atomic<uint32_t>& RingEvent();
std::atomic<int32_t>& RingWaiters();
std::atomic<uint32_t>& PumpEvent();
std::atomic<int32_t>& PumpWaiters();
// Set by E1 when the ring is full, taken by E3's wake mode.
std::atomic<bool>& PumpRequest();

// Waits until `word` is no longer `seen` or `timeout_ns` have passed. True: woken (or the word had
// already moved); false: timed out. WaitOnAddress counts whole milliseconds: the timeout is rounded
// UP to the next millisecond (never 0, which would not wait), so a wait can overrun a sub-ms bound by
// up to a millisecond at the host's 1 ms timer resolution; every caller re-checks its own condition.
bool WaitWord(std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns);

// Bumps `word` and wakes every waiter if `waiters` says there is one. Returns true when it woke.
bool Notify(std::atomic<uint32_t>& word, const std::atomic<int32_t>& waiters);

// E1 needs E2's wake; E3's wake mode needs E1's request.
bool PollReleaseOn();   // E2 compiled in and its cvar on
bool PumpWakeOn();      // E1 compiled in and on, E3 compiled in and on, mcla_cpu_pump_wake

// The combined 10 s line, from whichever hook runs first after the window (first call only starts it).
void MaybeReport(int64_t now_ns);

}  // namespace mcla::cpu
