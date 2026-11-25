// The launcher's UI: header (title, search, presets, reset), category sidebar, schema-driven settings cards, command
// line preview, the Launch button and a status bar. It owns the loaded schema and the current Values and only talks to
// the core through core/*.h.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/game_process.h"
#include "core/settings_schema.h"
#include "core/settings_store.h"
#include "imgui.h"
#include "ui/fonts.h"

namespace launcher::ui {

struct AppOptions {
  std::filesystem::path schema_path;    // default: DefaultSchemaPath()
  std::filesystem::path settings_path;  // default: DefaultSettingsPath()
  std::string initial_category;         // category id to open first (testing / shortcuts)
  bool show_advanced = false;
  std::string search;                   // initial search text
};

class LauncherApp {
 public:
  LauncherApp(void* hwnd, AppOptions options);
  ~LauncherApp();

  // Rebuilds fonts and the style for a DPI scale. Call outside a frame, after the renderer backend released its
  // device objects (they are re-created by the backend's NewFrame).
  void SetScale(float scale);
  float scale() const { return scale_; }

  // Builds the whole UI for one frame (between ImGui::NewFrame() and ImGui::Render()). `width`/`height` are the client
  // size in pixels.
  void Frame(float width, float height);

  // Writes pending setting changes now (called on exit and before launching).
  void Flush();

  // True once the user asked to leave (closed on launch).
  bool quit_requested() const { return quit_requested_; }
  // True while a game started from this launcher is running (the main loop throttles the frame rate then).
  bool game_running() const { return game_running_; }

 private:
  struct StatusLine {
    enum class Level { Ok, Info, Warn, Error } level = Level::Ok;
    std::string text;
    int goto_category = -1;  // index of the category that fixes this problem, or -1
  };

  struct CardExtra {
    std::string text;
    ImVec4 color;
  };

  // --- state helpers
  float S(float v) const { return v * scale_; }
  void SetValue(const SettingDef& def, Json value);
  bool IsModified(const SettingDef& def) const;
  bool IsTuned(const SettingDef& def) const;
  bool MatchesSearch(const SettingDef& def) const;
  int CountModified(const std::string& category_id) const;
  const SettingDef* DataRootDef() const;
  void RefreshValidation(bool force);
  std::vector<StatusLine> BuildStatus() const;
  void BuildPreviewText();
  void DoLaunch();
  void DoSaveBat();
  void DoResetAll();
  void Toast(std::string text, StatusLine::Level level, float seconds = 4.0f);
  void SelectCategory(int index);

  // --- drawing
  void DrawSchemaError(float width, float height);
  void DrawHeader(float width, float height);
  void DrawSidebar(float width, float height);
  void DrawContent(float width, float height);
  void DrawFooter(float width, float height);
  void DrawStatusBar(float width, float height);
  void DrawModals();
  void DrawSettingCard(const SettingDef& def, bool show_category);
  void HandleShortcuts();
  // The gamepad may drive the launcher only while its window is the foreground window and no game it started is running.
  bool GamepadAllowed() const;
  // present_effect=fsr together with temporal_aa=fsr (both active): the picture would be upscaled twice.
  bool FsrDoubleUpscale() const;
  void DrawControl(const SettingDef& def, ImVec2 pos, float widget_w, float frame_h);
  std::vector<CardExtra> CardExtras(const SettingDef& def) const;

  void* hwnd_ = nullptr;
  AppOptions options_;
  float scale_ = 1.0f;
  Fonts fonts_;

  Schema schema_;
  std::string schema_error_;
  Values values_;
  std::vector<std::string> load_notes_;

  int category_ = 0;
  bool category_changed_ = false;  // scroll the content back to the top
  char search_[128] = {};
  bool focus_search_ = false;
  bool show_advanced_ = false;
  bool show_descriptions_ = true;
  bool open_reset_popup_ = false;

  // persistence
  bool dirty_ = false;
  double last_change_time_ = 0.0;
  std::string save_error_;

  // game / validation
  GameProcess game_;
  bool game_running_ = false;
  std::filesystem::path game_exe_;
  DataRootCheck data_check_;
  std::string checked_root_;
  std::string checked_paths_key_;                                // all path values at the last check
  std::map<std::string, std::vector<std::string>> path_issues_;  // setting id -> problems (every path setting but the data root)
  std::vector<std::string> arg_notes_;                           // BuildArgs: why a setting is not on the command line
  bool checked_once_ = false;
  double last_validation_ = 0.0;
  std::string launch_error_;
  bool had_run_ = false;
  bool exit_reported_ = false;

  // command line preview
  std::string preview_text_;
  std::vector<std::wstring> args_;
  bool preview_dirty_ = true;
  std::string needle_;  // lower-cased search text of this frame

  // transient message in the status bar
  std::string toast_text_;
  std::string toast_detail_;
  StatusLine::Level toast_level_ = StatusLine::Level::Info;
  double toast_until_ = 0.0;

  bool quit_requested_ = false;
  bool gamepad_hints_ = false;
};

}  // namespace launcher::ui
