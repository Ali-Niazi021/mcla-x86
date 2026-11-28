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
| Machine, 2026-11-27 | Intel 6P+8E (20 logical), RTX 4060 Laptop 8 GB (driver 621.40; Iris Xe present but unused), panel 3200x2000 @ 165 Hz |
| Disk | C: ~209 GB free (2026-11-24) |
| VS | 2022 Community 17.14.13 (MSVC 14.44.35207), Windows SDK 10.0.26100 |
| CMake / Ninja / Python | 4.3.2 / 1.12.1 (VS-bundled) / 3.13.7 |
| Clang | LLVM 20.1.8 portable → `$WORK/toolchain/llvm-20.1.8` (W0.1) |
| SDK pin | ReXGlue v0.10.0 = `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` |
| Patch source | RenanQueiroz/mcla-recomp v0.3.17 = `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` |
| SDK series | 108 patches (72 Renan + 36 local), fingerprint `018391bf91544c45` (c7c4cc7; reset + build proven by LOCK60 wave 3) |

2026-11-27: the RTX 4060 Laptop's checkout is `C:/Users/Ali/Dev/mcla-x86` and its work folder is `C:/Users/Ali/Dev/mcla-x86-work`. The paths in the table above were recorded on the Radeon 880M machine.

## State on 2026-11-27 (native PC work)

The plan and the status board are in [NATIVE_PLAN.md](NATIVE_PLAN.md); read the board first. The numbers here are from
the RTX 4060 Laptop runs in `$WORK/scratch/t1/RESULTS.md` (the T1 build was `native-pc` @ 775ec0c). The Radeon 880M figures
in the sections below are unchanged.

**On by default now**

- Vsync on, tearing off, in the launcher and the PC profile (775ec0c). The launcher has no vsync option (3fe81e5). The
  `[mcla-display]` lines log the present settings and warn on `vsync=false`.
- Thread placement falls back to an unparked sibling when an ideal processor is parked (3fe81e5).
- 2000p profile: `scaled_copy_levers` = 27 (dual write, tone-map skip and the swap made from the render target at draw scale 2,
  0760 + 0800), `stencil_transfer_cut` = true (0812) and the unscaled-copy rule for the rain (6d2effe).
- 2000p profile, copy elision (SDK 0813 / 0814): `transfer_skip_scrambled` = 7, `resolve_reinterpret_reload` = 1,
  `resolve_elide` = 2 with `resolve_elide_keep_rules` = the five destinations of unlock-2x session 2 (R5). All on against
  all off: -0.73 +- 0.09 ms GPU busy (8.40 -> 7.68 ms at 2685 MHz), all written bytes -32 %. A `MISS` line in a log
  (`[resolve-elide]`) names a reader the rules do not know: add its printed rule to the list. The DLSS / FSR 2000p variants
  inherit these; they were not run with them.
- Render-target transfer pipelines are stored in `<title>.rtv.d3d12.xrtp` and made at start (0790; no cvar).
- Guest memory is pre-faulted on a background thread at boot in the PC profile (`mcla_prefault_guest`,
  `src/host/pf_probe.h`).
- `[present-stats]` every 10 s (`present_stats_log_s`; 0 turns it off). The shipped way to run is the 60 fps cap with
  the governor (`mcla_fps_target=60`).

**Measured at 1080p** (120 Hz guest vblank as the cap in these runs, so the 1080p ceiling is not measured)

- Ultra, saturated: 120.0 fps, GPU busy 5.32 ms a frame (mean) (`t1-m1-1080u`); 5.35 ms without the per-operation
  brackets (`t1-m1n-1080u`).
- Game thread 4.5 ms of the 8.3 ms budget (`t1-m1n-1080u`). No thread is over 8.3 ms in any run; the worst is 4.7 ms
  (RESULTS §3).
- Emulation (resolves, transfers, uploads) is 1.82 ms of the frame, 34 % (`t1-m1-1080u`).

**Measured at 2000p** (3200x2000 at draw scale 2, 165 Hz panel)

- Ultra capped at 60 with the governor (tier 5, no sheds): 60.00 fps, GPU busy p50 13.2 ms, p95 13.55 ms, 0 missed of
  4,207 frames (`t1-c60-2000ultra`). 311 s bench drive: 60.00 fps, p50 12.9 ms, 1 missed of 18,939 (`t1-m3-2000u`).
- Ultra saturated: 83.8 fps, GPU busy 11.71 ms (`t1-m2-2000u`). 120 fps is out of reach on this GPU in this scene.
- Performance capped (tier 2): 60.00 fps, GPU busy p50 12.6 ms (`t1-c60-2000perf`).
- After 0760 at 9: ultra capped, GPU busy p50 11.8 ms, was 13.2 (status board, R1 row, `r1-fin`); A/B -1.35 +/- 0.18
  ms (`r1-ab1`, `r1-ab2`); dual write 0 differing bytes over about 8,450 textures (`r1-ver`). Direct present stays off
  at 2x (`r1-sw2`).
- Before 0760 the 2x resolve path cost about 2.8 ms over the 1x line (`t1-m2-2000u`, RESULTS §4).
- 60 fps on the 165 Hz panel is a 3-3-3-2 beat by design: 1 % low 58 fps on the render side, 54 to 55 fps on the
  display side (`t1-m3-2000u`).
- Game thread tail: 11 frames over 20 ms and 2 over 34 ms in 6,306 (`t1-m2-2000u`), not chased.
- Not explained yet: the guest vblank display lock was not on in the capped 2000p runs (period source `qpc`, RESULTS
  §0), while the G1 runs had it on. The sources do not say why.

**Limits**

- The scripted drive keeps the car parked in every drive, so all drive numbers are a lower bound (RESULTS §0). The
  next measurement is a real moving drive.
- Not measured: GPU clocks, VRAM in use, the 5-minute soak (G1), 1080p capped at 60.

**Open** (the full list is on the board)

- G1: the 5-minute soak on the 60 Hz mode (118 s drives so far). G2: an even 60 on the 165 Hz panel needs VRR or a 120
  Hz mode.
- Menus and loading: the D3D12 allocations of the first 1080p frame and about 7 compute resolve pipelines a run remain
  (about 30 ms, never stored).
- `guest_vblank_display_offset_us` (0 to 8000) trades latency 1:1 (16.0 to 8.1 ms) with no repeats; the default stays
  0 until tested under load.
- Not covered by the 2x copy levers: the pause backdrop, the vinyl editor, HDR at 2x. 120 fps (T6) is queued.

## Current phase

