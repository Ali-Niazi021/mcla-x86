#!/usr/bin/env python3
"""test_log_parse.py - tests of the `[present-stats]` / `[mcla-hitch]` log parsing (stdlib unittest, no game, no lock).

    python tools/test_log_parse.py            (or: python -m unittest tools/test_log_parse.py -v)

Covers tools/display_log.py (parsers, classifier, totals, printed lines), the run summary of tools/run_game.py (LogScanner /
parse_log / --summarize, whole run and drive window, a log without these lines) and the METRICS notes of tools/bench.py
(scan_log, log_display, make_row, pooled).
"""

import contextlib
import datetime
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import bench  # noqa: E402
import display_log as dl  # noqa: E402
import run_game  # noqa: E402

PREFIX = "[2026-11-27 01:57:%02d.%03d] [info] [core] [t4124] "


def stamp(sec: float) -> str:
    """The log prefix of a line `sec` seconds after 01:57:00 (so sec 30.5 -> 01:57:30.500)."""
    whole = int(sec)
    return PREFIX % (whole, int(round((sec - whole) * 1000)))


# The exact samples of the task (one physical line each).
PS_WINDOW = ("[present-stats] 10.0 s: presents 600 shown 598 dropped 2 | refreshes 600 at 59.996 Hz | shown frames held 1 "
             "refresh 590, 2 6, 3+ 2 (max 4), repeats >= 10 | present call max 0.40 ms, over 2 ms 0 | present interval max "
             "18.1 ms, over 1.5 / 2.5 refreshes 3 / 1 | stats 600 ok 0 failed")
PS_MULTI = PS_WINDOW.replace("repeats >= 10 |", "repeats >= 10 (+20 frames over 49 refreshes sampled together) |")
PS_TAILS = PS_WINDOW + ", 3 presents FAILED, 2 counter resets, 7 more hitches not logged"
PS_HITCH = "[present-stats] hitch: a frame stayed 4 refreshes on screen (66.7 ms) before the next"
PS_HITCH_NEW = PS_HITCH + " (cadence 2.75 refreshes a frame: up to 3 is normal)"
# The SDK 0750 line with the cadence tail (real samples of the 165 Hz panel, one physical line each). PS_WINDOW above is the
# first version of the line (no tail): its refresh clock equals its present rate (60 Hz at 60 fps), so it is trusted.
PS_NEW = ("[present-stats] 10.0 s: presents 600 shown 600 dropped 0 | refreshes 1651 at 165.001 Hz | shown frames held 1 refresh "
          "0, 2 149, 3+ 451 (max 3), repeats >= 0 | present call max 2.73 ms, over 2 ms 1 | present interval max 19.9 ms, over "
          "1.5 / 2.5 refreshes 600 / 598 | stats 600 ok 0 failed | cadence 2.75 refreshes a frame (60.0 presents/s on a "
          "165.001 Hz refresh clock; display mode 165 Hz), held over 3 refreshes 0, hitches (>= 5 refreshes) 0, present "
          "interval over 1.5 / 2.5 times the mean 0 / 0 | reliable")
PS_NEW_REPEATS = ("[present-stats] 10.0 s: presents 599 shown 599 dropped 0 | refreshes 1653 at 165.001 Hz | shown frames held 1 "
                  "refresh 2, 2 150, 3+ 443 (max 7), repeats >= 10 (+4 frames over 12 refreshes sampled together) | present "
                  "call max 0.95 ms, over 2 ms 0 | present interval max 44.1 ms, over 1.5 / 2.5 refreshes 596 / 591 | stats 599 "
                  "ok 0 failed | cadence 2.76 refreshes a frame (59.8 presents/s on a 165.001 Hz refresh clock; display mode "
                  "165 Hz), held over 3 refreshes 7, hitches (>= 5 refreshes) 1, present interval over 1.5 / 2.5 times the "
                  "mean 4 / 1 | reliable")
PS_NEW_VRR = ("[present-stats] 10.0 s: presents 601 shown 601 dropped 0 | refreshes 1589 at 158.509 Hz | shown frames held 1 "
              "refresh 0, 2 150, 3+ 451 (max 3), repeats >= 0 | present call max 0.54 ms, over 2 ms 0 | present interval max "
              "17.1 ms, over 1.5 / 2.5 refreshes 601 / 601 | stats 601 ok 0 failed | cadence 2.64 refreshes a frame (60.0 "
              "presents/s on a 158.509 Hz refresh clock; display mode 165 Hz, the clock differs from it, as with variable "
              "refresh), held over 3 refreshes 0, hitches (>= 5 refreshes) 0, present interval over 1.5 / 2.5 times the mean "
              "0 / 0 | reliable")
PS_NEW_60HZ = ("[present-stats] 10.0 s: presents 600 shown 600 dropped 0 | refreshes 600 at 60.000 Hz | shown frames held 1 "
               "refresh 600, 2 0, 3+ 0 (max 1), repeats >= 0 | present call max 0.40 ms, over 2 ms 0 | present interval max "
               "17.5 ms, over 1.5 / 2.5 refreshes 0 / 0 | stats 600 ok 0 failed | cadence 1.00 refreshes a frame (60.0 "
               "presents/s on a 60.000 Hz refresh clock; display mode 60 Hz), held over 1 refreshes 0, hitches (>= 3 refreshes) "
               "0, present interval over 1.5 / 2.5 times the mean 0 / 0 | reliable")
PS_UNRELIABLE = ("[present-stats] 10.0 s: presents 597 shown ? dropped ? | refreshes ? at ? Hz | shown frames held 1 refresh ?, "
                 "2 ?, 3+ ? (max ?), repeats >= ? | present call max 2.32 ms, over 2 ms 1 | present interval max 52.2 ms, over "
                 "1.5 / 2.5 refreshes ? / ? | stats 597 ok 0 failed | cadence ?, held over ? refreshes ?, present interval "
                 "over 1.5 / 2.5 times the mean 2 / 1 | UNRELIABLE (the display stopped taking presents for part of the window "
                 "(29 presents queued, 597 of 597 retired): the window was hidden, covered or minimised, or the display was "
                 "off)")
PS_UNRELIABLE_HRESULT = (PS_UNRELIABLE.split(" | UNRELIABLE")[0] +
                         " | UNRELIABLE (GetFrameStatistics failed 3 times (HRESULT 0x887A0001))")
H_MENU = ("[mcla-hitch] 183.4 ms between engine frames (11.0 periods of 16.7) in a menu / pause / load | game thread on CPU "
          "12.3 ms (waiting 171.1 ms, of which pacer sleep 4.2 ms) | pipelines +148 shaders +0 in flight 3 | GPU frame 6.7 ms | "
          "page faults +2340, read +12.4 MiB")
H_DRIVE_CPU = ("[mcla-hitch] 38.4 ms between engine frames (2.3 periods of 16.7) in a drive | game thread on CPU 27.1 ms "
               "(waiting 11.3 ms, of which pacer sleep 10.5 ms) | pipelines +0 shaders +0 in flight 0 | GPU frame 6.1 ms | "
               "page faults +0, read +0.0 MiB")
