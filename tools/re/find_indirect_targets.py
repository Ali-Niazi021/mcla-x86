#!/usr/bin/env python3
"""Find code addresses the game can reach through `bctr`/`bctrl` that codegen has not registered.
(--game mcla; code ranges come from the PE section table.)

The runtime resolves an indirect call with the function table (the generated <project>_register.cpp). An
address that is called through a pointer but is not a function start there ends in
"[FATAL] Call to invalid or unregistered function at guest address X". This tool finds such
addresses before they crash:

  data   4-byte big-endian words in non-code sections (.rdata/.data, vtables, callback tables) that
         point 4-aligned into any code section (.text, MCLA's BINK)
  code   `lis rX,hi` + `addi/ori` address constructions in code that build such a pointer
  list   addresses from other projects, the 8-word "member adapter" thunk
         (pass --extra-list FILE for LARecomp / Renan lists)

and keeps the plausible entries: target decodes as a PPC instruction, is not a jump-table label
(tables found by `lis/addi ... lwzx ... bctr`), is not already registered.

Confidence:
  high    >=2 independent evidence kinds, or a data run that also holds registered functions (vtable /
          callback table), or a listed address, AND the previous word ends a function / is padding
  medium  one evidence kind, previous word ends a function / is padding
  low     anything else that decodes (previous word is ordinary code: likely a label)

Usage (repo root):
  python tools/re/find_indirect_targets.py --game mcla --gate
  python tools/re/find_indirect_targets.py [--game mcla] [--registered FILE|pdata|static] [--list high]
                                [--toml-out mcla/codegen/huecos.toml] [--exempt ADDR,..] [--extra-list FILE]
  --gate:       the M1 check. Needs the generated register table (default $WORK/generated/default/
                nfsmw_register.cpp: run tools/codegen.py first; either table form is read). Counts the high and
                medium candidates of every kind (gap and inside-pdata), after the built-in exempt set (7 SEH
                scope-table fields + 8 computed-switch bases, see game_config.py) and --exempt, lists them, and
                prints as its last line
                    gate: high <n> medium <m>
                Exit 0 iff both are 0, 1 otherwise, 2 if the image or the register table is missing.
  --registered: the generated register .cpp (default; <project>_register.cpp), a text file with one 0x82xxxxxx
                per line, `pdata` (only the .pdata function starts count as registered) or `static` (.pdata
                starts plus the gap segments the SDK's GapFill splits off, by gen_huecos.py's model: for before
                code has been generated; it is a model, not codegen output: the gate says so).
  --exempt:     further addresses to drop (stripped by codegen and hooked at run time, [rexcrt] / setjmp /
                longjmp), on top of the built-in set.
Output: a report on stdout; --toml-out rewrites the GENERATED block of huecos.toml.

Ported from the owner's iOS port (tools/carbon/find_indirect_targets.py).
"""
import argparse
import bisect
import collections
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pe_sections import PEImage, BASE  # noqa: E402
from game_config import add_game_argument, get_game, parse_addresses, parse_register_table  # noqa: E402

ADAPTER = [0x7C8B2378, 0x7C641B78, 0x814B0008, 0x812B0004, 0x816B0000, 0x7C6A4A14, 0x7D6903A6, 0x4E800420]

BLR, BCTR = 0x4E800020, 0x4E800420
HEADER_MARK = "# --- find_indirect_targets.py: GENERATED below, do not edit ---"


class Image:
    """The decrypted image through its section table (tools/re/pe_sections.py)."""

    def __init__(self, path):
        self.pe = PEImage(path)
        self.d = self.pe.d
        self.exec = self.pe.code_ranges                      # every code section, not just .text
        # initialised, non-code, non-discardable sections (.rdata, .data, ...) as (name, start, end)
        self.data = [(s.name, s.va, s.va + s.init_size) for s in self.pe.data_sections]
        self.pdata = [(b, b + ln) for b, ln in self.pe.pdata_entries(skip_zero=True)]

    def w(self, va):
        return struct.unpack_from(">I", self.d, va - BASE)[0]

    def is_exec(self, va):
        return self.pe.is_code(va)

    def pdata_containing(self, va):
        i = bisect.bisect_right(self.pdata, (va, 1 << 40)) - 1
        if i >= 0 and self.pdata[i][0] <= va < self.pdata[i][1]:
            return self.pdata[i]
        return None


