// Midnight Club: Los Angeles: the game's own quality knobs (docs/mcla/NATIVE_RENDERER.md section 22,
// ADDRESSES.md "Quality knobs"). Not a mid-asm hook: always compiled, called once per engine frame from
// the heartbeat hook (mcla_heartbeat.cpp, the game thread).
//
// The knobs are plain variables of the image's .data and of live heap objects (shadow map, culling
// views), written each frame, so they work with --set-at and need no re-codegen. Every knob restores
// the game's own value when it goes back to its default. mcla_gfx_poke is the generic form (any
// address) used to find them.
//
// mcla_gfx_preset (perf, perf+, lowest) is a bundle of these knobs, applied once, at first use, as app defaults
// (rex::cvar::SetDefaultValue): a knob set by hand (command line, config, console) keeps its value. See
// mcla_gfx_tune.cpp.
#pragma once

#include <cstdint>

namespace mcla::gfxtune {

// Applies mcla_gfx_preset once (idempotent, thread-safe; logs '[mcla-gfx] preset <name>: ...'). OnFrame, the accessors below
// and the hooks' announce lines call it, so a knob read at any time already carries the preset.
void EnsurePreset();

// Applies the cvars (named knobs and mcla_gfx_poke) and, when asked, logs the dump. Cheap when
// nothing is set: a few cvar reads.
void OnFrame();

// The phase bits the renderer's phase loop (sub_822E6408) leaves out of THIS frame: the environment-map phases
// (0x4 | 0x8) on the frames mcla_gfx_env_interval skips, the reflection phase (0x2) on the frames
// mcla_gfx_reflection_interval skips, and the far shadow cascades 2 and 3 (0x80 / 0x100) on alternate frames with
// mcla_gfx_far_cascades_interval. `active_phases` = [renderer+364] & [renderer+448], the phases that would run (a far cascade
// is only left out after a frame it was drawn in). 0 when every interval is 1 (as shipped). Called once per frame on the
// render thread at the start of the phase loop (mcla_fsr_markers.cpp); only relaxed atomics are read.
uint32_t PhasesToSkip(uint32_t active_phases);

// True while any of the three phase intervals is above 1 (the override reads the renderer's masks only then).
bool PhasesActive();

// The far-cascade bits (0x80 / 0x100) PhasesToSkip() left out of the current frame: the shadow constant setter override
// (mcla_gfx_far_cascades.cpp) holds those cascades' light matrices. 0 as shipped.
uint32_t FarCascadePhasesSkipped();

// mcla_gfx_far_cascades_interval (1 = as shipped, 2 = cascades 2 and 3 drawn on alternate frames), clamped to 1..8.
uint32_t FarCascadesInterval();

// True with mcla_gfx_far_cascades_interval=0: as shipped, plus a one-time log of the four cascade light matrices (to check
// the index order: the farthest cascade has the smallest scale).
bool FarCascadesDump();

// Whether the shadow atlas render target gets its own memory instead of the shared render-target pool (mcla_gfx_shadow_private;
// auto = on while the far cascades are throttled: a cascade that is not redrawn keeps its resolved depth in memory that no
// other pass may reuse). Asked when the shadow map's render target is created.
bool ShadowAtlasPrivate();

// mcla_gfx_impostor_interval: 1 = as shipped, N >= 2 = a tree-impostor type is regenerated at most every N-th frame,
// 0 = as shipped plus a statistics line (mcla_gfx_impostors.cpp). Clamped to 0..8.
uint32_t ImpostorInterval();

// The edge, in texels, of the sun-shadow map the CShadowMap constructor sub_825F6940 stores into its width and
// height fields: 640 as shipped, 512 or 384 with mcla_gfx_shadow_res (or a preset). Called by the shadow-res hook
// (mcla_gfx_shadow_res.cpp) once per shadow map created.
uint32_t ShadowMapSize();

// False when the tree-impostor system is switched off (mcla_gfx_impostors=false): the overrides of
// mcla_gfx_impostors.cpp read it on every call.
bool ImpostorsEnabled();

}  // namespace mcla::gfxtune
