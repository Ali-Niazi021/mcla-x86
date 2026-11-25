#include "core/settings_schema.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include "core/game_process.h"
#include "core/text.h"

namespace launcher {

const char* SettingTypeName(SettingType type) {
  switch (type) {
    case SettingType::Bool: return "bool";
    case SettingType::Int: return "int";
    case SettingType::Float: return "float";
    case SettingType::Enum: return "enum";
    case SettingType::String: return "string";
    case SettingType::Path: return "path";
  }
  return "?";
}

const char* EmitModeName(EmitMode mode) {
  switch (mode) {
    case EmitMode::NonDefault: return "nondefault";
    case EmitMode::Always: return "always";
    case EmitMode::Never: return "never";
  }
  return "?";
}

const Category* Schema::FindCategory(std::string_view id) const {
  for (const Category& c : categories)
    if (c.id == id) return &c;
  return nullptr;
}

const SettingDef* Schema::FindSetting(std::string_view id) const {
  for (const SettingDef& s : settings)
    if (s.id == id) return &s;
  return nullptr;
}

const Preset* Schema::FindPreset(std::string_view id) const {
  for (const Preset& p : presets)
    if (p.id == id) return &p;
  return nullptr;
}

std::filesystem::path DefaultSchemaPath() { return LauncherDir() / L"settings.json"; }

namespace {

const char* KindOf(const Json& j) {
  switch (j.type()) {
    case Json::value_t::null: return "null";
    case Json::value_t::boolean: return "a boolean";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned: return "an integer";
    case Json::value_t::number_float: return "a number";
    case Json::value_t::string: return "a string";
    case Json::value_t::array: return "an array";
    case Json::value_t::object: return "an object";
    default: return "?";
  }
}

// `value` as it should read in a message: its JSON text, shortened.
std::string Show(const Json& j) {
  std::string s = j.dump(-1, ' ', false, Json::error_handler_t::replace);
  if (s.size() > 60) s = s.substr(0, 57) + "...";
  return s;
}

// A bound as the shortest text that reads back: 240, not 240.0.
std::string NumberText(double d) {
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof(buf), d);
  return res.ec == std::errc() ? std::string(buf, res.ptr) : std::to_string(d);
}

bool IsIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

bool IsValidId(const std::string& s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (!IsIdChar(c)) return false;
  return true;
}

bool IsValidCvarName(const std::string& s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (!IsIdChar(c) || c == '.' || c == '-') return false;
  return true;
}

struct Report {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  void Error(const std::string& where, const std::string& what) { errors.push_back(where + ": " + what); }
  void Warn(const std::string& where, const std::string& what) { warnings.push_back(where + ": " + what); }
};

// Keys starting with '_' or '$' ("_comment", "$schema") are the author's own and never reported.
void WarnUnknownKeys(Report& r, const std::string& where, const Json& obj, std::initializer_list<const char*> known) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const std::string& key = it.key();
    if (!key.empty() && (key[0] == '_' || key[0] == '$')) continue;
    bool found = false;
    for (const char* k : known)
      if (key == k) found = true;
    if (!found) r.Warn(where, "unknown key \"" + key + "\" ignored");
  }
}

// obj[key] as a string. Returns true when present and a string; reports a wrong type, and absence when `required`.
bool ReadString(Report& r, const std::string& where, const Json& obj, const char* key, bool required, std::string& out) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    if (required) r.Error(where, std::string("missing \"") + key + "\"");
    return false;
  }
  if (!it->is_string()) {
    r.Error(where, std::string("\"") + key + "\" must be a string, got " + KindOf(*it));
    return false;
  }
  out = it->get<std::string>();
  return true;
}

bool ReadLabel(Report& r, const std::string& where, const Json& obj, std::string& out) {
  if (!ReadString(r, where, obj, "label", true, out)) return false;
  if (out.empty()) {
    r.Error(where, "\"label\" is empty");
    return false;
  }
  return true;
}

std::string Where(const char* list, size_t index, const std::string& id) {
  std::string w = std::string(list) + "[" + std::to_string(index) + "]";
  if (!id.empty()) w += " '" + id + "'";
  return w;
}

bool ParseType(const std::string& s, SettingType& out) {
  static const std::pair<const char*, SettingType> kTypes[] = {
      {"bool", SettingType::Bool}, {"int", SettingType::Int},       {"float", SettingType::Float},
      {"enum", SettingType::Enum}, {"string", SettingType::String}, {"path", SettingType::Path}};
  for (const auto& [name, type] : kTypes) {
    if (s == name) {
      out = type;
      return true;
    }
  }
  return false;
}

