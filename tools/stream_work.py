#!/usr/bin/env python3
"""stream_work.py - make a private $WORK for one work stream, so parallel streams never share an SDK tree or a build.

    python tools/stream_work.py <name> [--sdk own|shared] [--base W] [--root DIR]

Creates <root>/mcla-x86-work-<name> (root default: the folder of the base work tree) with:

    toolchain, fixtures, re, prior     junctions to the base work tree (read-only use)
    generated                          a copy (125 MB): the stream may re-run codegen
    user                               a copy (shader / pipeline storage; runs write to it)
    sdk                                --sdk own:    a copy of sdk/src without its build directory (out/) and of
                                                     sdk/install; the stream patches and builds its own SDK
                                                     (the first `prepare_sdk.py build` is a full build)
                                       --sdk shared: a junction to the base sdk (read-only: never build or reset it)
    build, runs, logs, scratch         empty; the stream's own

Then use it with the environment variable MCLA_WORK=<that folder> (every Python tool reads it). The CMake presets do
NOT: CMakePresets.json pins environment.MCLA_WORK to the main work tree, so a stream build needs a CMakeUserPresets.json
(gitignored) next to it, written by this script when it is run from the stream's checkout: preset `stream-release`
(configure and build) inherits win-amd64-release with this work tree. The heavy lock stays machine-global
(tools/heavy_lock.py lock_dir), so streams queue for the CPU and the GPU.
Safe to re-run: existing parts are left alone. Prints the folder as the last line.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_WORK = "C:/Users/Ali/Development/mcla-x86-work"


def junction(link: Path, target: Path) -> None:
    if link.exists():
        return
    if not target.exists():
        print("  skip %s (no %s)" % (link.name, target))
        return
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link).replace("/", "\\"), str(target).replace("/", "\\")],
                   check=True, stdout=subprocess.DEVNULL)
    print("  junction %s -> %s" % (link.name, target))


def copy_tree(src: Path, dst: Path, exclude_dirs: tuple = ()) -> None:
    if dst.exists():
        return
    if not src.exists():
        print("  skip %s (no %s)" % (dst.name, src))
        return
    cmd = ["robocopy", str(src).replace("/", "\\"), str(dst).replace("/", "\\"), "/E", "/MT:8", "/NFL", "/NDL", "/NJH",
           "/NJS", "/NP", "/R:2", "/W:1", "/SL"]
    for d in exclude_dirs:
        cmd += ["/XD", str(src / d).replace("/", "\\")]
    rc = subprocess.run(cmd).returncode
    if rc >= 8:
        raise SystemExit("robocopy %s -> %s failed (%d)" % (src, dst, rc))
    print("  copy %s" % dst.name)


def write_user_presets(work: Path) -> None:
    """CMakeUserPresets.json in the current checkout (if it is one and has none): the build goes to this work tree."""
    here = Path.cwd()
    target = here / "CMakeUserPresets.json"
    if not (here / "CMakePresets.json").is_file() or target.exists():
        return
    w = work.as_posix()
    presets = {
        "version": 6,
        "configurePresets": [{"name": "stream-release", "inherits": "win-amd64-release", "binaryDir": w + "/build",
                              "environment": {"MCLA_WORK": w}}],
        "buildPresets": [{"name": "stream-release", "configurePreset": "stream-release", "configuration": "Release"}],
    }
    target.write_text(json.dumps(presets, indent=4) + "\n", encoding="utf-8")
    print("  wrote %s (cmake --preset stream-release; cmake --build --preset stream-release)" % target.as_posix())


def main() -> int:
    ap = argparse.ArgumentParser(description="Make a private $WORK for one work stream.")
    ap.add_argument("name")
    ap.add_argument("--sdk", choices=("own", "shared"), default="own")
    ap.add_argument("--base", default=DEFAULT_WORK)
    ap.add_argument("--root")
    args = ap.parse_args()
    base = Path(args.base)
    root = Path(args.root) if args.root else base.parent
    work = root / ("mcla-x86-work-" + args.name)
    work.mkdir(parents=True, exist_ok=True)
    print("stream work tree %s" % work.as_posix())
    for name in ("toolchain", "fixtures", "re", "prior"):
        junction(work / name, base / name)
    copy_tree(base / "generated", work / "generated")
    copy_tree(base / "user", work / "user")
    if args.sdk == "shared":
        junction(work / "sdk", base / "sdk")
    else:
        (work / "sdk").mkdir(exist_ok=True)
        copy_tree(base / "sdk" / "src", work / "sdk" / "src", exclude_dirs=("out",))
        copy_tree(base / "sdk" / "install", work / "sdk" / "install")
    for name in ("build", "runs", "logs", "scratch"):
        (work / name).mkdir(exist_ok=True)
    write_user_presets(work)
    print(work.as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
