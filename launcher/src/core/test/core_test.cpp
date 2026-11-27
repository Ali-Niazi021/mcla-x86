// Unit test of the launcher core (launcher/src/core): the schema loader and its error reports, the command line built
// from the settings (BuildArgs: bools, enums, floats, `requires`, emit modes, nondefault, order, quoting), the settings
// store (round trip, unknown keys, repairs, corrupt file, versions), the data-folder check, and starting a process
// (this very executable is the child). Also validates the real schema, launcher/schema/settings.json, so a mistake
// there fails CTest.
//
// Standalone: no SDK, no game, no ImGui. Built and run by CTest as mcla_launcher_core_test (launcher/CMakeLists.txt).
// Exits 1 on the first run with a failed check.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/game_process.h"
#include "core/settings_schema.h"
#include "core/settings_store.h"
#include "core/text.h"

#ifndef MCLA_LAUNCHER_SCHEMA_PATH
#error "MCLA_LAUNCHER_SCHEMA_PATH must be defined by the build"
#endif

namespace fs = std::filesystem;
using namespace launcher;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(got, want)                                                                              \
  do {                                                                                                   \
    ++g_checks;                                                                                          \
    const auto got_ = (got);                                                                             \
    const auto want_ = (want);                                                                           \
    if (!(got_ == want_)) {                                                                              \
      ++g_failures;                                                                                      \
      std::printf("FAIL %s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #got,         \
                  Printable(got_).c_str(), Printable(want_).c_str());                                    \
    }                                                                                                    \
  } while (0)

#define CHECK_HAS(text, needle)                                                                          \
  do {                                                                                                   \
    ++g_checks;                                                                                          \
    const std::string text_ = (text);                                                                    \
    if (text_.find(needle) == std::string::npos) {                                                       \
      ++g_failures;                                                                                      \
      std::printf("FAIL %s:%d: %s does not contain \"%s\"\n  text: %s\n", __FILE__, __LINE__, #text,    \
                  std::string(needle).c_str(), text_.c_str());                                           \
    }                                                                                                    \
  } while (0)

std::string Printable(const std::string& s) { return "\"" + s + "\""; }
std::string Printable(const std::wstring& s) { return "L\"" + WideToUtf8(s) + "\""; }
std::string Printable(const std::vector<std::wstring>& v) {
  std::string out = "[";
  for (const std::wstring& s : v) out += (out.size() > 1 ? ", " : "") + Printable(s);
  return out + "]";
}
std::string Printable(const Json& j) { return j.dump(); }
std::string Printable(const fs::path& p) { return "path " + p.string(); }
std::string Printable(bool b) { return b ? "true" : "false"; }
template <typename T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
std::string Printable(T n) {
  return std::to_string(n);
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

using Args = std::vector<std::wstring>;

std::vector<std::wstring> ArgvOf(const std::wstring& command_line) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(command_line.c_str(), &argc);
  std::vector<std::wstring> out;
  for (int i = 0; argv && i < argc; ++i) out.push_back(argv[i]);
  if (argv) LocalFree(argv);
  return out;
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// The JSON in a file; a failed check and an empty object when the file is missing or malformed.
Json ReadJson(const fs::path& path) {
  const Json j = Json::parse(ReadText(path), nullptr, /*allow_exceptions=*/false);
  ++g_checks;
  if (j.is_discarded() || !j.is_object()) {
    ++g_failures;
    std::printf("FAIL: %s is missing or not a JSON object\n", path.string().c_str());
    return Json::object();
  }
  return j;
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

fs::path g_temp;  // scratch folder of this run

fs::path Temp(const char* name) { return g_temp / name; }

fs::path SelfExe() {
  wchar_t buf[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return buf;
}

// A game executable that asks for the UTF-8 code page: a copy of this program with such a manifest added (set by main).
fs::path g_utf8_exe;

const char kUtf8Manifest[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><application>"
    "<windowsSettings><activeCodePage xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">UTF-8</activeCodePage>"
    "</windowsSettings></application></assembly>";

bool MakeUtf8Exe(const fs::path& dest) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  fs::copy_file(SelfExe(), dest, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  const HANDLE update = BeginUpdateResourceW(dest.c_str(), FALSE);
  if (!update) return false;
  const BOOL added = UpdateResourceW(update, RT_MANIFEST, MAKEINTRESOURCEW(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                     const_cast<char*>(kUtf8Manifest), sizeof(kUtf8Manifest) - 1);
  return EndUpdateResourceW(update, added ? FALSE : TRUE) && added;
}

Schema MustParse(const std::string& text, const char* what) {
  std::string error = "unset";
  Schema schema = ParseSchema(text, &error, what);
  if (!error.empty() || schema.version != kSchemaVersion) {
    ++g_checks;
    ++g_failures;
    std::printf("FAIL: %s did not parse: %s\n", what, error.c_str());
  }
  return schema;
}

// A schema that must be rejected: its error text.
std::string MustFail(const std::string& text) {
  std::string error;
  const Schema schema = ParseSchema(text, &error, "test");
  CHECK(schema.version == 0);
  CHECK(schema.settings.empty());
  CHECK(!error.empty());
  return error;
}

// Schema pieces for the error tests.
std::string Doc(const std::string& settings, const std::string& presets = "[]") {
  return R"({"version":1,"categories":[{"id":"gfx","label":"Graphics"}],"presets":)" + presets + R"(,"settings":[)" +
         settings + "]}";
}
std::string Set(const std::string& id, const std::string& body) {
  return R"({"id":")" + id + R"(","label":"L","category":"gfx",)" + body + "}";
}

// ---------------------------------------------------------------------------------------------------------------------
// The schema every command-line test uses.

const char* kSchemaText = R"JSON({
  "version": 1,
  // a comment: the loader accepts them
  "_comment": "keys with a leading underscore are the author's",
  "categories": [ {"id": "gfx", "label": "Graphics"}, {"id": "misc", "label": "Misc"} ],
  "presets": [
    {"id": "empty", "label": "Nothing", "help": "", "set": {}},
    {"id": "p1080", "label": "1080p", "help": "1080p at 60", "set": {"profile": "1080p", "fps": 60}},
    {"id": "low", "label": "Low", "help": "", "set": {"fps": 30, "vsync": false}}
  ],
  "settings": [
    {"id": "vsync", "label": "VSync", "help": "h", "category": "gfx", "type": "bool", "cvar": "vsync", "default": true},
    {"id": "profile", "label": "Profile", "help": "h", "category": "gfx", "type": "enum", "cvar": "mcla_profile",
     "default": "", "options": [ {"value": "", "label": "Default"}, {"value": "1080p", "label": "1080p", "help": "x"},
                                 {"value": "900p", "label": "900p"} ]},
    {"id": "fps", "label": "FPS", "help": "h", "category": "gfx", "type": "int", "cvar": "mcla_fps_target",
     "default": 0, "min": 0, "max": 240, "step": 30},
    {"id": "scale", "label": "Scale", "help": "h", "category": "gfx", "type": "float", "cvar": "resolution_scale",
     "default": 1, "min": 0.5, "max": 4, "step": 0.25},
    {"id": "fsr", "label": "FSR", "help": "h", "category": "gfx", "type": "bool", "cvar": "mcla_fsr", "default": false},
    {"id": "fsr_sharp", "label": "Sharpness", "help": "h", "category": "gfx", "type": "float", "cvar": "mcla_fsr_sharp",
     "default": 0.3, "min": 0, "max": 1, "requires": {"fsr": true}},
    {"id": "fsr_adv", "label": "Advanced", "help": "h", "category": "gfx", "type": "int", "cvar": "mcla_fsr_adv",
     "default": 1, "min": 0, "max": 9, "advanced": true, "requires": {"fsr_sharp": 0.5}},
    {"id": "mode", "label": "Mode", "help": "h", "category": "gfx", "type": "enum", "cvar": "mode_int", "default": 2,
     "options": [ {"value": 1, "label": "One"}, {"value": 2, "label": "Two"}, {"value": 3, "label": "Three"} ]},
    {"id": "data_root", "label": "Game folder", "help": "h", "category": "misc", "type": "path",
     "cvar": "game_data_root", "default": ""},
    {"id": "tag", "label": "Tag", "help": "h", "category": "misc", "type": "string", "cvar": "user_tag", "default": "",
     "emit": "always"},
    {"id": "key", "label": "Key", "help": "h", "category": "misc", "type": "string", "cvar": "keybind_a", "default": "A"},
    {"id": "log", "label": "Log", "help": "h", "category": "misc", "type": "bool", "cvar": "log_enabled", "default": true,
     "emit": "always"},
    {"id": "secret", "label": "Secret", "help": "h", "category": "misc", "type": "bool", "cvar": "x_never",
     "default": false, "emit": "never"},
    {"id": "close", "label": "Close", "help": "h", "category": "misc", "type": "bool", "cvar": null, "default": false}
  ]
})JSON";

// What a default Values gives: the `emit: always` settings that have a value ("tag" is empty, so it is not passed).
const Args kDefaultArgs = {L"--log_enabled=true"};

// ---------------------------------------------------------------------------------------------------------------------
// Tests

void TestTextHelpers() {
  CHECK_EQ(Utf8ToWide("abc"), std::wstring(L"abc"));
  CHECK_EQ(Utf8ToWide("\xC3\x9C" "ser"), std::wstring(L"\u00DCser"));
  CHECK_EQ(WideToUtf8(L"\u00DCser \u4e2d"), std::string("\xC3\x9C" "ser \xE4\xB8\xAD"));
  CHECK_EQ(PathFromUtf8("C:/a/\xC3\x9C"), fs::path(L"C:/a/\u00DC"));
  CHECK_EQ(PathToUtf8(fs::path(L"C:/a/\u00DC")), std::string("C:/a/\xC3\x9C"));

  CHECK_EQ(QuoteArg(L"plain"), std::wstring(L"plain"));
  CHECK_EQ(QuoteArg(L""), std::wstring(L"\"\""));
  CHECK_EQ(QuoteArg(L"a b"), std::wstring(L"\"a b\""));
  CHECK_EQ(QuoteArg(L"C:\\dir\\"), std::wstring(L"C:\\dir\\"));            // no space: nothing to protect
  CHECK_EQ(QuoteArg(L"C:\\my dir\\"), std::wstring(L"\"C:\\my dir\\\\\""));  // trailing backslash doubled
  CHECK_EQ(QuoteArg(L"say \"hi\""), std::wstring(L"\"say \\\"hi\\\"\""));
  CHECK_EQ(QuoteArg(L"a\\\"b"), std::wstring(L"\"a\\\\\\\"b\""));
  CHECK_EQ(QuoteArg(L"x", true), std::wstring(L"\"x\""));

  // Whatever QuoteArg makes, CommandLineToArgvW reads back unchanged.
  const Args tricky = {L"plain",          L"",       L"a b",       L"C:\\dir\\",      L"C:\\my dir\\", L"C:\\my dir\\x y\\\\",
                       L"say \"hi\"",     L"a\\\"b", L"\"",        L"\\\\server\\share name\\", L"tab\there", L"\\",
                       L"--x=C:\\Program Files\\Game\\", L"--x=\u00DCser \u4e2d"};
  const fs::path exe = L"C:\\Program Files\\mcla\\mcla.exe";
  const std::wstring line = FormatCommandLine(exe, tricky);
  Args want = {exe.native()};
  want.insert(want.end(), tricky.begin(), tricky.end());
  CHECK_EQ(ArgvOf(line), want);
  CHECK_EQ(FormatCommandLine(L"C:\\x\\mcla.exe", {}), std::wstring(L"\"C:\\x\\mcla.exe\""));
}

void TestSchemaParse() {
  const Schema s = MustParse(kSchemaText, "kSchemaText");
  CHECK_EQ(s.version, 1);
  CHECK_EQ(s.categories.size(), size_t(2));
  CHECK_EQ(s.settings.size(), size_t(14));
  CHECK_EQ(s.presets.size(), size_t(3));
  CHECK_EQ(s.warnings.size(), size_t(1));  // the deliberately empty preset; "_comment" and // comments are not reported
  CHECK_HAS(s.warnings.empty() ? "" : s.warnings[0], "\"set\" is empty");

  const SettingDef* fps = s.FindSetting("fps");
  CHECK(fps != nullptr);
  if (fps) {
    CHECK(fps->type == SettingType::Int);
    CHECK_EQ(fps->cvar.value_or("?"), std::string("mcla_fps_target"));
    CHECK_EQ(fps->min, 0.0);
    CHECK_EQ(fps->max, 240.0);
    CHECK_EQ(fps->step, 30.0);
    CHECK(fps->default_value.is_number_integer());
    CHECK(fps->emit == EmitMode::NonDefault);
    CHECK(!fps->advanced);
  }
  const SettingDef* scale = s.FindSetting("scale");
  CHECK(scale && scale->default_value.is_number_float());  // the file says 1: canonical form is a double
  const SettingDef* adv = s.FindSetting("fsr_adv");
  CHECK(adv && adv->advanced && adv->requires_.size() == 1);
  if (adv) CHECK(adv->requires_.at("fsr_sharp").is_number_float());  // 0.5
  const SettingDef* profile = s.FindSetting("profile");
  CHECK(profile && profile->options.size() == 3 && profile->options[1].help == "x" && profile->options[1].label == "1080p");
  const SettingDef* close = s.FindSetting("close");
  CHECK(close && !close->cvar.has_value());
  const SettingDef* tag = s.FindSetting("tag");
  CHECK(tag && tag->emit == EmitMode::Always);
  const SettingDef* secret = s.FindSetting("secret");
  CHECK(secret && secret->emit == EmitMode::Never);
  CHECK(s.FindSetting("nope") == nullptr);
  CHECK(s.FindCategory("misc") != nullptr && s.FindCategory("nope") == nullptr);
  CHECK(s.FindPreset("p1080") != nullptr && s.FindPreset("nope") == nullptr);
  const Preset* p = s.FindPreset("p1080");
  if (p) {
    CHECK_EQ(p->set.size(), size_t(2));
    CHECK(p->set.at("fps").is_number_integer());
  }
  CHECK_EQ(std::string(SettingTypeName(SettingType::Path)), std::string("path"));
  CHECK_EQ(std::string(EmitModeName(EmitMode::NonDefault)), std::string("nondefault"));

  // Unknown keys are warnings, not errors.
  std::string error;
  const Schema w = ParseSchema(
      Doc(Set("a", R"("type":"bool","default":true,"colour":"red")"), "[]").replace(1, 0, R"("extra":1,)"), &error, "w");
  CHECK(error.empty());
  CHECK_EQ(w.warnings.size(), size_t(2));
  CHECK_HAS(w.warnings.empty() ? "" : w.warnings[0] + w.warnings.back(), "unknown key");

  // A missing file.
  std::string file_error;
  const Schema none = LoadSchema(Temp("no_such_schema.json"), &file_error);
  CHECK(none.version == 0);
  CHECK_HAS(file_error, "cannot open");
}

void TestSchemaErrors() {
  CHECK_HAS(MustFail("{ not json"), "not valid JSON");
  CHECK_HAS(MustFail("[]"), "top level must be a JSON object");
  CHECK_HAS(MustFail(R"({"categories":[],"settings":[]})"), "missing \"version\"");
  {
    std::string v = Doc(Set("a", R"("type":"bool","default":true)"));
    v.replace(v.find(R"("version":1)"), 11, R"("version":2)");
    CHECK_HAS(MustFail(v), "version 2 is not supported");
  }
  CHECK_HAS(MustFail(R"({"version":1,"settings":[]})"), "missing \"categories\"");
  CHECK_HAS(MustFail(R"({"version":1,"categories":[{"id":"g","label":"G"}]})"), "missing \"settings\"");

  // Categories.
  CHECK_HAS(MustFail(R"({"version":1,"categories":[{"id":"g","label":"G"},{"id":"g","label":"H"}],"settings":[]})"),
            "duplicate category id \"g\"");
  CHECK_HAS(MustFail(R"({"version":1,"categories":[{"id":"g"}],"settings":[]})"), "missing \"label\"");

  // Settings: ids, categories, types.
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)") + "," + Set("a", R"("type":"bool","default":false)"))),
            "duplicate setting id");
  CHECK_HAS(MustFail(Doc(Set("bad id", R"("type":"bool","default":true)"))), "must be made of letters");
  CHECK_HAS(MustFail(Doc(R"({"id":"a","label":"L","category":"nope","type":"bool","default":true})")),
            "unknown category \"nope\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"colour","default":true)"))), "unknown type \"colour\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("default":true)"))), "missing \"type\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool")"))), "missing \"default\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"cvar":"has space")"))), "\"cvar\" \"has space\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"emit":"sometimes")"))), "\"emit\" must be");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"advanced":"yes")"))), "\"advanced\" must be true or false");
  CHECK_HAS(MustFail(Doc(R"({"id":"a","category":"gfx","type":"bool","default":true})")), "missing \"label\"");

  // Defaults of the wrong type, value or range.
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":"true")"))), "expected true or false, got a string");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":"3","min":0,"max":9)"))), "expected an integer, got a string");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":3.5,"min":0,"max":9)"))), "expected an integer, got a number");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":60.0,"min":0,"max":99)"))), "expected an integer");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":10,"min":0,"max":9)"))), "above the maximum 9");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"float","default":-1,"min":0,"max":9)"))), "below the minimum 0");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"float","default":"x","min":0,"max":9)"))), "expected a number, got a string");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":1,"max":9)"))), "needs \"min\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"float","default":1,"min":0)"))), "needs \"max\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":1,"min":9,"max":0)"))), "\"min\" is greater than \"max\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":1,"min":0,"max":9,"step":-1)"))), "must not be negative");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"string","default":5)"))), "expected a string, got an integer");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"path","default":null)"))), "expected a string, got null");

  // Enums.
  const std::string two = R"("options":[{"value":"a","label":"A"},{"value":"b","label":"B"}])";
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":"x",)" + two))),
            "default \"x\": not one of the options (\"a\", \"b\")");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":"a")"))), "an enum needs \"options\"");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":"a","options":[])"))), "non-empty array");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":1,"options":[{"value":1,"label":"A"},{"value":1,"label":"B"}])"))),
            "duplicate value 1");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":1,"options":[{"value":1}])"))), "missing \"label\"");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":1,"options":[{"label":"x"}])"))), "missing \"value\"");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":1,"options":[{"value":[1],"label":"x"}])"))),
            "\"value\" must be a string, number or boolean");
  CHECK_HAS(MustFail(Doc(Set("e", R"("type":"enum","default":true,"options":[{"value":1,"label":"x"}])"))), "not one of the options");

  // requires.
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"requires":{"ghost":true})"))), "unknown setting \"ghost\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"requires":{"a":true})"))), "names the setting itself");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)") + "," +
                         Set("b", R"("type":"bool","default":true,"requires":{"a":"yes"})"))),
            "expected true or false, got a string");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"requires":{"b":true})") + "," +
                         Set("b", R"("type":"bool","default":true,"requires":{"a":true})"))),
            "\"requires\" cycle: a -> b -> a");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true,"requires":[1])"))), "\"requires\" must be an object");
  // A list of values is "any of": an empty list and a bad member are errors.
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)") + "," +
                         Set("b", R"("type":"bool","default":true,"requires":{"a":[]})"))),
            "the list of values is empty");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)") + "," +
                         Set("b", R"("type":"bool","default":true,"requires":{"a":[true,"yes"]})"))),
            "expected true or false, got a string");

  // A list in requires is "any of the values": active for 60 and for 120 (the 60 fps timing options), not for 0, and a stored value
  // that is not an option falls back to the default (60: active).
  {
    std::string error;
    const Schema any = ParseSchema(
        Doc(Set("fps", R"("type":"enum","default":60,"options":[{"value":60,"label":"60"},{"value":120,"label":"120"},{"value":0,"label":"0"}])") +
            "," + Set("tick", R"("type":"bool","default":false,"cvar":"tick","requires":{"fps":[60,120]})")),
        &error, "t");
    CHECK(error.empty());
    const SettingDef* tick = any.FindSetting("tick");
    CHECK(tick != nullptr);
    if (tick) {
      Values v;
      v["fps"] = 60;
      CHECK(IsActive(any, v, *tick));
      v["fps"] = 120;
      CHECK(IsActive(any, v, *tick));
      v["fps"] = 0;
      CHECK(!IsActive(any, v, *tick));
      v["fps"] = 30;
      CHECK(IsActive(any, v, *tick));
    }
  }

  // cvars shared by two settings: allowed only when both are conditional.
  const std::string bools =Set("sw", R"("type":"bool","default":false,"cvar":"mode")");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":false,"cvar":"same")") + "," +
                         Set("b", R"("type":"bool","default":false,"cvar":"same")"))),
            "cvar \"same\" is also set by 'a'");
  {
    std::string error;
    ParseSchema(Doc(bools + "," + Set("a", R"("type":"bool","default":false,"cvar":"same","requires":{"sw":true})") + "," +
                    Set("b", R"("type":"bool","default":false,"cvar":"same","requires":{"sw":false})")),
                &error, "t");
    CHECK(error.empty());
  }

  // Presets.
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)"), R"([{"id":"p","label":"P","set":{"ghost":1}}])")),
            "\"set\" names unknown setting \"ghost\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)"), R"([{"id":"p","label":"P","set":{"a":3}}])")),
            "expected true or false, got an integer");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"int","default":1,"min":0,"max":5)"), R"([{"id":"p","label":"P","set":{"a":6}}])")),
            "above the maximum 5");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)"),
                         R"([{"id":"p","label":"P","set":{}},{"id":"p","label":"Q","set":{}}])")),
            "duplicate preset id \"p\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)"), R"([{"id":"p","label":"P"}])")), "missing \"set\"");
  CHECK_HAS(MustFail(Doc(Set("a", R"("type":"bool","default":true)"), R"({"id":"p"})")), "\"presets\" must be an array");

  // Every problem is reported, one per line, with where it is.
  const std::string many = MustFail(Doc(Set("a", R"("type":"bool","default":"x")") + "," +
                                        Set("b", R"("type":"int","default":99,"min":0,"max":9)") + "," +
                                        Set("c", R"("type":"enum","default":"q","options":[{"value":"a","label":"A"}])")));
  CHECK_HAS(many, "3 problems");
  CHECK_HAS(many, "settings[0] 'a'");
  CHECK_HAS(many, "settings[1] 'b'");
  CHECK_HAS(many, "settings[2] 'c'");
  size_t lines = 0;
  for (const char c : many) lines += c == '\n';
  CHECK_EQ(lines, size_t(3));
}

