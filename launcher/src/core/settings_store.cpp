#include "core/settings_store.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "core/game_process.h"
#include "core/text.h"

namespace launcher {

Values DefaultValues(const Schema& schema) {
  Values values;
  for (const SettingDef& def : schema.settings) values[def.id] = def.default_value;
  return values;
}

void ResetToDefaults(const Schema& schema, Values& values) {
  for (const SettingDef& def : schema.settings) values[def.id] = def.default_value;
}

bool ApplyPreset(const Schema& schema, std::string_view preset_id, Values& values, std::string* error) {
  const Preset* preset = schema.FindPreset(preset_id);
  if (!preset) {
    if (error) *error = "unknown preset \"" + std::string(preset_id) + "\"";
    return false;
  }
  for (const auto& [id, value] : preset->set) values[id] = value;
  if (error) error->clear();
  return true;
}

bool SameValue(const Json& a, const Json& b) {
  if (a.is_number_integer() && b.is_number_integer()) return a.get<std::int64_t>() == b.get<std::int64_t>();
  if (a.is_number() && b.is_number()) {
    const double x = a.get<double>();
    const double y = b.get<double>();
    return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::fabs(y));
  }
  return a == b;
}

const Preset* MatchingPreset(const Schema& schema, const Values& values) {
  for (const Preset& preset : schema.presets) {
    if (preset.set.empty()) continue;
    bool all = true;
    for (const auto& [id, want] : preset.set) {
      const SettingDef* def = schema.FindSetting(id);
      if (!def || !SameValue(EffectiveValue(*def, values), want)) {
        all = false;
        break;
      }
    }
    if (all) return &preset;
  }
  return nullptr;
}

const Json& EffectiveValue(const SettingDef& def, const Values& values) {
  const auto it = values.find(def.id);
  if (it != values.end() && CheckValue(def, it->second)) return it->second;
  return def.default_value;
}