bool ParseEmit(const std::string& s, EmitMode& out) {
  if (s == "nondefault") out = EmitMode::NonDefault;
  else if (s == "always") out = EmitMode::Always;
  else if (s == "never") out = EmitMode::Never;
  else return false;
  return true;
}

bool IsScalar(const Json& j) { return j.is_boolean() || j.is_number() || j.is_string(); }

// The numeric key `key` of an Int / Float setting. Int settings need integers.
bool ReadNumber(Report& r, const std::string& where, const Json& obj, const char* key, bool integer, double& out) {
  const auto it = obj.find(key);
  if (it == obj.end()) return false;
  if (integer ? !it->is_number_integer() : !it->is_number()) {
    r.Error(where, std::string("\"") + key + "\" must be " + (integer ? "an integer" : "a number") + ", got " +
                       KindOf(*it));
    return false;
  }
  out = it->get<double>();
  return true;
}

void ParseOptions(Report& r, const std::string& where, const Json& j, SettingDef& d) {
  const auto it = j.find("options");
  if (it == j.end()) {
    r.Error(where, "an enum needs \"options\"");
    return;
  }
  if (!it->is_array() || it->empty()) {
    r.Error(where, "\"options\" must be a non-empty array");
    return;
  }
  size_t index = 0;
  for (const Json& o : *it) {
    const std::string ow = where + " options[" + std::to_string(index++) + "]";
    if (!o.is_object()) {
      r.Error(ow, std::string("must be an object, got ") + KindOf(o));
      continue;
    }
    WarnUnknownKeys(r, ow, o, {"value", "label", "help"});
    Option opt;
    if (!o.contains("value")) {
      r.Error(ow, "missing \"value\"");
    } else if (!IsScalar(o["value"])) {
      r.Error(ow, std::string("\"value\" must be a string, number or boolean, got ") + KindOf(o["value"]));
    } else {
      opt.value = o["value"];
    }
    ReadLabel(r, ow, o, opt.label);
    ReadString(r, ow, o, "help", false, opt.help);
    if (!opt.value.is_null()) {
      for (const Option& prev : d.options) {
        if (prev.value == opt.value) {
          r.Error(ow, "duplicate value " + Show(opt.value));
          break;
        }
      }
    }
    d.options.push_back(std::move(opt));
  }
}