Values Vals(const Schema& s, std::initializer_list<std::pair<const char*, Json>> set) {
  Values v = DefaultValues(s);
  for (const auto& [id, value] : set) v[id] = value;
  return v;
}

void TestBuildArgs() {
  const Schema s = MustParse(kSchemaText, "kSchemaText");

  // Defaults: only the `emit: always` settings; `emit: never` and launcher-only never appear.
  CHECK_EQ(BuildArgs(s, DefaultValues(s)), kDefaultArgs);
  CHECK_EQ(BuildArgs(s, Values{}), kDefaultArgs);  // a missing id is the default

  // Bools, ints, enums (string and integer), strings, paths; schema order whatever the order of assignment.
  Values v = Vals(s, {{"vsync", false}, {"fps", 60}, {"profile", "1080p"}, {"mode", 3}, {"tag", "a b"}});
  const Args expect = {L"--vsync=false", L"--mcla_profile=1080p", L"--mcla_fps_target=60", L"--mode_int=3",
                       L"--user_tag=a b", L"--log_enabled=true"};
  CHECK_EQ(BuildArgs(s, v), expect);
  Values reordered;
  for (auto it = v.rbegin(); it != v.rend(); ++it) reordered[it->first] = it->second;
  CHECK_EQ(BuildArgs(s, reordered), expect);

  // A value equal to the default is not emitted (an enum's "" default, a float given as an int).
  CHECK_EQ(BuildArgs(s, Vals(s, {{"profile", ""}, {"scale", 1}, {"fps", 0}, {"vsync", true}})), kDefaultArgs);
  // Floats: shortest text that reads back, no trailing zeros; tiny differences from the default are not a change.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"scale", 1.5}})), (Args{L"--resolution_scale=1.5", L"--log_enabled=true"}));
  CHECK_EQ(BuildArgs(s, Vals(s, {{"scale", 2.0}})), (Args{L"--resolution_scale=2", L"--log_enabled=true"}));
  CHECK_EQ(BuildArgs(s, Vals(s, {{"scale", 1.0000000000001}})), kDefaultArgs);
  CHECK_EQ(FormatCvarValue(Json(0.1)), std::string("0.1"));
  CHECK_EQ(FormatCvarValue(Json(true)), std::string("true"));
  CHECK_EQ(FormatCvarValue(Json(-7)), std::string("-7"));
  CHECK_EQ(FormatCvarValue(Json("x y")), std::string("x y"));
  CHECK_EQ(FormatCvarValue(Json(1.0 / 3.0)), std::string("0.3333333333333333"));

  // emit: always puts a default on the command line; emit: never keeps a changed value off it; launcher-only never.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"log", false}})), (Args{L"--log_enabled=false"}));
  CHECK_EQ(BuildArgs(s, Vals(s, {{"secret", true}, {"close", true}})), kDefaultArgs);

  // requires: hidden settings are not emitted whatever their stored value is.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr_sharp", 0.8}})), kDefaultArgs);  // fsr is off
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr", true}})), (Args{L"--mcla_fsr=true", L"--log_enabled=true"}));
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr", true}, {"fsr_sharp", 0.8}})),
           (Args{L"--mcla_fsr=true", L"--mcla_fsr_sharp=0.8", L"--log_enabled=true"}));
  // Chain: fsr_adv needs fsr_sharp == 0.5, which itself needs fsr.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr", false}, {"fsr_sharp", 0.5}, {"fsr_adv", 3}})), kDefaultArgs);
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr", true}, {"fsr_sharp", 0.8}, {"fsr_adv", 3}})),
           (Args{L"--mcla_fsr=true", L"--mcla_fsr_sharp=0.8", L"--log_enabled=true"}));
  CHECK_EQ(BuildArgs(s, Vals(s, {{"fsr", true}, {"fsr_sharp", 0.5}, {"fsr_adv", 3}})),
           (Args{L"--mcla_fsr=true", L"--mcla_fsr_sharp=0.5", L"--mcla_fsr_adv=3", L"--log_enabled=true"}));
  const SettingDef* sharp = s.FindSetting("fsr_sharp");
  const SettingDef* adv = s.FindSetting("fsr_adv");
  CHECK(sharp && adv);
  if (sharp && adv) {
    CHECK(!IsActive(s, Vals(s, {{"fsr", false}}), *sharp));
    CHECK(IsActive(s, Vals(s, {{"fsr", true}}), *sharp));
    CHECK(!IsActive(s, Vals(s, {{"fsr", true}}), *adv));                       // sharp is 0.3
    CHECK(IsActive(s, Vals(s, {{"fsr", true}, {"fsr_sharp", 0.5}}), *adv));
    CHECK(!IsActive(s, Vals(s, {{"fsr", false}, {"fsr_sharp", 0.5}}), *adv));  // the chain is followed
  }

  // A stored value of the wrong type or outside the range is ignored (the default stands), never emitted.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"vsync", "no"}, {"fps", 9999}, {"profile", "4k"}, {"scale", "big"}})), kDefaultArgs);

  // Values the command line must carry whole: spaces, quotes, trailing backslash, non-ASCII. A path setting is passed
  // absolute and normalised (here: forward slashes, trailing slash).
  const fs::path base = L"C:\\Games\\Midnight Club";
  const Args raw = BuildArgs(
      s, Vals(s, {{"data_root", "C:/Program Files/\xC3\x9C" "ser/"}, {"tag", "say \"hi\""}, {"key", "D:\\x y\\"}}), nullptr,
      base);
  CHECK_EQ(raw, (Args{L"--game_data_root=C:\\Program Files\\\u00DC" L"ser", L"--user_tag=say \"hi\"", L"--keybind_a=D:\\x y\\",
                      L"--log_enabled=true"}));
  const fs::path exe = L"C:\\Program Files\\mcla\\mcla.exe";
  const std::wstring line = FormatCommandLine(exe, raw);
  CHECK_HAS(WideToUtf8(line), "\"--game_data_root=C:\\Program Files\\\xC3\x9C" "ser\"");
  CHECK_HAS(WideToUtf8(line), "\"--keybind_a=D:\\x y\\\\\"");  // the trailing backslash is doubled in front of the quote
  Args round = {exe.native()};
  round.insert(round.end(), raw.begin(), raw.end());
  CHECK_EQ(ArgvOf(line), round);

  // Presets.
  Values p = DefaultValues(s);
  std::string error;
  CHECK(ApplyPreset(s, "p1080", p, &error));
  CHECK_EQ(BuildArgs(s, p), (Args{L"--mcla_profile=1080p", L"--mcla_fps_target=60", L"--log_enabled=true"}));
  const Preset* match = MatchingPreset(s, p);
  CHECK(match && match->id == "p1080");
  p["tag"] = "x";  // not part of a preset: still matches
  CHECK(MatchingPreset(s, p) && MatchingPreset(s, p)->id == "p1080");
  p["fps"] = 30;
  CHECK(MatchingPreset(s, p) == nullptr);  // the empty preset never matches
  CHECK(ApplyPreset(s, "low", p, &error));
  CHECK(MatchingPreset(s, p) && MatchingPreset(s, p)->id == "low");
  CHECK_EQ(p.at("vsync"), Json(false));
  CHECK(!ApplyPreset(s, "ghost", p, &error));
  CHECK_HAS(error, "unknown preset \"ghost\"");
  ResetToDefaults(s, p);
  CHECK_EQ(BuildArgs(s, p), kDefaultArgs);
  Values extra = p;
  extra["unknown_future_setting"] = 5;
  ResetToDefaults(s, extra);
  CHECK(extra.count("unknown_future_setting") == 1);  // unknown ids survive a reset
}

