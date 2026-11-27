#!/usr/bin/env python3
"""check_launcher_schema.py - the launcher's settings schema against the code it drives.

    python tools/check_launcher_schema.py [--sdk-src PATH] [--schema PATH] [--work DIR] [-q]

Reads launcher/schema/settings.json and checks that

  * the schema obeys the rules of launcher/src/core/settings_schema.h (ids, categories, types, min/max, enum options,
    defaults, `requires` targets and cycles, shared cvars, presets);
  * every cvar a setting names is defined by a REXCVAR_DEFINE_* in mcla/**, src/** (this repo) or the patched SDK source
    (<sdk-src>/src and <sdk-src>/include), or else by an SDK patch in patches/sdk/*.patch that the SDK tree does not have
    yet (reported as PATCH-ONLY with a warning: it needs the series applied and the SDK rebuilt);
  * the cvar's type matches the setting's type, the schema range lies inside the cvar's `.range(lo, hi)`, every enum
    option is in the cvar's `.allowed({...})`, and an `emit: nondefault` setting has the game's effective default (the
    cvar's own default, or the value mcla/profile/pc_profile.cpp's kProfileDefaults sets for it);
  * the mcla_profile, mcla_fps_target and mcla_gfx_preset options are values the game's code handles
    (pc_profile.cpp kPresets / kFpsTargets, mcla_gfx_tune.cpp), and the launcher's two hard-coded lookups exist
    (one `path` setting for game_data_root, a launcher-only bool close_on_launch).

--sdk-src PATH   the SDK source root, the folder that holds src/ and include/. Default $WORK/sdk/src; use
                 $WORK/scratch/ORC-series/sdk/src for the tree the series was applied to in the scratch.
--schema PATH    default launcher/schema/settings.json.
--work DIR       $WORK (default env MCLA_WORK, else the folder mcla-x86-work next to the repo).
-q               do not print the INFO lines (settings whose emit is not nondefault).

Output: a summary, INFO / WARN / ERROR lines, and as the last line `RESULT OK (<e> errors, <w> warnings)` or
`RESULT FAIL (...)`. Exit 0 iff there is no ERROR, 1 otherwise, 2 for a setup problem (no schema, no SDK tree).
The tool only reads: it creates no file anywhere.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional

REPO = Path(__file__).resolve().parents[1]
_LEGACY_WORK = "C:/Users/Ali/Development/mcla-x86-work"
DEFAULT_WORK = _LEGACY_WORK if os.path.isdir(_LEGACY_WORK) else (REPO.parent / "mcla-x86-work").as_posix()
SOURCE_SUFFIXES = (".cpp", ".h", ".cc", ".hpp", ".inc")

# ================================================================================================================
# REXCVAR_DEFINE_* extraction
# ================================================================================================================

_DEFINE = re.compile(r"REXCVAR_DEFINE_(\w+)\s*\(")


def _split_args(s: str, i: int):
    """The top-level arguments of the call whose '(' ends just before s[i]. Returns (args, index of the closing ')')."""
    depth, args, cur, in_str, esc = 1, [], [], False, False
    j = i
    while j < len(s):
        c = s[j]
        if in_str:
            cur.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c == "(":
            depth += 1
            cur.append(c)
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return args, j
            cur.append(c)
        elif c == "," and depth == 1:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        j += 1
    return args, j


def _unquote(arg: str) -> str:
    """The text of a (possibly concatenated) C string literal argument."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', arg))


