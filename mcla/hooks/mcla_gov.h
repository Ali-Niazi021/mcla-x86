// Midnight Club: Los Angeles: the GPU frame governor (LOCK60 A5 / F1). Closed loop on the GPU's own frame time, exported by the SDK
// (patch 04f0: rexgpu-xenos.dll data symbols rexgpu_gpu_frame_us / rexgpu_gpu_frame_seq), with the missed-frames signal of the
// 60 fps pacer as the fallback when the symbols are missing. It owns two actuators and moves them on ONE two-way ladder:
//
//   * the run-time detail tier of mcla_gfx_tune (cvar mcla_gfx_tier, 1 = the perf preset ... 5 = 1.5x distances), written with
//     rex::cvar::SetFlagByName and read back through mcla::gfxtune::CurrentTier();
//   * variable-rate shading (SDK patch 0500: rexgpu_vrs_level, 0 = full rate ... rexgpu_vrs_levels).
//
// The state is (tier t, vrs v). F = the floor (the configured tier: mcla_gov_tier_floor, or with -1 the tier mcla_gfx_tier had when the
// governor first ran if that was >= 1, else that of the preset mcla_gfx_preset: perf = 2 (perf + FXAA), perf+ / lowest = 1, quality and
// as shipped = 4, ultra = 5), C = the ceiling (mcla_gov_tier_ceiling, default 4; C <= F = no enrichment). It starts at (F, 0), the base.
//
//   SHED ORDER  (fast: the GPU busy time is over the line (median of the last 4 samples, or 3 of the last 30 over it: the tail rule),
//      or 2 of the last 30 engine frames were missed):
//        enriched tiers (C .. F+1)  ->  VRS 1 .. V  ->  tiers F-1 .. 1  ->  hold
//      the last level is (1, V); there, and only there, and only while the GPU is still over budget, HoldAllowed() lets the 60 -> 30 hold
//      of mcla_t_common.cpp start.
//   RESTORE ORDER (slow: below the restore line for mcla_gov_restore_s, with bounce back-off): the reverse, tiers up to F, then VRS
//      back to 0.
//   ENRICH (slower still): from (t >= F, 0) to (t + 1, 0) up to the ceiling, when over mcla_gov_enrich_s of driving with no missed
//      frame the p90 of the busy time plus the predicted cost of the tier (the table's estimate, or what the governor learned from
//      measuring earlier steps, when that is more) plus mcla_gov_enrich_margin_ms fits under mcla_gov_restore_ms. A tier that was
//      entered by enrichment and is shed within mcla_gov_enrich_hold_s is barred for 30 s, doubling (cap 600 s) with every repeat and
//      decaying while it stays quiet.
//   COMPILE BURSTS (SDK patch 0590 exports): while pipelines / shaders are being created at run time the frames are late for the
//      compiler's sake: the governor does not feed them into its shed rules and the 60 -> 30 judge does not judge the windows they
//      are in, both bounded (mcla_compile_excuse_s) so that a real overload still acts. See CompileBurstActive() below.
//   POP HIDING: a step across a tier that pops when switched (3 and 5) is only taken in a hiding moment: a pause, a load or a menu (the
//      driving flag is false), the first second after one, a camera cut, or camera speed above mcla_gov_pop_speed_kmh. Enrich and
//      restore wait for one without a limit; a shed waits at most mcla_gov_pop_wait_s and goes at once on a missed frame.
//      The camera's cut and speed need NoteCamera(); with no feed only the driving flag hides (see NoteCamera).
//
// The full law, with every number, is in mcla_gov_math.h (pure, unit-tested in mcla/tests/test_gov.cpp).
//
// Not a hook: always compiled, called once per engine frame on the game thread from mcla::sim::OnFrameStart.
#pragma once

#include <cstdint>

