// mcla_gfx_far_cascades: the two far sun-shadow cascades on alternate frames, done so that the picture stays valid
// (docs/PLAN.md W5.4, scratch/R4 change 2; the correct form of scratch/S3 lever 4 and of Renan's mcla_far_shadow_interval).
// Cvar mcla_gfx_far_cascades_interval (1 = as shipped, 2 = alternate; 0 = as shipped plus a one-time dump of the matrices),
// defined in mcla_gfx_tune.cpp together with the schedule (PhasesToSkip(): cascade 2 = phase 0x80 on frames = 1 mod N,
// cascade 3 = phase 0x100 on frames = 0 mod N, applied by the phase loop override in mcla_fsr_markers.cpp: the bit is cleared
// in [renderer+448] AND the per-phase object renderer[k] is zeroed around the loop, see there). This file holds the two function
// overrides that make a skipped cascade consistent.
//
// What a skipped cascade is. The shadow phase handler sub_823120C8 runs once per cascade phase; for cascade i it calls
//   sub_825F5B18(shadow map)               per-frame parameter blend, writes into cascade 0's object only
//   sub_825F5AB0(shadow map, i, ..)        the light's fit: sub_825F52F0 (the only caller) sets the cascade's viewport matrices
//   (i == 3) sub_825F4188(global, shadow map)  copies the four cascades' viewport matrices into [0x829019E4]+336, which the
//                                          CPU culling (sub_822DF960 -> sub_825F3F90) reads to put boxes into cascade lists
//   sub_825F5C48(shadow map, i, ..)        render target set, per-cascade constants (CShadowMap +0x160..0x24F), clear
// then the phase object draws and resolves into the cascade's quarter of the shadow atlas. All of this is a virtual call of the
// per-phase object renderer[k] (k = 7 and 8 for the cascades 2 and 3: the shadow system registered for the mask 0x7DE0), made at
// the start of every phase whatever the mask says; clearing the mask bit only stopped the draw-list objects. That was the first
// version's bug: the skipped cascade still set its target, CLEARED it and RESOLVED THE EMPTY DEPTH into its atlas quarter, so
// the far shadows alternated with no shadow at all (scratch/ORC flicker runs). The loop override now zeroes renderer[k] for a
// skipped phase too: none of it runs, no draws, no clear, no resolve, and every per-cascade value computed in it keeps what the
// last run wrote (the objects are persistent). Nothing else is recomputed per frame in a skipped cascade's slot.
//
// 1. sub_825F61F0 (override; callers 0x82311AF4 in sub_823115E8 and 0x82312D54 / 0x82312FC4 in sub_823120C8; r3 = the CShadowMap, BE32[0x8288D2B4],
//    the singleton the shadow collector phases pass) uploads the receivers' shader constants: 1/w, 1/h from +0x250/+0x254,
//    two 3-vector blocks at +0x1F0 / +0x220, and `count = lhz [r3+0x1C]` 4x4 matrices from [[r3+0x18]+4i]+0x10 (lvx128 at
//    +0x10..+0x40, 0x825F62CC..0x825F6314) into constant register 9 (sub_8218AEF8, r6 = 9). Those matrices are what the
//    collector samples the atlas with, so a skipped cascade's matrix MUST be the one its atlas quarter was drawn with: on
//    every call the override saves the matrix of each far cascade that was drawn this frame, and for a cascade the schedule
//    left out it puts the saved copy in for the duration of the call and the current bytes back after it. If the game
//    never rewrites that matrix outside the cascade's phase, the swap is a no-op; if it does (per-frame fit elsewhere), the
//    swap is what holds the shadow in place. With the schedule inactive the override is a pass-through.
//    Other readers of those matrices: none found (the class's methods that read [this+0x18] are sub_825F5C48 (viewport
//    offsets +0x240..), sub_825F4188 (embedded viewport at +0x70), sub_825F5AB0 / sub_825F5B18 (writers), sub_825F6940 (the
//    constructor) and this setter). CAVEAT: nothing here shows WHICH function writes [cascade+0x10] after the constructor
//    (the fit ends in sub_8217CE80 / sub_8217CC80 on the viewport); mcla_gfx_far_cascades_interval=0 logs the four matrices
//    once so the index order (cascade 3 = the smallest scale) and the values can be checked.
//    The consolidated buffer of sub_825F4188 is updated in cascade 3's handler only: on the frames cascade 3 is left out it
//    keeps the last frame's four matrices, which the CPU culling reads (a box near a cascade border may be assigned to the
//    previous frame's cascade for one frame): accepted, not rendering.
//
// 2. sub_825F4EF8 (override) creates the shadow render target from the CShadowMap's +0x250 / +0x254 (called from sub_82310CE8
//    at 0x82310D54, between sub_822E5048 and sub_822E50B8 which select the shared render-target pool's slot). The cascades
//    are resolved into a 1280x1280 24_8 atlas (R3: log lines `1280x1280 ... 24_8`) whose memory comes from that pool slot, and
//    the pool slots are shared by design: at 1080p the post chain's 960x540 16F targets sit at the SAME address, 0x074F7000,
//    and are written every frame. A cascade that is not redrawn is reloaded from shared memory by the texture cache (any
//    resolve into the atlas range invalidates the atlas texture) and would find post data in the rows the post target covers
//    (R3, open item 1). So while the far cascades are throttled the atlas gets its own physical block: the override zeroes the
//    pool pointer BE32[0x82839ED4] around the call, which is the game's own "no pool" path of sub_82177EB0 (0x82178170:
//    `lwz r11,-24876(r23); cmplwi; beq 0x8217818C` = XMemAlloc(size, physical), texture +16 = 1 = owns its memory, freed
//    with the texture). The pool-guard hook (mcla_ws_rt_pool.cpp) sees r11 == 0 and does nothing. Cvar
//    mcla_gfx_shadow_private (-1 auto = on while the far cascades are throttled, 0, 1); read when the render target is created
//    (a world load), so set the far interval at launch. Cost: one more physical block of 6.5 MB (4.2 MB at shadow_res 512).
//    NOT verified at run time: that the atlas is what that call allocates from the pool (the log line of the texture cache
//    for the atlas must no longer show 0x074F7000), and that 1080p has the physical memory for the block.
//
// Not decided by anything here, checked by VER with a moving picture (docs in the report): far shadows must not slide or flicker
// between the two cascades, and no garbage in the far quarter of the shadow. Build switch: MCLA_HOOK_GFX_FAR_CASCADES=0 compiles
// the overrides out (and PhasesToSkip() stops taking the cascades), MCLA_HOOK_GFX_PHASES / MCLA_HOOK_FSR_MARKERS=0 stop the
// phase skip itself.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_FAR_CASCADES
#define MCLA_HOOK_GFX_FAR_CASCADES 0
#endif
#ifndef MCLA_HOOK_GFX_PHASES
#define MCLA_HOOK_GFX_PHASES 0
#endif
#ifndef MCLA_HOOK_FSR_MARKERS
#define MCLA_HOOK_FSR_MARKERS 0
#endif

