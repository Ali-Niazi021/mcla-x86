// mcla_ws_single_tile (W5.1, widened by the W4.3 attribution): draw a tiled scene pass as ONE tile instead of the game's
// several, whenever the whole target's colour and depth fit the EDRAM the GPU emulates (edram_tiles=4096). It acts at ANY
// back-buffer size, the shipped 1280x720 included, independent of mcla_widescreen_width. cvar mcla_ws_single_tile
// (default FALSE: until a bench row says so, the game tiles as shipped).
//
// Why it matters. The game sizes a scene tile so that its colour AND depth take 1024 EDRAM tiles each
// (BeginTiledRendering, sub_8217A470 at 0x8217A680..0x8217A700: 80x16 samples a tile at 1x, 80x8 at 2x, 40x8 at 4x;
// rows = (unit height x 1024 / (W rounded up to 160 / unit width)) rounded down to 32; count = ceil(H / rows)).
// 1280x720 is one tile at 1x but TWO at the shipped 2x MSAA (512 + 208 rows); 1920x1080 is two tiles of 672 rows at 1x,
// four at 2x (mcla_ws_sizing.h GameSceneTiling), 1600x900 two of 800 at 1x. Every tile replays the scene's draws (about
// 1,370 in the city, Renan's frame logs) with the hardware tile predicate, and ends in its own resolve. One tile of the
// whole target costs half the draws and resolves. 1280x720 at 2x needs 16 x 92 = 1472 tiles of colour and 1472 of depth
// = 2944 of the 4096 (the surface is 1280x736); 1920x1080 at 1x needs 24 x 68 = 1632 + 1632 = 3264.
//
// Prior art. Renan's port draws 2x MSAA at 1280x720 as one tile by default (his src/render_perf.cpp `mcla_single_tile`,
// default true, GPL-3; his log shows a `2x 1280x736` tile surface) and measures a GPU frame of 14.3-14.6 ms where our
// two-tile 2x frame needs 16.0-16.9 ms (W4.3). This file is a re-implementation from those semantics; his analysis
// credits LARecomp / BadassBaboon, so no code was copied and every address below was re-read in our generated code.
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
// one bin and one resolve. Rules, as learned from Renan:
//   * the whole target fits: sizing::SingleTileFits (colour + depth within the host's edram_tiles, each within the
//     console's 2048: a host render target reaches no further from its base);
//   * the surface cache rule: a key that is not in the cache yet never takes the last free slot (the game's own keys
//     use 4 or 5 of the 8);
//   * a change of MSAA mode between two one-tile passes leaves the pass after it with the other mode's
//     RB_SURFACE_INFO (Renan's finding, until the scene is drawn in tiles again): when this pass would merge right
//     after a one-tile pass of another mode, the next kSettlePasses passes are drawn in the game's tiles first.
// The 4x MSAA passes (pause menu, GPS map) never fit and keep the game's tiles.
//
// Log, once per (size, MSAA, outcome): `[mcla-ws-single-tile] merged <W>x<H> <msaa>x into 1 tile (<n> EDRAM tiles of
// <limit>)` (n = colour + depth, limit = the GPU's edram_tiles), a first `active:` line with the surface details, and
// `[mcla-ws-single-tile] not merged <W>x<H> <msaa>x: <reason>` when it cannot (already one tile, does not fit, the
// surface cache is nearly full, settling after an MSAA change, the count word could not be written).
//
// codegen entry: a `void` hook, address 0x8217A700, registers r7, r8, r18, r19, r25, r28, no `after_instruction`, name
// mcla_hook_ws_single_tile (mcla/codegen/hooks_widescreen.toml).
//
// Off: --mcla_ws_single_tile=false (default), or MCLA_HOOK_WS_SINGLE_TILE=0 at build time.
#include <atomic>
#include <cstdint>
#include <mutex>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_BOOL(mcla_ws_single_tile, false, "MCLA",
                    "MCLA: draw a tiled scene pass as one tile when the whole target's colour and depth fit the EDRAM "
                    "the GPU emulates (edram_tiles=4096), at any back-buffer size, 1280x720 included and with or "
                    "without mcla_widescreen_width: 1280x720 at 2x MSAA is 2 tiles in the game's own arithmetic, "
                    "1920x1080 at 1x is 2, 1 tile with this. A/B switch, default off");

