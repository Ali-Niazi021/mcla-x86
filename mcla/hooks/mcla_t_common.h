// Midnight Club: Los Angeles: 60 fps timing (docs/mcla/TIMING.md): the state every mcla_t_* hook
// shares. Not a hook itself: mcla_t_common.cpp is always compiled.
//
// ONE master cvar, `mcla_sim60` (bool, default OFF): off = the game runs exactly as shipped (fixed
// step 1/30 s, 2 physics substeps of 1/60 s); every hook of this feature is then a no-op (one cvar
// read). On: the world step of each frame is n/60 s with n = 1..4 chosen from the real frame time
// (n = 2 at a 30 fps cap = the shipped step, n = 1 at 60), the frame runs n physics substeps (so the
// physics step stays 1/60 s), and the per-frame systems that assume 30 updates a second are corrected
// (camera and chassis smoothing, Flash movie steps, the race-start countdown, the race clock).
//
// THE FRAME CAP. The frame rate IS the cap, and two cvars name it (W3.3):
//
//   mcla_fps_cap  (host, src/host/frame_cap.h; 0, 30, 60, 120; default 0)  the HOST limiter: holds every guest swap
//                 to an even grid (high-resolution timer + QPC spin tail), from PaceAtSwap() below. 0 = no limiter,
//                 the shipped run: the guest's present interval and vblank pace the game.
//   mcla_sim_cap  (here; 0, 30, 60, 120; default 0)  the cap the WORLD assumes with mcla_sim60: the quantiser's
//                 grid, the game-thread pacer, the present interval. 0 = follow mcla_fps_cap; if that is 0 too, 60
//                 (none known: the world follows the real frame time).
//
//   mcla_sim60  mcla_fps_cap  mcla_sim_cap  world cap      host limiter    the run
//   off         0             any           (unused)       none            as shipped (present interval 2)
//   off         30            any           (unused)       30              M4: locked 30, the shipped timing
//   off         60 or 120     any           (unused)       never waits     as shipped: 30 (without sim60 only 30 is
//                                                                          a valid cap; the step is a fixed 1/30 s)
//   on          0             0             60 (unknown)   none            M5 minimum: pacer + vblank hold 60
//   on          60            0             60             60              M5: sim60 + cap 60
//   on          30            0             30             30              the shipped timing, quantised
//   on          120           0             120 (1/120)    120             P1 grid, needs a 120 Hz guest vblank
//   on          X             Y             Y              min(X, world)   Y overrides the world's cap (diagnostic)
//
// The limiter's rate is mcla_fps_cap lowered to EffectiveCap() while mcla_sim60 is on: the 60 -> 30 hold and the
// 120 -> 60 tier lower it for their duration, and it is never raised. It runs on the thread that presents, from the
// swap hook (mcla_t_present_interval.cpp), so with mcla_sim_pace=false the swap-side limiter holds the rate alone. A
// cap of 30 under mcla_sim60 makes every hook return what the game computed (the run equals the shipped one,
// `[mcla-sim]` reads n2 only).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "mcla_hooks.h"
#include "mcla_t_sim_math.h"

#ifndef MCLA_HOOK_SIM_FRAME_DELTA
#define MCLA_HOOK_SIM_FRAME_DELTA 0
#endif
#ifndef MCLA_HOOK_SIM_STEP
#define MCLA_HOOK_SIM_STEP 0
#endif
#ifndef MCLA_HOOK_SIM_CAMERA_POS
#define MCLA_HOOK_SIM_CAMERA_POS 0
#endif
#ifndef MCLA_HOOK_SIM_CAMERA_LOOKAT
#define MCLA_HOOK_SIM_CAMERA_LOOKAT 0
#endif
#ifndef MCLA_HOOK_SIM_CHASSIS
#define MCLA_HOOK_SIM_CHASSIS 0
#endif
#ifndef MCLA_HOOK_SIM_UI_ADVANCE
#define MCLA_HOOK_SIM_UI_ADVANCE 0
#endif
#ifndef MCLA_HOOK_SIM_UI_COUNTDOWN
#define MCLA_HOOK_SIM_UI_COUNTDOWN 0
#endif
#ifndef MCLA_HOOK_SIM_RACE_CLOCK
#define MCLA_HOOK_SIM_RACE_CLOCK 0
#endif
#ifndef MCLA_HOOK_SIM_PRESENT_INTERVAL
#define MCLA_HOOK_SIM_PRESENT_INTERVAL 0
#endif

