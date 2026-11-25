#!/usr/bin/env python3
"""bench.py - the MCLA bench harness: variants run back to back, PresentMon on the game PID, METRICS rows (PLAN 6, 7.4, C).

    python tools/bench.py --flow bench_drive --fixture after_intro --seconds 360 --variant base: \\
                          --variant sim60:mcla_sim60=true [--api d3d12|vulkan] [--repeat k] [--parked] [--gate m4|m5]
    python tools/bench.py --dry-metrics <pm.csv> <mcla.log> [--append] [--metrics-file F] [--pm-t0 S | --pm-meta F]
    python tools/bench.py ... --plan      # print the run sequence and stop

What a bench does
  1. Takes the heavy lock ONCE for the whole bench (tools/heavy_lock.py; `--no-lock` to skip), so the runs are back to back.
  2. Runs, each in its own game process through run_game.run() (fresh user dir + the fixture, shared shader cache; exe =
     $WORK/build/bin/Release/mcla.exe, benches use Release: `--config RelWithDebInfo` / `--exe` to change):
         a warm-up run (the first variant's cvars; dropped; `--no-warmup` skips it, `--warmup-each` warms every variant),
         then, for round r = 1..--repeat, every variant (the order rotates by one each round: A B / B A / ...).
     With `--parked` each measured drive run is followed by a parked run (flow bench_parked), see "Stable 60" below.
  3. A variant is `name:cvar=value,cvar=value` (`base:` = no cvars). Its cvars go to the game as `--cvar` overrides after the
     shared contract of run_game.py; `--cvar k=v` (repeatable) adds cvars to every variant, the variant wins on a clash.
  4. PresentMon 2.6 (`$WORK/toolchain/presentmon/PresentMon-2.6.0-x64.exe`, no admin) is started from run_game's on_start hook
     on the game PID: `--process_id <pid> --output_file <run>/pm.csv --qpc_time_ms --timed <seconds-2> --terminate_after_timed
     --terminate_on_proc_exit --no_console_stats --session_name mcla_bench_<pid> --stop_existing_session`. `--timed` ends the
     capture itself a moment before run_game kills the game: in a W2.2 test, a non-elevated PresentMon did NOT end on
     `--terminate_on_proc_exit` within 30 s after its target was killed, and a PresentMon that is killed leaves its ETW session
     behind. So a capture still alive 10 s after the run is killed and its session stopped (`--terminate_existing_session`);
     its CSV then lacks the last ~0.2 s (a cut last row is skipped). <run>/pm_meta.json records the command, the QPC offset, rc, killed.
  5. Per run, the metrics of the WINDOW = from `[mcla-mode] <from> -> free-roam` (first one) + 20 s (`--window-skip`) to the
     end of the capture (the drive runs; ~2 s before the kill), or for 60 s (`--parked-seconds`) in the parked runs.
  6. Prints per measured run, exactly:
         bench <id> <variant>: avg_fps=<x> p50_ms=<x> p99_ms=<x> pct_over_25ms=<x> pct_over_50ms=<x> max_ms=<x> world_real=<x>
     (<id> = the run id = the folder $WORK/runs/<id>; an INVALID run prints `bench <id> <variant>: INVALID <reasons>`), then
     indented detail lines, and, for --repeat > 1, `aggregate <variant> <kind> n=<k>: ...` (pooled frames of the k windows).
     Appends a row per measured run to docs/METRICS.md (see below), writes <run>/metrics.json and $WORK/runs/<bench id>.bench.json.

Time alignment (log time <-> PresentMon time)
  The log stamps are local wall-clock (spdlog, ms): the first `-> free-roam` line gives FR (epoch, local).
  PresentMon is started with `--qpc_time_ms`: every row carries CPUStartQPCTimeInMs, the CPU start of the frame on the QueryPerformanceCounter
  clock, which is also Python's time.perf_counter() on Windows. bench.py samples (time.time() - time.perf_counter()) just
  before it starts PresentMon (median of 9 reads, stored in <run>/pm_meta.json as qpc_offset_s) and maps frame epoch =
  CPUStartQPC/1000 + qpc_offset_s. Residual error: the ms of the log stamp plus the clock-sampling jitter (a few ms). (PresentMon's
  own `--date_time` was not used: on this machine its CPUStartDateTime ran 5 h behind the local clock.)
  A frame is IN the window when its CPU start epoch is >= FR + skip (and < FR + skip + window length, when there is one).
  The first row of a swap chain is dropped (its MsBetweenPresents has no predecessor in the capture; E2 did the same).
  For a CSV from a plain PresentMon run (no QPC column, as the E2 CSVs): `--dry-metrics` maps CPUStartTimeInMs relative to
  `--pm-t0 S` (seconds after the first log line at which the CSV clock is 0); without it the capture is assumed to have
  started at the window start (the E2 harness started PresentMon at FR + 20 s).

Metrics (over the window frames; E2's definitions, $WORK/scratch/e2_analyze.py)
  frame time = MsBetweenPresents (FrameTime / msBetweenPresents in other PresentMon versions); avg_fps = frames / sum(frame time);
  p50/p99 = linear interpolation between order statistics; pct_over_25ms / pct_over_50ms = % of frames longer than 25 / 50 ms;
  max_ms; gpu_busy_pct = sum(MsGPUBusy) / sum(MsBetweenPresents) (frames with a GPU value); the swap chain with the most frames in the
  window is the game's (a second one, e.g. an overlay, is ignored).
  world_real = sum(world s) / sum(real s) over the `[mcla-sim] <real> s real: world <world> s (x<ratio>)` check lines whose whole
  interval lies in the window (the line is stamped at the interval's end). n/a when the log has none: those lines exist only with
  `mcla_sim60=true` (mcla_t_common.cpp logs them every mcla_sim_log_s s while the sim is on). For the 30 fps M4 row use a
  variant such as `base:mcla_sim60=true,mcla_sim_cap=30` to get world_real (30 equals the shipped timing).
  The `[mcla-sim]   frame times` lines in the window give notes `sim frame times p50/p99/worst` (median / median / max), and the
  `[mcla-sim] on:` line gives `sim on`.

INVALID runs (exit 3; the row is still appended, notes start with INVALID, the numbers are n/a; partial numbers go into the notes)
  no `-> free-roam` line; the game exited by itself or logged a fatal / unregistered-function line; the guard killed it (no free
  roam `--guard-at` 150 s after the launch; 0 = off); PresentMon produced no frames in the window or was never started; the
  window is shorter than half of the expected length. Every other run is judged only by its numbers (`--gate`). A window that is
  not wholly in free roam (a `[mcla-mode]` line inside it, e.g. a Start press that opened the pause menu) is only a WARNING in the
  detail lines and the notes.

Stable 60 (PLAN 6): "a 300 s scripted drive plus 60 s parked in the busiest street, on a warm second run". Choice: TWO PROCESSES per
  variant and round with `--parked`: bench_drive (--seconds 360: FR + 20 s to the end is ~300-310 s) and bench_parked (--parked-run-seconds
  150: FR + 20 s for 60 s, the car stays in the spawn crowd = the heavy stationary scene of the iOS bench), each with its own row, plus
  a pooled row `<id>-stable` (flow `bench_drive+bench_parked`): the frames of both windows concatenated, world/real pooled. That pooled
  row is the one PLAN 6's definition is applied to (`--gate m5`: avg >= 59.5, p99 <= 20, <= 0.5 % over 25 ms, max <= 100, world/real
  1.000 +- 0.005; `--gate m4`: avg 29.9..30.1, p99 <= 35, <= 0.5 % over 50 ms, world/real 1.000 +- 0.005). `--gate` prints `gate <m> <id>:
  PASS|FAIL (...)` for the drive runs, or with `--parked` for the pooled rows only (never the parked segment alone); exit 4 on a FAIL.
  A metric that cannot be checked FAILS the gate: with no `world_real` (no `[mcla-sim]` lines: the variant lacks `mcla_sim60=true`) the
  line is `gate <m> <id>: FAIL [unchecked: world_real] (needs a variant with mcla_sim60=true)`.

docs/METRICS.md rows (never hand-edited; `--no-metrics` skips, `--metrics-file F` / env MCLA_METRICS_FILE appends elsewhere)
  Appended under `## Runs` (created, with the table header, if missing), after the last row of its table. The header of the existing
  table decides the columns (matched by name); a value without a column of its own goes into notes. PLAN 7.4 columns: run id | build id |
  flow | API | internal resolution | cap | variant cvars | avg fps | p50 ms | p99 ms | % > 25 ms | max ms | world/real | notes.
  build id: --build-id, else the log's `[mcla] <app> <config> built <date> <time>` stamp as `<config>@<yyyy-mm-ddThh:mm:ss>`, else `?`.
  internal resolution: the log's `[mcla-wide] ... back buffer WxH`, else cvars (`resolution_scale`, `mcla_widescreen_width` +
  `mcla_scene_height`, `resolution=WxH`), else 1280x720.  cap: the first of mcla_fps / mcla_fps_cap / mcla_frame_cap / mcla_cap /
  mcla_sim_cap among the cvars (0 = uncapped), else the `cap N` of `[mcla-sim] on:`, else `default`.
  notes: window length and frame count, `> 50 ms` and GPU busy when the table has no column for them, power (AC/battery via
  GetSystemPowerStatus, battery %, power scheme and the Windows power-mode overlay via powrprof), warm-up run id, repetition, fixture,
  `sim on` and the sim frame-time summary, --note.

Telemetry (R6: the package is capped at ~35 W and the GPU clock swings 1.25-1.9 GHz with background CPU load)
  If $WORK/toolchain/telemetry/sampler.exe exists (built from tools/telemetry, see its README; `--sampler PATH` names another,
  `--no-telemetry` skips everything in this paragraph), `sampler.exe <run>/adlx.csv <seconds-4> 100 <exe name>` runs next to
  PresentMon (ADLX: GPU clock, package power, temperature; 1 Hz real resolution; it ends by itself before the kill), and bench.py
  itself samples the machine's CPU load every 0.5 s into <run>/cpu.csv (busy cores of the whole system by GetSystemTimes minus
  the game's cores by GetProcessTimes = non-game cores). Alignment: the sampler's t_s clock has no epoch, so sampler_meta.json
  records start_epoch = the CSV's mtime minus its last t_s (the CSV is flushed every second and closed when sampler.exe ends or
  is killed); cpu.csv carries epochs. Over the measured window the notes get
  `gpuclk=<p50>/<p10>MHz pkg=<W>W nongame_cpu=<cores> tmax=<C>C` (medians; tmax is the maximum).
  TAINTED, leading the notes as `TAINTED (...)`: the non-game CPU median is above 1.5 cores (flagged at once); or, when the bench
  is done, the variants of one kind of run differ by more than 10 % of the lower in median GPU clock (the median of their runs'
  p50; every row of that kind is rewritten in place). A `TAINTED <id>: ...` line closes the output and bench.json has the flags;
  the exit code and --gate do not change. Telemetry never makes a run INVALID; without sampler.exe the notes lack gpuclk/pkg/tmax.

--dry-metrics PM_CSV LOG   recompute the metrics of existing files (no game, no lock); prints the `bench` line, exits 3 when INVALID.
  Options: --pm-meta F (default: pm_meta.json beside the CSV, for the QPC offset), --qpc-offset S, --pm-t0 S, --window-skip, --window-seconds,
  --pid N (only that process), --id, --variant name:cvars (labels the line, cap/resolution), --api, --build-id, --note, --gate,
  --append (append the row; off by default), --metrics-file. Telemetry files beside the CSV are used when present: --adlx F
  (adlx.csv), --adlx-meta F (sampler_meta.json: start_epoch), --adlx-t0 S (seconds after the first log line at which the sampler's
  t_s is 0; wins over the meta; with neither, t_s 0 is assumed at the window start), --cpu-csv F (cpu.csv).

Exit codes: 0 done (valid runs); 2 setup problem (bad option, flow, PresentMon missing; nothing ran); 3 a measured run INVALID;
  4 --gate failed on a valid run; 75 heavy-lock wait expired; 130 interrupted. Stdlib only.
"""

import argparse
import csv
import ctypes
import datetime
import json
import math
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
if str(HERE) not in sys.path:  # tools/ is on sys.path when run as a script; this lets `import bench` find its neighbours
    sys.path.insert(0, str(HERE))
import heavy_lock  # noqa: E402
import run_game  # noqa: E402

