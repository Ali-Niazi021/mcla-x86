# Midnight Club: Los Angeles → native x86-64 Windows: port plan

Written 2026-11-24 by a planning agent, for Ali. Nothing has been built yet. This file is the plan a fresh agent session
will **execute** as a swarm. Read sections 0–3 for the decisions. Read 6–9 to run it. The kickoff prompt is in section 13.

Notation:
- **V** = verified in this planning session (a file read, a hash computed, a command run). **[INFERENCE]** = reasoned, not
  measured. **[P]** = taken from prior art (the iOS port or a public port) and not re-checked against this machine.
- Paths: `$IOS` = `C:/Users/Ali/Development/mcla-ios-ref` (owner's iOS port, read-only clone). `$WORK` =
  `C:/Users/Ali/Development/mcla-x86-work` (SDK, builds, generated code, logs; never in git). `$GAME` =
  `C:/Users/Ali/Development/dumps/mcla-x360` (the disc image and the extracted game files; never in git). The repo is
  `C:/Users/Ali/Development/mcla-x86` (`Ali-Niazi021/mcla-x86`, branch `main`).

---

## 0. Executive summary

1. **Recompile the Xbox 360 build, not the PS3 build.** The owner's 360 dump is byte-identical to the build the iOS port
   uses (V: `default.xex` SHA-256 `c386f400…a432`, title `545407F8`, media `5940C9DB`, v0.0.0.8, no title update).
2. **Use native static recompilation, not Xenia.** Keep Xenia Canary and RPCS3 only as reference oracles.
3. **No partial or hybrid recompilation.** MCLA's codegen is already clean (30,204 functions, 0 unresolved calls). A JIT
   fallback would add a whole emulator for no expected gain.
4. **Key finding: public Windows recompilations of this exact XEX already exist.** RenanQueiroz/mcla-recomp (GPL-3) plays
   at 30–120 fps on D3D12 and Vulkan. So this port is a graft of three layers:
   - **SDK:** ReXGlue v0.10.0 (`f5337cd`), plus Renan's Windows-proven SDK patch series pinned at v0.3.17, plus the
     owner's portable codegen and correctness patches.
   - **Game layer:** the owner's iOS hooks and codegen config, nearly all of which are platform-independent.
   - **Host:** a new thin Win32 app.
5. **Toolchain:**
   - LLVM Clang 20.1.8 with VS 2022 17.14 (MSVC STL) and Windows SDK 10.0.26100, built with Ninja. The SDK refuses any
     compiler but Clang.
   - Graphics: D3D12 for bring-up, with Vulkan A/B tested on the 880M. Prior art measured Vulkan about 9 % faster on an
     RDNA iGPU.
6. **1080p:** render a true 1920x1080 frame. Use the owner's widescreen hooks (the game's own width/height option words,
   the render-target pool, the EDRAM tile limit, texclear) with 4096 EDRAM tiles. These are the same sites Xenia's
   "1920x1080 Resolution" patch edits. Fallback: render lower and upscale with FSR.
7. **60 fps:**
   - Timing: the owner's quantised n/60 simulation hooks, the present-interval hook, and a precise Win32 guest vblank.
   - CPU: the fence-spin and poll-wait levers, ported to `WaitOnAddress`.
8. **Cheap first experiments (wave 0):**
   - E2 builds Renan's port on this laptop and measures it, to learn the 880M's real GPU headroom before we invest.
   - E1 is a Xenia Canary baseline, run with the owner.
9. **Milestones:**

   | Milestone | Goal |
   |---|---|
   | M0 | SDK builds |
   | M1 | Codegen clean, exe links |
   | M2 | Title screen |
   | M3 | Free-roam driving |
   | M4 | Locked 30 fps |
   | M5 | 60 fps at 720p |
   | M6 | 60 fps at 1080p on the 880M |
   | M7 | Polish |

   Estimate: about 3–6 working days to M6 [INFERENCE: the iOS port went from repo seed to 60 fps on its Mac harness in
   about 2.5 days].
10. **Swarm:**
    - Orchestrator: Opus 5.5, thinking medium.
    - Implementers: Sonnet 5.5, thinking xhigh, each on a disjoint set of files.
    - Researchers, reviewer and verifier: Opus 5.5, thinking medium.
    - Only the verifier builds or runs anything, under one lock, and only after a wave.

---

## 1. The owner's questions, answered

### 1.1 "Would it be better to just use the Xbox 360 version with Xenia?"

**No, not for "stable 60 fps at 1080p on this laptop". Yes, as a free reference.**

**Why native recomp beats Xenia here**
- **Same GPU emulation, no CPU JIT.** Xenia and ReXGlue share the same GPU emulation lineage (Xenos command processor,
  EDRAM emulation, DXBC/SPIR-V shader translators): ReXGlue descends from Xenia (V, README). So the native recomp has
  the same GPU cost as Xenia, without Xenia's PowerPC JIT and its guest-CPU overhead.
