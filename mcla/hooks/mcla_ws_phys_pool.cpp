// mcla_ws_phys_pool (W5.1 fix after the first 1920x1080 boot): give the guest's physical memory back to what a bigger
// back buffer needs, by making the game's own physical pool smaller. Only with widescreen on (mcla_widescreen_width, see
// mcla_ws_common.h); cvar mcla_ws_phys_pool_reduce_mib (-1 auto, 0 off, N = N MiB).
//
// What failed. The console has 512 MiB of physical memory. The title takes it top-down at boot: its own physical pool
// (a buddy allocator the game sizes in KiB), the game heaps, then D3D's ring buffers, the four-allocation front-buffer
// block, the renderer's shared render-target pool and every texture below. At 1280x720 that fits with room to spare; at
// 1920x1080 the block grows by 17.5 MiB (4 x 8,355,840 bytes against 4 x 3,768,320) and the pool by 12.3 MiB (the area
// ratio; 16.4 MiB at the old x2.667) and nothing gives way. The run w4.2-q1080-n1080 died 3 s after boot, right after
// the renderer's 34th surface:
//     [sys] BaseHeap::Alloc failed to find contiguous range
//     [sys] PhysicalHeap::Alloc unable to alloc physical memory in parent heap
//     [mcla-fatal] Unable to create backup texture for rendertarget (caller 0x82185D68)
// 0x82185D68 is the `bl sub_821BD618` (RAGE's fatal) after `sub_82177EB0(width, height, 1, format, ...)` returned an
// error in sub_82185648, grcRenderTarget::Create: every render target gets a W x H texture, from the shared pool where
// the renderer opened one and from XMemAlloc's physical heap otherwise, and that second allocation is the one that found
// no free range.
//
// The fix, taken from Xenia Canary's working "1920x1080 Resolution" patch for this title (BSD license,
// xenia-canary/game-patches, 545407F8 - Midnight Club Los Angeles (Complete Edition).patch.toml): pay for the growth out
// of the game's own pool.
//     0x821312DC  ori r30,r9,44544       (sub_82131228, the boot heap setup; r9 = 0x40000) r30 = the physical pool in KiB:
//                                        0x4AE00 = 306,688 KiB = 299.5 MiB
//     Xenia       0x613E2E00 = ori r30,r9,0x2E00 -> 0x42E00 = 273,920 KiB = 267.5 MiB: 32 MiB less, "Reduce physical pool"
// r30 is the pool's size in KiB for everything that follows: the bookkeeping table (r30 / 32 x 6), the allocator's
// memory (r30 << 10 bytes, sub_821BC0F0) and the allocator itself (sub_821C0F80). A smaller r30 here is exactly the
// patch, and the memory the pool no longer takes ends up below the heaps, which is where the renderer allocates.
//
// How much. mcla_ws_sizing.h PhysicalGrowthBytes (the block's growth plus the pool's growth) rounded up to whole MiB
// plus two MiB of slack: 29.8 MiB -> 30 + 2 = 32 MiB = 32,768 KiB at 1920x1080, Xenia's number exactly (with the area
// pool, which is Xenia's 0x822E4924 = 0x1626000 too: mcla_ws_rt_pool.cpp); 16 MiB at 1600x900.
// mcla_ws_phys_pool_reduce_mib=N sets it by hand (to try more, if a boot still runs short).
//
// Timing. This runs at boot, long before the device init that decides the back buffer, so the size comes from
// ws::PlannedResolution(): the cvars against the SDK's own VdQueryVideoMode answer, which is what XGetVideoMode will give
// the scene-size hook. If the two ever differ the scene-size hook logs an error.
//
// Safety. The game's own physical pool is its texture and geometry streaming budget; 32 MiB of 299.5 is 10.7 %, and Xenia
// plays the whole game with it. A value that is not the shipped 0x4AE00 is left alone (some other build).
//
// codegen entry: a `void` hook, address 0x821312DC, register r30, `after_instruction = true` (it must see the value the
// `ori` computed), name mcla_hook_ws_physical_pool (mcla/codegen/hooks_widescreen.toml).
//
// Off: --mcla_ws_phys_pool_reduce_mib=0, widescreen off, or MCLA_HOOK_WS_PHYS_POOL=0 at build time. 720p and the
// default profile never reach the arithmetic: widescreen off returns first.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_INT32(mcla_ws_phys_pool_reduce_mib, -1, "MCLA",
                     "MCLA widescreen: MiB taken out of the game's own 299.5 MiB physical pool at boot (Xenia's "
                     "1920x1080 patch, 0x821312DC) to make room for the bigger front-buffer block and render-target "
                     "pool. -1 = auto: that growth rounded up to whole MiB plus 2 (32 at 1920x1080, 16 at 1600x900), "
                     "0 = off, N = N MiB. Only with widescreen on; read at boot");

