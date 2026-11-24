// mcla_ws_scene_size (W1): give the game a real W x 720 back buffer (docs/mcla/WIDESCREEN.md
// sections 1 and 8). Master cvar mcla_widescreen_width (0 = off = the game's own 1280x720).
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
// presentation parameters from it. So writing W and 720 into the two words is the same as the dev
// options -width=W -height=720, and everything downstream (render targets, EDRAM tiles, post-fx,
// Flash targets, the front buffer the presenter gets) follows from the globals.
//
// The hook takes r1 (the guest stack pointer) only to read the video mode the game was given, for
// the auto mode (cvar -1: W = the mode's width when it is wider than 1280 and exactly 720 high) and
// for the log line. The mode matters to the SDK presenter, not to the game: a mode that is not
// W x 720 makes the presenter stretch the picture (log WARNING below).
//
// Idempotent: a second device init writes the same words again; only the first logs.
// Off: --mcla_widescreen_width=0 (default), or MCLA_HOOK_WS_SCENE_SIZE=0 at build time (the whole
// widescreen feature stays off: every other hook waits for this one's answer).
#include <atomic>
#include <cstdint>

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

constexpr uint32_t kOptionWidthWord = 0x827D426C;   // dev option `width`  (static 1 = not set)
constexpr uint32_t kOptionHeightWord = 0x827D4270;  // dev option `height` (static 1 = not set)
constexpr uint32_t kModeWidthOffset = 96;           // [r1+96] / [r1+100]: XGetVideoMode's result
constexpr uint32_t kModeHeightOffset = 100;

}  // namespace

void mcla_hook_ws_scene_size(PPCRegister& r1) {
  MCLA_HOOK_ENTER();
  const uint32_t sp = r1.u32;
  uint32_t mode_w = 0, mode_h = 0;
  const bool mode_known = mcla::guest::U32(sp + kModeWidthOffset, &mode_w) &&
                          mcla::guest::U32(sp + kModeHeightOffset, &mode_h);
  const char* why = nullptr;
  const uint32_t width = mcla::ws::ResolveAtDeviceInit(mode_known ? mode_w : 0, mode_known ? mode_h : 0, &why);

  static std::atomic<bool> logged{false};
  const bool first = !logged.exchange(true);

  if (width == 0) {
    if (first) {
      REXLOG_INFO("[mcla-wide] off ({}): video mode {}x{}, the game keeps its own back buffer",
                  why ? why : "no reason", mode_w, mode_h);
    }
    return;
  }

  const bool wrote_width = mcla::ws::WriteImageU32(kOptionWidthWord, width);
  const bool wrote_height = mcla::ws::WriteImageU32(kOptionHeightWord, mcla::ws::kSceneHeight);
  uint32_t read_w = 0, read_h = 0;
  const bool verified = mcla::guest::U32(kOptionWidthWord, &read_w) &&
                        mcla::guest::U32(kOptionHeightWord, &read_h) && read_w == width &&
                        read_h == mcla::ws::kSceneHeight;
  if (!first) {
    return;
  }
  if (!wrote_width || !wrote_height || !verified) {
    REXLOG_ERROR("[mcla-wide] the option words 0x{:08X}/0x{:08X} could not be set to {}x{} (read back "
                 "{}x{}): the game keeps its own 1280x720 back buffer, but the widescreen hooks are on",
                 kOptionWidthWord, kOptionHeightWord, width, mcla::ws::kSceneHeight, read_w, read_h);
    return;
  }
  REXLOG_INFO("[mcla-wide] mode {}x{}, back buffer {}x{} (options width/height set), aspect {:.4f}, "
              "aspect mask {}, cutscene fov {}, rt pool scale {}{}{}",
              mode_w, mode_h, width, mcla::ws::kSceneHeight, mcla::ws::Aspect(),
              REXCVAR_GET(mcla_ws_aspect_mask), REXCVAR_GET(mcla_ws_cutscene_fov) ? "on" : "off",
              REXCVAR_GET(mcla_ws_rt_pool_scale) ? "on" : "off", why ? ", " : "", why ? why : "");
  if (!mode_known || mode_w != width || mode_h != mcla::ws::kSceneHeight) {
    REXLOG_WARN("[mcla-wide] the video mode ({}x{}) is not {}x{}: the SDK presenter stretches the "
                "picture (a native present path does not use the mode). Pass --resolution={}x{}",
                mode_w, mode_h, width, mcla::ws::kSceneHeight, width, mcla::ws::kSceneHeight);
  }
  if (!REXCVAR_QUERY(bool, mcla_noaa)) {
    REXLOG_INFO("[mcla-wide] note: mcla_noaa is off, the scene is drawn as two 2x MSAA tiles (WIDESCREEN.md "
                "section 3); --mcla_noaa=true is the one-pass 1x path");
  }
}

#else  // MCLA_HOOK_WS_SCENE_SIZE == 0: keeps hooks_widescreen.toml linking, does nothing.

void mcla_hook_ws_scene_size(PPCRegister&) {}

#endif
