#!/usr/bin/env python3
"""run_game.py - run mcla.exe with a scripted pad, watch the log, and summarise the run (PLAN Appendix C).

    python tools/run_game.py --flow <name> [--fixture <name>] [--seconds N] [--api d3d12|vulkan] [--cvar k=v]...
                             [--screenshot-every S] [--run-id ID] [--config RelWithDebInfo|Release] [--exe PATH]
                             [--no-lock] [--stop-on-reach] [--expect STATE] [--topmost]
                             [--schedule SPEC]... [--window-pos POS] [--behind | --no-behind]
                             [--work W] [--game G] [--lock-timeout S]
    python tools/run_game.py --flow <name> --check-flow [--schedule SPEC]...
    python tools/run_game.py --selftest-burn
    python tools/run_game.py --summarize <run-id | run dir | log file> [--expect STATE]

  --flow NAME      a pad script: tools/flows/<NAME>.txt, or a path to a script file. Format of the host's --input-file
                   (<second>:<button>[+<button>...][/<hold ms>], '#' comments; seconds count from process start).
                   Header comments (`# expect: menu`, `# seconds: 180`, `# fixture: after_intro`) give the state the
                   flow must reach (none|title|menu|free-roam), the default --seconds and the fixture it is made for.
  --fixture NAME   copy $WORK/fixtures/<NAME>/ into the fresh user dir before the start. The fixture is a copy of a
                   run's user/ folder: <xuid>/545407F8/{00000001/mc4.sav/..., Headers/00000001/mc4.sav.header, ...}.
                   The fixture itself is never written; every run gets its own copy.
  --seconds N      wall-clock run time, counted from process start (the script's clock); then the process tree is
                   killed (the game never quits by itself, and its shutdown can hang ~20 s). Default: the flow's
                   `# seconds:` header, else 180.
  --api A          GPU backend, `--mcla_gpu_backend=A` (d3d12 default, vulkan).
  --cvar k=v       extra `--k=v` on the command line, repeatable. A key that run_game itself sets (e.g. fullscreen)
                   is replaced, not duplicated.
  --screenshot-every S   every S seconds (>= 1) capture the game window's client area to shots/t<sec>.png (plus
                   shots/t<sec>_end.png just before the kill). PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) is the
                   primary capture (works for a covered window, and while the screen saver or the lock screen owns the
                   desktop); a screen BitBlt is the fallback (then the window must be visible and not covered:
                   --topmost raises it above other windows without taking focus). summary.json shot_methods counts both.
                   The run keeps the display and the system awake (SetThreadExecutionState), restored at the end.
  --audio          keep the game's sound (default: the run is muted with --audio_mute=true).
  --activate       let the game window take the keyboard focus (default: it is shown without activation, so whoever is
                   working on the machine keeps typing where they were; the scripted pad needs no focus).
  --window-pos POS where the game window goes, `--mcla_window_pos=POS` (a host cvar): bottom-right | bottom-left | top-right |
                   top-left | X,Y (pixels, may be negative) | "" (the SDK default, centred: no argument is passed).
                   Default bottom-right, but "" with --topmost or --activate (a window that is meant to be seen).
  --behind / --no-behind   `--mcla_window_behind=true` (a host cvar): after the window is shown it is put in the z-order just
                   behind the current foreground window, without activating it. Default on, off with --topmost / --activate.
                   (Both are only the defaults of build_args: a `--cvar mcla_window_pos=...` / `mcla_window_behind=...` of the
                   caller replaces them like any other key run_game sets.) Quietness, in all: audio muted (--audio_mute), the
                   process starts with SW_SHOWNOACTIVATE, the environment of the game gets SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0
                   (SDL3 reads its hints from the environment; it keeps SDL's own ShowWindow from activating the window;
                   left alone with --activate), and the window is placed out of the way (bottom-right, behind).
  --schedule SPEC  a timed schedule added to the run (repeatable); SPEC is a file (a path, or a name in tools/flows/schedules/,
                   e.g. `ab_example`; same grammar as a flow, '#' comments, only the four step kinds below) or an inline
                   `;`-separated string (`58:set mcla_fps_cap=0;70:burn 4;80:burn 0`; a '#' starts a comment as in a file).
                   Validated like the flow, then appended to the run's flow.txt (the host reads that file), so any flow carries an
                   A/B schedule without a copy. Steps may also sit in the flow file itself.
  --stop-on-reach  stop (after 5 s for the log to flush) as soon as the expected state is reached.
  --expect STATE   override the flow's `# expect:` header (none|title|menu|free-roam).
  --config C       build config of $WORK/build/bin/<C>/mcla.exe (default RelWithDebInfo). --exe PATH overrides.
  --no-lock        do not take the heavy lock (use only when a parent already holds it or for a dry experiment).
  --check-flow     only validate the flow file (same grammar as the host) and print its steps; no lock, no run.
  --summarize X    only parse an existing log and print the run line (re-analysis of a finished run).
  --work W / --game G   $WORK (default env MCLA_WORK, else C:/Users/Ali/Development/mcla-x86-work) and $GAME
                   (env MCLA_GAME, else C:/Users/Ali/Development/dumps/mcla-x360; the game files are in $GAME/game).

Schedule steps (the host's SchedStep, src/host/input_script.h; tools/segments.py cuts a run at them)
  <seconds>:set <cvar>=<value>   the host changes a run-time cvar then. cvar [A-Za-z0-9_]+ (not burn / burnmem / mark); value
                                 non-empty without whitespace, ';' or '#', passed on verbatim; one pair per step.
  <seconds>:burn <n>             n CPU (ALU) spinner processes, 0..32 (0 = stop them). EXECUTED HERE; the host only logs it.
  <seconds>:burnmem <n>          n memory-bandwidth spinner processes, 0..32, executed here.
  <seconds>:mark <label>         a named marker, label [A-Za-z0-9_.-]{1,40}.
  The keyword is the first word after the colon (case-insensitive); no /hold and no '+' on these steps. The host logs
  `[mcla-sched] t=<s> set <name>=<value> ok now=<value>` (or FAILED (rejected)), `... burn <n>` / `burnmem <n>` / `mark <label>` and
  `segment <k>: <label>` (k from 0 = the initial state; label = every key the schedule touches as sorted key=value).
  Spinners (burn / burnmem): child processes `python -I -S -c <code> <run_game pid>`, CREATE_NO_WINDOW, no stdio, normal priority,
  in the game's job object (they die with the run however it ends) and killed explicitly on every way out of the run (timeout,
  stop-on-reach, the process exited, interrupted). ALU spinner = a tight integer loop; memory spinner = two 64 MiB bytearrays copied
  into each other (memcpy bandwidth). A spinner exits by itself within ~0.1-1 s when its parent run_game process is gone. At a
  step's time (the script clock = process start; the loop wakes for it, plus the spawn time of its processes) the number of live
  spinners of that kind is set (more are started, extra ones killed); none exists before the game starts or after it ends. Printed when the counts change:
  `[run_game] burn: t=<s> cpu=<n> mem=<n>`; summary.json burn_events [[t, kind, n]...] has every executed step.
  `--selftest-burn` runs the spinner logic without a game (2 ALU + 1 memory spinner for 2 s in a private job, then checks they are
  gone, and that an orphaned spinner ends by itself).

What a run does
  1. Takes the heavy lock (tools/heavy_lock.py, FIFO; re-entrant under `heavy_lock.py -- python tools/run_game.py ...`).
  2. Makes $WORK/runs/<id>/ (id default r<yyyymmdd-HHMMSS>-<flow>) with user/ (fresh, plus the fixture), shots/,
     flow.txt (the script as run), mcla.log, stdout.log (anything the process prints), summary.json.
  3. Launches mcla.exe with, in this order:
         --game_data_root=$GAME/game --user_data_root=$WORK/runs/<id>/user --cache_root=$WORK/user/cache
         --log_file=$WORK/runs/<id>/mcla.log --log_level=info --fullscreen=false --mcla_gpu_backend=<api>
         --input-file=$WORK/runs/<id>/flow.txt   [--audio_mute=true] [--mcla_window_pos=<pos>] [--mcla_window_behind=true]
         [--<cvar>=<v> ...]
     (cache_root is shared between runs so the shader/pipeline storage survives the fresh user dirs.) The process runs
     in a Windows job object, so the whole tree dies with this script however it ends.
  4. Tails the log live and prints the notable lines (first engine frame, reached states, [mcla-mode] changes,
     fatals, unregistered calls, watchdog alarms, save-backup lines, the first errors), takes the screenshots, and
     stops at --seconds (or at --stop-on-reach, or when the process exits by itself, which counts as one fatal).
  5. Parses the whole log (rotated mcla.N.log files included), writes summary.json and prints, as the LAST line:
         run <id>: reached=<none|title|menu|free-roam> fatal=<n> unregistered=<n> heartbeat_last=<s>
     Exit 0 iff fatal=0, unregistered=0 and `reached` is at least the flow's expected state. Exit 1: the run was
     done but failed that test. Exit 2: setup problem (no exe, flow, fixture, game files; nothing was run).
     Exit 75: --lock-timeout expired. Exit 130: interrupted (the game is killed; summary.json is written).

Definitions (log lines; the file log pattern is `[Y-m-d H:M:S.mmm] [level] [category] [tTID] message`)
  reached      the highest of: title  = `[mcla-heartbeat] first engine frame`;
                               menu   = `[mcla-state] Startup/Startup_Checks` AFTER the first engine frame (the game
                                        registers its states before the first frame, so earlier lines do not count);
                               free-roam = `[mcla-mode] <from> -> free-roam`.
  fatal        EVENTS matching `\\[mcla-fatal\\]|\\[FATAL\\]` (the RAGE fatal hook and the SDK's REX_FATAL* macros), plus 1
               when the process exited by itself before the end of the run (a crash or the silent XamShowDirtyDiscErrorUI
               exit). Not counted: the hook status lines written at startup (`[mcla-<tag>] active (...`, `off (--...`,
               `armed, off by default (...`, `compiled out (...`; the RAGE hook's tag is `fatal`). One event is a burst:
               matching lines within 1 s of the previous one belong to it (the hook writes a message line and a `raw:`
               line), so a fatal counts once; summary.json fatal_lines has the raw line count. The SDK's
               unregistered-function line is a [FATAL] line, so it counts in both numbers.
  unregistered lines containing `unregistered function` (`[FATAL] Call to invalid or unregistered function at guest
               address 0x...`).
  heartbeat_last   whole seconds since process start of the last liveness line after the first engine frame:
               `[mcla-heartbeat]` (the first-frame line and the host watchdog's periodic `frames=<n>` line),
               `[mcla-sim]`, `[mcla-state]` or `[mcla-mode]`; 0 when there is none. The last one before the end of a
               healthy run is the watchdog's, so summary.json heartbeat_gap_s (= elapsed_s - heartbeat_last_s) is a few
               seconds at most. The host watchdog's alarm lines (`[mcla-watchdog] N s without a single register moving`)
               are counted as hang_alarms in summary.json; they do not change the exit code.
  A log is read from its own clock: t = line time - process start time (when run by run_game) or - first line time
  (when summarising a bare log).

summary.json keys: run_id, state (running|done), flow, flow_file, fixture, fixture_copied {files, bytes}, api, config,
  exe, args, window_pos, window_behind, schedule (the --schedule specs), schedule_steps (every schedule step of the run: the
  flow's and --schedule's, as written, sorted by time), burn_events [[t, kind, n]...], seconds, started, ended, elapsed_s, ended_by (timeout|stop-on-reach|exited|interrupted), exit_code (only
  when the process exited by itself), exit_status, pid, expected, reached, expect_met, ok, fatal, fatal_log (events),
  fatal_lines, fatal_process, unregistered, hang_alarms, heartbeat_last, heartbeat_last_s, heartbeat_gap_s,
  heartbeat_lines, heartbeat_frames_last, t_title, t_menu, t_free_roam (seconds since process start), mode_changes
  [[t, from, to]...], error_lines, save_rewrite_start, save_closed, save_restored, user_saves [{path, bytes}], shots,
  shot_notes, shot_methods, log, log_files, log_missing, log_lines, log_last_s, t0_epoch (process start, epoch s), fatal_samples,
  unregistered_samples, error_samples.

From Python (tools/bench.py later):
    import run_game
    s = run_game.parse_log("C:/.../mcla.log")                     # the parser: dict with the keys above
    summary = run_game.run(run_game.RunOptions(flow="bench_drive", fixture="after_intro", seconds=360,
                                               cvars=["mcla_sim60=true"]), on_start=lambda pid, run_dir: ...)
"""

