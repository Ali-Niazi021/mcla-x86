#!/usr/bin/env python3
"""display_log.py - the `[present-stats]` and `[mcla-hitch]` log lines: parsers, the cause classifier, totals (stdlib only).

Used by tools/run_game.py (the run summary, summary.json keys `display` and `hitches`) and tools/bench.py (METRICS notes);
tests: tools/test_log_parse.py. Pure functions, no I/O.

Two line kinds (the file log prefix is `[YYYY-MM-DD HH:MM:SS.mmm] [info] [core|gpu] [tNNN] `):

1. The D3D12 presenter (SDK patch patches/sdk/0750-present-stats.patch), every present_stats_log_s (10) s:
     [present-stats] 10.0 s: presents P shown S dropped D | refreshes R at H Hz | shown frames held 1 refresh a, 2 b,
         3+ c (max m), repeats >= r[ (+N frames over M refreshes sampled together)] | present call max X ms, over 2 ms K |
         present interval max Y ms, over 1.5 / 2.5 refreshes I / J | stats n ok f failed[, K presents FAILED][, K counter
         resets][, K more hitches not logged] | cadence C refreshes a frame (P presents/s on a H Hz refresh clock[; display
         mode M Hz[, note]]), held over K refreshes N[, hitches (>= L refreshes) H], present interval over 1.5 / 2.5 times
         the mean U / V | reliable
   (one physical line). A window the statistics cannot vouch for (the window hidden, covered or minimised, the display off,
   a failed or restarted GetFrameStatistics, a counter jump, the refresh clock not covering the window) prints `?` in every
   display-side number and ends ` | UNRELIABLE (<reason>)`; its CPU-side numbers (presents, present call, stats ok / failed,
   the interval-over-the-mean counts) stay. Semantics (what the numbers are, measured against a 165 Hz panel):
     shown     presents that reached the screen (a frame with a refresh of its own); `dropped` = replaced before display.
     cadence   refreshes one frame is expected to last = refresh clock / present rate (60 fps on 165 Hz = 2.75: a frame stays
               2 or 3 refreshes in a 3-3-3-2 beat, by design, NOT a repeat; on 60 Hz at 60 fps it is 1).
     held 1 / 2 / 3+   RAW counts of shown frames by refreshes on screen: read them against the cadence.
     held over K refreshes N   frames held longer than ceil(cadence) = K refreshes: the over-cadence count.
     repeats >= r   the refreshes beyond K, summed over those frames (a lower bound: frames sampled together are not attributed).
     hitches (>= L refreshes) H   the display-side hitch count: frames held L = K + present_stats_hitch_refreshes (default 2)
               refreshes or more (the section is absent when present_stats_hitch_refreshes is 0).
   plus, per shown frame that stayed present_stats_hitch_refreshes refreshes beyond what the cadence allows (at most 8 a window):
     [present-stats] hitch: a frame stayed N refreshes on screen (X ms) before the next (cadence C refreshes a frame: up to K is normal)
   and a once-only `[present-stats] GetFrameStatistics failed (HRESULT 0x...)` warning (kept as a warning sample).
   The window line is parsed section by section (split at ` | `): a section that does not parse leaves its fields out
   of the window dict (None) instead of losing the line (`partial`); a line that has the `N s: presents` head but cannot be read
   at all counts as `unparsed`. An UNRELIABLE window parses (`reliable` False, `reason`, `reason_short`, display fields None) and
   is counted as an unreliable window, never as unreadable.

   The FIRST VERSION of the line (old logs, `format` "old") has no cadence tail and judged repeats against 1 refresh. It is
   trusted only when its own refresh clock matches its present rate (cadence = H Hz / (P / secs) within 0.92 .. 1.08: 60 fps on
   60 Hz): then held 2 + 3+ are the over-cadence frames and 3+ the hitches. Otherwise (165 Hz at 60 fps, a clock that did not move)
   it is `old format: cadence unknown` and counts as an unreliable window.

   Totals (aggregate_display) cover the RELIABLE windows only for every display-side number: presents, shown, dropped,
   refreshes, the measured refresh clock (median), the cadence (median), held counts, over-cadence frames, display hitches,
   repeats, the over-refresh interval counts. CPU-side numbers (present call, interval max, stats, interval over the mean) and
   the log-event counts (hitch lines, not-logged hitches, counter resets, FAILED presents) cover all windows. The verdict:
     even             no over-cadence frame and a median cadence within 0.02 of a whole number;
     beat by design   no over-cadence frame and a cadence that is not a whole number (2.75: frames stay 2 or 3 refreshes);
     repeats          over-cadence frames > 0 (the count, and per minute of reliable windows);
     unknown          no reliable window.

2. The game thread (mcla/hooks/mcla_hitch.cpp), per engine frame longer than 1.5 periods, at most mcla_hitch_lines (12) per 5 s:
     [mcla-hitch] 183.4 ms between engine frames (11.0 periods of 16.7) in <mode> | <cpu> | <compile> | <gpu> |
         page faults +N, read +X MiB
   <mode>    `a drive` | `a menu / pause / load` | `an unknown screen`   (keys: drive | menu | unknown)
   <cpu>     `game thread on CPU X ms (waiting Y ms, of which pacer sleep Z ms)` | `game thread CPU time not measured yet
             (pacer sleep Z ms)`
   <compile> `pipelines +A shaders +B in flight C` | `pipelines ? (no SDK counters)`
   <gpu>     `GPU frame X ms` | `GPU frame ?`
   and, when the window's lines ran out: `[mcla-hitch] N more hitch(es) in the last 5 s not logged (worst X ms; mcla_hitch_lines 12)`.

Hitch causes (classify_hitch, first match wins): compile (pipelines or shaders created > 0), cpu-bound (the game thread was on
the CPU for >= 60 % of the frame), paging/io (page faults >= 1000 or read >= 4 MiB), else blocked (the game thread waited:
on the GPU, the pacer or something else). A blocked hitch whose GPU frame time is >= 80 % of the frame is counted again as
`blocked_gpu` (a sub-count of blocked, not a fifth class). A missing measurement (CPU not measured yet, no SDK counters) never
satisfies its test.
"""

