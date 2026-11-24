# Builds

Owner: VER. One row per build (PLAN §7.4). Logs and artefacts stay in `$WORK`; nothing here is a build output.

## Environment (W0.1, 2026-11-24)

| Item | Value |
|---|---|
| OS | Windows 11 IoT Enterprise LTSC 24H2, 10.0.26100.9974 |
| CPU | AMD Ryzen AI 9 365 w/ Radeon 880M, 10 cores / 20 threads |
| RAM | 33,413,771,264 B (31.1 GiB) usable; 4 × 8 GiB LPDDR5X-7500 |
| GPU | AMD Radeon(TM) 880M Graphics, driver 32.0.32029.2010 (driver date 2026-11-17), `AdapterRAM` 512 MiB (UMA carve-out) |
| Other display adapter | SuperDisplay Virtual Adapter 1.37.24.257 (second screen `\\.\DISPLAY2`, 1920x1080, not primary) |
| Panel mode | `\\.\DISPLAY1` primary, 3200x2000 @ 120 Hz (Win32_VideoController); 150 % scaling (logical 2133x1333) |
| Power | Scheme Balanced `381b4222-f694-41f0-9685-ff5bb260df2e`; AC overlay `ded574b5-45a0-4f42-8737-46345c09c238` (Best performance), DC overlay none; on AC, battery 96 % charging (at 2026-11-24 16:1x) |
| Privileges | Non-elevated shell (Administrators deny-only); member of `Performance Log Users` |
| VS | Visual Studio Community 2022 17.14.13 (17.14.36414.22), `C:/Program Files/Microsoft Visual Studio/2022/Community`, has `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` (vswhere also lists Build Tools 2019 16.11.52) |
| MSVC | 14.44.35207 (default `Microsoft.VCToolsVersion.default.txt`), MSVC STL `_MSVC_STL_UPDATE` 202503 |
| Windows SDK | 10.0.26100.0 (`Include/10.0.26100.0/{ucrt,shared,um,winrt,cppwinrt}`, `Lib/10.0.26100.0/{ucrt,um}`); 10.0.19041.0 also installed |
| CMake | 4.3.2 on PATH (`C:/Program Files/CMake/bin/cmake.exe`); VS-bundled 3.31.6-msvc6 |
| Ninja | 1.12.1, VS-bundled only (`C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`); not on PATH |
| Python / Git | 3.13.7 / 2.51.0.windows.1 |
| Clang | `$WORK/toolchain/llvm-20.1.8/bin/clang++.exe`: `clang version 20.1.8`, `Target: x86_64-pc-windows-msvc`; links with `lld-link` from the same dir |
| PresentMon | `$WORK/toolchain/presentmon/PresentMon-2.6.0-x64.exe` (`PresentMon 2.6.0`) |
| Xenia Canary | `$WORK/toolchain/xenia-canary/xenia_canary.exe`, release `02d2cb5` (`canary_experimental@02d2cb5cc`, 2026-03-24; latest at install time), exe SHA-256 `8946563cae2da4d8f08f7335e6e2627a42ee670231f2f8b8e917fabdcc914675` |

### Downloaded archives (kept in `$WORK/toolchain/dl/`)

| File | Bytes | SHA-256 | Verification |
|---|---|---|---|
| `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz` | 939,541,912 | `f229769f11d6a6edc8ada599c0cda964b7dee6ab1a08c6cf9dd7f513e85b107f` | Matches GitHub asset digest; `gpg --verify` of the release `.sig` with `releases.llvm.org/release-keys.asc`: Good signature, Hans Wennborg, key `B6C8 F982 82B9 44E3 B0D5 C253 0FC3 042E 345A D05D` |
| `PresentMon-2.6.0-x64.exe` | 980,320 | `b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af` | Matches GitHub asset digest |
| `xenia_canary_windows_.zip` | 4,975,811 | `1adbb2c1c69cbd5fabadb43d03b4c407db3b245de922e11aba699867c68d0796` | Matches GitHub asset digest |

### Toolchain notes

- LLVM unpacked with System32 `tar.exe` (bsdtar) in 45 s: 4,730 files + 280 dirs, no symlinks or hardlinks in the
  archive, all files present on disk. 4.0 GB unpacked.
- Smoke test: C++23 hello world (`<print>`, `std::byteswap`, `std::expected`, ranges) built with
  `clang++.exe --target=x86_64-pc-windows-msvc -std=c++23 -O2 -fuse-ld=lld` in 2.0 s and ran (exit 0). Clang found
  MSVC 14.44.35207 and Win SDK 10.0.26100.0 on its own, without a VS developer prompt.
- PresentMon `--help` (also `-?`) prints usage (UTF-16LE) and **exits 1**; PLAN W0.1 acceptance expected exit 0.
  A non-elevated capture (`--process_name x --timed 2 --terminate_after_timed --output_stdout`) runs and exits 0,
  warning that processes from other accounts or short-lived ones show as `<unknown>` without elevation.

## Builds

