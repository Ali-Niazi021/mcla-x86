// mcla_fence_spin: stop the D3D fence poll from burning a core (HOOKS_SPEC E4).
//
// Site 0x82412FB0, the head of the spin loop in sub_82412F98 (one poll of a GPU fence): the game
// does `li r11,4`, then four rounds of eight `or r31,r31,r31` (a PPC low-priority hint; our codegen
// emits NOTHING for it), then reads the fence state from 0x82412FD8. D3DDevice_BlockOnFence
// (0x82411E98) calls the poll in a loop with no sleep until the fence counter passes, so the cost is
// the busy loop itself (36-40 % of a thread in LARecomp's measurement).
//
// The hook replaces the four rounds (returns true: jump to 0x82412FD8, one hook call per poll
// instead of four):
//   * the first 16 consecutive polls of a thread only yield the CPU (a few hundred ns): a fence
//     that is about to pass costs no latency;
//   * after that the thread waits for the GPU command processor to publish a fence write, on the
//     SDK's atomic counter nfsmw_cp_event_seq (bumped and WakeByAddressAll'ed by the CP after each
//     EVENT_WRITE_SHD / MEM_WRITE / COND_WRITE it performs: the fence write itself), for at most 1 ms.
//     The export lives in the GPU plugin (rexgpu-xenos.dll, where the command processor is; SDK patch
//     W3.2, the 0207 idea): the hook looks there first and in rexruntime.dll second, once, at its
//     first wait. If neither exports it (an SDK without the patch) a 1 ms bounded wait stands in,
//     and that is logged once.
// The streak resets when more than 1 ms passes without a poll on that thread. The guest loop
// re-reads the fence after every poll, so a missed wake costs at most the 1 ms bound.
//
// What the iOS device numbers said (docs/mcla/LEARNINGS.md, "CPU levers"): about one fence streak a
// frame, ~18 ms of a 34 ms frame at the shipped interval-2 presentation, polled ~2,700 times a second
// on the 250 us read-pointer word; the fence-write word makes timeouts the exception and the lateness
// ~0. The 10 s line also reports the streaks (how long a thread blocks on fences per frame), which is
// what tells a GPU-bound frame from a serialised one.
//
// WaitOnAddress counts whole milliseconds (the host runs at a 1 ms timer resolution), so 1 ms is
// also the shortest bound.
//
// Off: --mcla_fence_spin=false (the game's own spin), or MCLA_HOOK_FENCE_SPIN=0 at build time.
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
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// WaitOnAddress lives in Synchronization.lib (the build links it too).
#pragma comment(lib, "Synchronization.lib")

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_fence_spin, true, "MCLA",
                    "MCLA: a GPU fence poll yields, then waits (up to 1 ms) for the command "
                    "processor instead of spinning (false: the game's own spin loop)");
REXCVAR_DEFINE_INT32(mcla_fence_spin_bound_us, 0, "MCLA",
                     "MCLA: longest single wait of a fence poll in microseconds (20..5000, rounded up to "
                     "a whole millisecond by WaitOnAddress). 0 = auto: 1000. Used only when the SDK "
                     "exports the fence-write word nfsmw_cp_event_seq; without it a poll waits 1 ms");

namespace {

mcla::hooks::Registration g_registration("fence-spin", "mcla_fence_spin", MCLA_HOOK_FENCE_SPIN != 0,
                                         [] { return REXCVAR_GET(mcla_fence_spin); });

}  // namespace

#if MCLA_HOOK_FENCE_SPIN

