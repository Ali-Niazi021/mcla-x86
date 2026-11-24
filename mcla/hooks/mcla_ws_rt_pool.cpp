// mcla_ws_rt_pool (W2) and mcla_ws_rt_pool_guard (W8): grow the renderer's shared render-target pool
// with the back buffer, and never let a texture be placed past a pool's end
// (docs/mcla/WIDESCREEN.md sections 4 and 13). Only with widescreen on (mcla_widescreen_width, see
// mcla_ws_common.h); cvars mcla_ws_rt_pool_scale and mcla_ws_rt_pool_guard (both default true).
//
// The renderer constructor (sub_822E47E0) creates one block of memory that all full-screen render
// targets share, fixed at 0x9D8000 = 10,321,920 bytes in HD (it was sized for 1280x720 targets):
//     0x822E4920  lwz r10,17012(r10)     r10 = [0x827D4274], the screen width
//     0x822E4924  lis r29,157            r29 = 0x9D8000
//     0x822E4928  ori r29,r29,32768
//     0x822E492C  cmpwi cr6,r10,720      width > 720: keep the HD size (otherwise an SD formula)
//     0x822E4980  mr r5,r29              <- the hook, BEFORE this: r5 = the size
//     0x822E4984  bl 0x821851C8          create the pool (flags in r6..r10), the result is stored at
//                                        renderer+436 (0x822E4990)
// r29 is rewritten at 0x822E4998 (it becomes a dev-option node pointer), so changing it here has no
// other reader. At 1560x720 every full-screen target is 22 % larger, so the unscaled pool is the
// predicted first failure. The game's own option `nosharingrendertargetmem` (node 0x8287E0E4)
// skips the pool.
//
// New size (changed after run W-1, WIDESCREEN.md section 13). The pool's allocator (sub_82183748) is
// a bump pointer per slot with NO bounds check: a texture that does not fit is placed past the
// pool's end, over whatever the kernel put after the block. That was the `sysMemSimpleAllocator`
// "guard word trashed" fatal at 1432 / 1440 with the first version of this hook (scale by area)
// and the null read with the scale off. Scaling by area is too little because every texture is
// rounded up to 32 texels a side: at 1280 the full, half, quarter and eighth widths (1280, 640,
// 320, 160) are multiples of 32 and waste nothing, at 1560 they are 1568, 800, 416 and 224. So:
//     factor = max over k = 1, 2, 4, 8, 16 of  align32(ceil(W / k)) / align32(ceil(1280 / k))
// (times the same for the height if it is not 720), which bounds the growth of every target whose
// width is the screen's divided by a power of two; fixed-size targets do not grow. Every slot
// cursor is a sum of such sizes, so no cursor grows by more than the factor. 1560: 224 / 160 = 1.4
// -> 14,483,456 bytes (13.8 MiB) after rounding up to 64 KiB; 1360: 1.2. The screen size is read
// from the game's own words 0x827D4274 / 0x827D4278 (not the cvar), so a back buffer that did not
// come out as asked is scaled by what it really is and is reported. Only the HD constant is
// touched; any other size (SD formula) is left alone.
//
// mcla_ws_rt_pool_guard (W8), the safety net for the same allocator. sub_82177EB0 (RAGE's render
// target texture create) chooses where the texture's memory comes from:
//     0x82178170  lwz r11,-24876(r23)    r11 = BE32[0x82839ED4], the current pool (0 = none)   <- hook AFTER
//     0x82178174  cmplwi cr6,r11,0
//     0x82178178  beq -> 0x8217818C      no pool: XMemAlloc(r3, physical | 4 KiB | id 128), the
//                                        texture owns its memory (texture +16 = 1)
//     0x8217817C  li r4,4096
//     0x82178180  bl sub_82183748        pool: bump the slot's cursor by r3 rounded up to 4 KiB
// r3 = the texture's size in bytes on all three ways into 0x82178170. The hook reads the pool
// (+8 size, +12 cursors, +20 slot) and, when cursor + size would pass the pool's size, sets r11 = 0:
// the game's own no-pool path gives that one texture its own block. That path is the one the
// option `nosharingrendertargetmem` uses for every target. It is not taken while the title's
// XMemAlloc wrapper (sub_82130B50) still counts its first four physical 4 KiB allocations
// (BE32[0x82830B08] < 4: those four get fixed places inside one block, k x size from the table
// 0x827D1554 = 1, 3, 2, 0, and a stray texture would take one of them): then the hook only warns.
//
// Correction (task ws3, WIDESCREEN.md section 14): the "guard word trashed" fatal at 1432 / 1440 was
// NOT this pool. It is the zero-fill of the device colour surface's texture running past the block
// of the title's first four physical allocations (fixed by mcla_ws_texclear.cpp); it came back at
// 1560 with the pool at x1.4 and the guard silent. The pool scale and the guard stay: the pool's
// allocator still has no bounds check, and mcla_ws_texclear's probe logs each slot's use.
//
// Off: --mcla_ws_rt_pool_scale=false / --mcla_ws_rt_pool_guard=false, or MCLA_HOOK_WS_RT_POOL=0 /
// MCLA_HOOK_WS_RT_GUARD=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-rt-pool", "mcla_ws_rt_pool_scale",
                                         MCLA_HOOK_WS_RT_POOL != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_rt_pool_scale); },
                                         /*default_on=*/false);
mcla::hooks::Registration g_guard_registration("ws-rt-guard", "mcla_ws_rt_pool_guard",
                                               MCLA_HOOK_WS_RT_GUARD != 0,
                                               [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_rt_pool_guard); },
                                               /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_RT_POOL

