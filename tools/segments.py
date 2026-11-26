#!/usr/bin/env python3
"""segments.py - per-segment analysis of ONE run: A/B inside one game process (PLAN 6; tools/run_game.py --schedule).

    python tools/segments.py <run dir | run id> [--work W] [--settle S=3] [--from-free-roam S=5] [--json OUT]
                             [--pid N] [--qpc-offset S | --pm-t0 S] [--no-telemetry]

A run with a schedule (`<seconds>:set <cvar>=<value>`, `burn <n>`, `burnmem <n>`, `mark <label>` steps in the flow or in
`run_game.py --schedule`) is cut by the host at every change of state: it logs `[mcla-sched] t=<s> segment <k>: <label>`
(k from 0 = the state at start-up; the label is every key the schedule touches as sorted `key=value`, e.g.
`burn=4 mcla_gfx_preset=perf`). This tool takes the PresentMon frames and the telemetry of the run and prints the numbers
per segment and per label (segments with the same label are POOLED: frames and telemetry samples concatenated), then the
A/B deltas between labels. Without segment lines the whole window is one segment labelled `all`.

Inputs, all in the run dir (bench.py wrote them; this tool REUSES bench.py's loaders and its time alignment)
  pm.csv + pm_meta.json   PresentMon 2.6 (qpc_offset_s maps CPUStartQPCTimeInMs to epoch seconds). Required.
  mcla.log (+ mcla.N.log)  the host log: segment lines, `-> free-roam`, `[mcla-sim] cap 60 -> 30`, `[mcla-threads]`. Required.
  adlx.csv + sampler_meta.json (start_epoch)   GPU clock and package power (ADLX). Optional.
  cpu.csv                 busy cores of the machine and of the game (bench.py CpuLoad). Optional.
  summary.json            t0_epoch (process start; times are printed relative to it, else the first log line). Optional.

Window of a segment: from the segment line's log time + --settle seconds (the transition: pipelines, caches, a set that takes
effect a frame later) to the next segment line (or the end of the capture); nothing before `[mcla-mode] <from> -> free-roam`
+ --from-free-roam seconds (no such line: no cut). bench.py's own window is free roam + 20 s: `--from-free-roam 20 --settle 0`
on a run without segment lines gives bench's numbers (frames, avg_fps, p50, p99, % > 25 ms, GPU busy %).

Numbers per segment / label (n/a where the file or column is missing)
  frames, sec (sum of frame times), avg_fps (frames / sec), p50_ms and p99_ms of MsBetweenPresents (linear interpolation,
  bench.frame_stats), over25 / over34 (% of frames longer than 25 / 34 ms), gpu_busy (sum MsGPUBusy / sum MsBetweenPresents),
  gpu_ms p50/p90 (MsGPUBusy), cpu_ms p50/p90 (MsCPUBusy), gpuwait_ms p50 (MsGPUWait),
  gfx_mhz p50/p10 and pkg_w p50 (ADLX samples inside the window; 1 Hz real resolution), cpu_nongame / cpu_game (median busy
  cores from cpu.csv: whole machine minus the game, and the game), cap60to30 (number of `[mcla-sim] cap 60 -> 30` lines inside),
  and, with `[mcla-threads] t=<s> dt=<s> total=<cores> | <name>=<cores> | ...` lines (a host feature that may be absent),
  the mean cores per thread name over the lines whose interval mid-point is inside, weighted by dt, a thread missing from a
  line counting as 0; the second, indented line lists the top 8.
A/B block: for every pair of labels (first appearance order) B - A of avg_fps, p50, p99, gpu_busy, gpu_ms p50, gfx_mhz p50, pkg_w.

--json OUT writes everything (segments, labels, A/B, the inputs found) as JSON.
Exit codes: 0 done; 2 setup problem (no run, no pm.csv, no log, a CSV that cannot be aligned); 3 no frame in any segment.
Stdlib only.
"""

import argparse
import bisect
import json
import os
import re
import statistics
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:     # tools/ is on sys.path when run as a script; this lets `import bench` work from anywhere
    sys.path.insert(0, str(HERE))
