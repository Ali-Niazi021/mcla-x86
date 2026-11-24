#!/usr/bin/env python3
"""guard_no_game_data.py - legal guard for the MCLA x86 repo (PLAN 3.9, 5, 7.3, Appendix C).

The repo holds only our own code, config, scripts and docs. This guard refuses anything that looks like the game, or
like something derived from it (translated code, builds, logs, captures, saves).

Usage
    python tools/guard_no_game_data.py --staged       the files in the git index that are being committed
                                                      (git diff --cached, added/copied/modified/renamed); this is
                                                      what .githooks/pre-commit runs
    python tools/guard_no_game_data.py PATH...        the same checks on files (or directory trees) on disk; for
                                                      tests. A relative PATH is judged under that name, an absolute
                                                      PATH under the repo root, or by its bare file name if it lies
                                                      outside the repo.

Exit codes: 0 clean (prints `guard ok: <n> files`), 1 offending files (listed, one `BLOCKED <path> [reasons]` per
line), 2 the guard itself could not run (git failed, bad usage). A guard that cannot run blocks the commit too.

A file is blocked when any of these holds:
  1. size > 2 MB (2 * 1024 * 1024 bytes);
  2. its path matches a blocked rule. Two rule sets are evaluated with gitignore semantics (last match wins, `!`
     re-includes, a trailing `/` means directory, a path inside an ignored directory is ignored, case-insensitive):
       - BUILTIN_RULES below: game and derived extensions and directories from PLAN 5. Hard-coded on purpose, so
         that editing .gitignore cannot disable the guard.
       - the repo's own .gitignore. A file that git would ignore can only be staged with `git add -f`; the guard
         treats that as a mistake. (If a file was tracked before a rule was added, `git rm --cached` it.)
  3. it is C/C++ source or a header (see CODE_EXTENSIONS) and contains a generated-code marker (MARKERS): the
     output of the code generator defines PPC_FUNC functions and the register table nfsmw_register.cpp, and the app
     entry macro REX_DEFINE_APP belongs to src/host/ only (it is allowed there). Other file types are not scanned:
     docs, TOML comments, Python tools and CMake files legitimately *name* those things (the iOS codegen TOMLs
     do). Exempt from the scan: docs/ and *.md (they mention the markers) and this file.

Staged content is read from the index (git cat-file on the staged blob), never from the working tree, and a file
over 2 MB is rejected from its size alone (the blob is not read).
"""

import argparse
import os
import re
import subprocess
import sys
from typing import Dict, Iterable, List, NamedTuple, Optional, Tuple

MAX_BYTES = 2 * 1024 * 1024

# Hard-coded rules (gitignore syntax). Mirrors the legal-critical part of .gitignore and PLAN 5.
BUILTIN_RULES = """
# the game's files
assets/
*.iso
*.xex
*.xexp
*.xex.*
*.pe
*.big
*.bun
*.viv
*.bin
*.rpf
*.bik
nxeart
*.xsh
*.xpso
*.vkcache
*.metallib
*.fxc
*.wtd
*.wft
*.wdr
*.wdd
*.wbd
*.wbn
*.wpfl
*.wtx
$SystemUpdate/
PS3_GAME/
PS3_DISC.SFB
*.pup
*.self
*.sprx
*.elf
*.god
# saves
*.sav
*.mclasave
# generated code and builds (the translated game code; the built app contains it)
generated/
build/
out/
cmake-build-*/
*.exe
*.pdb
*.obj
*.ilk
*.lib
*.exp
*.dll
*.dylib
*.ipa
*.app/
*.dSYM/
*.7z
*.part
# logs, captures, traces, dumps, run artefacts
logs/
*.log
captures/
*.gputrace
*.trace
*.etl
*.dmp
*.mdmp
*.csv
*.png
!docs/images/*.png
*.jpg
*.jpeg
*.dds
*.pso
*.cache
cache/
shaderstorage/
user/
fixtures/
runs/
/work/
mcla-x86-work/
""".splitlines()

MARKERS = (b"PPC_FUNC", b"nfsmw_register", b"REX_DEFINE_APP")
HOST_ONLY_MARKERS = (b"REX_DEFINE_APP",)
HOST_DIR = "src/host/"
CODE_EXTENSIONS = frozenset((".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".h++",
                             ".inc", ".inl", ".ipp", ".tcc", ".m", ".mm"))
