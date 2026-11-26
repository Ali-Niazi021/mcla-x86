// Midnight Club: Los Angeles: the GPU frame governor (LOCK60 A5). Closed loop on the GPU's own frame time, exported
// by the SDK (patch 04f0: rexgpu-xenos.dll data symbols rexgpu_gpu_frame_us / rexgpu_gpu_frame_seq), with the
// missed-frames signal of the 60 fps pacer as the fallback when the symbols are missing. It sheds load quickly when the
// GPU frame nears the budget and restores it slowly, through a table of levels ordered by visibility: variable rate
// shading first (patch 0500: rexgpu_vrs_level), then the game's own run-time knobs (mcla_gfx_*); the 60 -> 30 hold of
// mcla_t_common.cpp stays the last resort and is only allowed once the governor is at its last level (HoldAllowed()).
// Level 0 is the configured picture (the preset); positive levels shed; negative levels (richer than the preset) are
// reserved for the quality tiers above `perf`.
//
// Not a hook: always compiled, called once per engine frame on the game thread from mcla::sim::OnFrameStart.
#pragma once

#include <cstdint>

namespace mcla::gov {

// Once per engine frame (game thread). `now_ns` = steady clock; `last_n` = the world step n of the previous frame
// (1 = on time at 60, >= 2 = missed; 0 = unknown); `driving` = the player is driving (not a menu, pause or load).
//
// Call-site contract:
//  * `last_n >= 2` is a MISSED frame (it sheds when the GPU is within 1 ms of the budget, blocks every restore for the quiet
//    time, and with no GPU signal is the only signal). While the 60 -> 30 hold (or a 30 cap) is in force n = 2 is the
//    pacer's own on-time step, so pass 0 (unknown) then, or the governor reads every frame as missed.
//  * `driving` is false in menus, pauses, loads and cutscenes: the governor still sheds there but never restores, and its
//    restore timer starts over when the flag changes.
//  * Cheap when mcla_gov is off (one cvar read; when it was on the frame before, every knob it changed is put back first).
//    Call it every engine frame, also while loading: a gap longer than a second counts as "no GPU signal" until the next
//    published sample.
//  * Writes cvars (rex::cvar::SetFlagByName / ResetToDefault) and rexgpu_vrs_level: nothing else.
void OnFrame(int64_t now_ns, int last_n, bool driving);

// True when the 60 -> 30 hold may start: the governor is off, or it is at its last shed level (the table's, or the cap
// mcla_gov_max_level) and the last GPU sample is still above mcla_gov_shed_ms (with no GPU signal: at the last level
// alone). As of the last OnFrame. Not a trigger: the pacer's own window rule still decides whether to hold.
bool HoldAllowed();

// The level in force (0 = the configured picture; > 0 shed; < 0 only with mcla_gov_allow_rich).
int Level();

// The governor's master switch (cvar mcla_gov).
bool Enabled();

}  // namespace mcla::gov
