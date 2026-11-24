// mcla_t_camera_pos (T2a): the chase camera's position smoothing at any frame rate (docs/mcla/TIMING.md
// section 4.1). Only with --mcla_sim60=true and --mcla_smoothing_fix=true.
//
// Site 0x82320468 in sub_82320298 (the chase camera), BEFORE `addi r9,r1,80`:
//     0x82320454  cmpwi cr6,r11,60      r11 = round(1/dt), 1/dt = [timer+0x0C]
//     0x8232045C  lfs f13,224(r11)      f13 = the tuning value F (camera tuning +224)
//     0x82320460  bge cr6,0x82320468    60 or more: k = F (about four times stiffer than the 30 fps feel)
//     0x82320464  fmuls f13,f13,f30     below 60: k = F x 0.5
//     0x82320468  addi r9,r1,80         <- hooked BEFORE this: f13 = the game's k, cr6.lt = (fps < 60)
//     0x8232046C  stfs f13,80(r1)       new = in x k + old x (1 - k)
// k30 = what the game uses at 1/30 s (k, or k x 0.5 when the game took its 60+ branch); the hook
// returns k(dt) = 1 - (1 - k30)^(30 dt) with dt = the timer's step [0x827D7508] (live, so a substep or
// a slow-motion step is right too). At a step of exactly 1/30 in the below-60 branch the game's own
// value is already k30 and is left bit for bit. A factor outside (0, 1) is left alone.
//
// Off: --mcla_sim60=false (default), --mcla_smoothing_fix=false, or MCLA_HOOK_SIM_CAMERA_POS=0.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-camera-pos", "mcla_smoothing_fix", MCLA_HOOK_SIM_CAMERA_POS != 0,
                                         [] { return mcla::sim::Enabled() && REXCVAR_GET(mcla_smoothing_fix); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_CAMERA_POS

void mcla_hook_camera_pos(PPCRegister& f13, PPCCRRegister& cr6) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || !REXCVAR_GET(mcla_smoothing_fix)) {
    return;
  }
  double dt = 0.0;
  if (!mcla::sim::TimerDt(&dt)) {
    return;
  }
  const double game = f13.f64;
  const double k = mcla::sim::math::SmoothK(game, /*ge60=*/!cr6.lt, dt);
  const bool changed = k != game;
  if (changed) {
    f13.f64 = double(float(k));
  }
  mcla::sim::NoteSmooth(mcla::sim::kSmoothCameraPos, dt, changed);
}

#else  // MCLA_HOOK_SIM_CAMERA_POS == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_camera_pos(PPCRegister&, PPCCRRegister&) {}

#endif
