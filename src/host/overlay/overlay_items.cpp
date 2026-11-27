// mcla-x86 host overlay: reading, writing and saving the settings the menu shows. See overlay_items.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_gfx_tune.h"
#include "overlay_items.h"
#include "overlay_json.h"

namespace mcla_overlay {
namespace {

std::set<std::string> g_changed;  // UI thread only

// Never written by Save: the cheats (their cvars are the game's dev scene switches: a held hour or weather must not come back
// at the next start), the gate that unlocks them, and the run-time tier (transient anyway).
bool NeverSaved(const std::string& name) {
  return name.rfind("mcla_dev_", 0) == 0 || name == "mcla_overlay_cheats" || name == "mcla_gfx_tier";
}

const std::vector<const char*> kDetailKnobs = {
    "mcla_gfx_env_interval", "mcla_gfx_far_cascades_interval", "mcla_gfx_reflection_interval", "mcla_gfx_shadow_dist",
    "mcla_gfx_traffic_lod",  "mcla_gfx_city_lod",              "mcla_gfx_shadow_res",          "mcla_gfx_shadow_mode",
    "mcla_gfx_dl_disable",   "mcla_gfx_impostors",
};

const std::vector<const char*> kCheats = {
    "mcla_dev_tod",
    "mcla_dev_tod_freeze",
    "mcla_dev_weather",
    "mcla_dev_road_density",
};

}  // namespace

// ---- values ---------------------------------------------------------------------------------------------------------------

std::string CvarString(const char* name) { return rex::cvar::GetFlagByName(name); }

bool CvarBool(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

double CvarNumber(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  if (v.empty()) return 0.0;
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  return end == v.c_str() ? 0.0 : d;
}

void NoteChanged(const char* name) { g_changed.insert(name); }

bool SetCvar(const char* name, std::string_view value, bool track) {
  const bool ok = rex::cvar::SetFlagByName(name, value);
  if (ok && track) NoteChanged(name);
  return ok;
}

bool SetCvarBool(const char* name, bool value, bool track) { return SetCvar(name, value ? "true" : "false", track); }

bool SetCvarNumber(const char* name, double value, bool track) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  if (!info) return false;
  std::string text;
  switch (info->type) {
    case rex::cvar::FlagType::Int32:
    case rex::cvar::FlagType::Int64:
    case rex::cvar::FlagType::Uint32:
    case rex::cvar::FlagType::Uint64:
      text = std::to_string(static_cast<long long>(std::llround(value)));
      break;
    default:
      text = fmt::format("{}", value);  // the shortest text that reads back the same double
      break;
  }
  return SetCvar(name, text, track);
}

void ResetCvar(const char* name, bool track) {
  rex::cvar::ResetToDefault(name);
  if (track) NoteChanged(name);
}

bool IsNonDefault(const char* name) { return rex::cvar::HasNonDefaultValue(name); }

const char* SourceLabel(const char* name) {
  switch (rex::cvar::GetFlagSource(name)) {
    case rex::cvar::Source::kRuntime: return "changed here";
    case rex::cvar::Source::kCommandLine: return "launcher / command line";
    case rex::cvar::Source::kEnvironment: return "environment";
    case rex::cvar::Source::kConfig: return "config file";
    case rex::cvar::Source::kDefault: break;
  }
  return "default";
}

int ApplyMode(const char* name) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  if (!info) return -1;
  switch (info->lifecycle) {
    case rex::cvar::Lifecycle::kHotReload: return 0;
    case rex::cvar::Lifecycle::kRequiresRestart: return 1;
    case rex::cvar::Lifecycle::kInitOnly: return 2;
  }
  return 0;
}

const std::set<std::string>& ChangedCvars() { return g_changed; }

size_t SavableChangeCount() {
  size_t n = 0;
  for (const std::string& name : g_changed) {
    if (!NeverSaved(name)) ++n;
  }
  return n;
}

// ---- the live detail preset -----------------------------------------------------------------------------------------------

std::string CurrentPresetName() {
  const std::string value = rex::cvar::GetFlagByName("mcla_gfx_preset");
  return value.empty() ? "default" : value;
}