REXCVAR_DECLARE(bool, mcla_sim60);
REXCVAR_DECLARE(int32_t, mcla_sim_force_n);
REXCVAR_DECLARE(int32_t, mcla_physics_substeps);
REXCVAR_DECLARE(int32_t, mcla_sim_cap);
REXCVAR_DECLARE(bool, mcla_sim_pace);
REXCVAR_DECLARE(int32_t, mcla_sim_hold30_s);
REXCVAR_DECLARE(int32_t, mcla_sim_log_s);
REXCVAR_DECLARE(bool, mcla_smoothing_fix);
REXCVAR_DECLARE(bool, mcla_ui_30hz);
REXCVAR_DECLARE(std::string, mcla_race_clock);

namespace mcla::sim {

// Guest addresses (docs/mcla/TIMING.md section 1; ADDRESSES.md).
constexpr uint32_t kTimer = 0x827D7500;           // rage::sysTimeManager, the one instance
constexpr uint32_t kTimerDt = kTimer + 0x08;      // f32 dt, scaled and clamped (the game's dt)
constexpr uint32_t kTimerTotal = kTimer + 0x14;   // f32 total time
constexpr uint32_t kTimerFrames = kTimer + 0x2C;  // u32 frames the timer has run
constexpr uint32_t kTimerRaw = kTimer + 0x5C;     // f32 measured dt (the game's tick constant)
constexpr uint32_t kTimerUnscaled = kTimer + 0x58;  // f32 the frame's step before the time scale
constexpr uint32_t kFrameRatePtr = 0x828CCEA0;    // the frame-rate object; +0x10 = physics substeps

// The master cvar. One relaxed load.
inline bool Enabled() { return REXCVAR_GET(mcla_sim60); }

// ---- guest memory (static image words are always mapped; heap goes through the guarded reads)
bool ImageU32(uint32_t address, uint32_t* out);   // big-endian load of a word of the static image
bool ImageF32(uint32_t address, float* out);
bool WriteU32(uint32_t address, uint32_t value);  // guarded store: false if the page is unreadable
// The frame-rate object's physics substep word (BE32 [[0x828CCEA0]+0x10]); counts a failure for the log.
bool SetSubsteps(int count);

// ---- the frame (heartbeat, A2, T1)
// The frame cap the world runs at: `mcla_sim_cap`, else the host's cap `mcla_fps_cap`, else 0 = none known (60, so n
// follows the real frame time); 30 while the missed-frames hold is on; 60 while a 120 cap is held at 60 (P1).
// Above 60 the world runs on the 1/120 grid (TIMING.md section 15).
int EffectiveCap();
// mcla_fps_cap as the host understands it: 0 (no host limiter), 30, 60 or 120.
int HostCapSetting();
// The host's frame limiter (W3.3, src/host/frame_cap.h): holds the calling thread's swap to the grid of
// min(mcla_fps_cap, EffectiveCap() with mcla_sim60 on), then returns. Called once per guest swap, on the thread that
// presents, from the present-interval hook (mcla_t_present_interval.cpp), before the swap packet is written. Without
// a host cap (mcla_fps_cap 0) it is one cvar read and returns at once.
void PaceAtSwap();
// A2: the real time of the frame that is starting, in ms, already clamped (thread-local hand-off to
// the T1 call of the same Update).
void NoteRawDelta(double ms);
// T1: the quantised step of this frame, n = 1..4 (honours mcla_sim_force_n), after A2's value or the
// timer's own measurement; also counts it for the log line. `substeps` = what the frame runs.
// P1: `quantum_hz` = the grid n counts in (60, or 120 above a 60 cap: TIMING.md section 15).
int NextStep(int* substeps, int* quantum_hz = nullptr);
// Heartbeat (once per counted frame, on the game thread): the game-thread pacer, the substep reset
// for the reset path, the missed-frames rule and the `[mcla-sim]` line every mcla_sim_log_s seconds.
void OnFrameStart(int64_t now_ns);

// ---- smoothing counters (T2, T3): what the hooks saw, for the log line
enum SmoothKind { kSmoothCameraPos = 0, kSmoothCameraLookAt = 1, kSmoothChassis = 2, kSmoothKinds = 3 };
// dt class of a call: 0 = about 1/30, 1 = about 1/60, 2 = other; changed = the hook altered the value.
void NoteSmooth(SmoothKind kind, double dt, bool changed);
// The timer's current dt (seconds). False when unreadable or not a sane step.
bool TimerDt(double* dt);

// ---- Flash movies (T4, T5)
bool UiMovieHold(uint32_t context);                              // true = skip this movie update
bool UiCountdown(uint32_t context, double game_dt, double* out); // false = context unknown (pass through)

// ---- the race clock (T6): whole ms to add for a step of dt_ms; `race` keys the carry
int RaceClockMs(uint32_t race, double dt_ms);

// First lines of the hook files: "armed" check for the registry.
inline bool RaceClockActive() { return Enabled() && REXCVAR_GET(mcla_race_clock) != "off"; }

}  // namespace mcla::sim
