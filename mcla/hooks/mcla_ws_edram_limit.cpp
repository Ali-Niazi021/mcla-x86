// mcla_ws_edram_limit (W5.1): let the game place D3D surfaces up to the EDRAM the GPU emulates, not only up to
// the console's 2048 tiles. Needed by every back buffer whose two device surfaces do not fit 2048 tiles:
// 1600x900 (2 x 1140), 1920x1080 (2 x 1632 = 3264, PLAN 3.8). Only with widescreen on (mcla_widescreen_width, see
// mcla_ws_common.h); cvar mcla_ws_edram_limit (default true).
//
// The site. The title's create-surface (sub_82410D70, CreateRenderTarget / CreateDepthStencilSurface) places a
// surface in the EDRAM itself when the caller gives no base (r7 == 0), then checks the end against the console's
// EDRAM:
//     0x82410E04  lwz r30,80(r1)         r30 = the surface's size in EDRAM tiles
//     0x82410E10  bl 0x824252A8          best-fit search in D3D's tile bitmap (6144 bits), the base -> [r1+80]
//     0x82410E2C  lwz r3,80(r1)          r3  = the base
//     0x82410E30  add r11,r3,r30         r11 = base + size
//     0x82410E34  cmplwi cr6,r11,2048    <- the hook, BEFORE this: 2048 x 5120 bytes = 10 MiB = the Xenos EDRAM
//     0x82410E38  ble cr6,0x82410E48     within: the surface is owned (flag 0x80000000), its base is stored
//     0x82410E3C  mr r4,r30              past it: the tiles are given back (bl 0x82425258) and the create
//     0x82410E44  b 0x82410E1C           fails (RAGE: "Unable to create ... render target")
// codegen entry (the TOML spec): a `bool` hook with `jump_address_on_true = 0x82410E48`, registers r3, r30, r11,
// no `after_instruction`. true jumps to the success branch and skips the compare, false falls into it.
//
// The hook says true when the GPU emulates more than 2048 tiles (rex IGraphicsSystem::edram_tile_count(), the
// SDK's edram_tiles cvar = 4096 in the PC profile; 0 until the GPU has started) and the surface lies inside that
// EDRAM: base < host tiles and base + size <= host tiles. Otherwise, and for every surface that already passes
// the game's own check, it says false and the game's code runs. Anything it lets through ends past tile 2048,
// which the SDK's render-target cache handles because it emulates the larger EDRAM (12-bit bases, wrapping at
// 4096). Surfaces the game places itself with an explicit base (the scene tile pair, the renderer's full-screen
// targets: params +18 = 1) never come through this check.
//
// Each call records the limit in force (ws::NoteEdramLimit): mcla_ws_edram.cpp (W7) uses it to decide whether the
// allocator's tiles have to be shared, and the scene-size hook's `[mcla-wide] sizing:` line says whether this
// hook is needed. Without the codegen entry nothing here runs: the limit stays 2048 and the first surface past it
// is the RAGE fatal above (at 1080p: the device colour surface, which ends at tile 3264).
//
// Logs: `[mcla-ws-edram-limit] active: ...` at the first call, one line for each of the first eight surfaces it
// lets through, and `[mcla-ws-edram] error: ...` (once) when a surface is past even the host's EDRAM.
//
// Re-implemented from the semantics of Renan's mcla_edram_limit (GPL-3, same address and registers), without
// its dependency on a single-tile switch. Off: widescreen off, --mcla_ws_edram_limit=false, or
// MCLA_HOOK_WS_EDRAM_LIMIT=0 at build time (the function stays and returns false).
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"
#include "mcla_ws_common.h"

REXCVAR_DEFINE_BOOL(mcla_ws_edram_limit, true, "MCLA",
                    "MCLA widescreen: the game's surface-create check (0x82410E34) rejects a D3D surface that "
                    "ends past tile 2048, the console's EDRAM. With this on and the GPU emulating more "
                    "(edram_tiles=4096) the limit becomes the GPU's size, which the two device surfaces of a "
                    "1600x900 or 1920x1080 back buffer need. Only with widescreen on");

