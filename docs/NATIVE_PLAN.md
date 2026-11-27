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
| Pipeline cache misses at boot (176 ms menu hitches) | diagnosed: about 197 render-target transfer pipelines per run (`GetOrCreateTransferPipelines`, 8 stencil-bit variants + 1 per key on a GPU without pixel-shader stencil reference) are built on first use and never stored; draw pipelines persist correctly (created minus appended is constant: 203, 203, 197, 197, 197, 197, 204). Pipelines explain 45-115 ms of a 115-205 ms hitch; the rest follows about 73,000 page faults in the same frame (lead hypothesis: first touch of the game's 267 MiB physical pool, 68,480 pages). Fix queued for the machine: store and preload the transfer keys (proposed 0600), report line (0610), then find and pre-fault the faulting range | scratch `pipeline-cache/DIAGNOSIS.md` |
| Native roadmap study (T2, T3, T4, G3 bounds, T1 runs M1-M8) | done: `docs/native/ROADMAP.md`. Top five: the 2x path as cheap as 1x (dual write, direct present, transfer skips at draw scale 2), scale-aware off-screen targets at 2000p (shadow map first), VRS 2x2 on post at 2x, zero run-time compiles, resolve renaming. All gains are estimates from the 880M until M1-M3 run | lead spot-checked `overrides.toml` (native memcpy / memset ready but blocked by the SDK return path) |
| Overlay and mod menu (T7) | design and first code running | - |
| R1 (levers at draw scale 2), R2+R3 (2000p shadow size, VRS base), DLSS 4 (T9) | code being written without the machine; integration queued | scratch `r1/`, `r2r3/`, `dlss/` |
| 120 fps (T6) | queued | - |
| T1 measurement on the RTX 4060 (M1, M2, M4, capped pair, M3) | done. 2000p ultra holds 60: GPU busy 13.2 ms p50 / 13.55 p95 capped (12.9 / 13.2 over 311 s), governor never shed, 84 fps uncapped (11.6 ms). 1080p ultra: 5.32 ms, reaches the 120 Hz vblank cap. At 2000p the emulation operations are 51 % of the frame (34 % at 1080p): every resolve falls back `scaled` (51.8 a frame), no direct present, no tone-map skip: R1's prize is about 2.8 ms. CPU: no thread over 8.3 ms a frame (worst 4.7 ms, guest thread F80004DC, then GPU Commands 4.2, Main 3.5); the profile is flat (top function 3.1 %): T4 waits. CAVEAT: the scripted drive leaves the vehicle parked against a wall with this save (0 mph in every screenshot), so all drive numbers are a lower bound until the flow really drives | scratch `t1/RESULTS.md`, METRICS rows `t1-*` |