import math
import re
import statistics
from collections import Counter
from typing import Dict, List, Optional, Sequence

CAUSES = ("compile", "cpu-bound", "paging/io", "blocked")
MODES = ("drive", "menu", "unknown")
CPU_BOUND_FRAC = 0.60
PAGE_FAULTS_IO = 1000
READ_MIB_IO = 4.0
GPU_BOUND_FRAC = 0.80
DEAD_MIN_PRESENTS = 10     # a window with this many presents and no shown frame / refresh: the display counters did not move

_MODE_KEY = {"a drive": "drive", "a menu / pause / load": "menu", "an unknown screen": "unknown"}

# ---------------------------------------------------------------------------------------------------- [present-stats]
_RE_PS_HEAD = re.compile(r"\[present-stats\] (?P<secs>\d+(?:\.\d+)?) s: presents ")
# a count prints `?` in an UNRELIABLE window: `\d+|\?`
_RE_PS_PRESENTS = re.compile(r"presents (?P<presents>\d+) shown (?P<shown>\d+|\?) dropped (?P<dropped>\d+|\?)$")
_RE_PS_REFRESH = re.compile(r"refreshes (?P<refreshes>\d+|\?) at (?P<hz>\d+(?:\.\d+)?|\?) Hz$")
_RE_PS_HELD = re.compile(r"shown frames held 1 refresh (?P<h1>\d+|\?), 2 (?P<h2>\d+|\?), 3\+ (?P<h3>\d+|\?) \(max (?P<hmax>\d+|\?)\), "
                         r"repeats >= (?P<repeats>\d+|\?)(?: \(\+(?P<mf>\d+) frames over (?P<mr>\d+) refreshes sampled together\))?$")
_RE_PS_CALL = re.compile(r"present call max (?P<cmax>\d+(?:\.\d+)?) ms, over 2 ms (?P<c2>\d+)$")
_RE_PS_INTERVAL = re.compile(r"present interval max (?P<imax>\d+(?:\.\d+)?) ms, over 1\.5 / 2\.5 refreshes "
                             r"(?P<i15>\d+|\?) / (?P<i25>\d+|\?)$")
_RE_PS_STATS = re.compile(r"stats (?P<ok>\d+) ok (?P<failed>\d+) failed(?P<tail>.*)$")
_RE_PS_FAILED = re.compile(r", (\d+) presents FAILED")
_RE_PS_RESETS = re.compile(r", (\d+) counter resets")
_RE_PS_UNLOGGED = re.compile(r", (\d+) more hitches not logged")
# the cadence tail (SDK 0750): the reliable and the UNRELIABLE form, then the ` | reliable` / ` | UNRELIABLE (<reason>)` ending
_RE_PS_CADENCE = re.compile(r"cadence (?P<cadence>\d+(?:\.\d+)?) refreshes a frame \((?P<rate>\d+(?:\.\d+)?) presents/s on a "
                            r"(?P<clock>\d+(?:\.\d+)?) Hz refresh clock(?P<mode>.*?)\), held over (?P<k>\d+) refreshes (?P<over>\d+)"
                            r"(?:, hitches \(>= (?P<hk>\d+) refreshes\) (?P<hitches>\d+))?, "
                            r"present interval over 1\.5 / 2\.5 times the mean (?P<m15>\d+) / (?P<m25>\d+)$")
_RE_PS_CADENCE_NA = re.compile(r"cadence \?, held over \? refreshes \?, present interval over 1\.5 / 2\.5 times the mean "
                               r"(?P<m15>\d+) / (?P<m25>\d+)$")
_RE_PS_MODE = re.compile(r"; display mode (?P<hz>\d+(?:\.\d+)?) Hz(?:, (?P<note>.*))?$")
_RE_PS_END = re.compile(r" \| (?:(?P<ok>reliable)|UNRELIABLE \((?P<why>.*)\))\s*$")
_RE_PS_HITCH = re.compile(r"\[present-stats\] hitch: a frame stayed (?P<n>\d+) refreshes on screen "
                          r"\((?P<ms>\d+(?:\.\d+)?) ms\) before the next"
                          r"(?: \(cadence (?P<cadence>\d+(?:\.\d+)?) refreshes a frame: up to (?P<allowed>\d+) is normal\))?")
_RE_PS_WARN = re.compile(r"\[present-stats\] GetFrameStatistics failed")

OLD_CADENCE_TOL = 0.08          # an old-format window is trusted when its cadence (refresh clock / present rate) is 1 +- this
CADENCE_WHOLE_TOL = 0.02        # `even`: the median cadence is within this of a whole number
JUDGED_MIN_PCT = 90.0           # below this share of shown frames judged one by one, the verdict says its count is a lower bound
OLD_FORMAT_REASON = "old format: cadence unknown"
OLD_DEAD_REASON = "old format: display counters not moving"


def _f(m, name: str) -> float:
    return float(m.group(name))


def _i(m, name: str) -> int:
    return int(m.group(name))


def _oi(m, name: str) -> Optional[int]:
    v = m.group(name)
    return None if v is None or v == "?" else int(v)


def _of(m, name: str) -> Optional[float]:
    v = m.group(name)
    return None if v is None or v == "?" else float(v)


