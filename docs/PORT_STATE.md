# MCLA x86-64 Windows port: state

Owner: ORC. The plan is [PLAN.md](PLAN.md); this file tracks where execution stands. Newest facts win.

## Environment

| Item | Value |
|---|---|
| Repo | `C:/Users/Ali/Development/mcla-x86` (`Ali-Niazi021/mcla-x86`, `main`) |
| `$IOS` | `C:/Users/Ali/Development/mcla-ios-ref` (read-only; HEAD `e24c575`) |
| `$WORK` | `C:/Users/Ali/Development/mcla-x86-work` (created W0.0) |
| `$GAME` | `C:/Users/Ali/Development/dumps/mcla-x360` (ISO + `default.xex` copy) |
| Game | Complete Edition (USA, Europe), redump 64472, title `545407F8`, media `5940C9DB`, v0.0.0.8, no TU |
| `default.xex` | 9,252,864 B, SHA-256 `c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432` |
| ISO | 7,838,695,424 B, XGD2, game partition `0x0FD90000` |
| Game files | 12 files, 6,561,615,524 B → `$GAME/game/` (G0) |
| Machine | Ryzen AI 9 365 (20 threads), 31.1 GB RAM, Radeon 880M (driver 32.0.32029.2010), panel 3200x2000 @ 120 Hz |
| Disk | C: ~209 GB free (2026-11-24) |
| VS | 2022 Community 17.14.13 (MSVC 14.44.35207), Windows SDK 10.0.26100 |
| CMake / Ninja / Python | 4.3.2 / 1.12.1 (VS-bundled) / 3.13.7 |
| Clang | LLVM 20.1.8 portable → `$WORK/toolchain/llvm-20.1.8` (W0.1) |
| SDK pin | ReXGlue v0.10.0 = `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` |
| Patch source | RenanQueiroz/mcla-recomp v0.3.17 = `0c3bbc9c1e369a7aebd8bbc8097ad823096f13ca` |

## Current phase

Wave 0 (ground truth, toolchain, first ports).

## Milestones

| ID | Goal | Status | Date |
|---|---|---|---|
| G0 | Game dump gate | todo | |
| M0 | SDK builds | todo | |
| M1 | Codegen clean, exe links, tests pass | todo | |
| M2 | Title screen | todo | |
| M3 | Free-roam driving, 10 min soak | todo | |
| M4 | Locked 30 fps at 720p | todo | |
| M5 | 60 fps at 720p | todo | |
| M6 | 60 fps at 1080p on the 880M | todo | |
| M7 | Polish | todo | |

## Task board

### Doing

### Todo

### Done

- W0.0 ORC: `$WORK` tree, PORT_STATE/LEARNINGS skeletons, role files in `.omp/agents/` (local only).

## Ownership in force

PLAN §7.2, unchanged.

## Next actions

## Decisions changed since PLAN

- **Role files live in `.omp/agents/`, not `.claude/agents/`.** The harness is omp, which loads project agents from
  `.omp/agents/*.md` (it skips `.claude/agents`) and rediscovers them at every spawn, so no session restart is needed.
  Models: IMP `anthropic/claude-sonnet-5-5` xhigh; RES/REV/VER `anthropic/claude-opus-5-5` medium. Gitignored.
