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
//   PadTap           the controller, game thread: once per engine frame it asks the input system for the pad's state (with the
//                    input drivers told they are active, whatever the menu says: the game's own poll is the one that is muted),
//                    detects the chord that opens the menu, and keeps the game's pad quiet while the menu is open and until the
//                    buttons held at its close are up. The menu (UI thread) reads the newest sample and feeds ImGui's gamepad keys.
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
#include <bit>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
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
#include "overlay_pad.h"
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
REXCVAR_DEFINE_BOOL(mcla_overlay_pad, true, "MCLA",
                    "The overlay with a controller: the chord (mcla_overlay_pad_chord) opens and closes the mod menu and the menu is "
                    "operated with the D-pad, the buttons and the bumpers. Off: the pad is not looked at and the menu is for the keyboard "
                    "and the mouse only. Live");
REXCVAR_DEFINE_INT32(mcla_overlay_pad_chord, 1, "MCLA",
                     "The controller buttons that open and close the mod menu when held together for mcla_overlay_pad_hold_ms with no "
                     "other button down: 1 both stick clicks (L3 + R3), 2 Back + Start, 3 LB + RB + Back, 0 none. Live")
    .range(0, 3);
REXCVAR_DEFINE_INT32(mcla_overlay_pad_hold_ms, 500, "MCLA",
                     "How long the controller chord must be held (milliseconds) to open or close the mod menu. Live")
    .range(200, 2000);
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

namespace pad = mcla_overlay::pad;
using rex::X_RESULT;  // the SDK's X_ERROR_* macros expand to the unqualified type

// The menu is open: read by the input drivers' active callback on the game thread, by the gate and by the dialog.
std::atomic<bool> g_menu_open{false};
// Auto-repeat key-down events the gate saw since the menu was last closed (a line in the log: whether a held key repeats at all).
std::atomic<uint32_t> g_key_repeats{0};

// ---- the controller: shared by the game thread (PadTap) and the UI thread ---------------------------------------------------------
thread_local bool t_pad_probe = false;       // this thread is making the overlay's own look at the pad: the drivers answer as if active
pad::HoldOff g_game_hold;                    // the game's pad stays quiet until these inputs (held at the chord / the close) are up
std::atomic<int64_t> g_open_pending_ms{0};   // the chord was completed and the UI thread has not opened the menu yet (steady ms; 0 = no)
std::atomic<int> g_tap_busy{0};              // threads inside PadTap (Shutdown waits for them)
rex::input::InputSystem* g_input = nullptr;  // set before the tap is installed, cleared after it is removed

int64_t NowMs() { return mcla_overlay::NowNs() / 1'000'000; }

// The newest look at the pad (written by the game thread once per engine frame, read by the UI thread).
struct PadSample {
  uint32_t bits = 0;       // pad::Digitize()
  pad::Raw raw;            // the analog values (the right stick scrolls the menu)
  int64_t at_ms = 0;
  bool connected = false;  // the input system had a pad (or the script pad) for user 0
};
std::mutex g_sample_mutex;
PadSample g_sample;
constexpr int64_t kSampleStaleMs = 1000;  // a game thread that has not looked for this long (a stall) leaves no input behind

