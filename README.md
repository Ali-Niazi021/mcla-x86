# Midnight Club: Los Angeles, recompiled for x86-64 Windows

**Midnight Club: Los Angeles (Xbox 360, Complete Edition), statically recompiled to a native 64-bit Windows program.**
It is built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), a patch series on top of it, and a layer
of game hooks (60 fps, widescreen, 1080p and more, a launcher, HDR output).

Status: the game boots, drives, races and saves. On a Radeon 880M laptop it runs at 1080p and 60 fps with the
Recommended preset. On an RTX 4060 Laptop (Intel 6P+8E, 3200x2000 panel at 165 Hz) the native panel preset (the
3200x2000 profile) holds 60 fps in a scripted drive, at the Ultra and at the Performance scene detail, and 1080p Ultra
needs about 5.3 ms of GPU time a frame. The scripted drive keeps the car parked, so these are lower bounds. Details
and measurements are in [docs/PORT_STATE.md](docs/PORT_STATE.md); the native plan and its status board are in
[docs/NATIVE_PLAN.md](docs/NATIVE_PLAN.md).

> **You need your own copy of the game.** This repository contains no game files and no translated game code.
> Everything is made on your PC from your own dump of a disc you own. Do not ask for, or share, game files here.

Supported dump: Midnight Club: Los Angeles - Complete Edition (USA, Europe), title `545407F8`, media `5940C9DB`,
`default.xex` SHA-256 `c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432`. Other editions are different
builds and do not work. The setup checks this for you.

## Setup

### 1. What you need

