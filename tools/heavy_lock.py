#!/usr/bin/env python3
"""heavy_lock.py - first-come-first-served lock for heavy jobs (PLAN 7.3, Appendix C).

Two heavy jobs at once (an SDK or game build, codegen, a game run, a bench, an Xenia run) would ruin each other's
timings and can exhaust the 31 GB of RAM. Every such job runs under this lock.

Usage
    python tools/heavy_lock.py [--label L] [--wait-timeout S] [--work DIR] -- <command...>
        wait for the lock (FIFO), run the command with the lock held, release the lock, exit with the command's
        exit code. The lock is released on every way out: normal exit, error, Ctrl+C, Ctrl+Break, SIGTERM.
        Exit 75 if --wait-timeout expired before the lock was obtained, 127 if the command does not exist
        (checked before queueing), 130 if interrupted.
    python tools/heavy_lock.py status
        `free`, or `held pid=<pid> label=<label> started=<utc> elapsed=<s>s cmd=<command>`, then the waiters.
        A lock whose holder is gone prints as `free (stale lock ...)`. Always exit 0.
    python tools/heavy_lock.py release [--force]
        remove a stale lock; with --force remove the lock even if its holder is alive (it then keeps running
        without the lock: use only when the holder is known to be wedged).

Paths: $WORK is --work, else $MCLA_WORK, else the folder mcla-x86-work next to the repo (the owner's old
C:/Users/Ali/Development/mcla-x86-work when that exists).
    $WORK/locks/heavy.lock     the lock: JSON {pid, pid_created, label, cmd, started, started_epoch, token}
    $WORK/locks/queue/         one ticket file per waiter, named <time_ns>-<pid>-<random>.ticket

How it works
  - The lock file is created with os.open(O_CREAT | O_EXCL): creation is atomic, one creator wins.
  - FIFO: a waiter first drops a ticket in queue/. Only the waiter with the oldest live ticket may create the lock,
    so jobs start in arrival order, and a job arriving while the lock is being handed over cannot jump the queue.
  - Stale detection by process liveness: a lock or ticket belongs to (pid, process creation time); if that process is
    gone (or the pid was reused by another process) the lock is stale and the oldest waiter removes it, and dead
    tickets are dropped. On Windows liveness uses OpenProcess/GetProcessTimes through ctypes.
  - On Windows the wrapper puts itself in a job object with KILL_ON_JOB_CLOSE, so if the wrapper is killed hard
    (taskkill /F) the command and its children die with it instead of running on without the lock.
  - Re-entrancy: the command runs with MCLA_HEAVY_LOCK_TOKEN set. A nested `heavy_lock.py -- ...` (or hold()) that
    finds that token on the lock file knows an ancestor already holds the lock and runs without queueing, so
    `heavy_lock.py -- python tools/run_game.py ...` does not deadlock when run_game.py also takes the lock.

From Python (tools/ is on sys.path when a script in it runs):
    import heavy_lock
    with heavy_lock.hold("run_game title_to_menu", wait_timeout=None):
        ...                      # heavy work; raises heavy_lock.LockTimeout if wait_timeout expires
"""

import argparse
import datetime
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import uuid
from typing import Dict, List, Optional, Tuple

# The owner's folder when it exists (the global lock lives there for every checkout); on any other machine the folder
# mcla-x86-work next to the repo, the same place tools/setup.py puts the work folder.
_LEGACY_WORK = "C:/Users/Ali/Development/mcla-x86-work"
DEFAULT_WORK = _LEGACY_WORK if os.path.isdir(_LEGACY_WORK) else os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "mcla-x86-work").replace(os.sep, "/")
LOCK_NAME = "heavy.lock"
QUEUE_NAME = "queue"
ENV_TOKEN = "MCLA_HEAVY_LOCK_TOKEN"