PadSample LatestPad(int64_t now_ms) {
  PadSample s;
  {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    s = g_sample;
  }
  if (!s.connected || now_ms - s.at_ms > kSampleStaleMs) return PadSample();
  return s;
}

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
  // Game thread: the controller chord completed. Opens or closes the menu on the UI thread.
  void RequestPadToggle(bool open);
  void OpenMenuFromPad();
  pad::DeviceTracker& devices() { return devices_; }
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
  // The controller (UI thread).
  void PadFrame(ImGuiIO& io, int64_t now_ms);
  void FeedPadKeys(ImGuiIO& io, uint32_t want, float scroll);
  void SaveFromMenu();
  void FocusRing() const;
  void Legend();

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

  // The controller: while the menu is open from a session that has the pad on (mcla_overlay_pad at the open).
  bool pad_session_ = false;
  pad::MenuPad menu_pad_;             // the mapping of the pad to the menu's keys and actions
  pad::HoldOff menu_gate_;            // inputs held when the menu opened are not presses
  pad::DeviceTracker devices_;        // the last input device: the legend and the mouse cursor follow it
  pad::MenuOut pad_out_;              // this frame's pad actions
  pad::Focus focus_now_ = pad::Focus::kOther;   // what the focused row is, found while drawing this frame
  pad::Focus focus_last_ = pad::Focus::kOther;  // ... and last frame's (what the pad's left / right does now)
  uint32_t pad_bits_prev_ = 0;        // last frame's inputs after the hold-off (for the device tracker)
  uint32_t fed_ = 0;                  // ImGui gamepad keys held now (kFed* bits)
  float fed_scroll_ = 0.0f;
  bool focus_first_ = false;          // the first row of the tab takes the focus (after the open, after a tab change)
  bool nav_init_ = false;             // show the navigation cursor in the next frame
  bool nav_visible_ = false;          // ImGui draws the navigation cursor (a key or a pad was used last)
  ImGuiBackendFlags saved_backend_flags_ = 0;
  float saved_repeat_delay_ = 0.275f, saved_repeat_rate_ = 0.05f;
  bool saved_mouse_look_ = true;
  int64_t pad_log_ms_ = 0;            // rate limit of the "pad changed a value" log line
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
  pad_session_ = REXCVAR_GET(mcla_overlay_pad);
  if (pad_session_) {
    // The controller: ImGui's gamepad navigation on (the flag and the backend's "a gamepad is there"), slower key repeat for a TV, no
    // mouse capture for the right stick (the poll makes the drivers active), and what the pad holds now is not a press.
    const int64_t now = NowMs();
    saved_backend_flags_ = io.BackendFlags;
    saved_repeat_delay_ = io.KeyRepeatDelay;
    saved_repeat_rate_ = io.KeyRepeatRate;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.KeyRepeatDelay = 0.40f;
    io.KeyRepeatRate = 0.09f;
    saved_mouse_look_ = rex::input::mnk::IsMouseLookActive();
    rex::input::mnk::SetMouseLookActive(false);
    menu_gate_.Begin(LatestPad(now).bits, now);
    menu_pad_.Reset();
    pad_bits_prev_ = 0;
    fed_ = 0;
    fed_scroll_ = 0.0f;
    focus_now_ = focus_last_ = pad::Focus::kOther;
    pad_out_ = pad::MenuOut();
    focus_first_ = nav_init_ = devices_.last() == pad::Device::kPad;
    REXLOG_INFO("[mcla-overlay] pad: the menu answers the controller (last input: {}; {} input(s) held at the open are ignored until released)",
                devices_.last() == pad::Device::kPad ? "pad" : "keyboard / mouse", std::popcount(menu_gate_.Mask()));
  }
  if (!have_about_) {
    have_about_ = true;
    about_ = mcla_overlay::CollectAbout(window_->GetNativeWindowHandle());
  }
  Refresh();
}

