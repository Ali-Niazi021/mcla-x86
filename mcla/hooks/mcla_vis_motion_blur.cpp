// mcla_vis_motion_blur: the length of the game's motion blur streaks at 60 fps (VISUALS stream).
//
// The finding. The game's motion blur reprojects with a "previous" position that is built once per frame. In the branch that
// runs in normal play (the renderer byte [[0x8287E064] + 374] = 1, confirmed in a live run) the previous position is the current
// one minus a displacement times 0.25:
//   * a vehicle (sub_822E8448): previous = current - velocity * dt * 0.25, with dt = the float [0x827D7508] (the timer's step)
//   * the camera (sub_822E8780): previous = current - 0.25 * (the camera's displacement since the last frame)
// Both shrink with the frame time (the vehicle's by dt, the camera's because the displacement of one frame is smaller), so at 60 fps
// the streaks are half the length they have on the console (30 fps).
//
// The two sites, checked against the generated listing (python dis.py sub_822E8448 / sub_822E8780) and the words of the image:
//   sub_822E8448 (the vehicle), the branch taken when the renderer byte is set:
//     0x822E85A4  lis r11,-32120 ; 0x822E85A8  lwz r11,-8092(r11)           r11 = [0x8287E064], the renderer
//     0x822E85AC  lbz r10,374(r11) ; cmplwi cr6,r10,0 ; beq -> 0x822E8630    the code below runs only when the byte is not 0
//     0x822E85B8  lis r11,-32131 ; 0x822E85C8  addi r9,r11,29952             r9 = 0x827D7500, the timer
//     0x822E85E8  lvlx128 v50,r9,r10  (r10 = 8) ; vspltw128 v49,v50,0         dt = the float [0x827D7508], splatted
//     0x822E85F4  lfs f0,-29232(r8)   r8 = 0x82010000: the float at 0x82008DD0 = 0x3E800000 = 0.25   <- f0 = 0.25
//     0x822E85F8  stfs f0,80(r1)                                              <- the hook goes BEFORE this (word 0xD0010050)
//     0x822E8600  lvlx128 v48,r0,r7 ; vspltw128 v47,v48,0                     the 0.25 comes back from the stack, splatted
//     0x822E8608  vmulfp128 v46,v52,v49  (the vector at [r6] x dt) ; 0x822E8624  vmulfp128 v43,v46,v47  (x 0.25)
//     0x822E8628  vsubfp128 v59,v51,v43  (v51 = the position [r4+48])        the previous position
//   f0 is not read again in the function (its only other mention is the lfs / stfs pair above).
//   sub_822E8780 (the camera):
//     0x822E88A4  lwz r11,-8092(r20) ; 0x822E88A8  lbz r11,374(r11) ; cmplwi ; beq -> 0x822E8924      the same renderer byte
//     0x822E88D0  lvx128 v59,r0,r10 (r10 = r29+256: the camera's previous position) ; v63 = the current position ([r1+160])
//     0x822E88D8  vsubfp128 v58,v63,v59                                       the per-frame displacement
//     0x822E88DC  lfs f0,-29232(r7)   r7 = 0x82010000: the same 0.25                                  <- f0 = 0.25
//     0x822E88E0  stfs f0,80(r1)                                              <- the hook goes BEFORE this (word 0xD0010050)
//     0x822E88E8  lvlx128 v57,r0,r6 ; vspltw128 v56,v57,0 ; 0x822E88F0  vmulfp128 v55,v58,v56   displacement x 0.25
//     0x822E8904  stvx128 v55,r0,r9 (r9 = r29+272) ; 0x822E8918  lvx128 v51,r0,r9 ; 0x822E891C  vsubfp128 v50,v63,v51 ;
//     0x822E8920  stvx128 v50,r0,r10                                          [r29+256] = current - 0.25 x displacement
//   f0 is next written by `lfs f0,3796(r9)` at 0x822E8A50 before any read (the earlier `lfs f0,84(r9)` at 0x822E8884 is before the
//   site). sub_822E8780 also calls sub_822E8448 at 0x822E8A68 for one more entry whose velocity vector is 0 (the float 0.0 at
//   0x82000ED4), so the vehicle hook scales a zero there.
// No compounding: sub_822E8780 re-stores the block [r29+208 .. r29+272) from the current camera matrix ([[0x82839704]+64], rows at
// +0 / +16 / +32 / +48) at its end (0x822E8A10 .. 0x822E8A54), so [r29+256] is the real previous position at the next call and a
// scale above 1 does not build up from frame to frame. The vehicle's displacement is a velocity times dt, with no memory either.
// The codegen puts a hook at an address BEFORE the instruction there (see hooks_gfx.toml), i.e. after the lfs: f0 = 0.25 and its
// only use is the `stfs`, which feeds the splat that multiplies the displacement. Scaling f0 scales the streak: 0.25 x scale.
//
// The cvar mcla_vis_motion_blur (default 1; read on every call, live):
//    1   as the game does: the streak follows the frame time (at 60 fps half the console's length)
//   -1   console look: scale = (1/30) / dt (dt = the float [0x827D7508]; 1 when it is not finite or <= 0), clamped to 0.25..4, so
//        the streak matches a 30 fps frame at any frame rate (2 at 60 fps, 1 at 30)
//   > 0  a fixed multiplier (2 = double the game's length)
//    0   no displacement at all: no motion blur from movement (camera and vehicles)
// With 1 the hooks do not touch f0 (bit-identical to the game); with any other value f0 = double(float(f0) * scale).
//
// Codegen entries (mcla/codegen/hooks_vis.toml, included by mcla_manifest.toml). NEEDS A RE-CODEGEN:
//   [[midasm_hook]] address = 0x822E85F8, name = "mcla_hook_vis_mblur_vehicle", registers = ["f0"]
//   [[midasm_hook]] address = 0x822E88E0, name = "mcla_hook_vis_mblur_camera",  registers = ["f0"]
//
// Build switch: MCLA_HOOK_VIS_MOTION_BLUR=0 keeps the functions and does nothing. Log: `[mcla-vis] motion blur: ...` once per change
// of the cvar's value.
#include <atomic>
#include <cmath>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_t_common.h"

