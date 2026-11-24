// mcla_t_sim_step (T1): the quantised world step of the 60 fps timing (docs/mcla/TIMING.md section 3).
// Only with --mcla_sim60=true; off = the game's fixed 1/30 s step and 2 physics substeps.
//
// The shipped game publishes a FIXED step on every frame (timer path 2, +0x3C = 1): dt = [+0x20] =
// 1/30 s, and the frame (sub_822C1FA8) runs the number of physics substeps its frame-rate object
// holds at +0x10 (2: two passes of 1/60 s). Nothing in the guest waits for a vblank: the frame rate is
// the host cap. So a 60 cap with the shipped step is double speed.
//
// Site 0x821BDB90 in rage::sysTimeManager::Update (sub_821BDA90), AFTER `lfs f11,32(r3)`:
//     0x821BDB90  lfs f11,32(r3)        f11 = [timer+0x20] = 1/30       <- hooked AFTER this
//     0x821BDB98  fmuls f0,f11,f13      f13 = time scale: f0 = the game's dt
//     0x821BDB9C  stfs f11,88(r3)       +0x58 = the unscaled step; f0 -> +0x08, both accumulators
// The hook replaces f11 with n/60 s, n = 1..4 from sim::NextStep() (AdvanceWorldClock: whole 1/60 s
// quanta of real time, debt carried, at most 4 = 1/15 s, resync on a gap; n = 2 at a 30 cap is the
// shipped 1/30 s bit for bit). The physics step stays the shipped 1/60 s: the frame-rate object's
// substep word (BE32 [[0x828CCEA0]+0x10], read by the frame at 0x822C230C right after Update, on this
// thread) is set to n for this frame (cvar mcla_physics_substeps forces another count; never 0).
//
// Not touched: the reset path (0x821BDBC8, byte +0x38: after loads) publishes the shipped [+0x20];
// the heartbeat resets the substep word to 2 before every Update so that frame runs the shipped two.
// The reset guard at 0x821BDB08 is never patched (LARecomp: removing it blew up the audio).
// The time scale (+0x54, slow motion, pause) multiplies after the hook: unchanged.
//
// r3 must be the one engine timer (0x827D7500); any other timer object keeps the game's step.
// Off: --mcla_sim60=false (default), or MCLA_HOOK_SIM_STEP=0 at build time.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-step", "mcla_sim60", MCLA_HOOK_SIM_STEP != 0,
                                         [] { return mcla::sim::Enabled(); }, /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_STEP

void mcla_hook_sim_step(PPCRegister& r3, PPCRegister& f11) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || r3.u32 != mcla::sim::kTimer) {
    return;
  }
  int substeps = 2;
  int quantum_hz = 60;
  const int n = mcla::sim::NextStep(&substeps, &quantum_hz);
  // The single-precision n/60 the timer would store (P1: n/120 on the 120 grid).
  f11.f64 = quantum_hz == 60 ? mcla::sim::math::StepSeconds(n) : double(float(n / double(quantum_hz)));
  mcla::sim::SetSubsteps(substeps);
}

#else  // MCLA_HOOK_SIM_STEP == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_sim_step(PPCRegister&, PPCRegister&) {}

#endif