void Overlay::CloseMenu() {
  if (!menu_open()) return;
  if (pad_session_) {
    // A button held at the close (the B that closed it, the chord) must not reach the game until it is up: the game's pad stays quiet
    // until each input held now has been released (PadTap ends it). Begun BEFORE the menu stops muting, so there is no gap.
    const int64_t now = NowMs();
    const uint32_t held = LatestPad(now).bits;
    if (held != 0) {
      g_game_hold.Begin(held, now);
      REXLOG_INFO("[mcla-overlay] pad: {} input(s) held at the close (0x{:X}): the game's pad stays quiet until they are released",
                  std::popcount(held), held);
    }
  }
  g_menu_open.store(false, std::memory_order_relaxed);
  ImGuiIO& io = drawer_->GetIO();
  if (pad_session_) {
    FeedPadKeys(io, 0, 0.0f);  // nothing stays pressed inside ImGui
    io.BackendFlags = saved_backend_flags_;
    io.KeyRepeatDelay = saved_repeat_delay_;
    io.KeyRepeatRate = saved_repeat_rate_;
    rex::input::mnk::SetMouseLookActive(saved_mouse_look_);
    pad_session_ = false;
  }
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

void Overlay::RequestPadToggle(bool open) {
  window_->app_context().CallInUIThreadDeferred([open] {
    if (!g_overlay) return;
    if (open) {
      g_overlay->OpenMenuFromPad();
    } else {
      g_overlay->CloseMenu();
    }
  });
}

void Overlay::OpenMenuFromPad() {
  devices_.Force(pad::Device::kPad);
  OpenMenu();
  g_open_pending_ms.store(0, std::memory_order_relaxed);  // the menu mutes the game itself from here on
}

// ---- the controller's game-thread half ----------------------------------------------------------------------------------------

// Once per engine frame on the game thread (overlay_feed.h SetFrameTap): the thread that polls the pads, so asking the input system
// for the state here cannot race its device list. The drivers are made active for this one call (t_pad_probe), whatever the menu
// says: the game's own poll, a moment later, is the one that gets zeros.
void PadTap(int64_t now_ns) {
  struct Busy {
    Busy() { g_tap_busy.fetch_add(1, std::memory_order_acq_rel); }
    ~Busy() { g_tap_busy.fetch_sub(1, std::memory_order_acq_rel); }
  } busy;
  static uint32_t prev_bits = 0;
  static pad::ChordDetector chord;
  static bool reported = false, was_connected = false;
  Overlay* overlay = g_overlay;
  rex::input::InputSystem* input = g_input;
  if (!overlay || !input) return;
  const int64_t now_ms = now_ns / 1'000'000;
  if (!REXCVAR_GET(mcla_overlay_pad)) {
    // Off, or switched off while the game's pad was quiet: it has its pad back at once.
    g_game_hold.Clear();
    g_open_pending_ms.store(0, std::memory_order_relaxed);
    chord.Reset();
    prev_bits = 0;
    return;
  }
  const int chord_id = REXCVAR_GET(mcla_overlay_pad_chord);
  if (!overlay->menu_open() && chord_id == 0 && !g_game_hold.Active() && g_open_pending_ms.load(std::memory_order_relaxed) == 0) {
    chord.Reset();  // nothing wants the pad: no look at all
    prev_bits = 0;
    return;
  }

  rex::input::X_INPUT_STATE state = {};
  t_pad_probe = true;
  const X_RESULT result = input->GetState(0, &state);
  t_pad_probe = false;
  PadSample sample;
  sample.at_ms = now_ms;
  if (result == X_ERROR_SUCCESS) {
    const rex::input::X_INPUT_GAMEPAD& g = state.gamepad;
    sample.connected = true;
    sample.raw.buttons = uint16_t(g.buttons);
    sample.raw.lt = g.left_trigger;
    sample.raw.rt = g.right_trigger;
    sample.raw.lx = int16_t(g.thumb_lx);
    sample.raw.ly = int16_t(g.thumb_ly);
    sample.raw.rx = int16_t(g.thumb_rx);
    sample.raw.ry = int16_t(g.thumb_ry);
    sample.bits = pad::Digitize(sample.raw, prev_bits);
  }
  prev_bits = sample.bits;
  {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    g_sample = sample;
  }
  if (!reported || sample.connected != was_connected) {
    reported = true;
    was_connected = sample.connected;
    REXLOG_INFO("[mcla-overlay] pad: {}", sample.connected ? "a controller is connected (guest user 0)" : "no controller (guest user 0)");
  }

  // The game's pad: quiet until the inputs held at the chord / at the close are up.
  const bool was_held = g_game_hold.Active();
  const int64_t held_since = g_game_hold.StartMs();
  g_game_hold.Update(sample.bits, now_ms);
  if (was_held && !g_game_hold.Active()) {
    REXLOG_INFO("[mcla-overlay] pad: released after {} ms: the game has its pad again", now_ms - held_since);
  }
  if (g_game_hold.TakeExpired()) {
    REXLOG_WARN("[mcla-overlay] pad: an input is still held {} ms after the close (a stuck button or stick?): the game has its pad again",
                pad::HoldOff::kTimeoutMs);
  }
  const int64_t pending = g_open_pending_ms.load(std::memory_order_relaxed);
  if (pending != 0 && now_ms - pending > 1500) {
    g_open_pending_ms.store(0, std::memory_order_relaxed);  // the UI thread never opened it (shut down?): do not keep the pad quiet
    REXLOG_WARN("[mcla-overlay] pad: the menu did not open within 1.5 s of the chord");
  }

  // The chord: opens the menu, or closes it when it is open.
  const int64_t hold_ms = REXCVAR_GET(mcla_overlay_pad_hold_ms);
  if (chord.Update(sample.bits, pad::ChordMask(chord_id), now_ms, hold_ms)) {
    const bool is_open = overlay->menu_open() || g_open_pending_ms.load(std::memory_order_relaxed) != 0;
    REXLOG_INFO("[mcla-overlay] pad: chord {} held {} ms: {} the menu", pad::ChordText(chord_id), chord.HeldMs(now_ms),
                is_open ? "closing" : "opening");
    if (!is_open) {
      // Quiet from this very frame: the game's poll that follows must not see the chord's buttons.
      g_game_hold.Begin(sample.bits, now_ms);
      g_open_pending_ms.store(std::max<int64_t>(now_ms, 1), std::memory_order_relaxed);
    }
    overlay->RequestPadToggle(!is_open);
  }
}

// ---- the key listener -------------------------------------------------------------------------------------------------------

class Gate final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_overlay) return;
    const bool open = g_overlay->menu_open();
    if (!e.prev_state()) g_overlay->devices().NoteKey();  // the keyboard was used last (the legend follows)
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
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (g_overlay && g_overlay->menu_open()) g_overlay->devices().NoteKey();
    Swallow(e);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (g_overlay && g_overlay->menu_open()) g_overlay->devices().NoteMouseMove(float(e.x()), float(e.y()));
    Swallow(e);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (g_overlay && g_overlay->menu_open()) g_overlay->devices().NoteKey();
    Swallow(e);
  }

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

