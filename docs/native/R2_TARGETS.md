# R2: the game's off-screen render targets at draw scale 2

Study of 2026-11-27 for ROADMAP R2 (the soldier's brief: shadow map first, then environment, reflection and impostor targets) and R3. Static
work only: the generated code (`$WORK/generated/default`, the asm of every instruction is kept as a comment), the image
(`tools/re/xex_image.py`), the shader storage snapshots and the patched SDK source. Nothing here was run: every "safe" is a static audit, and
what a run must still show is in the last section. The scripts are in `$WORK/scratch/r2r3/` (`scan_640.py`, `scan_gen_640.py`,
`scan_rt_create2.py`, `scan_field_range.py`, `scan_singleton.py`, `scan_xsh.py`); addresses are guest addresses, a hook "before X" runs
before the instruction at X (`mcla/codegen/hooks_gfx.toml`).

## 1. Why the targets grow, and what shrinks them

The SDK draws every render target at the draw resolution scale (`draw_resolution_scale_x/y`, 2 in the `2000p` profile) and bakes it into each
translated shader; the guest never sees it (`TextureCache::GetConfigDrawResolutionScale`). A guest target of W x H is a 2W x 2H host target, and
its resolve goes through the scaled resolve buffer (host memory; the guest memory of the resolve texture keeps its guest size). So:

* **The guest memory is not affected.** The render-target pool of `mcla_ws_rt_pool.cpp` (renderer constructor 0x822E47E0, block created at
  0x822E4980, per-slot bump pointers) is guest physical memory sized for the guest full-screen targets; the draw scale lives in host memory
  (host render targets, the scaled resolve buffer, `texture_cache_memory_limit_render_to_texture` x (scale^2 - 1) of the SDK). A guest
  target that is made smaller only frees pool bytes: no pool change is needed for any hook below.
* **The cost is pixels.** Anything drawn into a target whose size the game takes from a constant costs 4x the pixels at scale 2 for no visible
  gain. The full-screen targets (scene, post chain, shadow collector, tone map) are meant to scale: they ARE the native picture.
* **What can shrink them is a guest-side size**, the way `mcla_gfx_shadow_res` does it: one constant register (or a field set from one)
  that the game stores into the target's description. A per-target host scale in the SDK is the clean version but large (ROADMAP section 5).

Baseline for the gains (T1 M2, `runs/t1-m2-2000u`: 2000p, `mcla_gfx_preset=ultra`, governor off, RTX 4060 Laptop, drive window 52-118 s,
`gpu_timing=1`, p50 of the per-frame group times in `gt.csv`): frame 11.83 ms; shadow 1.75, env 0.99, refl 0.13, pre 0.48, scene 2.60,
transl 0.45, comp 1.17, post 2.23, tonemap 0.91, ui 0.20, tail 0.13, swap 0.68.

## 2. Every `CreateRenderTarget` of the game (44 call sites)

The game's render targets are made by one virtual (slot +56 of the factory object at [0x82839CF0]; `lwz rX,-25232(rY)` after `lis rY,-32124`;
r4 = name, r5 = type 2 depth / 3 colour, r6 = width, r7 = height, r8 = bits per pixel). `scan_rt_create2.py` finds them and resolves the names:

