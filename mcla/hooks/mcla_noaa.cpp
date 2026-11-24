// mcla_noaa: switch on the game's own `noaa` dev option, so the renderer is created without MSAA
// (docs/mcla/RENDER_NOTES.md). A/B switch, default OFF (--mcla_noaa=true switches it on).
//
// The renderer's creation (sub_822E4AB0) reads two dev options: `noaa` (node 0x8287E0F8) and
// `fastdraw` (node 0x8287E10C); either one non-zero stores 1 (no MSAA) in renderer+384 instead of
// the 2x default (4 for SD output). With 1 the game draws the scene as ONE 1x tile instead of two
// 2x tiles: far fewer draws per frame (HOOKS_SPEC / RENDER_NOTES: 30-39 % in the prior art's
// measurements) and no 2x EDRAM tiling to emulate.
//
// Site 0x822E4B80 in sub_822E4AB0:
//     0x822E4B7C  addi r10,r11,-7944        r10 = node 0x8287E0F8
//     0x822E4B80  lwz r11,4(r10)            r11 = the option's value        <- hooked AFTER this load
//     0x822E4B84  cmplwi cr6,r11,0
//     0x822E4B88  bne cr6,0x822e4ba0        -> li r10,1; stw r10,384(renderer)
// The hook sets r11 = 1 after the load: the game's own code then takes its own `noaa` branch,
// exactly as with the dev option set.
//
// Read once, when the renderer is created: change the cvar before the game starts.
// Build switch: MCLA_HOOK_NOAA=0 compiles the hook to nothing.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_noaa, false, "MCLA",
                    "MCLA: turn on the game's own 'noaa' option: the renderer is created without "
                    "MSAA (one 1x scene tile instead of two 2x tiles). Read at renderer creation; "
                    "A/B switch, default off = the game's own 2x");

namespace {

mcla::hooks::Registration g_registration("noaa", "mcla_noaa", MCLA_HOOK_NOAA != 0,
                                         [] { return REXCVAR_GET(mcla_noaa); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_NOAA

void mcla_hook_noaa(PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_noaa)) {
    return;
  }
  r11.u64 = 1;  // the `noaa` option reads as non-zero
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-noaa] active: the renderer is created without MSAA (the game's own noaa "
                "option)");
  }
}

#else  // MCLA_HOOK_NOAA == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_noaa(PPCRegister&) {}

#endif