SELF_PATH = "tools/guard_no_game_data.py"
MARKER_RX = re.compile(b"|".join(re.escape(m) for m in MARKERS))

EXIT_OK = 0
EXIT_BLOCKED = 1
EXIT_ERROR = 2


class GuardError(Exception):
    """The guard could not run: exit 2."""


# ================================================================================================================
# gitignore-style rules
# ================================================================================================================

class Rule(NamedTuple):
    source: str      # "built-in" or ".gitignore"
    lineno: int
    text: str        # the pattern as written
    negated: bool
    dir_only: bool
    rx: "re.Pattern[str]"


def _glob_to_regex(p: str) -> str:
    out: List[str] = []
    i, n = 0, len(p)
    while i < n:
        c = p[i]
        if c == "*":
            if p.startswith("**", i):
                j = i + 2
                if p.startswith("/", j) and (i == 0 or p[i - 1] == "/"):  # "**/": zero or more directories
                    out.append("(?:.*/)?")
                    i = j + 1
                    continue
                if j == n and i > 0 and p[i - 1] == "/":                  # "/**": everything beneath
                    out.append(".*")
                    i = j
                    continue
                out.append("[^/]*")
                i = j
                continue
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        elif c == "[":
            j = i + 1
            if j < n and p[j] in "!^":
                j += 1
            if j < n and p[j] == "]":
                j += 1
            while j < n and p[j] != "]":
                j += 1
            if j >= n:
                out.append(re.escape(c))
                i += 1
            else:
                body = p[i + 1:j].replace("\\", "\\\\")
                if body[:1] in ("!", "^"):
                    body = "^" + body[1:]
                out.append("[" + body + "]")
                i = j + 1
        elif c == "\\" and i + 1 < n:
            out.append(re.escape(p[i + 1]))
            i += 2
        else:
            out.append(re.escape(c))
            i += 1
    return "".join(out)


def parse_rules(lines: Iterable[str], source: str) -> List[Rule]:
    rules: List[Rule] = []
    for lineno, line in enumerate(lines, 1):
        pat = re.sub(r"(?<!\\) +$", "", line.rstrip("\r\n"))  # trailing spaces do not count unless escaped
        if not pat or pat.startswith("#"):
            continue
        negated = False
        if pat.startswith("!"):
            negated = True
            pat = pat[1:]
        elif pat.startswith(("\\!", "\\#")):
            pat = pat[1:]
        dir_only = pat.endswith("/")
        pat = pat.rstrip("/")
        if not pat:
            continue
        anchored = "/" in pat
        pat = pat.lstrip("/")
        body = _glob_to_regex(pat)
        rx = re.compile("^" + ("" if anchored else "(?:.*/)?") + body + "$", re.IGNORECASE)
        rules.append(Rule(source, lineno, line.rstrip("\r\n"), negated, dir_only, rx))
    return rules


class RuleSet:
    def __init__(self, rules: List[Rule]) -> None:
        self.rules = rules
        self._dir_cache: Dict[str, Optional[Rule]] = {}

    def _last_match(self, path: str, is_dir: bool) -> Optional[Rule]:
        hit = None
        for r in self.rules:
            if r.dir_only and not is_dir:
                continue
            if r.rx.match(path):
                hit = r
        return hit

    def blocking_rule(self, path: str) -> Optional[Rule]:
        """The rule that makes `path` (a file, '/'-separated, relative) ignored, or None."""
        parts = path.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in self._dir_cache:
                self._dir_cache[d] = self._last_match(d, True)
            r = self._dir_cache[d]
            if r is not None and not r.negated:
                return r
        r = self._last_match(path, False)
        return r if r is not None and not r.negated else None


# ================================================================================================================
# Checks
# ================================================================================================================

def human_size(n: int) -> str:
    return "%.2f MB" % (n / (1024.0 * 1024.0)) if n >= 1024 * 1024 else "%d bytes" % n


def needs_marker_scan(path: str) -> bool:
    low = path.lower()
    if low == SELF_PATH or low.startswith("docs/") or low.endswith(".md"):
        return False
    return os.path.splitext(low)[1] in CODE_EXTENSIONS


