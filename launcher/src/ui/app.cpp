#include "ui/app.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/text.h"
#include "imgui_internal.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/util.h"
#include "ui/widgets.h"

namespace launcher::ui {
namespace {

constexpr const char* kTitle = "MIDNIGHT CLUB: LOS ANGELES";
constexpr const char* kSubtitle = "x86 native recompilation  \u00B7  Launcher";
constexpr const char* kCloseOnLaunchId = "close_on_launch";
constexpr const char* kDataRootCvar = "game_data_root";
constexpr const char* kResetPopup = "Reset settings";

// BeginChild whose colour / rounding / padding apply to that child only. ImGui reads them while the child begins, so they
// are popped right away: nothing leaks into the tooltips and combo popups that the child's widgets open.
bool BeginPanel(const char* id, ImVec2 size, ImGuiChildFlags child_flags, ImGuiWindowFlags window_flags, const ImVec4& background, float rounding,
                ImVec2 padding, const ImVec4& border = ImVec4(0, 0, 0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, (child_flags & ImGuiChildFlags_Borders) ? 1.0f : 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  const bool open = ImGui::BeginChild(id, size, child_flags, window_flags);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  return open;
}

const char* IconForCategory(std::string_view id) {
  if (id == "display" || id == "window") return icons::kMonitor;
  if (id == "graphics" || id == "video" || id == "visuals") return icons::kPicture;
  if (id == "performance" || id == "perf") return icons::kLightning;
  if (id == "gameplay" || id == "game") return icons::kFlag;
  if (id == "controls" || id == "input") return icons::kGamepad;
  if (id == "audio" || id == "sound") return icons::kVolume;
  if (id == "paths" || id == "files" || id == "folders") return icons::kFolder;
  if (id == "advanced" || id == "debug" || id == "developer" || id == "dev") return icons::kDeveloper;
  return icons::kGear;
}

long long AsInt(const Json& v) {
  if (v.is_number_integer()) return v.get<long long>();
  if (v.is_number()) return std::llround(v.get<double>());
  return 0;
}

double AsDouble(const Json& v) { return v.is_number() ? v.get<double>() : 0.0; }

std::string AsString(const Json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }

// InputText over a std::string (grows on demand: no fixed buffer, no silent truncation of long paths).
int StringResizeCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(data->UserData);
    str->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = str->data();
  }
  return 0;
}

bool InputTextString(const char* label, const char* hint, std::string* str, ImGuiInputTextFlags flags) {
  return ImGui::InputTextWithHint(label, hint, str->data(), str->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, StringResizeCallback, str);
}

constexpr const char* kPresentEffectId = "present_effect";
constexpr const char* kTemporalAaId = "temporal_aa";
constexpr const char* kFsrTwiceWarning =
    "FSR 1 output scaling is on together with the FSR temporal upscaler: the picture is upscaled twice and looks softer. "
    "Set the output scaling filter to Bilinear, or turn the upscaler off.";
constexpr const char* kFsrTwiceShort = "FSR 1 output scaling and the FSR upscaler are both on: the picture is upscaled twice.";

// Decimal places a float setting shows: from its step (0.05 -> 2), 2 when the step is not given.
int DecimalsFor(const SettingDef& def) {
  if (def.type != SettingType::Float) return 0;
  if (def.step <= 0.0) return 2;
  int d = 0;
  double s = def.step;
  while (d < 6 && std::fabs(s - std::round(s)) > 1e-9) {
    s *= 10.0;
    ++d;
  }
  return d;
}

// Moves a slider result onto the setting's step grid. Keyboard/gamepad tweaks change the value by the smallest display
// unit only; with a coarser step that would snap straight back, so a nav tweak moves by a whole step instead.
double SnapToStep(double nv, double before, double lo, double hi, double step, int decimals) {
  nv = std::clamp(nv, lo, hi);
  if (step <= 0.0) return nv;
  const double unit = std::pow(10.0, -decimals);
  if (step > unit * 1.5 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::fabs(nv - before) <= unit * 1.01 && nv != before) {
    nv = before + (nv > before ? step : -step);
    nv = std::clamp(nv, lo, hi);
  }
  if (nv >= hi) return hi;
  if (nv <= lo) return lo;
  // Round on the decimal grid by multiplying/dividing by a power of ten (35 / 100 is exactly the double nearest 0.35;
  // 35 * 0.01 would give 0.35000000000000003 and end up in the settings file and on the command line).
  const double scale = std::pow(10.0, decimals);
  const double snapped = std::round((lo + std::round((nv - lo) / step) * step) * scale) / scale;
  return std::clamp(snapped, lo, hi);
}

std::string ValueText(const SettingDef& def, const Json& v) {
  switch (def.type) {
    case SettingType::Bool:
      return v.is_boolean() && v.get<bool>() ? "On" : "Off";
    case SettingType::Enum:
      for (const Option& o : def.options) {
        if (SameValue(o.value, v)) return o.label;
      }
      break;
    default:
      break;
  }
  std::string s = FormatCvarValue(v);
  return s.empty() ? std::string("(empty)") : s;
}

// "Default: X" + range + the command-line option with the current value: the technical footer of a setting's tooltip.
std::string TechInfo(const SettingDef& def, const Json& current) {
  std::string s = "Default: " + ValueText(def, def.default_value);
  if (def.type == SettingType::Int || def.type == SettingType::Float) {
    s += "   Range: " + FormatCvarValue(Json(def.min)) + " \u2013 " + FormatCvarValue(Json(def.max));
  }
  if (!def.cvar) return s + "\nLauncher setting (not passed to the game)";
  const std::string arg = "--" + *def.cvar + "=" + FormatCvarValue(current);
  switch (def.emit) {
    case EmitMode::Always:
      return s + "\nCommand line: " + arg + "  (always passed)";
    case EmitMode::Never:
      return s + "\nNot passed on the command line";
    case EmitMode::NonDefault:
      break;
  }
  return s + "\nCommand line: " + arg + (SameValue(current, def.default_value) ? "  (passed only when changed)" : std::string());
}

}  // namespace

// ------------------------------------------------------------------------------------------------ lifecycle

LauncherApp::LauncherApp(void* hwnd, AppOptions options) : hwnd_(hwnd), options_(std::move(options)) {
  if (options_.schema_path.empty()) options_.schema_path = DefaultSchemaPath();
  if (options_.settings_path.empty()) options_.settings_path = DefaultSettingsPath();

  schema_ = LoadSchema(options_.schema_path, &schema_error_);
  if (schema_.version != 0) {
    values_ = LoadValues(schema_, options_.settings_path, &load_notes_);
    if (!load_notes_.empty()) {
      std::string joined;
      for (const std::string& n : load_notes_) joined += (joined.empty() ? "" : "\n") + n;
      Toast("Settings were repaired on load (hover for details).", StatusLine::Level::Warn, 10.0f);
      toast_detail_ = joined;
    }
  }
  show_advanced_ = options_.show_advanced;
  std::snprintf(search_, sizeof(search_), "%s", options_.search.c_str());
  if (!options_.initial_category.empty()) {
    for (size_t i = 0; i < schema_.categories.size(); ++i) {
      if (schema_.categories[i].id == options_.initial_category) category_ = static_cast<int>(i);
    }
  }
  RefreshValidation(true);
}