namespace {

mcla::hooks::Registration g_registration("gfx-far-cascades", "mcla_gfx_far_cascades_interval",
                                         MCLA_HOOK_GFX_FAR_CASCADES != 0 && MCLA_HOOK_GFX_PHASES != 0 &&
                                             MCLA_HOOK_FSR_MARKERS != 0,
                                         [] { return mcla::gfxtune::FarCascadesInterval() > 1; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_GFX_FAR_CASCADES

REX_EXTERN(__imp__sub_825F61F0);
REX_EXTERN(__imp__sub_825F4EF8);

namespace {

constexpr uint32_t kCascadeArray = 0x18;    // [shadow map+0x18] -> pointers to the cascade objects
constexpr uint32_t kCascadeCount = 0x1C;    // lhz [shadow map+0x1C]
constexpr uint32_t kMatrixOffset = 0x10;    // the 4x4 matrix inside a cascade object (64 bytes)
constexpr uint32_t kMatrixWords = 16;
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kFirstFar = 2;           // cascades 2 and 3 are the ones that are left out
constexpr uint32_t kRtPoolPointer = 0x82839ED4;  // BE32: the render-target pool the texture create takes memory from (0 = none)

// The matrices of the far cascades as of the last frame they were drawn (raw guest words, big-endian order not applied:
// loaded and stored through the same swap). Render thread only.
struct Held {
  bool valid = false;
  uint32_t words[kMatrixWords];
};
Held g_held[kMaxCascades];
std::atomic<uint32_t> g_holds{0};
std::atomic<bool> g_dumped{false};

float AsFloat(uint32_t word) {
  float value;
  std::memcpy(&value, &word, sizeof(value));
  return value;
}

// One-time dump of the cascades' matrices: 16 floats and the scale (the Frobenius norm of the upper-left 3x3; a cascade
// that covers more has the smaller one: the farthest cascade must come out smallest, which checks the index order).
void DumpMatrices(uint8_t* base, uint32_t shadow_map, uint32_t array, uint32_t count) {
  for (uint32_t i = 0; i < count && i < kMaxCascades; ++i) {
    const uint32_t object = REX_LOAD_U32(array + 4 * i);
    if (object < 0x10000) {
      REXLOG_INFO("[mcla-gfx] far cascades: cascade {} has no object", i);
      continue;
    }
    float m[kMatrixWords];
    for (uint32_t w = 0; w < kMatrixWords; ++w) {
      m[w] = AsFloat(REX_LOAD_U32(object + kMatrixOffset + 4 * w));
    }
    double sum = 0.0;
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 3; ++c) {
        sum += double(m[4 * r + c]) * double(m[4 * r + c]);
      }
    }
    REXLOG_INFO(
        "[mcla-gfx] far cascades: cascade {} object 0x{:08X} (shadow map 0x{:08X}) scale {:.5g}, matrix [{:.6g} {:.6g} {:.6g} {:.6g} | "
        "{:.6g} {:.6g} {:.6g} {:.6g} | {:.6g} {:.6g} {:.6g} {:.6g} | {:.6g} {:.6g} {:.6g} {:.6g}]",
        i, object, shadow_map, std::sqrt(sum), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
        m[13], m[14], m[15]);
  }
}

}  // namespace

// The receivers' constants: with a far cascade left out of this frame, its light matrix is the one it was drawn with.
extern "C" REX_FUNC(sub_825F61F0) {
  MCLA_HOOK_ENTER();
  const uint32_t skipped = mcla::gfxtune::FarCascadePhasesSkipped();
  const bool dump = mcla::gfxtune::FarCascadesDump();
  const bool throttled = mcla::gfxtune::FarCascadesInterval() > 1;
  const uint32_t shadow_map = ctx.r3.u32;
  if ((!throttled && !dump) || shadow_map < 0x10000) {
    __imp__sub_825F61F0(ctx, base);
    return;
  }
  const uint32_t array = REX_LOAD_U32(shadow_map + kCascadeArray);
  const uint32_t count = REX_LOAD_U16(shadow_map + kCascadeCount);
  if (array < 0x10000 || count == 0) {
    __imp__sub_825F61F0(ctx, base);
    return;
  }
  if (!g_dumped.exchange(true, std::memory_order_relaxed)) {
    DumpMatrices(base, shadow_map, array, count);
  }
  uint32_t swapped_objects[kMaxCascades] = {};
  uint32_t current[kMaxCascades][kMatrixWords];
  uint32_t swapped = 0;
  if (throttled) {
    for (uint32_t i = kFirstFar; i < count && i < kMaxCascades; ++i) {
      const uint32_t object = REX_LOAD_U32(array + 4 * i);
      if (object < 0x10000) {
        continue;
      }
      const uint32_t matrix = object + kMatrixOffset;
      const uint32_t bit = 0x20u << i;  // 0x80 = cascade 2, 0x100 = cascade 3
      Held& held = g_held[i];
      if ((skipped & bit) && held.valid) {
        for (uint32_t w = 0; w < kMatrixWords; ++w) {
          current[i][w] = REX_LOAD_U32(matrix + 4 * w);
          REX_STORE_U32(matrix + 4 * w, held.words[w]);
        }
        swapped_objects[i] = object;
        swapped |= 1u << i;
      } else if (!(skipped & bit)) {
        for (uint32_t w = 0; w < kMatrixWords; ++w) {
          held.words[w] = REX_LOAD_U32(matrix + 4 * w);
        }
        held.valid = true;
      }
    }
  }
  __imp__sub_825F61F0(ctx, base);
  for (uint32_t i = kFirstFar; i < kMaxCascades; ++i) {
    if (swapped & (1u << i)) {
      const uint32_t matrix = swapped_objects[i] + kMatrixOffset;
      for (uint32_t w = 0; w < kMatrixWords; ++w) {
        REX_STORE_U32(matrix + 4 * w, current[i][w]);
      }
    }
  }
  if (swapped) {
    // Logged at 1, 2, 4, 8... holds.
    const uint32_t holds = g_holds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!(holds & (holds - 1))) {
      REXLOG_INFO("[mcla-gfx] far cascades: light matrix held for cascade index mask 0x{:X} ({} setter calls so far)", swapped,
                  holds);
    }
  }
}

// The shadow render target's creation: with the far cascades throttled the atlas gets memory of its own (see the file header).
extern "C" REX_FUNC(sub_825F4EF8) {
  MCLA_HOOK_ENTER();
  if (!mcla::gfxtune::ShadowAtlasPrivate()) {
    __imp__sub_825F4EF8(ctx, base);
    return;
  }
  const uint32_t pool = REX_LOAD_U32(kRtPoolPointer);
  REX_STORE_U32(kRtPoolPointer, 0);
  __imp__sub_825F4EF8(ctx, base);
  REX_STORE_U32(kRtPoolPointer, pool);
  REXLOG_INFO("[mcla-gfx] shadow atlas: render target created outside the render-target pool 0x{:08X} (own physical block)",
              pool);
}

#else  // MCLA_HOOK_GFX_FAR_CASCADES == 0: nothing is overridden, the generated functions run as shipped.
#endif
