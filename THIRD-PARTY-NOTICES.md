# Third-party notices

This repository holds only this project's own code, configuration, scripts and documentation. It builds against, or
applies patches from, the projects below. Nothing here is a copy of the game or of generated code.

| Project | Pin | Licence | How it is used |
|---|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | v0.10.0, `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` | BSD-3-Clause. Copyright (c) 2026 Tom Clay; portions derived from Xenia, Copyright (c) 2022 Ben Vanik and Xenia project contributors | Recompiler and runtime. Fetched and patched by `tools/prepare_sdk.py` into `$WORK`; never vendored |
| [RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp) | v0.3.17, `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` | GPL-3.0 | Its Windows SDK patch series (incl. FSR 3.1 temporal upscaling) is fetched from the pinned commit, checked against the SHA-256s in `patches/sdk/series.toml` and applied by `tools/prepare_sdk.py`. Designs re-implemented in our game layer and host, credited at the point of use: cvar defaults (`mcla/profile/pc_profile.cpp`, from his `src/mcla_app.cpp`), pacing at the present hook (`src/frame_timing.cpp`), EDRAM limit and single-tile rendering, guest render markers for FSR (`src/render_markers.cpp`), the 60 fps menu key repeat and title Start latch. His LARecomp-derived fixes are not used |
| mcla-ios (the owner's iOS port) | `e24c575` | Same owner | Game layer (hooks, codegen config, tests, tools) ported from it, and the owner's SDK patches in `patches/sdk/` |
| [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp) | — | GPL-3.0 | Upstream history of Renan's fork; `runtime_discovered.toml` practice |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) signed runtime 2.4.0 (`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`) | v2.4.0, `7539ae8b`, SHA-256 pinned in SDK patch 0470 | MIT (its `docs/license.md`, staged next to the exe as `amd_fidelityfx_signed_LICENSE.md`) | FSR 4.1.2 / 3.1.6 upscaling; fetched at configure time into `$WORK`, never in this repo |
| [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and [game-patches](https://github.com/xenia-canary/game-patches) | — | BSD-3-Clause | Reference oracle; address confirmation. The XMA cross-buffer packet fix (`patches/sdk/0310-*`) follows Xenia Canary's decoder |
| [mzzvxm/LARecomp](https://github.com/mzzvxm/larecomp) | — | None (all rights reserved) | **Knowledge only** (addresses, pitfalls), credited where used. No code is copied |
| [LLVM](https://llvm.org) 20.1.8 | release asset | Apache-2.0 with LLVM exception | Compiler toolchain, installed in `$WORK`; not redistributed |
| [PresentMon](https://github.com/GameTechDev/PresentMon) 2.6.0 | release asset | MIT | Frame-time capture for benches; installed in `$WORK` |
