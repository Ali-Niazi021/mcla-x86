// mcla_ws_scene_size (W1): give the game a real W x H back buffer (docs/mcla/WIDESCREEN.md sections 1 and 8,
// W5.1: H is no longer fixed at 720). Master cvars mcla_widescreen_width (0 = off = the game's own 1280x720) and
// mcla_scene_height (0 = derived, see mcla_ws_common.h; sizes and rounding in mcla_ws_sizing.h).
//
// Why a hook. The back-buffer size is two RAGE dev options, `width` and `height`. Unset, device init
// (sub_8217AC30) clamps whatever video mode the kernel reports to 1280x720, so --resolution=1560x720
// alone gives a 1280x720 picture stretched to 19.5:9 (the Carbon dev004 bug). Set, the value is used
// as is, never clamped. The options live in two static words, 0x827D426C (width) and 0x827D4270
// (height), 1 = not set; the game's own getter writes them from the option's text.
//
// Site 0x8217ACDC in sub_8217AC30, `lis r11,-32124`, BEFORE it runs:
//     0x8217AC7C  addi r3,r1,96          out buffer of the video mode: [r1+96] width, [r1+100] height
//     0x8217AC80  bl 0x827BCD84          XGetVideoMode
//     0x8217ACC0  bl 0x821C0750          getter of option `width`  -> word 0x827D426C
//     0x8217ACD8  bl 0x821C0750          getter of option `height` -> word 0x827D4270
//     0x8217ACDC  lis r11,-32124         <- the hook: after both getters, before any read of the words
//     0x8217ACEC  lwz r9,0(r31)          first read of the width word (r31 = 0x827D426C)
//     0x8217AD1C / 0x8217AD4C  lwz r11,0(r29)   the height word
// The size rule that follows (0x8217ACFC..0x8217AD68) takes an option other than 1 unclamped, in
// both the "mode area <= 0xE1000" and the "larger mode" branches, then stores the result to
// 0x827D4274/78 (the screen size every GetWidth / GetHeight caller sees) and builds the D3D
// presentation parameters from it. So writing W and H into the two words is the same as the dev
// options -width=W -height=H, and everything downstream (render targets, EDRAM tiles, post-fx,
// Flash targets, the front buffer the presenter gets) follows from the globals. Xenia's "1920x1080
// Resolution" patch for this title edits the same two words (0x827D426E / 0x827D4272, the low halves).
//
// The hook takes r1 (the guest stack pointer) only to read the video mode the game was given, for
// the auto mode (cvar -1), for the derived height and for the log line. The mode matters to the SDK
// presenter, not to the game: a mode that is not W x H makes the presenter stretch the picture (log WARNING below).
//
// Log (once, the first device init): `[mcla-wide] mode <mw>x<mh>, back buffer <W>x<H> (...)` on success (PLAN M6
// greps the prefix), `[mcla-wide] off (...)` when nothing is asked for, `[mcla-wide] error: ...` for a rejected value or
// words that could not be set; then one `[mcla-wide] sizing:` line with the numbers the other W hooks work from.
//
// Idempotent: a second device init writes the same words again; only the first logs.
// Off: --mcla_widescreen_width=0 (default), or MCLA_HOOK_WS_SCENE_SIZE=0 at build time (the whole
// widescreen feature stays off: every other hook waits for this one's answer).
#include <atomic>
#include <cstdint>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"
#include "mcla_ws_common.h"