import bench  # noqa: E402
import run_game  # noqa: E402

DEFAULT_SETTLE_S = 3.0
DEFAULT_FROM_FREE_ROAM_S = 5.0
OVER_MS = 34.0
TOP_THREADS = 8
EXIT_SETUP = 2
EXIT_NO_FRAMES = 3

_RE_SEGMENT = re.compile(r"\[mcla-sched\] t=(?P<t>[0-9.]+) segment (?P<k>\d+): (?P<label>.*?)\s*$")
_RE_CAP = re.compile(r"\[mcla-sim\].*?\bcap (?P<a>\d+) -> (?P<b>\d+)")
_RE_T = re.compile(r"(?<![A-Za-z_])t=(?P<v>[0-9.]+)")
_RE_DT = re.compile(r"\bdt=(?P<v>[0-9.]+)")
_RE_TOTAL = re.compile(r"\btotal=(?P<v>[0-9.]+)")


class SetupError(Exception):
    """Something is wrong with the arguments or the run dir; nothing was analysed. Exit 2."""


def say(msg: str) -> None:
    print(msg, flush=True)


# ================================================================================================================
# The log: segment lines, cap changes, per-thread CPU lines
# ================================================================================================================

def scan_sched_log(path) -> Dict:
    """One pass over the log (rotated parts included) for what bench.scan_log does not read: the `[mcla-sched] ... segment`
    lines [(epoch, k, label, t_s)], `[mcla-sim] cap A -> B` lines [(epoch, A, B)] and `[mcla-threads]` lines
    [{ts, dt, total, threads: {name: cores}}]. Lines without a timestamp are skipped."""
    segs: List[Tuple[float, int, str, Optional[float]]] = []
    caps: List[Tuple[float, int, int]] = []
    threads: List[Dict] = []
    for f in run_game.log_files(path):
        with open(f, "rb") as fh:
            for raw in fh:
                if b"[mcla-" not in raw:
                    continue
                if b"[mcla-sched]" in raw:
                    line = raw.decode("utf-8", "replace")
                    m = _RE_SEGMENT.search(line)
                    ts = run_game._parse_ts(line[:25])
                    if m and ts is not None:
                        segs.append((ts, int(m.group("k")), m.group("label"), float(m.group("t"))))
                elif b"[mcla-sim]" in raw and b" -> " in raw:
                    line = raw.decode("utf-8", "replace")
                    m = _RE_CAP.search(line)
                    ts = run_game._parse_ts(line[:25])
                    if m and ts is not None:
                        caps.append((ts, int(m.group("a")), int(m.group("b"))))
                elif b"[mcla-threads]" in raw:
                    line = raw.decode("utf-8", "replace")
                    ts = run_game._parse_ts(line[:25])
                    entry = parse_threads_line(line)
                    if entry is not None and ts is not None:
                        entry["ts"] = ts
                        threads.append(entry)
    segs.sort(key=lambda r: r[0])
    return {"segments": segs, "caps": caps, "threads": threads}


def parse_threads_line(line: str) -> Optional[Dict]:
    """`[mcla-threads] t=<s> dt=<s> total=<cores> | <name>=<cores> | ...` -> {dt, total, threads}; names may hold spaces."""
    head, sep, rest = line.partition("[mcla-threads]")
    if not sep:
        return None
    parts = rest.split("|")
    dt = _RE_DT.search(parts[0])
    total = _RE_TOTAL.search(parts[0])
    if dt is None:
        return None
    threads: Dict[str, float] = {}
    for part in parts[1:]:
        name, eq, val = part.strip().rpartition("=")
        if not eq or not name.strip():
            continue
        try:
            threads[name.strip()] = threads.get(name.strip(), 0.0) + float(val)
        except ValueError:
            continue
    return {"dt": float(dt.group("v")), "total": float(total.group("v")) if total else None, "threads": threads}


# ================================================================================================================
# Inputs of a run dir
# ================================================================================================================

