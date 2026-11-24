// mcla_skip_intro: shorten the legals / Rockstar / logo sequence at boot (HOOKS_SPEC D1 and D2).
// Cvar mcla_skip_intro (default ON) gates both hooks; off = the game's own boot, unchanged.
//
// D1  mcla_hook_skip_intro, site 0x822C2F48 in the init function sub_822C2EA8 (called from main),
//     BEFORE `lwz r11,2836(r31)`; true = jump to 0x822C2F7C.
//     By then init has built the 16-byte "legals" object (slot 0x82830B14: +0 SWF file, +4 SWF
//     player, +0xC render-thread-owns byte, +0xD skip request, +0xE done) and stepped its movie.
//     The code that follows starts a render-thread callback (0x821FC008) that DRAWS the legals while
//     init loads. The hook sets the game's own skip request (byte +0xD = 1; the main loop's legals
//     update turns it into the SWF variable StartOfTransitionOut and the movie ends itself) and skips
//     the start of that callback, so the legals are not drawn during the load. The object, its SWF
//     and its end-of-transition path stay (the title logo keeps its set-up). When the slot is empty
//     or the movie has no player the hook returns false: the game's code handles that itself.
//
// D2  mcla_hook_intro_pass_mask, site 0x821315E4 in main (sub_82131508), BEFORE `bl 0x822E5B00`
//     with r4 = -1 set just before: that path runs when the legals object is null (skipped early, or
//     the legals SWF failed to load) and ORs r4 into the render-pass mask. Every other caller of the
//     routine passes 0xFEFFFFFF (bit 24 has no prepared stage); the hook does the same.
//
// Open point (HOOKS_SPEC D1): if the title logo is missing with the hook on, try --mcla_skip_intro=false.
//
// Off: --mcla_skip_intro=false, or MCLA_HOOK_SKIP_INTRO=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_skip_intro, true, "MCLA",
                    "MCLA: skip drawing the legals during the boot load (the game's own skip request) "
                    "and give the render passes the normal mask when the legals object is gone "
                    "(false: the game's own boot)");

namespace {

mcla::hooks::Registration g_registration("skip-intro", "mcla_skip_intro", MCLA_HOOK_SKIP_INTRO != 0,
                                         [] { return REXCVAR_GET(mcla_skip_intro); });

}  // namespace

#if MCLA_HOOK_SKIP_INTRO

namespace {

constexpr uint32_t kLegalsSlot = 0x82830B14;  // -> the 16-byte legals object (0 = none)
constexpr uint32_t kPlayerOffset = 4;         // the object's SWF player
constexpr uint32_t kSkipRequestOffset = 0xD;  // byte: the game's own "skip" request

}  // namespace

bool mcla_hook_skip_intro() {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_skip_intro)) {
    return false;
  }
  uint32_t object = 0;
  uint32_t player = 0;
  if (!mcla::guest::U32(kLegalsSlot, &object) || object == 0 ||
      !mcla::guest::U32(object + kPlayerOffset, &player) || player == 0) {
    return false;
  }
  if (!mcla::guest::WriteU8(object + kSkipRequestOffset, 1)) {
    return false;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-skip-intro] legals object 0x{:08X}: skip requested, render-thread legals not "
                "started",
                object);
  }
  return true;
}

void mcla_hook_intro_pass_mask(PPCRegister& r4) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_skip_intro)) {
    return;
  }
  r4.s64 = int32_t(0xFEFFFFFF);  // what `lis r4,-257; ori r4,r4,65535` would give
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[mcla-skip-intro] no legals object: render-pass mask 0xFEFFFFFF instead of -1");
  }
}

#else  // MCLA_HOOK_SKIP_INTRO == 0: keeps hooks.toml linking, does nothing.

bool mcla_hook_skip_intro() { return false; }
void mcla_hook_intro_pass_mask(PPCRegister&) {}

#endif
