# Learnings

Owner: ORC (merges what tasks report). One entry per non-obvious finding, newest first:

```
### <task-id> <title>
**Symptom** … **Cause** … **Fix** … **Evidence** (file:line, log line, run id) … **Cost** (time, builds)
```

---

Native PC work (2026-11-27, new machine: RTX 4060 Laptop, 6P+8E Intel, panel 3200x2000 @ 165 Hz):

### NATIVE-CHEATS The game ships its own cheats: find them before writing any
**Symptom** The Cheats tab listed money, nitro, damage and police as "needs the game's code studied" (no addresses). **Cause** The game's
string table names its cheat system (`cheat_Unlimited_Nitro`, `unlimitednitrous`, `nosave`, `unlockraces`, `mcExperienceManager_AddMoney`
...), and a cross-reference index of the generated code (every `lis` + `addi` / load / store address, 3.7 s in one Python pass) leads from a
name to its 540 sysParam nodes (`+4` = the value word), to the cheat-byte block `0x8288E5A7..0x8288E5BE` and to every reader. The pause
menu's Cheats page flips the same bytes. A drive with one on is "cheating" (`sub_82387A98`): no reputation, no money. **Fix** Hold the game's
own bytes / words from the heartbeat hook only while a drive is on screen and give the original back (`mcla_cheat.cpp`, no mid-asm hook,
no codegen run); multiply the timer's scale (`+0x54`) instead of replacing it; money is one s32 at `[[0x8287484C]+0x1FC]`. **Evidence**
`docs/CHEATS.md`; method: strings, xrefs, then read the consumers. **Cost** a static session; nothing was built or run.
**First run (2026-11-27, `docs/CHEATS.md` "Results of the first run")** The bytes, the timer word and the parameter words were right the first time; one
static misreading was caught only by the running game: the money row's vtable check (`sub_822BFE30` runs the base constructor `sub_826BA578`,
which sets `0x820BBDC4`, and then stores the derived class's `0x82039058`; no object with `0x820BBDC4` exists), so money would always have been
refused. A constructor's vtable store is not the object's final vtable when a caller overwrites it. How it was found and checked without any
in-game tool: a read-only Python probe (`ReadProcessMemory` on the game process; the 4 GiB guest mapping is found by testing candidate bases
against a known pointer, `[0x8286D874]` -> the string "nosave"; base `0x100000000` here) logging the cheat bytes, the timer word, the money
field and the nitro objects (found by their vtable `0x82046F90`, bars = s16 at `+18`) four times a second next to the screenshots.

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

### NATIVE-M A blind pad script can drive this save: reverse pulses, a 0.7 throttle, and B before A
**Symptom** The drive flows (`quick_drive` / `bench_drive`) leave the motorbike nose-first against the wall across the street after 3 s
(0 mph in every screenshot of every run): all drive numbers so far were a static scene. **Cause** Nothing in the script reverses or steers
away from an obstacle. Trying variants (about 50 runs, the speedometer read in the screenshots) showed: full throttle (65-90 mph) wedges the
bike for good at a building corner (reverse gear shows 0 mph, rear wheel spinning, 40+ s) and speeding past police starts a pursuit and an
arrest (ARRESTED screen, ~20 s); rt=0.45 only creeps (2-9 mph, gear 1); rt=0.7 gives 40-46 mph (the HUD shows `CR` at the speed limit);
no button resets the bike (X is nitrous); the DELIVERY job dialog the bike opens by riding over a job marker freezes the drive and is closed
by B (BACK), but A on it ACCEPTS the job, so a B tap goes before every A tap; reverse pulses shorter than 2 s do not free the bike and the
forward start after a pulse needs a 1.2 s counter-lock to slide along the obstacle; the title screen shows 20 s after the start in one boot and
28 s in the next, so the Start taps stay at 28..34 s. **Fix** `tools/flows/quick_drive2.txt` / `bench_drive2.txt`: 11 s of full throttle onto
Selma Ave with a left tap against the Food Mart corner, then 6.2 s units (2.0 s reverse with the stick locked to alternating sides, 4.0 s at
rt=0.7 with a 1.2 s counter-lock, B at +0.6 s, A every second unit). Speed above 0 mph in 93 / 86 / 86 % of the 5 s samples in three 2000p runs;
a police pursuit starts in about one run in two and an arrest in about one in four (10-20 s lost), so check the speedometer in the screenshots.
**Evidence** runs `dr-*`, `dr-v12-1..3`, `c-2000u-s`; header of `quick_drive2.txt`; scratch `drive/`.

