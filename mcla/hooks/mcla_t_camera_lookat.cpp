// mcla_t_camera_lookat (T2b): the chase camera's look-at smoothing at any frame rate (docs/mcla/TIMING.md
// section 4.1). Only with --mcla_sim60=true and --mcla_smoothing_fix=true. Same formula as
// mcla_t_camera_pos.cpp, second factor (camera tuning +228).
//
// Site 0x823204F4 in sub_82320298, BEFORE `li r9,288`:
//     0x823204E4  lfs f0,228(r11)       f0 = the tuning value F
//     0x823204E8  cmpwi cr6,r9,60       r9 = round(1/dt)
//     0x823204EC  bge cr6,0x823204F4    60 or more: k = F
//     0x823204F0  fmuls f0,f0,f30       below 60: k = F x 0.5
//     0x823204F4  li r9,288             <- hooked BEFORE this: f0 = the game's k, cr6.lt = (fps < 60)
// k(dt) = 1 - (1 - k30)^(30 dt), k30 = k (below-60 branch) or k x 0.5 (60+ branch), dt = [0x827D7508].
//
// Off: --mcla_sim60=false (default), --mcla_smoothing_fix=false, or MCLA_HOOK_SIM_CAMERA_LOOKAT=0.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-camera-lookat", "mcla_smoothing_fix",
                                         MCLA_HOOK_SIM_CAMERA_LOOKAT != 0,
                                         [] { return mcla::sim::Enabled() && REXCVAR_GET(mcla_smoothing_fix); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_CAMERA_LOOKAT

void mcla_hook_camera_lookat(PPCRegister& f0, PPCCRRegister& cr6) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || !REXCVAR_GET(mcla_smoothing_fix)) {
    return;
  }
  double dt = 0.0;
  if (!mcla::sim::TimerDt(&dt)) {
    return;
  }
  const double game = f0.f64;
  const double k = mcla::sim::math::SmoothK(game, /*ge60=*/!cr6.lt, dt);
  const bool changed = k != game;
  if (changed) {
    f0.f64 = double(float(k));
  }
  mcla::sim::NoteSmooth(mcla::sim::kSmoothCameraLookAt, dt, changed);
}

#else  // MCLA_HOOK_SIM_CAMERA_LOOKAT == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_camera_lookat(PPCRegister&, PPCCRRegister&) {}

#endif
