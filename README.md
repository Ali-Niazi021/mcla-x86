# Midnight Club: Los Angeles, recompiled for x86-64 Windows

**Midnight Club: Los Angeles (Xbox 360, Complete Edition), statically recompiled to a native 64-bit Windows program.**
It is built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), a patch series on top of it, and a layer
of game hooks (60 fps, widescreen, 1080p and more, a launcher, HDR output).

Status: the game boots, drives, races and saves. On a Radeon 880M laptop it runs at 1080p and 60 fps with the
Recommended preset. Details and measurements are in [docs/PORT_STATE.md](docs/PORT_STATE.md).

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
- **Presets:** Recommended 1080p, 16:10 panel (1920x1200), Balanced 900p, Performance 720p, native panel, and a
  console-like 30 fps mode. If the game does not hold 60 fps, pick a lighter preset.
- **Adaptive detail** (on by default): keeps 60 fps by lowering detail where it is least visible, and raises it again
  when the GPU has room.
- **Anti-aliasing:** automatic by default (FXAA).
- **HDR output:** off by default. Set it to Auto on the Display page (advanced settings) if Windows HDR is on for your
  screen. It is experimental.
- **Saves:** in `Documents\mcla_x86`. To move them (for example to a synced folder), set "User data folder" on the
  launcher's Paths page.
- **Settings:** the launcher stores them in `%LOCALAPPDATA%\mcla_x86\launcher.json`.

On a laptop: plug it in and select the vendor's performance mode. In a balanced or quiet mode the GPU is much slower
and the game lowers detail to keep 60 fps.

You can also start the game without the launcher:
`mcla.exe --game_data_root=<game folder> --mcla_profile=1080p --mcla_fps_target=60`. Every setting is a
`--name=value` option; the launcher shows the exact command line it uses.

## Known problems

- The `quality` preset stuttered for some minutes at a time in one long session on a Radeon 880M. The cause is not
  known yet. The Recommended preset did not show it.
- HDR has not been judged by eye on an HDR screen yet.
- "CPU boost while playing" (advanced) changes the Windows power plan while the game runs. It is not tested: leave it
  off.

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
| `docs/` | Plan, state, learnings, builds and metrics |

## Legal

Only this project's own code, configuration, scripts and documentation are in this repository. `.gitignore` and a
pre-commit guard keep the game's files, generated code, builds, logs and dumps out. Third-party credits:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). This is an unofficial fan project; it is not affiliated with
Rockstar San Diego, Rockstar Games, Take-Two, Sony or Microsoft.

Licensed under GPL-3.0, see [LICENSE](LICENSE).