DEFAULT_METRICS = REPO / "docs" / "METRICS.md"
DEFAULT_PRESENTMON = "toolchain/presentmon/PresentMon-2.6.0-x64.exe"   # under $WORK
DEFAULT_FLOW = "bench_drive"
DEFAULT_SECONDS = 360.0
WINDOW_SKIP_S = 20.0
PARKED_FLOW = "bench_parked"
PARKED_RUN_SECONDS = 150.0
PARKED_WINDOW_S = 60.0
GUARD_AT_S = 150.0
PM_TIMED_MARGIN_S = 2.0        # PresentMon stops this long before run_game kills the game
PM_FINISH_GRACE_S = 10.0
SIM_TOLERANCE_S = 0.6          # a [mcla-sim] interval may start this much before the window and still count
MIN_WINDOW_FRACTION = 0.5
SAMPLER_EXE = "toolchain/telemetry/sampler.exe"   # under $WORK (tools/telemetry/README.md)
SAMPLER_MARGIN_S = 4.0         # sampler.exe is told to run this much shorter than the run, so it ends by itself
CPU_SAMPLE_S = 0.5
TAINT_CPU_CORES = 1.5          # median non-game CPU load (cores) above which a run is TAINTED
TAINT_CLOCK_FRAC = 0.10        # variants whose median GPU clock differs by more than this (of the lower) are TAINTED

EXIT_SETUP = run_game.EXIT_SETUP
EXIT_INVALID = 3
EXIT_GATE = 4
EXIT_LOCK_TIMEOUT = run_game.EXIT_LOCK_TIMEOUT
EXIT_INTERRUPTED = run_game.EXIT_INTERRUPTED

# PLAN 6: M4 (locked 30 fps) and M5/M6 (stable 60). (key, test, bounds)
GATES: Dict[str, List[Tuple]] = {
    "m4": [("avg_fps", "range", 29.9, 30.1), ("p99_ms", "max", 35.0), ("pct_over_50ms", "max", 0.5),
           ("world_real", "near", 1.0, 0.005)],
    "m5": [("avg_fps", "min", 59.5), ("p99_ms", "max", 20.0), ("pct_over_25ms", "max", 0.5), ("max_ms", "max", 100.0),
           ("world_real", "near", 1.0, 0.005)],
}

CAP_CVARS = ("mcla_fps", "mcla_fps_cap", "mcla_frame_cap", "mcla_cap", "mcla_sim_cap")
CVAR_KEY = re.compile(r"[A-Za-z0-9_.-]+")
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


class SetupError(Exception):
    """Something is wrong with the options or the environment; nothing was run. Exit 2."""


# ================================================================================================================
# Small helpers
# ================================================================================================================

def say(msg: str) -> None:
    print("[bench] " + msg, flush=True)