POLL_SECONDS = 0.25
NOTICE_SECONDS = 30.0
YOUNG_SECONDS = 5.0       # an unreadable lock/ticket file younger than this is still being written
INTERRUPT_GRACE = 15.0    # after Ctrl+C: how long the command gets to exit by itself before it is killed

EXIT_TIMEOUT = 75         # EX_TEMPFAIL: --wait-timeout expired
EXIT_NOT_FOUND = 127
EXIT_INTERRUPTED = 130


class LockTimeout(Exception):
    """--wait-timeout expired before the lock was obtained."""


# ================================================================================================================
# Process identity (pid + creation time, so that a reused pid does not look alive)
# ================================================================================================================

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.restype = wintypes.BOOL
    _k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    _k32.GetExitCodeProcess.restype = wintypes.BOOL
    _k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    _k32.GetProcessTimes.restype = wintypes.BOOL
    _k32.GetCurrentProcess.argtypes = []
    _k32.GetCurrentProcess.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    _k32.SetInformationJobObject.restype = wintypes.BOOL
    _k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _k32.AssignProcessToJobObject.restype = wintypes.BOOL

    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _STILL_ACTIVE = 259
    _ERROR_ACCESS_DENIED = 5

    def process_start(pid: int) -> Optional[int]:
        """Creation time (FILETIME ticks) of the live process `pid`; 0 if it is alive but cannot be queried;
        None if no such process is running."""
        handle = _k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return 0 if ctypes.get_last_error() == _ERROR_ACCESS_DENIED else None
        try:
            code = wintypes.DWORD()
            if _k32.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value != _STILL_ACTIVE:
                return None  # exited, but somebody still holds a handle to it
            created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
            if _k32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                    ctypes.byref(kernel), ctypes.byref(user)):
                return (created.dwHighDateTime << 32) | created.dwLowDateTime
            return 0
        finally:
            _k32.CloseHandle(handle)

    _job_handle = None

    def kill_on_exit_job() -> bool:
        """Put this process (and so every child it starts later) in a job object that kills all its processes when
        the last handle to the job closes, that is when this process dies, however it dies."""
        global _job_handle

        class IO_COUNTERS(ctypes.Structure):
            _fields_ = [(n, ctypes.c_ulonglong) for n in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class BASIC(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                        ("SchedulingClass", wintypes.DWORD)]

        class EXTENDED(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", BASIC), ("IoInfo", IO_COUNTERS),
                        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

        job = _k32.CreateJobObjectW(None, None)
        if not job:
            return False
        ext = EXTENDED()
        ext.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        # 9 = JobObjectExtendedLimitInformation
        if not _k32.SetInformationJobObject(job, 9, ctypes.byref(ext), ctypes.sizeof(ext)):
            _k32.CloseHandle(job)
            return False
        if not _k32.AssignProcessToJobObject(job, _k32.GetCurrentProcess()):
            _k32.CloseHandle(job)
            return False
        _job_handle = job  # never closed on purpose: it closes when this process ends
        return True

    def kill_tree(proc: "subprocess.Popen") -> None:
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

else:
    def process_start(pid: int) -> Optional[int]:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return None
        except PermissionError:
            return 0
        try:  # Linux: the start time in clock ticks, field 22 of /proc/<pid>/stat
            with open("/proc/%d/stat" % pid, "rb") as f:
                return int(f.read().rsplit(b")", 1)[1].split()[19])
        except (OSError, ValueError, IndexError):
            return 0

    def kill_on_exit_job() -> bool:
        return False

    def kill_tree(proc: "subprocess.Popen") -> None:
        proc.kill()


def is_alive(pid: object, created: object) -> bool:
    if not isinstance(pid, int) or pid <= 0:
        return False
    now = process_start(pid)
    if now is None:
        return False
    if isinstance(created, int) and created and now and created != now:
        return False  # the pid now belongs to another process
    return True


# ================================================================================================================
# Files
# ================================================================================================================

def work_dir(arg: Optional[str] = None) -> str:
    return arg or os.environ.get("MCLA_WORK") or DEFAULT_WORK


def lock_dir(work: Optional[str] = None) -> str:
    """The lock is machine-global (one GPU, one power budget): $MCLA_LOCK_DIR, else <DEFAULT_WORK>/locks. A stream that
    works in its own $MCLA_WORK (its own SDK tree and build) still queues behind every other stream's builds and runs.
    MCLA_LOCK_DIR=work restores the old per-work-tree lock (`work`/locks), for tests of this tool only."""
    env = os.environ.get("MCLA_LOCK_DIR")
    if env == "work":
        return os.path.join(work_dir(work), "locks")
    return env or os.path.join(DEFAULT_WORK, "locks")


def paths(work: Optional[str]) -> Tuple[str, str]:
    base = lock_dir(work)
    queue = os.path.join(base, QUEUE_NAME)
    os.makedirs(queue, exist_ok=True)
    return os.path.join(base, LOCK_NAME), queue


def _retry(fn, attempts: int = 100, delay: float = 0.02):
    """Windows refuses to open/delete a file another process has open for a moment: retry that."""
    for i in range(attempts):
        try:
            return fn()
        except PermissionError:
            if i == attempts - 1:
                raise
            time.sleep(delay)


def read_raw(path: str) -> Optional[bytes]:
    """File content; None if the file does not exist; b"" if it exists but cannot be read right now."""
    def rd() -> bytes:
        with open(path, "rb") as f:
            return f.read()
    try:
        return _retry(rd)
    except FileNotFoundError:
        return None
    except PermissionError:
        return b""


def parse_record(raw: bytes) -> Optional[Dict]:
    try:
        rec = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return None
    return rec if isinstance(rec, dict) and isinstance(rec.get("pid"), int) else None


def age_seconds(path: str) -> float:
    try:
        return max(0.0, time.time() - os.path.getmtime(path))
    except OSError:
        return 0.0


def remove_quiet(path: str) -> bool:
    try:
        _retry(lambda: os.remove(path))
        return True
    except (FileNotFoundError, PermissionError):
        return False


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def describe(rec: Dict) -> str:
    cmd = rec.get("cmd")
    cmdline = subprocess.list2cmdline([str(c) for c in cmd]) if isinstance(cmd, list) else str(cmd)
    if len(cmdline) > 160:
        cmdline = cmdline[:157] + "..."
    started = rec.get("started_epoch")
    elapsed = " elapsed=%ds" % max(0, int(time.time() - started)) if isinstance(started, (int, float)) else ""
    return "pid=%s label=%s started=%s%s cmd=%s" % (rec.get("pid"), rec.get("label"), rec.get("started"),
                                                    elapsed, cmdline)


def make_record(label: str, cmd: List[str], token: str) -> Dict:
    pid = os.getpid()
    return {"pid": pid, "pid_created": process_start(pid) or 0, "label": label, "cmd": cmd, "started": now_iso(),
            "started_epoch": time.time(), "token": token}


# ================================================================================================================
# Lock state and queue
# ================================================================================================================

def lock_state(lock_path: str) -> Tuple[str, Optional[Dict], Optional[bytes]]:
    """("free" | "held" | "stale" | "pending", record, raw bytes). "pending": the file is still being written."""
    raw = read_raw(lock_path)
    if raw is None:
        return "free", None, None
    rec = parse_record(raw)
    if rec is None:
        return ("pending" if age_seconds(lock_path) < YOUNG_SECONDS else "stale"), None, raw
    return ("held" if is_alive(rec["pid"], rec.get("pid_created")) else "stale"), rec, raw


def live_tickets(queue_dir: str) -> List[Tuple[str, Dict]]:
    """Waiting tickets, oldest first. Tickets of dead processes are removed on the way."""
    out: List[Tuple[str, Dict]] = []
    try:
        names = sorted(n for n in os.listdir(queue_dir) if n.endswith(".ticket"))
    except FileNotFoundError:
        return out
    for name in names:
        p = os.path.join(queue_dir, name)
        raw = read_raw(p)
        if raw is None:
            continue
        rec = parse_record(raw)
        if rec is None:
            if age_seconds(p) < YOUNG_SECONDS:
                out.append((name, {"pid": 0, "label": "?"}))  # being written: counts as waiting
            else:
                remove_quiet(p)
            continue
        if is_alive(rec["pid"], rec.get("pid_created")):
            out.append((name, rec))
        else:
            remove_quiet(p)
    return out


def enqueue(queue_dir: str, label: str, cmd: List[str]) -> str:
    pid = os.getpid()
    body = json.dumps({"pid": pid, "pid_created": process_start(pid) or 0, "label": label, "cmd": cmd,
                       "enqueued": now_iso(), "enqueued_epoch": time.time()}).encode("utf-8")
    while True:
        name = "%020d-%010d-%s.ticket" % (time.time_ns(), pid, uuid.uuid4().hex[:8])
        p = os.path.join(queue_dir, name)
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0))
        except FileExistsError:
            continue
        try:
            os.write(fd, body)
        finally:
            os.close(fd)
        return name