namespace {

// How many decimals a slider's format shows ("%.2f x" -> 2, "%d" -> 0): a stepped value is rounded to them, so 0.75 stays 0.75 and not
// 0.7500000001 (which the "changed from the default" test would call a change).
int DecimalsOf(const char* format) {
  if (!format) return 2;
  const char* dot = std::strchr(format, '.');
  if (dot && dot[1] >= '0' && dot[1] <= '9') return dot[1] - '0';
  return std::strstr(format, "%d") ? 0 : 2;
}

// What one press of left / right changes a float slider by: a fiftieth of its range, never less than the last decimal it shows.
double FloatUnit(const mcla_overlay::Item& it) {
  return std::max((it.max - it.min) / 50.0, std::pow(10.0, -DecimalsOf(it.format)));
}

double RoundTo(double v, int decimals) {
  const double k = std::pow(10.0, decimals);
  return std::round(v * k) / k;
}

// The focus the controller (or the keyboard) has: a band across the row and a thick ring round the item, in a colour that reads
// from across a room. ImGui's own 2 px cursor stays (the same colour) for the items that do not draw the ring.
constexpr ImU32 kFocusBand = IM_COL32(255, 214, 64, 52);
constexpr ImU32 kFocusRing = IM_COL32(255, 214, 64, 255);

}  // namespace

