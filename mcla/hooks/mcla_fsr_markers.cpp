// mcla_fsr_markers (W5.6): the guest's render markers, the no-op command-stream packets that the SDK's FSR 3.1
// temporal anti-aliasing / upscaling (patches temporal-aa + fsr-upscaling + guest-markers, Renan's) uses to find where
// the 3D scene starts and ends. Without them `temporal_aa=fsr` does nothing: the pass only arms between markers
// (CommandProcessor::OnGuestMarker, src/graphics/command_processor.cpp: kSceneBegin enables the jitter and the squeeze,
// kSceneEnd opens the post window the FSR pass runs in, kTonemapBegin closes it).
//
// Ported from RenanQueiroz/mcla-recomp src/render_markers.cpp (GPL-3, with credit): the call sites, the packet and
// the make-room rule are his findings from launcher/dev/frame-structure-2026-10-16.md, which are his own analysis
// (not LARecomp's). The code here is re-written for this layout; every address below was re-read in our generated code.
// His ultrawide aspect phase is not ours and is left out; his shadow-phase skip is the phase loop override below
// (mcla_gfx_env_interval / mcla_gfx_reflection_interval, written for this layout from the findings of scratch/S3).
//
// What is written, and where. The frame is drawn by sub_822C0980 (called by the game loop sub_82131008), whose phase loop
// sub_822E6408 walks the renderer's phase bits and calls sub_822E55D0(r3 = renderer, r4 = the phase bit) at the start
// of each phase it runs. Phase 0x800 starts the main view's 3D scene (everything before it draws into its own targets:
// reflections, shadows), 0x100000 its translucent effects, 0x04000000 its full-screen composites (the fog), 0x08000000
// is post-processing, 0x10000000 the tone map, 0x20000000 the HUD and menus. At those calls a marker is written into the
// D3D device's command buffer: a PM4 type-3 NOP of 3 words, 'REXM', kind, value (header 0xC0021000). The GPU skips NOPs,
// so the picture does not change; the SDK reads the markers where they fall among the game's draws. The kinds are the
// SDK's GuestMarkerKind; FSR uses kSceneBegin (7, phase 0x800), kSceneEnd (3, 0x08000000) and kTonemapBegin (4,
// 0x10000000); kSceneTranslucent / kSceneComposite only feed the experimental reactive mask (temporal_aa_reactive), and
// kFrameBegin (the frame counter for the resolve log), kFrameEnd, kUIBegin and kRenderEnd (HDR) are written for parity
// and read by nothing FSR needs.
//
// The device is at BE32[0x82839254]: [device+48] is its command buffer write pointer (the last word written; the
// writers store from +4 on) and [device+56] the limit its writers compare the pointer with, once, before writing up to
// 8 words (`lwz r8,48(r31); lwz r11,56(r31); cmplw cr6,r8,r11; ble ...; bl sub_82412710`, e.g. sub_82422968). Past the
// limit they make room with sub_82412710 (r3 = the device; it returns the new write pointer), and so do the markers.
// Making room here is safe because sub_82412710 is what every D3D packet writer calls first, before writing a word of
// its packet: it runs whenever the device is between packets, and the markers' sites are such points (renderer functions,
// never called from inside D3D; none of them or their callers keeps the write pointer across these calls). The next D3D
// call on this thread would make the same call in the same state. Markers are written only on the thread that draws
// frames and only between whole D3D calls (these function boundaries): a development version of Renan's that also marked
// a render-state setter called inside other code corrupted the stream.
//
// HOW IT HOOKS. Function overrides, not mid-asm hooks: a mid-asm hook gets registers by reference and no PPCContext or
// memory base, and the make-room call needs both (it is a guest function). The codegen emits every recompiled function
// through the SDK's weak-alias function macro: sub_X is a weak alias of the strong `extern "C" __imp__sub_X`, so a strong `extern "C" REX_FUNC(
// sub_X)` in the exe wins at link time (mcla/sources.cmake compiles the hook objects directly into the exe, not through
// an archive) and calls the original through `__imp__sub_X`. No codegen entry is needed: a rebuild is enough. The
// existing mid-asm hook of the widescreen UI at 0x822E55D0 (mcla_hook_ws_ui_phase, hooks_widescreen.toml) sits INSIDE
// __imp__sub_822E55D0 and runs unchanged after the marker, so that site needs no sharing. The three originals' arguments
// are kept: the override only reads r3 / r4, and the make-room path saves and restores the whole context.
//
// Gate. cvar mcla_render_markers (INT32, default -1 = auto): 1 on, 0 off, -1 on only while `temporal_aa` is `fsr` or the
// SDK's `tonemap_transfer_skip` (patch 04e0: skips the full-screen ownership transfer the tone map's render target bind
// causes, and needs kTonemapBegin to find the tone map) or the SDK's per-phase GPU timing (`gpu_timing` >= 1, patch 04f0;
// it also turns on a kPhase marker at the start of every phase) is on (read once at the start of each frame, so the game's own
// setting of temporal_aa takes effect on the next frame). With the gate off every override is one relaxed load and the call
// of the original. Log, once, at the first scene marker:
// `[mcla-fsr] render markers: scene begin/end at ...`.
//
// Build switch: MCLA_HOOK_FSR_MARKERS=0 compiles the overrides out (the generated weak functions run as shipped).
#include <atomic>
#include <cstdint>
#include <string>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "mcla_hooks.h"