H_UNKNOWN_NA = ("[mcla-hitch] 40.0 ms between engine frames (2.4 periods of 16.7) in an unknown screen | game thread CPU time "
                "not measured yet (pacer sleep 0.0 ms) | pipelines ? (no SDK counters) | GPU frame ? | page faults +5, "
                "read +0.0 MiB")
H_GPU_BOUND = ("[mcla-hitch] 36.1 ms between engine frames (2.2 periods of 16.7) in a drive | game thread on CPU 1.8 ms "
               "(waiting 34.3 ms, of which pacer sleep 0.0 ms) | pipelines +0 shaders +0 in flight 0 | GPU frame 34.7 ms | "
               "page faults +666, read +0.1 MiB")
H_UNLOGGED = "[mcla-hitch] 5 more hitch(es) in the last 5 s not logged (worst 91.3 ms; mcla_hitch_lines 12)"


class PresentStatsParse(unittest.TestCase):
    def test_window_line(self):
        w = dl.parse_present_stats(stamp(1) + PS_WINDOW)
        self.assertEqual((w["secs"], w["presents"], w["shown"], w["dropped"]), (10.0, 600, 598, 2))
        self.assertEqual((w["refreshes"], w["hz"]), (600, 59.996))
        self.assertEqual((w["held_1"], w["held_2"], w["held_3plus"], w["held_max"], w["repeats"]), (590, 6, 2, 4, 10))
        self.assertEqual((w["multi_frames"], w["multi_refreshes"]), (0, 0))
        self.assertEqual((w["call_max_ms"], w["call_over_2ms"]), (0.40, 0))
        self.assertEqual((w["interval_max_ms"], w["interval_over_1_5"], w["interval_over_2_5"]), (18.1, 3, 1))
        self.assertEqual((w["stats_ok"], w["stats_failed"], w["failed"], w["resets"], w["hitch_unlogged"]), (600, 0, 0, 0, 0))
        self.assertFalse(w["partial"])
        self.assertEqual(w["format"], "old")                    # no cadence tail: the first version of the line
        self.assertTrue(w["reliable"])                          # ... but its clock (59.996 Hz) is its present rate (60 a second)
        self.assertAlmostEqual(w["cadence"], 1.0, places=2)
        self.assertEqual((w["held_over_k"], w["held_over"], w["hitch_k"], w["display_hitches"]), (1, 8, 3, 2))   # 2 + 3+ over 1; 3+

    def test_sampled_together_variant(self):
        w = dl.parse_present_stats(PS_MULTI)
        self.assertEqual((w["repeats"], w["multi_frames"], w["multi_refreshes"]), (10, 20, 49))
        self.assertEqual(w["call_max_ms"], 0.40)               # the sections after it still parse
        self.assertFalse(w["partial"])

    def test_tails(self):
        w = dl.parse_present_stats(PS_TAILS)
        self.assertEqual((w["stats_ok"], w["stats_failed"], w["failed"], w["resets"], w["hitch_unlogged"]), (600, 0, 3, 2, 7))
        w = dl.parse_present_stats(PS_WINDOW + ", 1 counter resets")
        self.assertEqual((w["failed"], w["resets"], w["hitch_unlogged"]), (0, 1, 0))
        w = dl.parse_present_stats(PS_WINDOW + ", 39 more hitches not logged")
        self.assertEqual((w["failed"], w["resets"], w["hitch_unlogged"]), (0, 0, 39))

    def test_real_line_with_failed_stats(self):
        line = ("[present-stats] 10.0 s: presents 983 shown 761 dropped 222 | refreshes 1049 at 165.001 Hz | shown frames held "
                "1 refresh 68, 2 1, 3+ 0 (max 2), repeats >= 1 (+692 frames over 979 refreshes sampled together) | present "
                "call max 12.48 ms, over 2 ms 11 | present interval max 31.6 ms, over 1.5 / 2.5 refreshes 0 / 0 | stats 982 ok "
                "1 failed, 1 counter resets")
        w = dl.parse_present_stats(line)
        self.assertEqual((w["presents"], w["shown"], w["dropped"], w["stats_ok"], w["stats_failed"], w["resets"]),
                         (983, 761, 222, 982, 1, 1))
        self.assertEqual((w["multi_frames"], w["multi_refreshes"], w["hz"]), (692, 979, 165.001))
        # 165 Hz at 98 presents a second: the first version counted repeats against 1 refresh, so they cannot be trusted
        self.assertEqual((w["format"], w["reliable"], w["cadence"]), ("old", False, None))
        self.assertEqual(w["reason_short"], "old format: cadence unknown")
        self.assertIn("165.0 Hz for 98.3 presents/s", w["reason"])

    def test_old_format_dead_window(self):
        w = dl.parse_present_stats("[present-stats] 10.0 s: presents 601 shown 0 dropped 601 | refreshes 0 at 165.001 Hz | shown "
                                   "frames held 1 refresh 0, 2 0, 3+ 0 (max 0), repeats >= 0 | present call max 3.64 ms, over 2 ms "
                                   "1 | present interval max 21.7 ms, over 1.5 / 2.5 refreshes 601 / 597 | stats 601 ok 0 failed")
        self.assertEqual((w["format"], w["reliable"]), ("old", False))
        self.assertEqual(w["reason_short"], "old format: display counters not moving")

    def test_old_format_cadence_tolerance(self):
        def trusted(hz, presents=600):
            line = PS_WINDOW.replace("refreshes 600 at 59.996 Hz", "refreshes 600 at %.3f Hz" % hz).replace("presents 600", "presents %d" % presents)
            return dl.parse_present_stats(line)["reliable"]
        self.assertTrue(trusted(60.0))
        self.assertTrue(trusted(64.5))                          # 1.075 refreshes a frame
        self.assertFalse(trusted(66.0))                         # 1.10
        self.assertFalse(trusted(120.0))                        # 2.0: a 30 fps cadence cannot be told from repeats
        self.assertFalse(trusted(59.996, presents=1200))        # 0.5: presents faster than the refresh clock

    def test_not_a_window_line(self):
        self.assertIsNone(dl.parse_present_stats(stamp(1) + PS_HITCH))
        self.assertIsNone(dl.parse_present_stats(stamp(1) + "[mcla-sim] 5.0 s real: world 5.0 s"))
        self.assertIsNone(dl.parse_present_stats(""))

    def test_a_changed_section_leaves_the_rest(self):
        broken = PS_WINDOW.replace("present call max 0.40 ms, over 2 ms 0", "present call 0.40 ms")
        w = dl.parse_present_stats(broken)
        self.assertTrue(w["partial"])
        self.assertIsNone(w["call_max_ms"])
        self.assertEqual((w["presents"], w["held_2"], w["interval_max_ms"]), (600, 6, 18.1))

    def test_unreadable_head(self):
        garbled = "[present-stats] 10.0 s: presents lots shown many"
        self.assertIsNone(dl.parse_present_stats(garbled))
        self.assertTrue(dl.is_present_stats_window_line(garbled))
        self.assertFalse(dl.is_present_stats_window_line(PS_HITCH))

    def test_hitch_line(self):
        self.assertEqual(dl.parse_present_hitch(stamp(2) + PS_HITCH), {"refreshes": 4, "ms": 66.7, "cadence": None, "allowed": None})
        self.assertEqual(dl.parse_present_hitch("[present-stats] hitch: a frame stayed 3 refreshes on screen (0.0 ms) before "
                                                "the next"), {"refreshes": 3, "ms": 0.0, "cadence": None, "allowed": None})
        self.assertEqual(dl.parse_present_hitch(stamp(2) + PS_HITCH_NEW), {"refreshes": 4, "ms": 66.7, "cadence": 2.75, "allowed": 3})
        self.assertIsNone(dl.parse_present_hitch(PS_WINDOW))

    def test_new_reliable_window(self):
        w = dl.parse_present_stats(stamp(1) + PS_NEW)
        self.assertEqual((w["format"], w["reliable"], w["partial"], w["reason"], w["reason_short"]), ("new", True, False, None, None))
        self.assertEqual((w["presents"], w["shown"], w["dropped"], w["refreshes"], w["hz"]), (600, 600, 0, 1651, 165.001))
        self.assertEqual((w["held_1"], w["held_2"], w["held_3plus"], w["held_max"], w["repeats"]), (0, 149, 451, 3, 0))
        self.assertEqual((w["cadence"], w["present_rate"], w["clock_hz"], w["mode_hz"], w["clock_note"]), (2.75, 60.0, 165.001, 165.0, None))
        self.assertEqual((w["held_over_k"], w["held_over"], w["hitch_k"], w["display_hitches"]), (3, 0, 5, 0))
        self.assertEqual((w["mean_over_1_5"], w["mean_over_2_5"], w["interval_over_1_5"], w["interval_over_2_5"]), (0, 0, 600, 598))
        self.assertEqual((w["stats_ok"], w["stats_failed"]), (600, 0))
        self.assertEqual((w["call_max_ms"], w["call_over_2ms"], w["interval_max_ms"]), (2.73, 1, 19.9))
        w = dl.parse_present_stats(PS_NEW_REPEATS)
        self.assertEqual((w["held_over"], w["display_hitches"], w["repeats"], w["multi_frames"], w["multi_refreshes"]), (7, 1, 10, 4, 12))
        self.assertEqual((w["mean_over_1_5"], w["mean_over_2_5"]), (4, 1))
        self.assertFalse(w["partial"])

    def test_new_window_variants(self):
        w = dl.parse_present_stats(PS_NEW_VRR)                  # the clock is not the display mode: variable refresh
        self.assertEqual((w["reliable"], w["clock_hz"], w["mode_hz"]), (True, 158.509, 165.0))
        self.assertTrue(w["clock_note"].startswith("the clock differs from it"))
        no_mode = dl.parse_present_stats(PS_NEW.replace("; display mode 165 Hz", ""))
        self.assertEqual((no_mode["reliable"], no_mode["cadence"], no_mode["mode_hz"]), (True, 2.75, None))
        no_hitches = dl.parse_present_stats(PS_NEW.replace(", hitches (>= 5 refreshes) 0", ""))    # present_stats_hitch_refreshes 0
        self.assertEqual((no_hitches["reliable"], no_hitches["held_over"], no_hitches["hitch_k"], no_hitches["display_hitches"]),
                         (True, 0, None, None))
        sixty = dl.parse_present_stats(PS_NEW_60HZ)
        self.assertEqual((sixty["reliable"], sixty["cadence"], sixty["held_over_k"], sixty["hitch_k"]), (True, 1.0, 1, 3))
        tails = dl.parse_present_stats(PS_NEW.replace("stats 600 ok 0 failed", "stats 599 ok 1 failed, 3 presents FAILED, 2 counter "
                                                                              "resets, 7 more hitches not logged"))
        self.assertEqual((tails["stats_ok"], tails["stats_failed"], tails["failed"], tails["resets"], tails["hitch_unlogged"]),
                         (599, 1, 3, 2, 7))
        self.assertTrue(tails["reliable"])

    def test_unreliable_window(self):
        line = stamp(1) + PS_UNRELIABLE
        self.assertTrue(dl.is_present_stats_window_line(line))
        w = dl.parse_present_stats(line)
        self.assertIsNotNone(w)                                 # counted as an unreliable window, not as unreadable
        self.assertEqual((w["format"], w["reliable"], w["partial"]), ("new", False, False))
        self.assertTrue(w["reason"].startswith("the display stopped taking presents for part of the window (29 presents queued, "
                                               "597 of 597 retired): the window was hidden"))
        self.assertTrue(w["reason"].endswith("or the display was off"))
        self.assertEqual(w["reason_short"], "window hidden, covered or minimised (or display off)")
        for key in ("shown", "dropped", "refreshes", "hz", "held_1", "held_2", "held_3plus", "held_max", "repeats", "interval_over_1_5",
                    "interval_over_2_5", "cadence", "held_over_k", "held_over", "hitch_k", "display_hitches"):
            self.assertIsNone(w[key], key)
        # the CPU side stays
        self.assertEqual((w["secs"], w["presents"], w["call_max_ms"], w["call_over_2ms"], w["interval_max_ms"]), (10.0, 597, 2.32, 1, 52.2))
        self.assertEqual((w["stats_ok"], w["stats_failed"], w["mean_over_1_5"], w["mean_over_2_5"]), (597, 0, 2, 1))
        h = dl.parse_present_stats(PS_UNRELIABLE_HRESULT)
        self.assertEqual((h["reliable"], h["reason"], h["reason_short"]),
                         (False, "GetFrameStatistics failed 3 times (HRESULT 0x887A0001)", "GetFrameStatistics failed (HRESULT 0x887A0001)"))

    def test_short_reasons(self):
        s = dl.short_reason
        self.assertEqual(s("GetFrameStatistics never succeeded (597 calls failed)"), "GetFrameStatistics never succeeded")
        self.assertEqual(s("the display statistics restarted 2 times (a display, mode or window change)"),
                         "display statistics restarted (display, mode or window change)")
        self.assertEqual(s("the counters jumped 1 times"), "counters jumped")
        self.assertEqual(s("too few presents or refreshes to judge (1 presents, 0 refreshes)"), "too few presents or refreshes to judge")
        self.assertEqual(s("the refresh counter covered 72% of the window's time"), s("the refresh counter covered 130% of the window's time"))
        self.assertEqual(s("something new 12 and 3.5"), "something new N and N")      # an unknown reason: numbers dropped so it groups
        self.assertEqual(s(""), "no reason given")

    def test_new_line_cut_short_or_changed(self):
        no_end = dl.parse_present_stats(PS_NEW.replace(" | reliable", ""))
        self.assertEqual((no_end["format"], no_end["reliable"], no_end["presents"]), ("new", False, 600))
        self.assertIn("without", no_end["reason"])
        broken = dl.parse_present_stats(PS_NEW.replace("held over 3 refreshes 0", "held beyond 3 refreshes 0"))
        self.assertEqual((broken["reliable"], broken["partial"], broken["cadence"]), (False, True, None))
        self.assertEqual(broken["reason_short"], "the line is only partly readable")
        self.assertEqual(broken["held_2"], 149)                  # the other sections still parse

    def test_warning_line(self):
        warn = "[present-stats] GetFrameStatistics failed (HRESULT 0x887A0001): only the CPU side of the presents is counted"
        self.assertTrue(dl.is_present_stats_warning(warn))
        self.assertFalse(dl.is_present_stats_warning(PS_WINDOW))


