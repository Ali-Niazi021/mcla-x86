// The launcher's settings schema (launcher/schema/settings.json): what the settings are, how the UI shows them, and
// which game cvar each one drives. The UI is generated from it and so is the command line (settings_store.h).
//
// File format (version 1; // and /* */ comments are accepted):
//
//   { "version": 1,
//     "categories": [ { "id": "display", "label": "Display" }, ... ],
//     "presets":    [ { "id": "...", "label": "...", "help": "...", "set": { "<setting id>": <value>, ... } }, ... ],
//     "settings":   [ { "id": "...", "label": "...", "help": "...", "category": "<category id>",
//                       "type": "bool|int|float|enum|string|path",
//                       "cvar": "<game cvar>" | null,            (null / absent: launcher-only, never on the command line)
//                       "default": <value>,
//                       "options": [ { "value": <v>, "label": "...", "help": "..." } ],     (enum only, required there)
//                       "min": <n>, "max": <n>, "step": <n>,                    (int / float: min and max required)
//                       "advanced": false,
//                       "requires": { "<setting id>": <value>, ... },                                        (optional)
//                       "emit": "nondefault" | "always" | "never" } ] }                         (default "nondefault")
//
// Validation is strict about things that would silently break the command line (missing or duplicate ids, unknown
// category, a default of the wrong type or outside min..max or not among the options, a preset or `requires` naming an
// unknown setting or an invalid value, `requires` cycles, a cvar named by two unconditional settings) and reports every
// problem found, one per line. Keys the loader does not know are not errors: they are listed in Schema::warnings.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace launcher {

using Json = nlohmann::json;

inline constexpr int kSchemaVersion = 1;

enum class SettingType { Bool, Int, Float, Enum, String, Path };
enum class EmitMode { NonDefault, Always, Never };

const char* SettingTypeName(SettingType type);  // "bool", "int", ...
const char* EmitModeName(EmitMode mode);        // "nondefault", "always", "never"

struct Category {
  std::string id;
  std::string label;
};

struct Option {
  Json value;
  std::string label;
  std::string help;
};

struct SettingDef {
  std::string id;
  std::string label;
  std::string help;
  std::string category;                // id of a Category
  SettingType type = SettingType::Bool;
  std::optional<std::string> cvar;     // nullopt: launcher-only
  Json default_value;                  // canonical: Bool boolean, Int integer, Float double, Enum one of the options, String/Path string
  std::vector<Option> options;         // Enum only
  double min = 0;                      // Int / Float only
  double max = 0;
  double step = 0;                     // Int / Float only; 0 = not given (the UI picks: 1 for Int)
  bool advanced = false;
  std::map<std::string, Json> requires_;  // `requires` of the file ("requires" is a C++20 keyword): all must hold
  EmitMode emit = EmitMode::NonDefault;
};

struct Preset {
  std::string id;
  std::string label;
  std::string help;
  std::map<std::string, Json> set;     // setting id -> value (canonical, valid for that setting)
};

struct Schema {
  int version = 0;                     // 0 = the schema failed to load
  std::vector<Category> categories;
  std::vector<SettingDef> settings;    // file order: the order of the UI and of the command line
  std::vector<Preset> presets;
  std::vector<std::string> warnings;   // non-fatal: unknown keys and the like

  const Category* FindCategory(std::string_view id) const;
  const SettingDef* FindSetting(std::string_view id) const;
  const Preset* FindPreset(std::string_view id) const;
};

// `settings.json` next to the running launcher (the build copies it there).
std::filesystem::path DefaultSchemaPath();

// Loads and validates a schema file. On any problem returns an empty Schema (version 0) and sets *error (when given) to
// the problems, one per line; on success *error is cleared.
Schema LoadSchema(const std::filesystem::path& path, std::string* error);

// The same from JSON text; `source` names the text in error messages.
Schema ParseSchema(std::string_view text, std::string* error, std::string_view source = "schema");

// True when `value` is a valid value of `def`: right JSON type, inside min..max, one of the options. Otherwise false and
// *why (when given) says what is wrong. An Int setting accepts integers only; a Float setting accepts any number.
bool CheckValue(const SettingDef& def, const Json& value, std::string* why = nullptr);

// The canonical JSON form of a valid value: a Float setting's number as double, an Int setting's as integer; every
// other type unchanged.
Json CanonicalValue(const SettingDef& def, const Json& value);

}  // namespace launcher
