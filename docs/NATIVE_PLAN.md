# Native PC plan

Owner's goal (2026-11-27): move the port from "the Xbox 360 game running through a translation layer" towards a native
PC game that can be optimised for any PC hardware. Lead: the orchestrator thread (see `docs/AGENT_SETUP.md` for the operating
model, the machine rules and how to recreate the setup). This file is the plan and the status board; `docs/PLAN.md` is
the original port plan and stays as history.

## Targets (acceptance, measured with PresentMon and the game's own log)

| Id | Target | Pass when |
|---|---|---|
| G1 | TV: 1920x1080 @ 60 Hz, no VRR | fullscreen, vsync locked, no tearing, 0 repeated frames and 0 `[mcla-hitch]` in a 5 min drive with a warm cache |
| G2 | Laptop panel: 3200x2000 native @ 60 fps minimum | true 3200x2000 picture, 1 % low >= 60 fps in the bench drive, panel at 120 Hz or VRR so 60 is even |
| G3 | 120 fps (optional, own track) | 1080p first, then the panel's native size; world speed x1.000, 1 % low >= 110 |
| G4 | In-game overlay | fps counter, statistics screen, mod menu on a key (display, graphics, game settings, cheats and mods) |
| G5 | Any PC | presets chosen from the GPU class at first start; no run-time shader hitch after the first minutes; one command setup |

## What "native" means here

The game's CPU code is already native: `default.xex` is recompiled ahead of time to x86-64 C++ (no interpreter, no JIT).
What is still an emulation of the console, and therefore the cost and the limit:

1. **GPU**: the game writes Xenos command buffers; a command-processor thread turns them into Direct3D 12. Render targets
   live in an emulated EDRAM and are copied ("resolved") to textures; shaders are Xenos microcode translated at run time.
2. **Memory**: one 32-bit big-endian guest address space, byte swaps on every access, write watches to see texture and
   vertex data change.
3. **Kernel, audio, I/O**: Xbox kernel and XAM calls, XMA audio decode, the disc file system.
4. **Timing**: a guest vblank and a 30 fps engine taught to step at 60 and 120.

There is no source code, so "native" is reached by replacing those layers piece by piece, measured each time, never by a
rewrite. Each track below removes one piece of emulation or makes it cost nothing.

## Tracks

