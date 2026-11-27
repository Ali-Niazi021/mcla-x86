// mcla-x86 host: the in-game overlay. See overlay.h for what it is and how it plugs in.
//
// Three pieces, all on the UI thread:
//
//   Gate             a WindowInputListener at z-order 32, always registered: between the ImGui drawer (64), which has already
//                    fed the event to ImGui, and the input drivers and the SDK's key binds (0), which come after. It handles the
//                    three hotkeys, and while the menu is open it swallows key-down, character, mouse-down, -move and -wheel
//                    events so the game never sees them. Key and button RELEASES always pass (nothing sticks); a key held
//                    since before the menu opened is "not down" for the game until it is pressed again. The drivers are also
//                    told to go quiet (the active callback of OnRuntimeReady), which silences the pads and the mouse capture.
//   Overlay          the state: what is shown (the cvars mcla_overlay_fps / _stats, and the menu), the one ImGui dialog that
//                    exists only while something is shown, the sampler, the menu's tab.
//   OverlayDialog    the ImGuiDialog whose OnDraw draws the FPS counter, the statistics window and the menu.
//
// Text size: the ImGui drawer bakes its font atlas once (no run-time font sizes), so ConfigureFonts bakes five Segoe UI sizes and
// the overlay picks the one that suits the window's height (overlay_math.h FontIndexFor); every length is in units of the font's
// size, so the layout follows. Without the baked fonts the overlay still works, in the atlas' default size.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "overlay.h"
#include "overlay_feed.h"
#include "overlay_items.h"
#include "overlay_math.h"
#include "overlay_sample.h"

// ---- cvars ----------------------------------------------------------------------------------------------------------------

REXCVAR_DEFINE_BOOL(mcla_overlay, true, "MCLA",
                    "The in-game overlay: the mod menu (default F1), the FPS counter (F2) and the statistics screen (F5). Off: "
                    "no key is listened to and nothing is created. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_overlay_fps, false, "MCLA",
                    "Show the FPS counter. A visible overlay makes the presenter compose an overlay layer over the picture (about "
                    "0.2 ms of GPU time at 1080p, and no fused gamma paint); hidden it costs nothing. Live");
REXCVAR_DEFINE_INT32(mcla_overlay_fps_corner, 1, "MCLA", "Corner of the FPS counter: 0 top left, 1 top right, 2 bottom left, 3 bottom right. Live")
    .range(0, 3);
REXCVAR_DEFINE_INT32(mcla_overlay_fps_detail, 1, "MCLA",
                     "What the FPS counter shows: 0 the rate, 1 the rate and the frame time, 2 also the 1 % low and the GPU time. Live")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(mcla_overlay_stats, false, "MCLA",
                    "Show the statistics screen: frame times and a graph, the GPU, the display's cadence, adaptive detail, hitches, "
                    "CPU clocks and memory. Same cost as the FPS counter while shown. Live");
REXCVAR_DEFINE_DOUBLE(mcla_overlay_scale, 1.0, "MCLA",
                      "Size of the overlay's text and windows as a factor of the automatic size (which follows the window's height). Live")
    .range(0.5, 3.0);
REXCVAR_DEFINE_DOUBLE(mcla_overlay_opacity, 0.75, "MCLA", "How opaque the panels behind the overlay's text are, 0.2 to 1. Live")
    .range(0.2, 1.0);
REXCVAR_DEFINE_BOOL(mcla_overlay_cheats, false, "MCLA",
                    "Unlocks the Cheats / mods tab of the menu. Off at every start (never saved; only the command line sets it). Live")
    .transient();
REXCVAR_DEFINE_BOOL(mcla_overlay_menu, false, "MCLA",
                    "Open the mod menu (the key does the same and keeps this in step): for scripted runs, where a schedule step "
                    "`set mcla_overlay_menu=true` opens it. Never saved. Live")
    .transient();
REXCVAR_DEFINE_INT32(mcla_overlay_tab, 0, "MCLA",
                     "The tab the mod menu shows: 0 Display, 1 Graphics, 2 Game, 3 Cheats / mods, 4 About. Setting it selects the tab (for "
                     "scripted runs); clicking a tab does not change it. Never saved. Live")
    .range(0, 4)
    .transient();
REXCVAR_DEFINE_STRING(mcla_overlay_key_menu, "F1", "Input/Keybinds/System", "Open or close the mod menu");
REXCVAR_DEFINE_STRING(mcla_overlay_key_fps, "F2", "Input/Keybinds/System", "Show or hide the FPS counter");
REXCVAR_DEFINE_STRING(mcla_overlay_key_stats, "F5", "Input/Keybinds/System", "Show or hide the statistics screen");

namespace mcla_host::overlay {
namespace {

using mcla_overlay::Choice;
using mcla_overlay::Item;
using mcla_overlay::Kind;

// The baked text sizes in pixels (ConfigureFonts), smallest first, and the fonts they gave (null: not baked).
constexpr float kFontPixels[] = {14.0f, 17.0f, 21.0f, 26.0f, 33.0f};
constexpr int kFontCount = int(sizeof(kFontPixels) / sizeof(kFontPixels[0]));
ImFont* g_fonts[kFontCount] = {};

// The menu is open: read by the input drivers' active callback on the game thread, by the gate and by the dialog.
std::atomic<bool> g_menu_open{false};
// Auto-repeat key-down events the gate saw since the menu was last closed (a line in the log: whether a held key repeats at all).
std::atomic<uint32_t> g_key_repeats{0};

constexpr ImVec4 kGood{0.45f, 0.95f, 0.45f, 1.0f};
constexpr ImVec4 kWarn{1.00f, 0.80f, 0.30f, 1.0f};
constexpr ImVec4 kBad{1.00f, 0.40f, 0.35f, 1.0f};
constexpr ImVec4 kDim{0.62f, 0.64f, 0.68f, 1.0f};
constexpr ImVec4 kAccent{0.45f, 0.78f, 1.00f, 1.0f};

enum Tab { kTabDisplay, kTabGraphics, kTabGame, kTabCheats, kTabAbout, kTabCount };
constexpr const char* kTabNames[kTabCount] = {"Display", "Graphics", "Game", "Cheats / mods", "About"};

class OverlayDialog;

// ---- the overlay ------------------------------------------------------------------------------------------------------------

class Overlay {
 public:
  Overlay(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path);
  ~Overlay();