def short_reason(why: str) -> str:
    """The reason text of an UNRELIABLE window without its numbers (so equal causes group): the known reasons shortened."""
    w = why.strip()
    low = w.lower()
    if low.startswith("the display stopped taking presents"):
        return "window hidden, covered or minimised (or display off)"
    if low.startswith("getframestatistics never succeeded"):
        return "GetFrameStatistics never succeeded"
    if low.startswith("getframestatistics failed"):
        m = re.search(r"HRESULT 0x[0-9A-Fa-f]+", w)
        return "GetFrameStatistics failed" + ("" if not m else " (%s)" % m.group(0))
    if low.startswith("the display statistics restarted"):
        return "display statistics restarted (display, mode or window change)"
    if low.startswith("the counters jumped"):
        return "counters jumped"
    if low.startswith("too few presents or refreshes"):
        return "too few presents or refreshes to judge"
    if low.startswith("the refresh counter covered"):
        return "refresh counter off the window's time (outside 90 to 110 %)"
    if low.startswith("old format"):
        return w
    return re.sub(r"\s+", " ", re.sub(r"\d+(?:\.\d+)?", "N", w))[:70] or "no reason given"


def parse_present_stats(line: str) -> Optional[Dict]:
    """A `[present-stats] <secs> s: ...` window line -> dict, else None. Fields of a section that did not parse are None
    (the dict also has `partial: True`). None when the line is not a window line at all.

    Keys: secs; presents, shown, dropped, refreshes, hz, held_1, held_2, held_3plus, held_max, repeats, multi_frames,
    multi_refreshes, call_max_ms, call_over_2ms, interval_max_ms, interval_over_1_5 / _2_5 (against the refresh), stats_ok,
    stats_failed, failed, resets, hitch_unlogged (the display-side ones are None in an UNRELIABLE window), and
      format          "new" (has the cadence tail) | "old" (the first version of the line)
      reliable        True when the display numbers can be trusted: a `| reliable` window, or an old-format window whose
                      cadence is 1 (see _judge_old)
      reason          the UNRELIABLE reason text (or why an old-format window is not trusted); None when reliable
      reason_short    the reason without its numbers (short_reason); None when reliable
      cadence         refreshes a frame (None unless reliable); present_rate, clock_hz, mode_hz (the display mode), clock_note
      held_over_k     K of `held over K refreshes N` (the cadence rounded up); held_over = N, the over-cadence count
      hitch_k         L of `hitches (>= L refreshes) H` (None when the section is absent); display_hitches = H
      mean_over_1_5 / mean_over_2_5   present interval over 1.5 / 2.5 times its mean (CPU side: kept when UNRELIABLE)"""
    at = line.find("[present-stats]")
    if at < 0:
        return None
    text = line[at:].rstrip("\r\n")
    head = _RE_PS_HEAD.match(text)
    if not head:
        return None
    out: Dict = {"secs": float(head.group("secs")), "partial": False, "format": "old", "reliable": False,
                 "reason": None, "reason_short": None,
                 "presents": None, "shown": None, "dropped": None, "refreshes": None, "hz": None,
                 "held_1": None, "held_2": None, "held_3plus": None, "held_max": None, "repeats": None,
                 "multi_frames": 0, "multi_refreshes": 0, "call_max_ms": None, "call_over_2ms": None,
                 "interval_max_ms": None, "interval_over_1_5": None, "interval_over_2_5": None,
                 "stats_ok": None, "stats_failed": None, "failed": 0, "resets": 0, "hitch_unlogged": 0,
                 "cadence": None, "present_rate": None, "clock_hz": None, "mode_hz": None, "clock_note": None,
                 "held_over_k": None, "held_over": None, "hitch_k": None, "display_hitches": None,
                 "mean_over_1_5": None, "mean_over_2_5": None}
    end = _RE_PS_END.search(text)
    marker = None                                           # "reliable" | "unreliable" | None (an old line, or cut short)
    if end:
        marker = "reliable" if end.group("ok") else "unreliable"
        out["reason"] = None if marker == "reliable" else end.group("why").strip()
        text = text[:end.start()]
    sections = text[len("[present-stats] "):].split(" | ")
    sections[0] = sections[0].split(" s: ", 1)[-1]          # `10.0 s: presents ...` -> `presents ...`
    seen = 0
    cadence_seen = False
    for sec in sections:
        sec = sec.strip()
        m = _RE_PS_PRESENTS.match(sec)
        if m:
            out.update(presents=_i(m, "presents"), shown=_oi(m, "shown"), dropped=_oi(m, "dropped"))
            seen += 1
            continue
        m = _RE_PS_REFRESH.match(sec)
        if m:
            out.update(refreshes=_oi(m, "refreshes"), hz=_of(m, "hz"))
            seen += 1
            continue
        m = _RE_PS_HELD.match(sec)
        if m:
            out.update(held_1=_oi(m, "h1"), held_2=_oi(m, "h2"), held_3plus=_oi(m, "h3"), held_max=_oi(m, "hmax"),
                       repeats=_oi(m, "repeats"), multi_frames=int(m.group("mf") or 0), multi_refreshes=int(m.group("mr") or 0))
            seen += 1
            continue
        m = _RE_PS_CALL.match(sec)
        if m:
            out.update(call_max_ms=_f(m, "cmax"), call_over_2ms=_i(m, "c2"))
            seen += 1
            continue
        m = _RE_PS_INTERVAL.match(sec)
        if m:
            out.update(interval_max_ms=_f(m, "imax"), interval_over_1_5=_oi(m, "i15"), interval_over_2_5=_oi(m, "i25"))
            seen += 1
            continue
        m = _RE_PS_STATS.match(sec)
        if m:
            tail = m.group("tail")
            out.update(stats_ok=_i(m, "ok"), stats_failed=_i(m, "failed"))
            for key, rx in (("failed", _RE_PS_FAILED), ("resets", _RE_PS_RESETS), ("hitch_unlogged", _RE_PS_UNLOGGED)):
                t = rx.search(tail)
                if t:
                    out[key] = int(t.group(1))
            seen += 1
            continue
        m = _RE_PS_CADENCE.match(sec)
        if m:
            out.update(cadence=_f(m, "cadence"), present_rate=_f(m, "rate"), clock_hz=_f(m, "clock"),
                       held_over_k=_i(m, "k"), held_over=_i(m, "over"), mean_over_1_5=_i(m, "m15"), mean_over_2_5=_i(m, "m25"))
            if m.group("hk") is not None:
                out.update(hitch_k=_i(m, "hk"), display_hitches=_i(m, "hitches"))
            mode = _RE_PS_MODE.match(m.group("mode").strip())
            if mode:
                out.update(mode_hz=float(mode.group("hz")), clock_note=mode.group("note"))
            seen += 1
            cadence_seen = True
            continue
        m = _RE_PS_CADENCE_NA.match(sec)
        if m:
            out.update(mean_over_1_5=_i(m, "m15"), mean_over_2_5=_i(m, "m25"))
            seen += 1
            cadence_seen = True
    if out["presents"] is None:
        return None
    if cadence_seen or marker is not None:
        out["format"] = "new"
        out["partial"] = seen < 7
        _judge_new(out, marker)
    else:
        out["partial"] = seen < 6
        _judge_old(out)
    return out