import argparse
import contextlib
import datetime
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple, Union

HERE = Path(__file__).resolve().parent
FLOWS_DIR = HERE / "flows"
SCHEDULES_DIR = FLOWS_DIR / "schedules"    # schedule fragments for --schedule: not flows, so kept out of flows/*.txt

if str(HERE) not in sys.path:  # tools/ is on sys.path when run as a script; this lets `import run_game` find heavy_lock
    sys.path.insert(0, str(HERE))
import heavy_lock  # noqa: E402

DEFAULT_WORK = "C:/Users/Ali/Development/mcla-x86-work"
DEFAULT_GAME = "C:/Users/Ali/Development/dumps/mcla-x360"
DEFAULT_CONFIG = "RelWithDebInfo"
DEFAULT_SECONDS = 180.0
TITLE_ID = "545407F8"
STOP_GRACE_S = 5.0          # --stop-on-reach: the log is flushed every 2 s (log_flush_interval)
KILL_WAIT_S = 15.0

STATES = ("none", "title", "menu", "free-roam")
RANK = {s: i for i, s in enumerate(STATES)}

EXIT_FAILED = 1
EXIT_SETUP = 2
EXIT_LOCK_TIMEOUT = 75
EXIT_INTERRUPTED = 130


class Fail(Exception):
    """A setup problem: nothing was run. Exit code 2."""


# ================================================================================================================
# The log parser (pure; importable)
# ================================================================================================================

_RE_TITLE = re.compile(r"\[mcla-heartbeat\] first engine frame")
_RE_MENU = re.compile(r"\[mcla-state\] Startup/Startup_Checks\b")
_RE_MODE = re.compile(r"\[mcla-mode\] (?P<from>[a-z][a-z-]*) -> (?P<to>[a-z][a-z-]*)")
_RE_FATAL = re.compile(r"\[mcla-fatal\]|\[FATAL\]")
# What mcla_hooks.cpp AnnounceHooks() writes for every hook at startup, whatever its tag (the RAGE fatal hook's is
# `fatal`): `[mcla-fatal] active (--mcla_rage_fatal=false turns it off)` is a status line, not a fatal.
_RE_STATUS = re.compile(r"\[mcla-[A-Za-z0-9_-]+\] (?:active \(|off \(--|armed, off by default \(|compiled out \()")
_FATAL_EVENT_S = 1.0     # fatal lines this close to the previous one are the same event
_RE_UNREG = re.compile(r"unregistered function")
_RE_LIVE = re.compile(r"\[mcla-(?:heartbeat|sim|state|mode)\]")
_RE_FRAMES = re.compile(r"\[mcla-heartbeat\].*?\bframes=(\d+)")
# Save-backup patch (SDK `saved-game-backup`): rewrite start, close, restore after a crash.
_RE_SAVE_START = re.compile(r"content: saved game '.*' is being rewritten")
_RE_SAVE_CLOSED = re.compile(r"content: '.*' closed; the previous contents of saved game")
_RE_SAVE_RESTORED = re.compile(r"content: saved game '.*' was being rewritten when the game ended")
# A cheap byte test before any decoding: only these lines carry facts (a real log is mostly other lines). The level is
# read from the first bytes of the line instead (`[2026-11-24 12:00:00.123] [error] ...`).
_IMPORTANT = re.compile(rb"\[mcla-|\[FATAL\]|unregistered function|content: ")

_SAMPLE_CAP = 10
_MODE_CAP = 200
_T0_ATTEMPTS = 50

LogSource = Union[str, os.PathLike]


def _parse_ts(head: str) -> Optional[float]:
    """Epoch seconds of a `[YYYY-MM-DD HH:MM:SS.mmm]` prefix (local time, as spdlog writes it), else None."""
    if len(head) < 25 or head[0] != "[" or head[24] != "]" or head[5] != "-" or head[8] != "-" \
            or head[11] != " " or head[14] != ":" or head[17] != ":" or head[20] != ".":
        return None
    try:
        return datetime.datetime(int(head[1:5]), int(head[6:8]), int(head[9:11]), int(head[12:14]),
                                 int(head[15:17]), int(head[18:20]), int(head[21:24]) * 1000).timestamp()
    except ValueError:
        return None


class LogScanner:
    """Feeds on log lines (bytes, one per call) and keeps the run's facts. Used live (run_game's tail) and offline
    (parse_log). `feed_bytes` returns a kind for lines worth printing live: reach, fatal, mode, watchdog, save,
    critical, error; else None."""

    def __init__(self, t0: Optional[float] = None) -> None:
        self.t0 = t0
        self.lines = 0
        self.title_seen = False
        self.menu_seen = False
        self.free_roam_seen = False
        self.t_title: Optional[float] = None
        self.t_menu: Optional[float] = None
        self.t_free_roam: Optional[float] = None
        self.fatal = 0                  # events
        self.fatal_lines = 0
        self._fatal_ts: Optional[float] = None
        self.fatal_samples: List[str] = []
        self.hang_alarms = 0
        self.unregistered = 0
        self.unregistered_samples: List[str] = []
        self.hb_last: Optional[float] = None
        self.hb_lines = 0
        self.hb_frames_last: Optional[int] = None
        self.mode_changes: List[List] = []
        self.error_lines = 0
        self.error_samples: List[str] = []
        self.save_start = 0
        self.save_closed = 0
        self.save_restored = 0
        self._last_ts: Optional[float] = None     # time of the last line that was examined
        self._last_raw: Optional[bytes] = None    # a later line that was skipped (its time is read at the end)
        self._t0_tries = 0

    # ------------------------------------------------------------------ results
    @property
    def reached(self) -> str:
        if self.free_roam_seen:
            return "free-roam"
        if self.menu_seen:
            return "menu"
        if self.title_seen:
            return "title"
        return "none"

    def last_ts(self) -> Optional[float]:
        if self._last_raw is not None:
            ts = _parse_ts(self._last_raw[:25].decode("ascii", "replace"))
            if ts is not None:
                return ts
        return self._last_ts

    def result(self) -> Dict:
        end = self.last_ts()
        log_last = None if end is None or self.t0 is None else max(0.0, round(end - self.t0, 3))
        hb = self.hb_last
        return {
            "t0_epoch": self.t0,
            "log_lines": self.lines,
            "log_last_s": log_last,
            "reached": self.reached,
            "t_title": self.t_title,
            "t_menu": self.t_menu,
            "t_free_roam": self.t_free_roam,
            "fatal_log": self.fatal,
            "fatal_lines": self.fatal_lines,
            "fatal_samples": list(self.fatal_samples),
            "hang_alarms": self.hang_alarms,
            "unregistered": self.unregistered,
            "unregistered_samples": list(self.unregistered_samples),
            "heartbeat_last_s": hb,
            "heartbeat_lines": self.hb_lines,
            "heartbeat_frames_last": self.hb_frames_last,
            "mode_changes": list(self.mode_changes),
            "error_lines": self.error_lines,
            "error_samples": list(self.error_samples),
            "save_rewrite_start": self.save_start,
            "save_closed": self.save_closed,
            "save_restored": self.save_restored,
        }

    # ------------------------------------------------------------------ feeding
    def feed_bytes(self, raw: bytes) -> Optional[str]:
        self.lines += 1
        if self.t0 is None and self._t0_tries < _T0_ATTEMPTS:
            self._t0_tries += 1
            self.t0 = _parse_ts(raw[:25].decode("ascii", "replace"))
        head = raw[:40]
        level_kind: Optional[str] = None
        if b"[error] " in head:
            self.error_lines += 1
            if len(self.error_samples) < 5:
                self.error_samples.append(raw.decode("utf-8", "replace").rstrip("\r\n")[:300])
            level_kind = "error"
        elif b"[critical] " in head:
            level_kind = "critical"
        if not _IMPORTANT.search(raw):
            self._last_raw = raw
            return level_kind
        self._last_raw = None
        return self._feed(raw.decode("utf-8", "replace").rstrip("\r\n")) or level_kind

    def _rel(self, ts: Optional[float]) -> Optional[float]:
        if ts is None:
            ts = self._last_ts
        if ts is None or self.t0 is None:
            return None
        return max(0.0, round(ts - self.t0, 3))

    def _feed(self, line: str) -> Optional[str]:
        ts = _parse_ts(line)
        if ts is not None:
            self._last_ts = ts
        rel = self._rel(ts)
        kind: Optional[str] = None

        if _RE_FATAL.search(line) and not _RE_STATUS.search(line):
            self.fatal_lines += 1
            if len(self.fatal_samples) < _SAMPLE_CAP:
                self.fatal_samples.append(line[:300])
            same_event = ts is not None and self._fatal_ts is not None and ts - self._fatal_ts <= _FATAL_EVENT_S
            if ts is not None:
                self._fatal_ts = ts
            if not same_event:
                self.fatal += 1
            kind = "fatal"
        if _RE_UNREG.search(line):
            self.unregistered += 1
            if len(self.unregistered_samples) < _SAMPLE_CAP:
                self.unregistered_samples.append(line[:300])
            kind = kind or "fatal"

        if "[mcla-" in line:
            if _RE_TITLE.search(line):
                if not self.title_seen:
                    self.title_seen = True
                    self.t_title = rel
                    kind = kind or "reach"
            elif _RE_MENU.search(line):
                if self.title_seen and not self.menu_seen:  # the game registers its states before the first frame
                    self.menu_seen = True
                    self.t_menu = rel
                    kind = kind or "reach"
            elif "[mcla-mode]" in line:
                m = _RE_MODE.search(line)
                if m:
                    if len(self.mode_changes) < _MODE_CAP:
                        self.mode_changes.append([rel, m.group("from"), m.group("to")])
                    if m.group("to") == "free-roam" and not self.free_roam_seen:
                        self.free_roam_seen = True
                        self.t_free_roam = rel
                    kind = kind or "mode"
            if self.title_seen and rel is not None and _RE_LIVE.search(line):
                self.hb_last = rel
                self.hb_lines += 1
                if "[mcla-heartbeat]" in line:
                    f = _RE_FRAMES.search(line)
                    if f:
                        self.hb_frames_last = int(f.group(1))
        elif "content: " in line:
            if _RE_SAVE_START.search(line):
                self.save_start += 1
                kind = kind or "save"
            elif _RE_SAVE_CLOSED.search(line):
                self.save_closed += 1
                kind = kind or "save"
            elif _RE_SAVE_RESTORED.search(line):
                self.save_restored += 1
                kind = kind or "save"
        if "[mcla-watchdog]" in line:
            if "without a single register moving" in line:
                self.hang_alarms += 1
            kind = kind or "watchdog"
        return kind