LauncherApp::~LauncherApp() { Flush(); }

void LauncherApp::SetScale(float scale) {
  scale_ = scale;
  ApplyTheme(scale);
  fonts_ = BuildFonts(ImGui::GetIO(), scale);
}

void LauncherApp::Flush() {
  if (!dirty_ || schema_.version == 0) return;
  std::string err;
  if (SaveValues(schema_, values_, options_.settings_path, &err)) {
    save_error_.clear();
  } else {
    save_error_ = err;
  }
  dirty_ = false;
}

void LauncherApp::Toast(std::string text, StatusLine::Level level, float seconds) {
  toast_text_ = std::move(text);
  toast_detail_.clear();
  toast_level_ = level;
  toast_until_ = ImGui::GetTime() + seconds;
}

// ------------------------------------------------------------------------------------------------ state

const SettingDef* LauncherApp::DataRootDef() const {
  for (const SettingDef& s : schema_.settings) {
    if (s.cvar && *s.cvar == kDataRootCvar) return &s;
  }
  return nullptr;
}

bool LauncherApp::IsModified(const SettingDef& def) const { return !SameValue(EffectiveValue(def, values_), def.default_value); }

bool LauncherApp::MatchesSearch(const SettingDef& def) const {
  if (needle_.empty()) return true;
  if (ContainsNoCase(def.label, needle_) || ContainsNoCase(def.help, needle_) || ContainsNoCase(def.id, needle_)) return true;
  if (def.cvar && ContainsNoCase(*def.cvar, needle_)) return true;
  if (const Category* c = schema_.FindCategory(def.category); c && ContainsNoCase(c->label, needle_)) return true;
  for (const Option& o : def.options) {
    if (ContainsNoCase(o.label, needle_)) return true;
  }
  return false;
}

// A setting the user tuned: changed from its default, not counting folder paths (a chosen game folder is setup, not tuning).
bool LauncherApp::IsTuned(const SettingDef& def) const { return def.type != SettingType::Path && IsModified(def); }

int LauncherApp::CountModified(const std::string& category_id) const {
  int n = 0;
  for (const SettingDef& s : schema_.settings) {
    if (s.category == category_id && IsActive(schema_, values_, s) && IsTuned(s)) ++n;
  }
  return n;
}

void LauncherApp::SetValue(const SettingDef& def, Json value) {
  Json& slot = values_[def.id];
  if (slot == value) return;
  slot = std::move(value);
  dirty_ = true;
  preview_dirty_ = true;
  last_change_time_ = ImGui::GetTime();
  launch_error_.clear();
}

void LauncherApp::SelectCategory(int index) {
  const int n = static_cast<int>(schema_.categories.size());
  if (n == 0) return;
  category_ = (index % n + n) % n;
  category_changed_ = true;
  search_[0] = '\0';
}

void LauncherApp::RefreshValidation(bool force) {
  const double now = ImGui::GetTime();
  const SettingDef* root_def = DataRootDef();
  const std::string root = root_def ? AsString(EffectiveValue(*root_def, values_)) : std::string();
  if (!force && checked_once_ && root == checked_root_ && now - last_validation_ < 1.5) return;

  const std::filesystem::path exe = FindGameExe();
  if (exe != game_exe_) preview_dirty_ = true;
  game_exe_ = exe;
  data_check_ = ValidateGameDataRoot(root.empty() ? std::filesystem::path() : PathFromUtf8(root));
  checked_root_ = root;
  checked_once_ = true;
  last_validation_ = now;
}

void LauncherApp::BuildPreviewText() {
  const std::filesystem::path exe = game_exe_.empty() ? LauncherDir() / L"mcla.exe" : game_exe_;
  args_ = BuildArgs(schema_, values_);
  preview_text_ = WideToUtf8(FormatCommandLine(exe, args_));
  preview_dirty_ = false;
}

std::vector<LauncherApp::StatusLine> LauncherApp::BuildStatus() const {
  std::vector<StatusLine> lines;
  if (game_exe_.empty()) {
    lines.push_back({StatusLine::Level::Error, "mcla.exe was not found next to the launcher (" + PathToUtf8(LauncherDir()) + ")", -1});
  }
  if (const SettingDef* root = DataRootDef()) {
    int goto_cat = -1;
    for (size_t i = 0; i < schema_.categories.size(); ++i) {
      if (schema_.categories[i].id == root->category) goto_cat = static_cast<int>(i);
    }
    if (checked_root_.empty()) {
      lines.push_back({StatusLine::Level::Error, "Choose the folder with the extracted game files (" + root->label + ").", goto_cat});
    } else if (!data_check_.ok) {
      std::string text = data_check_.issues.empty() ? std::string("Game data folder is not valid.") : data_check_.issues.front();
      if (data_check_.issues.size() > 1) text += "  (+" + std::to_string(data_check_.issues.size() - 1) + " more)";
      lines.push_back({StatusLine::Level::Error, std::move(text), goto_cat});
    }
  }
  if (FsrDoubleUpscale()) {
    int goto_cat = -1;
    if (const SettingDef* pe = schema_.FindSetting(kPresentEffectId)) {
      for (size_t i = 0; i < schema_.categories.size(); ++i) {
        if (schema_.categories[i].id == pe->category) goto_cat = static_cast<int>(i);
      }
    }
    lines.push_back({StatusLine::Level::Warn, kFsrTwiceShort, goto_cat});
  }
  if (!launch_error_.empty()) lines.push_back({StatusLine::Level::Warn, "Launch failed: " + launch_error_, -1});
  if (!save_error_.empty()) lines.push_back({StatusLine::Level::Warn, "Could not save settings: " + save_error_, -1});
  if (game_running_) lines.push_back({StatusLine::Level::Info, "The game is running (PID " + std::to_string(game_.pid()) + ").", -1});
  return lines;
}

// ------------------------------------------------------------------------------------------------ actions

bool LauncherApp::GamepadAllowed() const { return GetForegroundWindow() == static_cast<HWND>(hwnd_) && !game_running_; }

bool LauncherApp::FsrDoubleUpscale() const {
  const SettingDef* pe = schema_.FindSetting(kPresentEffectId);
  const SettingDef* taa = schema_.FindSetting(kTemporalAaId);
  if (!pe || !taa || !IsActive(schema_, values_, *taa)) return false;
  const Json& a = EffectiveValue(*pe, values_);
  const Json& b = EffectiveValue(*taa, values_);
  return a.is_string() && b.is_string() && a.get<std::string>() == "fsr" && b.get<std::string>() == "fsr";
}

