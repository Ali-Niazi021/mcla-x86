#!/usr/bin/env python3
"""setup.py - one command from a fresh clone to a playable build.

    python tools/setup.py --iso <your game .iso>          first time
    python tools/setup.py                                 later: rebuild after `git pull`
    python tools/setup.py --check                         only check the prerequisites and print the plan

Options
    --iso FILE     your own dump of the game disc (Midnight Club: Los Angeles - Complete Edition, Xbox 360)
    --game DIR     where the game files go, or already are (default: <work>/game)
    --work DIR     the work folder: compiler, SDK, generated code, build (default: env MCLA_WORK, else a folder
                   `mcla-x86-work` next to the repo). Keep the path short and without spaces.
    --jobs N       cap the compiler jobs of the SDK build (default: by free memory)
    --force        run every step again, even the ones that look done
    --check        check the prerequisites, print what would be done, and stop
    --no-nvidia    do not download NVIDIA Streamline (the game is built without DLSS)

Steps (each one is skipped when its result is already there, so the command is safe to run again):
    1 tools     Git, CMake 3.25+, Python 3.11+, Visual Studio 2022 C++ tools, Windows SDK, Ninja
    2 compiler  download Clang/LLVM 20.1.8 (940 MB, SHA-256 checked) and unpack it to <work>/toolchain/llvm-20.1.8
      nvidia    download NVIDIA Streamline 2.14.16 (276 MB, SHA-256 checked: DLSS Super Resolution, DLSS Frame Generation,
                Reflex) and unpack its headers and x64 DLLs to <work>/toolchain/nvidia/streamline-2.14.16; the SDK build
                is pointed at it and the DLLs end up next to mcla.exe. NVIDIA's files never enter the repo. --no-nvidia
                skips it (no DLSS)
    3 game      check the .iso is the supported dump and extract the game files (6.1 GiB)
    4 sdk       fetch the ReXGlue SDK, apply the patch series, build and install it (tools/prepare_sdk.py)
    5 codegen   recompile default.xex to C++ (tools/codegen.py)
    6 build     write CMakeUserPresets.json for this machine, configure and build the game and the launcher
    7 launcher  tell the launcher where the game files are (only if it has no settings yet)

Nothing needs administrator rights. Nothing of the game is written into the repo: it all goes to the work folder.
Exit codes: 0 ok, 1 a step failed, 2 a prerequisite is missing or an argument is wrong.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional

if sys.version_info < (3, 11):
    sys.exit("setup.py needs Python 3.11 or newer (this is %d.%d): winget install --id Python.Python.3.13 -e"
             % sys.version_info[:2])

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import prepare_sdk  # noqa: E402  (the Visual Studio / Ninja / Windows SDK lookups)

LLVM_VERSION = "20.1.8"
LLVM_FILE = "clang+llvm-%s-x86_64-pc-windows-msvc.tar.xz" % LLVM_VERSION
LLVM_URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-%s/%s" % (LLVM_VERSION, LLVM_FILE)
LLVM_SHA256 = "f229769f11d6a6edc8ada599c0cda964b7dee6ab1a08c6cf9dd7f513e85b107f"
LLVM_BYTES = 939541912

# NVIDIA Streamline (DLSS SR, DLSS-G, Reflex): the release zip from NVIDIA's GitHub, pinned. Only include/, bin/x64/ (the
# production DLLs and development/) and the licence files are unpacked. Personal use: the development DLLs (unsigned,
# NVIDIA's watermark) are the ones installed by default (REXGLUE_STREAMLINE_VARIANT, SDK patch 0770).
STREAMLINE_VERSION = "2.14.16"
STREAMLINE_FILE = "streamline-sdk-v%s.zip" % STREAMLINE_VERSION
STREAMLINE_URL = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v%s/%s" % (STREAMLINE_VERSION,
                                                                                         STREAMLINE_FILE)
STREAMLINE_SHA256 = "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b"
STREAMLINE_BYTES = 276016170
PRESET = "local-release"
PRESET_MARK = "mcla-x86/setup"
FREE_GB_WANTED = 30

WINGET = {
    "git": "winget install --id Git.Git -e",
    "cmake": "winget install --id Kitware.CMake -e",
    "vs": 'winget install --id Microsoft.VisualStudio.2022.Community -e --override "--passive --add '
          'Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.Windows11SDK.26100 '
          '--includeRecommended"',
}


def say(text: str = "") -> None:
    print(text, flush=True)


def step(n: int, title: str) -> None:
    say("\n[%d/7] %s" % (n, title))


class Fail(Exception):
    def __init__(self, text: str, code: int = 1):
        super().__init__(text)
        self.code = code


def posix(p: Path) -> str:
    return Path(p).as_posix()


def run(cmd: List[str], env: Dict[str, str], what: str) -> None:
    say("  $ " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    rc = subprocess.run(cmd, env=env, cwd=str(REPO)).returncode
    if rc != 0:
        raise Fail("%s failed (exit %d). Fix the problem above and run the same command again: finished steps are "
                   "skipped." % (what, rc))


def capture(cmd: List[str], env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, env=env, cwd=str(REPO), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


# ---------------------------------------------------------------------------------------------------------------
# 1 tools
# ---------------------------------------------------------------------------------------------------------------

def version_tuple(text: str) -> tuple:
    out = []
    for part in text.split("."):
        digits = "".join(ch for ch in part if ch.isdigit())
        if not digits:
            break
        out.append(int(digits))
    return tuple(out)


def check_tools(work: Path) -> Dict[str, str]:
    """Returns {'ninja': path}. Raises Fail(code 2) with one line per missing tool and how to install it."""
    missing: List[str] = []
    if os.name != "nt":
        raise Fail("this port builds on Windows 10/11 x64 only", 2)
    if not shutil.which("git"):
        missing.append("Git:  " + WINGET["git"])
    cmake = shutil.which("cmake")
    if not cmake:
        missing.append("CMake 3.25 or newer:  " + WINGET["cmake"])
    else:
        out = capture([cmake, "--version"]).stdout.split()
        found = version_tuple(out[2]) if len(out) > 2 else ()
        if found < (3, 25):
            missing.append("CMake 3.25 or newer (found %s):  %s" % (".".join(map(str, found)) or "?", WINGET["cmake"]))
        else:
            say("  cmake %s" % ".".join(map(str, found)))
    vs = prepare_sdk.vswhere_install_path()
    if vs is None:
        missing.append("Visual Studio 2022 (17.1 or newer) with 'Desktop development with C++':  " + WINGET["vs"])
    else:
        say("  Visual Studio C++ tools: %s" % posix(vs))
    sdk = prepare_sdk.newest_windows_sdk()
    if sdk is None:
        missing.append("a Windows 10/11 SDK (10.0.26100 is the tested one): add it in the Visual Studio Installer")
    else:
        say("  Windows SDK %s%s" % (sdk, "" if version_tuple(sdk) >= (10, 0, 26100) else
                                    "  (older than the tested 10.0.26100: if the build fails, add that SDK in the "
                                    "Visual Studio Installer)"))
    ninja = ""
    try:
        ninja = prepare_sdk.find_ninja(None, vs)
        say("  ninja: %s" % posix(Path(ninja)))
    except Exception:
        missing.append("Ninja (it comes with Visual Studio's 'C++ CMake tools'):  winget install --id Ninja-build.Ninja -e")
    if missing:
        raise Fail("Missing tools. Install them (each line ends with a command for a terminal), open a NEW terminal, "
                   "and run setup again:\n  - " + "\n  - ".join(missing), 2)

    if " " in str(work):
        raise Fail("the work folder path has a space in it (%s): pass --work with a path without spaces" % work, 2)
    if len(posix(work)) > 60:
        say("  warning: the work folder path is long (%d characters). If the SDK build fails on path length, use a "
            "short one, for example --work C:/mcla-work" % len(posix(work)))
    work.mkdir(parents=True, exist_ok=True)
    free_gb = shutil.disk_usage(work).free / 2 ** 30
    say("  work folder %s (%.0f GB free)" % (posix(work), free_gb))
    if free_gb < FREE_GB_WANTED and not (work / "sdk" / "install").is_dir():
        say("  warning: a first setup needs about %d GB free on that drive" % FREE_GB_WANTED)
    return {"ninja": ninja}


# ---------------------------------------------------------------------------------------------------------------
# 2 compiler
# ---------------------------------------------------------------------------------------------------------------

def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url: str, dest: Path, total: int) -> None:
    """Resumable download to dest.part, then rename."""
    part = dest.with_name(dest.name + ".part")
    have = part.stat().st_size if part.exists() else 0
    if have > total:
        part.unlink()
        have = 0
    if have < total:
        req = urllib.request.Request(url, headers={"User-Agent": "mcla-x86-setup", **({"Range": "bytes=%d-" % have} if have else {})})
        with urllib.request.urlopen(req, timeout=60) as resp:
            if have and resp.status != 206:  # the server sent the whole file again
                have = 0
            last = time.monotonic()
            with open(part, "ab" if have else "wb") as out:
                while True:
                    block = resp.read(1 << 20)
                    if not block:
                        break
                    out.write(block)
                    have += len(block)
                    if time.monotonic() - last > 5:
                        last = time.monotonic()
                        say("    %4.0f / %.0f MB" % (have / 2 ** 20, total / 2 ** 20))
    os.replace(part, dest)


def unpack_llvm(archive: Path, dest: Path) -> None:
    """Unpacks the release archive, without its top folder, to dest (through dest.tmp, so a cut-short run is redone)."""
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    count = 0
    last = time.monotonic()
    with tarfile.open(archive, "r:xz") as tar:
        for member in tar:
            parts = Path(member.name).parts[1:]  # drop 'clang+llvm-20.1.8-x86_64-pc-windows-msvc/'
            if not parts or any(p in ("..", "") for p in parts):
                continue
            target = tmp.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                src = tar.extractfile(member)
                with open(target, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
            else:
                continue  # links: the Windows archive has none that the build needs
            count += 1
            if time.monotonic() - last > 10:
                last = time.monotonic()
                say("    %d files" % count)
    if not (tmp / "bin" / "clang++.exe").exists():
        raise Fail("the unpacked archive has no bin/clang++.exe (%s)" % posix(tmp))
    os.replace(tmp, dest)


def ensure_compiler(work: Path, force: bool) -> None:
    llvm = work / "toolchain" / ("llvm-%s" % LLVM_VERSION)
    clang = llvm / "bin" / "clang++.exe"
    if clang.exists() and not force:
        say("  already there: %s" % posix(clang))
        return
    archive = work / "toolchain" / "dl" / LLVM_FILE
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists() or archive.stat().st_size != LLVM_BYTES:
        say("  downloading %s (940 MB)" % LLVM_URL)
        download(LLVM_URL, archive, LLVM_BYTES)
    say("  checking the SHA-256 of %s" % archive.name)
    got = sha256_of(archive)
    if got != LLVM_SHA256:
        archive.unlink()
        raise Fail("the downloaded compiler archive is damaged (SHA-256 %s, expected %s); it was deleted, run setup "
                   "again" % (got, LLVM_SHA256))
    say("  unpacking to %s (4 GB, a few minutes)" % posix(llvm))
    if llvm.exists():
        shutil.rmtree(llvm)
    unpack_llvm(archive, llvm)
    say("  compiler ready (you can delete %s to save 940 MB)" % posix(archive))


def ensure_streamline(work: Path, force: bool) -> None:
    """NVIDIA Streamline into <work>/toolchain/nvidia/streamline-<version> (include/, bin/x64/, licences)."""
    import zipfile
    dest = work / "toolchain" / "nvidia" / ("streamline-%s" % STREAMLINE_VERSION)
    if (dest / "include" / "sl.h").exists() and (dest / "bin" / "x64" / "sl.interposer.dll").exists() and not force:
        say("  already there: %s" % posix(dest))
        return
    archive = work / "toolchain" / "nvidia" / "dl" / STREAMLINE_FILE
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists() or archive.stat().st_size != STREAMLINE_BYTES:
        say("  downloading %s (276 MB)" % STREAMLINE_URL)
        download(STREAMLINE_URL, archive, STREAMLINE_BYTES)
    say("  checking the SHA-256 of %s" % archive.name)
    got = sha256_of(archive)
    if got != STREAMLINE_SHA256:
        archive.unlink()
        raise Fail("the downloaded Streamline archive is damaged (SHA-256 %s, expected %s); it was deleted, run setup "
                   "again" % (got, STREAMLINE_SHA256))
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    keep = ("include/", "bin/x64/", "license.txt", "3rd-party-licenses.md")
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            name = member.filename.replace("\\", "/")
            parts = Path(name).parts
            if member.is_dir() or not name.startswith(keep) or any(p in ("..", "") for p in parts):
                continue
            target = tmp.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(member) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
    if not (tmp / "include" / "sl.h").exists() or not (tmp / "bin" / "x64" / "development" / "sl.interposer.dll").exists():
        raise Fail("the unpacked Streamline archive lacks include/sl.h or bin/x64/development/sl.interposer.dll (%s)"
                   % posix(tmp))
    if dest.exists():
        shutil.rmtree(dest)
    os.replace(tmp, dest)
    say("  Streamline ready at %s (you can delete %s to save 276 MB)" % (posix(dest), posix(archive)))


# ---------------------------------------------------------------------------------------------------------------
# 3 game
# ---------------------------------------------------------------------------------------------------------------

def game_root_of(game: Path) -> Optional[Path]:
    """The folder that holds default.xex: <game>/game (the owner's layout) or <game>."""
    for d in (game / "game", game):
        if (d / "default.xex").is_file():
            return d
    return None


def ensure_game(iso: Optional[Path], game: Path, env: Dict[str, str], force: bool) -> Path:
    py = sys.executable
    xiso = str(HERE / "xiso.py")
    root = game_root_of(game)
    rpfs = sorted(root.glob("xarchive_*.rpf")) if root else []
    if root and len(rpfs) >= 4 and not force:
        p = capture([py, xiso, "verify-xex", str(root / "default.xex")], env)
        if p.returncode != 0:
            raise Fail("the game files in %s are not the supported build:\n%s" % (posix(root), p.stdout.strip()), 2)
        say("  already there: %s (default.xex is the supported build)" % posix(root))
        return root
    if iso is None:
        raise Fail("no game files in %s yet: pass --iso <your game .iso> (or --game <folder with default.xex and the "
                   "four xarchive_*.rpf files>)" % posix(game), 2)
    if not iso.is_file():
        raise Fail("no such file: %s" % iso, 2)
    p = capture([py, xiso, "verify", str(iso)], env)
    say("  " + p.stdout.strip().replace("\n", "\n  "))
    if p.returncode != 0:
        raise Fail("this .iso is not the supported dump (Midnight Club: Los Angeles - Complete Edition, USA/Europe, "
                   "media 5940C9DB). Other editions are different builds and do not work.", 2)
    game.mkdir(parents=True, exist_ok=True)
    run([py, xiso, "extract", str(iso), str(game)], env, "extracting the game files")
    root = game_root_of(game)
    if root is None:
        raise Fail("extraction finished but %s has no default.xex" % posix(game))
    return root


# ---------------------------------------------------------------------------------------------------------------
# 4 sdk, 5 codegen
# ---------------------------------------------------------------------------------------------------------------

def ensure_sdk(work: Path, env: Dict[str, str], jobs: Optional[int], force: bool) -> None:
    py = sys.executable
    tool = str(HERE / "prepare_sdk.py")
    if not force:
        p = capture([py, tool, "--work", str(work), "status"], env)
        if p.returncode == 0 and "built from this series" in p.stdout:
            say("  already built: " + p.stdout.strip().splitlines()[0])
            return
    run([py, tool, "--work", str(work), "fetch", "--no-reference"], env, "fetching the SDK")
    p = capture([py, tool, "--work", str(work), "status"], env)
    if p.returncode != 0 and "drifted" in p.stdout and ", 0 drifted" not in p.stdout:
        say("  the patch series changed since it was applied: resetting the SDK tree")
        run([py, tool, "--work", str(work), "reset"], env, "resetting the SDK tree")
    else:
        run([py, tool, "--work", str(work), "apply"], env, "applying the SDK patch series")
    say("  building the SDK (a first build takes 10 to 40 minutes; the log is in %s)" % posix(work / "logs"))
    build = [py, tool, "--work", str(work), "build", "--config", "Release"]
    if jobs:
        build += ["--jobs", str(jobs)]
    run(build, env, "building the SDK")


def run_codegen(work: Path, game: Path, env: Dict[str, str]) -> None:
    run([sys.executable, str(HERE / "codegen.py"), "--work", str(work), "--game", str(game)], env,
        "recompiling default.xex")


# ---------------------------------------------------------------------------------------------------------------
# 6 build, 7 launcher
# ---------------------------------------------------------------------------------------------------------------

def write_user_presets(work: Path, ninja: str) -> None:
    """CMakeUserPresets.json (gitignored): CMakePresets.json pins the owner's work folder and Ninja; this one is yours."""
    target = REPO / "CMakeUserPresets.json"
    w = posix(work)
    mine = {"name": PRESET, "displayName": "This machine, Release (written by tools/setup.py)",
            "inherits": "win-amd64-release", "binaryDir": w + "/build", "environment": {"MCLA_WORK": w},
            "cacheVariables": {"CMAKE_MAKE_PROGRAM": posix(Path(ninja))}}
    build = {"name": PRESET, "configurePreset": PRESET, "configuration": "Release"}
    doc = {"version": 6, "configurePresets": [], "buildPresets": []}
    if target.exists():
        try:
            doc = json.loads(target.read_text(encoding="utf-8"))
        except ValueError:
            raise Fail("%s is not valid JSON: fix or delete it, then run setup again" % posix(target), 2)
    doc.setdefault("vendor", {})[PRESET_MARK] = {"note": "the '%s' presets are rewritten by tools/setup.py" % PRESET}
    for key, entry in (("configurePresets", mine), ("buildPresets", build)):
        doc[key] = [p for p in doc.get(key, []) if p.get("name") != PRESET] + [entry]
    text = json.dumps(doc, indent=4) + "\n"
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        target.write_text(text, encoding="utf-8")
        say("  wrote %s (preset %s)" % (posix(target), PRESET))


def build_game(work: Path, ninja: str, env: Dict[str, str]) -> Path:
    write_user_presets(work, ninja)
    cmake = shutil.which("cmake")
    run([cmake, "--preset", PRESET], env, "configuring the game")
    run([cmake, "--build", "--preset", PRESET], env, "building the game")
    out = work / "build" / "bin" / "Release"
    for exe in ("mcla.exe", "mcla_launcher.exe"):
        if not (out / exe).exists():
            raise Fail("the build finished but %s is missing" % posix(out / exe))
    return out


def seed_launcher(game_root: Path) -> None:
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        return
    path = Path(base) / "mcla_x86" / "launcher.json"
    if path.exists():
        say("  the launcher already has settings (%s): left alone" % posix(path))
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"version": 1, "values": {"game_data_root": str(game_root)}}, indent=2) + "\n",
                    encoding="utf-8")
    say("  the launcher will use the game folder %s" % posix(game_root))


