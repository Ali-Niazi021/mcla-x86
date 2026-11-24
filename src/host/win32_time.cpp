// mcla-x86 host: Win32 timing helpers. See win32_time.h.
#include "win32_time.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <algorithm>

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace mcla_host {

namespace {

// Spin tail used when the timer is not a high-resolution one: it can only wake on the system timer grid.
constexpr double kLowResolutionSpinTailSeconds = 0.002;

// One waitable timer per thread, created on first use and closed when the thread ends.
struct ThreadTimer {
  HANDLE handle = nullptr;
  bool high_resolution = false;

  ThreadTimer() {
    handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (handle) {
      high_resolution = true;
      return;
    }
    handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  ~ThreadTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
  ThreadTimer(const ThreadTimer&) = delete;
  ThreadTimer& operator=(const ThreadTimer&) = delete;
};

ThreadTimer& CurrentThreadTimer() {
  thread_local ThreadTimer timer;
  return timer;
}

uint64_t ComputeQpcFrequency() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  return static_cast<uint64_t>(frequency.QuadPart);
}

}  // namespace

bool OptOutOfTimerResolutionThrottling(uint32_t* win32_error) {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  state.StateMask = 0;  // control bit set, state bit clear: the timer-resolution request is always honoured
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
    return true;
  }
  if (win32_error) {
    *win32_error = GetLastError();
  }
  return false;
}

bool BeginTimerResolution(uint32_t milliseconds) {
  return timeBeginPeriod(milliseconds) == TIMERR_NOERROR;
}

void EndTimerResolution(uint32_t milliseconds) {
  timeEndPeriod(milliseconds);
}

uint64_t QpcFrequency() {
  static const uint64_t frequency = ComputeQpcFrequency();
  return frequency;
}

uint64_t QpcNow() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart);
}

double QpcToSeconds(uint64_t ticks) {
  return static_cast<double>(ticks) / static_cast<double>(QpcFrequency());
}

uint64_t SecondsToQpc(double seconds) {
  return seconds <= 0.0 ? 0 : static_cast<uint64_t>(seconds * static_cast<double>(QpcFrequency()));
}

bool HighResolutionTimerAvailable() {
  return CurrentThreadTimer().high_resolution;
}

bool WaitableTimerAvailable() {
  return CurrentThreadTimer().handle != nullptr;
}

void SleepUntilQpc(uint64_t target_qpc, double spin_tail_seconds) {
  ThreadTimer& timer = CurrentThreadTimer();
  const double tail_seconds =
      timer.high_resolution ? spin_tail_seconds
                            : std::max(spin_tail_seconds, kLowResolutionSpinTailSeconds);
  const uint64_t tail = SecondsToQpc(tail_seconds);
  const uint64_t frequency = QpcFrequency();

  for (;;) {
    const uint64_t now = QpcNow();
    if (now >= target_qpc) {
      return;
    }
    const uint64_t remaining = target_qpc - now;
    if (timer.handle && remaining > tail) {
      // Relative due time in 100 ns units, negative. Leave `tail` of the wait to the spin.
      const uint64_t wait_ticks = remaining - tail;
      const uint64_t wait_100ns = std::max<uint64_t>(1, wait_ticks * 10'000'000ull / frequency);
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(wait_100ns);
      if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer.handle, INFINITE);
        continue;
      }
    }
    YieldProcessor();
  }
}

void PreciseSleepFor(double seconds, double spin_tail_seconds) {
  SleepUntilQpc(QpcNow() + SecondsToQpc(seconds), spin_tail_seconds);
}

void SpinUntilQpc(uint64_t target_qpc) {
  while (QpcNow() < target_qpc) {
    YieldProcessor();
  }
}

}  // namespace mcla_host
