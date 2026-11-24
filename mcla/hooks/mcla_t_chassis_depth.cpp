// mcla_t_chassis_depth (T3): the chassis ground-depth smoothing at any frame rate (docs/mcla/TIMING.md
// section 4.1). Only with --mcla_sim60=true and --mcla_smoothing_fix=true.
//
// Site 0x82563720 in sub_82563298, BEFORE `lis r11,-32256`:
//     0x82563704  cmpwi cr6,r11,60      r11 = round(1/dt)
//     0x82563708  blt cr6,0x82563718
//     0x8256370C  lis r11,-32256 ; lfs f0,14992(r11) ; b 0x82563720     60+: alpha = 0.05 (0x82003A90)
//     0x82563718  lis r11,-32256 ; lfs f0,17032(r11)                     below 60: alpha = 0.10 (0x82004288)
//     0x82563720  lis r11,-32256        <- hooked BEFORE this: f0 = the game's alpha, cr6.lt = (fps < 60)
//     0x82563724  fmuls f12,f0,f13      new = alpha x target + (1 - alpha) x old (0x82563738 fmadds)
// (r11 is overwritten on both paths, so the branch is taken from cr6.) alpha(dt) = 1 - 0.9^(30 dt):
// 0.10 at 1/30 s, 0.0513 at 1/60 s. The game's two values are within 3 % of that at their own step
// (and this smoothing may run in the 1/60 s physics passes, where the game's own 60+ value applies),
// so a step of 1/30 or 1/60 keeps the game's value and any other step gets the formula.
// dt = the timer's step [0x827D7508] at the time of the call.
//
// Off: --mcla_sim60=false (default), --mcla_smoothing_fix=false, or MCLA_HOOK_SIM_CHASSIS=0.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-chassis", "mcla_smoothing_fix", MCLA_HOOK_SIM_CHASSIS != 0,
                                         [] { return mcla::sim::Enabled() && REXCVAR_GET(mcla_smoothing_fix); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_CHASSIS

void mcla_hook_chassis_depth(PPCRegister& f0, PPCCRRegister& cr6) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || !REXCVAR_GET(mcla_smoothing_fix)) {
    return;
  }
  double dt = 0.0;
  if (!mcla::sim::TimerDt(&dt)) {
    return;
  }
  const double game = f0.f64;
  const double alpha = mcla::sim::math::ChassisAlpha(game, /*ge60=*/!cr6.lt, dt);
  const bool changed = alpha != game;
  if (changed) {
    f0.f64 = double(float(alpha));
  }
  mcla::sim::NoteSmooth(mcla::sim::kSmoothChassis, dt, changed);
}

#else  // MCLA_HOOK_SIM_CHASSIS == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_chassis_depth(PPCRegister&, PPCCRRegister&) {}

#endif