def percentile(sorted_vals: Sequence[float], p: float) -> float:
    """Linear interpolation between order statistics (the E2 analyzer's pct())."""
    k = (len(sorted_vals) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def num(v: Optional[float], digits: int = 2) -> str:
    return "n/a" if v is None else ("%.*f" % (digits, v))


def rel(epoch: Optional[float], base: Optional[float]) -> str:
    return "?" if epoch is None or base is None else "%.1f s" % (epoch - base)


def md_cell(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\r", " ").replace("\n", " ").strip()


# ================================================================================================================
# Variants
# ================================================================================================================

@dataclass
class Variant:
    name: str
    cvars: List[Tuple[str, str]] = field(default_factory=list)

    def specs(self) -> List[str]:
        return ["%s=%s" % kv for kv in self.cvars]


def parse_cvar(spec: str) -> Tuple[str, str]:
    key, eq, value = spec.strip().partition("=")
    key = key.strip().lstrip("-")
    if not eq or not CVAR_KEY.fullmatch(key):
        raise SetupError("'%s' is not cvar=value" % spec)
    return key, value.strip()


def parse_variant(text: str) -> Variant:
    name, colon, rest = text.partition(":")
    name = name.strip()
    if not colon or not NAME_RE.fullmatch(name):
        raise SetupError("variant '%s': the form is name:cvar=value,cvar=value (`base:` = no cvars; the name uses letters, "
                         "digits, '.', '_', '-')" % text)
    cvars: List[Tuple[str, str]] = []
    for part in rest.split(","):
        if part.strip():
            cvars.append(parse_cvar(part))
    return Variant(name, cvars)


def merged_cvars(common: Sequence[Tuple[str, str]], variant: Variant) -> List[Tuple[str, str]]:
    """The shared --cvar list, then the variant's; a repeated key keeps the last value at its first position."""
    out: Dict[str, str] = {}
    for key, value in list(common) + list(variant.cvars):
        out[key] = value
    return list(out.items())


# ================================================================================================================
# The log: free roam, [mcla-sim] lines, [mcla-wide], build stamp (absolute local epochs)
# ================================================================================================================

_RE_MODE = re.compile(r"\[mcla-mode\] (?P<frm>[a-z][a-z-]*) -> (?P<to>[a-z][a-z-]*)")
_RE_SIM = re.compile(r"\[mcla-sim\] (?P<real>[\d.]+) s real: world (?P<world>[\d.]+) s \(x(?P<x>[\d.]+)\), "
                     r"frames (?P<frames>\d+) .*?cap (?P<cap>\d+)")
_RE_SIM_FT = re.compile(r"\[mcla-sim\]\s+frame times: (?P<frames>\d+) frames, p50 (?P<p50>[\d.]+) p99 (?P<p99>[\d.]+) "
                        r"worst (?P<worst>[\d.]+) ms, over 20 / 25 / 34 ms: (?P<o20>\d+) / (?P<o25>\d+) / (?P<o34>\d+)")
_RE_SIM_ON = re.compile(r"\[mcla-sim\] on: world step n/60 s.*?cap (?P<cap>\d+)")
_RE_WIDE = re.compile(r"\[mcla-wide\] mode (?P<mw>\d+)x(?P<mh>\d+), back buffer (?P<bw>\d+)x(?P<bh>\d+)")
_RE_BUILT = re.compile(r"\[mcla\] \S+ (?P<cfg>\S+) built (?P<mon>[A-Z][a-z]{2}) +(?P<day>\d+) (?P<year>\d{4}) "
                       r"(?P<time>\d\d:\d\d:\d\d)")
_MONTHS = {m: i + 1 for i, m in enumerate(("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"))}


def scan_log(path) -> Dict:
    """One pass over the log (rotated parts included): the facts the bench needs, with absolute epochs (local clock)."""
    out: Dict = {"first_ts": None, "last_ts": None, "fr_ts": None, "fr_from": None, "modes": [], "sim": [], "sim_ft": [],
                 "sim_on_ts": None, "sim_on_cap": None, "sim_off": 0, "wide": None, "built": None, "lines": 0}
    last_raw: Optional[bytes] = None
    for f in run_game.log_files(path):
        with open(f, "rb") as fh:
            for raw in fh:
                out["lines"] += 1
                if out["first_ts"] is None:
                    out["first_ts"] = run_game._parse_ts(raw[:25].decode("ascii", "replace"))
                if b"[mcla" not in raw:
                    last_raw = raw
                    continue
                last_raw = None
                line = raw.decode("utf-8", "replace")
                ts = run_game._parse_ts(line[:25])
                if ts is not None:
                    out["last_ts"] = ts
                if "[mcla-mode]" in line:
                    m = _RE_MODE.search(line)
                    if m and ts is not None:
                        if len(out["modes"]) < 500:
                            out["modes"].append((ts, m.group("frm"), m.group("to")))
                        if m.group("to") == "free-roam" and out["fr_ts"] is None:
                            out["fr_ts"], out["fr_from"] = ts, m.group("frm")
                elif "[mcla-sim]" in line:
                    if ts is None:
                        continue
                    m = _RE_SIM.search(line)
                    if m:
                        out["sim"].append({"ts": ts, "real": float(m.group("real")), "world": float(m.group("world")),
                                           "frames": int(m.group("frames")), "cap": int(m.group("cap"))})
                        continue
                    m = _RE_SIM_FT.search(line)
                    if m:
                        out["sim_ft"].append({"ts": ts, "frames": int(m.group("frames")), "p50": float(m.group("p50")),
                                              "p99": float(m.group("p99")), "worst": float(m.group("worst")),
                                              "o25": int(m.group("o25"))})
                        continue
                    m = _RE_SIM_ON.search(line)
                    if m and out["sim_on_ts"] is None:
                        out["sim_on_ts"], out["sim_on_cap"] = ts, int(m.group("cap"))
                    elif "[mcla-sim] off:" in line:
                        out["sim_off"] += 1
                elif "[mcla-wide]" in line:
                    m = _RE_WIDE.search(line)
                    if m and out["wide"] is None:
                        out["wide"] = {"mode": (int(m.group("mw")), int(m.group("mh"))),
                                       "back": (int(m.group("bw")), int(m.group("bh")))}
                elif out["built"] is None:
                    m = _RE_BUILT.search(line)
                    if m and m.group("mon") in _MONTHS:
                        out["built"] = "%s@%04d-%02d-%02dT%s" % (m.group("cfg"), int(m.group("year")), _MONTHS[m.group("mon")],
                                                                 int(m.group("day")), m.group("time"))
    if last_raw is not None:
        ts = run_game._parse_ts(last_raw[:25].decode("ascii", "replace"))
        if ts is not None:
            out["last_ts"] = ts
    return out


def world_real(sim: List[Dict], w0: float, w1: float) -> Tuple[Optional[float], float, float, int]:
    """(world/real, sum world, sum real, lines): the check lines whose whole interval lies in [w0, w1]."""
    world = real = 0.0
    n = 0
    for e in sim:
        if e["ts"] - e["real"] >= w0 - SIM_TOLERANCE_S and e["ts"] <= w1 + SIM_TOLERANCE_S:
            world += e["world"]
            real += e["real"]
            n += 1
    return (world / real if real > 0 else None), world, real, n


def sim_frame_times(sim_ft: List[Dict], w0: float, w1: float) -> Optional[Dict]:
    lines = [e for e in sim_ft if w0 < e["ts"] <= w1 + SIM_TOLERANCE_S]
    if not lines:
        return None
    return {"lines": len(lines), "p50": statistics.median(e["p50"] for e in lines),
            "p99": statistics.median(e["p99"] for e in lines), "worst": max(e["worst"] for e in lines),
            "over_25": sum(e["o25"] for e in lines), "frames": sum(e["frames"] for e in lines)}


# ================================================================================================================
# The PresentMon CSV
# ================================================================================================================

_TIME_ABS = ("cpustartqpctimeinms",)                                    # ms on the QueryPerformanceCounter clock
_TIME_REL = (("cpustarttimeinms", 1e-3), ("timeinms", 1e-3), ("cpustarttime", 1e-3), ("timeinseconds", 1.0))
_FT_COLS = ("msbetweenpresents", "frametime")
_GPU_COLS = ("msgpubusy", "gpubusy")


@dataclass
class PmSeries:
    key: Tuple[str, str]
    t: List[float] = field(default_factory=list)          # seconds on the CSV clock
    ft: List[float] = field(default_factory=list)         # ms between presents
    gpu: List[Optional[float]] = field(default_factory=list)


@dataclass
class PmCsv:
    path: str
    clock: str                                             # "qpc" (absolute) or "relative"
    series: Dict[Tuple[str, str], PmSeries]
    rows: int
    skipped: int                                           # partial / unparsable rows


def _first_col(idx: Dict[str, int], names: Sequence[str]) -> Optional[int]:
    for n in names:
        if n in idx:
            return idx[n]
    return None


def _float_or_none(s: str) -> Optional[float]:
    try:
        v = float(s)
    except ValueError:
        return None
    return v if math.isfinite(v) else None


def load_pm_csv(path, pid: Optional[int] = None) -> PmCsv:
    """Read a PresentMon CSV (BOM, partial last row tolerated). Raises ValueError when it lacks the needed columns."""
    with open(path, "r", encoding="utf-8-sig", newline="") as fh:
        rd = csv.reader(fh)
        header = next(rd, None)
        if not header:
            raise ValueError("%s is empty" % path)
        idx = {h.strip().lower(): i for i, h in enumerate(header)}
        i_ft = _first_col(idx, _FT_COLS)
        i_gpu = _first_col(idx, _GPU_COLS)
        i_abs = _first_col(idx, _TIME_ABS)
        scale = 1e-3
        clock = "qpc"
        i_time = i_abs
        if i_time is None:
            clock = "relative"
            for name, sc in _TIME_REL:
                if name in idx:
                    i_time, scale = idx[name], sc
                    break
        if i_ft is None or i_time is None:
            raise ValueError("%s: no frame-time / time column (header: %s)" % (path, ",".join(header)[:200]))
        i_pid, i_chain = idx.get("processid"), idx.get("swapchainaddress")
        series: Dict[Tuple[str, str], PmSeries] = {}
        rows = skipped = 0
        width = len(header)
        for row in rd:
            if len(row) != width:           # the capture was killed in the middle of a row
                skipped += 1
                continue
            if pid is not None and i_pid is not None and row[i_pid] != str(pid):
                continue
            ft, t = _float_or_none(row[i_ft]), _float_or_none(row[i_time])
            if ft is None or t is None:     # NA
                skipped += 1
                continue
            key = (row[i_pid] if i_pid is not None else "", row[i_chain] if i_chain is not None else "")
            s = series.get(key)
            if s is None:
                s = series[key] = PmSeries(key)
            s.t.append(t * scale)
            s.ft.append(ft)
            s.gpu.append(_float_or_none(row[i_gpu]) if i_gpu is not None else None)
            rows += 1
    for s in series.values():               # the first row of a swap chain has no predecessor in the capture
        if s.t:
            del s.t[0], s.ft[0], s.gpu[0]
    return PmCsv(str(path), clock, series, rows, skipped)


def frame_stats(ft: Sequence[float], gpu: Sequence[Optional[float]]) -> Dict:
    n = len(ft)
    total = math.fsum(ft)
    s = sorted(ft)
    pairs = [(f, g) for f, g in zip(ft, gpu) if g is not None]
    busy = None
    if pairs:
        denom = math.fsum(f for f, _ in pairs)
        busy = 100.0 * math.fsum(g for _, g in pairs) / denom if denom > 0 else None
    return {
        "frames": n, "seconds": total / 1000.0, "avg_fps": n / (total / 1000.0),
        "p50_ms": percentile(s, 50), "p99_ms": percentile(s, 99),
        "pct_over_25ms": 100.0 * sum(1 for x in ft if x > 25.0) / n,
        "pct_over_50ms": 100.0 * sum(1 for x in ft if x > 50.0) / n,
        "max_ms": s[-1], "gpu_busy_pct": busy,
    }


# ================================================================================================================
# Analysis of one run (pure: files in, numbers out; used by the live path and by --dry-metrics)
# ================================================================================================================

def analyse_run(csv_path, log_path, *, skip: float = WINDOW_SKIP_S, window_s: Optional[float] = None,
                pm_meta: Optional[Dict] = None, pm_t0: Optional[float] = None, qpc_offset: Optional[float] = None,
                pid: Optional[int] = None, expected_s: Optional[float] = None) -> Dict:
    """The window metrics of one run. Raises SetupError when the CSV cannot be aligned (QPC CSV without an offset).
    The result has valid, reasons, stats (frame_stats or None), world_real, sim, wide, built, window, ft, gpu."""
    res: Dict = {"valid": True, "reasons": [], "stats": None, "world_real": None, "world_s": 0.0, "real_s": 0.0,
                 "sim_lines": 0, "sim_ft": None, "sim_on": False, "sim_cap": None, "wide": None, "built": None,
                 "ft": [], "gpu": [], "window": None, "pm_clock": None, "pm_rows": 0, "pm_skipped": 0, "fr_rel": None}
    log = scan_log(log_path)
    res["wide"], res["built"] = log["wide"], log["built"]
    res["sim_on"], res["sim_cap"] = log["sim_on_ts"] is not None, log["sim_on_cap"]
    base = log["first_ts"]
    if log["fr_ts"] is None:
        res["valid"] = False
        res["reasons"].append("the log never shows `-> free-roam` (the Start presses did not take: the numbers would be the "
                              "title or the menu)")
        return res
    fr = log["fr_ts"]
    res["fr_rel"] = None if base is None else fr - base
    w0 = fr + skip
    w1 = w0 + window_s if window_s else math.inf
    res["window"] = {"start_epoch": w0, "end_epoch": None if math.isinf(w1) else w1, "skip_s": skip, "fr_epoch": fr}


    try:
        pm = load_pm_csv(csv_path, pid)
    except (OSError, ValueError) as e:
        res["valid"] = False
        res["reasons"].append("PresentMon CSV unusable: %s" % e)
        return res
    res.update(pm_clock=pm.clock, pm_rows=pm.rows, pm_skipped=pm.skipped)
    if pm.clock == "qpc":
        if qpc_offset is None and pm_meta:
            qpc_offset = pm_meta.get("qpc_offset_s")
        if qpc_offset is None:
            raise SetupError("%s has absolute QPC times: give --pm-meta (pm_meta.json beside the CSV) or --qpc-offset" % csv_path)
        shift = float(qpc_offset)
    elif pm_t0 is not None:
        if base is None:
            raise SetupError("--pm-t0 needs a timestamp on the first log line")
        shift = base + pm_t0
    else:
        shift = w0       # the E2 harness started PresentMon at FR + 20 s: the CSV clock 0 is the window start
        res["pm_t0_assumed"] = True

    best_key, best_n = None, -1
    first_e = last_e = None
    for key, s in pm.series.items():
        if not s.t:
            continue
        a, b = s.t[0] + shift, s.t[-1] + shift
        first_e = a if first_e is None else min(first_e, a)
        last_e = b if last_e is None else max(last_e, b)
        n = sum(1 for t in s.t if w0 <= t + shift < w1)
        if n > best_n:
            best_key, best_n = key, n
    res["pm_range"] = None if first_e is None else (first_e, last_e)
    if best_key is None or best_n <= 0:
        res["valid"] = False
        cap = "no frames" if first_e is None else "frames from %s to %s after the first log line" % (rel(first_e, base), rel(last_e, base))
        res["reasons"].append("no PresentMon frames in the window (window starts %s after the first log line; capture: %s; "
                              "%d rows)" % (rel(w0, base), cap, pm.rows))
        return res
    s = pm.series[best_key]
    ft: List[float] = []
    gpu: List[Optional[float]] = []
    last_in = w0
    for t, f, g in zip(s.t, s.ft, s.gpu):
        if w0 <= t + shift < w1:
            ft.append(f)
            gpu.append(g)
            last_in = t + shift
    res["ft"], res["gpu"] = ft, gpu
    res["stats"] = frame_stats(ft, gpu)
    res["chain"] = best_key
    res["chains"] = len(pm.series)
    res["window"].update(captured_end=min(w1, last_in), log_first=base)
    # world/real over the part of the window that was captured (the log goes on a moment after the capture ended)
    wr, world_s, real_s, n_sim = world_real(log["sim"], w0, min(w1, last_in))
    res.update(world_real=wr, world_s=world_s, real_s=real_s, sim_lines=n_sim,
               sim_ft=sim_frame_times(log["sim_ft"], w0, min(w1, last_in)))
    cur = None
    for ts_, _, to in log["modes"]:
        if ts_ <= w0:
            cur = to
    inside = [(ts_, frm, to) for ts_, frm, to in log["modes"] if w0 < ts_ <= min(w1, last_in)]
    if cur not in (None, "free-roam") or inside:     # E6: a Start press that opened the pause menu measures something else
        changes = ", ".join("%s -> %s at +%.0f s" % (frm, to, ts_ - fr) for ts_, frm, to in inside[:3])
        res["mode_warning"] = "the window is not wholly in free roam (mode %s at its start%s)" % (
            cur or "?", "; changes: " + changes if changes else "")
    got = res["stats"]["seconds"]
    if expected_s is not None and got < MIN_WINDOW_FRACTION * expected_s:
        res["valid"] = False
        res["reasons"].append("the window holds %.0f s of frames, less than half of the expected %.0f s (the game ended or "
                              "PresentMon stopped early)" % (got, expected_s))
    return res


# ================================================================================================================
# Power state (notes of the METRICS row)
# ================================================================================================================

_SCHEMES = {"381b4222-f694-41f0-9685-ff5bb260df2e": "Balanced", "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c": "High performance",
            "a1841308-3541-4fab-bc81-f71556f20b4a": "Power saver", "e9a42b02-d5df-448d-aa00-03f14749eb61": "Ultimate performance"}
_OVERLAYS = {"00000000-0000-0000-0000-000000000000": "none", "ded574b5-45a0-4f42-8737-46345c09c238": "Best performance",
             "961cc777-2547-4f9d-8174-7d86181b8a7a": "Best power efficiency",
             "3af9b8d9-7c97-431d-ad78-34a8bfea439f": "Better battery"}


def power_state() -> Dict:
    """AC/battery, battery %, power scheme and power-mode overlay (Windows; fields are '?' when a call fails)."""
    out: Dict = {"source": "?", "battery": None, "scheme": "?", "overlay": "?"}
    if os.name != "nt":
        return out
    try:
        from ctypes import wintypes

        class SPS(ctypes.Structure):
            _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte),
                        ("BatteryLifePercent", ctypes.c_ubyte), ("SystemStatusFlag", ctypes.c_ubyte),
                        ("BatteryLifeTime", wintypes.DWORD), ("BatteryFullLifeTime", wintypes.DWORD)]

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16), ("Data3", ctypes.c_uint16),
                        ("Data4", ctypes.c_ubyte * 8)]

        def fmt(g: "GUID") -> str:
            return "%08x-%04x-%04x-%02x%02x-%s" % (g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], bytes(g.Data4[2:]).hex())

        sps = SPS()
        if ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(sps)):
            out["source"] = {0: "battery", 1: "AC"}.get(sps.ACLineStatus, "?")
            out["battery"] = None if sps.BatteryLifePercent == 255 else int(sps.BatteryLifePercent)
        powrprof = ctypes.WinDLL("powrprof")
        guid = GUID()
        if powrprof.PowerGetEffectiveOverlayScheme(ctypes.byref(guid)) == 0:
            out["overlay"] = _OVERLAYS.get(fmt(guid), fmt(guid))
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        powrprof.PowerGetActiveScheme.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(GUID))]
        ptr = ctypes.POINTER(GUID)()
        if powrprof.PowerGetActiveScheme(None, ctypes.byref(ptr)) == 0:
            out["scheme"] = _SCHEMES.get(fmt(ptr.contents), fmt(ptr.contents))
            kernel32.LocalFree(ptr)
    except (OSError, AttributeError, ValueError):
        pass
    return out


def power_note(before: Dict, after: Dict) -> str:
    src = before["source"] if before["source"] == after["source"] else "%s->%s" % (before["source"], after["source"])
    batt = "" if after["battery"] is None else ", battery %d %%" % after["battery"]
    return "power %s%s, %s scheme, power mode overlay %s%s" % (
        src, batt, after["scheme"], after["overlay"],
        "" if before["overlay"] == after["overlay"] else " (was %s)" % before["overlay"])


# ================================================================================================================
# PresentMon capture of a game PID, and the no-free-roam guard
# ================================================================================================================

def find_presentmon(arg: Optional[str], work: Path) -> Path:
    cands = []
    if arg:
        cands.append(Path(arg))
    if os.environ.get("MCLA_PRESENTMON"):
        cands.append(Path(os.environ["MCLA_PRESENTMON"]))
    cands.append(work / DEFAULT_PRESENTMON)
    pm_dir = work / "toolchain" / "presentmon"
    if pm_dir.is_dir():
        cands.extend(sorted(pm_dir.glob("PresentMon*.exe"), reverse=True))
    for c in cands:
        if c.is_file():
            return c
    raise SetupError("PresentMon not found (looked for %s); pass --presentmon PATH or set MCLA_PRESENTMON"
                     % ", ".join(run_game.posix(c) for c in cands[:3]))