namespace mcla::gov {

// Once per engine frame (game thread). `now_ns` = steady clock; `last_n` = the world step n of the previous frame
// (1 = on time at 60, >= 2 = missed; 0 = unknown); `driving` = the player is driving (not a menu, pause or load).
//
// Call-site contract:
//  * `last_n >= 2` is a MISSED frame (two of them within 30 engine frames shed, one blocks every restore and enrichment for the
//    quiet time, and with no GPU signal they are the only signal). While the 60 -> 30 hold (or a 30 cap) is in force n = 2 is the pacer's
//    own on-time step, so pass 0 (unknown) then, or the governor reads every frame as missed.
//  * `driving` is false in menus, pauses, loads and cutscenes: the governor holds its state there (it only takes a step that was
//    already waiting for a hiding moment) and its timers start over when the flag changes.
//  * Cheap when mcla_gov is off (one cvar read; when it was on the frame before, mcla_gfx_tier and rexgpu_vrs_level are put back first).
//    Call it every engine frame, also while loading: a gap longer than a second counts as "no GPU signal" until the next published
//    sample.
//  * Writes cvars (rex::cvar::SetFlagByName / ResetToDefault: mcla_gfx_tier only) and rexgpu_vrs_level: nothing else. While mcla_gov
//    is on the governor owns mcla_gfx_tier: someone else's write is logged and overwritten (set mcla_gov_tier_floor / _ceiling to
//    choose the tier range instead).
void OnFrame(int64_t now_ns, int last_n, bool driving);

// True when the 60 -> 30 hold may start: the governor is off, or it is at its last shed level (tier 1 with VRS at its maximum, or the cap
// mcla_gov_max_level) and the last GPU sample is still above mcla_gov_shed_ms (with no GPU signal: at the last level alone). As of the
// last OnFrame. Not a trigger: the pacer's own window rule still decides whether to hold.
bool HoldAllowed();

// The rung in force: 0 = the base (F, 0), > 0 shed (VRS, then tiers below F), < 0 enriched (tiers above F).
int Level();
// The tier and the VRS level the governor holds (0 / 0 while it is off).
int Tier();
int Vrs();

// The governor's master switch (cvar mcla_gov).
bool Enabled();

// ---- compile bursts ---------------------------------------------------------------------------------------------------
//
// ONE source of truth for the burst state: OnFrame computes it once per engine frame (math::CompileWatch over the SDK's
// rexgpu_pipeline_creations / rexgpu_shader_translations / rexgpu_pipelines_pending, with mcla_compile_quiet_ms) EVEN WITH mcla_gov
// OFF, and both consumers read it: the governor's own Step (Input::compile_burst: the bound is the sliding-window ExcuseBudget of
// mcla_gov_math.h, mcla_compile_excuse_s per 20 s) and the 60 -> 30 judge of mcla_t_common.cpp (HoldJudge::Tick(..., compile): a decision
// window a burst overlapped is not judged, at most CompileExcuseS() / window seconds' worth of windows in a row). The monotonic
// total of what the governor excused is math::Controller::CompileExcusedTotalNs().
//
// True while a burst is active as of the last OnFrame. False with an SDK that lacks the symbols (no compile signal: nothing is excused).
bool CompileBurstActive();
// mcla_compile_excuse_s clamped to 0..20: how much time a burst may be excused, in seconds (0 = never). The judge derives its bound
// from it (excuse / decision window = whole windows in a row).
double CompileExcuseS();

// ---- the camera feed (pop hiding by cut and speed) --------------------------------------------------------------------
//
// TODO(camera feed): nothing calls this yet. Neither the chase-camera hooks (mcla_t_camera_pos.cpp / _lookat.cpp: they only scale the
// smoothing factor) nor any other hook exposes the camera's world position, and the guest address of the camera object has not been
// found. The first caller that has one calls NoteCamera once per engine frame, from the game thread (any thread works: atomics), with
// the position in world units (metres) of the camera, or, failing that, of the player's car. The governor then derives the two hiding
// sources by itself: a jump of more than mcla_gov_cut_m in one frame is a camera cut, and the distance over the real time between
// frames (smoothed over 0.2 s) is the speed in km/h. Until then only the driving flag hides a popping step.
void NoteCamera(double x, double y, double z);
// True once NoteCamera has been called.
bool CameraFeedActive();

// ---- leaving: config save and exit -------------------------------------------------------------------------------------
//
// The governor writes mcla_gfx_tier and, through it (mcla_gfx_tune.cpp, ApplyTier), seven knob cvars with SetFlagByName: their source
// is "runtime", which is exactly what rex::cvar::SaveConfig merges into the TOML. The one caller of SaveConfig is the SDK's settings
// overlay ("Save to config", SDK ui/overlay/settings_overlay.cpp); nothing in src/host calls it and the SDK has no pre-save hook.
//
// BeforeConfigSave(): call it right before rex::cvar::SaveConfig / SaveConfigValues, from any thread. It puts mcla_gfx_tier back to what it
// was (-1, or the tier the user configured), waits up to 250 ms for the game thread to apply that (mcla_gfx_tune restores the knob
// cvars on its next frame), and keeps the governor from writing the tier again for 2 s so the save sees the original values; the
// governor then writes its tier back. Nothing calls it today: it needs an SDK patch (a pre-save callback in cvar.cpp's SaveConfig /
// SaveConfigValues, or a call in settings_overlay.cpp) and one line in McLaApp::OnPreSetup to register it.
// The GPU plugin's counters, read now (SDK exports; zero / false where a symbol is missing): cumulative pipelines created
// and shaders translated at run time, pipelines in flight (0590), and the GPU busy time of the last frame read back
// (04f0/0510, 2-4 frames late). For the hitch lines (mcla_hitch.cpp); resolved once, on the first call.
struct GpuCounters {
  bool compile = false;
  uint32_t pipelines = 0;
  uint32_t shaders = 0;
  int32_t pending = 0;
  bool gpu = false;
  uint32_t gpu_us = 0;
};
GpuCounters ReadGpuCounters();

void BeforeConfigSave();
// Process end: mcla_gfx_tier and rexgpu_vrs_level back to what they were, and the governor stays quiet. Registered with std::atexit
// when the governor first runs; the window-close path of the SDK (ReXApp::OnClosing) leaves through std::_Exit, which runs no atexit
// handler, but then nothing is saved either. The host may also call it from McLaApp::OnShutdown.
void Shutdown();

}  // namespace mcla::gov