**Sprint 2 merged (2026-11-26).** Five streams (LOCK60, HOST, VISUALS, HDR, MENUS60) are in main (c7c4cc7, SDK series
108). On the Radeon 880M laptop 1080p held 60 fps in the one-minute integration gates (gate1-base: 60.00 fps, p99 17.29
ms, 0.05 % > 25 ms, 0 holds, GPU busy 79 %; before the sprint, q0-base: 59.66 fps, p99 21.03 ms, 0.37 % > 25 ms).
Adaptive detail (the GPU frame governor) and FXAA in the base picture are on by default at 60 fps, the pause menu runs
at about 55 instead of 27 fps, and HDR output is available (not yet judged by eye). The 5-minute fullscreen M6 protocol
was not re-run. The final tree (c7c4cc7) ran the one-minute gate twice: 60.00 fps, p99 22.65 ms, 0.05 % > 25 ms with 1.8
cores of other load (gate3-base), and 59.49 fps, p99 26.15 ms, 1.25 % > 25 ms with 4.1 cores of the owner's own load
(gate3b-base); no hold in either. With other programs busy the tail is not locked yet. 720p60 and 30 fps modes passed their gates
earlier. Native 3200x2000 exists (`mcla_profile=2000p`, 23–26 fps). [Correction 2026-11-27: the 23–26 fps is the Radeon 880M's. On the RTX 4060 Laptop the 2000p profile holds 60 fps at the 60 fps cap; see the state section above.] Launcher done. Resume from the task board's Todo
list; the owner's open decisions are listed there.

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
| M6 | 60 fps at 1080p on the 880M | **near**: 60 fps in the one-minute gates, the 5-minute fullscreen protocol was NOT re-run. Native 1080p single tile, `mcla_profile=1080p mcla_fps_target=60 mcla_present_immediate=-1 mcla_gfx_preset=perf`, one-minute quick_drive runs, windowed (Composed: Flip), muted, the owner working on the machine (every run TAINTED by the 1.5-core rule). Before the sprint, q0-base (build c3b4bbb): 59.66 fps, p50 16.68, p99 21.03 ms, 0.37 % > 25 ms, max 33.4, GPU busy 94.6 %, non-game CPU 2.9 cores. After waves 1–2, gate1-base (main 57a32f2, series 107): 60.00 fps, p50 16.67, p99 17.29 ms, 0.05 % > 25 ms, max 25.4, world/real 0.9997, GPU busy 79.3 %, 0 holds, GPU 2759/2701 MHz at 44 W, non-game CPU 2.8 cores. Final tree (c7c4cc7, series 108): final tree (FXAA in the base picture): gate3-base 60.00 fps, p99 22.65 ms, 0.05 % > 25 ms (1.8 cores of other load); gate3b-base 59.49 fps, p99 26.15 ms, 1.25 % > 25 ms (4.1 cores of the owner's load); 0 holds in both. Before the sprint the 5-minute fullscreen drives (V5m6*; MyASUS Performance mode, ~45 W package, Hardware: Independent Flip) were bimodal: 3 clean runs (orc-ab-*: 59.97–60.00 fps, p99 17.3–17.7, ≤ 0.06 % > 25 ms) and 4 with one 60 → 30 hold episode of 6 + 12 s (V5m6fs, V5m6u: 58.0 fps, 3.4–3.7 % > 25 ms; gate FAIL). Those holds began at the pause menu (forced 4x MSAA) or in a load; pacer 2 and `mcla_menu_noaa` remove the cause (Sprint 2 findings), not yet re-measured over 5 minutes. Parked: 60.00 fps, p99 17.1–17.3 ms; 1920x1200 measures the same. Upscaling loses here (FSR 3 49 fps, FSR 4 37 fps) | |
| Launcher | Settings UI + Launch | **done**: `mcla_launcher.exe` (schema-driven, presets, validation); end-to-end launch to the title (V4-launcher). Sprint 2 added Adaptive detail (+ ceiling), Anti-aliasing (automatic / FXAA / off), HDR output, motion blur length and CPU boost entries | 2026-11-25 |

## Task board

### Doing

(nothing)

### Todo (next actions, in order)

1. **Close M6 on the 5-minute protocol.** The final tree's one-minute gates (gate3-base, gate3b-base, METRICS) show the tail depends on what else runs: run
   the M6 gate (drive + parked, 1080p and 1920x1200) on a quiet machine. `bench.py` gates run windowed (`run_game.py`
   sets `fullscreen=false` = Composed: Flip on this desktop); add `--cvar fullscreen=true` for Hardware: Independent
   Flip. The one-minute gates cannot show a late 6 + 12 s hold; the 5-minute drive can. The `dxbc_ps_ieee_mul`
   white-frame question (V3) and R1-E texture fast path stay as GPU candidates.
2. **Owner decisions still open** (none of these is decided):
   - The one within-run test of the CPU boost cap: it changes his power plan for 2 x 12 s (`mcla_cpu_boost_cap`; command
     and restore layers in Sprint 2 findings). Only the dry path has run.
   - Fullscreen gate runs (item 1).
   - A stream for the 60 fps gameplay fixes (item 3) and for race testing.
   - A later-story save as a fixture, to reach the GPS map and the garage (neither is reachable from `after_intro`).
   - HDR and FXAA judged by eye (steps under "HDR" in Sprint 2 findings).
   - Default output size 1200p on the 16:10 panel (VISUALS recommends it when there is at least 1 ms of headroom; 1080p
     letterboxes the panel).
3. **60 fps gameplay fixes not yet done.** LARecomp-derived in Renan's port, so re-derive from the PPC:
   - **Warp hang:** override `sub_8221BE98`; it calls `sub_8221B790`/`sub_8221B8C8`. The warper state is at
     `*(0x8286D8D4)+0x51240`, with the state word at +288.
   - **Checkpoint sweep:** override `sub_822653A8`, plus a mid-asm hook at `0x82688200` in `sub_826881A8` (`r1`, `r31`,
     `r27`).
   - **Bike gyro:** `sub_823688B8`, constants at `0x8236895C` (`f13`, 0.40) and `0x823689F0` (`f0`, 0.15), with
     k(dt) = 1-(1-k30)^(30dt).
4. **Owner checks:** audio (music, SFX, radio balance), controllers by hand (XInput/Xbox, DS4, DualSense), races, HDR
   and FXAA by eye.
5. **GPU and picture, largest first.**
   - Remaining floor (LOCK60): the tone map's depth transfer 0.33 ms, the composite resolve pair 0.55 ms each, hot-page
     uploads 0.43 ms. SDK 0590 does not count the render-target cache's own first-use pipelines (resolve and clear
     compute).
   - AA before the HUD, one mechanism for SDR and HDR: FXAA at the swap with a UI mask from the pre-UI snapshot (source
     ≠ snapshot means UI; SDR adds the snapshot, +0.18 ms) or on the tone-map target at the phase boundary (VISUALS spec
     A, HDR open item 2).
   - HDR: gamma-space FXAA for 1-px glints; find the night-only term in tone-map pixel shader E5EBEFAE4D165CBA; Vulkan
     stays on Renan's path (no fused scene, no FXAA in HDR).
   - Title screen: 12–18 ms of GPU at governor level 0 (the fly-over camera); let the closed loop run there.
   - Pop hiding needs a guest camera feed (speed, cuts): `mcla_gov_pop_speed_kmh` and `mcla_gov_cut_m` do nothing
     without it.
6. **Hold policy and measurement hygiene.**
   - Under steady foreign GPU or memory-bandwidth load the 60 → 30 hold escalates to 30 s (V5m6t, HOST): decide a
     shorter, non-escalating hold or a 40–50 fps fallback.
   - `bench.py` should flag TAINTED when PresentMon's MsGPUBusy is zero (the cores rule alone misses it).
   - Figures still old in code: launcher FXAA help (0.3 ms; per-pass timestamps say 0.54), launcher HDR help and 0710's
     `purpose` (0.1–0.3 ms; about 0.7), `Tiers()` tier 2 (+0.3). `mcla::gov::BeforeConfigSave()` is redundant.
7. Low:
   - GAME: the skip-intro hook causes 3 boot-time `Resolve region is empty` errors.
   - W0.8 E1 Xenia baseline (optional).
   - MENUS60: `sub_825F48E8` also hard-codes 4x MSAA (never seen in the tiling logs); the cost of the menu-only pass
     `sub_822E5358` is not split from the scene pass; the branch that forces 4x in `sub_822E5ED8` is not identified;
     PAUSE against driving was not re-checked on a quiet machine.
   - VISUALS: `mcla_dev_road_density` is untested; police and traffic toggles need a guest-function call facility
     (`sub_827016D8`, `sub_826D5D90`); the pedestrian-density array base is not found; FXAA with `texture_lod_bias` -0.5
     is not checked in motion or in fullscreen; tree impostors 128 → 256; a 768 shadow map (about +1 ms); `Frames` in
     the eye adaptation, rain streak length and light flicker rates are not traced to the end.
   - HOST: the 35 vs 45 W profile cannot be read without admin; thread policy is untested against a heavy real workload
     pinned to the Zen 5 cores; `mcla_thread_cold_ecoqos` and `mcla_process_priority` are untested; `set` on a string
     cvar is racy; help-text gaps (`burngpu` in bench.py / segments.py, the `mcla_app.h` header comment).

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
- S2 LOCK60 (waves 1–3): SDK 04f0 / 0510 / 0520 / 0590 (GPU timing, busy-time export, per-operation timing,
  pipeline-creation export), 0500 (VRS), 0530 / 0540 / 0580 (floor levers: −0.77 ms saturated, −0.4 ms at the cap); the
  GPU frame governor (Adaptive detail, on) and pacer 2; the cost model; FXAA in the base picture.
- S2 HOST: thread policy (on), start-up `[mcla-sys]` status lines, within-run schedules (`--schedule`,
  `tools/segments.py`, burn / burnmem / burngpu steps), the quiet test window, the CPU boost cap (prepared, not tested
  on the system), the background-load measurements.
- S2 VISUALS (2 missions): live FXAA + `texture_lod_bias` (SDK 0600), `quality` / `ultra` presets, `mcla_gfx_tier`, the
  picture-defect audit and AA recommendation, dev scene control (time of day, weather), the scene-tour schedule, the
  motion-blur length option.
- S2 HDR (2 missions): HDR runs (render-marker gate), fused HDR (0710), back-buffer dumps + analysis (0700,
  `tools/hdr/`), 2.2 decode, FXAA in HDR (0720), `hdr=on` SDR fallback (0730), fused gain (0740).
- S2 MENUS60: the pause backdrop and title Start at the renderer's own MSAA mode (`mcla_menu_noaa`; hooks 0x822E6008,
  0x822E5428): pause 27 → 55 fps at 1080p; flows `menus_tour`, `menus_probe`.
