// mcla-x86 host: Win32 timing helpers (QPC, timer resolution, precise sleep).
//
// Windows timers run on a 15.625 ms grid unless the process asks for more. The host asks for 1 ms at start
// (mcla_app.cpp, OnConfigurePaths) and opts out of Windows 11's "ignore the request while no window is visible"
// throttling. The precise sleep below is what the later pacing work uses (guest vblank, host frame cap, PLAN 3.6,
// W3.3): a high-resolution waitable timer for the bulk of the wait, then a QPC spin for the tail.
//
// Nothing here depends on the SDK, the game or the generated code.
#pragma once

#include <cstdint>

namespace mcla_host {

// --- Process timer resolution --------------------------------------------------------------------------------

// Opts the process out of "timer resolution is ignored while the window is hidden" (ProcessPowerThrottling,
// IGNORE_TIMER_RESOLUTION). Returns true on success; on failure `*win32_error` (if given) gets GetLastError().
bool OptOutOfTimerResolutionThrottling(uint32_t* win32_error = nullptr);

// timeBeginPeriod / timeEndPeriod. Every successful Begin needs one End with the same value.
bool BeginTimerResolution(uint32_t milliseconds);
void EndTimerResolution(uint32_t milliseconds);

// --- QueryPerformanceCounter ---------------------------------------------------------------------------------

uint64_t QpcFrequency();  // ticks per second, constant for the process
uint64_t QpcNow();
double QpcToSeconds(uint64_t ticks);
uint64_t SecondsToQpc(double seconds);

// --- Precise sleep -------------------------------------------------------------------------------------------

// Default spin tail of the precise sleeps: the part of the wait done by spinning on QPC, after the timer wait.
inline constexpr double kDefaultSpinTailSeconds = 0.0004;

// True if this thread's waitable timer is a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION one (Windows 10 1803+). Without it
// the timer fires on the system timer grid (1 ms after BeginTimerResolution(1)) and the spin tail is widened to 2 ms.
bool HighResolutionTimerAvailable();

// Returns once QpcNow() >= target_qpc. Waits on a per-thread high-resolution waitable timer until `spin_tail_seconds`
// before the target, then spins (YieldProcessor). Never returns early; returns at once if the target has passed.
void SleepUntilQpc(uint64_t target_qpc, double spin_tail_seconds = kDefaultSpinTailSeconds);

// SleepUntilQpc(QpcNow() + seconds).
void PreciseSleepFor(double seconds, double spin_tail_seconds = kDefaultSpinTailSeconds);

}  // namespace mcla_host