def log_files(path: LogSource) -> List[Path]:
    """The log and its rotated parts in time order: mcla.N.log (oldest) ... mcla.1.log, mcla.log. Only existing files."""
    path = Path(path)
    rotated: List[Tuple[int, Path]] = []
    pat = re.compile(r"^%s\.(\d+)%s$" % (re.escape(path.stem), re.escape(path.suffix)))
    if path.parent.is_dir():
        for p in path.parent.iterdir():
            m = pat.match(p.name)
            if m:
                rotated.append((int(m.group(1)), p))
    files = [p for _, p in sorted(rotated, key=lambda r: -r[0])]
    if path.is_file():
        files.append(path)
    return files


def parse_log(path: LogSource, t0: Optional[float] = None) -> Dict:
    """Parse a finished (or still growing) log, rotated parts included, into the run facts.

    t0: epoch seconds of the process start (times are relative to it); default the time of the first log line.
    Returns LogScanner.result() plus `log` and `log_files`; `reached` is none|title|menu|free-roam."""
    files = log_files(path)
    scanner = LogScanner(t0)
    for f in files:
        with open(f, "rb") as fh:
            for raw in fh:
                scanner.feed_bytes(raw)
    out = scanner.result()
    out["log"] = Path(path).as_posix()
    out["log_files"] = [f.as_posix() for f in files]
    return out


def evaluate(parsed: Dict, expected: str = "none", process_fatal: int = 0) -> Dict:
    """Adds fatal, heartbeat_last, expected, expect_met and ok to a parse_log() result (in place; returned)."""
    parsed["fatal_process"] = process_fatal
    parsed["fatal"] = parsed["fatal_log"] + process_fatal
    hb = parsed.get("heartbeat_last_s")
    parsed["heartbeat_last"] = 0 if hb is None else int(round(hb))
    parsed["expected"] = expected
    parsed["expect_met"] = RANK[parsed["reached"]] >= RANK[expected]
    parsed["ok"] = parsed["fatal"] == 0 and parsed["unregistered"] == 0 and parsed["expect_met"]
    return parsed


def run_line(run_id: str, s: Dict) -> str:
    return "run %s: reached=%s fatal=%d unregistered=%d heartbeat_last=%d" % (
        run_id, s["reached"], s["fatal"], s["unregistered"], s["heartbeat_last"])


# ================================================================================================================
# Flow files: the host's script grammar (mac_input_script.h), validated here, and the header comments
# ================================================================================================================

_BUTTONS = frozenset(("a", "b", "x", "y", "start", "back", "lb", "rb", "l3", "r3", "up", "down", "left", "right",
                      "lt", "rt", "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left",
                      "rs_right"))
_RE_HEADER = re.compile(r"^\s*#\s*(expect|seconds|fixture)\s*:\s*(\S+)\s*$")


# Schedule steps (the host's SchedStep, src/host/input_script.h): `<seconds>:set <cvar>=<value>`, `<seconds>:burn <n>`,
# `<seconds>:burnmem <n>`, `<seconds>:mark <label>`. The first word after the colon (case-insensitive) tells them from a
# pad step (no button is called set, burn, burnmem or mark).
SCHED_KINDS = ("set", "burn", "burnmem", "mark")
BURN_MAX = 32
_SCHED_RESERVED = ("burn", "burnmem", "mark")      # segment-label keys: not usable as a cvar name
_RE_SCHED_NAME = re.compile(r"[A-Za-z0-9_]+")
_RE_SCHED_COUNT = re.compile(r"[0-9]{1,2}")
_RE_SCHED_LABEL = re.compile(r"[A-Za-z0-9_.-]{1,40}")

# A schedule step: (at_s, kind, name, value_or_n, text). set: name = cvar, value = the value (verbatim string);
# burn / burnmem: name = "", value = n (int 0..32); mark: name = label, value = None. text = the step as written.
SchedStep = Tuple[float, str, str, Union[str, int, None], str]


def _parse_time(t_text: str, where: str) -> float:
    try:
        at = float(t_text)
    except ValueError:
        at = -1.0
    if not math.isfinite(at) or at < 0:
        raise Fail("%s: bad time '%s'" % (where, t_text))
    return at


def _parse_sched_body(kw: str, rest: str, where: str) -> Tuple[str, Union[str, int, None]]:
    """What follows the keyword of a schedule step (the host's ParseSchedBody): (name, value_or_n)."""
    if kw == "set":
        name, eq, value = rest.partition("=")
        if not eq:
            raise Fail("%s: set needs <cvar>=<value>" % where)
        if any(c.isspace() for c in rest):
            raise Fail("%s: set takes exactly one <cvar>=<value>, without spaces ('%s')" % (where, rest))
        if not _RE_SCHED_NAME.fullmatch(name):
            raise Fail("%s: bad cvar name '%s' (letters, digits, _)" % (where, name))
        if name in _SCHED_RESERVED:
            raise Fail("%s: '%s' cannot be set from a schedule (it is a segment label key)" % (where, name))
        if not value:
            raise Fail("%s: empty value for cvar '%s'" % (where, name))
        if ";" in value or "#" in value:
            raise Fail("%s: the value '%s' of cvar '%s' has a ';' or '#'" % (where, value, name))
        return name, value
    if kw in ("burn", "burnmem"):
        n = int(rest) if _RE_SCHED_COUNT.fullmatch(rest) else -1
        if not 0 <= n <= BURN_MAX:
            raise Fail("%s: %s needs a count 0..%d, got '%s'" % (where, kw, BURN_MAX, rest))
        return "", n
    if not _RE_SCHED_LABEL.fullmatch(rest):
        raise Fail("%s: bad mark label '%s' (1..40 of A-Z a-z 0-9 _ . -)" % (where, rest))
    return rest, None


def _parse_steps(text: str) -> Tuple[List[Tuple[float, int, str]], List[SchedStep]]:
    """Validate a script like the host's parser does: (pad steps, schedule steps), both in file order; raises Fail."""
    pad: List[Tuple[float, int, str]] = []
    sched: List[SchedStep] = []
    for lineno, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        body = line.split("#", 1)[0].strip()
        for raw in body.split(";"):
            s = raw.strip()
            if not s:
                continue
            where = "line %d, step '%s'" % (lineno, s)
            if ":" not in s:
                raise Fail("%s: no <seconds>: prefix" % where)
            t_text, buttons = (p.strip() for p in s.split(":", 1))
            at = _parse_time(t_text, where)
            words = buttons.split(None, 1)
            kw = words[0].lower() if words else ""
            if kw in SCHED_KINDS:
                name, value = _parse_sched_body(kw, words[1].strip() if len(words) > 1 else "", where)
                sched.append((at, kw, name, value, s))
                continue
            hold = 200
            if "/" in buttons:
                buttons, h_text = (p.strip() for p in buttons.split("/", 1))
                if not h_text.isdigit() or not 20 <= int(h_text) <= 600000:
                    raise Fail("%s: bad hold '%s' (20..600000 ms)" % (where, h_text))
                hold = int(h_text)
            for tok in buttons.split("+"):
                tok = tok.strip().lower()
                name, eq, value = tok.partition("=")
                if eq:
                    try:
                        ok = 0.0 <= float(value) <= 1.0
                    except ValueError:
                        ok = False
                    if not ok or name not in ("rt", "lt"):
                        raise Fail("%s: bad analog '%s' (only rt/lt take =<0..1>)" % (where, tok))
                elif name not in _BUTTONS:
                    raise Fail("%s: unknown button '%s'" % (where, tok))
            pad.append((at, hold, s))
    return pad, sched


def parse_flow(text: str) -> List[Tuple[float, int, str]]:
    """Validate a flow file like the host's parser does. Returns the PAD steps [(at_s, hold_ms, step text)]; raises Fail.
    Schedule steps (set / burn / burnmem / mark) are validated too but not returned: see parse_schedule."""
    return _parse_steps(text)[0]


def parse_schedule(text: str) -> List[SchedStep]:
    """The schedule steps of a flow / script text, [(at_s, kind, name, value_or_n, text)], stable-sorted by time (file
    order for equal times, as the host does); pad steps are validated but not returned. Raises Fail."""
    return sorted(_parse_steps(text)[1], key=lambda st: st[0])


def parse_script(text: str) -> Tuple[List[Tuple[float, int, str]], List[SchedStep]]:
    """(pad steps, schedule steps sorted by time) of a flow text, validated once."""
    pad, sched = _parse_steps(text)
    return pad, sorted(sched, key=lambda st: st[0])


def _is_inline_schedule(spec: str) -> bool:
    """Inline steps hold a `<seconds>:` prefix; a spec without one, or one that starts like a drive path (`C:/x`), is a file."""
    return ":" in spec and re.match(r"\s*[A-Za-z]:[\\/]", spec) is None


def load_schedule(specs: Sequence[str]) -> Tuple[List[SchedStep], List[str]]:
    """The steps of `--schedule` specs. A spec is a file (a path, or a name in tools/flows/schedules/; same grammar, '#'
    comments) or an inline string of ';'-separated steps (`58:set a=0;70:burn 4`; a '#' starts a comment as in a file).
    Only schedule steps are allowed (pad steps belong in the flow). Returns (steps sorted by time, lines to append to the
    run's flow.txt: a `# --schedule` comment per spec, then its steps verbatim). Raises Fail."""
    steps: List[SchedStep] = []
    lines: List[str] = []
    for spec in specs:
        label = spec
        p = Path(spec)
        try:
            is_file = bool(spec.strip()) and p.is_file()
        except OSError:                      # e.g. a name with characters a path cannot hold
            is_file = False
        if is_file:
            text, label = p.read_text(encoding="utf-8"), p.as_posix()
        elif _is_inline_schedule(spec):
            text = spec
        else:
            cand = SCHEDULES_DIR / (spec if spec.endswith(".txt") else spec + ".txt")
            if spec.strip() and re.fullmatch(r"[A-Za-z0-9_.-]+", spec) and cand.is_file():
                text, label = cand.read_text(encoding="utf-8"), cand.as_posix()
            else:
                have = sorted(f.stem for f in SCHEDULES_DIR.glob("*.txt")) if SCHEDULES_DIR.is_dir() else []
                raise Fail("--schedule '%s' is neither a file nor inline steps (`<seconds>:set k=v;<seconds>:burn 4`); "
                           "schedules in %s: %s" % (spec, SCHEDULES_DIR.as_posix(), ", ".join(have) or "none"))
        pad, sched = _parse_steps(text)
        if pad:
            raise Fail("--schedule '%s': pad step '%s' (a schedule takes only set / burn / burnmem / mark steps; "
                       "pad steps belong in the flow)" % (label, pad[0][2]))
        if not sched:
            raise Fail("--schedule '%s' has no steps" % label)
        lines.append("# --schedule %s" % (label if len(label) <= 160 else label[:157] + "..."))
        lines.extend(st[4] for st in sched)
        steps.extend(sched)
    steps.sort(key=lambda st: st[0])
    return steps, lines

