# In-game overlay: FPS counter, statistics screen, mod menu (T7, G4)

Status: built and run on the RTX 4060 laptop (T7 phase 2, 2026-11-27): the unit test passes, the five tabs, the counter in all four
corners and the statistics screen were looked at in 1080p windowed and in the 3200x2000 fullscreen window, and the key, mouse and pad
mute, the cheats, the live changes and Save were tested with real input. The results are at the end.

Controller support (2026-11-27, branch `t3/overlay-gamepad-navigation`): written and syntax-checked (the game build's flags, the unit test run by the
compiler), **not built or run yet**. Its design, its limits and the test plan with the scripted pad are in "Controller" below.

## What the player gets

| Key (cvar, rebindable) | What |
|---|---|
| F1 (`mcla_overlay_key_menu`) | The mod menu. Tabs: **Display** (the overlay itself; fullscreen, anti-aliasing, HDR, texture sharpness, anisotropy, tearing / variable refresh), **Graphics** (adaptive detail and its ceiling / floor, the detail preset, the individual knobs, motion blur, pedestrians), **Game** (frame-rate profile 30 / 60 / 120 fps, applied at the next start; frame cap; 60 / 120 fps fixes; HUD; mouse), **Cheats / mods** (time of day, weather, traffic; off by default and locked behind a switch), **About** (build, SDK series, GPU, display, keys, files). |
| F2 (`mcla_overlay_key_fps`) | The FPS counter in a corner (`mcla_overlay_fps_corner`, `_detail`). |
| F5 (`mcla_overlay_key_stats`) | The statistics screen. |

* A change applies at once through the cvar registry. Each row says when it takes effect (`live`, `next load`, `next start`), whether the
  value was changed here or came from the launcher, and has a `reset` button.
* **Save settings** writes what was changed here to the launcher's settings and to `mcla_x86.toml` (below). Nothing is saved without it.
  Cheats, the held hour / weather and the run-time detail tier are never saved; `mcla_overlay_cheats` is off at every start.
* While the menu is open the game gets **no input** (keyboard, mouse, pads) and is **not paused**: open it from a pause menu, the garage
  or a parked car. Escape or F1 closes it.