void ParseSetting(Report& r, size_t index, const Json& j, const Schema& schema, SettingDef& d, bool& id_ok) {
  id_ok = false;
  std::string where = Where("settings", index, "");
  if (!j.is_object()) {
    r.Error(where, std::string("must be an object, got ") + KindOf(j));
    return;
  }
  if (ReadString(r, where, j, "id", true, d.id)) {
    if (!IsValidId(d.id)) {
      r.Error(where, "\"id\" " + Show(d.id) + " must be made of letters, digits, '_', '.' and '-'");
    } else {
      id_ok = true;
    }
    where = Where("settings", index, d.id);
  }
  WarnUnknownKeys(r, where, j, {"id", "label", "help", "category", "type", "cvar", "default", "options", "min", "max",
                                "step", "advanced", "requires", "emit"});

  ReadLabel(r, where, j, d.label);
  ReadString(r, where, j, "help", false, d.help);

  if (ReadString(r, where, j, "category", true, d.category) && !schema.FindCategory(d.category))
    r.Error(where, "unknown category \"" + d.category + "\"");

  // cvar: absent / null = launcher-only.
  if (const auto it = j.find("cvar"); it != j.end() && !it->is_null()) {
    if (!it->is_string()) {
      r.Error(where, std::string("\"cvar\" must be a string or null, got ") + KindOf(*it));
    } else if (!IsValidCvarName(it->get<std::string>())) {
      r.Error(where, "\"cvar\" " + Show(*it) + " must be letters, digits and '_' (null for a launcher-only setting)");
    } else {
      d.cvar = it->get<std::string>();
    }
  }

  if (const auto it = j.find("advanced"); it != j.end()) {
    if (!it->is_boolean()) r.Error(where, std::string("\"advanced\" must be true or false, got ") + KindOf(*it));
    else d.advanced = it->get<bool>();
  }

  bool explicit_always = false;
  if (std::string emit; ReadString(r, where, j, "emit", false, emit)) {
    if (!ParseEmit(emit, d.emit)) r.Error(where, "\"emit\" must be \"nondefault\", \"always\" or \"never\", got " + Show(emit));
    explicit_always = d.emit == EmitMode::Always;
  }
  if (explicit_always && !d.cvar) r.Warn(where, "\"emit\": \"always\" has no effect on a setting without a cvar");

  std::string type_name;
  bool type_ok = false;
  if (ReadString(r, where, j, "type", true, type_name)) {
    type_ok = ParseType(type_name, d.type);
    if (!type_ok) r.Error(where, "unknown type " + Show(type_name) + " (bool, int, float, enum, string or path)");
  }

  if (type_ok) {
    const bool numeric = d.type == SettingType::Int || d.type == SettingType::Float;
    if (numeric) {
      const bool integer = d.type == SettingType::Int;
      const bool has_min = ReadNumber(r, where, j, "min", integer, d.min);
      const bool has_max = ReadNumber(r, where, j, "max", integer, d.max);
      if (!j.contains("min")) r.Error(where, std::string(SettingTypeName(d.type)) + " needs \"min\"");
      if (!j.contains("max")) r.Error(where, std::string(SettingTypeName(d.type)) + " needs \"max\"");
      if (has_min && has_max && d.min > d.max) r.Error(where, "\"min\" is greater than \"max\"");
      // A missing or wrong bound was reported; do not also fail the default against it.
      if (!has_min) d.min = -HUGE_VAL;
      if (!has_max) d.max = HUGE_VAL;
      if (ReadNumber(r, where, j, "step", integer, d.step) && d.step < 0) r.Error(where, "\"step\" must not be negative");
    } else {
      for (const char* key : {"min", "max", "step"})
        if (j.contains(key)) r.Warn(where, std::string("\"") + key + "\" is only used by int and float settings");
    }
    if (d.type == SettingType::Enum) {
      ParseOptions(r, where, j, d);
    } else if (j.contains("options")) {
      r.Warn(where, "\"options\" is only used by enum settings");
    }
  }

  if (!j.contains("default")) {
    r.Error(where, "missing \"default\"");
  } else if (type_ok) {
    std::string why;
    if (CheckValue(d, j["default"], &why)) d.default_value = CanonicalValue(d, j["default"]);
    else r.Error(where, "default " + Show(j["default"]) + ": " + why);
  }
  if (type_ok && d.type == SettingType::Enum && d.default_value.is_string() && !d.default_value.get<std::string>().empty()) {
    for (const Option& o : d.options) {
      if (o.value.is_string() && o.value.get<std::string>().empty()) {
        r.Warn(where, "the option with the empty value \"\" passes nothing to the game (an empty value cannot be given on the command line), which is not the default");
        break;
      }
    }
  }

  if (const auto it = j.find("requires"); it != j.end()) {
    if (!it->is_object()) {
      r.Error(where, std::string("\"requires\" must be an object, got ") + KindOf(*it));
    } else {
      for (auto rq = it->begin(); rq != it->end(); ++rq) d.requires_.emplace(rq.key(), rq.value());
    }
  }
}

// Second pass: `requires` of every setting (needs all settings known), and cycles.
void ResolveRequires(Report& r, Schema& schema) {
  for (size_t i = 0; i < schema.settings.size(); ++i) {
    SettingDef& d = schema.settings[i];
    const std::string where = Where("settings", i, d.id);
    for (auto& [dep_id, want] : d.requires_) {
      const SettingDef* dep = schema.FindSetting(dep_id);
      if (!dep) {
        r.Error(where, "\"requires\" names unknown setting \"" + dep_id + "\"");
        continue;
      }
      if (dep == &d) {
        r.Error(where, "\"requires\" names the setting itself");
        continue;
      }
      std::string why;
      if (!CheckValue(*dep, want, &why)) {
        r.Error(where, "\"requires\" \"" + dep_id + "\": " + Show(want) + ": " + why);
        continue;
      }
      want = CanonicalValue(*dep, want);
    }
  }

  // Cycle search: 0 = unvisited, 1 = on the current path, 2 = done.
  std::map<std::string, int> state;
  std::vector<std::string> path;
  struct Dfs {
    Report& r;
    Schema& schema;
    std::map<std::string, int>& state;
    std::vector<std::string>& path;
    void Visit(const SettingDef& d) {
      int& s = state[d.id];
      if (s == 2) return;
      if (s == 1) {
        std::string cycle;
        bool in_cycle = false;
        for (const std::string& id : path) {
          if (id == d.id) in_cycle = true;
          if (in_cycle) cycle += id + " -> ";
        }
        r.Error("settings", "\"requires\" cycle: " + cycle + d.id);
        return;
      }
      s = 1;
      path.push_back(d.id);
      for (const auto& [dep_id, want] : d.requires_) {
        (void)want;
        if (const SettingDef* dep = schema.FindSetting(dep_id); dep && dep != &d) Visit(*dep);
      }
      path.pop_back();
      state[d.id] = 2;
    }
  } dfs{r, schema, state, path};
  for (const SettingDef& d : schema.settings) dfs.Visit(d);
}