# ---------------------------------------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="One command from a fresh clone to a playable build.")
    ap.add_argument("--iso", help="your own dump of the game disc (.iso)")
    ap.add_argument("--game", help="where the game files go, or already are (default: <work>/game)")
    ap.add_argument("--work", help="the work folder (default: env MCLA_WORK, else mcla-x86-work next to the repo)")
    ap.add_argument("--jobs", type=int, help="cap the compiler jobs of the SDK build")
    ap.add_argument("--force", action="store_true", help="run every step again")
    ap.add_argument("--check", action="store_true", help="check the prerequisites, print the plan, and stop")
    ap.add_argument("--no-nvidia", action="store_true", help="do not download NVIDIA Streamline (no DLSS)")
    args = ap.parse_args()

    work = Path(args.work or os.environ.get("MCLA_WORK") or (REPO.parent / "mcla-x86-work")).resolve()
    game = Path(args.game or os.environ.get("MCLA_GAME") or (work / "game")).resolve()
    iso = Path(args.iso).resolve() if args.iso else None
    env = dict(os.environ)
    env["MCLA_WORK"] = posix(work)
    env["MCLA_GAME"] = posix(game)
    env.setdefault("MCLA_LOCK_DIR", "work")  # the build lock lives in this work folder (tools/heavy_lock.py)
    started = time.monotonic()
    try:
        step(1, "tools")
        tools = check_tools(work)
        if args.check:
            say("\nwork folder  %s\ngame folder  %s\niso          %s" % (posix(work), posix(game), posix(iso) if iso else "(none given)"))
            say("compiler     %s" % ("ready" if (work / "toolchain" / ("llvm-%s" % LLVM_VERSION) / "bin" / "clang++.exe").exists()
                                     else "will be downloaded (940 MB)"))
            say("game files   %s" % ("ready" if game_root_of(game) else "will be extracted from the .iso"))
            say("nvidia       %s" % ("skipped (--no-nvidia): no DLSS" if args.no_nvidia else
                                   ("ready" if (work / "toolchain" / "nvidia" / ("streamline-%s" % STREAMLINE_VERSION) / "include" / "sl.h").exists()
                                    else "will be downloaded (276 MB, DLSS Super Resolution / Frame Generation / Reflex)")))
            say("check ok: run the same command without --check")
            return 0
        step(2, "compiler (Clang/LLVM %s)" % LLVM_VERSION)
        ensure_compiler(work, args.force)
        if not args.no_nvidia:
            say("  NVIDIA Streamline %s (DLSS, Reflex)" % STREAMLINE_VERSION)
            ensure_streamline(work, args.force)
        step(3, "game files")
        game_root = ensure_game(iso, game, env, args.force)
        step(4, "ReXGlue SDK (fetch, patch, build)")
        ensure_sdk(work, env, args.jobs, args.force)
        step(5, "recompile default.xex to C++")
        run_codegen(work, game, env)
        step(6, "build the game and the launcher")
        out = build_game(work, tools["ninja"], env)
        step(7, "launcher settings")
        seed_launcher(game_root)
    except Fail as e:
        say("\nSETUP STOPPED: %s" % e)
        return e.code
    except KeyboardInterrupt:
        say("\ninterrupted: run the same command again, finished steps are skipped")
        return 130
    took = time.monotonic() - started
    say("\nDone in %s. Start the game with:\n\n    %s\n"
        % ("%.0f min" % (took / 60) if took >= 90 else "%.0f s" % took, out / "mcla_launcher.exe"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
