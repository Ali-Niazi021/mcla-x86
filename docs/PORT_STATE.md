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

**Wrapped up for now (2026-11-25).** Playable on the Radeon 880M laptop: 720p60 and 30 fps modes pass their gates;
1080p and 1920x1200 run at 60 fps most of the time, with a 6–18 s hold at 30 fps in the heaviest streets in about half
of the 5-minute bench drives. Native 3200x2000 exists (`mcla_profile=2000p`, 23–26 fps). Launcher done. Resume from the
task board's Todo list.

## Milestones

| ID | Goal | Status | Date |
|---|---|---|---|
| G0 | Game dump gate | **pass**: `OK default.xex sha256=c386f400…a432 title=545407F8 media=5940C9DB version=8 disc=1/1`; `extracted 12 files, 6561615524 bytes` → `$GAME/game` | 2026-11-24 |
| M0 | SDK builds | **pass**: `series 79/79 applied, 0 drifted`, fingerprint `b973b0ae1c8e04ed`, built with clang 20.1.8 in 4 min; `rexglue.exe --help` exit 0 | 2026-11-24 |
| M1 | Codegen clean, exe links, tests pass | **pass**: `codegen ok: cpp=121 functions=30204 unresolved=0 fingerprint=da9ab3c2…`; `gate: high 0 medium 0`; RelWithDebInfo 62 s, Release 68 s, 0 warnings; `100% tests passed` (3/3) | 2026-11-24 |
| M2 | Title screen | **pass** (b2.1, D3D12): `first engine frame` t+4.4 s, `Startup/Startup_Checks` t+30.6 s, 13/13 profile cvars, `fatal=0 unregistered=0`; title visible; audio endpoint opened. Owner check (audible music/SFX) pending | 2026-11-24 |
| M3 | Free-roam driving, 10 min soak | **pass** (b2.1): new profile → `cutscene -> free-roam` 136.9 s, save-backup lines ×2, drove 52 mph, pause open/close; fixture `after_intro`; 600 s soak `fatal=0` (run w2.1-m3c); parity with E2: cars coloured, no missing geometry/flashes. Owner check (radio balance) pending | 2026-11-24 |
| M4 | Locked 30 fps at 720p | **pass** (V4m4, series 94): avg 30.00, p99 33.89 (≤35), max 39.2, world/real 1.000 | 2026-11-25 |
| M5 | 60 fps at 720p | **pass** (V4m5): avg 60.00, p99 17.40, 0.00 % > 25 ms, max 26.7; GPU p50 10.6 ms | 2026-11-25 |
| M6 | 60 fps at 1080p on the 880M | **near**: native 1080p single tile with the defaults + `mcla_gfx_preset=perf` (MyASUS Performance mode, ~45 W package, borderless fullscreen = Hardware: Independent Flip). Parked: 60.00 fps, p99 17.1–17.3 ms. Drive: bimodal. 3 runs clean (orc-ab-*: 59.97–60.00 fps, p99 17.3–17.7, ≤ 0.06 % > 25 ms); 4 runs with one 60 → 30 hold episode of 6 + 12 s (V5m6fs, V5m6u: 58.0 fps, 3.4–3.7 % > 25 ms; gate FAIL). 1920x1200 measures the same. The earlier V4m6 gates (p99 20–22, 0.07 % > 25 ms) ran with the flickering preset. Upscaling loses here (FSR 3 49 fps, FSR 4 37 fps) | |
| Launcher | Settings UI + Launch | **done**: `mcla_launcher.exe` (schema-driven, presets, validation); end-to-end launch to the title (V4-launcher) | 2026-11-25 |

## Task board

### Doing

(nothing)

### Todo (next actions, in order)

1. **M6: the 60 → 30 hold decides it** (`mcla_t_common.cpp` `DecideHold`). In the bad runs a 2 s window with 18–55 %
   missed frames (GPU 88–91 % busy at 2.7–2.9 GHz, no clock dip) starts a 6 s hold; 2 s after the hold ends the next
   window reads 55–100 % missed and a 12 s hold follows (3 of 5 holds). Suspect: the GPU clock falls to ~2.1 GHz at 50 %
   load during the hold and is not back when 60 resumes, so the first window after a hold should not be judged (a grace
   window), or the return should be probed. Then re-gate; try `mcla_sim_hold30_s=0` as the A/B. Runs: `V5m6fs-m6fs`,
   `V5m6u-m6`, `V5m6u-p1200`, `orc-fix-p1080` (holds), `orc-ab-*` (none). Note: `bench.py` gates run windowed
   (`run_game.py` sets `fullscreen=false` = Composed: Flip on this desktop); add `--cvar fullscreen=true`.
   Other GPU candidates as before: SDK 04e0 tone-map transfer skip (needs a game run with `tonemap_transfer_skip=true,
   tonemap_transfer_skip_poison=true`, ~0.55 ms), 04a0's fused gamma paint (refused while a UI overlay is registered),
   R1-E texture fast path, R2 full-resource clears, the `dxbc_ps_ieee_mul` white-frame question (V3).