class HitchParse(unittest.TestCase):
    def test_menu_line(self):
        h = dl.parse_hitch(stamp(3) + H_MENU)
        self.assertEqual((h["frame_ms"], h["periods"], h["period_ms"], h["mode"]), (183.4, 11.0, 16.7, "menu"))
        self.assertEqual((h["cpu_ms"], h["wait_ms"], h["sleep_ms"]), (12.3, 171.1, 4.2))
        self.assertEqual((h["pipelines"], h["shaders"], h["in_flight"], h["gpu_ms"]), (148, 0, 3, 6.7))
        self.assertEqual((h["page_faults"], h["read_mib"]), (2340, 12.4))
        self.assertTrue(h["text"].startswith("[mcla-hitch] 183.4 ms"))
        self.assertEqual(h["cause"], "compile")

    def test_modes(self):
        self.assertEqual(dl.parse_hitch(H_DRIVE_CPU)["mode"], "drive")
        self.assertEqual(dl.parse_hitch(H_UNKNOWN_NA)["mode"], "unknown")
        self.assertEqual(dl.parse_hitch(H_MENU)["mode"], "menu")
        self.assertEqual(dl.parse_hitch(H_MENU.replace("a menu / pause / load", "the garage"))["mode"], "the garage")

    def test_not_measured_variants(self):
        h = dl.parse_hitch(H_UNKNOWN_NA)
        self.assertIsNone(h["cpu_ms"])
        self.assertIsNone(h["wait_ms"])
        self.assertEqual(h["sleep_ms"], 0.0)
        self.assertIsNone(h["pipelines"])
        self.assertIsNone(h["gpu_ms"])
        self.assertEqual((h["page_faults"], h["read_mib"]), (5, 0.0))
        self.assertEqual(h["cause"], "blocked")

    def test_unlogged_summary(self):
        self.assertEqual(dl.parse_hitch_unlogged(stamp(4) + H_UNLOGGED), {"count": 5, "worst_ms": 91.3})
        self.assertIsNone(dl.parse_hitch(H_UNLOGGED))
        self.assertIsNone(dl.parse_hitch_unlogged(H_MENU))

    def test_not_a_hitch_line(self):
        self.assertIsNone(dl.parse_hitch(stamp(1) + "[mcla-sim] on: world step n/60 s, cap 60"))
        self.assertIsNone(dl.parse_hitch("[mcla-hitch] garbled"))