def find_markers(path: str, data: bytes) -> List[str]:
    """Reasons for generated-code markers in `data` (the content of `path`)."""
    in_host = path.lower().startswith(HOST_DIR)
    seen: Dict[bytes, int] = {}
    for m in MARKER_RX.finditer(data):
        marker = m.group(0)
        if marker in HOST_ONLY_MARKERS and in_host:
            continue
        if marker not in seen:
            seen[marker] = data.count(b"\n", 0, m.start()) + 1
    return ["generated-code marker %s at line %d" % (k.decode(), seen[k]) for k in MARKERS if k in seen]


def path_reasons(path: str, size: int, rulesets: List[RuleSet]) -> List[str]:
    reasons: List[str] = []
    if size > MAX_BYTES:
        reasons.append("size %s exceeds the %d MB limit" % (human_size(size), MAX_BYTES >> 20))
    for rs in rulesets:
        r = rs.blocking_rule(path)
        if r is not None:
            where = "built-in rule" if r.source == "built-in" else ".gitignore line %d" % r.lineno
            reasons.append("blocked path (%s: %s)" % (where, r.text))
            break  # one path reason is enough; the built-in set comes first
    return reasons


# ================================================================================================================
# Sources of files: the git index, or the disk
# ================================================================================================================

class Item(NamedTuple):
    path: str       # name the rules are applied to
    size: int
    mode: str       # git mode ("" for disk files)
    ref: str        # blob sha (staged) or filesystem path (disk)


