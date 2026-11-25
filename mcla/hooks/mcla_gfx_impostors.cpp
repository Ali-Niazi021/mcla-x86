// mcla_gfx_impostors: the tree impostors (distant trees drawn as small pre-rendered views) off, with the guard that makes that
// safe (docs/PLAN.md W5.4; scratch/S3 lever #5 and finding 4). About 12 impostor views of 128x128 are refreshed per frame, each
// a render target switch, a clear and a resolve: ~0.7 ms of GPU on the Radeon 880M [INFERENCE]; on Renan's 2-CU part, impostors
// off was -4.2 % of the frame. The picture cost is the distant tree billboards (bare far hills) [INFERENCE].
//
// Two function overrides (no codegen entry: the weak-alias linkage of mcla_fsr_markers.cpp, the originals are called through
// __imp__sub_X), both pass-through while mcla_gfx_impostors is true (the default): a load or two and the original.
//
// 1. sub_8230B778 (0x8230B778), the creation of one impostor's render targets, called by sub_8230C388 (0x8230C3D8) for every
//    entry of [this+16] (array [container+8], count lhz [container+12], 224 bytes each) that has a non-zero word at +168. It
//    begins with `addi r10,r11,-12072; lwz r11,4(r10); cmplwi cr6,r11,0; bne -> 0x8230B9A4` (0x8230B78C): the dev option
//    `noimpostors` (node 0x8288D0D8, value pointer at +4); set, the function returns at once, before it changes anything
//    (0x8230B9A4 is the epilogue). The override does exactly that when the cvar says off: it returns without calling the
//    original, so the entry's targets (+188 / +192 / +196) stay 0, and no state of the game is written. That is the game's
//    own, shipped path for the switch.
//
// 2. sub_8230D988 (0x8230D988), the per-frame refresh of the impostor views, called from the shadow phase handler
//    sub_823120C8 (call at 0x823126A4, with [0x8288D0A0] the manager) when the cascade-0 phase (0x20) runs. After the setup it either
//    refreshes every entry that has a +168 (|time delta| >= the threshold, bounded), or searches round-robin for ONE entry
//    (0x8230DA08..0x8230DA64):
//        idx = (cursor + 1) % count; cursor = idx; entry = array + idx * 224;
//        if (entry[+168] == 0) continue; if (entry[+196] == 0) continue; refresh(entry) and leave
//    with `container = [this+16]`, `array = [container+8]`, `count = lhz [container+12]` (the function returns at once when the
//    container is null or the count is 0). `cursor` is the word 0x8288D064. The search has no bound: when no entry has both
//    fields set, it never ends. With impostors off no entry ever gets +196 (the targets are not created) while the entries
//    keep their +168, so the game's thread would spin there for good. The override looks for an entry that qualifies, with
//    the loop's own test, before the original runs and, while the cvar says off and none does, returns without calling it
//    (the original's bounded branch would only refresh entries that have no targets).
//
// Live: the switch is read at every call. Turning it off applies to the impostors created from then on (sub_8230B778 runs
// when the entries get their targets), turning it on again to the ones created after that. Cvar mcla_gfx_impostors (default
// true), preset perf+ = false. Log, once: `[mcla-gfx] impostors: off ...`.
//
// Build switch: MCLA_HOOK_GFX_IMPOSTORS=0 compiles the overrides out (the generated weak functions run as shipped).
#include <atomic>
#include <cstdint>

#include "nfsmw_pch.h"

#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_IMPOSTORS
#define MCLA_HOOK_GFX_IMPOSTORS 0
#endif

namespace {

mcla::hooks::Registration g_registration("gfx-impostors", "mcla_gfx_impostors", MCLA_HOOK_GFX_IMPOSTORS != 0,
                                         [] { return !mcla::gfxtune::ImpostorsEnabled(); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_GFX_IMPOSTORS

REX_EXTERN(__imp__sub_8230B778);
REX_EXTERN(__imp__sub_8230D988);

namespace {

constexpr uint32_t kManagerContainer = 16;   // [manager+16] -> the container of the entries
constexpr uint32_t kContainerArray = 8;      // [container+8] -> the entry array
constexpr uint32_t kContainerCount = 12;     // lhz [container+12] -> the entry count
constexpr uint32_t kEntrySize = 224;
constexpr uint32_t kEntryField168 = 168;     // non-zero once the entry is in use
constexpr uint32_t kEntryTarget = 196;       // the impostor's target, created by sub_8230B778

std::atomic<bool> g_logged_create{false};
std::atomic<bool> g_logged_guard{false};
// Set the first time sub_8230B778 is skipped: from then on entries may exist that never got their targets, also after the
// cvar goes back to true, until a world load rebuilds them, so the refresh guard stays on.
std::atomic<bool> g_creation_skipped{false};

// The test of the original's round-robin search (0x8230DA08..0x8230DA64) over all entries: true when at least one entry has
// both fields set, i.e. when the search ends. True too whenever the original would return before searching (null container,
// no entries) or when a pointer looks wrong: then the original decides, as it always did.
bool SearchEnds(uint8_t* base, uint32_t manager) {
  if (manager < 0x10000) {
    return true;
  }
  const uint32_t container = REX_LOAD_U32(manager + kManagerContainer);
  if (container < 0x10000) {
    return true;
  }
  const uint32_t count = REX_LOAD_U16(container + kContainerCount);
  if (count == 0) {
    return true;
  }
  const uint32_t array = REX_LOAD_U32(container + kContainerArray);
  if (array < 0x10000) {
    return true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = array + i * kEntrySize;
    if (REX_LOAD_U32(entry + kEntryField168) != 0 && REX_LOAD_U32(entry + kEntryTarget) != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

// The creation of one impostor's targets: with the cvar off, what the game's own `noimpostors` switch does.
extern "C" REX_FUNC(sub_8230B778) {
  MCLA_HOOK_ENTER();
  if (!mcla::gfxtune::ImpostorsEnabled()) {
    g_creation_skipped.store(true, std::memory_order_relaxed);
    if (!g_logged_create.exchange(true, std::memory_order_relaxed)) {
      REXLOG_INFO(
          "[mcla-gfx] impostors: off (mcla_gfx_impostors=false): sub_8230B778 creates no impostor targets, as the game's own "
          "noimpostors switch; sub_8230D988 is guarded against its search loop");
    }
    return;
  }
  __imp__sub_8230B778(ctx, base);
}

// The per-frame refresh: not entered when its round-robin search could never end, from the moment impostors are off (or were).
extern "C" REX_FUNC(sub_8230D988) {
  MCLA_HOOK_ENTER();
  const bool guarded = g_creation_skipped.load(std::memory_order_relaxed) || !mcla::gfxtune::ImpostorsEnabled();
  if (guarded && !SearchEnds(base, ctx.r3.u32)) {
    if (!g_logged_guard.exchange(true, std::memory_order_relaxed)) {
      REXLOG_INFO("[mcla-gfx] impostors: sub_8230D988 skipped (no entry has both +168 and +196: its search would not end)");
    }
    return;
  }
  __imp__sub_8230D988(ctx, base);
}

#endif  // MCLA_HOOK_GFX_IMPOSTORS
