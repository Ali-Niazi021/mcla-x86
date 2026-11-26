// mcla_menu_msaa: draw the scene passes the game forces to 4x MSAA (the front-end world, the pause-menu backdrop) in the
// renderer's own MSAA mode instead (docs/mcla/RENDER_NOTES.md). A/B switch, default OFF (--mcla_menu_noaa=true switches
// it on), read on every pass, so it can be flipped at run time (tools/flows/menus_tour.txt does).
//
// Why. In-game driving is a 1x single tile with mcla_noaa (2x without): the renderer's own mode is renderer+384. The
// front-end world and the pause screen are drawn at 4x, because the scene pass raises the mode it hands to
// BeginTiledRendering (sub_8217A470, r6 = MSAA mode 1 / 2 / 4) whenever one of three renderer flags is set. At
// 1920x1080 the game's own tile arithmetic makes 4x seven tiles, and one 4x tile needs 13056 EDRAM tiles of the 4096 the
// GPU emulates (mcla_ws_single_tile's SingleTileFits), so our single-tile merge cannot take it: the pause screen
// misses about every frame of its 60 fps budget. Drawn in the renderer's own mode (1x with mcla_noaa) it is the same
// one tile as driving, and mcla_ws_single_tile merges it like any other pass. A change of MSAA mode between two
// one-tile passes is also what makes that hook settle for a few passes (kSettlePasses): drawing the menus in the
// driving mode removes the change.
//
// The two sites (both in the generated code):
//
//   sub_822E5ED8, the scene pass (r31 = the renderer, `mr r31,r3` at the entry):
//       r8 = BE32[renderer+384]               the renderer's own mode (1 with mcla_noaa, else 2; 4 for SD output)
//       r6 = r8; if r8 == 3 -> r6 = 4
//       if BYTE[renderer+393] != 0 -> r6 = 4
//       if BYTE[renderer+432] != 0 and a TLS byte == 0 -> flag = BYTE[renderer+433] else flag = BYTE[renderer+373];
//       if flag != 0 -> r6 = 4
//       ...
//       0x822E6008  li r8,0                   <- hook A, BEFORE this (r6 = the mode, r31 = the renderer)
//                   mr r7,r11 ; addi r5,r1,80 ; li r4,0 ; mr r3,r29 ; bl 0x8217A470
//
//   sub_822E5358, the second pass (r28 = the renderer, `mr r28,r3` at the entry; r31 is a local zero here, NOT the
//   renderer): the same rule with a different base:
//       r27 = BE32[renderer+380]; if r27 == 3 or BYTE[renderer+393] != 0 or BYTE[renderer+373] != 0 -> r27 = 4
//       0x822E5424  mr r6,r27
//       0x822E5428  stb r31,164(r1)           <- hook B, BEFORE this (r6 already = r27, r28 = the renderer)
//                   ... ; 0x822E5438  bl 0x8217A470
//
// What a hook does. When the pass is forced (r6 is 4 and the renderer's own mode is neither 3 nor 4: 3 is the game's own
// route to 4, and 4 is the SD output's own mode, neither is a flag-forced 4x) and mcla_menu_noaa is on, it sets
// r6 = the renderer's own mode (a mode below 1 reads as 1). Only the register handed to BeginTiledRendering changes:
// no guest memory is written. A renderer pointer or mode word that cannot be read (mcla::guest::U32 fails) leaves the
// pass as the game computed it. mcla_noaa is read when the renderer is created, so the renderer's mode is a constant of
// the run (1 or 2). The tile surface the clamped pass then asks for is the one of the renderer's own mode: the game's
// surface cache makes it as for any pass (mcla_ws_single_tile guards its own merge against a full cache).
//
// Log (the state changes only: no line per frame, no periodic line; each site's passes run once a frame on the render
// thread, so the work per pass is one guest read while the pass is at 4x and a register compare when it is not):
//     [mcla-menu-msaa] <scene|pass2>: forced 4x <begins|ends|continues> (flags +373=<b> +393=<b> +432=<b> +433=<b>,
//         renderer mode <n>): <drawn at <n>x (mcla_menu_noaa) | kept at 4x (mcla_menu_noaa=false)>
// `begins` is the first forced pass of a run of them, with the cvar on or off: it is the inventory of where the game
// forces 4x, and the four flag bytes say which flag did it (a byte that cannot be read prints ?). `ends` is the first
// pass of the site that is not forced after such a run; its flags are the ones read then (the flag that forced the
// 4x is gone), its drawn / kept tail is what the run's last forced pass got, and it adds the run's pass counts
// (`; <n> forced passes, <m> drawn clamped`). `continues` is a toggle of mcla_menu_noaa during a run. Plus one
// `[mcla-menu-msaa] active: ...` line at the first clamped pass of the process.
//
// Thread safety: the hooks run on the game's render thread (one pass at a time); the state is relaxed atomics, and the
// exchange that detects a state change makes one thread the only logger of it, as in mcla_ws_single_tile.
//
// codegen entries (mcla/codegen/hooks_widescreen.toml, NEEDS A RE-CODEGEN, new entries), both `void`, no `after_instruction`:
//     0x822E6008  mcla_hook_menu_msaa_scene   registers r6, r31
//     0x822E5428  mcla_hook_menu_msaa_pass2   registers r6, r28
//
// Off: --mcla_menu_noaa=false (default), or MCLA_HOOK_MENU_MSAA=0 at build time (the functions stay and do nothing).
#include <atomic>
#include <cstdint>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_MENU_MSAA
#define MCLA_HOOK_MENU_MSAA 0
#endif