#ifndef MCLA_HOOK_GFX_PHASES
#define MCLA_HOOK_GFX_PHASES 0
#endif
#ifndef MCLA_HOOK_FSR_MARKERS
#define MCLA_HOOK_FSR_MARKERS 0
#endif

REXCVAR_DEFINE_INT32(mcla_render_markers, -1, "MCLA",
                     "MCLA: write the guest's render markers (no-op PM4 packets 'REXM' at the 3D scene's start, "
                     "its end and the tone map) into the GPU command stream, which the SDK's FSR 3.1 temporal AA / "
                     "upscaling and its tonemap_transfer_skip need to find the scene and the tone map. 1 = on, "
                     "0 = off, -1 = auto: on only while temporal_aa is 'fsr', tonemap_transfer_skip is on or gpu_timing >= 1 "
                     "(default; inert otherwise). Draws nothing. Read once per frame");

namespace {

// The SDK's GuestMarkerKind (include/rex/graphics/command_processor.h).
enum MarkerKind : uint32_t {
  kFrameBegin = 1,
  kFrameEnd = 2,
  kSceneEnd = 3,
  kTonemapBegin = 4,
  kUIBegin = 5,
  kRenderEnd = 6,
  kSceneBegin = 7,
  kSceneTranslucent = 8,
  kSceneComposite = 9,
  kPhase = 0x100,  // value = the renderer phase bit (LOCK60: written for every phase while the SDK's gpu_timing is on)
};

// The renderer's phase bits that mean something to the markers (frame-structure notes).
constexpr uint32_t kPhaseScene = 0x800;
constexpr uint32_t kPhaseTranslucent = 0x100000;
constexpr uint32_t kPhaseComposite = 0x04000000;
constexpr uint32_t kPhasePost = 0x08000000;
constexpr uint32_t kPhaseTonemap = 0x10000000;
constexpr uint32_t kPhaseUI = 0x20000000;

// The renderer's phase enable mask: the word the phase loop ANDs with [renderer+364] (0x822E6608); see the sub_822E6408 override.
constexpr uint32_t kRendererPhaseEnableMask = 448;
// The draw-type enable mask the game ORs the shadow types into every frame (sub_822F3BD8, called from sub_822C0980 at
// 0x822C0A08, before the phase loop at 0x822C0B08); the loop runs phase bits in this AND the word above.
constexpr uint32_t kRendererDrawTypeMask = 364;
// The loop's phases are the bits 0..29 (1 .. 0x20000000); the per-phase object of phase bit 1 << k is the word renderer[k].
constexpr uint32_t kPhaseCount = 30;

constexpr uint32_t kD3DDevicePointer = 0x82839254;
constexpr uint32_t kDeviceWritePointer = 48;
constexpr uint32_t kDeviceWriteLimit = 56;
constexpr uint32_t kNopHeader = 0xC0021000;  // PM4 type 3, opcode NOP (0x10), 3 data words
constexpr uint32_t kMagic = 0x5245584D;      // 'REXM'

// LOCK60: the SDK's per-phase GPU timing (patch 04f0, cvar gpu_timing >= 1) cuts the GPU frame at the render markers, and
// at a kPhase marker (value = the phase bit) written at the start of EVERY phase the loop runs. 0 on an SDK without the cvar.
bool PhaseMarkersWanted() { return rex::cvar::Query<int32_t>("gpu_timing") > 0; }

bool MarkersWanted() {
  const int32_t mode = REXCVAR_GET(mcla_render_markers);
  if (mode > 0) {
    return true;
  }
  if (mode == 0) {
    return false;
  }
  return rex::cvar::GetFlagByName("temporal_aa") == "fsr" || rex::cvar::Query<bool>("tonemap_transfer_skip") ||
         PhaseMarkersWanted();
}

mcla::hooks::Registration g_registration("fsr-markers", "mcla_render_markers", MCLA_HOOK_FSR_MARKERS != 0,
                                         [] { return MarkersWanted(); },
                                         /*default_on=*/false);

}  // namespace

