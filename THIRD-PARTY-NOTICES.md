# Third-party notices

This repository holds only this project's own code, configuration, scripts and documentation. It builds against, or
applies patches from, the projects below. Nothing here is a copy of the game or of generated code.

| Project | Pin | Licence | How it is used |
|---|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | v0.10.0, `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` | BSD-3-Clause. Copyright (c) 2026 Tom Clay; portions derived from Xenia, Copyright (c) 2022 Ben Vanik and Xenia project contributors | Recompiler and runtime. Fetched and patched by `tools/prepare_sdk.py` into `$WORK`; never vendored |
| [RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp) | v0.3.17, `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` | GPL-3.0 | Its Windows SDK patch series is fetched from the pinned commit, checked against the SHA-256s in `patches/sdk/series.toml` and applied by `tools/prepare_sdk.py`. Host and cvar-default patterns in `src/host/` and `mcla/profile/` follow its `src/mcla_app.cpp`, credited at the point of use |
| mcla-ios (the owner's iOS port) | `e24c575` | Same owner | Game layer (hooks, codegen config, tests, tools) ported from it, and the owner's SDK patches in `patches/sdk/` |
| [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp) | — | GPL-3.0 | Upstream history of Renan's fork; `runtime_discovered.toml` practice |
| [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and [game-patches](https://github.com/xenia-canary/game-patches) | — | BSD-3-Clause | Reference oracle; address confirmation. The XMA cross-buffer packet fix (`patches/sdk/0310-*`) follows Xenia Canary's decoder |
| [mzzvxm/LARecomp](https://github.com/mzzvxm/larecomp) | — | None (all rights reserved) | **Knowledge only** (addresses, pitfalls), credited where used. No code is copied |
| [LLVM](https://llvm.org) 20.1.8 | release asset | Apache-2.0 with LLVM exception | Compiler toolchain, installed in `$WORK`; not redistributed |
| [PresentMon](https://github.com/GameTechDev/PresentMon) 2.6.0 | release asset | MIT | Frame-time capture for benches; installed in `$WORK` |
