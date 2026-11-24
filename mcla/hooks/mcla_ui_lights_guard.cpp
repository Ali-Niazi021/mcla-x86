// mcla_ui_lights_guard: skip a Scaleform movie's lighting pass when its "lights" member is not
// there yet (HOOKS_SPEC C1). Needed: the pass crashes on a half-built movie.
//
// Site 0x82725100 in sub_827250A8 (the per-movie lighting pass), BEFORE `bl 0x825EF9F0`:
//     0x827250FC  bl 0x825ED480      r3 = the movie member named "lights" (0x82040C48)
//     0x82725100  bl 0x825EF9F0      r3 = (r3[+8] == 5) ? r3[0] : 0     <- the hook runs before this
//     0x82725108  lwz r10,0(r26)     r26 = that result: dereferenced unchecked
// A member that does not exist yet (r3 = 0: the callee reads guest 0x8) or one of another kind
// (callee returns 0, the next load reads guest 0x0) faults the host, because our SDK protects
// guest page 0. The race is the lighting pass running before the movie is built.
//
// The hook returns true (jump to 0x82725144, the `li r3,1` that closes the block; the temporary at
// r1+80 is neither built nor destroyed on the skipped path, so the skip is balanced) when the
// callee would return 0: r3 is 0, [r3+8] is not 5, or [r3] is 0. One movie goes unlit for the frame.
// The reads are validated, so a wild r3 also takes the skip. The first 4 skips are logged.
//
// Off: --mcla_ui_lights_guard=false, or MCLA_HOOK_UI_LIGHTS_GUARD=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_ui_lights_guard, true, "MCLA",
                    "MCLA: skip a UI movie's lighting pass when its 'lights' member is missing "
                    "(the game dereferences null there; false: the game's own code)");

namespace {

mcla::hooks::Registration g_registration("ui-lights", "mcla_ui_lights_guard",
                                         MCLA_HOOK_UI_LIGHTS_GUARD != 0,
                                         [] { return REXCVAR_GET(mcla_ui_lights_guard); });

}  // namespace

#if MCLA_HOOK_UI_LIGHTS_GUARD

bool mcla_hook_ui_lights_guard(PPCRegister& r3) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_ui_lights_guard)) {
    return false;
  }
  const uint32_t member = r3.u32;
  // [member+0] and [member+8] in one read.
  uint32_t words[3] = {};
  bool skip = true;
  const char* why = "no lights member";
  if (member != 0) {
    if (!mcla::guest::Read(member, words, sizeof(words))) {
      why = "lights member unreadable";
    } else if (__builtin_bswap32(words[2]) != 5) {
      why = "lights member of another kind";
    } else if (__builtin_bswap32(words[0]) == 0) {
      why = "lights member without an object";
    } else {
      skip = false;
    }
  }
  if (skip) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_WARN("[mcla-ui-lights] skipped a movie's lighting pass: {} (r3=0x{:08X}, kind {})", why,
                  member, member != 0 ? int(__builtin_bswap32(words[2])) : -1);
    }
  }
  return skip;
}

#else  // MCLA_HOOK_UI_LIGHTS_GUARD == 0: keeps hooks.toml linking, does nothing.

bool mcla_hook_ui_lights_guard(PPCRegister&) { return false; }

#endif