class Classify(unittest.TestCase):
    def cause(self, **kw):
        h = {"frame_ms": 50.0, "cpu_ms": 1.0, "pipelines": 0, "shaders": 0, "page_faults": 0, "read_mib": 0.0, "gpu_ms": None}
        h.update(kw)
        return dl.classify_hitch(h)

    def test_compile(self):
        self.assertEqual(self.cause(pipelines=1), "compile")
        self.assertEqual(self.cause(shaders=2), "compile")

    def test_compile_beats_the_rest(self):
        self.assertEqual(self.cause(pipelines=3, cpu_ms=49.0, page_faults=5000), "compile")

    def test_cpu_bound(self):
        self.assertEqual(self.cause(cpu_ms=30.0), "cpu-bound")          # exactly 60 %
        self.assertEqual(self.cause(cpu_ms=29.9), "blocked")
        self.assertEqual(self.cause(cpu_ms=45.0, page_faults=9999, read_mib=50.0), "cpu-bound")

    def test_paging_io(self):
        self.assertEqual(self.cause(page_faults=1000), "paging/io")
        self.assertEqual(self.cause(page_faults=999), "blocked")
        self.assertEqual(self.cause(read_mib=4.0), "paging/io")
        self.assertEqual(self.cause(read_mib=3.9), "blocked")

    def test_blocked_and_unknowns(self):
        self.assertEqual(self.cause(), "blocked")
        # CPU not measured and no SDK counters: neither test can be satisfied
        self.assertEqual(self.cause(cpu_ms=None, pipelines=None, shaders=None), "blocked")
        self.assertEqual(self.cause(cpu_ms=None, pipelines=None, shaders=None, page_faults=4000), "paging/io")

    def test_gpu_bound_sub_count(self):
        self.assertTrue(dl.is_gpu_bound(dl.parse_hitch(H_GPU_BOUND)))
        self.assertFalse(dl.is_gpu_bound(dl.parse_hitch(H_MENU)))
        self.assertFalse(dl.is_gpu_bound(dl.parse_hitch(H_UNKNOWN_NA)))