void LauncherApp::DoLaunch() {
  Flush();
  RefreshValidation(true);  // the folder may have changed on disk since the last check: never launch on a stale result
  for (const StatusLine& s : BuildStatus()) {
    if (s.level == StatusLine::Level::Error) {
      Toast("Not launched: " + s.text, StatusLine::Level::Error, 10.0f);
      return;
    }
  }
  if (game_running_ || schema_.version == 0) return;
  if (preview_dirty_) BuildPreviewText();
  launch_error_.clear();
  std::string err;
  if (!game_.Launch(game_exe_, args_, std::filesystem::path(), &err)) {
    launch_error_ = err.empty() ? std::string("The game could not be started.") : err;
    Toast("Launch failed: " + launch_error_, StatusLine::Level::Error, 12.0f);
    return;
  }
  had_run_ = true;
  exit_reported_ = false;
  game_running_ = true;
  Toast("Game started (PID " + std::to_string(game_.pid()) + ").", StatusLine::Level::Ok, 5.0f);
  if (const SettingDef* close = schema_.FindSetting(kCloseOnLaunchId)) {
    const Json& v = EffectiveValue(*close, values_);
    if (v.is_boolean() && v.get<bool>()) quit_requested_ = true;
  }
}

void LauncherApp::DoSaveBat() {
  if (preview_dirty_) BuildPreviewText();
  const std::filesystem::path bat = DefaultSettingsPath().parent_path() / L"launch_mcla.bat";
  std::error_code ec;
  std::filesystem::create_directories(bat.parent_path(), ec);
  const std::filesystem::path exe = game_exe_.empty() ? LauncherDir() / L"mcla.exe" : game_exe_;
  std::string err;
  if (!WriteLaunchBat(bat, exe, args_, std::filesystem::path(), &err)) {
    Toast("Could not write the .bat: " + err, StatusLine::Level::Error, 8.0f);
    return;
  }
  const std::wstring params = L"/select,\"" + bat.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
  Toast("Saved " + PathToUtf8(bat), StatusLine::Level::Ok, 6.0f);
}

void LauncherApp::DoResetAll() {
  // Folder paths are machine setup rather than tuning: keep them across a reset.
  std::vector<std::pair<std::string, Json>> keep;
  for (const SettingDef& s : schema_.settings) {
    if (s.type == SettingType::Path) keep.emplace_back(s.id, EffectiveValue(s, values_));
  }
  ResetToDefaults(schema_, values_);
  for (auto& [id, v] : keep) values_[id] = std::move(v);
  dirty_ = true;
  preview_dirty_ = true;
  last_change_time_ = ImGui::GetTime();
  Toast("Settings reset to defaults (folders kept).", StatusLine::Level::Ok);
}

void LauncherApp::HandleShortcuts() {
  const ImGuiIO& io = ImGui::GetIO();
  const bool pad_ok = GamepadAllowed();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) focus_search_ = true;
  if (!schema_.categories.empty() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    if (pad_ok && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) SelectCategory(category_ + 1);
    if (pad_ok && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) SelectCategory(category_ - 1);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) SelectCategory(category_ + 1);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) SelectCategory(category_ - 1);
  }
  gamepad_hints_ = pad_ok && ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad;
}

// ------------------------------------------------------------------------------------------------ frame

void LauncherApp::Frame(float width, float height) {
  game_running_ = game_.IsRunning();
  if (had_run_ && !game_running_ && game_.exit_code() && !exit_reported_) {
    exit_reported_ = true;
    const std::uint32_t code = *game_.exit_code();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Game exited (code %u / 0x%08X).", code, code);
    Toast(buf, code == 0 ? StatusLine::Level::Info : StatusLine::Level::Warn, 10.0f);
  }

  needle_ = ToLowerAscii(search_);
  RefreshValidation(false);
  if (preview_dirty_) BuildPreviewText();
  HandleShortcuts();

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, pal::kWindow);
  constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("##launcher", nullptr, kRootFlags);
  ImGui::PopStyleVar();

  if (schema_.version == 0) {
    DrawSchemaError(width, height);
  } else {
    const float header_h = S(78), status_h = S(34), footer_h = S(142), sidebar_w = S(248);
    const float body_h = std::max(S(120), height - header_h - footer_h - status_h);
    DrawHeader(width, header_h);
    ImGui::SetCursorPos(ImVec2(0, header_h));
    DrawSidebar(sidebar_w, body_h);
    ImGui::SetCursorPos(ImVec2(sidebar_w, header_h));
    DrawContent(width - sidebar_w, body_h);
    ImGui::SetCursorPos(ImVec2(0, header_h + body_h));
    DrawFooter(width, footer_h);
    ImGui::SetCursorPos(ImVec2(0, header_h + body_h + footer_h));
    DrawStatusBar(width, status_h);
  }
  ImGui::End();
  ImGui::PopStyleColor();

  DrawModals();

  // Persist edits once the user stopped dragging/typing: no write per slider tick.
  if (dirty_ && !ImGui::IsAnyItemActive() && ImGui::GetTime() - last_change_time_ > 0.3) Flush();
}

void LauncherApp::DrawSchemaError(float width, float height) {
  ImGui::SetCursorPos(ImVec2(S(40), S(40)));
  ImGui::BeginChild("##schema_error", ImVec2(width - S(80), height - S(80)), ImGuiChildFlags_NavFlattened);
  ImGui::PushFont(fonts_.title);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kError);
  ImGui::TextUnformatted("The settings schema could not be loaded");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
  ImGui::TextWrapped("File: %s", PathToUtf8(options_.schema_path).c_str());
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushFont(fonts_.mono);
  ImGui::TextWrapped("%s", schema_error_.c_str());
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::Spacing();
  if (ImGui::Button("Reload")) {
    schema_ = LoadSchema(options_.schema_path, &schema_error_);
    if (schema_.version != 0) {
      values_ = LoadValues(schema_, options_.settings_path, &load_notes_);
      category_ = 0;
      preview_dirty_ = true;
      RefreshValidation(true);
    }
  }
  ImGui::EndChild();
}

// ------------------------------------------------------------------------------------------------ header

