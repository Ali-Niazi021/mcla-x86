#!/usr/bin/env python3
"""usage_watch.py - read the Claude subscription's usage (the 5-hour and weekly limits) and wait on it.

    python tools/usage_watch.py                         print the current usage once
    python tools/usage_watch.py --until 96 [--interval 180]
                                                        poll; exit 10 as soon as the 5-hour usage is >= 96 %
    python tools/usage_watch.py --until-reset [--interval 300]
                                                        poll; exit 11 once the 5-hour window has reset (usage back
                                                        under --resume-below, default 50 %, or its reset time passed)

  The owner's rule: at 98 % of the 5-hour limit the lead pauses all work until the limit resets, then continues. The
  lead runs `--until 96` as a background command (the harness wakes the lead when it exits), winds the agents down
  between 96 and 98 %, then waits with `--until-reset`.

  Source: GET https://api.anthropic.com/api/oauth/usage with the Claude Code login's own access token, read fresh from
  ~/.claude/.credentials.json on every poll (Claude Code refreshes that file; this script never writes it, never
  refreshes the token and never prints it). The owner approved this use on 2026-11-27. The same numbers are what
  Claude Code's /usage shows.

  Every poll also writes <state dir>/usage.json ({"five_hour_pct", "five_hour_resets_at", "seven_day_pct", ...,
  "checked_at"}) so anything else can read the last value without a request. State dir: $MCLA_WORK/usage, else
  mcla-x86-work/usage next to the repo.
  Exit codes: 0 printed once; 10 threshold reached; 11 reset seen; 20 the usage could not be read 5 times in a row
  (no login, no network, the endpoint changed): monitoring is then OFF and the lead must say so.
"""
import argparse
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

URL = "https://api.anthropic.com/api/oauth/usage"
CREDENTIALS = Path.home() / ".claude" / ".credentials.json"


def state_dir() -> Path:
    work = os.environ.get("MCLA_WORK")
    base = Path(work) if work else Path(__file__).resolve().parents[1].parent / "mcla-x86-work"
    return base / "usage"


def fetch() -> dict:
    token = json.loads(CREDENTIALS.read_text(encoding="utf-8"))["claudeAiOauth"]["accessToken"]
    request = urllib.request.Request(URL, headers={
        "Authorization": "Bearer " + token,
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
        "User-Agent": "claude-code-usage-watch",
    })
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.loads(response.read().decode("utf-8"))


def window(data: dict, key: str):
    """(percent used, reset time as an aware datetime or None) of one limit window; (None, None) when absent."""
    entry = data.get(key) or {}
    pct = entry.get("utilization")
    resets = entry.get("resets_at")
    when = None
    if resets:
        try:
            when = datetime.datetime.fromisoformat(str(resets).replace("Z", "+00:00"))
        except ValueError:
            when = None
    return (float(pct) if pct is not None else None), when


def read() -> dict:
    data = fetch()
    five, five_reset = window(data, "five_hour")
    week, week_reset = window(data, "seven_day")
    status = {
        "five_hour_pct": five,
        "five_hour_resets_at": five_reset.isoformat() if five_reset else None,
        "seven_day_pct": week,
        "seven_day_resets_at": week_reset.isoformat() if week_reset else None,
        "checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "other_windows": {k: v.get("utilization") for k, v in data.items()
                          if isinstance(v, dict) and k not in ("five_hour", "seven_day") and "utilization" in v},
    }
    try:
        folder = state_dir()
        folder.mkdir(parents=True, exist_ok=True)
        tmp = folder / "usage.json.tmp"
        tmp.write_text(json.dumps(status, indent=1), encoding="utf-8")
        os.replace(tmp, folder / "usage.json")
    except OSError:
        pass
    return status


def describe(status: dict) -> str:
    def part(name, pct, resets):
        if pct is None:
            return "%s ?" % name
        text = "%s %.0f %%" % (name, pct)
        if resets:
            when = datetime.datetime.fromisoformat(resets).astimezone()
            minutes = (when - datetime.datetime.now().astimezone()).total_seconds() / 60
            text += " (resets %s, in %d h %02d min)" % (when.strftime("%H:%M"), max(0, minutes) // 60, max(0, minutes) % 60)
        return text
    return "usage: %s | %s" % (part("5-hour", status["five_hour_pct"], status["five_hour_resets_at"]),
                               part("week", status["seven_day_pct"], status["seven_day_resets_at"]))


def main() -> int:
    ap = argparse.ArgumentParser(description="Read or wait on the Claude subscription usage.")
    ap.add_argument("--until", type=float, metavar="PCT")
    ap.add_argument("--until-reset", action="store_true")
    ap.add_argument("--resume-below", type=float, default=50.0)
    ap.add_argument("--interval", type=int, default=180)
    args = ap.parse_args()

    failures = 0
    started_reset_at = None
    while True:
        try:
            status = read()
            failures = 0
        except (OSError, KeyError, ValueError, urllib.error.URLError) as error:
            failures += 1
            reason = "HTTP %s" % error.code if isinstance(error, urllib.error.HTTPError) else type(error).__name__
            if not (args.until is not None or args.until_reset) or failures >= 5:
                print("usage: could not be read (%s)" % reason, flush=True)
                return 20
            time.sleep(min(60, args.interval))
            continue
        five = status["five_hour_pct"]
        if args.until is None and not args.until_reset:
            print(describe(status))
            return 0
        if args.until is not None and five is not None and five >= args.until:
            print("THRESHOLD: " + describe(status), flush=True)
            return 10
        if args.until_reset:
            reset_at = status["five_hour_resets_at"]
            started_reset_at = started_reset_at or reset_at
            passed = False
            if started_reset_at:
                passed = datetime.datetime.now(datetime.timezone.utc) >= datetime.datetime.fromisoformat(started_reset_at)
            if five is None or five < args.resume_below or passed or (reset_at and reset_at != started_reset_at):
                print("RESET: " + describe(status), flush=True)
                return 11
        time.sleep(max(30, args.interval))


if __name__ == "__main__":
    sys.exit(main())