# ---- instruction plausibility ------------------------------------------------
XO31 = {0, 4, 8, 9, 10, 11, 14, 15, 19, 20, 21, 23, 24, 26, 27, 28, 32, 40, 53, 54, 55, 58, 60, 75, 83, 86, 87, 104, 107,
        119, 122, 124, 136, 138, 139, 144, 146, 150, 151, 157, 181, 183, 200, 202, 203, 215, 232, 234, 235, 247, 266, 279,
        280, 339, 341, 343, 359, 371, 373, 375, 407, 412, 439, 444, 458, 459, 467, 470, 476, 477, 478, 487, 535, 536, 554,
        567, 597, 598, 599, 616, 618, 622, 631, 662, 663, 695, 726, 759, 792, 794, 824, 826, 854, 886, 922, 954, 982, 986,
        1014, 1015, 1019, 316, 311, 7, 6, 38, 39, 103, 135, 167, 199, 231, 263, 295, 327, 359, 391, 423, 455, 487, 519,
        551, 583, 615, 647, 679, 711, 743, 775, 807, 839, 871, 903, 935, 967, 999, 1031, 1063, 1095, 1127, 1159, 1191, 1223,
        1255, 1287, 1319, 1351, 1383, 1415, 1447, 1479, 1511, 1543, 1575, 1607, 1639, 1671, 1703, 1735, 1767, 1799, 1831,
        1863, 1895, 1927, 1959, 1991, 2023, 2055, 2087, 2119, 2151, 2183, 2215, 2247, 2279, 2311, 2343, 2375, 2407, 2439}
VALID_PRIMARY = {3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
                 57, 58, 59, 60, 61, 62, 63}


def plausible_insn(w):
    if w == 0:
        return False
    op = w >> 26
    if op not in VALID_PRIMARY:
        return False
    if op == 19:
        return ((w >> 1) & 0x3FF) in (0, 16, 33, 129, 150, 193, 225, 257, 289, 417, 449, 528, 18, 50)
    if op == 31:
        return ((w >> 1) & 0x3FF) in XO31
    if op in (56, 57, 60, 61):  # unused on Xenon user code
        return False
    return True


def prev_kind(img, t):
    p = img.w(t - 4) if img.is_exec(t - 4) else 0
    if p == 0 or p == 0x60000000:
        return "pad"
    if p in (BLR, BCTR):
        return "term"
    if p >> 26 == 18 and not p & 1:  # b / ba, no link
        return "term"
    if p >> 26 == 19 and ((p >> 1) & 0x3FF) in (16, 528) and ((p >> 21) & 31) == 20 and not p & 1:
        return "term"  # unconditional blr / bctr forms with other bits
    return "code"


def looks_like_pointer_table(img, t):
    """Two consecutive words that are themselves 4-aligned code addresses: an inline jump table."""
    ok = 0
    for k in range(2):
        if t + 4 * k < BASE + len(img.d):
            w = img.w(t + 4 * k)
            ok += (w & 3 == 0 and img.is_exec(w))
    return ok == 2


def parse_registered(path, img=None):
    """Registered function starts: a generated register .cpp (either table form), a text file of 0x82xxxxxx
    addresses, or `pdata` (the .pdata starts only: what codegen registers before GapFill and huecos)."""
    if str(path) == "pdata":
        return {b for b, _e in img.pdata}
    if str(path) == "static":
        import gen_huecos  # same folder: the gap-segment model of the SDK's GapFill
        _e, _st, _pd, segs = gen_huecos.analyse(gen_huecos.Image(img.pe.path), include_all=True)
        return {b for b, _e in img.pdata} | {a for a, _b, _par, kind in segs if kind not in ("bctr", "b")}
    path = pathlib.Path(path)
    txt = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".cpp":
        entries, _form = parse_register_table(txt)
        if not entries:
            raise SystemExit(f"error: no register table (SetFunction calls or kRexRegisterTable rows) in {path}")
        return {a for a, _n in entries}
    return {int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8})", txt)}