  // UI thread: makes the dialog exist exactly while something is shown (and the menu match mcla_overlay_menu).
  void Refresh();
  // Any thread: Refresh on the UI thread, later (a cvar change callback runs under the registry's lock on the setting thread).
  void RequestRefresh();

  bool menu_open() const { return g_menu_open.load(std::memory_order_relaxed); }
  void OpenMenu();
  void CloseMenu();
  void ToggleMenu() { menu_open() ? CloseMenu() : OpenMenu(); }
  // Toggles a bool cvar the way the player's key does: through the registry, remembered for Save when `track`.
  void ToggleCvar(const char* name, bool track);

  void Draw(ImGuiIO& io);  // OverlayDialog::OnDraw
  bool RepaintsContinuously() const { return menu_open(); }

  rex::ui::ImGuiDrawer* drawer() const { return drawer_; }
  rex::ui::Window* window() const { return window_; }

 private:
  // The pieces of Draw.
  void DrawFps(const ImGuiIO& io, float em);
  void DrawStats(const ImGuiIO& io, float em, bool interactive);
  void DrawMenu(const ImGuiIO& io, float em);
  void DrawTabDisplay();
  void DrawTabGraphics();
  void DrawTabGame();
  void DrawTabCheats();
  void DrawTabAbout();
  void Section(const char* title, std::initializer_list<const Item*> items);
  void Row(const Item& item);
  void Footer();

  rex::ui::Window* window_;
  rex::ui::ImGuiDrawer* drawer_;
  std::filesystem::path config_path_;

  std::unique_ptr<OverlayDialog> dialog_;
  mcla_overlay::Sampler sampler_;

  // The menu.
  int tab_ = kTabDisplay;
  bool just_opened_ = false;
  int select_tab_ = -1;          // a tab the next frame must select (mcla_overlay_tab)
  bool stats_drawn_last_ = false;  // the statistics window was drawn in the previous frame
  bool focus_menu_ = false;        // the menu goes to the front (the statistics window appeared over it)
  int requested_tab_ = 0;        // the last value of mcla_overlay_tab acted on
  bool popup_open_last_frame_ = false;
  ImGuiConfigFlags saved_config_flags_ = 0;
  rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
  bool have_about_ = false;
  mcla_overlay::AboutInfo about_;
  std::string status_;  // the last Save's message, the last preset's, ...
  std::vector<std::string> restart_pending_;  // cvars changed that apply at the next start (this frame's list)
  bool cheats_allowed_ = false;
  bool gov_on_ = false;
};

class OverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  OverlayDialog(rex::ui::ImGuiDrawer* drawer, Overlay* owner) : ImGuiDialog(drawer), owner_(owner) {}
  // The menu needs a repaint every frame (input); the counter and the statistics are drawn with each new guest frame.
  bool RepaintsContinuously() const override { return owner_->RepaintsContinuously(); }
  bool HasVisibleContent() const override { return true; }

 protected:
  void OnDraw(ImGuiIO& io) override { owner_->Draw(io); }

 private:
  Overlay* owner_;
};

// A plain pointer, deleted only by Shutdown(): a static destructor at process exit must not touch an ImGui drawer that is gone
// (the SDK leaves through std::_Exit, which runs none, but a normal exit would).
Overlay* g_overlay = nullptr;

Overlay::Overlay(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
    : window_(window), drawer_(drawer), config_path_(std::move(config_path)) {}

Overlay::~Overlay() {
  dialog_.reset();
  mcla_overlay::SetDemand(false);
}

void Overlay::Refresh() {
  // The menu cvar (scripts, tests) and the key are one state: whichever changed last wins.
  if (REXCVAR_GET(mcla_overlay_menu) != menu_open()) {
    if (REXCVAR_GET(mcla_overlay_menu)) {
      OpenMenu();
    } else {
      CloseMenu();
    }
    return;  // OpenMenu / CloseMenu call Refresh again
  }
  const bool fps = REXCVAR_GET(mcla_overlay_fps);
  const bool stats = REXCVAR_GET(mcla_overlay_stats);
  const bool menu = menu_open();
  mcla_overlay::SetDemand(fps || stats);
  if ((fps || stats || menu) && !dialog_) {
    dialog_ = std::make_unique<OverlayDialog>(drawer_, this);
  } else if (!(fps || stats || menu) && dialog_) {
    dialog_.reset();
  }
}

void Overlay::RequestRefresh() {
  window_->app_context().CallInUIThreadDeferred([] {
    if (g_overlay) g_overlay->Refresh();
  });
}

void Overlay::OpenMenu() {
  if (menu_open()) return;
  g_menu_open.store(true, std::memory_order_relaxed);
  just_opened_ = true;
  popup_open_last_frame_ = false;
  saved_cursor_ = window_->GetCursorVisibility();
  window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  rex::cvar::SetFlagByName("mcla_overlay_menu", "true");
  REXLOG_INFO("[mcla-overlay] menu open: the game gets no input until it closes");
  // Keyboard navigation (arrows, Tab, Enter, Space) only while the menu is open: the SDK's own overlays keep what they had.
  ImGuiIO& io = drawer_->GetIO();
  saved_config_flags_ = io.ConfigFlags;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  if (!have_about_) {
    have_about_ = true;
    about_ = mcla_overlay::CollectAbout(window_->GetNativeWindowHandle());
  }
  Refresh();
}

void Overlay::CloseMenu() {
  if (!menu_open()) return;
  g_menu_open.store(false, std::memory_order_relaxed);
  ImGuiIO& io = drawer_->GetIO();
  io.ConfigFlags = saved_config_flags_;
  window_->SetCursorVisibility(saved_cursor_);
  rex::cvar::SetFlagByName("mcla_overlay_menu", "false");
  REXLOG_INFO("[mcla-overlay] menu closed ({} key auto-repeat event(s) seen since it opened)", g_key_repeats.exchange(0, std::memory_order_relaxed));
  Refresh();
}

void Overlay::ToggleCvar(const char* name, bool track) {
  mcla_overlay::SetCvarBool(name, !mcla_overlay::CvarBool(name), track);
  Refresh();
}

// ---- the key listener -------------------------------------------------------------------------------------------------------

class Gate final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_overlay) return;
    const bool open = g_overlay->menu_open();
    const rex::ui::VirtualKey vk = e.virtual_key();
    const size_t index = size_t(vk);
    if (!open && index < pressed_in_menu_.size() && pressed_in_menu_[index]) {
      // Pressed while the menu was open and still held: the key's auto-repeats after the menu closed are not a new press. The
      // game sees the key again only after it was released and pressed once more.
      ++repeats_swallowed_[index];
      e.set_handled(true);
      return;
    }
    if (e.is_alt_pressed() || e.is_ctrl_pressed()) {  // Alt+F4 and friends are the system's
      if (open) e.set_handled(true);
      return;
    }
    const bool repeat = e.prev_state();
    if (repeat) g_key_repeats.fetch_add(1, std::memory_order_relaxed);
    if (vk == rex::ui::ParseVirtualKey(REXCVAR_GET(mcla_overlay_key_menu))) {
      if (!repeat) g_overlay->ToggleMenu();
      e.set_handled(true);
      return;
    }
    if (vk == rex::ui::ParseVirtualKey(REXCVAR_GET(mcla_overlay_key_fps))) {
      if (!repeat) g_overlay->ToggleCvar("mcla_overlay_fps", false);
      e.set_handled(true);
      return;
    }
    if (vk == rex::ui::ParseVirtualKey(REXCVAR_GET(mcla_overlay_key_stats))) {
      if (!repeat) g_overlay->ToggleCvar("mcla_overlay_stats", false);
      e.set_handled(true);
      return;
    }
    if (open) {
      if (index < pressed_in_menu_.size()) pressed_in_menu_[index] = true;
      e.set_handled(true);  // the drawer (z 64) has seen it; the game must not
    }
  }
  // Releases always pass: a key or button that went down before the menu opened must come up for the game.
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const size_t index = size_t(e.virtual_key());
    if (index < pressed_in_menu_.size()) {
      pressed_in_menu_[index] = false;
      if (repeats_swallowed_[index] > 0) {
        REXLOG_INFO("[mcla-overlay] key 0x{:02X}: {} auto-repeat(s) after the menu closed were kept from the game", index,
                    repeats_swallowed_[index]);
        repeats_swallowed_[index] = 0;
      }
    }
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (g_overlay && g_overlay->menu_open()) e.set_handled(true);
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Swallow(e); }
  void OnMouseMove(rex::ui::MouseEvent& e) override { Swallow(e); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override { Swallow(e); }
 private:
  std::bitset<256> pressed_in_menu_;  // keys whose down went to the menu (UI thread only)
  uint32_t repeats_swallowed_[256] = {};  // auto-repeats of such a key after the menu closed (for the log)
  static void Swallow(rex::ui::MouseEvent& e) {
    if (g_overlay && g_overlay->menu_open()) e.set_handled(true);
  }
};

