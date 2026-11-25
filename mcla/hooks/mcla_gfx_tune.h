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
// mcla_gfx_reflection_interval skips. 0 when both intervals are 1 (as shipped). Called once per frame on the render
// thread at the start of the phase loop (mcla_fsr_markers.cpp); only relaxed atomics are read.
uint32_t PhasesToSkip();

// The edge, in texels, of the sun-shadow map the CShadowMap constructor sub_825F6940 stores into its width and
// height fields: 640 as shipped, 512 or 384 with mcla_gfx_shadow_res (or a preset). Called by the shadow-res hook
// (mcla_gfx_shadow_res.cpp) once per shadow map created.
uint32_t ShadowMapSize();

// False when the tree-impostor system is switched off (mcla_gfx_impostors=false): the overrides of
// mcla_gfx_impostors.cpp read it on every call.
bool ImpostorsEnabled();

}  // namespace mcla::gfxtune
