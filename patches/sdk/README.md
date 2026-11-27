# SDK patch series

`series.toml` is the ordered list of patches that turn ReXGlue v0.10.0 (`f5337cd`) into the SDK this port builds on.
`tools/prepare_sdk.py` is the only thing that applies it (`fetch`, `apply`, `status`, `build`, `reset`, `check`,
`rehash`; see its docstring). The SDK tree, the build and the install live in `$WORK/sdk/`, never in this repo.

| Part | Entries | Where the patch files are |
|---|---|---|
| Renan's Windows list (`RenanQueiroz/mcla-recomp` v0.3.17, `0c3bbc9c`), in `scripts/make_pc.ps1` order, minus 3 platform-only patches (see `[[dropped]]`) | 72 | Fetched from a pinned clone (`$WORK/prior/renan-mcla-recomp`), not vendored. GPL-3. Each file's SHA-256 is in `series.toml` and checked before anything is applied |
| The owner's patches (`source = "local"`) | 42 | This directory: `0001`, `0100`, `0205`, `0211`, `0300`, `0310`, `0320` (ports of the iOS series), then the Windows patches written for this port (bucket `REDO`): `0400` precise guest vblank, `0410` `nfsmw_cp_event_seq` export, `0420` `MmQueryAllocationSize` from the address, `0430` vblank-woken `WAIT_REG_MEM` (the swap gate), `0440` guest vblank locked to the display's vblank, `0450` `skip_overwritten_transfers` fix (cull/polygon bits, why-not log, poison check), `0460` batched-load variants of the stretching direct resolve shaders (D3D12; DXBC built with `fxc`, 1 MB), `0480` `fullscreen_mode` (exclusive display mode), `0470` FSR 4.1 through AMD's signed runtime (`temporal_aa_upscaler`), `0490` dual-write resolve (`resolve_dual_write`), `04a0` swap from the host render target (`swap_direct_present`), `04b0` pixel-shader DXBC options (`dxbc_ps_refactoring_allowed`, `dxbc_ps_mad`, `dxbc_sample_bias`, `dxbc_forward_cf`, `dxbc_ps_ieee_mul`), `04c0` spin trims (`cp_idle_spin_loops`), `04d0` straight-line vertex fetch (`dxbc_vs_straight_fetch`), `04e0` tone-map transfer skip (`tonemap_transfer_skip`), then the 14 of sprint 2, in series order: `0600` live FXAA and texture LOD bias (`swap_post_effect` is read on every swap; `texture_lod_bias`), `04f0` GPU timing and the governor's exports (`gpu_timing`, `gpu_timing_csv`, `gpu_timing_export`, `gpu_draw_experiment`, `gpu_experiment_cycle`; data `rexgpu_gpu_frame_us`, `rexgpu_vrs_level`), `0500` per-draw variable rate shading by render phase (`vrs_level`, `vrs_levels`, `vrs_min_target_width`), `0510` the exported frame time is the GPU busy time and `vrs_levels` is the two measured post levels, `0700` HDR back-buffer dumps (`hdr_dump_dir`, `hdr_dump_at`, `hdr_dump_every_s`), `0710` fused HDR scene at about SDR cost (`hdr_scene_fused`, `hdr_scene_fused_check`, `hdr_trim`, `hdr_ui_mode`), `0720` FXAA on the HDR image (`hdr_fxaa`), `0730` `hdr=on` stays SDR on a display that is not in HDR mode (no cvar), `0740` fused HDR as a gain on the game's own pixel (`hdr_scene_fused_gain`), `0520` GPU time per emulation operation (`gpu_timing_ops`), `0530` dual write for MSAA and offset resolves, deferred resolve memory write, full-resource clears (`resolve_dual_write_msaa`, `resolve_defer_memory_write`, `clear_full_resource`, `resolve_dual_write_nomem_dev`), `0540` `skip_overwritten_transfers` and `tonemap_transfer_skip` together (no cvar), `0580` the UI overlay counts only while drawn (`ui_overlay_active_when_drawn`), `0590` pipeline-creation and shader-translation export (data `rexgpu_pipeline_creations`, `[pipelines]` log line; no cvar). The performance patches are off by default; `mcla/profile/pc_profile.cpp` turns on the validated ones (among them `04e0`, `0450` with `0540`, `0530`'s `resolve_dual_write_msaa` and `0580`); the HDR patches act only when `hdr` is `auto` or `on`. Then, in series order: `0750` display statistics `[present-stats]`, `0755` the same counters as the exported struct `rex_present_stats` (no behaviour change, for the overlay), `0790` the render target cache's ownership-transfer and dump pipelines kept across runs (`<title>.rtv.d3d12.xrtp`, made at start; no cvar), `0791` the `[pipeline-storage]` report and the split `[pipelines]` line (no behaviour change), `0792` the shader / pipeline storage flushed before the hard exit (no cvar), `0760` `scaled_copy_levers`: the dual write, the direct present and the tone-map skip at a draw resolution scale above 1 (bits 1, 2, 4, 8; default 0) |

Vendored patches are `git format-patch`-style files: a header (`From`, `Subject`, then `Source`, `Licence`, `Purpose`,
`Bucket` lines, then the original author's notes) and, after a lone `---` line, the diff. `git apply` ignores
everything before the first `diff --git`. The diff bytes are LF and exact (`.gitattributes`: `*.patch -text -whitespace`).

## Changing the series

- **New or revised local patch**: write it as a git diff against the tree the series produces up to that point, add the
  header, put the file here, add (or edit) its `[[patch]]` entry at the end of the local group, run
  `python tools/prepare_sdk.py rehash`, then `reset` (applies the series afresh) and `build`. A patch that changed after it
  was applied is never re-applied on top of the old one: `status` reports it as drifted.
- **New patches go last**, so the record in `$WORK/sdk/src/.mcla-x86/series.applied` stays a prefix and `apply` only
  adds. Wave 3 added its Windows patches after `0320`: `0400` (cvars `guest_vblank_precise`, `guest_vblank_spin_us`; log
  line `[guest-vblank] period p50=.. p99=.. late=.. resync=.. early=.. n=.. src=display|qpc` every 10 s, `src` from
  `0440`), `0410` (the data symbol `nfsmw_cp_event_seq` exported by `rexgpu-xenos.dll`), `0420` (cvar
  `mm_query_allocation_size_from_address`), `0430` (cvar `cp_wait_vblank_wake`), `0440` (cvars
  `guest_vblank_display_lock`, `guest_vblank_display_offset_us`), `0450` (cvars `skip_overwritten_transfers_log`,
  `skip_overwritten_transfers_poison`), `0460` (none: `resolve_direct_batched_loads` picks the batched stretch shaders; how
  the bytecode is made is in its header).
- **Renan's pin** changes only by editing `[pins]` and every Renan SHA-256 together; his patches apply in his order.
- `status` prints `sdk f5337cd: series <applied>/<total> applied, <k> drifted` and `series fingerprint <hex16>`. The
  fingerprint (record it in `docs/BUILDS.md`) is the SHA-256 over the series' SHA-256s, so it changes with any patch.