### NATIVE-N The RTX 4060 Laptop's clock moves 1800-2500 MHz between runs: lock it for A/B, read group deltas
**Symptom** The same configuration read 9.7 ms GPU busy in one run and 10.8 in the next (tone map group, a fixed full-screen pass: 0.54, 0.69,
0.77, 1.05 ms in different runs); a lever table from runs minutes apart was wrong by more than the levers. **Cause** The GPU is power-capped at
55 W (clocks_event_reasons 0x4): under load it runs 1900-2000 MHz when the chassis is hot (73-82 C) and 2500 MHz when cool or lightly loaded; at
the 60 fps cap the clock is 2200-2330 MHz and a capped frame costs more than a saturated one. No NVIDIA telemetry was captured before.
**Fix** For A/B: `nvidia-smi -lgc 1800,1800` through the broker (accepted on this laptop; reset with `-rgc`), an `nvidia-smi -l 1` sampler per run
(`scratch/r2r3/runit.py` writes `scratch/r2r3/nv/<run>.csv`), at least two replicates, and the groups a lever touches (shadow, pre, env, post)
rather than the frame p50: the `scene` group alone varies 2.7-3.7 ms between identical runs because the route differs after the first 15 s.
Absolute numbers for the product (Part C) are taken with the clock free. **Evidence** METRICS rows `k-*` (locked) against `dr-v12-*` / `r23-*`
(free), `scratch/r2r3/nv/`.

