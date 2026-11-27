// Midnight Club: Los Angeles: the game's own quality knobs (docs/mcla/NATIVE_RENDERER.md section 22,
// ADDRESSES.md "Quality knobs"). Not a mid-asm hook: always compiled, called once per engine frame from
// the heartbeat hook (mcla_heartbeat.cpp, the game thread).
//
// The knobs are plain variables of the image's .data and of live heap objects (shadow map, culling
// views), written each frame, so they work with --set-at and need no re-codegen. Every knob restores
// the game's own value when it goes back to its default. mcla_gfx_poke is the generic form (any
// address) used to find them.
//
// mcla_gfx_preset (perf, perf+, lowest, quality, ultra) is a bundle of these knobs, applied once, at first use, as app defaults
// (rex::cvar::SetDefaultValue): a knob set by hand (command line, config, console) keeps its value. perf, perf+ and lowest are
// for a weak GPU; quality (the shipped detail plus FXAA) and ultra (quality with 1.5x distances) are not built on perf. See
// mcla_gfx_tune.cpp.
#pragma once

#include <cstdint>

namespace mcla::gfxtune {

// Applies mcla_gfx_preset once (idempotent, thread-safe; logs '[mcla-gfx] preset <name>: ...'). OnFrame, the accessors below
// and the hooks' announce lines call it, so a knob read at any time already carries the preset.
void EnsurePreset();

// The knobs of a preset, for the overlay's live preset: calls fn(cvar, value, user) for each knob the preset sets (the names of
// mcla_gfx_preset; "default" / "" = as shipped: quality's list with swap_post_effect "none"). False, with nothing called, for a
// name this build does not know. Nothing is written here: the caller decides how (the real preset writes app defaults).
bool ForEachPresetKnob(const char* preset, void (*fn)(const char* cvar, const char* value, void* user), void* user);

// Applies the cvars (named knobs and mcla_gfx_poke) and, when asked, logs the dump. Cheap when
// nothing is set: a few cvar reads.
void OnFrame();

// The phase bits the renderer's phase loop (sub_822E6408) leaves out of THIS frame: the environment-map phases
// (0x4 | 0x8) on the frames mcla_gfx_env_interval skips, the reflection phase (0x2) on the frames
// mcla_gfx_reflection_interval skips, and the far shadow cascade 2 (0x80; cascade 3 is never skipped) on alternate frames with
// mcla_gfx_far_cascades_interval. `active_phases` = [renderer+364] & [renderer+448], the phases that would run (a far cascade
// is only left out after a frame it was drawn in). 0 when every interval is 1 (as shipped). Called once per frame on the
// render thread at the start of the phase loop (mcla_fsr_markers.cpp); only relaxed atomics are read.
uint32_t PhasesToSkip(uint32_t active_phases);

// True while any of the three phase intervals is above 1 (the override reads the renderer's masks only then).
bool PhasesActive();

// The far-cascade bits (0x80 / 0x100) PhasesToSkip() left out of the current frame: the shadow constant setter override
// (mcla_gfx_far_cascades.cpp) holds those cascades' light matrices. 0 as shipped.
uint32_t FarCascadePhasesSkipped();

// mcla_gfx_far_cascades_interval (1 = as shipped, 2 = cascade 2 drawn on alternate frames), clamped to 1..8.
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

// ---- the run-time detail tier (mcla_gfx_tier), for the frame-rate governor (LOCK60) --------------------------------------------
//
// The governor's contract:
//   * It sets the tier with rex::cvar::SetFlagByName("mcla_gfx_tier", "N") (N = kTierMin..kTierMax; -1 = off) from any thread.
//     mcla_gfx_tier is a transient cvar (never written to the config file, the command line still sets it). OnFrame() applies it on
//     the next engine frame: a tier sets the DEFAULT (rex::cvar::SetDefaultValue) of the live knob cvars (mcla_gfx_env_interval,
//     far_cascades_interval, shadow_dist, city_lod, traffic_lod, shadow_mode) and swap_post_effect, and logs one '[mcla-gfx] tier N:
//     ...' line ('(yours)' after a knob the tier left alone, see below). CurrentTier() is the tier OnFrame has applied (-1 = off);
//     read it back instead of assuming a SetFlagByName took effect.
//   * Higher = more detail. The governor sheds by stepping DOWN and restores by stepping UP, ONE tier at a time, with hysteresis
//     (est_gpu_ms_over_prev is the budget each step gives or takes). It never goes below tier 1 except in an emergency (tier 0 drops
//     every sun shadow at once).
//   * A tier does not touch mcla_gfx_shadow_res: that is read when the shadow map is created (a world load), so the shadow map size is
//     whatever the preset or the knob gave at the load. mcla_gfx_dl_disable, impostors and the other knobs are not in the table either.
//   * The tier never makes a knob cvar 'runtime': a default set with SetDefaultValue keeps the cvar's source at kDefault, and a
//     config save (rex::cvar::SaveConfig) writes only cvars whose source is kRuntime, so a tier can never end up in the saved config.
//   * The player's own choice wins: a knob whose source is not kDefault (set on the command line, by the launcher, in the config file,
//     in the settings overlay or the console) is never written by a tier; it keeps the player's value in every tier. A knob the player
//     sets WHILE a tier is on is skipped from the next tier change on (the tier's default stays under it).
//   * At -1 each knob's default goes back (SetDefaultValue again) to what it was before the tier first wrote it: the preset's app
//     default (mcla_gfx_preset) or the compiled default. A knob the player has set since keeps the player's value; only its default moves.
constexpr int kTierMin = 0, kTierMax = 5;

struct TierInfo {
  int tier;                      // kTierMin..kTierMax
  const char* name;
  float est_gpu_ms_over_prev;    // GPU ms per frame over the tier below (tier 1 = the baseline, 0; tier 0 is given against tier 1,
                                 // so negative). Measured at 1080p on the Radeon 880M, parked scene, noise about +-0.3 ms, except tier 0
  bool runtime_pop_free;         // false = the switch shows (a pop or a sudden change); switch such a tier at a camera cut or at speed
  const char* note;
};

// The six rows, tier 0 first. `count` (may be null) gets 6. The array is static and lives for the process.
const TierInfo* Tiers(int* count);

// The tier OnFrame has applied: -1 = off (the knob cvars rule), else kTierMin..kTierMax. Any thread.
int CurrentTier();

}  // namespace mcla::gfxtune
