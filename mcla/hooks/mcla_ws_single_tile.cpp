// mcla_ws_single_tile (W5.1, an opt-in lever for E6): draw the main scene as ONE tile instead of the game's own
// several, when the whole target's colour and depth fit the EDRAM the GPU emulates (edram_tiles=4096). Only with
// widescreen on (mcla_widescreen_width, see mcla_ws_common.h); cvar mcla_ws_single_tile (default FALSE: until a bench
// row says so, the game tiles as shipped).
//
// Why it matters at 1080p. The game sizes a scene tile so that its colour AND depth take 1024 EDRAM tiles each
// (BeginTiledRendering, sub_8217A470 at 0x8217A680..0x8217A700: 80x16 samples a tile at 1x, 80x8 at 2x, 40x8 at 4x;
// rows = (unit height x 1024 / (W rounded up to 160 / unit width)) rounded down to 32; count = ceil(H / rows)).
// 1280x720 is one tile at 1x, two at 2x. 1920x1080 is TWO tiles of 672 rows at 1x, four at 2x (mcla_ws_sizing.h
// GameSceneTiling), 1600x900 two of 800 at 1x. Every tile replays the scene's draws (about 1,370 in the city, Renan's
// frame logs) with the hardware tile predicate, and ends in its own resolve. One tile of the whole target costs half
// the draws and resolves; at 1x 1920x1080 it needs 24 x 68 = 1632 tiles of colour and 1632 of depth = 3264 of the 4096.
//
// The site (all V in the generated code):
//     0x8217A5B0  mr r7,r28 ; mr r8,r25    r28 = target width, r25 = target height, r18 = the renderer's MSAA mode
//                                          (1 none, 2, 4), r19 = the target's format
//     0x8217A680..0x8217A6F8               tile height -> r8, tile count -> BE32[0x827D42A4]
//     0x8217A700  addi r11,r7,159          <- the hook, BEFORE this. Next: the tile surface is W rounded up to 160 by
//                                          r8 rounded up to 32 (r24 / r26), looked up in the game's cache of 8
//                                          (colour surface pointers at 0x828393D8, MSAA modes at 0x827D4284, a free
//                                          slot is 0, the ninth key is sub_821BD618 = a fatal), else created with
//                                          explicit EDRAM bases (params +18 = 1: depth at tile 0, colour after it),
//                                          so the surfaces never go through the 2048-tile check of mcla_ws_edram_limit.
// The hook sets r7 = W, r8 = H and the count word to 1: EndTiledRendering (sub_8217B430) reads the count back and does
// one bin and one resolve. The surface cache rule: a key that is not in the cache yet never takes the last free slot
// (the game's own keys use 4 or 5 of the 8).
//
// Not handled, on purpose: changing the MSAA mode while the game runs between one-tile frames (Renan found a stale
// RB_SURFACE_INFO until the scene is drawn in tiles again). mcla_noaa fixes the mode at renderer creation here.
// The 4x MSAA passes (pause menu, GPS map) never fit and keep the game's tiles.
//
// codegen entry (the TOML spec): a `void` hook, address 0x8217A700, registers r7, r8, r18, r19, r25, r28, no
// `after_instruction`, name mcla_hook_ws_single_tile.
//
// Re-implemented from the semantics of Renan's mcla_single_tile (GPL-3; his analysis credits LARecomp / BadassBaboon);
// the arithmetic and every address above were re-read in our generated code. Off: widescreen off,
// --mcla_ws_single_tile=false (default), or MCLA_HOOK_WS_SINGLE_TILE=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_BOOL(mcla_ws_single_tile, false, "MCLA",
                    "MCLA widescreen: draw the main scene as one tile when the whole target's colour and depth fit "
                    "the EDRAM the GPU emulates (edram_tiles=4096): 1920x1080 at 1x is 2 tiles of 672 rows in the "
                    "game's own arithmetic, 1 tile with this. A/B switch, default off. Only with widescreen on");