Gate* g_gate = nullptr;
rex::ui::Window* g_gate_window = nullptr;

// ---- drawing: helpers -------------------------------------------------------------------------------------------------------

// Pushes the baked font that suits the window and the player's size factor; the style lengths are in units of its size.
struct ScopedLook {
  explicit ScopedLook(const ImGuiIO& io) {
    const int index = mcla_overlay::math::FontIndexFor(double(io.DisplaySize.y), REXCVAR_GET(mcla_overlay_scale), kFontPixels,
                                                       kFontCount);
    if (index >= 0 && g_fonts[index]) {
      ImGui::PushFont(g_fonts[index], g_fonts[index]->LegacySize);
      pushed_font = true;
    }
    em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.7f * em, 0.55f * em));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.5f * em, 0.22f * em));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.6f * em, 0.3f * em));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0.4f * em, 0.3f * em));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.8f * em);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 0.8f * em);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.35f * em);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.25f * em);
  }
  ~ScopedLook() {
    ImGui::PopStyleVar(8);
    if (pushed_font) ImGui::PopFont();
  }
  float em = 14.0f;
  bool pushed_font = false;
};

ImVec4 RateColor(double fps, double target) {
  if (!(fps > 0.0) || !(target > 0.0)) return ImVec4(1, 1, 1, 1);
  if (fps >= 0.97 * target) return kGood;
  if (fps >= 0.85 * target) return kWarn;
  return kBad;
}

// A label and a value on one line, the value in a column (statistics).
void KeyValue(float column, const char* label, const char* fmt_text, ...) IM_FMTARGS(3);
void KeyValue(float column, const char* label, const char* fmt_text, ...) {
  ImGui::TextColored(kDim, "%s", label);
  ImGui::SameLine(column);
  va_list args;
  va_start(args, fmt_text);
  ImGui::TextV(fmt_text, args);
  va_end(args);
}

// A tooltip that wraps (the help texts are sentences).
// The frame cap the world runs at: the host's / the pacer's number, or what paces it when no cap is set.
// What the world's time step is now: the shipped 1/30 s, the 60 fps profile (n/60 s), or the 120 fps one (n/120 s: a cap above 60).
const char* TimingText(const mcla_overlay::Snapshot& s) {
  if (!s.sim_on) return "as shipped (30 fps steps)";
  return s.world_cap > 60 ? "120 fps timing (world n/120 s)" : "60 fps timing (world n/60 s)";
}

std::string CapText(const mcla_overlay::Snapshot& s) {
  if (s.world_cap > 0) return fmt::format("{} fps", s.world_cap);
  return s.sim_on ? "60 fps (the display's vblank paces it)" : "30 fps (the game's own pacing)";
}

void Tooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

}  // namespace

// ---- the FPS counter ------------------------------------------------------------------------------------------------------