REXCVAR_DEFINE_BOOL(mcla_menu_noaa, false, "MCLA",
                    "MCLA: the scene passes the game forces to 4x MSAA (front-end world, pause-menu backdrop: renderer "
                    "flags +373 / +393 / +433) are drawn in the renderer's own MSAA mode (renderer+384, 1 with "
                    "mcla_noaa) instead; read on every pass, so it can change at run time; default off = the game's 4x");

namespace {

mcla::hooks::Registration g_registration("menu-msaa", "mcla_menu_noaa", MCLA_HOOK_MENU_MSAA != 0,
                                         [] { return REXCVAR_GET(mcla_menu_noaa); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_MENU_MSAA

namespace {

constexpr uint32_t kMsaaMax = 4;               // the MSAA mode the game forces (BeginTiledRendering r6 = 1 / 2 / 4)
constexpr uint32_t kModeBased = 3;             // renderer mode 3: the game's own route to 4 (not a flag)
constexpr uint32_t kFlagOffsets[4] = {373, 393, 432, 433};  // renderer +373 / +393 / +432 / +433 (log only: +432 gates +433)

// One site's state. `forced` is 1 while the game forces 4x at this site; `clamping` is mcla_menu_noaa at the last forced
// pass (-1 before the first one); the run counters count the passes of the current run of forced passes.
struct Site {
  Site(const char* site_name, uint32_t mode_word_offset) : name(site_name), mode_offset(mode_word_offset) {}
  const char* const name;
  const uint32_t mode_offset;  // the renderer's own mode: renderer+384 (scene) / renderer+380 (pass2)
  std::atomic<int32_t> forced{0};
  std::atomic<int32_t> clamping{-1};
  std::atomic<uint32_t> run_forced{0};
  std::atomic<uint32_t> run_clamped{0};
};

Site g_scene("scene", 384);
Site g_pass2("pass2", 380);

enum class Event : uint8_t { kBegins, kEnds, kContinues };

std::string Number(bool ok, uint32_t value) { return ok ? std::to_string(value) : std::string("?"); }

// The log line of one state change (cold: a state change, not a pass). `clamped` = what the run's forced pass(es) got.
void LogChange(const Site& site, Event event, uint32_t renderer, bool clamped, uint32_t run_forced,
               uint32_t run_clamped) {
  std::string flags[4];
  for (int i = 0; i < 4; ++i) {
    uint8_t value = 0;
    flags[i] = Number(mcla::guest::U8(renderer + kFlagOffsets[i], &value), value);
  }
  uint32_t natural = 0;
  const bool have_natural = mcla::guest::U32(renderer + site.mode_offset, &natural);
  const std::string mode = Number(have_natural, natural);
  const char* what = event == Event::kBegins ? "begins" : event == Event::kEnds ? "ends" : "continues";
  std::string tail;
  if (!clamped) {
    tail = "kept at 4x (mcla_menu_noaa=false)";
  } else if (have_natural) {
    tail = "drawn at " + std::to_string(natural < 1 ? 1u : natural) + "x (mcla_menu_noaa)";
  } else {
    tail = "drawn at the renderer's own mode (mcla_menu_noaa)";
  }
  if (event == Event::kEnds) {
    REXLOG_INFO("[mcla-menu-msaa] {}: forced 4x {} (flags +373={} +393={} +432={} +433={}, renderer mode {}): {}; {} "
                "forced passes, {} drawn clamped",
                site.name, what, flags[0], flags[1], flags[2], flags[3], mode, tail, run_forced, run_clamped);
  } else {
    REXLOG_INFO("[mcla-menu-msaa] {}: forced 4x {} (flags +373={} +393={} +432={} +433={}, renderer mode {}): {}",
                site.name, what, flags[0], flags[1], flags[2], flags[3], mode, tail);
  }
}

// One pass of one site. r6 = the MSAA mode the game is about to hand to BeginTiledRendering, `renderer` = the pointer
// the site's register holds. Nothing here writes guest memory; r6 is the only thing that can change.
void Pass(Site& site, PPCRegister& r6, uint32_t renderer) {
  const bool at_max = r6.u32 == kMsaaMax;
  if (!at_max && site.forced.load(std::memory_order_relaxed) == 0) {
    return;  // the usual pass (driving): not 4x, was not forced: nothing to do, nothing to log
  }
  uint32_t natural = 0;
  bool forced = false;
  if (at_max) {
    if (!mcla::guest::U32(renderer + site.mode_offset, &natural)) {
      return;  // unknown: leave the pass and the logged state as they are
    }
    forced = natural != kModeBased && natural < kMsaaMax;
  }
  const bool clamp = forced && REXCVAR_GET(mcla_menu_noaa);
  if (clamp) {
    r6.u64 = natural < 1 ? 1u : natural;
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
      REXLOG_INFO("[mcla-menu-msaa] active: the scene passes the game forces to 4x MSAA (renderer flags +373 / +393 / "
                  "+433) are drawn in the renderer's own MSAA mode (first: {} pass, {}x instead of 4x)",
                  site.name, natural < 1 ? 1u : natural);
    }
  }
  const int32_t was_forced = site.forced.exchange(forced ? 1 : 0, std::memory_order_relaxed);
  if (forced) {
    const int32_t clamp_now = clamp ? 1 : 0;
    const int32_t was_clamping = site.clamping.exchange(clamp_now, std::memory_order_relaxed);
    if (was_forced == 0) {
      site.run_forced.store(0, std::memory_order_relaxed);
      site.run_clamped.store(0, std::memory_order_relaxed);
    }
    const uint32_t run_forced = site.run_forced.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t run_clamped =
        clamp ? site.run_clamped.fetch_add(1, std::memory_order_relaxed) + 1 : site.run_clamped.load(std::memory_order_relaxed);
    if (was_forced == 0) {
      LogChange(site, Event::kBegins, renderer, clamp, run_forced, run_clamped);
    } else if (was_clamping != clamp_now) {
      LogChange(site, Event::kContinues, renderer, clamp, run_forced, run_clamped);
    }
  } else if (was_forced != 0) {
    LogChange(site, Event::kEnds, renderer, site.clamping.load(std::memory_order_relaxed) != 0,
              site.run_forced.load(std::memory_order_relaxed), site.run_clamped.load(std::memory_order_relaxed));
  }
}

}  // namespace

// 0x822E6008 in sub_822E5ED8, BEFORE `li r8,0`: r6 = the MSAA mode for BeginTiledRendering, r31 = the renderer.
void mcla_hook_menu_msaa_scene(PPCRegister& r6, PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  Pass(g_scene, r6, r31.u32);
}

// 0x822E5428 in sub_822E5358, BEFORE `stb r31,164(r1)`: r6 (= r27) = the MSAA mode, r28 = the renderer.
void mcla_hook_menu_msaa_pass2(PPCRegister& r6, PPCRegister& r28) {
  MCLA_HOOK_ENTER();
  Pass(g_pass2, r6, r28.u32);
}

#else  // MCLA_HOOK_MENU_MSAA == 0: keeps the codegen entries linking, does nothing.

void mcla_hook_menu_msaa_scene(PPCRegister&, PPCRegister&) {}

void mcla_hook_menu_msaa_pass2(PPCRegister&, PPCRegister&) {}

#endif
