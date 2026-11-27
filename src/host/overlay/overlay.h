// mcla-x86 host: the in-game overlay (T7): an FPS counter, a statistics screen and a mod menu on the SDK's ImGui.
//
// What the player gets (docs: docs/OVERLAY.md once phase 2 lands; the design notes are scratch/overlay/research.md):
//
//   F1  the mod menu: tabs Display, Graphics, Game, Cheats / mods, About. Every change applies at once (through the cvar
//       registry); a setting that needs a restart says so. "Save" keeps the changes where the launcher and the game read them.
//       While the menu is open the game gets no input at all (keyboard, mouse, pads): it is not paused, so open it from a
//       pause menu, a garage or a parked car. Escape or F1 closes it.
//   F2  the FPS counter, a small readout in a corner.
//   F5  the statistics screen: frame times and a graph, GPU, display cadence, adaptive detail, hitches, clocks and memory.
//
// The keys are cvars in the keybind category (mcla_overlay_key_menu / _fps / _stats: the SDK's F4 overlay can rebind them, so
// can the launcher), mcla_overlay switches the whole thing off (no key is listened to, nothing is created).
//
// Cost model (docs/LEARNINGS.md "An always-registered toast blocked the fused gamma paint"): the ImGui dialog EXISTS ONLY while
// something is visible, so with the counter, the statistics and the menu all hidden there is no dialog, no per-frame work and
// the presenter's overlay layer stays empty (SDK 0580: the fused gamma paint is made). The one always-present piece is a key
// listener that only looks at key events.
//
// Where it plugs in (src/host/mcla_app.cpp, all on the UI thread):
//   OnCreateDialogs(drawer)  Install(window(), drawer, "mcla_x86")
//   OnConfigureFonts(atlas)  ConfigureFonts(atlas)             (the baked text sizes; without it the overlay uses the tiny default)
//   OnPostSetup()            OnRuntimeReady(input system)      (mutes the pads while the menu is open; shows what the cvars ask)
//   OnShutdown()             Shutdown()
// and, on the game thread, mcla_overlay::NoteEngineFrame / NoteHitch (overlay_feed.h) from OnFrameStart / mcla_hitch.cpp.
#pragma once

struct ImFontAtlas;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui
namespace rex::input {
class InputSystem;
}  // namespace rex::input

namespace mcla_host::overlay {

// Registers the key listener and the cvar change callbacks. Nothing is drawn yet. Does nothing with --mcla_overlay=false.
void Install(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, const char* app_name);

// Adds the baked text sizes to the ImGui font atlas (Segoe UI from the Windows fonts folder; the atlas' own default when it is
// missing). Call from ReXApp::OnConfigureFonts, which runs before the atlas is built.
void ConfigureFonts(ImFontAtlas* atlas);

// The runtime exists: the input system can be told to go quiet while the menu is open (null = no input system: no mute), and
// whatever the cvars ask to be shown at start (mcla_overlay_fps) is shown now.
void OnRuntimeReady(rex::input::InputSystem* input);

// Removes the dialog and the listener. Idempotent.
void Shutdown();

}  // namespace mcla_host::overlay
