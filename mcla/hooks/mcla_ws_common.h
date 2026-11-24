// Midnight Club: Los Angeles: true widescreen (docs/mcla/WIDESCREEN.md): the state every mcla_ws_*
// hook shares. Not a hook itself: mcla_ws_common.cpp is always compiled.
//
// One master cvar, `mcla_widescreen_width` (int):
//     0    off: the game runs exactly as shipped (1280x720, 16:9). Default.
//     W    (1288..1600, rounded down to a multiple of 8) a real W x 720 back buffer, Hor+ camera,
//          rt pool scaled by area. Needs the SDK's video mode to be W x 720 too (--resolution=Wx720)
//          or the presenter stretches the picture; the log line of the scene-size hook says so.
//     -1   auto: W = the video mode's width when the game was given a mode wider than 1280 and
//          exactly 720 high (what the launcher's "Fit this screen" passes as --resolution=1560x720);
//          otherwise off. Lets the profile turn widescreen on with no extra launcher plumbing.
//
// The scene-size hook (W1, mcla_ws_scene_size.cpp) runs once at device init, decides the final W
// from the cvar and the video mode, writes it into the game's two `width` / `height` option words
// and publishes it here. Every other hook asks ws::Width(): 0 until W1 has run (so a hook can never
// act before the buffer exists, and a build with W1 compiled out leaves the whole feature off).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "mcla_hooks.h"

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

REXCVAR_DECLARE(int32_t, mcla_widescreen_width);
REXCVAR_DECLARE(int32_t, mcla_ws_aspect_mask);
REXCVAR_DECLARE(bool, mcla_ws_cutscene_fov);
REXCVAR_DECLARE(bool, mcla_ws_rt_pool_scale);
REXCVAR_DECLARE(bool, mcla_ws_rt_pool_guard);
REXCVAR_DECLARE(int32_t, mcla_ws_edram_reuse);
REXCVAR_DECLARE(bool, mcla_ws_edram_log);

namespace mcla::ws {

constexpr uint32_t kSceneHeight = 720;  // the game's HD height; the scene is always 720 lines
constexpr uint32_t kBaseWidth = 1280;   // the shipped HD width
constexpr uint32_t kMinWidth = 1288;    // first multiple of 8 above 1280
constexpr uint32_t kMaxWidth = 1600;    // EDRAM: 1 tile at 1x up to here (WIDESCREEN.md section 3)

// The 16:9 loads that become W/H (cvar mcla_ws_aspect_mask, one bit per consumer).
enum AspectBit : int32_t {
  kAspectCamera = 1,  // 0x822E5E68  scene camera (sub_822E5E10)
  kAspectMirror = 2,  // 0x82233EB4  rear-view mirror ortho (sub_82233E18)
  kAspectUiA = 4,     // 0x82214BB8  sub_82214AC0 (not analysed)
  kAspectUiB = 8,     // 0x8223E5E0  sub_8223E230 (compared with the viewport's aspect)
  kAspectMap = 16,    // 0x82620700  pause-map visible box (sub_826206B0)
};
constexpr int32_t kAspectMaskAll = 31;

// The back-buffer width the hooks work with: 0 = widescreen off (or the scene-size hook has not
// run yet). Cheap: one atomic load.
uint32_t Width();

// The master cvar is non-zero (armed: it may still resolve to off at device init). For the
// "[mcla-<tag>] active" registry lines.
bool Armed();

// W / 720 as the game's single-precision float (2.1666667 for 1560), or 0 when widescreen is off.
double Aspect();

// For a hook that replaces a 16:9 constant: true (and the aspect in *aspect) when widescreen is on
// and the mask cvar has `bit` set.
bool AspectFor(int32_t bit, double* aspect);

// Called by the scene-size hook only. Resolves the master cvar against the video mode the game was
// given (the guest's XGetVideoMode result) and publishes the answer; returns it (0 = off). `why`
// (optional) receives a short reason when the answer is 0 or the request was adjusted.
uint32_t ResolveAtDeviceInit(uint32_t mode_width, uint32_t mode_height, const char** why);

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
