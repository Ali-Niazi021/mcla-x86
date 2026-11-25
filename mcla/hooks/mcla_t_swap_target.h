// MCLA present-immediate lever (W4.5; docs/PLAN.md section 6 M5): the guest's swap-release rule as pure arithmetic,
// and the timed-swap hand-off between the two hooks that meet on the presenting thread. No SDK, no guest memory, no
// cvars: everything here is a function of numbers (std only), so a unit test compiles it standalone and checks it.
//
// The rule is the swap callback sub_82419818 (D3D's present callback, run on the command-processor thread when the
// CP executes the INTERRUPT that D3DDevice_Present writes; 0x824198AC..0x824198D8):
//   target = last_target + interval;
//   if (target > vblank_count) keep it;                       // on time: waits for that vblank
//   else { target = vblank_count;                             // late: the current vblank ...
//          if (interval != 0 && pct > threshold) ++target; }  // ... plus one past the present-immediate threshold
// `pct` is how far into the current vblank period the callback runs, 1..100 (the callback clamps it to 100);
// `threshold` is the 7-bit D3DRS_PRESENTIMMEDIATETHRESHOLD the swap carries in bits 0..6 of the callback argument
// (the game ships 0, so every late frame is pushed to the next vblank: 33.3 ms at 60 Hz, interval 1). A target equal
// to the current vblank is released at once ([shared+4] = 0); a later one is queued until the vblank handler
// (sub_82419718) reaches it. A threshold of 100 can never be exceeded by pct, so a late frame is released at once.
#pragma once

#include <algorithm>
#include <cstdint>

// Build flag of the two hooks of mcla_t_present_immediate.cpp (1 in mcla/sources.cmake). The default is here, next to
// the rule, because mcla_t_present_interval.cpp (the timed-swap publisher) and the hook file both include this header.
#ifndef MCLA_HOOK_SIM_PRESENT_IMMEDIATE
#define MCLA_HOOK_SIM_PRESENT_IMMEDIATE 0
#endif

namespace mcla::sim::math {

constexpr uint32_t kSwapPctMax = 100;
// The threshold that makes every late frame release at once (pct never exceeds kSwapPctMax).
constexpr uint32_t kImmediateThreshold = 100;

// The target vblank of a swap. All counters are the guest's 32-bit ones (they compare unsigned, they wrap).
// last_target = [dev+16556], interval = bits 8..11 of the callback argument, vblank_count = [dev+16548],
// pct_into_period = r7 of the callback (clamped here to 100 as the callback does), threshold = bits 0..7 of the argument.
inline uint32_t SwapTarget(uint32_t last_target, uint32_t interval, uint32_t vblank_count, uint32_t pct_into_period,
                           uint32_t threshold) {
  const uint32_t target = last_target + interval;
  if (target > vblank_count) {
    return target;
  }
  const bool past_threshold = interval != 0 && std::min(pct_into_period, kSwapPctMax) > threshold;
  return vblank_count + (past_threshold ? 1u : 0u);
}

// What a swap turned out to be, from the callback's own registers (the census, mcla_hook_swap_census):
// r8 = last target, r30 = interval, r10 = vblank count, r11 = the target the callback chose.
enum class SwapClass {
  kOnTime,      // last + interval is still ahead of the vblank counter: waits for its vblank (the 60 Hz cadence)
  kLateAtOnce,  // late, target == the current vblank: released inside the callback
  kLateHeld,    // late, target == the next vblank: held one more vblank (the 33 ms frame)
  kOther,       // anything else (a swap callback of the game altered the target; never seen in practice)
};

inline SwapClass ClassifySwap(uint32_t last_target, uint32_t interval, uint32_t vblank_count, uint32_t target) {
  if (last_target + interval > vblank_count) {
    return SwapClass::kOnTime;
  }
  if (target == vblank_count) {
    return SwapClass::kLateAtOnce;
  }
  return target == vblank_count + 1 ? SwapClass::kLateHeld : SwapClass::kOther;
}

}  // namespace mcla::sim::math

namespace mcla::sim {

// Timed-swap hand-off, one flag per thread. The present-interval hook (0x82419AA0, mcla_t_present_interval.cpp)
// decides whether this swap is timed (an engine-timer frame ran since the previous swap; legals, logos and blocking
// loads are not) and publishes it; the present-immediate hook (0x82419AC0, mcla_t_present_immediate.cpp) runs a few
// instructions later in the same function (sub_824199B0) on the same thread and consumes it. A swap that does not
// pass through 0x82419AA0 (a device interval other than 2) leaves the flag false: never timed.
inline thread_local bool t_swap_timed = false;
inline void NoteSwapTimed(bool timed) { t_swap_timed = timed; }
inline bool TakeSwapTimed() {
  const bool timed = t_swap_timed;
  t_swap_timed = false;
  return timed;
}

}  // namespace mcla::sim