def preflight_presentmon(exe: Path) -> str:
    """`PresentMon --help` prints the usage (UTF-16 or UTF-8, depending on the redirect) and exits 1 by design."""
    try:
        r = subprocess.run([str(exe), "--help"], capture_output=True, timeout=30, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SetupError("cannot run %s: %s" % (run_game.posix(exe), e))
    text = (r.stdout + r.stderr)
    for enc in ("utf-8", "utf-16-le"):
        m = re.search(r"PresentMon (\d+\.\d+\.\d+)", text.decode(enc, "replace").replace("\x00", ""))
        if m:
            return m.group(1)
    raise SetupError("%s does not print `PresentMon <version>` for --help" % run_game.posix(exe))


def read_text_any(path: Path) -> str:
    """A text file that may be UTF-16 with a BOM (PresentMon's redirected console output) or UTF-8."""
    raw = path.read_bytes()
    return raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", "replace")


def qpc_offset_now() -> float:
    """time.time() - time.perf_counter(): epoch of the QueryPerformanceCounter origin (median of 9 reads)."""
    return statistics.median(time.time() - time.perf_counter() for _ in range(9))


class PresentMon:
    """One capture of the game PID, started from run_game's on_start hook and finished after the run."""

    def __init__(self, exe: Path, seconds: float) -> None:
        self.exe = exe
        self.timed = max(1, int(seconds - PM_TIMED_MARGIN_S))
        self.proc: Optional[subprocess.Popen] = None
        self.log = None
        self.run_dir: Optional[Path] = None
        self.pid: Optional[int] = None
        self.session: Optional[str] = None
        self.meta: Dict = {}
        self.error: Optional[str] = None

    def start(self, pid: int, run_dir: Path) -> None:
        self.pid, self.run_dir = pid, run_dir
        self.session = "mcla_bench_%d" % pid
        cmd = [str(self.exe), "--process_id", str(pid), "--output_file", str(run_dir / "pm.csv"), "--qpc_time_ms",
               "--timed", str(self.timed), "--terminate_after_timed", "--terminate_on_proc_exit", "--no_console_stats",
               "--session_name", self.session, "--stop_existing_session"]
        self.meta = {"pid": pid, "session": self.session, "cmd": cmd, "timed_s": self.timed}
        try:
            self.log = open(run_dir / "pm.log", "wb")
            self.meta["qpc_offset_s"] = qpc_offset_now()
            self.meta["launched_epoch"] = time.time()
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
                                         creationflags=0x08000000)  # CREATE_NO_WINDOW
        except OSError as e:
            self.error = "could not start PresentMon: %s" % e
            say(self.error)

    def finish(self) -> None:
        """Wait for the capture to end by itself, else kill it and stop its session; write pm_meta.json."""
        killed = False
        rc = None
        if self.proc is not None:
            deadline = time.monotonic() + PM_FINISH_GRACE_S
            while self.proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            if self.proc.poll() is None:
                killed = True
                self.proc.kill()
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
                self._stop_session()
            rc = self.proc.poll()
        if self.log is not None:
            self.log.close()
        self.meta.update({"rc": rc, "killed": killed, "error": self.error})
        if self.run_dir is not None:
            run_game.write_json(self.run_dir / "pm_meta.json", self.meta)

    def _stop_session(self) -> None:
        try:
            subprocess.run([str(self.exe), "--session_name", self.session, "--terminate_existing_session"],
                           capture_output=True, timeout=30, stdin=subprocess.DEVNULL, creationflags=0x08000000)
        except (OSError, subprocess.TimeoutExpired):
            pass


_RE_FR_BYTES = re.compile(rb"\[mcla-mode\] [a-z][a-z-]* -> free-roam")


class FreeRoamGuard(threading.Thread):
    """E6: the Starts of a flow sometimes never take. When the log still has no `-> free-roam` line `guard_at` seconds after
    the launch, kill the game (the run is INVALID anyway) so the slot is free again."""

    def __init__(self, pid: int, log_path: Path, guard_at: float) -> None:
        super().__init__(daemon=True)
        self.pid, self.log_path, self.guard_at = pid, log_path, guard_at
        self.fired = False
        self._stop_event = threading.Event()

    def stop(self) -> None:
        self._stop_event.set()

    def run(self) -> None:
        t0 = time.monotonic()
        offset = 0
        tail = b""
        while not self._stop_event.wait(1.0):
            try:
                size = self.log_path.stat().st_size
                if size < offset:
                    offset, tail = 0, b""
                with open(self.log_path, "rb") as fh:
                    fh.seek(offset)
                    chunk = fh.read()
                offset += len(chunk)
            except OSError:
                chunk = b""
            buf = tail + chunk
            if _RE_FR_BYTES.search(buf):
                return
            tail = buf[-80:]
            if time.monotonic() - t0 >= self.guard_at:
                self.fired = True
                say("guard: no `-> free-roam` line %.0f s after the launch; killing pid %d (this run is INVALID)"
                    % (self.guard_at, self.pid))
                subprocess.run(["taskkill", "/PID", str(self.pid), "/T", "/F"], capture_output=True)
                return


# ================================================================================================================
# Telemetry: GPU clock, package power, temperature (tools/telemetry/sampler.exe, ADLX) and the machine's CPU load
# ================================================================================================================

def _adlx_val(cells: List[str], idx: Dict[str, int], name: str) -> Optional[float]:
    i = idx.get(name)
    v = _float_or_none(cells[i]) if i is not None else None
    return v if v is not None and v > 0 else None       # -1 = not supported


def load_adlx_csv(path) -> List[Tuple[float, Optional[float], Optional[float], Optional[float]]]:
    """(t_s, gfx_mhz, gpu_w, temp_c) rows of a sampler.exe CSV: `#` lines and a cut last row skipped, unsupported (-1) = None.
    Raises ValueError for a file without the sampler's header."""
    rows: List[Tuple[float, Optional[float], Optional[float], Optional[float]]] = []
    header: Optional[List[str]] = None
    idx: Dict[str, int] = {}
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            cells = line.split(",")
            if header is None:
                header = [c.strip().lower() for c in cells]
                idx = {name: i for i, name in enumerate(header)}
                if "t_s" not in idx or "gfx_mhz" not in idx:
                    raise ValueError("%s is not a sampler.exe CSV (header: %s)" % (path, line[:120]))
                continue
            if len(cells) != len(header):
                continue
            t = _float_or_none(cells[idx["t_s"]])
            if t is not None:
                rows.append((t, _adlx_val(cells, idx, "gfx_mhz"), _adlx_val(cells, idx, "gpu_w"), _adlx_val(cells, idx, "temp_c")))
    if header is None:
        raise ValueError("%s is empty" % path)
    return rows


def load_cpu_csv(path) -> List[Tuple[float, float, float]]:
    """(epoch_s, sys_cores, game_cores) rows of a bench.py cpu.csv."""
    rows: List[Tuple[float, float, float]] = []
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        rd = csv.reader(fh)
        header = next(rd, None)
        if not header or [h.strip().lower() for h in header[:3]] != ["epoch_s", "sys_cores", "game_cores"]:
            raise ValueError("%s is not a bench.py cpu.csv" % path)
        for row in rd:
            vals = [_float_or_none(x) for x in row[:3]]
            if len(row) >= 3 and all(v is not None for v in vals):
                rows.append((vals[0], vals[1], vals[2]))
    return rows


def telemetry_summary(adlx, adlx_shift: Optional[float], cpu, w0: float, w1: float) -> Optional[Dict]:
    """Medians over the window [w0, w1) of the sampler rows (epoch = t_s + adlx_shift) and the CPU rows; None without data files."""
    if adlx is None and cpu is None:
        return None
    out: Dict = {"taint": []}
    if adlx is not None:
        sel = [r for r in adlx if w0 <= r[0] + adlx_shift < w1]
        out["adlx_rows"] = len(sel)
        out["adlx_span_s"] = (max(r[0] for r in sel) - min(r[0] for r in sel)) if sel else 0.0
        clk = sorted(r[1] for r in sel if r[1] is not None)
        if clk:
            out["gpuclk_p50"], out["gpuclk_p10"] = statistics.median(clk), percentile(clk, 10)
        pw = [r[2] for r in sel if r[2] is not None]
        if pw:
            out["pkg_w"] = statistics.median(pw)
        tc = [r[3] for r in sel if r[3] is not None]
        if tc:
            out["temp_max"] = max(tc)
    if cpu is not None:
        sel2 = [(s, g) for e, s, g in cpu if w0 <= e < w1]
        out["cpu_rows"] = len(sel2)
        if sel2:
            out["nongame_cpu"] = statistics.median(max(0.0, s - g) for s, g in sel2)
            out["sys_cores"] = statistics.median(s for s, _ in sel2)
            out["game_cores"] = statistics.median(g for _, g in sel2)
    if out.get("nongame_cpu", 0.0) > TAINT_CPU_CORES:
        out["taint"].append("non-game CPU %.1f cores > %g" % (out["nongame_cpu"], TAINT_CPU_CORES))
    return out


def attach_telemetry(res: Dict, *, adlx_path=None, adlx_start_epoch: Optional[float] = None, adlx_t0: Optional[float] = None,
                     cpu_path=None) -> None:
    """Adds res["telemetry"] (summary over the captured window) to an analysis with a window. Telemetry is advisory: a missing or
    unreadable file only sets res["telemetry_error"], it never makes the run INVALID."""
    win = res.get("window") or {}
    w0, w1 = win.get("start_epoch"), win.get("captured_end")
    if w0 is None or w1 is None:
        return
    adlx = cpu = None
    shift: Optional[float] = None
    errors: List[str] = []
    if adlx_path is not None and Path(adlx_path).is_file():
        try:
            adlx = load_adlx_csv(adlx_path)
            if adlx_start_epoch is not None:
                shift = float(adlx_start_epoch)
            elif adlx_t0 is not None and win.get("log_first") is not None:
                shift = win["log_first"] + adlx_t0
            else:
                shift = w0       # nothing says when the sampler started: t_s 0 is assumed at the window start
                res["adlx_t0_assumed"] = True
        except (OSError, ValueError) as e:
            errors.append("sampler CSV unusable: %s" % e)
            adlx = None
    if cpu_path is not None and Path(cpu_path).is_file():
        try:
            cpu = load_cpu_csv(cpu_path)
        except (OSError, ValueError) as e:
            errors.append("cpu.csv unusable: %s" % e)
    tel = telemetry_summary(adlx, shift, cpu, w0, w1)
    if tel is not None:
        if adlx is not None and not tel.get("adlx_rows"):
            errors.append("no sampler rows in the window (check the sampler's start time)")
        if adlx is not None and tel.get("adlx_rows") and tel.get("adlx_span_s", 0.0) < 0.5 * (w1 - w0):
            errors.append("the sampler covers only %.0f s of the %.0f s window" % (tel["adlx_span_s"], w1 - w0))
        if cpu is not None and not tel.get("cpu_rows"):
            errors.append("no CPU-load rows in the window")
    res["telemetry"] = tel
    if errors:
        res["telemetry_error"] = "; ".join(errors)


def telemetry_token(tel: Optional[Dict]) -> str:
    """`gpuclk=<p50>/<p10>MHz pkg=<W>W nongame_cpu=<cores> tmax=<C>C`, the parts that were measured."""
    if not tel:
        return ""
    parts = []
    if tel.get("gpuclk_p50") is not None:
        parts.append("gpuclk=%.0f/%.0fMHz" % (tel["gpuclk_p50"], tel["gpuclk_p10"]))
    if tel.get("pkg_w") is not None:
        parts.append("pkg=%.1fW" % tel["pkg_w"])
    if tel.get("nongame_cpu") is not None:
        parts.append("nongame_cpu=%.1f" % tel["nongame_cpu"])
    if tel.get("temp_max") is not None:
        parts.append("tmax=%.0fC" % tel["temp_max"])
    return " ".join(parts)


def find_sampler(args: argparse.Namespace, work: Path) -> Optional[Path]:
    """sampler.exe: --sampler (must exist), else $WORK/toolchain/telemetry/sampler.exe if built, else None. None with --no-telemetry."""
    if args.no_telemetry:
        return None
    if args.sampler:
        p = Path(args.sampler)
        if not p.is_file():
            raise SetupError("sampler not found at %s" % p)
        return p
    p = work / SAMPLER_EXE
    return p if p.is_file() else None


class Sampler:
    """tools/telemetry/sampler.exe beside the game: ADLX GPU clock / power / temperature into <run>/adlx.csv."""

    def __init__(self, exe: Path, seconds: float, proc_name: str) -> None:
        self.exe, self.proc_name = exe, proc_name
        self.secs = max(1, int(seconds - SAMPLER_MARGIN_S))
        self.proc: Optional[subprocess.Popen] = None
        self.log = None
        self.run_dir: Optional[Path] = None
        self.meta: Dict = {}
        self.error: Optional[str] = None

    def start(self, run_dir: Path) -> None:
        self.run_dir = run_dir
        cmd = [str(self.exe), str(run_dir / "adlx.csv"), str(self.secs), "100", self.proc_name]
        self.meta = {"cmd": cmd, "launched_epoch": time.time()}
        try:
            self.log = open(run_dir / "sampler.log", "wb")
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
                                         creationflags=0x08000000)  # CREATE_NO_WINDOW
        except OSError as e:
            self.error = "could not start sampler.exe: %s" % e
            say(self.error)

    def finish(self) -> None:
        """Wait for sampler.exe to end by itself, else kill it; record sampler_meta.json (start_epoch = mtime - last t_s)."""
        killed = False
        rc = None
        if self.proc is not None:
            deadline = time.monotonic() + PM_FINISH_GRACE_S
            while self.proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            if self.proc.poll() is None:
                killed = True
                self.proc.kill()
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
            rc = self.proc.poll()
        if self.log is not None:
            self.log.close()
        if self.run_dir is None:
            return
        csv_path = self.run_dir / "adlx.csv"
        try:
            rows = load_adlx_csv(csv_path)
            if rows:
                self.meta["start_epoch"] = csv_path.stat().st_mtime - rows[-1][0]
        except (OSError, ValueError):
            pass
        self.meta.update({"rc": rc, "killed": killed, "error": self.error})
        run_game.write_json(self.run_dir / "sampler_meta.json", self.meta)