def try_create_lock(lock_path: str, record: Dict) -> bool:
    try:
        fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0))
    except (FileExistsError, PermissionError):  # PermissionError: the old lock is still being deleted
        return False
    try:
        os.write(fd, json.dumps(record).encode("utf-8"))
    except BaseException:
        os.close(fd)
        remove_quiet(lock_path)
        raise
    os.close(fd)
    return True


def remove_lock_if_unchanged(lock_path: str, raw: Optional[bytes]) -> bool:
    if read_raw(lock_path) != raw:
        return False
    return remove_quiet(lock_path)


def say(msg: str) -> None:
    print("[heavy_lock] " + msg, file=sys.stderr, flush=True)


# ================================================================================================================
# Acquire / release
# ================================================================================================================

class Lease:
    def __init__(self, lock_path: str, token: str, reentrant: bool) -> None:
        self.lock_path = lock_path
        self.token = token
        self.reentrant = reentrant
        self._released = False

    def release(self) -> None:
        if self._released or self.reentrant:
            self._released = True
            return
        self._released = True
        if os.environ.get(ENV_TOKEN) == self.token:
            del os.environ[ENV_TOKEN]
        raw = read_raw(self.lock_path)
        rec = parse_record(raw) if raw else None
        if rec is not None and rec.get("token") == self.token:  # not ours any more after a `release --force`
            remove_quiet(self.lock_path)

    def __enter__(self) -> "Lease":
        return self

    def __exit__(self, *exc) -> None:
        self.release()


