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

**1080p60 sprint + launcher (2026-11-25).** Swarm: VER S1 benching, P1 GPU profiling (RGP + GPU timestamps), R1 shader
translator study, O2 swap-path patch, TOOLS bench telemetry. Launcher (`launcher/`) built and reviewed.

## Milestones

| ID | Goal | Status | Date |
|---|---|---|---|
| G0 | Game dump gate | **pass**: `OK default.xex sha256=c386f400…a432 title=545407F8 media=5940C9DB version=8 disc=1/1`; `extracted 12 files, 6561615524 bytes` → `$GAME/game` | 2026-11-24 |
| M0 | SDK builds | **pass**: `series 79/79 applied, 0 drifted`, fingerprint `b973b0ae1c8e04ed`, built with clang 20.1.8 in 4 min; `rexglue.exe --help` exit 0 | 2026-11-24 |
| M1 | Codegen clean, exe links, tests pass | **pass**: `codegen ok: cpp=121 functions=30204 unresolved=0 fingerprint=da9ab3c2…`; `gate: high 0 medium 0`; RelWithDebInfo 62 s, Release 68 s, 0 warnings; `100% tests passed` (3/3) | 2026-11-24 |
| M2 | Title screen | **pass** (b2.1, D3D12): `first engine frame` t+4.4 s, `Startup/Startup_Checks` t+30.6 s, 13/13 profile cvars, `fatal=0 unregistered=0`; title visible; audio endpoint opened. Owner check (audible music/SFX) pending | 2026-11-24 |
| M3 | Free-roam driving, 10 min soak | **pass** (b2.1): new profile → `cutscene -> free-roam` 136.9 s, save-backup lines ×2, drove 52 mph, pause open/close; fixture `after_intro`; 600 s soak `fatal=0` (run w2.1-m3c); parity with E2: cars coloured, no missing geometry/flashes. Owner check (radio balance) pending | 2026-11-24 |
| M4 | Locked 30 fps at 720p | **open**: run w3.4-m4 met avg 30.00 and world/real 1.0004 but p99 was 35.1–35.2 ms (limit 35.0). Root cause: the CP's `Sleep(1)` swap-gate poll. Fixed by SDK patch 0430 (series 84), not yet re-gated | |
| M5 | 60 fps at 720p | **close, not met**: `mcla_fps_target=60,mcla_present_immediate=-1,mcla_ws_single_tile=true` gives a stable run (w4.2-m5g-immst-stable) of avg 59.42, p99 25.25, world/real 0.9998 (bar: ≥59.5, ≤20). The run without the hold (w4.2-m5m2-immst) gets 60.00 / p99 22.66 | |
| M6 | 60 fps at 1080p on the 880M | **open**: native 1080p single tile 55.3–56.7 fps (GPU ms p50 17.1–17.8); 900p single tile 59.4; FSR 3 (720p→1080p) 49.3 (pass 4.1 ms), FSR 4 (540p→1080p) 37.5 (pass 6.4 ms): **upscaling is slower than native on the 880M**. 16:10 1280x800 exclusive 57.2. Package power-capped at ~35 W, GPU at ~1.4 GHz of 2.9 GHz; ~8–13 ms of the GPU frame doesn't scale with clock (R6) | |
| M7 | Polish | todo | |

## Task board

### Doing

- Optimization round (see "Sprint findings" below): S1 VER benches, P1 profile, R1 shaders, O2 swap path.

### Todo (next actions, in order)

1. **Owner's quick 1080p60 test on the new power adapter**, reporting back to Ali if 1080p60 changed. This was interrupted
   by the pause. Build at HEAD (`python tools/codegen.py`, then a Release rebuild), then
   `python tools/bench.py --flow bench_drive --fixture after_intro --seconds 240 --no-warmup` with these variants:
   - `n1080:mcla_profile=1080p,mcla_fps_target=60,mcla_present_immediate=-1,mcla_sim_hold30_s=0`
   - `n1080st` (the same, plus `mcla_ws_single_tile=true`)
   - `n1080fsr:mcla_profile=1080p-fsr,…`
   - `fsr1:mcla_profile=720p-fsr1,…`
   - `immnoaa` (the 720p reference)

   Record the power adapter in the notes.
2. **M4 re-gate on series 84** (0430/0440): `bench.py … --parked --gate m4 --variant sim30:mcla_sim60=true,mcla_sim_cap=30,mcla_fps_cap=30`.
3. **M5 closing:** the p99 tail and the 60→30 hold drops. Check the census (`[mcla-swap]`) and the `[mcla-input-fix]` lines
   (the input-fix hooks are new since the last codegen). Candidates are the hold thresholds and the remaining GPU-heavy
   streets. The bar is `--gate m5` on `immst` with the hold on.
4. **M6 per the owner:** FSR 3 (`mcla_profile=1080p-fsr`, render markers) against 720p+FSR1. Check `temporal_aa_debug=4/7`
   and look for ghosting in shots. Optional: FSR 4.1 on the 800M series (licence and SM 6.6 checks first).
5. **Controllers:** hand test of XInput, Xbox, DS4 and DualSense (owner).
6. **60 fps gameplay fixes not yet done.** These are LARecomp-derived in Renan's port, so re-derive them from the PPC:
   - **Warp hang:** override `sub_8221BE98`; it calls `sub_8221B790`/`sub_8221B8C8`. The warper state is at
     `*(0x8286D8D4)+0x51240`, with the state word at +288.
   - **Checkpoint sweep:** override `sub_822653A8`, plus a mid-asm hook at `0x82688200` in `sub_826881A8` (`r1`, `r31`,
     `r27`).
   - **Bike gyro:** `sub_823688B8`, constants at `0x8236895C` (`f13`, 0.40) and `0x823689F0` (`f0`, 0.15), with
     k(dt) = 1-(1-k30)^(30dt).
7. GAME (low): the skip-intro hook causes 3 boot-time `Resolve region is empty` errors.
8. W0.8 E1 Xenia baseline: waits for Ali.

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
series 84 (`5e4b01ee959eb13a`); since the last codegen, new hooks were added (input fixes, physical pool), so run
`python tools/codegen.py` before the next build. The role files are in `.omp/agents/` (local, gitignored).

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
- **Game-side detail cuts barely move it** (`mcla_gfx_preset=perf` −0.3 ms, inside noise); `perf+` (impostors off) is
  slower; `skip_overwritten_transfers=true` costs +6 ms (do not ship).
- **Exclusive fullscreen stays "Composed: Flip"** with the SuperDisplay virtual second display attached; owner check:
  detach DISPLAY2 for one run.

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