int PresetFloorTier(const char* name) {
  const std::string n = name ? name : "";
  if (n == "default" || n.empty() || n == "quality") return 4;
  if (n == "perf") return 2;
  if (n == "perfplus" || n == "lowest") return 1;
  if (n == "ultra") return 5;
  return -1;
}

int ApplyDetailPreset(const char* name, bool adaptive) {
  if (adaptive) {
    // The governor writes the knobs' defaults at every tier change and would overwrite the preset's: what it follows is the
    // floor (the tier it starts from and restores to) and, above it, the ceiling.
    const int tier = PresetFloorTier(name);
    if (tier < 0) return -1;
    SetCvar("mcla_gfx_preset", name);
    SetCvarNumber("mcla_gov_tier_floor", double(tier));
    return 0;
  }
  int applied = 0;
  const bool known = mcla::gfxtune::ForEachPresetKnob(
      name,
      [](const char* cvar, const char* value, void* user) {
        // As the real preset does it: the knob's app default. A knob the player has set keeps the player's value.
        if (rex::cvar::SetDefaultValue(cvar, value)) ++*static_cast<int*>(user);
      },
      &applied);
  if (!known) return -1;
  // What Save writes (and what a restart applies through the real preset): the preset's name, not every knob.
  SetCvar("mcla_gfx_preset", name);
  return applied;
}

void ReleaseDetailKnobs() {
  for (const char* name : kDetailKnobs) {
    rex::cvar::ResetToDefault(name);
    g_changed.erase(name);
  }
}

const std::vector<const char*>& DetailKnobCvars() { return kDetailKnobs; }

size_t DetailKnobsSetByHand() {
  size_t n = 0;
  for (const char* name : kDetailKnobs) {
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) ++n;
  }
  return n;
}

// ---- cheats ---------------------------------------------------------------------------------------------------------------

const std::vector<const char*>& CheatCvars() { return kCheats; }

bool AnyCheatActive() {
  for (const char* name : kCheats) {
    if (CvarNumber(name) >= 0.0) return true;  // -1 = off for every one of them
  }
  return false;
}

void ResetCheats() {
  for (const char* name : kCheats) {
    rex::cvar::ResetToDefault(name);  // back to -1: the hook writes the game's own values back on its next frame
    g_changed.erase(name);
  }
}

// ---- Save -----------------------------------------------------------------------------------------------------------------

std::filesystem::path LauncherSettingsPath() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return std::filesystem::path(buf) / L"mcla_x86" / L"launcher.json";
}

namespace {

// A decimal as JSON wants it: "0.600000" -> "0.6", "1.000000" -> "1". Anything that is not a plain number is "".
std::string JsonDecimal(std::string text) {
  if (text.empty()) return {};
  size_t digits = 0;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      ++digits;
    } else if (c != '-' && c != '.' && c != 'e' && c != 'E' && c != '+') {
      return {};
    }
  }
  if (digits == 0) return {};
  if (text.find('.') != std::string::npos && text.find('e') == std::string::npos && text.find('E') == std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  if (text.empty() || text == "-") return "0";
  return text;
}

json::Value ToJson(const CvarText& v, const std::string& name) {
  switch (v.type) {
    case 0: return json::Value::Bool(v.text == "true" || v.text == "1");
    case 1: return json::Value::Number(v.text);
    case 2: {
      const std::string number = JsonDecimal(v.text);
      return number.empty() ? json::Value::Null() : json::Value::Number(number);
    }
    default:
      // mcla_gfx_preset: the launcher cannot pass an empty value (`--name=` takes the next argument), "default" is its name for it.
      if (name == "mcla_gfx_preset" && v.text.empty()) return json::Value::String("default");
      return json::Value::String(v.text);
  }
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
}

// Writes next to the target and renames over it: a crash leaves the old file, not half a new one.
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    out.flush();
    if (!out) return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return false;
  }
  return true;
}

}  // namespace