class CpuLoad(threading.Thread):
    """The machine's CPU load beside the game, every CPU_SAMPLE_S into <run>/cpu.csv (epoch_s,sys_cores,game_cores): busy cores of
    the whole system (GetSystemTimes) and of the game process (GetProcessTimes); their difference is the non-game load."""

    def __init__(self, pid: int, csv_path: Path) -> None:
        super().__init__(daemon=True)
        self.pid, self.csv_path = pid, csv_path
        self.error: Optional[str] = None
        self._stop_event = threading.Event()

    def stop(self) -> None:
        self._stop_event.set()
        self.join(timeout=5)

    def run(self) -> None:
        try:
            self._loop()
        except Exception as e:  # noqa: BLE001 - telemetry must never break a run
            self.error = repr(e)

    def _loop(self) -> None:
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32")
        pft = ctypes.POINTER(wintypes.FILETIME)
        k32.OpenProcess.restype = wintypes.HANDLE
        k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k32.GetProcessTimes.argtypes = [wintypes.HANDLE, pft, pft, pft, pft]
        k32.GetSystemTimes.argtypes = [pft, pft, pft]
        k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = k32.OpenProcess(0x1000, False, self.pid)       # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            self.error = "OpenProcess(%d) failed" % self.pid
            return
        ncpu = os.cpu_count() or 1

        def ticks(ft) -> int:
            return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

        def read():
            idle, kern, user, c, e, pk, pu = (wintypes.FILETIME() for _ in range(7))
            code = wintypes.DWORD()
            if not k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user)):
                return None
            if not k32.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value != 259:     # STILL_ACTIVE
                return None
            if not k32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(pk), ctypes.byref(pu)):
                return None
            return (time.perf_counter(), time.time(), ticks(kern) + ticks(user), ticks(kern) - ticks(idle) + ticks(user),
                    ticks(pk) + ticks(pu))

        try:
            prev = read()
            if prev is None:
                return
            with open(self.csv_path, "w", newline="") as fh:
                fh.write("epoch_s,sys_cores,game_cores\n")
                while not self._stop_event.wait(CPU_SAMPLE_S):
                    cur = read()
                    if cur is None:
                        break
                    dt, dsys = cur[0] - prev[0], cur[2] - prev[2]
                    if dt > 0 and dsys > 0:
                        fh.write("%.3f,%.3f,%.3f\n" % (cur[1], ncpu * (cur[3] - prev[3]) / dsys, (cur[4] - prev[4]) / 1e7 / dt))
                        fh.flush()
                    prev = cur
        finally:
            k32.CloseHandle(handle)


# ================================================================================================================
# METRICS.md
# ================================================================================================================

PLAN_COLUMNS = ["run id", "build id", "flow", "API", "internal resolution", "cap", "variant cvars", "avg fps", "p50 ms",
                "p99 ms", "% > 25 ms", "max ms", "world/real", "notes"]


def _norm_col(name: str) -> str:
    return re.sub(r"\s+", " ", name.replace("`", "")).strip().lower()


def _split_row(line: str) -> List[str]:
    cells = re.split(r"(?<!\\)\|", line.strip())
    return [c.strip() for c in cells[1:-1]]


def metrics_values(r: Dict) -> Dict[str, str]:
    """The cell text per normalized column name. `r` is a row dict made by make_row()."""
    return {
        "run id": r["run_id"], "build id": r["build_id"], "flow": r["flow"], "api": r["api"],
        "internal resolution": r["resolution"], "cap": r["cap"], "variant cvars": r["cvars"],
        "avg fps": r["avg_fps"], "p50 ms": r["p50_ms"], "p99 ms": r["p99_ms"], "% > 25 ms": r["pct25"],
        "% > 50 ms": r["pct50"], "max ms": r["max_ms"], "world/real": r["world_real"], "gpu busy %": r["gpu_busy"],
    }


def append_metrics(path: Path, rows: List[Dict]) -> None:
    """Append rows under `## Runs` of the METRICS file (created if missing), in the columns of the table found there."""
    text = path.read_text(encoding="utf-8", newline="") if path.exists() else ""
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()
    plan_block = ["| " + " | ".join(PLAN_COLUMNS) + " |", "|" + "---|" * len(PLAN_COLUMNS)]
    head = next((i for i, l in enumerate(lines) if re.fullmatch(r"##\s+Runs\s*", l)), None)
    if head is None:                      # no section yet: add it, with the table header, at the end
        while lines and not lines[-1].strip():
            lines.pop()
        if lines:
            lines.append("")
        lines += ["## Runs", ""] + plan_block
        header_cells, insert_at = list(PLAN_COLUMNS), len(lines)
    else:
        j = head + 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j < len(lines) and lines[j].lstrip().startswith("|"):
            header_cells = _split_row(lines[j])
            insert_at = j + 1
            while insert_at < len(lines) and lines[insert_at].lstrip().startswith("|"):
                insert_at += 1
        else:                             # the section has no table yet
            lines[head + 1:head + 1] = [""] + plan_block
            header_cells, insert_at = list(PLAN_COLUMNS), head + 1 + 1 + len(plan_block)
    cols = [_norm_col(c) for c in header_cells]
    new_lines = []
    for r in rows:
        values = metrics_values(r)
        notes = [r["notes"]] if r["notes"] else []
        for col, label in (("% > 50 ms", "> 50 ms %s %%"), ("gpu busy %", "GPU busy %s %%")):
            if col not in cols and values[col] not in ("", "n/a"):
                notes.append(label % values[col])
        values["notes"] = "; ".join(notes)
        new_lines.append("| " + " | ".join(md_cell(values.get(c, "")) for c in cols) + " |")
    lines[insert_at:insert_at] = new_lines
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".part")
    tmp.write_text(nl.join(lines) + nl, encoding="utf-8", newline="")
    os.replace(tmp, path)


def retag_metrics(path: Path, tags: Dict[str, str]) -> int:
    """Prefix `TAINTED (<text>)` to the notes of the rows with these run ids in the `## Runs` table; returns the rows changed."""
    if not tags or not path.exists():
        return 0
    text = path.read_text(encoding="utf-8", newline="")
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()
    head = next((i for i, l in enumerate(lines) if re.fullmatch(r"##\s+Runs\s*", l)), None)
    if head is None:
        return 0
    j = head + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j >= len(lines) or not lines[j].lstrip().startswith("|"):
        return 0
    cols = [_norm_col(c) for c in _split_row(lines[j])]
    if "notes" not in cols:
        return 0
    ni = cols.index("notes")
    changed = 0
    k = j + 1
    while k < len(lines) and lines[k].lstrip().startswith("|"):
        cells = _split_row(lines[k])
        if len(cells) == len(cols) and cells[0] in tags:
            tag = "TAINTED (%s)" % md_cell(tags[cells[0]])
            cells[ni] = tag + ("; " + cells[ni] if cells[ni] else "")
            lines[k] = "| " + " | ".join(cells) + " |"
            changed += 1
        k += 1
    if changed:
        tmp = path.with_name(path.name + ".part")
        tmp.write_text(nl.join(lines) + nl, encoding="utf-8", newline="")
        os.replace(tmp, path)
    return changed


def internal_resolution(cvars: Sequence[Tuple[str, str]], wide: Optional[Dict]) -> str:
    if wide:
        return "%dx%d" % wide["back"]
    d = dict(cvars)
    try:
        if "resolution_scale" in d:
            s = float(d["resolution_scale"])
            return "%dx%d (%gx)" % (round(1280 * s), round(720 * s), s)
        if int(d.get("mcla_widescreen_width", "0")) > 0 and int(d.get("mcla_scene_height", "0")) > 0:
            return "%dx%d" % (int(d["mcla_widescreen_width"]), int(d["mcla_scene_height"]))
    except ValueError:
        pass
    if re.fullmatch(r"\d+x\d+", d.get("resolution", "")):
        return d["resolution"]
    return "1280x720"


def cap_label(cvars: Sequence[Tuple[str, str]], sim_cap: Optional[int]) -> str:
    """The frame cap of the row: the first cap cvar with a value (0 = uncapped; mcla_sim_cap 0 = none known), else the cap
    the sim logged in its `on:` line, else `default` (the shipped 30)."""
    d = dict(cvars)
    for name in CAP_CVARS:
        if name not in d:
            continue
        try:
            v = float(d[name])
        except ValueError:
            return d[name]
        if v > 0:
            return "%g" % v
        if name != "mcla_sim_cap":
            return "uncapped"
    return "%d" % sim_cap if sim_cap else "default"


# ================================================================================================================
# Results: numbers, the `bench` line, METRICS rows, gates
# ================================================================================================================

@dataclass
class RunSpec:
    kind: str                       # warmup | drive | parked
    variant: Variant
    run_id: str
    flow: str
    seconds: float
    window_s: Optional[float]       # None = to the end of the capture
    rep: int = 0                    # 1-based round of a measured run


@dataclass
class Result:
    run_id: str
    kind: str                       # drive | parked | stable (pooled drive + parked)
    variant: Variant
    flow: str
    rep: int
    cvars: List[Tuple[str, str]]
    valid: bool
    reasons: List[str]
    analysis: Dict
    power: str = ""
    warmup: str = ""
    pm_note: str = ""


def metrics_of(analysis: Dict) -> Dict:
    """avg_fps ... world_real as floats (None where unknown) from an analysis dict with stats."""
    st = analysis.get("stats")
    out = {k: None for k in ("avg_fps", "p50_ms", "p99_ms", "pct_over_25ms", "pct_over_50ms", "max_ms", "gpu_busy_pct",
                             "frames", "seconds")}
    if st:
        out.update({k: st[k] for k in out})
    out["world_real"] = analysis.get("world_real")
    return out


def bench_line(res: Result) -> str:
    if not res.valid:
        return "bench %s %s: INVALID %s" % (res.run_id, res.variant.name, "; ".join(res.reasons))
    m = metrics_of(res.analysis)
    return ("bench %s %s: avg_fps=%.2f p50_ms=%.2f p99_ms=%.2f pct_over_25ms=%.2f pct_over_50ms=%.2f max_ms=%.1f world_real=%s"
            % (res.run_id, res.variant.name, m["avg_fps"], m["p50_ms"], m["p99_ms"], m["pct_over_25ms"], m["pct_over_50ms"],
               m["max_ms"], num(m["world_real"], 4)))