namespace {

mcla::hooks::Registration g_registration("ws-phys-pool", "mcla_ws_phys_pool_reduce_mib", MCLA_HOOK_WS_PHYS_POOL != 0,
                                         [] {
                                           return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_phys_pool_reduce_mib) != 0;
                                         },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_PHYS_POOL

// 0x821312DC in sub_82131228, AFTER `ori r30,r9,44544`: r30 = the game's physical pool in KiB.
void mcla_hook_ws_physical_pool(PPCRegister& r30) {
  MCLA_HOOK_ENTER();
  namespace sizing = mcla::ws::sizing;
  const int32_t mode = REXCVAR_GET(mcla_ws_phys_pool_reduce_mib);
  if (mode == 0 || !mcla::ws::Armed()) {
    return;
  }
  static std::atomic<bool> logged{false};
  const bool first = !logged.exchange(true);
  const uint32_t before = r30.u32;
  if (before != sizing::kPhysicalPoolKiB) {
    if (first) {
      REXLOG_INFO("[mcla-ws-phys-pool] the game's physical pool is {} KiB, not the shipped {}: left alone", before,
                  sizing::kPhysicalPoolKiB);
    }
    return;
  }
  const sizing::Resolution planned = mcla::ws::PlannedResolution();
  if (planned.verdict != sizing::Verdict::kOn) {
    if (first) {
      REXLOG_INFO("[mcla-ws-phys-pool] no wide back buffer planned ({}): the game's physical pool stays at {} KiB",
                  sizing::Describe(planned.reason), before);
      mcla::ws::LogPhysicalMemory("boot");
    }
    return;
  }
  const uint64_t pool = REXCVAR_GET(mcla_ws_rt_pool_scale)
                            ? mcla::ws::PoolBytesFor(sizing::kHdPoolBytes, planned.width, planned.height)
                            : uint64_t(sizing::kHdPoolBytes);
  const uint64_t growth = sizing::PhysicalGrowthBytes(planned.width, planned.height, pool);
  const uint32_t reduce_kib = mode > 0 ? uint32_t(mode) * 1024u : sizing::PhysicalPoolReductionKiB(growth);
  if (reduce_kib == 0 || reduce_kib >= before / 2) {  // never take half the pool, whatever was asked
    if (first) {
      REXLOG_WARN("[mcla-ws-phys-pool] not changed: reduction {} KiB for the game's {} KiB pool (growth {} bytes)",
                  reduce_kib, before, growth);
    }
    return;
  }
  r30.u64 = before - reduce_kib;
  if (first) {
    REXLOG_INFO("[mcla-ws-phys-pool] the game's physical pool {} -> {} KiB ({:.1f} -> {:.1f} MiB, -{} MiB{}) for a {}x{} "
                "back buffer: the front-buffer block grows {} bytes and the render-target pool to {} bytes ({} bytes "
                "of growth in all)",
                before, before - reduce_kib, before / 1024.0, (before - reduce_kib) / 1024.0, reduce_kib / 1024,
                mode > 0 ? ", mcla_ws_phys_pool_reduce_mib" : ", auto", planned.width, planned.height,
                sizing::FrontBufferBlockBytes(planned.width, planned.height) -
                    sizing::FrontBufferBlockBytes(sizing::kBaseWidth, sizing::kBaseHeight),
                pool, growth);
    mcla::ws::LogPhysicalMemory("boot, before the game's physical pool");
  }
}

#else  // MCLA_HOOK_WS_PHYS_POOL == 0: keeps a codegen entry linking, does nothing.

void mcla_hook_ws_physical_pool(PPCRegister&) {}

#endif