void Overlay::FocusRing() const {
  if (!nav_visible_ || !ImGui::IsItemFocused()) return;
  const float em = ImGui::GetFontSize();
  const float grow = 0.10f * em;
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - grow, a.y - grow), ImVec2(b.x + grow, b.y + grow), kFocusRing, 0.30f * em, 0,
                                      0.16f * em);
}

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
  // The controller: this frame's left / right steps act on the item that has the focus. `row_focused`: a widget of this row has it.
  const int steps = pad_out_.adjust;
  const int mult = pad_out_.adjust_mult;
  bool row_focused = false;
  if (focus_first_ && !disabled_why) {
    ImGui::SetKeyboardFocusHere();  // the first row of a tab takes the focus (after the open and after LB / RB)
    focus_first_ = false;
  }
  // A line in the log for a change the pad made (one per 400 ms: a held button repeats).
  auto pad_log = [&](const std::string& value) {
    const int64_t now = NowMs();
    if (now - pad_log_ms_ < 400) return;
    pad_log_ms_ = now;
    REXLOG_INFO("[mcla-overlay] pad: {} ({}) -> {}", it.label, it.cvar, value);
  };
  switch (it.kind) {
    case Kind::kToggle: {
      bool v = CvarBool(it.cvar);
      bool changed = ImGui::Checkbox("##v", &v);
      if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kToggle;
        if (!changed && steps != 0 && v != (steps > 0)) {
          v = steps > 0;
          changed = true;
          pad_log(v ? "on" : "off");
        }
        FocusRing();
      }
      if (changed) {
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
      bool changed = ImGui::SliderInt("##v", &v, int(it.min), int(it.max), it.format);
      if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kSlider;
        if (!changed && steps != 0) {
          const int nv = int(std::lround(pad::StepValue(double(v), steps, mult, 1.0, it.min, it.max)));
          if (nv != v) {
            v = nv;
            changed = true;
            pad_log(std::to_string(v));
          }
        }
        FocusRing();
      }
      if (changed) SetCvarNumber(it.cvar, double(v), !is_cheat);
      break;
    }
    case Kind::kFloat: {
      float v = float(CvarNumber(it.cvar));
      bool changed = ImGui::SliderFloat("##v", &v, float(it.min), float(it.max), it.format);
      if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kSlider;
        if (!changed && steps != 0) {
          const double nv = RoundTo(pad::StepValue(double(v), steps, mult, FloatUnit(it), it.min, it.max), DecimalsOf(it.format));
          if (std::fabs(nv - double(v)) > 1e-9) {
            v = float(nv);
            changed = true;
            pad_log(fmt::format("{:.4g}", nv));
          }
        }
        FocusRing();
      }
      if (changed) SetCvarNumber(it.cvar, double(v), !is_cheat);
      break;
    }
    case Kind::kChoice: {
      const std::string current = CvarString(it.cvar);
      const char* preview = nullptr;
      std::string custom;
      int current_index = -1;
      for (int i = 0; i < it.choice_count; ++i) {
        if (current == it.choices[i].value) {
          preview = it.choices[i].label;
          current_index = i;
        }
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
      } else if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kChoice;
        if (steps != 0) {
          const int next = pad::StepChoice(current_index, steps, it.choice_count);
          if (next >= 0 && next != current_index) {
            SetCvar(it.cvar, it.choices[next].value, !is_cheat);
            pad_log(it.choices[next].label);
          }
        }
        FocusRing();
      }
      break;
    }
    case Kind::kHoldInt:
    case Kind::kHoldFloat: {
      const double value = CvarNumber(it.cvar);
      bool held = std::fabs(value - it.off_value) > 1e-6;
      bool hold_changed = ImGui::Checkbox("##hold", &held);
      if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kToggle;
        if (!hold_changed && steps != 0 && held != (steps > 0)) {
          held = steps > 0;
          hold_changed = true;
          pad_log(held ? "held" : "released");
        }
        FocusRing();
      }
      if (hold_changed) {
        SetCvarNumber(it.cvar, held ? it.on_value : it.off_value, !is_cheat);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (!held) ImGui::BeginDisabled();
      float v = float(held && !hold_changed ? value : it.on_value);
      bool changed = false;
      if (it.kind == Kind::kHoldInt) {
        int iv = int(std::lround(v));
        changed = ImGui::SliderInt("##v", &iv, int(it.min), int(it.max), it.format);
        if (ImGui::IsItemFocused()) {
          row_focused = true;
          focus_now_ = pad::Focus::kSlider;
          if (!changed && steps != 0) {
            const int nv = int(std::lround(pad::StepValue(double(iv), steps, mult, 1.0, it.min, it.max)));
            if (nv != iv) {
              iv = nv;
              changed = true;
              pad_log(std::to_string(iv));
            }
          }
          FocusRing();
        }
        v = float(iv);
      } else {
        changed = ImGui::SliderFloat("##v", &v, float(it.min), float(it.max), it.format);
        if (ImGui::IsItemFocused()) {
          row_focused = true;
          focus_now_ = pad::Focus::kSlider;
          if (!changed && steps != 0) {
            const double nv = RoundTo(pad::StepValue(double(v), steps, mult, FloatUnit(it), it.min, it.max), DecimalsOf(it.format));
            if (std::fabs(nv - double(v)) > 1e-9) {
              v = float(nv);
              changed = true;
              pad_log(fmt::format("{:.4g}", nv));
            }
          }
          FocusRing();
        }
      }
      if (changed) SetCvarNumber(it.cvar, double(v), !is_cheat);
      if (!held) ImGui::EndDisabled();
      break;
    }
    case Kind::kPreset: {
      const std::string current = CurrentPresetName();
      const int count = int(std::size(kPresets));
      int shown_index = 0;
      for (int i = 0; i < count; ++i) {
        if (current == kPresets[i].value) shown_index = i;
      }
      auto apply = [this](const PresetOption& option) {
        const int n = ApplyDetailPreset(option.value, gov_on_);
        if (n < 0) {
          status_ = fmt::format("Preset {} is not known to this build.", option.label);
        } else if (gov_on_) {
          status_ = fmt::format("Preset {}: adaptive detail now starts from tier {} (its floor) and goes up to its ceiling.",
                                option.label, PresetFloorTier(option.value));
        } else {
          status_ = fmt::format("Preset {}: {} knob(s) set (a knob you set yourself kept its value).", option.label, n);
        }
      };
      if (ImGui::BeginCombo("##v", kPresets[shown_index].label)) {
        for (const PresetOption& option : kPresets) {
          const bool selected = current == option.value;
          if (ImGui::Selectable(option.label, selected)) apply(option);
          if (ImGui::IsItemHovered()) Tooltip(option.help);
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      } else if (ImGui::IsItemFocused()) {
        row_focused = true;
        focus_now_ = pad::Focus::kChoice;
        if (steps != 0) {
          const int next = pad::StepChoice(shown_index, steps, count);
          if (next != shown_index) {
            apply(kPresets[next]);
            pad_log(kPresets[next].label);
          }
        }
        FocusRing();
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

  // The row that has the focus is lit; the pad's X puts it back to its default (the same as the small button).
  if (row_focused) {
    if (pad_out_.reset && !disabled_why && it.kind != Kind::kPreset && it.cvar && *it.cvar && IsNonDefault(it.cvar)) {
      ResetCvar(it.cvar, !is_cheat);
      status_ = fmt::format("{}: back to its default.", it.label);
      REXLOG_INFO("[mcla-overlay] pad: X reset {} ({})", it.label, it.cvar);
    }
    if (nav_visible_) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, kFocusBand);
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
  Section("Overlay", {&kOverlayOn, &kPadChord});
  ImGui::TextColored(kDim, "Keys: menu %s, FPS counter %s, statistics %s (rebind in the launcher, or in the F4 settings overlay).",
                     REXCVAR_GET(mcla_overlay_key_menu).c_str(), REXCVAR_GET(mcla_overlay_key_fps).c_str(),
                     REXCVAR_GET(mcla_overlay_key_stats).c_str());
  ImGui::TextColored(kDim, "Controller: hold %s for %.1f s to open or close this menu (B closes it too).",
                     pad::ChordText(REXCVAR_GET(mcla_overlay_pad_chord)), double(REXCVAR_GET(mcla_overlay_pad_hold_ms)) / 1000.0);
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
  ImGui::SeparatorText("Controller");
  ImGui::BulletText("%s held for %.1f s  opens and closes the mod menu (B closes it too)",
                    pad::ChordText(REXCVAR_GET(mcla_overlay_pad_chord)), double(REXCVAR_GET(mcla_overlay_pad_hold_ms)) / 1000.0);
  ImGui::BulletText("D-pad / left stick  move,  left / right  change a value (hold to speed up),  A  select,  B  back");
  ImGui::BulletText("X  reset the row,  Y  save,  LB / RB  previous / next tab,  right stick  scroll");
  ImGui::BulletText("While the menu is open the game gets nothing from the pad; a button held at the close is ignored until released");
  ImGui::SeparatorText("Files");
  ImGui::TextWrapped("Config: %s", config_path_.string().c_str());
  const std::filesystem::path launcher = mcla_overlay::LauncherSettingsPath();
  std::error_code ec;
  ImGui::TextWrapped("Launcher settings: %s%s", launcher.empty() ? "(none)" : launcher.string().c_str(),
                     (!launcher.empty() && std::filesystem::exists(launcher, ec)) ? "" : "  (not found)");
}

void Overlay::SaveFromMenu() {
  if (mcla_overlay::SavableChangeCount() == 0) {
    status_ = "No unsaved changes.";
  } else {
    status_ = mcla_overlay::SaveSettings(config_path_).message;
  }
}

namespace {

// One round (A B X Y) or rounded-square (LB, D-pad) button glyph and its label, drawn with the draw list, as one item of the legend.
float GlyphWidth(const char* text, bool round) {
  const float em = ImGui::GetFontSize();
  return round ? 1.3f * em : ImGui::CalcTextSize(text).x + 0.9f * em;
}
void Glyph(const char* text, ImU32 fill, bool round) {
  const float em = ImGui::GetFontSize();
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float h = 1.3f * em;
  const float w = GlyphWidth(text, round);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (round) {
    dl->AddCircleFilled(ImVec2(p.x + w * 0.5f, p.y + h * 0.5f), h * 0.5f, fill, 24);
  } else {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, 0.3f * em);
  }
  dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(16, 16, 20, 255), text);
  ImGui::Dummy(ImVec2(w, h));
}
void HintText(const char* text) {
  const float em = ImGui::GetFontSize();
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float h = 1.3f * em;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), IM_COL32(225, 228, 232, 255), text);
  ImGui::Dummy(ImVec2(ts.x, h));
}

}  // namespace