namespace {

mcla::hooks::Registration g_registration("ws-scene-size", "mcla_widescreen_width",
                                         MCLA_HOOK_WS_SCENE_SIZE != 0,
                                         [] { return mcla::ws::Armed(); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_WS_SCENE_SIZE

namespace {

namespace sizing = mcla::ws::sizing;

constexpr uint32_t kOptionWidthWord = 0x827D426C;   // dev option `width`  (static 1 = not set)
constexpr uint32_t kOptionHeightWord = 0x827D4270;  // dev option `height` (static 1 = not set)
constexpr uint32_t kModeWidthOffset = 96;           // [r1+96] / [r1+100]: XGetVideoMode's result
constexpr uint32_t kModeHeightOffset = 100;

// What a rejected value would have needed, for the error line.
std::string RejectionDetail(const sizing::Resolution& r) {
  const sizing::Limits& l = sizing::kLimits;
  switch (r.reason) {
    case sizing::Reason::kWidthRange:
      return "accepted widths " + std::to_string(l.min_width) + ".." + std::to_string(l.max_width) +
             " (rounded down to a multiple of " + std::to_string(sizing::kWidthStep) + ")";
    case sizing::Reason::kHeightRange:
      return "accepted heights " + std::to_string(l.min_height) + ".." + std::to_string(l.max_height) +
             " (rounded down to a multiple of " + std::to_string(sizing::kHeightStep) + "), got " +
             std::to_string(r.height);
    case sizing::Reason::kTooNarrow:
      return std::to_string(r.width) + "x" + std::to_string(r.height) + " is narrower than 4:3";
    case sizing::Reason::kEdramSurface:
      return std::to_string(r.width) + "x" + std::to_string(r.height) + " needs " +
             std::to_string(sizing::DeviceSurfaceTiles(r.width, r.height)) +
             " EDRAM tiles a surface, one surface reaches " + std::to_string(sizing::kConsoleEdramTiles);
    default:
      return "";
  }
}

std::string NotesText(uint32_t notes) {
  std::string text;
  auto add = [&text](const char* part) {
    text += text.empty() ? "" : "; ";
    text += part;
  };
  if (notes & sizing::kNoteWidthRounded) {
    add("width rounded down to a multiple of 8");
  }
  if (notes & sizing::kNoteHeightRounded) {
    add("height rounded down to a multiple of 4");
  }
  if (notes & sizing::kNoteHeightFromMode) {
    add("height from the video mode (mcla_scene_height=0)");
  }
  if (notes & sizing::kNoteModeHeightIgnored) {
    add("the video mode's height is outside the accepted heights: scene height stays 720");
  }
  return text;
}

// One line with the numbers the other widescreen hooks work from (all from mcla_ws_sizing.h).
void LogSizing(uint32_t width, uint32_t height) {
  const uint32_t host = mcla::ws::HostEdramTiles();
  const uint32_t device = sizing::DeviceSurfaceTiles(width, height);
  const int32_t left_console = sizing::FreeTilesAfterDeviceSurfaces(sizing::kConsoleEdramTiles, width, height);
  const uint64_t pool = mcla::ws::PoolBytesFor(sizing::kHdPoolBytes, width, height);
  const uint32_t reduce_kib = sizing::PhysicalPoolReductionKiB(sizing::PhysicalGrowthBytes(width, height, pool));
  const sizing::SceneTiling t1 = sizing::GameSceneTiling(width, height, 1);
  const sizing::SceneTiling t2 = sizing::GameSceneTiling(width, height, 2);
  const uint64_t unit = sizing::TextureBlockUnit(width, height);
  const bool reaches = sizing::TextureClearReachesHeap(width, height);
  std::string edram = host == 0 ? std::string("GPU EDRAM not up yet")
                                : "GPU EDRAM " + std::to_string(host) + " tiles, " +
                                      std::to_string(sizing::FreeTilesAfterDeviceSurfaces(host, width, height)) + " left";
  REXLOG_INFO("[mcla-wide] sizing: device surfaces 2 x {} EDRAM tiles ({} left of the game's 2048, {}; the shipped "
              "1280x720 has {}); scene tiling 1x {} x {} rows, 2x {} x {} rows, one tile at 1x {}; render-target pool "
              "x{:.3f} -> {} bytes; front-buffer block 4 x {} bytes, uncut zero-fill overshoot {} bytes{}; the game's "
              "physical pool gives back {} KiB (the 0x821312DC hook; no '[mcla-ws-phys-pool]' line at boot means it is "
              "not in the generated code)",
              device, left_console, edram, sizing::kShippedFreeTiles, t1.count, t1.tile_rows, t2.count, t2.tile_rows,
              sizing::SingleTileFits(width, height, 1, host) ? "fits the GPU EDRAM" : "does not fit",
              double(pool) / double(sizing::kHdPoolBytes), pool, unit,
              sizing::TextureClearOvershoot(width, height),
              reaches ? " (reaches a heap's guard word: mcla_ws_texclear_clamp or the SDK's MmQueryAllocationSize fix "
                        "is needed)"
                      : "",
              reduce_kib);
  if (left_console < 0) {
    if (host != 0 && sizing::FreeTilesAfterDeviceSurfaces(host, width, height) < 0) {
      REXLOG_ERROR("[mcla-wide] error: the two device surfaces need {} EDRAM tiles but the GPU emulates only {} "
                   "(edram_tiles={}): the game cannot create the second one (RAGE fatal 'Unable to create ... render "
                   "target'); pass --edram_tiles=4096 or a smaller back buffer",
                   2 * device, host, host);
    } else {
      REXLOG_INFO("[mcla-wide] the device surfaces ({} tiles) are past the game's own 2048-tile check: the limit hook "
                  "(mcla_hook_ws_edram_limit at 0x82410E34, mcla_ws_edram_limit.cpp) must be in the generated code "
                  "and edram_tiles=4096; look for '[mcla-ws-edram-limit] active' in this log",
                  2 * device);
    }
  }
  // The SDK's render_target_max_height (0 = no cap) makes every host render target at most that many rows tall (rounded
  // up to the 16-row tile): a back buffer taller than that loses its bottom rows. The PC profile's 720p value is 1024.
  const uint32_t need = sizing::RenderTargetMaxHeight(height);
  const uint32_t max_rows = REXCVAR_QUERY(uint32_t, render_target_max_height);
  if (max_rows != 0 && sizing::AlignUp(max_rows, 16) < need) {
    REXLOG_ERROR("[mcla-wide] error: render_target_max_height={} cuts a {}-row buffer; set it to {} (mcla_profile=1080p "
                 "does)",
                 max_rows, height, need);
  }
}

}  // namespace

void mcla_hook_ws_scene_size(PPCRegister& r1) {
  MCLA_HOOK_ENTER();
  const uint32_t sp = r1.u32;
  uint32_t mode_w = 0, mode_h = 0;
  const bool mode_known = mcla::guest::U32(sp + kModeWidthOffset, &mode_w) &&
                          mcla::guest::U32(sp + kModeHeightOffset, &mode_h);
  if (!mode_known) {
    mode_w = mode_h = 0;
  }
  const sizing::Resolution resolved = mcla::ws::ResolveRequest(mode_w, mode_h);

  static std::atomic<bool> logged{false};
  const bool first = !logged.exchange(true);

  if (resolved.verdict != sizing::Verdict::kOn) {
    mcla::ws::Publish(0, 0);
    if (!first) {
      return;
    }
    if (resolved.verdict == sizing::Verdict::kRejected) {
      REXLOG_ERROR("[mcla-wide] error: {} (mcla_widescreen_width={}, mcla_scene_height={}, video mode {}x{}{}{}): "
                   "widescreen stays off, the game keeps its own 1280x720 back buffer",
                   sizing::Describe(resolved.reason), REXCVAR_GET(mcla_widescreen_width),
                   REXCVAR_GET(mcla_scene_height), mode_w, mode_h, RejectionDetail(resolved).empty() ? "" : "; ",
                   RejectionDetail(resolved));
    } else {
      REXLOG_INFO("[mcla-wide] off ({}): video mode {}x{}, the game keeps its own back buffer",
                  sizing::Describe(resolved.reason), mode_w, mode_h);
    }
    return;
  }

  const uint32_t width = resolved.width;
  const uint32_t height = resolved.height;
  const bool wrote_width = mcla::ws::WriteImageU32(kOptionWidthWord, width);
  const bool wrote_height = mcla::ws::WriteImageU32(kOptionHeightWord, height);
  uint32_t read_w = 0, read_h = 0;
  const bool verified = mcla::guest::U32(kOptionWidthWord, &read_w) &&
                        mcla::guest::U32(kOptionHeightWord, &read_h) && read_w == width && read_h == height;
  if (!wrote_width || !wrote_height || !verified) {
    // The game keeps 1280x720: nothing may act on the wide size.
    mcla::ws::Publish(0, 0);
    if (first) {
      REXLOG_ERROR("[mcla-wide] error: the option words 0x{:08X}/0x{:08X} could not be set to {}x{} (read back "
                   "{}x{}): the game keeps its own 1280x720 back buffer and widescreen stays off",
                   kOptionWidthWord, kOptionHeightWord, width, height, read_w, read_h);
    }
    return;
  }
  mcla::ws::Publish(width, height);
  if (!first) {
    return;
  }
  const std::string notes = NotesText(resolved.notes);
  REXLOG_INFO("[mcla-wide] mode {}x{}, back buffer {}x{} (options width/height set), aspect {:.4f}{}, aspect mask {}, "
              "cutscene fov {}, rt pool scale {}{}{}",
              mode_w, mode_h, width, height, mcla::ws::Aspect(),
              mcla::ws::SixteenNine() ? " (16:9: the aspect and cutscene hooks stay idle)"
              : mcla::ws::Tall()      ? " (taller than 16:9: Vert+ camera, the UI in a 16:9 band)"
                                      : "",
              REXCVAR_GET(mcla_ws_aspect_mask), REXCVAR_GET(mcla_ws_cutscene_fov) ? "on" : "off",
              REXCVAR_GET(mcla_ws_rt_pool_scale) ? "on" : "off", notes.empty() ? "" : ", ", notes);
  if (!mode_known || mode_w != width || mode_h != height) {
    REXLOG_WARN("[mcla-wide] the video mode ({}x{}) is not {}x{}: the SDK presenter stretches the picture (a native "
                "present path does not use the mode). Pass --resolution={}x{}",
                mode_w, mode_h, width, height, width, height);
  }
  LogSizing(width, height);
  if (sizing::IsTall(width, height) && REXCVAR_QUERY(bool, mcla_noaa)) {
    // The game's own 1x tiling of a tall buffer can end in a sliver of a tile (1440x900: 2 x 896 rows, the second one 4).
    const sizing::SceneTiling t1 = sizing::GameSceneTiling(width, height, 1);
    const uint32_t last_rows = t1.count > 1 ? height - (t1.count - 1) * t1.tile_rows : t1.tile_rows;
    if (t1.count > 1 && !REXCVAR_QUERY(bool, mcla_ws_single_tile)) {
      REXLOG_WARN("[mcla-wide] the 1x scene of {}x{} is {} tiles of {} rows (the last has {}): --mcla_ws_single_tile=true "
                  "draws it as one{}",
                  width, height, t1.count, t1.tile_rows, last_rows,
                  last_rows < 32 ? " (mandatory here: a tile that short is not worth its resolve)" : "");
    }
  }
  if (rex::cvar::GetFlagByName("temporal_aa") == "fsr" && !sizing::FsrSqueezeWidthOk(width)) {
    REXLOG_WARN("[mcla-wide] temporal_aa=fsr squeezes only a scene surface whose pitch equals the output width, and the "
                "game's tile surface pitch is the width rounded up to 160: {} is not a multiple of 160 ({} would "
                "be), so the FSR pass will not arm",
                width, sizing::AlignUp(width, 160));
  }
  // The physical-pool hook (boot) sized the game's pool for the size the cvars and the SDK's video mode planned; this
  // is the size the game really got.
  const sizing::Resolution planned = mcla::ws::PlannedResolution();
  if (planned.verdict != sizing::Verdict::kOn || planned.width != width || planned.height != height) {
    REXLOG_ERROR("[mcla-wide] error: the boot-time plan was {} ({}x{}) but the back buffer is {}x{}: the game's physical "
                 "pool was sized for the plan (mcla_ws_phys_pool.cpp)",
                 planned.verdict == sizing::Verdict::kOn ? "on" : "off", planned.width, planned.height, width, height);
  }
  mcla::ws::LogPhysicalMemory("device init");
  if (!REXCVAR_QUERY(bool, mcla_noaa)) {
    const sizing::SceneTiling t2 = sizing::GameSceneTiling(width, height, 2);
    const sizing::SceneTiling t1 = sizing::GameSceneTiling(width, height, 1);
    REXLOG_INFO("[mcla-wide] note: mcla_noaa is off, the scene is drawn as {} 2x MSAA tiles of {} rows (WIDESCREEN.md "
                "section 3); --mcla_noaa=true is the 1x path in {} tile(s) of {} rows",
                t2.count, t2.tile_rows, t1.count, t1.tile_rows);
  }
}

#else  // MCLA_HOOK_WS_SCENE_SIZE == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_scene_size(PPCRegister&) {}

#endif