def resolve_run_dir(arg: str, work: Optional[str]) -> Path:
    p = Path(arg)
    if p.is_dir():
        return p
    root = Path(work or os.environ.get("MCLA_WORK") or run_game.DEFAULT_WORK)
    cand = root / "runs" / arg
    if cand.is_dir():
        return cand
    raise SetupError("no run '%s' (not a directory, and %s does not exist)" % (arg, cand.as_posix()))


def read_json(path: Path) -> Optional[Dict]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


class RunData:
    """Everything of one run, on the epoch clock (local seconds): frames, telemetry, log facts."""

    def __init__(self, run_dir: Path, pid: Optional[int], qpc_offset: Optional[float], pm_t0: Optional[float],
                 telemetry: bool) -> None:
        self.run_dir = run_dir
        self.notes: List[str] = []
        pm_csv, log = run_dir / "pm.csv", run_dir / "mcla.log"
        if not pm_csv.is_file():
            raise SetupError("no pm.csv in %s (a bench.py run writes it)" % run_dir.as_posix())
        if not run_game.log_files(log):
            raise SetupError("no mcla.log in %s" % run_dir.as_posix())
        self.pm_meta = read_json(run_dir / "pm_meta.json") or {}
        self.summary = read_json(run_dir / "summary.json") or {}

        self.log = bench.scan_log(log)
        self.sched = scan_sched_log(log)
        self.first_ts: Optional[float] = self.log["first_ts"]
        self.t0: Optional[float] = self.summary.get("t0_epoch") or self.first_ts
        self.fr_ts: Optional[float] = self.log["fr_ts"]

        try:
            pm = bench.load_pm_csv(pm_csv, pid if pid is not None else self.pm_meta.get("pid"))
        except (OSError, ValueError) as e:
            raise SetupError("PresentMon CSV unusable: %s" % e)
        self.pm_rows, self.pm_clock = pm.rows, pm.clock
        if pm.clock == "qpc":
            off = qpc_offset if qpc_offset is not None else self.pm_meta.get("qpc_offset_s")
            if off is None:
                raise SetupError("pm.csv has absolute QPC times: pm_meta.json (qpc_offset_s) is missing; give --qpc-offset")
            shift = float(off)
        elif pm_t0 is not None and self.first_ts is not None:
            shift = self.first_ts + pm_t0
        else:
            raise SetupError("pm.csv has a relative clock: give --pm-t0 (seconds after the first log line at which it is 0)")
        best = None
        for key, s in pm.series.items():
            if s.t and (best is None or len(s.t) > len(pm.series[best].t)):
                best = key
        if best is None:
            raise SetupError("pm.csv has no frames")
        s = pm.series[best]
        order = sorted(range(len(s.t)), key=lambda i: s.t[i])
        self.chain = best
        self.chains = len(pm.series)
        self.ep: List[float] = [s.t[i] + shift for i in order]
        self.ft: List[float] = [s.ft[i] for i in order]
        self.gpu = [s.gpu[i] for i in order]
        self.cpu = [s.cpu[i] for i in order]
        self.gwait = [s.gwait[i] for i in order]
        self.cap_first, self.cap_last = self.ep[0], self.ep[-1]

        self.adlx: List[Tuple[float, Optional[float], Optional[float]]] = []   # (epoch, gfx_mhz, gpu_w)
        self.cpu_rows: List[Tuple[float, float, float]] = []
        if telemetry:
            self._load_telemetry(run_dir)

    def _load_telemetry(self, run_dir: Path) -> None:
        adlx = run_dir / "adlx.csv"
        meta = read_json(run_dir / "sampler_meta.json")
        if adlx.is_file():
            start = (meta or {}).get("start_epoch")
            if start is None:
                self.notes.append("adlx.csv present but sampler_meta.json has no start_epoch: ADLX numbers n/a")
            else:
                try:
                    self.adlx = [(t + float(start), mhz, w) for t, mhz, w, _ in bench.load_adlx_csv(adlx)]
                except (OSError, ValueError) as e:
                    self.notes.append("adlx.csv unusable: %s" % e)
        cpu = run_dir / "cpu.csv"
        if cpu.is_file():
            try:
                self.cpu_rows = bench.load_cpu_csv(cpu)
            except (OSError, ValueError) as e:
                self.notes.append("cpu.csv unusable: %s" % e)

    def rel(self, epoch: Optional[float]) -> Optional[float]:
        return None if epoch is None or self.t0 is None else epoch - self.t0