namespace {

bool IsActiveAt(const Schema& schema, const Values& values, const SettingDef& def, int depth) {
  if (def.requires_.empty()) return true;
  if (depth > 32) return false;  // a schema from LoadSchema has no cycles; a hand-built one must not hang the UI
  for (const auto& [dep_id, want] : def.requires_) {
    const SettingDef* dep = schema.FindSetting(dep_id);
    if (!dep || !IsActiveAt(schema, values, *dep, depth + 1)) return false;
    const Json& have = EffectiveValue(*dep, values);
    if (want.is_array()) {  // any of the listed values
      bool any = false;
      for (const Json& one : want) any = any || SameValue(have, one);
      if (!any) return false;
    } else if (!SameValue(have, want)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool IsActive(const Schema& schema, const Values& values, const SettingDef& def) {
  return IsActiveAt(schema, values, def, 0);
}

std::string FormatCvarValue(const Json& value) {
  switch (value.type()) {
    case Json::value_t::boolean:
      return value.get<bool>() ? "true" : "false";
    case Json::value_t::number_integer:
      return std::to_string(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
      return std::to_string(value.get<std::uint64_t>());
    case Json::value_t::number_float: {
      char buf[64];
      const auto res = std::to_chars(buf, buf + sizeof(buf), value.get<double>());  // shortest round-trip
      return res.ec == std::errc() ? std::string(buf, res.ptr) : value.dump();
    }
    case Json::value_t::string:
      return value.get<std::string>();
    default:
      return value.dump(-1, ' ', false, Json::error_handler_t::replace);
  }
}

namespace {

std::string TrimBlanks(std::string s) {
  const auto blank = [](char c) { return c == ' ' || c == '\t'; };
  while (!s.empty() && blank(s.back())) s.pop_back();
  size_t first = 0;
  while (first < s.size() && blank(s[first])) ++first;
  return s.substr(first);
}

// A string or path setting whose stored value is "" while its default is not: CheckValue refuses that value, so the
// default is what BuildArgs uses.
bool EmptyOverDefault(const SettingDef& def, const Values& values) {
  if (def.type != SettingType::String && def.type != SettingType::Path) return false;
  const auto it = values.find(def.id);
  return it != values.end() && it->second.is_string() && it->second.get<std::string>().empty() &&
         def.default_value.is_string() && !def.default_value.get<std::string>().empty();
}

// The text of a value on the command line: a path setting's value as an absolute, normalised path.
std::string CvarText(const SettingDef& def, const Json& value, const std::filesystem::path& base_dir) {
  std::string text = FormatCvarValue(value);
  if (def.type != SettingType::Path) return text;
  text = TrimBlanks(std::move(text));
  if (text.empty()) return text;
  return PathToUtf8(ResolvePath(PathFromUtf8(text), base_dir));
}

}  // namespace

std::vector<std::wstring> BuildArgs(const Schema& schema, const Values& values, std::vector<std::string>* notes,
                                    const std::filesystem::path& base_dir) {
  const auto note = [&](std::string message) {
    if (notes) notes->push_back(std::move(message));
  };
  std::vector<std::wstring> args;
  for (const SettingDef& def : schema.settings) {
    if (!def.cvar || def.cvar->empty() || def.emit == EmitMode::Never) continue;
    if (!IsActive(schema, values, def)) continue;
    const Json& value = EffectiveValue(def, values);
    if (EmptyOverDefault(def, values)) note("\"" + def.label + "\" is empty: its default is used.");
    if (def.emit != EmitMode::Always && SameValue(value, def.default_value)) continue;
    const std::string text = CvarText(def, value, base_dir);
    if (text.empty()) {  // `--name=` would make the game's parser take the next argument as the value
      note("\"" + def.label + "\" is empty: it is not passed to the game.");
      continue;
    }
    args.push_back(L"--" + Utf8ToWide(*def.cvar) + L"=" + Utf8ToWide(text));
  }
  return args;
}

std::vector<PathProblem> CheckPathSettings(const Schema& schema, const Values& values, const std::filesystem::path& game_exe,
                                           const std::filesystem::path& base_dir) {
  std::vector<PathProblem> problems;
  for (const SettingDef& def : schema.settings) {
    if (def.type != SettingType::Path || !def.cvar || def.cvar->empty() || def.emit == EmitMode::Never) continue;
    if (!IsActive(schema, values, def)) continue;
    const std::string text = TrimBlanks(FormatCvarValue(EffectiveValue(def, values)));
    if (text.empty()) continue;
    const std::string problem = NonAsciiPathProblem(ResolvePath(PathFromUtf8(text), base_dir), game_exe);
    if (!problem.empty()) problems.push_back({def.id, def.label + ": " + problem});
  }
  return problems;
}

std::filesystem::path DefaultSettingsPath() {
  std::filesystem::path base;
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    base = buf;
  } else {
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known)) && known) base = known;
    CoTaskMemFree(known);
  }
  if (base.empty()) return LauncherDir() / L"launcher.json";  // no profile folder: keep the file with the program
  return base / L"mcla_x86" / L"launcher.json";
}

namespace {

bool ReadFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) return false;
  out = text.str();
  return true;
}

// A stored value that is not valid as it is: the nearest valid one, or null when there is none. `what` says what was done.
Json Repair(const SettingDef& def, const Json& v, std::string& what) {
  const bool numeric = def.type == SettingType::Int || def.type == SettingType::Float;
  if (!numeric || !v.is_number()) return Json();
  double d = v.get<double>();
  if (!std::isfinite(d)) return Json();
  if (def.type == SettingType::Int) {
    if (d != std::floor(d)) return Json();  // 2.5 is not a whole number: no honest repair
  }
  const double clamped = std::clamp(d, def.min, def.max);
  Json fixed = def.type == SettingType::Int ? Json(static_cast<std::int64_t>(clamped)) : Json(clamped);
  if (!CheckValue(def, fixed)) return Json();
  what = clamped != d ? "clamped to " + FormatCvarValue(fixed) : "";
  return fixed;
}

void MoveAside(const std::filesystem::path& path, std::vector<std::string>* notes, const std::string& why) {
  std::filesystem::path corrupt = path;
  corrupt += L".corrupt";
  std::error_code ec;
  std::filesystem::rename(path, corrupt, ec);
  if (notes) {
    notes->push_back(PathToUtf8(path) + ": " + why + (ec ? "; using the defaults" : "; moved to " + PathToUtf8(corrupt) +
                                                                                       ", using the defaults"));
  }
}

}  // namespace