def _judge_new(out: Dict, marker: Optional[str]) -> None:
    """Sets reliable / reason / reason_short of a window line that has the cadence tail."""
    if marker == "unreliable":
        out["reliable"] = False
        out["reason_short"] = short_reason(out["reason"] or "")
    elif marker == "reliable" and not out["partial"] and out["cadence"] is not None and out["shown"] is not None:
        out["reliable"] = True
    else:
        out["reliable"] = False
        out["reason"] = ("the line is only partly readable" if marker == "reliable"
                         else "the line ends without `reliable` / `UNRELIABLE (...)`")
        out["reason_short"] = out["reason"]


def _judge_old(out: Dict) -> None:
    """The first version of the line has no cadence. Its repeats are judged against 1 refresh, so it is trusted only when
    the refresh clock matches the present rate (cadence = clock / present rate within 1 +- OLD_CADENCE_TOL: 60 fps on 60 Hz).
    Sets present_rate and clock_hz, and when trusted reliable, cadence and the over-cadence fields (held 2 + 3+ over a cadence
    of 1, the 3+ frames as the hitches); otherwise reason / reason_short say `old format: cadence unknown`."""
    presents, secs, hz, refreshes, shown = out["presents"], out["secs"], out["hz"], out["refreshes"], out["shown"]
    rate = presents / secs if secs > 0 else 0.0
    out["present_rate"] = round(rate, 3) if rate > 0 else None
    if hz and refreshes and refreshes > 0 and rate > 0 and shown:
        out["clock_hz"] = hz
        c = hz / rate
        if out["partial"]:
            out["reason"] = OLD_FORMAT_REASON + " (the line is only partly readable)"
        elif abs(c - 1.0) <= OLD_CADENCE_TOL:
            out.update(reliable=True, cadence=round(c, 3), held_over_k=1, held_over=out["held_2"] + out["held_3plus"],
                       hitch_k=3, display_hitches=out["held_3plus"])
            return
        else:
            out["reason"] = ("%s (refresh clock %.1f Hz for %.1f presents/s = %.2f refreshes a frame; repeats were counted "
                             "against 1)" % (OLD_FORMAT_REASON, hz, rate, c))
        out["reason_short"] = OLD_FORMAT_REASON
    else:
        out["reason"] = "%s (the display counters did not move: presents but no shown frame or refresh)" % OLD_FORMAT_REASON
        out["reason_short"] = OLD_DEAD_REASON if presents >= DEAD_MIN_PRESENTS else OLD_FORMAT_REASON


def is_present_stats_window_line(line: str) -> bool:
    """True when the line has the `[present-stats] <secs> s: presents ` head (whether or not parse_present_stats could read it)."""
    at = line.find("[present-stats]")
    return at >= 0 and _RE_PS_HEAD.match(line[at:]) is not None


def parse_present_hitch(line: str) -> Optional[Dict]:
    """`[present-stats] hitch: a frame stayed N refreshes on screen (X ms) before the next[ (cadence C refreshes a frame: up
    to K is normal)]` -> {refreshes, ms, cadence, allowed} (cadence / allowed None on the older line), else None."""
    m = _RE_PS_HITCH.search(line)
    if not m:
        return None
    return {"refreshes": _i(m, "n"), "ms": _f(m, "ms"), "cadence": _of(m, "cadence"),
            "allowed": None if m.group("allowed") is None else _i(m, "allowed")}


def is_present_stats_warning(line: str) -> bool:
    return _RE_PS_WARN.search(line) is not None


# ------------------------------------------------------------------------------------------------------- [mcla-hitch]
_RE_H_HEAD = re.compile(r"\[mcla-hitch\] (?P<ms>\d+(?:\.\d+)?) ms between engine frames "
                        r"\((?P<periods>\d+(?:\.\d+)?) periods of (?P<period>\d+(?:\.\d+)?)\) in (?P<mode>[^|]+?) \| ")
_RE_H_CPU = re.compile(r"game thread on CPU (?P<cpu>\d+(?:\.\d+)?) ms \(waiting (?P<wait>\d+(?:\.\d+)?) ms, "
                       r"of which pacer sleep (?P<sleep>\d+(?:\.\d+)?) ms\)$")