| id | date | commit | SDK series fingerprint | codegen fingerprint + functions | toolchain | config | wall time | result | artefact path |
|---|---|---|---|---|---|---|---|---|---|
| E2 | 2026-11-24 | Renan `mcla-recomp` @ `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` (not ours) | rexglue-sdk `v0.10.0` + Renan's `patches/` (its own `make_pc.ps1` order) | Renan's manifest: `Codegen summary: 245 written` (11.7 s; no function count printed; 20 `Unexpected float16_4 pack instruction` notes) | Clang 20.1.8 from `$WORK/toolchain/llvm-20.1.8` (junction in `-ToolsDir $WORK/prior-build/renan-tools`), VS-bundled Ninja 1.12.1 (copied into the same dir), CMake 4.3.2, MSVC 14.44 headers/libs, Win SDK 10.0.26100; no Vulkan SDK (declined non-interactively: FSR built for D3D12 only) | `make_pc.ps1 -Toolchain msvc` (default), preset `win-amd64-release`; SDK 859 Ninja steps, game 155 steps; Ninja's default job count (no memory cap applied) | **450 s cold** (try 1: 126 s, tools + SDK clone/submodules, then died at the patch step; try 2 resumed: 324 s = SDK patch+configure (FidelityFX fetch)+build+install ~4 min, ISO extract 14 s, codegen + game build 82 s); peak disk 9.2 GB under the tree (6.2 GB `game/`, 2.6 GB `third_party/`) | pass after 1 env fix: try 1 failed `Get-FileHash : The term 'Get-FileHash' is not recognized` (`make_pc.ps1:156`) because Windows PowerShell 5.1 inherited PowerShell 7's `PSModulePath` from the bash env; fixed by `env -u PSModulePath` (no source change) | `$WORK/prior-build/renan/out/build/win-amd64-release/mcla.exe`; logs `$WORK/logs/E2-build-20261124-162150{,-try2}.log`, `$WORK/prior-build/renan/out/logs/` |
| sdk-1 | 2026-11-24 | `866e95d` (ORC, under the heavy lock) | `b973b0ae1c8e04ed` (`sdk f5337cd: series 79/79 applied, 0 drifted`) | n/a | Clang 20.1.8 (`$WORK/toolchain/llvm-20.1.8`), VS-bundled Ninja 1.12.1, CMake 4.3.2, MSVC 14.44, Win SDK 10.0.26100 | `prepare_sdk.py build`: preset `win-amd64`, Release, Vulkan on, Tracy off, FSR on; install to `$WORK/sdk/install` | ~4 min | pass (M0): install `exit 0`, `rexglue.exe --help` exit 0, log shows `clang version 20.1.8` | `$WORK/sdk/install`; log `$WORK/logs/sdk-build-20261124-171236.log` |
| cg-1 | 2026-11-24 | `866e95d` (ORC) | `b973b0ae1c8e04ed` | baseline `b3ae5b6a…`; full `da9ab3c29a566aa9f5be9a40bc63b953`, `cpp=121 functions=30204 unresolved=0` (missing 0 / new 0 vs baseline); indirect gate `high 0 medium 0` | `$WORK/sdk/install/bin/rexglue.exe` (sdk-1) | `codegen.py --baseline`, then `codegen.py` | 16.1 s / 15.3 s | pass (M1 a, b); 245 then 38 files written; `Unexpected float16_4 pack instruction` notes as in E2 | `$WORK/generated/default`; logs `$WORK/logs/codegen-20261124-171644.log` (baseline), `codegen-20261124-171704.log` (full) |
| b1.1 | 2026-11-24 | `866e95d` | `b973b0ae1c8e04ed` | `da9ab3c29a566aa9f5be9a40bc63b953`, 30204 functions | Clang 20.1.8 + lld-link, VS-bundled Ninja 1.12.1, CMake 4.3.2 | preset `win-amd64-relwithdebinfo` (Ninja Multi-Config, RelWithDebInfo), cold build tree; recomp job pool 17 (configure saw 10988 MiB free); configure picked `mcla: ReXGlue SDK 0.10.0 at $WORK/sdk/install (series b973b0ae1c8e04ed/79patches)` | 62 s build (178 steps) + 5 s configure | pass (M1 c, d): exit 0, 0 compiler warnings; `ctest -L unit` `100% tests passed, 0 tests failed out of 3` (sim_math 1113 checks, game_state 103, cadence 49); 56/56 `mcla_hook_*` references resolved by strong defs from `mcla/hooks` objects | `$WORK/build/bin/RelWithDebInfo/` (`mcla.exe` 38.1 MB + `mcla.pdb`, `rexruntime.dll`, `rexgpu-xenos.dll`, `amd_fidelityfx_dx12.dll`); logs `$WORK/logs/G1-configure.log`, `G1-build-rwdi.log`, `G1-ctest.log` |
| b1.2 | 2026-11-24 | `866e95d` | `b973b0ae1c8e04ed` | `da9ab3c29a566aa9f5be9a40bc63b953`, 30204 functions | as b1.1 | preset `win-amd64-release` (same tree, Release), cold for this config | 68 s build (178 steps); system-wide min available RAM 7200 of 31865 MiB during the build (other processes included) | pass (M1 c): exit 0, 0 compiler warnings; imports `rexruntime.dll`; 56/56 hook refs resolved | `$WORK/build/bin/Release/` (`mcla.exe` 38.2 MB, same three DLLs); log `$WORK/logs/G1-build-release.log` |