def flow_header(text: str) -> Dict[str, str]:
    """`# expect: menu`, `# seconds: 180`, `# fixture: after_intro` (first occurrence of each)."""
    out: Dict[str, str] = {}
    for line in text.replace("\r\n", "\n").split("\n"):
        m = _RE_HEADER.match(line)
        if m and m.group(1) not in out:
            out[m.group(1)] = m.group(2)
    if "expect" in out and out["expect"] not in STATES:
        raise Fail("flow header `expect: %s` is not one of %s" % (out["expect"], "|".join(STATES)))
    if "seconds" in out:
        try:
            if float(out["seconds"]) <= 0:
                raise ValueError
        except ValueError:
            raise Fail("flow header `seconds: %s` is not a positive number" % out["seconds"]) from None
    return out


def resolve_flow(name: str) -> Path:
    p = Path(name)
    if p.suffix and p.is_file():
        return p.resolve()
    cand = FLOWS_DIR / (name if name.endswith(".txt") else name + ".txt")
    if cand.is_file():
        return cand
    have = sorted(f.stem for f in FLOWS_DIR.glob("*.txt")) if FLOWS_DIR.is_dir() else []
    raise Fail("no flow '%s' (looked for %s; flows: %s)" % (name, cand, ", ".join(have) or "none"))


# ================================================================================================================
# Windows: job object (tree kill) and the window screenshot
# ================================================================================================================

_win = None