void TestStore() {
  const Schema s = MustParse(kSchemaText, "kSchemaText");
  const fs::path file = Temp("store/launcher.json");

  // No file: the defaults, and no notes.
  std::vector<std::string> notes;
  CHECK(LoadValues(s, file, &notes) == DefaultValues(s));
  CHECK(notes.empty());

  // Round trip, creating the folder.
  Values v = Vals(s, {{"fps", 60}, {"profile", "900p"}, {"scale", 1.75}, {"tag", "caf\xC3\xA9"}, {"close", true}});
  std::string error;
  CHECK(SaveValues(s, v, file, &error));
  CHECK(error.empty());
  CHECK(fs::exists(file));
  CHECK(!fs::exists(Temp("store/launcher.json.tmp")));
  const Json saved = Json::parse(ReadText(file));
  CHECK_EQ(saved.at("version"), Json(kSettingsFileVersion));
  CHECK_EQ(saved.at("values").size(), size_t(14));  // every setting is written
  CHECK_EQ(saved.at("values").at("tag"), Json("caf\xC3\xA9"));
  CHECK_HAS(ReadText(file), "caf\xC3\xA9");  // UTF-8, not \u escapes
  notes.clear();
  const Values back = LoadValues(s, file, &notes);
  CHECK(notes.empty());
  CHECK(back == v);
  CHECK_EQ(BuildArgs(s, back), BuildArgs(s, v));

  // Unknown keys survive: values of unknown ids, top-level keys, a newer version.
  Json doc = saved;
  doc["window"] = Json{{"x", 10}, {"y", 20}};
  doc["version"] = 7;
  doc["values"]["from_the_future"] = Json{{"a", 1}};
  doc["values"]["fps"] = 120;
  WriteText(file, doc.dump(2));
  notes.clear();
  Values loaded = LoadValues(s, file, &notes);
  CHECK_EQ(notes.size(), size_t(1));
  CHECK_HAS(notes.empty() ? "" : notes[0], "newer launcher (version 7)");
  CHECK_EQ(loaded.at("fps"), Json(120));
  CHECK_EQ(loaded.at("from_the_future"), (Json{{"a", 1}}));
  loaded["fps"] = 30;
  CHECK(SaveValues(s, loaded, file, &error));
  const Json again = Json::parse(ReadText(file));
  CHECK_EQ(again.at("version"), Json(7));                          // not downgraded
  CHECK_EQ(again.at("window"), (Json{{"x", 10}, {"y", 20}}));      // top-level extra kept
  CHECK_EQ(again.at("values").at("from_the_future"), (Json{{"a", 1}}));
  CHECK_EQ(again.at("values").at("fps"), Json(30));

  // Invalid stored values are repaired, each with a note.
  Json bad = Json::object();
  bad["version"] = 1;
  bad["values"] = Json{{"vsync", "yes"}, {"fps", 9999}, {"scale", -3}, {"profile", "4k"}, {"mode", 3.0}, {"tag", 5},
                       {"fsr", true}, {"key", ""}};
  WriteText(file, bad.dump());
  notes.clear();
  const Values repaired = LoadValues(s, file, &notes);
  CHECK_EQ(notes.size(), size_t(6));
  CHECK_EQ(repaired.at("vsync"), Json(true));      // default
  CHECK_EQ(repaired.at("fps"), Json(240));         // clamped
  CHECK_EQ(repaired.at("scale"), Json(0.5));       // clamped
  CHECK_EQ(repaired.at("profile"), Json(""));      // default
  CHECK_EQ(repaired.at("mode"), Json(3));          // 3.0 is the integer option 3
  CHECK_EQ(repaired.at("tag"), Json(""));          // default
  CHECK_EQ(repaired.at("key"), Json("A"));         // "" is not a value over a non-empty default
  CHECK_EQ(repaired.at("fsr"), Json(true));
  std::string all_notes;
  for (const std::string& n : notes) all_notes += n + "\n";
  CHECK_HAS(all_notes, "setting 'fps': stored value 9999 is above the maximum 240; clamped to 240");
  CHECK_HAS(all_notes, "setting 'vsync'");
  CHECK_HAS(all_notes, "setting 'profile'");

  // A file that is not JSON, or not a settings file, is moved aside and the defaults are used.
  WriteText(file, "{ this is not json");
  notes.clear();
  CHECK(LoadValues(s, file, &notes) == DefaultValues(s));
  CHECK_EQ(notes.size(), size_t(1));
  CHECK_HAS(notes.empty() ? "" : notes[0], "not valid JSON");
  CHECK_HAS(notes.empty() ? "" : notes[0], "launcher.json.corrupt");
  CHECK(!fs::exists(file));
  CHECK_EQ(ReadText(Temp("store/launcher.json.corrupt")), std::string("{ this is not json"));
  WriteText(file, R"({"version":1,"vals":{}})");
  notes.clear();
  CHECK(LoadValues(s, file, &notes) == DefaultValues(s));
  CHECK_HAS(notes.empty() ? "" : notes[0], "not a launcher settings file");
  CHECK(!fs::exists(file));

  // Saving over a corrupt file is fine, and so is a path with no existing folder.
  CHECK(SaveValues(s, DefaultValues(s), Temp("store/deeper/er/launcher.json"), &error));
  CHECK(LoadValues(s, Temp("store/deeper/er/launcher.json")) == DefaultValues(s));
  WriteText(file, "garbage");
  CHECK(SaveValues(s, v, file, &error));
  CHECK(LoadValues(s, file) == v);

  CHECK(DefaultSettingsPath().filename() == L"launcher.json");
  CHECK_EQ(DefaultSettingsPath().parent_path().filename().wstring(), std::wstring(L"mcla_x86"));
}