std::string BuildLauncherJson(std::string_view existing, const std::vector<std::pair<std::string, CvarText>>& values,
                              std::string* error) {
  std::vector<std::pair<std::string, json::Value>> updates;
  for (const auto& entry : values) {
    const json::Value value = ToJson(entry.second, entry.first);
    if (value.type == json::Value::Type::kNull) continue;  // not representable: leave the launcher's value alone
    updates.emplace_back(entry.first, value);
    // Launcher-only companions: the settings whose value the launcher ignores until another one says it is the player's.
    if (entry.first == "swap_post_effect") {
      updates.emplace_back("aa_auto", json::Value::Bool(false));
    }
  }
  std::string merged;
  if (!json::MergeLauncherValues(existing, updates, &merged, error)) return {};
  return merged;
}

SaveReport SaveSettings(const std::filesystem::path& config_path) {
  SaveReport report;
  std::vector<std::string> names;
  for (const std::string& name : g_changed) {
    if (!NeverSaved(name) && rex::cvar::GetFlagInfo(name) != nullptr) names.push_back(name);
  }
  if (names.empty()) {
    report.message = "Nothing to save: no setting was changed here.";
    return report;
  }
  report.saved = names.size();

  // 1. The game's own config file.
  report.toml_ok = rex::cvar::SaveConfigValues(config_path, names);

  // 2. The launcher's settings, when there are any (the launcher passes them on the command line, which outranks the file above).
  const std::filesystem::path launcher = LauncherSettingsPath();
  std::string launcher_note;
  std::error_code ec;
  report.launcher_present = !launcher.empty() && std::filesystem::exists(launcher, ec);
  if (report.launcher_present) {
    std::vector<std::pair<std::string, CvarText>> values;
    for (const std::string& name : names) {
      const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
      if (!info) continue;
      CvarText v;
      v.text = info->getter();
      switch (info->type) {
        case rex::cvar::FlagType::Boolean: v.type = 0; break;
        case rex::cvar::FlagType::Int32:
        case rex::cvar::FlagType::Int64:
        case rex::cvar::FlagType::Uint32:
        case rex::cvar::FlagType::Uint64: v.type = 1; break;
        case rex::cvar::FlagType::Double: v.type = 2; break;
        default: v.type = 3; break;
      }
      values.emplace_back(name, std::move(v));
    }
    std::string existing;
    std::string error;
    if (!ReadFile(launcher, &existing)) {
      launcher_note = "launcher.json could not be read";
    } else {
      const std::string merged = BuildLauncherJson(existing, values, &error);
      if (merged.empty()) {
        launcher_note = error;
      } else if (!WriteFileAtomic(launcher, merged)) {
        launcher_note = "launcher.json could not be written";
      } else {
        report.launcher_ok = true;
      }
    }
  }

  if (report.toml_ok) {
    // What was written is no longer a pending change. A cheat stays in the list: it was never written.
    for (const std::string& name : names) g_changed.erase(name);
  }

  if (!report.toml_ok) {
    report.message = fmt::format("Could not write {}.", config_path.filename().string());
  } else if (report.launcher_ok) {
    report.message = fmt::format("Saved {} setting(s) to the launcher and to {}. If the launcher is open, close it and open it again.",
                                 report.saved, config_path.filename().string());
  } else if (report.launcher_present) {
    report.message = fmt::format("Saved {} setting(s) to {}, but not to the launcher ({}): the launcher's own values still win "
                                 "when you start from it.",
                                 report.saved, config_path.filename().string(), launcher_note);
  } else {
    report.message = fmt::format("Saved {} setting(s) to {} (no launcher settings file found: nothing else to update).",
                                 report.saved, config_path.filename().string());
  }
  REXLOG_INFO("[mcla-overlay] save: {} setting(s); toml {}, launcher.json {}{}", report.saved, report.toml_ok ? "ok" : "FAILED",
              report.launcher_ok ? "updated" : (report.launcher_present ? "NOT updated" : "absent"),
              launcher_note.empty() ? "" : fmt::format(" ({})", launcher_note));
  return report;
}

}  // namespace mcla_overlay
