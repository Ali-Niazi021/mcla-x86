// mcla-x86 host overlay: what the mod menu can change, and how a change is kept.
//
// The menu is a table of Items, each one a cvar with a label, a help text and the way to edit it. The ImGui code
// (overlay_ui.cpp) draws a row for an Item; this file knows nothing of ImGui. What it does know:
//
//   * how a value is read and written (the cvar registry: rex::cvar::GetFlagByName / SetFlagByName, so a change applies at
//     once for a live cvar and is queued for the next start by the registry for a restart one), and which values the player
//     changed in this run;
//   * the live detail preset: mcla_gfx_preset is applied once at first use (a std::call_once), so the menu applies a preset
//     itself, the way the real one does: as the knobs' APP DEFAULTS (rex::cvar::SetDefaultValue), which a knob the player
//     set by hand keeps over, and which the adaptive-detail governor may still move (mcla_gfx_tune.h ForEachPresetKnob);
//   * the cheats: every cheat is a cvar whose default is "off" (-1), listed here so one button can switch them all off;
//   * Save: the one place that writes anything to disk, only when the player presses the button (see SaveSettings).
//
// Persistence, chosen after reading how the launcher works (docs: scratch/overlay/research.md section 8):
//   The launcher builds the game's command line from %LOCALAPPDATA%\mcla_x86\launcher.json on EVERY start, and a command-line
//   value outranks the game's own config file (mcla_x86.toml next to the exe). So a value written only to the TOML would be
//   overruled by the launcher for every setting the launcher passes. Save therefore writes both:
//     1. rex::cvar::SaveConfigValues(mcla_x86.toml, names): the game started without the launcher, and the same file the SDK's
//        own F4 "Save to config" writes;
//     2. when launcher.json exists and parses: values.<cvar> = <current value> for the same names (a launcher setting's id is
//        its cvar, launcher/schema/README.md), plus the launcher-only companion some settings need (swap_post_effect: aa_auto =
//        false, so the launcher stops leaving the choice to the game). An id the launcher does not know is carried by it
//        untouched. The file is replaced atomically, never created, and never touched if it does not parse.
//   Cheats and the run-time tier are never saved. Nothing is saved without the button.
#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mcla_overlay {

enum class Kind : uint8_t {
  kToggle,     // a bool cvar
  kInt,        // an integer cvar, a slider
  kFloat,      // a double cvar, a slider
  kChoice,     // one of a list of values (int, bool or string cvar): a combo
  kHoldInt,    // an integer cvar with an "off" value: a tick box, and a slider while it is ticked
  kHoldFloat,  // the same for a double cvar
  kPreset,     // the live detail preset combo (mcla_gfx_preset)
};

enum ItemFlags : uint32_t {
  kFlagNone = 0,
  kFlagRestart = 1u << 0,   // applies at the next start, whatever the cvar's lifecycle says
  kFlagNextLoad = 1u << 1,  // applies at the next world load (the game reads it when the world is built)
  kFlagCheat = 1u << 2,     // a cheat or a mod: off by default, needs mcla_overlay_cheats, never saved
  kFlagUntested = 1u << 3,  // the cvar's own help says it was not tried in the game yet
  kFlagNoSave = 1u << 4,    // Save leaves it out
  kFlagGovOff = 1u << 5,    // meaningless while adaptive detail is on (the governor chooses): shown disabled then
  kFlagGovOn = 1u << 6,     // meaningless while adaptive detail is off (its ceiling and floor): shown disabled then
};

struct Choice {
  const char* value;  // the cvar's text for it ("-1", "fxaa", "true")
  const char* label;
};

struct Item {
  Kind kind;
  const char* label;
  const char* cvar;
  const char* help;
  double min = 0.0;
  double max = 0.0;
  const char* format = "%.2f";
  const Choice* choices = nullptr;
  int choice_count = 0;
  uint32_t flags = kFlagNone;
  double off_value = -1.0;  // kHold*: the value that means "off" (the game's own behaviour)
  double on_value = 0.0;    // kHold*: where the slider starts when the tick box is ticked
  const char* companion = nullptr;  // a launcher-only setting Save sets to false with this one (aa_auto)
};

template <size_t N>
constexpr int CountOf(const Choice (&)[N]) {
  return int(N);
}