| Track | What | First steps | Owner tier |
|---|---|---|---|
| T0 Stabilise | display lock and tearing default, shader cache misses, logging (in flight) | 60 Hz TV test; pipeline-cache diagnosis and fix | soldier |
| T1 Measure | cost model on this machine: resolution x preset matrix, CPU per thread, GPU per operation; which of game thread, command processor, GPU binds at each target | bench matrix at 1080p / 1600p / 3200x2000; `[gpu-ops]` and thread-top captures | soldier + grunt |
| T2 Shaders | zero run-time compiles: complete persistence, full preload with a progress screen, D3D12 pipeline library (driver cache), a shareable shader pack | fix from T0, then pipeline library | soldier, commander review |
| T3 Native render path | native-resolution render targets without EDRAM round trips: resolves replaced by direct sampling where the result is only sampled, no copies for post-processing, native MSAA, upscalers as an option | roadmap study (commander), then one resolve class at a time | commander + soldiers |
| T4 CPU | command-processor cost (state dedupe, draw batching, upload hoisting), build flags (AVX2, LTO, PGO on the generated code), hot guest routines reimplemented natively, no spin waits | profile first (T1) | soldiers, commander for PGO/LTO design |
| T5 Game systems | reimplement what is console-shaped: streaming I/O on native threads, load hitches, LOD and culling distances for PC, 16:10 and ultrawide HUD | after T1 names the hitches | soldiers |
| T6 120 fps | the 1/120 world grid exists (`mcla_sim_cap`); make it a supported mode: timing checks, UI, physics, pacing on a 120 Hz mode | study, then runs on the panel at 120 Hz | soldier (own agent) |
| T9 DLSS 4 | Super Resolution into the existing temporal-upscaler path, then Frame Generation with Reflex wired in; a second route to G2 and G3 (owner's order) | design and code on a copy of the SDK, then build and test | commander |
| T7 Overlay and mod menu | fps counter, statistics screen, mod menu (F1) on the SDK's ImGui overlays; live cvars, presets, cheats | design + code, then build and test | soldier, grunt for the cvar inventory |
| T8 Hardware matrix | auto preset by GPU class, hybrid-GPU and refresh-rate advice at start, Intel / AMD checks | after G1 and G2 | soldier |

Order: T0 and T7 now; T1 as soon as the machine is free; T3 and T4 from T1's numbers; T2 alongside; T6 after T0's display
work lands; T5 and T8 last. Nothing is called faster or fixed without a measured run.

## Status board

| Item | State | Evidence |
|---|---|---|
| Stutter cause on the new laptop | found: `vsync=false` unlocked the guest vblank; launcher toggle removed | logs `mcla_x86_001..003`, LEARNINGS (pending entry) |
| Display-side statistics `[present-stats]` | done, agrees with PresentMon within 6 frames of 5,400 | runs `pm-win-nt`, `pm-win-vrr`, `pm-full` |
| Hitch attribution `[mcla-hitch]`, `[mcla-sys] sample`, display path, command line | done | run `final-check` |
| Thread placement on parked cores | fixed (0 warnings in 5 runs) | run `final-check` |
| G1 on a 1920x1080 @ 60 Hz mode; "no tearing" is the default | done (118 s drives; the 5 min soak is open): vsync on + tearing off = every frame exactly one refresh, 0 dropped, 0 repeats over 4 boots (about 16,600 frames, max interval 17.6 ms), display lock on, PresentMon and `[present-stats]` agree. vsync off + tearing off (the owner's run 3): 5 and 8 repeated and dropped frames in 69 s, 0 swaps on time. Tearing on: every flip taken mid-frame (ETW `FlipWaitHSync`, the tear 8-34 % down the picture). `guest_vblank_display_offset_us` 0..8000 trades latency 1:1 (16.0 -> 8.1 ms) with no repeats; default left at 0 until tested under load | runs `tv-t1-a/b/c`, `tv-t1-default`, `tv-t2-*`, `tv-t3-*`, `tv-t4-*`, `tv-t1-off*` |
| Run summaries for the new lines | done: display verdict per run, 50 parser tests | `tools/display_log.py` |
| Cvar inventory for the mod menu | done: 509 cvars, 119 marked live | scratch `native/cvars.json` |
| Claude usage watch (pause at 98 % of the 5-hour limit) | running | `tools/usage_watch.py` |
| Menu and loading hitches at every launch | improved, not gone: worst frame 167-197 ms -> 107-121 ms (best 82-87). SDK 0790 stores and preloads the render-target transfer pipelines (201 made at start in 21 ms; run-time creations 197 -> 0); they were only about 25 ms of the frame. The page faults were first touches of committed guest memory (+110 MiB in the front-end frame) plus about 74 MiB of new D3D12 allocations; the host now pre-faults the guest arena on a background thread at boot (`mcla_prefault_guest`, 1530 MiB in 431 ms, 62-81k faults -> 20-28k). Left: the D3D12 allocations of the first 1080p frame and about 7 compute resolve pipelines a run (about 30 ms, never stored). Noise between runs is about 20 ms (n=3) | runs `b-a1`, `b-b1..3`, `b-g1..3`, `b-p5..8`; scratch `pipeline-cache/`, `integ/` |
| Native roadmap study (T2, T3, T4, G3 bounds, T1 runs M1-M8) | done: `docs/native/ROADMAP.md`. Top five: the 2x path as cheap as 1x (dual write, direct present, transfer skips at draw scale 2), scale-aware off-screen targets at 2000p (shadow map first), VRS 2x2 on post at 2x, zero run-time compiles, resolve renaming. All gains are estimates from the 880M until M1-M3 run | lead spot-checked `overrides.toml` (native memcpy / memset ready but blocked by the SDK return path) |
| Overlay and mod menu (T7) | done, built and tested with real keys and mouse: F1 mod menu (Display, Graphics, Game, Cheats / mods, About; live / next load / next start per row; Save writes mcla_x86.toml and launcher.json), F2 FPS counter, F5 statistics (frames, GPU, display cadence from SDK 0755, adaptive detail, world speed, hitches, machine). Input is muted while the menu is open; hidden it draws nothing; cost not measurable at 1080p (noise 0.2 ms), about +0.3 ms at 2000p (not conclusive). Cheats now: time of day, weather, traffic and pedestrian density. Open: controller navigation, pause on open, classic cheats (money, nitro, damage, police) need reverse engineering | runs `ov-1`..`ov-17`; `docs/OVERLAY.md`; 164 unit checks |
| R1: copy levers at draw scale 2 (SDK 0760, `scaled_copy_levers`) | done for bits 1 + 8 (dual write, tone-map skip), default 9 in the 2000p profile: 0 differing bytes over about 8,450 textures, -1.35 +/- 0.18 ms; 2000p ultra capped now 11.8 ms GPU busy p50 (was 13.2). Direct present at 2x (bits 2, 4) stays off: a one-pixel wrong stripe on row 0 / column 0, worth only 0.17 ms. Rain was missing at 2000p (no unscaled-copy rule): fixed in the profile. Not covered: pause backdrop, vinyl editor, HDR at 2x | runs `r1-ver`, `r1-sw2`, `r1-px9`, `r1-ab1/2`, `r1-fin`, `r1-rain0/1/9` |
| R2+R3 (2000p shadow size, env map, VRS base) | done, built and measured on the new drive flow: shadow 320 on (-0.6 ms), env map 256 on (-0.5 to -0.6 ms), VRS base level 2 measured and OFF (-0.1 ms, stair-steps distant trees). 2000p ultra at the 60 fps target: GPU busy 11.45 ms p50 / 14.1 p95 (PresentMon), 60.00 fps, 0 missed, 0 hitches; uncapped 93 fps, GPU-bound at the 55 W cap (120 fps is 2.2 ms away) | runs `k-*`, `dr-v12-*`, `c-2000*`, `c-1080u`; METRICS section R2 / R3; scratch `r2r3/` |
| Drive flow `quick_drive2` / `bench_drive2` | done: the bike really drives (speed above 0 mph in 93 / 86 / 86 % of the 5 s samples at 2000p; first 15 s repeatable; police pursuits happen in about half of the runs and arrests in about one in four). Replaces the parked-bike flows for all drive numbers | header of `tools/flows/quick_drive2.txt`, LEARNINGS NATIVE-M |
| DLSS 4 (T9) | written, not built | scratch `dlss/` |
| 120 fps (T6) | built and measured: `mcla_fps_target=120` (profile row, launcher option, overlay row "next start", governor budget scaled with the cap). 1080p ultra, 5 min: 119.97 fps, world x0.9998, 9 missed frames of 37,821, 1 % low 111.8, CPU threads 4.4 / 4.3 / 3.4 ms a frame. The panel has NO 120 Hz mode (165 and 60 only): 120 fps is even only with variable refresh (Allow tearing: refresh clock follows, cadence 1.04, 7 % of presents dropped); at 165 Hz vsync it is a 1-2 refresh beat. 2000p: 118.4 fps with the governor at tier 1 + VRS 2 (picture cost), 90.3 without it; uncapped 106-108 fps. Open for the owner: handling at a 1/120 s physics step, race countdown / clock at 120 (the drive flow never reaches a race), audio by ear, a 120 Hz custom mode | runs `b1`..`b9`, METRICS rows `b*`, LEARNINGS NATIVE-P, NATIVE-Q |
| GPU power cap (T6 Part A) | found and lifted: Lenovo Intelligent Cooling (auto) held the RTX 4060 Laptop to 55 W of 100 W; Extreme Performance (set through Vantage's own API, `scratch/power/its_get.ps1`) gives 100 W: 2000p ultra uncapped 85.5 -> 105.6 / 108.0 fps, GPU busy 11.5 -> 9.4 ms, 2096 -> 2682 MHz, 71-76 C stable over 6 minutes. The machine is left in Extreme Performance. The game logs the GPU limit, the vendor mode and a warning (`[mcla-sys]`, NATIVE-O) | runs `p0-base`, `p1-perf*`, `p2-bal`; `scratch/power/ORIGINAL.md` |
| T1 measurement on the RTX 4060 (M1, M2, M4, capped pair, M3) | done. 2000p ultra holds 60: GPU busy 13.2 ms p50 / 13.55 p95 capped (12.9 / 13.2 over 311 s), governor never shed, 84 fps uncapped (11.6 ms). 1080p ultra: 5.32 ms, reaches the 120 Hz vblank cap. At 2000p the emulation operations are 51 % of the frame (34 % at 1080p): every resolve falls back `scaled` (51.8 a frame), no direct present, no tone-map skip: R1's prize is about 2.8 ms. CPU: no thread over 8.3 ms a frame (worst 4.7 ms, guest thread F80004DC, then GPU Commands 4.2, Main 3.5); the profile is flat (top function 3.1 %): T4 waits. CAVEAT: the scripted drive leaves the vehicle parked against a wall with this save (0 mph in every screenshot), so all drive numbers are a lower bound until the flow really drives | scratch `t1/RESULTS.md`, METRICS rows `t1-*` |