namespace {

mcla::hooks::Registration g_registration("ws-single-tile", "mcla_ws_single_tile", MCLA_HOOK_WS_SINGLE_TILE != 0,
                                         [] { return REXCVAR_GET(mcla_ws_single_tile); },
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
constexpr int32_t kSettlePasses = 8;                // passes drawn in tiles after an MSAA change between one-tile passes

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

enum class Outcome : uint8_t { kMerged, kAlreadyOne, kNoFit, kCacheFull, kSettling, kWriteFailed };

// True the first time this (size, MSAA, outcome) is seen: one log line per configuration, whatever the pass rate.
bool FirstTime(uint32_t msaa, uint32_t width, uint32_t height, Outcome outcome) {
  constexpr size_t kKeys = 32;
  static std::mutex mutex;
  static uint64_t seen[kKeys];
  static size_t count = 0;
  const uint64_t key = (uint64_t(msaa & 0xFF) << 56) | (uint64_t(outcome) << 48) | (uint64_t(width & 0xFFFFFF) << 24) |
                       uint64_t(height & 0xFFFFFF);
  std::lock_guard<std::mutex> lock(mutex);
  for (size_t i = 0; i < count; ++i) {
    if (seen[i] == key) {
      return false;
    }
  }
  if (count < kKeys) {
    seen[count++] = key;
  }
  return true;
}

// The previous tiled pass on the render thread: its MSAA mode and the tile count it was drawn with.
std::atomic<uint32_t> g_last_msaa{0};
std::atomic<uint32_t> g_last_count{0};
std::atomic<int32_t> g_hold_off{0};

void Drawn(uint32_t msaa, uint32_t tiles) {
  g_last_msaa.store(msaa, std::memory_order_relaxed);
  g_last_count.store(tiles, std::memory_order_relaxed);
}

}  // namespace

// 0x8217A700 in sub_8217A470, BEFORE `addi r11,r7,159`: r7 = tile width, r8 = tile height (the game's), r18 = MSAA mode,
// r19 = format, r25 = target height, r28 = target width.
void mcla_hook_ws_single_tile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r18, PPCRegister& r19, PPCRegister& r25,
                              PPCRegister& r28) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_ws_single_tile)) {
    return;
  }
  const uint32_t msaa = r18.u32;
  const uint32_t width = r28.u32;
  const uint32_t height = r25.u32;
  uint32_t game_count = 0;
  if (!mcla::guest::U32(kTileCountWord, &game_count)) {
    Drawn(msaa, 0);
    return;
  }
  if (game_count <= 1) {
    Drawn(msaa, 1);  // already one tile: nothing to merge, but it is a one-tile pass for the MSAA-change rule
    if (FirstTime(msaa, width, height, Outcome::kAlreadyOne)) {
      REXLOG_INFO("[mcla-ws-single-tile] not merged {}x{} {}x: the game already draws it as 1 tile", width, height,
                  msaa);
    }
    return;
  }
  const uint32_t host = mcla::ws::HostEdramTiles();
  const uint32_t surface_tiles = sizing::SingleTileSurfaceTiles(width, height, msaa);
  if (!sizing::SingleTileFits(width, height, msaa, host)) {
    Drawn(msaa, game_count);
    if (FirstTime(msaa, width, height, Outcome::kNoFit)) {
      REXLOG_INFO("[mcla-ws-single-tile] not merged {}x{} {}x: one tile needs {} EDRAM tiles (colour + depth, each "
                  "at most 2048) and the GPU emulates {} (edram_tiles); the game's {} tiles stay",
                  width, height, msaa, 2 * surface_tiles, host, game_count);
    }
    return;
  }
  const uint32_t surface_width = sizing::CeilDiv(width, 160) * 160;
  const uint32_t surface_height = sizing::AlignUp(height, 32);
  const CacheLookup cache = LookUpCache(msaa, surface_width, surface_height, r19.u32);
  if (!cache.has_key && cache.used >= kTileCacheSlots - 1) {
    Drawn(msaa, game_count);
    if (FirstTime(msaa, width, height, Outcome::kCacheFull)) {
      REXLOG_WARN("[mcla-ws-single-tile] not merged {}x{} {}x: {} of the tile surface cache's {} slots are taken and a "
                  "key for {}x{} is new (the ninth key is a RAGE fatal)",
                  width, height, msaa, cache.used, kTileCacheSlots, surface_width, surface_height);
    }
    return;
  }
  // A one-tile pass in another MSAA mode just before: let the game tile for a few passes first (Renan's finding).
  if (g_last_count.load(std::memory_order_relaxed) == 1 && g_last_msaa.load(std::memory_order_relaxed) != msaa) {
    g_hold_off.store(kSettlePasses, std::memory_order_relaxed);
  }
  if (g_hold_off.load(std::memory_order_relaxed) > 0) {
    g_hold_off.fetch_sub(1, std::memory_order_relaxed);
    Drawn(msaa, game_count);
    if (FirstTime(msaa, width, height, Outcome::kSettling)) {
      REXLOG_INFO("[mcla-ws-single-tile] not merged {}x{} {}x for now: the previous pass was one tile in another MSAA "
                  "mode, the next {} passes stay in the game's {} tiles",
                  width, height, msaa, kSettlePasses, game_count);
    }
    return;
  }
  if (!mcla::ws::WriteImageU32(kTileCountWord, 1)) {
    Drawn(msaa, game_count);
    if (FirstTime(msaa, width, height, Outcome::kWriteFailed)) {
      REXLOG_WARN("[mcla-ws-single-tile] not merged {}x{} {}x: the tile count word 0x{:08X} could not be written",
                  width, height, msaa, kTileCountWord);
    }
    return;
  }
  r7.u64 = width;
  r8.u64 = height;
  Drawn(msaa, 1);
  if (FirstTime(msaa, width, height, Outcome::kMerged)) {
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
      REXLOG_INFO("[mcla-ws-single-tile] active: tiled passes are drawn as 1 tile where the whole target fits the "
                  "GPU's EDRAM (first: {}x{} {}x MSAA instead of the game's {} tiles; {}x{} surfaces, {} EDRAM tiles "
                  "each; {} of {} tile-cache slots in use)",
                  width, height, msaa, game_count, surface_width, surface_height, surface_tiles, cache.used,
                  kTileCacheSlots);
    }
    REXLOG_INFO("[mcla-ws-single-tile] merged {}x{} {}x into 1 tile ({} EDRAM tiles of {})", width, height, msaa,
                2 * surface_tiles, host);
  }
}

#else  // MCLA_HOOK_WS_SINGLE_TILE == 0: keeps a codegen entry linking, does nothing.

void mcla_hook_ws_single_tile(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