* **With a controller**: hold **L3 + R3** (both stick clicks) for half a second to open it (`mcla_overlay_pad_chord`, `_hold_ms`; the same chord
  closes it, and so does **B**); D-pad or left stick move, **left / right change a value** (hold to speed up), **A** selects, **X** resets
  the row to its default, **Y** saves, **LB / RB** switch tabs, the right stick scrolls. The last device used decides the legend line at the
  bottom of the menu (the pad's buttons, or the keys). Details below ("Controller").
* `--mcla_overlay=false` removes it completely (no key is listened to, nothing is created).

## Cost model (read this before changing anything)

The ImGui dialog **exists only while something is visible** (counter, statistics or menu). Hidden = no dialog = no per-frame work, and
the presenter's overlay layer stays empty, so SDK patch 0580 (`ui_overlay_active_when_drawn`) lets the fused gamma paint (04a0) run
(LEARNINGS "An always-registered toast blocked the fused gamma paint"). While the counter is shown the overlay counts as shown: about
0.2 ms of GPU time at 1080p (lk-fl4 / lk-fl6) and no fused paint; the cvar help says so. The always-present piece is a key listener that
only looks at key events. On the game thread the overlay's hook costs one relaxed load per engine frame while nothing is shown.
The counter and the statistics return `RepaintsContinuously() == false` (drawn with each new guest frame, no presents of their own);
the menu returns true (it needs input every frame).

## How input is kept from the game

`Window` hands every input event to its listeners from the highest z-order down, and stops when one marks the event handled. The ImGui
drawer is at 64 and does not mark key or mouse events handled; the input drivers (keyboard to pad) and the SDK's key binds are at 0.
The overlay's listener sits at 32 (`Gate` in `overlay_ui.cpp`): the drawer has already fed ImGui, then the gate handles the three hotkeys
and, while the menu is open, swallows key-down, character, mouse-down, -move and -wheel events. Releases always pass, so nothing
sticks. A key that went down while the menu was open is also kept from the game after the menu closes until it has been released and
pressed again: its auto-repeats (a held key) are swallowed too, so Enter pressed in the menu and still held at the close does not become
the game's Start. The input system's active callback
(`InputSystem::SetActiveCallback`) is replaced by one that is also false while the menu is open, which silences the pads and drops mouse
look; it keeps the SDK's rule (silent while the mouse is over an ImGui window). With the controller code the callback has three more
rules, all for the pad: it answers true to the overlay's own look at the pad (a thread-local flag set only around that one call), it is
false from the moment the chord completes until the menu has opened, and it stays false after the close until the inputs held at the close
are released (`HoldOff`; the keyboard's `Gate` rule for pads). See "Controller".

## Controller

### What it does

| Input | In the menu |
|---|---|
| hold the chord (default **L3 + R3**, 500 ms, nothing else pressed; the triggers and sticks do not count) | opens the menu; again, closes it |
| D-pad, left stick up / down | moves the focus (ImGui's own gamepad navigation: the D-pad keys are fed as `ImGuiKey_GamepadDpad*`) |
| D-pad, left stick left / right | on a slider: changes the value (a step is 2 % of the range for a float, never less than the last decimal it shows; 1 for an integer slider); on a combo: previous / next choice; on a tick box: off / on. Held: the first repeat after 380 ms, then faster over 2.2 s, and the step grows (x2 after 1 s, x3 after 2 s, x5 after 3.5 s). On a button: moves the focus sideways |
| **A** | presses the focused button, switches a tick box, opens a combo and picks in its list (ImGui's `GamepadFaceDown`). On a slider it does nothing: the steps are the pad's way |
| **B** | closes the open combo list; with none open it closes the menu (the same rule as Esc) |
| **X** | puts the focused row back to its default (the "reset" button of the row) |
| **Y** | Save settings (the same as the button; "No unsaved changes." when there are none) |
| **LB / RB** | previous / next tab (wraps) |
| right stick | scrolls the tab |
| Start, Back, L3, R3, triggers | nothing (the game does not get them either while the menu is open) |

The focus is a yellow ring round the item and a band across its row (ImGui's own 2 px cursor is yellow too); it shows from the first press, and
from the open when the pad opened the menu (the first row takes the focus). The legend at the bottom is one line: `D-pad Move  < > Change  A
Select  B Back  LB RB Tabs  X Reset  Y Save` with round colour glyphs, when the pad was the last input; the key line otherwise. The last device
is the pad after any new button press or stick crossing its threshold (drift inside it does nothing), the keyboard / mouse after a key, a click,
the wheel or a mouse move of 8 px or more. With the pad last the mouse cursor is hidden (a mouse move brings it back). The statistics screen and
the FPS counter are rows of the Display tab ("Overlay"), so the pad reaches them.

Settings (Controls in the launcher; all live):

| cvar | default | |
|---|---|---|
| `mcla_overlay_pad` | true | master switch: false = the pad is not looked at (no poll, no chord, the menu is keyboard and mouse only) |
| `mcla_overlay_pad_chord` | 1 | 1 L3 + R3, 2 Back + Start, 3 LB + RB + Back, 0 none (also a row of the Game tab) |
| `mcla_overlay_pad_hold_ms` | 500 | 200 to 2000 |

Which chord: the game uses Start (PAUSE in free roam), Back held (the GPS map: `menus_probe.txt`), A / B (menus), RB (the PAUSE tabs, `menus_tour.txt`)
and the triggers and sticks (driving); nothing found in this repository says it binds the stick clicks, so L3 + R3 is the default. Before the chord is complete the game *does* see
the first buttons: with L3 + R3 that is two stick clicks (unverified whether they do anything in a drive: press `l3/300` in roam and look); with Back
+ Start it is Start (PAUSE opens), with LB + RB + Back it is the map. Those two are there for a pad whose sticks cannot be clicked.

### How the pad is read (no SDK change)

The SDK's drivers answer a zero state while the active callback is false, and the SDL driver does not even pump its events then (`GetDeviceState`
calls `QueueControllerUpdate` only when active). So the overlay cannot read a muted pad from the UI thread, and `InputSystem::GetState` is not
thread-safe (`RefreshDevices` rebuilds the device lists on every call: a UI-thread call against the game's own poll is a data race). The overlay
therefore reads the pad **on the game thread**, the thread that polls it (the XInput wrapper `sub_8275CB78` is called under the per-frame function
`sub_822C1FA8`, where the heartbeat hook runs):

* `NoteEngineFrame` (called from `OnFrameStart`, once per counted engine frame) first calls a function pointer, `PadTap` (`overlay_feed.h
  SetFrameTap`; null = one relaxed load).
* `PadTap` calls `InputSystem::GetState(0)` with a thread-local flag set; the replaced active callback answers true for that call only. It gets
  the merge of everything on guest user 0: the SDL pads, the keyboard emulation (`mnk`) and the scripted pad (`input_script.cpp`), so a scripted
  flow drives the menu exactly like a controller. Keyboard-emulation keys held at the open show up as pad buttons; they are masked until released
  (below), and `mnk::SetMouseLookActive(false)` is set while the menu is open so the probe does not make the mouse-look driver capture the mouse.
* The result is digitised (`overlay_pad.h`: 14 buttons, two triggers and four directions of each stick, with hysteresis) and stored as the
  newest sample under a mutex (stale after 1 s: a stalled game thread leaves no input behind). The chord detector and the game's hold-off run on
  the same call, so they are frame-exact with the game's own poll that follows.
* The UI thread reads the sample in `Overlay::PadFrame` (once per draw while the menu is open), minus what was held at the open, and maps it
  with `pad::MenuPad`: ImGui gets `AddKeyEvent` for D-pad up / down (and left / right when the focused item is not adjustable), `GamepadFaceDown`
  (A, not on a slider) and `GamepadFaceRight` (B); `NavEnableGamepad` and `HasGamepad` are set while the menu is open (restored at the close), the key
  repeat is slowed to 0.4 s / 0.09 s. ImGui's windowing mode (hold X) and its text-input key (Y) are never fed. Everything else (tabs, steps, reset,
  save, back) is the menu's own: `Row()` applies the steps to the focused item, so the behaviour of every row kind (toggle, int, float, choice, hold,
  preset) is in one place. The left stick is turned into D-pad presses (ImGui would scroll with it); the child list is `NavFlattened` so the focus
  can go from the rows to the Save and Close buttons; the tabs are marked `NoNav` for the pad (ImGui selects a tab the moment it is focused).

What a chord does to the game: the moment it completes, `PadTap` starts the hold-off on the chord's inputs and sets "opening", so the game's next
poll (the same frame, after the hook) already gets zeros; the UI thread opens the menu a moment later and clears "opening".

### The gate for pads

`g_game_hold` (`pad::HoldOff`, atomic) holds the inputs that were down when the chord completed or when the menu closed. The active callback is
false while the menu is open, while the chord's "opening" is pending, and while the hold-off has anything left. The game thread removes an input
from the hold-off the first time it sees it up (a second press is a new press); after 4 s a stuck input is let go with a warning. The close begins
the hold-off **before** the menu stops muting (no gap), with the inputs of the newest sample, so the B that closed the menu, a held Start and the
chord's buttons never reach the game; sticks and triggers held at the close count too (the game's pad is quiet until they are back). The menu has
the same rule for the open (`menu_gate_`): a button held when it opened is not a press (LB + RB held by the chord does not switch tabs).
The keyboard's rule is unchanged (`Gate`).

### Limits and decisions

* The pad needs the engine's frame loop (the heartbeat); before the first engine frame (the legals, the logos) and in a long blocking load there is
  no poll, so the chord does nothing there. The keyboard still does.
* The poll assumes the game polls its pad on the thread that counts the frame. If a second thread ever calls `GetState` together with `PadTap`,
  that is the SDK's `RefreshDevices` race; `--mcla_overlay_pad=false` removes it (no poll at all).
* Cost on the game thread: one `GetState` per engine frame while the chord is on or the menu is open (the game's own poll does the same work:
  tens of microseconds at most; measure it with the counter on and off before leaving it on for good); none with the chord `0` and the menu closed.
* With the keyboard emulation on (`mnk_mode`, the default) the keys F and K are the pad's L3 and R3 (`keybind_lstick_press`, `keybind_rstick_press`), so
  holding F + K for half a second opens the menu too; the keys held when the menu opens are masked like pad buttons.
* The pad's input goes through the engine's frame rate: at 30 fps a tap shorter than one frame can be missed.
* **Pause while the menu is open: not done.** It would have to send the game its own Start (a synthetic input driver added to the input system in
  the factory, exempt from the mute, with a hold of 300 ms because the title polls every 2 to 4 updates), confirm the `free-roam -> pause`
  transition in `[mcla-mode]`, and on the close send B (or Start) only if the menu opened that pause and the screen is still PAUSE. None of the
  three steps can be tried without the game (whether Start toggles the pause, how the pause menu reacts to a B while a mod menu is over it, what
  `ProfileMode` says during the 20 ms settling), and a wrong guess leaves the player in a pause menu he did not open. Not made robust, so left out.

### Test plan (the integrator, with the machine)

`tools/flows/overlay_pad_menu.txt` is the scripted pad: boot to free roam (~37 s), then the steps below. Run
`LOCALAPPDATA=<scratch> python tools/run_game.py --flow overlay_pad_menu --config Release --screenshot-every 2 --run-id padnav-1`; screenshots
`t<sec>.png` at even seconds, one at most 1.5 s after each step; delete `<exe dir>/mcla_x86.toml` afterwards (Save wrote it).

| step (s) | pad | log lines (`[mcla-overlay]`, `[mcla-sched]` marks) | picture |
|---|---|---|---|
| start-up | | `pad: on (chord L3 + R3, hold 500 ms)` (when the runtime is ready), `pad: a controller is connected (guest user 0: ...)` (the first engine frame, ~4 s: the script pad and the keyboard emulation count) | |
| 44.5 | L3 + R3 held 900 ms | `pad: chord L3 + R3 held 5xx ms: opening the menu`, `menu open: ...`, `pad: the menu answers the controller (last input: pad; 2 input(s) held at the open ...)` | t46: menu, pad legend, yellow ring on "FPS counter" |
| 46.5 | A | `pad: FPS counter (mcla_overlay_fps) -> on` | t48: counter top right, box ticked |
| 48.5, 49.5, 50.5 | down, right, left | `pad: Corner (mcla_overlay_fps_corner) -> Bottom left`, then `-> Top right` | t50: counter bottom left; t52: top right |
| 52.5 to 54.5 | down, down, A | `pad: Statistics screen (mcla_overlay_stats) -> on` | t56: statistics panel at the left |
| 56.5 to 58.5 | down, down, left held 2.5 s | `pad: Overlay background (mcla_overlay_opacity) -> 0.73`, ... (one line per 300 ms, the steps getting bigger) | t60: panels see-through (about 0.4) |
| 61.5 | X | `pad: X reset Overlay background (mcla_overlay_opacity)` | t64: back to 0.75, no "reset" button on the row |
| 64.5 | Start (menu open) | none: **no `[mcla-mode] free-roam -> pause`** | t66: no pause menu |
| 66.5 to 76.5 | RB x5, LB | `pad: RB -> tab Graphics`, `Game`, `Cheats / mods`, `About`, `Display`; `pad: LB -> tab About` | t68 Graphics, t70 Game, t72 Cheats, t74 About, t76 Display, t78 About; the first row has the ring in each |
| 78.5 to 87.5 | RB, down x7, A | `pad: RB -> tab Display` | t90: the Anti-aliasing list open |
| 90.5, 91.5 | B, right | **no** `pad: B closes the menu`; `pad: Anti-aliasing (swap_post_effect) -> FXAA` | t92: list closed, menu open, FXAA |
| 92.5 | Y | `pad: Y save: Saved ...` | t94: status line (a screenshot of the status text) |
| 96.5 | B + Start held 1.2 s | `pad: B closes the menu`, `pad: 2 input(s) held at the close (0x2010): the game's pad stays quiet until they are released`, `menu closed`, then ~1.1 s later `pad: released after N ms: the game has its pad again`; **no `-> pause`** | t98: free roam, no menu, no pause |
| 100.5, 104.5 | L3 + R3, L3 + R3 | `chord ... opening the menu` (menu open), `chord ... closing the menu`, `menu closed`, `input(s) held at the close`, `released after` | |
| 108.5, 112.5 | Start, B (menu closed) | `[mcla-mode] free-roam -> pause`, then `pause -> free-roam`: the detector works | PAUSE menu |

The game saw none of it: no `[mcla-mode]` transition between 44 s and 108 s (`grep "\[mcla-mode\]"`), the PAUSE only in the control at 108.5.
Also check: the car did not move (no throttle in the flow; the speedometer reads 0 in every shot), the FPS counter and statistics are still on
after the close (they are settings), `--cvar mcla_overlay_pad=false` repeats the run with no chord, no `pad:` lines and the menu opened by
`set mcla_overlay_menu=true` ignoring the pad; `--cvar mcla_overlay_pad_chord=2` with `back+start/900` in place of `l3+r3/900` opens it too
(and the game's PAUSE opens at the Start of the chord: expected, see above). By hand, with a real pad: the legend flips to the keys when the mouse
moves 8 px and back to the glyphs on the next pad press; a drift of a stick inside its dead zone does not flip it.

## Where the numbers come from

| Statistic | Source |
|---|---|
| fps, frame time, 1 % / 0.1 % low, over 20 / 25 / 34 ms, graph | `overlay_feed.cpp`: a lock-free ring written by `mcla::sim::OnFrameStart` (game thread, engine frames) |
| GPU busy ms, GPU fps, lag | `mcla::gov::ReadGpuCounters` (SDK 04f0 / 0510), `rexgpu_gpu_frame_seq / _lag` |
| pipelines created / in flight, shaders | SDK 0590 exports through `ReadGpuCounters` |
| adaptive detail rung, tier, VRS, compile burst | `mcla::gov::*`, `mcla::gfxtune::CurrentTier / Tiers` |
| world speed, frame cap | `mcla::sim::ImageF32(kTimerTotal)` over 2 s of real time, `mcla::sim::EffectiveCap` |
| display: presented, shown, dropped, refresh clock, cadence, held 1 / 2 / 3+, over cadence | SDK patch 0755 (`rex_present_stats`, layout `present_stats_abi.h`); without it the section says so |
| hitches: count, last cause | `mcla_hitch.cpp` calls `NoteHitch` with `math::ClassifyHitch` |
| CPU clocks per class (CAPPED), process CPU, memory, reads, page faults | `overlay_sample.cpp`, 1 Hz, only while the statistics are shown (the logic of `system_status.cpp LogSystemSample`) |

## Persistence

The launcher builds the game's command line from `%LOCALAPPDATA%\mcla_x86\launcher.json` on every start, and a command-line value
outranks `mcla_x86.toml`, so a TOML-only save would be overruled for every setting the launcher passes (`emit: always`: the detail
preset, adaptive detail, ...). Save therefore writes both: `rex::cvar::SaveConfigValues(mcla_x86.toml, names)` and, when `launcher.json`
exists and parses, `values.<cvar>` for the same names (a launcher setting's id is its cvar), plus `aa_auto = false` with
`swap_post_effect`. The file is replaced atomically, never created, never touched if it does not parse; unknown keys and ids survive
(`overlay_json.h`, unit tested). If the launcher is open it keeps its own copy: close and reopen it.

The live detail preset applies a preset's knobs as app defaults (what the real `mcla_gfx_preset` does once at first use, a `call_once`
that cannot be repeated), so a knob you set yourself keeps your value and adaptive detail can still move the others; Save writes the
preset's name. With adaptive detail on the governor owns the tier and writes the knob defaults itself, so choosing a preset sets the
tier the governor starts from instead (`mcla_gov_tier_floor`: perf 2, perf+ and lowest 1, quality and as shipped 4, ultra 5); its
ceiling has its own row.

## Files

`src/host/overlay/`: `overlay.h` (what `McLaApp` calls), `overlay_ui.cpp` (ImGui, the key listener, the cvars, the controller's glue: `PadTap`,
`PadFrame`), `overlay_pad.h` (the controller's pure logic), `overlay_items.{h,cpp}`
(the setting table, values, presets, cheats, Save), `overlay_sample.{h,cpp}` (the statistics snapshot), `overlay_feed.{h,cpp}` (the
game-thread side), `overlay_math.h`, `overlay_json.h`, `present_stats_abi.h`. Tests: `mcla/tests/test_overlay.cpp`, `mcla/tests/test_overlay_pad.cpp` (constexpr: the compiler runs it; `clang++ -fsyntax-only -std=c++23 -I
src/host/overlay mcla/tests/test_overlay_pad.cpp` is a test run).
To add a setting: an `Item` in `overlay_items.h` (cvar, label, help, how to edit) and a reference in the tab's `Section(...)` in
`overlay_ui.cpp`; a cheat gets `kFlagCheat | kFlagNoSave` and goes in `CheatCvars()`.

## Scripted runs

`mcla_overlay_fps`, `mcla_overlay_stats`, `mcla_overlay_fps_corner` / `_detail` take a schedule step like any live cvar
(`44:set mcla_overlay_fps=true`). `mcla_overlay_menu` opens and closes the menu (the key keeps it in step) and `mcla_overlay_tab`
(0 Display, 1 Graphics, 2 Game, 3 Cheats, 4 About) selects its tab: both are transient, never saved, for screenshots. The controller:
`mcla_overlay_pad` (false = no look at the pad at all), `mcla_overlay_pad_chord`, `mcla_overlay_pad_hold_ms`; the scripted pad (`--input-file`,
`tools/flows/overlay_pad_menu.txt`) drives the menu through the same poll as a real pad. Its log lines are `[mcla-overlay] pad: ...` (the chord, each
change, X / Y / LB / RB / B actions, the hold-off and its release). The log says
`[mcla-overlay] menu open` / `menu closed` and the presenter says `the UI drawers have something / nothing on the screen`, which is
how a run shows that nothing exists while everything is hidden.

## Results of the first run on the machine (2026-11-27)

* Unit test `mcla_overlay_test`: 164 checks, 0 failures. `check_launcher_schema.py`: OK (118 settings, 0 errors).
* Statistics against the log of the same seconds: frames (rate, median, p99, worst, over 20 / 25 / 34 ms, 300 frames), hitch count and
  last cause, GPU busy, presented / shown / dropped, the refresh clock (165.0 Hz, 2.75 refreshes a frame), held 1 / 2 / 3+ (0 / 25 / 75 %
  against 0 / 150 / 450 frames in the `[present-stats]` line), CPU class clocks and memory (the `[mcla-sys]` sample) all agree. World
  speed reads x1.000 (the game timer is stored with each frame start, so the span is exact; read between two UI paints it jittered by a frame, x1.007).
* Keys: F1 / F2 / F5 and their rebinds (`--mcla_overlay_key_menu=F9`, ...) work; with the menu open Return, Backspace, X, Space, E and a
  held Return, left and right clicks, a held left button released after the close, and the scripted pad's Start did not reach the game
  (the game's pause menu is the detector); with the menu closed each one did. `--mcla_overlay=false`: no key does anything.
* Live changes seen on the screenshots: sun shadows off, FXAA off, time of day 12.0 h / 2.6 h, stormy weather (and released with the
  button); detail preset Performance (sets the governor's floor with adaptive detail on).
* Save, with a copy of the owner's `launcher.json` (LOCALAPPDATA redirected): `mcla_x86.toml` got the five changed settings, the copy got
  the same values at their old places (everything else byte for byte), `aa_auto` false with `swap_post_effect`, the two ids the
  launcher does not know appended; cheats and the gate were not written.
* Cost (PresentMon 2.6, elevated, quick_drive, GPU busy of the whole frame): at the 1080p profile (3200x2000 window) hidden 6.76 ms,
  counter 6.58, statistics 6.74, both 6.70 (window to window noise 0.2 ms): not measurable. At 2000p the GPU busy drifts by 1.5 ms
  inside a run; alternating both on / off every 6 s and fitting the drift out gives +0.3 ms (sd of the fit 0.5 ms), the 0.2 ms of the
  design estimate being inside it. Frame times (p99 17.1 to 17.3 ms), Independent Flip and the `swap_direct_present` lines are the same
  in every state and the same as in b-g1; hidden, the presenter logs "the UI drawers have nothing on the screen".

## Not done yet

A pause while the menu is open (see "Controller": not made robust); the controller code itself has not run in the game yet (the
test plan above); help text of the focused row for the pad (a tooltip needs a mouse); per-thread CPU in the statistics (the thread
monitor keeps it private); the classic cheats (money, unlocks, nitro, damage, police, AI: the systems are listed in the Cheats tab).