Values LoadValues(const Schema& schema, const std::filesystem::path& path, std::vector<std::string>* notes) {
  Values values = DefaultValues(schema);
  const auto note = [&](std::string s) {
    if (notes) notes->push_back(std::move(s));
  };

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return values;

  std::string text;
  if (!ReadFile(path, text)) {
    note(PathToUtf8(path) + ": cannot read the settings file; using the defaults");
    return values;
  }

  Json doc;
  try {
    doc = Json::parse(text);
  } catch (const Json::parse_error& e) {
    MoveAside(path, notes, std::string("not valid JSON (") + e.what() + ")");
    return values;
  }
  const auto stored = doc.is_object() ? doc.find("values") : doc.end();
  if (!doc.is_object() || stored == doc.end() || !stored->is_object()) {
    MoveAside(path, notes, "not a launcher settings file (no \"values\" object)");
    return values;
  }

  if (const auto version = doc.find("version"); version == doc.end() || !version->is_number_integer()) {
    note(PathToUtf8(path) + ": no version number; read as version " + std::to_string(kSettingsFileVersion));
  } else if (version->get<std::int64_t>() > kSettingsFileVersion) {
    note(PathToUtf8(path) + ": written by a newer launcher (version " + std::to_string(version->get<std::int64_t>()) +
         "); settings this launcher does not know are kept");
  }

  for (auto it = stored->begin(); it != stored->end(); ++it) {
    const SettingDef* def = schema.FindSetting(it.key());
    if (!def) {
      values[it.key()] = it.value();  // a removed or future setting: kept for the next save
      continue;
    }
    std::string why;
    if (CheckValue(*def, it.value(), &why)) {
      values[it.key()] = CanonicalValue(*def, it.value());
      continue;
    }
    // An integral number written as float (3.0) for an Int setting is the same value.
    Json candidate = it.value();
    if (def->type == SettingType::Int && candidate.is_number_float() && std::isfinite(candidate.get<double>()) &&
        candidate.get<double>() == std::floor(candidate.get<double>()))
      candidate = Json(static_cast<std::int64_t>(candidate.get<double>()));
    if (CheckValue(*def, candidate)) {
      values[it.key()] = CanonicalValue(*def, candidate);
      continue;
    }
    std::string what;
    const Json fixed = Repair(*def, it.value(), what);
    if (!fixed.is_null()) {
      values[it.key()] = fixed;
      note("setting '" + def->id + "': stored value " + FormatCvarValue(it.value()) + " is " + why + "; " + what);
    } else {
      note("setting '" + def->id + "': stored value " + it.value().dump(-1, ' ', false, Json::error_handler_t::replace) +
           ": " + why + "; using the default");
    }
  }
  return values;
}

bool SaveValues(const Schema& schema, const Values& values, const std::filesystem::path& path, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = PathToUtf8(path) + ": " + std::move(message);
    return false;
  };

  // Start from what is on disk so that keys this launcher does not know (and a newer file version) survive.
  Json doc = Json::object();
  std::int64_t version = kSettingsFileVersion;
  if (std::string old_text; ReadFile(path, old_text)) {
    const Json old = Json::parse(old_text, nullptr, /*allow_exceptions=*/false);
    if (old.is_object()) {
      doc = old;
      if (const auto v = old.find("version"); v != old.end() && v->is_number_integer())
        version = std::max<std::int64_t>(version, v->get<std::int64_t>());
    }
  }
  doc["version"] = version;

  Json out_values = Json::object();
  for (const SettingDef& def : schema.settings) out_values[def.id] = EffectiveValue(def, values);
  for (const auto& [id, value] : values)
    if (!schema.FindSetting(id)) out_values[id] = value;  // unknown ids are carried along
  doc["values"] = std::move(out_values);

  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail("cannot create the folder: " + ec.message());
  }

  std::filesystem::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write " + PathToUtf8(tmp));
    out << doc.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      return fail("write failed (disk full?)");
    }
  }
  std::filesystem::rename(tmp, path, ec);  // replaces the old file
  if (ec) {
    const std::string message = "cannot replace the settings file: " + ec.message();
    std::filesystem::remove(tmp, ec);
    return fail(message);
  }
  if (error) error->clear();
  return true;
}

}  // namespace launcher
