// mcla_gfx_env_res: the size of the environment map, 512x256 as shipped, 384x192 or 256x128 (R2 of docs/native/ROADMAP.md: at draw
// resolution scale 2 the map is drawn at 1024x512 4x MSAA for no visible gain; 256x128 on the guest is the shipped 512x256 on the host).
//
// What the map is. The cars' paint (and the speedometer) reflect a dual-paraboloid map: two square hemispheres side by side in one
// 512x256 colour and depth render target ("dpMapFrontColor" / "dpMapFrontDepth", 32 bpp), drawn by the render phases 0x4 and 0x8
// (mcla_gfx_env_interval skips them on alternate frames), plus a 64x32 "dpSpecMap" (its own fields, left alone). The system is created
// once, by sub_8233AB18 (the singleton at 0x8288DCA4; its targets are made by sub_82338FC0, which sub_82305230 calls in render-target
// pool slot 1):
//     0x8233AB24  li r3,16944       the object's size (sub_82130528 = the allocator)
//     0x8233AB34  li r5,256         height                              (0x38A00100)
//     0x8233AB38  li r4,512         width                               (0x38800200)
//     0x8233AB3C  bl 0x8233A648     the constructor: stw r4,216(r31) ; stw r5,220(r31), then the grcViewports
//                                   (sub_8217D120 with [+216] x [+220]; the spec map's [+224] x [+228] = 64x32 is set by sub_82338FC0)
// The hook goes BEFORE the bl (0x8233AB3C, word 0x4BFFFB0D): r4 and r5 are the two constants, untouched since their `li`. It is the
// only call of the constructor and the object's only creator.
//
// Audit of every reader of the two fields (found by a scan of the whole generated code, scratch/r2r3/scan_field_range.py; the twelve
// functions that load the singleton call the class's methods and read no size field themselves). In the class's code,
// 0x82338000..0x8233C000, +216 / +220 are read by
//     sub_8233A648   the constructor above (the viewport)
//     sub_82338FC0   the render target creation: width = [+216], height = [+220] for "dpMapFrontDepth" (type 2) and "dpMapFrontColor" (3)
//     sub_8233AB98   the render of a hemisphere: the sizes into the viewport / clear and the hemisphere's rectangle (x, 0, width / 2,
//                    height) (srawi r10,r11,1; addze)
//     sub_8233AF30   the next pass: the same fields into its viewport and resolve
// and the class has no other 512 / 256 literal (the `li r9,256` of sub_8233AF30 and the `li r3,256` of sub_82339590 are a vector
// load offset and an allocation size). The spec map's size comes from the literals 64 and 32 in sub_82338FC0 and stays: a 64x32 target
// at either map size.
// NOT checked, because nothing here shows the effect files' constants or the shaders' use of the map: a texel size or half-texel
// inset of the cars' paint shader (a seam along the border between the two hemispheres would be the symptom), and the spec map's blur
// footprint, which follows the map's texel size if it is derived from the fields. A moving picture of a car's paint settles it.
//
// Read once (the environment map system's creation, a world load), not live. Cvar mcla_gfx_env_res (0 or 512 = as shipped, 384, 256),
// defined in mcla_gfx_tune.cpp. Log: `[mcla-gfx] env res: ...`. With 0 / 512 the hook does not touch r4 / r5 (bit-identical).
//
// Codegen entry (mcla/codegen/hooks_gfx.toml; the integrator re-runs `python tools/codegen.py` once):
//   [[midasm_hook]]
//   address = 0x8233AB3C
//   name = "mcla_hook_gfx_env_res"
//   registers = ["r4", "r5"]
//
// Build switch: MCLA_HOOK_GFX_ENV_RES=0 keeps the function and does nothing.
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_gfx_res_math.h"
#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_ENV_RES
#define MCLA_HOOK_GFX_ENV_RES 0
#endif

namespace {

constexpr uint32_t kShippedWidth = mcla::gfxres::kEnvShippedWidth;
constexpr uint32_t kShippedHeight = mcla::gfxres::kEnvShippedHeight;

mcla::hooks::Registration g_registration("gfx-env-res", "mcla_gfx_env_res", MCLA_HOOK_GFX_ENV_RES != 0,
                                         [] { return mcla::gfxtune::EnvMapSize().width != kShippedWidth; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_GFX_ENV_RES
// sub_8233AB18, before `bl 0x8233a648` at 0x8233AB3C: r4 = 512 (width), r5 = 256 (height), the environment map constructor's arguments.
void mcla_hook_gfx_env_res(PPCRegister& r4, PPCRegister& r5) {
  MCLA_HOOK_ENTER();
  const mcla::gfxtune::EnvMapDimensions size = mcla::gfxtune::EnvMapSize();
  if (size.width == kShippedWidth || r4.u32 != kShippedWidth || r5.u32 != kShippedHeight) {
    return;  // as shipped (or registers that are not the constructor's constants: never touched)
  }
  r4.u64 = size.width;
  r5.u64 = size.height;
  REXLOG_INFO("[mcla-gfx] env res: the environment map is created {}x{} instead of {}x{} (hook 0x8233AB3C; the host render targets "
              "are the guest size times the draw resolution scale)",
              size.width, size.height, kShippedWidth, kShippedHeight);
}
#else
void mcla_hook_gfx_env_res(PPCRegister&, PPCRegister&) {}
#endif