void TestDataRoot() {
  CHECK(!ValidateGameDataRoot({}, g_utf8_exe).ok);
  CHECK_HAS(ValidateGameDataRoot({}, g_utf8_exe).issues.at(0), "No game data folder");
  CHECK_HAS(ValidateGameDataRoot(Temp("no_such_dir"), g_utf8_exe).issues.at(0), "does not exist");
  WriteText(Temp("a_file"), "x");
  CHECK_HAS(ValidateGameDataRoot(Temp("a_file"), g_utf8_exe).issues.at(0), "not a folder");

  const fs::path root = Temp("data");
  fs::create_directories(root);
  DataRootCheck c = ValidateGameDataRoot(root, g_utf8_exe);
  CHECK(!c.ok);
  CHECK_EQ(c.issues.size(), size_t(5));  // default.xex + the four archives
  CHECK_HAS(c.issues.at(0), "default.xex is missing");
  CHECK_HAS(c.issues.at(1), "xarchive_audio.rpf is missing");

  // default.xex of the wrong size, archives present.
  WriteText(root / "default.xex", "tiny");
  for (const char* name : {"xarchive_audio.rpf", "xarchive_audlo.rpf", "xarchive_cache.rpf", "xarchive_music.rpf"})
    WriteText(root / name, "x");
  c = ValidateGameDataRoot(root, g_utf8_exe);
  CHECK(!c.ok);
  CHECK_EQ(c.issues.size(), size_t(1));
  CHECK_HAS(c.issues.at(0), "default.xex is 4 bytes");
  CHECK_HAS(c.issues.at(0), "9,252,864-byte");

  std::error_code ec;
  fs::resize_file(root / "default.xex", kDefaultXexSize, ec);
  CHECK(!ec);
  c = ValidateGameDataRoot(root, g_utf8_exe);
  CHECK(c.ok);
  CHECK(c.issues.empty());

  fs::remove(root / "xarchive_music.rpf");
  WriteText(root / "xarchive_cache.rpf", "");
  c = ValidateGameDataRoot(root, g_utf8_exe);
  CHECK(!c.ok);
  CHECK_EQ(c.issues.size(), size_t(2));

  // The parent of the real folder: a hint to the subfolder.
  fs::create_directories(Temp("data_parent/game"));
  WriteText(Temp("data_parent/game/default.xex"), "x");
  CHECK_HAS(ValidateGameDataRoot(Temp("data_parent"), g_utf8_exe).issues.at(0), "subfolder \"game\"");

  // FindGameExeIn
  CHECK(FindGameExeIn(Temp("data")).empty());
  WriteText(Temp("data/mcla.exe"), "MZ");
  CHECK(FindGameExeIn(Temp("data")) == Temp("data") / "mcla.exe");
  CHECK(!LauncherDir().empty());
  CHECK(DefaultSchemaPath().filename() == L"settings.json");
}