_RE_H_CPU_NA = re.compile(r"game thread CPU time not measured yet \(pacer sleep (?P<sleep>\d+(?:\.\d+)?) ms\)$")
_RE_H_COMPILE = re.compile(r"pipelines \+(?P<pipelines>\d+) shaders \+(?P<shaders>\d+) in flight (?P<flight>\d+)$")
_RE_H_GPU = re.compile(r"GPU frame (?P<gpu>\d+(?:\.\d+)?) ms$")
_RE_H_PAGES = re.compile(r"page faults \+(?P<faults>\d+), read \+(?P<read>\d+(?:\.\d+)?) MiB")
_RE_H_UNLOGGED = re.compile(r"\[mcla-hitch\] (?P<n>\d+) more hitch\(es\) in the last 5 s not logged "
                            r"\(worst (?P<worst>\d+(?:\.\d+)?) ms; mcla_hitch_lines (?P<lines>\d+)\)")


def parse_hitch(line: str) -> Optional[Dict]:
    """A `[mcla-hitch] <ms> ms between engine frames ...` line -> dict (see classify_hitch), else None.

    Keys: frame_ms, periods, period_ms, mode (drive|menu|unknown, else the raw text), cpu_ms / wait_ms (None when the game
    thread CPU time was not measured), sleep_ms, pipelines / shaders / in_flight (None without SDK counters), gpu_ms (None
    when `GPU frame ?`), page_faults, read_mib, cause, text (the log line from `[mcla-hitch]` on)."""
    at = line.find("[mcla-hitch]")
    if at < 0:
        return None
    text = line[at:].rstrip("\r\n")
    head = _RE_H_HEAD.match(text)
    if not head:
        return None
    mode_text = head.group("mode").strip()
    h: Dict = {"frame_ms": _f(head, "ms"), "periods": _f(head, "periods"), "period_ms": _f(head, "period"),
               "mode": _MODE_KEY.get(mode_text, mode_text), "cpu_ms": None, "wait_ms": None, "sleep_ms": None,
               "pipelines": None, "shaders": None, "in_flight": None, "gpu_ms": None, "page_faults": 0, "read_mib": 0.0,
               "text": text}
    for sec in text[head.end():].split(" | "):
        sec = sec.strip()
        m = _RE_H_CPU.match(sec)
        if m:
            h.update(cpu_ms=_f(m, "cpu"), wait_ms=_f(m, "wait"), sleep_ms=_f(m, "sleep"))
            continue
        m = _RE_H_CPU_NA.match(sec)
        if m:
            h["sleep_ms"] = _f(m, "sleep")
            continue
        m = _RE_H_COMPILE.match(sec)
        if m:
            h.update(pipelines=_i(m, "pipelines"), shaders=_i(m, "shaders"), in_flight=_i(m, "flight"))
            continue
        m = _RE_H_GPU.match(sec)
        if m:
            h["gpu_ms"] = _f(m, "gpu")
            continue
        m = _RE_H_PAGES.match(sec)
        if m:
            h.update(page_faults=_i(m, "faults"), read_mib=_f(m, "read"))
    h["cause"] = classify_hitch(h)
    return h


def parse_hitch_unlogged(line: str) -> Optional[Dict]:
    """`[mcla-hitch] N more hitch(es) in the last 5 s not logged (worst X ms; mcla_hitch_lines 12)` -> {count, worst_ms}."""
    m = _RE_H_UNLOGGED.search(line)
    if not m:
        return None
    return {"count": _i(m, "n"), "worst_ms": _f(m, "worst")}


def classify_hitch(h: Dict) -> str:
    """compile | cpu-bound | paging/io | blocked (first match wins; see the module doc)."""
    if (h.get("pipelines") or 0) > 0 or (h.get("shaders") or 0) > 0:
        return "compile"
    frame = h.get("frame_ms") or 0.0
    if h.get("cpu_ms") is not None and frame > 0 and h["cpu_ms"] >= CPU_BOUND_FRAC * frame:
        return "cpu-bound"
    if (h.get("page_faults") or 0) >= PAGE_FAULTS_IO or (h.get("read_mib") or 0.0) >= READ_MIB_IO:
        return "paging/io"
    return "blocked"


def is_gpu_bound(h: Dict) -> bool:
    """A hitch whose GPU frame time covers at least 80 % of the frame (a sub-count of `blocked`)."""
    frame = h.get("frame_ms") or 0.0
    return h.get("gpu_ms") is not None and frame > 0 and h["gpu_ms"] >= GPU_BOUND_FRAC * frame


# ----------------------------------------------------------------------------------------------------------- totals
def display_verdict(reliable_windows: int, cadence: Optional[float], held_over: int, reliable_seconds: float,
                    judged_pct: Optional[float] = None):
    """(key, phrase) of a run / drive window: even | beat by design | repeats | unknown (see the module doc). judged_pct: the
    share of the shown frames that were judged one by one (the rest was sampled together); below JUDGED_MIN_PCT the phrase says
    so, because the over-cadence count is then a lower bound."""
    if not reliable_windows or cadence is None:
        return "unknown", "unknown (no reliable window)"
    caveat = "" if judged_pct is None or judged_pct >= JUDGED_MIN_PCT else (
        "; only %.0f %% of the shown frames were judged one by one, the count is a lower bound" % judged_pct)
    if held_over > 0:
        per_min = held_over / (reliable_seconds / 60.0) if reliable_seconds > 0 else 0.0
        return "repeats", "repeats (%s frame%s held over the cadence, %.1f a minute; cadence %.2f%s)" % (
            _n(held_over), "" if held_over == 1 else "s", per_min, cadence, caveat)
    whole = int(round(cadence))
    if whole >= 1 and abs(cadence - whole) <= CADENCE_WHOLE_TOL + 1e-9:        # (the cadence prints with 2 decimals)
        return "even", "even (cadence %.2f, no frame held over it%s)" % (cadence, caveat)
    if cadence < 1.0:
        return "beat by design", "beat by design (cadence %.2f: more presents than refreshes, some presents cannot be shown%s)" % (
            cadence, caveat)
    return "beat by design", "beat by design (cadence %.2f is not a whole number: frames stay %d or %d refreshes, not repeats%s)" % (
        cadence, int(math.floor(cadence)), int(math.ceil(cadence)), caveat)