def git(*args: str, input_bytes: Optional[bytes] = None) -> bytes:
    try:
        p = subprocess.run(("git",) + args, input=input_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as e:
        raise GuardError("cannot run git: %s" % e)
    if p.returncode != 0:
        raise GuardError("git %s failed (%d): %s" % (" ".join(args), p.returncode,
                                                     p.stderr.decode("utf-8", "replace").strip()))
    return p.stdout


def repo_root_from_git() -> str:
    return git("rev-parse", "--show-toplevel").decode("utf-8", "surrogateescape").strip()


def staged_items() -> List[Item]:
    # --raw -z gives ":<old mode> <new mode> <old sha> <new sha> <status>\0<path>\0" per file; --no-renames keeps one
    # path per record, so a rename is an add of the new path. Same file set as `--name-only -z --diff-filter=ACMR`.
    out = git("diff", "--cached", "--raw", "-z", "--no-abbrev", "--no-renames", "--diff-filter=ACMR")
    tokens = out.split(b"\0")
    if tokens and tokens[-1] == b"":
        tokens.pop()
    if len(tokens) % 2:
        raise GuardError("unexpected `git diff --cached --raw -z` output (%d tokens)" % len(tokens))
    entries: List[Tuple[str, str, str]] = []
    for meta, rawpath in zip(tokens[0::2], tokens[1::2]):
        fields = meta.decode("ascii", "replace").lstrip(":").split(" ")
        if len(fields) != 5:
            raise GuardError("unexpected raw diff record %r" % meta)
        entries.append((rawpath.decode("utf-8", "surrogateescape"), fields[1], fields[3]))
    if not entries:
        return []
    sizes = git("cat-file", "--batch-check", input_bytes=("\n".join(sha for _, _, sha in entries) + "\n").encode()
                ).decode("ascii", "replace").splitlines()
    if len(sizes) != len(entries):
        raise GuardError("git cat-file --batch-check answered %d of %d objects" % (len(sizes), len(entries)))
    items: List[Item] = []
    for (path, mode, sha), line in zip(entries, sizes):
        parts = line.split(" ")
        if mode == "160000":  # submodule pointer: no blob, nothing to size or scan
            items.append(Item(path, 0, mode, sha))
        elif len(parts) == 3 and parts[1] == "blob":
            items.append(Item(path, int(parts[2]), mode, sha))
        else:
            raise GuardError("staged object for %s is not readable: %r" % (path, line))
    return items


class BlobReader:
    """Lock-step `git cat-file --batch` (one request, one answer: no pipe deadlock, memory bounded by one blob)."""

    def __init__(self) -> None:
        self._p: Optional[subprocess.Popen] = None

    def read(self, sha: str) -> bytes:
        if self._p is None:
            try:
                self._p = subprocess.Popen(("git", "cat-file", "--batch"), stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE)
            except OSError as e:
                raise GuardError("cannot run git: %s" % e)
        assert self._p.stdin is not None and self._p.stdout is not None
        self._p.stdin.write(sha.encode("ascii") + b"\n")
        self._p.stdin.flush()
        header = self._p.stdout.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            raise GuardError("git cat-file --batch could not read %s: %r" % (sha, header))
        size = int(header[2])
        data = self._p.stdout.read(size)
        self._p.stdout.read(1)  # the LF after the content
        if len(data) != size:
            raise GuardError("short read of %s from git" % sha)
        return data

    def close(self) -> None:
        if self._p is not None:
            if self._p.stdin:
                self._p.stdin.close()
            self._p.wait()
            if self._p.stdout:
                self._p.stdout.close()


def disk_items(paths: List[str], root: str) -> List[Item]:
    files: List[str] = []
    for p in paths:
        if os.path.isdir(p):
            for dp, dns, fns in os.walk(p):
                dns[:] = sorted(d for d in dns if d != ".git")
                files.extend(os.path.join(dp, f) for f in sorted(fns))
        elif os.path.isfile(p):
            files.append(p)
        else:
            raise GuardError("no such file or directory: %s" % p)
    items: List[Item] = []
    root_abs = os.path.normcase(os.path.abspath(root))
    for f in files:
        if os.path.isabs(f):
            rel = os.path.relpath(f, root)
            inside = os.path.normcase(os.path.abspath(f)).startswith(root_abs + os.sep)
            name = rel if inside else os.path.basename(f)
        else:
            name = f
        name = os.path.normpath(name).replace("\\", "/")
        items.append(Item(name, os.path.getsize(f), "", f))
    return items


# ================================================================================================================
# Driver
# ================================================================================================================

def load_rulesets(root: str) -> List[RuleSet]:
    sets = [RuleSet(parse_rules(BUILTIN_RULES, "built-in"))]
    gi = os.path.join(root, ".gitignore")
    try:
        with open(gi, "r", encoding="utf-8", errors="replace") as f:
            sets.append(RuleSet(parse_rules(f.read().splitlines(), ".gitignore")))
    except FileNotFoundError:
        pass
    except OSError as e:
        raise GuardError("cannot read %s: %s" % (gi, e))
    return sets


def check(items: List[Item], rulesets: List[RuleSet], staged: bool) -> Dict[str, List[str]]:
    blocked: Dict[str, List[str]] = {}
    reader = BlobReader() if staged else None
    try:
        for it in items:
            reasons = path_reasons(it.path, it.size, rulesets)
            if it.size <= MAX_BYTES and it.mode != "160000" and needs_marker_scan(it.path):
                if staged:
                    assert reader is not None
                    data = reader.read(it.ref)
                else:
                    with open(it.ref, "rb") as f:
                        data = f.read()
                reasons += find_markers(it.path, data)
            if reasons:
                blocked[it.path] = reasons
    finally:
        if reader is not None:
            reader.close()
    return blocked


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="guard_no_game_data.py",
                                 description="Refuse game data, derived files and generated code (PLAN 3.9).")
    ap.add_argument("--staged", action="store_true", help="check the files staged in git")
    ap.add_argument("paths", nargs="*", metavar="PATH", help="check these files or directory trees on disk")
    args = ap.parse_args(argv)
    if args.staged == bool(args.paths):
        ap.error("give either --staged or one or more PATHs")

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")

    try:
        if args.staged:
            root = repo_root_from_git()
            items = staged_items()
        else:
            root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            items = disk_items(args.paths, root)
        blocked = check(items, load_rulesets(root), args.staged)
    except GuardError as e:
        print("guard: error: %s" % e, file=sys.stderr)
        return EXIT_ERROR

    if not blocked:
        print("guard ok: %d files" % len(items))
        return EXIT_OK
    print("guard FAILED: %d of %d files blocked (PLAN 3.9: the repo holds no game data, no derived files, no "
          "generated code)" % (len(blocked), len(items)))
    for path in sorted(blocked):
        print("BLOCKED %s  [%s]" % (path, "; ".join(blocked[path])))
    if args.staged:
        print("unstage with: git restore --staged <path>   (or git rm --cached <path> if it was already tracked)")
    return EXIT_BLOCKED


if __name__ == "__main__":
    sys.exit(main())