def acquire(label: str, cmd: Optional[List[str]] = None, wait_timeout: Optional[float] = None,
            work: Optional[str] = None, quiet: bool = False) -> Lease:
    """Wait (FIFO) for the heavy lock and take it. Raises LockTimeout if wait_timeout (seconds) expires first."""
    cmd = list(cmd) if cmd else [os.path.basename(sys.argv[0])] + sys.argv[1:]
    lock_path, queue_dir = paths(work)

    mine = os.environ.get(ENV_TOKEN)
    if mine:
        state, rec, _ = lock_state(lock_path)
        if state == "held" and rec is not None and rec.get("token") == mine:
            if not quiet:
                say("heavy lock already held by the parent job (%s); running without queueing" % rec.get("label"))
            return Lease(lock_path, mine, True)

    token = uuid.uuid4().hex
    record = make_record(label, cmd, token)
    ticket = enqueue(queue_dir, label, cmd)
    ticket_path = os.path.join(queue_dir, ticket)
    start = time.monotonic()
    last_notice = None
    try:
        while True:
            tickets = live_tickets(queue_dir)
            names = [n for n, _ in tickets]
            if ticket not in names:  # our ticket was removed under us (a hand-cleaned queue): re-enter at the tail
                ticket = enqueue(queue_dir, label, cmd)
                ticket_path = os.path.join(queue_dir, ticket)
                continue
            holder: Optional[Dict] = None
            if names[0] == ticket:
                state, rec, raw = lock_state(lock_path)
                if state == "free":
                    if try_create_lock(lock_path, record):
                        remove_quiet(ticket_path)
                        if not quiet:
                            say("acquired heavy lock (label=%s, waited %.1f s)" % (label, time.monotonic() - start))
                        os.environ[ENV_TOKEN] = token  # children (and nested heavy_lock calls) see who holds it
                        return Lease(lock_path, token, False)
                    continue
                if state == "stale":
                    if not quiet:
                        say("removing stale lock (%s)" % (describe(rec) if rec else "unreadable"))
                    remove_lock_if_unchanged(lock_path, raw)
                    continue
                holder = rec
            else:
                _, holder, _ = lock_state(lock_path)

            elapsed = time.monotonic() - start
            if wait_timeout is not None and elapsed >= wait_timeout:
                raise LockTimeout("heavy lock not obtained within %.1f s (held by %s)"
                                  % (wait_timeout, describe(holder) if holder else "an unknown holder"))
            if not quiet and (last_notice is None or time.monotonic() - last_notice >= NOTICE_SECONDS):
                last_notice = time.monotonic()
                say("waiting for the heavy lock: held by %s; I am #%d of %d waiting"
                    % (describe(holder) if holder else "an unknown holder", names.index(ticket) + 1, len(names)))
            time.sleep(POLL_SECONDS if wait_timeout is None else min(POLL_SECONDS, max(0.0, wait_timeout - elapsed)))
    except BaseException:
        remove_quiet(ticket_path)
        raise
    finally:
        remove_quiet(ticket_path)  # no-op once the lock is ours (the ticket is gone already)