// The last line of the menu: what the buttons do. The pad's when the pad was used last, the keyboard's and the mouse's otherwise.
void Overlay::Legend() {
  const float em = ImGui::GetFontSize();
  if (pad_session_ && devices_.last() == pad::Device::kPad) {
    constexpr ImU32 kGrey = IM_COL32(200, 202, 208, 255), kGreen = IM_COL32(110, 205, 110, 255), kRed = IM_COL32(235, 90, 85, 255),
                    kBlue = IM_COL32(95, 150, 245, 255), kYellow = IM_COL32(245, 205, 70, 255);
    struct Hint {
      const char* glyph;
      const char* glyph2;  // a second glyph before the text (LB RB)
      ImU32 color;
      bool round;
      const char* text;
    };
    static constexpr Hint kHints[] = {{"D-pad", nullptr, kGrey, false, "Move"}, {"< >", nullptr, kGrey, false, "Change"},
                                      {"A", nullptr, kGreen, true, "Select"},   {"B", nullptr, kRed, true, "Back"},
                                      {"LB", "RB", kGrey, false, "Tabs"},       {"X", nullptr, kBlue, true, "Reset"},
                                      {"Y", nullptr, kYellow, true, "Save"}};
    const float gap = 0.35f * em, between = 1.0f * em;
    float room = ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const Hint& h : kHints) {
      float need = GlyphWidth(h.glyph, h.round) + gap + ImGui::CalcTextSize(h.text).x;
      if (h.glyph2) need += gap + GlyphWidth(h.glyph2, h.round);
      if (!first) need += between;
      if (need > room) break;  // a narrow window drops the last hints, never wraps
      room -= need;
      if (!first) ImGui::SameLine(0.0f, between);
      first = false;
      Glyph(h.glyph, h.color, h.round);
      if (h.glyph2) {
        ImGui::SameLine(0.0f, gap);
        Glyph(h.glyph2, h.color, h.round);
      }
      ImGui::SameLine(0.0f, gap);
      HintText(h.text);
    }
  } else {
    ImGui::TextColored(kDim, "Arrow keys or mouse: move   Enter / Space or click: select   Left / Right: change   Ctrl+Tab: tabs   Esc or %s: close%s%s",
                       REXCVAR_GET(mcla_overlay_key_menu).c_str(), pad_session_ ? "   Controller: hold " : "",
                       pad_session_ ? pad::ChordText(REXCVAR_GET(mcla_overlay_pad_chord)) : "");
  }
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
    SaveFromMenu();
  }
  FocusRing();
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
  FocusRing();
  Legend();
}

