#!/usr/bin/env python3
"""Run ReXGlue's `rexglue codegen` on the MCLA manifest (the game's recompiled C++ goes to $WORK).

    python tools/codegen.py [--baseline] [--work W] [--game G] [--rexglue PATH]
                            [--xex PATH] [--use-stamp] [--no-verify] [--manifest-only]

  (no flag)     full config: mcla/codegen/mcla_manifest.toml with all its includes (overrides, harvest, huecos,
                the four hooks files).
  --baseline    the same manifest with includes = ["overrides.toml"] only: no harvest, no huecos, no hooks.
  --work W      $WORK (default: env MCLA_WORK, else C:/Users/Ali/Development/mcla-x86-work)
  --game G      $GAME, the game files (default: env MCLA_GAME, else C:/Users/Ali/Development/dumps/mcla-x360).
                default.xex is taken from $GAME/game/ if it is there, else from $GAME/.
  --rexglue P   the CLI (default: $WORK/sdk/install/bin/rexglue.exe)
  --xex PATH    use this default.xex (its folder becomes the codegen's game root)
  --use-stamp   let rexglue skip the run when its inputs match the recorded stamp. Off by default: the stamp does
                not see a changed SDK binary (a GapFill patch, say), so the default is always `--ignore-stamp`.
                That costs nothing in rebuilds: unchanged output files are not rewritten.
  --no-verify   do not require default.xex to be the supported build (SHA-256 c386f400...a432)
  --manifest-only  write the resolved manifest and its include copies, print where, and stop (no SDK needed)

What it does (PLAN section 3.7, Appendix C):
  1. Resolves the placeholder tokens of the repo manifest (@GAME_ROOT@, @XEX@, @OUT_DIR@: see the top of
     mcla/codegen/mcla_manifest.toml) and writes the result to $WORK/generated/manifest/mcla_manifest.toml with
     verbatim copies of the included TOMLs next to it. Output directory: $WORK/generated/default.
  2. Runs `rexglue codegen <that manifest>` and tees everything to $WORK/logs/codegen-<ts>.log.
  3. Fails (exit 1) on rexglue's own non-zero exit, on any `UnresolvedCall` or `Validation failed` line, or when the
     output has no sources.cmake / <project>_register.cpp. Exit 2: setup problems (no SDK, no game, bad files).
  4. On success prints, as its last line,
         codegen ok: cpp=<n> functions=<n> unresolved=0 fingerprint=<md5>
     cpp          the *.cpp files in $WORK/generated/default
     functions    entries of the generated register table in <project>_register.cpp. Both forms are read: the
                  stock one SetFunction(addr, fn) call per function, and the one constant table
                  (kRexRegisterTable) of the owner's register-table patch.
     fingerprint  md5 over the files of $WORK/generated/default, sorted by relative name, of
                  (name, NUL, contents, NUL). Left out: codegen.d, codegen.stamp and codegen.build.stamp, which
                  hold absolute host paths and input hashes, not code, so the value compares across machines.
  The register addresses are also saved to $WORK/generated/registered-<baseline|full>.txt; a full run reports how
  its table differs from the last baseline's.

The generated code is the game, translated: it stays in $WORK, never in the repo.
"""
import argparse
import datetime
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "re"))
import game_config  # noqa: E402

CODEGEN_DIR = REPO / "mcla" / "codegen"
MANIFEST_SRC = CODEGEN_DIR / "mcla_manifest.toml"
MANIFEST_NAME = "mcla_manifest.toml"
BASELINE_INCLUDES = ["overrides.toml"]
TOKENS = ("@GAME_ROOT@", "@XEX@", "@OUT_DIR@")
# Bookkeeping files of rexglue's output directory: absolute paths and input hashes, not code.
FINGERPRINT_SKIP = frozenset({"codegen.d", "codegen.stamp", "codegen.build.stamp"})


class Fail(Exception):
    """A setup problem: bad or missing inputs, no SDK. Exit code 2 (a failed codegen exits 1)."""


def posix(path):
    return Path(path).resolve().as_posix()


def toml_string(value):
    if '"' in value or "\\" in value:
        raise Fail(f"path cannot go into a TOML basic string: {value}")
    return value


def resolve_includes(src_text):
    """The `includes` array of [entrypoint] as written in the repo manifest."""
    try:
        data = tomllib.loads(src_text)
    except tomllib.TOMLDecodeError as e:
        raise Fail(f"{MANIFEST_SRC}: invalid TOML: {e}")
    includes = data.get("entrypoint", {}).get("includes")
    if not isinstance(includes, list) or not includes:
        raise Fail(f"{MANIFEST_SRC}: [entrypoint].includes is missing")
    return includes, data["project"]["name"]


