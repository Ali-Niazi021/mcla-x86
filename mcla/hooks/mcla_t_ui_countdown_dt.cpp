// mcla_t_ui_countdown_dt (T5): the race-start countdown keeps real time when Flash movie steps are
// held (docs/mcla/TIMING.md section 4.2). Only with --mcla_sim60=true and --mcla_ui_30hz=true.
//
// Site 0x822124DC in sub_82212298 (the HUD countdown component), BEFORE `addi r4,r8,-9924`:
//     0x822124D4  lwz r3,56(r3)          r3 = the HUD movie's swfCONTEXT
//     0x822124D8  fmr f1,f31             f31 = [timer+0x64] (the measured real dt of this frame) or f30
//     0x822124DC  addi r4,r8,-9924       <- hooked BEFORE this (r4 = "countdownUpdate", the variable name)
//     0x822124E0  bl 0x825ee188          set the script variable countdownUpdate = f1 on the movie
// The script adds that variable ONCE PER MOVIE STEP. Shipped: a step per frame, the variable is the
// frame's time, the countdown is real time. With T4 holding steps at 60 fps the variable must be the
// time since the movie's LAST step: the hook adds the game's value to a per-context accumulator that
// restarts whenever T4 let a step through since the last call (order-agnostic: either order within a
// frame is right to within one frame), and passes the accumulator. A game value of 0 stays 0. A
// context T4 has never seen (a movie not stepped by sub_825EDC08) keeps the game's value. At a 30
// cap every frame steps and the value is bit for bit the game's.
//
// Off: --mcla_sim60=false (default), --mcla_ui_30hz=false, or MCLA_HOOK_SIM_UI_COUNTDOWN=0.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-ui-countdown", "mcla_ui_30hz", MCLA_HOOK_SIM_UI_COUNTDOWN != 0,
                                         [] { return mcla::sim::Enabled() && REXCVAR_GET(mcla_ui_30hz); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_UI_COUNTDOWN

void mcla_hook_ui_countdown_dt(PPCRegister& r3, PPCRegister& f1) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || !REXCVAR_GET(mcla_ui_30hz)) {
    return;
  }
  double adjusted = 0.0;
  if (mcla::sim::UiCountdown(r3.u32, f1.f64, &adjusted)) {
    f1.f64 = adjusted;
  }
}

#else  // MCLA_HOOK_SIM_UI_COUNTDOWN == 0: keeps hooks_timing.toml linking, does nothing.

void mcla_hook_ui_countdown_dt(PPCRegister&, PPCRegister&) {}

#endif
