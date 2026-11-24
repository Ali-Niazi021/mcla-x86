// The PC cvar profile (see pc_profile.h). One table per phase; each entry says what it is for and where the value
// comes from. "W0.4" = the research report $WORK/scratch/W0.4/REPORT.md section 1 (the patch lines there are
// the REXCVAR_DEFINE of the cvar). Left at the SDK default on purpose, so nobody ports them by mistake:
// vsync (false makes the guest vblank fire at ~1 kHz), readback_resolve (stays `none`: the vinyl rule below is
// the only readback), clear_memory_page_state, resolution_scale, hdr.
//
// Named presets (W5.1): `--mcla_profile=1080p` or `900p` (cvar below; "" / "default" = the table below, unchanged)
// adds a second table on top of the first. Same mechanism, so the command line, the environment and the TOML file
// still win over every entry, and an entry that names a cvar of the base table replaces its value.
//
// Frame-rate target (W4.1): `--mcla_fps_target=60` (cvar below; 0 = the shipped timing, nothing is set) is the whole 60 fps
// profile in one switch. It is a separate axis from the resolution presets: `--mcla_profile=1080p --mcla_fps_target=60`
// is M6. It is applied in phase 0 (OnConfigurePaths) so that the host's start-up `[mcla-cap]` line, which is logged
// before OnPreSetup, already reads the cap; a value that only the TOML file gives (loaded after phase 0) is applied in
// phase 2, with a note. Same mechanism as everything here: the command line, the environment and the TOML file win.
#include "pc_profile.h"

#include <cstddef>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_ws_sizing.h"  // mcla/hooks: the presets' numbers are checked against the same maths the hooks use

