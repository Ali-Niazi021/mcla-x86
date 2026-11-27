// mcla_gfx_shadow_res: the edge of the sun-shadow map, 640 as shipped, 512, 384, 320, 256 or 192 (docs/PLAN.md W5.4; scratch/S3
// lever #3; 320 / 256 / 192 for the 2000p profile, R2 of docs/native/ROADMAP.md).
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
// 512 and 384 are multiples of 32 (texture tile) and 16 (EDRAM tile height); so are 320, 256 and 192.
//
// Why 320 (R2, the 2000p profile). The SDK draws every render target at the draw resolution scale (2 in the 2000p profile), baked
// into each translated shader, and the guest does not know: a guest 640x640 4x-MSAA shadow target is a 1280x1280 host target, 4x
// the shadow pixels for no visible gain (the shadow is a depth map, the 1080p look has 640 host texels). With the guest edge 320 the
// host target is 640x640 again, the 1080p look. mcla_gfx_res_math.h has the accepted edges (and static / run-time checks that every
// one is tile aligned and passes the EDRAM check below); mcla_gfx_tune.cpp's ApplyPreset halves a gfx preset's value at scale 2
// (perf 512 -> 256, quality 0 -> 320) so a preset does not undo the profile's choice.
//
// What 320 changes, checked on the generated code ($WORK/generated/default) and the image (tools/re/xex_image.py), scripts in
// $WORK/scratch/r2r3/ (scan_640.py, scan_gen_640.py, scan_xsh.py):
//   * EDRAM fit (sub_825F4EF8): (320 >> 4) * (320 >> 3) = 800 <= 3600, the flag is set as at 640 (3200); the surface is a quarter of
//     the area of the shipped one, so any EDRAM reach limit that 640 met, 320 meets.
//   * Tiles: 320 = 10 x 32 = 20 x 16, like 384 and 512, which ran (384 at 2000p in the launcher's panel_2000 preset, 512 at 1080p).
//   * Texel size constants: register 5 of the pixel shader constants is {1/w, 1/h, w, h} from the fields (sub_825F61F0, sub_825F5C48:
//     fdivs 1.0 / (float)w), so a shader that takes its texel size from c5 follows the guest edge: {1/320, 1/320, 320, 320}. The
//     consequence is a wider filter, not a wrong one: a tap at +-1/320 of the map is +-2 host texels of the 640 host target, so
//     the penumbra is about twice as soft as at 640 / 1x (the filter of a 320 map at 1x, drawn on finer host edges); the
//     bias constants, if they are per texel, follow the same way. NOT checked without a run: whether that softness is acceptable
//     (a moving picture of the car's own shadow settles it); the fallback is 384 or 512 at scale 2 (host 768 / 1024).
//   * No other size: every `li rX,640` and `cmpwi rX,640` of the generated code (68) was classified by the instructions that follow
//     it: structure offsets of vector loads and stores, dcbt / dcbz strides, allocation counts, the SD 640x480 defaults of the
//     renderer constructor 0x822E47E0, the width test of the phase loop (sub_822E6408), words stored into stack argument blocks
//     (sub_8279xxxx); this constructor is the only 640 that is a size. The shadow object's width and height fields are read only by
//     sub_825F4EF8, sub_825F5850, sub_825F5C48, sub_825F61F0 and the constructor; the 592 / 596 of sub_825F4188 and sub_825FE838 are
//     other objects (four floats of another class, a stack frame).
//   * Data: the 17 words of 640.0f / 1/640 / 0.5/640 / 320.0f / 1280.0f in the image (.rdata, .data): the only one loaded by code
//     in 0x825F0000..0x82600000 is the 640.0f at 0x82086674 (the virtual above, a clamped "everything" request). The 1/640 words
//     (0x8281B0F0..0x8281B230, ten) are the half-texel inset of a 5 x 5 table of atlas rectangles of a 320-pixel atlas; 0x82806A2C /
//     0x827ED24C belong to 640x480 layouts, 0x82817288 to a table of bias epsilons: not shadow data.
//   * Shaders: Xenos microcode has no immediates (constants arrive from the CPU), so a literal 1/640 can only hide in a constant
//     the CPU uploads. The ucode of the 565 + 514 shaders of two shader storage snapshots ($WORK/scratch/pipeline-cache/snap)
//     has no word that is the float 640 / 1/640 / 320 / 1/320 / 0.5/640 / 0.5/320 / 1280 / 1/1280 (as expected). NOT checked: a
//     `def` constant in the game's own effect files (RPF3 archives, table of contents flagged encrypted, not parsed) and the
//     shaders that no stored run has used.
//
// Read when the shadow map is created (the shadow system's constructor), not live: the render target exists after that.
// Cvar mcla_gfx_shadow_res (0 or 640 = as shipped, 512, 384, 320, 256, 192), defined in mcla_gfx_tune.cpp; presets perf / perf+ /
// lowest = 512 (256 at draw scale 2); the 2000p profile = 320. Log, per creation: `[mcla-gfx] shadow res: ...`. With 0 / 640 the
// hook does not touch r10 (bit-identical).
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

#include "mcla_gfx_res_math.h"
#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_SHADOW_RES
#define MCLA_HOOK_GFX_SHADOW_RES 0
#endif

namespace {

constexpr uint32_t kShippedSize = mcla::gfxres::kShadowShippedEdge;

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
  REXLOG_INFO("[mcla-gfx] shadow res: the sun-shadow map is created {}x{} instead of {}x{} (hook 0x825F69A0; the host render "
              "target is the guest size times the draw resolution scale)",
              size, size, kShippedSize, kShippedSize);
}
#else
void mcla_hook_gfx_shadow_res(PPCRegister&) {}
#endif