void LauncherApp::DrawHeader(float width, float height) {
  BeginPanel("##header", ImVec2(width, height), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
             pal::kHeader, 0.0f, ImVec2(0, 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  dl->AddRectFilledMultiColor(o, ImVec2(o.x + width, o.y + S(3)), Col(pal::kAccent), Col(pal::kAccent, 0.0f), Col(pal::kAccent, 0.0f), Col(pal::kAccent));
  dl->AddLine(ImVec2(o.x, o.y + height - 1), ImVec2(o.x + width, o.y + height - 1), Col(pal::kBorder));

  // Title block.
  ImGui::PushFont(fonts_.title);
  const float title_h = ImGui::GetTextLineHeight();
  const float title_w = ImGui::CalcTextSize(kTitle).x;
  ImGui::SetCursorPos(ImVec2(S(26), S(11)));
  ImGui::TextUnformatted(kTitle);
  ImGui::PopFont();
  ImGui::PushFont(fonts_.caption);
  ImGui::SetCursorPos(ImVec2(S(27), S(11) + title_h + S(1)));
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccent);
  ImGui::TextUnformatted(kSubtitle);
  ImGui::PopStyleColor();
  ImGui::PopFont();

  // Controls, right to left.
  const float frame_h = ImGui::GetFrameHeight();
  const float y = std::round((height - frame_h) * 0.5f) + S(1);
  float x = width - S(26);

  const char* reset_label = "Reset all";
  const float reset_w = ImGui::CalcTextSize(reset_label).x + ImGui::GetStyle().FramePadding.x * 2.0f + S(26);
  x -= reset_w;
  ImGui::SetCursorPos(ImVec2(x, y));
  {
    ImGui::PushID("reset_all");
    if (ImGui::Button("##reset_all", ImVec2(reset_w, frame_h))) open_reset_popup_ = true;
    const ImVec2 mn = ImGui::GetItemRectMin();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float icon_w = DrawIconAt(d, fonts_, icons::kUndo, ImVec2(mn.x + S(12), mn.y + (frame_h - (fonts_.icons ? fonts_.icons->FontSize : 0.0f)) * 0.5f), Col(pal::kTextDim));
    const float text_x = mn.x + S(12) + (icon_w > 0 ? icon_w + S(8) : 0.0f);
    d->AddText(ImVec2(text_x, mn.y + ImGui::GetStyle().FramePadding.y), Col(pal::kText), reset_label);
    ItemTooltip("Set every setting back to its default. Folder paths are kept.");
    ImGui::PopID();
  }

  // Preset combo + label.
  // Wide enough for the longest preset name, but never so wide that it squeezes the search box out of the header.
  const float preset_label_w = ImGui::CalcTextSize("Preset").x;
  float longest = ImGui::CalcTextSize("Custom").x;
  for (const Preset& p : schema_.presets) longest = std::max(longest, ImGui::CalcTextSize(p.label.c_str()).x);
  const float combo_want = longest + ImGui::GetStyle().FramePadding.x * 2.0f + frame_h + S(8);
  const float combo_room = (x - S(12)) - (S(26) + title_w + S(34)) - S(110) - S(22) - (S(10) + preset_label_w);
  const float combo_w = std::clamp(combo_want, S(150), std::max(S(150), combo_room));
  x -= S(12) + combo_w;
  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::SetNextItemWidth(combo_w);
  const Preset* active = MatchingPreset(schema_, values_);
  const std::string preview = active ? active->label : std::string("Custom");
  if (ImGui::BeginCombo("##preset", preview.c_str())) {
    for (const Preset& p : schema_.presets) {
      const bool selected = active == &p;
      if (ImGui::Selectable(p.label.c_str(), selected)) {
        std::string err;
        if (ApplyPreset(schema_, p.id, values_, &err)) {
          dirty_ = true;
          preview_dirty_ = true;
          last_change_time_ = ImGui::GetTime();
          Toast("Preset applied: " + p.label, StatusLine::Level::Ok);
        } else {
          Toast("Preset failed: " + err, StatusLine::Level::Error);
        }
      }
      if (selected) ImGui::SetItemDefaultFocus();
      ItemTooltip(p.help);
    }
    if (schema_.presets.empty()) ImGui::TextDisabled("No presets");
    ImGui::EndCombo();
  }
  if (!active) ItemTooltip("Your current settings do not match a preset exactly.");
  const float label_w = ImGui::CalcTextSize("Preset").x;
  x -= S(10) + label_w;
  ImGui::SetCursorPos(ImVec2(x, y + ImGui::GetStyle().FramePadding.y));
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
  ImGui::TextUnformatted("Preset");
  ImGui::PopStyleColor();

  // Search box (what is left of the title block).
  const float right_edge = x - S(22);
  const float left_edge = S(26) + title_w + S(34);
  const float search_w = std::clamp(right_edge - left_edge, S(110), S(260));
  x = right_edge - search_w;
  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::SetNextItemWidth(search_w);
  if (focus_search_) {
    ImGui::SetKeyboardFocusHere();
    focus_search_ = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(34), ImGui::GetStyle().FramePadding.y));
  ImGui::InputTextWithHint("##search", "Search settings", search_, sizeof(search_), ImGuiInputTextFlags_EscapeClearsAll);
  ImGui::PopStyleVar();
  {
    const ImVec2 mn = ImGui::GetItemRectMin();
    const float icon_sz = fonts_.icons ? fonts_.icons->FontSize : 0.0f;
    DrawIconAt(ImGui::GetWindowDrawList(), fonts_, icons::kSearch, ImVec2(mn.x + S(11), mn.y + (frame_h - icon_sz) * 0.5f), Col(pal::kTextDim));
    ItemTooltip("Filter all settings by name, description or command-line option (Ctrl+F).");
  }

  ImGui::EndChild();
}

// ------------------------------------------------------------------------------------------------ sidebar