namespace {

mcla::hooks::Registration g_registration("ws-edram-limit", "mcla_ws_edram_limit", MCLA_HOOK_WS_EDRAM_LIMIT != 0,
                                         [] { return mcla::ws::Armed() && REXCVAR_GET(mcla_ws_edram_limit); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_EDRAM_LIMIT

// 0x82410E34 in sub_82410D70, BEFORE `cmplwi cr6,r11,2048`: r3 = base, r30 = size, r11 = base + size, all in tiles.
// True: jump to 0x82410E48 (the surface is accepted).
bool mcla_hook_ws_edram_limit(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  constexpr uint32_t kConsole = mcla::ws::sizing::kConsoleEdramTiles;
  if (mcla::ws::Width() == 0 || !REXCVAR_GET(mcla_ws_edram_limit)) {
    mcla::ws::NoteEdramLimit(kConsole);
    return false;
  }
  const uint32_t host = mcla::ws::HostEdramTiles();
  const uint32_t limit = host > kConsole ? host : kConsole;
  mcla::ws::NoteEdramLimit(limit);

  // Announced when the limit first exceeds the console's (the GPU may not have been up at the very first call).
  static std::atomic<bool> active_logged{false};
  static std::atomic<bool> console_logged{false};
  if (limit > kConsole) {
    if (!active_logged.exchange(true)) {
      REXLOG_INFO("[mcla-ws-edram-limit] active: the game's 2048-tile surface check (0x82410E34) allows surfaces up "
                  "to the GPU's EDRAM of {} tiles (edram_tiles)",
                  host);
    }
  } else if (!console_logged.exchange(true)) {
    const uint32_t device = mcla::ws::sizing::DeviceSurfaceTiles(mcla::ws::Width(), mcla::ws::Height());
    if (2 * device > kConsole) {
      REXLOG_WARN("[mcla-ws-edram-limit] the GPU emulates {} EDRAM tiles (edram_tiles; 0 = not started yet): the "
                  "game's 2048-tile surface check stays, but a {}x{} back buffer needs 2 x {} tiles for its device "
                  "surfaces",
                  host, mcla::ws::Width(), mcla::ws::Height(), device);
    } else {
      REXLOG_INFO("[mcla-ws-edram-limit] the GPU emulates {} EDRAM tiles: the game's 2048-tile surface check stays "
                  "(the {}x{} device surfaces fit: 2 x {} tiles)",
                  host, mcla::ws::Width(), mcla::ws::Height(), device);
    }
  }

  const uint32_t base = r3.u32;
  const uint32_t tiles = r30.u32;
  const uint32_t end = r11.u32;
  if (end <= kConsole) {
    return false;  // the game's own check passes
  }
  if (limit > kConsole && base < limit && end >= base && end <= limit) {
    static std::atomic<uint32_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_INFO("[mcla-ws-edram-limit] a surface of {} tiles at base {} ends at tile {}, past the console's 2048 "
                  "and inside the GPU's {}: accepted",
                  tiles, base, end, limit);
    }
    return true;
  }
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true)) {
    REXLOG_ERROR("[mcla-ws-edram] error: a D3D surface of {} EDRAM tiles at base {} ends at tile {}, past the {} "
                 "tiles the GPU emulates (edram_tiles={}): the game rejects it and fails with 'Unable to create ... "
                 "render target'; use edram_tiles=4096 or a smaller back buffer ({}x{} needs 2 x {} tiles for its "
                 "device surfaces)",
                 tiles, base, end, limit, host, mcla::ws::Width(), mcla::ws::Height(),
                 mcla::ws::sizing::DeviceSurfaceTiles(mcla::ws::Width(), mcla::ws::Height()));
  }
  return false;
}

#else  // MCLA_HOOK_WS_EDRAM_LIMIT == 0: keeps a codegen entry linking, does nothing.

bool mcla_hook_ws_edram_limit(PPCRegister&, PPCRegister&, PPCRegister&) { return false; }

#endif