void Overlay::DrawMenu(const ImGuiIO& io, float em) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x * 0.92f, 58.0f * em), std::min(io.DisplaySize.y * 0.86f, 40.0f * em)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(std::min(1.0f, float(REXCVAR_GET(mcla_overlay_opacity)) + 0.2f));
  // Esc closes the menu, except when it was only closing a combo (ImGui has already closed the popup by now).
  const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !popup_open_last_frame_ && !io.WantTextInput;
  // B on the pad does the same (ImGui closes the open combo with it; the menu closes when there was none).
  const bool pad_back = pad_out_.back && !popup_open_last_frame_ && !io.WantTextInput;
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
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.35f + ImGui::GetFontSize() * 1.9f +  // ... and the legend
                         ImGui::GetTextLineHeightWithSpacing() * float((status_.empty() ? 0 : 2) + (restart_pending_.empty() ? 0 : 1));
    if (ImGui::BeginTabBar("##tabs")) {
      for (int t = 0; t < kTabCount; ++t) {
        const bool tab_selected =
            ImGui::BeginTabItem(kTabNames[t], nullptr, select_tab_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
        FocusRing();
        if (tab_selected) {
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
    focus_first_ = false;  // it was for the first row of this frame's tab, if it had one
    Footer();
  }
  ImGui::End();
  popup_open_last_frame_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (escape || pad_back) {
    if (pad_back) REXLOG_INFO("[mcla-overlay] pad: B closes the menu");
    // Not from inside the draw: the dialog may go away with the menu.
    window_->app_context().CallInUIThreadDeferred([] {
      if (g_overlay) g_overlay->CloseMenu();
    });
  }
}

// ---- the controller's menu half -----------------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kFedUp = 1u << 0, kFedDown = 1u << 1, kFedLeft = 1u << 2, kFedRight = 1u << 3, kFedActivate = 1u << 4,
                   kFedCancel = 1u << 5;
}  // namespace

// What ImGui is told: its gamepad keys for the directions, A (activate) and B (cancel) go in as key events (ImGui's navigation does
// the rest: it moves the focus, presses buttons and tick boxes, opens and closes combos, scrolls to the focused item); the right
// stick goes in as the "left stick" scroll axis, which ImGui turns into scrolling of the focused window. Events are queued and
// processed at the next frame; only changes are sent.
void Overlay::FeedPadKeys(ImGuiIO& io, uint32_t want, float scroll) {
  struct Map {
    uint32_t bit;
    ImGuiKey key;
  };
  static constexpr Map kMap[] = {{kFedUp, ImGuiKey_GamepadDpadUp},       {kFedDown, ImGuiKey_GamepadDpadDown},
                                 {kFedLeft, ImGuiKey_GamepadDpadLeft},   {kFedRight, ImGuiKey_GamepadDpadRight},
                                 {kFedActivate, ImGuiKey_GamepadFaceDown}, {kFedCancel, ImGuiKey_GamepadFaceRight}};
  for (const Map& m : kMap) {
    if ((want ^ fed_) & m.bit) io.AddKeyEvent(m.key, (want & m.bit) != 0);
  }
  fed_ = want;
  if (scroll != fed_scroll_) {
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, scroll > 0.0f, scroll > 0.0f ? scroll : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, scroll < 0.0f, scroll < 0.0f ? -scroll : 0.0f);
    fed_scroll_ = scroll;
  }
}