# ================================================================================================================
# Segments
# ================================================================================================================

def build_segments(d: RunData, settle: float, from_fr: float) -> Tuple[List[Dict], str]:
    """The segments with their windows [start, end) (epoch) and raw samples. Returns (segments, how they were cut)."""
    end_all = d.cap_last + 1e-3
    cut = d.fr_ts + from_fr if d.fr_ts is not None else None
    lines = d.sched["segments"]
    if lines:
        how = "%d segment line(s) in the log" % len(lines)
        bounds = [(ts, k, label) for ts, k, label, _ in lines]
    else:
        how = "no segment lines in the log: one segment `all`"
        bounds = [(d.cap_first, 0, "all")]
    segs: List[Dict] = []
    for i, (ts, k, label) in enumerate(bounds):
        nxt = bounds[i + 1][0] if i + 1 < len(bounds) else end_all
        start = ts + settle
        if cut is not None:
            start = max(start, cut)
        end = min(nxt, end_all)
        segs.append({"k": k, "label": label, "line_epoch": ts, "start": start, "end": end})
    for sg in segs:
        sg["raw"] = extract(d, sg["start"], sg["end"])
    return segs, how


def extract(d: RunData, a: float, b: float) -> Dict:
    """The samples inside [a, b): frame columns, ADLX rows, CPU rows, cap lines, thread lines."""
    if b <= a:
        return {"ft": [], "gpu": [], "cpu": [], "gwait": [], "adlx": [], "cpu_rows": [], "caps": [], "threads": []}
    lo, hi = bisect.bisect_left(d.ep, a), bisect.bisect_left(d.ep, b)
    return {
        "ft": d.ft[lo:hi], "gpu": d.gpu[lo:hi], "cpu": d.cpu[lo:hi], "gwait": d.gwait[lo:hi],
        "adlx": [r for r in d.adlx if a <= r[0] < b],
        "cpu_rows": [r for r in d.cpu_rows if a <= r[0] < b],
        "caps": [(ts, x, y) for ts, x, y in d.sched["caps"] if a <= ts < b],
        "threads": [e for e in d.sched["threads"] if a <= e["ts"] - e["dt"] / 2.0 < b],
    }


def pool(raws: Sequence[Dict]) -> Dict:
    out: Dict[str, List] = {k: [] for k in ("ft", "gpu", "cpu", "gwait", "adlx", "cpu_rows", "caps", "threads")}
    for r in raws:
        for k in out:
            out[k] += r[k]
    return out


def pct(vals: Sequence[float], p: float) -> Optional[float]:
    return bench.percentile(sorted(vals), p) if vals else None


