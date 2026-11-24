// mcla_t_ui_movie_advance (T4): Flash movies (HUD, menus) step 30 times a second whatever the frame
// rate (docs/mcla/TIMING.md section 4.2). Only with --mcla_sim60=true and --mcla_ui_30hz=true.
//
// swfCONTEXT update = sub_825EDC08(r3 = ctx, f1 = dt, r5 = one-frame flag, r6). The callers
// (mcUIManager::Update sub_821FC588 at 0x821FC8FC, the HUD sub_8223D648 at 0x8223D6C8) pass the
// static byte 0x827DC1FC (= 1): "exactly one movie frame per call". So every movie runs at the update
// rate: 30 a second as shipped, 60 with the 60 fps world step. Site 0x825EDC6C, BEFORE the branch:
//     0x825EDC48  cmplwi cr6,r25,0       r25 = the one-frame flag (r31 = ctx)
//     0x825EDC6C  beq cr6,0x825edc78     <- hooked BEFORE this: flag clear = the movie accumulates dt itself
//     0x825EDC70  stfs f31,24(r31)       flag set: accumulator = one frame -> the step below runs once
//     0x825EDD44  (the "not due" exit: clears byte +136, releases the lock taken at entry)
// The hook returns true (jump to 0x825EDD44: that exit is what the game takes when no movie frame is
// due, so the skip is balanced) when this context has not yet earned its next 1/30 s: per context,
// since += the frame's unscaled step ([0x827D7558], once per timer frame) and a step is let through
// when since >= 1/30 - 1 ms. The unscaled step, not the game's dt: a paused world (time scale 0)
// must keep its menus running. At a 30 cap (n = 2) it never holds. Flag clear (a caller that passes
// elapsed time) is not touched.
//
// Off: --mcla_sim60=false (default), --mcla_ui_30hz=false, or MCLA_HOOK_SIM_UI_ADVANCE=0.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

namespace {

mcla::hooks::Registration g_registration("sim-ui-advance", "mcla_ui_30hz", MCLA_HOOK_SIM_UI_ADVANCE != 0,
                                         [] { return mcla::sim::Enabled() && REXCVAR_GET(mcla_ui_30hz); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_SIM_UI_ADVANCE

bool mcla_hook_ui_movie_advance(PPCRegister& r31, PPCRegister& r25) {
  MCLA_HOOK_ENTER();
  if (!mcla::sim::Enabled() || !REXCVAR_GET(mcla_ui_30hz) || (r25.u32 & 0xFF) == 0) {
    return false;
  }
  return mcla::sim::UiMovieHold(r31.u32);
}

#else  // MCLA_HOOK_SIM_UI_ADVANCE == 0: keeps hooks_timing.toml linking, does nothing.

bool mcla_hook_ui_movie_advance(PPCRegister&, PPCRegister&) { return false; }

#endif