// One frame of the pad while the menu is open (UI thread, inside the ImGui frame): the newest look the game thread took, minus what
// was held at the open, through the mapping, into ImGui's keys and the menu's own actions (tabs, save; the row actions are in Row).
void Overlay::PadFrame(ImGuiIO& io, int64_t now_ms) {
  focus_last_ = popup_open_last_frame_ ? pad::Focus::kOther : focus_now_;  // an open combo's list takes left / right for itself
  focus_now_ = pad::Focus::kOther;
  pad_out_ = pad::MenuOut();
  nav_visible_ = io.NavVisible;
  if (!pad_session_) return;

  const PadSample sample = LatestPad(now_ms);
  menu_gate_.Update(sample.bits, now_ms);
  const uint32_t gate = menu_gate_.Mask();
  const uint32_t bits = sample.bits & ~gate;
  devices_.NotePad(bits, pad_bits_prev_);
  pad_bits_prev_ = bits;
  pad_out_ = menu_pad_.Update(bits, now_ms, focus_last_);

  uint32_t want = 0;
  if (pad_out_.dirs & pad::kNavUp) want |= kFedUp;
  if (pad_out_.dirs & pad::kNavDown) want |= kFedDown;
  if (pad_out_.dirs & pad::kNavLeft) want |= kFedLeft;
  if (pad_out_.dirs & pad::kNavRight) want |= kFedRight;
  if (pad_out_.activate) want |= kFedActivate;
  if (pad_out_.cancel) want |= kFedCancel;
  const float scroll = (gate & (pad::kRsUp | pad::kRsDown)) ? 0.0f : -pad::StickValue(sample.raw.ry);  // stick up = scroll up
  FeedPadKeys(io, want, scroll);

  // The focus rectangle shows from the first press (ImGui shows it for its own navigation keys; LB, RB, X and Y are ours), and from
  // the open when the pad opened the menu.
  if (nav_init_ || pad_out_.pressed != 0) {
    ImGui::SetNavCursorVisible(true);
    nav_init_ = false;
  }

  if (pad_out_.tab_step != 0) {
    const int next = (tab_ + pad_out_.tab_step + kTabCount) % kTabCount;
    select_tab_ = next;
    focus_first_ = true;  // the new tab's first row takes the focus: the old row is gone
    REXLOG_INFO("[mcla-overlay] pad: {} -> tab {}", pad_out_.tab_step > 0 ? "RB" : "LB", kTabNames[next]);
  }
  if (pad_out_.save) {
    SaveFromMenu();
    REXLOG_INFO("[mcla-overlay] pad: Y save: {}", status_);
  }
}

void Overlay::Draw(ImGuiIO& io) {
  const int64_t now = mcla_overlay::NowNs();
  const bool fps = REXCVAR_GET(mcla_overlay_fps);
  const bool stats = REXCVAR_GET(mcla_overlay_stats);
  const bool menu = menu_open();
  sampler_.Update(now, stats);
  ScopedLook look(io);
  if (menu) {
    PadFrame(io, now / 1'000'000);
    // The mouse-look driver puts its own visibility back when it lets go of the mouse; the menu needs the cursor (hidden while the
    // pad is what is being used: a mouse that moves brings it back).
    const auto want = pad_session_ && devices_.last() == pad::Device::kPad ? rex::ui::Window::CursorVisibility::kHidden
                                                                            : rex::ui::Window::CursorVisibility::kVisible;
    if (window_->GetCursorVisibility() != want) window_->SetCursorVisibility(want);
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
    //
    // The controller adds two things. The overlay's own look at the pad (PadTap, on the game thread) is answered as if active
    // (t_pad_probe), or the menu could not read what the game must not see. And the game's pad stays quiet in two more cases: the
    // chord was completed and the menu is about to open, and after the close until the inputs held then are released (g_game_hold).
    rex::ui::ImGuiDrawer* drawer = g_overlay->drawer();
    input->SetActiveCallback([drawer]() {
      if (t_pad_probe) return true;
      if (g_menu_open.load(std::memory_order_relaxed)) return false;
      if (g_open_pending_ms.load(std::memory_order_relaxed) != 0 || g_game_hold.Active()) return false;
      return !drawer->GetIO().WantCaptureMouse;
    });
    g_input = input;
    mcla_overlay::SetFrameTap(&PadTap);
    REXLOG_INFO("[mcla-overlay] pad: {} (chord {}, hold {} ms)", REXCVAR_GET(mcla_overlay_pad) ? "on" : "off (--mcla_overlay_pad=false)",
                pad::ChordText(REXCVAR_GET(mcla_overlay_pad_chord)), REXCVAR_GET(mcla_overlay_pad_hold_ms));
  }
  g_overlay->Refresh();  // mcla_overlay_fps on the command line or in the config shows at start
}

void Shutdown() {
  mcla_overlay::SetFrameTap(nullptr);  // the game thread leaves PadTap, then nobody calls it again
  for (int i = 0; i < 250 && g_tap_busy.load(std::memory_order_acquire) != 0; ++i) Sleep(1);
  g_input = nullptr;
  g_game_hold.Clear();
  g_open_pending_ms.store(0, std::memory_order_relaxed);
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