void TestUtf8Manifest() {
  CHECK(ManifestRequestsUtf8(kUtf8Manifest));
  CHECK(ManifestRequestsUtf8("<assembly><ws2:activeCodePage xmlns:ws2=\"x\">\n  Utf-8 </ws2:activeCodePage></assembly>"));
  CHECK(!ManifestRequestsUtf8("<activeCodePage>Legacy</activeCodePage>"));
  CHECK(!ManifestRequestsUtf8("<!-- <activeCodePage>UTF-8</activeCodePage> -->"));
  CHECK(!ManifestRequestsUtf8("<assembly><application/></assembly>"));
  CHECK(!ManifestRequestsUtf8("<activeCodePage/>"));
  CHECK(!ManifestRequestsUtf8("<activeCodePage>"));
  CHECK(!ManifestRequestsUtf8(""));

  // Real executables: the resource is read from the file.
  WriteText(Temp("notpe/mcla.exe"), "MZ not an executable");
  CHECK(!GameUsesUtf8CodePage({}));
  CHECK(!GameUsesUtf8CodePage(Temp("nope.exe")));
  CHECK(!GameUsesUtf8CodePage(Temp("notpe/mcla.exe")));
  CHECK(!GameUsesUtf8CodePage(SelfExe()));  // this program has no such manifest
  CHECK(GameUsesUtf8CodePage(g_utf8_exe));
  CHECK(GameUsesUtf8CodePage(g_utf8_exe));  // (the second answer comes from the cache)
}