void Overlay::DrawFps(const ImGuiIO& io, float em) {
  const mcla_overlay::Snapshot& s = sampler_.Latest();
  const int corner = std::clamp(REXCVAR_GET(mcla_overlay_fps_corner), 0, 3);
  const int detail = std::clamp(REXCVAR_GET(mcla_overlay_fps_detail), 0, 2);
  const float margin = 0.9f * em;
  const ImVec2 pos((corner & 1) ? io.DisplaySize.x - margin : margin, (corner & 2) ? io.DisplaySize.y - margin : margin);
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f));
  ImGui::SetNextWindowBgAlpha(float(REXCVAR_GET(mcla_overlay_opacity)) * 0.8f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##mcla_fps", nullptr, flags)) {
    // What the frame rate should be: the world's cap, or the profile's (60 with the 60 fps timing, the shipped 30 without).
    const double target = s.world_cap > 0 ? double(s.world_cap) : (s.sim_on ? 60.0 : 30.0);
    if (s.frames_fed && s.fps_1s > 0.0) {
      ImGui::TextColored(RateColor(s.fps_1s, target), "%.0f FPS", s.fps_1s);
      if (detail >= 1) ImGui::TextColored(kDim, "%.1f ms", s.avg_ms_1s);
      if (detail >= 2) {
        if (s.frames.low1_fps > 0.0) ImGui::TextColored(kDim, "1%% low %.0f", s.frames.low1_fps);
        if (s.gpu_known) ImGui::TextColored(kDim, "GPU %.1f ms", s.gpu_busy_ms);
      }
    } else {
      ImGui::TextColored(kDim, "-- FPS");
    }
  }
  ImGui::End();
}

// ---- the statistics screen ------------------------------------------------------------------------------------------------

