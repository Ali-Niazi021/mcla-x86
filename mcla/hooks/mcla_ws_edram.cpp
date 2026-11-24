// mcla_ws_edram (W7): room in the D3D EDRAM tile allocator for a wide back buffer
// (docs/mcla/WIDESCREEN.md section 13). Only with widescreen on (mcla_widescreen_width, see
// mcla_ws_common.h); cvars mcla_ws_edram_reuse (1 default, 2, 0 = off) and mcla_ws_edram_log.
//
// What fails. The title's D3D (CreateRenderTarget / CreateDepthStencilSurface = sub_82410D70) places
// a surface in the Xenos EDRAM itself when the caller gives no D3DSURFACE_PARAMETERS (r7 == 0):
//     0x82410E10  bl sub_824252A8        best-fit search in a bitmap of tiles (object at
//                                        BE32[0x828CDA38]: +28 free count, +32 bitmap of 6144 bits,
//                                        MSB first, set = used, +800 the sentinel 0x55555555)
//     0x82410E30  add r11,r3,r30         base + size, in tiles of 5120 bytes (80x16 samples)
//     0x82410E34  cmplwi cr6,r11,2048    2048 x 5120 = 10 MiB: the whole EDRAM
//     0x82410E38  ble -> ok              else the tiles are given back and the create fails
// Surfaces placed like that never overlap. The game (RAGE) creates two of them at device init
// (sub_821784B8): the device depth surface (0x8217853C, pointer at 0x8283935C) and the device
// colour surface (0x82178560, pointer at 0x82839250), both W x 720 at 1x (the D3D presentation
// parameters' multisample type is 0 whatever `noaa` says), then its small effect targets with
// default parameters (grcRenderTarget::Create = sub_82185648: params +18 == 0 -> r7 == 0).
// 1280x720 = 16 x 45 = 720 tiles per device surface: 1440 used, 608 left. 1560x720 = 20 x 45 = 900
// each: 1800 used, 248 left, and the first small target that does not fit is the RAGE fatal
// "Unable to create 256 x 256 depth render target" (run W-1). The same at 2x MSAA: the device
// surfaces do not depend on it.
//
// The host does not offer more: our SDK models the EDRAM as 2048 tiles with 11-bit bases that wrap
// (xenos.h kEdramTileCount, registers.h color_base / depth_base : 11), like the console. So the
// limit at 0x82410E34 cannot be raised; something has to share tiles.
//
// What already shares. Every target the game creates with params +18 = 1 is placed by RAGE at the
// device depth surface's base (0x82185808: BE32[0x8283935C] +28 & 0xFFF = tile 0), or right after
// its parent target: the scene tile pair (sub_8217A470, 1600x800 at 1x = 1000 + 1000 tiles from
// tile 0), the full-screen targets of the renderer (sub_822E4CC8), shadow / reflection pairs. They
// overwrite the device depth surface's tiles all the time, so nothing the game draws can rely on the
// device depth buffer keeping its content across another target.
//
// The fix (mcla_ws_edram_reuse=1). Right after the two device surfaces exist (0x82178564, the
// instruction after the second create; r3 = the colour surface):
//   1. the tiles of the device DEPTH surface are cleared in the allocator's bitmap: the allocator
//      can hand them out again, the surface keeps its base. Its "placed by D3D" flag
//      (surface +0 bit 0x80000000, the only thing sub_82421AA8 tests before it frees the tiles) is
//      cleared so a later release of the surface does not free tiles a second time;
//   2. the tiles 2048..6143 of the bitmap are marked used. The bitmap is three EDRAMs long and the
//      search is best-fit, so without this the big run above the device surfaces is the last choice
//      and every small target would land on tile 0. With it the 248 tiles above the device
//      surfaces are filled first (those targets sit where they would at 1280: above both device
//      surfaces), and only what does not fit there goes to the device depth surface's tiles;
//   3. the free count (+28) is set to the number of clear bits.
// Nothing is moved past the EDRAM, no base wraps, the 2048 check stays. =2 gives the tiles of the
// device COLOUR surface instead (A/B if a depth-tested 2D element flickers with 1).
//
// The second hook is a probe: every surface sub_82410D70 returns is logged (first 160) with its
// size, format, MSAA, tiles and who placed it, so one run gives the list of targets by width.
//
// Off: widescreen off, --mcla_ws_edram_reuse=0, or MCLA_HOOK_WS_EDRAM=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-edram", "mcla_ws_edram_reuse", MCLA_HOOK_WS_EDRAM != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_edram_reuse) != 0; },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_EDRAM

