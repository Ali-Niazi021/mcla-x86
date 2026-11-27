# Learnings

Owner: ORC (merges what tasks report). One entry per non-obvious finding, newest first:

```
### <task-id> <title>
**Symptom** … **Cause** … **Fix** … **Evidence** (file:line, log line, run id) … **Cost** (time, builds)
```

---

Native PC work (2026-11-27, new machine: RTX 4060 Laptop, 6P+8E Intel, panel 3200x2000 @ 165 Hz):

### NATIVE-A vsync=false unlocked the guest vblank: judder with flawless game-thread frame times
**Symptom** The game thread was flawless (p99 19 ms, no missed frame) while every swap was late on the RTX 4060
laptop, and the picture judders. **Cause** vsync=false makes the guest vblank free-run at 1 kHz, so the 60 fps pacer
drifts against the panel's refresh and frames are shown a refresh late. The cvar changes no presentation: the
presenter always uses sync interval 0. **Fix** Launcher vsync toggle removed; the host warns on vsync=false (3fe81e5).
**Evidence** logs `mcla_x86_001..003`; `src/host/mcla_app.cpp`, comment above `LogPresentSettings`. **Cost** three
logged boots.

### NATIVE-B Tearing allowed tears on a display without VRR; vsync on with tearing off is one refresh a frame
**Symptom** With tearing allowed on a 60 Hz mode, each frame flipped mid-refresh and the tear sat 8 to 34 % down the
picture (ETW FlipWaitHSync). **Cause** `d3d12_allow_variable_refresh_rate_and_tearing` lets a frame reach the screen
at once; only a G-Sync or FreeSync display hides that. **Fix** Tearing off in the PC profile and by default in the
launcher (775ec0c). **Evidence** Vsync on, tearing off: every frame exactly one refresh, 0 dropped, 0 repeats over
four boots, about 16,600 frames, max interval 17.6 ms (PresentMon, ETW FlipWaitVSync; runs `tv-t1-a/b/c`,
`tv-t1-default`). **Cost** 118 s drives; the 5-minute soak is open.

### NATIVE-C SetThreadIdealProcessorEx fails with error 31 on a parked logical processor
**Symptom** Thread placement on a 6P+8E Intel laptop in Balanced mode failed: SetThreadIdealProcessorEx returned error
31. **Cause** Once a thread has CPU sets, Windows refuses an ideal processor that is parked at that moment. **Fix**
The placement falls back to an unparked sibling of the core (`high_core_siblings`, `src/host/thread_policy.cpp`;
3fe81e5). **Evidence** 0 placement warnings in 5 runs (run `final-check`). **Cost** a fallback in the placement code.

### NATIVE-D DXGI frame statistics freeze while the window is covered: mark display statistics unreliable
**Symptom** The display counters stopped while the window was covered or minimised, and the lag behind
GetLastPresentCount grew. **Cause** PresentCount counts presents the compositor retired, replaced ones included.
SyncRefreshCount and SyncQPCTime, the compositor's refresh clock, stop while the window is hidden. **Fix** A window
that cannot be trusted is marked UNRELIABLE and its display numbers print '?' (0750). **Evidence** Judged against
PresentMon at 60 and 300 presents a second: `[present-stats]` within 6 frames of 5,400 (runs `pm-win-nt`,
`pm-win-vrr`, `pm-full`). **Cost** SDK patches 0750 and 0755 (the export).

### NATIVE-E 60 fps on a 165 Hz panel without VRR is a 3-3-3-2 beat by design
**Symptom** 60 fps on the 165 Hz panel looks uneven although every frame time is flat. **Cause** 165 / 60 = 2.75
refreshes a frame, so each frame stays on screen for 3 refreshes (75 %) or 2 (25 %). In the 2000p drive 4,660 frames
were held 2 refreshes and 13,903 three or more (`t1-m3-2000u`). **Fix** None in the game; the log says so once when
VRR is off. Variable refresh (G-Sync or FreeSync with tearing allowed), or a 60 or 120 Hz mode, removes the beat.
**Evidence** `src/host/mcla_app.cpp`, comment above `LogRefreshCadence`; `$WORK/scratch/t1/RESULTS.md` §1. **Cost** none.

### NATIVE-F Render-target transfer pipelines were built at every launch because they were never stored
**Symptom** About 200 pipelines were created at run time in every launch, and the first 1080p frame of a scene hitched
114 to 205 ms. **Cause** The render-target transfer pipelines (`render_target_cache.cpp:4842`, `:4883`) were counted
but never written to the storage, so every process made them again on the command processor thread. **Fix** SDK 0790
stores their keys in `<title>.rtv.d3d12.xrtp` and makes the pipelines at start: run-time creations 197 to 0 on a warm
run, 201 made at start in 21 ms. **Evidence** `$WORK/scratch/pipeline-cache/DIAGNOSIS.md` §2.1; runs `b-a1`, `b-b1..b-b3`.
**Cost** 25 ms of the 148.5 ms worst frame (`b-a1`).

### NATIVE-G The first 3D frame's page faults were first touches of committed guest memory
**Symptom** The hitch frame took about 73,000 page faults (about 285 MiB), and the worst menu or loading frame took
167 to 197 ms. **Cause** First touches of committed guest memory (+110 MiB in the front-end frame) and about 74 MiB of
new D3D12 allocations. **Fix** `mcla_prefault_guest` reads one byte of every guest page on a background thread at
boot: 1,530 MiB in 431 ms; faults 62k to 81k down to 20k to 28k; worst frame 107 to 121 ms (6d2effe). **Evidence**
`src/host/pf_probe.h` (header); the status board, row 'Menu and loading hitches'. **Cost** 431 ms of background work
at boot; no guest memory is written.

### NATIVE-H At draw scale 2 every copy lever was off; dual write and the tone-map skip are back, direct present is not
**Symptom** At 3200x2000 every resolve fell back as `scaled` (51.8 a frame), no swap went direct and no tone map was
skipped: about 2.8 ms over the 1x line. **Cause** At a draw scale above 1 the dual write, the direct present and the
tone-map skip were refused or wrong at the half-pixel gap. **Fix** SDK 0760: `scaled_copy_levers` = 9 in the 2000p
profile (dual write and tone-map skip), 0 differing bytes over about 8,450 textures, GPU busy down 1.35 ms (+/- 0.18).
Direct present stays off: a one-pixel stripe on row 0 and column 0 in 100 of 111 checks. **Evidence** runs `r1-ver`,
`r1-sw2`, `r1-px9`, `r1-ab1/2`, `r1-fin`. **Cost** 8 DXBC shaders built with fxc.

