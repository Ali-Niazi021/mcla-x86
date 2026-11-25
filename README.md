# Midnight Club: Los Angeles, recompiled for x86-64 Windows

**Midnight Club: Los Angeles (Xbox 360, Complete Edition), statically recompiled to native x86-64 Windows** with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), a patch series on top of it, and a game layer of hooks shared in
spirit with the owner's iOS port. It boots, plays (free roam, races, saves) and runs at 60 fps on a Radeon 880M laptop
at 720p–900p; 1080p is close (see [docs/PORT_STATE.md](docs/PORT_STATE.md) for the current numbers).

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

Start **`mcla_launcher.exe`**: pick the game folder (the extracted files), a preset (Recommended 1080p, Balanced 900p 60,
Performance 720p 60, native 16:10 panel, or console-like 30), adjust graphics, performance, gameplay and controls, and
press **Launch**. Settings are saved in `%LOCALAPPDATA%/mcla_x86/launcher.json`. Controllers: XInput/Xbox, DualShock 4
and DualSense (through SDL), plus keyboard.

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
