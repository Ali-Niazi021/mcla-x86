# tools/telemetry

Three small Windows tools from the R6 investigation (the Radeon 880M shares a ~35 W package power cap with the CPU, and
its clock swings between 1.25 and 1.9 GHz with background CPU load). `tools/bench.py` runs `sampler.exe` next to
PresentMon so that a METRICS row can say whether the GPU clock and the machine's load were the same in two variants.

| Source | Exe | What it does |
|---|---|---|
| `sampler.cpp` | `sampler.exe <out.csv> <seconds> [interval_ms=100] [process=mcla.exe]` | ADLX GPU metrics (GFX clock, utilisation, memory clock, package power, temperature) plus system CPU every 100 ms into a CSV. Header comment lines record the power scheme, the power-mode overlay, AC state and which metrics the driver supports. Columns: `t_s,gfx_mhz,gpu_util,mem_mhz,gpu_w,tbp_w,temp_c,hot_c,volt_mv,shared_mb,sys_cpu_util_adlx,...,proc_cores,pdh_cpu_util,pdh_cpu_perf,pdh_cpu_mhz`. `t_s` counts from the sampler's own start (after its init). `gpu_w` is the package power on this APU. |
| `threadtop.cpp` | `threadtop.exe [process=mcla.exe] [seconds=10]` | Per-thread CPU of a process, with thread descriptions. |
| `burner.cpp` | `burner.exe [seconds=30] [cpu_spinners=0]` | A D3D11 compute ALU loop (optionally with CPU spinner threads) for GPU clock experiments. It aborts within 0.3 s if `mcla.exe` appears, so it never competes with a Verifier run. |

ADLX limits on this driver (32.0.32029.2010): metrics update at 1 Hz whatever the interval (rows are written at 10 Hz); a
reading lags by about 1 s; SmartShift, power distribution, TBP, hotspot, voltage and all GPU tuning are unsupported.

## Build (portable LLVM, no Visual Studio prompt)

`sampler.cpp` needs the **ADLX SDK** headers and two of its `.cpp` files. The SDK (AMD, MIT-licensed; the license PDF is in
its root) is fetched, never vendored:

```
git clone https://github.com/GPUOpen-LibrariesAndSDKs/ADLX.git C:/Users/Ali/Development/mcla-x86-work/toolchain/ADLX
git -C C:/Users/Ali/Development/mcla-x86-work/toolchain/ADLX checkout 32b5a74
```

(`32b5a74` is the commit R6 used.) Then, from a plain `cmd` or via `cmd /c`:

```
tools\telemetry\build.bat [ADLX_DIR [OUT_DIR]]
```

`ADLX_DIR` defaults to `%WORK%\toolchain\ADLX`, `OUT_DIR` to `%WORK%\toolchain\telemetry`, and `%WORK%` is `%MCLA_WORK%` or
`C:\Users\Ali\Development\mcla-x86-work`. The script calls `%WORK%\toolchain\llvm-20.1.8\bin\clang++.exe` with
`--target=x86_64-pc-windows-msvc -std=c++17 -O2 -fuse-ld=lld` and links `winmm powrprof pdh` (sampler) and
`d3d11 d3dcompiler` (burner). It builds all three in about 9 s and checked clean with Clang 20.1.8 on 2026-11-25 (the output
of a 3 s sampler run matches the R6 format).

The exes are build outputs: they stay in `$WORK/toolchain/telemetry`, never in the repo.

`sampler.cpp` differs from R6's original in its last lines: the original ended with `g.Terminate()` while the ADLX smart pointers of
`main` were alive, so the process exited with `0xC0000005` after the CSV was closed (data intact). The copy here leaves with
`ExitProcess(0)` instead (exit code 0, checked 2026-11-25). The prebuilt `$WORK/toolchain/telemetry/sampler.exe` is R6's build and
still exits with `0xC0000005`; `tools/bench.py` records the exit code in `sampler_meta.json` and ignores it. Rebuild to get the clean one.

## bench.py

If `$WORK/toolchain/telemetry/sampler.exe` exists (or `--sampler PATH`; `--no-telemetry` skips it), `bench.py` starts
`sampler.exe <run>/adlx.csv <seconds-4> 100 mcla.exe` with PresentMon, and it samples the machine's CPU load itself
(`GetSystemTimes` and the game's `GetProcessTimes` every 0.5 s into `<run>/cpu.csv`). Over the measured window it
puts `gpuclk=<p50>/<p10>MHz pkg=<W>W nongame_cpu=<cores> tmax=<C>C` into the METRICS notes and flags `TAINTED` when the
non-game CPU load is above 1.5 cores or when two variants of one bench differ by more than 10 % in median GPU clock. See the
`tools/bench.py` docstring ("Telemetry").