### NATIVE-I The rain was not drawn at 2000p: the profile had no unscaled-copy rule
**Symptom** The rain was not drawn at 2000p. **Cause** The 2000p profile set no `resolve_unscaled_copy_rules`, so the
rain's four 128x128 64-bit resolves reached no vertex fetch (the upstream default `mcla_rain_unscaled_copy` is not in
this host). **Fix** Rule `resolve_unscaled_copy_rules=len=131072,pitch=128,height=128,fmt=37` in the 2000p profile
(6d2effe). **Evidence** Runs `r1-rain0/1/9` (status board, R1 row); `$WORK/scratch/r1/DESIGN.md`, 'Not in the patch'.
**Cost** one profile line.

### NATIVE-J The scripted drive leaves the vehicle parked: drive numbers are a static scene
**Symptom** Every drive run shows 0 mph in every screenshot: the motorbike ends against a wall with its rear wheel
smoking. **Cause** The scripted flow's throttle slices leave the vehicle parked, with the owner's save (fixture
`after_intro`). **Fix** None yet. Drive numbers are a lower bound for driving through traffic; a real moving drive is
the next measurement. **Evidence** `$WORK/scratch/t1/RESULTS.md` §0 (runs `t1-m2-2000u`, `t1-c60-2000ultra`, `t1-m3-2000u`).
**Cost** the T1 drive numbers are read as a static scene.

### NATIVE-K A CMake configure from Git Bash produced an SDK that does not link: build from PowerShell
**Symptom** An SDK reconfigured and built from Git Bash (MSYSTEM=UCRT64) failed at the link of `rexruntime.dll`:
`lld-link: undefined symbol: __declspec(dllimport) isatty` (libavutil `log.c`) and `itoa` (SDL `SDL_string.c`); the
same command line from PowerShell links. **Cause** Not identified: the configure's function checks come out differently
in the MSYS environment (the old POSIX names resolve in the check but not in the DLL link). **Fix** Configure and build
from PowerShell; after a bad configure, `prepare_sdk.py build --clean` (`docs/AGENT_SETUP.md` §2, item 6). **Evidence**
`$WORK/logs/sdk-build-20261127-014811.log` (fails), `sdk-build-20261127-015116.log` (PowerShell, clean, links).
**Cost** two SDK builds.

### NATIVE-L The usage endpoint answers 429 to close requests
**Symptom** A second usage read 28 s after the first was refused with HTTP 429. **Cause** The usage endpoint refuses
requests that come close together. **Fix** `tools/usage_watch.py` (c9ab705): one watcher polls and writes
`usage.json`; a plain read uses its last value if it is under 10 minutes old; a 429 is waited out for 5 minutes.
**Evidence** `tools/usage_watch.py`, header. **Cost** if the usage cannot be read for 30 minutes, monitoring is off
and the lead must say so.

Sprint 2 (2026-11-26, streams LOCK60, HOST, VISUALS, HDR, MENUS60): the first block. A finding several streams reported
has one entry (the CMake preset trap, the dead PresentMon GPU column). Run ids `lk-*`, `h*run*`, `vis*`, `hdr*`/`h2-*`,
`mn*` are in the stream work trees' `runs/`; `gate*` and `q0-base` are the orchestrator's (METRICS).

### Sprint2-Process A commander that waits with a shell sleep loop can stall on a coder that already finished
**Symptom** One LOCK60 stall: the commander sat waiting for a coder whose work was done. **Cause** It polled with a shell
sleep loop instead of taking the coder's completion message. **Fix** Wait on the completion notification; no sleep loops.
**Evidence** The orchestrator's note on the LOCK60 stream. **Cost** One stall; its length was not recorded.

### Sprint2-Process The CMake preset pins MCLA_WORK, so a stream configured the main build tree
**Symptom** `cmake --preset win-amd64-release` with `MCLA_WORK=<stream tree>` stopped on "source ... does not match the
source ... used to generate cache": it had configured the MAIN build tree (HOST, VISUALS and HDR each hit it). **Cause**
`CMakePresets.json` sets `binaryDir: $env{MCLA_WORK}/build` and `environment.MCLA_WORK` to the main work tree, and a
preset's own environment wins over the caller's. **Fix** A gitignored `CMakeUserPresets.json` with a preset inheriting
`windows-base` (or `win-amd64-release`), its own `binaryDir` and `environment.MCLA_WORK`; `tools/stream_work.py` now
writes it (a45d160). Related: a local commit in `$WORK/sdk/src` makes `prepare_sdk build` refuse ("not at the pin"); to
snapshot the SDK tree for patch export use a temporary `GIT_INDEX_FILE`, `write-tree` / `commit-tree` and a tag, never a
commit on HEAD. **Evidence** `$WORK-host/host-configure.log`, `$WORK-visuals/logs/vis-cmake-config.log`; the configure
failed before anything was written.