### NATIVE-O The RTX 4060 Laptop was held to 55 W of its 100 W by Lenovo's default thermal mode; Extreme Performance lifts it (+23 % fps) and no NVIDIA tool can
**Symptom** 2000p ultra uncapped sat at 85 fps, GPU-bound: 54.8 W mean (p95 55.0), `clocks_event_reasons` 0x4 (software power cap) in every sample, 1920-2250 MHz at 67-75 C; `nvidia-smi -q` said
power limit 55 W (default 55, enforced 55, min 5, max 100) and `nvidia-smi -pl` answered "not supported in current scope". The charger was not the limit (battery discharge 1.1 W under load), nor the
Windows side. **Cause** The machine is a Lenovo IdeaPad Slim Pro 9 16IRP8 (83C0). Lenovo's Intelligent Cooling (the "Lenovo Notebook ITS Service", `LNBITSSvc`, ITS v5) was in its default mode (auto) and
the platform (NVIDIA Dynamic Boost, `nvpcf`) gives the dGPU 55 W there. Windows' power plan and the Windows power-mode slider do nothing to it: with the High performance plan the slider is switched off
(`PowerGetEffectiveOverlayScheme` reads Balanced although the AC slider says Best performance; the game's `[mcla-sys]` line printed "Balanced"), and a Balanced plan with Best performance changed neither
the GPU's power nor its clock (p2-bal). **Fix** Switch Lenovo's mode to Extreme Performance. There is no WMI class for it on this machine (`LENOVO_GAMEZONE_DATA` only has the hybrid-GPU methods), so it is
set through the vendor's own addin: the managed `IdeaNotebookAddin.PowerAgent.SetITSMode` of Lenovo Vantage (`...\Vantage\Addins\IdeaNotebookAddin\1.0.13.114\IdeaPowerAgent.dll`, a C++/CLI mixed assembly: Windows PowerShell
5.1 only, elevated, on a thread with a 256 MB stack, else StackOverflowException), `scratch/power/its_get.ps1 get | all | set <1 auto | 2 battery saving | 3 extreme performance>` through the broker. The registry mirrors it
(`HKLM\SYSTEM\CurrentControlSet\Services\LITSSVC\LNBITS\IC\MMC`: CurrentSetting 0 = auto, 1 = battery saving, 3 = extreme performance; the game reads it). Result with nothing else changed: enforced limit 100 W, 2000p ultra
uncapped **105.6 / 108.0 fps** (+23.5 / +26 %), GPU busy 11.5 -> 9.4 ms, 82.7 / 84.6 W, 2682 MHz (was 2096), 69-75 C; 6 minutes (bench_drive2) 112.7 fps, 87 W, 2684 MHz, 71-76 C, no thermal throttle reason in 614
samples. Cost: about 30 W more at the wall, a louder fan (not measured), and battery discharge up to 5-8 W on peaks (the charger's wattage is not readable: the vendor API returns none for it; the battery assists when the load peaks above it). The enforced limit moves with the CPU load (Dynamic Boost): 93.7 W at start,
71-77 W at 1080p, 87-91 W at 2000p. The game now logs it: `[mcla-sys] gpu: ... power limit N W enforced (board default, range)`, the Lenovo mode, and a WARNING when the limit is under 70 % of the maximum
(`system_status_rules.h`, 50 unit checks). The mode is also what Fn+Q cycles; it survives a reboot (registry) but was not tested across one. **Evidence** runs `p0-base`, `p1-perf`, `p1-perf-b`, `p2-bal`, `p1-perf-bench`,
METRICS rows `p*`, `scratch/power/ORIGINAL.md` (the original state and the restore command). **Cost** About 12 runs, one elevated PowerShell script.

### NATIVE-P 120 fps on the 165 Hz panel needs variable refresh: there is no 120 Hz mode, and with tearing allowed G-Sync shows it evenly
**Symptom** T6 asked for a 120 Hz display mode for the 120 fps checks. **Cause** The panel (Lenovo LEN8BA2) lists only 165 Hz and 60 Hz modes at every resolution (EnumDisplaySettings; `ChangeDisplaySettingsEx` with 120 Hz returns
DISP_CHANGE_BADMODE, -2, at 1920x1080 and 3200x2000); its EDID range limits are 48-165 Hz vertical, so it is an adaptive-sync panel. 120 on 165 is 1.375 refreshes a frame: with vsync and tearing off every frame stays 1 or 2
refreshes (`[present-stats]` cadence 1.38, MsBetweenDisplayChange p50 6.07 p99 12.19 ms). **Fix** None needed in the game: with `d3d12_allow_variable_refresh_rate_and_tearing=true` (launcher: Allow tearing) the display followed the presents:
refresh clock 124.25 Hz against the 165 Hz mode, cadence 1.04, MsBetweenDisplayChange p50 8.33 p99 10.26, no tearing seen in PresentMon (G-Sync is on for this panel in the NVIDIA driver; it was only observed through the SDK's own statistics, no screen
capture of the panel). 7.4 % of presents were dropped (replaced before display) and 12 % of frames stayed 2 refreshes: even, not perfect. A 120 Hz custom mode (NVIDIA Control Panel or an EDID override) was not tried: persistent
driver change, left to the owner. `mcla_fps_target=120` sets mcla_sim_cap=120, video_mode_refresh_rate=120, the 60 fps rows (UI at 30 Hz, k(dt) smoothing, race clock, pacer), motion blur x2 and the governor budget scaling.
At 1080p ultra: 119.93 fps, world x0.9999, n1 8397 of 8402 frames, 1 % low 110.8, 5 minutes (b8) 119.97 fps, 9 missed frames of 37821; the CPU threads need 4.4 / 4.3 / 3.4 ms a frame (guest, GPU Commands, Main XThread), GPU 6 ms.
`mcla_cpu_pump_wake` does nothing measurable at 1080p (ring waits 17 -> 5 in 70 s, frame times equal), so it is not in the profile. **Evidence** runs `b1-1080-120`, `b2-1080-120-vrr`, `b3-1080-120-wake`, `b8-1080-120-bench`,
`scratch/power/dispmode.py` (list / hold / edid), `bsum.py`.

### NATIVE-Q The governor's budget was written for 16.7 ms; at 120 it never sheds on GPU time and restores into a miss. 2000p at 120 is only reached at tier 1
**Symptom** At a 120 target the adaptive-detail governor shed only on "2 of the last 30 frames missed" and then restored a rung every 8-10 s (busy p90 8.3 ms < its restore line 15.0), missed, and shed again: 13 tier changes and 3
restores in a 70 s drive (b6). **Cause** `mcla_gov_shed_ms` 15.6, `mcla_gov_restore_ms` 15.0 and `mcla_gov_enrich_margin_ms` 1.5 are the 60 fps frame's numbers and nothing derived them from the world's cap. **Fix** `mcla_gov_math.h`:
`BudgetScale(cap)` = 60 / cap above a cap of 60 (1 at 60 and below, so 60 fps is bit for bit unchanged), `ScaleThresholdMs` keeps the 0.7 ms presenter paint (shed 7.45, restore 7.15 ms at 120), the margin halves; `MakeParams` applies it
with `mcla::sim::EffectiveCap()`, so a 120 -> 60 tier or a 30 hold puts the 60 fps budget back while it lasts. 25 new checks in `test_gov.cpp` (a 9.5 ms GPU: nothing at cap 60, sheds within 2 s at 120; 7.0 and 7.3 ms hold; 6.0 ms restores).
Result at 2000p ultra / 120 (b4): the ladder goes 6 rungs in 0.6 s to tier 1 + VRS 2 and stays (6 changes, 0 restores), 118.4 fps, 1 % low 91.4, GPU busy 8.1-8.3 ms. Without the governor (b5) the GPU (9.4 ms at ultra) is 1.1 ms over the
budget: 90.3 fps, the swap gate turns slow frames into 16.7 ms ones, and the world falls to its 60 tier three times. So 2000p at 120 is reachable only at the lowest detail tier (reflections and far cascade every 2nd frame, shadow distance 0.6, city LOD 0.67,
traffic LOD 0.7, FXAA off) plus VRS: it costs picture quality. **Evidence** runs `b4-2000-120-gov`, `b5-2000-120-nogov`, `b6-2000-120-gov60thr`, `b7-2000-60` (60 fps unchanged: shed above 15.6 ms, 0 missed), `test_gov.cpp` `TestBudgetFollowsCap`.

### NATIVE-R PowerGetEffectiveOverlayScheme says Balanced under the High performance plan: the log named the wrong mode
**Symptom** The `[mcla-sys]` line printed "Windows power mode: Balanced; plan: High performance" while Settings showed Best performance. **Cause** The effective overlay is 00000000 (none) whenever the active plan is not Balanced; the slider's
own value is `PowerGetActualOverlayScheme` (ded574b5 = Best performance here, `ActiveOverlayAcPowerScheme` in the registry). **Fix** The line now reads "Balanced in effect (the slider says Best performance, but the High performance plan switches it off)",
and the warning only asks for Best performance when the plan is not High / Ultimate performance. **Evidence** `system_status_rules.h`, `test_system_status.cpp`.

### NATIVE-S The front-end hitch was the first use of 94 tile-heap windows and 35 render targets, not 19 windows; the 256 MiB budget missed half, and what is left is page faults of upload pages
**Symptom** After 0790 and the guest pre-fault the first 1080p scene still took 89-127 ms (median 113), with 13k-35k page faults, 7 compute pipelines and 64+ tile heaps made inside the frame. **Cause** (1) The direct resolve compute pipelines
were made on first use but cost only 0.7-1 ms each (5-7 ms for the 7 in all, not the 4-8 ms each that was assumed), so storing them (0793) is worth about 5 ms. (2) The shared memory's 4 MiB tile heaps and the host render targets are made on the first
request of each: a 118 s drive needs 94 windows (376 MiB) and 35 render targets (236 MiB), not the 19 windows the logs of the first scene named (uploads allocate windows too, not only resolves); they cost 27-37 ms in the first 64 heap lines plus
the render target clears, and are created again every run. (3) What is left is page faults: the frame is 21 ms + 2.1 us per fault when warm (n = 10, R2 0.83; cold 70 ms + 1.85 us, n = 12) and the fault count (11k-35k) is the number of fresh
upload-buffer pages (2 MiB, written for the first time by the command processor: shared-memory upload pool, constant-buffer pool) that the frame needs because the GPU has not finished earlier submissions; the scene also creates about 600
textures (`tex_n` 22 -> 590-664, 126-132 MiB) in that frame. **Fix** SDK 0793 (the 7 pipelines made at start from the key file, 5-7 ms) and 0794 (kinds 5-7 of the same file: render targets, shared-memory windows, scaled-resolve heaps; made at start
within `d3d12_warmup_mib`, default 1024 MiB x the draw-scale area and at most a quarter of the free video memory; `d3d12_warmup_seed` lets a first run start from a key file of a warm run). Results (3 runs each, same day and power mode): worst
front-end frame 113 -> 109.5 (0793 alone) -> 81 ms (53-87); 129 objects / 612 MiB in 31-35 ms at start; run-time heap lines 64+ -> 0; boot +37-40 ms; video memory +126 MiB (1080p), +682 MiB (2000p, 3.56 of 7.19 GB). The first draft of 0794 had a
256 MiB default budget (sized for the 19 windows): it made 51 of 125 objects and left the frame at 117 ms; the cost of making everything is so small (31 ms) that the budget only needs to be a guard. **Not fixed, measured** The first engine
frame (60-80 ms: game thread waiting, GPU 0.3 ms, +83 faults, same windowed) and the load (42-57 ms, game thread on the CPU for 38-42 ms) are not allocations. Pre-growing and pre-touching the two upload pools at start (experiment only, 96 + 48 pages,
+46-54 ms at boot) made the scene frame 40.7 / 40.0 ms with 2k faults; with only the shared-memory pool 52.5 / 68.6 ms, because the constant-buffer pool made 21 more pages inside the frame: that is the next lever (record the pool high-water marks in the
key file, make and touch the pages at start), and the 600 textures are what would remain. **Evidence** runs `h2-o1..3`, `h2-k1..3`, `h2-r1b / r2 / r3`, `h2-p0 / p1` (`[mcla-pf]`: 173 MiB newly committed in 8 resource-sized ranges, 41k faults), `h2-g0 / g1`, `h2-2k0 / 2k1 / 2koff`, `h2-s0 / s1`,
`h2-x1..6` (upload pool page log and the pre-grow experiment); METRICS 'Menu and loading hitches: SDK 0793 and 0794'. **Cost** 3 full rebuilds of the SDK series (2-5 min each) and 3 incremental ones for the experiment, 37 runs of 2.5 min.

### NATIVE-T The front-end frame's page faults are the upload pools' fresh pages; making them at start (0795) cuts the faults by 80 % and the frame by about a quarter, but the pool sizes are learned slowly
**Symptom** After 0793 / 0794 the first 1080p scene still took 53-87 ms (median of 7: 66.9) with 12.6k-34.6k page faults. **Cause** The shared-memory and constant-buffer upload pools make a 2 MiB page whenever all they have are in flight, and the first write to a page faults it in (512 faults a page, 2.1 us each): a two minute drive ends with 65 shared-memory, 17 constants and 2 transfer-vertex pages (the design had assumed 34 / 12 / 2 from one run), the scene frame wants 66-74 shared-memory pages from run to run. **Fix** SDK 0795: the largest page count of each pool is kept as kind 8 of the key file (a record when a pool grows past what the file has) and `WarmUpHostAllocations` makes and touches the missing pages first, out of the same budget but at most a quarter of it (`d3d12_warmup_pools`, `d3d12_warmup_pool_touch`). **Result** (n = 5 warm runs, 1080p, same day) page faults in that frame 0 / 3.9k / 4.4k / 5.3k / 5.7k, the worst frame < 25 / 55.7 / 43.0 / 62.8 / 61.1 ms (median 55.7); the same build with the pools off 59.6 / 80.2 ms with 18.9k / 34.4k faults; 2000p 95.6 -> 65.5 ms (24.4k -> 2.3k faults). The pool part costs 20-25 ms at start, the whole warm-up line 51-60 ms, and the boot to the first engine frame stays at 3.87-3.93 s. It missed the plan's own line (median <= 55 ms: 55.7; <= 4k faults in 4 of 5: 2 of 5) because the file keeps the largest count seen and the scene's need varies (the marks were 65, 74 after 6 runs): a headroom of 8-10 pages on top of the largest mark would remove the pages still made in the scene (q5: 6 shared + 1 constants pages, 5.7k faults). **Not fixed** A frame with 0 faults (q3) still takes 43 ms: the 600 textures and the front end's draws. A 0790-0794 build cuts a key file at the first kind 8 record: never run one on a cache a 0795 build wrote (the runs used copies until the decision). **Evidence** runs `h3-q0..q5`, `h3-n0a/b` (pools off), `h3-f1` (forward compatibility: kinds 9 and 63 kept in place), `h3-2k0 / 2k1`; scratch `hitch3/RESULTS.md`.

### NATIVE-U DLSS does not make this game faster: the transformer pass costs 4-5 ms at the output size, and the 2x path's floor stays at 3200x2000
**Symptom** DLSS Quality, Balanced and Performance were expected to beat the plain 3200x2000 picture; every mode is slower: saturated quick_drive2 GPU busy p50 8.4 ms and 113-117 fps plain, 13.5 ms (DLAA), 12.5-13.5 (Quality), 12.5 (Balanced), 12.4-13.5 (Performance), 12.5 (Ultra Performance), 72-82 fps. **Cause** DLSS Super Resolution's network (transformer models J and K) runs at the OUTPUT size, about 4-5 ms of the 4060 Laptop's tensor time at 3200x2000 whatever the mode; drawing the scene smaller saves only about 1 ms because the emulation operations (resolves, transfers, post-processing, HUD) stay at 3200x2000 (NATIVE-O / T1: they are half of the frame). The models differ: J and K 12.5-13.5, M +1 to +3 ms, L +5 ms; the automatic model for Performance is M (13.5), K is 12.4. FSR 3.1 costs less than DLSS (11.4 ms at 1.5x, 10.4 at 2x) and is slower than plain too. DLSS runs also show a 1 ms spread between runs (12.5 or 13.5 for the same setting) that native runs do not (8.39, 8.41, 8.38). **Fix** Nothing to fix: DLSS is offered as DLAA (quality: the plain picture has no AA) and Performance with model K; the plain 3200x2000 preset stays the recommended one. At 60 fps DLSS draws 20-26 W more than the plain picture (78-85 W against 58.5 W, +6 C), so it is no help for the fan; no mode reaches a clean 120 at 3200x2000 (best 82 fps). **Evidence** runs `cm-native / -2 / -nf`, `cm-dlaa`, `cm-q / q2 / q-nf / q-j`, `cm-b`, `cm-p / p-k / p-nf`, `cm-u`, `cm-fsr15 / fsr20`, `cm-qpre / ppre` (models inside one run), `cw-*` (60 fps and nvidia-smi power); METRICS 'DLSS'.

### NATIVE-V The blue tint in DLSS screenshots was the game's own two states, found after trying alpha and exposure
**Symptom** DLSS Quality screenshots showed a lilac sky and a bluish wall in about a third of the runs, native and FSR a cream sky (sky (165,153,164) against (159,154,143), wall B 39 against 29). **Cause** Not DLSS: native 2000p without any upscaler shows the same two states (3 of 6 runs); the scene tour (`mcla/schedules/vis_scenes.txt`) freezes the hour and weather and removes it (noon and night colours agree to 1-2 levels between native, Quality and DLAA). Two wrong turns first: DLSS's alpha upscaling (the output's alpha is otherwise left undefined) and a fixed 1x1 exposure texture instead of its automatic exposure each looked like a fix on a handful of runs and were not (tinted runs with both on). Alpha upscaling stays on (0.03 ms); the exposure texture was dropped. **Fix** Compare screenshots only inside one state (a colour check of a sky and a wall box first, `scratch/dlss/int/colors.py`). Note also that the ultra preset switches FXAA on over a profile's `swap_post_effect=none`, so all screenshot comparisons here had FXAA on in every configuration, and that Frame Generation needs FXAA off. **Evidence** runs `n1..n6`, `g1..g5`, `e1..e5`, `f1..f3`, `dlss-vs-2000p*`.

