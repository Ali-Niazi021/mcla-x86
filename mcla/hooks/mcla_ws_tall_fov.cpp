// mcla_ws_tall_fov (S4 plan item 2): Vert+ for a back buffer taller than 16:9 (1280x800, 1440x900, 1920x1200). Only
// with widescreen on (mcla_widescreen_width, see mcla_ws_common.h) and a tall buffer; cvar mcla_ws_tall_fov
// (vert = default, hor = the game's own Hor-).
//
// Why. The game treats 16:10 as widescreen (IsWidescreen sub_82178848 returns XGetVideoMode().fIsWideScreen, which the
// SDK sets when W x 3 >= H x 4), so it takes its 16:9 branch, and mcla_ws_aspect hooks every load of the 16/9 constant to
// W/H. Perspective (sub_8217D460) keeps the VERTICAL field of view and sub_8217CB68 sets tanH = tanV x aspect (V), so a
// buffer taller than 16:9 sees LESS than the 16:9 horizontal view: Hor-. At 1.6 that is 10 % of the width. Vert+ widens
// the vertical angle instead, so the horizontal view is the 16:9 one and the extra rows add to the top and bottom.
//
// The site (V in the generated code, nfsmw_recomp.61.cpp:8902). sub_822E5E10(this, f1 fov, f2 near, f3 far, f4) is the
// scene camera; it is called from sub_822C0320 (0x822C08E4) with the FOV loaded at 0x822C08E0, which both the gameplay
// FOV and the cutscene's `fdivs` result (the cutscene hook sets f1 = 16/9 so that angle is the 16:9 vertical one) end up in.
//     0x822E5E34  fmr f30,f1            f30 = the FOV, the FULL vertical angle in DEGREES, callee-saved by the function's
//                                       own __savefpr_28 / __restfpr_28
//     0x822E5E4C  fneg f2,f31 ; b 0x822E5E94   explicit aspect (f4 < -0.1): skips the site, left alone
//     0x822E5E68  lfs f12,-6164(r11)    the 16/9 constant            (mcla_ws_aspect_camera, hooked)
//     0x822E5E88  fmadds f11,f13,f31,f0 lerp of the aspect by f4
//     0x822E5E90  fmuls f2,f11,f12      f2 = the aspect                                  <- the hook, AFTER this
//     0x822E5E94  ...                   joins; 0x822E5EA0 `fmr f1,f30` hands the FOV to Perspective (+696), f2 is +700
// sub_8217CB68: tanV = tan(fov x k) with k = 0x820086A8 = 3C0EFA35 = pi / 360 (V, read in the PE): so tanV is the tangent
// of HALF the degrees. The hook widens f30 only:
//     fov' = atan(tan(fov x k) x (16/9) / aspect) / k,      aspect = the buffer's W / H (not f2)
// so that tanH' = tanV' x (aspect x lerp) = tanV x (16/9) x lerp: exactly what a 16:9 buffer would give the same camera,
// whatever the lerp f4 asks for. 60 degrees -> 65.36 and 45 -> 49.43 at 1.6 (mcla_ws_sizing.h VertPlusFov); the identity at
// 16:9 and wider, where nothing is done (1280x720 and 1920x1080 never reach the arithmetic).
//
// Not touched: the other 19 Perspective callers (render-to-texture cameras with their own FOV: sub_8223F870 and its
// constant 0x82003768; a second view of it uses GetAspect at 0x8223FB4C, so that element stays Hor-). Look at them in the
// shots. LOD or screen-size culling that reads tanV (+712) will see the wider vertical angle [INFERENCE: ~11 % lower LOD at
// 1.6]. LARecomp's FOVScale site is the earlier 0x822C08E0 [P]; this one leaves the explicit-aspect path alone.
//
// Log: the first three calls, `[mcla-ws-tall-fov] camera: fov <in> -> <out> degrees at buffer aspect <a> (f2 <f2>)`, and one
// `active` line.
//
// codegen entry: a `void` hook, address 0x822E5E90, registers f2, f30, `after_instruction = true` (it needs the aspect
// the `fmuls` just wrote and must run before the join), name mcla_hook_ws_tall_fov (mcla/codegen/hooks_widescreen.toml).
//
// Off: --mcla_ws_tall_fov=hor, widescreen off, a buffer that is not taller than 16:9, or MCLA_HOOK_WS_TALL_FOV=0 at build time.
#include <atomic>
#include <bit>
#include <cstdint>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_STRING(mcla_ws_tall_fov, "vert", "MCLA",
                      "MCLA widescreen: the camera on a back buffer taller than 16:9 (1280x800, 1920x1200). vert = "
                      "Vert+: the vertical angle is widened so the horizontal view is the 16:9 one (default); hor = "
                      "the game's own Hor-: the vertical angle stays, the sides lose view. Only with widescreen on "
                      "and a tall buffer; read on every camera update")
    .allowed({"vert", "hor"});

namespace {

mcla::hooks::Registration g_registration("ws-tall-fov", "mcla_ws_tall_fov", MCLA_HOOK_WS_TALL_FOV != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_tall_fov) == "vert"; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_TALL_FOV

namespace {

// sub_8217CB68's k: 0x820086A8 = 3C0EFA35 = pi / 360 (the FOV is the full vertical angle in degrees).
const float kFovToHalfRadians = std::bit_cast<float>(0x3C0EFA35u);

}  // namespace

// 0x822E5E90 in sub_822E5E10, AFTER `fmuls f2,f11,f12`: f2 = the aspect Perspective will get, f30 = the FOV (degrees).
void mcla_hook_ws_tall_fov(PPCRegister& f2, PPCRegister& f30) {
  MCLA_HOOK_ENTER();
  if (mcla::ws::Width() == 0 || !mcla::ws::Tall() || REXCVAR_GET(mcla_ws_tall_fov) != "vert") {
    return;
  }
  const double fov = f30.f64;
  if (!(fov > 0.0 && fov < 170.0)) {
    return;  // not a camera angle: leave it
  }
  const double aspect = mcla::ws::Aspect();  // the buffer's W / H as the game's float
  const double widened = double(float(mcla::ws::sizing::VertPlusFov(fov, aspect, double(kFovToHalfRadians))));
  static std::atomic<uint32_t> calls{0};
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0) {
    REXLOG_INFO("[mcla-ws-tall-fov] active: a {}x{} buffer is taller than 16:9, the camera widens its vertical angle "
                "so the horizontal view stays the 16:9 one (--mcla_ws_tall_fov=hor keeps the game's Hor-)",
                mcla::ws::Width(), mcla::ws::Height());
  }
  if (n < 3) {
    REXLOG_INFO("[mcla-ws-tall-fov] camera: fov {:.4f} -> {:.4f} degrees at buffer aspect {:.4f} (f2 {:.4f})", fov,
                widened, aspect, f2.f64);
  }
  f30.f64 = widened;
}

#else  // MCLA_HOOK_WS_TALL_FOV == 0: keeps a codegen entry linking, does nothing.

void mcla_hook_ws_tall_fov(PPCRegister&, PPCRegister&) {}

#endif