def aggregate_display(windows: Sequence[Dict], hitches: Sequence[Dict] = (), warnings: Sequence[str] = (),
                      unparsed: int = 0) -> Optional[Dict]:
    """Totals over `[present-stats]` window dicts (parse_present_stats) and `hitch:` dicts (parse_present_hitch).
    None when there is nothing (no window and no hitch line).

    The display-side totals (presents, shown, dropped, refreshes, hz = the measured refresh clock, cadence, held_*, held_over,
    display_hitches, repeats_min, interval_over_1_5 / _2_5) cover the RELIABLE windows only; `reliable_windows` /
    `unreliable_windows` count the two kinds (`unreliable_reasons` = [[short reason, windows]...] most common first). The
    CPU-side numbers (presents_all, call_max_ms, interval_max_ms, stats_*, interval_over_mean_*) and the log-event counts
    (hitch_lines, hitch_unlogged, counter_resets, presents_failed) cover every window. `verdict` / `verdict_text`: display_verdict()."""
    if not windows and not hitches and not warnings and not unparsed:
        return None
    rel = [w for w in windows if w.get("reliable")]
    unrel = [w for w in windows if not w.get("reliable")]

    def total(key: str, ws: Sequence[Dict] = rel) -> int:
        return sum(w.get(key) or 0 for w in ws)

    def worst(key: str, ws: Sequence[Dict]) -> float:
        return max([w[key] for w in ws if w.get(key) is not None] or [0.0])

    clocks = [w["hz"] for w in rel if w.get("hz")]
    cadences = [w["cadence"] for w in rel if w.get("cadence") is not None]
    modes = [w["mode_hz"] for w in rel if w.get("mode_hz")]
    differs = sum(1 for w in rel if w.get("mode_hz") and w.get("clock_hz") and abs(w["clock_hz"] - w["mode_hz"]) > 0.005 * w["mode_hz"])
    rel_secs = sum(w["secs"] for w in rel)
    shown = total("shown")
    attributed = total("held_1") + total("held_2") + total("held_3plus")
    held_over = total("held_over")
    dead = [w for w in windows if w.get("shown") is not None and (w["presents"] or 0) >= DEAD_MIN_PRESENTS
            and ((w["shown"] or 0) == 0 or (w["refreshes"] or 0) == 0)]
    reasons = Counter(w.get("reason_short") or "no reason given" for w in unrel).most_common()
    top = max(hitches, key=lambda x: (x["refreshes"], x["ms"]), default=None)
    cadence = None if not cadences else round(statistics.median(cadences), 3)
    judged_pct = None if shown <= 0 else round(100.0 * attributed / shown, 1)
    verdict, verdict_text = display_verdict(len(rel), cadence, held_over, rel_secs, judged_pct)
    return {
        "windows": len(windows), "seconds": round(sum(w["secs"] for w in windows), 1),
        "reliable_windows": len(rel), "unreliable_windows": len(unrel), "reliable_seconds": round(rel_secs, 1),
        "unreliable_reasons": [[r, n] for r, n in reasons[:5]], "unreliable_top": None if not reasons else reasons[0][0],
        "old_format_windows": sum(1 for w in windows if w.get("format") == "old"),
        "old_format_trusted": sum(1 for w in windows if w.get("format") == "old" and w.get("reliable")),
        "presents": total("presents"), "presents_all": total("presents", windows),
        "shown": shown, "dropped": total("dropped"), "refreshes": total("refreshes"),
        "hz": None if not clocks else round(statistics.median(clocks), 3),
        "mode_hz": None if not modes else round(statistics.median(modes), 3), "clock_differs_windows": differs,
        "cadence": cadence, "cadence_min": None if not cadences else min(cadences),
        "cadence_max": None if not cadences else max(cadences),
        "held_1": total("held_1"), "held_2": total("held_2"), "held_3plus": total("held_3plus"),
        "held_max": int(worst("held_max", rel)), "attributed": attributed, "judged_pct": judged_pct,
        "held_over": held_over, "held_over_per_min": 0.0 if rel_secs <= 0 else round(held_over / (rel_secs / 60.0), 2),
        "display_hitches": total("display_hitches"),
        "repeats_min": total("repeats"), "multi_frames": total("multi_frames"), "multi_refreshes": total("multi_refreshes"),
        "call_max_ms": worst("call_max_ms", windows), "call_over_2ms": total("call_over_2ms", windows),
        "interval_max_ms": worst("interval_max_ms", windows), "interval_over_1_5": total("interval_over_1_5"),
        "interval_over_2_5": total("interval_over_2_5"),
        "interval_over_mean_1_5": total("mean_over_1_5", windows), "interval_over_mean_2_5": total("mean_over_2_5", windows),
        "interval_over_mean_windows": sum(1 for w in windows if w.get("mean_over_1_5") is not None),     # new-format lines only
        "stats_ok": total("stats_ok", windows), "stats_failed": total("stats_failed", windows),
        "presents_failed": total("failed", windows), "counter_resets": total("resets", windows),
        "no_stats_windows": len(dead), "partial_windows": sum(1 for w in windows if w.get("partial")),
        "hitch_lines": len(hitches), "hitch_unlogged": total("hitch_unlogged", windows),
        "hitch_worst_refreshes": 0 if top is None else top["refreshes"],
        "hitch_worst_ms": 0.0 if top is None else top["ms"],
        "verdict": verdict, "verdict_text": verdict_text,
        "warnings": list(warnings)[:3], "unparsed": unparsed,
    }