def write_manifest(manifest_dir, xex, game_root, out_dir, baseline):
    """Resolved manifest + verbatim include copies in manifest_dir. Returns (manifest path, includes, project)."""
    src_text = MANIFEST_SRC.read_text(encoding="utf-8")
    full_includes, project = resolve_includes(src_text)
    includes = list(BASELINE_INCLUDES) if baseline else list(full_includes)
    for name in includes:
        if not (CODEGEN_DIR / name).is_file():
            raise Fail(f"missing {CODEGEN_DIR / name}")

    try:
        out_value = os.path.relpath(out_dir, manifest_dir)
    except ValueError:                       # another drive: the SDK accepts an absolute path
        out_value = posix(out_dir)
    values = {"@GAME_ROOT@": posix(game_root), "@XEX@": posix(xex), "@OUT_DIR@": Path(out_value).as_posix()}

    counts = dict.fromkeys(TOKENS, 0)
    includes_line = re.compile(r"^includes\s*=\s*\[[^\]]*\]\s*$")
    new_lines = []
    n_includes = 0
    for line in src_text.splitlines():
        if not line.lstrip().startswith("#"):          # tokens in the header comments are documentation
            for token in TOKENS:
                if token in line:
                    counts[token] += line.count(token)
                    line = line.replace(token, toml_string(values[token]))
            if includes_line.match(line):
                n_includes += 1
                line = "includes = [" + ", ".join('"%s"' % n for n in includes) + "]"
        new_lines.append(line)
    bad = [t for t, c in counts.items() if c != 1]
    if bad or n_includes != 1:
        raise Fail(f"{MANIFEST_SRC} no longer has exactly one of each token {bad or ''} / includes line "
                   f"({n_includes}); fix the template or tools/codegen.py")
    text = "\n".join(new_lines) + "\n"
    leftovers = [ln for ln in new_lines if not ln.lstrip().startswith("#") and re.search(r"@[A-Z_]+@", ln)]
    if leftovers:
        raise Fail(f"unresolved placeholder in the manifest: {leftovers[0]}")
    try:
        resolved = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise Fail(f"resolved manifest is not valid TOML: {e}")
    entry = resolved["entrypoint"]
    if (resolved["project"]["game_root"] != values["@GAME_ROOT@"] or entry["file_path"] != values["@XEX@"]
            or entry["out_directory_path"] != values["@OUT_DIR@"] or entry["includes"] != includes):
        raise Fail("resolved manifest does not hold the values that were substituted")

    manifest_dir.mkdir(parents=True, exist_ok=True)
    for stale in manifest_dir.glob("*.toml"):         # leftovers of the other mode
        if stale.name != MANIFEST_NAME and stale.name not in includes:
            stale.unlink()
    for name in includes:
        shutil.copyfile(CODEGEN_DIR / name, manifest_dir / name)
    manifest = manifest_dir / MANIFEST_NAME
    manifest.write_text(text, encoding="utf-8", newline="\n")
    return manifest, includes, project


def find_xex(game, explicit):
    if explicit:
        xex = Path(explicit)
        if not xex.is_file():
            raise Fail(f"{xex} does not exist")
        return xex
    for cand in (game / "game" / "default.xex", game / "default.xex"):
        if cand.is_file():
            return cand
    raise Fail(f"no default.xex in {game / 'game'} or {game}: extract it first "
               f"(python tools/xiso.py extract ...), or pass --game / --xex / MCLA_GAME")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(out_dir):
    """md5 over (relative name, NUL, contents, NUL) of the generated files sorted by name."""
    files = sorted((p.relative_to(out_dir).as_posix(), p) for p in out_dir.rglob("*") if p.is_file())
    h = hashlib.md5()
    counted = 0
    for rel, path in files:
        if rel in FINGERPRINT_SKIP:
            continue
        h.update(rel.encode("utf-8") + b"\0")
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        h.update(b"\0")
        counted += 1
    return h.hexdigest(), counted