def hold(label: str, cmd: Optional[List[str]] = None, wait_timeout: Optional[float] = None,
         work: Optional[str] = None, quiet: bool = False) -> Lease:
    """Context manager form: `with heavy_lock.hold("label"):`. Also usable as a plain Lease (call .release())."""
    return acquire(label, cmd, wait_timeout, work, quiet)


# ================================================================================================================
# Running a command
# ================================================================================================================

def resolve_command(argv: List[str]) -> Optional[List[str]]:
    exe = shutil.which(argv[0])
    if exe is None and os.path.isfile(argv[0]):
        exe = argv[0]
    return None if exe is None else [exe] + argv[1:]


def _raise_interrupt(signum, frame) -> None:
    raise KeyboardInterrupt()


def install_signal_handlers() -> None:
    for name in ("SIGTERM", "SIGBREAK", "SIGHUP"):  # SIGBREAK also stands for "console window closed" on Windows
        sig = getattr(signal, name, None)
        if sig is not None:
            try:
                signal.signal(sig, _raise_interrupt)
            except (ValueError, OSError):
                pass


def run_child(argv: List[str], token: str) -> int:
    env = dict(os.environ)
    env[ENV_TOKEN] = token
    proc = subprocess.Popen(argv, env=env)
    try:
        while True:
            try:
                rc = proc.wait(timeout=0.5)  # short waits: Ctrl+C is delivered between them
                break
            except subprocess.TimeoutExpired:
                continue
    except KeyboardInterrupt:
        say("interrupted: waiting up to %d s for the command to stop (Ctrl+C again to kill it)" % INTERRUPT_GRACE)
        try:
            proc.wait(timeout=INTERRUPT_GRACE)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            say("killing the command")
            kill_tree(proc)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        return EXIT_INTERRUPTED
    if rc < 0:  # POSIX: killed by signal -rc
        rc = 128 - rc
    return rc


