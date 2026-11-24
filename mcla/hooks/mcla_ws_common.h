// Midnight Club: Los Angeles: true widescreen (docs/mcla/WIDESCREEN.md, extended to W x H by W5.1): the state every
// mcla_ws_* hook shares. Not a hook itself: mcla_ws_common.cpp is always compiled.
//
// Two master cvars:
//
//   mcla_widescreen_width (int)
//     0    off: the game runs exactly as shipped (1280x720, 16:9). Default.
//     W    (1288..1920, rounded down to a multiple of 8) a real W x H back buffer, Hor+ camera, rt pool scaled by
//          area. Needs the SDK's video mode to be W x H too (--resolution=WxH, or video_mode_width/height) or the
//          presenter stretches the picture; the log line of the scene-size hook says so.
//     -1   auto: W x H = the video mode's size when it is larger than 1280x720 (what the launcher's "Fit this
//          screen" passes as --resolution=1560x720, and --resolution=1920x1080); otherwise off.
//
//   mcla_scene_height (int): the back buffer's height.
//     0    derived: the video mode's height when the width is auto or equals the mode's width, else 720
//          (the original W x 720 behaviour).
//     H    (720..1080, rounded down to a multiple of 4) explicit.
//
// A value that cannot be honoured (outside the ranges, narrower than 4:3) is not clamped: widescreen stays off and
// the log gets `[mcla-wide] error: ...`. The ranges and the maths are in mcla_ws_sizing.h (kLimitsM6; 2560 x 1200 is
// reserved for M7). The profile presets `mcla_profile=1080p` / `900p` (profile/pc_profile.cpp) set both.
//
// The scene-size hook (W1, mcla_ws_scene_size.cpp) runs once at device init, decides the final W x H from the cvars
// and the video mode, writes it into the game's two `width` / `height` option words and publishes it here. Every
// other hook asks ws::Width() / ws::Height(): 0 until W1 has run (so a hook can never act before the buffer exists,
// and a build with W1 compiled out leaves the whole feature off).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "mcla_hooks.h"
#include "mcla_ws_sizing.h"

#ifndef MCLA_HOOK_WS_SCENE_SIZE
#define MCLA_HOOK_WS_SCENE_SIZE 0
#endif
#ifndef MCLA_HOOK_WS_RT_POOL
#define MCLA_HOOK_WS_RT_POOL 0
#endif
#ifndef MCLA_HOOK_WS_ASPECT
#define MCLA_HOOK_WS_ASPECT 0
#endif
#ifndef MCLA_HOOK_WS_CUTSCENE_FOV
#define MCLA_HOOK_WS_CUTSCENE_FOV 0
#endif
#ifndef MCLA_HOOK_WS_MAP_ASPECT
#define MCLA_HOOK_WS_MAP_ASPECT 0
#endif
#ifndef MCLA_HOOK_WS_UI
#define MCLA_HOOK_WS_UI 0
#endif
#ifndef MCLA_HOOK_WS_EDRAM
#define MCLA_HOOK_WS_EDRAM 0
#endif
#ifndef MCLA_HOOK_WS_RT_GUARD
#define MCLA_HOOK_WS_RT_GUARD 0
#endif
#ifndef MCLA_HOOK_WS_TEXCLEAR
#define MCLA_HOOK_WS_TEXCLEAR 0
#endif
// Like every hook flag (mcla_hooks.h): 0 here, =1 from mcla/sources.cmake's MCLA_HOOK_DEFINITIONS.
#ifndef MCLA_HOOK_WS_EDRAM_LIMIT
#define MCLA_HOOK_WS_EDRAM_LIMIT 0
#endif
#ifndef MCLA_HOOK_WS_SINGLE_TILE
#define MCLA_HOOK_WS_SINGLE_TILE 0
#endif