void Overlay::DrawStats(const ImGuiIO& io, float em, bool interactive) {
  const mcla_overlay::Snapshot& s = sampler_.Latest();
  // Left, or right when the FPS counter sits on the left; fixed while the menu is closed, free to be dragged while it is open.
  const bool fps_left = REXCVAR_GET(mcla_overlay_fps) && (std::clamp(REXCVAR_GET(mcla_overlay_fps_corner), 0, 3) & 1) == 0;
  const float panel_width = std::min(io.DisplaySize.x * 0.42f, 34.0f * em);
  ImGui::SetNextWindowPos(ImVec2(fps_left ? io.DisplaySize.x - panel_width - 0.8f * em : 0.8f * em, 0.8f * em),
                          interactive ? ImGuiCond_Appearing : ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(panel_width, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(float(REXCVAR_GET(mcla_overlay_opacity)));
  // Click-through (no mouse capture, no focus) unless the menu is open: then it can be moved and scrolled.
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_AlwaysAutoResize;
  if (!interactive) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar;
  if (!ImGui::Begin("Statistics###mcla_stats", nullptr, flags)) {
    ImGui::End();
    return;
  }
  const float col = 9.5f * em;

  // ---- frames
  ImGui::SeparatorText("Frames (game thread, last 5 s)");
  const mcla_overlay::math::FrameTimeStats& f = s.frames;
  if (!s.frames_fed) {
    ImGui::TextColored(kDim, "no frames yet");
  } else {
    const double target = s.world_cap > 0 ? double(s.world_cap) : (s.sim_on ? 60.0 : 30.0);
    ImGui::TextColored(kDim, "rate");
    ImGui::SameLine(col);
    ImGui::TextColored(RateColor(f.fps, target), "%.1f fps  (%.2f ms)", f.fps, f.avg_ms);
    KeyValue(col, "median / p99", "%.1f / %.1f ms   worst %.1f ms", f.p50_ms, f.p99_ms, f.worst_ms);
    KeyValue(col, "1 % / 0.1 % low", "%.0f / %.0f fps", f.low1_fps, f.low01_fps);
    KeyValue(col, "over 20/25/34 ms", "%u / %u / %u  of %u", f.over20, f.over25, f.over34, f.count);
    const std::vector<float> bars = mcla_overlay::math::Downsample(sampler_.Graph(480), 240);
    const float ceiling = float(mcla_overlay::math::GraphCeilingMs(f.worst_ms));
    char overlay_text[48];
    std::snprintf(overlay_text, sizeof(overlay_text), "0 - %.0f ms", ceiling);
    ImGui::PlotLines("##frametimes", bars.data(), int(bars.size()), 0, overlay_text, 0.0f, ceiling,
                     ImVec2(ImGui::GetContentRegionAvail().x, 4.5f * em));
  }

  // ---- GPU
  ImGui::SeparatorText("GPU");
  if (s.gpu_known) {
    const double period = f.avg_ms > 0.0 ? f.avg_ms : 16.7;
    KeyValue(col, "busy", "%.1f ms  (%.0f %% of the frame)", s.gpu_busy_ms, 100.0 * s.gpu_busy_ms / period);
    if (s.gpu_fps > 0.0) KeyValue(col, "frames", "%.0f /s, read back %u frame(s) late", s.gpu_fps, s.gpu_lag);
  } else {
    ImGui::TextColored(kDim, "GPU timing not available");
  }
  if (s.compile_known) {
    ImGui::TextColored(kDim, "pipelines");
    ImGui::SameLine(col);
    ImGui::Text("%u created, %d in flight, %u shaders", s.pipelines, int(s.pending), s.shaders);
    if (s.compile_burst || s.pending > 0) {
      ImGui::SameLine();
      ImGui::TextColored(kWarn, " compiling");
    }
    KeyValue(col, "new / minute", "%.1f", s.pipelines_per_min);
  }

  // ---- display
  ImGui::SeparatorText("Display");
  const mcla_overlay::PresentView& p = s.present;
  if (!p.available) {
    ImGui::TextColored(kDim, "not available: this SDK build lacks the present statistics export (patch 0755)");
  } else if (!p.valid) {
    ImGui::TextColored(kWarn, "the window is not taking presents (hidden or covered)");
  } else if (!p.rates_valid) {
    ImGui::TextColored(kDim, "measuring...");
  } else {
    KeyValue(col, "presented", "%.1f /s   shown %.1f /s", p.present_fps, p.shown_fps);
    ImGui::TextColored(kDim, "dropped");
    ImGui::SameLine(col);
    ImGui::TextColored(p.dropped_per_s > 0.05 ? kWarn : kGood, "%.2f /s  (%llu in all)", p.dropped_per_s,
                       static_cast<unsigned long long>(p.dropped_total));
    if (p.refresh_hz > 0.0) KeyValue(col, "refresh clock", "%.1f Hz, %.2f refreshes a frame", p.refresh_hz, p.cadence);
    KeyValue(col, "held 1 / 2 / 3+", "%.0f %% / %.0f %% / %.0f %% of the refreshes", p.held1_pct, p.held2_pct, p.held3p_pct);
    ImGui::TextColored(kDim, "over cadence");
    ImGui::SameLine(col);
    ImGui::TextColored(p.over_cadence_per_min > 0.5 ? kWarn : kGood, "%.1f /min  (%llu in all)", p.over_cadence_per_min,
                       static_cast<unsigned long long>(p.over_cadence_total));
  }

  // ---- adaptive detail
  ImGui::SeparatorText("Adaptive detail");
  if (s.gov_on) {
    KeyValue(col, "governor", "on: rung %+d, tier %d, VRS %d%s", s.gov_level, s.gov_tier, s.gov_vrs,
             s.compile_burst ? "  (compile burst excused)" : "");
  } else {
    KeyValue(col, "governor", "off");
  }
  if (s.gfx_tier >= 0) {
    KeyValue(col, "detail tier", "%d  %s", s.gfx_tier, s.gfx_tier_name.c_str());
  } else {
    KeyValue(col, "detail tier", "none (the knobs decide)");
  }

  // ---- world
  ImGui::SeparatorText("World");
  if (s.world_speed < 0.0) {
    KeyValue(col, "speed", "--");
  } else if (s.world_speed < 0.02) {
    KeyValue(col, "speed", "paused");
  } else {
    ImGui::TextColored(kDim, "speed");
    ImGui::SameLine(col);
    ImGui::TextColored(std::fabs(s.world_speed - 1.0) <= 0.005 ? kGood : kWarn, "x%.3f", s.world_speed);
  }
  KeyValue(col, "frame cap", "%s", CapText(s).c_str());
  KeyValue(col, "timing", "%s", TimingText(s));

  // ---- hitches
  ImGui::SeparatorText("Hitches");
  const mcla_overlay::HitchSummary& h = s.hitches;
  ImGui::TextColored(kDim, "last 60 s");
  ImGui::SameLine(col);
  ImGui::TextColored(h.recent == 0 ? kGood : kWarn, "%u  (worst %.0f ms)   %llu in all", h.recent, h.worst_recent_ms,
                     static_cast<unsigned long long>(h.total));
  if (h.last_ns != 0) {
    const double ago = double(mcla_overlay::NowNs() - h.last_ns) / 1e9;
    KeyValue(col, "last", "%.0f ms, %s  (%.0f s ago%s)", h.last_ms, h.last_cause.c_str(), ago,
             h.last_screen_mode == 1 ? ", in a menu / load" : "");
  }

  // ---- machine
  ImGui::SeparatorText("Machine");
  if (!s.system_known) {
    ImGui::TextColored(kDim, "sampling...");
  } else {
    for (const mcla_overlay::ClockClass& c : s.clocks) {
      ImGui::TextColored(kDim, "cores class %d (%d)", c.efficiency_class, c.logical);
      ImGui::SameLine(col);
      ImGui::TextColored(c.capped ? kWarn : kGood, "%u MHz of %u%s", c.avg_mhz, c.max_mhz,
                         c.capped ? fmt::format("   CAPPED at {} MHz", c.min_limit_mhz).c_str() : "");
    }
    KeyValue(col, "game CPU", "%.2f cores   reads %.1f MiB/s, %.0f page faults/s", s.cpu_cores, s.read_mib_s, s.page_faults_s);
    KeyValue(col, "memory", "%u MiB in use (%u private), system %u %% used", s.working_set_mib, s.private_mib, s.memory_load_pct);
  }
  ImGui::End();
}

// ---- the menu: rows -------------------------------------------------------------------------------------------------------

void Overlay::Row(const Item& it) {
  using namespace mcla_overlay;
  const bool is_cheat = (it.flags & kFlagCheat) != 0;
  const int mode = it.cvar && *it.cvar ? ApplyMode(it.cvar) : 0;
  const char* disabled_why = nullptr;
  if (mode < 0) disabled_why = "this build does not have the setting";
  else if (mode == 2) disabled_why = "read-only";
  else if (is_cheat && !cheats_allowed_) disabled_why = "switch on \"Allow cheats and mods\" first";
  else if ((it.flags & kFlagGovOff) && gov_on_) disabled_why = "adaptive detail is on and chooses this";
  else if ((it.flags & kFlagGovOn) && !gov_on_) disabled_why = "adaptive detail is off";

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  if (disabled_why) ImGui::BeginDisabled();
  ImGui::TextUnformatted(it.label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
    std::string tip = it.help;
    tip += fmt::format("\n[{}]", it.cvar);
    if (disabled_why) tip += fmt::format("\n({})", disabled_why);
    Tooltip(tip.c_str());
  }

  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::PushID(it.cvar);
  switch (it.kind) {
    case Kind::kToggle: {
      bool v = CvarBool(it.cvar);
      if (ImGui::Checkbox("##v", &v)) {
        SetCvarBool(it.cvar, v, !is_cheat);
        if (std::strcmp(it.cvar, "mcla_overlay_fps") == 0 || std::strcmp(it.cvar, "mcla_overlay_stats") == 0) Refresh();
        if (std::strcmp(it.cvar, "mcla_overlay_cheats") == 0 && !v && AnyCheatActive()) {
          // Locking the cheats gives the game its own values back: nothing may stay on behind a locked tab.
          ResetCheats();
          status_ = "Cheats locked: every cheat was switched off.";
        }
      }
      break;
    }
    case Kind::kInt: {
      int v = int(CvarNumber(it.cvar));
      if (ImGui::SliderInt("##v", &v, int(it.min), int(it.max), it.format)) SetCvarNumber(it.cvar, double(v), !is_cheat);
      break;
    }
    case Kind::kFloat: {
      float v = float(CvarNumber(it.cvar));
      if (ImGui::SliderFloat("##v", &v, float(it.min), float(it.max), it.format)) SetCvarNumber(it.cvar, double(v), !is_cheat);
      break;
    }
    case Kind::kChoice: {
      const std::string current = CvarString(it.cvar);
      const char* preview = nullptr;
      std::string custom;
      for (int i = 0; i < it.choice_count; ++i) {
        if (current == it.choices[i].value) preview = it.choices[i].label;
      }
      if (!preview) {
        custom = fmt::format("{} (custom)", current);
        preview = custom.c_str();
      }
      if (ImGui::BeginCombo("##v", preview)) {
        for (int i = 0; i < it.choice_count; ++i) {
          const bool selected = current == it.choices[i].value;
          if (ImGui::Selectable(it.choices[i].label, selected)) {
            SetCvar(it.cvar, it.choices[i].value, !is_cheat);
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      break;
    }
    case Kind::kHoldInt:
    case Kind::kHoldFloat: {
      const double value = CvarNumber(it.cvar);
      bool held = std::fabs(value - it.off_value) > 1e-6;
      if (ImGui::Checkbox("##hold", &held)) {
        SetCvarNumber(it.cvar, held ? it.on_value : it.off_value, !is_cheat);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (!held) ImGui::BeginDisabled();
      float v = float(held ? value : it.on_value);
      if (it.kind == Kind::kHoldInt) {
        int iv = int(std::lround(v));
        if (ImGui::SliderInt("##v", &iv, int(it.min), int(it.max), it.format)) SetCvarNumber(it.cvar, double(iv), !is_cheat);
      } else {
        if (ImGui::SliderFloat("##v", &v, float(it.min), float(it.max), it.format)) SetCvarNumber(it.cvar, double(v), !is_cheat);
      }
      if (!held) ImGui::EndDisabled();
      break;
    }
    case Kind::kPreset: {
      const std::string current = CurrentPresetName();
      const PresetOption* shown = &kPresets[0];
      for (const PresetOption& option : kPresets) {
        if (current == option.value) shown = &option;
      }
      if (ImGui::BeginCombo("##v", shown->label)) {
        for (const PresetOption& option : kPresets) {
          const bool selected = current == option.value;
          if (ImGui::Selectable(option.label, selected)) {
            const int n = ApplyDetailPreset(option.value, gov_on_);
            if (n < 0) {
              status_ = fmt::format("Preset {} is not known to this build.", option.label);
            } else if (gov_on_) {
              status_ = fmt::format("Preset {}: adaptive detail now starts from tier {} (its floor) and goes up to its ceiling.",
                                    option.label, PresetFloorTier(option.value));
            } else {
              status_ = fmt::format("Preset {}: {} knob(s) set (a knob you set yourself kept its value).", option.label, n);
            }
          }
          if (ImGui::IsItemHovered()) Tooltip(option.help);
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      break;
    }
  }
  ImGui::PopID();
  if (disabled_why) ImGui::EndDisabled();

  // The badge: when the change takes effect, and where the value came from.
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  if (it.cvar && *it.cvar) {
    if ((it.flags & kFlagRestart) || mode == 1) {
      ImGui::TextColored(kWarn, "next start");
    } else if (it.flags & kFlagNextLoad) {
      ImGui::TextColored(kWarn, "next load");
    } else {
      ImGui::TextColored(kDim, "live");
    }
    if (it.flags & kFlagUntested) {
      ImGui::SameLine();
      ImGui::TextColored(kWarn, "untested");
    }
    const char* source = SourceLabel(it.cvar);
    if (std::strcmp(source, "changed here") == 0) {
      ImGui::SameLine();
      ImGui::TextColored(kAccent, "changed");
    } else if (std::strcmp(source, "launcher / command line") == 0) {
      ImGui::SameLine();
      ImGui::TextColored(kDim, "launcher");
    }
    if (it.kind != Kind::kPreset && !disabled_why && IsNonDefault(it.cvar)) {
      ImGui::SameLine();
      ImGui::PushID(it.cvar);
      if (ImGui::SmallButton("reset")) ResetCvar(it.cvar, !is_cheat);
      ImGui::PopID();
      if (ImGui::IsItemHovered()) Tooltip("Back to the default of this setup (the PC profile's or the game's)");
    }
  }
}

void Overlay::Section(const char* title, std::initializer_list<const Item*> items) {
  if (title && *title) ImGui::SeparatorText(title);
  if (ImGui::BeginTable((title && *title) ? title : "##rows", 3,
                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    for (const Item* item : items) Row(*item);
    ImGui::EndTable();
  }
}

// ---- the menu: tabs ---------------------------------------------------------------------------------------------------------

void Overlay::DrawTabDisplay() {
  using namespace mcla_overlay::items;
  Section("Overlay", {&kFps, &kFpsCorner, &kFpsDetail, &kStats, &kScale, &kOpacity});
  Section("Picture", {&kFullscreen, &kAa, &kHdr, &kLodBias, &kAniso, &kTearing});
  ImGui::TextColored(kDim, "The picture's size and the upscaler are chosen in the launcher (they need a restart).");
}

void Overlay::DrawTabGraphics() {
  using namespace mcla_overlay::items;
  const mcla_overlay::Snapshot& s = sampler_.Latest();
  Section("Adaptive detail", {&kGov, &kGovCeiling, &kGovFloor});
  if (s.gov_on) {
    ImGui::TextColored(kDim, "now: rung %+d, detail tier %d%s%s, VRS %d", s.gov_level, s.gfx_tier,
                       s.gfx_tier_name.empty() ? "" : " ", s.gfx_tier_name.c_str(), s.gov_vrs);
  } else if (s.gfx_tier >= 0) {
    ImGui::TextColored(kDim, "now: detail tier %d %s", s.gfx_tier, s.gfx_tier_name.c_str());
  }
  Section("Detail", {&kPreset});
  if (s.gov_on) {
    ImGui::TextColored(kDim, "Adaptive detail is on: a preset sets the tier it starts from (its floor); it raises the picture up to its ceiling when the GPU has room.");
  }
  const size_t by_hand = mcla_overlay::DetailKnobsSetByHand();
  ImGui::TextColored(kDim, "%zu of the knobs below are set by you (they keep your value over any preset and over adaptive detail).",
                     by_hand);
  if (by_hand > 0) {
    ImGui::SameLine();
    if (ImGui::SmallButton("release them")) {
      mcla_overlay::ReleaseDetailKnobs();
      status_ = "Detail knobs released: the preset (or adaptive detail) decides them again.";
    }
  }
  Section("Distances and rates", {&kCityLod, &kTrafficLod, &kShadowDist, &kShadowMode, &kShadowRes, &kEnvInterval, &kFarCascades,
                                  &kImpostors});
  Section("Other", {&kMotionBlur, &kAmbient});
}

void Overlay::DrawTabGame() {
  using namespace mcla_overlay::items;
  const mcla_overlay::Snapshot& s = sampler_.Latest();
  ImGui::TextColored(kDim, "timing now: %s, frame cap %s", TimingText(s), CapText(s).c_str());
  Section("Frame rate", {&kFpsTarget, &kFpsCap});
  Section("60 / 120 fps fixes", {&kSmoothing, &kUi30});
  Section("Screen", {&kHudAspect, &kSkipIntro});
  Section("Keyboard and mouse", {&kMouse, &kMouseSens});
  Section("Overlay", {&kOverlayOn});
  ImGui::TextColored(kDim, "Keys: menu %s, FPS counter %s, statistics %s (rebind in the launcher, or in the F4 settings overlay).",
                     REXCVAR_GET(mcla_overlay_key_menu).c_str(), REXCVAR_GET(mcla_overlay_key_fps).c_str(),
                     REXCVAR_GET(mcla_overlay_key_stats).c_str());
}

void Overlay::DrawTabCheats() {
  using namespace mcla_overlay::items;
  ImGui::TextWrapped("Cheats and mods change the running game. They are all OFF by default, switch off again when you "
                     "press the button below or restart, and are never saved. Nothing here touches your save file.");
  Section("", {&kCheatsGate});
  if (!cheats_allowed_) {
    ImGui::TextColored(kDim, "Switch on \"Allow cheats and mods\" to use the settings below.");
  }
  Section("Time and weather", {&kTod, &kClock, &kWeather, &kWeatherSnap});
  Section("Traffic", {&kRoadDensity});
  const bool any = mcla_overlay::AnyCheatActive();
  if (!any) ImGui::BeginDisabled();
  if (ImGui::Button("Switch every cheat off")) {
    mcla_overlay::ResetCheats();
    status_ = "All cheats are off; the game's own values are written back.";
  }
  if (!any) ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextColored(any ? kWarn : kGood, any ? "a cheat is ON" : "no cheat is on");

  if (ImGui::CollapsingHeader("Not available yet")) {
    ImGui::TextColored(kDim, "These need the game's code studied first; nothing is guessed and nothing is written:");
    ImGui::BulletText("Money, unlock all cars and parts: the career economy and the save's inventory.");
    ImGui::BulletText("Infinite nitro: the vehicle's nitro state (an offset from another project's HUD hook, never tried here).");
    ImGui::BulletText("No damage: the vehicle damage and collision response.");
    ImGui::BulletText("No police: cop spawning and the pursuit AI (two traffic functions are named, no way to call them yet).");
    ImGui::BulletText("Freeze the AI, slow motion: the AI update loops and the time scale of the game's timer.");
  }
}

void Overlay::DrawTabAbout() {
  ImGui::SeparatorText("This build");
  ImGui::TextWrapped("%s", about_.build.c_str());
  ImGui::TextWrapped("%s", about_.sdk.c_str());
  ImGui::SeparatorText("This machine");
  ImGui::TextWrapped("GPU: %s", about_.gpu.c_str());
  ImGui::TextWrapped("Display: %s", about_.display.c_str());
  ImGui::TextWrapped("%s", about_.machine.c_str());
  ImGui::TextWrapped("%s", about_.guest.c_str());
  ImGui::SeparatorText("Keys");
  ImGui::BulletText("%s  mod menu (Esc closes it too)", REXCVAR_GET(mcla_overlay_key_menu).c_str());
  ImGui::BulletText("%s  FPS counter", REXCVAR_GET(mcla_overlay_key_fps).c_str());
  ImGui::BulletText("%s  statistics screen", REXCVAR_GET(mcla_overlay_key_stats).c_str());
  ImGui::BulletText("F4  all settings (SDK overlay), `  console, F3  debug stamp");
  ImGui::SeparatorText("Files");
  ImGui::TextWrapped("Config: %s", config_path_.string().c_str());
  const std::filesystem::path launcher = mcla_overlay::LauncherSettingsPath();
  std::error_code ec;
  ImGui::TextWrapped("Launcher settings: %s%s", launcher.empty() ? "(none)" : launcher.string().c_str(),
                     (!launcher.empty() && std::filesystem::exists(launcher, ec)) ? "" : "  (not found)");
}

void Overlay::Footer() {
  ImGui::Separator();
  const size_t pending = mcla_overlay::SavableChangeCount();
  if (!status_.empty()) {
    ImGui::TextWrapped("%s", status_.c_str());
  }
  if (!restart_pending_.empty()) {
    std::string names;
    for (const std::string& n : restart_pending_) names += (names.empty() ? "" : ", ") + n;
    ImGui::TextColored(kWarn, "Takes effect at the next start: %s", names.c_str());
  }
  if (pending == 0) ImGui::BeginDisabled();
  if (ImGui::Button("Save settings")) {
    const mcla_overlay::SaveReport report = mcla_overlay::SaveSettings(config_path_);
    status_ = report.message;
  }
  if (pending == 0) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
    Tooltip("Writes the settings you changed to the launcher's settings and to the game's config file, so they are there at the "
            "next start. Cheats, the held hour and weather, and the run-time detail tier are never saved.");
  }
  ImGui::SameLine();
  if (pending > 0) {
    ImGui::TextColored(kAccent, "%zu unsaved change(s)", pending);
  } else {
    ImGui::TextColored(kDim, "no unsaved changes");
  }
  ImGui::SameLine(ImGui::GetContentRegionMax().x - 6.0f * ImGui::GetFontSize());
  if (ImGui::Button("Close")) {
    // Not from inside the draw: closing the menu may delete the dialog that is running this very function.
    window_->app_context().CallInUIThreadDeferred([] {
      if (g_overlay) g_overlay->CloseMenu();
    });
  }
}

void Overlay::DrawMenu(const ImGuiIO& io, float em) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x * 0.92f, 58.0f * em), std::min(io.DisplaySize.y * 0.86f, 40.0f * em)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(std::min(1.0f, float(REXCVAR_GET(mcla_overlay_opacity)) + 0.2f));
  // Esc closes the menu, except when it was only closing a combo (ImGui has already closed the popup by now).
  const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !popup_open_last_frame_ && !io.WantTextInput;
  if (focus_menu_) {
    ImGui::SetNextWindowFocus();  // a window created after the menu is drawn over it: the menu comes back to the front
    focus_menu_ = false;
  }
  if (ImGui::Begin("MCLA mod menu###mcla_menu", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    if (just_opened_) {
      ImGui::SetWindowFocus();
      just_opened_ = false;
    }
    cheats_allowed_ = REXCVAR_GET(mcla_overlay_cheats);
    gov_on_ = mcla_overlay::CvarBool("mcla_gov");
    if (REXCVAR_GET(mcla_overlay_tab) != requested_tab_) {
      requested_tab_ = REXCVAR_GET(mcla_overlay_tab);
      select_tab_ = requested_tab_;
    }
    // The footer: a rule, the status (up to two lines), the restart warning when there is one, and the button row.
    restart_pending_ = rex::cvar::GetPendingRestartFlags();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.35f +
                         ImGui::GetTextLineHeightWithSpacing() * float((status_.empty() ? 0 : 2) + (restart_pending_.empty() ? 0 : 1));
    if (ImGui::BeginTabBar("##tabs")) {
      for (int t = 0; t < kTabCount; ++t) {
        if (ImGui::BeginTabItem(kTabNames[t], nullptr, select_tab_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
          tab_ = t;
          if (ImGui::BeginChild("##scroll", ImVec2(0.0f, -footer), ImGuiChildFlags_None)) {
            switch (t) {
              case kTabDisplay: DrawTabDisplay(); break;
              case kTabGraphics: DrawTabGraphics(); break;
              case kTabGame: DrawTabGame(); break;
              case kTabCheats: DrawTabCheats(); break;
              default: DrawTabAbout(); break;
            }
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      select_tab_ = -1;
    }
    Footer();
  }
  ImGui::End();
  popup_open_last_frame_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (escape) {
    // Not from inside the draw: the dialog may go away with the menu.
    window_->app_context().CallInUIThreadDeferred([] {
      if (g_overlay) g_overlay->CloseMenu();
    });
  }
}

void Overlay::Draw(ImGuiIO& io) {
  const int64_t now = mcla_overlay::NowNs();
  const bool fps = REXCVAR_GET(mcla_overlay_fps);
  const bool stats = REXCVAR_GET(mcla_overlay_stats);
  const bool menu = menu_open();
  sampler_.Update(now, stats);
  ScopedLook look(io);
  if (menu && window_->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible) {
    // The mouse-look driver puts its own visibility back when it lets go of the mouse; the menu needs the cursor.
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
  if (stats && !stats_drawn_last_ && menu) focus_menu_ = true;
  stats_drawn_last_ = stats;
  if (fps) DrawFps(io, look.em);
  if (stats) DrawStats(io, look.em, menu);
  if (menu) DrawMenu(io, look.em);
}

}  // namespace mcla_host::overlay

// ---- the public functions ---------------------------------------------------------------------------------------------------

namespace mcla_host::overlay {

void Install(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, const char* app_name) {
  if (g_overlay || !window || !drawer) return;
  if (!REXCVAR_GET(mcla_overlay)) {
    REXLOG_INFO("[mcla-overlay] off (--mcla_overlay=false): no key is listened to");
    return;
  }
  // <exe dir>/<app>.toml: where the SDK's own "Save to config" writes (ReXApp builds the same path).
  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  std::filesystem::path config = n > 0 && n < std::size(exe) ? std::filesystem::path(exe).parent_path() : std::filesystem::path();
  config /= std::string(app_name) + ".toml";

  g_overlay = new Overlay(window, drawer, config);
  g_gate = new Gate();
  g_gate_window = window;
  window->AddInputListener(g_gate, 32);

  // What the player may set from anywhere (the launcher's command line, the F4 overlay, the console, a script): show or hide the
  // dialog to match. The callback runs under the registry's lock, on the thread that set the value: hand over to the UI thread.
  for (const char* name : {"mcla_overlay_fps", "mcla_overlay_stats", "mcla_overlay_menu", "mcla_overlay_tab"}) {
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
      if (g_overlay) g_overlay->RequestRefresh();
    });
  }
  REXLOG_INFO("[mcla-overlay] keys: menu {}, FPS counter {}, statistics {}", REXCVAR_GET(mcla_overlay_key_menu),
              REXCVAR_GET(mcla_overlay_key_fps), REXCVAR_GET(mcla_overlay_key_stats));
}

void ConfigureFonts(ImFontAtlas* atlas) {
  if (!atlas || !REXCVAR_GET(mcla_overlay)) return;
  const char* path = "C:\\Windows\\Fonts\\segoeui.ttf";
  std::error_code ec;
  const bool have_file = std::filesystem::exists(path, ec);
  int baked = 0;
  for (int i = 0; i < kFontCount; ++i) {
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 1;
    config.GlyphRanges = atlas->GetGlyphRangesDefault();  // Basic Latin and Latin-1: the atlas is built once, without run-time glyphs
    config.SizePixels = kFontPixels[i];
    std::snprintf(config.Name, sizeof(config.Name), "mcla overlay %.0f px", double(kFontPixels[i]));
    g_fonts[i] = have_file ? atlas->AddFontFromFileTTF(path, kFontPixels[i], &config) : atlas->AddFontDefault(&config);
    if (g_fonts[i]) ++baked;
  }
  // The first font added stays the atlas' default (the SDK's overlays): ours are only pushed by the overlay itself.
  REXLOG_INFO("[mcla-overlay] fonts: {} sizes baked from {}", baked, have_file ? path : "the atlas' default font");
}

void OnRuntimeReady(rex::input::InputSystem* input) {
  if (!g_overlay) return;
  if (input) {
    // The SDK installed its own callback (silent while the mouse is over one of its overlays); this one is the same, plus
    // silent while the menu is open: no key, no mouse, no pad reaches the game. It reads the ImGui flag the way the SDK's does.
    rex::ui::ImGuiDrawer* drawer = g_overlay->drawer();
    input->SetActiveCallback([drawer]() {
      if (g_menu_open.load(std::memory_order_relaxed)) return false;
      return !drawer->GetIO().WantCaptureMouse;
    });
  }
  g_overlay->Refresh();  // mcla_overlay_fps on the command line or in the config shows at start
}

void Shutdown() {
  for (const char* name : {"mcla_overlay_fps", "mcla_overlay_stats", "mcla_overlay_menu", "mcla_overlay_tab"}) {
    rex::cvar::UnregisterChangeCallbacks(name);
  }
  if (g_gate) {
    if (g_gate_window) g_gate_window->RemoveInputListener(g_gate);
    delete g_gate;
    g_gate = nullptr;
    g_gate_window = nullptr;
  }
  g_menu_open.store(false, std::memory_order_relaxed);
  delete g_overlay;
  g_overlay = nullptr;
}

}  // namespace mcla_host::overlay