| Target (name) | Guest size | Where the size comes from | Draw phase / use | Verdict |
|---|---|---|---|---|
| **Shadow map** (the CShadowMap's RT, 4 cascades, 4x MSAA depth; atlas 2w x 2h 24_8) | 640 x 640 | constructor sub_825F6940 `li r10,640` at 0x825F696C, fields +592 / +596 | phases 0x20..0x100 | **done**: `mcla_gfx_shadow_res`, 320 in `2000p` (section 3) |
| **dpMapFrontColor / Depth** (environment map, dual paraboloid, 4x MSAA) | 512 x 256 | `li r5,256` 0x8233AB34, `li r4,512` 0x8233AB38 in sub_8233AB18, fields +216 / +220 of the object [0x8288DCA4] | phases 0x4 and 0x8 (the cars' paint and speedometer reflection) | **done, default off**: `mcla_gfx_env_res` (section 4) |
| dpSpecMapFrontColor / WorkArea (blurred spec map) | 64 x 32 | `li r11,64; li r10,32` sub_82338FC0 (0x82338FC0 + 4 x 9) into +224 / +228 | the env phases | leave (4x of 2048 pixels) |
| **Road Reflection Color / Depth** | 256 x 256 | `li r7,256` 0x82314284, `li r6,256` 0x82314288 (depth) and 0x823142C8, 0x823142D8 (colour), sub_82314218; **no size field** in the object (only the two RT pointers at +4 / +8) | reflection view phase 0x2 (vtable 0x82043C4C; begin = sub_823143C0, `SetRenderTarget` of +8 / +4) | not done: section 5 |
| **ImpostorDepth** (shared) | 256 x 256 | `li r7,256` 0x8230B7FC, `li r6,256` 0x8230B804 (sub_8230B778) | tree impostors | not done: section 5 |
| ShadowImpostor | 128 or 256 square, 8 bpp | `li r6,128 / li r7,128` 0x8230B8B8 / 0x8230B8C0, replaced by 256 at 0x8230B8D4 / 0x8230B8D8 when a float of the type is above a threshold | impostors | not done: section 5 |
| ImpostorColor / ImpostorNormal | (count x 128) x 128, 32 / 16 bpp | `rlwinm r6,r10,7,0,24` (count << 7) 0x8230B92C and 0x8230B974, `li r7,128` 0x8230B938 / 0x8230B970 | impostors | not done: section 5 |
| FastCollectorNight / its Depth Buffer | 512 x 512 (8 bpp / 32) | `li r7,512; li r6,512` 0x82310EB4 / 0x82310EB8 and 0x82311008 / 0x8231100C (sub_82310E40) | the fast (night) shadow path, phase bit 0x400 | not done: section 5 |
| FastCollector, its Depth Buffer, FastCollectorTemp | 256 x 256 | 0x82310EF4 / 0x82310F08, 0x82310F5C / 0x82310F60, 0x82311074 / 0x82311078 | the same (day fast collector) | not done: section 5 |
| Shadow Collector, Depth Buffer (sub_825F4750) | screen size ([0x827D4274] x [0x827D4278]) | the back buffer | the screen-space shadow mask | meant to scale |
| stencil, tiling render depthbuffer (sub_822E5D40, sub_822E4CC8) | screen size ([r31+416] x [r31+420]) | the back buffer | scene | meant to scale |
| uiOverlay / uiOverlayDepth, sidekickColorRT / ZBuffer | 256 x 256 | `li r7,256; li r6,256` 0x821FE2F0.. / 0x821F9E60.. | the Flash UI's render-to-texture, the car preview | leave: UI sharpness at 2x is wanted |
| __hud_map, minimaprtdepthbuffer, __nav_arrow_depth | 220 x 220, 749 x 430, 256 x 256 | 0x821F7A40.., 0x821F7B74.. (sub_821F7920) | HUD map | leave (HUD sharpness) |
| Arrow RT, Arrow RT2, WorldIconDepthBuffer | 128 x 128 | 0x82262650.., sub_822E4C18 | HUD icons | leave |
| vinylWorkArea | 1024 x 1024, 512 x 512 | sub_82370C38 | the vinyl editor (garage) only | leave |
| depthbufferself / fullscreenself | **960 x 640** | `li r7,640` 0x821F89B4 / 0x821F89F4, `li r6,960` 0x821F89BC / 0x821F8A10 (sub_821F8960) | unknown (a UI / front-end capture?) | **check first**: 1920 x 1280 on the host, 2.5 MP; if it is drawn in free roam it is the largest unexplained off-screen target |
| Particle Pos ZW, Vel XY, Vel ZW (64 bpp state textures) | fields +660 / +664 of the particle system (sub_822EF0D8) | unknown value | the GPU particle update | candidate: 3 targets at 4x pixels, value of the fields to be read from a run |
| Collision Map | [r31+1248] square | sub_8249B380 | particle collision (phase 0x10) | candidate, value to be read |
| DmgTex, Fake Collector / Fake FastCollector (8 x 8) | [0x8288D04C] x [0x8288D050]; 8 x 8 | | | leave |

## 3. The sun-shadow map (done)

Hook 0x825F69A0 (already in the generated code) accepts 320 (and 256 and 192, which `ApplyPreset` produces at scale 2). The audit is in the
header of `mcla/hooks/mcla_gfx_shadow_res.cpp`; the short version: the size enters only through the two fields; EDRAM check
`(w>>4)*(h>>3) <= 3600` is 800 at 320; 320 = 10 x 32 = 20 x 16; the pixel shader constant register 5 is `{1/w, 1/h, w, h}` from the fields, so
a shader that takes its texel size from it sees 1/320 (a filter tap of +-1 guest texel is +-2 host texels: a penumbra about twice as wide as at
640 / 1x). No literal 640 or 1/640 is shadow data in the image, the generated code or the ucode of 1079 stored shaders. A gfx preset (which sets
`mcla_gfx_shadow_res` as an app default at first use) would have undone the profile's 320 with its own 512 / 640: `ApplyPreset` now halves its
value at scale 2 (perf 512 -> 256, quality / ultra 0 -> 320; `ShadowEdgeAtScale`, unit-tested). A value given by hand (command line, config) is
used as it is.

## 4. The environment map (done, off by default)

Hook 0x8233AB3C (before `bl 0x8233A648` in sub_8233AB18), registers r4 = 512, r5 = 256 -> 384 x 192 or 256 x 128 (`mcla_gfx_env_res` = 384 /
256). New codegen entry `[G8]` in `hooks_gfx.toml`: **the integrator must run `python tools/codegen.py` once**. The audit is in the header of
`mcla/hooks/mcla_gfx_env_res.cpp`: the constructor stores the two numbers as +216 / +220 and every reader found (the constructor's viewport,
the target creation, the two render functions sub_8233AB98 / sub_8233AF30 with the hemisphere rectangle `width / 2`) uses the fields; no other
512 / 256 literal is in the class's code; the twelve users of the singleton read no field. It is NOT in the `2000p` profile: ROADMAP's rule
(a cvar is turned on in the profile after its checks) and two things only a run shows (a seam at the border of the two hemispheres, the spec
map's blur). The line to add after the checks: `{"mcla_gfx_env_res", "256"}` in `k2000pDefaults`.

## 5. Not done, with the addresses and what blocks each

* **Road Reflection** (256 x 256 colour and depth, 0x82314284 / 0x82314288 / 0x823142C8 / 0x823142D8): four constant registers at two call
  sites, so a hook of two registers per site is easy (each pair before its `bctrl`: 0x8231429C and 0x823142E0). What blocks it is the audit: the
  object keeps no size, so the reflection pass's viewport, its projection and any blur in the road shader either read the render target or hold
  a literal 256 of their own; the class's code (0x82313000..0x82316000) has no 256 / 128 / 512 literal other than the creation, and
  `SetRenderTarget` of the begin function (sub_823143C0, virtual slot +60 of the factory) is the only place the pass binds the target, but the
  draws of the reflection phase are made by the generic scene code with a camera the phase sets elsewhere (not traced). Pixel cost at
  2x: 0.26 MP of colour and depth against 0.07 at 1x, small; the group is 0.13 ms (refl) at 2000p, so the lever is worth at most about 0.1 ms.
* **Tree impostors** (sub_8230B778 creates ImpostorDepth 256 x 256, ShadowImpostor 128 / 256, and the colour / normal atlases of count x 128 x 128):
  the view size is a per-type class (64, 128, 256) chosen by compares in sub_8230F8E0 (`cmpwi 256 / 64 / 128`), used by the type loop
  sub_8230CF50 and by sub_823095A8 (655 asm lines, `li 128 / 512`), and the atlas width is `count << 7`: at least four sites must agree, so a
  size hook is not a one-register change. Cost: about six types are regenerated per frame (1800 requests per 300 frames, `mcla_gfx_impostors.cpp`),
  128 x 128 each: about 0.1 MP at 1x, 0.4 MP at 2x, below the cost of the views' clears and resolves. Not worth the risk.
* **Fast shadow collector** (night / `mcla_gfx_shadow_mode=1`: FastCollectorNight 512 x 512 + depth, FastCollector 256 x 256 + depth + temp, the
  literals listed above in sub_82310E40): the same shape as the road reflection (literal pairs at five call sites, no size field), larger
  pixel cost at night (two 1024 x 1024 targets at 2x). Wants the same reader audit of 0x82310000..0x82312000 (the class that owns the
  collectors) plus the night path's projection; the candidate after the environment map if a night run shows `shadow` or `pre` groups too high.
* **depthbufferself / fullscreenself** 960 x 640 (0x821F89B4 etc.): first find out what draws into it (the resolve log at 2000p lists the
  destination size), then size it the same way.

## 6. What a run must still show (see the integrator's test plan in the task report)

Shadow 320: penumbra softness and acne / peter-panning of the car's own shadow in motion; far cascade flicker with `mcla_gfx_far_cascades_interval=2`.
Environment map 256 (run: no seam, the reflection as soft as at 1080p, now a 2000p default). VRS level 2 (run: -0.1 ms, distant tree masses stair-stepped: off): the post chain and
the glass / particles at 2000p against `--vrs_level=0` (parked screenshot diff, the post-processing phases), and that the main scene and the HUD
stay sharp.