- Windows 10 or 11, 64-bit. A GPU with Direct3D 12.
- About 30 GB of free disk space (plus your `.iso`), and an internet connection.
- Your game disc dump as an `.iso` file.
- Four programs. Open a terminal (PowerShell) and paste the lines for the ones you do not have:

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Python.Python.3.13 -e
winget install --id Microsoft.VisualStudio.2022.Community -e --override "--passive --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended"
```

Visual Studio is large (about 10 GB) and asks for permission once. If you already have Visual Studio 2022 with
"Desktop development with C++", you do not need to install it again. After installing, **close the terminal and open a
new one**.

### 2. Download and build

```powershell
git clone https://github.com/Ali-Niazi021/mcla-x86.git
cd mcla-x86
python tools/setup.py --iso "C:\path\to\your game.iso"
```

That one command does everything. Expect about 30 to 60 minutes the first time, mostly for the SDK build; you do not
have to watch it:

| Step | What it does |
|---|---|
| 1 tools | Checks Git, CMake, Python, Visual Studio and the Windows SDK. If one is missing, it prints the command to install it. |
| 2 compiler | Downloads Clang/LLVM 20.1.8 (940 MB, checksum verified) and unpacks it. No install, no admin rights. |
| nvidia | Downloads NVIDIA Streamline 2.14.16 (276 MB, checksum verified: DLSS, Reflex) into the work folder and unpacks its headers and DLLs; the SDK build copies the DLLs next to the game. Nothing of NVIDIA's is stored in the repository. `--no-nvidia` skips it (no DLSS; the game builds and plays the same). |
| 3 game | Checks that your `.iso` is the supported dump and extracts the game files (6.1 GB). |
| 4 sdk | Downloads the ReXGlue SDK, applies the patches, builds it. This is the long step. |
| 5 codegen | Translates the game's `default.xex` to C++ on your PC. |
| 6 build | Builds the game and the launcher. |
| 7 launcher | Tells the launcher where your game files are. |

If a step fails or you stop it, run the same command again. Finished steps are skipped and the download resumes.

Everything large goes to a work folder named `mcla-x86-work` next to the repo, not into the repo. To use another
place, add `--work D:\mcla-work` (a short path without spaces). To only check your PC without changing anything, run
`python tools/setup.py --check`.

### 3. Play

When setup is done it prints the path of the launcher:

```
mcla-x86-work\build\bin\Release\mcla_launcher.exe
```

Start it, keep the **Recommended (1080p)** preset, and press **Launch**. The first minutes of play can stutter a
little while the shader cache is built; it is saved and the next start is smooth.

### Update later

```powershell
git pull
python tools/setup.py
```

Finished steps are skipped, so this is much faster than the first setup.

## Playing

- **Controllers:** Xbox (XInput), DualShock 4 and DualSense work. Keyboard works too; the keys are on the launcher's
  Controls page and you can change them.
- **Overlay and mod menu:** **F1** opens the mod menu (Display, Graphics, Game, Cheats / mods, About): a change applies
  at once, each row says whether it is `live`, `next load` or `next start`, and **Save settings** keeps what you changed
  for the next start. **F2** shows an FPS counter in a corner, **F5** a statistics screen (frame times, GPU time, display
  cadence, adaptive detail, hitches). With a controller, hold both stick clicks (**L3 + R3**) for half a second to open
  the menu (the same chord closes it, and so does B); the D-pad or left stick moves, left / right change a value (hold to
  speed up), **A** selects, **X** puts the row back to its default, **Y** saves, **LB / RB** switch tabs and the right
  stick scrolls. The last device you touched decides the hint line at the bottom of the menu. While the menu is open the
  game gets no input from the keyboard, mouse or pad, and it keeps running: open it from the pause menu or when parked,
  or switch on **Pause the game while this menu is open** (Game tab, off by default; free roam only).
  The cheats (time of day, weather, traffic) are off at every start. The chord is a row of the Game tab (L3 + R3, Back +
  Start or LB + RB + Back) and the keys are rebound in the launcher; `--mcla_overlay=false` removes the whole thing,
  `--mcla_overlay_pad=false` leaves it to the keyboard and mouse.
- **Presets:** Recommended 1080p, 16:10 panel (1920x1200), Balanced 900p, Performance 720p, native panel (plain, with NVIDIA DLAA,
  with DLSS Performance, or with Frame Generation on RTX cards), and a console-like 30 fps mode. If the game does not hold 60 fps, pick a lighter preset.
- **Adaptive detail** (on by default): keeps 60 fps by lowering detail where it is least visible, and raises it again
  when the GPU has room.
- **NVIDIA DLSS (RTX cards):** the launcher has presets **Native panel 3200x2000, NVIDIA DLAA** (anti-aliasing at the full size) and **NVIDIA DLSS Performance** (scene drawn at half size, model K), and a preset for **DLSS Frame Generation** (below); the mod menu's Display tab has an Upscaling section (the quality and the DLSS model change live, the upscaler and on / off at the next start). DLSS here is a picture-quality choice, not a speed-up: on an RTX 4060 Laptop it costs about 4-5 ms a frame at 3200x2000 (8.4 ms plain, 12.5-13.5 ms with DLSS; every mode holds 60 fps, none reaches a clean 120) and 20-26 W more GPU power at 60 fps. It needs an RTX GPU and NVIDIA's DLLs next to the game (the setup downloads them); without them, on another GPU, or with `--nvidia_streamline=off`, FSR runs and the log says so once. The DLLs are NVIDIA's development ones: they print a small NVIDIA notice at the bottom of the screen, which is for personal use only and must not be passed on.
- **DLSS Frame Generation** (RTX 40 or newer, Windows' hardware-accelerated GPU scheduling on, G-Sync with **Allow tearing**): the preset **Native panel 3200x2000, 120 fps shown with NVIDIA DLSS Frame Generation** renders 60 frames a second and shows 118 (measured over a 6 minute drive); the game still simulates at 60, the HUD stays sharp, the frame between two rendered ones delays the real one a little (input latency was not measured). FXAA must be off (the preset does it). Only at the display's own size (3200x2000 on the laptop panel); not on the 1080p TV.
- **Anti-aliasing:** automatic by default (FXAA).
- **Vsync and tearing:** vsync is always on, and with tearing off each frame waits for the display's next refresh.
  Tearing is off by default, which is right for TVs and ordinary monitors. The launcher's **Allow tearing (G-Sync /
  FreeSync only)** (Display page, advanced settings) lets a frame reach the screen in the middle of a refresh. Turn it
  on only with a G-Sync or FreeSync display that has variable refresh on in the graphics driver; on any other screen
  it tears. The launcher has no VSync option.
- **HDR output:** off by default. Set it to Auto on the Display page (advanced settings) if Windows HDR is on for your
  screen. It is experimental.
- **Saves:** in `Documents\mcla_x86`. To move them (for example to a synced folder), set "User data folder" on the
  launcher's Paths page.
- **Settings:** the launcher stores them in `%LOCALAPPDATA%\mcla_x86\launcher.json`.

On a laptop: plug it in and select the vendor's performance mode. In a balanced or quiet mode the GPU is much slower
and the game lowers detail to keep 60 fps. The difference can be large: on an RTX 4060 Laptop (Lenovo IdeaPad Slim
Pro 9) the vendor's default mode (Intelligent Cooling, "auto") holds the GPU to 55 W of its 100 W maximum, and its
Extreme Performance mode (Fn+Q, or Lenovo Vantage) lets it draw up to 100 W: 85 fps instead of 106 fps at 3200x2000, a
quarter more, at a louder fan. The game says so at the start: the `[mcla-sys]` lines of its log show the GPU's power
limit against its maximum, the vendor mode and the Windows power mode, and a `WARNING` line appears when the GPU is held
well below its maximum (or the Windows power mode is not Best performance) while a 60 fps or higher target is asked for. The
game only reads these settings and never changes them. On Windows also set the power mode (Settings > System > Power &
battery) to Best performance (it made no measurable difference to the GPU on that laptop, but it is the setting the
warning checks).

**120 fps** (launcher: Frame rate > 120 fps, or `--mcla_fps_target=120`): the same profile on a 1/120 s world step. The display has
to follow: a 120 Hz mode (or a multiple), or variable refresh. A panel that only has 165 and 60 Hz modes, like the RTX 4060
laptop's, shows 120 fps as a beat of 1 and 2 refreshes; with G-Sync on in the driver and the launcher's **Allow tearing**
(Display page, advanced) it follows the game and shows it evenly. The GPU has to render a frame in under 8.3 ms: 1080p ultra
holds 120 fps with room on an RTX 4060 Laptop in its performance mode (the GPU needs 6 ms), 3200x2000 reaches 118 fps only
when adaptive detail drops to its lowest level (without it the GPU needs 9.4 ms and the game runs at about 90 fps and drops to 60
for a while). When frames are missed the game drops to 60 fps by itself. The car's physics runs at a 1/120 s step: how the
handling feels has not been judged yet.

You can also start the game without the launcher:
`mcla.exe --game_data_root=<game folder> --mcla_profile=1080p --mcla_fps_target=60`. Every setting is a
`--name=value` option; the launcher shows the exact command line it uses.

## Known problems

- The `quality` preset stuttered for some minutes at a time in one long session on a Radeon 880M. The cause is not
  known yet. The Recommended preset did not show it.
- HDR has not been judged by eye on an HDR screen yet.
- "CPU boost while playing" (advanced) changes the Windows power plan while the game runs. It is not tested: leave it
  off.
- On a panel whose refresh rate is not a multiple of the frame rate, 60 fps is shown unevenly. On a 165 Hz laptop
  panel each frame stays on the screen for 3, 3, 3 and then 2 refreshes. This is by design, not a fault, and no
  setting in the game removes it. Variable refresh (G-Sync or FreeSync, with **Allow tearing** on) or a 60 or 120 Hz
  mode of the panel does.
- The first frames of menus and loading screens still hitch at each start: the worst frame is 107 to 121 ms on the RTX
  4060 Laptop, down from 167 to 197 ms. What is left is the memory allocation of the first 3D frame and about 7
  compute pipelines a run, which are not stored.
- At 3200x2000 the game still makes some copies that it avoids at 1080p. Direct present stays off, because it leaves a
  one-pixel stripe along the top and left edge. The pause backdrop, the vinyl editor and HDR are not covered by the
  copy shortcuts yet.
- The 60 Hz TV target has been checked on 118-second drives; the 5-minute soak is not done yet.

## Troubleshooting

| Problem | What to do |
|---|---|
| `Missing tools` | Run the commands that setup prints, open a new terminal, run setup again. |
| `this .iso is not the supported dump` | Only the Complete Edition (USA, Europe) works. Check that the file is a full disc image. |
| The SDK build stops with a path-length error | Use a short work folder: `python tools/setup.py --work C:\mcla-work --iso ...` |
| The SDK build runs out of memory | Limit the compiler jobs: add `--jobs 4`. |
| The download stops | Run setup again; it continues where it stopped. |
| Something looks half-done or broken | `python tools/setup.py --force` runs every step again. The SDK build log is in `mcla-x86-work\logs`. |
| The game starts but is slow | Pick a lighter preset. On a laptop, plug it in and use the performance mode. |

## For developers

The steps that `tools/setup.py` runs, if you want to run them yourself (`MCLA_WORK` and `MCLA_GAME` are environment
variables for the work folder and the game folder):

```sh
python tools/xiso.py verify "<your.iso>"                  # is it the supported dump?
python tools/xiso.py extract "<your.iso>" "<game folder>"  # 12 files, 6.1 GiB
python tools/prepare_sdk.py fetch                          # clone the SDK at the pinned commit
python tools/prepare_sdk.py apply                          # apply patches/sdk/series.toml
python tools/prepare_sdk.py build                          # build and install to $MCLA_WORK/sdk/install
python tools/codegen.py                                    # default.xex -> C++ in $MCLA_WORK/generated
cmake --preset local-release && cmake --build --preset local-release
```

`CMakePresets.json` holds the owner's paths; `tools/setup.py` writes a `CMakeUserPresets.json` (not in git) with the
preset `local-release` for your machine. Before your first commit, turn on the guard that keeps game data out of the
repo: `git config core.hooksPath .githooks`.

| Path | What |
|---|---|
| `mcla/codegen/` | Codegen configuration: function hints and the mid-function hook sites (addresses only) |
| `mcla/hooks/` | Game hooks: 60 fps timing, widescreen/1080p/16:10, GPU levers, adaptive detail, input fixes |
| `mcla/profile/` | PC defaults and presets |
| `src/host/` | The Windows host: app, watchdog, frame cap, thread policy, window sizing |
| `patches/sdk/` | The SDK patch series (`series.toml`): Renan's Windows series and this project's patches |
| `launcher/` | The launcher (Dear ImGui + D3D11), driven by `launcher/schema/settings.json` |
| `tools/` | Setup, dump tools, codegen, SDK preparation, run and bench harness, legal guard |
| `tools/elevate/` | Owner-approved elevated command broker for test machines only (`start.ps1`, `broker.ps1`, `elevated.py`) |
| `tools/usage_watch.py` | Reads the Claude 5-hour and weekly usage for the agent work; uses the local Claude Code login and never prints the token |
| `docs/` | Plan, state, learnings, builds and metrics |
| `docs/NATIVE_PLAN.md` | The native PC plan: targets, tracks and the status board |
| `docs/AGENT_SETUP.md` | How the project is worked on by agents: roles, machine rules, briefs |

## Legal

Only this project's own code, configuration, scripts and documentation are in this repository. `.gitignore` and a
pre-commit guard keep the game's files, generated code, builds, logs and dumps out. Third-party credits:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). This is an unofficial fan project; it is not affiliated with
Rockstar San Diego, Rockstar Games, Take-Two, Sony or Microsoft.

Licensed under GPL-3.0, see [LICENSE](LICENSE).
