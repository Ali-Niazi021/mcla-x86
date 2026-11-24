#!/usr/bin/env python3
"""Loads the codegen manifest plus its includes and reports keys that appear in more than one file
(functions by numeric address, root keys, other tables, [[midasm_hook]] by address) and conflicting values.

    python tools/re/check_codegen_toml.py [--game mcla] [manifest.toml]

Static: reads only the repo's TOMLs (mcla/codegen/), needs neither the SDK nor the game. Default manifest:
mcla/codegen/mcla_manifest.toml (the placeholder tokens in it are plain strings here).

Prints, last line:
    <n> entries, 0 duplicates                 exit 0
    FAIL duplicates=<k> (entries <n>)         exit 1   (each one listed as a `DUP ...` line before)
Exit 2: a file is missing or is not valid TOML.

Ported from the owner's iOS port (tools/carbon/check_codegen_toml.py).
"""
import argparse
import pathlib
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from game_config import add_game_argument, get_game  # noqa: E402


def norm(key):
    try:
        return int(key, 0)
    except (TypeError, ValueError):
        return key


def load(path):
    with open(path, "rb") as fh:  # tomllib already rejects duplicates inside one file
        return tomllib.load(fh)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_game_argument(ap)
    ap.add_argument("manifest", nargs="?", help="manifest TOML (default: the game's)")
    args = ap.parse_args()
    game = get_game(args.game, args.work)
    manifest = pathlib.Path(args.manifest or game.codegen_dir / game.manifest_name).resolve()
    if not manifest.exists():
        print(f"error: manifest not found: {manifest}", file=sys.stderr)
        return 2
    try:
        top = load(manifest)
        includes = top.get("entrypoint", {}).get("includes", [])
        files = [manifest] + [manifest.parent / i for i in includes]
        missing = [f for f in files if not f.exists()]
        if missing:
            for f in missing:
                print(f"error: included file not found: {f}", file=sys.stderr)
            return 2
        datas = [(f, top if f == manifest else load(f)) for f in files]
    except tomllib.TOMLDecodeError as e:
        print(f"error: invalid TOML: {e}", file=sys.stderr)
        return 2

    seen = {}  # (table, key) -> (file, value)
    problems = 0
    for f, data in datas:
        n = 0
        for table, val in data.items():
            if f == manifest and table in ("project", "entrypoint"):
                continue
            if isinstance(val, dict):
                for k, v in val.items():
                    n += 1
                    ident = (table, norm(k))
                    if ident in seen:
                        problems += 1
                        same = "same value" if seen[ident][1] == v else "CONFLICT"
                        print(f"DUP [{table}] {k}: {seen[ident][0]} vs {f.name} ({same}: {seen[ident][1]} / {v})")
                    else:
                        seen[ident] = (f.name, v)
            elif isinstance(val, list) and table == "midasm_hook":  # SDK keys these by address (config.cpp)
                for entry in val:
                    n += 1
                    if "address" not in entry:
                        problems += 1
                        print(f"BAD [[midasm_hook]] without address in {f.name}: {entry}")
                        continue
                    ident = (table, norm(entry["address"]))
                    if ident in seen:
                        problems += 1
                        print(f"DUP [{table}] 0x{entry['address']:08X}: {seen[ident][0]} vs {f.name}")
                    else:
                        seen[ident] = (f.name, entry)
            else:
                n += 1
                ident = ("<root>", table)
                if ident in seen:
                    problems += 1
                    print(f"DUP root key {table}: {seen[ident][0]} vs {f.name}")
                else:
                    seen[ident] = (f.name, val)
        print(f"{f.name}: parsed, {n} entries")
    if problems:
        print(f"FAIL duplicates={problems} (entries {len(seen)})")
        return 1
    print(f"{len(seen)} entries, 0 duplicates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