def switch_labels(path):
    out = set()
    if path and pathlib.Path(path).exists():
        for blk in re.findall(r"labels\s*=\s*\[(.*?)\]", pathlib.Path(path).read_text(encoding="utf-8"), re.S):
            out.update(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", blk))
    return out


def ext_list(path, pat):
    if not path.exists():
        return set()
    return {int(x, 16) for x in re.findall(pat, path.read_text(encoding="utf-8", errors="replace"), re.M)}


def scan_lis(img):
    """lis rX,hi ... addi/ori rX -> (site, target) for every address construction, plus jump-table
    starts: a data address built within 24 insns before a `bctr` that has an `lwzx` before it."""
    hits = []
    jt_starts = set()
    for a, b in img.exec:
        lis = {}  # reg -> (index, value)
        built = {}  # index -> address
        lastlwzx = -100
        for i, va in enumerate(range(a, b, 4)):
            x = img.w(va)
            op = x >> 26
            if op == 15 and ((x >> 16) & 31) == 0:  # lis
                lis[(x >> 21) & 31] = (i, (x & 0xFFFF) << 16)
            elif op in (14, 24):  # addi (RA) / ori (RS) from a recent lis
                reg = (x >> 16) & 31 if op == 14 else (x >> 21) & 31
                if reg in lis and i - lis[reg][0] <= 8:
                    si, hi = lis[reg]
                    lo = x & 0xFFFF
                    if op == 14:
                        lo -= 0x10000 if lo & 0x8000 else 0
                        t = (hi + lo) & 0xFFFFFFFF
                    else:
                        t = hi | lo
                    if BASE <= t < BASE + len(img.d):
                        hits.append((a + 4 * si, t))
                        built[i] = t
            elif op == 31 and ((x >> 1) & 0x3FF) == 23:
                lastlwzx = i
            elif x == BCTR and i - lastlwzx <= 12:
                for j in range(max(0, i - 24), i):
                    t = built.get(j)
                    if t is not None:
                        jt_starts.add(t)  # data or inline (.text) table
            if x == BLR or (op == 18 and not x & 1):
                lis.clear()
    return hits, jt_starts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_game_argument(ap)
    ap.add_argument("--image", help="decrypted PE image; default: $WORK/re/default.pe")
    ap.add_argument("--registered", help="register .cpp / address list / `pdata` / `static` (see Usage); default: "
                                         "the game's generated register .cpp")
    ap.add_argument("--exempt", action="append", default=[], metavar="ADDR[,ADDR..]",
                    help="further addresses to drop (default: the game's built-in set)")
    ap.add_argument("--extra-list", action="append", default=[], metavar="FILE",
                    help="another project's address list (any text with 0x82xxxxxx tokens; repeatable); "
                         "counts as evidence kind list:<file stem>")
    ap.add_argument("--switch-labels", metavar="TOML", help="harvest_switch_tables.toml (default: $WORK/re/...)")
    ap.add_argument("--toml-out", help="rewrite the generated block of this huecos.toml")
    ap.add_argument("--list", choices=("high", "medium", "low"), help="print candidates of that confidence")
    ap.add_argument("--pin", action="append", default=[], metavar="ADDR[:why]",
                    help="always emit this address (a runtime [invalid-call] target), with a reason")
    ap.add_argument("--no-prior", action="store_true", help="skip the prior-project address lists")
    ap.add_argument("--gate", action="store_true",
                    help="M1 check: exit 0 iff 0 high and 0 medium candidates; prints `gate: high <n> medium <m>`")
    args = ap.parse_args()

    game = get_game(args.game, args.work)
    args.image = args.image or str(game.pe)
    if not pathlib.Path(args.image).is_file():
        print(f"error: image {args.image} does not exist (python tools/re/xex_image.py)", file=sys.stderr)
        return 2
    note = None
    if args.registered is None:
        args.registered = str(game.register_cpp)
        if not game.register_cpp.exists():
            if args.gate:
                print(f"error: {game.register_cpp} does not exist: run python tools/codegen.py first "
                      f"(or pass --registered static|pdata|FILE for a model)", file=sys.stderr)
                return 2
            args.registered = "static"
            note = (f"{game.register_cpp} does not exist yet: registered baseline = .pdata starts + GapFill gap "
                    f"segments (--registered static); unregistered/gap counts are a model, not codegen output")
    elif not pathlib.Path(args.registered).is_file() and args.registered not in ("pdata", "static"):
        print(f"error: --registered {args.registered}: no such file", file=sys.stderr)
        return 2
    model = args.registered in ("pdata", "static")
    exempt = set(game.exempt) | parse_addresses(args.exempt)
    img = Image(args.image)
    reg = parse_registered(args.registered, img)
    sw_labels = switch_labels(args.switch_labels or game.switch_toml)
    print(f"image {args.image}: exec {[(hex(a), hex(b)) for a, b in img.exec][:2]}.. ({len(img.exec)} ranges); "
          f"{len(reg)} registered, {len(sw_labels)} switch labels, {len(img.pdata)} pdata")
    print(f"game {game.name}; code ranges {' '.join('%08X-%08X' % r for r in img.exec)}; "
          f"data {' '.join('%s %08X-%08X' % r for r in img.data)}; {len(exempt)} exempt")
    if note:
        print("note: " + note)

    # ---- data words and runs
    ev = collections.defaultdict(lambda: {"kinds": set(), "notes": []})
    runs = []  # (start, [(va, target)])
    for nm, a, b in img.data:
        run = []
        for va in range(a, b, 4):
            t = img.w(va)
            if t & 3 == 0 and img.is_exec(t) and t >= img.exec[0][0]:
                run.append((va, t))
            else:
                if run:
                    runs.append(run)
                run = []
        if run:
            runs.append(run)
    jt_code_hits, jt_starts = scan_lis(img)
    jt_data = set()  # data word addresses belonging to a jump table
    for run in runs:
        for va, _t in run:
            if any(s <= va for s in jt_starts if run[0][0] <= s <= run[-1][0] and s <= va):
                jt_data.add(va)
    n_words = 0
    for run in runs:
        regcnt = sum(1 for _v, t in run if t in reg)
        for va, t in run:
            n_words += 1
            if t in reg or va in jt_data:
                continue
            e = ev[t]
            if len(run) >= 2 and regcnt >= 1:
                e["kinds"].add("vtable")
            elif len(run) >= 2:
                e["kinds"].add("run")
            else:
                e["kinds"].add("data")
            e["notes"].append(f"{va:08X}")
    # ---- code constructions
    n_code = 0
    for site, t in jt_code_hits:
        if t & 3 == 0 and img.is_exec(t):
            n_code += 1
            if t not in reg:
                ev[t]["kinds"].add("lis")
                ev[t]["notes"].append(f"@{site:08X}")
    # ---- lists
    lists = {}
    if not args.no_prior:
        for name, (path, pat) in game.prior_lists.items():
            lists[name] = ext_list(path, pat)
        for f in args.extra_list:
            lists[pathlib.Path(f).stem] = ext_list(pathlib.Path(f), r"0x(82[0-9A-Fa-f]{6})")
        # member-adapter thunks
        pat = struct.pack(">8I", *ADAPTER)
        ad, i = set(), -1
        while True:
            i = img.d.find(pat, i + 1)
            if i < 0:
                break
            if i % 4 == 0:
                ad.add(BASE + i)
        lists["adapter"] = ad
        for k, s in lists.items():
            for t in s:
                if t & 3 == 0 and img.is_exec(t) and t not in reg:
                    ev[t]["kinds"].add("list:" + k)
    # ---- filter and classify
    cands = []
    drop = collections.Counter()
    for t, e in ev.items():
        if t in exempt:
            drop["exempt (reviewed, not function starts)"] += 1
            continue
        if not plausible_insn(img.w(t)):
            drop["not-an-instruction"] += 1
            continue
        if t in jt_starts:
            drop["inline/known jump table start"] += 1
            continue
        if looks_like_pointer_table(img, t):
            drop["words at target are code pointers (table)"] += 1
            continue
        if t in sw_labels:
            drop["switch-label"] += 1
            continue
        pk = prev_kind(img, t)
        pd = img.pdata_containing(t)
        cat = "inside-pdata" if pd and pd[0] != t else "gap"
        kinds = e["kinds"]
        strong = any(k == "vtable" or (k.startswith("list:") and not k.startswith("list:cp_")) for k in kinds)
        if pk in ("pad", "term") and (strong or len(kinds) >= 2):
            conf = "high"
        elif pk in ("pad", "term"):
            conf = "medium"
        else:
            conf = "low"
        cands.append((t, cat, conf, pk, sorted(kinds), pd, e["notes"][:3]))
    cands.sort()

    print(f"data: {len(runs)} pointer runs, {n_words} exec pointers; code: {n_code} lis/addi pointers to code; "
          f"jump-table data words excluded {len(jt_data)}")
    for k, s in lists.items():
        print(f"list {k}: {len(s)} addresses, {sum(1 for t in s if t in reg)} registered, "
              f"{sum(1 for t in s if t not in reg and img.is_exec(t))} unregistered in code")
    print(f"unregistered distinct targets seen: {len(ev)}; dropped " + ", ".join(f"{v} {k}" for k, v in drop.items()))
    tbl = collections.Counter((c[2], c[1]) for c in cands)
    print("candidates by confidence x place:")
    for conf in ("high", "medium", "low"):
        print(f"  {conf:7s} gap {tbl[(conf, 'gap')]:5d}   inside-pdata {tbl[(conf, 'inside-pdata')]:5d}")
    kc = collections.Counter(k for c in cands if c[2] == "high" for k in c[4])
    print("high by evidence kind: " + ", ".join(f"{k}={v}" for k, v in sorted(kc.items())))

    if args.list or args.gate:
        for t, cat, conf, pk, kinds, pd, notes in cands:
            if conf == args.list or (args.gate and conf in ("high", "medium")):
                print(f"{t:08X} {conf:6s} {cat:12s} prev={pk:4s} {','.join(kinds)} {' '.join(notes)}")

    if args.toml_out:
        out = pathlib.Path(args.toml_out)
        # addresses another include file already declares (check_codegen_toml.py rejects duplicates)
        declared = set()
        for f in out.parent.glob("*.toml"):
            if f.name != out.name and f.name != game.manifest_name:
                declared |= {int(x, 16) for x in re.findall(r'^"0x([0-9A-Fa-f]{8})"\s*=', f.read_text(encoding="utf-8"), re.M)}
        keep = out.read_text(encoding="utf-8").split(HEADER_MARK)[0].rstrip() + "\n"
        lines = [HEADER_MARK,
                 "# High-confidence indirect-call targets that codegen does not register on its own, plus pinned",
                 "# runtime crash addresses (tools/re/find_indirect_targets.py --toml-out ...).",
                 "# `{ }` = standalone function start. Inside a .pdata function it is a secondary entry: the code is",
                 "# emitted twice (once in the .pdata function, once as the entry), which is harmless."]
        emitted = set()
        for spec in args.pin:
            addr, _, why = spec.partition(":")
            t = int(addr, 16)
            if t not in declared and t not in emitted:
                lines.append(f'"0x{t:08X}" = {{ }}   # pinned: {why or "runtime invalid-call"}')
                emitted.add(t)
        n_gap = n_in = 0
        for t, cat, conf, pk, kinds, pd, notes in cands:
            if conf == "high" and t not in declared and t not in emitted:
                lines.append(f'"0x{t:08X}" = {{ }}   # {cat}; {",".join(kinds)} {" ".join(notes[:2])}')
                emitted.add(t)
                n_gap += cat == "gap"
                n_in += cat != "gap"
        out.write_text(keep + "\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {len(emitted)} entries ({n_gap} gap, {n_in} inside-pdata, {len(args.pin)} pinned) to {out}; "
              f"{len(declared)} addresses declared in other include files were skipped")

    if args.gate:
        high = sum(1 for c in cands if c[2] == "high")
        medium = sum(1 for c in cands if c[2] == "medium")
        if model:
            print(f"note: gate computed against the `{args.registered}` model, not against codegen output")
        print(f"gate: high {high} medium {medium}")
        return 0 if high == 0 and medium == 0 else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