class Totals(unittest.TestCase):
    def windows(self):
        """Old-format lines: two of a 60 Hz run (trusted: clock = present rate) and one of 165 Hz whose counters did not move."""
        a = dl.parse_present_stats(PS_WINDOW)
        b = dl.parse_present_stats(PS_MULTI)
        dead = dl.parse_present_stats("[present-stats] 10.0 s: presents 601 shown 0 dropped 601 | refreshes 0 at 165.001 Hz | "
                                      "shown frames held 1 refresh 0, 2 0, 3+ 0 (max 0), repeats >= 0 | present call max 3.64 "
                                      "ms, over 2 ms 1 | present interval max 21.7 ms, over 1.5 / 2.5 refreshes 601 / 597 | "
                                      "stats 601 ok 0 failed")
        return [a, b, dead]

    def mixed(self):
        """New-format lines: three reliable (165 Hz, one with a variable-refresh clock) and two unreliable."""
        return [dl.parse_present_stats(x) for x in (PS_NEW, PS_NEW_REPEATS, PS_NEW_VRR, PS_UNRELIABLE, PS_UNRELIABLE_HRESULT)]

    def test_display_totals(self):
        d = dl.aggregate_display(self.windows(), [{"refreshes": 4, "ms": 66.7}, {"refreshes": 3, "ms": 50.0}])
        self.assertEqual((d["windows"], d["seconds"], d["reliable_windows"], d["unreliable_windows"], d["reliable_seconds"]),
                         (3, 30.0, 2, 1, 20.0))
        # the display totals cover the reliable windows only (the dead one does not count); presents_all is every window
        self.assertEqual((d["presents"], d["presents_all"], d["shown"], d["dropped"]), (1200, 1801, 1196, 4))
        self.assertEqual((d["held_1"], d["held_2"], d["held_3plus"], d["held_max"], d["attributed"]), (1180, 12, 4, 4, 1196))
        self.assertEqual((d["repeats_min"], d["multi_frames"], d["multi_refreshes"]), (20, 20, 49))
        self.assertEqual((d["held_over"], d["display_hitches"], d["cadence"]), (16, 4, 1.0))    # 2 + 3+ over a cadence of 1; 3+
        self.assertEqual((d["call_max_ms"], d["call_over_2ms"], d["interval_max_ms"]), (3.64, 1, 21.7))   # CPU side: every window
        self.assertEqual(d["hz"], 59.996)                       # the dead window's Hz does not count
        self.assertEqual((d["no_stats_windows"], d["hitch_lines"], d["hitch_worst_refreshes"], d["hitch_worst_ms"]), (1, 2, 4, 66.7))
        self.assertEqual((d["old_format_windows"], d["old_format_trusted"]), (3, 2))
        self.assertEqual(d["unreliable_reasons"], [["old format: display counters not moving", 1]])
        self.assertEqual(d["verdict"], "repeats")
        json.dumps(d)

    def test_mixed_totals_from_reliable_windows_only(self):
        d = dl.aggregate_display(self.mixed())
        self.assertEqual((d["windows"], d["reliable_windows"], d["unreliable_windows"], d["reliable_seconds"], d["seconds"]),
                         (5, 3, 2, 30.0, 50.0))
        self.assertEqual((d["presents"], d["presents_all"], d["shown"], d["dropped"], d["refreshes"]), (1800, 2994, 1800, 0, 4893))
        self.assertEqual((d["hz"], d["mode_hz"], d["clock_differs_windows"]), (165.001, 165.0, 1))
        self.assertEqual((d["cadence"], d["cadence_min"], d["cadence_max"]), (2.75, 2.64, 2.76))
        self.assertEqual((d["held_over"], d["held_over_per_min"], d["display_hitches"], d["repeats_min"]), (7, 14.0, 1, 10))
        self.assertEqual((d["multi_frames"], d["multi_refreshes"], d["held_max"]), (4, 12, 7))
        self.assertEqual((d["interval_over_1_5"], d["interval_over_2_5"]), (1797, 1790))          # against the refresh: reliable only
        self.assertEqual((d["interval_over_mean_1_5"], d["interval_over_mean_2_5"]), (8, 3))      # against the mean: every window
        self.assertEqual((d["call_max_ms"], d["call_over_2ms"], d["interval_max_ms"], d["stats_ok"]), (2.73, 3, 52.2, 2994))
        self.assertEqual(d["unreliable_reasons"], [["window hidden, covered or minimised (or display off)", 1],
                                                   ["GetFrameStatistics failed (HRESULT 0x887A0001)", 1]])
        self.assertEqual((d["no_stats_windows"], d["old_format_windows"], d["partial_windows"]), (0, 0, 0))
        json.dumps(d)

    def test_unreliable_windows_only(self):
        d = dl.aggregate_display([dl.parse_present_stats(PS_UNRELIABLE), dl.parse_present_stats(PS_UNRELIABLE)])
        self.assertEqual((d["reliable_windows"], d["unreliable_windows"], d["presents"], d["presents_all"]), (0, 2, 0, 1194))
        self.assertEqual((d["shown"], d["hz"], d["cadence"], d["held_over"]), (0, None, None, 0))
        self.assertEqual((d["call_max_ms"], d["interval_over_mean_1_5"]), (2.32, 4))
        self.assertEqual((d["verdict"], d["verdict_text"]), ("unknown", "unknown (no reliable window)"))
        line = dl.display_line(d, "whole run")
        self.assertIn("verdict unknown (no reliable window)", line)
        self.assertIn("windows 0 reliable, 2 unreliable (most common: window hidden, covered or minimised (or display off))", line)
        self.assertIn("presents 1,194 (all windows; no display numbers)", line)
        self.assertNotIn("shown", line)
        self.assertEqual(dl.note_text(d, None)[0], "display: verdict unknown, no reliable window (2 unreliable, most common: window "
                                                   "hidden, covered or minimised (or display off)), 1,194 presents")

    def test_verdicts(self):
        def verdict(*lines):
            d = dl.aggregate_display([dl.parse_present_stats(x) for x in lines])
            return d["verdict"], d["verdict_text"]
        key, text = verdict(PS_NEW)                              # 165 Hz at 60 fps, nothing over the cadence
        self.assertEqual(key, "beat by design")
        self.assertIn("cadence 2.75 is not a whole number: frames stay 2 or 3 refreshes", text)
        key, text = verdict(PS_NEW_60HZ)                         # 60 Hz at 60 fps
        self.assertEqual((key, text), ("even", "even (cadence 1.00, no frame held over it)"))
        key, text = verdict(PS_NEW_REPEATS)
        self.assertEqual((key, text), ("repeats", "repeats (7 frames held over the cadence, 42.0 a minute; cadence 2.76)"))
        self.assertEqual(verdict(PS_UNRELIABLE)[0], "unknown")
        self.assertEqual(verdict(PS_NEW_60HZ, PS_UNRELIABLE)[0], "even")                          # unreliable windows do not count
        # a cadence within 0.02 of a whole number is even, beyond it a beat
        self.assertEqual(verdict(PS_NEW_60HZ.replace("cadence 1.00", "cadence 2.02"))[0], "even")
        self.assertEqual(verdict(PS_NEW_60HZ.replace("cadence 1.00", "cadence 0.98"))[0], "even")
        self.assertEqual(verdict(PS_NEW_60HZ.replace("cadence 1.00", "cadence 2.03"))[0], "beat by design")
        self.assertIn("frames stay 1 or 2 refreshes", verdict(PS_NEW_60HZ.replace("cadence 1.00", "cadence 1.49"))[1])
        self.assertIn("more presents than refreshes", verdict(PS_NEW_60HZ.replace("cadence 1.00", "cadence 0.50"))[1])
        # one repeat is a repeat; the median cadence is the one reported
        one = PS_NEW_60HZ.replace("held over 1 refreshes 0", "held over 1 refreshes 1")
        self.assertEqual(verdict(one)[1], "repeats (1 frame held over the cadence, 6.0 a minute; cadence 1.00)")

    def test_verdict_says_when_most_frames_were_sampled_together(self):
        # 4 of 5 shown frames were retired together (not judged one by one): the over-cadence count is a lower bound
        sampled = PS_NEW.replace("held 1 refresh 0, 2 149, 3+ 451 (max 3), repeats >= 0",
                                 "held 1 refresh 0, 2 20, 3+ 100 (max 3), repeats >= 0 (+480 frames over 1300 refreshes sampled together)")
        d = dl.aggregate_display([dl.parse_present_stats(sampled)])
        self.assertEqual((d["attributed"], d["shown"], d["judged_pct"], d["verdict"]), (120, 600, 20.0, "beat by design"))
        self.assertIn("; only 20 % of the shown frames were judged one by one, the count is a lower bound)", d["verdict_text"])
        self.assertEqual(dl.aggregate_display([dl.parse_present_stats(PS_NEW)])["judged_pct"], 100.0)
        self.assertNotIn("lower bound", dl.aggregate_display([dl.parse_present_stats(PS_NEW)])["verdict_text"])

    def test_display_nothing(self):
        self.assertIsNone(dl.aggregate_display([]))
        self.assertIsNone(dl.aggregate_hitches([]))
        self.assertIn("no [present-stats] lines", dl.display_line(None, "whole run"))
        self.assertIn("no [mcla-hitch] lines", dl.hitch_line(None, "whole run"))

    def test_hitch_totals(self):
        hs = [dl.parse_hitch(x) for x in (H_MENU, H_DRIVE_CPU, H_UNKNOWN_NA, H_GPU_BOUND, H_GPU_BOUND)]
        hs[1]["t"] = 12.5
        t = dl.aggregate_hitches(hs, [dl.parse_hitch_unlogged(H_UNLOGGED)])
        self.assertEqual(t["logged"], 5)
        self.assertEqual(t["unlogged"], 5)
        self.assertEqual(t["by_mode"], {"drive": 3, "menu": 1, "unknown": 1})
        self.assertEqual(t["by_cause"], {"compile": 1, "cpu-bound": 1, "paging/io": 0, "blocked": 3})
        self.assertEqual(t["blocked_gpu"], 2)
        self.assertEqual(t["worst_ms"], 183.4)
        self.assertTrue(t["worst_line"].startswith("[mcla-hitch] 183.4 ms"))
        self.assertEqual(t["unlogged_worst_ms"], 91.3)
        json.dumps(t)

    def test_printed_lines(self):
        hs = [dl.parse_hitch(H_MENU), dl.parse_hitch(H_DRIVE_CPU)]
        hs[1]["t"] = 12.5
        disp = {"run": dl.aggregate_display(self.windows()), "drive": None, "drive_from_s": None}
        hit = {"run": dl.aggregate_hitches(hs, [dl.parse_hitch_unlogged(H_UNLOGGED)]), "drive": None, "drive_from_s": None}
        text = "\n".join(dl.summary_lines(disp, hit))
        self.assertIn("verdict repeats (16 frames held over the cadence, 48.0 a minute; cadence 1.00)", text)
        self.assertIn("windows 2 reliable (20 s), 1 unreliable (most common: old format: display counters not moving)", text)
        self.assertIn("presents 1,200 shown 1,196 dropped 4 (reliable windows, 1,801 in all)", text)
        self.assertIn("shown frames held 1 refresh 1,180, 2 12, 3+ 4 (worst 4)", text)
        self.assertIn("held over the cadence 16 (48.0 a minute), repeats >= 20", text)
        self.assertIn("3 of 3 windows in the old line format (no cadence tail): 2 trusted", text)
        self.assertIn("over 1.5 / 2.5 refreshes 6 / 2", text)                  # a 60 Hz run: the over-refresh counts are shown
        self.assertIn("hitches (whole run): 2 logged + 5 not logged (worst 91.3 ms)", text)
        self.assertIn("by screen: drive 1, menu 1, unknown 0", text)
        self.assertIn("cause: compile 1, cpu-bound 1, paging/io 0, blocked 0", text)
        self.assertIn("worst hitch: [mcla-hitch] 183.4 ms", text)
        self.assertNotIn("drive window", text)                          # no free roam: no drive window lines

    def test_printed_line_of_a_165_hz_run(self):
        line = dl.display_line(dl.aggregate_display(self.mixed(), [{"refreshes": 5, "ms": 30.3, "cadence": 2.75, "allowed": 3}]), "whole run")
        self.assertTrue(line.startswith("display (whole run, 5 windows, 50 s): verdict repeats (7 frames held over the cadence, 14.0 a "
                                        "minute; cadence 2.75) | windows 3 reliable (30 s), 2 unreliable (most common: window "
                                        "hidden, covered or minimised (or display off), 1 of 2)"), line)
        self.assertIn("presents 1,800 shown 1,800 dropped 0 (reliable windows, 2,994 in all)", line)
        self.assertIn("refresh clock 165.001 Hz (display mode 165 Hz, the clock differs from it in 1 of 3 windows: variable refresh)", line)
        self.assertIn("cadence 2.75 refreshes a frame (median, windows 2.64 to 2.76)", line)
        self.assertIn("held over the cadence 7 (14.0 a minute), repeats >= 10 (+4 frames over 12 refreshes sampled together, not "
                      "attributed) | display hitches 1 | hitch lines 1 logged, worst 5 refreshes (30.3 ms)", line)
        self.assertIn("over 1.5 / 2.5 times the mean 8 / 3", line)
        self.assertNotIn("over 1.5 / 2.5 refreshes", line)             # cadence 2.75: every interval is over 1.5 refreshes, say nothing
        sixty = dl.display_line(dl.aggregate_display([dl.parse_present_stats(PS_NEW_60HZ)]), "whole run")
        self.assertIn("verdict even (cadence 1.00, no frame held over it) | windows 1 reliable (10 s), 0 unreliable | presents 600", sixty)
        self.assertIn("over 1.5 / 2.5 refreshes 0 / 0", sixty)


