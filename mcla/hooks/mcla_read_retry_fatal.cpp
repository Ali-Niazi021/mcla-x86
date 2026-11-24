// mcla_read_retry_fatal: say which file read the game gave up on (HOOKS_SPEC B2). Log only.
//
// Two functions read through the ReadFile wrapper 0x8244F4C0 (r3 handle, r4 buffer, r5 length) and
// retry up to 10 times (a counter in r31, a call to 0x82135DB0 between tries); after the 10th
// failure they call the fatal handler [0x8285FEA0] (the same route as mcla_rage_fatal: the SDK's
// XamShowDirtyDiscErrorUI, which exits). The hook sits on the `lwz r11,-352(r26)` that loads the
// handler, so it runs exactly when the game gives up, before the handler.
//
//   0x821C4FF0  in sub_821C4F98(_, handle r4, buffer r5, length r6):   r29 handle, r28 buffer, r27 length
//   0x821C50C8  in sub_821C5038(_, handle r4, offset r5, buffer r6, length r7):
//                                                                      r30 handle, r29 buffer, r28 length
//
// The register roles were read from the generated code of both functions (each `mr rN,r4..r7`
// before the call). NOTE: docs/mcla/HOOKS_SPEC.md lists "r29, r28, r27" for both sites; that is
// right for the first only. Register values only: nothing is dereferenced.
//
// Off: --mcla_read_retry_fatal=false, or MCLA_HOOK_READ_RETRY_FATAL=0 at build time.
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_read_retry_fatal, true, "MCLA",
                    "MCLA: log the handle, buffer and length of a file read that failed 10 times "
                    "(just before the game's fatal handler)");

namespace {

mcla::hooks::Registration g_registration("io", "mcla_read_retry_fatal", MCLA_HOOK_READ_RETRY_FATAL != 0,
                                         [] { return REXCVAR_GET(mcla_read_retry_fatal); });

}  // namespace

#if MCLA_HOOK_READ_RETRY_FATAL

namespace {

void LogReadFailure(uint32_t site, uint32_t handle, uint32_t buffer, uint32_t length) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= 8) {
    return;
  }
  REXLOG_ERROR("[mcla-io] read failed 10x handle=0x{:X} buffer=0x{:08X} len=0x{:X} (site 0x{:08X}): "
               "the game's fatal handler runs next",
               handle, buffer, length, site);
  rex::FlushLogging();
}

}  // namespace

void mcla_hook_read_retry_fatal(PPCRegister& r29, PPCRegister& r28, PPCRegister& r27) {
  MCLA_HOOK_ENTER();
  if (REXCVAR_GET(mcla_read_retry_fatal)) {
    LogReadFailure(0x821C4FF0, r29.u32, r28.u32, r27.u32);
  }
}

void mcla_hook_read_retry_fatal_offset(PPCRegister& r30, PPCRegister& r29, PPCRegister& r28) {
  MCLA_HOOK_ENTER();
  if (REXCVAR_GET(mcla_read_retry_fatal)) {
    LogReadFailure(0x821C50C8, r30.u32, r29.u32, r28.u32);
  }
}

#else  // MCLA_HOOK_READ_RETRY_FATAL == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_read_retry_fatal(PPCRegister&, PPCRegister&, PPCRegister&) {}
void mcla_hook_read_retry_fatal_offset(PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