def detail_lines(res: Result) -> List[str]:
    a = res.analysis
    out: List[str] = []
    st = a.get("stats")
    if st:
        skip = (a.get("window") or {}).get("skip_s")
        out.append("  window: %d frames over %.1f s (free roam + %s s%s); gpu_busy=%s%%%s" % (
            st["frames"], st["seconds"], "%g" % skip if skip is not None else "?",
            "" if res.kind != "stable" else ", pooled drive + parked", num(st["gpu_busy_pct"], 1),
            "" if a.get("chains", 1) <= 1 else "; %d swap chains, the busiest used" % a["chains"]))
    if a.get("fr_rel") is not None:
        out.append("  free roam at log +%.1f s; PresentMon %s, %d rows (%d skipped)" % (
            a["fr_rel"], a.get("pm_clock") or "?", a.get("pm_rows", 0), a.get("pm_skipped", 0)))
    if a.get("mode_warning"):
        out.append("  WARNING: " + a["mode_warning"])
    if a.get("pm_t0_assumed"):
        out.append("  note: relative-time CSV and no --pm-t0: the capture is assumed to start at the window start")
    if st or a.get("sim_lines"):
        ft = a.get("sim_ft")
        out.append("  world/real: %s from %d [mcla-sim] intervals (world %.2f s / real %.2f s); sim %s%s" % (
            num(a.get("world_real"), 4), a.get("sim_lines", 0), a.get("world_s", 0.0), a.get("real_s", 0.0),
            "on" if a.get("sim_on") else "no `on:` line",
            "" if not ft else "; sim frame times p50 %.0f p99 %.0f worst %.1f ms (%d lines)" % (ft["p50"], ft["p99"],
                                                                                               ft["worst"], ft["lines"])))
    tel = a.get("telemetry")
    if tel:
        cpu_part = ""
        if tel.get("sys_cores") is not None:
            cpu_part = " (CPU: system %.1f cores, game %.1f; %d samples)" % (tel["sys_cores"], tel["game_cores"], tel.get("cpu_rows", 0))
        adlx_part = "; %d sampler rows" % tel["adlx_rows"] if "adlx_rows" in tel else ""
        out.append("  telemetry: %s%s%s" % (telemetry_token(tel) or "no samples", cpu_part, adlx_part))
        for t in tel.get("taint", []):
            out.append("  TAINTED: " + t)
    if a.get("adlx_t0_assumed"):
        out.append("  note: no sampler start time (--adlx-t0 / sampler_meta.json): the sampler CSV is assumed to start at the window start")
    if a.get("telemetry_error"):
        out.append("  telemetry: " + a["telemetry_error"])
    if res.pm_note:
        out.append("  presentmon: " + res.pm_note)
    return out


def format_cvars(cvars: Sequence[Tuple[str, str]]) -> str:
    return "`%s`" % " ".join("%s=%s" % kv for kv in cvars) if cvars else "(none)"


def make_row(res: Result, build_id: str, api: str, fixture: Optional[str], note: str) -> Dict:
    a = res.analysis
    st = a.get("stats") if res.valid else None
    m = metrics_of(a) if st else metrics_of({})
    notes: List[str] = []
    if not res.valid:
        notes.append("INVALID: " + "; ".join(res.reasons))
        part = metrics_of(a)
        if part["avg_fps"] is not None:
            notes.append("partial window, not valid: avg %.2f fps p99 %.2f ms max %.1f ms" % (part["avg_fps"], part["p99_ms"],
                                                                                         part["max_ms"]))
    else:
        notes.append("window %.0f s, %d frames" % (m["seconds"], m["frames"]) + (", pooled drive + parked" if res.kind == "stable" else ""))
        if a.get("sim_ft"):
            f = a["sim_ft"]
            notes.append("sim frame times p50 %.0f p99 %.0f worst %.1f ms" % (f["p50"], f["p99"], f["worst"]))
        if any(k == "mcla_sim60" and v.lower() in ("1", "true", "yes", "on") for k, v in res.cvars):
            notes.append("sim on" if a.get("sim_on") else "WARNING: mcla_sim60 set but no `[mcla-sim] on:` line")
        if a.get("mode_warning"):
            notes.append("WARNING: " + a["mode_warning"])
        if a.get("pm_t0_assumed"):
            notes.append("relative-time CSV: capture assumed to start at the window start")
    if res.power:
        notes.append(res.power)
    token = telemetry_token(a.get("telemetry"))
    if token:
        notes.append(token)
    if a.get("telemetry_error"):
        notes.append("telemetry: " + a["telemetry_error"])
    if res.warmup:
        notes.append(res.warmup)
    if res.rep:
        notes.append("round %d" % res.rep)
    if fixture:
        notes.append("fixture %s" % fixture)
    if res.pm_note:
        notes.append("PresentMon: " + res.pm_note)
    for taint in (a.get("telemetry") or {}).get("taint", []):
        notes.insert(0, "TAINTED (%s)" % taint)
    if note:
        notes.append(note)

    def f2(v: Optional[float], d: int = 2) -> str:
        return "n/a" if v is None else "%.*f" % (d, v)

    return {
        "run_id": res.run_id, "build_id": build_id, "flow": res.flow, "api": {"d3d12": "D3D12", "vulkan": "Vulkan"}.get(api, api),
        "resolution": internal_resolution(res.cvars, a.get("wide")), "cap": cap_label(res.cvars, a.get("sim_cap")),
        "cvars": format_cvars(res.cvars), "avg_fps": f2(m["avg_fps"], 2), "p50_ms": f2(m["p50_ms"]), "p99_ms": f2(m["p99_ms"]),
        "pct25": f2(m["pct_over_25ms"]), "pct50": f2(m["pct_over_50ms"]), "max_ms": f2(m["max_ms"], 1),
        "world_real": f2(m["world_real"], 4), "gpu_busy": f2(m["gpu_busy_pct"], 1), "notes": "; ".join(notes),
    }


def gate_check(gate: str, m: Dict) -> Tuple[List[str], List[str]]:
    """(failed checks, unchecked keys) of a metrics dict against GATES[gate]."""
    failed: List[str] = []
    unchecked: List[str] = []
    for item in GATES[gate]:
        key, test = item[0], item[1]
        v = m.get(key)
        if v is None:
            unchecked.append(key)
        elif test == "min" and v < item[2]:
            failed.append("%s=%.4g < %g" % (key, v, item[2]))
        elif test == "max" and v > item[2]:
            failed.append("%s=%.4g > %g" % (key, v, item[2]))
        elif test == "range" and not item[2] <= v <= item[3]:
            failed.append("%s=%.4g outside %g..%g" % (key, v, item[2], item[3]))
        elif test == "near" and abs(v - item[2]) > item[3]:
            failed.append("%s=%.4f not %g +- %g" % (key, v, item[2], item[3]))
    return failed, unchecked


def gate_line(gate: str, res: Result) -> Tuple[str, bool]:
    """The `gate` line and whether the result passes: a failed check or a metric that could not be checked is a FAIL."""
    failed, unchecked = gate_check(gate, metrics_of(res.analysis))
    ok = not failed and not unchecked
    text = "gate %s %s: %s" % (gate, res.run_id, "PASS" if ok else "FAIL")
    if failed:
        text += " (" + "; ".join(failed) + ")"
    if unchecked:
        text += " [unchecked: %s] (%s)" % (", ".join(unchecked), "needs a variant with mcla_sim60=true"
                                           if "world_real" in unchecked else "the metric is missing")
    return text, ok


def pooled(results: Sequence[Result], run_id: str, kind: str, flow: str, variant: Variant, rep: int) -> Result:
    """One Result from the frames of several valid results (concatenated windows; world/real from the summed intervals)."""
    ft: List[float] = []
    gpu: List[Optional[float]] = []
    world = real = 0.0
    sim_lines = 0
    for r in results:
        ft += r.analysis["ft"]
        gpu += r.analysis["gpu"]
        world += r.analysis["world_s"]
        real += r.analysis["real_s"]
        sim_lines += r.analysis["sim_lines"]
    first = results[0].analysis
    taints = list(dict.fromkeys(t for r in results for t in (r.analysis.get("telemetry") or {}).get("taint", [])))
    analysis = {"valid": True, "reasons": [], "stats": frame_stats(ft, gpu), "world_real": world / real if real > 0 else None,
                "world_s": world, "real_s": real, "sim_lines": sim_lines, "sim_ft": None, "sim_on": first.get("sim_on"),
                "sim_cap": first.get("sim_cap"), "wide": first.get("wide"), "built": first.get("built"), "ft": ft, "gpu": gpu,
                "window": first.get("window"), "pm_clock": first.get("pm_clock"), "pm_rows": 0, "pm_skipped": 0, "fr_rel": None}
    analysis["telemetry"] = {"taint": taints} if taints else None
    return Result(run_id, kind, variant, flow, rep, results[0].cvars, True, [], analysis, results[0].power, results[0].warmup)


def aggregate_line(variant: str, kind: str, results: Sequence[Result]) -> str:
    p = pooled(results, "agg", kind, "", results[0].variant, 0)
    m = metrics_of(p.analysis)
    per = [metrics_of(r.analysis) for r in results]
    return ("aggregate %s %s n=%d: avg_fps=%.2f p50_ms=%.2f p99_ms=%.2f pct_over_25ms=%.2f pct_over_50ms=%.2f max_ms=%.1f "
            "world_real=%s | per run avg_fps %s p99_ms %s"
            % (variant, kind, len(results), m["avg_fps"], m["p50_ms"], m["p99_ms"], m["pct_over_25ms"], m["pct_over_50ms"],
               m["max_ms"], num(m["world_real"], 4), "/".join("%.2f" % x["avg_fps"] for x in per),
               "/".join("%.2f" % x["p99_ms"] for x in per)))


def result_json(res: Result) -> Dict:
    a = res.analysis
    return {"run_id": res.run_id, "kind": res.kind, "variant": res.variant.name, "flow": res.flow, "round": res.rep,
            "cvars": ["%s=%s" % kv for kv in res.cvars], "valid": res.valid, "reasons": res.reasons,
            "metrics": metrics_of(a), "world_s": a.get("world_s"), "real_s": a.get("real_s"), "sim_lines": a.get("sim_lines"),
            "sim_frame_times": a.get("sim_ft"), "sim_on": a.get("sim_on"), "wide": a.get("wide"), "built": a.get("built"),
            "window": a.get("window"), "free_roam_log_s": a.get("fr_rel"), "pm_clock": a.get("pm_clock"),
            "pm_rows": a.get("pm_rows"), "power": res.power, "warmup": res.warmup, "telemetry": a.get("telemetry")}


# ================================================================================================================
# Running
# ================================================================================================================

@dataclass
class Ctx:
    args: argparse.Namespace
    work: Path
    pm_exe: Path
    common: List[Tuple[str, str]]
    fixture: Optional[str]
    bench_id: str
    holding_lock: bool
    metrics_file: Optional[Path]
    warmup_ids: List[str] = field(default_factory=list)
    sampler_exe: Optional[Path] = None


def run_one(ctx: Ctx, spec: RunSpec) -> Result:
    """One game process with a PresentMon capture; returns the analysed result (INVALID results included)."""
    args = ctx.args
    cvars = merged_cvars(ctx.common, spec.variant)
    # lock=False: bench.py holds the heavy lock for the whole bench (cmd_bench), or the user passed --no-lock.
    opts = run_game.RunOptions(
        flow=spec.flow, fixture=ctx.fixture, seconds=spec.seconds, api=args.api, cvars=["%s=%s" % kv for kv in cvars],
        run_id=spec.run_id, config=args.config, exe=args.exe, lock=False, expect="free-roam", work=str(ctx.work),
        game=args.game)
    pm = PresentMon(ctx.pm_exe, spec.seconds)
    exe_name = Path(args.exe).name if args.exe else "mcla.exe"
    sampler = Sampler(ctx.sampler_exe, spec.seconds, exe_name) if ctx.sampler_exe is not None else None
    holder: Dict[str, threading.Thread] = {}

    def on_start(pid: int, run_dir: Path) -> None:
        try:
            pm.start(pid, run_dir)
        except Exception as e:  # noqa: BLE001 - a broken capture must not kill the run before it is judged
            pm.error = "PresentMon start failed: %r" % (e,)
            say(pm.error)
        if args.guard_at > 0:
            g = FreeRoamGuard(pid, run_dir / "mcla.log", args.guard_at)
            g.start()
            holder["guard"] = g
        if not args.no_telemetry:
            if sampler is not None:
                try:
                    sampler.start(run_dir)
                except Exception as e:  # noqa: BLE001
                    sampler.error = "sampler.exe start failed: %r" % (e,)
                    say(sampler.error)
            load = CpuLoad(pid, run_dir / "cpu.csv")
            load.start()
            holder["cpu"] = load

    power0 = power_state()
    try:
        summary = run_game.run(opts, on_start=on_start)
    finally:
        guard = holder.get("guard")
        if guard is not None:
            guard.stop()
        load = holder.get("cpu")
        if load is not None:
            load.stop()
        pm.finish()
        if sampler is not None:
            sampler.finish()
    power1 = power_state()
    if summary.get("ended_by") == "interrupted":
        raise KeyboardInterrupt()

    run_dir = ctx.work / "runs" / spec.run_id
    t_fr = summary.get("t_free_roam")
    if spec.window_s:
        expected: Optional[float] = spec.window_s
    elif t_fr is not None:
        expected = max(1.0, spec.seconds - PM_TIMED_MARGIN_S - t_fr - args.window_skip)
    else:
        expected = None
    reasons: List[str] = []
    try:
        analysis = analyse_run(run_dir / "pm.csv", run_dir / "mcla.log", skip=args.window_skip, window_s=spec.window_s,
                               pm_meta=pm.meta, pid=summary.get("pid"), expected_s=expected)
    except SetupError as e:
        analysis = {"valid": False, "reasons": [str(e)], "stats": None, "ft": [], "gpu": []}
    if analysis.get("window"):
        attach_telemetry(analysis, adlx_path=run_dir / "adlx.csv" if sampler is not None else None,
                         adlx_start_epoch=sampler.meta.get("start_epoch") if sampler is not None else None,
                         cpu_path=run_dir / "cpu.csv" if holder.get("cpu") is not None else None)
        if sampler is not None and sampler.error and not analysis.get("telemetry_error"):
            analysis["telemetry_error"] = sampler.error
    guard = holder.get("guard")
    if guard is not None and guard.fired:
        reasons.append("the guard killed the game: no `-> free-roam` line %.0f s after the launch" % args.guard_at)
    if pm.error:
        reasons.append(pm.error)
    reasons += analysis["reasons"]
    if summary.get("ended_by") == "exited":
        if not (guard is not None and guard.fired):
            reasons.append("the game exited by itself after %.0f s (exit code %s)" % (summary.get("elapsed_s", 0), summary.get("exit_code")))
    if summary.get("fatal_log"):
        reasons.append("%d fatal line(s): %s" % (summary["fatal_log"], (summary.get("fatal_samples") or ["?"])[0][:160]))
    if summary.get("unregistered") and not summary.get("fatal_log"):
        reasons.append("%d unregistered-function line(s)" % summary["unregistered"])
    analysis["reasons"] = reasons
    pm_note = "killed after the run" if pm.meta.get("killed") else ""
    pm_log = run_dir / "pm.log"
    if reasons and pm_log.is_file():       # PresentMon's own words next to a run that has no usable capture
        words = " ".join(read_text_any(pm_log).split())
        if words:
            pm_note = (pm_note + "; " if pm_note else "") + "pm.log: " + words[-200:]
    warmup = ("warm-up run %s dropped" % ", ".join(ctx.warmup_ids)) if ctx.warmup_ids else ""
    res = Result(spec.run_id, spec.kind, spec.variant, spec.flow, spec.rep, cvars, not reasons, reasons, analysis,
                 power_note(power0, power1), warmup, pm_note)
    return res