def log_lines(with_new: bool = True):
    """A small log: menu until 30 s, free roam from 30 s; the new line kinds before and after."""
    lines = [stamp(0) + "[mcla] mcla.exe Release built Nov 27 2026 01:00:00",
             stamp(1) + "[mcla-heartbeat] first engine frame (the legals, logos and loads before it run without the timer)",
             stamp(2) + "[mcla-mode] unknown -> front-end (menu) via UserInterface/Setup (mcGameState 2)"]
    if with_new:
        lines += [stamp(5.0) + H_MENU,                                              # menu: compile
                  stamp(9.0) + H_UNKNOWN_NA,                                        # unknown, blocked
                  stamp(10.0) + PS_WINDOW,                                          # window 0 .. 10: before free roam
                  stamp(20.0) + PS_WINDOW.replace("shown 598 dropped 2", "shown 590 dropped 10"),
                  stamp(21.0) + H_UNLOGGED]
    lines += [stamp(30.0) + "[mcla-mode] loading -> free-roam (driving) via Cruising/Cruising_Offline (mcGameState 2)"]
    if with_new:
        lines += [stamp(31.0) + H_DRIVE_CPU,                                        # drive: cpu-bound
                  stamp(32.0) + H_GPU_BOUND,                                        # drive: blocked, GPU
                  stamp(33.0) + PS_HITCH,
                  stamp(34.0) + PS_HITCH.replace("4 refreshes", "6 refreshes").replace("66.7", "100.0"),
                  stamp(35.0) + PS_MULTI,                                           # window 25 .. 35: not wholly after free roam
                  stamp(45.0) + PS_TAILS,                                           # window 35 .. 45: in the drive window
                  stamp(46.0) + H_MENU.replace("in a menu / pause / load", "in a drive")]    # drive, compile
    lines += [stamp(50.0) + "[mcla-sim] 5.0 s real: world 5.0 s (x1.000), frames 300 x, cap 60"]
    return [l + "\n" for l in lines]


def log_lines_cadence():
    """A small log of a 165 Hz run: the menu windows before free roam (30 s), then reliable and UNRELIABLE windows in the drive."""
    lines = [stamp(0) + "[mcla] mcla.exe Release built Nov 27 2026 01:00:00",
             stamp(1) + "[mcla-heartbeat] first engine frame (the legals, logos and loads before it run without the timer)",
             stamp(2) + "[mcla-mode] unknown -> front-end (menu) via UserInterface/Setup (mcGameState 2)",
             stamp(10.0) + PS_NEW_REPEATS,                                      # window 0 .. 10: before free roam, 7 over-cadence frames
             stamp(30.0) + "[mcla-mode] loading -> free-roam (driving) via Cruising/Cruising_Offline (mcGameState 2)",
             stamp(31.0) + PS_HITCH_NEW,
             stamp(40.0) + PS_NEW,                                              # window 30 .. 40: the drive, clean
             stamp(50.0) + PS_UNRELIABLE,                                       # window 40 .. 50: the window was covered
             stamp(59.0) + PS_NEW_VRR.replace("cadence 2.64", "cadence 2.65")]    # window 49 .. 59: the drive, a variable-refresh clock
    return [l + "\n" for l in lines]