### Sprint2-Process PresentMon's GPU columns read zero under some conditions; check them per run
**Symptom** `gpu_busy_pct` 78 % in V5m6t-m6 while ADLX showed the GPU about 95 % busy (HOST); from about 22:45 on
2026-11-25 `MsGPUBusy` was 0.0000 for every frame in every bench.py and run_game capture of VISUALS (the rows were
present); empty in MENUS60's runs. **Cause** Under heavy foreign GPU or kernel-graphics load MsGPUBusy / MsGPUWait /
MsGPULatency read 0 for about 99 % of frames with a few absurd values; the other cases are unexplained (Radeon Software
was running, no foreign ETW session was visible). It is intermittent: gate1-base (01:03 on 2026-11-26) read 79.3 %
again. **Fix** Check that the column is non-zero before using `gpu_busy`. Where it is dead use ADLX `gpu_util` x 10 /
fps as GPU ms per frame (it agrees within 0.3 ms when both exist), but `gpu_util` reads 97–99 % in every segment of a
saturated drive and cannot separate states, so for within-run A/B prefer the SDK's exported busy time (`[mcla-gov]`
lines, `gpu_timing_csv`). Flag a run TAINTED when MsGPUBusy is zero: bench.py does not yet. Under a 60 cap frame times
also understate cost differences. **Evidence** V5m6t-m6, V5m6t-m6-parked, V5m6t-warmup, V3d2-b (HOST's re-analysis),
vis6–vis10 `pm.csv`, `vis7-tour/adlx.csv`, mn1–mn3.

### MENUS60 The pause screen was 4x MSAA, not a menu cost
**Symptom** PAUSE at 27 fps at 1080p and 23 fps at 1200p (GPU p50 33 / 41 ms, 90–97 % of frames over 25 ms); every old
60 → 30 hold began at `free-roam -> pause`. **Cause** The scene pass `sub_822E5ED8` raises the MSAA mode it hands to
BeginTiledRendering (r6) to 4 for the pause backdrop (and for about 0.3 s when Start is pressed at the title), whatever
`noaa` set in renderer+384; `sub_822E5358`, a second pass that runs only in menus, does the same from renderer+380. At
4x the game tiles 1920x1080 in 7 tiles and 1920x1200 in 8, which `mcla_ws_single_tile` cannot merge (13,056 / 14,592
EDRAM tiles). The renderer flags the code tests (+373, +393, +432/+433) all read 0 at the hook while the 4x was forced,
so the forcing branch is not identified; the clamp does not depend on it. The 4x entry was also the entry hitch: the
first frames of the 7- or 8-tile pass took 224 + 265 ms at 1200p (66 ms at 1080p). **Fix** `mcla_menu_msaa.cpp` (hooks
0x822E6008, 0x822E5428), cvar `mcla_menu_noaa` (on in the PC profile, every preset): a forced 4x is drawn in the
renderer's own mode (1x with `mcla_noaa`) and merges into one tile. GPU 33 → 15.5 ms (1080p), 41 → 17 ms (1200p): the
pause costs what driving costs in the same run; one 94 ms frame remains at the cut to the pause camera at 1080p; leaving
PAUSE never stalled. The backdrop is a live orbit camera on the player's car and shows the same mild edge aliasing as
driving, no artefacts (`mn1-tour/pause_4x_vs_1x.png`). **Evidence** `mn1-tour` (segments `pause mcla_menu_noaa=false` /
`=true`), `mn3-p1200`, PresentMon frames of `mn3-p1200` 75.5–78 s and `mn2-probe` 77.5–79.5 s. **Cost** 3 runs, 1
codegen, 2 builds.

### MENUS60 The governor shed on samples from the previous screen
**Symptom** Entering free roam from loading or from PAUSE, the governor went from level 0 to 4 within 0.3–0.4 s.
**Cause** It judged the last GPU samples of the loading screen or the pause (15.8–19.6 ms) as driving. **Fix** LOCK60's
60-frame screen-change grace in the governor (merged in 57a32f2; `mcla_gov_screen_grace_frames`). **Evidence**
`[mcla-gov] level 0 -> 1 ... 3 -> 4` lines at `mn1-tour` +57.5 s, `mn2-probe` +94.0 s, `mn3-p1200` +57.8 s.

### MENUS60 The GPS map is not reachable from the after_intro fixture
**Symptom** Back in free roam did nothing (pressed for 300 ms and for 1 s, twice in two runs). **Cause** At that point
in the story the pause menu offers only CONTINUE / MODES / SETTINGS, and Back opens no map. **Fix** None: measuring the
map (and the garage) needs a later-story save as a fixture. **Evidence** shots of `mn1-tour` t114–t126, `mn2-probe`
t96–t126.

### LOCK60-W3c A cold shader cache looks like overload
**Symptom** First visits of night or fog with a fresh shader cache gave 60 → 30 holds (four in vis6). **Cause** The
first visit of a state compiles shaders and creates pipelines, a stall the governor and the hold rule read as GPU
overload. **Fix** SDK 0590 exports pipeline creations, shader translations and pipelines pending; the governor and pacer
2 excuse compile bursts (bounded: 4 s of excused time per 20 s, at most 2 excused windows in a row;
`mcla_compile_quiet_ms` 500, `mcla_compile_excuse_s` 4.0). **Evidence** lk-w3cold2: fresh cache, the VISUALS scene tour
(day / dusk / night / rain / fog), 0 holds, bursts 0.5–1.9 s per window. Not counted by 0590: the render-target cache's
own first-use pipelines (resolve and clear compute).

### LOCK60-W3b A detail tier must never be saved, and a player's own value must win
**Symptom** The tier wrote its knob cvars with SetFlagByName (source: runtime), so a save from the settings overlay
would have made the tier's values the player's. **Cause** SaveConfig saves the runtime-source, non-transient cvars.
**Fix** The tier writes its knobs with SetDefaultValue (source stays default: never saved; a knob the player set keeps
the player's value, logged as `(yours)`) and `mcla_gfx_tier` is transient. No SDK hook needed;
`mcla::gov::BeforeConfigSave()` is now redundant. **Evidence** `mcla/hooks/mcla_gfx_tune.cpp` `ApplyTier`. This removes
the risk VISUALS listed for the tier table.

### LOCK60-W3a A governor that can drop the base picture on spikes makes it flicker
**Symptom** From the perf floor (tier 1) the governor enriched to tier 2 (FXAA), shed it on a burst and enriched again
(gate1-base); with the floor at 2, single 18–19 ms scene snaps still dropped FXAA six times a minute (lk-w3cold2).
**Cause** The tail rule (3 of the last 30 samples over 15.6 ms) is right for invisible VRS steps and wrong for a visible
step below the floor. **Fix** Perf floor = tier 2; below the floor only on sustained overload (median over the line for
30 GPU frames, or 2 of 30 missed; `mcla_gov_floor_shed_frames` 30), a 30 s bounce window, and cost learning discards
spiky windows. **Evidence** lk-w3tour: tier 2 (FXAA) all through the scene tour, only VRS moves, 0 holds.

### LOCK60-W2e An always-registered toast blocked the fused gamma paint
**Symptom** 04a0's fused gamma paint was refused in every run ("a UI overlay is shown"). **Cause** ReXApp always creates
the AchievementToastDialog, so the ImGui drawer is registered all run and the presenter counted an overlay as shown
(HDR's open item 5 is the same cause). **Fix** SDK 0580 `ui_overlay_active_when_drawn` (profile on): the overlay counts
only while something is drawn; the apply-gamma pass goes (swap −0.18 ms), same picture. **Evidence** lk-fl4, lk-fl6.

### LOCK60-W2c 0450 switched off 04e0, which is why it looked like +6 ms
**Symptom** `skip_overwritten_transfers` (0450) looked useless or harmful (S1: +6 ms). **Cause** With it on, 04e0's
tone-map cut was disabled and 0450 itself did not cut the tone map (+0.4 ms there), hiding its −0.4 ms in the post
chain. The +6 ms did not reproduce. **Fix** SDK 0540: the tone map's draw takes 04e0's cut, every other rectangle draw
0450's. Post −0.25 to −0.31 ms; the magenta poison is clean by day (lk-fl2) and at night (gate2-night-hdr). **Evidence**
lk-fl1, lk-fl2, lk-fl6. PORT_STATE's old line "costs +6 ms (do not ship)" is withdrawn.

### LOCK60-W2b Two floor levers that do not pay here: deferred resolve memory writes and whole-resource clears
**Symptom** Discarding the memory stores of dual-written resolves (null UAV, not exact) saved 1.07 ms (lk-fl2), and
NumRects=0 clears were expected to hit AMD's fast clear. **Cause** The exact deferred write (0530
`resolve_defer_memory_write`) nets only −0.09 ms: the game reuses addresses for differently shaped surfaces every frame
(0x08F28000 is the 16F composite resolve and the 8888 front buffer; 0x06CFF000 is 1080p 8888 and 224²/256² targets), so
the bytes are materialised again. For clears, guest surfaces leave partly used EDRAM tiles and host targets are 1088
rows, so the strict ownership proof qualifies almost nothing, and the unprovable "pad" mode measured +0.26 ms (probably
fast-clear eliminates later). **Fix** `resolve_defer_memory_write` and `clear_full_resource` stay off. **Evidence**
lk-fl2, lk-fl4, lk-fl5, lk-fl6.

### LOCK60-W2a The emulation floor is the resolves
**Symptom** 7 ms of the saturated 1080p frame is emulation and state. **Cause** Per-operation GPU timestamps (SDK 0520):
the 15 dual-written resolves take 2.58 ms (the five full-screen 1080p ones about 1.9), EDRAM transfers 1.01, other
resolves 0.47, clears 0.47, hot-page uploads 0.43, texture loads only 0.32 after 0490, barriers 0.08. **Fix** The wave-2
levers (0530, 0540, 0580) take −0.77 ± 0.02 ms saturated (lk-fl6: 11.23 → 10.46; scene −0.33, post −0.25, swap −0.18)
and about −0.4 ms busy at the 60 cap (lk-val2). **Evidence** lk-fl1 (`gpu_timing_ops=1`), lk-fl6, lk-val2.

### LOCK60-A4 Variable rate shading only pays on post-processing
**Symptom** VRS was meant as the invisible load valve. **Cause** Post / composite / translucent / pre at 2x2 save
0.4–0.7 ms with no visible change (parked screenshot diffs at base-to-base noise level); the main scene at 2x1 or 2x2 is
0.2–0.5 ms SLOWER and visibly blurred (dither artefacts). **Fix** SDK 0510: the `vrs_levels` table is the two post
levels only; VRS is reached only through the governor. **Evidence** lk-vrs1: L1 −0.36..−0.43 ms, L2 −0.56..−0.73, scene
2x1 +0.49, 2x2 +0.18.

### LOCK60-busy At a frame cap the GPU frame's span is the CPU's frame
**Symptom** The exported GPU frame time read about 16 ms whatever the load at the 60 cap (lk-gov1) and the governor shed
to its last level at once. **Cause** The GPU follows the command processor's submissions and idles between a frame's
lists while the game produces the frame; the idle sat inside the scene phase. **Fix** SDK 0510: timestamps at every list
boundary; the exported value is the busy time (span minus the gaps). **Evidence** lk-gov2: busy 11–12 ms at level 0 in
normal 1080p driving.

### LOCK60-A3 The 60 → 30 holds of the bad bench runs were the pause menu, not driving
**Symptom** 1080p bench drives were bimodal: some had a 6 s and then a 12 s hold at 30. **Cause** Every hold in
V5m6fs-m6fs, V5m6u-m6, V5m6u-p1200 and orc-fix-p1080 started within about 1 s of `free-roam -> pause` (the flow's A tap
opened the pause menu, then 7 tiles at 4x MSAA, about 100 % missed; MENUS60) or in a load. **Fix** Pacer 2 (default
`mcla_sim_pacer` 2): judge drives only, 4 s grace after a hold or a menu, two high windows in a row, and only once the
governor has shed everything (`HoldAllowed`); and `mcla_menu_noaa` removes the 4x. **Evidence** Offline replay
`scratch/A3/replay.py` (LOCK60 tree): pacer 1 reproduces all 9 logged holds within 0.1 s, pacer 2 gives 0. **Cost** 0
game runs.

### LOCK60-A2 The fixed part of the GPU frame is emulation and pixels, not per-draw cost
**Symptom** About 10 ms of the 1080p frame did not scale with resolution or clock; P1's regression gave 1.4–1.8 µs per
draw. **Cause** In-run experiment (zero-area scissor / one primitive / draw not issued, cycled per phase group; SDK
04f0): 14.8 ms = 6.0 pixels + 0.6 vertex + 1.2 per-draw (0.2–0.5 µs a draw) + 7.0 emulation and state floor. P1's slope
was draw-correlated raster and emulation, not the command processor. **Fix** Attack the emulation floor (resolves,
transfers), not the per-draw path. **Evidence** lk-cm1 (`gt.csv`, `scratch/A2/cm_an.py`), `scratch/A2/REPORT_P1REG.md`.

### HDR-8 hdr=on made a PQ swap chain on an SDR desktop
**Symptom** `hdr=on` with Windows HDR off still made an HDR10 swap chain, which DWM converts back badly; it also blocked
04a0's fused paint. **Fix** SDK 0730: `on` behaves like `auto` when the display state is known (logged once); the last
known state is kept while a query comes back unknown (a minimized window), so neither `on` nor `auto` flaps.
**Evidence** Code path only: it has not been run on an SDR desktop.

### HDR-7 FXAA on PQ codes is the cheap HDR anti-aliasing, but it dims 1-px glints
**Symptom** FXAA was forced off while HDR10 was active. **Cause** FXAA was assumed to need SDR values. **Fix** SDK 0720:
the HDR refresher writes PQ codes plus a luma (SDR's 0.299 / 0.587 / 0.114 weights applied to PQ) into the FXAA source,
then the unchanged FXAA pass runs. **Evidence** h2-6: +0.54 ms in HDR and +0.54 ms in SDR; edges changed 33.8 % in HDR
vs 29.3 % in SDR, HUD edges 26 % vs 24 %, p99.9 unchanged. Limit: the sub-pixel blend in PQ takes a 1-px glint from 406
to 95 nits (SDR: 291 to 110), so thin glints end at about the SDR + FXAA level.

### HDR-6 The game's tone map has a night-only term that Renan's recompute lacks
**Symptom** At night (`mcla_dev_tod=23`) fused HDR was 1.43 / 1.19 / 1.12 / 1.07 times brighter than SDR in the bands up
to 0.6 of white, where they should be equal. **Cause** The recomputed game tone map differs from the real 8-bit output
on 88–98 % of pixels by up to 9 codes at night; by day it is off on 0–7 px at 5 codes and it switches at once when the
clock moves to noon. 0710 read the difference as glow and doubled it. The term itself is not identified. **Fix** SDK
0740 `hdr_scene_fused_gain`: the scene pixel is the game's own pre-UI 8-bit pixel times a gain (HDR curve / game curve,
same math), so the error cancels; the gain snaps to 1 at ≤ 1.0001, so below the knee the pixel is bit-identical to SDR,
day and night. **Evidence** h2-7 and h2-8 fused checks; parked comparison in h2-8 against SDR 1.007 / 1.000 / 1.000 /
1.001, highlights 1.31x / 1.59x; no GPU cost (swap 1254 vs 1249 µs); gate2-night-hdr: scene 0 mismatches, tone map 0–3
px at 5/255.

### HDR-5 Measure HDR cost with the per-pass timestamps, not whole-frame GPU busy
**Symptom** Whole-frame GPU-busy medians said fused HDR cost +0.1–0.3 ms (hdr3). **Cause** Phases taken at different
points of the drive measure different scenes, so the medians are confounded. **Fix** Use SDK 04f0 `gpu_timing=1` with
`gpu_experiment_cycle` (frame-exact entries) and compare the swap and ui groups. **Evidence** h2-6: swap 730 vs 200 µs,
ui 347 vs 169 µs, so fused HDR is about +0.7 ms (+0.53 the refresher, +0.18 the pre-UI snapshot); h2-8 agrees (+0.48
swap, +0.19 ui, FXAA on both sides). The same method puts FXAA at +0.54 ms, not VISUALS' +0.3 ms from whole-frame busy
(vis3). The launcher help texts, `Tiers()` and 0710's `purpose` still carry the older figures.

### HDR-4 Test HDR from dumps, not screenshots
**Symptom** Screenshots can't judge HDR: PrintWindow and BitBlt capture DWM's SDR conversion. **Cause** The real picture
is only in the PQ back buffer. **Fix** SDK 0700 `hdr_dump_dir` / `hdr_dump_at` write the back buffer as DDS plus JSON
(the levels used); `tools/hdr/hdr_analyze.py` reports nits, black floor, HUD white, gamut, banding, an SDR comparison
and preview / nits PNGs. For like-for-like comparisons use quick_drive's parked seconds (free roam to 52 s) with
frame-exact `gpu_experiment_cycle` entries and map dumps to entries through `guest_frame` in the gpu_timing CSV.
**Evidence** hdr3, h2-6, h2-8.

### HDR-3 Decode the game's SDR codes with a 2.2 power on an OLED
**Symptom** The black floor (p0.5) was 0.14–0.20 nits in HDR on a panel that shows 0.0057. **Cause** The SDK decoded SDR
codes in HDR with piecewise sRGB, which is how Windows shows SDR apps; it lifts near-blacks 2–8x against the 2.2-gamma
TV the game targets. **Fix** pc_profile sets `hdr_sdr_eotf=gamma22`. **Evidence** hdr2 dumps (sRGB) against hdr3 and
h2-8 (2.2): the floor is now 0.0009–0.009 nits; at night (h2-8, parked) 0.0036 nits, against 0.135 with sRGB.

### HDR-2 On a bandwidth-bound iGPU, fuse instead of staging
**Symptom** Renan's HDR at 1080p cost +1.8 ms of GPU (hdr2: busy median 16.71 vs 14.93 ms SDR); 21 % of frames went over
17.5 ms. **Cause** It staged everything in memory, about 146 MB per frame of extra traffic on a GPU that moves about 110
MB/ms: an RGBA16F scene image (16.6 MB written and read back), a copy of the tone map output, and a UI layer (snapshot,
clear, copy, restore). It also switched off 04a0's direct present. **Fix** SDK 0710: nothing is drawn at the tone map
draw, only its four inputs and constants are captured (a texture-cache content serial guards them); the swap's refresher
computes the HDR scene per pixel inline, the UI is found by difference against one pre-UI snapshot, and the refresher
reads the front buffer from its render target. **Evidence** hdr3: `hdr_scene_fused_check` 84 checks, 0 scene mismatches.
Cost per pass: HDR-5.

### HDR-1 Our render-marker gate hid HDR completely
**Symptom** With `hdr=on` and Windows HDR on, the game came up in HDR10 but every frame showed as SDR content at paper
white, and nothing in the log said why. **Cause** The SDK's HDR scene and UI-layer passes are driven by the guest
markers kTonemapBegin / kUIBegin. Our `MarkersWanted()` (`mcla_fsr_markers.cpp`) switched them on only for FSR or
`tonemap_transfer_skip`, so `HdrGetComposite` always chose the whole-frame SDR mode. **Fix** The gate is also on
whenever `hdr` is not `off` (the markers are NOPs and cost nothing). **Evidence** hdr1-baseline log ("HDR: the tone map
draw was found", "frames are put together from the UI layer" only with the markers on). **Cost** 1 run.

### Sprint2-HOST What hurts under background load is bandwidth and the GPU, not CPU time
**Symptom** With other programs open the game fell from 60 to 31–42 fps (V5m6t-*: 31 fps for 5 min). **Cause** In-run
A/B (h1run1, h2run2): 4 memory-bandwidth spinner processes made the GPU take 21.6 instead of 15.1 ms per frame (60 → 42
fps) and a 40 % foreign GPU load 24 ms per frame (60 → 40 fps). 6 CPU spinners kept 60 fps, but the GPU clock fell from
1913 to 1374 MHz at a flat 45 W package (CPU boost takes the shared watts). In V5m6t the GPU did about 19 ms of work per
frame at an almost unchanged clock (2528 vs 2700 MHz), and the 60 → 30 hold turned that into 30 s holds. **Fix** None
host-side for bandwidth or a foreign GPU user: the D3DKMT GPU scheduling class HIGH (`mcla_gpu_priority=2`, really
applied) changed nothing (40.10 vs 40.33 fps). The governor does help: lk-gst1 (4 CPU + 4 memory spinners) 58.76 fps
with it, 39.92 fps without. The GPU-clock loss is the open boost-cap question (PORT_STATE). **Evidence** `segments.py
h1run1-base`, `h2run2-base`; `$WORK-host/scratch/h2_analysis/` (V5m6t re-analysis).

### Sprint2-HOST Late frames under CPU load are a scheduling problem: thread policy
**Symptom** With 10–12 foreign spinner processes, 4 of 9 segments had late frames (p99 up to 55 ms, up to 12.5 % over 25
ms); game threads were sampled in the Ready state only then. **Cause** The frame-critical threads (GPU Commands, GPU
Submission, Main XThread, the busy guest XThreads: render thread, worker) run at normal priority on any core and get
pre-empted or land on the Zen 5c cores. **Fix** `src/host/thread_policy.*`, on via the PC profile: hot threads
THREAD_PRIORITY_HIGHEST, each on its own physical Zen 5 core (CPU sets of efficiency class 1 + ideal processor), compile
/ storage / timer threads on Zen 5c, the process opted out of EcoQoS. Policy on: 12 segments, 0 frames over 25 ms.
Average fps unchanged; it fixes the tail only. **Evidence** h2run2, h2run3, h2run5 (`segments.py <run> --settle 2
--from-free-roam 8`).

### Sprint2-HOST EcoQoS on the game's hot threads does not buy GPU watts
**Symptom** n/a (H3 experiment). **Cause** EcoQoS slows the threads, so they spend longer per frame. **Fix**
`mcla_thread_hot_ecoqos` stays off: +4 % GPU clock (within ADLX's 1 Hz noise) at 2.94 instead of 1.9 game cores, and p99
went from 17.3 to 18.6 ms. **Evidence** h2run3 segments 10–12.

### Sprint2-HOST A test window can stay out of the owner's way
**Symptom** n/a (owner request). **Cause** SDL3 shows its window with `ShowWindow(SW_SHOW)` (activates) unless the hint
`SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0` is set; keyboard reaches the game only through SDL window events behind a focus gate
(SDK `mnk_input_driver.cpp:484,500`; no GetAsyncKeyState, no raw-input sink). **Fix** run_game sets the env hint and
`--mcla_window_pos=bottom-right --mcla_window_behind=true` (z-order below the foreground window, no activation), muted.
PrintWindow(PW_RENDERFULLCONTENT) captures the covered window. **Evidence** every HOST run logs `[mcla-window] focus:
foreground=no`; h5run4 shots 6/6 via PrintWindow, real frames.

### VISUALS-10 Scene switches settle in 1–2 s
**Symptom** n/a (measurement). **Cause** The hour and weather change on the next frame and the night path switches with
them; eye adaptation leaves about 1 s of exposure drift; night auto-exposes brighter than dusk; the first visit of a
state with a fresh shader cache compiles shaders (four 60 → 30 holds in vis6; LOCK60-W3c). **Fix** Use `segments.py
--settle 2` and warm the cache before measuring. **Evidence** vis6 1-s screenshots, 37–52 s: the per-second luma
difference is 3.2 / 2.2 / 3.4 one second after a switch, then about 0.2.

### VISUALS-8 Time of day and weather are plain memory; check a poke's first write
**Symptom** The first time-of-day poke (base `0x8287E264`) changed nothing; the log's `(was ...)` showed ASCII `6C656374
696F6E50 6172616D` ("lectionParam"). **Cause** The base was off by 8 bytes: `lis -32120` combined with `-7572` gives
`0x8287E26C`. **Fix** Time-of-day object `T = [0x8287E26C]`: hour slots T+0x38C4 / 38C8 / 38CC (the renderer reads slot
`[0x827E5640]`), freeze byte T+0x38D1 (0 = frozen), warp flag T+0x38D8. Weather int `0x8287E134` (0 nice, 1 cloudy, 2
rain, 3 fog), auto-change byte `0x8287E138`, blend weights `0x827E1B30`; while the freeze byte is 0 weather snaps
instead of fading. Always read the `(was XXXXXXXX)` value of a new poke. **Evidence** `vis5-scenes-parked/mcla.log` (the
bad base), `vis6-scene-tour` (working).

### VISUALS-7 Motion blur halves at 60 fps; eye adaptation does not
**Symptom** At 60 fps the motion-blur streaks are half the console's length. **Cause** In the branch normal play takes
(renderer byte `[[0x8287E064]+374] = 1`, read live in vis5) the blur is built per frame: vehicle previous position =
current − velocity·dt·0.25 (`sub_822E8448`, `lfs f0` at `0x822E85F4`), camera previous position = current − 0.25 × the
per-frame camera displacement (`sub_822E8780`, `lfs f0` at `0x822E88DC`). Eye adaptation is dt-based (`ElapsedTime` = dt
from `[0x827D7508]`, `Lambda` = ln2 / half-life, `sub_8260DA68`, `0x8260E4A8`–`E54C`). **Fix** A taste option, not a
bug: a half-length streak is physically right for a 16.7 ms frame. `mcla_vis_motion_blur` uses mid-asm hooks right after
the two loads (`0x822E85F8`, `0x822E88E0`, `hooks_vis.toml`); default 1 = today's behaviour. **Evidence** vis6 / vis10,
`scratch/VIS/audit/v6_mblur_1x_vs_console.png`.

### VISUALS-6 `prepare_sdk.py rehash` failed on a CRLF checkout
**Symptom** "cannot find the sha256 line of 0600-... in series.toml". **Cause** A worktree checked out with autocrlf has
a CRLF `series.toml` and the rehash regex expected `\n`; the `.patch` files are `-text`, so their bytes and hashes were
stable. **Fix** 45c4543 makes rehash accept CRLF; with an older script write the SHA-256 by hand, then run
`prepare_sdk.py check`.

### VISUALS-5 FXAA was start-up only; now live (SDK 0600)
**Symptom** `swap_post_effect` could not be compared within one run. **Cause** `graphics_system.cpp` applied it once at
init although `CommandProcessor::SetDesiredSwapPostEffect` already supported live changes. **Fix** SDK 0600 re-reads the
cvar string on every swap, on the CP thread (D3D12 and Vulkan); it is still forced off while HDR10 output is active
unless 0720's `hdr_fxaa` applies. The same patch adds `texture_lod_bias`, live because the bias is part of the sampler
key. **Evidence** vis2 log lines `swap_post_effect: fxaa (live)`.

### VISUALS-4 GPU-busy noise inside one run
**Symptom** One 4 s segment against the next varied ±0.3–0.5 ms of GPU busy at 60 fps, and clocks drifted upward over a
run. **Cause** 1 Hz power and clock behaviour of the power-capped APU, plus other load on the machine. **Fix** Alternate
A/B/A/B segments, pool each label and compare neighbours; one A/B pair cannot resolve 0.3 ms. **Evidence**
`vis2-parked-ab/slices.csv` (its base segments span 14.95–15.41 ms), `vis3-parked-ab/slices.csv`.

### VISUALS-3 The idle camera ruins parked A/B windows
**Symptom** Same-view A/B segments diverged late in parked runs. **Cause** About 40 s after the last pad input (around
88–92 s of a quick flow) the game's idle camera starts orbiting the car; a 100 ms right-stick tap did not reliably
prevent it (in vis3 it still orbited from 92 to 102 s). **Fix** Keep parked comparisons inside about 52–88 s of a quick
flow and check the screenshots for a scene change (mean luma difference per second). **Evidence** vis2 and vis3 shot
diffs, `scratch/VIS/audit/vis2_contact.png`, `vis3_contact.png`.

### VISUALS-2 A flow's `# fixture:` header was not applied for a flow given by path
**Symptom** `run_game --flow <file path>` booted to "A Midnight Club: Los Angeles save game could not be found" and free
roam was never reached (vis1-parked-ab; `summary.json` showed `fixture: null`). **Cause** The header only documented the
fixture. **Fix** `run_game.py` now defaults `--fixture` to the flow's header (HOST, f9679ab; `--fixture none` = none);
with an older copy pass `--fixture after_intro`. **Evidence** `vis1-parked-ab/shots/t60.png`, `summary.json`.

### Owner-report Skipping a render phase by its draw-list mask still clears and resolves its target
**Symptom** With `mcla_gfx_preset=perf` the car paint alternated bright/dark and far shadows flickered every other frame.
**Cause** `mcla_gfx_env_interval=2` / `mcla_gfx_far_cascades_interval=2` clear bits in `[renderer+448]`, which only gates
the 13 draw-list objects. The phase loop `sub_822E6408` still calls each phase's own object (`renderer[k]`, vtable +4 begin
at `0x822E6800`, +8 end at `0x822E68E4`): it sets the target, clears it, draws nothing and resolves, so the env map / atlas
quarter is overwritten with an empty target on every "skipped" frame (resolve log: env resolves in 726 of 731 frames).
**Fix** The `sub_822E6408` override zeroes `renderer[k]` of each skipped phase around the call (`mcla_fsr_markers.cpp`);
cascade 3 is never skipped (zeroing its slot drops the "end of last cascade" call and the player's car body is not drawn).
A still picture is not a flicker test. **Evidence** `$WORK/scratch/ORC/burst.py` (24 window captures in 1 s of the parked
scene: 2.6 % of pixels with a temporal std > 6 before, 0.2 % after = the level without the preset), `$WORK/runs/s7-env2-rlog`.

### Owner-report One late burst costs 18 s at 30 fps (the 60 → 30 hold)
**Symptom** 1080p drive runs are bimodal: 0.06 % or 3.5 % of frames over 25 ms. **Cause** Not the GPU clock: one 2 s
window with > 10 % missed frames starts a 6 s hold at 30 (`DecideHold`), and in 3 of 5 cases the first window after the
hold reads 55–100 % missed, which starts a 12 s hold. **Fix** Open (PORT_STATE Todo 1). **Evidence** `[mcla-sim] cap 60 ->
30` lines of `V5m6fs-m6fs`, `V5m6u-*`; none in `orc-ab-*`. Also: `bench.py` runs windowed unless `--cvar fullscreen=true`.
**Update 2026-11-26** The "late burst" was the pause menu's forced 4x MSAA or a load, and the fix is in: LOCK60-A3
(pacer 2) and MENUS60 (`mcla_menu_noaa`); a 5-minute re-run is still to do.

### Owner-request Native 3200x2000 = a 1600x1000 guest drawn at 2x2
**Symptom** A 3200x2000 guest back buffer cannot exist (5,000 EDRAM tiles per surface, reach 2,048). **Fix** Profile
`2000p`: the `1000p` guest with `draw_resolution_scale_x/y=2` (`resolution_scale` is ignored as a default: it is only read
when set by hand). The picture is correct (HUD, Vert+); 23–26 fps on the 880M, GPU 100 % busy; dual-write resolves and
the direct present fall back at a scale above 1. **Evidence** runs `orc-native-n2000`, `orc-native2-*`, `orc-flk-n2000`.

### Sprint P1/R6 Where the 880M's time goes
**Symptom** 1080p single tile ~17.8 ms GPU; game-side detail cuts barely moved it. **Cause** (1) the package is capped at
~35 W, so the GPU runs ~1.25–1.6 GHz of 2.9 and loses more when any other CPU work runs (agent builds, rga, VS Code,
Defender); (2) ~6.5 ms of the frame is emulation work, mostly the resolve → shared memory → texture reload round trip
(~3.5–3.9 ms), frame-end page re-uploads (25–30 MB/frame, ~2,200 copies that split passes into drains), EDRAM transfers,
swap. **Fix** 0490 dual-write (−0.6 ms), 04a0 swap from the RT (−0.35), `clear_memory_page_state=false`, quad lists as
triangles; window pixel-exact (a "1080p" window was 2880x1620 at 150 % DPI). **Evidence** P1/R3/R5/R6 reports, METRICS
V3d-*. **Cost** a day of swarm work.
**Update 2026-11-26** The floor is split per operation (LOCK60-A2, LOCK60-W2a: 7.0 ms, resolves 2.58) and the per-draw
slope of 1.4–1.8 µs was not command-processor cost (0.2–0.5 µs a draw).

### Sprint S1 Upscaling loses on a power-capped iGPU
**Symptom** FSR 3 (720p→1080p) 49 fps, FSR 4 (540p→1080p) 37 fps, native 1080p 55+. **Cause** the FSR 3.1 pass is 4.1 ms
and FSR 4.1 6.4 ms on the 880M, more than the smaller scene saves (68 % of the frame doesn't scale with pixels).
**Fix** native resolution + emulation fixes. **Evidence** METRICS s1-fsr3, s1c-fsr4p.

### Sprint R1/R8 The DXBC translator is the next big lever
**Symptom** ~25 VALU per Xenos instruction slot in pixel shaders. **Cause** the 0×anything=0 emulation (27 %), no
`refactoringAllowed` (no FMA), `sample_d` with hand gradients, serial texture fetches, the control-flow loop; vertex
fetch is a chain of branches. **Fix** 04b0 (PS options, −48 % VALU with all five), 04d0 (VS straight-line fetch), behind
cvars folded into the shader modification so caches never mix. **Evidence** RGA gfx1150 on the 1,061-shader corpus.

### Sprint Process Benchmarks need the machine to themselves
**Symptom** the same variant measured ±2 ms apart; world/real collapsed mid-run. **Cause** builds/rga outside the heavy
lock (`prepare_sdk --work` used a scratch lock), and the APU's shared power budget. **Fix** global lock for every
CPU-heavy job, telemetry (GPU clock, package W, non-game CPU) and a TAINTED flag in every bench row.
**Update 2026-11-26** When the machine cannot be quiet (other streams, the owner working), trust only within-run A/B:
`--schedule` + `tools/segments.py`, with alternating segments (Sprint2-HOST, VISUALS-4).

### W3.4-fix Present jitter was the CP's Sleep(1) poll, not the display
**Symptom** M4 p99 35.1–35.2 ms (limit 35.0) at cap 30 with an exact guest vblank on an exact 120.000 Hz panel.
**Cause** `MsBetweenPresents` lag-1 autocorrelation −0.50 = independent per-swap delay; the swap gate's `WAIT_REG_MEM`
(wait 0x100) is served by `Sleep(1)` polls (`command_processor.cpp:2305-2325`), so the release is seen 0–2 ms late.
**Fix** Patch 0430 wakes it with `WaitOnAddress` from `MarkVblank`; 0440 locks the guest vblank to a line fitted to
`WaitForVBlank` wakes (wake scatter sd 0.155 ms: never fire on a raw wake). **Check** acf1 before blaming a beat.
**Evidence** `$WORK/scratch/W3.2/present_jitter.md`, probe: Sleep(1) consumer p99 35.5 vs WaitOnAddress 33.44.

### W3.2-fix MMCSS join rewrites the thread priority
**Symptom** Log said "time-critical priority set" but the vblank thread ran at kernel priority 17 (MMCSS), not 15.
**Cause** `AvSetMmThreadCharacteristicsW` overwrites a priority set before it. Separately, guest interrupt code ran at
TIME_CRITICAL under the guest locks (G3-REV). **Fix** Join MMCSS first, then `SetThreadPriority`; drop to ABOVE_NORMAL
around `MarkVblank()`. **Evidence** `$WORK/scratch/W3.2/mmcss_probe*.cpp` (NtQueryInformationThread, 300 toggles).

### W3.3 Frame cap at the swap; a late frame must not move the grid's phase
**Symptom** n/a (design). **Cause** The SDK has no swap callback; the only no-patch swap-side point is the guest present
hook `0x82419AA0` (Renan paces there too). A limiter that re-anchors at a late frame holds every later frame past its
vblank. High-resolution waitable timers wake 0.45–0.85 ms late (p99) with a 300–400 µs spin tail, 0.04–0.11 ms with
1000 µs. **Fix** Phase-preserving slot skipping, grid 25 ppm short, 1000 µs tail. **Evidence** `src/host/frame_cap.h`,
`$WORK/scratch/W3.3/test_frame_pacer.cpp` (46 checks).

### W4.1 The iOS "60 fps profile" is two cvars
**Cause** `catalog.cpp:120` sets only `ios_fps_cap=60` + `mcla_sim60=true`; smoothing, 30 Hz UI, race clock, pacer and the
A5 interval are hook defaults. Start-up log lines read cvars in `OnPostInitLogging`, before `OnPreSetup`. **Fix**
`mcla_fps_target=60` lists them all and is applied in `OnConfigurePaths` (TOML-only values later, with a note).

### W5.1 1080p sizing facts
**Cause** `SetDefaultValue("resolution")` is a silent no-op (the SDK compares against the new default); the PC
profile's `render_target_max_height=1024` cuts a 1080-row buffer; at 1x the game tiles 1080p and 900p as **2** tiles
(only W x 720 up to 1600 is one), so single-tile rendering matters for M6; RT pool grows x2.667 at 1080p. **Fix** Presets
set `video_mode_*`/`window_*` and `render_target_max_height` 1088/912; `mcla_ws_single_tile` hook (E6 lever).

### W0.7 The Radeon 880M is GPU-bound on this game
**Symptom** Renan's build: 69.6 fps D3D12 / 58.1 Vulkan at 720p uncapped, 22–25 fps at 1440p or FSR-1080p.
**Cause** GPU busy 99.9–100 % in every uncapped run; the CPU is not the limit. FSR 3.1 costs more than it saves here.
**Fix** D3D12 default; GPU levers before 1080p; expect 900p + upscale. **Evidence** METRICS E2-a…f,
`$WORK/runs/E2-*/pm.csv`. **Cost** 52 min (build 7.5 min).

### W0.7 Building Renan's port from bash
**Symptom** `make_pc.ps1:156` "Get-FileHash is not recognized". **Cause** Windows PowerShell 5.1 inherits PowerShell 7's
`PSModulePath` from the bash environment. **Fix** `env -u PSModulePath powershell …`; `-ToolsDir` with a
`clang+llvm-20.1.8-*` junction to our LLVM and a copy of VS's `ninja.exe` avoids `-InstallTools`. A new profile stops at
"save game could not be found": make one save, restore a snapshot per run. **Evidence** `$WORK/logs/E2-build-*.log`.

### W1.2 SDL3 is private inside `rexruntime.dll`
**Symptom** An SDL virtual joystick created by the exe would never reach the SDK's input driver. **Cause** SDL3 is
linked statically into `rexruntime.dll` with 0 `SDL_*` exports; the exe would get its own SDL copy. **Fix** The scripted
pad is a synthetic `rex::input::InputDriver` (as Renan's dev pad). Also: `find_package(rexglue)` must skip the CMake user
package registry (Renan's E2 SDK registers itself there). **Evidence** Renan's `rexruntime.dll` export table.

### W1.3 Hook signatures and Win32 waits
**Symptom** n/a (pre-empted). **Cause** The codegen declares each mid-asm hook as `extern void|bool NAME(PPCRegister&…)`
with C++ linkage; MSVC mangling encodes the return type, so a `bool`/`void` mismatch is a link error. `WaitOnAddress`
and `Sleep` count whole milliseconds and round to 15.6 ms without `timeBeginPeriod(1)`. **Fix** AST-dump check of all 56
hooks; host sets the timer resolution. Guarded guest reads use `ReadProcessMemory(GetCurrentProcess())` (race-free,
bypasses the SDK's vectored handlers). **Evidence** `$WORK/scratch/W1.3/check_hooks.py`.

### W1.1 / W0.2 Windows tooling traps
**Symptom** Lock deleted under a running build; patches failing to apply. **Cause** omp's bash `$$` is a synthetic PID
(≥2^30), so hand-written locks look stale; `git apply` of LF patches needs the checkout's `core.autocrlf`. **Fix** Take
locks only via `tools/heavy_lock.py`; run every git command in the SDK tree under one fixed config (`GIT_CONFIG_COUNT`);
record applied patches by SHA-256 and `reset` instead of reverse-checking. `.gitattributes` keeps `*.patch` byte-exact.

### G0-REV The guard missed real generated code
**Symptom** An SDK-shaped `nfsmw_recomp.12.cpp` passed the guard. **Cause** ReXGlue v0.10.0 never writes `PPC_FUNC`; it
writes `DEFINE_REX_FUNC(`/`REX_FUNC_PROLOGUE`/`registrar->SetFunction(0x`. **Fix** Markers and path rules updated.
**Evidence** SDK `src/codegen/codegen_writer.cpp:293,370,482`.