REXCVAR_DECLARE(int32_t, mcla_widescreen_width);
REXCVAR_DECLARE(int32_t, mcla_scene_height);
REXCVAR_DECLARE(int32_t, mcla_ws_aspect_mask);
REXCVAR_DECLARE(bool, mcla_ws_cutscene_fov);
REXCVAR_DECLARE(bool, mcla_ws_rt_pool_scale);
REXCVAR_DECLARE(bool, mcla_ws_rt_pool_guard);
REXCVAR_DECLARE(int32_t, mcla_ws_edram_reuse);
REXCVAR_DECLARE(bool, mcla_ws_edram_log);

namespace mcla::ws {

// The 16:9 loads that become W/H (cvar mcla_ws_aspect_mask, one bit per consumer).
enum AspectBit : int32_t {
  kAspectCamera = 1,  // 0x822E5E68  scene camera (sub_822E5E10)
  kAspectMirror = 2,  // 0x82233EB4  rear-view mirror ortho (sub_82233E18)
  kAspectUiA = 4,     // 0x82214BB8  sub_82214AC0 (not analysed)
  kAspectUiB = 8,     // 0x8223E5E0  sub_8223E230 (compared with the viewport's aspect)
  kAspectMap = 16,    // 0x82620700  pause-map visible box (sub_826206B0)
};
constexpr int32_t kAspectMaskAll = 31;

// The back-buffer size the hooks work with: 0 = widescreen off (or the scene-size hook has not run yet). Cheap:
// one atomic load each.
uint32_t Width();
uint32_t Height();

// The master cvar is non-zero (armed: it may still resolve to off at device init). For the
// "[mcla-<tag>] active" registry lines.
bool Armed();

// W / H as the game's single-precision float (2.1666667 for 1560x720, 1.7777778 for 1920x1080), or 0 when
// widescreen is off.
double Aspect();

// The resolved back buffer is exactly 16:9 (1920x1080, 1600x900): the game's own constants are already right and
// every aspect hook stays idle.
bool SixteenNine();

// For a hook that replaces a 16:9 constant: true (and the aspect in *aspect) when widescreen is on, the buffer is
// not exactly 16:9 and the mask cvar has `bit` set.
bool AspectFor(int32_t bit, double* aspect);

// The width in screen pixels of a 16:9 block of the back buffer's full height (1280 at 720 lines, 1920 at 1080);
// 0 when widescreen is off. The UI squeeze's middle block.
double UiBlockWidth();

// Called by the scene-size hook only. Reads the cvars and resolves them against the video mode the game was given
// (the guest's XGetVideoMode result); pure with respect to the published state.
sizing::Resolution ResolveRequest(uint32_t mode_width, uint32_t mode_height);

// Called by the scene-size hook only, after the game's option words hold the size. 0 x 0 = off.
void Publish(uint32_t width, uint32_t height);

// The EDRAM the GPU emulates, in tiles (rex::system::IGraphicsSystem::edram_tile_count: the SDK's edram_tiles
// cvar, 2048 or 4096 where the host allows it); 0 until the GPU has started.
uint32_t HostEdramTiles();

// The tile limit the game's surface-create check is really under: 2048, or HostEdramTiles() once the limit hook
// (mcla_ws_edram_limit.cpp, 0x82410E34) has run with the GPU emulating more. The hook records it on every call.
uint32_t EdramLimitInForce();
void NoteEdramLimit(uint32_t tiles);

// The game's current screen size words (0x827D4274 / 0x827D4278: what GetWidth / GetHeight return
// off the device thread). False when guest memory cannot be read.
bool GuestScreenSize(uint32_t* width, uint32_t* height);

// Big-endian store of one word into STATIC image data (0x82000000..0x829E0000). Checks that the
// page is readable first. False (nothing written) otherwise.
bool WriteImageU32(uint32_t address, uint32_t value);

// Big-endian store of one word anywhere in guest memory (heap objects of the game: the D3D tile
// allocator's bitmap, a surface header). Checks that the word is readable first (so the page is
// mapped); false (nothing written) otherwise. Only for words the game itself writes.
bool WriteGuestU32(uint32_t address, uint32_t value);

}  // namespace mcla::ws
