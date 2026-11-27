# Agent setup: how this project is worked on by agents

This file describes the agent setup used on the owner's machine so it can be recreated elsewhere: the tools, the roles,
the rules for sharing one PC, and the standing instructions. The plan and the status board are in
`docs/NATIVE_PLAN.md`; findings go to `docs/LEARNINGS.md`, numbers to `docs/METRICS.md`.

The repository ignores a root `AGENTS.md` on purpose (agent instructions stay local). On a new machine, create one that
says: "Read docs/AGENT_SETUP.md and docs/NATIVE_PLAN.md before any work; follow the machine rules in AGENT_SETUP section 3."

## 1. Tooling

- **Host app**: T3 Code running the Claude Code harness (Claude Agent SDK), full-access mode, Windows 11, PowerShell 7 and
  Git Bash. Sub-agents are started with T3 Code's `delegate_task` (model and effort per task); a finished sub-agent wakes
  the lead thread with its report.
- **Lead / orchestrator**: one long-lived thread on the strongest model available (Claude Fable 5.2 here). It plans,
  writes briefs, grants the machine, reviews reports, integrates, commits and pushes. It does not code or test itself
  except for trivial edits.
- **Tiers** (the owner's rule: be token-efficient, use the top tier only where intelligence is the bottleneck):

  | Tier | Model, effort | Used for |
  |---|---|---|
  | Commander | Claude Opus 5.5, high | architecture studies, root cause of hard bugs, design and review of risky SDK changes. Few, short-lived |
  | Soldier | Claude Sonnet 5.5, xhigh | implementation, builds, test runs, measurements, most research. The default |
  | Grunt | Claude Haiku 5.5, xhigh | inventories, log tabulation, mechanical edits, doc upkeep, run matrices from a recipe |

- **Memory**: the lead keeps short notes in the harness's project memory (`~/.claude/projects/<project>/memory/`): the
  delegation preference, the investigation state, the elevated broker. Everything a new machine needs is in this file and
  in `docs/`; the memory is only a cache of it.

## 2. Setting up a machine

1. `README.md` section "Setup": tools, then `python tools/setup.py --iso <your.iso>` (work folder `mcla-x86-work` next to
   the repo; `MCLA_WORK` overrides). `git config core.hooksPath .githooks` (keeps game data out of commits).
2. Test fixture: copy a save that is past the intro (`<user data>/<xuid>/545407F8/...`, the user data folder is
   `Documents\mcla_x86` unless the launcher says otherwise) to `$WORK/fixtures/after_intro/<xuid>/545407F8/`.
   Scripted runs copy it into a fresh user folder; the original is never written.
3. Shader cache for scripted runs: `$WORK/user/cache` (shared by all runs). Seed it from the player's
   `<user data>/cache/shaders` to start warm.
4. PresentMon 2.6.0 x64 from the GameTechDev/PresentMon releases to
   `$WORK/toolchain/presentmon/PresentMon-2.6.0-x64.exe`. It needs administrator rights to see frame data.
5. Elevation without a person at the keyboard (test machines only): `pwsh tools/elevate/start.ps1`, approve the one UAC
   prompt. Agents then run `python tools/elevate/elevated.py -- "<command>"` (`--status`, `--no-wait`, `--wait ID`,
   `--kill ID`; every command is logged in `$WORK/elevate/broker.log`). It lasts until reboot, logoff or `--stop`. While
   it runs, any program of that user can run administrator commands: do not start it on a machine that matters.
   Agents never stop or kill the broker and never run the game itself elevated.
6. Scripted run: `python tools/run_game.py --flow quick_drive --config Release --work <WORK> --game <WORK> --run-id <id>
   --cvar k=v ...` (118 s; `--summarize <log>` re-reads a log). Bench: `tools/bench.py`. Run from PowerShell, not Git
   Bash, for anything that configures CMake (a configure from Git Bash produced an SDK that does not link).

## 3. One PC, many agents: the machine rules

Measurements are only valid when nothing else loads the machine, and a build replaces the binaries a test is using. So:

- **The machine slot.** At most ONE agent at a time may build, run the game, capture with PresentMon or change the display
  mode. The lead grants it in the brief ("you hold the machine"); every other agent is told it does not hold it and must
  only read, analyse files, and write code. `tools/heavy_lock.py` (taken by `run_game.py`, `bench.py` and
  `prepare_sdk.py build`) is the technical backstop, not the schedule.
- **Without the slot** an agent may: read anything, parse logs and cache files (copy files a running game appends to before
  parsing), write code in its own files, and check it with `clang++ -fsyntax-only` using the build's flags. It may not
  link, start the game, or edit a file another running agent owns (the brief lists them).
- **The SDK tree** (`$WORK/sdk/src`) is one shared, patched checkout. One SDK writer at a time; every SDK change is a patch
  in `patches/sdk/` with a `series.toml` entry (`prepare_sdk.py rehash`, `status`, `build`).
- **Display mode** changes (a 60 Hz or 120 Hz test) use `ChangeDisplaySettingsEx` with `CDS_FULLSCREEN` (not saved), are
  restored in a `finally`, and the restore is verified. The machine is left at its original mode.
- **The game window** must be visible and in front for display-side numbers (`[present-stats]` prints UNRELIABLE for a
  covered window); `run_game.py --activate --cvar fullscreen=true` for those runs.
- **Parallel code changes** go in git worktrees (`delegate_task` isolation `worktree`) once the base is committed; a
  worktree agent does syntax checks only, and the slot holder integrates, builds and tests on the main checkout.
- **Crash safety**: one game instance, default compiler jobs (lower with `--jobs` if memory runs short), no agent kills
  processes it did not start, and long captures always have a timeout.

## 4. Briefs

A sub-agent starts with no history: its brief is everything it knows. Every brief has:

1. The repo and work-folder paths, and what to read first.
2. The goal and the evidence so far (log lines, numbers, run ids), and what is already ruled out.
3. Whether it holds the machine; the files it owns and the files it must not touch.
4. The exact commands for build and run, and the elevation line if it may need administrator rights.
5. "Do not commit" (the lead commits) unless it works in a worktree and is told to commit there.
6. What to report: files changed, numbers with run ids, what is proven and what is inference, what did not work.

The lead reads the report, checks the claims that matter (tests, a log line, `prepare_sdk.py status`), then integrates.
A follow-up check is a new task with a fresh brief, not a continuation.

## 5. Standing instructions from the owner

- Coding and testing go to sub-agents; the lead orchestrates.
- Commit and push as the work proceeds. Work branch: `native-pc` (pushed to `origin`); `main` is merged by the owner or
  on request. Commit only verified states; never commit game data, logs or anything from the work folder.
- If something in the game has to be reimplemented for the PC, do it; measure before and after.
- Watch Claude usage: at 98 % of the 5-hour limit, pause until it resets, then continue. The harness does not expose the
  usage meter to the agent, so the lead keeps the number of concurrent agents small, commits and updates the status board
  after every step (a stop loses nothing), and resumes from `docs/NATIVE_PLAN.md` after a reset.
- The test laptop may be changed freely (display mode, power settings, elevated commands). No damage concerns.
