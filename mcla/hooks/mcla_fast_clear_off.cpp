// mcla_fast_clear_off: make the game's D3D Clear take its plain path (docs/mcla/RENDER_NOTES.md,
// R-GREY). A/B switch, default OFF (--mcla_fast_clear_off=true switches it on).
//
// The Xbox 360 D3D Clear splits a clear rectangle (sub_82418640): the tile-aligned inside is drawn
// as a 4x MSAA rectangle at HALF the pitch (a 4x pixel covers 2x2 samples, so the EDRAM fills four
// times faster); border strips use the surface's own mode. Emulated with the MSAA cap
// (gpu_sin_msaa) that rectangle reaches only half of the target's EDRAM range: the grey blocks.
// Even without the cap it is a 4x host target for every clear. The split runs only if the word
// 0x827EC96C (.data, static 1, read nowhere else) is non-zero; with 0 the whole rectangle is
// drawn once in the target's own MSAA mode and pitch (0x82418894), the path the game already
// takes for rectangles smaller than a tile.
//
// Site 0x82418734 in sub_82418640:
//     0x82418730  lis r11,-32129
//     0x82418734  lwz r11,-13972(r11)       r11 = [0x827EC96C]       <- hooked AFTER this load
//     0x82418738  cmpwi cr6,r11,0
//     0x8241873C  beq cr6,0x82418894        the plain path
// The hook sets r11 = 0 after the load, exactly as if the word held 0. Chosen over writing 0 to
// 0x827EC96C at the first heartbeat because (1) it works from the very first clear (the legals and
// logos clear before the engine timer's first frame), (2) it needs no guest memory write and no
// thread ordering, (3) the word has no other reader, so the two are equivalent afterwards.
//
// Off (default): the hook does nothing. On: --mcla_fast_clear_off=true. Per call cost: one cvar read.
// Build switch: MCLA_HOOK_FAST_CLEAR_OFF=0 compiles the hook to nothing.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_fast_clear_off, false, "MCLA",
                    "MCLA: the game's D3D Clear draws one rectangle in the target's own MSAA mode "
                    "instead of its 4x half-pitch fast clear (A/B switch for the grey blocks; "
                    "default off = the game's own clear)");

namespace {

mcla::hooks::Registration g_registration("fast-clear", "mcla_fast_clear_off",
                                         MCLA_HOOK_FAST_CLEAR_OFF != 0,
                                         [] { return REXCVAR_GET(mcla_fast_clear_off); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_FAST_CLEAR_OFF

void mcla_hook_fast_clear_off(PPCRegister& r11) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_fast_clear_off)) {
    return;
  }
  r11.u64 = 0;  // [0x827EC96C] reads as 0: sub_82418640 takes the plain path (0x82418894)
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-fast-clear] active: D3D clears draw one rectangle in the target's own MSAA "
                "mode and pitch (no 4x half-pitch fast clear)");
  }
}

#else  // MCLA_HOOK_FAST_CLEAR_OFF == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_fast_clear_off(PPCRegister&) {}

#endif