def cmd_run(args: argparse.Namespace, command: List[str]) -> int:
    argv = resolve_command(command)
    if argv is None:
        print("heavy_lock: command not found: %s" % command[0], file=sys.stderr)
        return EXIT_NOT_FOUND
    label = args.label or os.path.splitext(os.path.basename(command[0]))[0]
    install_signal_handlers()
    try:
        lease = acquire(label, command, args.wait_timeout, args.work)
    except LockTimeout as e:
        print("heavy_lock: %s" % e, file=sys.stderr)
        return EXIT_TIMEOUT
    except KeyboardInterrupt:
        say("interrupted while waiting; not run")
        return EXIT_INTERRUPTED
    t0 = time.monotonic()
    rc = EXIT_INTERRUPTED
    try:
        if not lease.reentrant and not kill_on_exit_job() and os.name == "nt":
            say("warning: could not create a job object; if this wrapper is killed hard, the command keeps running "
                "without the lock")
        rc = run_child(argv, lease.token)
        return rc
    except KeyboardInterrupt:  # during job setup or spawn (run_child handles it once the command runs)
        rc = EXIT_INTERRUPTED
        return rc
    finally:
        lease.release()
        if not lease.reentrant:
            say("released heavy lock (label=%s, exit=%s, ran %.1f s)" % (label, rc, time.monotonic() - t0))


def cmd_status(args: argparse.Namespace) -> int:
    lock_path, queue_dir = paths(args.work)
    state, rec, _ = lock_state(lock_path)
    if state == "free":
        print("free")
    elif state == "held" and rec is not None:
        print("held " + describe(rec))
    elif state == "pending":
        print("held (lock file is being written)")
    elif rec is not None:
        print("free (stale lock: holder is gone: %s; the next waiter clears it)" % describe(rec))
    else:
        print("free (stale lock: unreadable file; the next waiter clears it)")
    waiting = live_tickets(queue_dir)
    if waiting:
        print("queue: %d waiting" % len(waiting))
        for i, (_, t) in enumerate(waiting, 1):
            print("  #%d pid=%s label=%s enqueued=%s" % (i, t.get("pid"), t.get("label"), t.get("enqueued", "?")))
    return 0


def cmd_release(args: argparse.Namespace) -> int:
    lock_path, _ = paths(args.work)
    state, rec, raw = lock_state(lock_path)
    if state == "free":
        print("free (nothing to release)")
        return 0
    what = describe(rec) if rec else "unreadable lock file"
    if state in ("held", "pending") and not args.force:
        print("heavy_lock: lock is held by %s; use --force to remove it anyway" % what, file=sys.stderr)
        return 1
    if remove_quiet(lock_path):
        print("released %s lock (%s)" % ("stale" if state == "stale" else "held", what))
        return 0
    print("heavy_lock: could not remove %s" % lock_path, file=sys.stderr)
    return 1


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    command: List[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, command = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(
        prog="heavy_lock.py",
        description="FIFO lock for heavy jobs. Run a command under it: heavy_lock.py [options] -- <command...>")
    ap.add_argument("action", nargs="?", choices=("status", "release"), help="inspect or clear the lock")
    ap.add_argument("--label", help="name shown to waiters (default: the command's program name)")
    ap.add_argument("--wait-timeout", type=float, metavar="S",
                    help="give up (exit %d) if the lock is not obtained within S seconds (default: wait forever)"
                         % EXIT_TIMEOUT)
    ap.add_argument("--work", metavar="DIR", help="work directory (default: $MCLA_WORK or %s)" % DEFAULT_WORK)
    ap.add_argument("--force", action="store_true", help="with `release`: also remove a lock whose holder is alive")
    args = ap.parse_args(argv)
    if args.action and command:
        ap.error("`%s` does not take a command" % args.action)
    if args.force and args.action != "release":
        ap.error("--force only applies to `release`")
    if args.action == "status":
        return cmd_status(args)
    if args.action == "release":
        return cmd_release(args)
    if not command:
        ap.error("missing `-- <command...>`")
    return cmd_run(args, command)


if __name__ == "__main__":
    sys.exit(main())
