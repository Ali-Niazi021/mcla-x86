# SDK patch series

`series.toml` is the ordered list of patches that turn ReXGlue v0.10.0 (`f5337cd`) into the SDK this port builds on.
`tools/prepare_sdk.py` is the only thing that applies it (`fetch`, `apply`, `status`, `build`, `reset`, `check`,
`rehash`; see its docstring). The SDK tree, the build and the install live in `$WORK/sdk/`, never in this repo.

| Part | Entries | Where the patch files are |
|---|---|---|
| Renan's Windows list (`RenanQueiroz/mcla-recomp` v0.3.17, `0c3bbc9c`), in `scripts/make_pc.ps1` order, minus 3 platform-only patches (see `[[dropped]]`) | 72 | Fetched from a pinned clone (`$WORK/prior/renan-mcla-recomp`), not vendored. GPL-3. Each file's SHA-256 is in `series.toml` and checked before anything is applied |
| The owner's patches (`source = "local"`) | 22 | This directory: `0001`, `0100`, `0205`, `0211`, `0300`, `0310`, `0320` (ports of the iOS series), then the Windows patches written for this port (bucket `REDO`): `0400` precise guest vblank, `0410` `nfsmw_cp_event_seq` export, `0420` `MmQueryAllocationSize` from the address, `0430` vblank-woken `WAIT_REG_MEM` (the swap gate), `0440` guest vblank locked to the display's vblank, `0450` `skip_overwritten_transfers` fix (cull/polygon bits, why-not log, poison check), `0460` batched-load variants of the stretching direct resolve shaders (D3D12; DXBC built with `fxc`, 1 MB), `0480` `fullscreen_mode` (exclusive display mode), `0470` FSR 4.1 through AMD's signed runtime (`temporal_aa_upscaler`), `0490` dual-write resolve (`resolve_dual_write`), `04a0` swap from the host render target (`swap_direct_present`), `04b0` pixel-shader DXBC options (`dxbc_ps_refactoring_allowed`, `dxbc_ps_mad`, `dxbc_sample_bias`, `dxbc_forward_cf`, `dxbc_ps_ieee_mul`), `04c0` spin trims (`cp_idle_spin_loops`), `04d0` straight-line vertex fetch (`dxbc_vs_straight_fetch`), `04e0` tone-map transfer skip (`tonemap_transfer_skip`). The performance patches are off by default; `mcla/profile/pc_profile.cpp` turns on the validated ones |

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