class RunGameSummary(unittest.TestCase):
    def parse(self, lines):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mcla.log"
            path.write_text("".join(lines), encoding="utf-8", newline="\n")
            return run_game.parse_log(path)

    def test_whole_run_and_drive_window(self):
        p = self.parse(log_lines())
        json.dumps(p)                                                   # summary.json must serialize
        d, h = p["display"], p["hitches"]
        self.assertEqual(d["drive_from_s"], 30.0)
        run = d["run"]
        self.assertEqual((run["windows"], run["presents"], run["shown"], run["dropped"]), (4, 2400, 598 + 590 + 598 + 598, 2 + 10 + 2 + 2))
        self.assertEqual((run["hitch_lines"], run["hitch_worst_refreshes"], run["hitch_worst_ms"]), (2, 6, 100.0))
        self.assertEqual(run["hitch_unlogged"], 7)
        self.assertEqual((run["counter_resets"], run["presents_failed"]), (2, 3))
        drive = d["drive"]
        self.assertEqual(drive["windows"], 1)                           # only the window 35..45 lies wholly after free roam
        self.assertEqual((drive["presents"], drive["hitch_lines"], drive["hitch_unlogged"]), (600, 2, 7))
        hr, hd = h["run"], h["drive"]
        self.assertEqual((hr["logged"], hr["unlogged"]), (5, 5))
        self.assertEqual(hr["by_mode"], {"drive": 3, "menu": 1, "unknown": 1})
        self.assertEqual(hr["by_cause"], {"compile": 2, "cpu-bound": 1, "paging/io": 0, "blocked": 2})
        self.assertEqual(hr["worst_ms"], 183.4)
        self.assertEqual(hr["worst_t"], 5.0)
        self.assertEqual((hd["logged"], hd["unlogged"]), (3, 0))        # the not-logged summary came before free roam
        self.assertEqual(hd["by_mode"], {"drive": 3, "menu": 0, "unknown": 0})
        self.assertEqual(hd["by_cause"], {"compile": 1, "cpu-bound": 1, "paging/io": 0, "blocked": 1})
        self.assertEqual(hd["blocked_gpu"], 1)

    def test_cadence_log_whole_run_and_drive_window(self):
        p = self.parse(log_lines_cadence())
        json.dumps(p)
        run, drive = p["display"]["run"], p["display"]["drive"]
        self.assertEqual((run["windows"], run["reliable_windows"], run["unreliable_windows"]), (4, 3, 1))
        self.assertEqual((run["presents"], run["presents_all"], run["held_over"], run["verdict"]), (1800, 2397, 7, "repeats"))
        self.assertEqual(run["unreliable_reasons"], [["window hidden, covered or minimised (or display off)", 1]])
        self.assertEqual((drive["windows"], drive["reliable_windows"], drive["unreliable_windows"]), (3, 2, 1))
        self.assertEqual((drive["presents"], drive["held_over"], drive["verdict"], drive["hitch_lines"]), (1201, 0, "beat by design", 1))
        self.assertEqual((drive["cadence"], drive["hz"], drive["mode_hz"], drive["clock_differs_windows"]), (2.7, 161.755, 165.0, 1))
        self.assertEqual(p["display"]["run"]["unparsed"], 0)               # an UNRELIABLE window is not an unreadable one

    def test_cadence_log_live_scanner_agrees(self):
        s = run_game.LogScanner()
        for l in log_lines_cadence():
            s.feed_bytes(l.encode("utf-8"))
        self.assertEqual(s.result()["display"], self.parse(log_lines_cadence())["display"])

    def test_old_log_without_the_lines(self):
        p = self.parse(log_lines(with_new=False))
        self.assertEqual(p["reached"], "free-roam")
        self.assertIsNone(p["display"]["run"])
        self.assertIsNone(p["display"]["drive"])
        self.assertIsNone(p["hitches"]["run"])
        text = "\n".join(dl.summary_lines(p["display"], p["hitches"]))
        self.assertIn("no [present-stats] lines", text)
        self.assertIn("no [mcla-hitch] lines", text)
        json.dumps(p)

    def test_live_scanner_agrees(self):
        # the live tail feeds bytes line by line: the same totals as the offline parse
        s = run_game.LogScanner()
        for l in log_lines():
            s.feed_bytes(l.encode("utf-8"))
        p = self.parse(log_lines())
        self.assertEqual(s.result()["display"], p["display"])
        self.assertEqual(s.result()["hitches"], p["hitches"])

    def test_unreadable_window_line_is_counted(self):
        lines = log_lines() + [stamp(51) + "[present-stats] 10.0 s: presents lots shown many\n"]
        p = self.parse(lines)
        self.assertEqual(p["display"]["run"]["unparsed"], 1)
        self.assertIn("UNREADABLE", dl.display_line(p["display"]["run"], "whole run"))

    def test_summarize_prints_the_run_line_last(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp) / "runs" / "t1"
            run_dir.mkdir(parents=True)
            (run_dir / "mcla.log").write_text("".join(log_lines()), encoding="utf-8", newline="\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = run_game.main(["--summarize", str(run_dir / "mcla.log"), "--work", tmp])
            lines = out.getvalue().splitlines()
            self.assertEqual(rc, 0)
            self.assertTrue(lines[-1].startswith("run t1: reached=free-roam fatal=0 unregistered=0 heartbeat_last="), lines[-1])
            self.assertTrue(any(l.startswith("display (whole run") for l in lines))
            self.assertTrue(any(l.startswith("display (drive window, from free roam at +30 s") for l in lines))
            self.assertTrue(any(l.startswith("hitches (whole run): 5 logged + 5 not logged") for l in lines))
            self.assertTrue(any(l.startswith("hitches (drive window, from free roam at +30 s): 3 logged") for l in lines))


class SummarizeCadenceLog(unittest.TestCase):
    def test_summarize_prints_verdicts_and_the_run_line_last(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp) / "runs" / "t1"
            run_dir.mkdir(parents=True)
            (run_dir / "mcla.log").write_text("".join(log_lines_cadence()), encoding="utf-8", newline="\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = run_game.main(["--summarize", str(run_dir / "mcla.log"), "--work", tmp])
            lines = out.getvalue().splitlines()
        self.assertEqual(rc, 0)
        self.assertTrue(lines[-1].startswith("run t1: reached=free-roam fatal=0 unregistered=0 heartbeat_last="), lines[-1])
        whole = [l for l in lines if l.startswith("display (whole run, 4 windows, 40 s): ")]
        drive = [l for l in lines if l.startswith("display (drive window, from free roam at +30 s, 3 windows, 30 s): ")]
        self.assertEqual((len(whole), len(drive)), (1, 1))
        self.assertIn("verdict repeats (7 frames held over the cadence", whole[0])
        self.assertIn("verdict beat by design (cadence 2.70 is not a whole number: frames stay 2 or 3 refreshes, not repeats)", drive[0])
        self.assertIn("windows 2 reliable (20 s), 1 unreliable (most common: window hidden, covered or minimised (or display off))", drive[0])
        self.assertIn("the clock differs from it in 1 of 2 windows: variable refresh", drive[0])


class BenchNotes(unittest.TestCase):
    def log_path(self, tmp, lines):
        path = Path(tmp) / "mcla.log"
        path.write_text("".join(lines), encoding="utf-8", newline="\n")
        return path

    def epoch(self, sec):
        return datetime.datetime(2026, 11, 27, 1, 57, 0).timestamp() + sec

    def test_scan_log_and_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = bench.scan_log(self.log_path(tmp, log_lines()))
        self.assertEqual((len(log["ps_windows"]), len(log["ps_hitches"]), len(log["hitches"]), len(log["hitch_unlogged"])),
                         (4, 2, 5, 1))
        self.assertEqual(log["fr_ts"], self.epoch(30.0))
        # the window of a bench: 30 .. 50 s; windows wholly inside (35..45 and 25..35 is not), hitches by time
        got = bench.log_display(log, self.epoch(30.0), self.epoch(50.0))
        self.assertEqual(got["display"]["windows"], 1)
        self.assertEqual(got["display"]["presents"], 600)
        self.assertEqual(got["display"]["hitch_lines"], 2)
        self.assertEqual(got["hitches"]["logged"], 3)
        self.assertEqual(got["hitches"]["by_cause"]["compile"], 1)
        none = bench.log_display(log, self.epoch(100.0), self.epoch(110.0))
        self.assertIsNone(none["display"])
        self.assertIsNone(none["hitches"])

    def test_notes_in_the_row(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = bench.scan_log(self.log_path(tmp, log_lines()))
        a = {"valid": True, "reasons": [], "stats": {"avg_fps": 60.0, "p50_ms": 16.7, "p99_ms": 18.0, "pct_over_25ms": 0.1,
                                                    "pct_over_50ms": 0.0, "max_ms": 40.0, "gpu_busy_pct": 50.0, "frames": 1200,
                                                    "seconds": 20.0},
             "world_real": None, "sim_ft": None, "sim_on": False, "wide": None, "built": None}
        a.update(bench.log_display(log, self.epoch(30.0), self.epoch(50.0)))
        res = bench.Result("r1", "drive", bench.Variant("base"), "bench_drive", 0, [], True, [], a)
        notes = bench.make_row(res, "b", "d3d12", None, "")["notes"]
        self.assertIn("display: verdict repeats, 598 shown, held 2 6 / 3+ 2 (worst 4), over cadence 8, repeats >= 10 over 10 s, "
                      "2 dropped, cadence 1.00 on a 60 Hz clock, display hitches 2", notes)
        self.assertIn("hitches 3 (compile 1, cpu-bound 1, paging/io 0, blocked 1)", notes)
        self.assertIn("window 20 s, 1200 frames", notes)               # the existing notes are still first
        detail = "\n".join(bench.detail_lines(res))
        self.assertIn("display (window, 1 windows, 10 s)", detail)
        self.assertIn("hitches (window): 3 logged", detail)

    def test_notes_of_cadence_windows(self):
        mixed = [dl.parse_present_stats(x) for x in (PS_NEW, PS_NEW_REPEATS.replace("cadence 2.76", "cadence 2.75"), PS_UNRELIABLE)]
        notes = dl.note_text(dl.aggregate_display(mixed), None)
        self.assertEqual(notes[0], "display: verdict repeats, 1199 shown, held 2 299 / 3+ 894 (worst 7), over cadence 7, repeats >= 10 over "
                                   "20 s, cadence 2.75 on a 165 Hz clock, display hitches 1 (4 frames sampled together) (1 of 3 windows "
                                   "unreliable: window hidden, covered or minimised (or display off))")
        self.assertEqual(notes[1], "hitches 0")
        self.assertFalse(any(";" in n for n in notes))                  # the row joins the notes with `; `
        clean = dl.note_text(dl.aggregate_display([dl.parse_present_stats(PS_NEW)]), None)[0]
        self.assertEqual(clean, "display: verdict beat by design, 600 shown, held 2 149 / 3+ 451 (worst 3), over cadence 0, repeats >= 0 "
                                "over 10 s, cadence 2.75 on a 165 Hz clock")

    def test_no_hitches_in_a_build_that_logs_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = bench.scan_log(self.log_path(tmp, log_lines()))
        got = bench.log_display(log, self.epoch(35.0), self.epoch(45.0))    # the window 35..45 s holds no `[mcla-hitch]` line
        self.assertEqual(got["hitches"], None)
        self.assertEqual(dl.note_text(got["display"], got["hitches"])[-1], "hitches 0")

    def test_notes_of_an_old_log_are_unchanged(self):
        a = {"valid": True, "reasons": [], "stats": {"avg_fps": 60.0, "p50_ms": 16.7, "p99_ms": 18.0, "pct_over_25ms": 0.1,
                                                    "pct_over_50ms": 0.0, "max_ms": 40.0, "gpu_busy_pct": 50.0, "frames": 1200,
                                                    "seconds": 20.0},
             "world_real": None, "sim_ft": None, "sim_on": False, "wide": None, "built": None}
        with tempfile.TemporaryDirectory() as tmp:
            log = bench.scan_log(self.log_path(tmp, log_lines(with_new=False)))
        a.update(bench.log_display(log, self.epoch(30.0), self.epoch(50.0)))
        res = bench.Result("r1", "drive", bench.Variant("base"), "bench_drive", 0, [], True, [], a)
        self.assertEqual(bench.make_row(res, "b", "d3d12", None, "")["notes"], "window 20 s, 1200 frames")
        self.assertEqual(bench.detail_lines(res)[0].split(":")[0], "  window")
        self.assertFalse(any("display" in l or "hitches" in l for l in bench.detail_lines(res)))

    def test_pooled(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = bench.scan_log(self.log_path(tmp, log_lines()))
        parts = []
        for _ in range(2):
            a = {"valid": True, "reasons": [], "stats": None, "world_real": None, "world_s": 0.0, "real_s": 0.0, "sim_lines": 0,
                 "sim_on": False, "wide": None, "built": None, "ft": [10.0], "gpu": [None], "window": None, "pm_clock": None}
            a.update(bench.log_display(log, self.epoch(30.0), self.epoch(50.0)))
            parts.append(bench.Result("r", "drive", bench.Variant("base"), "bench_drive", 0, [], True, [], a))
        pooled = bench.pooled(parts, "r-stable", "stable", "bench_drive+bench_parked", bench.Variant("base"), 0)
        self.assertEqual(pooled.analysis["display"]["windows"], 2)
        self.assertEqual(pooled.analysis["hitches"]["logged"], 6)


if __name__ == "__main__":
    unittest.main(verbosity=2)