// A complete data folder, whatever its name.
void MakeDataRoot(const fs::path& dir) {
  fs::create_directories(dir);
  WriteText(dir / "default.xex", "x");
  std::error_code ec;
  fs::resize_file(dir / "default.xex", kDefaultXexSize, ec);
  for (const char* name : {"xarchive_audio.rpf", "xarchive_audlo.rpf", "xarchive_cache.rpf", "xarchive_music.rpf"})
    WriteText(dir / name, "x");
}

void TestNonAsciiPaths() {
  const fs::path plain = SelfExe();  // a game executable without the UTF-8 manifest
  const fs::path odd = g_temp / L"d\u00E4t\u00E4";
  MakeDataRoot(odd);

  CHECK_EQ(NonAsciiPathProblem(L"C:\\plain\\dir", plain), std::string());
  CHECK_HAS(NonAsciiPathProblem(odd, plain), "path contains non-ASCII characters; the game cannot open it yet: ");
  CHECK_EQ(NonAsciiPathProblem(odd, g_utf8_exe), std::string());
  CHECK(IsAsciiPath(L"C:\\plain") && !IsAsciiPath(odd));

  // The data root: the only problem of a complete folder is its name.
  DataRootCheck c = ValidateGameDataRoot(odd, plain);
  CHECK(!c.ok);
  CHECK_EQ(c.issues.size(), size_t(1));
  CHECK_HAS(c.issues.empty() ? "" : c.issues[0], "path contains non-ASCII characters; the game cannot open it yet");
  c = ValidateGameDataRoot(odd, g_utf8_exe);
  CHECK(c.ok);
  CHECK(!ValidateGameDataRoot(odd).ok);  // no game exe given: FindGameExe(), none next to this test
  c = ValidateGameDataRoot(g_temp / L"missing_d\u00E4t\u00E4", plain);  // both problems are reported
  CHECK_EQ(c.issues.size(), size_t(2));

  // The user data root: empty is fine.
  CHECK(ValidateUserDataRoot({}, plain).ok);
  c = ValidateUserDataRoot(odd, plain);
  CHECK(!c.ok);
  CHECK_HAS(c.issues.empty() ? "" : c.issues[0], "The user data folder path contains non-ASCII characters");
  CHECK(ValidateUserDataRoot(odd, g_utf8_exe).ok);
  CHECK(ValidateUserDataRoot(Temp("no_such_user_dir"), g_utf8_exe).ok);  // the game creates it
  WriteText(Temp("user_file"), "x");
  CHECK(!ValidateUserDataRoot(Temp("user_file"), g_utf8_exe).ok);

  // Through the settings: every active path setting, not the other string settings.
  const Schema s = MustParse(kSchemaText, "kSchemaText");
  const Values v = Vals(s, {{"data_root", PathToUtf8(odd)}, {"tag", "caf\xC3\xA9"}});
  std::vector<PathProblem> problems = CheckPathSettings(s, v, plain);
  CHECK_EQ(problems.size(), size_t(1));
  if (!problems.empty()) {
    CHECK_EQ(problems[0].setting_id, std::string("data_root"));
    CHECK_HAS(problems[0].message, "Game folder: path contains non-ASCII characters; the game cannot open it yet");
  }
  CHECK(CheckPathSettings(s, v, g_utf8_exe).empty());
  CHECK(CheckPathSettings(s, Vals(s, {{"data_root", "C:\\plain"}}), plain).empty());
  CHECK(CheckPathSettings(s, DefaultValues(s), plain).empty());
  // A relative path is judged where it resolves to.
  CHECK_EQ(CheckPathSettings(s, Vals(s, {{"data_root", "sub"}}), plain, L"C:\\Users\\Jos\u00E9\\mcla").size(), size_t(1));
  CHECK(CheckPathSettings(s, Vals(s, {{"data_root", "sub"}}), plain, L"C:\\Users\\Jose\\mcla").empty());
}