def _winapi():
    """ctypes bindings, built on first use (nothing happens on import)."""
    global _win
    if _win is not None:
        return _win
    import ctypes
    from ctypes import wintypes

    class NS:
        pass

    ns = NS()
    ns.ctypes, ns.wintypes = ctypes, wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    g32 = ctypes.WinDLL("gdi32", use_last_error=True)
    ns.k32, ns.u32, ns.g32 = k32, u32, g32
    HANDLE = wintypes.HANDLE

    class POINT(ctypes.Structure):
        _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]

    class RECT(ctypes.Structure):
        _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG), ("right", wintypes.LONG), ("bottom", wintypes.LONG)]

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                    ("biClrImportant", wintypes.DWORD)]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class JOB_BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class JOB_EXTENDED(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", JOB_BASIC), ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    class JOB_PIDS(ctypes.Structure):
        _fields_ = [("NumberOfAssignedProcesses", wintypes.DWORD), ("NumberOfProcessIdsInList", wintypes.DWORD),
                    ("ProcessIdList", ctypes.c_size_t * 64)]

    ns.POINT, ns.RECT, ns.BITMAPINFOHEADER = POINT, RECT, BITMAPINFOHEADER
    ns.JOB_EXTENDED, ns.JOB_PIDS = JOB_EXTENDED, JOB_PIDS
    ns.WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def sig(fn, restype, *argtypes):
        fn.restype = restype
        fn.argtypes = list(argtypes)

    sig(k32.CreateJobObjectW, HANDLE, ctypes.c_void_p, wintypes.LPCWSTR)
    sig(k32.SetInformationJobObject, wintypes.BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    sig(k32.QueryInformationJobObject, wintypes.BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD))
    sig(k32.AssignProcessToJobObject, wintypes.BOOL, HANDLE, HANDLE)
    sig(k32.TerminateJobObject, wintypes.BOOL, HANDLE, wintypes.UINT)
    sig(k32.OpenProcess, HANDLE, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    sig(k32.CloseHandle, wintypes.BOOL, HANDLE)
    sig(k32.GetExitCodeProcess, wintypes.BOOL, HANDLE, ctypes.POINTER(wintypes.DWORD))
    sig(k32.GetProcessTimes, wintypes.BOOL, HANDLE, ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME))
    sig(k32.SetThreadExecutionState, wintypes.DWORD, wintypes.DWORD)
    sig(u32.EnumWindows, wintypes.BOOL, ns.WNDENUMPROC, wintypes.LPARAM)
    sig(u32.GetWindowThreadProcessId, wintypes.DWORD, wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    sig(u32.IsWindowVisible, wintypes.BOOL, wintypes.HWND)
    sig(u32.IsIconic, wintypes.BOOL, wintypes.HWND)
    sig(u32.GetClientRect, wintypes.BOOL, wintypes.HWND, ctypes.POINTER(RECT))
    sig(u32.ClientToScreen, wintypes.BOOL, wintypes.HWND, ctypes.POINTER(POINT))
    sig(u32.GetWindowTextW, ctypes.c_int, wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    sig(u32.WindowFromPoint, wintypes.HWND, POINT)
    sig(u32.GetAncestor, wintypes.HWND, wintypes.HWND, wintypes.UINT)
    sig(u32.GetDC, wintypes.HDC, wintypes.HWND)
    sig(u32.ReleaseDC, ctypes.c_int, wintypes.HWND, wintypes.HDC)
    sig(u32.SetWindowPos, wintypes.BOOL, wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, wintypes.UINT)
    sig(u32.PrintWindow, wintypes.BOOL, wintypes.HWND, wintypes.HDC, wintypes.UINT)
    sig(g32.CreateCompatibleDC, wintypes.HDC, wintypes.HDC)
    sig(g32.CreateDIBSection, HANDLE, wintypes.HDC, ctypes.POINTER(BITMAPINFOHEADER), wintypes.UINT,
        ctypes.POINTER(ctypes.c_void_p), HANDLE, wintypes.DWORD)
    sig(g32.SelectObject, HANDLE, wintypes.HDC, HANDLE)
    sig(g32.DeleteObject, wintypes.BOOL, HANDLE)
    sig(g32.DeleteDC, wintypes.BOOL, wintypes.HDC)
    sig(g32.BitBlt, wintypes.BOOL, wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD)
    _win = ns
    return ns


class Job:
    """A Windows job object that kills every process in it when it is terminated or when this process ends."""

    def __init__(self) -> None:
        self.handle = None
        if os.name != "nt":
            return
        w = _winapi()
        job = w.k32.CreateJobObjectW(None, None)
        if not job:
            return
        ext = w.JOB_EXTENDED()
        ext.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not w.k32.SetInformationJobObject(job, 9, w.ctypes.byref(ext), w.ctypes.sizeof(ext)):  # 9 = extended limits
            w.k32.CloseHandle(job)
            return
        self.handle = job

    def add(self, proc: "subprocess.Popen") -> bool:
        if not self.handle:
            return False
        w = _winapi()
        h = w.k32.OpenProcess(0x0101, False, proc.pid)  # PROCESS_SET_QUOTA | PROCESS_TERMINATE
        if not h:
            return False
        try:
            return bool(w.k32.AssignProcessToJobObject(self.handle, h))
        finally:
            w.k32.CloseHandle(h)

    def pids(self) -> List[int]:
        if not self.handle:
            return []
        w = _winapi()
        info = w.JOB_PIDS()
        if not w.k32.QueryInformationJobObject(self.handle, 3, w.ctypes.byref(info), w.ctypes.sizeof(info), None):
            return []
        return [int(info.ProcessIdList[i]) for i in range(min(info.NumberOfProcessIdsInList, 64))]

    def terminate(self) -> None:
        if self.handle:
            _winapi().k32.TerminateJobObject(self.handle, 1)

    def close(self) -> None:
        if self.handle:
            _winapi().k32.CloseHandle(self.handle)
            self.handle = None


def kill_tree(proc: "subprocess.Popen", job: Optional[Job]) -> None:
    if job is not None and job.handle:
        job.terminate()
    elif os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        proc.kill()
    try:
        proc.wait(timeout=KILL_WAIT_S)
    except subprocess.TimeoutExpired:
        proc.kill()


def keep_awake(on: bool) -> None:
    """No screen saver, display-off or sleep while a run is in progress (a locked or dimmed desktop breaks the
    screenshots and a sleeping machine stops the game): ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED for the
    calling thread, and ES_CONTINUOUS alone (back to normal) when `on` is false."""
    if os.name == "nt":
        _winapi().k32.SetThreadExecutionState(0x80000000 | 0x00000002 | 0x00000001 if on else 0x80000000)


# ================================================================================================================
# Load spinners (`burn` / `burnmem` schedule steps): child processes that eat CPU or memory bandwidth during a run
# ================================================================================================================

# The code of a spinner; argv[1] is the pid of the run_game process. The spinner exits by itself when that process is gone
# (OpenProcess(SYNCHRONIZE) + WaitForSingleObject(h, 0): signalled = ended), so it cannot outlive its run whatever happens.
_SPIN_PRELUDE = (
    "import sys\n"
    "ppid = int(sys.argv[1])\n"
    "h = None\n"
    "try:\n"
    "    import ctypes\n"
    "    k = ctypes.windll.kernel32\n"
    "    k.OpenProcess.restype = ctypes.c_void_p\n"
    "    k.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]\n"
    "    k.WaitForSingleObject.restype = ctypes.c_uint32\n"
    "    k.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]\n"
    "    h = k.OpenProcess(0x00100000, 0, ppid)\n"                  # SYNCHRONIZE
    "except Exception:\n"
    "    pass\n"
    "if not h:\n"
    "    sys.exit(0)\n"                                             # no parent to watch (already gone): do not run
    "def gone():\n"
    "    return k.WaitForSingleObject(h, 0) != 0x102\n"             # anything but WAIT_TIMEOUT
)
SPINNER_CODE = {
    # ALU: a tight integer loop; the parent check runs every 2**20 iterations (~0.1 s).
    "burn": _SPIN_PRELUDE
            + "def main():\n"
              "    x = 0\n"
              "    while True:\n"
              "        for _ in range(1 << 20):\n"
              "            x = (x * 1103515245 + 12345) & 0xFFFFFFFF\n"
              "        if gone():\n"
              "            return\n"
              "main()\n",
    # Memory bandwidth: two 64 MiB buffers (well beyond any cache; both written first so every page is really mapped),
    # copied into each other for ever (a slice assignment between two bytearrays of one size is a memcpy).
    "burnmem": _SPIN_PRELUDE
               + "a = bytearray(b'\\xa5') * (64 << 20)\n"
                 "b = bytearray(a)\n"
                 "while True:\n"
                 "    b[:] = a\n"
                 "    a[:] = b\n"
                 "    if gone():\n"
                 "        break\n",
}
_SPIN_LABEL = {"burn": "cpu", "burnmem": "mem"}
_CREATE_NO_WINDOW = 0x08000000


def spawn_spinner(kind: str, parent_pid: int) -> "subprocess.Popen":
    """One spinner process (`python -I -S -c CODE <parent_pid>`): no window, no stdio, normal priority."""
    return subprocess.Popen([sys.executable, "-I", "-S", "-c", SPINNER_CODE[kind], str(parent_pid)],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=_CREATE_NO_WINDOW)


class Spinners:
    """The spinner processes of one run: `cpu` (kind `burn`) and `mem` (kind `burnmem`) counts, set by schedule steps.

    Every spinner is added to `job` (the game's job object: they die with the run whatever happens) and killed explicitly by
    kill_all(), which the run calls on every exit path. None exists before the game is started: the object is made empty and
    only apply()/set() start processes. `started` (time.monotonic() at the game start) turns apply()'s time into seconds."""

    def __init__(self, job: Optional[Job] = None, started: Optional[float] = None, parent_pid: Optional[int] = None,
                 quiet: bool = False) -> None:
        self.job = job
        self.started = time.monotonic() if started is None else started
        self.parent_pid = os.getpid() if parent_pid is None else parent_pid
        self.quiet = quiet
        self.procs: Dict[str, List["subprocess.Popen"]] = {"burn": [], "burnmem": []}
        self.events: List[List] = []             # [[t, kind, n], ...] one per executed step
        self.start_failures = 0
        self.died_early = 0
        self.killed_at_end = 0                   # spinners still running when kill_all() ended the run
        self._warned_job = False

    # ------------------------------------------------------------------ state
    def _reap(self) -> None:
        """Drop spinners that ended on their own (a crash; they never end otherwise while the run is alive)."""
        for kind, lst in self.procs.items():
            alive = [p for p in lst if p.poll() is None]
            self.died_early += len(lst) - len(alive)
            self.procs[kind] = alive

    def counts(self) -> Dict[str, int]:
        self._reap()
        return {kind: len(lst) for kind, lst in self.procs.items()}

    def pids(self) -> List[int]:
        return [p.pid for lst in self.procs.values() for p in lst]

    # ------------------------------------------------------------------ changes
    def set(self, kind: str, n: int) -> None:
        """Make exactly n live spinners of this kind: start more, or kill the extra ones (the newest first)."""
        self._reap()
        lst = self.procs[kind]
        while len(lst) < n:
            try:
                p = spawn_spinner(kind, self.parent_pid)
            except OSError as e:
                self.start_failures += 1
                say("warning: cannot start a %s spinner: %s" % (kind, e))
                break
            lst.append(p)
            if self.job is not None and not self.job.add(p) and not self._warned_job:
                self._warned_job = True
                say("warning: a spinner could not be put in the job object (it still ends with this process and the "
                    "run's kill)")
        extra = lst[n:]
        del lst[n:]
        _kill_procs(extra)

    def apply(self, kind: str, n: int, t: Optional[float] = None) -> None:
        """Run one `burn` / `burnmem` step: set the count, record the event, print `burn: t=<s> cpu=<n> mem=<n>`."""
        if t is None:
            t = time.monotonic() - self.started
        before = self.counts()
        self.set(kind, n)
        self.events.append([round(t, 2), kind, n])
        after = self.counts()
        if after != before and not self.quiet:
            say("burn: t=%.1f cpu=%d mem=%d" % (t, after["burn"], after["burnmem"]))

    def kill_all(self) -> int:
        """Kill every spinner and wait for them; returns how many are still alive afterwards (0 in the normal case)."""
        self._reap()
        victims = [p for lst in self.procs.values() for p in lst]
        self.killed_at_end += len(victims)
        for lst in self.procs.values():
            lst.clear()
        _kill_procs(victims)
        return sum(1 for p in victims if p.poll() is None)


def _kill_procs(procs: Sequence["subprocess.Popen"]) -> None:
    for p in procs:
        try:
            p.kill()
        except OSError:
            pass
    for p in procs:
        try:
            p.wait(timeout=5)
        except (subprocess.TimeoutExpired, OSError):
            pass


def _process_alive(pid: int) -> bool:
    """True while a process with this pid exists and has not exited (ctypes; used by the spinner self-test)."""
    w = _winapi()
    h = w.k32.OpenProcess(0x1000, False, pid)         # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        code = w.wintypes.DWORD()
        if not w.k32.GetExitCodeProcess(h, w.ctypes.byref(code)):
            return False
        return code.value == 259                       # STILL_ACTIVE
    finally:
        w.k32.CloseHandle(h)


def _process_cpu_s(pid: int) -> Optional[float]:
    """Kernel + user CPU seconds of a process (GetProcessTimes), None when it cannot be opened."""
    w = _winapi()
    h = w.k32.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    try:
        ft = [w.wintypes.FILETIME() for _ in range(4)]
        if not w.k32.GetProcessTimes(h, *(w.ctypes.byref(f) for f in ft)):
            return None
        return sum(((f.dwHighDateTime << 32) | f.dwLowDateTime) for f in ft[2:4]) / 1e7
    finally:
        w.k32.CloseHandle(h)


def spinner_selftest(cpu: int = 2, mem: int = 1, seconds: float = 2.0) -> int:
    """No game: start `cpu` ALU + `mem` memory spinners in a private job object for `seconds` s, check they live, burn CPU and
    are in the job, kill them, check they are gone; then check that a spinner whose parent process dies ends by itself.
    Only processes started here are touched. Returns 0 when every check passes."""
    if os.name != "nt":
        print("spinner self-test: Windows only")
        return EXIT_SETUP
    ok = True

    def check(cond: bool, what: str) -> None:
        nonlocal ok
        ok = ok and cond
        print("  %-4s %s" % ("ok" if cond else "FAIL", what), flush=True)

    job = Job()
    check(bool(job.handle), "job object created")
    sp = Spinners(job)
    try:
        sp.apply("burn", cpu)
        sp.apply("burnmem", mem)
        pids = sp.pids()
        check(len(pids) == cpu + mem, "%d spinner processes started (%s)" % (len(pids), pids))
        cpu0 = {p: _process_cpu_s(p) for p in pids}
        time.sleep(seconds)
        check(all(_process_alive(p) for p in pids), "all alive after %.1f s" % seconds)
        in_job = set(job.pids())
        check(all(p in in_job for p in pids), "all in the job object (job pids %s)" % sorted(in_job))
        used = {p: (_process_cpu_s(p) or 0.0) - (cpu0[p] or 0.0) for p in pids}
        # A busy machine (other builds running) gives a spinner less than a whole core: the bar is only "clearly not idle".
        check(all(used[p.pid] > 0.25 * seconds for p in sp.procs["burn"]),
              "ALU spinners used %s CPU s in %.1f s" % ([round(used[p.pid], 2) for p in sp.procs["burn"]], seconds))
        check(all(used[p.pid] > 0.25 * seconds for p in sp.procs["burnmem"]),
              "memory spinners used %s CPU s in %.1f s" % ([round(used[p.pid], 2) for p in sp.procs["burnmem"]], seconds))
        sp.apply("burn", 1)
        check(sp.counts() == {"burn": 1, "burnmem": mem}, "burn 1 leaves %s" % sp.counts())
        sp.apply("burn", 0)
        check(sp.counts() == {"burn": 0, "burnmem": mem}, "burn 0 stops the ALU spinners: %s" % sp.counts())
    finally:
        left = sp.kill_all()
    time.sleep(0.2)
    check(left == 0 and not any(_process_alive(p) for p in pids), "all killed; none alive")
    check(sp.counts() == {"burn": 0, "burnmem": 0}, "counts back to zero")
    job.terminate()
    job.close()

    # A spinner without its parent: a helper process starts one for its own pid and exits; the spinner must end by itself.
    helper = ("import os, sys, time\nsys.path.insert(0, %r)\nimport run_game\n"
              "p = run_game.spawn_spinner('burn', os.getpid())\nprint(p.pid, flush=True)\ntime.sleep(1.0)\n" % str(HERE))
    out = subprocess.run([sys.executable, "-I", "-c", helper], capture_output=True, text=True, timeout=30,
                         creationflags=_CREATE_NO_WINDOW)
    try:
        orphan = int(out.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        orphan = 0
        check(False, "orphan test: the helper did not print a pid (%s)" % out.stderr.strip()[-200:])
    if orphan:
        t_end = time.monotonic() + 8.0
        while _process_alive(orphan) and time.monotonic() < t_end:
            time.sleep(0.1)
        gone = not _process_alive(orphan)
        if not gone:                                         # started here: clean it up
            subprocess.run(["taskkill", "/PID", str(orphan), "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        check(gone, "an orphaned spinner (parent gone) ended by itself (pid %d)" % orphan)
    print("spinner self-test: %s" % ("PASS" if ok else "FAIL"), flush=True)
    return 0 if ok else EXIT_FAILED


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    """A truecolour PNG from RGB bytes (zlib only; written to a temp name and renamed)."""
    stride = width * 3
    raw = bytearray((stride + 1) * height)           # each row: filter byte 0, then the pixels
    for y in range(height):
        o = y * (stride + 1)
        raw[o + 1:o + 1 + stride] = rgb[y * stride:(y + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return len(data).to_bytes(4, "big") + body + (zlib.crc32(body) & 0xFFFFFFFF).to_bytes(4, "big")

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0]))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 3))
           + chunk(b"IEND", b""))
    tmp = path.with_name(path.name + ".part")
    tmp.write_bytes(png)
    os.replace(tmp, path)


class Shooter:
    """Screenshots of the game window's client area: found by process (any process of the job), captured with
    PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) and, when that fails, copied from the screen with BitBlt (the same as
    .NET's CopyFromScreen; then a covered window is noted once and the shot shows whatever is on top), written as PNG."""

    def __init__(self, pids: Callable[[], List[int]], shots_dir: Path, topmost: bool = False) -> None:
        self.pids = pids
        self.dir = shots_dir
        self.topmost = topmost
        self.taken: List[str] = []
        self.notes: List[str] = []              # one-off problems
        self.misses: Dict[str, int] = {}        # problems that come and go (no window yet): message -> count
        self.attempts = 0
        self._raised = set()
        self.use_printwindow = True             # False: the screen BitBlt only (tests)
        self.methods: Dict[str, int] = {}       # how the shots were taken: printwindow | bitblt -> count
        shots_dir.mkdir(parents=True, exist_ok=True)
        w = _winapi()
        # Physical pixels for GetClientRect/ClientToScreen/BitBlt on a scaled (200 %) panel.
        try:
            w.u32.SetProcessDpiAwarenessContext.argtypes = [w.ctypes.c_void_p]
            if not w.u32.SetProcessDpiAwarenessContext(w.ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
                w.u32.SetProcessDPIAware()
        except (AttributeError, OSError):
            try:
                w.u32.SetProcessDPIAware()
            except (AttributeError, OSError):
                pass

    def _note(self, msg: str, transient: bool = False) -> None:
        if transient:
            self.misses[msg] = self.misses.get(msg, 0) + 1
        elif msg not in self.notes:
            self.notes.append(msg)

    def final_notes(self) -> List[str]:
        return self.notes + ["%s (%d of %d attempts)" % (m, n, self.attempts) for m, n in self.misses.items()]

    def find_window(self):
        w = _winapi()
        wanted = set(self.pids())
        best = [0, None]

        def visit(hwnd, _lparam):
            if not w.u32.IsWindowVisible(hwnd):
                return True
            owner = w.wintypes.DWORD()
            w.u32.GetWindowThreadProcessId(hwnd, w.ctypes.byref(owner))
            if owner.value not in wanted:
                return True
            rc = w.RECT()
            if w.u32.GetClientRect(hwnd, w.ctypes.byref(rc)):
                area = rc.right * rc.bottom
                if area > best[0]:
                    best[0], best[1] = area, hwnd
            return True

        w.u32.EnumWindows(w.WNDENUMPROC(visit), 0)
        return best[1]

    def title_of(self, hwnd) -> str:
        w = _winapi()
        buf = w.ctypes.create_unicode_buffer(256)
        w.u32.GetWindowTextW(hwnd, buf, 256)
        return buf.value

    def capture(self, hwnd) -> Optional[Tuple[int, int, bytes]]:
        """The window's client area as (width, height, RGB bytes). PrintWindow reads the window's own composed surface, so
        it works while the window is covered and while the screen saver or the lock screen owns the desktop (where a
        BitBlt from the screen fails); the screen BitBlt is the fallback."""
        w = _winapi()
        ct = w.ctypes
        rc = w.RECT()
        if not w.u32.GetClientRect(hwnd, ct.byref(rc)) or rc.right < 2 or rc.bottom < 2:
            return None
        width, height = rc.right, rc.bottom
        screen = w.u32.GetDC(None)
        mem = w.g32.CreateCompatibleDC(screen)
        bmi = w.BITMAPINFOHEADER()
        bmi.biSize, bmi.biWidth, bmi.biHeight = ct.sizeof(w.BITMAPINFOHEADER), width, -height  # top-down
        bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
        bits = ct.c_void_p()
        bitmap = w.g32.CreateDIBSection(screen, ct.byref(bmi), 0, ct.byref(bits), None, 0)
        method = None
        try:
            if not bitmap or not bits.value:
                return None
            old = w.g32.SelectObject(mem, bitmap)
            try:
                if self.use_printwindow:
                    if w.u32.PrintWindow(hwnd, mem, 0x3):  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
                        method = "printwindow"
                    else:
                        self._note("PrintWindow failed: fell back to the screen BitBlt", True)
                if method is None and self._blit(hwnd, mem, screen, width, height):
                    method = "bitblt"
            finally:
                w.g32.SelectObject(mem, old)
            if method is None:
                return None
            bgra = ct.string_at(bits.value, width * height * 4)
        finally:
            if bitmap:
                w.g32.DeleteObject(bitmap)
            w.g32.DeleteDC(mem)
            w.u32.ReleaseDC(None, screen)
        self.methods[method] = self.methods.get(method, 0) + 1
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
        return width, height, bytes(rgb)

    def _blit(self, hwnd, mem, screen, width: int, height: int) -> bool:
        """The fallback: copy the client area from the screen into `mem`. The window has to be on top."""
        w = _winapi()
        ct = w.ctypes
        origin = w.POINT(0, 0)
        w.u32.ClientToScreen(hwnd, ct.byref(origin))
        if self.topmost and hwnd not in self._raised:
            # HWND_TOPMOST = -1; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
            w.u32.SetWindowPos(hwnd, ct.c_void_p(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
            self._raised.add(hwnd)
            time.sleep(0.15)
        root = w.u32.GetAncestor(hwnd, 2)  # GA_ROOT
        for gx in (1, 3, 5):
            for gy in (1, 3, 5):
                top = w.u32.WindowFromPoint(w.POINT(origin.x + width * gx // 6, origin.y + height * gy // 6))
                top_root = w.u32.GetAncestor(top, 2) if top else None
                if top_root != root:
                    self._note("window covered or off screen (by '%s'): shots show what is on top of the game "
                               "(use --topmost)" % (self.title_of(top_root) if top_root else "nothing"))
        # SRCCOPY | CAPTUREBLT (layered windows too)
        return bool(w.g32.BitBlt(mem, 0, 0, width, height, screen, origin.x, origin.y, 0x00CC0020 | 0x40000000))

    def shoot(self, name: str) -> Optional[Path]:
        self.attempts += 1
        try:
            hwnd = self.find_window()
            if not hwnd:
                self._note("no visible game window with a client area (not created yet, or minimized)", True)
                return None
            if _winapi().u32.IsIconic(hwnd):
                self._note("game window is minimized", True)
                return None
            got = self.capture(hwnd)
            if got is None:
                self._note("could not copy the game window from the screen", True)
                return None
            width, height, rgb = got
            path = self.dir / name
            write_png(path, width, height, rgb)
            self.taken.append(name)
            return path
        except OSError as e:
            self._note("screenshot failed: %s" % e)
            return None


# ================================================================================================================
# Live log tail
# ================================================================================================================

class LogTail:
    """Reads what the game appended to its log since the last pump. The file is opened for each read and closed again,
    so the game's own log rotation (a rename of mcla.log) is never blocked by this reader."""

    def __init__(self, path: Path, scanner: LogScanner) -> None:
        self.path = path
        self.scanner = scanner
        self.offset = 0
        self.partial = b""
        self.rotated = False

    def pump(self) -> List[Tuple[str, str]]:
        """New (kind, line) pairs worth printing."""
        try:
            size = self.path.stat().st_size
        except OSError:
            return []
        if size < self.offset:                       # rotated: the new file starts again
            self.offset, self.partial, self.rotated = 0, b"", True
        if size == self.offset:
            return []
        try:
            with open(self.path, "rb") as fh:
                fh.seek(self.offset)
                data = fh.read(size - self.offset)
        except OSError:
            return []
        self.offset += len(data)
        data = self.partial + data
        lines = data.split(b"\n")
        self.partial = lines.pop()                   # an unfinished last line waits for the next pump
        out: List[Tuple[str, str]] = []
        for raw in lines:
            kind = self.scanner.feed_bytes(raw)
            if kind:
                out.append((kind, raw.decode("utf-8", "replace").rstrip("\r")))
        return out


# ================================================================================================================
# A run
# ================================================================================================================

@dataclass
class RunOptions:
    flow: str
    fixture: Optional[str] = None
    seconds: Optional[float] = None
    api: str = "d3d12"
    cvars: List[str] = field(default_factory=list)
    screenshot_every: Optional[float] = None
    run_id: Optional[str] = None
    config: str = DEFAULT_CONFIG
    exe: Optional[str] = None
    lock: bool = True
    lock_timeout: Optional[float] = None
    stop_on_reach: bool = False
    expect: Optional[str] = None
    topmost: bool = False
    audio: bool = False
    activate: bool = False
    work: Optional[str] = None
    game: Optional[str] = None
    schedule: List[str] = field(default_factory=list)   # --schedule specs: files or inline `;`-separated steps
    window_pos: Optional[str] = None     # None = automatic (bottom-right unless topmost / activate); "" = SDK default
    behind: Optional[bool] = None        # None = automatic (on unless topmost / activate)


WINDOW_POS_NAMES = ("", "bottom-right", "bottom-left", "top-right", "top-left")
_RE_WINDOW_XY = re.compile(r"-?[0-9]+,-?[0-9]+")


def effective_window(opts: RunOptions) -> Tuple[str, bool]:
    """(window_pos, behind) of a run: the explicit options, else the quiet defaults (bottom-right, behind the foreground
    window) unless --topmost or --activate asks for a window that is meant to be seen."""
    visible = opts.topmost or opts.activate
    pos = opts.window_pos if opts.window_pos is not None else ("" if visible else "bottom-right")
    behind = opts.behind if opts.behind is not None else not visible
    return pos, behind


def check_window_pos(pos: str) -> str:
    pos = pos.strip()
    if pos not in WINDOW_POS_NAMES and not _RE_WINDOW_XY.fullmatch(pos):
        raise Fail("--window-pos '%s' is not one of %s or X,Y" % (pos, ", ".join(n or '""' for n in WINDOW_POS_NAMES)))
    return pos


def _work(opts: RunOptions) -> Path:
    return Path(opts.work or os.environ.get("MCLA_WORK") or DEFAULT_WORK)


def _game(opts: RunOptions) -> Path:
    return Path(opts.game or os.environ.get("MCLA_GAME") or DEFAULT_GAME)


def posix(p: Union[str, os.PathLike]) -> str:
    return Path(p).as_posix()


def build_args(opts: RunOptions, game_root: Path, user_dir: Path, cache_dir: Path, log_path: Path,
               flow_path: Path) -> List[str]:
    """The argument list of the shared contract, then the --cvar overrides (a repeated key replaces its value)."""
    pairs: Dict[str, str] = {
        "game_data_root": posix(game_root),
        "user_data_root": posix(user_dir),
        "cache_root": posix(cache_dir),
        "log_file": posix(log_path),
        "log_level": "info",
        "fullscreen": "false",
        "mcla_gpu_backend": opts.api,
        "input-file": posix(flow_path),
    }
    if not opts.audio:
        pairs["audio_mute"] = "true"  # the owner works on this machine while tests run
    pos, behind = effective_window(opts)
    if pos:
        pairs["mcla_window_pos"] = check_window_pos(pos)    # "" = the SDK default (centred): no argument
    if behind:
        pairs["mcla_window_behind"] = "true"
    for spec in opts.cvars:
        key, eq, value = spec.partition("=")
        key = key.strip().lstrip("-")
        if not eq or not re.fullmatch(r"[A-Za-z0-9_.-]+", key):
            raise Fail("--cvar '%s' is not key=value" % spec)
        pairs[key] = value
    return ["--%s=%s" % (k, v) for k, v in pairs.items()]


def check_fixture(src: Path) -> Dict[str, int]:
    """Validate a fixture folder (a copy of a run's user/ folder); returns its file count and size."""
    if not src.is_dir():
        raise Fail("no fixture '%s' (%s does not exist; a fixture is a copy of a run's user/ folder)"
                   % (src.name, posix(src)))
    files = [p for p in src.rglob("*") if p.is_file()]
    if not files:
        raise Fail("fixture %s is empty" % posix(src))
    if not any(p.is_dir() for p in src.glob("*/" + TITLE_ID)):
        raise Fail("fixture %s has no <xuid>/%s folder; it must be a copy of a run's user/ folder "
                   "(cp -r $WORK/runs/<id>/user/. %s)" % (posix(src), TITLE_ID, posix(src)))
    return {"files": len(files), "bytes": sum(p.stat().st_size for p in files)}


def user_saves(user_dir: Path) -> List[Dict]:
    out = []
    for p in sorted(user_dir.glob("*/%s/00000001/**/*" % TITLE_ID)):
        if p.is_file():
            out.append({"path": p.relative_to(user_dir).as_posix(), "bytes": p.stat().st_size})
    return out


def write_json(path: Path, data: Dict) -> None:
    tmp = path.with_name(path.name + ".part")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def say(msg: str) -> None:
    print("[run_game] " + msg, flush=True)


def iso(ts: float) -> str:
    return datetime.datetime.fromtimestamp(ts).isoformat(timespec="seconds")


def shot_name(t: float) -> str:
    return "t%d.png" % t if float(t).is_integer() else "t%.1f.png" % t


_LIVE_CAPS = {"error": 25, "watchdog": 60}  # kinds that can flood the console: print the first N lines only


def show_live(kind: str, line: str, since_start: float, shown: Dict[str, int]) -> None:
    cap = _LIVE_CAPS.get(kind)
    if cap is not None:
        if shown.get(kind, 0) >= cap:
            return
        shown[kind] = shown.get(kind, 0) + 1
    head = _parse_ts(line[:25])
    text = line[26:] if head is not None else line
    print("  t+%-7.1f %s" % (since_start, text[:220]), flush=True)


def _install_signals() -> None:
    def raise_interrupt(signum, frame):
        raise KeyboardInterrupt()

    for name in ("SIGTERM", "SIGBREAK", "SIGHUP"):
        sig = getattr(signal, name, None)
        if sig is not None:
            try:
                signal.signal(sig, raise_interrupt)
            except (ValueError, OSError):
                pass


def run(opts: RunOptions, on_start: Optional[Callable[[int, Path], None]] = None) -> Dict:
    """Do one run and return its summary dict (also written to summary.json). Raises Fail for setup problems and
    heavy_lock.LockTimeout when the lock wait expires. on_start(pid, run_dir) is called right after the launch."""
    if opts.api not in ("d3d12", "vulkan"):
        raise Fail("--api must be d3d12 or vulkan")
    if opts.config not in ("RelWithDebInfo", "Release"):
        raise Fail("--config must be RelWithDebInfo or Release")
    if opts.expect is not None and opts.expect not in STATES:
        raise Fail("--expect must be one of %s" % "|".join(STATES))
    if opts.screenshot_every is not None and opts.screenshot_every < 1:
        raise Fail("--screenshot-every must be at least 1 second")
    if os.name != "nt":
        raise Fail("run_game.py launches the Windows build: it runs on Windows only")

    work, game = _work(opts), _game(opts)
    flow_file = resolve_flow(opts.flow)
    flow_text = flow_file.read_text(encoding="utf-8")
    steps, flow_sched = parse_script(flow_text)
    extra_sched, sched_lines = load_schedule(opts.schedule)
    sched = sorted(flow_sched + extra_sched, key=lambda st: st[0])
    if opts.window_pos is not None:
        check_window_pos(opts.window_pos)
    header = flow_header(flow_text)
    expected = opts.expect or header.get("expect", "none")
    seconds = float(opts.seconds if opts.seconds is not None else header.get("seconds", DEFAULT_SECONDS))
    if seconds <= 0:
        raise Fail("--seconds must be positive")
    exe = Path(opts.exe) if opts.exe else work / "build" / "bin" / opts.config / "mcla.exe"
    if not exe.is_file():
        raise Fail("no game executable at %s (build it: cmake --build %s --config %s; or pass --exe)"
                   % (posix(exe), posix(work / "build"), opts.config))
    game_root = game / "game"
    if not (game_root / "default.xex").is_file():
        raise Fail("no default.xex in %s (python tools/xiso.py extract <iso> %s)" % (posix(game_root), posix(game_root)))
    fixture_dir = work / "fixtures" / opts.fixture if opts.fixture else None
    fixture_note = check_fixture(fixture_dir) if fixture_dir is not None else None
    if opts.run_id is not None and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", opts.run_id):
        raise Fail("--run-id may only hold letters, digits, '.', '_' and '-'")
    flow_name = flow_file.stem
    if header.get("fixture") and header["fixture"] != opts.fixture:
        print("[run_game] note: flow %s is made for fixture '%s' (--fixture %s)"
              % (flow_name, header["fixture"], opts.fixture or "not given"), file=sys.stderr, flush=True)
    last_step = max((at + hold / 1000.0 for at, hold, _ in steps), default=0.0)
    if last_step > seconds:
        print("[run_game] note: the flow has steps until %.0f s, --seconds %.0f cuts it" % (last_step, seconds),
              file=sys.stderr, flush=True)
    late = [st for st in sched if st[0] >= seconds]
    if late:
        print("[run_game] note: %d schedule step(s) at or after --seconds %.0f never run (first: '%s')"
              % (len(late), seconds, late[0][4]), file=sys.stderr, flush=True)

    lease: contextlib.AbstractContextManager = contextlib.nullcontext()
    if opts.lock:
        cmd = [os.path.basename(sys.argv[0])] + sys.argv[1:]
        lease = heavy_lock.hold("run_game %s%s" % (flow_name, " +" + opts.fixture if opts.fixture else ""),
                                cmd=cmd, wait_timeout=opts.lock_timeout, work=str(work))
    with lease:
        return _run_locked(opts, work, game_root, exe, flow_file, flow_text, flow_name, expected, seconds,
                           fixture_dir, fixture_note, on_start, sched, sched_lines)


def _run_locked(opts: RunOptions, work: Path, game_root: Path, exe: Path, flow_file: Path, flow_text: str,
                flow_name: str, expected: str, seconds: float, fixture_dir: Optional[Path],
                fixture_note: Optional[Dict[str, int]],
                on_start: Optional[Callable[[int, Path], None]], sched: Sequence[SchedStep] = (),
                sched_lines: Sequence[str] = ()) -> Dict:
    run_id = opts.run_id or "r%s-%s" % (datetime.datetime.now().strftime("%Y%m%d-%H%M%S"), flow_name)
    run_dir = work / "runs" / run_id
    if run_dir.exists():
        raise Fail("run dir %s exists already (pick another --run-id)" % posix(run_dir))
    user_dir, shots_dir = run_dir / "user", run_dir / "shots"
    cache_dir = work / "user" / "cache"
    log_path, stdout_path = run_dir / "mcla.log", run_dir / "stdout.log"
    run_flow = run_dir / "flow.txt"
    try:
        user_dir.mkdir(parents=True)
        shots_dir.mkdir()
        cache_dir.mkdir(parents=True, exist_ok=True)
        run_text = flow_text.replace("\r\n", "\n")
        if sched_lines:                      # --schedule steps ride in the run's own copy of the flow (the host reads it)
            run_text = run_text.rstrip("\n") + "\n\n" + "\n".join(sched_lines) + "\n"
        run_flow.write_bytes(run_text.encode("utf-8"))
        if fixture_dir is not None:
            shutil.copytree(fixture_dir, user_dir, dirs_exist_ok=True)
        args = build_args(opts, game_root, user_dir, cache_dir, log_path, run_flow)
        command = [str(exe)] + args

        say("run %s: flow %s%s, %s, %.0f s, expect %s" % (run_id, flow_name,
            " + fixture " + opts.fixture if opts.fixture else "", opts.api, seconds, expected))
        say("run dir %s" % posix(run_dir))

        summary: Dict = {
            "run_id": run_id, "state": "running", "flow": flow_name, "flow_file": posix(flow_file),
            "fixture": opts.fixture, "fixture_copied": fixture_note, "api": opts.api, "config": opts.config,
            "exe": posix(exe), "args": args, "seconds": seconds, "expected": expected,
            "window_pos": effective_window(opts)[0], "window_behind": effective_window(opts)[1],
            "schedule": list(opts.schedule), "schedule_steps": [st[4] for st in sched],
        }
        write_json(run_dir / "summary.json", summary)

        job = Job()
        env = dict(os.environ)
        if not opts.activate:
            # SDL3 reads its hints from the environment: its own ShowWindow must not activate (focus-steal) the window.
            env["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] = "0"
        started_wall = time.time()
        started = time.monotonic()
        try:
            with open(stdout_path, "wb") as out:
                startupinfo = None
                if not opts.activate:
                    # The game's first ShowWindow takes this show state: shown, not activated, so the owner's
                    # foreground window keeps the keyboard (the scripted pad needs no focus).
                    startupinfo = subprocess.STARTUPINFO()
                    startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                    startupinfo.wShowWindow = 4  # SW_SHOWNOACTIVATE
                proc = subprocess.Popen(command, cwd=str(run_dir), stdin=subprocess.DEVNULL, stdout=out,
                                        stderr=subprocess.STDOUT, env=env, startupinfo=startupinfo,
                                        creationflags=0x00000200 | 0x08000000)  # NEW_PROCESS_GROUP | NO_WINDOW
        except OSError as e:
            raise Fail("cannot start %s: %s" % (posix(exe), e)) from e
    except BaseException:  # nothing ran: leave no half-made run dir behind
        shutil.rmtree(run_dir, ignore_errors=True)
        raise
    if not job.add(proc):
        say("warning: could not put the game in a job object; the kill falls back to taskkill /T")
    summary["pid"] = proc.pid
    write_json(run_dir / "summary.json", summary)
    say("started pid %d" % proc.pid)

    shooter: Optional[Shooter] = None
    if opts.screenshot_every is not None:
        shooter = Shooter(lambda: job.pids() or [proc.pid], shots_dir, opts.topmost)
    scanner = LogScanner(started_wall)
    tail = LogTail(log_path, scanner)
    errors_shown: Dict[str, int] = {}
    ended_by = "timeout"
    reached_at: Optional[float] = None
    next_shot = opts.screenshot_every or 0.0
    exit_code: Optional[int] = None
    # Load spinners of the schedule's burn / burnmem steps (their time is the script's clock = this process start). None is
    # started before the game runs; the finally below kills them on every way out (the job object is the second net).
    spinners = Spinners(job, started)
    burns = [st for st in sched if st[1] in ("burn", "burnmem")]
    spinners_left = 0

    try:
        keep_awake(True)
        if on_start is not None:
            on_start(proc.pid, run_dir)
        while True:
            for kind, line in tail.pump():
                show_live(kind, line, time.monotonic() - started, errors_shown)
            elapsed = time.monotonic() - started
            rc = proc.poll()
            if rc is not None:
                ended_by, exit_code = "exited", rc
                break
            while burns and burns[0][0] <= elapsed:
                _at, burn_kind, _name, burn_n, _text = burns.pop(0)
                spinners.apply(burn_kind, int(burn_n), elapsed)
            if opts.stop_on_reach and expected != "none" and RANK[scanner.reached] >= RANK[expected]:
                if reached_at is None:
                    reached_at = elapsed
                if elapsed - reached_at >= STOP_GRACE_S:
                    ended_by = "stop-on-reach"
                    break
            if elapsed >= seconds:
                ended_by = "timeout"
                break
            if shooter is not None and elapsed >= next_shot:
                shooter.shoot(shot_name(next_shot))
                next_shot += opts.screenshot_every
            nap = 0.25
            if burns:                                  # wake for the next burn step (a spinner step is on time to ~10 ms)
                nap = max(0.005, min(nap, burns[0][0] - (time.monotonic() - started)))
            time.sleep(nap)
    except KeyboardInterrupt:
        ended_by = "interrupted"
    finally:
        elapsed = time.monotonic() - started
        try:
            if proc.poll() is None:
                if shooter is not None and ended_by != "interrupted":
                    shooter.shoot("t%d_end.png" % int(round(elapsed)))
                kill_tree(proc, job)
        finally:
            spinners_left = spinners.kill_all()
            job.close()
            keep_awake(False)

    # One last read of what the kill left on disk, then the authoritative parse of the whole log.
    for kind, line in tail.pump():
        show_live(kind, line, time.monotonic() - started, errors_shown)
    process_fatal = 1 if ended_by == "exited" else 0
    parsed = parse_log(log_path, t0=started_wall)
    evaluate(parsed, expected, process_fatal)
    parsed["heartbeat_gap_s"] = (None if parsed["heartbeat_last_s"] is None
                                 else round(elapsed - parsed["heartbeat_last_s"], 1))
    summary.update(parsed)
    summary.update({
        "state": "done", "started": iso(started_wall), "ended": iso(time.time()), "elapsed_s": round(elapsed, 1),
        "ended_by": ended_by, "exit_code": exit_code, "user_saves": user_saves(user_dir),
        "shots": shooter.taken if shooter else [], "shot_notes": shooter.final_notes() if shooter else [],
        "shot_methods": shooter.methods if shooter else {},
        "burn_events": spinners.events,
    })
    if not log_path.is_file():
        summary["log_missing"] = True
    summary["exit_status"] = EXIT_INTERRUPTED if ended_by == "interrupted" else (0 if summary["ok"] else EXIT_FAILED)
    write_json(run_dir / "summary.json", summary)

    if ended_by == "exited":
        say("the process exited by itself after %.1f s with code %s (0x%08X): counted as one fatal; see %s"
            % (elapsed, exit_code, (exit_code or 0) & 0xFFFFFFFF, posix(stdout_path)))
        tail_text = stdout_path.read_text(encoding="utf-8", errors="replace").strip()
        if tail_text:
            print("\n".join("  stdout| " + l for l in tail_text.splitlines()[-15:]), flush=True)
    if not log_path.is_file():
        say("warning: no log was written at %s (the host died before logging?)" % posix(log_path))
    if spinners.events or spinners.died_early:
        say("burn: %d step(s) run, %d spinner(s) killed at the end%s%s" % (
            len(spinners.events), spinners.killed_at_end,
            (", %d died early" % spinners.died_early) if spinners.died_early else "",
            "" if not spinners_left else "; %d STILL ALIVE (kill them by hand)" % spinners_left))
    if not summary["expect_met"]:
        say("expected '%s' but reached '%s'" % (expected, summary["reached"]))
    if shooter is not None:
        for note in shooter.final_notes():
            say("screenshots: " + note)
    say("summary %s" % posix(run_dir / "summary.json"))
    return summary


# ================================================================================================================
# Command line
# ================================================================================================================

def _summarize(arg: str, opts: RunOptions) -> int:
    work = _work(opts)
    p = Path(arg)
    if not p.exists():
        p = work / "runs" / arg
    summary_json = None
    if p.is_dir():
        summary_json = p / "summary.json"
        p = p / "mcla.log"
    if not p.is_file() and not log_files(p):
        raise Fail("no log at %s" % p)
    run_id = p.parent.name if p.name == "mcla.log" else p.stem
    meta: Dict = {}
    if summary_json is not None and summary_json.is_file():  # a run_game run: its process start, expectation, exit
        try:
            meta = json.loads(summary_json.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            meta = {}
    expected = opts.expect or meta.get("expected") or "none"
    parsed = evaluate(parse_log(p, t0=meta.get("t0_epoch")), expected, int(meta.get("fatal_process", 0)))
    print(run_line(run_id, parsed))
    return 0 if parsed["ok"] else EXIT_FAILED


def _sched_line(st: SchedStep, source: str) -> str:
    at, kind, name, value, text = st
    who = "run_game spinners + host log" if kind in ("burn", "burnmem") else ("host sets the cvar" if kind == "set" else "host marker")
    return "  %7.1f s  %-34s [%s; %s]" % (at, text.split(":", 1)[1].strip(), who, source)


def _check_flow(name: str, opts: RunOptions) -> int:
    path = resolve_flow(name)
    text = path.read_text(encoding="utf-8")
    steps, flow_sched = parse_script(text)
    extra, _ = load_schedule(opts.schedule)
    check_window_pos(opts.window_pos or "")
    header = flow_header(text)
    end = max((at + hold / 1000.0 for at, hold, _ in steps), default=0.0)
    print("flow %s: %d steps, last ends at %.1f s; expect=%s seconds=%s fixture=%s"
          % (path.name, len(steps), end, header.get("expect", "none"), header.get("seconds", "-"),
             header.get("fixture", "-")))
    both = [(st, "flow") for st in flow_sched] + [(st, "--schedule") for st in extra]
    if both:
        both.sort(key=lambda r: r[0][0])
        print("schedule: %d step(s) (%d in the flow, %d from --schedule)" % (len(both), len(flow_sched), len(extra)))
        for st, source in both:
            print(_sched_line(st, source))
    pos, behind = effective_window(opts)
    print("window: pos=%s behind=%s (SDL activate-when-shown %s)"
          % (pos or "SDK default", "yes" if behind else "no", "as inherited" if opts.activate else "off"))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        prog="run_game.py", formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Run mcla.exe with a scripted pad, tail its log, take screenshots and summarise the run.",
        epilog="The last line printed is `run <id>: reached=<none|title|menu|free-roam> fatal=<n> unregistered=<n> "
               "heartbeat_last=<s>`. Exit 0 iff fatal=0, unregistered=0 and the flow's expected state was reached; "
               "1 run failed that test; 2 setup problem; 75 lock timeout; 130 interrupted. Details: the docstring "
               "at the top of this file.")
    ap.add_argument("--flow", metavar="NAME", help="pad script: tools/flows/<NAME>.txt or a file path")
    ap.add_argument("--fixture", metavar="NAME", help="copy $WORK/fixtures/<NAME>/ into the fresh user dir")
    ap.add_argument("--seconds", type=float, metavar="N",
                    help="run time in seconds from process start (default: the flow's `# seconds:` header, else %d)"
                         % DEFAULT_SECONDS)
    ap.add_argument("--api", choices=("d3d12", "vulkan"), default="d3d12", help="GPU backend (default d3d12)")
    ap.add_argument("--cvar", action="append", default=[], metavar="K=V",
                    help="extra --K=V for the game; repeatable")
    ap.add_argument("--screenshot-every", type=float, metavar="S", help="screenshot the game window every S seconds")
    ap.add_argument("--run-id", metavar="ID", help="run name (default r<yyyymmdd-HHMMSS>-<flow>)")
    ap.add_argument("--config", choices=("RelWithDebInfo", "Release"), default=DEFAULT_CONFIG,
                    help="build config of $WORK/build/bin/<config>/mcla.exe (default %s)" % DEFAULT_CONFIG)
    ap.add_argument("--exe", metavar="PATH", help="game executable (default $WORK/build/bin/<config>/mcla.exe)")
    ap.add_argument("--no-lock", action="store_true", help="do not take the heavy lock")
    ap.add_argument("--lock-timeout", type=float, metavar="S", help="give up (exit 75) if the lock is not obtained in S s")
    ap.add_argument("--stop-on-reach", action="store_true",
                    help="stop %d s after the expected state is reached" % STOP_GRACE_S)
    ap.add_argument("--expect", choices=STATES, help="override the flow's `# expect:` header")
    ap.add_argument("--topmost", action="store_true", help="raise the game window above others for the screenshots")
    ap.add_argument("--audio", action="store_true", help="do not mute the game (default: --audio_mute=true)")
    ap.add_argument("--activate", action="store_true",
                    help="let the game window take the focus (default: shown without activation)")
    ap.add_argument("--schedule", action="append", default=[], metavar="SPEC",
                    help="a timed schedule (set / burn / burnmem / mark steps) added to the flow: a file, a name in "
                         "tools/flows/schedules/, or inline `58:set k=v;70:burn 4`; repeatable")
    ap.add_argument("--window-pos", metavar="POS",
                    help="game window position, --mcla_window_pos=POS: bottom-right | bottom-left | top-right | top-left | X,Y | "
                         "\"\" (SDK default, centred). Default bottom-right (\"\" with --topmost / --activate)")
    ap.add_argument("--behind", dest="behind", action="store_true", default=None,
                    help="place the window just behind the foreground window after it is shown, --mcla_window_behind=true "
                         "(default on, off with --topmost / --activate)")
    ap.add_argument("--no-behind", dest="behind", action="store_false", help="do not place the window behind the foreground one")
    ap.add_argument("--work", metavar="W", help="$WORK (default env MCLA_WORK, else %s)" % DEFAULT_WORK)
    ap.add_argument("--game", metavar="G", help="$GAME (default env MCLA_GAME, else %s)" % DEFAULT_GAME)
    ap.add_argument("--check-flow", action="store_true", help="validate the flow file (and --schedule) and stop")
    ap.add_argument("--selftest-burn", action="store_true",
                    help="no game: start 2 ALU + 1 memory spinner for 2 s, check them and that they are gone, and stop")
    ap.add_argument("--summarize", metavar="RUN", help="parse an existing run (id, run dir or log file) and stop")
    args = ap.parse_args(argv)

    opts = RunOptions(flow=args.flow or "", fixture=args.fixture, seconds=args.seconds, api=args.api,
                      cvars=args.cvar, screenshot_every=args.screenshot_every, run_id=args.run_id,
                      config=args.config, exe=args.exe, lock=not args.no_lock, lock_timeout=args.lock_timeout,
                      stop_on_reach=args.stop_on_reach, expect=args.expect, topmost=args.topmost,
                      audio=args.audio, activate=args.activate, work=args.work, game=args.game,
                      schedule=args.schedule, window_pos=args.window_pos, behind=args.behind)
    try:
        if args.selftest_burn:
            return spinner_selftest()
        if args.summarize:
            return _summarize(args.summarize, opts)
        if not args.flow:
            ap.error("--flow is required")
        if args.check_flow:
            return _check_flow(args.flow, opts)
        _install_signals()
        summary = run(opts)
    except Fail as e:
        print("run_game: %s" % e, file=sys.stderr)
        return EXIT_SETUP
    except heavy_lock.LockTimeout as e:
        print("run_game: %s" % e, file=sys.stderr)
        return EXIT_LOCK_TIMEOUT
    except KeyboardInterrupt:
        print("run_game: interrupted", file=sys.stderr)
        return EXIT_INTERRUPTED
    print(run_line(summary["run_id"], summary))
    return summary["exit_status"]


if __name__ == "__main__":
    sys.exit(main())
