#!/usr/bin/env python3
"""hdr_ab.py - GPU cost of the HDR settings from an in-run A/B run (mcla_hdr_ab), phase by phase.

    python tools/hdr/hdr_ab.py RUN_DIR [--skip-after-switch 1.0] [--from-free-roam S] [--csv out.csv]
                               [--per-occurrence] [--qpc-offset S] [--pid N]
    python tools/hdr/hdr_ab.py --selftest

The run. The game ran with `--mcla_hdr_ab="<label>:<cvar>=<value>,...;<label>:..."` (mcla/hooks/mcla_hdr.cpp: a thread that
cycles the phases on a clock and sets the cvars while the game runs), under tools/bench.py or any harness that leaves in
RUN_DIR
    mcla.log           (and the rotated mcla.N.log, older parts first)
    pm.csv             PresentMon 2.x, started with --qpc_time_ms (CPUStartQPCTimeInMs on the QueryPerformanceCounter clock)
    pm_meta.json       qpc_offset_s: time.time() - perf_counter() of the capture (bench.py writes it before PresentMon starts)

Method. The log stamps are local wall-clock (spdlog, ms): the `[mcla-hdr] ab phase <label> (<i>/<n>) at <t> s: ...` lines
give the switches, the first `[mcla-mode] <from> -> free-roam` line gives free roam. A frame's epoch is
CPUStartQPCTimeInMs / 1000 + qpc_offset_s (the mapping of tools/bench.py, analyse_run); it belongs to the phase of the last
switch at or before it. Dropped: frames before the first switch; frames after the last phase (the last switch plus the median
spacing of the switches: the game ends within a phase of its last log line); frames less than --skip-after-switch seconds after a switch
(default 1.0: the settings land within a frame or two, and the frames in flight still carry the old ones); with
--from-free-roam S, frames before free roam + S seconds (without the option nothing is cut at free roam; give the switcher a
mcla_hdr_ab_start_s that is past it). As bench.py does, the first row of each swap chain is dropped (no predecessor) and the
swap chain with the most frames at or after the first switch is used. Phases with the same label are pooled over all their
occurrences, which is the point of cycling them: slow drift (clocks, temperature, the scene) falls on all labels alike.

Report, per label: frames, seconds (sum of MsBetweenPresents), MsGPUBusy median / mean / p90, MsGPUTime median / mean when the
CSV has the column, MsBetweenPresents median / p99, % of frames over 17.5 ms and over 25 ms; then the GPU-busy median and mean of
each label minus the first label's (first in the log), in ms and in % of the first label's. --per-occurrence adds one line per
phase occurrence (to see drift). --csv writes the kept frames: epoch, phase, gpu_busy, between (ms; empty = NA), then gpu_time
and occurrence (1-based, per label).

Exit 0; 1 when no frame is left to report; 2 for input that is missing or unusable (no `ab phase` line, no QPC column, no offset).
Only the standard library is used.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import datetime
import io
import json
import math
import re
import statistics
import sys
import tempfile
from contextlib import redirect_stdout
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SKIP_AFTER_SWITCH_S = 1.0
OVER_MS = (17.5, 25.0)           # the frame-time thresholds of the report
LABEL_DRIFT_WARN_S = 0.25        # the log clock vs the process clock between switches


class SetupError(Exception):
    """Input that is missing or unusable (exit 2)."""


# ================================================================================================================
# The log
# ================================================================================================================

_RE_PHASE = re.compile(r"\[mcla-hdr\] ab phase (?P<label>\S+) \((?P<i>\d+)/(?P<n>\d+)\) at (?P<t>-?[\d.]+) s:\s*(?P<text>.*?)\s*$")
_RE_ORIGINAL = re.compile(r"\[mcla-hdr\] ab: (?P<name>\w+)=(?P<value>.*?) before the switcher")
_RE_MODE = re.compile(r"\[mcla-mode\] (?P<frm>[a-z][a-z-]*) -> (?P<to>[a-z][a-z-]*)")


def parse_ts(head: str) -> Optional[float]:
    """Epoch seconds of a `[YYYY-MM-DD HH:MM:SS.mmm]` prefix (local time, as spdlog writes it), else None."""
    if len(head) < 25 or head[0] != "[" or head[24] != "]" or head[5] != "-" or head[8] != "-" \
            or head[11] != " " or head[14] != ":" or head[17] != ":" or head[20] != ".":
        return None
    try:
        return datetime.datetime(int(head[1:5]), int(head[6:8]), int(head[9:11]), int(head[12:14]),
                                 int(head[15:17]), int(head[18:20]), int(head[21:24]) * 1000).timestamp()
    except ValueError:
        return None


def log_files(path) -> List[Path]:
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


@dataclass
class Switch:
    epoch: float          # the log line's stamp
    label: str
    index: int            # phase i (1-based) of n
    count: int
    t: float              # the process clock the switcher printed
    text: str
    occurrence: int = 0   # 1-based, per label


@dataclass
class LogFacts:
    switches: List[Switch] = field(default_factory=list)
    free_roam: Optional[float] = None
    first_ts: Optional[float] = None
    originals: List[Tuple[str, str]] = field(default_factory=list)


def scan_log(path) -> LogFacts:
    facts = LogFacts()
    files = log_files(path)
    if not files:
        raise SetupError("no log at %s" % path)
    seen: Dict[str, int] = {}
    for f in files:
        with open(f, "rb") as fh:
            for raw in fh:
                if facts.first_ts is None:
                    facts.first_ts = parse_ts(raw[:25].decode("ascii", "replace"))
                if b"[mcla-" not in raw:
                    continue
                line = raw.decode("utf-8", "replace")
                ts = parse_ts(line[:25])
                if ts is None:
                    continue
                if "[mcla-hdr] ab" in line:
                    m = _RE_PHASE.search(line)
                    if m:
                        label = m.group("label")
                        seen[label] = seen.get(label, 0) + 1
                        facts.switches.append(Switch(ts, label, int(m.group("i")), int(m.group("n")), float(m.group("t")),
                                                     m.group("text"), seen[label]))
                        continue
                    m = _RE_ORIGINAL.search(line)
                    if m and not facts.switches:
                        facts.originals.append((m.group("name"), m.group("value")))
                elif "[mcla-mode]" in line and facts.free_roam is None:
                    m = _RE_MODE.search(line)
                    if m and m.group("to") == "free-roam":
                        facts.free_roam = ts
    facts.switches.sort(key=lambda s: s.epoch)
    return facts


# ================================================================================================================
# The PresentMon CSV
# ================================================================================================================

@dataclass
class Frame:
    epoch: float
    between: float                 # MsBetweenPresents
    busy: Optional[float]          # MsGPUBusy
    gpu: Optional[float]           # MsGPUTime


@dataclass
class PmCsv:
    series: Dict[Tuple[str, str], List[Tuple[float, float, Optional[float], Optional[float]]]]   # t_ms, between, busy, gpu
    rows: int
    skipped: int
    has_gpu_time: bool


def _float_or_none(s: str) -> Optional[float]:
    try:
        v = float(s)
    except ValueError:
        return None          # NA
    return v if math.isfinite(v) else None


def _first_col(idx: Dict[str, int], names: Sequence[str]) -> Optional[int]:
    for n in names:
        if n in idx:
            return idx[n]
    return None


def load_pm_csv(path, pid: Optional[int] = None) -> PmCsv:
    """A PresentMon CSV with CPUStartQPCTimeInMs (BOM and a cut last row tolerated). The first row of every swap chain is dropped."""
    with open(path, "r", encoding="utf-8-sig", newline="") as fh:
        rd = csv.reader(fh)
        header = next(rd, None)
        if not header:
            raise SetupError("%s is empty" % path)
        idx = {h.strip().lower(): i for i, h in enumerate(header)}
        i_t = idx.get("cpustartqpctimeinms")
        i_ft = _first_col(idx, ("msbetweenpresents", "frametime"))
        i_busy = _first_col(idx, ("msgpubusy", "gpubusy"))
        i_gpu = idx.get("msgputime")
        if i_t is None:
            raise SetupError("%s has no CPUStartQPCTimeInMs column: the capture needs PresentMon --qpc_time_ms (tools/bench.py does)" % path)
        if i_ft is None:
            raise SetupError("%s has no MsBetweenPresents column" % path)
        if i_busy is None:
            raise SetupError("%s has no MsGPUBusy column" % path)
        i_pid, i_chain = idx.get("processid"), idx.get("swapchainaddress")
        series: Dict[Tuple[str, str], List] = {}
        rows = skipped = 0
        width = len(header)
        any_gpu = False
        for row in rd:
            if len(row) != width:           # the capture was killed in the middle of a row
                skipped += 1
                continue
            if pid is not None and i_pid is not None and row[i_pid] != str(pid):
                continue
            t, ft = _float_or_none(row[i_t]), _float_or_none(row[i_ft])
            if t is None or ft is None:
                skipped += 1
                continue
            busy = _float_or_none(row[i_busy])
            gpu = _float_or_none(row[i_gpu]) if i_gpu is not None else None
            any_gpu = any_gpu or gpu is not None
            key = (row[i_pid] if i_pid is not None else "", row[i_chain] if i_chain is not None else "")
            series.setdefault(key, []).append((t, ft, busy, gpu))
            rows += 1
    for s in series.values():                # the first row of a swap chain has no predecessor in the capture
        if s:
            del s[0]
    return PmCsv(series, rows, skipped, any_gpu)


def to_frames(rows, offset_s: float) -> List[Frame]:
    """Frame epoch = CPUStartQPCTimeInMs / 1000 + qpc_offset_s (bench.py: analyse_run's `shift`)."""
    frames = [Frame(t / 1000.0 + offset_s, ft, busy, gpu) for t, ft, busy, gpu in rows]
    frames.sort(key=lambda f: f.epoch)
    return frames


# ================================================================================================================
# Phases and statistics
# ================================================================================================================

@dataclass
class Kept:
    frame: Frame
    label: str
    occurrence: int


def phases_end(switches: Sequence[Switch]) -> float:
    """When the last phase is taken to end: its switch plus the median spacing of the switches (infinity for a single switch)."""
    if len(switches) < 2:
        return math.inf
    return switches[-1].epoch + statistics.median(b.epoch - a.epoch for a, b in zip(switches, switches[1:]))


def assign(frames: Sequence[Frame], switches: Sequence[Switch], skip_s: float, from_epoch: Optional[float]) -> Tuple[List[Kept], Dict[str, int]]:
    """Each frame to the phase active at its CPU start. Returns the kept frames and the counts of what was dropped."""
    starts = [s.epoch for s in switches]
    end = phases_end(switches)
    dropped = {"before_first_switch": 0, "before_free_roam": 0, "after_switch": 0, "after_last_phase": 0}
    kept: List[Kept] = []
    for fr in frames:
        k = bisect.bisect_right(starts, fr.epoch) - 1
        if k < 0:
            dropped["before_first_switch"] += 1
        elif fr.epoch >= end:
            dropped["after_last_phase"] += 1
        elif from_epoch is not None and fr.epoch < from_epoch:
            dropped["before_free_roam"] += 1
        elif fr.epoch - starts[k] < skip_s:
            dropped["after_switch"] += 1
        else:
            kept.append(Kept(fr, switches[k].label, switches[k].occurrence))
    return kept, dropped


def percentile(sorted_vals: Sequence[float], p: float) -> float:
    """Linear interpolation between order statistics (bench.py's percentile())."""
    k = (len(sorted_vals) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def _med_mean_p90(values: List[float]) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    if not values:
        return None, None, None
    s = sorted(values)
    return statistics.median(s), math.fsum(s) / len(s), percentile(s, 90)


def summarize(kept: Sequence[Kept]) -> Dict:
    ft = [k.frame.between for k in kept]
    busy = [k.frame.busy for k in kept if k.frame.busy is not None]
    gpu = [k.frame.gpu for k in kept if k.frame.gpu is not None]
    out: Dict = {"frames": len(kept), "seconds": math.fsum(ft) / 1000.0}
    out["busy_n"] = len(busy)
    out["busy_med"], out["busy_mean"], out["busy_p90"] = _med_mean_p90(busy)
    out["gpu_med"], out["gpu_mean"], _ = _med_mean_p90(gpu)
    if ft:
        s = sorted(ft)
        out["between_med"], out["between_p99"] = statistics.median(s), percentile(s, 99)
        for thr in OVER_MS:
            out["over_%g" % thr] = 100.0 * sum(1 for x in ft if x > thr) / len(ft)
    else:
        out["between_med"] = out["between_p99"] = None
        for thr in OVER_MS:
            out["over_%g" % thr] = None
    return out


def by_label(kept: Sequence[Kept], order: Sequence[str]) -> Dict[str, Dict]:
    groups: Dict[str, List[Kept]] = {label: [] for label in order}
    for k in kept:
        groups[k.label].append(k)
    return {label: summarize(groups[label]) for label in order}


def label_order(switches: Sequence[Switch]) -> List[str]:
    order: List[str] = []
    for s in switches:
        if s.label not in order:
            order.append(s.label)
    return order


# ================================================================================================================
# Output
# ================================================================================================================

def _f(v: Optional[float], digits: int = 2) -> str:
    return "-" if v is None else "%.*f" % (digits, v)


def _signed(v: Optional[float], digits: int = 2) -> str:
    return "-" if v is None else "%+.*f" % (digits, v)


def print_table(rows: List[List[str]], header: List[str], left: int = 1) -> None:
    """Columns right-aligned except the first `left`."""
    widths = [max(len(header[c]), *(len(r[c]) for r in rows)) if rows else len(header[c]) for c in range(len(header))]

    def line(cells: Sequence[str]) -> str:
        return "  ".join(c.ljust(widths[i]) if i < left else c.rjust(widths[i]) for i, c in enumerate(cells))

    print(line(header))
    for r in rows:
        print(line(r))


def report(facts: LogFacts, kept: List[Kept], dropped: Dict[str, int], total_frames: int, chain: Tuple[str, str],
           has_gpu_time: bool, skip_s: float, from_free_roam: Optional[float], per_occurrence: bool) -> Dict[str, Dict]:
    order = label_order(facts.switches)
    base = facts.first_ts
    sw = facts.switches
    print("switches: %d (%s), first at %s, last at %s; free roam %s" % (
        len(sw), " ".join(order), _rel(sw[0].epoch, base), _rel(sw[-1].epoch, base),
        "at " + _rel(facts.free_roam, base) if facts.free_roam is not None else "not in the log"))
    print("swap chain %s: %d frames from the first switch on; kept %d (dropped: %d before free roam, %d within %g s after a switch, "
          "%d after the last phase)" % (chain[1] or "?", total_frames, len(kept), dropped["before_free_roam"], dropped["after_switch"],
                                        skip_s, dropped["after_last_phase"]))
    if facts.originals:
        print("originals: " + ", ".join("%s=%s" % nv for nv in facts.originals))
    drift = max((abs((s.epoch - sw[0].epoch) - (s.t - sw[0].t)) for s in sw), default=0.0)
    if drift > LABEL_DRIFT_WARN_S:
        print("WARNING: the log's clock and the switcher's process clock disagree by up to %.2f s between switches (a log stamp from "
              "another process, or the machine's clock was stepped)" % drift)
    if facts.free_roam is not None and from_free_roam is None and sw[0].epoch < facts.free_roam:
        early = sum(1 for s in sw if s.epoch < facts.free_roam)
        print("note: %d switch(es) happen before free roam; their frames are menu / loading frames (--from-free-roam 0 cuts them)" % early)
    print()

    stats = by_label(kept, order)
    header = ["phase", "frames", "sec", "busy_med", "busy_mean", "busy_p90"]
    if has_gpu_time:
        header += ["gpu_med", "gpu_mean"]
    header += ["btw_med", "btw_p99", ">%g%%" % OVER_MS[0], ">%g%%" % OVER_MS[1]]
    rows = []
    for label in order:
        s = stats[label]
        r = [label, str(s["frames"]), _f(s["seconds"], 1), _f(s["busy_med"]), _f(s["busy_mean"]), _f(s["busy_p90"])]
        if has_gpu_time:
            r += [_f(s["gpu_med"]), _f(s["gpu_mean"])]
        r += [_f(s["between_med"]), _f(s["between_p99"]), _f(s["over_%g" % OVER_MS[0]], 1), _f(s["over_%g" % OVER_MS[1]], 1)]
        rows.append(r)
    print("per phase label, pooled over its occurrences (ms; busy = MsGPUBusy%s, btw = MsBetweenPresents; >x%% = share of frames over x ms):" %
          (", gpu = MsGPUTime" if has_gpu_time else ""))
    print_table(rows, header)
    print()

    ref = order[0]
    r0 = stats[ref]
    rows = []
    for label in order[1:]:
        s = stats[label]
        cells = [label]
        for key in ("busy_med", "busy_mean"):
            if s[key] is None or r0[key] is None:
                cells += ["-", "-"]
            else:
                d = s[key] - r0[key]
                cells += [_signed(d), "-" if r0[key] == 0 else _signed(100.0 * d / r0[key], 1) + "%"]
        rows.append(cells)
    if rows:
        print("GPU busy against the first label (%s): median %s ms, mean %s ms" % (ref, _f(r0["busy_med"]), _f(r0["busy_mean"])))
        print_table(rows, ["phase", "d_med_ms", "d_med_%", "d_mean_ms", "d_mean_%"])
    for label in order:
        if stats[label]["frames"] < 100:
            print("WARNING: only %d frame(s) in phase '%s': too few for a median" % (stats[label]["frames"], label))

    if per_occurrence:
        print()
        occ: Dict[Tuple[str, int], List[Kept]] = {}
        for k in kept:
            occ.setdefault((k.label, k.occurrence), []).append(k)
        rows = []
        for s in sw:
            g = occ.get((s.label, s.occurrence), [])
            summary = summarize(g)
            rows.append([s.label, str(s.occurrence), _rel(s.epoch, base), str(summary["frames"]), _f(summary["busy_med"]),
                         _f(summary["busy_mean"]), _f(summary["between_med"])])
        print("per occurrence:")
        print_table(rows, ["phase", "occ", "at", "frames", "busy_med", "busy_mean", "btw_med"], left=2)
    return stats


def _rel(epoch: Optional[float], base: Optional[float]) -> str:
    return "?" if epoch is None or base is None else "+%.1f s" % (epoch - base)


def write_csv(path, kept: Sequence[Kept]) -> None:
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["epoch", "phase", "gpu_busy", "between", "gpu_time", "occurrence"])
        for k in kept:
            fr = k.frame
            w.writerow(["%.4f" % fr.epoch, k.label, "" if fr.busy is None else repr(fr.busy), repr(fr.between),
                        "" if fr.gpu is None else repr(fr.gpu), k.occurrence])


# ================================================================================================================
# One run
# ================================================================================================================

def analyse(run_dir, skip_s: float = SKIP_AFTER_SWITCH_S, from_free_roam: Optional[float] = None, csv_path=None,
            per_occurrence: bool = False, qpc_offset: Optional[float] = None, pid: Optional[int] = None) -> Optional[Dict[str, Dict]]:
    """Prints the report of the run in `run_dir`; returns the per-label statistics, or None when no frame is left."""
    run = Path(run_dir)
    if not run.is_dir():
        raise SetupError("%s is not a directory" % run)
    facts = scan_log(run / "mcla.log")
    if not facts.switches:
        raise SetupError("%s has no `[mcla-hdr] ab phase` line (was the game started with --mcla_hdr_ab, and did it get that far?)" %
                         (run / "mcla.log"))
    if from_free_roam is not None and facts.free_roam is None:
        raise SetupError("--from-free-roam: the log has no `[mcla-mode] ... -> free-roam` line")
    pm_path = run / "pm.csv"
    if not pm_path.is_file():
        raise SetupError("no PresentMon CSV at %s" % pm_path)
    pm = load_pm_csv(pm_path, pid)
    if qpc_offset is None:
        meta_path = run / "pm_meta.json"
        if not meta_path.is_file():
            raise SetupError("no pm_meta.json beside the CSV (and no --qpc-offset): the QPC times cannot be put on the log's clock")
        try:
            qpc_offset = float(json.loads(meta_path.read_text(encoding="utf-8"))["qpc_offset_s"])
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise SetupError("%s has no usable qpc_offset_s (%s)" % (meta_path, e))
    first_switch = facts.switches[0].epoch
    best_key, best_frames = None, []
    for key, rows in pm.series.items():
        frames = [f for f in to_frames(rows, qpc_offset) if f.epoch >= first_switch]
        if best_key is None or len(frames) > len(best_frames):
            best_key, best_frames = key, frames
    if best_key is None or not best_frames:
        capture = ""
        if pm.series:
            lo = min(r[0][0] for r in pm.series.values() if r) / 1000.0 + qpc_offset
            hi = max(r[-1][0] for r in pm.series.values() if r) / 1000.0 + qpc_offset
            capture = "; the capture covers %s to %s, the first switch is at %s" % (
                _rel(lo, facts.first_ts), _rel(hi, facts.first_ts), _rel(first_switch, facts.first_ts))
        print("no PresentMon frame at or after the first switch (%d rows)%s" % (pm.rows, capture))
        return None
    from_epoch = None if from_free_roam is None else facts.free_roam + from_free_roam
    kept, dropped = assign(best_frames, facts.switches, skip_s, from_epoch)
    if not kept:
        print("every frame was dropped (%s)" % dropped)
        return None
    stats = report(facts, kept, dropped, len(best_frames), best_key, pm.has_gpu_time, skip_s, from_free_roam, per_occurrence)
    if csv_path:
        write_csv(csv_path, kept)
        print("\nwrote %d frame rows to %s" % (len(kept), csv_path))
    return stats


# ================================================================================================================
# Self test: a synthetic run
# ================================================================================================================

def _stamp(epoch: float) -> str:
    d = datetime.datetime.fromtimestamp(round(epoch, 3))
    return d.strftime("[%Y-%m-%d %H:%M:%S.") + "%03d]" % (d.microsecond // 1000)


def _synth_run(root: Path) -> Dict:
    """A run of 3 labels cycled 4 times (period 4 s from T0 + 2 s, free roam at T0 + 11 s), 60 fps frames on two swap chains, the
    log split over mcla.1.log and mcla.log. Returns what the analysis should find, computed by plain loops."""
    t0 = datetime.datetime(2026, 11, 25, 12, 0, 0).timestamp()
    offset = 1795421594.8849978
    labels = ["sdr", "frame", "full"]
    busy_of = {"sdr": 5.0, "frame": 6.0, "full": 7.5}
    period, start, cycles, fr = 4.0, 2.0, 4, 11.0
    switches = [(round(t0 + start + k * period, 3), labels[k % 3], k // 3 + 1 + 0, k % 3 + 1, start + k * period) for k in range(3 * cycles)]
    end = t0 + start + 3 * cycles * period + 2.0

    lines: List[str] = []
    lines.append("%s [info] [core] [t1] [mcla] mcla_x86 started" % _stamp(t0))
    lines.append("%s [info] [core] [t1] [mcla-hdr] ab: hdr=off before the switcher" % _stamp(t0 + 1.0))
    lines.append("%s [info] [core] [t1] [mcla-hdr] ab: hdr_ui_layer=true before the switcher" % _stamp(t0 + 1.0))
    lines.append("%s [info] [core] [t2] [mcla-mode] title -> free-roam" % _stamp(t0 + fr))
    lines.append("not a stamped line: [mcla-hdr] ab phase bogus (1/1) at 0.000 s: x=1")
    entries = []
    for epoch, label, occ, idx, t in switches:
        text = {"sdr": "hdr=off", "frame": "hdr=on,hdr_ui_layer=false", "full": "hdr=on,hdr_ui_layer=true"}[label]
        entries.append((epoch, "%s [info] [core] [t9] [mcla-hdr] ab phase %s (%d/3) at %.3f s: %s" % (_stamp(epoch), label, idx, t, text)))
    for epoch, line in entries:
        lines.append(line)
    stamped = sorted(lines[:3] + [l for _, l in entries] + lines[3:4], key=lambda l: l[:25])      # time order
    stamped.insert(3, lines[4])                                                                     # the unstamped line
    half = len(stamped) // 2
    (root / "mcla.1.log").write_text("\n".join(stamped[:half]) + "\n", encoding="utf-8")            # the older part
    (root / "mcla.log").write_text("\n".join(stamped[half:]) + "\n", encoding="utf-8")

    header = ("Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,TimeInSeconds,"
              "MsBetweenSimulationStart,MsBetweenPresents,MsBetweenDisplayChange,MsInPresentAPI,MsRenderPresentLatency,MsUntilDisplayed,"
              "CPUStartQPCTimeInMs,MsBetweenAppStart,MsCPUBusy,MsCPUWait,MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait,MsAnimationError,"
              "AnimationTime,MsFlipDelay,MsAllInputToPhotonLatency,MsClickToPhotonLatency")
    out = ["\ufeff" + header]
    frames = []          # (epoch as the analysis decodes it, between, busy, gpu)
    n = int((end - (t0 + 1.0)) * 60)
    for j in range(n):
        epoch = t0 + 1.0003 + j / 60.0
        k = max((i for i, s in enumerate(switches) if s[0] <= epoch), default=-1)
        busy = 4.0 if k < 0 else busy_of[switches[k][1]] + ((j % 5) - 2) * 0.1
        if k >= 0 and epoch - switches[k][0] < 0.5:
            busy = 99.0                                       # the frames every skip used below must remove
        between = 30.0 if j % 100 == 99 else 16.6667
        gpu = busy + 0.5
        ms = (epoch - offset) * 1000.0
        cols = ["mcla.exe", "100", "0xAAA", "DXGI", "0", "516", "0", "Composed: Flip", "%.4f" % (j / 60.0), "NA", "%.4f" % between, "NA", "0.1", "0.9",
                "NA", "%.4f" % ms, "1.0", "1.0", "0.1", "5.0", "%.4f" % gpu, "%.4f" % busy if j % 50 else "NA", "0.0", "NA", "NA", "NA", "NA", "NA"]
        out.append(",".join(cols))
        frames.append((float("%.4f" % ms) / 1000.0 + offset, between, None if j % 50 == 0 else float("%.4f" % busy), float("%.4f" % gpu)))
    for j in range(40):                                       # a second swap chain with fewer frames
        ms = (t0 + 20.0 + j / 30.0 - offset) * 1000.0
        out.append(",".join(["mcla.exe", "100", "0xBBB", "DXGI", "0", "0", "0", "x", "0", "NA", "33.3", "NA", "0", "0", "NA", "%.4f" % ms,
                             "1", "1", "0", "1", "1.0", "1.0", "0", "NA", "NA", "NA", "NA", "NA"]))
    out.append("mcla.exe,100,0xAAA,DXGI,0,516")           # the capture was cut inside a row
    (root / "pm.csv").write_text("\n".join(out) + "\n", encoding="utf-8")
    (root / "pm_meta.json").write_text(json.dumps({"pid": 100, "qpc_offset_s": offset, "rc": 0}), encoding="utf-8")
    return {"t0": t0, "fr": t0 + fr, "switches": switches, "frames": frames, "labels": labels, "period": period}


def _expect(synth: Dict, skip: float, cut: Optional[float]) -> Dict[str, Dict]:
    """The same numbers by plain loops over the synthetic frames."""
    sw = synth["switches"]
    per: Dict[str, Dict[str, List[float]]] = {l: {"busy": [], "between": []} for l in synth["labels"]}
    first = sw[0][0]
    for epoch, between, busy, gpu in synth["frames"]:
        if epoch < first or epoch >= sw[-1][0] + synth["period"]:      # the last phase lasts one period; the game runs 2 s longer here
            continue
        k = max(i for i, s in enumerate(sw) if s[0] <= epoch)
        if epoch - sw[k][0] < skip or (cut is not None and epoch < synth["fr"] + cut):
            continue
        per[sw[k][1]]["between"].append(between)
        if busy is not None:
            per[sw[k][1]]["busy"].append(busy)
    return per


def selftest() -> int:
    def check(cond: bool, what: str) -> None:
        if not cond:
            raise AssertionError("selftest: " + what)

    with tempfile.TemporaryDirectory(prefix="hdr_ab_") as tmp:
        root = Path(tmp)
        synth = _synth_run(root)
        for skip, cut in ((1.0, None), (1.0, 0.0), (0.5, 2.0)):
            buf = io.StringIO()
            csv_path = root / "frames.csv"
            with redirect_stdout(buf):
                stats = analyse(root, skip_s=skip, from_free_roam=cut, csv_path=csv_path, per_occurrence=True)
            text = buf.getvalue()
            check(stats is not None, "no result")
            want = _expect(synth, skip, cut)
            for label in synth["labels"]:
                check(stats[label]["frames"] == len(want[label]["between"]), "frames of %s: %s vs %d" % (label, stats[label]["frames"], len(want[label]["between"])))
                check(abs(stats[label]["busy_med"] - statistics.median(want[label]["busy"])) < 1e-9, "busy median of %s" % label)
                check(abs(stats[label]["busy_mean"] - statistics.mean(want[label]["busy"])) < 1e-9, "busy mean of %s" % label)
                check(abs(stats[label]["busy_p90"] - percentile(sorted(want[label]["busy"]), 90)) < 1e-9, "busy p90 of %s" % label)
                over = 100.0 * sum(1 for b in want[label]["between"] if b > 25.0) / len(want[label]["between"])
                check(abs(stats[label]["over_25"] - over) < 1e-9, "share over 25 ms of %s" % label)
                check(abs(stats[label]["over_17.5"] - over) < 1e-9, "share over 17.5 ms of %s" % label)
                check(stats[label]["between_med"] == 16.6667, "between median of %s" % label)
                check(stats[label]["busy_p90"] < 50.0, "a skipped frame (busy 99) was kept in %s" % label)
            d1 = stats["frame"]["busy_med"] - stats["sdr"]["busy_med"]
            d2 = stats["full"]["busy_mean"] - stats["sdr"]["busy_mean"]
            check(abs(d1 - 1.0) < 0.05 and abs(d2 - 2.5) < 0.05, "deltas %.3f %.3f" % (d1, d2))
            with open(csv_path, newline="", encoding="utf-8") as fh:
                rows = list(csv.reader(fh))
            check(rows[0] == ["epoch", "phase", "gpu_busy", "between", "gpu_time", "occurrence"], "csv header")
            check(len(rows) - 1 == sum(s["frames"] for s in stats.values()), "csv rows")
            check({r[1] for r in rows[1:]} == set(synth["labels"]), "csv phases")
            check("per occurrence" in text and "GPU busy against the first label (sdr)" in text, "report text")
            if cut is None:
                early = sum(1 for sw in synth["switches"] if sw[0] < synth["fr"])
                check("note: %d switch(es) happen before free roam" % early in text, "note about early switches: " + text[:600])
            else:
                check("note:" not in text, "note shown although --from-free-roam was given")
        # failures that are reported, not raised from deep inside
        (root / "pm_meta.json").unlink()
        try:
            analyse(root)
        except SetupError:
            pass
        else:
            raise AssertionError("selftest: a missing pm_meta.json was accepted")
        far = io.StringIO()
        with redirect_stdout(far):
            result = analyse(root, qpc_offset=1795421594.8849978 + 100000.0)
        check(result is None and "every frame was dropped" in far.getvalue(), "frames far from the log were accepted")
        # the last report, for the eyes
        analyse(root, qpc_offset=1795421594.8849978, per_occurrence=False)
    print("\nselftest OK")
    return 0


# ================================================================================================================
# Command line
# ================================================================================================================

def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="GPU cost per phase of an in-run HDR A/B run (mcla_hdr_ab); see the module docstring.")
    ap.add_argument("run_dir", nargs="?", help="the run folder: mcla.log (+ rotated), pm.csv, pm_meta.json")
    ap.add_argument("--skip-after-switch", type=float, default=SKIP_AFTER_SWITCH_S, metavar="S",
                    help="drop the frames within S s after a switch (default %g)" % SKIP_AFTER_SWITCH_S)
    ap.add_argument("--from-free-roam", type=float, default=None, metavar="S",
                    help="drop the frames before free roam + S s (default: no cut at free roam)")
    ap.add_argument("--csv", metavar="FILE", help="write the kept frames: epoch, phase, gpu_busy, between, gpu_time, occurrence")
    ap.add_argument("--per-occurrence", action="store_true", help="also one line per phase occurrence (drift over the run)")
    ap.add_argument("--qpc-offset", type=float, default=None, metavar="S", help="time.time() - perf_counter() of the capture (default: pm_meta.json)")
    ap.add_argument("--pid", type=int, default=None, metavar="N", help="only the rows of this process id")
    ap.add_argument("--selftest", action="store_true", help="run on a synthetic log + PresentMon CSV and check the numbers")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.run_dir:
        ap.error("RUN_DIR is required (or --selftest)")
    if args.skip_after_switch < 0 or (args.from_free_roam is not None and args.from_free_roam < 0):
        ap.error("--skip-after-switch and --from-free-roam are not negative")
    try:
        stats = analyse(args.run_dir, args.skip_after_switch, args.from_free_roam, args.csv, args.per_occurrence, args.qpc_offset, args.pid)
    except SetupError as e:
        print("hdr_ab: %s" % e, file=sys.stderr)
        return 2
    return 0 if stats is not None else 1


if __name__ == "__main__":
    sys.exit(main())