void TestEmptyValuesAndPaths() {
  const Schema s = MustParse(kSchemaText, "kSchemaText");
  const fs::path base = L"C:\\Games\\Midnight Club";
  const SettingDef* key = s.FindSetting("key");
  const SettingDef* tag = s.FindSetting("tag");
  CHECK(key && tag);
  if (!key || !tag) return;

  // An empty string setting is not passed (`--name=` would swallow the next argument): "tag" has `emit: always`.
  std::vector<std::string> notes;
  CHECK_EQ(BuildArgs(s, DefaultValues(s), &notes, base), kDefaultArgs);
  CHECK_EQ(notes.size(), size_t(1));
  CHECK_HAS(notes.empty() ? "" : notes[0], "\"Tag\" is empty: it is not passed to the game");
  CHECK_EQ(BuildArgs(s, DefaultValues(s), nullptr, base), kDefaultArgs);  // notes are optional

  // A string with a non-empty default: "" is not a value, the default stands.
  std::string why;
  CHECK(!CheckValue(*key, Json(""), &why));
  CHECK_HAS(why, "the default is used");
  CHECK(CheckValue(*key, Json("B")));
  CHECK(CheckValue(*tag, Json("")));  // its default is empty: "" is fine
  const Values emptied = Vals(s, {{"key", ""}});
  CHECK_EQ(EffectiveValue(*key, emptied), Json("A"));
  notes.clear();
  CHECK_EQ(BuildArgs(s, emptied, &notes, base), kDefaultArgs);
  CHECK_EQ(notes.size(), size_t(2));  // the tag, and the key that fell back to its default
  CHECK_HAS(notes.size() > 1 ? notes[1] : "", "\"Key\" is empty: its default is used");
  CHECK_EQ(BuildArgs(s, Vals(s, {{"key", "B"}}), nullptr, base), (Args{L"--keybind_a=B", L"--log_enabled=true"}));
  // Whatever the values, no argument is empty or ends in '=' (and so none swallows its successor).
  for (const char* text : {"", " ", "\t", "x"}) {
    const Args args = BuildArgs(s, Vals(s, {{"key", text}, {"data_root", text}, {"tag", text}}), nullptr, base);
    for (const std::wstring& a : args) CHECK(!a.empty() && a.back() != L'=');
  }
  // A blank path is empty too.
  notes.clear();
  CHECK_EQ(BuildArgs(s, Vals(s, {{"data_root", "   "}}), &notes, base), kDefaultArgs);
  CHECK_EQ(notes.size(), size_t(2));
  CHECK_HAS(notes.empty() ? "" : notes[0], "\"Game folder\" is empty: it is not passed to the game");

  // An enum option with the empty value that is not the default passes nothing: the schema warns, BuildArgs skips it.
  std::string error;
  const Schema es = ParseSchema(
      Doc(Set("e", R"("type":"enum","cvar":"e_cvar","default":"perf","options":[{"value":"","label":"None"},{"value":"perf","label":"Perf"}])")),
      &error, "t");
  CHECK(error.empty());
  CHECK_EQ(es.warnings.size(), size_t(1));
  CHECK_HAS(es.warnings.empty() ? "" : es.warnings[0], "empty value");
  Values ev = DefaultValues(es);
  CHECK(BuildArgs(es, ev).empty());
  ev["e"] = "";
  notes.clear();
  CHECK(BuildArgs(es, ev, &notes).empty());
  CHECK_EQ(notes.size(), size_t(1));

  // Paths are passed absolute and normalised; a relative one is taken from the launcher's folder.
  const auto root_arg = [&](const char* value) {
    return BuildArgs(s, Vals(s, {{"data_root", value}}), nullptr, base).front();
  };
  CHECK_EQ(root_arg("data"), std::wstring(L"--game_data_root=C:\\Games\\Midnight Club\\data"));
  CHECK_EQ(root_arg("..\\x\\.\\y\\"), std::wstring(L"--game_data_root=C:\\Games\\x\\y"));
  CHECK_EQ(root_arg("  data  "), std::wstring(L"--game_data_root=C:\\Games\\Midnight Club\\data"));
  CHECK_EQ(root_arg("D:/Game Data/"), std::wstring(L"--game_data_root=D:\\Game Data"));
  CHECK_EQ(root_arg("D:\\a\\..\\b"), std::wstring(L"--game_data_root=D:\\b"));
  CHECK_EQ(root_arg("D:\\"), std::wstring(L"--game_data_root=D:\\"));
  CHECK_EQ(root_arg("."), std::wstring(L"--game_data_root=C:\\Games\\Midnight Club"));
  CHECK_EQ(ResolvePath({}, base), fs::path());
  CHECK_EQ(ResolvePath(L"data", base), fs::path(L"C:\\Games\\Midnight Club\\data"));
  CHECK_EQ(ResolvePath(L"C:\\Games\\Midnight Club\\data\\", base), fs::path(L"C:\\Games\\Midnight Club\\data"));
  // Other string settings are not paths: passed as they are.
  CHECK_EQ(BuildArgs(s, Vals(s, {{"key", "a/b/"}}), nullptr, base),
           (Args{L"--keybind_a=a/b/", L"--log_enabled=true"}));
  // A relative root with no explicit base is taken from the launcher's folder (this program's).
  const Args rel = BuildArgs(s, Vals(s, {{"data_root", "gamedata"}}));
  CHECK_EQ(rel.front(), L"--game_data_root=" + (LauncherDir() / L"gamedata").lexically_normal().native());
  CHECK_HAS(ValidateGameDataRoot(L"gamedata", g_utf8_exe).issues.at(0), PathToUtf8((LauncherDir() / L"gamedata").native()));
}

// The child of TestLaunch: this executable run as `core_test --child-echo <out> args...` writes its working directory
// and its arguments (as CommandLineToArgvW parsed them) to <out> as JSON; `--child-sleep <ms>` waits; `--child-exit <n>`.
int ChildMain(const Args& argv) {
  if (argv.size() >= 3 && argv[1] == L"--child-echo") {
    wchar_t cwd[MAX_PATH * 2] = {};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    Json out = Json::object();
    out["cwd"] = WideToUtf8(cwd);
    out["args"] = Json::array();
    for (size_t i = 3; i < argv.size(); ++i) out["args"].push_back(WideToUtf8(argv[i]));
    WriteText(PathFromUtf8(WideToUtf8(argv[2])), out.dump());
    return 0;
  }
  if (argv.size() >= 3 && argv[1] == L"--child-sleep") {
    Sleep(static_cast<DWORD>(std::stoul(argv[2])));
    return 0;
  }
  if (argv.size() >= 3 && argv[1] == L"--child-exit") return std::stoi(argv[2]);
  return -1;
}

bool WaitUntilExit(GameProcess& p, int timeout_ms) {
  const auto start = std::chrono::steady_clock::now();
  while (p.IsRunning()) {
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeout_ms)) return false;
    Sleep(10);
  }
  return true;
}

void TestLaunch() {
  const fs::path self = SelfExe();
  std::string error;

  // Arguments and working directory arrive as given.
  const fs::path out = Temp("child/echo.json");
  fs::create_directories(Temp("child/work dir"));
  const Args tricky = {L"--x=C:\\Program Files\\Game\\", L"say \"hi\"", L"", L"--y=\u00DCser \u4e2d", L"C:\\a b\\\\", L"plain"};
  Args args = {L"--child-echo", out.native()};
  args.insert(args.end(), tricky.begin(), tricky.end());
  GameProcess p;
  CHECK_EQ(p.pid(), 0u);
  CHECK(!p.IsRunning());
  CHECK(p.Launch(self, args, Temp("child/work dir"), &error));
  CHECK(error.empty());
  CHECK(p.pid() != 0);
  CHECK(WaitUntilExit(p, 15000));
  CHECK(p.exit_code().has_value() && *p.exit_code() == 0);
  CHECK(!p.IsRunning());
  const Json echoed = ReadJson(out);
  Args got;
  for (const Json& a : echoed.value("args", Json::array())) got.push_back(Utf8ToWide(a.get<std::string>()));
  CHECK_EQ(got, tricky);
  CHECK(fs::equivalent(PathFromUtf8(echoed.value("cwd", "")), Temp("child/work dir")));

  // An empty working directory is the program's own folder.
  fs::remove(out);
  CHECK(p.Launch(self, {L"--child-echo", out.native()}, {}, &error));
  CHECK(WaitUntilExit(p, 15000));
  CHECK(fs::equivalent(PathFromUtf8(ReadJson(out).value("cwd", "")), self.parent_path()));

  // Exit code; a second launch while one runs is refused.
  CHECK(p.Launch(self, {L"--child-exit", L"7"}, {}, &error));
  CHECK(WaitUntilExit(p, 15000));
  CHECK(p.exit_code().has_value() && *p.exit_code() == 7u);
  GameProcess sleeper;
  CHECK(sleeper.Launch(self, {L"--child-sleep", L"1500"}, {}, &error));
  CHECK(sleeper.IsRunning());
  CHECK(!sleeper.Launch(self, {L"--child-exit", L"0"}, {}, &error));
  CHECK_HAS(error, "already running");
  CHECK(sleeper.IsRunning());
  CHECK(WaitUntilExit(sleeper, 15000));
  CHECK(sleeper.exit_code().has_value() && *sleeper.exit_code() == 0u);

  // Failures say why.
  GameProcess nope;
  CHECK(!nope.Launch(Temp("child/not_there.exe"), {}, {}, &error));
  CHECK_HAS(error, "was not found");
  CHECK(!nope.Launch({}, {}, {}, &error));
  WriteText(Temp("child/not_a_program.exe"), "this is not an executable");
  CHECK(!nope.Launch(Temp("child/not_a_program.exe"), {}, {}, &error));
  CHECK_HAS(error, "did not start");
  CHECK_EQ(nope.pid(), 0u);

  // The debugging .bat: the exact command line and working directory.
  const fs::path bat = Temp("child/launch.bat");
  const fs::path game = L"C:\\Games\\Midnight Club\\mcla.exe";
  const Args game_args = {L"--game_data_root=D:\\Data Files\\", L"--mcla_fps_target=60", L"--user_tag=100%&more", L"--user_tag2=caf\u00E9"};
  CHECK(WriteLaunchBat(bat, game, game_args, {}, &error));
  const std::string text = ReadText(bat);
  CHECK_HAS(text, "@echo off\r\n");
  CHECK_HAS(text, "chcp 65001");
  CHECK_HAS(text, "cd /d \"C:\\Games\\Midnight Club\"\r\n");
  CHECK_HAS(text, "start \"\" /wait \"C:\\Games\\Midnight Club\\mcla.exe\" \"--game_data_root=D:\\Data Files\\\\\" "
                  "--mcla_fps_target=60 \"--user_tag=100%%&more\" --user_tag2=caf\xC3\xA9\r\n");
  CHECK(!WriteLaunchBat(bat, game, {L"--x=say \"hi\""}, {}, &error));
  CHECK_HAS(error, "quote");
  CHECK(!WriteLaunchBat(Temp("child/launch2.bat"), game, {L"--x=a\nb"}, {}, &error));
  CHECK(!fs::exists(Temp("child/launch2.bat")));
}

