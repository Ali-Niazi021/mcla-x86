// The launcher's current settings (a value per setting id), their file, presets, and the game command line they
// produce.
//
// File: %LOCALAPPDATA%\mcla_x86\launcher.json (the game's own user folder name),
//
//   { "version": 1, "values": { "<setting id>": <value>, ... } }
//
// Keys the launcher does not know survive a load and save: values of ids that are not in the schema (a removed or
// future setting) stay in Values, other top-level keys are carried over from the file on disk, and a file written by a
// newer launcher keeps its higher version number.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/settings_schema.h"

namespace launcher {

inline constexpr int kSettingsFileVersion = 1;

// Setting id -> value. A missing id means "the schema's default" everywhere (BuildArgs, IsActive, EffectiveValue).
using Values = std::map<std::string, Json>;

// Every schema setting at its default.
Values DefaultValues(const Schema& schema);

// Sets every schema setting to its default. Ids the schema does not know are left alone.
void ResetToDefaults(const Schema& schema, Values& values);

// Assigns the values of the preset's `set`; nothing else changes. False (and *error) for an unknown preset id.
bool ApplyPreset(const Schema& schema, std::string_view preset_id, Values& values, std::string* error = nullptr);

// The first preset (schema order) with a non-empty `set` whose entries all equal the current values, or nullptr (UI:
// "Custom").
const Preset* MatchingPreset(const Schema& schema, const Values& values);

// The value in effect for a setting: the stored one when it is valid for the setting, the default otherwise.
const Json& EffectiveValue(const SettingDef& def, const Values& values);

// Two values are the same setting value: numbers compare by value (1 == 1.0, floats within 1e-9 relative), everything
// else by JSON equality.
bool SameValue(const Json& a, const Json& b);

// The setting's `requires` all hold: each named setting is itself active and its effective value equals the one asked
// for (a chain of `requires` is followed). A hidden setting is never emitted and the UI should not show it.
bool IsActive(const Schema& schema, const Values& values, const SettingDef& def);

// A value as the game's cvar parser reads it: true/false, an integer, the shortest decimal that reads back the same
// double, or the string itself (UTF-8).
std::string FormatCvarValue(const Json& value);

// The game's command-line arguments (without the program name), one element per argument, raw (not quoted): for each
// setting in schema order that has a cvar, is not `emit: never` and is active, `--<cvar>=<value>` when `emit: always`
// or when the effective value differs from the default. Bools are true/false. Quote with FormatCommandLine (text.h);
// GameProcess::Launch does.
std::vector<std::wstring> BuildArgs(const Schema& schema, const Values& values);

// %LOCALAPPDATA%\mcla_x86\launcher.json
std::filesystem::path DefaultSettingsPath();

// Reads the settings file. Always returns a complete Values (every schema setting present). A missing file gives the
// defaults. Stored values that are invalid for their setting are replaced by the default (numbers outside min..max are
// clamped); each such repair, an unreadable or malformed file (moved aside to launcher.json.corrupt) and a newer file
// version are described in *notes (when given), one string each.
Values LoadValues(const Schema& schema, const std::filesystem::path& path, std::vector<std::string>* notes = nullptr);

// Writes the settings file (creating its folder; written to a temporary file and renamed over the old one). False and
// *error on failure.
bool SaveValues(const Schema& schema, const Values& values, const std::filesystem::path& path,
                std::string* error = nullptr);

}  // namespace launcher