#if MCLA_HOOK_FSR_MARKERS

// The originals (strong `__imp__` symbols of the generated code) and the D3D make-room function.
REX_EXTERN(__imp__sub_822C0980);
REX_EXTERN(__imp__sub_822E6408);
REX_EXTERN(__imp__sub_822E55D0);
REX_EXTERN(sub_82412710);

namespace {

std::atomic<bool> g_on{false};               // MarkersWanted() at the start of the current frame
std::atomic<bool> g_phase_on{false};         // LOCK60: PhaseMarkersWanted() at the start of the current frame
std::atomic<uintptr_t> g_render_thread{0};   // the thread that last ran the frame function
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_made_room{0};
std::atomic<uint32_t> g_dropped{0};
std::atomic<bool> g_logged_scene{false};

// A value unique to the calling thread for as long as it lives.
uintptr_t ThisThread() {
  static thread_local char tag;
  return reinterpret_cast<uintptr_t>(&tag);
}

// Writes one marker into the device's command buffer when the markers are on and this is the render thread.
void EmitMarker(PPCContext& ctx, uint8_t* base, uint32_t kind, uint32_t value) {
  if (!g_on.load(std::memory_order_relaxed) || g_render_thread.load(std::memory_order_relaxed) != ThisThread()) {
    return;
  }
  const uint32_t device = REX_LOAD_U32(kD3DDevicePointer);
  uint32_t put = device >= 0x10000 ? REX_LOAD_U32(device + kDeviceWritePointer) : 0;
  if (put >= 0x10000 && put > REX_LOAD_U32(device + kDeviceWriteLimit)) {
    // Make room as the game's own writers do; the caller's registers are kept.
    PPCContext saved = ctx;
    ctx.r3.u64 = device;
    sub_82412710(ctx, base);
    put = ctx.r3.u32;
    ctx = saved;
    // The context's FPSCR is only a cache of the host's MXCSR: the call may have changed the flush-to-zero mode
    // (sub_82412ED0 turns it off), and the copy put back above no longer says what MXCSR holds, so the recompiled
    // code would skip setting it. Put MXCSR back to the caller's rounding and flush modes too.
    ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
    // Logged at 1, 2, 4, 8... times (the count is read once: `++x & (x - 1)` in one expression is unsequenced).
    const uint32_t made_room = g_made_room.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!(made_room & (made_room - 1))) {
      REXLOG_INFO("[mcla-fsr] render markers: made room in the command buffer {} times", made_room);
    }
  }
  if (put < 0x10000) {
    const uint32_t dropped = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!(dropped & (dropped - 1))) {
      REXLOG_WARN("[mcla-fsr] render markers: {} skipped (no command buffer)", dropped);
    }
    return;
  }
  REX_STORE_U32(put + 4, kNopHeader);
  REX_STORE_U32(put + 8, kMagic);
  REX_STORE_U32(put + 12, kind);
  REX_STORE_U32(put + 16, value);
  REX_STORE_U32(device + kDeviceWritePointer, put + 16);
  if (kind == kSceneBegin && !g_logged_scene.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO(
        "[mcla-fsr] render markers: scene begin/end at the start of renderer phase 0x{:08X} / 0x{:08X} (tone map "
        "0x{:08X}): a 3-word PM4 NOP 'REXM' is written into the command buffer of the D3D device 0x{:08X} before "
        "sub_822E55D0 runs each phase, on the thread that runs the frame function sub_822C0980 only "
        "(mcla_render_markers={}, temporal_aa={}, tonemap_transfer_skip={})",
        kPhaseScene, kPhasePost, kPhaseTonemap, device, REXCVAR_GET(mcla_render_markers),
        rex::cvar::GetFlagByName("temporal_aa"), rex::cvar::GetFlagByName("tonemap_transfer_skip"));
  }
}

}  // namespace