def report(ctx: Ctx, res: Result, rows: List[Dict]) -> None:
    """Print the bench line (+ details, gate) of a measured result and queue its METRICS row."""
    print(bench_line(res), flush=True)
    for line in detail_lines(res):
        print(line, flush=True)
    if res.valid and is_gated(ctx.args, res):
        print(gate_line(ctx.args.gate, res)[0], flush=True)
    a = res.analysis
    build_id = ctx.args.build_id or a.get("built") or "?"
    row = make_row(res, build_id, ctx.args.api, ctx.fixture, ctx.args.note or "")
    rows.append(row)
    if ctx.metrics_file is not None:
        append_metrics(ctx.metrics_file, [row])
        say("row appended to %s" % run_game.posix(ctx.metrics_file))
    run_dir = ctx.work / "runs" / res.run_id
    if res.kind != "stable" and run_dir.is_dir():
        run_game.write_json(run_dir / "metrics.json", result_json(res))


def build_plan(ctx: Ctx, variants: List[Variant], drive_seconds: float) -> List[RunSpec]:
    args = ctx.args
    plan: List[RunSpec] = []
    warm = [] if args.no_warmup else (variants if args.warmup_each else variants[:1])
    for v in warm:
        rid = "%s-warmup" % ctx.bench_id + ("-%s" % v.name if len(warm) > 1 else "")
        plan.append(RunSpec("warmup", v, rid, args.flow, args.warmup_seconds or drive_seconds, None))
    for r in range(1, args.repeat + 1):
        order = variants[(r - 1) % len(variants):] + variants[:(r - 1) % len(variants)]
        for v in order:
            rid = "%s-%s" % (ctx.bench_id, v.name) + ("-%d" % r if args.repeat > 1 else "")
            plan.append(RunSpec("drive", v, rid, args.flow, drive_seconds, args.window_seconds, r))
            if args.parked:
                plan.append(RunSpec("parked", v, rid + "-parked", args.parked_flow, args.parked_run_seconds, args.parked_seconds, r))
    return plan


def run_all(ctx: Ctx, plan: List[RunSpec]) -> Tuple[List[Result], List[Dict]]:
    results: List[Result] = []
    rows: List[Dict] = []
    last_drive: Dict[Tuple[str, int], Result] = {}
    for i, spec in enumerate(plan, 1):
        say("run %d/%d: %s (%s, variant %s, %g s)" % (i, len(plan), spec.run_id, spec.flow, spec.variant.name, spec.seconds))
        res = run_one(ctx, spec)
        if spec.kind == "warmup":
            say("warm-up run %s dropped (%s)" % (spec.run_id, "ok" if res.valid else "INVALID: " + "; ".join(res.reasons)))
            ctx.warmup_ids.append(spec.run_id)      # the notes of the measured runs name it
            continue
        results.append(res)
        report(ctx, res, rows)
        if spec.kind == "drive":
            last_drive[(spec.variant.name, spec.rep)] = res
        else:
            drive = last_drive.get((spec.variant.name, spec.rep))
            if drive is not None and drive.valid and res.valid:
                both = pooled([drive, res], drive.run_id + "-stable", "stable", "%s+%s" % (drive.flow, res.flow), spec.variant,
                              spec.rep)
                results.append(both)
                report(ctx, both, rows)
            else:
                say("no pooled stable row for %s: the drive or the parked run is INVALID" % spec.variant.name)
    return results, rows


def cross_variant_taint(results: List[Result]) -> Dict[str, str]:
    """TAINTED across variants: for each kind of run (drive, parked), when the variants' median GPU clock (the median of their
    runs' p50) differs by more than TAINT_CLOCK_FRAC of the lower one, every run of that kind with telemetry is tainted, and so is
    a pooled `-stable` result whose drive or parked run is. Adds the text to the results' telemetry; returns {run id: text}."""
    tags: Dict[str, str] = {}
    for kind in ("drive", "parked"):
        per: Dict[str, List[float]] = {}
        for r in results:
            tel = r.analysis.get("telemetry") if r.valid else None
            if r.kind == kind and tel and tel.get("gpuclk_p50"):
                per.setdefault(r.variant.name, []).append(tel["gpuclk_p50"])
        if len(per) < 2:
            continue
        med = {v: statistics.median(x) for v, x in per.items()}
        lo, hi = min(med.values()), max(med.values())
        if (hi - lo) / lo <= TAINT_CLOCK_FRAC:
            continue
        text = "median GPU clock differs %.0f %% between variants (%s)" % (
            100.0 * (hi - lo) / lo, ", ".join("%s %.0f MHz" % kv for kv in sorted(med.items())))
        for r in results:
            if r.kind == kind and r.valid and r.analysis.get("telemetry"):
                tags[r.run_id] = text
    for r in results:
        if r.kind == "stable":
            drive_id = r.run_id[:-len("-stable")]
            text = tags.get(drive_id) or tags.get(drive_id + "-parked")
            if text:
                tags[r.run_id] = text
    for r in results:
        if r.run_id in tags:
            tel = r.analysis.get("telemetry") or {"taint": []}
            tel.setdefault("taint", []).append(tags[r.run_id])
            r.analysis["telemetry"] = tel
    return tags


def apply_taint(ctx: Ctx, results: List[Result], rows: List[Dict], tags: Dict[str, str]) -> None:
    """The rows are already appended: flag them in the METRICS file, in `rows` and in the runs' metrics.json."""
    if ctx.metrics_file is not None:
        n = retag_metrics(ctx.metrics_file, tags)
        say("TAINTED: %d METRICS row(s) flagged in %s" % (n, run_game.posix(ctx.metrics_file)))
    for row in rows:
        if row["run_id"] in tags:
            row["notes"] = "TAINTED (%s); %s" % (tags[row["run_id"]], row["notes"])
    for r in results:
        run_dir = ctx.work / "runs" / r.run_id
        if r.run_id in tags and r.kind != "stable" and run_dir.is_dir():
            run_game.write_json(run_dir / "metrics.json", result_json(r))


def is_gated(args: argparse.Namespace, res: Result) -> bool:
    """--gate judges the drive runs, or with --parked the pooled drive + parked result (PLAN 6's definition)."""
    return bool(args.gate) and res.kind == ("stable" if args.parked else "drive")


def summarize(ctx: Ctx, results: List[Result]) -> int:
    """Aggregates (--repeat > 1), the gate verdicts, and the exit code."""
    groups: Dict[Tuple[str, str], List[Result]] = {}
    for r in results:
        if r.valid:
            groups.setdefault((r.variant.name, r.kind), []).append(r)
    for (variant, kind), rs in groups.items():
        if len(rs) > 1:
            print(aggregate_line(variant, kind, rs), flush=True)
    invalid = [r for r in results if not r.valid]
    gate_failed = False
    for r in results:
        if r.valid and is_gated(ctx.args, r) and not gate_line(ctx.args.gate, r)[1]:
            gate_failed = True
    say("%d measured result(s), %d INVALID%s" % (len(results), len(invalid), ", gate %s FAILED" % ctx.args.gate if gate_failed else ""))
    for r in invalid:
        print("INVALID %s: %s" % (r.run_id, "; ".join(r.reasons)), flush=True)
    for r in results:
        for t in (r.analysis.get("telemetry") or {}).get("taint", []):
            print("TAINTED %s: %s" % (r.run_id, t), flush=True)
    return EXIT_INVALID if invalid else (EXIT_GATE if gate_failed else 0)


# ================================================================================================================
# Command line
# ================================================================================================================

