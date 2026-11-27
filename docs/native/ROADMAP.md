# Native roadmap (T2 shaders, T3 render path, T4 CPU, G3 120 fps)

Commander study, 2026-11-27. For the lead and the soldiers who implement it. Plan and status: `docs/NATIVE_PLAN.md`.
Numbers come from `docs/PORT_STATE.md`, `docs/LEARNINGS.md` and `docs/METRICS.md`. Most of them were measured on the
**old machine** (Radeon 880M iGPU, a 45 W shared package). The only figure from the new machine (RTX 4060 Laptop, 6P+8E
Intel) is the lead's: 1080p ultra, GPU busy about 6 ms of 16.7, the game thread holds 60. No run id or per-operation
split exists for it yet.

Labels: **[M]** measured (run id given), **[E]** estimate (reasoning given), **[U]** no measurement exists.
Paths: `$SDK` = `C:/Users/Ali/Dev/mcla-x86-work/sdk/src/src/graphics`, `$GEN` = `C:/Users/Ali/Dev/mcla-x86-work/generated/default`,
other paths are relative to the repo.

---

## 1. The frame today

### Threads and what they do each frame

| Thread | Work per frame | Emulation only? |
|---|---|---|
| **Main XThread** (game) | `rage::sysTimeManager::Update` (step hook at 0x821BDB90, `mcla/hooks/mcla_t_sim_step.cpp`). Physics substeps: the frame `sub_822C1FA8` reads the substep word `[[0x828CCEA0]+0x10]`. Render phases: the phase loop `sub_822E6408` (`mcla/hooks/mcla_fsr_markers.cpp`). The guest D3D writes the PM4 command buffer. It waits for a ring slot (0x82148AB0, `mcla_cpu_poll_wait.cpp`), polls GPU fences (`D3DDevice_BlockOnFence` 0x82411E98, poll at 0x82412FB0, `mcla_fence_spin.cpp`) and presents (0x82419AA0 / 0x82419AC0, `mcla_t_present_immediate.cpp`). | The game logic is native code. The fence and ring waits are console timing; the hooks turn them into real waits |
| **Pump** (guest `audioFrameTimer`) | One round at most every 7 ms (`mcla/hooks/mcla_cpu_pump.cpp:56`): it frees the frame ring (`mcla_cpu_poll_release.cpp`), runs the audio and engine callbacks, then `Sleep(remaining ms)` | The 7 ms beat is console tuning |
| **GPU Commands** (command processor, CP) | `WorkerThreadMain` (`$SDK/command_processor.cpp:967`). With an empty ring it spins `cp_idle_spin_loops` (64) times with `MaybeYield`, then waits 5 ms at a time (`:983-1004`). It parses PM4 (`ExecutePrimaryBuffer` `:1398`, `ExecutePacket` `:1449`) with a `GpuSwap` byte swap of every value (`:2615`, `:2722-2799`). It writes registers (`$SDK/d3d12/command_processor.cpp:2416`). The swap gate `WAIT_REG_MEM` (`$SDK/command_processor.cpp:2591`, wait `:2650-2668`) sleeps until the guest vblank releases it (`WaitOnAddress`, patch 0430). For each of the ~2,600 draws `IssueDraw` (`$SDK/d3d12/command_processor.cpp:3798`) runs the steps in the next table. Each resolve is `IssueCopy` (`:4630`), each swap `IssueSwap` (`:2886`) | **All of it.** A native renderer issues the same draws without parsing, swaps, the EDRAM model or the translation |
| **GPU Submission** | Replays the deferred command list into D3D12 and submits (`SubmissionThread` `:2194`, Renan's `threaded-submission-d3d12`) | Partly: a native renderer records directly |
| **Guest vblank** | A QPC grid locked to the display (0400 / 0440) with a 100 µs spin before each point (`guest_vblank_spin_us`, `$SDK/graphics_system.cpp:80`, 04c0). It raises the guest interrupt that releases the swap gate | Yes (the console's 60 Hz interrupt) |
| **Pipeline creation x N, storage writer** | Shader translation and PSO creation on a miss (`$SDK/d3d12/pipeline_cache.cpp:3422`, `:3250`). New pipelines are appended to `545407F8.rtv.d3d12.xpso` (`:323-326`) | Yes (Xenos microcode to DXBC at run time) |

### Per draw on the CP thread (`IssueDraw`, `$SDK/d3d12/command_processor.cpp`)

| Step | Line | Emulation only? | Cost |
|---|---|---|---|
| Shader ucode analysis (cached after the first time) | 3827, 3845 | yes | [U] CPU |
| Primitive processing: fans, quads and rectangles converted, guest index endianness (`$SDK/primitive_processor.cpp:279`, `:462`) | 3893 | yes | [U] CPU |
| Render-target cache update: EDRAM ownership, **ownership transfers** between surfaces that share tiles | 3950 (`$SDK/d3d12/render_target_cache.cpp:1634`) | yes | GPU transfers 1.01 ms [M lk-fl1] |
| `ConfigurePipeline`: hash of the state, translation and PSO on a miss | 3980 (`pipeline_cache.cpp:1055`) | yes | hitches: 176 ms menu hitches at boot [M, T0 diagnosis running] |
| `RequestTextures`: untiling and conversion on a load, watched pages | 4004 | yes | GPU texture loads 0.32 ms [M lk-fl1] |
| Fixed function, system constants, bindings (descriptor writes) | 4082, 4090, 4096 | mostly | [U] CPU |
| Shared memory `RequestRange` for each vertex fetch and memexport: upload of pages the CPU wrote (write watches: `$SDK/shared_memory.cpp:110`, hot pages `:35-50`, `UploadRanges` `:814`) | 4140, 4156-4180 | yes | GPU hot-page uploads 0.43 ms [M lk-fl1]; CPU fault cost [U] |
| Record the draw into the deferred list | 4296 | no | per draw 0.2-0.5 µs GPU [M lk-cm1] |

### Per resolve (`D3D12RenderTargetCache::Resolve`, `$SDK/d3d12/render_target_cache.cpp:2004`)

A direct resolve reads the host render target that owns the area (`:2096`). Without one it first dumps the render
targets into the EDRAM buffer (`:2161-2170`). It always writes the **guest memory copy** (shared memory `RequestRange`,
`:2174-2200`). With 0490 / 0530 it also writes the texture that will sample it (dual write, `:2151`). **Dual write is
refused whenever the draw resolution is scaled** (`:1858`, `Fallback::kScaled`). The direct present also falls back
at a scale above 1 (`$SDK/d3d12/command_processor.cpp:3015`; LEARNINGS "Owner-request Native 3200x2000").

### The GPU frame (880M, 1080p perf, saturated) [M lk-cm1, lk-fl1, lk-fl6]

14.8 ms = 6.0 pixels + 0.6 vertex + 1.2 per-draw + **7.0 emulation floor**. The floor breaks down as: dual-written
resolves 2.58 ms (the five full-screen ones about 1.9), EDRAM transfers 1.01, other resolves 0.47, clears 0.47, hot-page
uploads 0.43, texture loads 0.32, barriers 0.08. The wave-2 levers then took off 0.77 ms. By phase group: shadow
cascades 2.33 ms, opaque scene 4.19, translucent + composite + post + tone map 6.32, env/reflection 1.19, UI/tail/swap
0.82. **On the 4060 the split is [U]**; its 272 GB/s of dedicated memory should shrink the bandwidth-bound floor
relative to the pixels [E].

### The emulation layers, cost and status

| Layer | Where | Cost | Notes |
|---|---|---|---|
| EDRAM, resolves, transfers | render_target_cache.cpp | 4.1 ms of 14.8 at 880M 1080p [M] | the largest emulation cost on the GPU |
| Tiling | texture loads, resolve writes into tiled memory | inside the above [M] | |
| Invalidation through guest memory | shared_memory.cpp write watches, hot pages | 0.43 ms GPU [M]; CPU [U] | |
| Shader translation | pipeline_cache.cpp | 0 per frame when warm; hitches cold [M lk-w3cold2] | RT-cache pipelines are not counted by 0590 (LOCK60-W3c) |
| Command buffer translation | CP thread | **[U] on any machine** | thread stats will give the cost per frame |
| Byte swapping | every guest load and store (`$GEN/nfsmw_pch.h:128-137`, all `volatile`, plus a physical-offset compare `:121`); PM4 `GpuSwap`; indices in the VS | [U] | pervasive but cheap per access |
| Kernel waits and spins | fence poll, ring wait, pump sleep, CP idle spin, vblank spin | [U] CPU; on the APU a CPU spinner cost GPU clock 1913 to 1374 MHz [M h1run1] | the 4060 laptop shares chassis power (NVIDIA Dynamic Boost) [E] |
| The vblank | 0400 / 0440 grid, swap gate | timing only | `vsync=false` frees it at about 1 kHz: never use it |

---

## 2. Ranked projects

The order is expected gain toward G2 (the only target the GPU fails today) and G5, divided by cost. G1 is already met
on GPU time on the 4060 [lead's figure]; what remains for G1 is display work in T0. G3 items follow because they only
pay once T1 shows which CPU thread binds. Every project lands behind a cvar that defaults off, is A/B tested in one run
(`--schedule` + `tools/segments.py`, or `gpu_experiment_cycle`), and is turned on in `pc_profile.cpp` only after the
checks pass. Without that the decision table has nothing to compare.

### R1 (T3) The 2x path as cheap as 1x: dual write, direct present and transfer skips at draw scale 2

- **Mechanism.** `2000p` is the 1600x1000 guest with every render target drawn at 2x2 (`mcla/profile/pc_profile.cpp:360-386`,
  `:502`). At that scale the resolve takes the scaled-resolve-buffer path (`render_target_cache.cpp:2174-2183`) and the
  texture is reloaded from that buffer. Dual write is refused (`:1858`) and the swap falls back (`command_processor.cpp:3015`),
  so every 1x lever since 0490 stops applying. Extend 0490/0530 (`PrepareDirectResolveDualWrite`, `:1830`) and 04a0
  (`PrepareSwapDirect`, `command_processor.cpp:7175`) to scaled targets. The texture and the host RT are both 2x, so the
  write is the same shader with scaled extents. Then confirm that 04e0 and 0450/0540 still cut at 2x (`[resolve-dual-write]`
  fallback lines and the poison modes).
- **Gain.** [E] At 1x on the 880M these levers were worth about 1.7 ms (0490 −0.6, 04a0 −0.35, wave 2 −0.77 [M]). At 2x
  the bytes are 4x. On the 4060 at 2000p: **−1 to −3 ms**. M2 and M4 below turn this into a number: the 2x penalty is
  M2's per-op resolve time minus the 1x pixel ladder scaled up.
- **What can break.** The scaled resolve buffer is also what the **unscaled copy rules** (rain at >1x,
  `resolve-unscaled-copy`) and the **readback rules** (vinyls, `resolve-readback-rules`) read. A dual-written texture must
  not skip a resolve those rules need. The texel origin and offset of the MSAA kinds (0530) at 2x. The HUD and FXAA on a
  directly presented 3200x2000 RT (0580, 0720 HDR path).
- **Measure.** `gpu_timing_ops=1` at 2000p: dual-written resolve vs resolve vs texture-load time, and `[resolve-dual-write]`
  counts with `kScaled` fallbacks at 0. `resolve_dual_write_verify=30` must report 0 bytes. `swap_direct_present` log
  "direct". Compare busy ms with the levers on and off in one run.
- **Size, tier, depends.** M. Commander design note first (one page: which scaled-buffer readers exist), then a soldier.
  Depends on M2.

### R2 (T3) Scale-aware off-screen targets at 2000p (shadow map first)

- **Mechanism.** Draw scale is global (`$SDK/pipeline/texture/cache.cpp:76-80`; it is baked into each translated shader,
  `$SDK/pipeline/shader/dxbc_translator.cpp:127`). The **640x640 4x-MSAA shadow target** therefore becomes 1280x1280 4x
  at 2000p, 4x the shadow pixels for no visible gain, and the same happens to env, reflection and impostor targets.
  Compensate on the guest side: at draw scale 2, `mcla_gfx_shadow_res` = 320 (host 640, the 1080p look), and halve the
  env and impostor sizes where a hook can (the RT pool sizing in `mcla_ws_rt_pool.cpp`). A per-target host scale in the
  SDK is the clean version but L: the render-target cache shares EDRAM tiles across scales (see "do not do").
- **Gain.** [E] Shadow cascades are 2.33 ms of the 880M 1080p frame [M lk-cm1], 1.12 of it pixels. At 2x that becomes
  roughly 4x the pixel part. On the 4060 at 2000p: **−1.5 to −3 ms** for shadows, more if env and impostors follow.
- **What can break.** `mcla_gfx_shadow_res.cpp:1-30` audits the CPU readers of the size fields (texel size constants
  follow, and the EDRAM-fit check `(w>>4)*(h>>3) <= 3600` only gets easier). It does **not** check for a literal 1/640
  inside a shader. 320 must stay a multiple of 32 and of 16 (it is). Shadow acne or peter-panning from the bias, which is
  tuned per texel. A motion check of the car's own shadow (the burst flicker test in `$WORK/scratch/ORC/burst.py`) is
  required.
- **Measure.** Separate runs, because the size is read at world load: 2000p ultra with shadow_res 640 / 384 / 320,
  `gpu_timing=1` shadow phase group in ms, plus screenshots compared with 1080p.
- **Size, tier, depends.** S for the shadow (allow 320 in the hook, set it in the `2000p` profile). M for env and
  impostors (new hook sites: soldier with an address study). After R1 or alongside it.

### R3 (T3) Post-processing at native size at 2x: VRS 2x2 on the post phases

- **Mechanism.** At 2000p bloom, the downsample chains, composite and the tone map run at 4x the pixels of 1600x1000.
  The 0500 VRS table already shades post, composite, translucent and pre at 2x2 with no visible change at 1x [M lk-vrs1].
  At draw scale 2 that is exactly native-size shading for those passes. Make level 2 the base of the `2000p` profile, not
  only a governor rung. Check that `vrs_min_target_width` (640) and the per-phase selection see scaled RT widths.
- **Gain.** [M at 1x] −0.56 to −0.73 ms on the 880M at 1080p. [E] At 2000p it is larger, since the post group (6.32 ms
  at 1x) grows with pixels: **−1 to −2 ms** on the 4060.
- **What can break.** Dither patterns in translucent (the main scene must stay 1x1: +0.2..+0.5 ms and blur, LOCK60-A4).
  The HUD is drawn in the UI phase and must stay 1x1. FXAA in the swap is not a VRS phase.
- **Measure.** `gpu_experiment_cycle` of `vrs_level` 0/1/2 at 2000p (frame-exact A/B in one run), plus the parked
  screenshot diff inside 52-88 s (VISUALS-3).
- **Size, tier, depends.** S, soldier. Needs M2's run setup only.

### R4 (T2) Zero run-time compiles: RT-cache pipelines, pipeline library, a local shader pack

- **Mechanism.** (a) The render-target cache creates its transfer, resolve and clear pipelines on first use
  (`render_target_cache.cpp:1388, 4840, 4880, 6220`); 0590 does not count them (LOCK60-W3c). Record their keys and create
  them at start. (b) Pipelines are stored as descriptions (`.xpso`) and every boot calls `CreateGraphicsPipelineState`
  again (preload `pipeline_cache.cpp:754-815`). Add an `ID3D12PipelineLibrary1` keyed by the description hash, so boot and
  first use load driver blobs. NVIDIA keeps its own disk cache, so the gain there is mostly after driver updates; AMD and
  Intel gain at every boot [E]. (c) A **shader pack built locally** at setup (Renan's `shader-storage-prepare` hooks): run
  `vis_scenes` plus a menus tour once on a fresh cache. Game-derived data is never committed.
- **Gain.** Removes the 176 ms menu hitches at boot [M, T0] and the first-visit night and fog holds (vis6, 4 holds
  [M]). Faster boot [U]. This serves G5 directly, and G1's "0 hitch" rule.
- **What can break.** Library invalidation on a driver change (catch `D3D12_ERROR_DRIVER_VERSION_MISMATCH` and rebuild).
  Modification bits (04b0/04d0 options are part of the key, so a cvar change must miss cleanly). Disk size.
- **Measure.** M7 (cold and warm boot): `[pipelines]` creations after the first 60 s must be 0, no `[mcla-hitch]` with
  cause compile, boot-to-title time.
- **Size, tier, depends.** M. Soldier, commander review of the library key. After T0's pipeline-cache diagnosis.

### R5 (T3) Resolve elimination by class (render-target renaming)

The resolves of one frame fall into classes. Only the first needs no copy at all; the rest need a real copy:

| Class (examples from the logs) | Needs a real copy? | Path |
|---|---|---|
| Front buffer from the tone-map 8888 RT | no | done at 1x (04a0); at 2x = R1 |
| Tone-map ownership transfer at 1080p single tile | no | done (04e0) |
| Full-screen colour (16F composite `0x08F28000`), sampled by post in the same frame | **texture: no; memory: yes today**, because the same address is the 8888 front buffer later and differently shaped textures are requested from it (LOCK60-W2b) | rename: the texture takes the RT's resource and the cache gets a fresh RT for those tiles; the memory bytes are written only for a reader that needs memory |
| Shadow cascades: 4x MSAA depth into atlas quarters | yes (sample selection, partial destination) | keep; R2 makes it 4x smaller |
| Env / reflection faces, impostor views (224², 256² at `0x06CFF000`) | yes (partial and offset destinations) | keep dual write (0530) |
| Depth resolves sampled later | yes (format conversion) | keep |
| CPU readbacks (vinyls, `readback_resolve_rules`) | yes, to memory | keep |

- **Mechanism.** For the "no" rows the texture cache would hold a view of the host RT (same format and layout, 1x or
  scaled), and the RT cache would retire the RT instead of copying it. A later draw into those EDRAM tiles gets a new RT
  (copy-on-write). Memory is materialised only when a non-renamed reader asks.
- **Gain.** Upper bound [M lk-fl2]: dropping the memory stores of all dual-written resolves saved 1.07 ms on the 880M at
  1080p. Renaming removes the texture write as well. [E] −0.5 to −1.5 ms at 1080p on the 880M; on the 4060 −0.3 to −1 ms
  at 2000p after R1.
- **What can break.** Exactness. Address reuse across shapes is real in this game (two cases logged in LOCK60-W2b). EDRAM
  aliasing with the next pass. HDR's capture at the tone-map draw. The poison and verify modes must stay clean by day and
  at night.
- **Measure.** A `resolve` log of one frame by class (a new `gpu_timing_ops` sub-key), then per-class A/B.
- **Size, tier, depends.** L. Commander design first (ownership model in the RT and texture caches), then soldiers one
  class at a time. After R1.

### R6 (T4) Spin and sleep census: no console timing loops on the PC

- **Mechanism.** The known loops: the CP idle spin (64 `MaybeYield` loops then 5 ms waits, `command_processor.cpp:983-1004`,
  cvar `cp_idle_spin_loops` `:62`); the guest vblank QPC spin of 100 µs (`guest_vblank_spin_us`); the pump's `Sleep(r3)` of
  up to 7 ms (`mcla_cpu_pump.cpp`, `mcla_cpu_pump_wake` default **off**, `:40`); the fence poll and ring wait (already
  hooked); `MaybeYield` in non-swap `WAIT_REG_MEM` (`command_processor.cpp:2655, 2676`). Unknown: guest workers that spin on
  `KeDelayExecutionThread(0)` or `Sleep(0)`. Find them with `mcla_thread_stats_s` (CPU per thread) and an ETW
  context-switch capture. Replace each with an event wait where a producer can signal.
- **Gain.** [E] The CPU watts go back to the GPU's boost budget (the APU lost 28 % GPU clock to 6 spinners, h1run1 [M];
  Dynamic Boost on the 4060 laptop moves up to about 15-25 W between CPU and GPU [E, vendor figure, check]). Lower
  latency at 120 fps (G3: the pump's 7 ms beat against an 8.3 ms frame).
- **What can break.** The pump wake changes audio timing (radio crackle, XMA underruns: listen). The vblank spin protects
  the grid's precision (0400: the timer wakes 0.45-0.85 ms late at p99, W3.3).
- **Measure.** `segments.py` A/B of `cp_idle_spin_loops=0`, `mcla_cpu_pump_wake=true`, `guest_vblank_spin_us=0/100`:
  package W, GPU clock, per-thread CPU, `[mcla-cpu]` pump line, `[present-stats]` on-time swaps.
- **Size, tier, depends.** S, soldier. Needs GPU clock telemetry on NVIDIA (see open questions).

### R7 (T4) Build level: x86-64-v3, then PGO; LTO only for the SDK and host

- **Is the generated code amenable?** It is straight C++ on a register struct (`ctx.rN`), and each function is a separate
  definition in about 121 files, 125 MB (`$GEN`). Three facts set the limits:
  1. Every guest function is `weak, noinline`, aliased to `__imp__name` (`$GEN/nfsmw_pch.h:75-78`), so hooks can override
     it. **LTO cannot inline guest functions into each other**, and inlining is where its gain would come from.
  2. Every guest load and store is a `volatile` byte-swapped access (`:128-137`), so the compiler cannot combine or drop
     memory operations. PGO can still lay out branches and split hot from cold code; it cannot remove those accesses.
  3. Indirect calls go through a table lookup (`:261`). PGO's indirect-call promotion applies there.
- **Steps.** (a) `-march=x86-64-v3` (today `-march=x86-64-v2`, `CMakePresets.json:25`): MOVBE turns every bswap and load
  into one instruction, and BMI2 helps the `rlwinm` rotate-and-mask chains. Add **`-ffp-contract=off`**, so FMA never
  changes float rounding (physics and replays). Keep a v2 build for CPUs without AVX2, so the launcher picks one (G5).
  (b) PGO: `-fprofile-instr-generate` on `nfsmw_recomp` and the SDK, the `bench_drive` flow as the training run,
  `-fprofile-instr-use`. The build system needs a third configuration and a `.profdata` that is never committed (it is
  derived from playing the game). (c) ThinLTO on `rexruntime`, the GPU plugin and the host only; the CP is SDK code that
  gains from it.
- **Gain.** [U] Only pays if T1 finds a CPU thread near its budget (G3). [E] v3: low single-digit % of game-thread time;
  PGO on large branchy code is typically 5-15 % [E, general experience, not this code].
- **What can break.** Float results (contraction is off, but check `simde` paths that change with AVX2:
  `simde_mm_*` widen to VEX forms with the same semantics [E]). Weak symbol resolution under LTO: hooks must still win
  (the link policy in `CMakeLists.txt:95`; check with `llvm-nm` that `sub_822E6408` resolves to the host's strong
  definition). Compile time and RAM of the 125 MB (`mcla_recomp.cmake` job pool).
- **Measure.** Main XThread and GPU Commands CPU ms per frame from `[mcla-threads]` at 1080p `mcla_sim_cap=120`
  uncapped, before and after, 3 runs each; world/real stays 1.000.
- **Size, tier, depends.** S for v3, M for PGO (commander designs the configuration). After M1/M6.

### R8 (T4) Native replacements for hot guest routines

- **How to find them.** The symbol names are the guest addresses (`sub_823DA950` etc.), so any sampling profiler on the
  RelWithDebInfo PDB gives a guest profile directly: WPR/WPA or Intel VTune on this Intel CPU. 60 s of `bench_drive`,
  sorted by self time per thread. The SDK also has per-guest-function Tracy zones (`REXGLUE_PROFILE_GUEST_FUNCTIONS`,
  `$GEN/nfsmw_pch.h:87-100`), but they are too heavy for timing and only useful for call counts.
- **Mechanism.** Override a guest function with a strong `extern "C" void sub_X(PPCContext&, uint8_t*)` in `mcla/hooks/`,
  falling back to `__imp__sub_X` (as `mcla_fsr_markers.cpp:162` does), or use a mid-asm hook (`mcla/codegen/hooks.toml`)
  for part of one. **First candidates:** `memcpy` 0x823DA950 (592 callers) and `memset` 0x823D9890 (349), verified in
  `mcla/codegen/overrides.toml:28-45`. They are disabled there only because the SDK's `[rexcrt]` wrapper returns `r3` off
  by 0x1000 for addresses at or above 0xE0000000. A hand-written override that copies through `REX_RAW_ADDR` and leaves
  r3 untouched avoids that. Likely next candidates from experience [E]: vector and matrix math, culling, string hashing,
  decompression. Only the profile decides.
- **Gain.** [U] Up to the routine's share of the profile. A native `memcpy` beats a recompiled byte-swapping loop by a
  large factor [E].
- **What can break.** Guest semantics: overlapping copies (memcpy vs memmove use), writes into GPU-watched pages (a host
  write still trips the watch: exact), and a range that crosses 0xE0000000 (assert and fall back).
- **Measure.** The profile share before and after, `[mcla-threads]` Main XThread ms, the `check_hooks.py` signature check.
- **Size, tier, depends.** S per routine after the profile. Soldier. Depends on M1 (profile capture).

### R9 (T4) Command-processor cost

- **Mechanism.** Act only if M1 shows GPU Commands above about 5 ms per frame at 60 (or 3 ms for 120). Candidates in
  order: (a) skip `UpdateBindings` and system constant uploads when the dirty state is unchanged (`command_processor.cpp:4090-4096`);
  (b) hoist shared-memory uploads out of passes (D3D12 already has `d3d12-hoist-shared-memory-uploads`; check the
  `LogSharedMemoryHoistStats` line, `:4469`); (c) cache primitive conversion results for static index buffers
  (`primitive_processor.cpp`; check whether the cache already hits); (d) bulk register writes (`gpu-regwrite-bulk`, on).
  Draw batching is **not** on the list: per-draw GPU cost is 0.2-0.5 µs [M lk-cm1].
- **Gain.** [U] Until M1.
- **What can break.** Dirty tracking is the classic source of stale-state bugs: wrong textures and blend for a draw.
- **Measure.** GPU Commands CPU per frame; a VTune profile of the CP thread.
- **Size, tier, depends.** M. Soldier with commander review. Depends on M1.

### R10 (T3) Anti-aliasing on the 4060: supersampling through draw scale, not MSAA

- **Mechanism.** Native MSAA is blocked: 2x at 1080p needs 3264 EDRAM tiles per surface against the 2048 a surface can
  reach (VISUALS AA), the host sample count is tied to the guest's MSAA mode, and the game's forced 4x pause backdrop ran
  27 fps (MENUS60). With R1 to R3 done, a `1080p-ss` profile (1080p guest, draw scale 2 = 3840x2160 host, presenter 2:1
  downsample) is 4x SSAA on the TV. It also fixes the alpha-tested foliage shimmer that FXAA cannot (VISUALS defect 2).
  It costs about what 2000p costs. FXAA stays the cheap default and the governor's floor.
- **Gain.** Picture quality (G1 "looks native"). GPU cost ≈ the M2 figure [E].
- **What can break.** The 1080p guest at 2x needs 4096 EDRAM tiles (edram_tiles 4096 is already set) and a 2x
  `render_target_max_height`. The single-tile hook at 2x. The downsample filter must be a 2x2 box, not FSR's EASU.
- **Measure.** M2's method at 1080p-ss; the burst flicker test on foliage.
- **Size, tier, depends.** S after R1 and R3. Soldier.

### R11 (T2) Pixel shader IEEE multiply per shader

- **Mechanism.** The emulated Xenos rule 0 x anything = 0 costs about 27 % of the pixel ALU [M R1/R8, RGA corpus].
  `dxbc_ps_ieee_mul` is off because of one unexplained white frame (`pc_profile.cpp:135`). Make it a per-shader
  allow-list: hash the ucode and exclude the shaders that multiply a possible Inf or NaN by 0, found by a NaN-poison
  pixel shader run.
- **Gain.** [E] A part of the 6.0 ms pixel time at 1x on the 880M. On NVIDIA [U]. Most valuable at 2000p, where pixels
  dominate.
- **What can break.** White or black pixels (lighting with zero lights, 0 x Inf). Keep the modification bit so caches
  never mix.
- **Size, tier, depends.** M. Soldier, commander review. After M2 shows pixels are the larger part at 2000p.

### R12 (T4, study) Non-volatile frame-slot accesses in the codegen

`volatile` on every access (`$GEN/nfsmw_pch.h:128-137`) keeps stack spills and reloads in memory. Stack slots of the
function's own frame whose address never escapes (no `addi rX,r1,…` that leaves the function) are thread-private and
never MMIO. The codegen could emit them as plain accesses and skip the physical-offset compare (the stack is below
0xE0000000). [U] gain, real risk: kernel objects on the stack that another thread signals (KEVENTs). Commander study
only, L, after R7 shows CPU matters.

---

## 3. G3 (120 fps): what bounds it

1. **CPU per thread at 8.3 ms.** Main XThread, GPU Commands and GPU Submission must each finish a frame in 8.3 ms, and
   the pipeline lets them overlap only as far as the 3-slot frame ring between the main thread and the pump allows
   (`mcla_cpu_poll_wait.cpp`). **No per-thread figure exists [U]**. M1 and M6 are the first measurements. At 60 fps on the
   4060 the game thread holds with no missed frames, which says only "< 16.7 ms".
2. **The pump's 7 ms beat** (`mcla_cpu_pump.cpp:56`). A round frees the ring at most every 7 ms; at 8.3 ms per frame
   the phase between the two slides, and the producer waits up to a round. `mcla_cpu_pump_wake` (off) exists for exactly
   this case. Test it at 120, and listen to the audio.
3. **Physics substeps.** On the 1/120 grid, n = 1 per frame and `SubstepsFor` gives one substep of 1/120 s
   (`mcla_t_sim_math.h:51, 71`). That doubles the substeps per real second against 60 fps (n = 1, one 1/60 substep) and
   runs the tyre and contact model at half the shipped step. MCLA-4iOS ran 1/120 substeps (the comment at `:45-50`), but
   handling is not verified on PC. Cost [U], feel [U].
4. **UI at 30 Hz.** Flash movie steps are held at 30 a second (`kUiPeriodS`, `mcla_t_sim_math.h:184`;
   `mcla_ui_30hz` `mcla_t_common.cpp:97`), so the HUD animates at 30 at any frame rate. That is correct (the console
   rate) and costs nothing; the countdown uses its accumulator (`mcla_t_ui_countdown_dt.cpp`).
5. **Display.** The panel is 165 Hz. 120 is not an integer divisor, so G3 needs a 120 Hz mode or G-Sync (VRR).
   `PresentInterval` is exact only on a 120 Hz guest vblank (`mcla_t_sim_math.h:79`; `video_mode_refresh_rate=120`).
6. **Effects that scale with the frame.** Motion blur halves again at 120 (0.25 x per-frame displacement, VISUALS-7);
   `mcla_vis_motion_blur` compensates. Smoothing and the race clock are dt-based (`mcla_t_sim_math.h:117` onwards).
7. **GPU.** At 1080p the 4060 has room (about 6 ms) [lead's figure]. At 2000p 120 is out of reach [E]. G3 at the panel's
   native size is 1080p-class rendering plus an upscaler, or nothing.

---

## 4. T1 measurement plan (run first; each run settles a decision)

Common to every run: the machine slot (AGENT_SETUP §3), PowerShell, the warm shared cache `$WORK/user/cache`, the fixture
`after_intro` (the flow header gives it), fullscreen and in front (`--activate --cvar fullscreen=true`) so
`[present-stats]` is valid, the laptop on AC, and the same power mode recorded in `[mcla-sys]`. `W=C:/Users/Ali/Dev/mcla-x86-work`.
Base line = `--cvar mcla_profile=<P> --cvar mcla_fps_target=60 --cvar mcla_present_immediate=-1 --cvar mcla_gfx_preset=<preset>`.
Saturated line = `--cvar mcla_sim_cap=120 --cvar video_mode_refresh_rate=120 --cvar mcla_sim_hold30_s=0 --cvar mcla_gov=false --cvar gpu_timing=1`
(no cap, no hold, no governor). Add `--cvar mcla_thread_stats_s=5` to every run.

| Run | Command (beyond `python tools/run_game.py --flow quick_drive --config Release --work $W --game $W --activate --cvar fullscreen=true`) | Settles |
|---|---|---|
| **M1** 1080p ultra, saturated, per-op | `--run-id t1-m1-1080u` base line (1080p, ultra) + saturated line + `--cvar gpu_timing_ops=1 --cvar gpu_timing_csv=$W/runs/t1-m1-1080u/gt.csv` | The 4060's floor split (resolves, transfers, uploads) vs pixels; CPU ms per frame of Main XThread, GPU Commands, GPU Submission uncapped. **Decides R6-R9 and G3: if no CPU thread is above about 6 ms, T4 waits.** Also start a WPR CPU sampling capture (`wpr -start CPU`, via the elevated broker) for 30 s of the drive, for R8 |
| **M2** 2000p ultra, saturated, per-op | `--run-id t1-m2-2000u` base line (2000p, ultra) + saturated line + `--cvar gpu_timing_ops=1 --cvar gpu_timing_csv=...` | Today's 2000p GPU ms on the 4060, per phase group and per op, and the `[resolve-dual-write]` `kScaled` fallback count. **Decides whether G2 is in reach with R1-R3 (target ≤ 14 ms busy for a 1 % low ≥ 60) and their order** |
| **M3** 2000p ultra, capped, PresentMon | `python tools/bench.py --flow bench_drive --config Release --work $W --game $W --id t1-m3-2000u --repeat 2 --cvar mcla_profile=2000p --cvar mcla_fps_target=60 --cvar mcla_present_immediate=-1 --cvar mcla_gfx_preset=ultra --cvar fullscreen=true --cvar mcla_thread_stats_s=5` (panel with G-Sync on) | G2 as defined: 1 % low, `[present-stats]` on-time swaps, `[mcla-hitch]` count, governor rungs. The baseline every G2 project is judged against |
| **M4** 1x pixel ladder | Three runs like M2 with `mcla_profile=1000p`, `1200p`, `1080p`, ultra, saturated, `gpu_timing=1` | ms per megapixel at 1x. Extrapolated to 6.4 Mpx it gives the "2x path costs nothing extra" figure. **M2 minus that = R1's prize** |
| **M5** 2000p in-run A/B | M2's line + `gpu_experiment_cycle` of `vrs_level` 0/1/2 (the lk-vrs1 recipe; the LOCK60 write-up `scratch/SPRINT2/LOCK60.md` is on the old machine, not in this work folder), then separate runs with `--cvar mcla_gfx_shadow_res=384` (and 320 once allowed) | R3's and R2's gains at 2000p |
| **M6** 120 fps CPU | `--run-id t1-m6-120` 1080p perf + `--cvar mcla_sim_cap=120 --cvar video_mode_refresh_rate=120` on a 120 Hz display mode (machine slot: the display rule) or G-Sync, with `--schedule "60:set mcla_cpu_pump_wake=true;80:set mcla_cpu_pump_wake=false;100:set mcla_cpu_pump_wake=true"` | Which thread binds at 120, the `[mcla-cpu]` pump period, whether pump wake removes ring waits; world/real 1.000. **Decides T6's next step and whether R7 or R9 comes first** |
| **M7** cold boot | `--run-id t1-m7-cold` with an empty shader cache (run_game passes `--cache_root=$W/user/cache`, `tools/run_game.py:95`: move that folder aside for the run and restore it after, or check first that a later `--cvar cache_root=` wins), then the same again warm, with `--schedule mcla/schedules/vis_scenes.txt` | `[pipelines]` creations over time, compile-caused `[mcla-hitch]`, boot-to-title s. **R4's baseline** |
| **M8** spin and power A/B | 1080p ultra capped, `--schedule "56:set cp_idle_spin_loops=0;70:set cp_idle_spin_loops=64;84:set cp_idle_spin_loops=0;98:set cp_idle_spin_loops=64"` + `tools/segments.py <run> --settle 2` | R6: whether the CPU's spin watts move the 4060's clock (needs NVIDIA clock telemetry) |

Order: M1, M2, M3 first (one session, about 15 minutes of machine time), then M4 and M5 the same day; M6 after the
TV/120 Hz display work frees the display; M7 and M8 when convenient. Every run goes into METRICS with the build id,
TAINTED flag and telemetry columns. A run whose window lost focus is redone.

---

## 5. Do not do

| Idea | Why not here |
|---|---|
| A full renderer rewrite ("native D3D12 renderer") | No source: the draws come from guest code through the guest D3D. The only path is replacing emulation layers one at a time, measured (NATIVE_PLAN) |
| Draw batching or merging for GPU time | Per-draw GPU cost is 0.2-0.5 µs; the floor is resolves (LOCK60-A2) |
| VRS on the main scene | Slower (+0.2..+0.5 ms) and blurred (LOCK60-A4) |
| Exact deferred resolve memory writes; whole-resource clears | −0.09 ms and +0.26 ms (LOCK60-W2b). R5 approaches the same bytes by renaming instead |
| A 3200x2000 guest back buffer | 5,000 EDRAM tiles per surface against a reach of 2,048 (LEARNINGS Owner-request). Draw scale 2 is the route |
| Non-integer draw scale (1.5x) | The scale is an integer per axis, baked into each shader (`cache.cpp:263-273`, `dxbc_translator.cpp:127`). For a middle step use the FSR squeeze (`temporal_aa=fsr`) at 2000p, measured |
| Per-render-target draw scale in the SDK, as a first step | The RT cache shares EDRAM tiles between surfaces; mixed scales need transfers at every boundary. R2 gets most of the gain from the game side |
| Native MSAA at 1080p and above | EDRAM reach (3264 > 2048) and the 4x pause backdrop at 27 fps (VISUALS, MENUS60). Use SSAA (R10) or FXAA |
| Upscalers as the G2 answer before R1-R3 | They lost on the 880M (S1). On the 4060 they are a measured fallback only, and G2 asks for a true 3200x2000 picture |
| `vsync=false`, a free-running guest vblank | The vblank runs at about 1 kHz, every swap is late: the new laptop's stutter (NATIVE_PLAN status) |
| GPU scheduling priority; EcoQoS on hot threads | No effect, or a worse p99 (HOST) |
| Skipping a render phase by its draw-list mask | Still clears and resolves: flicker (Owner-report) |
| The `[rexcrt]` heap group | All-or-nothing, replaces the guest heap semantics (`overrides.toml:47-55`) |
| Patching the timer's reset guard 0x821BDB08 | Broke audio in LARecomp (`mcla_t_sim_step.cpp` header) |
| LTO expecting guest-code inlining; `-ffast-math` or FMA contraction on generated code | Guest functions are `weak, noinline`; FP changes alter physics (R7) |
| Spending T4 effort before M1 | At 60 fps the CPU holds; no thread number exists yet |

---

## 6. Open questions

**For a measurement:**
1. The 4060's per-operation split at 1080p and 2000p (M1, M2). Does the floor still dominate with dedicated VRAM?
2. CPU ms per frame of Main XThread, GPU Commands and GPU Submission, uncapped (M1, M6). Is any thread near 8.3 ms?
3. How much of 2000p is the scaled-path penalty (M2 minus M4)? This sizes R1.
4. Do 04e0, 0450/0540 and 0580 still cut at draw scale 2 (their log lines in M2)?
5. Does a CPU spin move the 4060's GPU clock (Dynamic Boost)? `segments.py` telemetry reads ADLX (AMD). NVIDIA needs
   NVML or `nvidia-smi --query-gpu=clocks.gr,power.draw -lms 100` added to `tools/telemetry`.
6. Hot guest routines: is `memcpy`/`memset` in the top ten of the profile (M1 + WPR)?
7. Is the `dxbc_ps_ieee_mul` white frame one shader (R11)?
8. Does the panel offer 120 Hz at 3200x2000, or only 165/60? Is G-Sync on for the internal panel (MUX mode)?

**For the owner:**
1. G2 picture: is a 3200x2000 output drawn by FSR from about 2133x1333 acceptable if R1-R3 fall short, or must it be
   true 2x?
2. G1 picture on the TV: offer 4x SSAA (R10) as the "quality" anti-aliasing when the GPU has room?
3. Minimum CPU: is an AVX2 (x86-64-v3) build acceptable as the default, with a v2 build kept for older PCs?
4. G3 feel: at 120 fps the physics substep is 1/120 s, half the step the handling was tuned at (the iOS port ran it).
   Accept it after a driving test, or ask for physics at 1/60 every other frame with interpolation (a larger project)?
5. A PGO profile and a shader pack are derived from playing the game: generated on each machine at setup and never
   shipped. Agreed?