namespace {

constexpr uint32_t kRelaxPolls = 16;         // polls of a streak that only yield the CPU
constexpr int64_t kStreakGapNs = 1'000'000;  // a longer pause between polls ends the streak
constexpr uint64_t kEventWaitUs = 1000;      // the bound of one wait on the fence-write word (a real wake, so a safety net)
constexpr DWORD kFallbackWaitMs = 1;         // the bound of a poll when no fence-write word exists

struct Streak {
  uint32_t polls = 0;
  int64_t start_ns = 0;
  int64_t last_ns = 0;
};
thread_local Streak t_streak;

std::atomic<uint64_t> g_polls{0}, g_waits{0}, g_timeouts{0};
std::atomic<uint64_t> g_streaks{0}, g_streak_ns{0}, g_streak_max_ns{0};
std::atomic<int64_t> g_report_ns{0};
std::atomic<uint64_t> g_report_frames{0};
std::mutex g_longest_mutex;
char g_longest_thread[32] = "";
std::atomic<uint64_t> g_longest_ns{0};  // the longest streak of the window; its thread's name above

// The calling thread's name for the report: its description (the SDK names guest threads), else its id.
void CurrentThreadName(char* out, size_t size) {
  out[0] = '\0';
  PWSTR wide = nullptr;
  if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wide)) && wide) {
    char utf8[128];
    if (wide[0] != L'\0' && WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, int(sizeof(utf8)), nullptr, nullptr) > 0) {
      std::snprintf(out, size, "%s", utf8);
    }
    LocalFree(wide);
  }
  if (out[0] == '\0') {
    std::snprintf(out, size, "tid %lu", static_cast<unsigned long>(GetCurrentThreadId()));
  }
}