- **Hooks Xenia cannot apply.** Xenia's `.text` patches can't hook mid-function with host code. The native build
  gets:
  - correct 60 fps timing (quantised steps, camera/chassis `k(dt)`, Flash UI held at 30 Hz);
  - CPU wait levers. Renan's PC port cut the game's CPU time per frame at the 30 fps cap from 64.5 to 15.9 ms with
    them (V, `docs/status.md`).
  - Xenia's patch only offers a deltatime "60 FPS – Game Speed Fix" (V,
    [game-patches](https://github.com/xenia-canary/game-patches/blob/main/patches/545407F8%20-%20Midnight%20Club%20Los%20Angeles%20(Complete%20Edition).patch.toml)).
- **Known Xenia issues with this title:** Xenia Canary's tracker lists MCLA as `state-playable` but also tags
  `apu-silent`, `apu-garbage`, `gpu-readback`, `vsync-off-speedup` and `regression` (V,
  [issue #145](https://github.com/xenia-canary/game-compatibility/issues/145)). 60 fps needs the patch plus
  `framerate_limit=120`, and drops to 30 under load because of double buffering (V,
  [issue #1143](https://github.com/xenia-canary/xenia-canary/issues/1143) and the patch's own description).

**How we use Xenia instead**
- Xenia Canary is a correctness oracle for pictures, audio and saves.
- Experiment E1 (section 9) measures it on this machine, because we have no number for it on the Radeon 880M.

**What would change the verdict**
- If E1 shows Xenia Canary holding ≥58 fps at ≥1080p in free roam with the CE 60 fps patch, and you only want to *play*,
  use Xenia.
- If you only want to play and E2 shows Renan's build already does 1080p60 here, just build that: it is one command.
  This plan assumes you want your own port, sharing the iOS game layer.

### 1.2 "Maybe a partial recomp?"

**No.** Three variants were considered.

1. **AOT recomp with an interpreter/JIT fallback for code the recompiler missed.**
   - ReXGlue is pure AOT. It has no CPU emulator to fall back to, so adding one means embedding Xenia's x64 JIT backend
     and its memory model.
   - The failure it would cover hardly exists for MCLA:
     - The iOS port's codegen has 0 unresolved calls and an indirect-target gate of "high 0 / medium 0" (V,
       `$IOS/docs/mcla/PORT_STATE.md`, task 2.4/2.5).
     - It uses 30,204 registered functions, of which 4,905 are harvested gap hints (V, `$IOS/games/mcla/codegen/harvest.toml`).
     - Three public Windows ports play the whole game.
   - The cheap safety net instead:
     - The SDK's fatal "Call to invalid or unregistered function at guest address X" stays loud.
     - A `runtime_discovered.toml` hint file (Renan's practice) turns any miss into a one-line fix.
2. **Recompile only hot subsystems and emulate the rest in Xenia.** Xenia cannot load AOT modules, and the boundary
   (guest calling conventions, guest memory, kernel objects) is the whole problem twice over. Rejected.
3. **PS3: recompile the PPU and keep RPCS3's SPU LLVM.** RPCS3's recompilers are bound to its runtime (vm, LV2, SPURS,
   RSX). Its PPU LLVM already compiles ahead of time into its cache, so what costs frames under RPCS3 is that runtime
   (SPU threads, RSX, synchronisation), not translation [INFERENCE]. There is nothing standalone to extract. Rejected.

### 1.3 Xbox 360 or PS3 as the source

**Xbox 360.** The evidence is in section 3.1.

---

## 2. Ground truth verified in this session

| Item | Value | How |
|---|---|---|
| 360 download | Finished: `C:/Users/Ali/Downloads/Midnight Club - Los Angeles - Complete Edition (USA, Europe).7z`, 6,699,244,153 B, one ISO inside (7-Zip notes 38 trailing bytes after the archive, which is harmless) | V `7z l` |
| 360 ISO | Extracted to `$GAME/Midnight Club - Los Angeles - Complete Edition (USA, Europe).iso`, **7,838,695,424 B** (same as the iOS ISO). XGD2, game partition `0x0FD90000`, root dir sector 1,783,935, 15 files | V, stdlib XDVDFS reader |
| `default.xex` | 9,252,864 B, SHA-256 **`c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432`**, SHA-1 `03e5e2c6a5666edc79bd3b230ffaa5fcc03ef7e6`; title `545407F8`, media `5940C9DB`, version 8 / base 8, disc 1/1, entry `0x821322B8`, base `0x82000000`. A copy is at `$GAME/default.xex` | V, matches `$IOS/docs/mcla/XEX_INFO.md` field for field |
| Edition required | Complete Edition (USA, Europe), redump 64472, media `5940C9DB`, v0.0.0.8. No title update (no `.xexp`; `$SystemUpdate` is a dashboard update). The non-CE disc (media `11CCEA2A`, Xenia hash `B9BE89200A856E37`) is **a different build: unsupported**. The CE alt executable (media `0F1CB201`, v0.0.0.13) is "same code" per MCLA-4iOS [P]; treat as unsupported until verified | V + [P] |
| Game files to extract | 12 files, 6,561,615,524 B: `default.xex`, `xarchive_{audio,audlo,cache,music}.rpf`, 6 `.bik`, `nxeart` (skip `$SystemUpdate/`, `root_directory_padding.pad`) | V |
| PS3 dump | `C:/Users/Ali/Development/ps3-games/Midnight Club - Los Angeles - Complete Edition.iso`, 5,669,519,360 B, UDF. `PS3_GAME/USRDIR/EBOOT.BIN` 12,461,000 B is an encrypted SELF (`SCE\0`). `carchive_{audio,cache,cache2,music}.rpf` (no "audlo"), BLUS30442 APP_VER 01.06 | V `7z l`, PARAM.SFO, RPCS3 log |
| RPCS3 on PS3 build | Owner: 14–29 fps (mostly 20–28) at 1080p. Log (read only): PPU and SPU decoders = LLVM; the game uses **SPURS task sets, job chains, queues, job guards** (`cellSpursCreateTaskset`, `cellSpursRunJobChain`, `cellSpursJobGuardInitialize`, …; 339 `cellSpurs` log hits) | V `rpcs3-portable/log/RPCS3.log` (config untouched) |
| Machine | Ryzen AI 9 365 (20 threads), 31.1 GB RAM, Radeon 880M driver 32.0.32029.2010, **panel 3200x2000 @ 120 Hz (16:10)**, C: 210 GB free after the ISO extract | V |
| Toolchain present | VS 2022 Community 17.14.13 (MSVC 14.44.35207), Windows SDK 10.0.19041 + **10.0.26100**, CMake 4.3.2, Ninja 1.12.1 (VS-bundled), Python 3.13 | V |
| Toolchain missing | **No Clang compiler** (VS has only clang-format/clang-tidy), no Vulkan SDK, no PresentMon, no Xenia | V |
| ReXGlue | Windows is a first-class target: win-amd64 CI; D3D12 and Vulkan drivers; `gpu.driver=any` means D3D12 on Windows; requires Clang ≥20 (MSVC refused); BSD-3 (Xenia-derived). v0.10.0 = `f5337cd` (2026-08-21); latest v0.11.0 (2026-11-14), nightly 2026-11-22 | V (README, LICENSE, docs, releases, `gh api`) |
| Public MCLA PC ports of this XEX | RenanQueiroz/mcla-recomp GPL-3 v0.3.17 (`0c3bbc9c`, 2026-11-23), 75 SDK patches on `f5337cd`; holdmysocks/mcla-recomp GPL-3 (upstream; D3D12 plays); mzzvxm/LARecomp (no licence, "don't copy"); holdmysocks/rusty-mcla (Rust re-implementation, ~17 % of functions closed) | V (GitHub) |

---

## 3. Decisions with evidence

### 3.1 Source build: Xbox 360 vs PS3 → **Xbox 360**

| Factor | Xbox 360 (`default.xex`) | PS3 (`EBOOT.BIN`) |
|---|---|---|
| Recompiler maturity | ReXGlue: 1,005 commits, CI on win-amd64, D3D12 + Vulkan, kernel/XAM/XMA/GPU runtime included (V) | ps3recomp v0.20.0 (2026-11-21, MIT): "runs a recompiled game, in this tree" only on Windows/D3D12; downstream ports "carry patches still on their way upstream" (V, [README](https://github.com/sp00nznet/ps3recomp)) |
| This game | ≥3 public Windows recomps of **this exact XEX**; Renan's plays career/races at 30–120 fps; plus the owner's iOS port at 60 fps on an A20 | No MCLA PS3 recomp found in the searches run |
| CPU surface | PPC only: 6.9 MB `.text` + `BINK` code section, 20,071 `.pdata` entries, 30,204 registered functions (V, XEX_INFO/PORT_STATE) | PPU **plus SPU**. MCLA uses SPURS task sets and job chains (V, RPCS3 log), so SPU programs must be lifted and SPURS re-implemented |
| Executable access | AES retail key plus "basic" compression; the codegen decrypts it (V) | Encrypted SELF; must be decrypted first |
| GPU | Xenos: Xenia's mature DXBC/SPIR-V translators and EDRAM emulation. MCLA-specific fixes already exist as patches (blend MIN/MAX, vinyl readback, alpha-to-mask, depth float24, …) | RSX via a young backend; no MCLA fixes anywhere |
| Data files | Read by the game itself in guest endianness either way; no conversion needed | Same, but different archive names (`carchive_*`) and no `audlo` set |
| Owner's reusable assets | All of them: 56 hooks, codegen config, timing/widescreen research, tools, flows | None (different binary, different addresses) |
| Baseline measured here | Pending E1/E2 | RPCS3 14–29 fps @1080p |
| Risk | Low (proven path) | Very high |

**What would change this:** a maintained public MCLA PS3 recomp outperforming the 360 path on this laptop, or a 360-path
blocker that only the PS3 code avoids. Nothing in the evidence suggests either.

### 3.2 Native recomp vs Xenia vs RPCS3 vs adopting an existing port

| Option | Verdict | Why |
|---|---|---|
| RPCS3 (PS3) | Rejected | Measured 14–29 fps here. SPU and RSX emulation cost is structural |
| Xenia Canary (360) | Oracle only | JIT overhead plus the same GPU path. Known MCLA audio, readback and vsync issues (#145). Measure in E1 |
| Fork Renan's port wholesale | Fallback (Option A) | Fastest to a playable build. But its game layer (real-delta timing, ~17 hook configs, launcher) would replace the owner's iOS game layer, so the two repos would diverge |
| **Graft (recommended, Option B)** | **Chosen** | Three layers: SDK = `f5337cd` + Renan's proven Windows SDK patch series (GPL-3, pinned) + the owner's portable SDK patches; game = the owner's iOS hooks/config (one game layer for iOS and PC); host = thin new Win32 app. Renan's build stays as oracle #2 |

**Switch to Option A** if M3 is not reached within two working sessions because the graft does not fit: patch-stack
conflicts or the owner's hooks failing under Renan's SDK patches.

### 3.3 SDK and pin

**Pin: ReXGlue `v0.10.0` = `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`.**
- It is the base of the iOS port's SDK, of all 75 of Renan's patches, and of holdmysocks' patches (V,
  `$IOS/docs/mcla/HARVEST.md` §4, Renan `patches/README.md`).
- The iOS codegen config (manifest schema, register table of 30,204) was validated on it.

**Patch series** (owned by the SDK slice, `patches/sdk/series.toml`, applied by `tools/prepare_sdk.py`), in this order:
1. **Renan's Windows list**, in the exact order `scripts/make_pc.ps1` applies it, at tag `v0.3.17` / commit
   `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca`.
   - It is fetched from a pinned clone and checked against SHA-256s in `series.toml`, not vendored.
   - The mingw, Linux and PS5-only patches are dropped, but only where W0.3 proves they apply independently. Otherwise
     keep them: they are compiler- or platform-guarded.
   - This brings, among others:
     - vblank resync (the interrupt storm fix)
     - `minmax-blend-factor` (white cars; D3D12 and Vulkan)
     - `resolve-readback-rules` (vinyls)
     - `alpha-to-mask-tolerance`
     - `content-header-padding`, `saved-game-backup`
     - `upstream-host-fp-state` (**x86: a zero MXCSR unmasks FP exceptions**)
     - `xma-decoder-fixes`
     - `dxbc-relative-constant-clamp` (D3D12 DEVICE_HUNG)
     - `d3d12-msaa-line-guard` (AMD flashes)
     - `vulkan-odepth-scale`
     - `edram-size` (`edram_tiles` 4096)
     - the CPU and GPU performance set (`gpu-regwrite-bulk`, `guest-atomics-lock-free`, `timer-queue-blocking-wait`,
       threaded submission, placed heaps, direct resolve, clear-rectangle draws, texture-load-direct, hoisted uploads)
     - memory caps (`render-target-max-height`, `-trim`, `gpu-memory`)
2. **The owner's portable patches**, rebased on top as `patches/sdk/0xxx-*.patch`:
   - `$IOS/tools/carbon/sdk_patches/0001-gapfill-claim-code-after-bctr-tail-calls.patch` (codegen; the 4,066
     GapFill-only targets depend on it)
   - `0100-register-table.patch` (one table instead of ~30k `SetFunction` calls; that TU took 10+ min at -O3)
   - `$IOS/games/mcla/sdk_patches/0205-xam-dirty-disc-log-flush.patch`
   - `0211-int-cvar-hex-whole-numbers.patch`
   - `tools/ios/patch_ios_stack_top.py`, re-expressed as a git patch: the first guest frame sits 256 B below the stack
     top (freeze fix)
   - any XMA fix from `tools/ios/patch_ios_xma_*.py`, `tools/parche_xma_{paquetes,edge}.py`, `parche_anillo.py` or
     `parche_desatasco.py` that W0.3 shows Renan's `xma-decoder-fixes` does not already cover
3. **New Windows patches**, from wave 3:
   - precise guest vblank: a Win32 high-resolution waitable timer plus a QPC spin tail plus MMCSS. This is the `0208`
     idea re-implemented.
   - `nfsmw_cp_event_seq` wake via `WakeByAddressAll`. This is `0207` re-implemented; the symbol is exported from
     `rexruntime.dll`.
   - `MmQueryAllocationSize` returns the bytes from the queried address to the end of its allocation. This is the root
     fix of the iOS W9 texclear finding, and Xenia's 1080p patch carries its own replacement for that call.

Not taken: anything Apple, Android, MoltenVK, Turnip, Metal-native or iOS-QoS (section 4.3).

**Later (optional M7.5):** rebase onto ReXGlue v0.11+. Gate: the same register count, M3 flows pass, M5 bench within
noise.

### 3.4 Compiler and build toolchain

- **Clang 20.1.8 from LLVM's portable `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz`**, unpacked to
  `$WORK/toolchain/llvm-20.1.8` (V, the release asset exists). No admin rights are needed.
  - **Why not MSVC `cl`:** the SDK refuses it ("ReXGlue requires the Clang compiler") and LARecomp says "MSVC will not
    build this" (V). The generated code relies on Clang semantics (weak hook aliases, volatile loads, builtins).
  - **Why not `clang-cl`:** the SDK presets use the GNU-style driver (`clang`/`clang++`, `-march=x86-64-v2`, C++23,
    Ninja Multi-Config) (V, `CMakePresets.json`).
  - **Why 20.1.8 exactly:** it is the version Renan builds `f5337cd` with on Windows (V, `docs/building.md`), and
    the VS 2022 toolset has no Clang.
- Target `x86_64-pc-windows-msvc`, linking the VS 2022 17.14 STL/UCRT and Windows SDK 10.0.26100 (needed for
  `D3D12_FEATURE_DATA_D3D12_OPTIONS8`, Renan troubleshooting), with lld.
- Ninja: VS-bundled 1.12.1. CMake 4.3.2 (≥3.25 required).
- **Vulkan SDK 1.4.313+ is optional.** It is needed only if the SDK configure step says "Could NOT find Vulkan", or for
  validation layers. Renan builds the Vulkan backend without it except for FSR-on-Vulkan (V, `docs/building.md`).
- **Do not change the SDK's code-generation flags for the generated code in M0–M6.**
  - Keep `-march=x86-64-v2`, as the SDK applies to targets.
  - **Never allow FP contraction (`-ffp-contract`) beyond what the SDK sets.** Fusing mul+add would change PPC float
    results, and physics with them.
  - x86-64-v3/znver5 is experiment E5 (optional, with a determinism check).

### 3.5 Graphics API

- **D3D12 for bring-up (M2–M4).** It is the SDK default on Windows, upstream holdmysocks plays only on D3D12 (Vulkan "reaches
  the title screen"), and Renan's AMD-iGPU fixes for it are in the series.
- **Vulkan is built too and A/B tested (E3).** Renan's "Automatic" picks **Vulkan on RDNA integrated GPUs**; on a
  9800X3D's RDNA2 iGPU at native resolution Vulkan measured 38.2 ms against D3D12's 41.9 ms (V, `KNOWN-ISSUES.md`,
  anisotropic test). The 880M is RDNA 3.5 integrated, so expect Vulkan to win by 5–10 % [INFERENCE]. Decide the default
  at M5 from our own numbers.
- **Do not port the iOS native Metal renderer.**
  - It was designed around MoltenVK's ~4.6 µs/draw translation cost, 250 passes per frame and thermal throttling (V,
    `$IOS/docs/mcla/NATIVE_RENDERER.md` §1, §6). None of those apply on Windows.
  - The stock EDRAM emulation already gets exponent bias, gamma fetch and packed mips right, which the native renderer
    had to rediscover (NR §13, §19).
  - What does carry over is its *knowledge*: shader roles (tonemap `ps bc5f0eed0a746dea` for HDR, HUD `vs f8b6972a…`),
    the frame structure (~3,500 draws, 62–65 passes, shadows 47 % of draws) and the game's quality knobs (`hooks_gfx.toml`).

### 3.6 The other subsystems

| Subsystem | Decision | Notes |
|---|---|---|
| Window and present | SDK's Windows UI and presenter | Flip model with tearing allowed (`d3d12.allow_tearing`). Present letterboxed 16:9 on the 16:10 120 Hz panel. Borderless fullscreen (Vulkan SDR in a window is DWM-composed, per Renan) |
| Audio | SDK XMA decoder (FFmpeg-based, Xenia lineage) → SDK SDL3 audio driver → WASAPI | XMA fixes come through the series. **Check the radio on the first audio test**: Renan's `xma-decoder-fixes` exists because "radio balance goes wild after seconds" |
| Input | SDK SDL3 HID (XInput, DualSense, keyboard via `mnk`) | Scripted SDL3 virtual pad for automation: port `$IOS/mac/src/mac_input.cpp` + `mac_input_script.h` (std + SDL3 only, V) |
| Disc and filesystem | Extract the 12 game files with `tools/xiso.py` to `$GAME/game/` and run with `--game_data_root` | Same as Renan and LARecomp. ISO-direct is not needed. No STFS/GOD: disc build, no TU, no DLC packages (CE has the DLC on disc) |
| Timing | Guest vblank on an exact 60 Hz grid, separate from host presentation | Windows timers default to a 15.625 ms grid (LARecomp saw frame times on that grid). Use `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` plus a QPC spin tail and MMCSS "Games" for the vblank and CP threads. **Never let the guest vblank free-run** (SDK `vsync=false` makes it ~1 kHz → the game speeds up; Xenia's `vsync-off-speedup` label). Host cap/pacer lives in the host (W3.3) |
| Shader translation | SDK runtime translators (DXBC for D3D12, SPIR-V for Vulkan), on-disk shader storage `545407F8.xsh` plus driver pipeline caches under `$WORK/user/cache` | Async compilation is on. Stutter on first visits is fixed in M7 by Renan's `shader-storage-prepare` (prewarm) |
| Saves | SDK content under `$WORK/user` | `saved-game-backup` comes through the series. Fixtures (an `after_intro` save) live in `$WORK/fixtures/`, never in git |

### 3.7 Building the generated C++ (it is large)

Size and cost:
- MCLA's codegen writes about **121 `.cpp` files plus a register TU** (V, `$IOS` BUILDS mac001: 245 files, 121 `.cpp`).
- Carbon's 10 MB `.text` gave 181 MB of C++, so MCLA's 6.9 MB should give about 120 MB [INFERENCE].
- One TU at -O3 took 3.8 s and 374 MB of RAM on the Mac [P]. Renan measured up to 0.4 GB per compiler and 0.7 GB for
  codegen on Windows (V).

Layout:
- Generated code goes to `$WORK/generated/default`, outside the repo.
- `tools/codegen.py` writes a path-resolved copy of the manifest to `$WORK/generated/manifest/` and runs
  `rexglue codegen` there.
- The codegen rewrites only changed files: 17 of 245 on a hook change [P]. So an incremental hook-address change
  recompiles roughly 10 % of TUs, and a hook-*body* change recompiles one small host TU.

Build structure:
- Static library `nfsmw_recomp`. Keep the project name `nfsmw`, as the iOS manifest does: generated symbols
  (`nfsmw_init.h`, `nfsmw_pch.h`, `REX_DEFINE_APP(nfsmw,…)`) and the owner's hooks use it. This keeps the game layer
  diff-able with `$IOS`.
- Shared PCH (`nfsmw_pch.h`). **No unity build**: the TUs are already 1–2 MB each, and unity would kill parallelism.
- Ninja job pool `recomp` = min(logical CPUs, floor(free RAM GB / 0.6)), which is 20 here.
- The register TU is compiled at `-O0` without the PCH, as on iOS.

Configs and linking:
- `RelWithDebInfo` with `-gline-tables-only` for development (readable crash stacks). `Release` for benches.
- lld with `/OPT:REF` (add `/OPT:ICF` only in Release), `/INCREMENTAL:NO`. **No LTO.**

Build times and caching:
- The SDK is built and installed once per series change. Renan calls this "the long step".
- Our build consumes it via `CMAKE_PREFIX_PATH=$WORK/sdk/install`. Copy `rexgpu-xenos.dll` and any SDK DLLs next to
  `mcla.exe`, as Renan's script does.
- Add `sccache` only if a cold game build measures >10 min in M1.

### 3.8 Resolution strategy for "1080p"

**Primary: true internal 1920x1080 at `resolution_scale=1`.**
- The owner's widescreen hooks set the game's own render size:
  - W1 `0x8217ACDC` writes the `width`/`height` option words `0x827D426C/70`.
  - W2 `0x822E4980` scales the render-target pool.
  - `mcla_ws_edram` (`0x82410E34`, `cmplwi r11,2048`) raises the EDRAM tile limit.
  - W9 texclear (`0x8217829C`/`0x821782C0`).
  - (All V, `$IOS/docs/mcla/WIDESCREEN.md`, `games/mcla/codegen/hooks_widescreen.toml`.)
- Add `edram_tiles=4096` (Renan's `edram-size`).
- **Independent confirmation:** Xenia's "1920x1080 Resolution" patch edits exactly these places:
  - render width/height constants at `0x827d426e`/`0x827d4272`;
  - CreateSurface `0x82410e36` = 4096;
  - RT pool `0x822e4924`;
  - texture clear `0x82178298`;
  - an `MmQueryAllocationSize` replacement;
  - it requires extended EDRAM. (V, the patch file.)
- 1080p at 1x needs 1620 colour + 1620 depth tiles = 3,240 > 2,048, hence the 4096 tiles [INFERENCE from tile size
  80x16 samples, 4 B each].
- **The hooks need extending (new work, W5.1).** Today the W1 hook accepts only W x 720:
  - `mcla_widescreen_width` takes 1288..1600 and the height is the constant `kSceneHeight` = 720 (V,
    `$IOS/games/mcla/hooks/mcla_ws_common.cpp:17-22,104`, `mcla_ws_scene_size.cpp:59,86`).
  - W5.1 adds an explicit WxH (a new `mcla_scene_height` cvar, widths up to 1920+).
  - It also extends the RT-pool, EDRAM and texclear arithmetic to a 2.25x area, and sets the SDK video mode to 1920x1080.
    On iOS the video mode was the `--resolution=WxH` cvar of that patch stack; W0.3 finds our stack's equivalent.
    The iOS note: without it the presenter stretches.

**Fallbacks, in order:**
1. 1600x900 internal plus FSR1/CAS to 1080p (cheap GPU).
2. 720p plus FSR 3.1 upscaling (Renan's patches).
3. `resolution_scale=2` (1440p) downscaled. LARecomp says scale 2 corrupts frustum culling (cars tilted, HARVEST §7),
   while Renan ships 2x/3x and plays. Treat this as unverified and check it in E4.

**Display:** the panel is 16:10. Present 16:9 letterboxed for M6. Hor+/Vert+ at 16:10 (1920x1200) is an M7 option. The
owner's aspect hooks handle wider-than-16:9; taller needs new work, which LARecomp shows is possible.

### 3.9 Legal posture (unchanged from the iOS repo)

- **The repo holds only our own code, config, scripts and docs.** Never game files, `default.xex`/EBOOT, decrypted PE,
  generated C++, shader storage, saves, logs, captures or builds.
- **Game-derived data stays in `$GAME` and `$WORK`**, both outside the repo.
- **Codegen TOMLs and hooks are facts about the binary.** They hold addresses, sizes, registers and our own code, as in
  the iOS repo.
- **LARecomp has no licence and asks for no copy-paste.** Use it as address knowledge only, credited, exactly as `$IOS`
  does (HARVEST §1).
- **Renan's patches are GPL-3.** They are compatible with this GPL-3 repo. Credit them in `THIRD-PARTY-NOTICES.md` with
  the pinned commit.
- **Enforcement:**
  - A `.githooks/pre-commit` guard (`tools/guard_no_game_data.py`) refuses staged files that are over 2 MB, have a
    blocked extension or path, or carry generated-code markers (`PPC_FUNC`, `REX_DEFINE_APP` in non-host files,
    `nfsmw_register`).
  - The orchestrator runs `git config core.hooksPath .githooks` once.

---

## 4. Reuse map

### 4.1 From the owner's iOS port (`$IOS`)

Buckets: **AS-IS** (copy, change paths/names only), **PORT** (logic kept, platform calls replaced), **REDO** (new code
for Windows), **REF** (read for knowledge, cite, don't copy), **SKIP**.

| `$IOS` path | What it is | Bucket | Destination in `mcla-x86` | Task |
|---|---|---|---|---|
| `games/mcla/codegen/mcla_manifest.toml` | Root config. `[project] name="nfsmw"`, `sdk_version="0.10.0"`, `cr_as_local=true` (the only safe `*_as_local`), includes | AS-IS (rewrite 3 paths) | `mcla/codegen/` | W0.5 |
| `games/mcla/codegen/{overrides,harvest,huecos}.toml` | 4,905 gap-function hints (addresses + sizes only); `[rexcrt]` memcpy/memset left commented out; empty gaps file | AS-IS | `mcla/codegen/` | W0.5 |
| `games/mcla/codegen/hooks{,_timing,_widescreen,_gfx}.toml` | ~56 `[[midasm_hook]]` (about 14 core + 9 timing + 28 widescreen + 5 gfx) | AS-IS | `mcla/codegen/` | W0.5 |
| `games/mcla/hooks/` guest-only bodies (35 files): `mcla_fast_clear_off`, `mcla_game_state.{h,cpp}`, `mcla_heartbeat`, `mcla_hooks.h`, `mcla_noaa`, `mcla_rage_fatal`, `mcla_read_retry_fatal`, `mcla_skip_intro`, `mcla_state_trace`, `mcla_t_camera_{lookat,pos}`, `mcla_t_chassis_depth`, `mcla_t_frame_delta`, `mcla_t_present_interval`, `mcla_t_race_clock`, `mcla_t_sim_math.h`, `mcla_t_sim_step`, `mcla_t_ui_{countdown_dt,movie_advance}`, `mcla_ui_lights_guard`, `mcla_ws_{aspect,common.{h,cpp},cutscene_fov,edram,map_aspect,rt_pool,scene_size,texclear,ui}`, `mcla_gfx_tune.h`, `mcla_cpu_{poll_release,poll_wait,pump}` (via common) | Hook bodies that touch only `PPCRegister&`, guest memory, cvars and `REXLOG` | AS-IS | `mcla/hooks/` | W1.3 |
| `games/mcla/hooks/mcla_hooks.cpp`, `mcla_guest.h` | Guarded guest read via Mach `vm_read_overwrite` | PORT → `VirtualQuery` check plus copy (or SEH `__try`) | `mcla/hooks/` | W1.3 |
| `mcla_cpu_common.{h,cpp}` | Event words via `os_sync_wait_on_address` / `os_sync_wake_by_address_all` | PORT → `WaitOnAddress` / `WakeByAddressAll` (Synchronization.lib) | `mcla/hooks/` | W1.3 |
| `mcla_fence_spin.cpp` | Bounded fence wait. Uses `os_sync`, `pthread_getname_np`, and `dlsym("nfsmw_cp_event_seq")` | PORT → `WaitOnAddress`, `GetThreadDescription`, `GetProcAddress(GetModuleHandleW(L"rexruntime.dll"), …)` with a 1 ms bound fallback | `mcla/hooks/` | W1.3 (+W3.2 for the symbol) |
| `mcla_t_common.{h,cpp}` | 60/120 sim switch and pacer policy. Uses `dlsym` for the iOS battery cap and governor | PORT: delete the battery/governor couplings; keep the 2 s window / 10 % miss / 6→30 s hold policy; sleep via the Win32 high-resolution timer | `mcla/hooks/` | W1.3, W3.3 |
| `mcla_gfx_tune.cpp` | `mcla_gfx_*` quality knobs; thermal-governor `dlsym` | PORT (drop the governor) | `mcla/hooks/` | W1.3 |
| `mcla_game_state_host.cpp` | Feeds iOS battery/QoS (`TARGET_OS_IPHONE`) | REDO as a PC no-op (later: an overlay feed) | `mcla/hooks/` | W1.3 |
| `games/mcla/game_profile.cpp` | Launcher cvar defaults (`readback_resolve=none` + vinyl rule, `resolution_scale=1`, `mcla_noaa`, log size …) | PORT subset into a PC profile table. **Apply each cvar only if `rex::cvar::GetFlagInfo(name)` exists, and log the misses.** `gpu_sin_msaa` / `gpu_msaa_muestras` / `ios_*` / `nfsmw_performance_mode` do not exist in our SDK | `mcla/profile/pc_profile.cpp` | W1.3 |
| `games/mcla/sources.cmake` | `MCLA_HOOK_*` flags plus Apple identity | PORT (flags only) | `mcla/sources.cmake` | W1.3 |
| `games/mcla/tests/{test_sim_math,mcla_game_state_test,mcla_present_cadence_test}.cpp` | Pure C++ tests (93 checks in sim_math) | AS-IS | `mcla/tests/` | W0.6 |
| `games/mcla/sdk_patches/0200–0204` | Blend / vinyl / header / save / alpha-to-mask (Vulkan-only versions of Renan's) | REF: **superseded by Renan's originals with D3D12/DXBC hunks** | — | W0.3 |
| `games/mcla/sdk_patches/0205`, `0211` | Dirty-disc fatal log flush; hex/whole integer cvars | AS-IS (rebase) | `patches/sdk/` | W1.1 |
| `games/mcla/sdk_patches/0207`, `0208` (+0209) | CP event wake; precise guest vblank | REDO for Win32 | `patches/sdk/` | W3.2 |
| `games/mcla/sdk_patches/0206`, `0210`, `0212`, `0214` | MoltenVK repaint, native-renderer dirty mask, iOS 120 Hz lock, input-latency probe | SKIP | — | — |
| `tools/carbon/sdk_patches/0001`, `0100` | GapFill after `bctr` tail calls; register table | AS-IS (rebase) | `patches/sdk/` | W1.1 |
| `tools/carbon/sdk_patches/0003`, `0008`, `0102`, `0103`, `0113` | Vulkan/logging items from the MoltenVK era | Evaluate in W0.3 against Renan; default SKIP | — | W0.3 |
| `tools/ios/patch_ios_stack_top.py` | Guest stack-top freeze fix | PORT to a git patch | `patches/sdk/` | W1.1 |
| `tools/ios/patch_ios_xma_*.py`, `tools/parche_xma_{paquetes,edge}.py`, `parche_anillo.py`, `parche_desatasco.py` | XMA voice/packet fixes (voice-19 hang) | Evaluate in W0.3; port only what Renan lacks | `patches/sdk/` | W0.3 → W1.1 |
| `tools/parche_{privilegios,teclado,cvars,pipeline}.py` | Privileges granted, keyboard Start, plugin cvars after a reload, presenter pipeline remade every frame | Evaluate in W0.3 | `patches/sdk/` | W0.3 → W1.1 |
| `tools/fase1_extraer.py` | Stdlib XDVDFS reader | PORT → `tools/xiso.py` | `tools/` | W0.2 |
| `tools/ediciones/imagen.py` | XEX → decrypted PE (RE input) | PORT → `tools/re/xex_image.py` (output to `$WORK/re/default.pe`, SHA-256 `5a9de5cd…f319`) | `tools/re/` | W0.5 |
| `tools/core/{pe_sections,game_config,game_tool}.py`, `tools/carbon/{check_codegen_toml,find_indirect_targets,gen_huecos,find_rexcrt}.py` | Codegen QA (duplicate keys, indirect-target gate, gaps, rexcrt finder) | PORT (paths → `$WORK`; game `mcla` only) | `tools/re/` | W0.5 |
| `mac/src/mac_input_script.h`, `mac/src/mac_input.cpp` | Scripted controller (`<s>:<button>/<hold_ms>`, SDL3 virtual joystick) | AS-IS (rename namespace) | `src/host/input_script.*` | W1.2 |
| `mac/flows/mcla_{title_to_menu,menu_to_freeroam,new_profile_to_freeroam}.txt`, `tools/mcla/bench_flow{,_parked}.txt` | Input scripts | AS-IS (re-time after M2) | `tools/flows/` | W1.4 |
| `common/src/nfsmw_app.h` (787 lines) | `ReXApp` subclass: cvar profile in `OnPostSetup` (GPU cvars exist only after the plugin loads), **hang watchdog** | PORT the generic parts (watchdog, profile apply, log setup); drop MW menu/Black Edition | `src/host/mcla_app.*` | W1.2 |
| `tools/mcla/bench.py` (848 lines), `read_device_log.py`, `sample_summary.py` | A/B bench in one instance, INVALID-run guard, warm-up round dropped, `[mcla-sim]` parsing | PORT (drop `run_mac.sh` and Apple probes; add PresentMon) | `tools/bench.py` | W2.2 |
| `docs/mcla/{ADDRESSES,HOOKS_SPEC,TIMING,WIDESCREEN,FLOWS,HARVEST,RENDER_NOTES,XEX_INFO,LEARNINGS}.md` | Engine knowledge | REF (cite by path and section; do not duplicate) | — | all |

### 4.2 From public projects

| Project | Use | Licence |
|---|---|---|
| RenanQueiroz/mcla-recomp @ `0c3bbc9c` (v0.3.17) | Inputs:<ul><li>SDK patch series (3.3)</li><li>`src/mcla_app.cpp` `ApplyPcDefaults` as the PC cvar reference (W0.4)</li><li>`launcher/dev/scripts/*.pad` + `src/dev_pad.cpp` for E2 automation</li><li>`docs/off-switches.md`, `KNOWN-ISSUES.md` (each with how it was checked)</li></ul>Its build is oracle #2 | GPL-3: OK, credit |
| holdmysocks/mcla-recomp | Upstream history; `config/runtime_discovered.toml` practice | GPL-3 |
| mzzvxm/LARecomp | **Knowledge only:**<ul><li>addresses</li><li>pitfalls (vblank storm, 15.625 ms grid, `clear_memory_page_state`, scale-2 culling claim)</li></ul>Already harvested in `$IOS/docs/mcla/HARVEST.md` | None: **no copying** |
| Xenia Canary game-patches (CE file) | Address confirmation (60 fps, MSAA `0x822E4B80`, 1080p sites) | BSD |
| Xenia Canary binary | Oracle #1 (E1) | BSD |

### 4.3 Skip list (iOS/Apple only)

- **Platform layers:**
  - `ios/**`
  - the Apple parts of `mac/**`
  - MoltenVK
  - `tools/ios/patch_ios_*` platform patches (Mach memory entry, `vm_region_64`, QoS, display link, Retina, signals,
    LiveContainer paths)
- **Native Metal renderer and its tooling:**
  - `ios/src/native_renderer.mm`
  - `tools/native/*` (Metal shader build)
  - `tools/xenosrecomp` (MSL target)
  - `games/mcla/renderer_quirks.cpp`
- **iOS UI and power features:**
  - MetalFX
  - CAS-on-Metal
  - HDR/EDR layer
  - touch (`games/mcla/touch_layout.*`, `TOUCH.md`)
  - the launcher catalog (`games/mcla/catalog.*`, its tests)
  - the thermal/energy governor and `adaptive_qos`
  - `battery.mm`
  - CADisplayLink
  - `test_governor_policy.cpp`
- **Distribution:** IPA and codesign, SideStore/LiveContainer, iCloud build logistics.
- **Other platforms' SDK patches:**
  - the Android hells-gate SDK patch
  - `parche_turnip/pausa/fallos/cola_presentar`
  - Carbon `0101/0104/0110/0111/0112/0120/0121`

---

## 5. Repository layout and work directories

```
mcla-x86/                         (git; GPL-3; our code only)
  CMakeLists.txt  CMakePresets.json  cmake/           BUILD slice
  mcla/codegen/   *.toml                              CODEGEN slice
  mcla/hooks/     *.cpp *.h                           GAME slice
  mcla/profile/   pc_profile.cpp                      GAME slice
  mcla/sources.cmake                                  GAME slice
  mcla/tests/     *.cpp CMakeLists.txt                TESTS slice
  src/host/       main.cpp mcla_app.* watchdog.* input_script.* win32_time.* win32_wait.*   HOST slice
  patches/sdk/    series.toml  0xxx-*.patch           SDK slice
  tools/prepare_sdk.py                                SDK slice
  tools/codegen.py  tools/re/*.py                     CODEGEN slice
  tools/xiso.py tools/run_game.py tools/bench.py tools/heavy_lock.py tools/guard_no_game_data.py tools/flows/*  .githooks/   TOOLS slice
  docs/PLAN.md PORT_STATE.md LEARNINGS.md             ORC
  docs/BUILDS.md METRICS.md                           VER
  README.md THIRD-PARTY-NOTICES.md .gitignore         ORC

$WORK = C:/Users/Ali/Development/mcla-x86-work/   (never in git)
  toolchain/llvm-20.1.8/  toolchain/presentmon/  toolchain/xenia-canary/
  prior/rexglue-sdk-f5337cd/ (pristine)  prior/renan-mcla-recomp@0c3bbc9c/  prior-build/renan/ (E2)
  sdk/src/ (patched)  sdk/install/  build/ (CMake binary dir)  generated/{manifest,default}/  re/default.pe
  user/ (saves, shader storage, caches)  fixtures/after_intro/  runs/<run-id>/  logs/  locks/heavy.lock  scratch/<task>/

$GAME = C:/Users/Ali/Development/dumps/mcla-x360/
  <the .iso>  default.xex (copy)  game/ (12 extracted files, the --game_data_root)
```

Add these to `.gitignore` (W0.2): `/work/`, `/mcla-x86-work/`, `*.csv`, `*.etl`, `*.dmp`, `*.mdmp`, `*.png`, `*.jpg`,
`*.dds`, `*.pso`, `*.cache`, `user/`, `fixtures/`, `runs/`, `cache/`, `shaderstorage/`, `mcla.toml`, `nfsmw.toml`,
`*.xex.*`. Keep the existing rules. `*.png` is allowed only via `!docs/images/*.png` for our own diagrams, never
screenshots.

---

## 6. Milestones and acceptance

Every check is a command with an exact expected output. The tools' output lines are **contracts** the implementers must
honour (Appendix C). "Owner check" items need Ali, and are batched at gates.

| ID | Milestone | Acceptance (all must hold) | Verify with |
|---|---|---|---|
| **G0** | Game dump gate (V once already) | `default.xex` matches XEX_INFO.md; 12 game files extracted | `python tools/xiso.py verify "$GAME/<iso>"` → `OK default.xex sha256=c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432 title=545407F8 media=5940C9DB version=8 disc=1/1`; then `python tools/xiso.py extract "$GAME/<iso>" "$GAME/game"` → `extracted 12 files, 6561615524 bytes` |
| **M0** | SDK builds on this machine | Series fully applied. SDK built (Release) and installed with Clang 20.1.8. `rexglue` CLI runs | `python tools/prepare_sdk.py status` → `sdk f5337cd: series N/N applied, 0 drifted` (N from `series.toml`); `"$WORK/sdk/install/bin/rexglue.exe" --help` exit 0; `$WORK/logs/sdk-build-*.log` shows `clang version 20.1.8` |
| **M1** | Codegen clean, exe links, tests pass | (a) Baseline codegen (empty harvest/hooks) exit 0. (b) Full config exit 0 with 0 unresolved, a register table **= 30,204** (or a difference explained in LEARNINGS) and an indirect gate of high 0 / medium 0. (c) `mcla.exe` links in RelWithDebInfo and Release. (d) Unit tests pass | `python tools/codegen.py --baseline` then `python tools/codegen.py` → `codegen ok: cpp=<n> functions=30204 unresolved=0 fingerprint=<md5>`; `python tools/re/find_indirect_targets.py --game mcla --gate` → `gate: high 0 medium 0`; `cmake --build $WORK/build --config RelWithDebInfo` exit 0; `ctest --test-dir $WORK/build -C RelWithDebInfo` → `100% tests passed` |
| **M2** | Boots to the title screen with audio (D3D12, 720p, shipped 30 fps) | Engine frames run; the title is reached and accepts Start; no fatal | `python tools/run_game.py --flow title_to_menu --seconds 180 --api d3d12` → summary `reached=menu fatal=0 unregistered=0`; the log contains `[mcla-heartbeat] first engine frame` and `[mcla-state] Startup/Startup_Checks`; no line matches `unregistered function\|\[mcla-fatal\]\|\[FATAL\]`. Screenshot `runs/<id>/shots/` shows the title. **Owner check:** music and SFX audible |
| **M3** | Drives in free roam; correctness parity; 10-minute soak | Free roam reached; car drives (flow throttle); PAUSE opens/closes; save written and reloaded; no visual regressions vs the oracles; 10 min with no fatal | `run_game.py --flow new_profile_to_freeroam --seconds 240` → `reached=free-roam` with `[mcla-mode] loading -> free-roam` and `[mcla-state] Cruising/Cruising_Offline`; the save-backup patch logs its rewrite-start then close lines (on iOS 0203 they were `being rewritten` then `closed`; W0.3 confirms the strings in Renan's `saved-game-backup`); then `run_game.py --flow menu_to_freeroam --fixture after_intro --seconds 600` → `fatal=0`, heartbeat advancing to the end. **Parity checklist** (VER screenshots vs E2 shots at the same flow times): cars not white at night (min/max blend), vinyls present (readback rules), no grey blocks or lattice holes (alpha-to-mask), minimap roads visible, HUD placed, radio balanced over 2 min (**owner check**) |
| **M4** | Locked 30 at 720p, bench harness, CPU levers | 30.0 fps held; bench and metrics automated; CPU levers on and stable | `python tools/bench.py --flow bench_drive --fixture after_intro --seconds 360 --variant base:` → METRICS row with `avg_fps 29.9–30.1`, `p99_ms ≤ 35.0`, `pct_over_50ms ≤ 0.5`, `world_real 1.000±0.005` (window = 20 s after `-> free-roam` to end). A/B `mcla_fence_spin`, `mcla_cpu_poll_wait`: rows recorded, no fatal |
| **M5** | 60 fps at 720p (sim60) | Owner's timing hooks on (`mcla_sim60=true`, present interval A5, pacer cap 60), precise guest vblank | `bench.py … --variant sim60:mcla_sim60=true` (second run, warm caches) → `avg_fps ≥ 59.5`, `p99_ms ≤ 20.0`, `pct_over_25ms ≤ 0.5`, `max_ms ≤ 100`, `world_real 1.000±0.005`; the log has `[mcla-sim] on: world step n/60 s`; the `[mcla-sim]   frame times` line has p50 16–17 ms. E3 done: D3D12 vs Vulkan rows recorded, default chosen |
| **M6** | **60 fps at 1080p on the Radeon 880M** (the target) | Internal 1920x1080 (primary path, 3.8) or a documented fallback; same stability bar as M5; on AC power, Windows power mode "Best performance" (recorded) | `bench.py … --variant 1080:mcla_widescreen_width=1920,mcla_scene_height=1080,resolution=1920x1080,edram_tiles=4096` (exact names fixed by W5.1 and recorded in PORT_STATE) → same thresholds as M5; the log has `[mcla-wide] mode 1920x1080, back buffer 1920x1080` and no `[mcla-wide]`/`[mcla-ws-edram]` error line; a screenshot shows 1920x1080 content (HUD placed, no stretched UI). **Owner check:** 10 min of play feels smooth, audio OK |
| **M7** | Polish (optional, owner picks) | Each item has its own bench row and an off switch | Examples: 16:10 / ultrawide (`hooks_widescreen` + UI squeeze), 120 fps grid on the 120 Hz panel (`TIMING.md` §15), HDR (Renan's HDR patches + the tonemap shader), FSR 3.1 / frame generation, shader prewarm, settings UI/launcher, SDK rebase to v0.11+ (M7.5) |

The definition of **"stable 60"** used everywhere: over a 300 s scripted drive plus 60 s parked in the busiest street,
measured after warm-up on a warm second run:
- average ≥ 59.5 fps
- p99 frame time ≤ 20 ms
- ≤ 0.5 % of frames over 25 ms
- no frame over 100 ms
- simulated time / real time = 1.000 ± 0.005

---

## 7. Swarm operating model

### 7.1 Standing staffing rules (the owner's requirement; tag every task with its role)

| Role | Tag | Model | Thinking | Writes | Count at once |
|---|---|---|---|---|---|
| Orchestrator | ORC | **Claude Opus 5.5** | **medium** | `docs/PLAN.md` (amendments section only), `docs/PORT_STATE.md`, `docs/LEARNINGS.md`, `README.md`, `THIRD-PARTY-NOTICES.md`, `.gitignore`, git commits/pushes | 1 |
| Implementer | IMP | **Claude Sonnet 5.5** | **xhigh** | Only its slice's files (7.2) | ≤ 4 |
| Researcher | RES | **Claude Opus 5.5** | **medium** | Nothing in the repo; only `$WORK/scratch/<task-id>/`. Returns compressed findings (≤1,500 words, file:line evidence) | ≤ 2 |
| Reviewer | REV | **Claude Opus 5.5** | **medium** | Nothing; returns a review: blocking issues, legal leaks, cross-slice breaks | 1 per wave |
| Verifier / QA | VER | **Claude Opus 5.5** | **medium** | `docs/BUILDS.md`, `docs/METRICS.md`, everything under `$WORK/{sdk,build,generated,runs,logs,user,fixtures}`. **The only role that builds, runs the game or benches**, always under `$WORK/locks/heavy.lock` | 1 |

**Why Opus 5.5 (medium) for review and verification:**
- A failed build or run costs 5–60 min of the single machine.
- The verifier must judge logs and screenshots, bisect, and decide pass/fail against section 6.
- The reviewer must catch legal leaks and cross-slice breaks before that cost is paid.
- Medium thinking is enough because the acceptance checks are explicit.

**Why no extra roles:** mechanical edits (copying iOS files, path rewrites) go to the owning implementer. A separate
low-effort role would add a coordination hop for no saving.

**If the harness cannot set a model per subagent:** materialise the role files in Appendix B under `.claude/agents/`.
That path is gitignored (local only). Then **restart the session**: custom agent types load only at session start (iOS
LEARNINGS, PORT_STATE line 171).

### 7.2 File ownership (one integration owner per shared path)

| Slice | Paths | Notes |
|---|---|---|
| SDK | `patches/sdk/**`, `tools/prepare_sdk.py` | Single owner of `series.toml` order. Any SDK change in any wave goes through this slice |
| BUILD | `CMakeLists.txt`, `CMakePresets.json`, `cmake/**` | Other slices hand over `sources.cmake` fragments (`mcla/sources.cmake`, `mcla/tests/CMakeLists.txt`) that BUILD includes |
| HOST | `src/host/**` | — |
| GAME | `mcla/hooks/**`, `mcla/profile/**`, `mcla/sources.cmake` | — |
| CODEGEN | `mcla/codegen/**`, `tools/codegen.py`, `tools/re/**` | — |
| TESTS | `mcla/tests/**` | — |
| TOOLS | `tools/xiso.py`, `tools/run_game.py`, `tools/bench.py`, `tools/heavy_lock.py`, `tools/guard_no_game_data.py`, `tools/flows/**`, `.githooks/**` | — |
| ORC | `docs/PLAN.md` (amendments), `docs/PORT_STATE.md`, `docs/LEARNINGS.md`, `README.md`, `THIRD-PARTY-NOTICES.md`, `.gitignore` | — |
| VER | `docs/BUILDS.md`, `docs/METRICS.md` | — |

**Rules**
- **One task per slice per wave.**
- **A task that needs a change in another slice stops and reports it.** The orchestrator schedules it as a dependent
  task. No agent edits outside its slice.
- **Hooks are one file per hook** (as in `$IOS`). A new hook means a TOML entry (CODEGEN slice) plus a `.cpp` (GAME
  slice), done as two tasks with a dependency edge.

### 7.3 Merging, builds, commits

- **One working tree** (`C:/Users/Ali/Development/mcla-x86`), no per-agent branches.
  - Slices are disjoint and builds run only against the integrated tree.
  - Generated code and SDK live outside the repo, so worktrees would only multiply 20 GB build dirs.
  - Exception: risky experiments (the SDK rebase, E5) run in a separate worktree
    `C:/Users/Ali/Development/mcla-x86-wt/<name>` on branch `exp/<name>`, with their own `$WORK/build-<name>`.
- **Agents never commit.** At each wave's fan-in the orchestrator:
  1. Checks `git status --porcelain` against the slices that ran. Out-of-slice edits are reverted and re-tasked.
  2. Runs `python tools/guard_no_game_data.py --staged`.
  3. Commits with explicit paths: `git add <paths>`, **never `-A`**. If the harness offers the `commit` helper, use
     it; it commits only the lines this session wrote.
  4. Pushes `origin main` at every gate.
- **No builds, tests or game runs mid-wave.**
  - Implementers may run only *static* checks on their own files (`python -m py_compile`, `clang++ -fsyntax-only` on a
    host file against the installed SDK headers).
  - The verifier builds once after the wave, under the heavy lock.
  - Wave 2's fix loop is a sequence of mini-waves: VER run → RES triage → IMP fix → VER run.
- **Heavy lock:** `tools/heavy_lock.py` wraps any SDK/game build, codegen, game run, bench, Xenia run or E2 build.
  - The lock file is `$WORK/locks/heavy.lock`. It is first come, first served, and holds the PID and a label.
  - Two heavy jobs would ruin each other's timings and can exhaust 31 GB.

### 7.4 Knowledge recording (the iOS habit, leaner)

| File | Owner | Format |
|---|---|---|
| `docs/PORT_STATE.md` | ORC | Environment table (paths, pins, toolchain), current phase, milestone table with dates, task board (Doing / Todo / Done, one line each with task ID and result), ownership in force, next actions, decisions changed since PLAN |
| `docs/LEARNINGS.md` | ORC (merges what tasks report) | One entry per non-obvious finding: `### <task-id> <title>`, then **Symptom**, **Cause**, **Fix**, **Evidence** (file:line, log line, run id) and **Cost** (time, builds) |
| `docs/BUILDS.md` | VER | One row per build: `id` (`b<wave>.<n>`), date, commit, SDK series fingerprint (from `prepare_sdk.py status`), codegen fingerprint + function count, toolchain, config, wall time, result, artefact path in `$WORK` |
| `docs/METRICS.md` | VER | One row per bench/run: `run id`, build id, flow, API, internal resolution, cap, variant cvars, avg fps, p50/p99 ms, % > 25 ms, max ms, world/real, notes. E1/E2 baselines at the top |

Every task's final report contains:
- the files changed
- the self-checks run
- what VER must check
- risks
- a LEARNINGS draft of ≤5 lines, or "none"

### 7.5 Task card (use verbatim when spawning)

```
ID: W<wave>.<n>  Role: <IMP|RES|REV|VER>  Model: <per 7.1>  Thinking: <per 7.1>
Slice / owned paths (exclusive): ...
Read-only inputs: $IOS/<paths>, docs/PLAN.md §<n>, ...
Goal: one sentence.
Change: numbered steps, exact APIs/addresses/cvars.
Acceptance: observable checks (commands + expected lines) VER will run after the wave.
Do not: build, run the game, commit, edit outside your paths, copy LARecomp code, put game data in the repo.
Report: files changed; self-checks; checks for VER; risks; LEARNINGS draft (<=5 lines).
```

---

## 8. Wave DAG

Edges are true dependencies only. "→" means "needs". Within a wave, tasks without edges between them start together.
Every wave ends with **REV** (review of the combined diff), then **VER** (build/run under the lock), then **ORC** fan-in
(docs, commit, push, gate).

### Wave 0: ground truth, toolchain, first ports (start immediately)

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W0.0 | ORC | `docs/PORT_STATE.md`, `docs/LEARNINGS.md` (skeletons) | — | Create the skeletons, `$WORK` tree and `$WORK/locks/`; copy the environment table from section 2 into PORT_STATE | Files exist |
| W0.1 | VER | `$WORK/toolchain/**`, `docs/BUILDS.md` (env section) | — | Install portable LLVM 20.1.8 (`clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz` from the llvm-project release, verify the `.sig` or SHA-256), PresentMon 2.6.0 CLI, latest Xenia Canary (`xenia-canary-releases`). Check VS 2022 C++ workload and Win SDK 10.0.26100 via `vswhere`. Record GPU driver, panel mode (3200x2000 @ 120 Hz), power mode | `clang++ --version` → `clang version 20.1.8`; `PresentMon-2.6.0-x64.exe --help` exit 0; Xenia exe present |
| W0.2 | IMP (TOOLS) | `tools/xiso.py`, `tools/guard_no_game_data.py`, `tools/heavy_lock.py`, `.githooks/pre-commit`, `.gitignore` (hand the diff to ORC) | — | Port `$IOS/tools/fase1_extraer.py` into `xiso.py`: `verify`/`list`/`extract` per Appendix C. Write the guard (3.9) and the lock | Static checks; VER then runs G0 |
| W0.3 | RES | `$WORK/scratch/W0.3/` only | — | **SDK stack dry run.**<ol><li>Clone `rexglue-sdk` `v0.10.0` (recursive) and Renan @ `0c3bbc9c`.</li><li>Apply Renan's patches in `scripts/make_pc.ps1` order (msvc toolchain path).</li><li>Then try `$IOS` Carbon `0001`/`0100`, mcla `0205`/`0211`, `patch_ios_stack_top.py` (as a diff), and each XMA/privileges/keyboard/cvars/pipeline candidate from 4.1. For XMA, note per fix whether Renan's `xma-decoder-fixes` covers it.</li><li>Grep the SDK for every SDK symbol/cvar the owner's hooks and the iOS profile use (`mcla/hooks` list, `game_profile.cpp`), including the video-mode setting (iOS `resolution`) and the save-backup log strings, and report the missing ones.</li></ol> | Report with proposed `series.toml` (order, source, sha256, purpose, keep/drop, conflicts + resolution hints), and the API/cvar presence table |
| W0.4 | RES | `$WORK/scratch/W0.4/` only | — | **Host and defaults digest.** Read:<ul><li>Renan `src/mcla_app.cpp` (`ApplyPcDefaults`, adapter choice, timer resolution), `src/main.cpp`, `config/*.toml`, `docs/off-switches.md`</li><li>`$IOS/common/src/nfsmw_app.h`, `$IOS/mac/src/*.cpp`, `$IOS/games/mcla/game_profile.cpp`</li></ul>Produce:<ol><li>The PC cvar default table (only cvars in the W0.3 stack; value; why; source)</li><li>The host responsibilities list (window, cvars after plugin load, timer resolution, adapter choice, log setup, watchdog)</li><li>A hook-address overlap table owner vs Renan (any Renan-only fix we need?)</li></ol> | Three tables with file:line |
| W0.5 | IMP (CODEGEN) | `mcla/codegen/**`, `tools/codegen.py`, `tools/re/**` | — | Copy the 8 TOMLs from `$IOS/games/mcla/codegen/` (paths rewritten). Write `codegen.py` per Appendix C (baseline mode = manifest with only `overrides.toml`). Port `imagen.py` → `tools/re/xex_image.py`, and `pe_sections`, `game_config` (mcla only, `$WORK` paths), `check_codegen_toml`, `find_indirect_targets` (`--gate`), `gen_huecos`, `find_rexcrt` | `python tools/re/check_codegen_toml.py` → `0 duplicates` (static; runs without an SDK) |
| W0.6 | IMP (TESTS) | `mcla/tests/**` | — | Port the 3 pure tests. Add a `CMakeLists.txt` defining a `mcla_unit_tests` target with CTest, using only the headers it needs (`mcla_t_sim_math.h`, `mcla_game_state.h`); provide include paths as variables BUILD will set | Static check: `clang++ -fsyntax-only` with `$WORK/toolchain` |
| W0.7 | VER | `$WORK/prior-build/renan/**`, `docs/METRICS.md` | W0.1 | **E2: reference native build on this laptop** (section 9) | METRICS rows E2-a…f; build wall times recorded |
| W0.8 | VER + owner | `docs/METRICS.md` | W0.1 | **E1: Xenia Canary baseline** (section 9). Not blocking; run when Ali is available | METRICS rows E1-a…c |
| G0 | ORC | — | W0.1–W0.6 (W0.7 if done) | VER runs the G0 checks; ORC commits W0, records decisions from W0.3/W0.4 (and E2 if done) in PORT_STATE; `git config core.hooksPath .githooks` | Section 6 G0 lines; push |

**Decision point D1 (after E2):**
- If Renan's build on this laptop holds ≥70 fps uncapped at 1440p (2x) or at FSR-1080p in free roam: proceed as
  planned.
- If 720p uncapped is <60 fps: the GPU is the risk. Pull the GPU levers (W5.2–W5.4) forward into wave 3, and expect
  fallback 1 of 3.8.

### Wave 1: M0 + M1 (SDK builds, codegen clean, exe links)

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W1.1 | IMP (SDK) | `patches/sdk/**`, `tools/prepare_sdk.py` | W0.3 | Write `series.toml` from W0.3. Vendor our patches as `git format-patch`-style files, each with a header: source, licence, purpose, bucket. Write `prepare_sdk.py` (`fetch`, `apply`, `status`, `build`) per Appendix C. The build command is `cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FSR=ON -DCMAKE_C_COMPILER=<llvm>/bin/clang.exe -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++.exe`, then `cmake --build … --config Release` and `cmake --install … --config Release --prefix $WORK/sdk/install` (Renan's sequence, V) | M0 |
| W1.2 | IMP (BUILD + HOST) | `CMakeLists.txt`, `CMakePresets.json`, `cmake/**`, `src/host/**` | W0.4 | Presets `win-amd64-{relwithdebinfo,release}` (Ninja Multi-Config, Clang 20.1.8 paths from `$WORK/toolchain`, `CMAKE_PREFIX_PATH=$WORK/sdk/install`, `MCLA_GENERATED_DIR=$WORK/generated/default`). The recomp static lib from generated `sources.cmake` (job pool, PCH, register TU at -O0), the `mcla.exe` host, `mcla/sources.cmake` + `mcla/tests` inclusion, post-build copy of SDK DLLs. Host: `REX_DEFINE_APP(nfsmw, …)`, `McLaApp : rex::ReXApp` with `OnPostSetup` applying the PC profile (existence-checked), logs to `$WORK/logs` with size 50 MB, window title, `timeBeginPeriod(1)` at start, hang watchdog (port), `--input-file` scripted pad (port of `mac_input*`) | M1(c) |
| W1.3 | IMP (GAME) | `mcla/hooks/**`, `mcla/profile/**`, `mcla/sources.cmake` | W0.3 (API table), W0.4 (cvar table) | Copy the 35 AS-IS hook files. Port the 9 platform files per 4.1 (`WaitOnAddress`, guarded read, `GetProcAddress`, drop iOS governor/battery). `pc_profile.cpp` from W0.4's table. `sources.cmake` with `MCLA_HOOK_*` flags, all hooks compiled; cvars default to shipped behaviour except `mcla_state_trace`/`heartbeat` logging on | M1(c) links with 0 undefined `mcla_hook_*` |
| W1.4 | IMP (TOOLS) | `tools/run_game.py`, `tools/flows/**` | W0.2 | `run_game.py` per Appendix C (heavy lock, fresh `--user-data` per run, fixture copy, `--input-file`, timeout, log tail and summary, screenshots via the window rectangle and PowerShell `CopyFromScreen` every N s). Port the flows | Static check; first real use in wave 2 |
| G1 | REV → VER → ORC | — | W1.1–W1.4, W0.5, W0.6 | VER:<ol><li>`prepare_sdk.py fetch apply build`, then `status` → M0.</li><li>`xex_image.py` → `$WORK/re/default.pe` (SHA-256 `5a9de5cd…f319`).</li><li>`codegen.py --baseline`, then `codegen.py` and the gate → M1(a,b).</li><li>Configure and build both configs → M1(c); `ctest` → M1(d).</li></ol>Failures go back to the owning slice as fix tasks | M0, M1 |

### Wave 2: M2 + M3 (bring-up), with the bench port in parallel

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W2.1 | VER | runs only | G1 | `run_game.py --flow title_to_menu` (D3D12, 720p, shipped). Then `new_profile_to_freeroam`. Copy the resulting save to `$WORK/fixtures/after_intro/`. Then the 10 min soak and the parity screenshots at fixed flow times, next to E2 shots | M2, M3 |
| W2.2 | IMP (TOOLS) | `tools/bench.py`, `tools/flows/bench_*` | W1.4 | Port `$IOS/tools/mcla/bench.py`:<ul><li>Variants as `name:cvar=v,…`, run back to back in separate processes.</li><li>PresentMon CSV capture for the game PID, windowed from `-> free-roam` + 20 s.</li><li>Parse `[mcla-sim]` lines.</li><li>INVALID-run guard (no free roam → exit 3).</li><li>Warm-up run dropped.</li><li>Appends METRICS rows.</li></ul> | Used in wave 3 |
| W2.3 | RES | scratch | a failed W2.1 run | Log triage: map the fatal/hang to `ADDRESSES.md`/`HOOKS_SPEC.md`/LEARNINGS entries; propose a fix and its slice | Findings with evidence |
| W2.4+ | IMP (slice per fix) | per fix | W2.3 | Fixes, one slice each; the mini-wave repeats until M2 and M3 pass | M2/M3 |

**Known traps to pre-empt** [P, all from `$IOS`]:
- **A RAGE fatal exits silently through `XamShowDirtyDiscErrorUI`.** The `rage_fatal` hook plus 0205 make it loud.
- **The UI-lights null guard `0x82725100` is needed.**
- **The title polls the pad every 2–4 updates**, so hold Start for 200–300 ms.
- **The first save shows an ALERT.** Answer CREATE A NEW SAVE GAME.
- **Shutdown can hang about 20 s.** `run_game.py` kills the process after its timeout.
- **GPU cvars must be set in `OnPostSetup`**, after the plugin has loaded.
- **Leave `clear_memory_page_state` at the SDK default** until it has been A/B tested.
- **Do not set `resolution_scale` > 1.**

### Wave 3: M4 (locked 30, timing infrastructure, CPU levers)

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W3.1 | RES | scratch | M3 | Profile one bench run, using Tracy (rebuild the SDK with `REXGLUE_ENABLE_TRACY=ON` in a separate `$WORK/sdk-tracy`) or Windows Performance Recorder. Report:<ul><li>per-thread CPU: CP, guest render, main, workers, audio</li><li>CP time per draw</li><li>fence-spin and poll-wait share</li><li>GPU busy from PresentMon</li></ul> | Report |
| W3.2 | IMP (SDK) | `patches/sdk/**` | M3 | Win32 precise guest vblank: an absolute 16.667 ms grid, high-resolution waitable timer plus a QPC spin of ≤0.3 ms, the vblank thread at MMCSS "Games"/time-critical, cvar `guest_vblank_precise` (default on), and a `[guest-vblank]` stats line every 10 s (period p50/p99, late count). Export `nfsmw_cp_event_seq` with `WakeByAddressAll` (the 0207 idea) | VER: `[guest-vblank]` p50 16.66–16.68 ms, p99 ≤ 17.2 ms |
| W3.3 | IMP (HOST + GAME) | `src/host/**`, `mcla/hooks/mcla_t_common.*` | M3 | Host frame cap at swap for 30/60/120 (high-resolution timer + spin), exposed to `mcla_t_common`'s pacer. Keep the iOS pacer policy (2 s windows, >10 % frames with n≥2 = miss, 6 s hold doubling to 30 s). Remove Apple-specific hold sources | VER: cap 30 bench meets M4 |
| W3.4 | VER | runs | W3.2, W3.3, W2.2 | Build, run M4, A/B the CPU levers (`mcla_fence_spin`, `mcla_cpu_poll_wait` + release + pump), record per-thread CPU | M4 |

### Wave 4: M5 (60 fps at 720p)

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W4.1 | IMP (GAME) | `mcla/hooks/**`, `mcla/profile/**` | M4 | Turn on the 60 fps profile: `mcla_sim60`, A5 present interval with the real guest vblank Hz, `k(dt)` camera/chassis, Flash 30 Hz hold, countdown, race clock. Profile entries for cap 60. Do not touch `0x821BDB08` or r24 at `0x822C2430` (`$IOS` TIMING) | — |
| W4.2 | VER | runs | W4.1 | M5 benches on D3D12 and Vulkan (E3), world/real check, the `[mcla-sim]` lines. A 10 min race (owner check) for physics feel, and the bike if available | M5 + E3 rows |
| W4.3 | RES | scratch | W4.2 | If M5 fails: attribute the misses (CPU vs GPU vs pacing) from W3.1 tooling; propose levers | Report |

### Wave 5: M6 (60 fps at 1080p on the 880M)

| ID | Role | Owned paths | → | Task | Acceptance |
|---|---|---|---|---|---|
| W5.1 | IMP (GAME) | `mcla/hooks/mcla_ws_*`, `mcla/profile/**` | M5 | Extend W1 (`mcla_ws_scene_size`/`mcla_ws_common`) from W x 720 to an explicit W x H: a new cvar `mcla_scene_height`, widths up to 1920 (2560 for M7), and the pool, EDRAM and texclear sizing by area. 1080p profile: 1920x1080, `edram_tiles=4096`, `resolution=1920x1080`, aspect hooks idle at 16:9, UI unsqueezed. Unit-test the sizing maths (TESTS slice, a dependent task) | `[mcla-wide] mode 1920x1080, back buffer 1920x1080`, no error lines |
| W5.2 | IMP (SDK) | `patches/sdk/**` | M5 | `MmQueryAllocationSize` root fix (3.3 item 3). Keep W9 texclear as the belt | — |
| W5.3 | VER | runs | W5.1, W5.2 | E4 (resolution paths) and E6 (`mcla_noaa` vs 2xMSAA single tile) at 1080p on the chosen API; M6 bench | E4/E6 rows; M6 or a gap analysis |
| W5.4 | IMP (GAME) | `mcla/hooks/mcla_gfx_*`, `mcla/profile/**` | W5.3 (if short) | GPU levers in order of picture cost:<ol><li>`mcla_noaa` (1 tile)</li><li>Renan's GPU cvars on (direct resolve, clear-rect draws, texture-load-direct-unscaled, `render_target_max_height`)</li><li>reflection/impostor half-rate equivalents via the game's knobs</li><li>shadow cascades via `mcla_gfx_dl_disable` (−38 % of draws [P])</li><li>FSR fallback</li></ol>Each lever is a bench row | M6 |

### Wave 6+: M7 polish (owner-prioritised, one wave per item)

- **Ultrawide and 16:10:**
  - `hooks_widescreen` aspect hooks plus the UI squeeze (`mcla_ws_ui_squeeze`, `mcla_ws_hud_edges`).
  - Taller-than-16:9 needs new research (LARecomp shows it is possible; use its notes as knowledge only).
- **120 fps:** the `TIMING.md` §15 grid on the 120 Hz panel.
- **HDR:** Renan's `hdr-*` patches plus the tonemap shader insight (`ps bc5f0eed`).
- **FSR 3.1 and frame generation:** Renan's patches.
- **Shader prewarm:** `shader-storage-prepare`.
- **Settings UI or launcher.**
- **Mods.**
- **M7.5:** SDK rebase to v0.11+ on an `exp/` worktree.

---

## 9. Experiments (cheap, early, decisive)

| ID | When | What | Measure | Decides |
|---|---|---|---|---|
| **E1** Xenia Canary | W0.8 (owner present) | Latest canary, the 360 ISO from `$GAME`, D3D12, the CE patch file. (a) Defaults (30 fps), (b) "60 FPS – Game Speed Fix" + `framerate_limit=120`, (c) (b) at `draw_resolution_scale` 2. Ali drives 3 min in free roam per case while PresentMon records the Xenia PID | avg fps, p99, % > 25 ms; owner notes (audio, vinyls, glitches) | The Xenia verdict (1.1); oracle pictures |
| **E2** Renan's native build | W0.7 | `git clone` Renan @ `0c3bbc9c` into `$WORK/prior-build/renan`, then `scripts/make_pc.ps1 -Iso "$GAME/<iso>"` (its own SDK, Clang, patches; ~20 GB). **Without `-InstallTools`:** if the script wants to install anything that needs admin rights, stop and ask Ali. Run it unattended with its scripted controller (options and `launcher/dev/scripts/*.pad` are described in its `launcher/README.md`, "Scripted controller input"; `src/dev_pad.cpp`). (a) D3D12 720p uncapped, (b) Vulkan 720p uncapped, (c) D3D12 2x, (d) Vulkan 2x, (e) FSR to 1080p, (f) cap 60 at the best setting. Free roam + drive pad, PresentMon | Cold build wall time, avg/p99 frame time, GPU busy % | D1 (GPU headroom of the 880M), E3 prior, parity oracle, build-time planning |
| **E3** API choice | W4.2 (own build), prior from E2 | D3D12 vs Vulkan, same flow, same settings | M5 metrics | Default API for the 880M |
| **E4** 1080p path | W5.3 | (a) true 1920x1080 (W hooks + 4096 tiles), (b) 1600x900 + FSR1/CAS, (c) 720p + FSR 3.1, (d) `resolution_scale=2` (watch for tilted cars and culling pop) | Metrics plus screenshots | Which 1080p path ships |
| **E5** `-march` (optional) | M7 | x86-64-v2 (SDK) vs v3 vs znver5 for the generated code only, with `-ffp-contract=off` asserted; determinism check = the same scripted drive ends with the same `[mcla-sim]` world-time and the same car position (log via a heartbeat probe) | Metrics | Whether to ship v3 |
| **E6** AA vs tiles | W5.3 | `mcla_noaa=true` (1x, 1 tile) vs 2xMSAA with `edram_tiles=4096` single tile vs 2 tiles | GPU ms, CP ms/draw, picture | AA default at 1080p |

---

## 10. Risks and mitigations

| # | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| 1 | **The 880M is GPU-bound at 1080p.** Renan's 2-CU RDNA2 iGPU ran 35.6 ms at 720p, but the 880M has ~6x the CUs [INFERENCE] | Medium / high | Measure E2 first (D1). Levers W5.4. FSR fallbacks. Vulkan (E3) |
| 2 | **The CP thread limits dense scenes.** The map zoom-out draws ~16k objects; the PS5 port drops to 15–20 fps there | High / low (menus) | Accept in the map. Use the regwrite-bulk/atomics patches and `mcla_larger_tiles`-style tiling from Renan's knowledge |
| 3 | **Windows timer granularity** (15.625 ms grid) breaks pacing | High / high if ignored | `timeBeginPeriod(1)`, high-resolution waitable timers, QPC spin tail, MMCSS. `[guest-vblank]` stats (W3.2) |
| 4 | **Renan's series and our patches conflict**, or Renan's patches assume his host | Medium / medium | W0.3 dry run before any code. Keep Renan's order with ours after. Most of his patches are cvar-gated. Single SDK-slice owner. Switch to Option A if M3 slips (3.2) |
| 5 | **The owner's hooks rely on iOS-only SDK APIs** (dlsym symbols from iOS patches) | Medium / low | W0.3 API presence table. Bounded-wait fallbacks already exist in the hooks. Export the needed symbols in W3.2 |
| 6 | **Clang/VS toolchain mismatch** (`crtversion.h`, `oldnames.lib`, `std::byteswap` errors in Renan's troubleshooting) | Medium / low | Pinned portable LLVM 20.1.8. Explicit compiler paths in presets. VS 17.14 has the C++ workload (verify in W0.1). Configure from a clean dir when the Windows SDK changes |
| 7 | **FP semantics differ on x86** (MXCSR, denormals, contraction) | Low / high | `upstream-host-fp-state` in the series. No `-march`/contraction changes before E5. Physics sanity (world/real, the owner's drive) |
| 8 | **Vblank interrupt storm** (unsigned underflow) | Known / high | The fix is in Renan's `rexglue-v0.10.0-mcla.patch`. `clock.no_scaling` stays unused |
| 9 | **Legal leakage** (generated C++, saves or screenshots committed) | Low / very high | `.gitignore` + pre-commit guard + REV checks every wave + ORC adds explicit paths only |
| 10 | **Laptop power and thermals change results** | High / medium | Benches on AC in "Best performance". Record power mode and an idle-temperature note in each METRICS row; repeat runs. Battery-mode results are separate rows |
| 11 | **AMD driver quirks** (D3D12 flashes on AMD, Vulkan issues on Vega) | Medium / medium | Renan's AMD fixes in the series. Both APIs built. E3 |
| 12 | **The pinned SDK ages** (v0.10.0, Aug 2026, vs v0.11.0) | Certain / low | M7.5 rebase on a worktree with gates |
| 13 | **Shader-compile stutter on first visits** | High / medium (first run only) | Benches use a warm second run. M7 prewarm |
| 14 | **`resolution_scale=2` culling bug** (LARecomp) vs Renan shipping 2x | Unknown | The primary 1080p path avoids scale 2; E4(d) settles it |
| 15 | **Pad-script timing differs on PC** (the title polls every 2–4 updates) | Medium / low | Long holds and repeated Starts (`$IOS` flows). Re-time the flows in W2.1 |

---

## 11. Open questions for Ali

1. **Goal.** Is the aim your own port (shared game layer with iOS), or just to play MCLA well on this laptop? If the
   latter, E2 (Renan's build) may already be the answer, today.
2. **Graft approval.** Is it OK to build our SDK on Renan's GPL-3 patch series (credited, pinned) instead of re-deriving
   the Windows fixes? This plan assumes yes.
3. **What "1080p" means.** Must it be a true 1920x1080 internal render, or is 900p/720p upscaled with FSR acceptable if it
   is the difference between 60 and 50 fps? Is MSAA required, or may `noaa` be used?
4. **Benchmark conditions.** Plugged in, Windows power mode "Best performance": OK as the benchmark condition? Is
   battery a target?
5. **Display.** The panel is 16:10 at 120 Hz. Is letterboxed 16:9 OK for M6, with 16:10 fill as an M7 item?
6. **Installs.** OK to install portable LLVM 20.1.8 (~2.5 GB unpacked), PresentMon, Xenia Canary, optionally the Vulkan
   SDK (admin), and to build Renan's fork for E2 (~20 GB under `$WORK`)?
7. **Controller.** Which pad will you test with (Xbox, DualSense, keyboard)?
8. **One game layer.** Should `mcla/` later become shared with `mcla-ios` (submodule or sync script), or may the two copies
   diverge? Recommendation: copy now, add a `tools/sync_game_layer.py` diff report later.
9. **E1 time.** E1 needs ~15 min of you driving in Xenia. When?

---

## 12. Not verified in this session

- **No Xenia, Renan or ReXGlue build was run here** (the brief forbids big builds). Every performance expectation for the
  880M is [INFERENCE] until E1/E2.
- **The exact console strings of `rexglue codegen` success at `f5337cd`.** Our tools wrap them (Appendix C), so
  acceptance does not depend on them.
- **Whether every Renan patch applies on top of the owner's `0001`/`0100`/`stack_top` patches.** That is W0.3.
- **That v0.10.0 configures without the Vulkan SDK on Windows** [P: Renan says it is needed only for FSR-on-Vulkan].
- **The 30,204 register count on our stack.** It was measured on the iOS stack (same base, different patches). The M1
  gate allows an explained difference.
- **The DSOGaming article** (Sept 2026, "MCLA gets unofficial PC ports") returned HTTP 403. The ports themselves were
  verified on GitHub.
- **No public MCLA PS3 recompilation was found**, but "none exists" is not proven.
- **RPCS3's bottleneck for this game** (SPU vs RSX vs PPU) was not profiled; only the SPURS usage was seen in the log.

---

## 13. Kickoff prompt (copy-paste into the new session, opened in `C:/Users/Ali/Development/mcla-x86`)

```
You are the ORCHESTRATOR (Claude Opus 5.5, thinking medium) of the MCLA x86-64 Windows port.
Repo: C:/Users/Ali/Development/mcla-x86 (GitHub Ali-Niazi021/mcla-x86, branch main).

1. Read docs/PLAN.md completely. It is the contract: decisions (§3), reuse map (§4), layout (§5),
   milestones and exact acceptance checks (§6), swarm rules (§7), wave DAG (§8), experiments (§9).
2. Staffing (standing rules, PLAN §7.1), tag every task with its role:
   - Implementers: Claude Sonnet 5.5, thinking xhigh, one exclusive slice of files each (PLAN §7.2), <=4 at once.
   - Researchers: Claude Opus 5.5, thinking medium, read-only, write only to $WORK/scratch/<task>/, <=2 at once.
   - Reviewer and Verifier: Claude Opus 5.5, thinking medium. ONLY the Verifier builds, runs the game, or benches,
     always under $WORK/locks/heavy.lock, only after a wave.
   If you cannot set models per subagent, create the role files from PLAN Appendix B in .claude/agents/
   (gitignored), then tell Ali to restart the session, and stop.
3. Start Wave 0 now (PLAN §8): do W0.0 yourself, then spawn in ONE batch W0.1 (VER), W0.2 (IMP TOOLS),
   W0.3 (RES), W0.4 (RES), W0.5 (IMP CODEGEN), W0.6 (IMP TESTS) using the task card in PLAN §7.5.
   When W0.1 is done, have the Verifier run W0.7 (E2). W0.8 (E1) waits for Ali.
4. Fan-in per PLAN §7.3: check slice boundaries, REV review, VER G0 checks, update docs/PORT_STATE.md and
   docs/LEARNINGS.md, run tools/guard_no_game_data.py --staged, commit explicit paths only, push origin main.
5. Legal: never put game files, default.xex, decrypted PE, generated C++, saves, logs, screenshots or builds in
   the repo. Game data lives in C:/Users/Ali/Development/dumps/mcla-x360; everything heavy in
   C:/Users/Ali/Development/mcla-x86-work. LARecomp is knowledge-only (no code copying).
6. Never modify C:/Users/Ali/Development/mcla-ios-ref (read-only reference) or the RPCS3 install.
7. Ask Ali only the PLAN §11 questions that block the current wave; otherwise proceed and record decisions
   in PORT_STATE. Continue wave by wave to M6; stop at each gate only if it fails twice.
```

---

## Appendix A: Sources (fetched 2026-11-24)

- ReXGlue SDK:
  - [repo](https://github.com/rexglue/rexglue-sdk): README (XenonRecomp lineage, win-amd64 CI), `LICENSE` (BSD-3, Xenia-derived), `CMakePresets.json` (clang, x86-64-v2, C++23, Ninja Multi-Config)
  - [Getting Started](https://docs.rexglue.com/getting-started/): Clang ≥20 required, Vulkan SDK, `rexglue init/codegen`
  - [Settings reference](https://docs.rexglue.com/reference/cvars/): `gpu.driver` any = d3d12 on Windows, d3d12/vulkan settings
  - [Releases](https://github.com/rexglue/rexglue-sdk/releases): v0.11.0 2026-11-14, nightly 2026-11-22
  - [v0.10.0 commit](https://github.com/rexglue/rexglue-sdk/commit/f5337cdc947ff6d4c4196737e2c807a48f2a1fc2): 2026-08-21
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): MIT, converter only ("does not provide a runtime"), last commit 2025-08-04.
- RenanQueiroz/mcla-recomp, all at commit 0c3bbc9c (v0.3.17):
  - [repo](https://github.com/RenanQueiroz/mcla-recomp)
  - [`docs/status.md`](https://github.com/RenanQueiroz/mcla-recomp/blob/main/docs/status.md): frame times, iGPU
  - [`patches/README.md`](https://github.com/RenanQueiroz/mcla-recomp/blob/main/patches/README.md): base `f5337cd`, order
  - [`KNOWN-ISSUES.md`](https://github.com/RenanQueiroz/mcla-recomp/blob/main/KNOWN-ISSUES.md): Vulkan on RDNA iGPU, AMD fixes
  - [`docs/building.md`](https://github.com/RenanQueiroz/mcla-recomp/blob/main/docs/building.md): Clang 20.1.8, VS 2022 ≥17.1, Win SDK 26100
  - [`scripts/make_pc.ps1`](https://github.com/RenanQueiroz/mcla-recomp/blob/0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca/scripts/make_pc.ps1): SDK configure/build/install commands
- [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp): GPL-3, D3D12 plays, Vulkan title only; supported release = CE USA/Europe 545407F8.
- [mzzvxm/LARecomp](https://github.com/mzzvxm/larecomp): no licence, "don't copy and paste our code"; vblank storm write-up; 15.625 ms grid.
- [holdmysocks/rusty-mcla](https://github.com/holdmysocks/rusty-mcla): Rust re-implementation, early.
- Xenia Canary:
  - [compatibility #145](https://github.com/xenia-canary/game-compatibility/issues/145): labels
  - [issue #1143](https://github.com/xenia-canary/xenia-canary/issues/1143): 60 fps via `framerate_limit`
  - [CE patch file](https://github.com/xenia-canary/game-patches/blob/main/patches/545407F8%20-%20Midnight%20Club%20Los%20Angeles%20(Complete%20Edition).patch.toml): 60 fps, MSAA, 1080p sites
  - [releases](https://github.com/xenia-canary/xenia-canary-releases/releases)
- RPCS3:
  - [compatibility (Ingame)](https://rpcs3.net/compatibility?g=midnight%20club%3A%20los%20angeles&t=b&type=1)
  - [issue #13452](https://github.com/RPCS3/rpcs3/issues/13452): FPS drop when driving
- [ps3recomp](https://github.com/sp00nznet/ps3recomp): MIT, v0.20.0, platform table.
- Tools:
  - [LLVM 20.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8): `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz`
  - [PresentMon releases](https://github.com/GameTechDev/PresentMon/releases): v2.6.0
- Local, read-only:
  - `$IOS/docs/mcla/{PLAN,PORT_STATE,XEX_INFO,HARVEST,HOOKS_SPEC,TIMING,WIDESCREEN,FLOWS,NATIVE_RENDERER,RENDER_NOTES}.md`
  - `$IOS/games/mcla/**`, `$IOS/tools/**`, `$IOS/mac/src/**`, `$IOS/common/src/nfsmw_app.h`
  - `C:/Users/Ali/Development/rpcs3-portable/log/RPCS3.log`

## Appendix B: role files (for `.claude/agents/`, local only)

Adapt the frontmatter keys to the harness. The model and effort are the owner's rules.

```markdown
---
name: mcla-imp
description: MCLA x86 implementer. Owns exactly the files listed in its task card.
model: claude-sonnet-5-5
effort: xhigh
---
You implement one task of docs/PLAN.md in C:/Users/Ali/Development/mcla-x86. Edit ONLY the paths in your task card.
Reference: C:/Users/Ali/Development/mcla-ios-ref (read-only). Never build, run the game, commit, or touch game data.
No LARecomp code. Finish with: files changed, self-checks, checks for the Verifier, risks, LEARNINGS draft (<=5 lines).
```

```markdown
---
name: mcla-res
description: MCLA x86 researcher. Read-only; returns compressed findings with file:line evidence.
model: claude-opus-5-5
effort: medium
---
Investigate the question in your task card. Write only under C:/Users/Ali/Development/mcla-x86-work/scratch/<task-id>/.
Return <=1,500 words: findings, evidence (file:line, log lines, URLs), recommendation, open items.
```

```markdown
---
name: mcla-rev
description: MCLA x86 reviewer of a wave's combined diff.
model: claude-opus-5-5
effort: medium
---
Review `git diff` of the wave against docs/PLAN.md. Report blocking issues only: compile-breaking mistakes, cross-slice
edits, contract violations (Appendix C output lines), legal leaks (game data, generated C++, LARecomp code), unsafe
cvar defaults. Edit nothing.
```

```markdown
---
name: mcla-ver
description: MCLA x86 verifier. The only agent that builds, runs the game, or benches.
model: claude-opus-5-5
effort: medium
---
Take C:/Users/Ali/Development/mcla-x86-work/locks/heavy.lock (tools/heavy_lock.py) for every build/codegen/run/bench.
Run exactly the acceptance checks of docs/PLAN.md §6 for the gate in your task card. Append rows to docs/BUILDS.md and
docs/METRICS.md. Report pass/fail per check with the log lines and run ids; on failure, the first bad line and a
hypothesis. Never edit source files.
```

The orchestrator runs in the main session (Opus 5.5, medium) and needs no file.

## Appendix C: tool contracts (outputs that the acceptance checks grep)

| Tool | Commands | Must print / do |
|---|---|---|
| `tools/xiso.py` | `verify <iso>`, `list <iso>`, `extract <iso> <dest>` | `verify` prints `OK default.xex sha256=<64 hex> title=<8 hex> media=<8 hex> version=<n> disc=<i>/<n>` and exits 0 only if it equals the pinned identity (constants in the script). Otherwise `FAIL <field> expected=<x> got=<y>`, exit 2. `extract` skips `$SystemUpdate/` and `*.pad` and prints `extracted <n> files, <bytes> bytes`. Stdlib only; partition probe `0x0FD90000`, `0x02080000`, `0x18300000`, `0` |
| `tools/prepare_sdk.py` | `fetch`, `apply`, `status`, `build` (+ `--work`) | `status` prints `sdk f5337cd: series <applied>/<total> applied, <k> drifted` (drift = patch file SHA-256 differs from the one recorded at apply time). `apply` is idempotent (records `.mcla-x86/series.applied`). `build` logs to `$WORK/logs/sdk-build-<ts>.log` and prints `installed <prefix>` |
| `tools/codegen.py` | `[--baseline]` | Writes the resolved manifest to `$WORK/generated/manifest/`, runs `rexglue codegen`, logs to `$WORK/logs/codegen-<ts>.log`. Prints `codegen ok: cpp=<n> functions=<n> unresolved=0 fingerprint=<md5 of sorted file names+contents>`; exits non-zero on any `UnresolvedCall`/`Validation failed` line or a missing `sources.cmake`. `functions` = entries of the generated register table |
| `tools/re/find_indirect_targets.py` | `--game mcla --gate` | `gate: high <n> medium <m>`; exit 0 iff both are 0 (exempt list as in `$IOS` game_config: 7 SEH fields, 8 computed-switch bases) |
| `tools/run_game.py` | `--flow <name> [--fixture <name>] [--seconds N] [--api d3d12\|vulkan] [--cvar k=v]… [--screenshot-every S] [--run-id <id>]` | Takes the heavy lock; fresh user dir under `$WORK/runs/<id>/user`; launches `mcla.exe --game_data_root=$GAME/game --input-file=<flow>`; writes `$WORK/runs/<id>/{mcla.log,shots/,summary.json}`. Prints `run <id>: reached=<none\|title\|menu\|free-roam> fatal=<n> unregistered=<n> heartbeat_last=<s>`. `reached` comes from the `[mcla-heartbeat]`, `[mcla-state] Startup/Startup_Checks` and `[mcla-mode] … -> free-roam` lines. Exit 0 iff `fatal=0 unregistered=0` and the flow's expected `reached` was met |
| `tools/bench.py` | `--flow <f> --fixture <x> --seconds N --variant name:cvars … [--api …] [--repeat k]` | Uses `run_game.py` plus PresentMon on the PID. Window = `-> free-roam` + 20 s to the end. Drops the first (warm-up) run. Prints and appends to METRICS: `bench <id> <variant>: avg_fps=<x> p50_ms=<x> p99_ms=<x> pct_over_25ms=<x> pct_over_50ms=<x> max_ms=<x> world_real=<x>`. Exit 3 = INVALID (no free roam) |
| `tools/heavy_lock.py` | `-- <command…>` | FIFO lock on `$WORK/locks/heavy.lock` with holder label and PID; stale-lock detection by PID |
| `tools/guard_no_game_data.py` | `--staged` | Exit 1, listing the offending paths, if any staged file is >2 MB, matches a blocked extension or path (section 5 + the existing `.gitignore`), or contains generated-code markers |

---

## Amendments (orchestrator only; newest first)

_None yet._