// The frame's rendering: the thread that calls it is the one that draws.
extern "C" REX_FUNC(sub_822C0980) {
  MCLA_HOOK_ENTER();
  g_on.store(MarkersWanted(), std::memory_order_relaxed);
  g_phase_on.store(PhaseMarkersWanted(), std::memory_order_relaxed);
  g_render_thread.store(ThisThread(), std::memory_order_relaxed);
  const uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  EmitMarker(ctx, base, kFrameBegin, frame);
  __imp__sub_822C0980(ctx, base);
  EmitMarker(ctx, base, kFrameEnd, frame);
}

// The render phase loop: what is drawn after it in the frame (Bink movies) is over the finished picture.
//
// With MCLA_HOOK_GFX_PHASES the same override also lets the loop leave phases out of a frame (mcla_gfx_env_interval,
// mcla_gfx_reflection_interval, mcla_gfx_far_cascades_interval: mcla_gfx_tune.cpp, PhasesToSkip(), which also explains the
// phase bits). A phase has TWO parts in the loop, and both must be left out:
//   1. The 13 draw-list objects at renderer+128.. run only for a phase in [renderer+364] & [renderer+448] (reads at
//      0x822E6624 / 0x822E6608, once, before the first phase; the test is `and r10,r30,r21; beq` at 0x822E6844). [renderer+448]
//      is the phase enable mask (-1 from the constructor) and nothing else reads or writes it while a frame is drawn, so the
//      bits of the phases left out are cleared here, before the original runs, and OR-ed back after it.
//   2. The per-phase object renderer[k], k = the bit's index (renderer+4k, registered by sub_822E50D8 from 0x823048D0: the env
//      maps 0x4 | 0x8 are the object of sub_823064C0, the shadow cascades 0x20.. the shadow system of sub_823109E8), has its
//      virtual +4 called at the start of EVERY phase (0x822E6800) and its virtual +8 at the end (0x822E68E4), mask or not. Those
//      set the phase's render target, clear it and resolve it into the phase's texture (sub_82306500 tests the phase 4 and 8
//      itself). With only the mask cleared a "skipped" phase still ran begin and end: it cleared its target, drew nothing and
//      RESOLVED THE EMPTY TARGET over the env map / the shadow atlas quarter (R-flicker, scratch/ORC: the env resolves were in
//      726 of 731 frames at mcla_gfx_env_interval=2). So the pointers renderer[k] of the skipped phases are zeroed around the
//      call (the loop tests `cmplwi r11,0` before each virtual call) and put back after it; a phase that is skipped does
//      nothing at all, and its texture / atlas quarter keeps what the last drawn frame resolved.
// The game never sees a changed mask or pointer outside this call. Only the bits that were set and that this call cleared are
// put back, and a pointer only while its slot still holds zero. The call is made on the render thread; with every interval at 1
// nothing is read or written (PhasesToSkip() is 0). The far cascades are only left out after a frame they were active in, so
// PhasesToSkip() is told which phases would run: [renderer+364] & [renderer+448].
extern "C" REX_FUNC(sub_822E6408) {
#if MCLA_HOOK_GFX_PHASES
  const uint32_t renderer = ctx.r3.u32;  // the original takes the renderer in r3 and uses the register
  uint32_t active = 0;
  if (mcla::gfxtune::PhasesActive() && renderer >= 0x10000) {
    active = REX_LOAD_U32(renderer + kRendererPhaseEnableMask) & REX_LOAD_U32(renderer + kRendererDrawTypeMask);
  }
  const uint32_t skip = mcla::gfxtune::PhasesToSkip(active);
  uint32_t left_out = 0;
  uint32_t nulled = 0;                // bit k set: renderer[k] was zeroed
  uint32_t saved[kPhaseCount] = {};   // the pointers that were there
  if (skip && renderer >= 0x10000) {
    const uint32_t mask = REX_LOAD_U32(renderer + kRendererPhaseEnableMask);
    left_out = mask & skip;
    if (left_out) {
      REX_STORE_U32(renderer + kRendererPhaseEnableMask, mask & ~left_out);
      for (uint32_t bits = left_out; bits; bits &= bits - 1) {
        const uint32_t k = uint32_t(__builtin_ctz(bits));
        if (k >= kPhaseCount) {
          continue;
        }
        const uint32_t object = REX_LOAD_U32(renderer + 4 * k);
        if (object != 0) {
          saved[k] = object;
          nulled |= 1u << k;
          REX_STORE_U32(renderer + 4 * k, 0);
        }
      }
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true, std::memory_order_relaxed)) {
        REXLOG_INFO("[mcla-gfx] phases: first frame with phases left out: mask bits 0x{:X}, per-phase objects zeroed for 0x{:X} "
                    "(renderer 0x{:08X}; phase 2 -> 0x{:08X}, 3 -> 0x{:08X}, 7 -> 0x{:08X}, 8 -> 0x{:08X})",
                    left_out, nulled, renderer, saved[2], saved[3], saved[7], saved[8]);
      }
    }
  }