def aggregate_hitches(hitches: Sequence[Dict], unlogged: Sequence[Dict] = ()) -> Optional[Dict]:
    """Totals over the parsed `[mcla-hitch]` lines (and their `N more hitch(es) ... not logged` summaries).
    The by-mode and by-cause counts cover the logged lines only. None when there is nothing."""
    if not hitches and not unlogged:
        return None
    by_mode: Dict[str, int] = {m: 0 for m in MODES}
    by_cause: Dict[str, int] = {c: 0 for c in CAUSES}
    gpu_bound = 0
    for h in hitches:
        by_mode[h["mode"]] = by_mode.get(h["mode"], 0) + 1
        by_cause[h["cause"]] += 1
        if h["cause"] == "blocked" and is_gpu_bound(h):
            gpu_bound += 1
    worst = max(hitches, key=lambda x: x["frame_ms"], default=None)
    unl_worst = max((u["worst_ms"] for u in unlogged), default=0.0)
    return {
        "logged": len(hitches), "unlogged": sum(u["count"] for u in unlogged), "unlogged_worst_ms": unl_worst,
        "by_mode": by_mode, "by_cause": by_cause, "blocked_gpu": gpu_bound,
        "worst_ms": 0.0 if worst is None else worst["frame_ms"],
        "worst_line": None if worst is None else worst["text"],
        "worst_t": None if worst is None else worst.get("t"),
    }


# ---------------------------------------------------------------------------------------------------------- printing
def _n(v: int) -> str:
    return "{:,}".format(v)


def display_line(d: Optional[Dict], label: str) -> str:
    """One printable line for aggregate_display() output: the verdict first, then reliable vs unreliable windows, the display
    totals of the reliable windows, the CPU-side numbers of all windows."""
    if d is None:
        return "display (%s): no [present-stats] lines" % label
    if not d["windows"]:
        return "display (%s): no [present-stats] window line%s" % (
            label, "" if not d["hitch_lines"] else "; %d hitch line(s), worst %d refreshes" % (d["hitch_lines"], d["hitch_worst_refreshes"]))
    nrel, nun = d["reliable_windows"], d["unreliable_windows"]
    wins = "windows %d reliable%s, %d unreliable" % (nrel, "" if not nrel else " (%.0f s)" % d["reliable_seconds"], nun)
    if nun:
        reason, count = d["unreliable_reasons"][0]
        wins += " (most common: %s%s)" % (reason, "" if count == nun else ", %d of %d" % (count, nun))
    parts = ["verdict " + d["verdict_text"], wins]
    if nrel:
        parts.append("presents %s shown %s dropped %s%s" % (
            _n(d["presents"]), _n(d["shown"]), _n(d["dropped"]),
            "" if d["presents"] == d["presents_all"] else " (reliable windows, %s in all)" % _n(d["presents_all"])))
        clock = "refresh clock %s" % ("?" if d["hz"] is None else "%.3f Hz" % d["hz"])
        if d["mode_hz"]:
            clock += " (display mode %.0f Hz%s)" % (d["mode_hz"], "" if not d["clock_differs_windows"] else
                                                    ", the clock differs from it in %d of %d windows: variable refresh" % (
                                                        d["clock_differs_windows"], nrel))
        parts.append(clock)
        cad = "cadence %.2f refreshes a frame (median" % d["cadence"]
        if d["cadence_min"] != d["cadence_max"]:
            cad += ", windows %.2f to %.2f" % (d["cadence_min"], d["cadence_max"])
        parts.append(cad + ")")
        parts.append("shown frames held 1 refresh %s, 2 %s, 3+ %s (worst %d)" % (_n(d["held_1"]), _n(d["held_2"]), _n(d["held_3plus"]),
                                                                              d["held_max"]))
        over = "held over the cadence %s (%.1f a minute), repeats >= %s" % (_n(d["held_over"]), d["held_over_per_min"],
                                                                         _n(d["repeats_min"]))
        if d["multi_frames"]:
            over += " (+%s frames over %s refreshes sampled together, not attributed)" % (_n(d["multi_frames"]), _n(d["multi_refreshes"]))
        parts.append(over)
        parts.append("display hitches %s" % _n(d["display_hitches"]))
    else:
        parts.append("presents %s (all windows; no display numbers)" % _n(d["presents_all"]))
    parts.append("hitch lines %s logged%s%s" % (
        _n(d["hitch_lines"]), "" if not d["hitch_unlogged"] else " + %s not logged" % _n(d["hitch_unlogged"]),
        "" if not d["hitch_lines"] else ", worst %d refreshes (%.1f ms)" % (d["hitch_worst_refreshes"], d["hitch_worst_ms"])))
    cpu = "present call max %.2f ms (over 2 ms %s), interval max %.1f ms" % (d["call_max_ms"], _n(d["call_over_2ms"]), d["interval_max_ms"])
    if d["interval_over_mean_windows"]:                                  # the old line format has no such count
        cpu += ", over 1.5 / 2.5 times the mean %s / %s" % (_n(d["interval_over_mean_1_5"]), _n(d["interval_over_mean_2_5"]))
    if nrel and d["cadence"] is not None and d["cadence"] < 1.5:      # against the refresh: a missed vblank only when a frame is ~1 refresh
        cpu += ", over 1.5 / 2.5 refreshes %s / %s" % (_n(d["interval_over_1_5"]), _n(d["interval_over_2_5"]))
    parts.append(cpu)
    extra = []
    if d["old_format_windows"]:
        extra.append("%d of %d windows in the old line format (no cadence tail): %d trusted (refresh clock = present rate), "
                     "%d with the cadence unknown" % (d["old_format_windows"], d["windows"], d["old_format_trusted"],
                                                     d["old_format_windows"] - d["old_format_trusted"]))
    if d["stats_failed"] or d["presents_failed"] or d["counter_resets"]:
        extra.append("stats failed %d, presents FAILED %d, counter resets %d" % (d["stats_failed"], d["presents_failed"],
                                                                                d["counter_resets"]))
    if d["partial_windows"]:
        extra.append("%d window line(s) only partly readable" % d["partial_windows"])
    if d["unparsed"]:
        extra.append("%d window line(s) UNREADABLE" % d["unparsed"])
    if d["warnings"]:
        extra.append("warning: " + d["warnings"][0][:160])
    return "display (%s, %d windows, %.0f s): %s%s" % (label, d["windows"], d["seconds"], " | ".join(parts),
                                                      "".join(" | " + e for e in extra))