// ---- the choices -----------------------------------------------------------------------------------------------------------
inline constexpr Choice kChoicesCorner[] = {{"0", "Top left"}, {"1", "Top right"}, {"2", "Bottom left"}, {"3", "Bottom right"}};
inline constexpr Choice kChoicesFpsDetail[] = {
    {"0", "Frames per second"}, {"1", "Frames per second and frame time"}, {"2", "That, the 1 % low and the GPU time"}};
inline constexpr Choice kChoicesAa[] = {{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA (extreme)"}};
inline constexpr Choice kChoicesHdr[] = {{"off", "Off"}, {"auto", "Auto (when Windows HDR is on)"}, {"on", "On"}};
inline constexpr Choice kChoicesAniso[] = {{"-1", "Game default"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"},
                                          {"3", "4x"},           {"4", "8x"},  {"5", "16x"}};
inline constexpr Choice kChoicesTier[] = {{"1", "1: Performance"},
                                          {"2", "2: Performance + FXAA"},
                                          {"3", "3: Shipped distances"},
                                          {"4", "4: Shipped rates (quality)"},
                                          {"5", "5: Beyond console (1.5x distances)"}};
inline constexpr Choice kChoicesTierFloor[] = {{"-1", "Automatic (from the preset)"},
                                               {"1", "1: Performance"},
                                               {"2", "2: Performance + FXAA"},
                                               {"3", "3: Shipped distances"},
                                               {"4", "4: Shipped rates (quality)"},
                                               {"5", "5: Beyond console"}};
inline constexpr Choice kChoicesShadowMode[] = {{"0", "Sun shadows (as shipped)"}, {"1", "No sun shadows (the game's night path)"}};
inline constexpr Choice kChoicesShadowRes[] = {{"0", "As shipped (640)"}, {"512", "512"}, {"384", "384"}};
inline constexpr Choice kChoicesInterval[] = {{"1", "Every frame (as shipped)"}, {"2", "Every 2nd frame"}, {"3", "Every 3rd frame"},
                                              {"4", "Every 4th frame"}};
inline constexpr Choice kChoicesFpsCap[] = {
    {"0", "None (the game's own pacing)"}, {"30", "30 fps"}, {"60", "60 fps"}, {"120", "120 fps"}};
inline constexpr Choice kChoicesFpsTarget[] = {{"0", "Shipped timing (the game's own 30 fps)"},
                                               {"30", "30 fps (locked)"},
                                               {"60", "60 fps"},
                                               {"120", "120 fps (needs 120 Hz or variable refresh)"}};
inline constexpr Choice kChoicesWeather[] = {{"-1", "The game's own"}, {"0", "Nice"}, {"1", "Cloudy"}, {"2", "Stormy (rain)"},
                                             {"3", "Foggy"}};
inline constexpr Choice kChoicesClock[] = {{"-1", "The game decides"}, {"1", "Stopped"}, {"0", "Forced to run"}};
inline constexpr Choice kChoicesPadChord[] = {{"1", "Both stick clicks (L3 + R3)"},
                                              {"2", "Back + Start"},
                                              {"3", "LB + RB + Back"},
                                              {"0", "Nothing (the keyboard opens it)"}};

// The presets, as the launcher and mcla_gfx_preset name them.
struct PresetOption {
  const char* value;
  const char* label;
  const char* help;
};
inline constexpr PresetOption kPresets[] = {
    {"default", "Full (as shipped)", "The game's own detail levels."},
    {"perf", "Performance", "Car reflections and the far shadow cascade every 2nd frame, shorter shadow and detail distances, 512 shadow map."},
    {"perfplus", "Performance+", "Performance, no far shadow cascade and no tree impostors."},
    {"lowest", "Lowest", "Performance, no sun shadows (the game's night path)."},
    {"quality", "Quality", "The game's full detail plus FXAA."},
    {"ultra", "Ultra (beyond console)", "Quality with the shadow, city and vehicle distances at 1.5x."},
};

// ---- the items (overlay_ui.cpp draws rows for these) ----------------------------------------------------------------
namespace items {

// Display: the overlay itself.
inline constexpr Item kFps{Kind::kToggle, "FPS counter", "mcla_overlay_fps",
                           "A small readout of the frames per second in a corner of the screen. While it is shown the picture is "
                           "composed with an overlay layer (about 0.2 ms of GPU time at 1080p); hidden it costs nothing."};
inline constexpr Item kFpsCorner{Kind::kChoice, "Corner", "mcla_overlay_fps_corner", "Which corner of the screen the FPS counter sits in.",
                                 0, 0, "", kChoicesCorner, CountOf(kChoicesCorner)};
inline constexpr Item kFpsDetail{Kind::kChoice, "What it shows", "mcla_overlay_fps_detail",
                                 "How much the FPS counter says: just the rate, with the frame time, or with the 1 % low "
                                 "(the rate of the slowest 1 % of frames) and the GPU time too.",
                                 0, 0, "", kChoicesFpsDetail, CountOf(kChoicesFpsDetail)};
inline constexpr Item kStats{Kind::kToggle, "Statistics screen", "mcla_overlay_stats",
                             "Frame times with a graph, the GPU, the display, adaptive detail, hitches and the machine's clocks "
                             "and memory. Needs the overlay layer like the FPS counter."};
inline constexpr Item kScale{Kind::kFloat, "Overlay size", "mcla_overlay_scale",
                             "The size of the overlay's text and windows, on top of the automatic size that follows the window's "
                             "height.",
                             0.5, 3.0, "%.2f x"};
inline constexpr Item kOpacity{Kind::kFloat, "Overlay background", "mcla_overlay_opacity",
                               "How opaque the panels behind the overlay's text are.", 0.2, 1.0, "%.2f"};

// Display: the picture.
inline constexpr Item kFullscreen{Kind::kToggle, "Fullscreen", "fullscreen", "Borderless fullscreen on the display the window is on."};
inline constexpr Item kAa{Kind::kChoice,
                          "Anti-aliasing",
                          "swap_post_effect",
                          "FXAA smooths stair-stepped edges over the finished frame (HUD included), about 0.3 ms of GPU time at "
                          "1080p. While adaptive detail is on it may turn FXAA on or off by itself unless you choose here.",
                          0, 0, "", kChoicesAa, CountOf(kChoicesAa), kFlagNone, -1.0, 0.0, "aa_auto"};
inline constexpr Item kHdr{Kind::kChoice, "HDR output", "hdr",
                           "Auto uses HDR when Windows HDR is on for this display. Experimental.", 0, 0, "", kChoicesHdr,
                           CountOf(kChoicesHdr)};
inline constexpr Item kLodBias{Kind::kFloat, "Texture sharpness (mip bias)", "texture_lod_bias",
                               "Negative values pick sharper texture levels (and shimmer more). 0 is the game's own.", -1.875, 0.0,
                               "%.3f"};
inline constexpr Item kAniso{Kind::kChoice, "Anisotropic filtering", "anisotropic_override",
                             "Sharper textures at oblique angles (roads), at a small GPU cost. New samplers use it at once.", 0, 0, "",
                             kChoicesAniso, CountOf(kChoicesAniso)};
inline constexpr Item kTearing{Kind::kToggle, "Allow tearing (G-Sync / FreeSync)", "d3d12_allow_variable_refresh_rate_and_tearing",
                               "Lets a variable-refresh display follow the game's frame rate. On a TV or an ordinary monitor it "
                               "tears: leave it off there. Read when the swap chain is made: takes effect at the next start.",
                               0, 0, "", nullptr, 0, kFlagRestart};

// Graphics.
inline constexpr Item kGov{Kind::kToggle, "Adaptive detail", "mcla_gov",
                           "Keeps the frame rate by lowering the detail where it shows least, and raises it again when the GPU has "
                           "room. While it is on it chooses the detail tier itself."};
inline constexpr Item kGovCeiling{Kind::kChoice, "Highest detail it may reach", "mcla_gov_tier_ceiling",
                                  "The richest tier adaptive detail may raise the picture to when the GPU has room.", 0, 0, "",
                                  kChoicesTier, CountOf(kChoicesTier), kFlagGovOn};
inline constexpr Item kGovFloor{Kind::kChoice, "Lowest detail it starts from", "mcla_gov_tier_floor",
                                "The tier adaptive detail starts at and restores to. Automatic follows the detail preset.", 0, 0, "",
                                kChoicesTierFloor, CountOf(kChoicesTierFloor), kFlagGovOn};
inline constexpr Item kPreset{Kind::kPreset, "Detail preset", "mcla_gfx_preset",
                              "A bundle of the knobs below, applied as their defaults: a knob you set yourself keeps your value. "
                              "While adaptive detail is on it chooses the detail itself, so the preset sets the tier it starts "
                              "from (its floor) instead."};
inline constexpr Item kCityLod{Kind::kFloat, "City detail distance", "mcla_gfx_city_lod",
                               "Scale of the distance the city is drawn in full detail (the game's 300 m). Lower = fewer far "
                               "objects, less CPU and GPU.",
                               0.25, 2.0, "%.2f x"};
inline constexpr Item kTrafficLod{Kind::kFloat, "Vehicle detail distance", "mcla_gfx_traffic_lod",
                                  "Scale of the distances vehicles switch to a simpler model at.", 0.25, 2.0, "%.2f x"};
inline constexpr Item kShadowDist{Kind::kFloat, "Shadow distance", "mcla_gfx_shadow_dist",
                                  "Scale of the distance shadows are drawn at (the game's 200 m).", 0.25, 2.0, "%.2f x"};
inline constexpr Item kShadowMode{Kind::kChoice, "Sun shadows", "mcla_gfx_shadow_mode",
                                  "The game's own night path draws no cascaded sun shadows: about 45 % fewer draws.", 0, 0, "",
                                  kChoicesShadowMode, CountOf(kChoicesShadowMode)};
inline constexpr Item kShadowRes{Kind::kChoice, "Shadow map size", "mcla_gfx_shadow_res",
                                 "Edge of the sun-shadow map. Read when the world is built: applies at the next load.", 0, 0, "",
                                 kChoicesShadowRes, CountOf(kChoicesShadowRes), kFlagNextLoad};
inline constexpr Item kEnvInterval{Kind::kChoice, "Car reflections", "mcla_gfx_env_interval",
                                   "How often the cars' paint and speedometer reflections are redrawn.", 0, 0, "", kChoicesInterval,
                                   CountOf(kChoicesInterval)};
inline constexpr Item kFarCascades{Kind::kChoice, "Far shadow cascade", "mcla_gfx_far_cascades_interval",
                                   "How often the second-farthest shadow cascade is redrawn.", 0, 0, "", kChoicesInterval,
                                   CountOf(kChoicesInterval)};
inline constexpr Item kImpostors{Kind::kToggle, "Tree impostors", "mcla_gfx_impostors",
                                 "Off draws no distant trees (the cheapest, and a visible change)."};
inline constexpr Item kAmbient{Kind::kFloat, "Pedestrians and parked cars", "mcla_gfx_ambient_density",
                               "Scale of pedestrian density and the share of parked cars in zones built after the change. Applies "
                               "at the next load.",
                               0.0, 2.0, "%.2f x", nullptr, 0, kFlagNextLoad};
inline constexpr Item kMotionBlur{Kind::kFloat, "Motion blur", "mcla_vis_motion_blur",
                                  "Length of the streaks from movement. 1 = as the game does (half the console's at 60 fps), "
                                  "-1 = the console's look at any frame rate, 0 = none.",
                                  -1.0, 4.0, "%.2f"};

// Game.
// The profile is read at start (the 120 fps one also sets the guest's video mode to 120 Hz, which cannot change under a running
// game), so this row only queues the value: it applies at the next start, and Save keeps it for the launcher.
inline constexpr Item kFpsTarget{Kind::kChoice, "Frame-rate profile (next start)", "mcla_fps_target",
                                 "The whole frame-rate profile: the world's time step, the guest's present interval and video mode, the "
                                 "30 Hz menus, the race clock. 120 needs a 120 Hz mode of the display (Windows display settings) or "
                                 "variable refresh, and a GPU that renders a frame in under 8.3 ms. Applies at the next start.",
                                 0, 0, "", kChoicesFpsTarget, CountOf(kChoicesFpsTarget), kFlagRestart};
inline constexpr Item kFpsCap{Kind::kChoice, "Frame cap", "mcla_fps_cap",
                              "A host limit on the frames at the game's swap, lowered to what the world assumes (a 60 or 30 hold, the "
                              "120 -> 60 tier). It cannot change the profile: 120 fps needs the 120 Hz guest vblank the profile sets "
                              "at start (row above). Changing this changes the speed the world is paced at: use it from a menu, not "
                              "in a race.",
                              0, 0, "", kChoicesFpsCap, CountOf(kChoicesFpsCap)};
inline constexpr Item kSmoothing{Kind::kToggle, "Camera smoothing fix (60 / 120 fps)", "mcla_smoothing_fix",
                                 "Chase camera and chassis smoothing that behaves the same at any frame rate."};
inline constexpr Item kUi30{Kind::kToggle, "Menus and HUD at 30 Hz (60 / 120 fps)", "mcla_ui_30hz",
                            "The Flash menus and HUD step 30 times a second whatever the frame rate. Off = every frame (twice "
                            "as fast at 60, four times at 120)."};
inline constexpr Item kHudAspect{Kind::kToggle, "HUD at 16:9 proportions", "mcla_ws_hud_aspect",
                                 "On a wider screen the HUD is drawn at 16:9 proportions in the middle."};
inline constexpr Item kSkipIntro{Kind::kToggle, "Skip the legal screens", "mcla_skip_intro",
                                 "Skips drawing the legals during the boot load. Read at start.", 0, 0, "", nullptr, 0, kFlagRestart};
inline constexpr Item kMouse{Kind::kToggle, "Mouse looks around", "mnk_mouse",
                             "The mouse moves the right stick (keyboard mode only). The menu always frees the mouse."};
inline constexpr Item kMouseSens{Kind::kFloat, "Mouse sensitivity", "mnk_sensitivity", "Mouse speed for the right stick.", 0.1, 5.0,
                                 "%.2f"};
inline constexpr Item kOverlayOn{Kind::kToggle, "In-game overlay (this menu, counter, statistics)", "mcla_overlay",
                                 "Turns the whole overlay off: no key is listened to and nothing is created. Takes effect at the "
                                 "next start.",
                                 0, 0, "", nullptr, 0, kFlagRestart};

inline constexpr Item kPadChord{Kind::kChoice, "Controller: open the menu with", "mcla_overlay_pad_chord",
                                "The buttons that open and close this menu from a controller when they are held together for half "
                                "a second (nothing else pressed; the hold time is mcla_overlay_pad_hold_ms). Both stick clicks is "
                                "the default: two clicks at once do not happen while driving. With Back + Start the game sees the "
                                "Start press first (its pause menu opens). While the menu is open the game gets nothing from the "
                                "pad, and a button held at the close does not reach it until it is let go.",
                                0, 0, "", kChoicesPadChord, CountOf(kChoicesPadChord)};

// Cheats and mods.
inline constexpr Item kCheatsGate{Kind::kToggle, "Allow cheats and mods", "mcla_overlay_cheats",
                                  "Off by default, and off at every start. The settings below change the game; none is saved.",
                                  0, 0, "", nullptr, 0, kFlagNoSave};
inline constexpr Item kTod{Kind::kHoldFloat, "Hold the time of day", "mcla_dev_tod",
                           "Holds the game's clock at this hour (0 to 24) every frame. Needs a loaded world. Releasing puts the "
                           "hour back.",
                           0.0, 24.0, "%.1f h", nullptr, 0, kFlagCheat | kFlagNoSave, -1.0, 12.0};
inline constexpr Item kClock{Kind::kChoice, "The game's clock", "mcla_dev_tod_freeze",
                             "Stops the clock where it is, or forces it to run. Ignored while an hour is held.", 0, 0, "",
                             kChoicesClock, CountOf(kChoicesClock), kFlagCheat | kFlagNoSave};
inline constexpr Item kWeather{Kind::kChoice, "Hold the weather", "mcla_dev_weather",
                               "Holds the weather and stops the game's random change every 5 minutes. Releasing puts it back.", 0, 0,
                               "", kChoicesWeather, CountOf(kChoicesWeather), kFlagCheat | kFlagNoSave};
inline constexpr Item kWeatherSnap{Kind::kToggle, "Weather changes at once", "mcla_dev_weather_snap",
                                   "On: the new weather appears at once. Off: the game's own 16 s fade.", 0, 0, "", nullptr, 0,
                                   kFlagCheat | kFlagNoSave};
inline constexpr Item kRoadDensity{Kind::kHoldInt, "Hold the traffic density", "mcla_dev_road_density",
                                   "A global traffic density index (0 to 15; 6 is the usual) instead of each road's own. Not yet "
                                   "tried in the game.",
                                   0, 15, "%d", nullptr, 0, kFlagCheat | kFlagNoSave | kFlagUntested, -1.0, 6.0};

}  // namespace items

// ---- reading and writing values ----------------------------------------------------------------------------------------------
// All of these run on the UI thread. The cvar registry takes its lock inside each call.

std::string CvarString(const char* name);
bool CvarBool(const char* name);
double CvarNumber(const char* name);

// Sets the cvar (SetFlagByName: the player's value, source "runtime") and remembers the change for Save unless `track` is false
// or the cvar is on the never-saved list. False when the registry refused the value (out of range, unknown name).
bool SetCvar(const char* name, std::string_view value, bool track = true);
bool SetCvarBool(const char* name, bool value, bool track = true);
bool SetCvarNumber(const char* name, double value, bool track = true);  // formats by the cvar's type (integer or decimal)

// Back to the app default (the compiled one, or the PC profile's or a tier's) and source "default"; counts as a change for Save.
void ResetCvar(const char* name, bool track = true);
// The value differs from the app default.
bool IsNonDefault(const char* name);

// Where the value came from, in words for the badge: "default", "config file", "environment", "launcher / command line", "changed here".
const char* SourceLabel(const char* name);

// The lifecycle of the cvar as the player needs it: 0 = applies at once, 1 = applies at the next start. -1 = no such cvar.
int ApplyMode(const char* name);

// Changes made through the menu (and the overlay's own hotkeys) that Save would write.
const std::set<std::string>& ChangedCvars();
size_t SavableChangeCount();
void NoteChanged(const char* name);

// ---- the live detail preset -------------------------------------------------------------------------------------------------
// The name mcla_gfx_preset holds now, as one of kPresets (empty -> "default").
std::string CurrentPresetName();
// Sets mcla_gfx_preset and the knobs' app defaults to the preset's (like the real preset does at first use). Returns how many
// knobs it set, or -1 when the preset is not known to this build. With `adaptive` (mcla_gov is on: the governor owns the tier and
// writes the knob defaults itself) no knob is written: the preset's tier becomes the governor's floor (mcla_gov_tier_floor) and 0
// is returned.
int ApplyDetailPreset(const char* name, bool adaptive);
// The tier a preset corresponds to (mcla_gfx_tune.h: perf 2, perf+ and lowest 1, quality and as shipped 4, ultra 5); -1 = unknown.
int PresetFloorTier(const char* name);
// Every detail knob back to its app default: a value set by hand no longer pins it, so adaptive detail may move it again.
void ReleaseDetailKnobs();
// The cvars of the detail knobs (for the release button and the "yours" count).
const std::vector<const char*>& DetailKnobCvars();
size_t DetailKnobsSetByHand();

// ---- cheats -------------------------------------------------------------------------------------------------------------------
const std::vector<const char*>& CheatCvars();
bool AnyCheatActive();
// Every cheat cvar back to its off value (-1, or the game's own).
void ResetCheats();

// ---- Save ---------------------------------------------------------------------------------------------------------------------
struct SaveReport {
  size_t saved = 0;               // settings written
  bool toml_ok = false;           // mcla_x86.toml written
  bool launcher_present = false;  // launcher.json exists
  bool launcher_ok = false;       // ... and was updated
  std::string message;            // one or two sentences for the menu's status line
};

// Writes the changed settings (see the file comment). `config_path` = <exe dir>/mcla_x86.toml. Clears the change list when at
// least the TOML was written.
SaveReport SaveSettings(const std::filesystem::path& config_path);

// %LOCALAPPDATA%\mcla_x86\launcher.json (the launcher's own rule), empty when there is no profile folder.
std::filesystem::path LauncherSettingsPath();

// For the unit test and the status line: the launcher.json update a set of cvar names would make, without touching a file.
// `read(name)` gives the cvar's type and text. Returns the merged JSON text, or an empty string and *error.
struct CvarText {
  int type = 0;  // 0 bool, 1 integer, 2 decimal, 3 string
  std::string text;
};
std::string BuildLauncherJson(std::string_view existing, const std::vector<std::pair<std::string, CvarText>>& values,
                              std::string* error);

}  // namespace mcla_overlay
