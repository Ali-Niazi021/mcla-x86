# Metrics

Owner: VER. One row per bench or run (PLAN §7.4). E1/E2 baselines at the top.

## Baselines

E2 (W0.7, 2026-11-24): Renan's `mcla-recomp` @ `0c3bbc9c` (BUILDS `E2`) on this laptop. Common to every row:
- Power: on AC, Balanced scheme with the AC overlay Best performance. GPU: Radeon 880M, driver 32.0.32029.2010.
  The SuperDisplay virtual second display (`\\.\DISPLAY2`, 1920x1080) was attached. Panel 3200x2000 @ 120 Hz.
- Windowed (`--fullscreen=false`), `--vsync=false` (Renan warns that vsync on makes the emulated 60 Hz vblank cap and pace
  unevenly), Renan's PC defaults otherwise: MSAA 2x, single tile, `edram_tiles=4096`. PresentMon reports `Composed: Flip`
  for both APIs (Vulkan presents through DXGI here).
- Flow: harness `$WORK/scratch/e2_run.py` with dev-pad script `$WORK/runs/E2-pad/e2-roam-drive.pad`: the save (made once:
  new career, intro skipped with A, saved in the city; restored before each run) loads from the title, map zoom-out,
  `free_roam` mark, then the car drives out of the spawn and circles on the streets (Hollywood/Vine; it scrapes walls,
  21 to 56 mph in the shots). PresentMon 2.6.0 on the PID from 20 s after `free_roam`, 150 s
  (`$WORK/runs/E2-<x>/pm.csv`; screenshots `circle-60s.png`, `circle-120s.png`). One run each; shader storage warm
  (`E2-warmup` D3D12, `E2-vkwarmup` Vulkan, 30 s each, not tabled).