namespace {

constexpr uint32_t Align32(uint32_t texels) { return (texels + 31) & ~31u; }

}  // namespace

void mcla_hook_ws_rt_pool(PPCRegister& r29) {
  MCLA_HOOK_ENTER();
  const uint32_t wanted_width = mcla::ws::Width();
  if (wanted_width == 0) {
    return;
  }
  constexpr uint32_t kHdPool = 0x9D8000;  // the shipped HD pool, 10,321,920 bytes
  constexpr uint64_t kPage = 0x10000;     // 64 KiB

  uint32_t width = 0, height = 0;
  if (!mcla::ws::GuestScreenSize(&width, &height) || width == 0 || height == 0) {
    width = wanted_width;
    height = mcla::ws::kSceneHeight;
  }

  static std::atomic<bool> logged{false};
  const bool first = !logged.exchange(true);
  if (first && (width != wanted_width || height != mcla::ws::kSceneHeight)) {
    REXLOG_WARN("[mcla-wide] the back buffer is {}x{}, not {}x{}: the scene-size hook did not take "
                "effect (the pool is scaled by the real size)",
                width, height, wanted_width, mcla::ws::kSceneHeight);
  }

  const uint32_t before = r29.u32;
  const bool wider = width > mcla::ws::kBaseWidth || height > mcla::ws::kSceneHeight;
  if (!REXCVAR_GET(mcla_ws_rt_pool_scale) || before != kHdPool || !wider) {
    if (first) {
      REXLOG_INFO("[mcla-ws-rt-pool] pool left at {} bytes (back buffer {}x{}: {})", before, width, height,
                  !REXCVAR_GET(mcla_ws_rt_pool_scale) ? "mcla_ws_rt_pool_scale is off"
                  : before != kHdPool                 ? "not the HD size"
                                                      : "not larger than 1280x720");
    }
    return;
  }
  // The largest growth of a 32-texel-aligned target whose side is the screen's divided by 1..16.
  uint64_t best_num = 1, best_den = 1;
  for (uint32_t k = 1; k <= 16; k *= 2) {
    const uint64_t num = uint64_t(Align32((width + k - 1) / k)) *
                         Align32((height + k - 1) / k);
    const uint64_t den = uint64_t(Align32((mcla::ws::kBaseWidth + k - 1) / k)) *
                         Align32((mcla::ws::kSceneHeight + k - 1) / k);
    if (num * best_den > best_num * den) {
      best_num = num;
      best_den = den;
    }
  }
  const uint64_t scaled = (uint64_t(before) * best_num + best_den - 1) / best_den;
  const uint64_t rounded = (scaled + kPage - 1) / kPage * kPage;
  r29.u64 = rounded;
  if (first) {
    REXLOG_INFO("[mcla-ws-rt-pool] shared target pool {} -> {} bytes ({:.1f} -> {:.1f} MB, x{:.3f} = the "
                "largest growth of a 32-texel-aligned target) for a {}x{} back buffer",
                before, rounded, before / 1.0e6, double(rounded) / 1.0e6,
                double(best_num) / double(best_den), width, height);
  }
}

#else  // MCLA_HOOK_WS_RT_POOL == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_rt_pool(PPCRegister&) {}

#endif

#if MCLA_HOOK_WS_RT_GUARD

// 0x82178170 in sub_82177EB0, AFTER `lwz r11,-24876(r23)`: r3 = the texture's size in bytes,
// r11 = the current pool or 0.
void mcla_hook_ws_rt_pool_guard(PPCRegister& r3, PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  if (mcla::ws::Width() == 0 || !REXCVAR_GET(mcla_ws_rt_pool_guard)) {
    return;
  }
  const uint32_t pool = r11.u32;
  if (pool == 0) {
    return;  // the game already takes the no-pool path
  }
  uint32_t limit = 0, cursors = 0, slot = 0, cursor = 0;
  if (!mcla::guest::U32(pool + 8, &limit) || !mcla::guest::U32(pool + 12, &cursors) ||
      !mcla::guest::U32(pool + 20, &slot) || slot >= 64 || !mcla::guest::U32(cursors + 4 * slot, &cursor)) {
    return;
  }
  const uint64_t need = (uint64_t(r3.u32) + 4095) & ~uint64_t(4095);
  if (uint64_t(cursor) + need <= limit) {
    return;
  }
  static std::atomic<uint32_t> lines{0};
  const bool log = lines.fetch_add(1, std::memory_order_relaxed) < 16;
  uint32_t early = 0;
  if (!mcla::guest::U32(0x82830B08, &early) || early < 4) {
    if (log) {
      REXLOG_WARN("[mcla-ws-rt-pool] a {} byte texture passes the end of pool 0x{:08X} (slot {}, cursor {}, "
                  "size {}) while XMemAlloc still places its first four blocks ({}): left in the pool, "
                  "memory after the pool is overwritten",
                  r3.u32, pool, slot, cursor, limit, early);
    }
    return;
  }
  r11.u64 = 0;  // the game's own path for "no pool": the texture gets its own physical block
  if (log) {
    REXLOG_WARN("[mcla-ws-rt-pool] a {} byte texture does not fit in pool 0x{:08X} (slot {}, cursor {}, "
                "size {}): it gets its own allocation (raise the pool: this line should not appear)",
                r3.u32, pool, slot, cursor, limit);
  }
}

#else  // MCLA_HOOK_WS_RT_GUARD == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_rt_pool_guard(PPCRegister&, PPCRegister&) {}

#endif