namespace {

mcla::hooks::Registration g_registration("ws-single-tile", "mcla_ws_single_tile", MCLA_HOOK_WS_SINGLE_TILE != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_single_tile); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_SINGLE_TILE

namespace {

namespace sizing = mcla::ws::sizing;

constexpr uint32_t kTileCountWord = 0x827D42A4;     // BE32: tiles of the current tiled pass
constexpr uint32_t kTileCacheColour = 0x828393D8;   // BE32 x 8: colour surface of each cached tile key (0 = free)
constexpr uint32_t kTileCacheMsaa = 0x827D4284;     // BE32 x 8: the MSAA mode of each key
constexpr uint32_t kTileCacheSlots = 8;
constexpr uint32_t kSurfaceWidthOffset = 56;        // surface +56: u16 width (GetWidth = sub_82183C30), +58 height
constexpr uint32_t kSurfaceHeightOffset = 58;
constexpr uint32_t kSurfaceFormatOffset = 60;

struct CacheLookup {
  uint32_t used = 0;     // slots in use
  bool has_key = false;  // a slot already holds (msaa, width, height, format)
};

// The same key the game's own loop compares (0x8217A724..0x8217A7A0): MSAA mode, surface width and height, format.
CacheLookup LookUpCache(uint32_t msaa, uint32_t width, uint32_t height, uint32_t format) {
  CacheLookup lookup;
  for (uint32_t i = 0; i < kTileCacheSlots; ++i) {
    uint32_t surface = 0;
    if (!mcla::guest::U32(kTileCacheColour + 4 * i, &surface) || surface == 0) {
      continue;
    }
    ++lookup.used;
    uint32_t slot_msaa = 0, slot_format = 0;
    uint8_t w[2] = {}, h[2] = {};
    if (surface == 0xFFFFFFFFu || !mcla::guest::U32(kTileCacheMsaa + 4 * i, &slot_msaa) ||
        !mcla::guest::Read(surface + kSurfaceWidthOffset, w, 2) ||
        !mcla::guest::Read(surface + kSurfaceHeightOffset, h, 2) ||
        !mcla::guest::U32(surface + kSurfaceFormatOffset, &slot_format)) {
      continue;
    }
    const uint32_t slot_width = (uint32_t(w[0]) << 8) | w[1];
    const uint32_t slot_height = (uint32_t(h[0]) << 8) | h[1];
    lookup.has_key |= slot_msaa == msaa && slot_width == width && slot_height == height && slot_format == format;
  }
  return lookup;
}

}  // namespace

// 0x8217A700 in sub_8217A470, BEFORE `addi r11,r7,159`: r7 = tile width, r8 = tile height (the game's), r18 = MSAA mode,
// r19 = format, r25 = target height, r28 = target width.
void mcla_hook_ws_single_tile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r18, PPCRegister& r19, PPCRegister& r25,
                              PPCRegister& r28) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_ws_single_tile) || mcla::ws::Width() == 0) {
    return;
  }
  uint32_t game_count = 0;
  if (!mcla::guest::U32(kTileCountWord, &game_count) || game_count <= 1) {
    return;  // already one tile (or unreadable): nothing to merge
  }
  const uint32_t msaa = r18.u32;
  const uint32_t width = r28.u32;
  const uint32_t height = r25.u32;
  const uint32_t host = mcla::ws::HostEdramTiles();
  if (!sizing::SingleTileFits(width, height, msaa, host)) {
    static std::atomic<uint32_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("[mcla-ws-single-tile] a {}x{} {}x MSAA pass stays in the game's {} tiles: one tile needs 2 x {} "
                  "EDRAM tiles (each at most 2048), the GPU emulates {}",
                  width, height, msaa, game_count, sizing::SingleTileSurfaceTiles(width, height, msaa), host);
    }
    return;
  }
  const uint32_t surface_width = sizing::CeilDiv(width, 160) * 160;
  const uint32_t surface_height = sizing::AlignUp(height, 32);
  const CacheLookup cache = LookUpCache(msaa, surface_width, surface_height, r19.u32);
  if (!cache.has_key && cache.used >= kTileCacheSlots - 1) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      REXLOG_WARN("[mcla-ws-single-tile] not used: {} of the tile surface cache's {} slots are taken and a key for "
                  "{}x{} is new (the ninth key is a RAGE fatal)",
                  cache.used, kTileCacheSlots, surface_width, surface_height);
    }
    return;
  }
  if (!mcla::ws::WriteImageU32(kTileCountWord, 1)) {
    return;
  }
  r7.u64 = width;
  r8.u64 = height;
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true)) {
    REXLOG_INFO("[mcla-ws-single-tile] active: a {}x{} {}x MSAA pass is drawn as 1 tile instead of the game's {} "
                "({}x{} surfaces, {} EDRAM tiles each; {} of {} cache slots in use)",
                width, height, msaa, game_count, surface_width, surface_height,
                sizing::SingleTileSurfaceTiles(width, height, msaa), cache.used, kTileCacheSlots);
  }
}

#else  // MCLA_HOOK_WS_SINGLE_TILE == 0: keeps a codegen entry linking, does nothing.

void mcla_hook_ws_single_tile(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
