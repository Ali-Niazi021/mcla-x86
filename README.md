# Midnight Club: Los Angeles, recompiled for x86-64 Windows

**Midnight Club: Los Angeles (Xbox 360, Complete Edition), statically recompiled to native x86-64 Windows** with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), a patch series on top of it, and a game layer of hooks shared in
spirit with the owner's iOS port. It boots, reaches free roam, drives and saves (races not yet tested). On a Radeon 880M
laptop it holds 60 fps at 720p. At 1080p it held 60 fps in one-minute test runs when little else was running (60.00 fps average, 99th percentile
17.3 to 22.7 ms, 0.05 % of frames over 25 ms); with other programs busy on the machine 1.25 % of frames ran late; the 5-minute fullscreen drives have not been re-run since the latest changes, and
before them 1080p could drop to 30 fps for several seconds in the heaviest streets. Adaptive detail (on by default at 60
fps) keeps the frame rate by lowering detail where it is least visible and raising it again when the GPU has room, FXAA
is part of the default picture at 60 fps, and the pause menu runs at about 55 fps instead of 27. HDR output is available
through the launcher's Auto setting; it has not been judged by eye on an HDR screen yet. A 30 fps console-like mode is
also available (numbers in [docs/PORT_STATE.md](docs/PORT_STATE.md)).

> **You need your own copy of the game.** This repository contains no game files and no translated game code.
> Everything is read from your own dump of a disc you own. Do not ask for, or share, game files here.

Supported dump: Midnight Club: Los Angeles — Complete Edition (USA, Europe), title `545407F8`, media `5940C9DB`,
`default.xex` SHA-256 `c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432`. Other editions are different
builds and won't work.

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 17.14 (C++ workload, Windows SDK 10.0.26100), CMake ≥ 3.25,
Python 3.13, Git, and the portable LLVM 20.1.8 (`clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz`). Everything heavy
lives in a work tree outside the repo, `$MCLA_WORK` (default `C:/Users/Ali/Development/mcla-x86-work`; see
[docs/PLAN.md](docs/PLAN.md) §5). Put LLVM in `$MCLA_WORK/toolchain/llvm-20.1.8`.

```sh
python tools/xiso.py verify "<your.iso>"                       # checks it is the supported dump
python tools/xiso.py extract "<your.iso>" "<game folder>"       # 12 files, ~6.1 GiB
python tools/prepare_sdk.py fetch && python tools/prepare_sdk.py apply && python tools/prepare_sdk.py build
python tools/re/xex_image.py                                    # decrypted image for the tools ($MCLA_WORK/re)
python tools/codegen.py                                         # recompiles default.xex into $MCLA_WORK/generated
cmake --preset win-amd64-release && cmake --build --preset win-amd64-release
```

The game and the launcher land in `$MCLA_WORK/build/bin/Release/` (`mcla.exe`, `mcla_launcher.exe`, the SDK DLLs).

## Play

Start **`mcla_launcher.exe`**: pick the game folder (the extracted files), a preset (Recommended 1080p, 16:10 panel 60
(1920x1200), Balanced 900p 60, Performance 720p 60, native panel 3200x2000 at about 25 fps, or console-like 30), adjust
graphics, performance, gameplay and controls, and press **Launch**. Settings are saved in
`%LOCALAPPDATA%/mcla_x86/launcher.json`. Controllers: XInput/Xbox, DualShock 4 and DualSense (through SDL), plus
keyboard.

At the 60 fps presets the settings that matter most are on the Graphics and Performance pages:
- **Adaptive detail** (on by default): keeps 60 fps by giving back extra detail first, then lowering the shading rate
  where you cannot see it, and only last the visible detail; it raises detail again, one step at a time, when there is
  headroom, up to the ceiling you choose. Settings you change yourself are left alone.
- **Anti-aliasing**: automatic by default (FXAA is on with Adaptive detail); or choose Off or FXAA yourself. It costs
  about 0.5 ms of GPU time at 1080p on a Radeon 880M.
- **HDR output** (Display, advanced, experimental, off by default): Auto uses HDR when Windows HDR is on for the display
  (Settings > System > Display > Use HDR). Fused HDR costs about 0.7 ms of GPU time at 1080p; the picture has not been
  judged by eye yet.
- **CPU boost while playing** (advanced, off by default) changes the active Windows power plan while the game runs and
  puts it back afterwards; only a dry run of it has been done, so leave it off for now.

Or run the game directly: `mcla.exe --game_data_root=<game folder> --mcla_profile=1080p --mcla_fps_target=60`
(every setting is a `--name=value` command-line option; the launcher shows the exact command line it uses).

## Layout

| Path | What |
|---|---|
| `mcla/codegen/` | Codegen configuration: function hints and the mid-function hook sites (addresses only) |
| `mcla/hooks/` | Game hooks: 60 fps timing, widescreen/1080p/16:10, GPU levers, input fixes |
| `mcla/profile/` | PC defaults and presets (`mcla_profile`, `mcla_fps_target`, `mcla_gfx_preset`) |
| `src/host/` | The Windows host: app, watchdog, frame cap, scripted input, window sizing |
| `patches/sdk/` | The SDK patch series (`series.toml`): Renan's Windows series + our own patches |
| `launcher/` | The launcher (Dear ImGui + D3D11), driven by `launcher/schema/settings.json` |
| `tools/` | Dump tools, codegen, SDK preparation, run/bench harness, legal guard |
| `docs/` | Plan, state, learnings, builds and metrics |

## Legal

Only this project's own code, configuration, scripts and documentation live in this repository. `.gitignore` and a
pre-commit guard keep the game's files, generated code, builds, logs and dumps out. Third-party credits:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Unofficial fan project; not affiliated with Rockstar San Diego,
Rockstar Games, Take-Two, Sony or Microsoft.

Licensed under GPL-3.0, see [LICENSE](LICENSE).
