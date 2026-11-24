# Learnings

Owner: ORC (merges what tasks report). One entry per non-obvious finding, newest first:

```
### <task-id> <title>
**Symptom** … **Cause** … **Fix** … **Evidence** (file:line, log line, run id) … **Cost** (time, builds)
```

---

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