- S2 ORC: merges, the gates q0-base / gate1-base / gate2-night-hdr gate3-base / gate3b-base, series 108.

## Ownership in force

PLAN §7.2. Shared contracts for wave 1:
- GAME owns `mcla/hooks/nfsc_progress.h` (heartbeat counter; the host watchdog reads it) and
  `mcla/profile/pc_profile.h` (`mcla::ApplyPcProfileLogging()` in `OnConfigurePaths`, `mcla::ApplyPcProfile()` in
  `OnPreSetup` after the explicit GPU plugin load). HOST calls them.
- GAME's `mcla/sources.cmake` sets `MCLA_HOOK_SOURCES`, `MCLA_HOOK_DEFINITIONS`, `MCLA_HOOK_INCLUDE_DIRS`; BUILD
  compiles the hook sources into the exe as objects (strong hook symbols must win over generated weak defaults).
- The exe lands at `$WORK/build/bin/<Config>/mcla.exe`.

## Next actions

See "Todo (next actions, in order)" in the task board. Note for resuming: the repo's SDK series is 108 (fingerprint
`018391bf91544c45`); a work tree whose `$WORK/sdk/install` is older needs `python tools/prepare_sdk.py reset` + `build`
before the next game build (`status` prints the applied count and fingerprint). The codegen TOMLs changed in Sprint 2
(new mid-asm hook sites for MENUS60 and the VISUALS motion blur), so the codegen fingerprint noted before (`7c0b18d7…`,
30204 functions) is out of date: run `python tools/codegen.py` and its gate again
(docs/BUILDS.md has no row for the sprint's builds). Streams work in their own work trees
(see "Test controls and process" below). Role files are in `.omp/agents/` (local, gitignored). Every heavy job goes
through `tools/heavy_lock.py`.

## Sprint 2 findings (2026-11-26)

Five streams worked in parallel in their own git worktrees and are merged (main c7c4cc7, SDK series 108). Write-ups:
`$WORK/scratch/SPRINT2/{LOCK60,HOST,VISUALS,HDR,MENUS60}.md`; the runs (`lk-*`, `h*run*`, `vis*`, `hdr*` / `h2-*`,
`mn*`) are in the stream work trees' `runs/`, the gates (`q0-base`, `gate*`) in `$WORK/runs`. The machine was busy
during most runs (other streams' builds, the owner working on it), so most rows carry the TAINTED flag and only
within-run comparisons are trusted. Older sections below are corrected where this sprint overturned them. Entries in
LEARNINGS (LOCK60-*, Sprint2-*, VISUALS-*, HDR-*, MENUS60).

### Cost model and the floor (LOCK60)

In-run draw experiments by render phase group (lk-cm1; 1080p perf, saturated GPU, driving; SDK 04f0). Frame 14.8 ms =
6.0 pixels + 0.6 vertex + 1.2 per-draw (0.2–0.5 µs a draw; P1's 1.4–1.8 µs slope was raster and emulation) + 7.0
emulation and state floor:

| Group | base ms | pixels | vertex | per-draw | floor (no draws) |
|---|---|---|---|---|---|
| Shadow cascades (1,252 draws) | 2.33 | 1.12 | 0.13 | 0.28 | 0.81 |
| Scene opaque (1,189 draws) | 4.19 | 2.11 | 0.24 | 0.57 | 1.27 |
| Translucent + composite + post + tone map (145 draws) | 6.32 | 2.52 | 0.19 | 0.22 | 3.39 |
| Env / reflection / other pre | 1.19 | 0.29 | 0.04 | 0.10 | 0.76 |
| Lead, UI, tail, swap | 0.82 | | | | 0.82 |

- **The floor is resolves and transfers** (lk-fl1, saturated, per-operation timestamps, SDK 0520): the 15 dual-written
  resolves 2.58 ms (the five full-screen 1080p ones about 1.9), EDRAM transfers 1.01, other resolves 0.47, clears 0.47,
  hot-page uploads 0.43, texture loads 0.32 (after 0490), barriers 0.08.
- **Wave-2 levers, on:** 0530 (MSAA and offset resolves dual-written: scene −0.33 ms), 0540 (0450 and 04e0 together:
  post −0.25), 0580 (fused gamma paint no longer blocked by the always-registered toast: swap −0.18). Together −0.77 ±
  0.02 ms saturated (lk-fl6: 11.23 → 10.46 ms) and about −0.4 ms busy at the 60 cap (lk-val2: 11.65 on / 12.05 off,
  mean).
- **Measured and left off:** resolve memory write deferred (exact: −0.09 ms; dropped outright, not exact: −1.07 ms,
  lk-fl2), whole-resource clears (+0.26 ms), VRS on the main scene (+0.2 to +0.5 ms slower and blurred; only post /
  composite / translucent / pre at 2x2 pay: −0.36..−0.43 ms level 1, −0.56..−0.73 ms level 2, lk-vrs1), GPU priority,
  EcoQoS on hot threads.
- **Typical load** (1080p perf, 60 cap, daylight drive; lk-val2): GPU busy mean 11.65 ms, p50 11.7, p90 12.65, p99 13.75
  of 16.7 (about 0.7 ms of presenter paint is outside the counter). The governor's own run, lk-gov2: 11–12 ms at level
  0.
- **Weather and time of day** (VISUALS; same-view parked or driving pairs): night + rain is the heaviest state, about +1
  ms over noon (range +0.5 to +2.5; the ranking held in every pair, the size is not sure: frame times at a 60 cap give a
  lower bound and PresentMon's GPU busy was dead then); rain at dusk −1.6 ms (vis5), fog +0.25 ms, night alone and dusk
  not heavier. Street-to-street variation and first-visit shader compiles weigh more than weather.

### What is on by default now (PC profile blocks, `mcla/profile/pc_profile.cpp`)

*Dated note 2026-11-27: this list predates the native PC work. Since then vsync has no launcher option and tearing is off by default,
the 2000p profile has `scaled_copy_levers` = 9 and the rain rule, the transfer pipelines are stored, and the guest
memory is pre-faulted. See the state section near the top.*

- **LOCK60 block:** `tonemap_transfer_skip` (04e0), `mcla_gov` (the governor; it runs with the 60 fps target),
  `resolve_dual_write_msaa` (0530), `skip_overwritten_transfers` (0450 with 0540), `ui_overlay_active_when_drawn`
  (0580); pacer 2 (`mcla_sim_pacer` default 2); `gpu_timing_export` (SDK default: two timestamps and the list boundaries
  a frame).
- **HOST block:** the thread policy (`mcla_thread_hot_priority=2`, `mcla_thread_hot_cpus=2`, `mcla_thread_cold_cpus=1`,
  `mcla_process_noecoqos=true`); `[mcla-sys]` status lines at every start.
- **MENUS60 block:** `mcla_menu_noaa=true`, for every preset (the pause backdrop and the title's Start draw in the
  renderer's own mode, 1x with `noaa`, per the owner's anti-aliasing decision).
- **HDR block:** `hdr_sdr_eotf=gamma22`; `hdr` itself stays off (the launcher offers Off / Auto / On).
- **The picture:** with the governor on, FXAA is in the base picture (the perf preset's floor is tier 2 = perf + FXAA)
  and detail is added up to `mcla_gov_tier_ceiling` (default 4, the shipped look) when the GPU has headroom. The
  launcher's "Anti-aliasing: automatic" is on, so the launcher does not set `swap_post_effect`; a value the player sets
  is never touched by a tier or a preset. With the governor off, the perf preset has no AA.
- **Off** (measurement, or not paying): `gpu_timing`, `gpu_timing_ops`, `gpu_draw_experiment`, `gpu_experiment_cycle`,
  `vrs_level` (VRS only through the governor), `resolve_defer_memory_write`, `clear_full_resource`,
  `resolve_dual_write_nomem_dev` (not exact), `mcla_gpu_priority`, `mcla_cpu_boost_cap`, `mcla_thread_hot_ecoqos`,
  `mcla_window_pos` / `mcla_window_behind` (run_game passes them).

### The perf preset now (corrects the 2026-11-25 description)

The preset itself is unchanged: car reflections and shadow cascade 2 at half rate, shadow distance 0.6, traffic LOD 0.7,
city LOD 0.67, shadow map 512; it carries no AA. With Adaptive detail on, its floor tier is 2 (perf + FXAA, +0.54 ms by
per-pass timestamps, not VISUALS' first +0.3 ms from whole-frame busy). `quality` = shipped detail + 640 shadow map +
FXAA (about +1.5–1.8 ms over perf), `ultra` = quality + 1.5x distances, `perf+` and `lowest` as before. Governor floors:
perf 2, perf+ and lowest 1, quality and default 4, ultra 5. `skip_overwritten_transfers` is on (it did not cost +6 ms;
below).

### The governor in five lines (LOCK60; `mcla/hooks/mcla_gov.cpp`, cvar `mcla_gov`)

1. A closed loop on the GPU's busy time per frame, exported by the SDK (04f0 / 0510: the frame's span minus the idle
   gaps between its lists; fallback: missed frames).
2. One two-way ladder: enriched detail tiers (up to the ceiling, default 4) → VRS 1–2 (post passes only) → tiers below
   the preset's floor → the 60 → 30 hold. Sheds fast on any of median over 15.6 ms / 3 of 30 samples over / missed
   frames for VRS and enriched tiers, but below the floor only on sustained overload (median over the line for 30 GPU
   frames, or 2 of 30 missed; 30 s bounce window).
3. Restores and enriches slowly: one tier at a time when p90 + the predicted (learned) cost + 1.5 ms ≤ 15.0 ms, with
   back-off (`mcla_gov_enrich_s` 10, `mcla_gov_enrich_margin_ms` 1.5, `mcla_gov_enrich_hold_s` 30); the tiers that pop
   (3 and 5) wait for a pause, load or menu.
4. Holds its rung outside driving; 60-frame grace after a screen change; shader-compile bursts excused (4 s per 20 s, at
   most 2 windows in a row).
5. Tiers write the knobs as app defaults (never saved; a knob the player set wins) and `mcla_gfx_tier` is transient.

Evidence: lk-gov2 (busy 11–12 ms at level 0; sheds within 0.1–0.3 s of a load rise; back at level 0 in about 16 s of
calm), lk-gst1 (4 CPU + 4 memory spinners, governor on / off in one run: 58.76 fps, 1.70 % > 25 ms, no hold / 39.92 fps,
50.3 %, 60 → 30 hold), lk-gov3 (enriched tier 1 → 2 at +42 s), lk-w3tour (tier 2 throughout the scene tour, only VRS
moves), lk-w3cold2 (fresh shader cache, 0 holds). The integration gates on the final tree: gate3-base and gate3b-base (METRICS).

**The 60 → 30 hold, corrected:** the holds of the bad 1080p bench runs (V5m6fs-m6fs, V5m6u-m6, V5m6u-p1200,
orc-fix-p1080) all started within about 1 s of `free-roam -> pause` (the pause backdrop's forced 4x MSAA, 7 tiles, about
100 % missed) or in a load, not in driving; the earlier suspicion (a GPU clock dip after a hold) is withdrawn. Pacer 2
(default) judges drives only, gives 4 s grace after a hold or a menu, needs two high windows in a row and only acts once
the governor has shed everything; an offline replay reproduces all 9 logged holds with pacer 1 and none with pacer 2
(LOCK60-A3).

### Thread policy and background load (HOST)

- **On by default:** the frame-critical threads (GPU Commands, GPU Submission, Main XThread, the busy guest XThreads) at
  THREAD_PRIORITY_HIGHEST, each on its own physical Zen 5 core; compile / storage / timer threads on Zen 5c; the process
  out of EcoQoS. It fixes the tail only: with 10–12 foreign spinner processes, 12 segments with the policy had 0 frames
  over 25 ms, 9 without had 4 segments with late frames (p99 up to 55 ms); average fps unchanged.
- **Correction 2026-11-27 (Intel 6P+8E laptop):** the frame-critical threads are placed one per physical core of the
  highest efficiency class (`high_cores`). When an ideal processor is parked, `SetThreadIdealProcessorEx` fails with
  error 31 and the placement falls back to an unparked sibling (3fe81e5; `src/host/thread_policy.cpp`).
- **What hurts is bandwidth and the GPU, not CPU time** (table below). Nothing host-side helps bandwidth or a foreign
  GPU user (GPU scheduling class HIGH changed nothing); the governor does (lk-gst1 above). CPU spinners cost the GPU its
  clock (1913 → 1374 MHz) at a flat 45 W package, because CPU boost takes the shared watts: the boost-cap question.
- **CPU boost cap, prepared and NOT tested on the system** (`mcla_cpu_boost_cap`: 1 = PERFBOOSTMODE 0 on AC and DC in
  the active plan while the game runs, 2 = max processor state 99 %; launcher "CPU boost while playing", advanced,
  default leave). Restore layers: a record `%LOCALAPPDATA%\mcla_x86\power_restore.json` written first; restore on cvar →
  0 or exit; `mcla_power_restore.exe` (breakaway helper) waits on the game's pid and restores after a crash or kill; any
  later start restores a stale record; a value is restored only if it is still ours. Only the dry path
  (`mcla_cpu_boost_cap_dry=true`) ran (h5run4); the real write, ACCESS_DENIED and the helper's real restore are
  untested. The one test, **pending the owner's approval** (it changes his plan for 2 x 12 s): `bench.py --flow
  quick_drive ... --cvar mcla_sim_hold30_s=0 --schedule "46:burn 8;52:set mcla_cpu_boost_cap=1;64:set
  mcla_cpu_boost_cap=0;76:set mcla_cpu_boost_cap=1;88:set mcla_cpu_boost_cap=0;100:burn 0"` + `tools/segments.py`, then
  `powercfg /q` shows 2/2 again and no record is left. The "Best performance" power-mode overlay is active and may
  override the plan's boost value.
- **Start-up `[mcla-sys]` lines:** power source, battery saver, Windows power mode, plan boost mode and max state, CPU
  classes, ASUS fan raw values (`QuietFan=2 FanMode=2`, mapping unverified), display refresh, a warning when 60 fps at
  1080p is unlikely.

Background load, in-run A/B (quick_drive, hold off; the machine was busy in every HOST run, so only within-run figures):

| Foreign load | fps | GPU ms/frame p50 | GPU MHz p50 | Run |
|---|---|---|---|---|
| none (same run) | 60.0 | 15.1 | 1913 | h1run1 |
| 6 CPU spinners | 59.7 | 16.3 | 1374 | h1run1 |
| 4 memory-bandwidth spinners | 42.0 | 21.6 | 1519 | h1run1 |
| 40 % foreign GPU duty | 40.1 | 24.0 | 2512 | h2run2 |
| 40 % GPU duty + `mcla_gpu_priority=2` | 40.3 | 23.9 | 2512 | h2run2 |
| 10–12 CPU spinners, policy off | 56.3–58.6 | 16.5–17.0 | 1374–1539 | h2run2/3/5 |
| 10–12 CPU spinners, policy on | 58.4–60.0 | 16.1–16.9 | 1452–1566 | h2run2/3/5 |

### Pause menu and other screens (MENUS60)

The game forces 4x MSAA on the pause backdrop (and on the title's Start for about 0.3 s): 7 tiles at 1080p, 8 at 1200p,
which the single-tile hook cannot merge. `mcla_menu_noaa` (on) draws those passes in the renderer's own mode; the pause
costs what driving costs in the same run (mn1-tour, mn3-p1200; all three runs TAINTED, within-run only):

| Screen | Before | After |
|---|---|---|
| PAUSE, 1080p | 27.3 fps, GPU 32–34 ms, 90 % > 25 ms | 54.8–56.5 fps, GPU 15.1–16.3 ms (driving in the same run 57.1–57.3 fps) |
| PAUSE, 1200p | 22.6 fps, GPU 41 ms, 97 % > 25 ms | 52.8 fps, GPU 16–17.5 ms (driving 53.3–53.6 fps) |
| PAUSE tabs, SETTINGS > OPTIONS | as PAUSE | 1080p 56.4 fps; 1200p 52.5–53.0 fps |
| Entry into PAUSE | the first frames of the 4x pass took 66 ms (1080p) / 224 + 265 ms (1200p) | one 94 ms frame at 1080p (the cut to the pause camera and the menu); leaving PAUSE never stalled |
| Title (city fly-over, 1x, 1 tile) | not forced 4x | GPU 12–18 ms at 1080p by camera path, 13–15 ms at 1200p (open) |
| Loading screen | n/a | GPU 13–18 ms, streaming + map view |
| GPS map, garage | not reached | not reachable from `after_intro`; probably covered (same scene pass) |

### HDR (state after two missions)

How it works now:
1. An HDR10 swap chain (R10G10B10A2, PQ, BT.2020) whenever `hdr` is auto or on and Windows HDR is on for the window's
   display; peak and paper white come from Windows; the display is re-read about once a second.
2. The scene stays the game's own 8-bit picture below the knee (60 % of the game's white); above it a gain from Renan's
   HDR curve over the game's curve lifts highlights up to the peak (0710 fused + 0740 gain), computed in the swap's
   refresher from the tone map's captured inputs.
3. The HUD and menus are found by difference against a pre-UI snapshot and shown as SDR content at Windows' SDR white
   (304 nits on the owner's OLED), never above it.
4. The game's SDR codes are decoded with a 2.2 power (`hdr_sdr_eotf=gamma22`); the display table still applies, light
   rolls off from 0.75 of the peak, the output is PQ.
5. FXAA also runs on the HDR10 image (0720); the refresher reads the front buffer from its render target (04a0 record).

Measured (880M, 1080p; owner's OLED: peak 772, full frame 501, black 0.0057, SDR white 304 nits):
- **Cost** (per-pass timestamps, h2-6; the figure to use, not whole-frame busy): fused HDR about +0.7 ms per frame (swap
  group +0.53 the refresher, ui group +0.18 the pre-UI snapshot; the gain fix +0.005); FXAA +0.54 ms in HDR, the same as
  in SDR. Renan's original path was about +1.8 ms (hdr2). The first fused figure of +0.1–0.3 ms (hdr3) was
  scene-confounded.
- **Black floor** (p0.5) 0.0009–0.009 nits by day, 0.0036 at night (sRGB decode: 0.14–0.20 by day, 0.135 at night);
  shadow codes 1–10 map to 0.008–0.26 nits, monotonic, above panel black. **HUD** flat white 299.5 nits (98.5 % of 304);
  at night the speedometer ≤ 109 nits, the minimap ≤ 199.
- **Highlights:** dusk lights 400–690 nits by day; night lamps and lit windows 553–706; parked night 133 px above 304
  nits, night rain 1,239 px above 500. **Keeps the game's look:** HDR / SDR median luminance ratio 1.00 ± 0.02 in every
  band up to 0.6 of white (day hdr3, night h2-8), 1.3–1.6x above 0.8. **Banding:** day sky median step 1 PQ code vs 2 in
  the 8-bit frame.
- **Fused exactness:** scene 0 mismatches in every check (hdr3, h2-6 with VRS 2 and the governor, h2-8,
  gate2-night-hdr); gate2-night-hdr (scene tour, `hdr=auto`, `hdr_scene_fused_check=30`): tone map 0–3 px at 5/255, no
  magenta in 40 screenshots, 0 holds, governor busy p50 12–13 ms, only VRS levels moved.
- **Needs the owner's eyes:** the HDR look by day and at night with FXAA; whether the HUD at 304 nits feels right; thin
  glints under FXAA (a 1-px glint of 406 nits ends at 95, SDR + FXAA 110); the brief flash when `hdr` is toggled; `auto`
  on an SDR display (code path only, not run). **Unidentified:** the game's tone map has a night-only term Renan's
  recompute lacks (88–98 % of pixels, up to 9 codes at tod 23); 0740 makes it harmless below the knee, above it the
  highlight gain can be off by up to about 8 %; Renan's non-fused path (Vulkan, `hdr_scene_fused=false`) probably
  differs at night too (not measured).
- **Owner's steps to try HDR:** keep Windows HDR on for the laptop panel (optionally run the Windows HDR Calibration app
  once); in the launcher set Display > HDR output = Auto (peak, paper white and HUD at 0 = from Windows) and launch;
  look at dusk and night lights, headlights and lit windows (brighter than SDR), midtones (same as SDR), the HUD (the
  desktop's white level, not glaring), night shadows (true black, no grey veil), the sky (no bands), edges (smoothed by
  FXAA). Tune: highlights clip → lower HDR peak brightness; HUD too bright → lower HDR HUD brightness; shadows too deep
  → HDR shadow curve = sRGB.
- **Tools:** `tools/hdr/hdr_analyze.py <dump.dds>... [--compare-sdr sdr.dds --sdr-eotf gamma22] [--rect x,y,w,h]
  [--band-rect ...]` (nits, black floor, HUD white, gamut, banding, SDR comparison, preview and nits PNGs; `--selftest`)
  and `tools/hdr/hdr_ab.py <run dir>`; screenshots cannot judge HDR (LEARNINGS HDR-4).

### Picture quality, tiers and output size (VISUALS)

- **Defects, worst first** (1080p perf on the 3200x2000 panel): geometry aliasing (near-horizontal edges, worse at
  night; FXAA fixes most); alpha-tested foliage (1-px blocky, shimmering edges; FXAA softens, a real fix needs
  alpha-to-coverage and therefore MSAA); reduced detail from the perf preset (512 shadow map, shadow distance 0.6, LOD
  0.67 / 0.7; tiers and `quality` restore it); blobby distant tree impostors (128x128 views); black level (floor at
  pixel value 2–3, about 9 % of a noon frame ≤ 3; the game's tone map, a decision for HDR or the owner); motion blur at
  half the console's length at 60 fps; thin dark slivers beside building window frames; 1080p letterboxes a 16:10 panel.
  Checked and fine: HUD, daylight sky (no banding), rain, fog, street lamps, neon, headlights; no broken pass in any
  time-of-day or weather state.
- **AA:** default when there is headroom: `swap_post_effect=fxaa`, +0.54 ms (per-pass timestamps; vis3's whole-frame
  figure was +0.3, range 0.15–0.6). It runs in the swap pass, so it also touches the HUD (legible) and keeps neon glow;
  `fxaa_extreme` costs the same and is slightly softer (not recommended). `texture_lod_bias` -0.5 pairs well with FXAA
  (cost in the noise; without AA it adds shimmer; default 0). MSAA 2x is impossible at 1080p single tile: 3264 EDRAM
  tiles per surface against the 2048 a surface can reach, 6528 for colour + depth against 4096 (1000p at 2x: 2500 per
  surface); tiled 2x measured +1.7 ms at 720p. FSR1 on the presenter +1.46 ms: not worth it.
- **Detail tiers** (`mcla_gfx_tier`, live, -1 = off; higher = more detail; table below): vis4-drive-tiers, pooled GPU ms
  while driving: tier 1 14.13, tier 2 15.07, tier 4 15.49, tier 5 16.50. A tier never touches `mcla_gfx_shadow_res`
  (read at world load: 640 shipped, 512 perf). The governor sets the tier; the tier writes knob defaults that are never
  saved and a knob the player set wins. API: `mcla::gfxtune::Tiers()`, `CurrentTier()`, `kTierMin` / `kTierMax` in
  `mcla_gfx_tune.h`.
- **Output size** (offline proxy, a native 3200x2000 shot decimated and bilinear-upscaled): 1920x1200 and 1920x1080 are
  the same per pixel (PSNR 35.2 dB, detail 0.77); 1600x1000 at an exact 2x is worse (32.9 dB, 0.68); at about 236 ppi
  integer scaling buys nothing visible. Recommendation: `1200p` on this laptop when there is at least 1 ms of headroom
  (it fills the panel at the same sharpness for +0.4–1.0 ms: V5m6u busy 82.7 vs 80.5 % driving, 81.7 vs 78.6 % parked),
  else `1080p`; `1080p` on a 16:9 monitor; `1000p` only as a last resort. The size is set at start. The default is the
  owner's open decision.
- **60 fps effects:** eye adaptation is dt-based (no fix); motion blur is half length at 60 fps because the blur takes
  0.25 x the per-frame displacement: an option, `mcla_vis_motion_blur` (default 1; -1 = console look), the console look
  is the owner's call; light streaks and trails have no timer reads. Camera shake and tyre slip are simulation side. No
  render effect is simply wrong at 60.

GPU cost of each detail tier over the tier below (880M, 1080p, 60 fps):

| Tier | Knobs | GPU cost | Gain | Switch without a visible pop |
|---|---|---|---|---|
| 0 | perf + `shadow_mode=1` (no sun shadows), no AA | −3.0 ms (S3 estimate, not measured on PC) | big loss | no (emergency only) |
| 1 | perf: env and far cascade at half rate, shadow distance 0.6, LOD 0.67 / 0.7 | baseline | | yes |
| 2 | 1 + FXAA | +0.54 ms | high (edges) | yes |
| 3 | 2 + shipped distances 1.0 / 1.0 / 1.0 | +0.3–0.6 ms (parked) | medium | no (small pop at the LOD and shadow boundary) |
| 4 | 3 + shipped rates (env 1, far cascade 1) = the live part of `quality` | +0.8 ms | low–medium | yes |
| 5 | 4 + distances 1.5x = `ultra` | +0.3 ms parked, about +1 ms driving | low | no (small pop) |

### Test controls and process

- **Work trees:** `python tools/stream_work.py <name> --sdk shared` makes a private work tree, and a gitignored
  `CMakeUserPresets.json` (written by the tool: own `binaryDir` and `environment.MCLA_WORK`, inherits `windows-base`)
  makes cmake build there, because `CMakePresets.json` pins `MCLA_WORK` (LEARNINGS Sprint2-Process). The heavy lock is
  machine-global (`tools/heavy_lock.py`; `prepare_sdk.py build` takes it).
- **Runs:** `run_game.py` runs muted, bottom-right behind the foreground window, without activation (`--window-pos`,
  `--behind` / `--no-behind`; PrintWindow captures the covered window); `--fixture` defaults to the flow header's.
  `tools/flows/quick_drive.txt` is the standard short run (118 s: boot, free roam at about 36.5 s, drive 52–118 s);
  `menus_tour.txt` (132 s: title, drive, PAUSE with the fix off then on, tabs, SETTINGS > OPTIONS, a second drive) and
  `menus_probe.txt` (128 s: title and PAUSE with the governor closed loop and pinned) are MENUS60's.
- **Within-run A/B:** `run_game.py` / `bench.py --schedule FILE|"steps"` appends `<s>:set <cvar>=<value>`, `<s>:burn N`
  (CPU spinners), `<s>:burnmem N` (memory-bandwidth spinners), `<s>:burngpu PCT` (foreign GPU duty,
  `tools/telemetry/gpuload.exe`), `<s>:mark L` (a named boundary) to any flow (example
  `tools/flows/schedules/ab_example.txt`; `--check-flow --schedule ...` validates); `python tools/segments.py <run>
  [--settle S] [--from-free-roam S] [--json F]` prints per-segment and pooled-label frame time, GPU busy, GPU ms, clock,
  package W, foreign and game CPU, holds, per-thread CPU and the A/B deltas. Use alternating A/B/A/B segments and pool
  each label; one pair cannot resolve 0.3 ms; keep parked comparisons inside about 52–88 s (the idle camera).
- **Scenes:** `mcla/schedules/vis_scenes.txt` (noon, dusk, night, night + rain, fog, noon between them): `run_game.py
  --flow quick_drive --fixture after_intro --schedule mcla/schedules/vis_scenes.txt`, then `segments.py <run> --settle
  2`; warm the shader cache first. Dev cvars: `mcla_dev_tod` (-1 | 0..24, freezes the clock), `mcla_dev_tod_freeze`,
  `mcla_dev_weather` (0 nice, 1 cloudy, 2 rain, 3 fog) with `mcla_dev_weather_snap`, `mcla_dev_road_density` (untested);
  they override race events' own time of day and weather while set.
- **Proof modes:** `skip_overwritten_transfers_poison`, `tonemap_transfer_skip_poison` (magenta in the cut rectangle),
  `resolve_dual_write_verify`, `hdr_scene_fused_check=N`, `hdr_dump_dir` / `hdr_dump_at` (SDK 0700), `gpu_timing=1` +
  `gpu_timing_csv` + `gpu_experiment_cycle` (frame-exact A/B inside one run), `gpu_timing_ops`.

### Cvars of the sprint

- **Player-facing (launcher):** `mcla_gov` (Adaptive detail, on at 60 fps), `mcla_gov_tier_ceiling` (2 Performance, 4
  Shipped look (default), 5 Beyond console), `swap_post_effect` (none / fxaa / fxaa_extreme; the launcher's automatic
  entry leaves it to the game), `mcla_sim_hold30_s` (the 60 → 30 hold length; 0 = never hold), `hdr` (off / auto / on)
  with `hdr_peak_nits`, `hdr_paper_white_nits`, `hdr_ui_nits` (0 = from Windows), `mcla_hdr_highlights` (1 / 1.5 / 2 /
  3), `hdr_sdr_eotf` (gamma22 / srgb), `mcla_vis_motion_blur`, `mcla_cpu_boost_cap`, `mcla_gfx_preset` (perf default;
  quality, ultra, perf+, lowest, default).
- **Governor tuning:** `mcla_gov_shed_ms` 15.6 / `mcla_gov_restore_ms` 15.0, `mcla_gov_shed_over_count` 3,
  `mcla_gov_floor_shed_frames` 30 (0 = old rule), `mcla_gov_tier_floor` -1 (auto from the preset), `mcla_gov_vrs_levels`
  2, `mcla_gov_enrich_s` 10, `mcla_gov_enrich_margin_ms` 1.5, `mcla_gov_enrich_hold_s` 30, `mcla_gov_restore_s` 4,
  `mcla_gov_screen_grace_frames` 60, `mcla_gov_pop_wait_s` 1.5, `mcla_gov_pop_speed_kmh` 90, `mcla_gov_cut_m` 20,
  `mcla_compile_quiet_ms` 500, `mcla_compile_excuse_s` 4.0, `mcla_gov_force_level` / `mcla_gov_max_level` (pin or cap
  the rung), `mcla_gov_log_s` (`[mcla-gov]` lines); pacer: `mcla_sim_pacer` 2 (0 shipped, 1 adaptive),
  `mcla_sim_hold_grace_s` 4, `mcla_sim_hold_windows` 2; `mcla_gfx_tier` -1 | 0..5.
- **Host:** `mcla_thread_hot_priority` / `mcla_thread_hot_cpus` / `mcla_thread_cold_cpus` / `mcla_process_noecoqos` (the
  policy, live, 0 restores), `mcla_thread_stats_s N` (`[mcla-threads]` per-thread CPU every N s), `mcla_thread_hot_min`,
  `mcla_thread_hot_ecoqos`, `mcla_thread_hot_noecoqos`, `mcla_thread_cold_ecoqos`, `mcla_process_priority` (off),
  `mcla_gpu_priority` (0/1/2, no effect), `mcla_window_pos` / `mcla_window_behind`, `mcla_menu_noaa`.
- **SDK (D3D12):** `gpu_timing`, `gpu_timing_csv`, `gpu_timing_ops`, `gpu_timing_export` (true; exports
  `rexgpu_gpu_frame_us` = busy time), `gpu_draw_experiment` 0..3 + `gpu_draw_experiment_phases`, `gpu_experiment_cycle`
  (+ `_period_ms`, `_start_s`), `vrs_level`, `vrs_levels`, `vrs_min_target_width` 640, `resolve_dual_write_msaa` (on) +
  `resolve_dual_write_verify`, `skip_overwritten_transfers` (on) + `_poison`, `ui_overlay_active_when_drawn` (on),
  `resolve_defer_memory_write` (+ `_poison`, `_verify`, `resolve_defer_backoff_frames`), `clear_full_resource` (+
  `_pad`, `_poison`), `resolve_dual_write_nomem_dev` (off), `swap_post_effect` (live, 0600), `texture_lod_bias`
  0..-1.875 (live), `hdr_fxaa`, `hdr_scene_fused`, `hdr_scene_fused_gain`, `hdr_trim`, `hdr_ui_mode`,
  `hdr_scene_fused_check`, `hdr_dump_*`, `hdr_test_pattern`; pipeline counts through 0590 (`[pipelines]` log line).
- **Scene and detail knobs:** `mcla_gfx_shadow_dist` / `mcla_gfx_city_lod` / `mcla_gfx_traffic_lod` 0.2–1.5 (live; above
  1 is beyond the console), `mcla_hdr_ab` (+ `_period_s`, `_start_s`) and `mcla_vis_ab` (`SEC:CVAR:VALUE+...`) are
  in-run switchers, superseded by `--schedule` and `gpu_experiment_cycle`.

## Sprint findings (2026-11-25; corrected 2026-11-26 where Sprint 2 overturned it)

Reports in `$WORK/scratch/{S2,S3,S4,S5,R2,R3,R4,R5,R6}/REPORT.md`.
- **Upscaling loses on the 880M.** The FSR 3.1 pass costs 4.1 ms, FSR 4.1 6.4 ms, against ~3 ms saved by drawing the scene
  smaller. Native 1080p single tile is the fastest 1080p.
- **Power, not compute, is the wall.** The package sits at ~35 W; the GPU runs ~1.25–1.6 GHz (2.9 GHz boost) and drops
  further when other CPU work runs (agent builds, Defender). Benches need a quiet machine (heavy lock for every
  CPU-heavy job; `prepare_sdk.py build` now always takes the global lock) and telemetry in each row.
  (Update 2026-11-26: in MyASUS Performance mode the package reads 44–45 W and the GPU 2.7–2.76 GHz when the machine is
  quiet, e.g. gate1-base 2759/2701 MHz at 44 W; foreign CPU load still costs the GPU its clock, Sprint 2 findings.)
- **8–13 ms of the GPU frame doesn't scale with the shader clock** (R6): bandwidth- or serialization-bound emulation work
  (resolves + texture reloads ~140 MB/frame, EDRAM transfers, swap path). Levers in flight: 0490 dual-write resolve
  (−0.8–1.1 ms est.), swap-direct-present (−0.2–0.35), quad lists without the GS, `clear_memory_page_state=false`.
  (Update 2026-11-26: shipped (0490 −0.6 ms, 04a0 −0.35 ms measured) and split per operation in Sprint 2: a 7.0 ms
  floor, resolves 2.58 ms of it; wave 2 took another −0.77 ms.)
- **The `perf` scene-detail preset** is: car reflections and shadow cascade 2 at half rate, shadow distance 0.6, LODs,
  shadow map 512. The two half-rate knobs flickered until the phase's own object was skipped too (LEARNINGS,
  Owner-report); they are worth ~5 points of GPU load at 1080p (81.5 → 76.5 % busy). Cascade 3 cannot be skipped (its
  end call draws the player's car body path); `mcla_gfx_impostor_interval` holds nothing back. `perf+` (impostors off)
  is slower. `skip_overwritten_transfers=true` was measured at +6 ms here (S1); that did not reproduce: it switched off
  04e0's tone-map cut, and with 0540 it is on and cuts the post chain by 0.25 ms (lk-fl2, lk-fl6; LEARNINGS LOCK60-W2c).
  (Update 2026-11-26: with Adaptive detail on, perf's floor tier is 2 = perf + FXAA, and the governor adds detail up to
  the shipped look when there is headroom; see "The perf preset now".)
- **Flicker test:** `$WORK/scratch/ORC/burst.py` + `flicker_ab.py` (24 window captures in 1 s of the parked scene;
  reference 0.2–0.4 % of pixels with a temporal std > 6). An empty `--mcla_gfx_preset=` swallows the next argument.
- **Native 3200x2000:** `mcla_profile=2000p` = the 1600x1000 guest at `draw_resolution_scale` 2x2: correct picture,
  23–26 fps, GPU 100 % busy (Radeon 880M); dual-write resolves and the direct present fall back at a scale above 1.
  [Correction 2026-11-27, RTX 4060 Laptop: the dual-write resolves run at a scale above 1 with `scaled_copy_levers` bit 1 (9 in the 2000p profile); the direct present still falls back.]
  Since 2026-11-27 (R2 / R3 of `docs/native/ROADMAP.md`, measured on the RTX 4060 with `quick_drive2`, runs `k-*` at a locked 1800 MHz):
  the profile also sets `mcla_gfx_shadow_res=320` (the host shadow map is the shipped 640, not 1280; a gfx preset's shadow size is
  halved at scale 2; -0.6 ms) and `mcla_gfx_env_res=256` (the cars' reflection map 512x256 -> 256x128 guest, the 1080p look; -0.5 to
  -0.6 ms). `vrs_level=2` as a base was measured and left OFF: it saves only 0.1 ms and stair-steps the distant tree masses (5 of 5 runs
  with it, 0 of 5 without), and the governor's VRS rungs share that look. Targets and addresses: `docs/native/R2_TARGETS.md`. A/B:
  `--mcla_gfx_shadow_res=640`, `--mcla_gfx_env_res=0`, `--vrs_level=2`.
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
- **Tearing cvar** is `d3d12_allow_variable_refresh_rate_and_tearing` (the SDK's default is on; the PC profile sets it OFF,
  and the launcher's "Allow tearing (G-Sync / FreeSync only)" is off by default: with it on, every frame flips mid-refresh, which
  tears on a TV or ordinary monitor; only a G-Sync / FreeSync display wants it). **Guest vblank rate** cvar is
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
- **The 60 → 30 hold is the governor's last rung (pacer 2, 2026-11-26).** Adaptive detail (`mcla_gov`) sheds detail
  first and the hold only fires once it has shed everything, in driving only; FXAA, a post-process, sits in the base
  picture next to the owner's `noaa` decision (MSAA stays off; the pause backdrop now follows it too). Both are on by
  default at the 60 fps target; the owner has not yet judged FXAA or HDR by eye.
