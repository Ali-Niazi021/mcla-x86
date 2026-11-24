# MCLA x86-64 Windows port: state

Owner: ORC. The plan is [PLAN.md](PLAN.md); this file tracks where execution stands. Newest facts win.

## Environment

| Item | Value |
|---|---|
| Repo | `C:/Users/Ali/Development/mcla-x86` (`Ali-Niazi021/mcla-x86`, `main`) |
| `$IOS` | `C:/Users/Ali/Development/mcla-ios-ref` (read-only; HEAD `e24c575`) |
| `$WORK` | `C:/Users/Ali/Development/mcla-x86-work` (created W0.0) |
| `$GAME` | `C:/Users/Ali/Development/dumps/mcla-x360` (ISO + `default.xex` copy) |
| Game | Complete Edition (USA, Europe), redump 64472, title `545407F8`, media `5940C9DB`, v0.0.0.8, no TU |
| `default.xex` | 9,252,864 B, SHA-256 `c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432` |
| ISO | 7,838,695,424 B, XGD2, game partition `0x0FD90000` |
| Game files | 12 files, 6,561,615,524 B → `$GAME/game/` (G0) |
| Machine | Ryzen AI 9 365 (20 threads), 31.1 GB RAM, Radeon 880M (driver 32.0.32029.2010), panel 3200x2000 @ 120 Hz |
| Disk | C: ~209 GB free (2026-11-24) |
| VS | 2022 Community 17.14.13 (MSVC 14.44.35207), Windows SDK 10.0.26100 |
| CMake / Ninja / Python | 4.3.2 / 1.12.1 (VS-bundled) / 3.13.7 |
| Clang | LLVM 20.1.8 portable → `$WORK/toolchain/llvm-20.1.8` (W0.1) |
| SDK pin | ReXGlue v0.10.0 = `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` |
| Patch source | RenanQueiroz/mcla-recomp v0.3.17 = `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` |

## Current phase

Wave 3 (M4 locked 30, timing infrastructure, CPU levers), with the 1080p work (W5.1/W5.2) pulled forward after D1.

## Milestones

| ID | Goal | Status | Date |
|---|---|---|---|
| G0 | Game dump gate | **pass**: `OK default.xex sha256=c386f400…a432 title=545407F8 media=5940C9DB version=8 disc=1/1`; `extracted 12 files, 6561615524 bytes` → `$GAME/game` | 2026-11-24 |
| M0 | SDK builds | **pass**: `series 79/79 applied, 0 drifted`, fingerprint `b973b0ae1c8e04ed`, built with clang 20.1.8 in 4 min; `rexglue.exe --help` exit 0 | 2026-11-24 |
| M1 | Codegen clean, exe links, tests pass | **pass**: `codegen ok: cpp=121 functions=30204 unresolved=0 fingerprint=da9ab3c2…`; `gate: high 0 medium 0`; RelWithDebInfo 62 s, Release 68 s, 0 warnings; `100% tests passed` (3/3) | 2026-11-24 |
| M2 | Title screen | **pass** (b2.1, D3D12): `first engine frame` t+4.4 s, `Startup/Startup_Checks` t+30.6 s, 13/13 profile cvars, `fatal=0 unregistered=0`; title visible; audio endpoint opened. Owner check (audible music/SFX) pending | 2026-11-24 |
| M3 | Free-roam driving, 10 min soak | **pass** (b2.1): new profile → `cutscene -> free-roam` 136.9 s, save-backup lines ×2, drove 52 mph, pause open/close; fixture `after_intro`; 600 s soak `fatal=0` (run w2.1-m3c); parity with E2: cars coloured, no missing geometry/flashes. Owner check (radio balance) pending | 2026-11-24 |
| M4 | Locked 30 fps at 720p | todo | |
| M5 | 60 fps at 720p | todo | |
| M6 | 60 fps at 1080p on the 880M | todo | |
| M7 | Polish | todo | |

## Task board

### Doing