void ParsePresets(Report& r, const Json& arr, Schema& schema) {
  std::set<std::string> seen;
  size_t index = 0;
  for (const Json& j : arr) {
    std::string where = Where("presets", index++, "");
    if (!j.is_object()) {
      r.Error(where, std::string("must be an object, got ") + KindOf(j));
      continue;
    }
    Preset p;
    if (ReadString(r, where, j, "id", true, p.id)) {
      if (!IsValidId(p.id)) r.Error(where, "\"id\" " + Show(p.id) + " must be made of letters, digits, '_', '.' and '-'");
      else if (!seen.insert(p.id).second) r.Error(where, "duplicate preset id \"" + p.id + "\"");
      where = Where("presets", index - 1, p.id);
    }
    WarnUnknownKeys(r, where, j, {"id", "label", "help", "set"});
    ReadLabel(r, where, j, p.label);
    ReadString(r, where, j, "help", false, p.help);

    const auto set = j.find("set");
    if (set == j.end() || !set->is_object()) {
      r.Error(where, set == j.end() ? "missing \"set\"" : std::string("\"set\" must be an object, got ") + KindOf(*set));
    } else {
      if (set->empty()) r.Warn(where, "\"set\" is empty");
      for (auto it = set->begin(); it != set->end(); ++it) {
        const SettingDef* def = schema.FindSetting(it.key());
        if (!def) {
          r.Error(where, "\"set\" names unknown setting \"" + it.key() + "\"");
          continue;
        }
        std::string why;
        if (!CheckValue(*def, it.value(), &why)) {
          r.Error(where, "\"set\" \"" + it.key() + "\": " + Show(it.value()) + ": " + why);
          continue;
        }
        p.set.emplace(it.key(), CanonicalValue(*def, it.value()));
      }
    }
    schema.presets.push_back(std::move(p));
  }
}

std::string JoinErrors(const std::string& source, const std::vector<std::string>& errors) {
  constexpr size_t kMaxShown = 40;
  std::string out = source + ": invalid schema, " + std::to_string(errors.size()) + " problem" +
                    (errors.size() == 1 ? "" : "s") + ":";
  for (size_t i = 0; i < errors.size() && i < kMaxShown; ++i) out += "\n  " + errors[i];
  if (errors.size() > kMaxShown) out += "\n  ... and " + std::to_string(errors.size() - kMaxShown) + " more";
  return out;
}

Schema ParseSchemaImpl(std::string_view text, std::string* error, const std::string& source) {
  Json doc;
  try {
    doc = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
  } catch (const Json::parse_error& e) {
    if (error) *error = source + ": not valid JSON: " + e.what();
    return {};
  }

  Report r;
  Schema schema;
  if (!doc.is_object()) {
    if (error) *error = source + ": the top level must be a JSON object";
    return {};
  }
  WarnUnknownKeys(r, "schema", doc, {"version", "categories", "presets", "settings"});

  int version = 0;
  if (const auto it = doc.find("version"); it == doc.end()) {
    r.Error("schema", "missing \"version\"");
  } else if (!it->is_number_integer()) {
    r.Error("schema", std::string("\"version\" must be an integer, got ") + KindOf(*it));
  } else if (it->get<int>() != kSchemaVersion) {
    r.Error("schema", "version " + Show(*it) + " is not supported (this launcher reads version " +
                          std::to_string(kSchemaVersion) + ")");
  } else {
    version = kSchemaVersion;
  }

  // Categories.
  if (const auto it = doc.find("categories"); it == doc.end() || !it->is_array()) {
    r.Error("schema", it == doc.end() ? "missing \"categories\"" : "\"categories\" must be an array");
  } else {
    std::set<std::string> seen;
    size_t index = 0;
    for (const Json& j : *it) {
      std::string where = Where("categories", index++, "");
      if (!j.is_object()) {
        r.Error(where, std::string("must be an object, got ") + KindOf(j));
        continue;
      }
      Category c;
      if (ReadString(r, where, j, "id", true, c.id)) {
        if (!IsValidId(c.id)) r.Error(where, "\"id\" " + Show(c.id) + " must be made of letters, digits, '_', '.' and '-'");
        else if (!seen.insert(c.id).second) r.Error(where, "duplicate category id \"" + c.id + "\"");
        where = Where("categories", index - 1, c.id);
      }
      WarnUnknownKeys(r, where, j, {"id", "label"});
      ReadLabel(r, where, j, c.label);
      schema.categories.push_back(std::move(c));
    }
    if (schema.categories.empty()) r.Error("schema", "\"categories\" is empty");
  }

  // Settings.
  if (const auto it = doc.find("settings"); it == doc.end() || !it->is_array()) {
    r.Error("schema", it == doc.end() ? "missing \"settings\"" : "\"settings\" must be an array");
  } else {
    std::set<std::string> seen;
    size_t index = 0;
    for (const Json& j : *it) {
      SettingDef d;
      bool id_ok = false;
      ParseSetting(r, index, j, schema, d, id_ok);
      if (id_ok && !seen.insert(d.id).second) r.Error(Where("settings", index, d.id), "duplicate setting id");
      ++index;
      schema.settings.push_back(std::move(d));
    }
    if (schema.settings.empty()) r.Error("schema", "\"settings\" is empty");
  }

  ResolveRequires(r, schema);

  // A cvar behind two settings is only fine when each one is conditional (see the `requires` of both).
  for (size_t i = 0; i < schema.settings.size(); ++i) {
    for (size_t k = i + 1; k < schema.settings.size(); ++k) {
      const SettingDef& a = schema.settings[i];
      const SettingDef& b = schema.settings[k];
      if (a.cvar && b.cvar && *a.cvar == *b.cvar && (a.requires_.empty() || b.requires_.empty()))
        r.Error(Where("settings", k, b.id), "cvar \"" + *b.cvar + "\" is also set by '" + a.id +
                                                "'; give both a \"requires\" so only one is active");
    }
  }

  if (const auto it = doc.find("presets"); it != doc.end()) {
    if (!it->is_array()) r.Error("schema", "\"presets\" must be an array");
    else ParsePresets(r, *it, schema);
  }

  if (!r.errors.empty()) {
    if (error) *error = JoinErrors(source, r.errors);
    return {};
  }
  schema.version = version;
  schema.warnings = std::move(r.warnings);
  if (error) error->clear();
  return schema;
}

}  // namespace