def make_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="bench.py", formatter_class=argparse.RawDescriptionHelpFormatter,
        description="MCLA bench: variants back to back, PresentMon on the game PID, METRICS rows. Details: the docstring at "
                    "the top of this file.",
        epilog="Prints `bench <id> <variant>: avg_fps=<x> p50_ms=<x> p99_ms=<x> pct_over_25ms=<x> pct_over_50ms=<x> "
               "max_ms=<x> world_real=<x>` per measured run. Exit 0 ok; 2 setup problem; 3 INVALID run; 4 gate failed; "
               "75 lock timeout; 130 interrupted.")
    ap.add_argument("--flow", metavar="NAME", help="drive flow (default %s)" % DEFAULT_FLOW)
    ap.add_argument("--fixture", metavar="NAME", help="fixture for run_game (default: the flow's `# fixture:` header)")
    ap.add_argument("--seconds", type=float, metavar="N", help="run time of a drive/warm-up run (default: the flow's header, "
                                                              "else %g)" % DEFAULT_SECONDS)
    ap.add_argument("--variant", action="append", default=[], metavar="NAME:CVAR=V,...",
                    help="a variant; repeatable (default `base:`). Run back to back, each in its own process")
    ap.add_argument("--api", choices=("d3d12", "vulkan"), help="GPU backend (default d3d12)")
    ap.add_argument("--repeat", type=int, default=1, metavar="K", help="measured runs per variant (default 1); the variant "
                                                                         "order rotates each round; K > 1 prints aggregates")
    ap.add_argument("--no-warmup", action="store_true", help="no warm-up run: the first run is measured as it is")
    ap.add_argument("--warmup-each", action="store_true", help="a warm-up run for every variant, not just the first")
    ap.add_argument("--warmup-seconds", type=float, metavar="S", help="length of the warm-up run (default: --seconds)")
    ap.add_argument("--parked", action="store_true",
                    help="after each drive run, a parked run (flow %s) and a pooled `-stable` row (PLAN 6: 300 s drive + 60 s "
                         "parked)" % PARKED_FLOW)
    ap.add_argument("--parked-flow", default=PARKED_FLOW, metavar="NAME")
    ap.add_argument("--parked-run-seconds", type=float, default=PARKED_RUN_SECONDS, metavar="S",
                    help="run time of a parked run (default %g)" % PARKED_RUN_SECONDS)
    ap.add_argument("--parked-seconds", type=float, default=PARKED_WINDOW_S, metavar="S",
                    help="window length of a parked run (default %g)" % PARKED_WINDOW_S)
    ap.add_argument("--window-skip", type=float, default=WINDOW_SKIP_S, metavar="S",
                    help="the window starts this long after `-> free-roam` (default %g)" % WINDOW_SKIP_S)
    ap.add_argument("--window-seconds", type=float, metavar="S", help="drive window length (default: to the end)")
    ap.add_argument("--guard-at", type=float, default=GUARD_AT_S, metavar="S",
                    help="kill a run that has no `-> free-roam` line S s after its launch (default %g; 0 = off)" % GUARD_AT_S)
    ap.add_argument("--cvar", action="append", default=[], metavar="K=V", help="a cvar for every variant; repeatable")
    ap.add_argument("--id", metavar="ID", help="bench id, the prefix of the run ids (default bench-<yyyymmdd-HHMMSS>)")
    ap.add_argument("--build-id", metavar="ID", help="METRICS build id (default: the log's build stamp, else ?)")
    ap.add_argument("--note", metavar="TEXT", help="extra METRICS notes")
    ap.add_argument("--gate", choices=sorted(GATES),
                    help="judge the drive runs (with --parked: the pooled drive + parked rows) against PLAN 6: m4 = locked 30, "
                         "m5 = stable 60 (also M6); exit 4 on a FAIL")
    ap.add_argument("--metrics-file", metavar="F", help="METRICS file to append to (default docs/METRICS.md; env MCLA_METRICS_FILE)")
    ap.add_argument("--no-metrics", action="store_true", help="do not append rows")
    ap.add_argument("--config", choices=("RelWithDebInfo", "Release"), default="Release",
                    help="build config of $WORK/build/bin/<config>/mcla.exe (default Release: benches use Release, PLAN 3)")
    ap.add_argument("--exe", metavar="PATH", help="game executable (default $WORK/build/bin/<config>/mcla.exe)")
    ap.add_argument("--work", metavar="W", help="$WORK (default env MCLA_WORK, else %s)" % run_game.DEFAULT_WORK)
    ap.add_argument("--game", metavar="G", help="$GAME (default env MCLA_GAME, else %s)" % run_game.DEFAULT_GAME)
    ap.add_argument("--presentmon", metavar="EXE", help="PresentMon (default env MCLA_PRESENTMON, else $WORK/%s)" % DEFAULT_PRESENTMON)
    ap.add_argument("--no-lock", action="store_true", help="do not take the heavy lock")
    ap.add_argument("--lock-timeout", type=float, metavar="S", help="give up (exit 75) if the lock is not obtained in S s")
    ap.add_argument("--plan", action="store_true", help="print the run sequence and stop")
    ap.add_argument("--dry-metrics", nargs=2, metavar=("PM_CSV", "LOG"),
                    help="recompute the metrics of an existing capture and log (no game, no lock)")
    ap.add_argument("--append", action="store_true", help="--dry-metrics: append the row to the METRICS file")
    ap.add_argument("--pm-meta", metavar="F", help="--dry-metrics: pm_meta.json (default: beside the CSV)")
    ap.add_argument("--qpc-offset", type=float, metavar="S", help="--dry-metrics: time.time() - perf_counter() of the capture")
    ap.add_argument("--pm-t0", type=float, metavar="S",
                    help="--dry-metrics, relative-time CSV: seconds after the first log line at which the CSV clock is 0")
    ap.add_argument("--pid", type=int, metavar="N", help="--dry-metrics: only the rows of this process id")
    ap.add_argument("--no-telemetry", action="store_true", help="no sampler.exe and no CPU-load sampling (GPU clock, power, non-game CPU)")
    ap.add_argument("--sampler", metavar="EXE", help="sampler.exe (default: $WORK/%s when built; tools/telemetry/README.md)" % SAMPLER_EXE)
    ap.add_argument("--adlx", metavar="F", help="--dry-metrics: sampler CSV (default: adlx.csv beside the PresentMon CSV)")
    ap.add_argument("--adlx-meta", metavar="F", help="--dry-metrics: sampler_meta.json (start_epoch; default: beside the CSV)")
    ap.add_argument("--adlx-t0", type=float, metavar="S",
                    help="--dry-metrics: seconds after the first log line at which the sampler's t_s is 0 (default: its meta, else the window start)")
    ap.add_argument("--cpu-csv", metavar="F", help="--dry-metrics: cpu.csv (default: beside the PresentMon CSV)")
    return ap


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    ap = make_parser()
    args = ap.parse_args(argv)
    if args.repeat < 1:
        ap.error("--repeat must be at least 1")
    if args.window_skip < 0:
        ap.error("--window-skip must not be negative")
    if args.dry_metrics and (args.plan or args.parked):
        ap.error("--dry-metrics runs nothing: no --plan / --parked")
    return args


def metrics_path(args: argparse.Namespace) -> Path:
    return Path(args.metrics_file or os.environ.get("MCLA_METRICS_FILE") or DEFAULT_METRICS)


def cmd_dry(args: argparse.Namespace) -> int:
    csv_path, log_path = Path(args.dry_metrics[0]), Path(args.dry_metrics[1])
    if not csv_path.is_file():
        raise SetupError("no CSV at %s" % csv_path)
    if not run_game.log_files(log_path):
        raise SetupError("no log at %s" % log_path)
    if len(args.variant) > 1:
        raise SetupError("--dry-metrics takes at most one --variant (it only labels the row)")
    variant = parse_variant(args.variant[0]) if args.variant else Variant("dry")
    cvars = merged_cvars([parse_cvar(c) for c in args.cvar], variant)
    meta_path = Path(args.pm_meta) if args.pm_meta else csv_path.with_name("pm_meta.json")
    meta: Optional[Dict] = None
    if meta_path.is_file():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise SetupError("cannot read %s: %s" % (meta_path, e))
    elif args.pm_meta:
        raise SetupError("no pm_meta.json at %s" % meta_path)
    analysis = analyse_run(csv_path, log_path, skip=args.window_skip, window_s=args.window_seconds, pm_meta=meta,
                           pm_t0=args.pm_t0, qpc_offset=args.qpc_offset, pid=args.pid)
    adlx = Path(args.adlx) if args.adlx else csv_path.with_name("adlx.csv")
    if args.adlx and not adlx.is_file():
        raise SetupError("no sampler CSV at %s" % adlx)
    cpu = Path(args.cpu_csv) if args.cpu_csv else csv_path.with_name("cpu.csv")
    if args.cpu_csv and not cpu.is_file():
        raise SetupError("no CPU-load CSV at %s" % cpu)
    smeta = Path(args.adlx_meta) if args.adlx_meta else csv_path.with_name("sampler_meta.json")
    if args.adlx_meta and not smeta.is_file():
        raise SetupError("no sampler meta at %s" % smeta)
    start_epoch = None
    if args.adlx_t0 is None and smeta.is_file():
        try:
            start_epoch = json.loads(smeta.read_text(encoding="utf-8")).get("start_epoch")
        except (OSError, ValueError) as e:
            raise SetupError("cannot read %s: %s" % (smeta, e))
    attach_telemetry(analysis, adlx_path=adlx if adlx.is_file() else None, adlx_start_epoch=start_epoch, adlx_t0=args.adlx_t0,
                     cpu_path=cpu if cpu.is_file() else None)
    facts = run_game.evaluate(run_game.parse_log(log_path), "free-roam")
    reasons = list(analysis["reasons"])
    if facts["fatal_log"]:
        reasons.append("%d fatal line(s) in the log" % facts["fatal_log"])
    if facts["unregistered"] and not facts["fatal_log"]:
        reasons.append("%d unregistered-function line(s) in the log" % facts["unregistered"])
    analysis["reasons"] = reasons
    run_id = args.id or (csv_path.parent.name if csv_path.parent.name not in ("", ".") else csv_path.stem)
    flow = args.flow or "dry-metrics"
    res = Result(run_id, "drive", variant, flow, 0, cvars, not reasons, reasons, analysis, "", "")
    print(bench_line(res), flush=True)
    for line in detail_lines(res):
        print(line, flush=True)
    code = 0 if res.valid else EXIT_INVALID
    if res.valid and args.gate:
        text, ok = gate_line(args.gate, res)
        print(text, flush=True)
        code = 0 if ok else EXIT_GATE
    if args.append:
        row = make_row(res, args.build_id or analysis.get("built") or "?", args.api or "?", args.fixture, args.note or "")
        target = metrics_path(args)
        append_metrics(target, [row])
        say("row appended to %s" % run_game.posix(target))
    return code


def cmd_bench(args: argparse.Namespace) -> int:
    work = Path(args.work or os.environ.get("MCLA_WORK") or run_game.DEFAULT_WORK)
    args.api = args.api or "d3d12"
    args.flow = args.flow or DEFAULT_FLOW
    variants = [parse_variant(v) for v in (args.variant or ["base:"])]
    names = [v.name for v in variants]
    if len(set(names)) != len(names):
        raise SetupError("duplicate variant names: %s" % ", ".join(names))
    common = [parse_cvar(c) for c in args.cvar]
    flow_file = run_game.resolve_flow(args.flow)          # raises run_game.Fail
    header = run_game.flow_header(flow_file.read_text(encoding="utf-8"))
    drive_seconds = float(args.seconds if args.seconds is not None else header.get("seconds", DEFAULT_SECONDS))
    if drive_seconds <= 0:
        raise SetupError("--seconds must be positive")
    fixture = args.fixture or header.get("fixture")
    if args.parked:
        run_game.resolve_flow(args.parked_flow)
    bench_id = args.id or "bench-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    if not NAME_RE.fullmatch(bench_id):
        raise SetupError("--id may only hold letters, digits, '.', '_' and '-'")
    metrics_file = None if args.no_metrics else metrics_path(args)
    pm_exe = find_presentmon(args.presentmon, work)
    ctx = Ctx(args, work, pm_exe, common, fixture, bench_id, not args.no_lock, metrics_file)
    ctx.sampler_exe = find_sampler(args, work)
    plan = build_plan(ctx, variants, drive_seconds)
    taken = [s.run_id for s in plan if (work / "runs" / s.run_id).exists()]
    if taken:
        raise SetupError("run dir(s) exist already: %s (pick another --id)" % ", ".join(taken))
    print("plan %s: %d run(s), %d measured; api %s, fixture %s, window = free roam + %g s to %s" % (
        bench_id, len(plan), sum(1 for s in plan if s.kind != "warmup"), args.api, fixture or "none", args.window_skip,
        "the end" if not args.window_seconds else "+%g s" % args.window_seconds), flush=True)
    print("telemetry: %s" % ("off (--no-telemetry)" if args.no_telemetry else (
        "sampler %s + CPU load" % run_game.posix(ctx.sampler_exe) if ctx.sampler_exe is not None else
        "CPU load only (no sampler.exe at %s: build tools/telemetry)" % run_game.posix(work / SAMPLER_EXE))), flush=True)
    for i, s in enumerate(plan, 1):
        print("  %d. %-6s %-24s %-14s %5g s  variant %s %s" % (i, s.kind, s.run_id, s.flow, s.seconds, s.variant.name,
                                                           format_cvars(merged_cvars(common, s.variant))), flush=True)
    if args.plan:
        return 0
    say("PresentMon %s at %s" % (preflight_presentmon(pm_exe), run_game.posix(pm_exe)))
    run_game._install_signals()
    lease = heavy_lock.hold("bench %s %s" % (args.flow, ",".join(names)), cmd=[os.path.basename(sys.argv[0])] + sys.argv[1:],
                            wait_timeout=args.lock_timeout, work=str(work)) if ctx.holding_lock else None
    started = time.monotonic()
    try:
        results, rows = run_all(ctx, plan)
    finally:
        if lease is not None:
            lease.release()
    tags = cross_variant_taint(results)
    if tags:
        apply_taint(ctx, results, rows, tags)
    code = summarize(ctx, results)
    out = work / "runs" / (bench_id + ".bench.json")
    try:
        run_game.write_json(out, {"bench_id": bench_id, "argv": sys.argv[1:], "elapsed_s": round(time.monotonic() - started, 1),
                                  "exit": code, "runs": [result_json(r) for r in results], "rows": rows})
        say("summary %s" % run_game.posix(out))
    except OSError as e:
        say("could not write %s: %s" % (out, e))
    return code


def main(argv: Optional[Sequence[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    args = parse_args(argv)
    try:
        return cmd_dry(args) if args.dry_metrics else cmd_bench(args)
    except (SetupError, run_game.Fail) as e:
        print("bench: %s" % e, file=sys.stderr)
        return EXIT_SETUP
    except heavy_lock.LockTimeout as e:
        print("bench: %s" % e, file=sys.stderr)
        return EXIT_LOCK_TIMEOUT
    except KeyboardInterrupt:
        print("bench: interrupted", file=sys.stderr)
        return EXIT_INTERRUPTED


if __name__ == "__main__":
    sys.exit(main())
