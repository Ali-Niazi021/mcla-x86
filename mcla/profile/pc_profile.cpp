// The PC cvar profile (see pc_profile.h). One table per phase; each entry says what it is for and where the value
// comes from. "W0.4" = the research report $WORK/scratch/W0.4/REPORT.md section 1 (the patch lines there are
// the REXCVAR_DEFINE of the cvar). Left at the SDK default on purpose, so nobody ports them by mistake:
// vsync (false makes the guest vblank fire at ~1 kHz), readback_resolve (stays `none`: the vinyl rule below is
// the only readback), clear_memory_page_state, resolution_scale, hdr.
#include "pc_profile.h"

#include <cstddef>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

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

enum class Outcome { kApplied, kMissing, kRejected };

struct Applied {
  const CvarDefault* entry;
  Outcome outcome;
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
  switch (applied.outcome) {
    case Outcome::kMissing:
      ++tally->missing;
      REXLOG_WARN("[mcla-profile] MISSING cvar {} (profile value {}): this SDK build has no such cvar",
                  entry.name, entry.value);
      return;
    case Outcome::kRejected:
      ++tally->rejected;
      REXLOG_WARN("[mcla-profile] REJECTED {}={}: the SDK refused the value (current {})", entry.name,
                  entry.value, rex::cvar::GetFlagByName(entry.name));
      return;
    case Outcome::kApplied:
      break;
  }
  const rex::cvar::Source source = rex::cvar::GetFlagSource(entry.name);
  if (source == rex::cvar::Source::kDefault) {
    ++tally->applied;
    REXLOG_INFO("[mcla-profile] applied {}={}", entry.name, rex::cvar::GetFlagByName(entry.name));
  } else {
    ++tally->user_set;
    REXLOG_INFO("[mcla-profile] user-set {}={} ({}); the profile's {} stays only the default", entry.name,
                rex::cvar::GetFlagByName(entry.name), SourceName(source), entry.value);
  }
}

// What phase 0 did, logged by phase 2 once logging is up.
std::vector<Applied>& LoggingResults() {
  static std::vector<Applied> results;
  return results;
}
bool g_logging_phase_ran = false;

}  // namespace

void ApplyPcProfileLogging() {
  g_logging_phase_ran = true;
  for (const CvarDefault& entry : kLoggingDefaults) {
    LoggingResults().push_back({&entry, Apply(entry)});
  }
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
  for (const CvarDefault& entry : kProfileDefaults) {
    Log({&entry, Apply(entry)}, &tally);
  }
  REXLOG_INFO("[mcla-profile] {} cvars: {} applied, {} user-set, {} missing, {} rejected",
              tally.applied + tally.user_set + tally.missing + tally.rejected, tally.applied, tally.user_set,
              tally.missing, tally.rejected);
}

}  // namespace mcla