### NATIVE-W DLSS Frame Generation: four problems between "the patch compiles" and "120 shown", and a use after free that crashed one start in two
**Symptom** 0780 compiled first time and Streamline reported DLSS-G and Reflex available, but nothing was generated: `DLSS Frame Generation: the guest output isn't shown 1:1 in the 0x0 swap chain`, then no further line; about every second start died at 3 s with 0xC0000005. **Cause** (1) `FrameGenerationSwapChainActive()` was false for dlss (it asks for AMD's context) while `frame_generation_wanted()` was true, so `PaintAndPresentImpl` answered "connection outdated" to every paint: the swap chain was made and destroyed in a loop and no frame was presented. (2) With the UI layer separation off (AMD's swap chain switches it on, ours did not) the HDR composite is `kWholeFrame`, the refresher never writes the HUD-less image and the swap path gives no depth or vectors (`inputs valid false`). (3) The proxy device made for the queue was released at once; the proxy queue does not hold it, and `CreateSwapChainForHwnd` of the proxy factory then used a freed device proxy in `DXGISwapChain::DXGISwapChain` (found with a vectored exception handler that logs the stack as module + offset and `llvm-pdbutil dump --publics` on NVIDIA's pdb: `sl.interposer.dll+0x2C1A3`). (4) In SDR the swap path feeds frame generation only without FXAA and without the game's piecewise-linear gamma ramp: the ultra preset's FXAA kept it off silently. (Streamline 2.14 also wants `PreferenceFlags::eUseFrameBasedResourceTagging` for `slSetTagForFrame`, in 0770.) **Fix** SDK 0780 as built: active = as wanted for dlss; the separation follows `DlssgActive()`; the proxy device is kept to `Shutdown`; a warning after a minute of frames without inputs names FXAA; `streamline_log >= 1` installs the crash logger. **Result** 60 simulated frames shown as 118.7 a second (PresentMon Independent Flip with tearing, G-Sync), 6 starts in a row, a 6 minute bench drive without a crash, `frame_generation` off / dlss live without a hitch. **Evidence** runs `dlss-fg0..fg9x`, `fgx-fg`, `fgx-pre`, `fgx-ab`, `fgx-bench`, `dlss-fgv` (generated frames only); scratch `dlss/RESULTS.md`.

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

### R2 / R3 2000p: off-screen targets double with the draw scale, VRS as the base (2026-11-27, static work, not run)
**Symptom** At draw scale 2 every render target is 4x the pixels, also the ones the game sizes from a constant: the 640x640 4x-MSAA
sun-shadow target is 1280x1280 on the host (shadow group 1.75 ms, environment map 0.99, post 2.23, composite 1.17 in the T1 M2 run
`t1-m2-2000u`, RTX 4060, governor off). **Fix** Game side: `mcla_gfx_shadow_res=320` in the `2000p` profile (host 640, the 1080p look; the
audit is in `mcla_gfx_shadow_res.cpp`), a gfx preset's shadow size halved at scale 2, `mcla_gfx_env_res` for the environment map
(0x8233AB3C), `vrs_level=2` as the profile's VRS base (see the outcome below: the base was dropped, the env size is now on). **Outcome 2026-11-27
(runs `k-*`, quick_drive2, 1800 MHz locked, 2 replicates)** shadow 320: -0.6 ms (shadow group -0.4, pre -0.2), picture clean (slightly wider
penumbra); env 256: -0.5 to -0.6 ms, the paint reflection as soft as at 1080p, no seam; VRS level 2: -0.1 ms and the distant tree masses get 4-8 px
stair steps (5 of 5 runs with VRS, 0 of 5 without): off. **Findings** (1) The SDK's VRS selection reads the GUEST
surface pitch (`RB_SURFACE_INFO`) and the guest phase markers, never the scaled width: `vrs_min_target_width` 640 selects the same targets at
scale 2 as at 1x (1600 and 800 shaded, 400 not), no SDK change needed. (2) The governor wrote its VRS rungs as absolute levels 1 and 2, so with
`vrs_level=2` a shed would have walked the shading rate back to level 1: the rungs now sit above the base (`mcla_gfx_res_math.h VrsRungs`,
none left with two levels). (3) A gfx preset applied at first use replaces a profile's app default of the same knob (`SetDefaultValue` keeps the
source at default), so `--mcla_gfx_preset=ultra` would have restored 640. (4) The shadow texel-size constants (pixel shader register 5) follow
the guest edge: the penumbra at 320 is about twice as wide as at 640 / 1x. (5) Every other off-screen target (reflection 256, impostors,
fast collector 256 / 512, 960x640 `self`) is sized by literals at several sites with no size field: addresses and blockers in
`docs/native/R2_TARGETS.md`. **Evidence** static: `$WORK/scratch/r2r3/`; run pending (integrator).

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