- W3.3 HOST+GAME: host frame cap `mcla_fps_cap`, paced at `mcla_hook_present_interval`.
- W5.1 GAME: W x H widescreen (`mcla_scene_height`, up to 1920x1080), EDRAM-limit hook body, 1080p/900p profiles.
- TOOLS fix: flow re-timing for x86, PrintWindow screenshots, keep display awake.

### Todo

- SDK rebuild with 0400/0410/0420 (series 82, fingerprint `cfeae5d95249d70a`), then W3.4 VER: M4 bench + CPU-lever A/B.
- CODEGEN: TOML entry for the EDRAM-limit hook at `0x82410E34` (spec from W5.1).
- GAME (low): skip-intro hook causes 3 boot-time `Resolve region is empty` / `PM4_DRAW_INDX_2 … Failed in backend` errors.
- W0.8 E1 Xenia baseline: waits for Ali.

### Done

- W0.0 ORC: `$WORK` tree, PORT_STATE/LEARNINGS skeletons, role files in `.omp/agents/` (local only).
- W0.1 VER: LLVM 20.1.8 (gpg-verified), PresentMon 2.6.0, Xenia Canary `02d2cb5` in `$WORK/toolchain`; env in BUILDS.md.
- W0.2 TOOLS: `xiso.py`, `guard_no_game_data.py` + `.githooks/pre-commit` (`core.hooksPath` set), `heavy_lock.py`.
- W2.1 VER: M2 + M3 pass (runs w2.1-m2a/m2b/m3a/m3b/m3c); fixture `$WORK/fixtures/after_intro`.
- W2.2 TOOLS: `bench.py` (PresentMon by PID, QPC alignment, `--gate m4|m5`).
- W3.2/W5.2 SDK: patches 0400 (precise guest vblank), 0410 (`nfsmw_cp_event_seq` export), 0420 (MmQueryAllocationSize).
- W0.3 RES: Renan's 75 apply clean in `make_pc.ps1` order; drop 3; 7 owner patches apply on top
  (`$WORK/scratch/W0.3/REPORT.md`, `owner-patches/`).
- W0.4 RES: PC cvar table, host responsibilities, hook overlap (`$WORK/scratch/W0.4/REPORT.md`).
- W0.5 CODEGEN: 8 TOMLs, `codegen.py`, `tools/re/*`; `check_codegen_toml` → `4961 entries, 0 duplicates`;
  `default.pe` SHA-256 `5a9de5cd05f32ae66d2a7fd3b09a569e18239ffa258bb1a0efb9126de1cff319`.
- W0.6 TESTS: 3 tests + CTest; pass in scratch with Clang 20.1.8 (1113 + 103 + 49 checks).
- W1.1 SDK: `series.toml` (72 Renan + 7 local), `prepare_sdk.py`; fetch+apply → `series 79/79 applied, 0 drifted`,
  fingerprint `b973b0ae1c8e04ed`.
- W1.2 BUILD+HOST: presets, CMake, `src/host` (scripted pad is a synthetic `InputDriver`: SDL3 is static and private
  inside `rexruntime.dll`).
- W1.3 GAME: 56/56 hook symbols defined with the declared signatures; PC profile (13 cvars).
- W1.4 TOOLS: `run_game.py`, 5 flows.
- W0.7 VER (E2): Renan's build, cold 450 s / 9.2 GB. **D1 fails** (see Decisions).
- G0: pass (see Milestones).

## Ownership in force

PLAN §7.2. Shared contracts for wave 1:
- GAME owns `mcla/hooks/nfsc_progress.h` (heartbeat counter; the host watchdog reads it) and
  `mcla/profile/pc_profile.h` (`mcla::ApplyPcProfileLogging()` in `OnConfigurePaths`, `mcla::ApplyPcProfile()` in
  `OnPreSetup` after the explicit GPU plugin load). HOST calls them.
- GAME's `mcla/sources.cmake` sets `MCLA_HOOK_SOURCES`, `MCLA_HOOK_DEFINITIONS`, `MCLA_HOOK_INCLUDE_DIRS`; BUILD
  compiles the hook sources into the exe as objects (strong hook symbols must win over generated weak defaults).