def metrics(raw: Dict) -> Dict:
    """The numbers of a (pooled) sample set; None where there is no data."""
    ft = raw["ft"]
    m: Dict = {"frames": len(ft)}
    keys = ("seconds", "avg_fps", "p50_ms", "p99_ms", "pct_over_25ms", "pct_over_34ms", "gpu_busy_pct", "gpu_ms_p50",
            "gpu_ms_p90", "cpu_ms_p50", "cpu_ms_p90", "gpuwait_ms_p50", "gfx_mhz_p50", "gfx_mhz_p10", "pkg_w_p50",
            "adlx_rows", "cpu_nongame", "cpu_game", "cpu_rows", "cap_60_30", "cap_changes", "thread_lines", "thread_total")
    for k in keys:
        m[k] = None
    m["threads"] = {}
    m["cap_60_30"] = sum(1 for _, a, b in raw["caps"] if (a, b) == (60, 30))
    m["cap_changes"] = len(raw["caps"])
    if ft:
        st = bench.frame_stats(ft, raw["gpu"])
        m.update(seconds=st["seconds"], avg_fps=st["avg_fps"], p50_ms=st["p50_ms"], p99_ms=st["p99_ms"],
                 pct_over_25ms=st["pct_over_25ms"], gpu_busy_pct=st["gpu_busy_pct"], max_ms=st["max_ms"],
                 pct_over_34ms=100.0 * sum(1 for x in ft if x > OVER_MS) / len(ft))
        g = [v for v in raw["gpu"] if v is not None]
        m["gpu_ms_p50"], m["gpu_ms_p90"] = pct(g, 50), pct(g, 90)
        c = [v for v in raw["cpu"] if v is not None]
        m["cpu_ms_p50"], m["cpu_ms_p90"] = pct(c, 50), pct(c, 90)
        w = [v for v in raw["gwait"] if v is not None]
        m["gpuwait_ms_p50"] = pct(w, 50)
    clk = [r[1] for r in raw["adlx"] if r[1] is not None]
    pw = [r[2] for r in raw["adlx"] if r[2] is not None]
    m["adlx_rows"] = len(raw["adlx"])
    if clk:
        m["gfx_mhz_p50"], m["gfx_mhz_p10"] = pct(clk, 50), pct(clk, 10)
    if pw:
        m["pkg_w_p50"] = pct(pw, 50)
    if raw["cpu_rows"]:
        m["cpu_rows"] = len(raw["cpu_rows"])
        m["cpu_nongame"] = statistics.median(max(0.0, s - g_) for _, s, g_ in raw["cpu_rows"])
        m["cpu_game"] = statistics.median(g_ for _, _, g_ in raw["cpu_rows"])
    th = raw["threads"]
    if th:
        wsum = sum(e["dt"] for e in th)
        m["thread_lines"] = len(th)
        if wsum > 0:
            totals = [(e["total"], e["dt"]) for e in th if e["total"] is not None]
            if totals:
                m["thread_total"] = sum(v * w_ for v, w_ in totals) / sum(w_ for _, w_ in totals)
            acc: Dict[str, float] = {}
            for e in th:
                for name, cores in e["threads"].items():
                    acc[name] = acc.get(name, 0.0) + cores * e["dt"]
            m["threads"] = {name: v / wsum for name, v in sorted(acc.items(), key=lambda kv: -kv[1])}
    return m


# ================================================================================================================
# Printing
# ================================================================================================================

def f(v: Optional[float], digits: int = 2) -> str:
    return bench.num(v, digits)


def fpct(v: Optional[float], digits: int = 2) -> str:
    return "n/a" if v is None else "%.*f%%" % (digits, v)


def pair(a: Optional[float], b: Optional[float], digits: int = 1) -> str:
    return "%s/%s" % (f(a, digits), f(b, digits))


def metric_line(m: Dict) -> str:
    if not m["frames"]:
        return "frames=0 (no frame in the window)"
    return ("frames=%d sec=%s avg_fps=%s p50_ms=%s p99_ms=%s over25=%s over34=%s gpu_busy=%s gpu_ms(p50/p90)=%s "
            "cpu_ms(p50/p90)=%s gpuwait_ms(p50)=%s gfx_mhz(p50/p10)=%s pkg_w=%s cpu_nongame=%s cpu_game=%s cap60to30=%d"
            % (m["frames"], f(m["seconds"], 1), f(m["avg_fps"]), f(m["p50_ms"]), f(m["p99_ms"]), fpct(m["pct_over_25ms"]),
               fpct(m["pct_over_34ms"]), fpct(m["gpu_busy_pct"], 1), pair(m["gpu_ms_p50"], m["gpu_ms_p90"]),
               pair(m["cpu_ms_p50"], m["cpu_ms_p90"]), f(m["gpuwait_ms_p50"]),
               pair(m["gfx_mhz_p50"], m["gfx_mhz_p10"], 0), f(m["pkg_w_p50"], 1), f(m["cpu_nongame"]), f(m["cpu_game"]),
               m["cap_60_30"]))