void LauncherApp::DrawSidebar(float width, float height) {
  BeginPanel("##sidebar", ImVec2(width, height), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
             pal::kSidebar, 0.0f, ImVec2(0, 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetWindowPos();
  dl->AddLine(ImVec2(origin.x + width - 1, origin.y), ImVec2(origin.x + width - 1, origin.y + height), Col(pal::kBorder));

  const bool searching = !needle_.empty();
  const float margin = S(12);
  const float row_h = S(46);
  const float icon_sz = fonts_.icons ? fonts_.icons->FontSize : 0.0f;

  ImGui::SetCursorPos(ImVec2(margin, S(16)));
  ImGui::PushFont(fonts_.caption);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDisabled);
  ImGui::TextUnformatted("CATEGORIES");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  const float list_top = S(16) + ImGui::GetTextLineHeight() + S(8);

  // The persisted "close the launcher after starting the game" setting gets a toggle next to the two view options, so it is
  // one click away from Launch (it also stays in its category like every other setting).
  const SettingDef* close_def = schema_.FindSetting(kCloseOnLaunchId);
  bool close_value = false;
  if (close_def) {
    const Json& v = EffectiveValue(*close_def, values_);
    close_value = v.is_boolean() && v.get<bool>();
  }
  const float toggle_h = std::round(ImGui::GetFrameHeight() * 0.74f);
  const float toggle_w = std::round(toggle_h * 1.85f);
  const float row = S(36);
  const float toggles_h = row * (close_def ? 3.0f : 2.0f) + S(14);
  float ty = height - toggles_h;

  // Category list: scrolls by itself when the window is too short for every category plus the toggles.
  ImGui::SetCursorPos(ImVec2(0, list_top));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, S(3)));
  BeginPanel("##categories", ImVec2(width - 1.0f, std::max(S(80), ty - S(8) - list_top)),
             ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None, pal::kSidebar, 0.0f, ImVec2(margin, 0));
  ImDrawList* cdl = ImGui::GetWindowDrawList();
  const float row_w = ImGui::GetContentRegionAvail().x;
  for (int i = 0; i < static_cast<int>(schema_.categories.size()); ++i) {
    const Category& cat = schema_.categories[i];
    ImGui::PushID(i);
    const bool selected = !searching && i == category_;
    if (ImGui::Selectable("##cat", selected, ImGuiSelectableFlags_None, ImVec2(row_w, row_h))) SelectCategory(i);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const ImU32 fg = Col(selected ? pal::kText : (hovered ? pal::kText : pal::kTextDim));
    if (selected) {
      cdl->AddRectFilled(ImVec2(mn.x, mn.y + S(9)), ImVec2(mn.x + S(3.5f), mx.y - S(9)), Col(pal::kAccent), S(2));
    }
    DrawIconAt(cdl, fonts_, IconForCategory(cat.id), ImVec2(mn.x + S(18), mn.y + (row_h - icon_sz) * 0.5f), selected ? Col(pal::kAccent) : fg);
    const float text_h = ImGui::GetTextLineHeight();
    cdl->AddText(ImVec2(mn.x + S(18) + (fonts_.icons ? icon_sz + S(14) : 0.0f), mn.y + (row_h - text_h) * 0.5f), fg, cat.label.c_str());
    if (const int modified = CountModified(cat.id); modified > 0) {
      cdl->AddCircleFilled(ImVec2(mx.x - S(16), mn.y + row_h * 0.5f), S(3.5f), Col(pal::kAccent), 12);
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Toggles pinned to the bottom of the sidebar.
  dl->AddLine(ImVec2(origin.x + margin, origin.y + ty - S(8)), ImVec2(origin.x + width - margin - 1, origin.y + ty - S(8)), Col(pal::kBorder));

  struct Toggle {
    const char* id;
    const char* label;
    bool* value;
    const char* tip;
  };
  Toggle toggles[] = {
      {"##adv", "Advanced settings", &show_advanced_, "Show expert and diagnostic settings that most players never need."},
      {"##desc", "Descriptions", &show_descriptions_, "Show each setting's description under its name. When off, hover a setting to read it."},
      {"##close", "Close on launch", &close_value, "Close this launcher as soon as the game has started."},
  };
  for (const Toggle& t : toggles) {
    if (t.value == &close_value && !close_def) continue;
    ImGui::SetCursorPos(ImVec2(margin + S(6), ty + (row - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
    ImGui::TextUnformatted(t.label);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(width - margin - S(6) - toggle_w, ty + (row - toggle_h) * 0.5f));
    if (ToggleSwitch(t.id, t.value, toggle_h) && t.value == &close_value) SetValue(*close_def, Json(close_value));
    ItemTooltip(t.tip);
    ty += row;
  }

  ImGui::EndChild();
}

// ------------------------------------------------------------------------------------------------ content

void LauncherApp::DrawContent(float width, float height) {
  BeginPanel("##content", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_None,
             pal::kWindow, 0.0f, ImVec2(S(30), S(22)));
  if (category_changed_) {
    ImGui::SetScrollY(0.0f);
    category_changed_ = false;
  }

  const bool searching = !needle_.empty();
  const Category* cat = (!searching && category_ >= 0 && category_ < static_cast<int>(schema_.categories.size())) ? &schema_.categories[category_] : nullptr;

  std::vector<const SettingDef*> shown;
  int hidden_advanced = 0;
  for (const SettingDef& s : schema_.settings) {
    if (!searching && (!cat || s.category != cat->id)) continue;
    if (searching && !MatchesSearch(s)) continue;
    if (!IsActive(schema_, values_, s)) continue;
    if (!searching && !show_advanced_ && s.advanced) {
      ++hidden_advanced;
      continue;
    }
    shown.push_back(&s);
  }

  // Title row.
  ImGui::PushFont(fonts_.heading);
  ImGui::TextUnformatted(searching ? "Search results" : (cat ? cat->label.c_str() : ""));
  ImGui::PopFont();
  {
    ImGui::PushFont(fonts_.caption);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
    int modified = 0;
    for (const SettingDef* s : shown) modified += IsTuned(*s) ? 1 : 0;
    char line[160];
    if (searching) {
      std::snprintf(line, sizeof(line), "%d match%s for \u201C%s\u201D", static_cast<int>(shown.size()), shown.size() == 1 ? "" : "es", search_);
    } else {
      std::snprintf(line, sizeof(line), "%d setting%s%s", static_cast<int>(shown.size()), shown.size() == 1 ? "" : "s",
                    modified > 0 ? ("  \u00B7  " + std::to_string(modified) + " changed from default").c_str() : "");
    }
    ImGui::TextUnformatted(line);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, S(8)));

  if (shown.empty()) {
    ImGui::Dummy(ImVec2(0, S(30)));
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
    ImGui::TextWrapped("%s", searching ? "No setting matches the search." : "Nothing to show here.");
    ImGui::PopStyleColor();
  }
  for (const SettingDef* s : shown) DrawSettingCard(*s, searching);

  if (hidden_advanced > 0) {
    ImGui::PushFont(fonts_.caption);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDim);
    ImGui::TextWrapped("%d advanced setting%s hidden. Turn on \u201CAdvanced settings\u201D in the sidebar to show %s.", hidden_advanced,
                       hidden_advanced == 1 ? "" : "s", hidden_advanced == 1 ? "it" : "them");
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, S(14)));

  ImGui::EndChild();
}

std::vector<LauncherApp::CardExtra> LauncherApp::CardExtras(const SettingDef& def) const {
  std::vector<CardExtra> extras;
  if (show_descriptions_ && def.type == SettingType::Enum) {
    for (const Option& o : def.options) {
      if (SameValue(o.value, EffectiveValue(def, values_)) && !o.help.empty()) extras.push_back({o.label + ": " + o.help, pal::kTextDim});
    }
  }
  if (def.cvar && *def.cvar == kDataRootCvar) {
    if (checked_root_.empty()) {
      extras.push_back({"No folder chosen yet.", pal::kWarn});
    } else if (data_check_.ok) {
      extras.push_back({"Game files found.", pal::kSuccess});
    } else {
      for (const std::string& issue : data_check_.issues) extras.push_back({issue, pal::kError});
    }
  }
  if (FsrDoubleUpscale() && (def.id == kPresentEffectId || def.id == kTemporalAaId)) extras.push_back({kFsrTwiceWarning, pal::kWarn});
  return extras;
}