namespace {

constexpr uint32_t kAllocatorSlot = 0x828CDA38;      // BE32 = the D3D tile allocator object
constexpr uint32_t kDeviceDepthSlot = 0x8283935C;    // BE32 = the device depth surface
constexpr uint32_t kFreeCount = 28;                  // allocator +28
constexpr uint32_t kBitmap = 32;                     // allocator +32, 192 big-endian words
constexpr uint32_t kSentinel = 800;                  // allocator +800 = 0x55555555
constexpr uint32_t kBitmapWords = 192;               // 6144 tiles
constexpr uint32_t kEdramTiles = 2048;               // 10 MiB / 5120
constexpr uint32_t kTileBytes = 5120;
constexpr uint32_t kPlacedByD3D = 0x80000000;        // surface +0: tiles owned by the allocator
constexpr uint32_t kProbeLines = 160;

struct Surface {
  uint32_t flags = 0;   // +0
  uint32_t info = 0;    // +24: bits 16..17 MSAA
  uint32_t base = 0;    // +28 & 0xFFF, tiles
  uint32_t size = 0;    // +36: W-1 in bits 18..31, H-1 in bits 3..17
  uint32_t format = 0;  // +40
  uint32_t bytes = 0;   // +44 = tiles x 5120
  uint32_t width() const { return (size >> 18) + 1; }
  uint32_t height() const { return ((size >> 3) & 0x7FFF) + 1; }
  uint32_t tiles() const { return bytes / kTileBytes; }
  uint32_t msaa() const { return (info >> 16) & 3; }
  bool depth() const { return (format & 63) == 22 || (format & 63) == 23; }
};

bool ReadSurface(uint32_t address, Surface* out) {
  uint32_t base = 0;
  if (address == 0 || !mcla::guest::U32(address, &out->flags) ||
      !mcla::guest::U32(address + 24, &out->info) || !mcla::guest::U32(address + 28, &base) ||
      !mcla::guest::U32(address + 36, &out->size) || !mcla::guest::U32(address + 40, &out->format) ||
      !mcla::guest::U32(address + 44, &out->bytes)) {
    return false;
  }
  out->base = base & 0xFFF;
  return true;
}

const char* MsaaName(uint32_t msaa) { return msaa == 0 ? "1x" : msaa == 1 ? "2x" : "4x"; }

}  // namespace

