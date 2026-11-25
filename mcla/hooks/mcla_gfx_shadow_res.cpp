// mcla_gfx_shadow_res: the edge of the sun-shadow map, 640 as shipped, 512 or 384 (docs/PLAN.md W5.4; scratch/S3 lever #3).
//
// The game draws its four shadow cascades one after the other into one square, 4x-MSAA depth render target and resolves
// each from it (scratch/S3: 640x640). The target's size is two fields of the shadow map object, which its constructor
// rage::CShadowMap::CShadowMap, sub_825F6940 (called from sub_823109E8 at 0x82310A30, the shadow system's constructor,
// which stores the object in the singleton at 0x8288D2B4), stores as constants:
//     0x825F696C  li   r10,640                 (0x39400280)
//     0x825F69A0  stw  r10,592(r31)  width     (0x915F0250)
//     0x825F69A8  stw  r10,596(r31)  height    (0x915F0254)      r10 is not touched in between
// (checked against the words of the image). The hook goes BEFORE the first store: r10 = the size, and both stores take
// it from the register. +600 of the object is its cascade count (4).
//
// Audit of every reader of the two fields (a single store changes all of them consistently). The singleton pointer is loaded by
// only seven functions (sub_823109E8, sub_82310CE8, sub_823110F0, sub_82311138, sub_823115E8, sub_823120C8, and sub_822E0230
// for the viewport array at +16); they call the class's methods and read no size field themselves. The class's methods
// that read +592 / +596 (scan of the whole generated code for those offsets; the other hits belong to other classes):
//   sub_825F4EF8 (0x825F4F60 / 64)  creates the render target: lwz r6,592 / r7,596 are its width and height, and the EDRAM
//                                   tile check `(w >> 4) * (h >> 3) <= 3600` (0x825F4F6C..0x825F4F7C, stored as the
//                                   "fits EDRAM" flag at 97(r1)) only gets easier (640: 3200, 512: 2048, 384: 1152)
//   sub_825F61F0 (0x825F6210 / 14), sub_825F5C48 (0x825F5DB0 / B4)  the pixel shader constants {1/w, 1/h, w, h} (register 5,
//                                   sub_8218AEF8) from the fields: the shader's texel size follows
//   sub_825F5850 (0x825F586C / 7C), sub_825F5C48 (0x825F5D7C / 88)  only compare both with 1344 (the "large map" flag, +661)
// The only other 640 in the class is the 640.0f of its virtual sub_825F6D40 (vtable 0x82086668, slot 1): it calls sub_8217D548
// with (0, 0, 640.0, 640.0, 0, 1) = fractions of the viewport's window, which sub_8217D120 clamps to the window [vp+688] x
// [vp+692]: an "everything" request, not a size. The other `li rX,640` / 640.0f / 1/640 of the shadow code (825F0000..82600000,
// 82300000..82330000, 822E0000..822F9000) are structure offsets. The target's own format and tile math use only the fields.
// 512 and 384 are multiples of 32 (texture tile) and 16 (EDRAM tile height). NOT checked, because nothing here shows the
// shader microcode or the SDK's host render target handling: a literal 1/640 inside a shader; a moving picture check of
// the car's shadow settles it.
//
// Read when the shadow map is created (the shadow system's constructor), not live: the render target exists after that.
// Cvar mcla_gfx_shadow_res (0 or 640 = as shipped, 512, 384), defined in mcla_gfx_tune.cpp; presets perf / perf+ / lowest
// = 512. Log, per creation: `[mcla-gfx] shadow res: ...`. With 0 / 640 the hook does not touch r10 (bit-identical).
//
// Codegen entry (mcla/codegen/hooks_gfx.toml, a CODEGEN task adds it):
//   [[midasm_hook]]
//   address = 0x825F69A0
//   name = "mcla_hook_gfx_shadow_res"
//   registers = ["r10"]
//
// Build switch: MCLA_HOOK_GFX_SHADOW_RES=0 keeps the function and does nothing.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_SHADOW_RES
#define MCLA_HOOK_GFX_SHADOW_RES 0
#endif

namespace {

constexpr uint32_t kShippedSize = 640;

mcla::hooks::Registration g_registration("gfx-shadow-res", "mcla_gfx_shadow_res", MCLA_HOOK_GFX_SHADOW_RES != 0,
                                         [] { return mcla::gfxtune::ShadowMapSize() != kShippedSize; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_GFX_SHADOW_RES
// sub_825F6940 (the CShadowMap constructor), before `stw r10,592(r31)` at 0x825F69A0: r10 = 640, stored as the width and,
// at 0x825F69A8, as the height.
void mcla_hook_gfx_shadow_res(PPCRegister& r10) {
  MCLA_HOOK_ENTER();
  const uint32_t size = mcla::gfxtune::ShadowMapSize();
  if (size == kShippedSize || r10.u32 != kShippedSize) {
    return;  // as shipped (or a register that is not the constructor's constant: never touched)
  }
  r10.u64 = size;
  REXLOG_INFO("[mcla-gfx] shadow res: the sun-shadow map is created {}x{} instead of {}x{} (hook 0x825F69A0)", size, size,
              kShippedSize, kShippedSize);
}
#else
void mcla_hook_gfx_shadow_res(PPCRegister&) {}
#endif