void LauncherApp::DrawSettingCard(const SettingDef& def, bool show_category) {
  ImGui::PushID(def.id.c_str());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float pad = S(16);
  const float gap = S(12);
  const float frame_h = ImGui::GetFrameHeight();
  const bool modified = IsModified(def);

  float widget_w = std::clamp(avail * 0.36f, S(230), S(340));
  if (def.type == SettingType::Path || def.type == SettingType::String) widget_w = std::clamp(avail * 0.5f, S(300), S(520));
  if (def.type == SettingType::Bool) widget_w = S(90);
  const float reset_w = S(30);
  const float text_w = std::max(S(140), avail - pad * 2.0f - widget_w - reset_w - gap * 2.0f);

  const std::vector<CardExtra> extras = CardExtras(def);
  const float label_h = ImGui::GetTextLineHeight();
  const float small_gap = S(3);
  float help_h = 0.0f;
  std::vector<float> extra_h;
  ImGui::PushFont(fonts_.caption);
  const bool has_help = show_descriptions_ && !def.help.empty();
  if (has_help) help_h = ImGui::CalcTextSize(def.help.c_str(), nullptr, false, text_w).y;
  for (const CardExtra& e : extras) extra_h.push_back(ImGui::CalcTextSize(e.text.c_str(), nullptr, false, text_w).y);
  ImGui::PopFont();
  float left_h = label_h;
  if (has_help) left_h += small_gap + help_h;
  for (const float h : extra_h) left_h += small_gap + h;
  const float card_h = std::max(left_h, frame_h) + pad * 2.0f;
  const float spacing = S(10);

  ImGui::Dummy(ImVec2(avail, card_h + spacing));  // claims the space; content is placed by hand below
  const ImVec2 mn = p;
  const ImVec2 mx(p.x + avail, p.y + card_h);
  const bool hovered_card = ImGui::IsMouseHoveringRect(mn, mx) && ImGui::IsWindowHovered();
  dl->AddRectFilled(mn, mx, Col(hovered_card ? pal::kCardHover : pal::kCard), S(10));
  dl->AddRect(mn, mx, Col(pal::kBorder, 0.7f), S(10));
  if (modified) dl->AddRectFilled(ImVec2(mn.x, mn.y + S(12)), ImVec2(mn.x + S(3.5f), mx.y - S(12)), Col(pal::kAccent), S(2));

  // Left block: label (+ chips), description, selected-option help, validation lines.
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad));
  ImGui::TextUnformatted(def.label.c_str());
  float chip_x = ImGui::GetItemRectMax().x + S(10);
  auto chip = [&](const char* text, ImU32 bg, ImU32 fg) {
    ImGui::SetCursorScreenPos(ImVec2(chip_x, p.y + pad));
    Badge(text, bg, fg, label_h);
    chip_x = ImGui::GetItemRectMax().x + S(6);
  };
  if (def.advanced) chip("ADVANCED", Col(pal::kWarn, 0.18f), Col(pal::kWarn));
  if (show_category) {
    if (const Category* c = schema_.FindCategory(def.category)) chip(c->label.c_str(), Col(pal::kFrame), Col(pal::kTextDim));
  }

  float ly = p.y + pad + label_h + small_gap;
  ImGui::PushFont(fonts_.caption);
  auto paragraph = [&](const std::string& text, const ImVec4& color) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad, ly));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + text_w);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ly += ImGui::GetItemRectSize().y + small_gap;
  };
  if (has_help) paragraph(def.help, pal::kTextDim);
  for (const CardExtra& e : extras) paragraph(e.text, e.color);
  ImGui::PopFont();

  // Hover target over the whole text block (plain text has no ID, so it cannot drive a delayed tooltip): shows the
  // setting's technical details, and its description when the inline descriptions are off. Skipped by keyboard/gamepad nav.
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad));
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##info", ImVec2(text_w, std::max(left_h, 1.0f)));
  ImGui::PopItemFlag();
  ItemTooltip(show_descriptions_ ? std::string() : def.help, TechInfo(def, EffectiveValue(def, values_)));

  // Right block: the control, vertically centred, with a per-setting reset to its left.
  const ImVec2 wpos(p.x + avail - pad - widget_w, p.y + std::round((card_h - frame_h) * 0.5f));
  DrawControl(def, wpos, widget_w, frame_h);
  if (modified) {
    ImGui::SetCursorScreenPos(ImVec2(wpos.x - gap - reset_w, wpos.y));
    if (IconButton("##reset", icons::kUndo, "R", fonts_, ImVec2(reset_w, frame_h))) SetValue(def, def.default_value);
    ItemTooltip("Reset to default", ValueText(def, def.default_value));
  }


  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + card_h + spacing));
  ImGui::PopID();
}

