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
//
// ---- mcla_gfx_impostor_interval: the regeneration throttle (scratch/R4 change 5) -------------------------------------
//
// sub_8230DDD0 (0x8230DDD0) generates one impostor type's view (a 128x128 target: lock, clear, draws, resolve); it is called
// by the type loop sub_8230EE20 (call at 0x8230F064) for every type whose last-visible frame [type+0x9C] is this frame or the
// one before (0x8230F01C..0x8230F05C). The type is the same 224-byte entry as above (r31). The function decides whether to
// regenerate:
//     0x8230DE1C  lwz r10,-8092(r10)            the renderer
//     0x8230DE20  lwz r11,11724(r9)             r11 = the frame counter BE32[0x827D2DCC] (incremented once per frame in
//                                               sub_8217B7B0 at 0x8217B7EC)
//     0x8230DE24  lbz r8,435(r10); beq -> 0x8230DE3C
//     0x8230DE30  r11 = 1 << (r11 & 3)          (renderer +435 set)
//     0x8230DE3C  r11 = (1 << (r11 & 1)) * 5    the usual case: 0b0101 on even frames, 0b1010 on odd ones
//     0x8230DE4C  lwz r10,152(r31)              r10 = the type's frame mask [type+0x98]
//     0x8230DE50  and r9,r10,r11                <- the hook, BEFORE this (word 0x7D495838 in the image)
//     0x8230DE54  cmpwi r9,0; bne -> 0x8230DE6C  regenerate when the mask has this frame's bit
//     0x8230DE5C  lbz r11,217(r31); r10 = the night flag (r30): beq -> 0x8230E15C (return) when the night flag did not change
// The tuning table sets a type's mask (sub_8230B6B0), defaulting to 3 (`li r9,3` at 0x8230B724): bit 0 | bit 1 hits both
// the even-frame pattern and the odd-frame one, so an untuned type regenerates EVERY frame. The hook zeroes r10 for a type
// that was regenerated less than N frames ago: the `and` is then 0 and the game takes its own "no bit" path, which still
// regenerates when the night flag changed. A type with a tuned period of 2 or more is not affected (its own mask already
// spaces it by N), nor is a type the game would not regenerate this frame. Per type (r31) the last frame it was let through
// is kept; `frame` is the game's counter (read straight from the image's .data through a cached host pointer: this runs
// per type per frame, a syscall each would be too much).
// Cvar mcla_gfx_impostor_interval: 1 = as shipped (the hook returns at once), N >= 2 = at most every N-th frame, 0 = as
// shipped plus `[mcla-gfx] impostors: ...` every 300 frames (types seen, regenerated, throttled).
// Build switch: MCLA_HOOK_GFX_IMPOSTOR_THROTTLE=0 keeps the function and does nothing (the codegen entry stays linking).
//
// Codegen entry (mcla/codegen/hooks_gfx.toml):
//   [[midasm_hook]]
//   address = 0x8230DE50
//   name = "mcla_hook_gfx_impostor_throttle"
//   registers = ["r10", "r11", "r31"]
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "nfsmw_pch.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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

// ---- the regeneration throttle ----------------------------------------------------------------------------------------

#ifndef MCLA_HOOK_GFX_IMPOSTOR_THROTTLE
#define MCLA_HOOK_GFX_IMPOSTOR_THROTTLE 0
#endif

namespace {

mcla::hooks::Registration g_throttle_registration("gfx-impostor-throttle", "mcla_gfx_impostor_interval",
                                                  MCLA_HOOK_GFX_IMPOSTOR_THROTTLE != 0,
                                                  [] { return mcla::gfxtune::ImpostorInterval() != 1; },
                                                  /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_GFX_IMPOSTOR_THROTTLE

namespace {

constexpr uint32_t kGuestFrameCounter = 0x827D2DCC;  // BE32 in the image's .data, always mapped
constexpr uint32_t kStatFrames = 300;
constexpr size_t kMaxTypes = 4096;

// The host address of the guest frame counter (the translation of an address inside the image never changes).
const volatile uint32_t* FrameCounterPointer() {
  static std::atomic<const volatile uint32_t*> cached{nullptr};
  const volatile uint32_t* pointer = cached.load(std::memory_order_relaxed);
  if (!pointer) {
    auto* kernel = rex::system::kernel_state();
    rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
    if (!memory) {
      return nullptr;
    }
    pointer = memory->TranslateVirtual<const volatile uint32_t*>(kGuestFrameCounter);
    cached.store(pointer, std::memory_order_relaxed);
  }
  return pointer;
}

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_last_generated;  // type (entry address) -> the frame it was let through in
struct Stats {
  uint32_t first_frame = 0;
  uint32_t seen = 0;         // calls with the game's mask asking for a regeneration
  uint32_t regenerated = 0;  // of those, let through
  uint32_t throttled = 0;    // of those, held back
} g_stats;
std::atomic<bool> g_logged_interval{false};

}  // namespace

// sub_8230DDD0, BEFORE `and r9,r10,r11` at 0x8230DE50: r10 = the type's frame mask [r31+0x98], r11 = this frame's bit(s),
// r31 = the type. r10 = 0 makes the game skip the regeneration (unless its night flag changed).
void mcla_hook_gfx_impostor_throttle(PPCRegister& r10, PPCRegister& r11, PPCRegister& r31) {
  MCLA_HOOK_ENTER();
  const uint32_t interval = mcla::gfxtune::ImpostorInterval();
  if (interval == 1) {
    return;  // as shipped
  }
  const volatile uint32_t* counter = FrameCounterPointer();
  if (!counter) {
    return;
  }
  const uint32_t frame = __builtin_bswap32(*counter);
  const bool asks = (r10.u32 & r11.u32) != 0;  // the game would regenerate this type for its own mask
  if (!g_logged_interval.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[mcla-gfx] impostors: regeneration {} (mcla_gfx_impostor_interval={}): hook 0x8230DE50", 
                interval ? "throttled to at most every Nth frame per type" : "counted only", interval);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_stats.first_frame == 0 || uint32_t(frame - g_stats.first_frame) >= kStatFrames) {
    if (g_stats.first_frame != 0) {
      REXLOG_INFO("[mcla-gfx] impostors: {} frames: {} regenerations asked for, {} let through, {} held back (interval {})",
                  frame - g_stats.first_frame, g_stats.seen, g_stats.regenerated, g_stats.throttled, interval);
    }
    g_stats = Stats{};
    g_stats.first_frame = frame;
  }
  if (!asks) {
    return;
  }
  ++g_stats.seen;
  if (interval >= 2) {
    if (g_last_generated.size() >= kMaxTypes) {
      g_last_generated.clear();  // entries are rebuilt with the world; stale addresses are harmless but must not pile up
    }
    auto it = g_last_generated.find(r31.u32);
    if (it != g_last_generated.end() && uint32_t(frame - it->second) < interval) {
      r10.u64 = 0;
      ++g_stats.throttled;
      return;
    }
    g_last_generated[r31.u32] = frame;
  }
  ++g_stats.regenerated;
}

#else  // MCLA_HOOK_GFX_IMPOSTOR_THROTTLE == 0: keeps hooks_gfx.toml linking, does nothing.

void mcla_hook_gfx_impostor_throttle(PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
