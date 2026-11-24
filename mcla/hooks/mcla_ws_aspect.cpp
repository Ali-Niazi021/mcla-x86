// mcla_ws_aspect (W3a-d): the four loads of the 16:9 constant become W/H (docs/mcla/WIDESCREEN.md
// section 5). One hook, four sites (four functions only because the host cannot tell the sites
// apart from the registers); cvar mcla_ws_aspect_mask has one bit per site for A/B.
//
// Hor+ is the aspect alone: Perspective (sub_8217D460) keeps the vertical FOV and sub_8217CB68 sets
// tanH = tanV x GetAspect(), so the culling planes widen with it. The constant 0x8201E7EC (16/9,
// `3FE38E39`) has exactly four `lfs` consumers, each right after `bl IsWidescreen` (0x82178848) on
// the widescreen branch; the 4:3 branch loads 0x8201E7E8 and is never hooked. With a real W x H
// back buffer the screen IS W/H for all four, unlike prior art (its buffer stays 1280x720, so it
// keeps three of them at 16:9). The .rdata constant is not written.
//
//   bit 1   0x822E5E68  lfs f12  sub_822E5E10  scene camera: aspect = lerp([0x827E1AD8],[0x827E1ADC],f4) x f12
//   bit 2   0x82233EB4  lfs f0   sub_82233E18  rear-view mirror: ortho right edge = H x aspect, bottom = H
//   bit 4   0x82214BB8  lfs f10  sub_82214AC0  not analysed (uses renderer +416/+420)
//   bit 8   0x8223E5E0  lfs f13  sub_8223E230  compared with the current viewport's aspect (+700)
//
// Every hook runs AFTER its `lfs` (after_instruction) and overwrites the float register with the
// single-precision W/H. Bits 4 and 8 are the two to switch off first if a HUD 3D element or the
// mirror looks wrong (mask 3, then 1). The first call of each site logs the constant it replaced.
// The pause-map site (bit 16) is mcla_ws_map_aspect.cpp.
//
// At exactly 16:9 (1920x1080, 1600x900) W/H is the game's own constant: ws::AspectFor() says no for every
// bit and the whole file stays idle (nothing is replaced, nothing is logged).
//
// Off: widescreen off (mcla_widescreen_width), the mask bit cleared, a back buffer that is exactly 16:9, or
// MCLA_HOOK_WS_ASPECT=0.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-aspect", "mcla_ws_aspect_mask", MCLA_HOOK_WS_ASPECT != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_aspect_mask) != 0; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_ASPECT

namespace {

// The shared body. `fpr` is the float register that just got the 16:9 constant.
void Apply(PPCRegister& fpr, int32_t bit, const char* site, std::atomic<bool>& logged) {
  MCLA_HOOK_ENTER();
  double aspect = 0.0;
  if (!mcla::ws::AspectFor(bit, &aspect)) {
    return;
  }
  const double replaced = fpr.f64;
  fpr.f64 = aspect;
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-ws-aspect] {}: 16:9 constant {:.4f} -> {:.4f} (mask bit {})", site, replaced, aspect, bit);
  }
}

}  // namespace

void mcla_hook_ws_aspect_camera(PPCRegister& f12) {
  static std::atomic<bool> logged{false};
  Apply(f12, mcla::ws::kAspectCamera, "scene camera 0x822E5E68", logged);
}

void mcla_hook_ws_aspect_mirror(PPCRegister& f0) {
  static std::atomic<bool> logged{false};
  Apply(f0, mcla::ws::kAspectMirror, "rear-view mirror 0x82233EB4", logged);
}

void mcla_hook_ws_aspect_82214bb8(PPCRegister& f10) {
  static std::atomic<bool> logged{false};
  Apply(f10, mcla::ws::kAspectUiA, "sub_82214AC0 0x82214BB8", logged);
}

void mcla_hook_ws_aspect_8223e5e0(PPCRegister& f13) {
  static std::atomic<bool> logged{false};
  Apply(f13, mcla::ws::kAspectUiB, "sub_8223E230 0x8223E5E0", logged);
}

#else  // MCLA_HOOK_WS_ASPECT == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_aspect_camera(PPCRegister&) {}
void mcla_hook_ws_aspect_mirror(PPCRegister&) {}
void mcla_hook_ws_aspect_82214bb8(PPCRegister&) {}
void mcla_hook_ws_aspect_8223e5e0(PPCRegister&) {}

#endif