// A streak that went past the relax polls is a real block on the GPU: count it (called when it ends,
// i.e. when the same thread starts the next one, so the numbers lag by at most one streak).
void EndStreak(const Streak& streak) {
  if (streak.polls <= kRelaxPolls || streak.last_ns <= streak.start_ns) {
    return;
  }
  const uint64_t length = uint64_t(streak.last_ns - streak.start_ns);
  g_streaks.fetch_add(1, std::memory_order_relaxed);
  g_streak_ns.fetch_add(length, std::memory_order_relaxed);
  uint64_t seen = g_streak_max_ns.load(std::memory_order_relaxed);
  while (length > seen && !g_streak_max_ns.compare_exchange_weak(seen, length, std::memory_order_relaxed)) {
  }
  if (length > g_longest_ns.load(std::memory_order_relaxed)) {  // the lock below decides
    std::lock_guard<std::mutex> lock(g_longest_mutex);
    if (length > g_longest_ns.load(std::memory_order_relaxed)) {
      g_longest_ns.store(length, std::memory_order_relaxed);
      CurrentThreadName(g_longest_thread, sizeof(g_longest_thread));
    }
  }
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The word a fence wait sleeps on: nfsmw_cp_event_seq, exported by the GPU plugin (rexgpu-xenos.dll,
// postfix d / rd in Debug / RelWithDebInfo) or, failing that, by the runtime (rexruntime.dll). The
// plugin is loaded before the first guest frame, so a module lookup at the first wait finds it.
std::atomic<uint32_t>* FindEventWord() {
  for (const wchar_t* base : {L"rexgpu-xenos", L"rexruntime"}) {
    for (const wchar_t* postfix : {L"", L"d", L"rd"}) {
      HMODULE handle = GetModuleHandleW((std::wstring(base) + postfix + L".dll").c_str());
      if (!handle) {
        continue;
      }
      if (FARPROC symbol = GetProcAddress(handle, "nfsmw_cp_event_seq")) {
        return reinterpret_cast<std::atomic<uint32_t>*>(symbol);
      }
    }
  }
  return nullptr;
}

std::atomic<uint32_t>* EventWord() {
  static std::atomic<uint32_t>* const word = []() -> std::atomic<uint32_t>* {
    std::atomic<uint32_t>* found = FindEventWord();
    if (found) {
      REXLOG_INFO("[mcla-fence-spin] waits on nfsmw_cp_event_seq: the CP wakes a fence poll when it "
                  "writes the fence");
    } else {
      REXLOG_WARN("[mcla-fence-spin] nfsmw_cp_event_seq not found in rexgpu-xenos.dll or rexruntime.dll: "
                  "fence polls wait {} ms instead of waiting for the command processor",
                  kFallbackWaitMs);
    }
    return found;
  }();
  return word;
}

// Milliseconds of one wait on the fence-write word.
DWORD WaitBoundMs() {
  const int32_t configured = REXCVAR_GET(mcla_fence_spin_bound_us);
  const uint32_t us = configured > 0 ? uint32_t(std::clamp<int32_t>(configured, 20, 5000)) : uint32_t(kEventWaitUs);
  return (us + 999) / 1000;
}

void Report(int64_t now_ns) {
  int64_t since = g_report_ns.load(std::memory_order_relaxed);
  if (since == 0) {
    g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed);
    g_report_frames.store(mcla::FrameCount(), std::memory_order_relaxed);
    return;
  }
  if (now_ns - since < 10'000'000'000 ||
      !g_report_ns.compare_exchange_strong(since, now_ns, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t frames_now = mcla::FrameCount();
  const uint64_t frames = frames_now - g_report_frames.exchange(frames_now, std::memory_order_relaxed);
  const uint64_t streaks = g_streaks.exchange(0);
  const uint64_t streak_ns = g_streak_ns.exchange(0);
  const uint64_t streak_max_ns = g_streak_max_ns.exchange(0);
  char longest_thread[sizeof(g_longest_thread)];
  {
    std::lock_guard<std::mutex> lock(g_longest_mutex);
    std::memcpy(longest_thread, g_longest_thread, sizeof(longest_thread));
    g_longest_thread[0] = '\0';
    g_longest_ns.store(0, std::memory_order_relaxed);
  }
  const bool event_word = EventWord() != nullptr;
  const uint64_t bound_ms = event_word ? WaitBoundMs() : kFallbackWaitMs;
  REXLOG_INFO("[mcla-fence-spin] 10 s: {} fence polls, {} waited for the command processor, {} of them "
              "timed out ({} ms bound{}) | {} fence blocks (streaks of more than {} polls): avg {:.2f} ms, "
              "max {:.2f} ms (thread '{}'), {:.2f} ms a frame over {} frames",
              g_polls.exchange(0), g_waits.exchange(0), g_timeouts.exchange(0), bound_ms,
              event_word ? ", fence-write wake" : ", no fence-write word: plain wait", streaks, kRelaxPolls,
              streaks ? double(streak_ns) / double(streaks) / 1e6 : 0.0, double(streak_max_ns) / 1e6,
              longest_thread, frames ? double(streak_ns) / double(frames) / 1e6 : 0.0, frames);
}

}  // namespace

bool mcla_hook_fence_spin() {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_fence_spin)) {
    return false;
  }
  Streak& streak = t_streak;
  const int64_t now = NowNs();
  if (now - streak.last_ns > kStreakGapNs) {
    EndStreak(streak);
    streak.polls = 0;
    streak.start_ns = now;
  }
  g_polls.fetch_add(1, std::memory_order_relaxed);
  if (streak.polls < kRelaxPolls) {
    ++streak.polls;
    YieldProcessor();
    streak.last_ns = now;
    return true;
  }
  ++streak.polls;
  std::atomic<uint32_t>* const word = EventWord();
  if (word) {
    // The sequence first, then the wait: if the CP publishes in between, the word has already
    // changed and the wait returns at once.
    uint32_t before = word->load(std::memory_order_acquire);
    const bool woke = WaitOnAddress(word, &before, sizeof(before), WaitBoundMs()) != FALSE;
    g_waits.fetch_add(1, std::memory_order_relaxed);
    if (!woke && GetLastError() == ERROR_TIMEOUT) {
      g_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    Sleep(kFallbackWaitMs);  // nothing to be woken by: a plain 1 ms bounded wait
  }
  const int64_t after = NowNs();
  streak.last_ns = after;
  Report(after);
  return true;
}

#else  // MCLA_HOOK_FENCE_SPIN == 0: keeps hooks.toml linking, does nothing.

bool mcla_hook_fence_spin() { return false; }

#endif