- Frame time = `MsBetweenPresents`; GPU busy % = Σ`MsGPUBusy` / Σ`MsBetweenPresents` (`$WORK/scratch/e2_analyze.py`).
  world/real: not measured (Renan's build has no sim-time probe).

| run id | build id | flow | API | internal resolution | cap | variant cvars | avg fps | p50 ms | p99 ms | % > 25 ms | max ms | world/real | GPU busy % | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| E2-a | E2 | roam + drive, 150 s | D3D12 | 1280x720 (1x) | uncapped | `mcla_gpu_backend=d3d12 resolution_scale=1 mcla_fps=0` | 69.6 | 14.24 | 18.86 | 0.03 | 58.8 | n/a | 100.0 | GPU bound (GPU busy p50 14.27 ms) |
| E2-b | E2 | roam + drive, 150 s | Vulkan | 1280x720 (1x) | uncapped | `mcla_gpu_backend=vulkan resolution_scale=1 mcla_fps=0` | 58.1 | 16.84 | 24.18 | 0.62 | 66.7 | n/a | 99.9 | GPU bound; 16.5% lower fps than D3D12 (E2-a) at the same settings, although Renan's Automatic API picks Vulkan for RDNA iGPUs |
| E2-c | E2 | roam + drive, 150 s | D3D12 | 2560x1440 (2x) | uncapped | `mcla_gpu_backend=d3d12 resolution_scale=2 mcla_fps=0` | 24.8 | 39.96 | 49.94 | 99.73 | 155.0 | n/a | 100.0 | GPU bound; 2.8x the 1x frame time for 4x the pixels |
| E2-d | E2 | roam + drive, 150 s | Vulkan | 2560x1440 (2x) | uncapped | `mcla_gpu_backend=vulkan resolution_scale=2 mcla_fps=0` | 22.4 | 44.38 | 53.41 | 99.73 | 174.3 | n/a | 100.0 | GPU bound |
| E2-e | E2 | roam + drive, 150 s | D3D12 | 1920x1080 scene, FSR 3.1 to 2560x1440 | uncapped | `mcla_gpu_backend=d3d12 resolution_scale=2 mcla_fps=0 mcla_anti_aliasing=fsr mcla_upscaling=ultra_quality` | 22.2 | 44.58 | 53.15 | 99.73 | 173.3 | n/a | 100.0 | Renan's route to a 1080p scene (FSR replaces MSAA). Log: `FSR 3.1 ready for 2560x1440 from 1920x1080 (Direct3D 12)`. D3D12 only: FSR on Vulkan not built (no Vulkan SDK). Slower than plain 2x MSAA (E2-c); [inference] FSR's own pass at 1440p output costs more than the smaller scene saves on this iGPU |
| E2-f | E2 | roam + drive, 150 s | D3D12 | 1280x720 (1x) | 60 | `mcla_gpu_backend=d3d12 resolution_scale=1 mcla_fps=60` | 60.0 | 16.67 | 17.88 | 0.00 | 22.0 | n/a | 87.4 | Best setting that reaches 60 (E2-a). Meets the PLAN "stable 60" fps/p99/>25 ms/max bars over this 150 s window (not the full 300 s + 60 s parked protocol; world/real not measured) |

## Runs

| run id | build id | flow | API | internal resolution | cap | variant cvars | avg fps | p50 ms | p99 ms | % > 25 ms | max ms | world/real | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w2.1-m2a | b2.1 | title_to_menu, 180 s, no fixture | D3D12 | 1280x720 (1x) | shipped (30) | none (profile defaults) | n/a | n/a | n/a | n/a | n/a | n/a | M2 check. `reached=free-roam fatal=0 unregistered=0 heartbeat_last=171`. Title at ~27 s; first Start that takes 28 s (`Startup_Checks` 30.6 s); free roam 136.9 s; save rewrite/close ×2. Engine frames from heartbeat 28.9/s (27.4–30.0 per 10 s). run_game.py shots 0/18 (screen saver on the input desktop; BitBlt fails) |
| w2.1-m2b | b2.1 | title_to_menu, 180 s, no fixture | D3D12 | 1280x720 (1x) | shipped (30) | none | n/a | n/a | n/a | n/a | n/a | n/a | M2 repeat. Same result as m2a (`Startup_Checks` 30.4 s, free roam 142.8 s), 29.0 frames/s. run_game shots 0/18 again; the scratch PrintWindow capturer crashed on a bug of its own (no shots) |
| w2.1-m3a | b2.1 | new_profile_to_freeroam, 240 s, no fixture | D3D12 | 1280x720 (1x) | shipped (30) | none | n/a | n/a | n/a | n/a | n/a | n/a | M3 step 1. `reached=free-roam fatal=0 unregistered=0 heartbeat_last=231`; `cutscene -> free-roam` 136.9 s, `Cruising/Cruising_Offline`; save rewrite/close at 71.9 s and 137.5 s; pause 200.0→208.0 s. 28.8 frames/s. Shots from the PrintWindow capturer (`runs/w2.1-m3a-ws/`, 24/24); `user/` copied to `fixtures/after_intro` |
| w2.1-m3b | b2.1 | menu_to_freeroam, 600 s, fixture after_intro | D3D12 | 1280x720 (1x) | shipped (30) | none | n/a | n/a | n/a | n/a | n/a | n/a | M3 soak. `reached=free-roam fatal=0 unregistered=0 heartbeat_last=591`, frames 17505 (29.8/s, 28.5–30.1 per 10 s). `loading -> free-roam` 36.5 s. Pauses at 40.1→44.2 (stray: the flow's Starts at 40/44), 100.3→108.3 and 249.2→253.3 s. From 282.1 s a 'DAMAGED OUT / QUICK FIX' modal (needs A) held the game until the end; the Start/B at 361/365 and 473/477 s did nothing. run_game shots 18/20 once the screen woke |
| w2.1-bisect-skipintro | b2.1 | scratch idle flow, 45 s | D3D12 | 1280x720 (1x) | shipped (30) | `mcla_skip_intro=false` | n/a | n/a | n/a | n/a | n/a | n/a | Bisect. With the hook off, the 3× `Resolve region is empty` + `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend` at the first engine frame are gone (they appear in every run with the hook on). First engine frame at 11.4 s (4.0 s with the hook on). `reached=title fatal=0` |
| w2.1-m3c | b2.1 | scratch `$WORK/scratch/W2.1/menu_to_freeroam_soak_a.txt` (menu_to_freeroam minus the Starts at 40/44, plus 30 `a/300` taps 4 s into the throttle slices from 140 s, none in the pause windows), 600 s, fixture after_intro | D3D12 | 1280x720 (1x) | shipped (30) | none | n/a | n/a | n/a | n/a | n/a | n/a | Soak re-run that tests the proposed flow fix. `reached=free-roam fatal=0 unregistered=0 heartbeat_last=591`, frames 17589, `hang_alarms 0`. No stray pause at 40 s. Scripted pauses open and close at 100/108, 249/253, 361/365 and 473/477 s. Two unscripted modals (DAMAGED OUT, logged as `PauseTab/PM_Continue`) at 332.5 and 550.1 s were cleared by the A taps at 336 and 560 s. The car still drives at 570 s, at night (not white, damage decals drawn). run_game shots 20/20 |