def hitch_line(h: Optional[Dict], label: str) -> str:
    """One printable line for aggregate_hitches() output (the worst hitch is a second line from hitch_worst_line)."""
    if h is None:
        return "hitches (%s): no [mcla-hitch] lines (no long frames, or a build without them)" % label
    bm, bc = h["by_mode"], h["by_cause"]
    modes = ", ".join("%s %d" % (m, bm[m]) for m in MODES) + "".join(", %s %d" % (m, n) for m, n in bm.items() if m not in MODES)
    blocked = "blocked %d" % bc["blocked"] + ("" if not h["blocked_gpu"] else " (GPU frame >= %d%% of it: %d)" % (
        round(GPU_BOUND_FRAC * 100), h["blocked_gpu"]))
    return "hitches (%s): %d logged%s | by screen: %s | cause: compile %d, cpu-bound %d, paging/io %d, %s" % (
        label, h["logged"], "" if not h["unlogged"] else " + %d not logged (worst %.1f ms)" % (h["unlogged"], h["unlogged_worst_ms"]),
        modes, bc["compile"], bc["cpu-bound"], bc["paging/io"], blocked)


def hitch_worst_line(h: Optional[Dict]) -> Optional[str]:
    if h is None or not h["worst_line"]:
        return None
    return "  worst hitch%s: %s" % ("" if h["worst_t"] is None else " at +%.1f s" % h["worst_t"], h["worst_line"])


def summary_lines(display: Dict, hitches: Dict) -> List[str]:
    """The printed lines for a parsed run: `display` / `hitches` as run_game.LogScanner.result() gives them
    ({"run": ..., "drive": ..., "drive_from_s": ...})."""
    out: List[str] = []
    fr = display.get("drive_from_s", hitches.get("drive_from_s"))
    drive_note = None if fr is None else "drive window, from free roam at +%.0f s" % fr
    out.append(display_line(display["run"], "whole run"))
    if drive_note is not None and display["run"] is not None:
        out.append(display_line(display["drive"], drive_note))
    out.append(hitch_line(hitches["run"], "whole run"))
    w = hitch_worst_line(hitches["run"])
    if w:
        out.append(w)
    if drive_note is not None and hitches["run"] is not None:
        out.append(hitch_line(hitches["drive"], drive_note))
        w = hitch_worst_line(hitches["drive"])
        if w:
            out.append(w)
    return out


def note_text(display: Optional[Dict], hitches: Optional[Dict]) -> List[str]:
    """Short METRICS notes (bench.py) from aggregate_display() / aggregate_hitches() over the measured window; [] when neither.
    The notes are joined with `; ` by the caller, so none contains one. The display note leads with the verdict and covers the
    reliable windows only; unreliable windows are counted with their most common reason."""
    notes: List[str] = []
    if display is not None and display["windows"]:
        d = display
        if not d["reliable_windows"]:
            notes.append("display: verdict unknown, no reliable window (%d unreliable, most common: %s), %s presents" % (
                d["unreliable_windows"], d["unreliable_top"], _n(d["presents_all"])))
        else:
            txt = "display: verdict %s, %d shown, held 2 %d / 3+ %d (worst %d), over cadence %d, repeats >= %d over %.0f s" % (
                d["verdict"], d["shown"], d["held_2"], d["held_3plus"], d["held_max"], d["held_over"], d["repeats_min"],
                d["reliable_seconds"])
            if d["dropped"]:
                txt += ", %d dropped" % d["dropped"]
            if d["cadence"] is not None:
                txt += ", cadence %.2f" % d["cadence"]
            if d["hz"]:
                txt += " on a %.0f Hz clock" % d["hz"]
            if d["display_hitches"]:
                txt += ", display hitches %d" % d["display_hitches"]
            if d["multi_frames"]:
                txt += " (%d frames sampled together)" % d["multi_frames"]
            if d["unreliable_windows"]:
                txt += " (%d of %d windows unreliable: %s)" % (d["unreliable_windows"], d["windows"], d["unreliable_top"])
            notes.append(txt)
    if hitches is None and display is not None and display["windows"]:
        notes.append("hitches 0")                       # a build that logs `[present-stats]` also logs `[mcla-hitch]`
    if hitches is not None:
        bc = hitches["by_cause"]
        txt = "hitches %d" % hitches["logged"]
        if hitches["unlogged"]:
            txt += "+%d unlogged" % hitches["unlogged"]
        if hitches["logged"]:
            txt += " (compile %d, cpu-bound %d, paging/io %d, blocked %d)" % (bc["compile"], bc["cpu-bound"], bc["paging/io"],
                                                                              bc["blocked"])
        notes.append(txt)
    return notes
