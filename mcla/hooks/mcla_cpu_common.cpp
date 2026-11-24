// State shared by the CPU-lever hooks E1-E3 (see mcla_cpu_common.h): the event words, the waits and
// wakes, and the combined 10 s report. Always compiled; nothing here touches guest memory.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

// WaitOnAddress / WakeByAddressAll live in Synchronization.lib (the build links it too).
#pragma comment(lib, "Synchronization.lib")

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_cpu_common.h"

namespace mcla::cpu {

Counters& Stats() {
  static Counters counters;
  return counters;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void AtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t seen = target.load(std::memory_order_relaxed);
  while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

namespace {
// Function-local statics are not needed: these have constant initialisers.
std::atomic<uint32_t> g_ring_event{0};
std::atomic<int32_t> g_ring_waiters{0};
std::atomic<uint32_t> g_pump_event{0};
std::atomic<int32_t> g_pump_waiters{0};
std::atomic<bool> g_pump_request{false};
std::atomic<int64_t> g_report_ns{0};
}  // namespace

std::atomic<uint32_t>& RingEvent() { return g_ring_event; }
std::atomic<int32_t>& RingWaiters() { return g_ring_waiters; }
std::atomic<uint32_t>& PumpEvent() { return g_pump_event; }
std::atomic<int32_t>& PumpWaiters() { return g_pump_waiters; }
std::atomic<bool>& PumpRequest() { return g_pump_request; }

bool WaitWord(std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) {
  // WaitOnAddress counts whole milliseconds: round up, and never 0 (which returns at once).
  const DWORD timeout_ms = DWORD(std::clamp<uint64_t>((timeout_ns + 999'999) / 1'000'000, 1, 0xFFFFFFFEull));
  if (WaitOnAddress(&word, &seen, sizeof(uint32_t), timeout_ms)) {
    return true;  // woken, or the word had already moved
  }
  return GetLastError() != ERROR_TIMEOUT;
}

bool Notify(std::atomic<uint32_t>& word, const std::atomic<int32_t>& waiters) {
  word.fetch_add(1, std::memory_order_seq_cst);
  if (waiters.load(std::memory_order_seq_cst) <= 0) {
    return false;
  }
  WakeByAddressAll(&word);
  return true;
}

bool PollReleaseOn() {
  return MCLA_HOOK_CPU_POLL_RELEASE != 0 && REXCVAR_GET(mcla_cpu_poll_release);
}

bool PumpWakeOn() {
  return MCLA_HOOK_CPU_POLL_WAIT != 0 && REXCVAR_GET(mcla_cpu_poll_wait) &&
         MCLA_HOOK_CPU_PUMP != 0 && REXCVAR_GET(mcla_cpu_pump) && REXCVAR_GET(mcla_cpu_pump_wake);
}

void MaybeReport(int64_t now_ns) {
  int64_t since = g_report_ns.load(std::memory_order_relaxed);
  if (since == 0) {
    g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed);
    return;
  }
  if (now_ns - since < 10'000'000'000 ||
      !g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed)) {
    return;
  }
  const double window_s = double(now_ns - since) / 1e9;
  Counters& c = Stats();
  const uint64_t ring_calls = c.ring_calls.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_waits = c.ring_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_woken = c.ring_woken.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_timeouts = c.ring_timeouts.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_free = c.ring_free.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_waited_us = c.ring_waited_us.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_max_us = c.ring_max_us.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_laps = c.ring_laps_replaced.exchange(0, std::memory_order_relaxed);
  const uint64_t ring_asks = c.ring_pump_asks.exchange(0, std::memory_order_relaxed);
  const uint64_t rel_calls = c.rel_calls.exchange(0, std::memory_order_relaxed);
  const uint64_t rel_wakes = c.rel_wakes.exchange(0, std::memory_order_relaxed);
  const uint64_t rounds = c.pump_rounds.exchange(0, std::memory_order_relaxed);
  const uint64_t period_us = c.pump_period_us.exchange(0, std::memory_order_relaxed);
  const uint64_t period_max_us = c.pump_period_max_us.exchange(0, std::memory_order_relaxed);
  const uint64_t late = c.pump_late.exchange(0, std::memory_order_relaxed);
  const uint64_t ask_ms = c.pump_ask_ms.exchange(0, std::memory_order_relaxed);
  const uint64_t ask_zero = c.pump_ask_zero.exchange(0, std::memory_order_relaxed);
  const uint64_t pump_waits = c.pump_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t pump_early = c.pump_early.exchange(0, std::memory_order_relaxed);
  const uint64_t pump_timeouts = c.pump_timeouts.exchange(0, std::memory_order_relaxed);
  const uint64_t pump_cut_us = c.pump_cut_us.exchange(0, std::memory_order_relaxed);

  // "Time saved" for E1: the shipped loop notices a release at the next lap boundary, on average half
  // a lap (50 us) after it; a woken wait notices it at once. The wakeups saved are the laps the wait
  // stands for minus the one wake it costs. Both are estimates, labelled as such.
  const double saved_ms = double(ring_woken) * (kGameSleepLapUs / 2.0) / 1000.0;
  const uint64_t wakeups_saved = ring_laps > ring_waits ? ring_laps - ring_waits : 0;
  REXLOG_INFO(
      "[mcla-cpu] {:.0f} s ring wait (E1): {} hook calls, {} waits ({} woken by a release, {} timed "
      "out at the game's 1 ms bound), {} slots already free; {:.2f} ms waited in all (avg {} us, "
      "max {} us); ~{} Sleep(0) laps replaced = ~{} host wakeups saved, ~{:.2f} ms of release "
      "latency saved (est. 50 us a woken wait) | release (E2): {} slot releases, {} woke the main "
      "thread | pump (E3): {} rounds, period avg {:.2f} ms max {:.2f} ms ({} over 10 ms), sleep "
      "asked avg {:.2f} ms ({} of 0 ms)",
      window_s, ring_calls, ring_waits, ring_woken, ring_timeouts, ring_free,
      double(ring_waited_us) / 1000.0, ring_waits ? ring_waited_us / ring_waits : 0, ring_max_us,
      ring_laps, wakeups_saved, saved_ms, rel_calls, rel_wakes, rounds,
      rounds ? double(period_us) / double(rounds) / 1000.0 : 0.0, double(period_max_us) / 1000.0,
      late, rounds ? double(ask_ms) / double(rounds) : 0.0, ask_zero);
  if (pump_waits || ring_asks) {
    REXLOG_INFO(
        "[mcla-cpu] {:.0f} s pump wake mode (E3 + E1): {} asks from the main thread, {} pump sleeps "
        "replaced by a wait ({} cut short by an ask, {} ran the whole sleep), {:.2f} ms of sleep "
        "cut",
        window_s, ring_asks, pump_waits, pump_early, pump_timeouts, double(pump_cut_us) / 1000.0);
  }
}

}  // namespace mcla::cpu
