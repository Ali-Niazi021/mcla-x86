#!/usr/bin/env python3
"""elevated.py - run a command elevated (as administrator) through the broker the owner approved once (broker.ps1).

    python elevated.py [--cwd DIR] [--shell pwsh|cmd] [--timeout S] [--no-wait] -- <command ...>
    python elevated.py --status          is the broker up? what is running?
    python elevated.py --wait ID         collect a --no-wait request (prints its output, exits with its code)
    python elevated.py --kill ID         kill a running request and its child processes
    python elevated.py --stop            end the broker (elevation is gone until the owner approves it again)

  The command is everything after `--`, joined with spaces and run as a script by PowerShell 7 (default) or cmd.exe, so
  pipes, `;`, `&&` and quotes work as in that shell. Quote the whole command once for YOUR shell, e.g.
      python elevated.py -- "PresentMon-2.6.0-x64.exe --process_name mcla.exe --output_file C:/x/pm.csv --timed 60"
      python elevated.py --shell cmd -- "powercfg /energy /duration 5"
  --cwd       working directory (default: the caller's)
  --timeout   seconds before the broker kills the command and its children (default 600; 0 = no limit)
  --no-wait   print the request id and return at once (a capture or a server that runs alongside something else);
              later: --wait ID, or --kill ID
  Output: the command's stdout, then its stderr (to stderr). Exit code: the command's; 124 when it timed out; 125 when
  the broker is not running or stopped answering (then nothing can be elevated: say so, do not retry in a loop).

  The command runs as the same user with the full administrator token, in the broker's environment (the variables of the
  moment it started), with no console and no stdin: nothing interactive. A GUI program still shows its window.
  Every request is written to broker.log.
"""
import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path

def _root() -> Path:
    """The broker's folder: $MCLA_ELEVATE_DIR, else $MCLA_WORK/elevate, else mcla-x86-work/elevate next to the repo
    (this file is tools/elevate/elevated.py). A copy that sits in the broker's own folder uses that folder."""
    here = Path(__file__).resolve().parent
    if os.environ.get("MCLA_ELEVATE_DIR"):
        return Path(os.environ["MCLA_ELEVATE_DIR"])
    if (here / "broker.log").exists():
        return here
    if os.environ.get("MCLA_WORK"):
        return Path(os.environ["MCLA_WORK"]) / "elevate"
    return here.parents[2] / "mcla-x86-work" / "elevate"


ROOT = _root()
QUEUE = ROOT / "queue"
DONE = ROOT / "done"
STATE = ROOT / "broker.json"


def broker_state():
    """The broker's state if its heartbeat is fresh (under 5 s old), else None."""
    try:
        if time.time() - STATE.stat().st_mtime > 5:
            return None
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def submit(request: dict) -> None:
    QUEUE.mkdir(parents=True, exist_ok=True)
    tmp = QUEUE / (request["id"] + ".tmp")
    tmp.write_text(json.dumps(request), encoding="utf-8")
    os.replace(tmp, QUEUE / (request["id"] + ".json"))


def read_text(path: str) -> str:
    try:
        return Path(path).read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return ""


def wait_for(request_id: str) -> int:
    result_path = DONE / (request_id + ".json")
    silent_since = None
    while not result_path.exists():
        if broker_state() is None:
            silent_since = silent_since or time.time()
            if time.time() - silent_since > 10:
                print("elevated: the broker is not running (no heartbeat for 10 s); request %s has no result" % request_id,
                      file=sys.stderr)
                return 125
        else:
            silent_since = None
        time.sleep(0.2)
    result = {}
    for _ in range(10):  # the rename is atomic; a retry only covers a sharing violation
        try:
            result = json.loads(result_path.read_text(encoding="utf-8-sig"))
            break
        except (OSError, ValueError):
            time.sleep(0.1)
    if result.get("error"):
        print("elevated: %s" % result["error"], file=sys.stderr)
    out = read_text(result.get("stdout", "")) if result.get("stdout") else ""
    err = read_text(result.get("stderr", "")) if result.get("stderr") else ""
    if out:
        sys.stdout.write(out)
        sys.stdout.flush()
    if err:
        sys.stderr.write(err)
    if result.get("timed_out"):
        print("elevated: timed out, killed by the broker", file=sys.stderr)
        return 124
    return int(result.get("exit_code", 1))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Run a command elevated through the approved broker.", add_help=True)
    ap.add_argument("--cwd")
    ap.add_argument("--shell", choices=["pwsh", "cmd"], default="pwsh")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--no-wait", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--wait", metavar="ID")
    ap.add_argument("--kill", metavar="ID")
    ap.add_argument("--stop", action="store_true")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    state = broker_state()
    if args.status:
        if state is None:
            print("broker: NOT running (the owner has to approve a new one)")
            return 125
        print("broker: running, pid %s, admin=%s, user %s, since %s, running requests: %s"
              % (state.get("pid"), state.get("admin"), state.get("user"), state.get("started"),
                 ", ".join(state.get("running") or []) or "none"))
        return 0
    if args.wait:
        return wait_for(args.wait)
    if state is None:
        print("elevated: the broker is not running: nothing can be elevated until the owner approves a new one",
              file=sys.stderr)
        return 125
    if args.stop:
        (ROOT / "stop").write_text("stop", encoding="utf-8")
        print("broker: asked to stop")
        return 0
    request_id = "%s-%s" % (time.strftime("%H%M%S"), uuid.uuid4().hex[:8])
    if args.kill:
        submit({"id": request_id, "kill": args.kill})
        return wait_for(request_id)
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        ap.error("no command (put it after `--`)")
    text = " ".join(command)
    if args.shell == "pwsh":
        text = "$PSStyle.OutputRendering = 'PlainText'\n" + text  # no colour escapes in the captured output
    submit({"id": request_id, "command": text, "shell": args.shell,
            "cwd": str(Path(args.cwd).resolve()) if args.cwd else os.getcwd(), "timeout_s": max(0, args.timeout)})
    if args.no_wait:
        print(request_id)
        return 0
    return wait_for(request_id)


if __name__ == "__main__":
    sys.exit(main())
