// mcla_ws_cutscene_fov (W4): cutscene cameras keep their 16:9 vertical angle on a wide buffer
// (docs/mcla/WIDESCREEN.md section 5). Only with widescreen on; cvar mcla_ws_cutscene_fov
// (default true) switches just this hook off for A/B.
//
// The scene camera update (sub_822C0320) builds a cutscene camera from an AUTHORED HORIZONTAL angle:
//     0x822C04E4  bl 0x82171600            GetAspect(): f1 = the viewport's aspect
//     0x822C04E8  fdivs f12,f28,f1         <- the hook, BEFORE this: vertical angle = authored / aspect
//     0x822C04EC  stfs f12,80(r1)          nothing reads f1 after the division
// With the buffer at W/H the aspect is 2.1667, so the vertical angle shrinks 18 % and the cutscene
// zooms in (Vert-). Replacing f1 by 16/9 (as the shipped game saw it) gives back the 16:9 vertical
// angle; Perspective then multiplies by the real aspect, so the cutscene shows more at the sides
// (Hor+) instead of cropping top and bottom. LARecomp does the same at this site.
//
// The incoming f1 of the first three calls is logged (it is W/H when the camera hook is on and the
// cutscene viewport is the whole screen; anything else means a letterboxed viewport, which this
// hook does not rescale: look at it in the first cutscene).
//
// Off: widescreen off, --mcla_ws_cutscene_fov=false, or MCLA_HOOK_WS_CUTSCENE_FOV=0.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-cutscene-fov", "mcla_ws_cutscene_fov",
                                         MCLA_HOOK_WS_CUTSCENE_FOV != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_cutscene_fov); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_CUTSCENE_FOV

void mcla_hook_ws_cutscene_fov(PPCRegister& f1) {
  MCLA_HOOK_ENTER();
  if (mcla::ws::Width() == 0 || !REXCVAR_GET(mcla_ws_cutscene_fov)) {
    return;
  }
  const double sixteen_nine = double(16.0f / 9.0f);  // 0x8201E7EC, the constant the game uses
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
    REXLOG_INFO("[mcla-ws-cutscene-fov] cutscene camera: viewport aspect {:.4f} -> {:.4f} (wide aspect {:.4f})",
                f1.f64, sixteen_nine, mcla::ws::Aspect());
  }
  f1.f64 = sixteen_nine;
}

#else  // MCLA_HOOK_WS_CUTSCENE_FOV == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_cutscene_fov(PPCRegister&) {}

#endif
