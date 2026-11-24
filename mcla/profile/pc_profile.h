// The PC cvar profile: the launcher defaults of the iOS port (games/mcla/game_profile.cpp) and the values
// Renan's PC port found for the Radeon/UMA class, as one table of SDK cvar defaults (docs/PLAN.md 3.8, W0.4
// report section 1). The host applies it in two phases, because the SDK reads some cvars before the app's
// later hooks run (rex_app.cpp: SetupEnvironment, then SetupPresentation, then ConstructRuntime):
//
//   ApplyPcProfileLogging()  OnConfigurePaths, the only app hook before InitLogging reads the log cvars.
//   ApplyPcProfile()         OnPreSetup, right after the explicit LoadGpuPlugin("xenos", backend) and before
//                            Runtime::Setup: the GPU cvars exist once the plugin is loaded and are read when
//                            the graphics system starts (edram_tiles, render_target_max_height), which is
//                            before OnPostSetup.
//
// Both go through rex::cvar::SetDefaultValue (patches/sdk cvar-toml-strings): an app default that the
// command line, the environment and the TOML file still override, and that SaveConfig never writes. Every
// entry is logged with the [mcla-profile] tag as applied, user-set (somebody else's value wins), MISSING
// (the SDK has no such cvar) or REJECTED (the SDK refused the value).
#pragma once

namespace mcla {

// Phase 0. Applies the log cvars; nothing is logged yet (logging is not up). ApplyPcProfile() logs the result.
void ApplyPcProfileLogging();

// Phase 2. Logs the phase 0 results, then applies and logs the rest of the profile. Call it once.
void ApplyPcProfile();

}  // namespace mcla