void TestRealSchema() {
  const fs::path path = fs::path(Utf8ToWide(MCLA_LAUNCHER_SCHEMA_PATH));
  std::string error;
  const Schema s = LoadSchema(path, &error);
  if (s.version == 0) {
    ++g_checks;
    ++g_failures;
    std::printf("FAIL: the real schema %s is invalid:\n%s\n", MCLA_LAUNCHER_SCHEMA_PATH, error.c_str());
    return;
  }
  for (const std::string& w : s.warnings) std::printf("note: schema warning: %s\n", w.c_str());
  std::printf("real schema: %zu categories, %zu settings, %zu presets\n", s.categories.size(), s.settings.size(), s.presets.size());
  CHECK(!s.settings.empty());

  // Every category is used, every setting has a label and help, every enum option has a label.
  std::set<std::string> used;
  for (const SettingDef& d : s.settings) {
    used.insert(d.category);
    CHECK(!d.label.empty());
    if (d.help.empty()) std::printf("note: setting '%s' has no help text\n", d.id.c_str());
    for (const Option& o : d.options) CHECK(!o.label.empty());
    CHECK(CheckValue(d, d.default_value));
  }
  for (const Category& c : s.categories)
    if (!used.count(c.id)) std::printf("note: category '%s' has no settings\n", c.id.c_str());

  // The defaults give a command line made of `emit: always` settings only, in schema order.
  Values defaults = DefaultValues(s);
  const Args default_args = BuildArgs(s, defaults);
  size_t always = 0;
  for (const SettingDef& d : s.settings)
    if (d.cvar && d.emit == EmitMode::Always && IsActive(s, defaults, d)) ++always;
  CHECK_EQ(default_args.size(), always);
  for (const std::wstring& a : default_args) CHECK(a.rfind(L"--", 0) == 0 && a.find(L'=') != std::wstring::npos);

  // Every preset applies, and every setting it names is valid; every cvar appears at most once on a command line.
  for (const Preset& p : s.presets) {
    Values v = DefaultValues(s);
    CHECK(ApplyPreset(s, p.id, v));
    for (const auto& [id, value] : p.set) {
      const SettingDef* d = s.FindSetting(id);
      CHECK(d && CheckValue(*d, value));
      if (d) CHECK(SameValue(EffectiveValue(*d, v), value));
    }
    const Args args = BuildArgs(s, v);
    std::set<std::wstring> names;
    for (const std::wstring& a : args) CHECK(names.insert(a.substr(0, a.find(L'='))).second);
  }

  // Each setting changed on its own yields a well-formed argument (when active), and the file round-trips.
  for (const SettingDef& d : s.settings) {
    Values v = DefaultValues(s);
    Json changed = d.default_value;
    switch (d.type) {
      case SettingType::Bool: changed = !d.default_value.get<bool>(); break;
      case SettingType::Int: changed = static_cast<std::int64_t>(d.max); break;
      case SettingType::Float: changed = d.max; break;
      case SettingType::Enum: changed = d.options.back().value; break;
      case SettingType::String:
      case SettingType::Path: changed = "x y"; break;
    }
    v[d.id] = changed;
    for (const auto& [dep, want] : d.requires_) v[dep] = want.is_array() ? want.front() : want;  // make it visible (one level is enough for a check)
    if (d.cvar && d.emit != EmitMode::Never && IsActive(s, v, d) && !SameValue(changed, d.default_value)) {
      const Args args = BuildArgs(s, v);
      bool found = false;
      for (const std::wstring& a : args) found = found || a.rfind(L"--" + Utf8ToWide(*d.cvar) + L"=", 0) == 0;
      CHECK(found);
    }
  }
  const fs::path file = Temp("real/launcher.json");
  std::string save_error;
  CHECK(SaveValues(s, defaults, file, &save_error));
  CHECK(LoadValues(s, file) == defaults);
}

}  // namespace

int main() {
  const Args argv = ArgvOf(GetCommandLineW());
  if (argv.size() >= 2 && argv[1].rfind(L"--child-", 0) == 0) return ChildMain(argv);

  std::error_code ec;
  g_temp = fs::temp_directory_path(ec) / (L"mcla_launcher_core_test_" + std::to_wstring(GetCurrentProcessId()));
  fs::remove_all(g_temp, ec);
  fs::create_directories(g_temp, ec);
  g_utf8_exe = Temp("utf8/mcla.exe");
  if (!MakeUtf8Exe(g_utf8_exe)) {
    ++g_checks;
    ++g_failures;
    std::printf("FAIL: cannot make the UTF-8 manifest stand-in executable\n");
  }

  // A crash or an exception in one test must fail this program, never leave a dialog waiting for somebody to click.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  const auto run = [](const char* name, void (*test)()) {
    const auto start = std::chrono::steady_clock::now();
    try {
      test();
    } catch (const std::exception& e) {
      ++g_checks;
      ++g_failures;
      std::printf("FAIL: %s threw: %s\n", name, e.what());
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("%-18s %5lld ms\n", name, static_cast<long long>(ms));
    std::fflush(stdout);
  };
  run("text helpers", TestTextHelpers);
  run("schema parse", TestSchemaParse);
  run("schema errors", TestSchemaErrors);
  run("build args", TestBuildArgs);
  run("store", TestStore);
  run("data root", TestDataRoot);
  run("utf8 manifest", TestUtf8Manifest);
  run("non-ascii paths", TestNonAsciiPaths);
  run("empty and paths", TestEmptyValuesAndPaths);
  run("launch", TestLaunch);
  run("real schema", TestRealSchema);

  fs::remove_all(g_temp, ec);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
