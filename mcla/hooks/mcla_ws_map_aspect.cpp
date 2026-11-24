// mcla_ws_map_aspect (W5): the pause map's cursor box gets the wide aspect
// (docs/mcla/WIDESCREEN.md section 5, prior art LARecomp `MapVisible*`; effect not seen in our
// image yet). Only with widescreen on; mask bit 16 of mcla_ws_aspect_mask.
//
// Site 0x82620700 in sub_826206B0 (the map's visible box / cursor), AFTER `lfs f27,-1336(r11)`:
//     0x826206E0  lfs f27,-1332(r11)     4:3 constant 0x8201FACC (1.3333)
//     0x826206EC  bl 0x82178848          IsWidescreen
//     0x826206F8  beq 0x82620704         not wide: keep the 4:3 value
//     0x82620700  lfs f27,-1336(r11)     16:9 constant 0x8201FAC8 (3FE38EF3 = 1.7778, not exactly 16/9)
// The 3D map itself uses the scene camera (wide for free); this box limits where the cursor can go,
// so with W/H it reaches the sides of the wide screen. The map's icon placement (sub_82627738,
// 0x826277E8) and sub_8221BE98 stay 16:9: with a true wide buffer they must be judged from a
// screenshot (icons against roads). The hook runs only on the widescreen branch.
//
// Off: widescreen off, mask bit 16 cleared (--mcla_ws_aspect_mask=15), or MCLA_HOOK_WS_MAP_ASPECT=0.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-map-aspect", "mcla_ws_aspect_mask", MCLA_HOOK_WS_MAP_ASPECT != 0,
                                         [] {
                                           return mcla::ws::Armed() &&
                                                  (REXCVAR_GET(mcla_ws_aspect_mask) & mcla::ws::kAspectMap) != 0;
                                         },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_MAP_ASPECT

void mcla_hook_ws_map_aspect(PPCRegister& f27) {
  MCLA_HOOK_ENTER();
  double aspect = 0.0;
  if (!mcla::ws::AspectFor(mcla::ws::kAspectMap, &aspect)) {
    return;
  }
  const double replaced = f27.f64;
  f27.f64 = aspect;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-ws-map-aspect] pause map box 0x82620700: {:.4f} -> {:.4f}", replaced, aspect);
  }
}

#else  // MCLA_HOOK_WS_MAP_ASPECT == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_map_aspect(PPCRegister&) {}

#endif