#ifndef MCLA_HOOK_VIS_MOTION_BLUR
#define MCLA_HOOK_VIS_MOTION_BLUR 0
#endif

REXCVAR_DEFINE_DOUBLE(mcla_vis_motion_blur, 1.0, "MCLA",
                      "MCLA visuals: the length of the motion blur streaks that come from movement (camera and vehicles). The game "
                      "builds the previous position as the current one minus 0.25 x the displacement of ONE frame, so at 60 fps the "
                      "streaks are half the console's (30 fps). 1 = as the game does, -1 = console look (the streak of a 30 fps "
                      "frame at any frame rate: x (1/30)/dt, limited to 0.25..4), > 0 = a fixed multiplier (2 = double), 0 = no "
                      "motion blur from movement. Live (mid-asm hooks 0x822E85F8 and 0x822E88E0, mcla_vis_motion_blur.cpp)")
    .range(-1.0, 4.0);

namespace {

mcla::hooks::Registration g_registration("vis-motion-blur", "mcla_vis_motion_blur", MCLA_HOOK_VIS_MOTION_BLUR != 0,
                                          [] { return REXCVAR_GET(mcla_vis_motion_blur) != 1.0; },
                                          /*default_on=*/false);

constexpr double kConsoleStep = 1.0 / 30.0;  // the console's frame time
constexpr double kScaleMin = 0.25;
constexpr double kScaleMax = 4.0;

// The factor for f0 (the game's 0.25) this call; 1.0f = leave it alone.
[[maybe_unused]] float Scale() {
  const double v = REXCVAR_GET(mcla_vis_motion_blur);
  if (v == 1.0) {
    return 1.0f;
  }
  double scale = v;
  if (v < 0.0) {  // console look
    float dt = 0.0f;
    scale = 1.0;
    if (mcla::sim::ImageF32(mcla::sim::kTimerDt, &dt) && std::isfinite(dt) && dt > 0.0f) {
      scale = kConsoleStep / double(dt);
    }
    scale = scale < kScaleMin ? kScaleMin : (scale > kScaleMax ? kScaleMax : scale);
    if (std::fabs(scale - 1.0) < 1e-4) {
      scale = 1.0;  // a 30 fps frame: the float rounding of dt must not turn into a change of f0
    }
  }
  static std::atomic<double> logged{1.0};
  if (logged.load(std::memory_order_relaxed) != v && logged.exchange(v, std::memory_order_relaxed) != v) {
    if (v < 0.0) {
      REXLOG_INFO("[mcla-vis] motion blur: console look (streak x (1/30)/dt, limited to {}..{})", kScaleMin, kScaleMax);
    } else if (v == 0.0) {
      REXLOG_INFO("[mcla-vis] motion blur: no streaks from movement (camera and vehicle displacement x 0)");
    } else {
      REXLOG_INFO("[mcla-vis] motion blur: streak x {} (hooks 0x822E85F8, 0x822E88E0)", v);
    }
  }
  return float(scale);
}

}  // namespace

#if MCLA_HOOK_VIS_MOTION_BLUR
// sub_822E8448 (the vehicle's previous position), before `stfs f0,80(r1)` at 0x822E85F8: f0 = 0.25, the factor of
// previous = current - velocity * dt * 0.25.
void mcla_hook_vis_mblur_vehicle(PPCRegister& f0) {
  MCLA_HOOK_ENTER();
  const float scale = Scale();
  if (scale != 1.0f) {
    f0.f64 = double(float(f0.f64) * scale);
  }
}

// sub_822E8780 (the camera's previous position), before `stfs f0,80(r1)` at 0x822E88E0: f0 = 0.25, the factor of
// previous = current - 0.25 * (the displacement of the frame).
void mcla_hook_vis_mblur_camera(PPCRegister& f0) {
  MCLA_HOOK_ENTER();
  const float scale = Scale();
  if (scale != 1.0f) {
    f0.f64 = double(float(f0.f64) * scale);
  }
}
#else
void mcla_hook_vis_mblur_vehicle(PPCRegister&) {}
void mcla_hook_vis_mblur_camera(PPCRegister&) {}
#endif