def threads_line(m: Dict) -> Optional[str]:
    if not m["threads"]:
        return None
    top = list(m["threads"].items())[:TOP_THREADS]
    return "    threads (mean cores, total %s, %d line(s)): %s" % (
        f(m["thread_total"]), m["thread_lines"], " | ".join("%s=%.2f" % (n, c) for n, c in top))


AB_KEYS = (("avg_fps", "avg_fps", 2, ""), ("p50_ms", "p50_ms", 2, ""), ("p99_ms", "p99_ms", 2, ""),
           ("gpu_busy_pct", "gpu_busy", 1, "pts"), ("gpu_ms_p50", "gpu_ms_p50", 2, ""), ("gfx_mhz_p50", "gfx_mhz_p50", 0, ""),
           ("pkg_w_p50", "pkg_w", 1, ""))


def ab_delta(a: Dict, b: Dict) -> Dict[str, Optional[float]]:
    return {key: (None if a[key] is None or b[key] is None else b[key] - a[key]) for key, *_ in AB_KEYS}


def ab_line(delta: Dict[str, Optional[float]]) -> str:
    cells = []
    for key, name, digits, unit in AB_KEYS:
        v = delta[key]
        cells.append("%s=%s" % (name, "n/a" if v is None else "%+.*f%s" % (digits, v, unit)))
    return " ".join(cells)


# ================================================================================================================
# Main
# ================================================================================================================

def analyse(run_dir: Path, settle: float, from_fr: float, pid: Optional[int] = None, qpc_offset: Optional[float] = None,
            pm_t0: Optional[float] = None, telemetry: bool = True) -> Tuple[RunData, Dict]:
    d = RunData(run_dir, pid, qpc_offset, pm_t0, telemetry)
    segs, how = build_segments(d, settle, from_fr)
    seg_out: List[Dict] = []
    for sg in segs:
        m = metrics(sg["raw"])
        m.update(k=sg["k"], label=sg["label"], start_s=d.rel(sg["start"]), end_s=d.rel(sg["end"]),
                 line_s=d.rel(sg["line_epoch"]))
        seg_out.append(m)
    labels: Dict[str, List[int]] = {}
    for i, sg in enumerate(segs):
        labels.setdefault(sg["label"], []).append(i)
    lab_out: List[Dict] = []
    for label, idxs in labels.items():
        m = metrics(pool([segs[i]["raw"] for i in idxs]))
        m.update(label=label, segments=[segs[i]["k"] for i in idxs])
        lab_out.append(m)
    ab: List[Dict] = []
    for i in range(len(lab_out)):
        for j in range(i + 1, len(lab_out)):
            a, b = lab_out[i], lab_out[j]
            ab.append({"a": a["label"], "b": b["label"], "delta": ab_delta(a, b)})
    result = {
        "run_id": run_dir.name, "run_dir": run_dir.as_posix(), "settle_s": settle, "from_free_roam_s": from_fr,
        "cut": how, "t0_epoch": d.t0, "free_roam_s": d.rel(d.fr_ts), "free_roam_epoch": d.fr_ts,
        "capture": {"first_s": d.rel(d.cap_first), "last_s": d.rel(d.cap_last), "rows": d.pm_rows, "chain": list(d.chain),
                    "chains": d.chains, "clock": d.pm_clock},
        "inputs": {"adlx_rows": len(d.adlx), "cpu_rows": len(d.cpu_rows), "thread_lines": len(d.sched["threads"]),
                   "cap_lines": len(d.sched["caps"]), "segment_lines": len(d.sched["segments"])},
        "notes": d.notes, "segments": seg_out, "labels": lab_out, "ab": ab,
    }
    return d, result