REXCVAR_DEFINE_STRING(mcla_profile, "", "MCLA",
                      "MCLA PC profile preset applied on top of the default profile at startup (an app default: "
                      "any cvar given explicitly still wins). '' or 'default' = the default profile only. "
                      "'1080p' = true 1920x1080 internal render (back buffer, video mode, render-target height, "
                      "noaa; needs edram_tiles=4096, which the default profile sets). '900p' = 1600x900 "
                      "(the cheaper fallback: present it scaled to 1080p). Read once, in OnPreSetup")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(mcla_fps_target, 0, "MCLA",
                     "MCLA frame-rate target preset, applied as app defaults at startup (any cvar given explicitly "
                     "still wins). 0 = the shipped timing: nothing is set (the guest's present interval 2 holds 30 "
                     "fps; no host cap, mcla_sim60 off). 30 = locked 30 (mcla_fps_cap=30, the shipped timing). 60 = "
                     "the 60 fps profile: mcla_sim60=true, mcla_fps_cap=60, the A5 present interval on the guest "
                     "vblank rate (1 at 60 Hz), k(dt) camera and chassis, Flash movies at 30 Hz, countdown and race "
                     "clock. 120 is reserved (M7) and is not applied. Independent of mcla_profile (the resolution)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla {
namespace {

struct CvarDefault {
  const char* name;
  const char* value;
};

// Phase 0 (OnConfigurePaths): read by InitLogging, which runs before any later app hook.
constexpr CvarDefault kLoggingDefaults[] = {
    // At the SDK's 5 MB the log rotates the startup lines away within about a minute.
    // Source: iOS games/mcla/game_profile.cpp:60-63; SDK src/core/logging.cpp:51 (5).
    {"log_max_file_size_mb", "50"},
    // A flush every 2 s: the log survives a timeout kill or a hang. Source: iOS mac/src/main.mm:280;
    // SDK src/core/logging.cpp:47 (0 = no periodic flush).
    {"log_flush_interval", "2"},
};

// Phase 2 (OnPreSetup, after LoadGpuPlugin and before Runtime::Setup). The GPU cvars below are defined in
// the plugin (rexgpu-xenos.dll), so they exist only once it is loaded.
constexpr CvarDefault kProfileDefaults[] = {
    // 1080p at 1x needs 3,240 EDRAM tiles (PLAN 3.8); changes nothing until the game allocates past 2,048.
    // Read when the render-target cache is created. Source: Renan mcla_app.cpp:1099, off-switches.md:25
    // (his PC and PS5 default); defined in rexglue-edram-size.patch:787 (2048). W0.4.
    {"edram_tiles", "4096"},
    // The SDK makes every host render target as tall as the EDRAM; the game's tallest is 736. The 880M is
    // UMA, so this saves system RAM. Source: Renan :1100, off-switches.md:72; rexglue-render-target-max-height.patch:25 (0).
    // 736 rows is the 720p case ONLY: a back buffer taller than 1024 rows (1080p: 1088) needs the preset's value,
    // or the bottom of every full-screen target is cut.
    {"render_target_max_height", "1024"},
    // Frees render targets idle for 60 s. Source: Renan :1101, off-switches.md:73;
    // rexglue-render-target-trim.patch:186 (0 = never).
    {"render_target_trim_seconds", "60"},
    // Trims and halves the texture cache at 90 % of the GPU memory budget. Source: Renan :1102,
    // off-switches.md:74; rexglue-gpu-memory.patch:234 (0 = off).
    {"gpu_memory_pressure_percent", "90"},
    // Unclipped draws stop claiming the whole EDRAM (Xenia's behaviour). Source: Renan :1096,
    // off-switches.md:17; SDK src/graphics/util/draw_extent_estimator.cpp:27 (false).
    {"execute_unclipped_draw_vs_on_cpu", "true"},
    // Hot shared-memory pages: after 4 faults within the window a page is uploaded wholesale. Needs
    // rexglue-hot-pages-watched-ranges in the series, else the attract-movie colours break (KNOWN-ISSUES.md:35-39).
    // Source: Renan :1097, off-switches.md:15; rexglue-v0.10.0-mcla.patch:1561 (0).
    {"shared_memory_hot_page_faults", "4"},
    // The window of the row above, in ms. Source: Renan :1098; rexglue-v0.10.0-mcla.patch:1569 (2000).
    {"shared_memory_hot_page_ms", "10000"},
    // 16x anisotropic filtering (index 5): never below what the game asks for (16x on the road). Costs
    // 1.5 % on an RDNA2 iGPU; the W5.4 lever. Source: Renan :1103, KNOWN-ISSUES.md:680-686;
    // SDK src/graphics/pipeline/texture/cache.cpp:52 (3 = 4x).
    {"anisotropic_override", "5"},
    // -2.4 % frame time on a Radeon iGPU. Pictures are not proved identical: the first suspect in any parity
    // diff. Source: Renan :1105, KNOWN-ISSUES.md:695; rexglue-d3d12-hoist-shared-memory-uploads.patch:243 (false).
    {"d3d12_hoist_shared_memory_uploads", "true"},
    // Custom vinyls are the only readback the game needs: a 1 MiB, 512x512, format-6 resolve into the
    // vinyl editor's range (readback_resolve itself stays `none`). The owner's and Renan's strings are
    // identical. Source: iOS games/mcla/game_profile.cpp:40; Renan perf_options.cpp:619,652;
    // rexglue-resolve-readback-rules.patch:113 (empty).
    {"readback_resolve_rules", "len=1048576,pitch=512,height=512,fmt=6;addr=0x051A0000-0x051F7FFF"},
    // Keyboard play for a human at the laptop. Source: iOS common/src/nfsmw_app.h:185;
    // SDK src/input/mnk/mnk_input_driver.cpp:26 (false).
    {"mnk_mode", "true"},
};

// ---- presets (W5.1) -------------------------------------------------------------------------------------------
//
// Why these cvars and not `resolution=WxH`: the SDK's `resolution` preset is only honoured while
// HasNonDefaultValue("resolution") is true, and SetDefaultValue makes the value the cvar's own default, so a profile
// entry for `resolution` would silently do nothing. The two halves it stands for are set instead:
// video_mode_width / video_mode_height (the guest video mode, read directly by xboxkrnl_video.cpp
// GetConfiguredVideoModeWidth, so they work as defaults) and window_width / window_height (the startup window,
// read directly by window_sdl.cpp ResolveWindowWidth). `--resolution=1080p` or any of the four on the command line
// still overrides them. The guest video mode is what the presenter and the scene-size hook compare the back buffer
// with; the window matters for windowed runs (fullscreen takes the desktop).
//
// `mcla_noaa=true` is PLAN D1's recommendation (the 880M is GPU-bound): the renderer is created without MSAA, one 1x
// scene instead of two 2x MSAA tiles. At 1x the game's own tiling is 2 tiles of 672 rows at 1920x1080 and 2 of 800 at
// 1600x900 (mcla_ws_sizing.h GameSceneTiling), against 4 tiles of 320 rows with MSAA at 1080p.
//
// The UI squeeze stays off (mcla_ws_ui_squeeze=0: at exactly 16:9 the Flash stage already fits the buffer, nothing
// to narrow), and so does its probe, which would only count calls on the 60 fps path; the aspect and cutscene hooks
// idle by themselves at exactly 16:9.
namespace sizing = ::mcla::ws::sizing;

// The values below must be accepted by the hooks' own maths: checked at compile time.
static_assert(sizing::Resolve({1920, 1080}, {1920, 1080}).verdict == sizing::Verdict::kOn);
static_assert(sizing::Resolve({1600, 900}, {1600, 900}).verdict == sizing::Verdict::kOn);
static_assert(sizing::IsSixteenNine(1920, 1080) && sizing::IsSixteenNine(1600, 900));
static_assert(sizing::RenderTargetMaxHeight(1080) == 1088 && sizing::RenderTargetMaxHeight(900) == 912);

constexpr CvarDefault k1080pDefaults[] = {
    // The scene-size hook (mcla_ws_scene_size.cpp) writes 1920x1080 into the game's width/height options.
    {"mcla_widescreen_width", "1920"},
    {"mcla_scene_height", "1080"},
    // The guest video mode: the SDK presenter shows the 1920x1080 front buffer 1:1 instead of stretching it.
    {"video_mode_width", "1920"},
    {"video_mode_height", "1080"},
    {"window_width", "1920"},
    {"window_height", "1080"},
    // Rows of the tallest host render target: the device surfaces are 1080 rows (1088 aligned to the 16-row tile).
    // Replaces the default profile's 1024, which would cut the bottom 56 rows.
    {"render_target_max_height", "1088"},
    {"mcla_noaa", "true"},
    {"mcla_ws_ui_squeeze", "0"},
    {"mcla_ws_ui_probe", "false"},
};

constexpr CvarDefault k900pDefaults[] = {
    {"mcla_widescreen_width", "1600"},
    {"mcla_scene_height", "900"},
    {"video_mode_width", "1600"},
    {"video_mode_height", "900"},
    {"window_width", "1600"},
    {"window_height", "900"},
    {"render_target_max_height", "912"},  // 900 rows aligned to the 16-row tile
    {"mcla_noaa", "true"},
    {"mcla_ws_ui_squeeze", "0"},
    {"mcla_ws_ui_probe", "false"},
};

struct Preset {
  const char* name;     // the value of mcla_profile
  const char* summary;  // for the log
  const CvarDefault* entries;
  size_t count;
};

constexpr Preset kPresets[] = {
    {"1080p", "true 1920x1080 internal render, 1x (noaa), edram_tiles=4096 + the EDRAM limit hook",
     k1080pDefaults, sizeof(k1080pDefaults) / sizeof(k1080pDefaults[0])},
    {"900p", "1600x900 internal render, 1x (noaa): the cheaper fallback, present scaled to 1080p", k900pDefaults,
     sizeof(k900pDefaults) / sizeof(k900pDefaults[0])},
};

// ---- frame-rate targets (W4.1) --------------------------------------------------------------------------------
//
// What the iOS port's 60 fps profile is: the launcher row "Frame rate: 60" (games/mcla/catalog.cpp:120, kFps60) sets
// two cvars, `ios_fps_cap=60` (the host's frame cap: here mcla_fps_cap) and `mcla_sim60=true`. The rest is hook
// defaults that mcla_sim60 switches on by itself (TIMING.md 6): the quantised world step n/60 s with n substeps (T1
// sim_step, A2 frame_delta), the k(dt) chase camera and chassis (T2/T3, mcla_smoothing_fix), Flash movies held at 30
// Hz (T4 ui_movie_advance) and the countdown's dt (T5, mcla_ui_30hz), the race clock (T6, mcla_race_clock=retail),
// the game-thread pacer (mcla_sim_pace), and the A5 present interval (mcla_present_interval=-1 auto: round(guest
// vblank Hz / 60) = 1). The rows below name all of them, so the profile does not depend on the hooks' defaults and
// the log lists the whole set.
//
// Left out on purpose (iOS only, none of them exists in this build): the battery / thermal cap limit, the renderer's
// resolution-governor "room" input to the 60 -> 30 rule, the 120 Hz guest-vblank lock of ios_fps_cap=40/120,
// nfsmw_performance_mode, the 30 fps menu cap. Not touched, by contract (PLAN W4.1, TIMING.md): the reset guard at
// 0x821BDB08 and r24 at 0x822C2430.
constexpr CvarDefault kFps30Defaults[] = {
    // The host limiter at 30 with the shipped timing (mcla_sim60 stays off): M4. src/host/frame_cap.h.
    {"mcla_fps_cap", "30"},
};

constexpr CvarDefault kFps60Defaults[] = {
    // The master switch of every hook of the 60 fps timing (mcla_t_common.h). Source: iOS catalog.cpp:120.
    {"mcla_sim60", "true"},
    // The host limiter at the swap. With mcla_sim60 the world's cap follows it (mcla_sim_cap stays 0), and the limiter
    // runs at min(this, the world's cap): a 60 -> 30 hold lowers both. Source: iOS catalog.cpp:120 (ios_fps_cap);
    // mcla_t_common.cpp PaceAtSwap; the table in mcla_t_common.h ("on 60 0").
    {"mcla_fps_cap", "60"},
    // A5, auto (the hook's own default): the guest's presentation interval 2 is lowered to
    // round(video_mode_refresh_rate / 60) = 1 on every swap that had an engine-timer frame; loads and logos keep 2.
    // 0 would switch the hook off and the guest would hold 30 fps. mcla_t_present_interval.cpp.
    {"mcla_present_interval", "-1"},
    // The guest vblank rate in Hz, which the A5 hook reads too (interval = round(this / 60)). The SDK default, pinned
    // so that the interval does not move with an SDK default; 120 belongs to the reserved 120 target (M7). SDK
    // src/ui/window.cpp (60.0); patch 0400 (the precise vblank runs on the 1/this grid).
    {"video_mode_refresh_rate", "60"},
    // T2/T3: chase camera position and look-at, chassis ground depth: k(dt) = 1 - (1 - k30)^(30 dt).
    {"mcla_smoothing_fix", "true"},
    // T4/T5: Flash movies (HUD, menus) step 30 times a second; the countdown gets the time since its last step.
    {"mcla_ui_30hz", "true"},
    // T6: 0.99 of real time with the fraction carried (the console's 33 ms per 1/30 s).
    {"mcla_race_clock", "retail"},
    // The game thread sleeps to an even 1/60 s deadline each frame so n does not flip with the render thread's jitter.
    {"mcla_sim_pace", "true"},
};

struct FpsTarget {
  int fps;              // the value of mcla_fps_target
  const char* summary;  // for the log
  const CvarDefault* entries;
  size_t count;
};

constexpr FpsTarget kFpsTargets[] = {
    {30, "locked 30 (M4): the host limiter holds the shipped 1/30 s timing to an even 30 fps", kFps30Defaults,
     sizeof(kFps30Defaults) / sizeof(kFps30Defaults[0])},
    {60,
     "60 fps (M5): world step n/60 s (mcla_sim60) with k(dt) camera and chassis, Flash movies at 30 Hz, countdown and "
     "race clock corrected, guest present interval 1 on the 60 Hz guest vblank, host limiter at 60",
     kFps60Defaults, sizeof(kFps60Defaults) / sizeof(kFps60Defaults[0])},
};

enum class Outcome { kApplied, kMissing, kRejected };

struct Applied {
  const CvarDefault* entry;
  Outcome outcome;
  std::string origin;  // "" for the default tables, else what applied it (a preset), for the log
};

const char* SourceName(rex::cvar::Source source) {
  switch (source) {
    case rex::cvar::Source::kDefault:
      return "default";
    case rex::cvar::Source::kConfig:
      return "config file";
    case rex::cvar::Source::kEnvironment:
      return "environment";
    case rex::cvar::Source::kCommandLine:
      return "command line";
    case rex::cvar::Source::kRuntime:
      return "runtime";
  }
  return "?";
}

Outcome Apply(const CvarDefault& entry) {
  if (!rex::cvar::GetFlagInfo(entry.name)) {
    return Outcome::kMissing;
  }
  return rex::cvar::SetDefaultValue(entry.name, entry.value) ? Outcome::kApplied : Outcome::kRejected;
}

// Counts of one logged table, for the summary line.
struct Tally {
  size_t applied = 0, user_set = 0, missing = 0, rejected = 0;
};

// Logs one entry with the cvar's source at this moment: a value that came from the command line, the
// environment or the TOML file is "user-set", and wins over the profile's default.
void Log(const Applied& applied, Tally* tally) {
  const CvarDefault& entry = *applied.entry;
  const std::string from = applied.origin.empty() ? std::string() : " (" + applied.origin + ")";
  switch (applied.outcome) {
    case Outcome::kMissing:
      ++tally->missing;
      REXLOG_WARN("[mcla-profile] MISSING cvar {} (profile value {}{}): this SDK build has no such cvar", entry.name,
                  entry.value, from);
      return;
    case Outcome::kRejected:
      ++tally->rejected;
      REXLOG_WARN("[mcla-profile] REJECTED {}={}{}: the SDK refused the value (current {})", entry.name, entry.value,
                  from, rex::cvar::GetFlagByName(entry.name));
      return;
    case Outcome::kApplied:
      break;
  }
  const rex::cvar::Source source = rex::cvar::GetFlagSource(entry.name);
  if (source == rex::cvar::Source::kDefault) {
    ++tally->applied;
    REXLOG_INFO("[mcla-profile] applied {}={}{}", entry.name, rex::cvar::GetFlagByName(entry.name), from);
  } else {
    ++tally->user_set;
    REXLOG_INFO("[mcla-profile] user-set {}={} ({}); the profile's {}{} stays only the default", entry.name,
                rex::cvar::GetFlagByName(entry.name), SourceName(source), entry.value, from);
  }
}

// What phase 0 did, logged by phase 2 once logging is up.
std::vector<Applied>& LoggingResults() {
  static std::vector<Applied> results;
  return results;
}
bool g_logging_phase_ran = false;

// The frame-rate target's entries and the value of mcla_fps_target they were applied for (-1: not applied yet).
struct FpsPhase {
  int target = -1;
  std::vector<Applied> results;
};
FpsPhase& FpsApplied() {
  static FpsPhase phase;
  return phase;
}

const FpsTarget* FindFpsTarget(int fps) {
  for (const FpsTarget& target : kFpsTargets) {
    if (target.fps == fps) {
      return &target;
    }
  }
  return nullptr;
}

// Applies the entries of the target that mcla_fps_target names right now (none for 0 or a value that is no target:
// LogFpsTarget says which). Nothing is logged here (phase 0 has no log yet).
void ApplyFpsTarget() {
  FpsPhase& phase = FpsApplied();
  phase.target = REXCVAR_GET(mcla_fps_target);
  phase.results.clear();
  const FpsTarget* target = FindFpsTarget(phase.target);
  if (target == nullptr) {
    return;
  }
  const std::string origin = "fps target " + std::to_string(target->fps);
  for (size_t i = 0; i < target->count; ++i) {
    phase.results.push_back({&target->entries[i], Apply(target->entries[i]), origin});
  }
}

void AddItem(std::string* list, const std::string& item) {
  if (!list->empty()) {
    *list += ", ";
  }
  *list += item;
}

// One entry's place in the summary line: what the target set, what somebody else's value kept, what failed.
void Describe(const Applied& applied, std::string* set, std::string* kept, std::string* problems) {
  const CvarDefault& entry = *applied.entry;
  if (applied.outcome == Outcome::kMissing) {
    AddItem(problems, std::string("MISSING ") + entry.name);
    return;
  }
  if (applied.outcome == Outcome::kRejected) {
    AddItem(problems, std::string("REJECTED ") + entry.name + "=" + entry.value);
    return;
  }
  const std::string now = std::string(entry.name) + "=" + rex::cvar::GetFlagByName(entry.name);
  const rex::cvar::Source source = rex::cvar::GetFlagSource(entry.name);
  if (source == rex::cvar::Source::kDefault) {
    AddItem(set, now);
  } else {
    AddItem(kept, now + " (" + SourceName(source) + "; the target's " + entry.value + ")");
  }
}

// Phase 2: the per-entry lines, then ONE `[mcla-profile] fps target N (source): ...` line that lists everything the
// target set (and what a user value kept). The value may have changed since phase 0: the TOML file is loaded between
// the two phases, so it is applied now in that case.
void LogFpsTarget(Tally* tally) {
  const int target = REXCVAR_GET(mcla_fps_target);
  const char* source = SourceName(rex::cvar::GetFlagSource("mcla_fps_target"));
  FpsPhase& phase = FpsApplied();
  bool late = false;
  if (phase.target != target) {
    ApplyFpsTarget();
    late = true;
  }
  if (target == 0) {
    REXLOG_INFO("[mcla-profile] fps target 0 ({}): the shipped timing, no frame-rate preset (mcla_sim60 and "
                "mcla_fps_cap keep their own values)",
                source);
    return;
  }
  const FpsTarget* chosen = FindFpsTarget(target);
  if (chosen == nullptr) {
    REXLOG_ERROR("[mcla-profile] error: mcla_fps_target={} ({}) is {} (known: 0, 30, 60); the shipped timing is used",
                 target, source,
                 target == 120 ? "reserved for M7 (it needs a 120 Hz guest vblank and the 1/120 world grid)"
                               : "not a frame-rate target");
    return;
  }
  std::string set, kept, problems;
  for (const Applied& applied : phase.results) {
    Log(applied, tally);
    Describe(applied, &set, &kept, &problems);
  }
  std::string line = std::string("[mcla-profile] fps target ") + std::to_string(target) + " (" + source + "): " +
                     chosen->summary + " | set: " + (set.empty() ? "nothing" : set);
  if (!kept.empty()) {
    line += " | user-set, kept: " + kept;
  }
  if (!problems.empty()) {
    line += " | problems: " + problems;
  }
  if (late) {
    line += " | applied in OnPreSetup (the TOML file gave it): the start-up [mcla-cap] line read the shipped values";
  }
  if (problems.empty()) {
    REXLOG_INFO("{}", line);
  } else {
    REXLOG_WARN("{}", line);
  }
  if (target >= 60 && !rex::cvar::Query<bool>("mcla_sim60")) {
    REXLOG_WARN("[mcla-profile] mcla_fps_target={} but mcla_sim60 is false: the world keeps the shipped 1/30 s step and "
                "the guest's present interval 2, so the run is 30 fps and the host limiter never waits",
                target);
  }
}

// The preset named by mcla_profile (nullptr: none), with the log lines that say how it was chosen.
const Preset* SelectPreset() {
  const std::string name = REXCVAR_GET(mcla_profile);
  if (name.empty() || name == "default") {
    return nullptr;
  }
  for (const Preset& preset : kPresets) {
    if (name == preset.name) {
      REXLOG_INFO("[mcla-profile] mcla_profile={} ({}): {}", name,
                  SourceName(rex::cvar::GetFlagSource("mcla_profile")), preset.summary);
      return &preset;
    }
  }
  std::string known = "default";
  for (const Preset& preset : kPresets) {
    known += std::string(", ") + preset.name;
  }
  REXLOG_ERROR("[mcla-profile] error: unknown mcla_profile '{}' (known: {}); the default profile is used", name, known);
  return nullptr;
}

// The preset's entry for a cvar of the default table, if it has one.
const CvarDefault* PresetEntryFor(const Preset* preset, const char* name) {
  if (preset == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < preset->count; ++i) {
    if (std::string(preset->entries[i].name) == name) {
      return &preset->entries[i];
    }
  }
  return nullptr;
}

}  // namespace

void ApplyPcProfileLogging() {
  g_logging_phase_ran = true;
  for (const CvarDefault& entry : kLoggingDefaults) {
    LoggingResults().push_back({&entry, Apply(entry), std::string()});
  }
  ApplyFpsTarget();
}

void ApplyPcProfile() {
  Tally tally;
  if (!g_logging_phase_ran) {
    REXLOG_WARN("[mcla-profile] ApplyPcProfileLogging() did not run in OnConfigurePaths: the log cvars ({}, {}) "
                "keep the SDK defaults",
                kLoggingDefaults[0].name, kLoggingDefaults[1].name);
  }
  for (const Applied& applied : LoggingResults()) {
    Log(applied, &tally);
  }
  const Preset* preset = SelectPreset();
  for (const CvarDefault& entry : kProfileDefaults) {
    if (PresetEntryFor(preset, entry.name) != nullptr) {
      continue;  // the preset's value replaces this one (logged with the preset's entries below)
    }
    Log({&entry, Apply(entry), std::string()}, &tally);
  }
  if (preset != nullptr) {
    for (size_t i = 0; i < preset->count; ++i) {
      const CvarDefault& entry = preset->entries[i];
      std::string origin = std::string(preset->name) + " preset";
      for (const CvarDefault& base : kProfileDefaults) {
        if (std::string(base.name) == entry.name) {
          origin += ", replaces the default profile's " + std::string(base.value);
        }
      }
      Log({&entry, Apply(entry), origin}, &tally);
    }
  }
  LogFpsTarget(&tally);
  REXLOG_INFO("[mcla-profile] {} cvars: {} applied, {} user-set, {} missing, {} rejected",
              tally.applied + tally.user_set + tally.missing + tally.rejected, tally.applied, tally.user_set,
              tally.missing, tally.rejected);
}

}  // namespace mcla