bool CheckValue(const SettingDef& def, const Json& value, std::string* why) {
  const auto fail = [&](std::string message) {
    if (why) *why = std::move(message);
    return false;
  };
  switch (def.type) {
    case SettingType::Bool:
      if (!value.is_boolean()) return fail(std::string("expected true or false, got ") + KindOf(value));
      return true;
    case SettingType::Int:
    case SettingType::Float: {
      const bool is_int = def.type == SettingType::Int;
      if (is_int ? !value.is_number_integer() : !value.is_number())
        return fail(std::string("expected ") + (is_int ? "an integer" : "a number") + ", got " + KindOf(value));
      const double v = value.get<double>();
      if (!std::isfinite(v)) return fail("not a finite number");
      if (v < def.min) return fail("below the minimum " + NumberText(def.min));
      if (v > def.max) return fail("above the maximum " + NumberText(def.max));
      return true;
    }
    case SettingType::Enum: {
      for (const Option& o : def.options)
        if (o.value == value) return true;
      std::string list;
      for (const Option& o : def.options) list += (list.empty() ? "" : ", ") + Show(o.value);
      return fail("not one of the options (" + list + ")");
    }
    case SettingType::String:
    case SettingType::Path:
      if (!value.is_string()) return fail(std::string("expected a string, got ") + KindOf(value));
      // An empty value cannot go on the command line (`--name=` makes the parser take the next argument as the value), so
      // for a setting that has a default, "" means "use the default".
      if (value.get<std::string>().empty() && def.default_value.is_string() && !def.default_value.get<std::string>().empty())
        return fail("empty; the default is used instead");
      return true;
  }
  return fail("unknown setting type");
}

Json CanonicalValue(const SettingDef& def, const Json& value) {
  if (def.type == SettingType::Float && value.is_number()) return Json(value.get<double>());
  if (def.type == SettingType::Int && value.is_number_integer()) return Json(value.get<std::int64_t>());
  return value;
}

Schema ParseSchema(std::string_view text, std::string* error, std::string_view source) {
  try {
    return ParseSchemaImpl(text, error, std::string(source));
  } catch (const std::exception& e) {
    if (error) *error = std::string(source) + ": internal error while reading the schema: " + e.what();
    return {};
  }
}

Schema LoadSchema(const std::filesystem::path& path, std::string* error) {
  const std::string source = PathToUtf8(path);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = source + ": cannot open the settings schema";
    return {};
  }
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) {
    if (error) *error = source + ": cannot read the settings schema";
    return {};
  }
  return ParseSchema(text.str(), error, source);
}

}  // namespace launcher
