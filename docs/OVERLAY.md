# In-game overlay: FPS counter, statistics screen, mod menu (T7, G4)

Status: built and run on the RTX 4060 laptop (T7 phase 2, 2026-11-27): the unit test passes, the five tabs, the counter in all four
corners and the statistics screen were looked at in 1080p windowed and in the 3200x2000 fullscreen window, and the key, mouse and pad
mute, the cheats, the live changes and Save were tested with real input. The results are at the end.

## What the player gets

| Key (cvar, rebindable) | What |
|---|---|
| F1 (`mcla_overlay_key_menu`) | The mod menu. Tabs: **Display** (the overlay itself; fullscreen, anti-aliasing, HDR, texture sharpness, anisotropy, tearing / variable refresh), **Graphics** (adaptive detail and its ceiling / floor, the detail preset, the individual knobs, motion blur, pedestrians), **Game** (frame cap, 60 fps fixes, HUD, mouse), **Cheats / mods** (time of day, weather, traffic; off by default and locked behind a switch), **About** (build, SDK series, GPU, display, keys, files). |
| F2 (`mcla_overlay_key_fps`) | The FPS counter in a corner (`mcla_overlay_fps_corner`, `_detail`). |
| F5 (`mcla_overlay_key_stats`) | The statistics screen. |

* A change applies at once through the cvar registry. Each row says when it takes effect (`live`, `next load`, `next start`), whether the
  value was changed here or came from the launcher, and has a `reset` button.
* **Save settings** writes what was changed here to the launcher's settings and to `mcla_x86.toml` (below). Nothing is saved without it.
  Cheats, the held hour / weather and the run-time detail tier are never saved; `mcla_overlay_cheats` is off at every start.
* While the menu is open the game gets **no input** (keyboard, mouse, pads) and is **not paused**: open it from a pause menu, the garage
  or a parked car. Escape or F1 closes it.
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
look; it keeps the SDK's rule (silent while the mouse is over an ImGui window).

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

`src/host/overlay/`: `overlay.h` (what `McLaApp` calls), `overlay_ui.cpp` (ImGui, the key listener, the cvars), `overlay_items.{h,cpp}`
(the setting table, values, presets, cheats, Save), `overlay_sample.{h,cpp}` (the statistics snapshot), `overlay_feed.{h,cpp}` (the
game-thread side), `overlay_math.h`, `overlay_json.h`, `present_stats_abi.h`. Test: `mcla/tests/test_overlay.cpp`.
To add a setting: an `Item` in `overlay_items.h` (cvar, label, help, how to edit) and a reference in the tab's `Section(...)` in
`overlay_ui.cpp`; a cheat gets `kFlagCheat | kFlagNoSave` and goes in `CheatCvars()`.

## Scripted runs

`mcla_overlay_fps`, `mcla_overlay_stats`, `mcla_overlay_fps_corner` / `_detail` take a schedule step like any live cvar
(`44:set mcla_overlay_fps=true`). `mcla_overlay_menu` opens and closes the menu (the key keeps it in step) and `mcla_overlay_tab`
(0 Display, 1 Graphics, 2 Game, 3 Cheats, 4 About) selects its tab: both are transient, never saved, for screenshots. The log says
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

Controller navigation of the menu; a pause while the menu is open (no known safe hook); per-thread CPU in the statistics (the thread
monitor keeps it private); the classic cheats (money, unlocks, nitro, damage, police, AI: the systems are listed in the Cheats tab).