#endif
  __imp__sub_822E6408(ctx, base);
#if MCLA_HOOK_GFX_PHASES
  if (left_out) {
    REX_STORE_U32(renderer + kRendererPhaseEnableMask, REX_LOAD_U32(renderer + kRendererPhaseEnableMask) | left_out);
    for (uint32_t bits = nulled; bits; bits &= bits - 1) {
      const uint32_t k = uint32_t(__builtin_ctz(bits));
      if (REX_LOAD_U32(renderer + 4 * k) == 0) {
        REX_STORE_U32(renderer + 4 * k, saved[k]);
      }
    }
  }
#endif
  EmitMarker(ctx, base, kRenderEnd, g_frame.load(std::memory_order_relaxed));
}

// The start of a render phase (r3 = the renderer, r4 = the phase bit). The marker goes in before the original runs, so
// it falls between the previous phase's draws and this phase's; the ws_ui probe inside the original then runs as usual.
extern "C" REX_FUNC(sub_822E55D0) {
  if (g_on.load(std::memory_order_relaxed)) {
    const uint32_t phase = ctx.r4.u32;
    if (g_phase_on.load(std::memory_order_relaxed)) {
      EmitMarker(ctx, base, kPhase, phase);  // LOCK60: before the named marker of the same phase (same cut point)
    }
    switch (phase) {
      case kPhaseScene:
        EmitMarker(ctx, base, kSceneBegin, phase);
        break;
      case kPhaseTranslucent:
        EmitMarker(ctx, base, kSceneTranslucent, phase);
        break;
      case kPhaseComposite:
        EmitMarker(ctx, base, kSceneComposite, phase);
        break;
      case kPhasePost:
        EmitMarker(ctx, base, kSceneEnd, phase);
        break;
      case kPhaseTonemap:
        EmitMarker(ctx, base, kTonemapBegin, phase);
        break;
      case kPhaseUI:
        EmitMarker(ctx, base, kUIBegin, phase);
        break;
      default:
        break;
    }
  }
  __imp__sub_822E55D0(ctx, base);
}

#endif  // MCLA_HOOK_FSR_MARKERS
