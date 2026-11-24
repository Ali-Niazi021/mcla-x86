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