void LauncherApp::DrawControl(const SettingDef& def, ImVec2 pos, float widget_w, float frame_h) {
  const Json& cur = EffectiveValue(def, values_);
  ImGui::SetCursorScreenPos(pos);
  switch (def.type) {
    case SettingType::Bool: {
      bool v = cur.is_boolean() && cur.get<bool>();
      const float th = std::round(frame_h * 0.74f);
      const float tw = std::round(th * 1.85f);
      const char* txt = v ? "On" : "Off";
      const ImVec2 ts = ImGui::CalcTextSize(txt);
      ImGui::SetCursorScreenPos(ImVec2(pos.x + widget_w - tw, pos.y + std::round((frame_h - th) * 0.5f)));
      const bool toggled = ToggleSwitch("##v", &v, th);
      ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + widget_w - tw - S(10) - ts.x, pos.y + std::round((frame_h - ts.y) * 0.5f)),
                                          Col(v ? pal::kText : pal::kTextDim), txt);
      if (toggled) SetValue(def, Json(v));
      break;
    }
    case SettingType::Int:
    case SettingType::Float: {
      const bool is_int = def.type == SettingType::Int;
      const double step = def.step > 0.0 ? def.step : (is_int ? 1.0 : 0.0);
      const int decimals = DecimalsFor(def);
      const double lo = def.min, hi = def.max;
      const bool use_slider = step <= 0.0 ? true : (hi - lo) / step <= 4000.0;
      ImGui::SetNextItemWidth(widget_w);
      if (is_int) {
        long long v = AsInt(cur);
        const long long before = v, ilo = static_cast<long long>(lo), ihi = static_cast<long long>(hi);
        bool changed;
        if (use_slider) {
          changed = ImGui::SliderScalar("##v", ImGuiDataType_S64, &v, &ilo, &ihi, "%lld", ImGuiSliderFlags_AlwaysClamp);
        } else {
          const long long s1 = static_cast<long long>(step), s2 = s1 * 10;
          changed = ImGui::InputScalar("##v", ImGuiDataType_S64, &v, &s1, &s2, "%lld");
        }
        if (changed) {
          const double snapped = SnapToStep(static_cast<double>(v), static_cast<double>(before), lo, hi, step, 0);
          SetValue(def, Json(static_cast<long long>(std::llround(snapped))));
        }
      } else {
        double v = AsDouble(cur);
        const double before = v;
        char fmt[16];
        std::snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
        bool changed;
        if (use_slider) {
          changed = ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &lo, &hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        } else {
          const double s1 = step, s2 = step * 10.0;
          changed = ImGui::InputScalar("##v", ImGuiDataType_Double, &v, &s1, &s2, fmt);
        }
        if (changed) SetValue(def, Json(SnapToStep(v, before, lo, hi, step, decimals)));
      }
      if (use_slider) ItemTooltip(show_descriptions_ ? std::string() : def.help, "Ctrl+Click to type a value");
      else ItemTooltip(show_descriptions_ ? std::string() : def.help);
      break;
    }
    case SettingType::Enum: {
      int index = -1;
      for (size_t i = 0; i < def.options.size(); ++i) {
        if (SameValue(def.options[i].value, cur)) index = static_cast<int>(i);
      }
      const std::string preview = index >= 0 ? def.options[index].label : "Custom (" + FormatCvarValue(cur) + ")";
      ImGui::SetNextItemWidth(widget_w);
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (size_t i = 0; i < def.options.size(); ++i) {
          const Option& o = def.options[i];
          std::string label = o.label;
          if (SameValue(o.value, def.default_value)) label += "  (default)";
          const bool selected = static_cast<int>(i) == index;
          if (ImGui::Selectable(label.c_str(), selected)) SetValue(def, o.value);
          if (selected) ImGui::SetItemDefaultFocus();
          ItemTooltip(o.help);
        }
        ImGui::EndCombo();
      }
      if (!show_descriptions_) ItemTooltip(def.help);
      break;
    }
    case SettingType::String:
    case SettingType::Path: {
      const bool is_path = def.type == SettingType::Path;
      std::string text = AsString(cur);
      const float browse_w = is_path ? ImGui::CalcTextSize("Browse\u2026").x + ImGui::GetStyle().FramePadding.x * 2.0f + S(24) : 0.0f;
      const float input_w = widget_w - (is_path ? browse_w + S(8) : 0.0f);
      ImGui::SetNextItemWidth(input_w);
      if (InputTextString("##v", is_path ? "Choose a folder" : "", &text, ImGuiInputTextFlags_AutoSelectAll)) {
        SetValue(def, Json(text));
      }
      if (!show_descriptions_) ItemTooltip(def.help);
      if (is_path) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + input_w + S(8), pos.y));
        ImGui::PushID("browse");
        if (ImGui::Button("##browse", ImVec2(browse_w, frame_h))) {
          const std::string current = AsString(cur);
          std::filesystem::path start;
          if (!current.empty()) {
            std::error_code ec;
            const std::filesystem::path candidate = PathFromUtf8(current);
            if (std::filesystem::is_directory(candidate, ec)) start = candidate;
          }
          std::wstring picked;
          if (PickFolder(hwnd_, Utf8ToWide(def.label), start.wstring(), &picked)) {
            SetValue(def, Json(PathToUtf8(std::filesystem::path(picked))));
          }
          ImGui::GetIO().ClearInputKeys();
          ImGui::GetIO().ClearInputMouse();
        }
        const ImVec2 bmn = ImGui::GetItemRectMin();
        ImDrawList* d = ImGui::GetWindowDrawList();
        const float icon_sz = fonts_.icons ? fonts_.icons->FontSize : 0.0f;
        const float iw = DrawIconAt(d, fonts_, icons::kFolderOpen, ImVec2(bmn.x + S(12), bmn.y + (frame_h - icon_sz) * 0.5f), Col(pal::kTextDim));
        d->AddText(ImVec2(bmn.x + S(12) + (iw > 0 ? iw + S(8) : 0.0f), bmn.y + ImGui::GetStyle().FramePadding.y), Col(pal::kText), "Browse\u2026");
        ItemTooltip("Pick a folder with the file dialog.");
        ImGui::PopID();
      }
      break;
    }
  }
}

// ------------------------------------------------------------------------------------------------ footer + status