- The exe lands at `$WORK/build/bin/<Config>/mcla.exe`.

## Next actions

1. G1 (VER) once the SDK build and REV are in.
2. Wave 2 (M2/M3) with W2.2 bench port in parallel. Because D1 failed, pull GPU levers (W5.2–W5.4) into wave 3.

## Decisions changed since PLAN

- **Role files live in `.omp/agents/`, not `.claude/agents/`.** The harness is omp, which loads project agents from
  `.omp/agents/*.md` (it skips `.claude/agents`) and rediscovers them at every spawn, so no session restart is needed.
  Models: IMP `anthropic/claude-sonnet-5-5` xhigh; RES/REV/VER `anthropic/claude-opus-5-5` medium. Gitignored.
- **SDK series (W0.3):** Renan's 75 in `make_pc.ps1` order minus `ffmpeg-ps5-config`, `mingw`, `ffmpeg-mingw-config`
  (drops proven apply-independent), then owner `0001` gapfill, `0100` register table, `0205` dirty-disc log flush, `0211`
  hex cvars, `0300` stack top, `0310` XMA cross-buffer packets (new rebase), `0320` Vulkan no-draw-in-transfer (rebase).
- **Cvar timing (W0.4):** GPU cvars (`edram_tiles`, `render_target_max_height`, …) are read when graphics starts, before
  `OnPostSetup`. The host loads the plugin itself in `OnPreSetup` (`LoadGpuPlugin("xenos", backend)`) and applies the
  profile there with `rex::cvar::SetDefaultValue` (Renan's `cvar-toml-strings` patch). Log cvars go in
  `OnConfigurePaths`. Supersedes the PLAN "Known traps" line about `OnPostSetup`.
- **App name `mcla_x86`** (toml, Documents folder, log names), so saves never mix with Renan's `mcla` (E2). The
  generated identity stays `nfsmw`.
- **No GPU-backend cvar in the SDK:** host cvar `mcla_gpu_backend` (`d3d12` default, `vulkan`).
- **`vsync` stays true.** `vsync=false` makes the guest vblank fire at ~1 kHz.
- **Tearing cvar** is `d3d12_allow_variable_refresh_rate_and_tearing` (default on). **Guest vblank rate** cvar is
  `video_mode_refresh_rate` (no `guest_vblank_rate`). **Video mode**: `resolution=WxH`.
- **`nfsmw_cp_event_seq` export lives in `rexgpu-xenos.dll`** (the command processor is in the GPU plugin), not
  `rexruntime.dll`. W3.2 exports it; `mcla_fence_spin` resolves it there with a bounded fallback.
- **`0x82410E34` EDRAM limit** is Renan's `mcla_edram_limit` hook; the owner has none. W5.1 re-implements it.
- **LARecomp-derived fixes inside Renan's game layer** (car-shader-bind, warp, checkpoint) are re-implemented from
  addresses if needed, never copied.
- **`run_game.py` passes** `--cache_root=$WORK/user/cache` so shader storage survives fresh per-run user dirs.
- **D1 (E2) failed: the 880M is GPU-bound.** Renan's build: D3D12 720p uncapped 69.6 fps (GPU 100 %), Vulkan 58.1;
  1440p (2x) 24.8, FSR-1080p 22.2; 720p cap 60 holds (p99 17.9 ms, GPU 87 %). Per PLAN D1: GPU levers (W5.2–W5.4,
  `mcla_noaa` first) move into wave 3, and PLAN §3.8 fallback 1 (1600x900 + FSR1/CAS) is the expected 1080p path.
  True 1920x1080 stays the first thing measured in E4, with `noaa`.
- **Default API: D3D12** (beat Vulkan by 20 % at 720p in E2, against PLAN §3.5's Vulkan prior). E3 still runs at M5.
- **Scripted pad = synthetic `rex::input::InputDriver`** (SDL3 lives statically inside `rexruntime.dll` with no
  exports), not an SDL virtual joystick (PLAN §3.6, §4.1).