def print_report(d: RunData, r: Dict) -> None:
    say("run %s: free-roam at +%ss; capture +%s..+%s s (%d rows, chain %s); %s" % (
        r["run_id"], f(r["free_roam_s"], 1), f(r["capture"]["first_s"], 1), f(r["capture"]["last_s"], 1), r["capture"]["rows"],
        r["capture"]["chain"][1] or "?", r["cut"]))
    say("window: settle %g s per segment, from free-roam +%g s%s; telemetry: adlx %d row(s), cpu %d row(s), thread lines %d, "
        "cap lines %d" % (r["settle_s"], r["from_free_roam_s"], "" if d.fr_ts is not None else " (no `-> free-roam` line: no cut)",
                          r["inputs"]["adlx_rows"], r["inputs"]["cpu_rows"], r["inputs"]["thread_lines"], r["inputs"]["cap_lines"]))
    for n in r["notes"]:
        say("note: " + n)
    say("segments (%d):" % len(r["segments"]))
    for sg in r["segments"]:
        say("  segment %d [%s] %s: %s" % (sg["k"], sg["label"], "+%s..+%s s" % (f(sg["start_s"], 1), f(sg["end_s"], 1)),
                                         metric_line(sg)))
        tl = threads_line(sg)
        if tl:
            say(tl)
    say("labels (%d, segments with the same label pooled):" % len(r["labels"]))
    for lb in r["labels"]:
        say("  label [%s] segments %s: %s" % (lb["label"], ",".join(str(k) for k in lb["segments"]), metric_line(lb)))
        tl = threads_line(lb)
        if tl:
            say(tl)
    if r["ab"]:
        say("A/B (B - A; label order = first appearance):")
        for row in r["ab"]:
            say("  A=[%s] B=[%s]: %s" % (row["a"], row["b"], ab_line(row["delta"])))


def make_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="segments.py", formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Per-segment analysis of one run (PresentMon + telemetry + the host's `[mcla-sched] segment` lines). "
                    "Details: the docstring at the top of this file.")
    ap.add_argument("run", metavar="RUN", help="a run directory, or a run id under $WORK/runs")
    ap.add_argument("--work", metavar="W", help="$WORK (default env MCLA_WORK, else %s)" % run_game.DEFAULT_WORK)
    ap.add_argument("--settle", type=float, default=DEFAULT_SETTLE_S, metavar="S",
                    help="drop the first S seconds of every segment (default %g)" % DEFAULT_SETTLE_S)
    ap.add_argument("--from-free-roam", type=float, default=DEFAULT_FROM_FREE_ROAM_S, metavar="S",
                    help="nothing before `-> free-roam` + S seconds (default %g; bench.py's window is 20)" % DEFAULT_FROM_FREE_ROAM_S)
    ap.add_argument("--json", metavar="OUT", help="write everything as JSON")
    ap.add_argument("--pid", type=int, metavar="N", help="only the PresentMon rows of this process id (default: pm_meta.json's)")
    ap.add_argument("--qpc-offset", type=float, metavar="S", help="time.time() - perf_counter() of the capture (default: pm_meta.json)")
    ap.add_argument("--pm-t0", type=float, metavar="S",
                    help="relative-clock CSV only: seconds after the first log line at which the CSV clock is 0")
    ap.add_argument("--no-telemetry", action="store_true", help="ignore adlx.csv and cpu.csv")
    return ap


def main(argv: Optional[Sequence[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    args = make_parser().parse_args(argv)
    if args.settle < 0 or args.from_free_roam < 0:
        print("segments: --settle and --from-free-roam must not be negative", file=sys.stderr)
        return EXIT_SETUP
    try:
        run_dir = resolve_run_dir(args.run, args.work)
        d, result = analyse(run_dir, args.settle, args.from_free_roam, args.pid, args.qpc_offset, args.pm_t0,
                            not args.no_telemetry)
    except SetupError as e:
        print("segments: %s" % e, file=sys.stderr)
        return EXIT_SETUP
    print_report(d, result)
    if args.json:
        try:
            run_game.write_json(Path(args.json), result)
            say("json %s" % Path(args.json).as_posix())
        except OSError as e:
            print("segments: cannot write %s: %s" % (args.json, e), file=sys.stderr)
            return EXIT_SETUP
    return 0 if any(sg["frames"] for sg in result["segments"]) else EXIT_NO_FRAMES


if __name__ == "__main__":
    sys.exit(main())
