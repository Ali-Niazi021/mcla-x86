// Midnight Club: Los Angeles: the mid-asm hooks (docs/mcla/HOOKS_SPEC.md; declared in
// games/mcla/codegen/hooks.toml, one hook per file in this folder).
//
// What a hook is. The codegen splices a call to a global C++ function into the middle of a
// recompiled guest function and hands it the registers named in hooks.toml BY REFERENCE
// (PPCRegister&, in that order; there is no ctx or memory base argument). A bool hook decides the
// control flow: true takes the jump / return the toml names, false falls through to the guest
// instruction. Hooks run on whichever guest thread reached the site.
//
// Every hook file follows the same pattern (docs/mcla/PLAN.md Phase 5):
//   * a build flag MCLA_HOOK_<NAME> (games/mcla/sources.cmake passes =1; 0 here when nothing does).
//     With the flag 0 the function still exists and does nothing (void) or returns false, so
//     hooks.toml keeps linking: switching a hook off at build time never needs a codegen run;
//   * a cvar mcla_<name>, default = what is wanted by default (the game's own behaviour for the
//     hooks that change it), read on every call, so `--mcla_<name>=false` works without rebuilding;
//   * one "[mcla-<tag>] active" log line, written by the first hook call of the run for every hook
//     file at once (AnnounceHooks), so a log shows which hooks are in and which are switched off
//     even for hooks that fire once in a run (the fatal ones).
//
// Two hooks are A/B switches that change what the game renders and default OFF (cvar false):
// mcla_fast_clear_off and mcla_noaa (docs/mcla/RENDER_NOTES.md).
//
// Log tags: heartbeat, fatal, io, ui-lights, skip-intro, fence-spin, state, fast-clear, noaa.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rex/logging.h>

#ifndef MCLA_HOOK_HEARTBEAT
#define MCLA_HOOK_HEARTBEAT 0
#endif
#ifndef MCLA_HOOK_RAGE_FATAL
#define MCLA_HOOK_RAGE_FATAL 0
#endif
#ifndef MCLA_HOOK_READ_RETRY_FATAL
#define MCLA_HOOK_READ_RETRY_FATAL 0
#endif
#ifndef MCLA_HOOK_UI_LIGHTS_GUARD
#define MCLA_HOOK_UI_LIGHTS_GUARD 0
#endif
#ifndef MCLA_HOOK_SKIP_INTRO
#define MCLA_HOOK_SKIP_INTRO 0
#endif
#ifndef MCLA_HOOK_FENCE_SPIN
#define MCLA_HOOK_FENCE_SPIN 0
#endif
#ifndef MCLA_HOOK_STATE_TRACE
#define MCLA_HOOK_STATE_TRACE 0
#endif
#ifndef MCLA_HOOK_FAST_CLEAR_OFF
#define MCLA_HOOK_FAST_CLEAR_OFF 0
#endif
#ifndef MCLA_HOOK_NOAA
#define MCLA_HOOK_NOAA 0
#endif

namespace mcla::hooks {

// A hook file declares one of these (static, at namespace scope). `enabled` returns the cvar;
// `compiled` is the MCLA_HOOK_<NAME> value; `default_on` is the cvar's default (false for the A/B
// switches that change what the game does, so the log can say "armed, off by default").
struct Registration {
  Registration(const char* tag, const char* cvar, bool compiled, bool (*enabled)(),
               bool default_on = true);
};

// Writes "[mcla-<tag>] active" (or "... compiled out" / "... off" / "... armed") for every
// registered hook, once per process. Called by MCLA_HOOK_ENTER.
void AnnounceHooks();

inline std::atomic<bool> g_announced{false};

}  // namespace mcla::hooks

// First line of every hook function: the announcement, once, from whichever hook runs first.
#define MCLA_HOOK_ENTER()                                                                        \
  do {                                                                                           \
    if (!::mcla::hooks::g_announced.load(std::memory_order_relaxed)) [[unlikely]] {              \
      ::mcla::hooks::AnnounceHooks();                                                            \
    }                                                                                            \
  } while (0)

namespace mcla {

// ---- state shared between hook files (read by the host later: profile mode()/in_menu(), the
// ---- watchdog, the log)

// Frames the engine timer has completed (mcla_heartbeat.cpp). 0 until the first one: the legals,
// the logos and every blocking load before the first frame run without it.
uint64_t FrameCount();

// True once the RAGE fatal hook has seen a fatal (mcla_rage_fatal.cpp).
bool FatalSeen();

namespace state {
// The game's current top state (mcla_state_trace.cpp): the name of the state activated last and of
// its parent. Copies into `name` / `parent` (NUL-terminated, truncated to the buffer); returns the
// number of activations seen so far (0 = none yet, buffers get ""). Safe from any thread.
uint64_t Current(char* name, size_t name_size, char* parent, size_t parent_size);
}  // namespace state

}  // namespace mcla