// 0x82178564 in sub_821784B8, before `lis r9,-32124`: both device surfaces exist. r3 = the colour
// surface (stored to 0x82839250 four instructions later).
void mcla_hook_ws_edram_device_surfaces(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  const uint32_t wide = mcla::ws::Width();
  if (wide == 0) {
    return;
  }
  const int32_t mode = REXCVAR_GET(mcla_ws_edram_reuse);
  uint32_t depth_address = 0;
  Surface depth, colour;
  const bool have_depth = mcla::guest::U32(kDeviceDepthSlot, &depth_address) && ReadSurface(depth_address, &depth);
  const uint32_t colour_address = r3.u32;
  const bool have_colour = ReadSurface(colour_address, &colour);
  if (have_depth && have_colour) {
    REXLOG_INFO("[mcla-ws-edram] device surfaces: depth {}x{} {} tiles {}..{} ({}), colour {}x{} {} tiles "
                "{}..{} ({}); {} of {} EDRAM tiles left for targets placed by D3D",
                depth.width(), depth.height(), MsaaName(depth.msaa()), depth.base,
                depth.base + depth.tiles() - 1, depth.tiles(), colour.width(), colour.height(),
                MsaaName(colour.msaa()), colour.base, colour.base + colour.tiles() - 1, colour.tiles(),
                int32_t(kEdramTiles) - int32_t(depth.tiles() + colour.tiles()), kEdramTiles);
  }
  if (mode != 1 && mode != 2) {
    REXLOG_INFO("[mcla-ws-edram] mcla_ws_edram_reuse={}: the allocator is left as it is", mode);
    return;
  }
  const bool use_depth = mode == 1;
  const Surface& given = use_depth ? depth : colour;
  const uint32_t given_address = use_depth ? depth_address : colour_address;
  if (!(use_depth ? have_depth : have_colour)) {
    REXLOG_WARN("[mcla-ws-edram] the device {} surface cannot be read: nothing changed",
                use_depth ? "depth" : "colour");
    return;
  }
  const uint32_t tiles = given.tiles();
  if ((given.flags & kPlacedByD3D) == 0 || tiles == 0 || given.bytes % kTileBytes != 0 ||
      given.base + tiles > kEdramTiles) {
    REXLOG_WARN("[mcla-ws-edram] the device {} surface 0x{:08X} is not a D3D-placed EDRAM range (flags "
                "0x{:08X}, base {}, {} bytes): nothing changed",
                use_depth ? "depth" : "colour", given_address, given.flags, given.base, given.bytes);
    return;
  }

  uint32_t allocator = 0, sentinel = 0, count_before = 0;
  if (!mcla::guest::U32(kAllocatorSlot, &allocator) || allocator == 0 ||
      !mcla::guest::U32(allocator + kSentinel, &sentinel) || sentinel != 0x55555555u ||
      !mcla::guest::U32(allocator + kFreeCount, &count_before)) {
    REXLOG_WARN("[mcla-ws-edram] the tile allocator 0x{:08X} does not look as expected (sentinel "
                "0x{:08X}): nothing changed",
                allocator, sentinel);
    return;
  }
  uint32_t words[kBitmapWords];
  for (uint32_t i = 0; i < kBitmapWords; ++i) {
    if (!mcla::guest::U32(allocator + kBitmap + 4 * i, &words[i])) {
      REXLOG_WARN("[mcla-ws-edram] the tile bitmap cannot be read: nothing changed");
      return;
    }
  }
  uint32_t after[kBitmapWords];
  for (uint32_t i = 0; i < kBitmapWords; ++i) {
    after[i] = words[i];
  }
  // 1. the surface's tiles become free again (bit i: word i / 32, mask 0x80000000 >> (i % 32)).
  for (uint32_t tile = given.base; tile < given.base + tiles; ++tile) {
    after[tile >> 5] &= ~(0x80000000u >> (tile & 31));
  }
  // 2. nothing can be placed past the EDRAM: the small run above the device surfaces is the best
  //    fit, the freed tiles come second.
  for (uint32_t i = kEdramTiles / 32; i < kBitmapWords; ++i) {
    after[i] = 0xFFFFFFFFu;
  }
  // 3. free count = clear bits.
  uint32_t free_tiles = 0;
  for (uint32_t i = 0; i < kBitmapWords; ++i) {
    free_tiles += uint32_t(__builtin_popcount(~after[i]));
  }
  bool ok = true;
  for (uint32_t i = 0; i < kBitmapWords && ok; ++i) {
    if (after[i] != words[i]) {
      ok = mcla::ws::WriteGuestU32(allocator + kBitmap + 4 * i, after[i]);
    }
  }
  ok = ok && mcla::ws::WriteGuestU32(allocator + kFreeCount, free_tiles);
  // The surface keeps its base but no longer owns the tiles: its release must not free them.
  ok = ok && mcla::ws::WriteGuestU32(given_address, given.flags & ~kPlacedByD3D);
  if (!ok) {
    REXLOG_ERROR("[mcla-ws-edram] a write into the tile allocator failed half way: expect a fatal");
    return;
  }
  const uint32_t top = depth.tiles() + colour.tiles();
  REXLOG_INFO("[mcla-ws-edram] active: the {} tiles {}..{} of the device {} surface are free for the "
              "game's small targets again (after the {} above tile {}); tiles 2048..6143 closed; free "
              "count {} -> {} ({}x720 back buffer, mcla_ws_edram_reuse={})",
              tiles, given.base, given.base + tiles - 1, use_depth ? "depth" : "colour",
              top < kEdramTiles ? kEdramTiles - top : 0, top, count_before, free_tiles, wide, mode);
}

// 0x82410E9C in sub_82410D70, before `mr r3,r31`: every surface that was created. r31 = the surface.
void mcla_hook_ws_edram_surface(PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  if (mcla::ws::Width() == 0 || !REXCVAR_GET(mcla_ws_edram_log)) {
    return;
  }
  static std::atomic<uint32_t> lines{0};
  if (lines.load(std::memory_order_relaxed) >= kProbeLines) {
    return;
  }
  const uint32_t n = lines.fetch_add(1, std::memory_order_relaxed);
  if (n >= kProbeLines) {
    return;
  }
  Surface s;
  if (!ReadSurface(r31.u32, &s)) {
    return;
  }
  const uint32_t end = s.base + s.tiles();
  REXLOG_INFO("[mcla-ws-edram] surface {:3}: {}x{} {} format 0x{:08X} {}, tiles {}..{} ({}), placed by {}{}",
              n + 1, s.width(), s.height(), MsaaName(s.msaa()), s.format, s.depth() ? "depth" : "colour",
              s.base, end == 0 ? 0 : end - 1, s.tiles(), (s.flags & kPlacedByD3D) ? "D3D" : "the game",
              end > kEdramTiles ? " PAST THE EDRAM" : "");
}

#else  // MCLA_HOOK_WS_EDRAM == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_edram_device_surfaces(PPCRegister&) {}
void mcla_hook_ws_edram_surface(PPCRegister&) {}

#endif
