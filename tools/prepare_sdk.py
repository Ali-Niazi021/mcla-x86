#!/usr/bin/env python3
"""prepare_sdk.py - fetch, patch, build and install the ReXGlue SDK (PLAN 3.3, Appendix C).

    python tools/prepare_sdk.py [--work W] fetch  [--reference DIR | --no-reference]
    python tools/prepare_sdk.py [--work W] apply
    python tools/prepare_sdk.py [--work W] status
    python tools/prepare_sdk.py [--work W] build  [--config Release] [--jobs N] [--clean] [--dry-run] [--cmake-arg ARG]...
    python tools/prepare_sdk.py [--work W] reset
    python tools/prepare_sdk.py [--work W] check
    python tools/prepare_sdk.py rehash

The series is patches/sdk/series.toml: ReXGlue v0.10.0 (f5337cd) + RenanQueiroz/mcla-recomp's Windows patch list
(v0.3.17, fetched from a pinned clone, never vendored) + the owner's patches (vendored in patches/sdk/). This
script is the only thing that applies it. Paths ($WORK is --work, else $MCLA_WORK, else
C:/Users/Ali/Development/mcla-x86-work):

    $WORK/sdk/src                         the patched SDK tree (git clone of rexglue-sdk at the pin, recursive)
    $WORK/sdk/src/.mcla-x86/series.applied  what was applied: one `<sha256> <name>` line per patch, in order
    $WORK/sdk/install                     `build` installs here
    $WORK/prior/renan-mcla-recomp         Renan's repo at the pin (source of his patch files)
    $WORK/prior/rexglue-sdk-f5337cd       optional --reference for the SDK clone (saves ~1 GB of download)
    $WORK/logs/sdk-build-<ts>.log         everything `build` runs, with the compiler version on top

Commands
  fetch   Clone Renan's repo (if missing) and the SDK (recursive, at the pin) and check both against the pins and
          the patch files against series.toml. Safe to re-run: it completes a cut-short clone, restores files
          missing from submodules, and re-does the libmspack symlink fix-up (git on Windows checks the 10 symlinks
          of thirdparty/libmspack/cabextract/mspack out as small text files; each is replaced by a copy of its
          target, as Renan's make_pc.ps1 does).
  apply   Apply every entry of series.toml in order, idempotently. For each entry: the patch file's SHA-256 must
          equal series.toml's; an entry recorded in series.applied is skipped; otherwise `git apply --reverse
          --check` says "already in the tree" (recorded and skipped), else `git apply` (run in `subdir` if set).
          The record is written after every patch. It stops at the first failure with the patch name and git's
          error (exit 1). A patch that changed after it was applied is never re-applied on top: run `reset`.
  status  Prints, and exits 0 only if everything is applied and nothing drifted:
              sdk f5337cd: series <applied>/<total> applied, <k> drifted
              series fingerprint <hex16>
              install <prefix>: ...
          drift = the patch file's SHA-256 now differs from the one recorded at apply time (or the entry moved,
          or is no longer in the series). The fingerprint is the first 16 hex digits of the SHA-256 over the
          series' SHA-256s, in order, each followed by a newline. Exit 2 if there is no SDK tree.
  build   Under the heavy lock: configure (cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON
          -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FSR=ON with Clang and Ninja by full path), build, and install
          into $WORK/sdk/install. Needs a clean `status`. Prints `installed <prefix>` on success. --clean wipes
          the build directory first; --jobs caps the compiler jobs (default: Ninja's, but one per 0.5 GB of
          available memory if that is fewer); --cmake-arg adds a configure argument (e.g. -DREXGLUE_FSR_SOURCE_DIR=
          to pre-seed FidelityFX). The build directory is wiped by itself when the Windows SDK or the Visual Studio
          tools changed since it was made (stale precompiled headers, Renan issue #21). --dry-run prints the
          toolchain, versions and the three commands and runs nothing (no lock).
  reset   git reset --hard and git clean in the SDK tree and its submodules, forget the record, redo the symlink
          fix-up, then `apply`. For drift recovery, a patch revised after it was applied, or a failed apply.
  check   Compare every patch file with series.toml (needs Renan's clone for his entries; no SDK tree).
  rehash  Rewrite the sha256 of the `local` entries of series.toml from the files in patches/sdk/ (after editing one).

Every git command runs with Renan's settings (his mcla-lib.ps1 Set-GitEnvironment): no system or user config,
core.autocrlf=true, core.symlinks=false, core.longpaths=true, http.sslBackend=schannel, safe.directory=*. The LF
patches only match the CRLF working tree when git converts line endings the same way, so every command here, and
the git started by CMake, must run the same way (the settings are also written into the SDK repos' own config).

Exit codes: 0 ok; 1 a patch, build, or status check failed; 2 setup problem (no tree, tool missing, bad series).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import heavy_lock  # noqa: E402

SERIES_FILE = REPO / "patches" / "sdk" / "series.toml"
PRESET = "win-amd64"
STATE_DIR = ".mcla-x86"
APPLIED_NAME = "series.applied"
INSTALL_STAMP = "mcla-x86-sdk-build.json"
TOOLCHAIN_STAMP = "mcla-toolchain.txt"

EXIT_FAIL = 1
EXIT_SETUP = 2

# Renan's Set-GitEnvironment (scripts/mcla-lib.ps1), same order.
GIT_SETTINGS = [
    ("http.sslBackend", "schannel"), ("safe.directory", "*"), ("core.longpaths", "true"),
    ("core.autocrlf", "true"), ("core.symlinks", "false"), ("core.fscache", "true"),
    ("advice.detachedHead", "false"),  # not Renan's: the pinned checkouts are detached on purpose
]
# With --reference: let each submodule clone borrow objects from the matching submodule of the reference clone.
REFERENCE_SETTINGS = [("submodule.alternateLocation", "superproject"), ("submodule.alternateErrorStrategy", "info")]

PROGRESS_RE = re.compile(r"^\[\d+/\d+\] ")


class ToolError(Exception):
    def __init__(self, message: str, code: int = EXIT_SETUP) -> None:
        super().__init__(message)
        self.code = code


def say(message: str = "") -> None:
    print(message, flush=True)


def posix(p) -> str:
    return Path(p).as_posix()


# ================================================================================================================
# Series
# ================================================================================================================

@dataclass(frozen=True)
class Patch:
    index: int
    name: str
    source: str
    path: str
    sha256: str
    purpose: str
    subdir: str
    bucket: str


@dataclass
class Series:
    pins: Dict[str, str]
    patches: List[Patch]
    fingerprint: str      # 16 hex digits


def series_fingerprint(shas: List[str]) -> str:
    return hashlib.sha256("".join(s + "\n" for s in shas).encode("ascii")).hexdigest()[:16]


def load_series(path: Path = SERIES_FILE) -> Series:
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise ToolError("cannot read %s: %s" % (posix(path), e))
    pins = data.get("pins", {})
    for key in ("sdk_url", "sdk_tag", "sdk_commit", "renan_url", "renan_tag", "renan_commit"):
        if not isinstance(pins.get(key), str) or not pins[key]:
            raise ToolError("%s: [pins] lacks %s" % (posix(path), key))
    patches: List[Patch] = []
    seen = set()
    for i, e in enumerate(data.get("patch", [])):
        try:
            p = Patch(i, e["name"], e["source"], e["path"], e["sha256"], e["purpose"], e["subdir"], e["bucket"])
        except KeyError as k:
            raise ToolError("%s: [[patch]] #%d (%s) lacks %s" % (posix(path), i + 1, e.get("name", "?"), k))
        if p.source not in ("renan", "local"):
            raise ToolError("%s: patch %s: source must be \"renan\" or \"local\"" % (posix(path), p.name))
        if not re.fullmatch(r"[0-9a-f]{64}", p.sha256):
            raise ToolError("%s: patch %s: sha256 is not 64 lowercase hex digits" % (posix(path), p.name))
        if p.name in seen:
            raise ToolError("%s: duplicate patch name %s" % (posix(path), p.name))
        seen.add(p.name)
        patches.append(p)
    if not patches:
        raise ToolError("%s: no [[patch]] entries" % posix(path))
    return Series(pins, patches, series_fingerprint([p.sha256 for p in patches]))


def sha256_file(path: Path) -> Optional[str]:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


# ================================================================================================================
# Layout
# ================================================================================================================

class Layout:
    def __init__(self, args: argparse.Namespace) -> None:
        self.work = Path(heavy_lock.work_dir(args.work))
        self.src = Path(args.src) if args.src else self.work / "sdk" / "src"
        self.prefix = Path(getattr(args, "prefix", None) or self.work / "sdk" / "install")
        self.renan = Path(args.renan_dir) if args.renan_dir else self.work / "prior" / "renan-mcla-recomp"
        self.logs = self.work / "logs"
        self.state = self.src / STATE_DIR
        self.applied_file = self.state / APPLIED_NAME

    def patch_file(self, p: Patch) -> Path:
        return (REPO / p.path) if p.source == "local" else (self.renan / p.path)

    def has_tree(self) -> bool:
        return (self.src / ".git").exists()


# ================================================================================================================
# git
# ================================================================================================================

def git_exe() -> str:
    exe = shutil.which("git")
    if not exe:
        raise ToolError("git not found on PATH")
    return exe


def git_env(extra: Optional[List[Tuple[str, str]]] = None) -> Dict[str, str]:
    env = dict(os.environ)
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["GIT_TERMINAL_PROMPT"] = "0"
    settings = GIT_SETTINGS + (extra or [])
    for i, (k, v) in enumerate(settings):
        env["GIT_CONFIG_KEY_%d" % i] = k
        env["GIT_CONFIG_VALUE_%d" % i] = v
    env["GIT_CONFIG_COUNT"] = str(len(settings))
    return env


def git(cwd: Path, *args: str, extra: Optional[List[Tuple[str, str]]] = None, stdin: Optional[bytes] = None
        ) -> Tuple[int, str]:
    """Run git -C cwd ... with Renan's settings, capturing stdout+stderr (merged) as text."""
    feed = {"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}
    p = subprocess.run([git_exe(), "-C", str(cwd), *args], env=git_env(extra), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, **feed)
    return p.returncode, p.stdout.decode("utf-8", "replace").strip()


def git_inherit(cwd: Optional[Path], *args: str, extra: Optional[List[Tuple[str, str]]] = None) -> int:
    """Like git() but its output goes straight to the console (clone/submodule progress)."""
    cmd = [git_exe()] + (["-C", str(cwd)] if cwd else []) + list(args)
    return subprocess.run(cmd, env=git_env(extra)).returncode


def git_head(repo: Path) -> Optional[str]:
    rc, out = git(repo, "rev-parse", "HEAD")
    return out if rc == 0 and re.fullmatch(r"[0-9a-f]{40}", out) else None


def submodule_paths(src: Path) -> List[Path]:
    rc, out = git(src, "submodule", "status", "--recursive")
    if rc != 0:
        return []
    paths = []
    for line in out.splitlines():
        parts = line[1:].split(" ")
        if len(parts) >= 2:
            paths.append(src / parts[1])
    return paths


def fix_symlink_stubs(src: Path) -> int:
    """With core.symlinks=false git writes a symlink as a small text file holding its target: replace each with a
    copy of the target (make_pc.ps1:895-904). Real symlinks (Developer Mode) and files already replaced are left."""
    links = src / "thirdparty" / "libmspack" / "cabextract" / "mspack"
    if not links.is_dir():
        return 0
    n = 0
    for f in sorted(links.iterdir()):
        try:
            if f.is_symlink() or not f.is_file() or f.stat().st_size >= 200:
                continue
            target = links / f.read_text(encoding="utf-8", errors="replace").strip()
            if target.is_file():
                shutil.copyfile(target, f)
                n += 1
        except (OSError, ValueError):
            continue
    return n


# ================================================================================================================
# fetch
# ================================================================================================================

def check_patch_files(L: Layout, S: Series) -> List[str]:
    """Problems with the patch files (missing, or SHA-256 not the series')."""
    problems = []
    for p in S.patches:
        f = L.patch_file(p)
        got = sha256_file(f)
        if got is None:
            problems.append("%s: patch file not found: %s" % (p.name, posix(f)))
        elif got != p.sha256:
            hint = ""
            if p.source == "renan":
                hint = " (Renan's clone must be at %s with clean patches/: git -C %s checkout -- patches)" % (
                    S.pins["renan_commit"][:8], posix(L.renan))
            else:
                hint = " (after editing a local patch run: python tools/prepare_sdk.py rehash)"
            problems.append("%s: SHA-256 of %s is %s, series.toml has %s%s" % (p.name, posix(f), got[:16], p.sha256[:16], hint))
    if len(problems) > 8:
        problems = problems[:8] + ["... and %d more" % (len(problems) - 8)]
    return problems


def ensure_renan(L: Layout, S: Series) -> None:
    pin = S.pins["renan_commit"]
    if not (L.renan / ".git").exists():
        if L.renan.exists() and any(L.renan.iterdir()):
            raise ToolError("%s exists but is not a git clone; move it away and run fetch again" % posix(L.renan))
        L.renan.parent.mkdir(parents=True, exist_ok=True)
        say("cloning Renan's repo %s -> %s" % (S.pins["renan_url"], posix(L.renan)))
        if git_inherit(None, "clone", "--no-checkout", S.pins["renan_url"], str(L.renan)) != 0:
            raise ToolError("git clone of %s failed" % S.pins["renan_url"], EXIT_FAIL)
        rc, out = git(L.renan, "-c", "advice.detachedHead=false", "checkout", "--detach", pin)
        if rc != 0:
            raise ToolError("cannot check out %s in %s: %s" % (pin, posix(L.renan), out), EXIT_FAIL)
    head = git_head(L.renan)
    if head != pin:
        raise ToolError("Renan's clone %s is at %s, the pin is %s (%s): run `git -C %s checkout --detach %s`"
                        % (posix(L.renan), head, pin, S.pins["renan_tag"], posix(L.renan), pin))
    say("renan  %s @ %s (%s)" % (posix(L.renan), pin[:8], S.pins["renan_tag"]))


def ensure_sdk(L: Layout, S: Series, reference: Optional[Path]) -> None:
    pin = S.pins["sdk_commit"]
    src = L.src
    extra: List[Tuple[str, str]] = []
    used_reference = None
    if reference is not None:
        if (reference / ".git").exists() and git_head(reference) == pin:
            used_reference = reference
            extra = REFERENCE_SETTINGS
        else:
            say("note: %s is not a clone at %s; cloning from the network" % (posix(reference), pin[:8]))
    if not (src / ".git").exists():
        if src.exists() and any(src.iterdir()):
            raise ToolError("%s exists but is not a git checkout; move it away and run fetch again" % posix(src))
        src.parent.mkdir(parents=True, exist_ok=True)
        cmd = ["clone", "--recursive", "--jobs", "8", "--branch", S.pins["sdk_tag"]]
        if used_reference:
            cmd += ["--reference", str(used_reference)]
        cmd += [S.pins["sdk_url"], str(src)]
        say("cloning the SDK %s (%s) -> %s%s" % (S.pins["sdk_url"], S.pins["sdk_tag"], posix(src),
                                               " (objects borrowed from %s)" % posix(used_reference) if used_reference else ""))
        if git_inherit(None, *cmd, extra=extra) != 0:
            raise ToolError("git clone of the SDK failed (a re-run of fetch completes a cut-short clone)", EXIT_FAIL)
    head = git_head(src)
    if head != pin:
        raise ToolError("the SDK tree %s is at %s, the pin is %s (%s)" % (posix(src), head, pin, S.pins["sdk_tag"]))

    # A first clone that was cut short leaves submodules missing or half checked out: complete them.
    if git_inherit(src, "submodule", "update", "--init", "--recursive", "--jobs", "8", extra=extra) != 0:
        raise ToolError("git submodule update failed (re-run fetch to resume)", EXIT_FAIL)
    repos = [src] + submodule_paths(src)
    for repo in repos:
        rc, out = git(repo, "ls-files", "--deleted", "-z")
        missing = [x for x in out.split("\0") if x]
        if missing:
            say("restoring %d missing files in %s" % (len(missing), posix(repo)))
            rc, out = git(repo, "checkout", "--pathspec-from-file=-", "--pathspec-file-nul",
                          stdin=("\0".join(missing) + "\0").encode("utf-8"))
            if rc != 0:
                raise ToolError("could not restore the files missing in %s: %s" % (posix(repo), out), EXIT_FAIL)
    if not (src / "thirdparty" / "FFmpeg" / "config.h").exists():
        raise ToolError("the SDK download is incomplete (thirdparty/FFmpeg/config.h missing): "
                        "re-run fetch, or delete %s and run it again" % posix(src))
    for repo in repos:  # the same settings in the repos' own config, for a git run by hand
        git(repo, "config", "core.autocrlf", "true")
        git(repo, "config", "core.symlinks", "false")
    exclude = src / ".git" / "info" / "exclude"
    if exclude.parent.is_dir():
        text = exclude.read_text(encoding="utf-8") if exclude.exists() else ""
        if "/%s/" % STATE_DIR not in text.splitlines():
            exclude.write_text(text + ("" if text.endswith("\n") or not text else "\n") + "/%s/\n" % STATE_DIR,
                               encoding="utf-8", newline="\n")
    n = fix_symlink_stubs(src)
    say("sdk    %s @ %s (%s)%s" % (posix(src), pin[:8], S.pins["sdk_tag"], "; %d libmspack symlink stubs replaced" % n if n else ""))


def cmd_fetch(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    ensure_renan(L, S)
    problems = check_patch_files(L, S)
    if problems:
        raise ToolError("patch files do not match series.toml:\n  " + "\n  ".join(problems), EXIT_FAIL)
    reference = None
    if not args.no_reference:
        reference = Path(args.reference) if args.reference else L.work / "prior" / "rexglue-sdk-f5337cd"
    ensure_sdk(L, S, reference)
    say("fetch ok: %d patch files verified against series.toml; next: python tools/prepare_sdk.py apply" % len(S.patches))
    return 0


# ================================================================================================================
# apply / status
# ================================================================================================================

def read_applied(L: Layout) -> List[Tuple[str, str]]:
    """[(sha256, name)] in the order recorded."""
    try:
        text = L.applied_file.read_text(encoding="utf-8")
    except OSError:
        return []
    out = []
    for line in text.splitlines():
        m = re.fullmatch(r"([0-9a-f]{64}) (\S+)", line.strip())
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def write_applied(L: Layout, S: Series, recorded: List[Tuple[str, str]]) -> None:
    L.state.mkdir(parents=True, exist_ok=True)
    lines = ["# mcla-x86 SDK series record (tools/prepare_sdk.py): <sha256 of the patch file> <name>, in apply order",
             "# sdk %s renan %s" % (S.pins["sdk_commit"], S.pins["renan_commit"])]
    lines += ["%s %s" % (sha, name) for sha, name in recorded]
    tmp = L.applied_file.with_suffix(".tmp")
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    os.replace(tmp, L.applied_file)


def cmd_apply(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    return do_apply(L, S)


def do_apply(L: Layout, S: Series) -> int:
    if not L.has_tree():
        raise ToolError("no SDK tree at %s: run `python tools/prepare_sdk.py fetch` first" % posix(L.src))
    pin = S.pins["sdk_commit"]
    head = git_head(L.src)
    if head != pin:
        raise ToolError("the SDK tree %s is at %s, the pin is %s: run `reset`, or fetch into a fresh tree"
                        % (posix(L.src), head, pin))
    problems = check_patch_files(L, S)
    if problems:
        raise ToolError("patch files do not match series.toml (nothing applied):\n  " + "\n  ".join(problems), EXIT_FAIL)

    recorded = read_applied(L)
    for i, (sha, name) in enumerate(recorded):
        if i >= len(S.patches) or S.patches[i].name != name:
            raise ToolError("the tree was patched with a different series (record position %d is %s, the series has %s "
                            "there): run `python tools/prepare_sdk.py reset`"
                            % (i + 1, name, S.patches[i].name if i < len(S.patches) else "nothing"), EXIT_FAIL)
        if sha != S.patches[i].sha256:
            raise ToolError("patch %s changed since it was applied (recorded %s, now %s): reverting an older version "
                            "can fail where later patches touch the same lines, so patch afresh: run "
                            "`python tools/prepare_sdk.py reset`" % (name, sha[:16], S.patches[i].sha256[:16]), EXIT_FAIL)

    fix_symlink_stubs(L.src)
    total = len(S.patches)
    applied_now = already = 0
    for p in S.patches:
        if p.index < len(recorded):
            continue
        repo = L.src / p.subdir if p.subdir else L.src
        patch = str(L.patch_file(p))
        tag = "[%*d/%d] %s" % (len(str(total)), p.index + 1, total, p.name)
        rc, _ = git(repo, "apply", "--reverse", "--check", patch)
        if rc == 0:
            already += 1
            say("%s: already in the tree" % tag)
        else:
            rc, out = git(repo, "apply", patch)
            if rc != 0:
                lines = out.splitlines()
                shown = "\n".join(lines[:30]) + ("\n... (%d more lines)" % (len(lines) - 30) if len(lines) > 30 else "")
                sys.stderr.write("error: patch %s (%s) does not apply:\n%s\n" % (p.name, posix(patch), shown))
                sys.stderr.write("       if the tree was patched before and %s was lost, or an earlier apply failed halfway,\n"
                                 "       run: python tools/prepare_sdk.py reset\n" % posix(L.applied_file))
                return EXIT_FAIL
            applied_now += 1
            say("%s: applied" % tag)
        recorded.append((p.sha256, p.name))
        write_applied(L, S, recorded)
    say("apply: %d applied now, %d already in the tree, %d recorded earlier" % (applied_now, already, len(S.patches) - applied_now - already))
    return print_status(L, S)


@dataclass
class Analysis:
    total: int
    applied: int
    drifted: List[Tuple[str, str]]
    notes: List[str]
    head_problem: Optional[str]


def analyse(L: Layout, S: Series) -> Analysis:
    recorded = read_applied(L)
    names = {p.name: p for p in S.patches}
    applied = 0
    drifted: List[Tuple[str, str]] = []
    notes: List[str] = []
    unverified = 0
    for i, (sha, name) in enumerate(recorded):
        p = names.get(name)
        if p is None:
            drifted.append((name, "applied, but no longer in series.toml"))
            continue
        applied += 1
        if i >= len(S.patches) or S.patches[i].name != name:
            drifted.append((name, "applied at position %d, the series has it at %d" % (i + 1, p.index + 1)))
            continue
        f = L.patch_file(p)
        now = sha256_file(f)
        if now is None:
            if p.source == "renan" and not (L.renan / ".git").exists():
                unverified += 1
            else:
                drifted.append((name, "patch file missing: %s" % posix(f)))
        elif now != sha:
            drifted.append((name, "recorded %s, patch file now %s" % (sha[:16], now[:16])))
    if unverified:
        notes.append("%d Renan patch files not re-checked: no clone at %s" % (unverified, posix(L.renan)))
    head = git_head(L.src)
    head_problem = None if head == S.pins["sdk_commit"] else \
        "the SDK tree is at %s, not at the pin %s" % (head or "an unreadable HEAD", S.pins["sdk_commit"])
    return Analysis(len(S.patches), applied, drifted, notes, head_problem)


def install_line(L: Layout, S: Series) -> str:
    stamp = L.prefix / INSTALL_STAMP
    if not (L.prefix / "bin" / "rexglue.exe").exists():
        return "install %s: not installed" % posix(L.prefix)
    try:
        rec = json.loads(stamp.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "install %s: built from an unknown series (no %s)" % (posix(L.prefix), INSTALL_STAMP)
    built = rec.get("series_fingerprint", "?")
    if built == S.fingerprint:
        return "install %s: built from this series (%s, %s, %s)" % (posix(L.prefix), built, rec.get("config", "?"), rec.get("clang", "?"))
    return "install %s: STALE, built from series %s (current %s): run build" % (posix(L.prefix), built, S.fingerprint)


def print_status(L: Layout, S: Series) -> int:
    a = analyse(L, S)
    say("sdk %s: series %d/%d applied, %d drifted" % (S.pins["sdk_commit"][:7], a.applied, a.total, len(a.drifted)))
    say("series fingerprint %s" % S.fingerprint)
    say(install_line(L, S))
    for name, why in a.drifted:
        say("  drifted: %s: %s" % (name, why))
    for note in a.notes:
        say("  note: %s" % note)
    if a.head_problem:
        say("  problem: %s" % a.head_problem)
    if a.applied < a.total:
        say("  not applied yet: run `python tools/prepare_sdk.py apply`")
    if a.drifted:
        say("  drift: run `python tools/prepare_sdk.py reset`")
    return 0 if (a.applied == a.total and not a.drifted and not a.head_problem) else EXIT_FAIL


def cmd_status(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    if not L.has_tree():
        sys.stderr.write("sdk %s: no SDK tree at %s: run `python tools/prepare_sdk.py fetch` then `apply`\n"
                         % (S.pins["sdk_commit"][:7], posix(L.src)))
        return EXIT_SETUP
    return print_status(L, S)


# ================================================================================================================
# reset / check / rehash
# ================================================================================================================

def cmd_reset(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    if not L.has_tree():
        raise ToolError("no SDK tree at %s: run `python tools/prepare_sdk.py fetch` first" % posix(L.src))
    pin = S.pins["sdk_commit"]
    say("resetting %s to %s" % (posix(L.src), pin[:8]))
    rc, out = git(L.src, "reset", "--hard", pin)
    if rc != 0:
        raise ToolError("git reset --hard %s failed: %s" % (pin, out), EXIT_FAIL)
    # -fd without -x: the ignored build directory (out/) and the record directory (.git/info/exclude) stay.
    git(L.src, "clean", "-fdq")
    rc = git_inherit(L.src, "submodule", "update", "--init", "--recursive", "--force", "--jobs", "8")
    if rc != 0:
        raise ToolError("git submodule update --force failed", EXIT_FAIL)
    for repo in submodule_paths(L.src):
        git(repo, "reset", "--hard", "-q")
        git(repo, "clean", "-fdq")
    try:
        L.applied_file.unlink()
    except FileNotFoundError:
        pass
    n = fix_symlink_stubs(L.src)
    if n:
        say("%d libmspack symlink stubs replaced" % n)
    return do_apply(L, S)


def cmd_check(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    problems = check_patch_files(L, S)
    if problems:
        sys.stderr.write("\n".join(problems) + "\n")
        return EXIT_FAIL
    say("check: %d patch files match series.toml (series fingerprint %s)" % (len(S.patches), S.fingerprint))
    return 0


def cmd_rehash(args: argparse.Namespace) -> int:
    S = load_series()
    text = SERIES_FILE.read_bytes().decode("utf-8")
    changed = 0
    for p in S.patches:
        if p.source != "local":
            continue
        now = sha256_file(REPO / p.path)
        if now is None:
            raise ToolError("patch file not found: %s" % p.path)
        if now == p.sha256:
            continue
        pattern = re.compile(r'(\[\[patch\]\]\nname = "%s"\n(?:[^\n]*\n)*?sha256 = ")%s(")' % (re.escape(p.name), p.sha256))
        text, n = pattern.subn(lambda m: m.group(1) + now + m.group(2), text, count=1)
        if n != 1:
            raise ToolError("cannot find the sha256 line of %s in series.toml" % p.name)
        say("%s: %s -> %s" % (p.name, p.sha256[:16], now[:16]))
        changed += 1
    if changed:
        SERIES_FILE.write_bytes(text.encode("utf-8"))
    S = load_series()
    say("rehash: %d local entries updated; series fingerprint %s" % (changed, S.fingerprint))
    return 0


# ================================================================================================================
# build
# ================================================================================================================

def vswhere_install_path() -> Optional[Path]:
    pf86 = os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")
    exe = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not exe.exists():
        return None
    for version in ("[17.1,18.0)", "[17.1,"):  # VS 2022 17.1+ first (what Renan tests with), else any newer
        p = subprocess.run([str(exe), "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-version", version, "-latest", "-property", "installationPath"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        out = p.stdout.strip().splitlines()
        if p.returncode == 0 and out and Path(out[0]).is_dir():
            return Path(out[0])
    return None


def vc_tools_dir(vs: Optional[Path]) -> Optional[str]:
    """env.ps1:108-120: the MSVC tools Clang builds against (otherwise it takes the newest VS, even one without them)."""
    cur = os.environ.get("VCToolsInstallDir")
    if cur and Path(cur).is_dir():
        return cur
    if vs is None:
        return None
    version_file = vs / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
    try:
        version = version_file.read_text(encoding="utf-8").strip().splitlines()[0].strip()
    except (OSError, IndexError):
        return None
    tools = vs / "VC" / "Tools" / "MSVC" / version
    return (str(tools) + "\\") if tools.is_dir() else None


def newest_windows_sdk() -> Optional[str]:
    roots = []
    try:
        import winreg
        for sub in (r"SOFTWARE\Microsoft\Windows Kits\Installed Roots", r"SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots"):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, sub) as k:
                    roots.append(Path(winreg.QueryValueEx(k, "KitsRoot10")[0]))
            except OSError:
                pass
    except ImportError:
        pass
    roots.append(Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits" / "10")
    versions = set()
    for root in roots:
        inc = root / "Include"
        if inc.is_dir():
            for d in inc.iterdir():
                if re.fullmatch(r"10\.[0-9.]+", d.name) and (d / "um" / "d3d12.h").exists():
                    versions.add(d.name)
    if not versions:
        return None
    return max(versions, key=lambda v: tuple(int(x) for x in v.split(".")))


def find_ninja(arg: Optional[str], vs: Optional[Path]) -> str:
    if arg:
        return arg
    found = shutil.which("ninja")
    if found:
        return found
    candidates = []
    if vs:
        candidates.append(vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe")
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates += sorted((Path(local) / "Microsoft" / "WinGet" / "Packages").glob("**/ninja.exe"))
    for c in candidates:
        if Path(c).exists():
            return str(c)
    raise ToolError("ninja.exe not found (not on PATH, not in Visual Studio's CMake, not from winget): pass --ninja PATH")


def available_memory_mb() -> float:
    import ctypes

    class MemStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("sullAvailExtendedVirtual", ctypes.c_ulonglong)]
    try:
        st = MemStatus()
        st.dwLength = ctypes.sizeof(MemStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):  # type: ignore[attr-defined]
            return st.ullAvailPhys / (1024 * 1024)
    except (AttributeError, OSError):
        pass
    return 0.0


def default_jobs() -> Optional[int]:
    """make_pc.ps1 Get-JobLimit: one compiler job per 0.5 GB available, but only when that is fewer than Ninja's own
    count (CPU threads + 2). None = leave it to Ninja."""
    mb = available_memory_mb()
    if mb <= 0:
        return None
    by_memory = max(1, int(mb // 512))
    return by_memory if by_memory < (os.cpu_count() or 1) + 2 else None


def preset_binary_dir(src: Path, preset: str) -> Path:
    try:
        data = json.loads((src / "CMakePresets.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ToolError("cannot read %s: %s" % (posix(src / "CMakePresets.json"), e))
    presets = {p["name"]: p for p in data.get("configurePresets", [])}
    if preset not in presets:
        raise ToolError("the SDK's CMakePresets.json has no configure preset %s" % preset)

    def lookup(name: str) -> Optional[str]:
        p = presets.get(name, {})
        if "binaryDir" in p:
            return p["binaryDir"]
        parents = p.get("inherits", [])
        for parent in ([parents] if isinstance(parents, str) else parents):
            r = lookup(parent)
            if r:
                return r
        return None

    template = lookup(preset)
    if not template:
        raise ToolError("preset %s has no binaryDir" % preset)
    return Path(template.replace("${sourceDir}", src.as_posix()).replace("${presetName}", preset))


def rmtree_force(path: Path) -> None:
    """rmtree that also removes read-only files (git packs in a FetchContent clone) and paths over 260 characters."""
    def onexc(func, p, exc) -> None:
        os.chmod(p, stat.S_IWRITE)
        func(p)

    target = os.path.abspath(path)
    shutil.rmtree("\\\\?\\" + target if os.name == "nt" else target, onexc=onexc)


def kill_tree(proc: subprocess.Popen) -> None:
    try:
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        proc.kill()


def run_logged(cmd: List[str], cwd: Path, env: Dict[str, str], log, label: str) -> int:
    """Run cmd with stdout+stderr into the log, echoing a ninja progress line every 30 s."""
    log.write("\n===== %s: %s\n===== cwd %s\n" % (label, " ".join(cmd), cwd))
    log.flush()
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=str(cwd), env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    last_echo = t0
    try:
        for raw in proc.stdout:  # type: ignore[union-attr]
            line = raw.decode("utf-8", "replace")
            log.write(line if line.endswith("\n") else line + "\n")
            now = time.monotonic()
            if PROGRESS_RE.match(line) and now - last_echo >= 30.0:
                last_echo = now
                say("  %s %4ds %s" % (label, now - t0, line.strip()[:110]))
        rc = proc.wait()
    except BaseException:
        kill_tree(proc)
        raise
    log.write("===== %s finished: exit %d after %.0f s\n" % (label, rc, time.monotonic() - t0))
    log.flush()
    return rc


def tool_version(cmd: List[str], env: Dict[str, str]) -> str:
    try:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        return p.stdout.strip()
    except (OSError, subprocess.SubprocessError) as e:
        return "(failed: %s)" % e


def layout_problems(prefix: Path) -> Tuple[List[str], List[str]]:
    """(missing required files, missing optional ones) of an installed SDK (make_pc.ps1 SdkLayoutProblem)."""
    need = ["bin/rexglue.exe", "bin/rexruntime.dll", "bin/rexgpu-xenos.dll", "lib/rexruntime.lib",
            "lib/cmake/rexglue/rexglueConfig.cmake", "include/rex"]
    optional = ["bin/amd_fidelityfx_dx12.dll"]
    return [n for n in need if not (prefix / n).exists()], [n for n in optional if not (prefix / n).exists()]


@dataclass
class BuildPlan:
    llvm: Path
    cmake: str
    ninja: str
    bindir: Path
    jobs: Optional[int]
    env: Dict[str, str]
    toolchain: str            # what the build directory is stamped with: Windows SDK + MSVC tools
    commands: List[Tuple[str, List[str]]]


def plan_build(L: Layout, args: argparse.Namespace) -> BuildPlan:
    llvm = Path(args.llvm) if args.llvm else L.work / "toolchain" / "llvm-20.1.8"
    clang, clangxx = llvm / "bin" / "clang.exe", llvm / "bin" / "clang++.exe"
    for exe in (clang, clangxx):
        if not exe.exists():
            raise ToolError("%s not found (Clang 20.1.8 is expected in %s; --llvm DIR)" % (posix(exe), posix(llvm)))
    cmake = shutil.which("cmake")
    if not cmake:
        raise ToolError("cmake not found on PATH")
    vs = vswhere_install_path()
    ninja = find_ninja(args.ninja, vs)
    bindir = preset_binary_dir(L.src, PRESET)
    try:
        bindir.relative_to(L.src / "out" / "build")
    except ValueError:
        raise ToolError("unexpected build directory %s (expected below %s)" % (posix(bindir), posix(L.src / "out" / "build")))
    if len(posix(bindir)) > 100:
        say("warning: the build directory path is %d characters; FidelityFX's shader compiler paths must stay under "
            "260 (Renan advises a short root)" % len(posix(bindir)))

    jobs = args.jobs if args.jobs else default_jobs()
    env = git_env()  # CMake's FetchContent (FidelityFX) runs git: same settings as Renan's build
    env["PATH"] = os.pathsep.join([str(llvm / "bin"), str(Path(ninja).parent), env.get("PATH", "")])
    vct = vc_tools_dir(vs)
    if vct:
        env["VCToolsInstallDir"] = vct
    if not env.get("VULKAN_SDK"):
        vk = sorted((d for d in Path("C:/VulkanSDK").glob("*") if re.fullmatch(r"\d+(\.\d+)+", d.name)),
                    key=lambda d: tuple(int(x) for x in d.name.split(".")))
        if vk:
            env["VULKAN_SDK"] = str(vk[-1])
    if jobs:
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(jobs)
    toolchain = "Windows SDK %s, %s" % (newest_windows_sdk() or "?", vct or "VCToolsInstallDir unset")

    configure = [cmake, "--preset", PRESET, "-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_ENABLE_TRACY=OFF",
                 "-DREXGLUE_ENABLE_FSR=ON", "-DCMAKE_C_COMPILER=" + clang.as_posix(),
                 "-DCMAKE_CXX_COMPILER=" + clangxx.as_posix(), "-DCMAKE_MAKE_PROGRAM=" + Path(ninja).as_posix()]
    ffx_signed = L.work / "toolchain" / "ffx-signed"  # patch 0470's pinned signed FidelityFX DLLs, cached (58 MB)
    if ffx_signed.is_dir():
        configure.append("-DREXGLUE_FSR_SIGNED_DIR=" + ffx_signed.as_posix())
    configure += args.cmake_arg or []
    build = [cmake, "--build", str(bindir), "--config", args.config]
    if jobs:
        build += ["--parallel", str(jobs)]
    install = [cmake, "--install", str(bindir), "--config", args.config, "--prefix", str(L.prefix)]
    return BuildPlan(llvm, cmake, ninja, bindir, jobs, env, toolchain,
                     [("configure", configure), ("build", build), ("install", install)])


def cmd_build(args: argparse.Namespace) -> int:
    S = load_series()
    L = Layout(args)
    if not L.has_tree():
        raise ToolError("no SDK tree at %s: run fetch and apply first" % posix(L.src))
    a = analyse(L, S)
    if a.applied != a.total or a.drifted or a.head_problem:
        print_status(L, S)
        raise ToolError("the series is not cleanly applied: fix that before building", EXIT_FAIL)
    plan = plan_build(L, args)
    env, bindir = plan.env, plan.bindir
    clangxx = plan.llvm / "bin" / "clang++.exe"

    if args.dry_run:
        say("dry run: nothing is built, no lock is taken")
        say("series fingerprint %s, %d patches, config %s, jobs %s" % (S.fingerprint, a.total, args.config, plan.jobs or "ninja default"))
        say("toolchain: %s" % plan.toolchain)
        say("VULKAN_SDK=%s" % env.get("VULKAN_SDK", "(unset: FidelityFX Vulkan runtime not built)"))
        say("clang++: %s" % (tool_version([str(clangxx), "--version"], env).splitlines() or ["?"])[0])
        say("cmake:   %s (%s)" % (plan.cmake, (tool_version([plan.cmake, "--version"], env).splitlines() or ["?"])[0]))
        say("ninja:   %s (%s)" % (plan.ninja, tool_version([plan.ninja, "--version"], env)))
        say("source %s\nbuild dir %s (%s)\nprefix %s" % (posix(L.src), posix(bindir), "exists" if bindir.exists() else "new", posix(L.prefix)))
        for label, cmd in plan.commands:
            say("%s: %s" % (label, " ".join(cmd)))
        return 0

    L.logs.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = L.logs / ("sdk-build-%s.log" % ts)

    with heavy_lock.hold("sdk-build", cmd=["prepare_sdk.py", "build", "--config", args.config], work=str(L.work)):
        token = os.environ.get(heavy_lock.ENV_TOKEN)
        if token:
            env[heavy_lock.ENV_TOKEN] = token
        stamp = bindir / TOOLCHAIN_STAMP
        if bindir.exists():
            then = stamp.read_text(encoding="utf-8").strip() if stamp.exists() else None
            installed = (L.prefix / "bin" / "rexglue.exe").exists()
            if args.clean:
                say("--clean: removing %s" % posix(bindir))
                rmtree_force(bindir)
            elif (then and then != plan.toolchain) or (not then and not installed):
                say("the Windows SDK or Visual Studio changed since the SDK build folder was made (or it was never "
                    "finished): starting it again")
                rmtree_force(bindir)

        with open(log_path, "w", encoding="utf-8", errors="replace", newline="\n") as log:
            log.write("prepare_sdk.py build %s\n" % ts)
            log.write("sdk %s (%s) series fingerprint %s, %d patches\n" % (S.pins["sdk_commit"], S.pins["sdk_tag"], S.fingerprint, a.total))
            log.write("renan %s (%s)\n" % (S.pins["renan_commit"], S.pins["renan_tag"]))
            log.write("src %s\nbuild dir %s\nprefix %s\nconfig %s\njobs %s\n" % (posix(L.src), posix(bindir), posix(L.prefix), args.config, plan.jobs or "ninja default"))
            log.write("toolchain: %s\nVULKAN_SDK=%s\n" % (plan.toolchain, env.get("VULKAN_SDK", "(unset: FidelityFX Vulkan runtime not built)")))
            clang_version = tool_version([str(clangxx), "--version"], env)
            log.write("\n$ clang++ --version\n%s\n" % clang_version)
            log.write("\n$ cmake --version\n%s\n" % tool_version([plan.cmake, "--version"], env))
            log.write("\n$ ninja --version\n%s\n" % tool_version([plan.ninja, "--version"], env))
            log.flush()
            say("build log: %s" % posix(log_path))
            say(clang_version.splitlines()[0] if clang_version else "clang++: no version output")

            for label, cmd in plan.commands:
                say("%s ..." % label)
                rc = run_logged(cmd, L.src, env, log, label)
                if rc != 0:
                    log.flush()
                    tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
                    sys.stderr.write("\n".join(tail) + "\n")
                    sys.stderr.write("error: sdk %s failed (exit %d); full log: %s\n" % (label, rc, posix(log_path)))
                    return EXIT_FAIL
                if label == "configure":
                    stamp.write_text(plan.toolchain + "\n", encoding="utf-8", newline="\n")

        missing, optional = layout_problems(L.prefix)
        if missing:
            sys.stderr.write("error: the install is incomplete, it lacks %s; log: %s\n" % (", ".join(missing), posix(log_path)))
            return EXIT_FAIL
        if optional:
            say("warning: FSR runtime missing from the install (%s): FidelityFX was not built (offline? see the log); "
                "the SDK works without FSR upscaling/frame generation" % ", ".join(optional))
        record = {"series_fingerprint": S.fingerprint, "sdk_commit": S.pins["sdk_commit"], "renan_commit": S.pins["renan_commit"],
                  "patches": a.total, "config": args.config,
                  "clang": clang_version.splitlines()[0] if clang_version else "?", "toolchain": plan.toolchain,
                  "built_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                  "log": posix(log_path)}
        (L.prefix / INSTALL_STAMP).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8", newline="\n")
        say("installed %s" % posix(L.prefix))
        return 0


# ================================================================================================================
# CLI
# ================================================================================================================

def add_common(p: argparse.ArgumentParser, default) -> None:
    """The options every command takes, accepted before or after the command name. Each parser gets its own
    actions: the subparsers' default is SUPPRESS so that they never overwrite what the top-level parser parsed."""
    p.add_argument("--work", metavar="W", default=default,
                   help="$WORK (default: env MCLA_WORK, else %s)" % heavy_lock.DEFAULT_WORK)
    p.add_argument("--src", metavar="DIR", default=default, help="the SDK tree (default: $WORK/sdk/src)")
    p.add_argument("--renan-dir", metavar="DIR", default=default,
                   help="Renan's clone at the pin (default: $WORK/prior/renan-mcla-recomp)")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="prepare_sdk.py", description=__doc__.split("\n\n")[0])
    add_common(ap, None)
    sub = ap.add_subparsers(dest="command", required=True)

    def command(name: str, help_text: str, func) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help_text)
        add_common(p, argparse.SUPPRESS)
        p.set_defaults(func=func)
        return p

    p = command("fetch", "clone Renan's repo and the SDK at the pins", cmd_fetch)
    p.add_argument("--reference", metavar="DIR", help="borrow the SDK's objects from this clone "
                   "(default: $WORK/prior/rexglue-sdk-f5337cd if it is at the pin)")
    p.add_argument("--no-reference", action="store_true", help="clone from the network only")

    command("apply", "apply the series (idempotent)", cmd_apply)
    command("status", "series applied/drift and the install state", cmd_status)

    p = command("build", "configure, build and install the SDK (heavy lock)", cmd_build)
    p.add_argument("--config", default="Release", choices=["Release", "RelWithDebInfo", "Debug"])
    p.add_argument("--jobs", type=int, metavar="N", help="compiler jobs (default: Ninja's, capped by free memory)")
    p.add_argument("--clean", action="store_true", help="wipe the build directory first")
    p.add_argument("--prefix", metavar="DIR", help="install prefix (default: $WORK/sdk/install)")
    p.add_argument("--llvm", metavar="DIR", help="LLVM root (default: $WORK/toolchain/llvm-20.1.8)")
    p.add_argument("--ninja", metavar="PATH", help="ninja.exe (default: PATH, Visual Studio's, winget's)")
    p.add_argument("--cmake-arg", action="append", metavar="ARG", help="extra configure argument (repeatable)")
    p.add_argument("--dry-run", action="store_true", help="print the toolchain and the commands, run nothing")

    command("reset", "git reset --hard + clean the SDK tree, then apply", cmd_reset)
    command("check", "compare the patch files with series.toml", cmd_check)
    command("rehash", "rewrite the sha256 of the local entries of series.toml", cmd_rehash)
    return ap


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ToolError as e:
        sys.stderr.write("error: %s\n" % e)
        return e.code
    except KeyboardInterrupt:
        sys.stderr.write("interrupted\n")
        return 130
    except heavy_lock.LockTimeout as e:
        sys.stderr.write("error: %s\n" % e)
        return heavy_lock.EXIT_TIMEOUT


if __name__ == "__main__":
    sys.exit(main())
