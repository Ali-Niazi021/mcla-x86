// Midnight Club: Los Angeles: the game's own quality knobs (docs/mcla/NATIVE_RENDERER.md section 22,
// ADDRESSES.md "Quality knobs"). Not a mid-asm hook: always compiled, called once per engine frame from
// the heartbeat hook (mcla_heartbeat.cpp, the game thread).
//
// The knobs are plain variables of the image's .data and of live heap objects (shadow map, culling
// views), written each frame, so they work with --set-at and need no re-codegen. Every knob restores
// the game's own value when it goes back to its default. mcla_gfx_poke is the generic form (any
// address) used to find them.
#pragma once

#include <cstdint>

namespace mcla::gfxtune {

// Applies the cvars (named knobs and mcla_gfx_poke) and, when asked, logs the dump. Cheap when
// nothing is set: a few cvar reads.
void OnFrame();

}  // namespace mcla::gfxtune