2. **60 fps gameplay fixes not yet done.** LARecomp-derived in Renan's port, so re-derive from the PPC:
   - **Warp hang:** override `sub_8221BE98`; it calls `sub_8221B790`/`sub_8221B8C8`. The warper state is at
     `*(0x8286D8D4)+0x51240`, with the state word at +288.
   - **Checkpoint sweep:** override `sub_822653A8`, plus a mid-asm hook at `0x82688200` in `sub_826881A8` (`r1`, `r31`,
     `r27`).
   - **Bike gyro:** `sub_823688B8`, constants at `0x8236895C` (`f13`, 0.40) and `0x823689F0` (`f0`, 0.15), with
     k(dt) = 1-(1-k30)^(30dt).
3. **Owner checks:** audio (music, SFX, radio balance), controllers by hand (XInput/Xbox, DS4, DualSense), races.
4. GAME (low): the skip-intro hook causes 3 boot-time `Resolve region is empty` errors.
5. W0.8 E1 Xenia baseline (optional).

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

See "Todo (next actions, in order)" in the task board. Note for resuming: the SDK install at `$WORK/sdk/install` is
series 94 (`fd9b4e47d09a0e4f`); the repo's series has a newer 04e0 (fingerprint `7bcdf49bed3a73c8`), so run
`python tools/prepare_sdk.py reset` + `build` before the next game build. Codegen is current (`7c0b18d7…`, 30204
functions). Role files are in `.omp/agents/` (local, gitignored). Every heavy job goes through `tools/heavy_lock.py`.

## Sprint findings (2026-11-25)

Reports in `$WORK/scratch/{S2,S3,S4,S5,R2,R3,R4,R5,R6}/REPORT.md`.
- **Upscaling loses on the 880M.** The FSR 3.1 pass costs 4.1 ms, FSR 4.1 6.4 ms, against ~3 ms saved by drawing the scene
  smaller. Native 1080p single tile is the fastest 1080p.
- **Power, not compute, is the wall.** The package sits at ~35 W; the GPU runs ~1.25–1.6 GHz (2.9 GHz boost) and drops
  further when other CPU work runs (agent builds, Defender). Benches need a quiet machine (heavy lock for every
  CPU-heavy job; `prepare_sdk.py build` now always takes the global lock) and telemetry in each row.
- **8–13 ms of the GPU frame doesn't scale with the shader clock** (R6): bandwidth- or serialization-bound emulation work
  (resolves + texture reloads ~140 MB/frame, EDRAM transfers, swap path). Levers in flight: 0490 dual-write resolve
  (−0.8–1.1 ms est.), swap-direct-present (−0.2–0.35), quad lists without the GS, `clear_memory_page_state=false`.
- **The `perf` scene-detail preset** is: car reflections and shadow cascade 2 at half rate, shadow distance 0.6, LODs,
  shadow map 512. The two half-rate knobs flickered until the phase's own object was skipped too (LEARNINGS,
  Owner-report); they are worth ~5 points of GPU load at 1080p (81.5 → 76.5 % busy). Cascade 3 cannot be skipped (its
  end call draws the player's car body path); `mcla_gfx_impostor_interval` holds nothing back. `perf+` (impostors off)
  is slower; `skip_overwritten_transfers=true` costs +6 ms (do not ship).
- **Flicker test:** `$WORK/scratch/ORC/burst.py` + `flicker_ab.py` (24 window captures in 1 s of the parked scene;
  reference 0.2–0.4 % of pixels with a temporal std > 6). An empty `--mcla_gfx_preset=` swallows the next argument.
- **Native 3200x2000:** `mcla_profile=2000p` = the 1600x1000 guest at `draw_resolution_scale` 2x2: correct picture,
  23–26 fps, GPU 100 % busy; dual-write resolves and the direct present fall back at a scale above 1.
- **Fullscreen is Hardware: Independent Flip** now that the SuperDisplay virtual display is detached; a window is
  Composed: Flip (the desktop is 3200x2000).

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
- **Owner decisions (Ali, 2026-11-24):** anti-aliasing = `noaa` (stable frame times over MSAA). M6 = 720p internal
  upscaled to 1080p with **FSR 3** (the most advanced upscaler available), after one more native-1080p60 check on the
  new, stronger power adapter (report back if the result changes). Controllers: generic XInput, Xbox, DualShock 4,
  DualSense.
- **Present jitter fix (0430) and display-locked vblank (0440)**: the M4 p99 miss was the CP's `Sleep(1)` swap-gate poll.
- **60 fps lever = present-immediate hook** (`mcla_present_immediate=-1`, 0x82419AC0): the guest D3D's
  D3DRS_PRESENTIMMEDIATETHRESHOLD is 0, so a late frame was held one whole vblank (33 ms). Raising it to 100 on timed
  swaps releases late frames at once and keeps the real 60 Hz vblank. Renan's alternative (`vsync=false`, free-running
  vblank) is not used.