def extract_cvars(text: str, file: str, defs: Dict[str, List[dict]]) -> None:
    """Add every REXCVAR_DEFINE_<TYPE>(name, default, category, description)<chained calls>; of `text` to defs."""
    if "REXCVAR_DEFINE_" not in text:
        return
    for m in _DEFINE.finditer(text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        if "#define" in text[line_start:m.start()]:
            continue  # the macro's own definition
        args, end = _split_args(text, m.end())
        if len(args) < 4:
            continue  # REXCVAR_DEFINE_COMMAND and friends
        semi = text.find(";", end)
        tail = " ".join(text[end + 1:semi].split()) if semi > 0 else ""
        defs.setdefault(args[0].strip(), []).append({
            "type": m.group(1), "default": args[1].strip(), "category": _unquote(args[2]),
            "description": _unquote(args[3]), "tail": tail, "file": file,
            "line": text.count("\n", 0, m.start()) + 1,
        })


def scan_sources(roots: List[Path]) -> Dict[str, List[dict]]:
    defs: Dict[str, List[dict]] = {}
    for root in roots:
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if p.suffix in SOURCE_SUFFIXES and p.is_file():
                extract_cvars(p.read_text(encoding="utf-8", errors="replace"), p.as_posix(), defs)
    return defs


def patch_new_side(text: str) -> str:
    """The new-file view of a unified diff: added and context lines without their first character, every other line
    (headers, hunk markers, removed lines) blank, so line numbers stay those of the patch file."""
    out = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if line.startswith("+") and not line.startswith("+++"):
            out.append(line[1:])
        elif line.startswith(" "):
            out.append(line[1:])
        else:
            out.append("")
    return "\n".join(out)


def scan_patches(patch_dir: Path) -> Dict[str, List[dict]]:
    defs: Dict[str, List[dict]] = {}
    if patch_dir.is_dir():
        for p in sorted(patch_dir.glob("*.patch")):
            extract_cvars(patch_new_side(p.read_text(encoding="utf-8", errors="replace")), p.as_posix(), defs)
    return defs


# ================================================================================================================
# Reading C++ literals
# ================================================================================================================

def literal(code_default: str, typ: str):
    """The value of a cvar default written as a literal, else None (a constant or an expression)."""
    d = code_default.strip()
    if typ == "BOOL":
        return (d == "true") if d in ("true", "false") else None
    if typ == "STRING":
        m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', d)
        return m.group(1) if m else None
    d = d.replace("'", "")  # C++ digit separators
    if re.fullmatch(r"-?0[xX][0-9a-fA-F]+[uUlL]*", d):
        return float(int(re.sub(r"[uUlL]+$", "", d), 16))
    m = re.fullmatch(r"(-?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?|-?\.\d+)[fFuUlL]*", d)
    return float(m.group(1)) if m else None


# ================================================================================================================
# The check
# ================================================================================================================

SETTING_TYPES = ("bool", "int", "float", "enum", "string", "path")
# the cvar kinds (REXCVAR_DEFINE_<KIND>) a setting type may drive
CVAR_KINDS = {"bool": {"BOOL"}, "int": {"INT32", "UINT32", "INT64", "UINT64"}, "float": {"DOUBLE"},
              "string": {"STRING"}, "path": {"STRING"},
              "enum": {"STRING", "INT32", "UINT32", "INT64", "UINT64", "DOUBLE", "BOOL"}}
_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
_CVAR = re.compile(r"^[A-Za-z0-9_]+$")


class Report:
    def __init__(self) -> None:
        self.errors: List[str] = []
        self.warns: List[str] = []
        self.infos: List[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warns.append(msg)

    def info(self, msg: str) -> None:
        self.infos.append(msg)


def is_valid(setting: dict, value) -> bool:
    """settings_schema.h's IsValidValue: does `value` have the setting's type and lie in its range / options."""
    t = setting.get("type")
    if t == "bool":
        return isinstance(value, bool)
    if t == "int":
        return (isinstance(value, int) and not isinstance(value, bool)
                and setting.get("min", float("-inf")) <= value <= setting.get("max", float("inf")))
    if t == "float":
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
                and setting.get("min", float("-inf")) <= value <= setting.get("max", float("inf")))
    if t == "enum":
        return any(o.get("value") == value and type(o.get("value")) == type(value) for o in setting.get("options", []))
    return isinstance(value, str)


def check_structure(schema: dict, rep: Report) -> Dict[str, dict]:
    """The validator's rules (launcher/src/core/settings_schema.h). Returns the settings by id."""
    for key in ("categories", "settings", "presets"):
        if not isinstance(schema.get(key), list):
            rep.err("schema has no `%s` list" % key)
            return {}
    cats = {c.get("id") for c in schema["categories"]}
    if len(cats) != len(schema["categories"]):
        rep.err("duplicate category ids")
    by_id: Dict[str, dict] = {}
    for s in schema["settings"]:
        i = s.get("id", "<no id>")
        if not _ID.match(str(i)):
            rep.err("bad id %s" % i)
        if i in by_id:
            rep.err("duplicate setting id %s" % i)
        by_id[i] = s
        if s.get("category") not in cats:
            rep.err("%s: unknown category %s" % (i, s.get("category")))
        if s.get("cvar") is not None and not _CVAR.match(str(s["cvar"])):
            rep.err("%s: bad cvar" % i)
        for k in ("label", "help"):
            if not s.get(k):
                rep.err("%s: empty %s" % (i, k))
        if len(s.get("help", "")) > 400:
            rep.warn("%s: help is %d chars" % (i, len(s["help"])))
        t = s.get("type")
        if t not in SETTING_TYPES:
            rep.err("%s: bad type %s" % (i, t))
            continue
        if t in ("int", "float") and ("min" not in s or "max" not in s):
            rep.err("%s: int/float needs min and max" % i)
        if t == "enum":
            vals = [json.dumps(o.get("value")) for o in s.get("options", [])]
            if not vals or len(set(vals)) != len(vals):
                rep.err("%s: enum options empty or duplicate values" % i)
            for o in s.get("options", []):
                if not o.get("label"):
                    rep.err("%s: option %r has no label" % (i, o.get("value")))
        if "default" not in s:
            rep.err("%s: no default" % i)
            continue
        if t == "int" and (not isinstance(s["default"], int) or isinstance(s["default"], bool)):
            rep.err("%s: int default must be an integer literal" % i)
        if not is_valid(s, s["default"]):
            rep.err("%s: default %r invalid" % (i, s["default"]))
        if s.get("emit") not in ("nondefault", "always", "never"):
            rep.err("%s: bad emit" % i)
        if s.get("cvar") is None and s.get("emit") != "never":
            rep.err("%s: launcher-only should be emit never" % i)
    for i, s in by_id.items():
        for rid, rv in s.get("requires", {}).items():
            if rid not in by_id:
                rep.err("%s: requires unknown %s" % (i, rid))
                continue
            # a list is "any of these values" (launcher/src/core/settings_schema.h)
            if isinstance(rv, list) and not rv:
                rep.err("%s: requires %s: the list of values is empty" % (i, rid))
            for one in (rv if isinstance(rv, list) else [rv]):
                if not is_valid(by_id[rid], one):
                    rep.err("%s: requires %s=%r invalid" % (i, rid, one))

    def cyclic(i: str, seen: frozenset) -> bool:
        if i in seen:
            return True
        return any(cyclic(r, seen | {i}) for r in by_id[i].get("requires", {}) if r in by_id)

    for i in by_id:
        if cyclic(i, frozenset()):
            rep.err("requires cycle through %s" % i)
    seen: Dict[str, dict] = {}
    for s in schema["settings"]:
        c = s.get("cvar")
        if c:
            if c in seen and not (s.get("requires") and seen[c].get("requires")):
                rep.err("cvar %s shared by unconditional settings" % c)
            seen[c] = s
    pids = set()
    for p in schema["presets"]:
        if p.get("id") in pids:
            rep.err("duplicate preset %s" % p.get("id"))
        pids.add(p.get("id"))
        for k, v in p.get("set", {}).items():
            if k not in by_id:
                rep.err("preset %s: unknown setting %s" % (p.get("id"), k))
            elif not is_valid(by_id[k], v):
                rep.err("preset %s: %s=%r invalid" % (p.get("id"), k, v))
    return by_id


def profile_defaults(profile_cpp: str) -> Dict[str, str]:
    """The cvar values the game applies at startup (kProfileDefaults in mcla/profile/pc_profile.cpp)."""
    start = profile_cpp.find("constexpr CvarDefault kProfileDefaults[]")
    if start < 0:
        return {}
    block = profile_cpp[start:]
    block = block[:block.find("\n};")]
    return dict(re.findall(r'^\s*\{"([A-Za-z0-9_]+)",\s*"([^"]*)"\}', block, re.M))


def check_cvars(by_id: Dict[str, dict], repo: dict, sdk: dict, patches: dict, prof_defaults: Dict[str, str],
                rep: Report) -> Dict[str, tuple]:
    """Existence, type, range, allowed values and default of every cvar a setting drives. Returns cvar -> (where, site)."""
    where: Dict[str, tuple] = {}
    for s in by_id.values():
        cv = s.get("cvar")
        if not cv or s.get("type") not in SETTING_TYPES:
            continue
        src, defs = None, None
        if cv in repo:
            src, defs = "repo", repo[cv]
        elif cv in sdk:
            src, defs = "sdk", sdk[cv]
        elif cv in patches:
            src, defs = "PATCH-ONLY", patches[cv]
            rep.warn("%s: defined only in patches/sdk (not in the SDK tree yet: needs the series applied and the SDK "
                     "rebuilt)" % cv)
        if defs is None:
            rep.err("%s: cvar %s not found in the repo, the SDK source or patches/sdk" % (s["id"], cv))
            continue
        d = defs[0]
        where[cv] = (src, d["file"] + ":" + str(d["line"]))
        if d["type"] not in CVAR_KINDS[s["type"]]:
            rep.err("%s: schema type %s vs code %s" % (cv, s["type"], d["type"]))
        tail = d["tail"]
        m = re.search(r"\.range\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)", tail)
        if m and s["type"] in ("int", "float") and "min" in s and "max" in s:
            lo, hi = float(m.group(1)), float(m.group(2))
            if s["min"] < lo or s["max"] > hi:
                rep.err("%s: schema range %s..%s outside code range %s..%s" % (cv, s["min"], s["max"], lo, hi))
        m = re.search(r"\.allowed\(\{(.*?)\}\)", tail)
        if m and s["type"] == "enum":
            allowed = set(re.findall(r'"([^"]*)"', m.group(1)))
            for o in s.get("options", []):
                if str(o.get("value")) not in allowed:
                    rep.err("%s: option %r not in code .allowed %s" % (cv, o.get("value"), sorted(allowed)))
        code_default = literal(d["default"], d["type"])
        raw_profile = prof_defaults.get(cv)  # the game starts with the profile's value, not the cvar's own
        if raw_profile is None:
            eff = code_default
        else:
            eff = raw_profile if d["type"] == "STRING" else literal(raw_profile, d["type"])
        sd = s.get("default")
        if eff is None:
            rep.warn("%s: code default %r is not a literal; not compared" % (cv, d["default"]))
        elif s.get("emit") == "nondefault":
            if isinstance(sd, str) or isinstance(eff, str):
                same = str(eff).lower() == str(sd).lower()
            else:
                same = float(eff) == float(sd)
            if not same:
                rep.err("%s: emit=nondefault but schema default %r != code%s default %r"
                        % (cv, sd, "/profile" if raw_profile is not None else "", eff))
        else:
            rep.info("%s: emit=%s, schema default %r (code default %r%s)"
                     % (cv, s.get("emit"), sd, code_default,
                        ", profile sets %r" % raw_profile if raw_profile is not None else ""))
    return where


def check_game_lookups(schema: dict, by_id: Dict[str, dict], repo_root: Path, rep: Report) -> None:
    """Option values the game's code must handle, and the launcher's two hard-coded lookups."""
    prof_path = repo_root / "mcla/profile/pc_profile.cpp"
    tune_path = repo_root / "mcla/hooks/mcla_gfx_tune.cpp"
    prof = prof_path.read_text(encoding="utf-8") if prof_path.is_file() else ""
    tune = tune_path.read_text(encoding="utf-8") if tune_path.is_file() else ""

    def options(setting_id: str) -> List[dict]:
        if setting_id not in by_id:
            rep.err("schema has no %s setting" % setting_id)
            return []
        return by_id[setting_id].get("options", [])

    a = prof.find("constexpr Preset kPresets[]")
    if a < 0:
        rep.err("pc_profile.cpp: no `constexpr Preset kPresets[]` (the mcla_profile options cannot be checked)")
    else:
        block = prof[a:].split("\n};")[0]
        names = set(re.findall(r'^\s*\{"([0-9a-z\-]+)",', block, re.M)) | {"default"}
        for o in options("mcla_profile"):
            if o.get("value") not in names:
                rep.err("mcla_profile option %s is not a profile preset (known: %s)" % (o.get("value"), sorted(names)))
    a = prof.find("constexpr FpsTarget kFpsTargets[]")
    if a < 0:
        rep.err("pc_profile.cpp: no `constexpr FpsTarget kFpsTargets[]` (the mcla_fps_target options cannot be checked)")
    else:
        block = prof[a:].split("\n};")[0]
        targets = {int(x) for x in re.findall(r"^\s*\{(\d+),", block, re.M)} | {0}
        for o in options("mcla_fps_target"):
            if o.get("value") not in targets:
                rep.err("mcla_fps_target option %s is not a target (known %s)" % (o.get("value"), sorted(targets)))
    if not tune:
        rep.err("mcla/hooks/mcla_gfx_tune.cpp not found (the mcla_gfx_preset options cannot be checked)")
    for o in options("mcla_gfx_preset"):
        if o.get("value") and tune and '"%s"' % o["value"] not in tune:
            rep.err("mcla_gfx_preset option %s not handled in mcla_gfx_tune.cpp" % o["value"])
    gd = [s for s in schema.get("settings", []) if s.get("cvar") == "game_data_root"]
    if len(gd) != 1 or gd[0].get("type") != "path":
        rep.err("need exactly one path setting with cvar game_data_root")
    co = by_id.get("close_on_launch")
    if not co or co.get("cvar") is not None or co.get("type") != "bool":
        rep.err("close_on_launch must be a launcher-only bool")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="check_launcher_schema.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--sdk-src", metavar="PATH", help="SDK source root (holds src/ and include/); default $WORK/sdk/src")
    ap.add_argument("--schema", metavar="PATH", help="default launcher/schema/settings.json")
    ap.add_argument("--work", metavar="DIR", help="$WORK (default env MCLA_WORK, else %s)" % DEFAULT_WORK)
    ap.add_argument("-q", "--quiet", action="store_true", help="do not print the INFO lines")
    args = ap.parse_args(argv)

    work = Path(args.work or os.environ.get("MCLA_WORK") or DEFAULT_WORK)
    sdk_src = Path(args.sdk_src) if args.sdk_src else work / "sdk" / "src"
    schema_path = Path(args.schema) if args.schema else REPO / "launcher" / "schema" / "settings.json"
    if not schema_path.is_file():
        print("check_launcher_schema: no schema at %s" % schema_path.as_posix(), file=sys.stderr)
        return 2
    sdk_roots = [sdk_src / "src", sdk_src / "include"]
    if not any(r.is_dir() for r in sdk_roots):
        print("check_launcher_schema: no SDK source at %s (it must hold src/ and include/; pass --sdk-src)"
              % sdk_src.as_posix(), file=sys.stderr)
        return 2
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except ValueError as e:
        print("ERROR: %s is not valid JSON: %s" % (schema_path.as_posix(), e))
        print("RESULT FAIL (1 errors, 0 warnings)")
        return 1

    repo = scan_sources([REPO / "mcla", REPO / "src"])
    sdk = scan_sources(sdk_roots)
    patches = scan_patches(REPO / "patches" / "sdk")

    rep = Report()
    by_id = check_structure(schema, rep)
    prof_cpp = REPO / "mcla" / "profile" / "pc_profile.cpp"
    prof_defaults = profile_defaults(prof_cpp.read_text(encoding="utf-8")) if prof_cpp.is_file() else {}
    where = check_cvars(by_id, repo, sdk, patches, prof_defaults, rep)
    check_game_lookups(schema, by_id, REPO, rep)

    n_settings = len(schema.get("settings", []))
    print("%d settings, %d presets, %d categories; cvars checked: %d (SDK source %s)"
          % (n_settings, len(schema.get("presets", [])), len(schema.get("categories", [])), len(where),
             sdk_src.as_posix()))
    for k in ("repo", "sdk", "PATCH-ONLY"):
        print("  %s: %d" % (k, sum(1 for v in where.values() if v[0] == k)))
    if not args.quiet:
        for m in rep.infos:
            print("INFO", m)
    for m in rep.warns:
        print("WARN", m)
    for m in rep.errors:
        print("ERROR", m)
    print("RESULT %s (%d errors, %d warnings)" % ("FAIL" if rep.errors else "OK", len(rep.errors), len(rep.warns)))
    return 1 if rep.errors else 0


if __name__ == "__main__":
    sys.exit(main())