def run_rexglue(cmd, cwd, log_path, header):
    """Runs the CLI, echoing and logging every line. Returns (returncode, lines)."""
    env = dict(os.environ, NO_COLOR="1")
    lines = []
    with open(log_path, "w", encoding="utf-8", newline="\n") as log:
        for h in header:
            log.write("== " + h + "\n")
        log.flush()
        proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            for line in io.TextIOWrapper(proc.stdout, encoding="utf-8", errors="replace", newline=None):
                line = line.rstrip("\n")
                lines.append(line)
                log.write(line + "\n")
                print(line, flush=True)
            rc = proc.wait()
        except BaseException:
            proc.kill()
            raise
        log.write(f"== rexglue exit code {rc}\n")
    return rc, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", action="store_true", help="manifest with only overrides.toml (no harvest/huecos/hooks)")
    ap.add_argument("--work", metavar="W", help="$WORK (default: env MCLA_WORK, else %s)" % game_config.DEFAULT_WORK)
    ap.add_argument("--game", metavar="G", help="$GAME (default: env MCLA_GAME, else %s)" % game_config.DEFAULT_GAME)
    ap.add_argument("--rexglue", metavar="PATH", help="the rexglue CLI (default: $WORK/sdk/install/bin/rexglue.exe)")
    ap.add_argument("--xex", metavar="PATH", help="default.xex (default: $GAME/game/default.xex, else $GAME/default.xex)")
    ap.add_argument("--use-stamp", action="store_true", help="let rexglue skip an up-to-date run (default: always regenerate)")
    ap.add_argument("--no-verify", action="store_true", help="accept any default.xex")
    ap.add_argument("--manifest-only", action="store_true", help="write the resolved manifest and stop")
    args = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    mode = "baseline" if args.baseline else "full"
    work = game_config.work_root(args.work)
    game = game_config.game_dir(args.game)
    xex = find_xex(game, args.xex)
    if not args.no_verify:
        got = sha256_file(xex)
        if got != game_config.XEX_SHA256:
            raise Fail(f"FAIL xex sha256 expected={game_config.XEX_SHA256} got={got} ({xex}); "
                       f"only the Complete Edition build is supported (--no-verify to override)")
    generated = work / "generated"
    manifest_dir = generated / "manifest"
    out_dir = generated / "default"

    manifest, includes, project = write_manifest(manifest_dir, xex, xex.parent, out_dir, args.baseline)
    print(f"codegen: mode={mode} manifest={manifest} includes={','.join(includes)}")
    print(f"codegen: xex={xex} out={out_dir}")
    if args.manifest_only:
        print(f"manifest written: {manifest}")
        return 0

    rexglue = Path(args.rexglue) if args.rexglue else \
        work / "sdk" / "install" / "bin" / ("rexglue.exe" if os.name == "nt" else "rexglue")
    if not rexglue.is_file():
        raise Fail(f"rexglue CLI not found: {rexglue}\n  build the SDK first (python tools/prepare_sdk.py fetch apply "
                   f"build) or pass --rexglue PATH")

    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = logs / f"codegen-{ts}.log"
    cmd = [str(rexglue), "codegen", str(manifest)] + ([] if args.use_stamp else ["--ignore-stamp"])
    header = [f"tools/codegen.py {ts} mode={mode}", "cmd " + " ".join(cmd), f"cwd {manifest_dir}",
              f"includes {', '.join(includes)}"]
    print(f"codegen: log={log_path}")
    rc, lines = run_rexglue(cmd, manifest_dir, log_path, header)

    # ---- verdict ----------------------------------------------------------------------------------------
    bad = [ln for ln in lines if "UnresolvedCall" in ln or "Validation failed" in ln]
    unresolved = 0
    for ln in lines:
        m = re.search(r"UnresolvedCall \((\d+)\)", ln)
        if m:
            unresolved += int(m.group(1))
    if bad and not unresolved:
        unresolved = len([ln for ln in bad if "UnresolvedCall" in ln]) or 1
    problems = []
    if bad:
        problems.append(f"{len(bad)} UnresolvedCall/Validation failed line(s)")
    if rc != 0:
        problems.append(f"rexglue exit code {rc}")
    sources = out_dir / "sources.cmake"
    register = out_dir / f"{project}_register.cpp"
    if not sources.is_file():
        problems.append(f"missing {sources}")
    if not register.is_file():
        problems.append(f"missing {register}")
    if problems:
        for ln in bad[:10]:
            print("  " + ln.strip())
        print(f"codegen FAILED: {'; '.join(problems)} unresolved={unresolved} log={log_path}")
        return 1

    entries, form = game_config.parse_register_table(register.read_text(encoding="utf-8", errors="replace"))
    if not entries:
        print(f"codegen FAILED: no register table (SetFunction calls or kRexRegisterTable rows) in {register} "
              f"log={log_path}")
        return 1
    addrs = [a for a, _n in entries]
    unique = sorted(set(addrs))
    summary = next((ln for ln in lines if "Codegen summary:" in ln), None)
    if summary:
        print("codegen: " + summary[summary.index("Codegen summary:"):])
    print(f"codegen: register table form={form} entries={len(addrs)} unique={len(unique)}"
          + ("" if len(addrs) == len(unique) else "  (DUPLICATE ADDRESSES)"))
    saved = generated / f"registered-{mode}.txt"
    baseline_saved = generated / "registered-baseline.txt"
    if mode == "full" and baseline_saved.is_file():
        base = {int(x, 16) for x in baseline_saved.read_text(encoding="utf-8").split()}
        print(f"codegen: register table vs the last baseline ({len(base)} entries): "
              f"missing {len(base - set(unique))}, new {len(set(unique) - base)}")
    saved.write_text("".join("0x%08X\n" % a for a in unique), encoding="utf-8", newline="\n")

    cpp = len(list(out_dir.glob("*.cpp")))
    md5, counted = fingerprint(out_dir)
    print(f"codegen: fingerprint covers {counted} files of {out_dir}")
    print(f"codegen ok: cpp={cpp} functions={len(addrs)} unresolved=0 fingerprint={md5}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(f"error: {e}" if not str(e).startswith("FAIL") else str(e), file=sys.stderr)
        sys.exit(2)