void LauncherApp::DrawFooter(float width, float height) {
  BeginPanel("##footer", ImVec2(width, height), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
             pal::kHeader, 0.0f, ImVec2(0, 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  dl->AddLine(o, ImVec2(o.x + width, o.y), Col(pal::kBorder));

  const float margin = S(26);
  const float launch_w = S(290), launch_h = S(70);
  const float left_w = width - launch_w - margin * 3.0f;

  // Command line preview.
  ImGui::SetCursorPos(ImVec2(margin, S(14)));
  ImGui::PushFont(fonts_.caption);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextDisabled);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("COMMAND LINE");
  ImGui::PopStyleColor();
  ImGui::PopFont();

  ImGui::PushFont(fonts_.caption);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(3)));
  const float btn_h = ImGui::GetFrameHeight();
  const char* copy_label = "Copy";
  const char* bat_label = "Save .bat";
  const float copy_w = ImGui::CalcTextSize(copy_label).x + S(20);
  const float bat_w = ImGui::CalcTextSize(bat_label).x + S(20);
  const float btn_y = S(11);
  ImGui::SetCursorPos(ImVec2(margin + left_w - copy_w - S(8) - bat_w, btn_y));
  if (ImGui::Button(bat_label, ImVec2(bat_w, btn_h))) DoSaveBat();
  ItemTooltip("Write launch_mcla.bat with this exact command line next to your settings, for debugging.");
  ImGui::SetCursorPos(ImVec2(margin + left_w - copy_w, btn_y));
  if (ImGui::Button(copy_label, ImVec2(copy_w, btn_h))) {
    ImGui::SetClipboardText(preview_text_.c_str());
    Toast("Command line copied to the clipboard.", StatusLine::Level::Ok);
  }
  ItemTooltip("Copy the command line to the clipboard.");
  ImGui::PopStyleVar();
  ImGui::PopFont();

  const float box_y = btn_y + btn_h + S(8);
  ImGui::SetCursorPos(ImVec2(margin, box_y));
  BeginPanel("##cmdline", ImVec2(left_w, height - box_y - S(16)),
             ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_None,
             pal::kInset, S(8), ImVec2(S(12), S(8)), pal::kBorder);
  ImGui::PushFont(fonts_.mono);
  ImGui::PushStyleColor(ImGuiCol_Text, Rgb(0xC9D1DD));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(preview_text_.c_str(), preview_text_.c_str() + preview_text_.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndChild();

  // Launch.
  const std::vector<StatusLine> status = BuildStatus();
  const StatusLine* blocking = nullptr;
  for (const StatusLine& s : status) {
    if (s.level == StatusLine::Level::Error) {
      blocking = &s;
      break;
    }
  }
  const bool can_launch = !blocking && !game_running_;
  const float lx = width - margin - launch_w;
  ImGui::SetCursorPos(ImVec2(lx, S(16)));
  const bool clicked = ImGui::InvisibleButton("##launch", ImVec2(launch_w, launch_h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const ImVec2 bmn = ImGui::GetItemRectMin();
  const ImVec2 bmx = ImGui::GetItemRectMax();
  ImU32 fill;
  ImU32 text_col = Col(Rgb(0xFFFFFF));
  if (game_running_) {
    fill = Col(pal::kFrame);
    text_col = Col(pal::kText);
  } else if (!can_launch) {
    fill = Col(pal::kFrame, 0.8f);
    text_col = Col(pal::kTextDisabled);
  } else {
    fill = Col(held ? pal::kAccentActive : (hovered ? pal::kAccentHover : pal::kAccent));
  }
  dl->AddRectFilled(bmn, bmx, fill, S(12));
  if (can_launch) dl->AddRect(bmn, bmx, Col(pal::kAccentHover, 0.55f), S(12), 0, S(1.5f));
  ImGui::RenderNavCursor(ImRect(bmn, bmx), ImGui::GetItemID());
  {
    ImGui::PushFont(fonts_.title);
    const char* label = game_running_ ? "Running\u2026" : "Launch game";
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float icon_sz = fonts_.icons ? fonts_.icons->FontSize * 1.35f : 0.0f;
    const float spinner_d = S(26);
    const float lead = game_running_ ? spinner_d : (fonts_.icons ? icon_sz : 0.0f);
    const float total = ts.x + (lead > 0 ? lead + S(12) : 0.0f);
    float x = std::round((bmn.x + bmx.x - total) * 0.5f);
    const float cy = (bmn.y + bmx.y) * 0.5f;
    if (game_running_) {
      DrawSpinner(dl, ImVec2(x + spinner_d * 0.5f, cy), spinner_d * 0.5f, S(3), Col(pal::kAccent));
      x += spinner_d + S(12);
    } else if (fonts_.icons) {
      dl->AddText(fonts_.icons, icon_sz, ImVec2(x, cy - icon_sz * 0.5f - S(1)), text_col, icons::kPlay);
      x += icon_sz + S(12);
    }
    dl->AddText(ImVec2(x, std::round(cy - ts.y * 0.5f)), text_col, label);
    ImGui::PopFont();
  }
  if (can_launch) {
    ItemTooltip("Start the game with the command line shown on the left. (Ctrl+Enter or the gamepad Start button also launches.)");
  }

  // Reason / state line under the button.
  ImGui::SetCursorPos(ImVec2(lx, S(16) + launch_h + S(8)));
  ImGui::PushFont(fonts_.caption);
  ImVec4 reason_col = pal::kTextDim;
  std::string reason;
  if (game_running_) {
    reason = "The game is running (PID " + std::to_string(game_.pid()) + ").";
  } else if (blocking) {
    reason = blocking->text;
    reason_col = pal::kError;
  } else {
    reason = "Ready to launch.";
    reason_col = pal::kSuccess;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, reason_col);
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + launch_w);
  ImGui::TextUnformatted(reason.c_str(), reason.c_str() + reason.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();

  const bool shortcut = (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ||
                        (GamepadAllowed() && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false));
  if (can_launch && (clicked || shortcut)) DoLaunch();

  ImGui::EndChild();
}

void LauncherApp::DrawStatusBar(float width, float height) {
  BeginPanel("##status", ImVec2(width, height), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
             pal::kInset, 0.0f, ImVec2(0, 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  dl->AddLine(o, ImVec2(o.x + width, o.y), Col(pal::kBorder));

  // Pick the most important message: a live toast, else the first problem, else "ready".
  const std::vector<StatusLine> status = BuildStatus();
  StatusLine shown;
  std::string detail;
  if (ImGui::GetTime() < toast_until_) {
    shown = {toast_level_, toast_text_, -1};
    detail = toast_detail_;
  } else if (!status.empty()) {
    shown = status.front();
    for (const StatusLine& s : status) detail += (detail.empty() ? "" : "\n") + s.text;
  } else {
    shown = {StatusLine::Level::Ok, game_exe_.empty() ? "" : "Ready  \u00B7  " + PathToUtf8(game_exe_), -1};
  }

  const float margin = S(26);
  const float text_h = ImGui::GetTextLineHeight();
  const float cy = std::round((height - text_h) * 0.5f);
  const float icon_sz = fonts_.icons ? fonts_.icons->FontSize : 0.0f;
  ImVec4 color = pal::kSuccess;
  const char* icon = icons::kCheck;
  switch (shown.level) {
    case StatusLine::Level::Ok: break;
    case StatusLine::Level::Info: color = pal::kAccent; icon = icons::kInfo; break;
    case StatusLine::Level::Warn: color = pal::kWarn; icon = icons::kWarning; break;
    case StatusLine::Level::Error: color = pal::kError; icon = icons::kError; break;
  }
  float x = o.x + margin;
  if (fonts_.icons) {
    DrawIconAt(dl, fonts_, icon, ImVec2(x, o.y + (height - icon_sz) * 0.5f), Col(color));
    x += icon_sz + S(10);
  }

  // Right side: gamepad hints or the settings file path.
  std::string right = gamepad_hints_ ? "A select   B back   LB/RB category   Start launch" : "Settings are saved automatically";
  ImGui::PushFont(fonts_.caption);
  const float right_w = ImGui::CalcTextSize(right.c_str()).x;
  float fix_w = 0.0f;
  const float text_right = o.x + width - margin - right_w - S(24);
  if (shown.goto_category >= 0) {
    fix_w = ImGui::CalcTextSize("Open").x + S(22);
  }
  ImGui::PopFont();

  dl->PushClipRect(ImVec2(x, o.y), ImVec2(text_right - fix_w, o.y + height), true);
  dl->AddText(ImVec2(x, o.y + cy), Col(shown.level == StatusLine::Level::Ok ? pal::kTextDim : pal::kText), shown.text.c_str());
  dl->PopClipRect();
  if (!detail.empty() || shown.text.size() > 80) {
    ImGui::SetCursorScreenPos(ImVec2(x, o.y));
    ImGui::InvisibleButton("##status_text", ImVec2(std::max(1.0f, text_right - fix_w - x), height));
    ItemTooltip(detail.empty() ? shown.text : detail);
  }

  if (shown.goto_category >= 0) {
    const ImVec2 sz(fix_w, height - S(10));
    ImGui::PushFont(fonts_.caption);
    ImGui::SetCursorScreenPos(ImVec2(text_right - fix_w + S(4), o.y + S(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(1)));
    if (ImGui::Button("Open", sz)) SelectCategory(shown.goto_category);
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }

  ImGui::PushFont(fonts_.caption);
  dl->AddText(ImVec2(o.x + width - margin - right_w, o.y + std::round((height - ImGui::GetTextLineHeight()) * 0.5f)), Col(pal::kTextDisabled), right.c_str());
  ImGui::PopFont();

  ImGui::EndChild();
}

void LauncherApp::DrawModals() {
  if (open_reset_popup_) {
    ImGui::OpenPopup(kResetPopup);
    open_reset_popup_ = false;
  }
  const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(22)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
  if (ImGui::BeginPopupModal(kResetPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::PushFont(fonts_.heading);
    ImGui::TextUnformatted("Reset all settings?");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + S(380));
    ImGui::TextUnformatted("Every setting goes back to its default value. Your folder paths are kept. This cannot be undone.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(10)));
    const float bw = S(130);
    ImGui::PushStyleColor(ImGuiCol_Button, pal::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal::kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal::kAccentActive);
    if (ImGui::Button("Reset", ImVec2(bw, 0))) {
      DoResetAll();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

}  // namespace launcher::ui
