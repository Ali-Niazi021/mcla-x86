# In-game overlay: FPS counter, statistics screen, mod menu (T7, G4)

Status: built and run on the RTX 4060 laptop (T7 phase 2, 2026-11-27): the unit test passes, the five tabs, the counter in all four
corners and the statistics screen were looked at in 1080p windowed and in the 3200x2000 fullscreen window, and the key, mouse and pad
mute, the cheats, the live changes and Save were tested with real input. The results are at the end.

Controller support (2026-11-27, merged from the branch `t3/overlay-gamepad-navigation`): built and run on the same laptop with the scripted pad
(the same poll as a real pad: `input_script.cpp` is a driver of the same input system) and with real keyboard and mouse (`tools/inject_input.py`).
Every row of the test plan passed after four fixes; the runs, the fixes, what was measured and what could not be reached (garage, GPS map, a race)
are in "Results of the controller runs" at the end. Its design and its limits are in "Controller" below. **Not tried: a physical pad in hands**
(the stick clicks are scripted, the legend flip on a real stick's drift is only argued from the code).

## What the player gets

| Key (cvar, rebindable) | What |
|---|---|
| F1 (`mcla_overlay_key_menu`) | The mod menu. Tabs: **Display** (the overlay itself; fullscreen, anti-aliasing, HDR, texture sharpness, anisotropy, tearing / variable refresh, **Upscaling**: on / off and the upscaler (AMD FSR or NVIDIA DLSS) next start, the quality ratio (DLAA / Quality / Balanced / Performance / Ultra Performance) and the DLSS model live), **Graphics** (adaptive detail and its ceiling / floor, the detail preset, the individual knobs, motion blur, pedestrians), **Game** (frame-rate profile 30 / 60 / 120 fps, applied at the next start; frame cap; 60 / 120 fps fixes; HUD; mouse), **Cheats / mods** (time of day, weather, traffic, and the classic cheats: nitro, special abilities, damage, police, slow motion, money, unlocks, no-save; off by default and locked behind a switch), **About** (build, SDK series, GPU, display, keys, files). |
| F2 (`mcla_overlay_key_fps`) | The FPS counter in a corner (`mcla_overlay_fps_corner`, `_detail`). |
| F5 (`mcla_overlay_key_stats`) | The statistics screen. |

* A change applies at once through the cvar registry. Each row says when it takes effect (`live`, `next load`, `next start`), whether the
  value was changed here or came from the launcher, and has a `reset` button.
* **Save settings** writes what was changed here to the launcher's settings and to `mcla_x86.toml` (below). Nothing is saved without it.
  Cheats, the held hour / weather and the run-time detail tier are never saved; `mcla_overlay_cheats` is off at every start.
* While the menu is open the game gets **no input** (keyboard, mouse, pads) and is **not paused**: open it from a pause menu, the garage
  or a parked car, or switch on **Pause the game while this menu is open** (Game tab, `mcla_overlay_pause`, off by default): in free roam the
  overlay then opens the game's own pause menu with the menu and closes it just before the menu closes. Escape or F1 closes it.
* **With a controller**: hold **L3 + R3** (both stick clicks) for half a second to open it (`mcla_overlay_pad_chord`, `_hold_ms`; the same chord
  closes it, and so does **B**); D-pad or left stick move, **left / right change a value** (hold to speed up), **A** selects, **X** resets
  the row to its default, **Y** saves, **LB / RB** switch tabs, the right stick scrolls. The last device used decides the legend line at the
  bottom of the menu (the pad's buttons, or the keys). The keyboard emulation's F + K (the keys of L3 and R3) are not a chord. Details below
  ("Controller").
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
| **X** | puts the focused row back to its default (the "reset" button of the row; on a "hold" row of the cheats it releases the hold) |
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
| `mcla_overlay_pause` | false | pause the game (free roam only) while the menu is open, see "Pause while the menu is open" (also a row of the Game tab) |

Which chord: the game uses Start (PAUSE in free roam), Back held (the GPS map: `menus_probe.txt`), A / B (menus), RB (the PAUSE tabs, `menus_tour.txt`)
and the triggers and sticks (driving). Before the chord is complete the game *does* see the first buttons: with Back + Start it is Start (PAUSE opens
and the chord's own Start is the press that closes it again), with LB + RB + Back it is the map (Back held); with L3 + R3 it is two stick clicks, and
**the game does nothing with them** (run `padprobe-1`, `tools/flows/overlay_pad_probe_l3r3.txt`: L3, R3 and both for 500 ms, parked in free roam, driving
at 49 to 78 mph, in the PAUSE menu and on its next tab: the screenshots do not change beyond the noise of the scene, the camera stays behind the rider,
the game's audio session shows no sound above its ambience; `quick_drive2.txt` had also found that L3 / R3 do not free a wedged bike). So **L3 + R3
stays the default**: it is the only one of the three that costs the game nothing while it is held, and two stick clicks at once do not happen by
accident. Not tried, because the save cannot reach them (the garage, the GPS map and a race need a later-story save, `PORT_STATE.md`): L3 / R3 in a race
start (a horn or a rear view is what they are on the 360 pads of other racers), in the garage and on the map. Back + Start and LB + RB + Back are there
for a pad whose sticks cannot be clicked.

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

* The pad needs the engine's frame loop (the heartbeat). **It runs from the first engine frame, ~4 s after the start: the legal notices, the title and
  the loading screen are inside it** (run `padboot-1`: the chord opened the menu over the legal screen at 8.5 s and over the loading screen at 32.5 s,
  and Start, the load and the game's pad afterwards were intact). Only the first seconds, before the first engine frame, have no poll; the keyboard works there.
* The poll assumes the game polls its pad on the thread that counts the frame. If a second thread ever calls `GetState` together with `PadTap`,
  that is the SDK's `RefreshDevices` race; `--mcla_overlay_pad=false` removes it (no poll at all).
* Cost on the game thread: one `GetState` per engine frame while the chord is on or the menu is open, none with the chord `0` and the menu closed.
  Measured by the poll itself (`[mcla-overlay] pad: poll cost` every 30 s of polling in the log): **2.0 to 6.3 us a call on average** (0.01 to 0.04 % of a
  frame), the worst call 10 to 453 us (the 453 us was in the loading screen); frame times with the poll on and off are in the results.
* **The keyboard emulation is not a chord.** With `mnk_mode` on (the default) the keys F and K are the pad's L3 and R3 (`keybind_lstick_press`,
  `keybind_rstick_press`). First build: F + K held for half a second opened the menu (run `padlegend-1`), and a second F + K could **not** close it:
  the Gate keeps keys from the game while the menu is open, so the emulation never sees them, while a real pad's chord closes it. An open-only chord
  is a trap and the keyboard has F1, so `PadTap` now takes the buttons the emulation makes out of what the chord sees: for each pad button it reads the
  emulation's own `keybind_*` cvar (once a second), and a button whose key is physically down (`GetAsyncKeyState`, only while the game's window is the
  foreground window and `mnk_mode` is on) does not count towards the chord or against its "nothing else pressed" rule. A real pad's chord still works
  with the keyboard in use; the menu itself and the hold-off are unchanged (a key held when the menu opens is masked like a pad button).
* The pad's input goes through the engine's frame rate: at 30 fps a tap shorter than one frame can be missed.

### Pause while the menu is open (opt-in, `mcla_overlay_pause`, off by default)

What the runs of the first build settled: a scripted Start in free roam gives `free-roam -> pause` within 50 ms and a second Start gives `pause ->
free-roam` at once (`padnav2-1` t108.5; Start toggles, B closes too), the screen tracker (`mcla::gamestate::CurrentScreen`, the `[mcla-mode]` lines)
shows both, and the engine frame loop (so `PadTap`) keeps running in the PAUSE menu. So the sketch of `NOTES.md` is now made:

* `InjectDriver` (`overlay_ui.cpp`): a synthetic gamepad added to the input factory (`overlay::AttachInjector`, called next to the script pad) that
  presses Start for 300 ms (the title and the menus poll every 2 to 4 updates) and reports the key-down / key-up in the keystroke stream. Unlike the
  other drivers it ignores the active callback (the menu mutes the player's pads, not this one). It exists as a device only while
  `mcla_overlay_pause` is on, so with the option off the game sees nothing new.
* `PauseTick` (game thread, the top of `PadTap`, one state and one command read while idle): when the menu opens and the screen is **free roam**, Start
  is sent and the tracker must say PAUSE within 3 s; if it does not, the overlay lets go and nothing is ever sent back. Any other screen at the open (a
  race, the garage, loading, an open pause menu, the title) sends nothing. A close request (key, Esc, B, Close, the chord, the cvar) while the overlay
  owns a pause does **not** close the menu at once: Start is sent first, the tracker must leave PAUSE (3 s limit), and then the menu closes, so the
  pause menu never shows after the mod menu is gone. If the pause ends by itself the menu closes normally.
* Log lines: `pause: the menu opened in free roam: Start sent to the game`, `the game is paused (N ms after the Start)`, `giving the game back: Start sent`,
  `the game is back (screen ..., N ms after the Start): closing the menu`, `nothing sent: the game is on screen '...'`, and WARN lines for each give-up.
* Runs: `padpause-1` (flow `overlay_pad_pause`, `--cvar mcla_overlay_pause=true`): the chord opens the menu in free roam, `Start sent` 93 ms later
  (the next engine frame), `free-roam -> pause` 66 ms after that; the chord closes it: `giving the game back`, `pause -> free-roam` 66 ms after the
  Start, `menu closed` 11 ms later (t54: free roam, no pause menu behind where the mod menu was); the player's own PAUSE (Start at 58.5), then the
  chord at 62.5: `nothing sent: the game is on screen 'pause'`, the close at 66.5 sends nothing and the player's PAUSE stays until his B (70.5);
  the menu opened and closed with `set mcla_overlay_menu=true / false` behaves like the chord. With the option on the pause menu shows round
  the mod menu (t46). Not tried: Esc / B / Close / F1 as the close (they take the same `CloseMenu`), a free-roam screen where Start opens no pause
  (a modal dialog: the 3 s give-up covers it, untested), a physical pad's own Start while the menu is open (muted like every other input).

### Test plan (the integrator, with the machine)

The six flows of this plan (all `fixture: after_intro`, LOCALAPPDATA at a scratch folder with a copy of `launcher.json`, `mcla_x86.toml` removed after each):
`overlay_pad_menu` (the table below), `overlay_pad_menu2` (long tabs held down, the right stick, the left stick, a tick box stepped, the "hold" rows, the
footer buttons), `overlay_pad_probe_l3r3` (does the game react to L3 / R3, with `--cvar mcla_overlay_pad=false --audio`), `overlay_pad_boot` (the chord at
the title and in the loading screen), `overlay_pad_legend` (+ `tools/flows/overlay_pad_legend_inject.txt` for `tools/inject_input.py`: the legend follows the last
device, keyboard and mouse as before, F + K), `overlay_pad_pause` (the pause option). Every flow has two B taps at 40 and 42 s: a late Start of the boot
can open PAUSE just as free roam begins (run `padnav-2`, where the whole test then ran in PAUSE behind the menu), and a B in free roam does nothing.

`tools/flows/overlay_pad_menu.txt` is the scripted pad: boot to free roam (~37 s), then the steps below. Run
`LOCALAPPDATA=<scratch> python tools/run_game.py --flow overlay_pad_menu --config Release --activate --cvar fullscreen=true --cvar mcla_profile=1080p --screenshot-every 2 --run-id padnav-1`; screenshots
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
`PadFrame`, the keyboard-emulation filter, `InjectDriver` / `PauseTick` of the pause option), `overlay_pad.h` (the controller's pure logic), `overlay_items.{h,cpp}`
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

## Results of the controller runs (2026-11-27)

RTX 4060 laptop, 1080p profile, fullscreen (`--activate`), Release build of the merged tree. The scripted pad goes through the same poll as a
real one; keyboard and mouse were real (`tools/inject_input.py`). Screenshots: `runs/<id>/shots/t<s>.png`, contact sheets in `scratch/padnav/sheets/`.
The merge built and linked at the first attempt (`ImGui::FocusItem`, `mnk::SetMouseLookActive` and the rest resolved), the 11 unit tests pass
(`mcla_overlay_pad_test` is the pure pad logic), `check_launcher_schema.py` is OK.

### What had to be fixed

1. **The focus was lost when the Statistics screen appeared** (`padnav-1`, t56: no ring anywhere; the next two Downs started from the first row and X
   reset the Corner instead of the slider). The cause is `SetNextWindowFocus()`, which brought the menu back over the new panel: it also moved the
   navigation focus to the window itself, and the rows are in a NavFlattened child. Now `BringWindowToDisplayFront` (only the order of the windows
   changes), and while the pad is in use the statistics window is `NoNav`. (`GetCurrentWindow()` reads `GImGui`, which the SDK does not export: the
   window comes from `GetCurrentContext()->CurrentWindow`.)
2. **The tooltip of a focused button covered the legend** (`padnav2-1`, t54 / t98: the Save settings button). ImGui shows the tooltip of a focused item
   as it does a hovered one. No tooltip while the pad is the device in use.
3. **The slider of a "hold" row could not be reached** (the cheats' "Hold the time of day", "Hold the traffic density": a tick box and a slider on one
   row; right on the box is "on", so the pad never left it; `padnav2-1` t88-t90). Right on a box that is already ticked now goes on to the slider, A
   on the slider goes back to the box, X puts the row back and releases the hold (`padnav2-2`: `-> held`, then `-> 12.5`, t90 the ring on the slider).
4. **F + K (the keyboard emulation's L3 + R3) opened the menu and could not close it** (`padlegend-1`): the chord now ignores the emulation (see "Limits").

And two things in the plan rather than the code: a late Start of the boot can open PAUSE just as free roam begins (`padnav-2`: the whole run was in
PAUSE behind the menu, so "the game saw nothing" could not be judged), so every flow has B taps at 40 and 42 s (`padprobe-1` shows the PAUSE closing at
t40); and the claim that the chord does nothing before the title was wrong (the engine frame loop runs from ~4 s, see "Limits").
The log also gained the `pad: poll cost` line (below).

### The test plan, row by row (`padnav-3`, the first valid run of the fixed build; the owner's `launcher.json` has the same SHA-256 before and after)

| Row | Result |
|---|---|
| start-up | pass: `pad: on (chord L3 + R3, hold 500 ms)` at 0.7 s, `a controller is connected (guest user 0 ...)` at the first engine frame (~4 s) |
| 44.5 chord | pass: `chord L3 + R3 held 500 ms: opening the menu`, `menu open`, `the menu answers the controller (last input: pad; 2 input(s) held at the open ...)`, `released after 400 ms`. t46: menu, yellow ring + band on "FPS counter", the legend readable at 3200x2000 (D-pad, < >, A green, B red, LB RB, X blue, Y yellow) |
| 46.5 A | pass: `FPS counter -> on`; t48 counter at the top right, box ticked |
| 48.5 - 50.5 | pass: `Corner -> Bottom left`, `-> Top right`; t50 counter bottom left, t52 top right |
| 52.5 - 54.5 | pass: `Statistics screen -> on`; t56 panel at the left, ring on the row (in `padnav-1`: ring lost, fixed) |
| 56.5 - 58.5 slider | pass: `Overlay background -> 0.73, 0.71, 0.65, 0.57, 0.48, 0.36, 0.2` (a line per 300 ms, the steps 0.02, 0.06, 0.08, 0.09, 0.12, 0.16: it accelerates), t60 panels see-through, t58 ring on the slider, t60 value 0.45 |
| 61.5 X | pass: `X reset Overlay background`; t64 0.75, no "reset" button on the row |
| 64.5 Start, menu open | pass: **no `[mcla-mode] -> pause`** between the open (t44) and the close; t66 no pause menu |
| tabs | pass: `RB -> tab Graphics / Game / Cheats / mods / About / Display`, `LB -> tab About`; the first row has the ring on Display, Graphics (Adaptive detail), Game (Frame-rate profile) and Cheats / mods (Allow cheats); About is text only, so there is nothing to focus |
| 87.5 A | pass: t90 the Anti-aliasing list is open (Off / FXAA / FXAA (extreme)) |
| 90.5 B, 91.5 right | pass: no `B closes the menu`, the list closes, the menu stays; `Anti-aliasing (swap_post_effect) -> FXAA`; t92 |
| 92.5 Y | pass: `Y save: Saved 5 setting(s) to the launcher and to mcla_x86.toml`; t94 status line, Save settings greyed, "no unsaved changes" (the toml has the five settings, the scratch `launcher.json` copy got them) |
| 96.5 B + Start held 1.2 s | pass: `B closes the menu`, `2 input(s) held at the close (0x2010)`, `menu closed`, `released after 1190 ms`; no `-> pause`; t98 free roam, counter and statistics still on |
| 100.5, 104.5 chords | pass: `opening the menu` (held 500 ms), `closing the menu` (500 ms), `input(s) held at the close (0xC0)`, `released after 395 ms` |
| 108.5 / 112.5 control | pass: `free-roam -> pause` within 50 ms of the Start, `pause -> free-roam` after the B: the detector works |
| car, counter | the speedometer reads 0 in every shot; the counter and statistics stay on after the close (they are settings) |

### Beyond the table

* **Long tabs, the sticks, the footer** (`padnav2-1`, `padnav2-2`; flow `overlay_pad_menu2`): the D-pad held down for 4.5 s runs the focus down the Graphics tab, the
  list scrolls with it and ends on the Save settings button; held up it goes back to the first row and the top. The right stick scrolls the list without
  moving the focus (down 1.5 s, up 1.5 s: back at the top). The left stick is the D-pad (`ls_down` x2, `ls_right` changed the Lowest-detail combo). Left on a tick box
  unticks it, X puts it back (`Menus and HUD at 30 Hz ... -> off`, `X reset`). The cheats gate, the hold row (fix 3), A on Save settings (`save: 3 setting(s)`),
  right to Close, A on Close: `menu closed`. No PAUSE, no stray input.
* **(2) The chord at the title and in a loading screen** (`padboot-1`, flow `overlay_pad_boot`): at 8.5 s, with the legal notices still on the screen, the chord opened the
  menu (`menu open`, the menu drawn over the legals) and 2 s later closed it; at 32.5 s, **in the loading screen** (`front-end -> loading` at 30.8 s), it
  opened the menu over the loading screen, the load finished with the menu open (`loading -> free-roam` at 36.6 s, the Start at 36 s muted), and the chord
  closed it at 40.5 s; the control Start at 50 s opened PAUSE, the B at 54 s closed it: the game's pad was intact. The engine frame loop (`PadTap`) runs from ~4 s.
* **(3) Keyboard and mouse, and the legend** (`padlegend-1`, pad flow `overlay_pad_legend` + `tools/flows/overlay_pad_legend_inject.txt`): the legend line was the pad's glyphs after the
  chord (t46), the key line after a real Down key at 47 s (t48), the pad's after a pad Down at 50.5 s (t52), the key line after a mouse move of 24 px at 54 s
  (t56) and after a click on "Statistics screen" at 57 s (t58, the box ticked by the mouse), the pad's after a pad A at 58.5 s (t60, the box unticked by the pad). Esc closed the
  menu (62 s); F1 opened it (66 s, last input: keyboard / mouse, the key line with "Pad: hold L3 + R3"); Down, Return, X, Space inside it moved the
  focus and worked on the Corner list (it opened, a choice was made: "unsaved changes" went 1 -> 2), and **no `-> pause`** (Return and X are the game's Start in the emulation); F1 closed it (74 s); Return with the menu
  closed opened PAUSE (76 s, the control), Backspace closed it (80 s).
* **(5) `--cvar mcla_overlay_pad=false`** (`padoff-1`, flow `overlay_pad_menu` + `set mcla_overlay_menu=true` at 44 s): `pad: off (--mcla_overlay_pad=false)`, no chord at 44.5 s,
  no other `pad:` line (no poll, no `poll cost`), the menu opened by the cvar ignores every pad step (the scripted A, down, RB, Y, B did nothing: the
  screenshots t46 - t70 are one picture with no ring and the key line without the "Pad:" hint), the game saw nothing (no `-> pause` at the scripted Start of 64.5 s), the cvar
  closed it at 80 s.
* **(1) L3 / R3 in the game** (`padprobe-1`, `--cvar mcla_overlay_pad=false --audio`, the game's WASAPI session peak logged at 20 Hz by `tools/audio_meter.py`): L3, R3 and both for 500 ms in free roam
  parked (46.5 - 55 s), driving at 49 - 78 mph (66.5 - 75 s), in the PAUSE menu and on its next tab (83.5 - 101 s): the screenshots do not change beyond the scene's own
  noise (mean difference of consecutive seconds 0.3 - 0.8 of 255 parked, 0.01 - 0.02 in PAUSE), the camera stays behind the rider, and the audio session shows no
  peak above its ambience (0.10 - 0.23 in roam, 0.04 in PAUSE; the 500 ms windows stay inside the range of the 1.5 s before them). **No reaction.** The garage, the GPS map and a race start
  could not be reached (the `after_intro` save), so those are untested.
* **(6) Cost** (the poll's own timer, every 30 s of polling): mean 2.0 - 6.3 us per engine frame over all the runs (2.2, 4.4, 5.8, 3.0 in `padnav-3`; 2.0 - 2.1 in `padab-1`), the worst call 10 - 453 us (the 453 us in a
  loading screen). Frame times with the poll off and on, `padab-1` (`quick_drive2` at `mcla_fps_target=60`, `mcla_overlay_pad` switched by the schedule every 10 s from 50 s: off, on, off, on, off, on, off): every 5 s window of the drive (50 - 118 s, 60.0 fps) reads p50 17 ms, p99 18 ms and the worst frame 17.4 - 18.1 ms with no frame over 20 ms, in both states: the 3 windows wholly inside poll-off stretches (903 frames) and the 3 wholly inside poll-on stretches (902 frames) have p99 18.0 / 18.0 ms and worst 18.0 / 18.1 ms, and the windows that straddle a switch look the same (the boot and the first seconds of free roam, before 50 s, have the usual start-up hitches in both). The frame limiter and the vsync hold every frame at 16.7 ms, a 2 us call cannot show in that; the poll's own timer in the same run: mean 2.0 - 2.1 us, worst 10 - 44 us. **No measurable cost** (0.013 % of a 16.7 ms frame).
* **(4) F + K** (the keyboard emulation's L3 + R3, `mnk_mode` on): in `padlegend-1` F + K held for 0.9 s (real keys, 84 s) opened the menu (`chord L3 + R3 held 501 ms:
  opening the menu`, last input "pad"), and a second F + K (90 s) did nothing, because the Gate keeps the keys from the emulation while the menu is open: only the
  real chord (96.5 s) closed it. Decision: **the chord ignores the keyboard emulation** (it is a pad chord; the keyboard has F1; an open-only chord is a trap). In
  `padlegend-2` (with the filter) the same two F + K left the menu alone (no `chord` line from 84 to 96.5 s, the keys were sent: `padlegend-2.inject.log`), and the real
  chord at 96.5 s opened the menu and the one at 100.5 s closed it.

## Not done yet

the controller with a physical pad in hands (everything above is the scripted pad, which goes through the same poll, and real keys and mouse); the chord and
L3 / R3 in a race start, the garage and the GPS map (the `after_intro` save does not reach them); help text of the focused row for the pad (a tooltip needs a
mouse, and is off while the pad is used); per-thread CPU in the statistics (the thread
monitor keeps it private); **the classic cheats: five work, three are untested, none is known broken** (next section); the ones still open are reputation points, unlocking
cars, freezing the traffic or the opponents and ending a pursuit (`CHEATS.md`, "Leads").

## The classic cheats (first run 2026-11-27: 5 work, 3 untested)

The Cheats / mods tab has rows for infinite nitro, infinite special abilities, no damage, no police, slow motion, money (give / take / set buttons), an
unlock switch for races, parts, upgrades and abilities, and a "don't write saves this session" switch. They use the game's own cheat bytes and
parameter words (the ones its pause menu's Cheats page and its command line use) and the engine timer's scale, so no hook was added and no codegen run
is needed. Every one is off at every start, behind "Allow cheats and mods", transient, never saved, and puts the game's own value back when it is
switched off, when the drive ends or when the gate is locked. The five driving cheats act only in free roam and races; the game itself counts a
drive with one of the first four on as cheating and pays no reputation or money for it. Money, unlocks and what is done with them can reach the
save: the tab says so, and shows the folder to back up (`<user data>\<xuid>\545407F8`). Evidence, addresses, grades and the risk of each: `docs/CHEATS.md`.
Log lines: `[mcla-cheat]` (one per change of a cheat's state; one per money press with the old and the new amount).

**Tested in the running game** (`docs/CHEATS.md`, "Results of the first run", runs `cht-ck1`..`cht-ck3`): **infinite nitro** (fires with no bar, bars never spent, and with bars
left they stay), **infinite special abilities** (LB+RB fires it with an empty meter; with the cheat off nothing happens), **no police** (the "POLICE RESPONDING" panel and the
cops are gone within two seconds of switching it on, no pursuit afterwards in that run), **slow motion** (the game's own clock runs at x0.50 / x0.25, 300 frames per 5 s, p99 18 ms;
read `guest total` in `[mcla-sim]`, the `world` figure does not show the factor) and **money** (give / take / set change the career's field, the tab shows it; the game's
own screens were not looked at). **Untested** (the row says so): **no damage** (the byte is held and given back, no damaging crash was seen with it on), **unlock races, parts,
upgrades and abilities** (the four words are set and read back, no unlocked item was looked at) and **don't write saves** (the word is set and held on every screen, but no
save happened while it was on). The gate, the release on pause, on switching off and on locking the gate, the tab with every row and its state text, and the frame times
(identical with every cheat off and on) were checked too. One defect was found and fixed: the money row checked the wrong vtable and would always have refused.

By hand (two minutes, in free roam): F1, tab Cheats / mods, tick "Allow cheats and mods", tick "Infinite nitro", close the menu (F1), drive and
fire the nitro: the bars stay where they are, and the log has `[mcla-cheat] nitro: ON`. The row's own line under the box says what the game thread
is doing (holding, waiting for a drive, refused). A row that says "refused" or a cheat that does nothing: untick it (the game's value is written
back at once) and send the log. Money: open the menu in free roam, pick an amount, press Give: the old and new amount are in the log and in the
tab, the garage shows the new amount.
