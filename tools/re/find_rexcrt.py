#!/usr/bin/env python3
"""Locate Xbox 360 CRT functions for ReXGlue's [rexcrt] table (and setjmp/longjmp,
save/rest helpers) in the game's decrypted PE (--game mcla), by structure.  No capstone needed:
only a handful of PPC opcodes are decoded by hand.

[rexcrt] (SDK v0.10.0): any key `Name = 0xADDR` makes codegen rename that function
to rexcrt_<Name>, drop it from recompilation and register it in the function table;
the runtime must provide REX_HOOK(rexcrt_<Name>).  Supported hooks: memcpy memmove
memset memchr XMemCpy XMemSet XMemSet128 memset_vmx memcpy_s memmove_s str*/wcs*/lstr*
and file/fiber Win32 wrappers; the heap group (RtlAllocateHeap RtlFreeHeap RtlSizeHeap
RtlReAllocateHeap) is all-or-nothing.  setjmp/longjmp are NOT [rexcrt]: they are root
keys setjmp_address / longjmp_address.

Usage: find_rexcrt.py [--game mcla] [--work DIR] [--pe PATH] [--top N] [--leaf-xrefs N] [-v]
Scores every function start (.pdata starts + bl targets, in every code section of the PE's section
table: .text and MCLA's `BINK`) by: leaf (no bl/mflr), dcbt count, dcbz/dcbz128
count, size, number of direct `bl` xrefs.  --leaf-xrefs N adds the N leaf functions with the most
xrefs (memset/strlen-class candidates that use neither dcbt nor dcbz).
Default image: $WORK/re/default.pe.

Ported from the owner's iOS port (tools/carbon/find_rexcrt.py).
"""
import argparse, bisect, collections, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_sections import PEImage  # noqa: E402
from game_config import add_game_argument, get_game  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_game_argument(ap)
    ap.add_argument('--pe', help='decrypted PE image; default: $WORK/re/default.pe')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--leaf-xrefs', type=int, default=0, metavar='N',
                    help='also list the N leaf functions with the most direct bl xrefs')
    ap.add_argument('-v', '--verbose', action='store_true', help='print the section-table ranges used to stderr')
    a = ap.parse_args()
    game = get_game(a.game, a.work)
    img = PEImage(a.pe or str(game.pe))
    ranges = img.code_ranges
    if not ranges or img.pdata is None:
        raise SystemExit('no code or .pdata section in the section table')
    if a.verbose:
        print('game %s; code ranges: %s; pdata %08X-%08X' % (
            game.name, ' '.join('%08X-%08X' % r for r in ranges), *img.pdata_range), file=sys.stderr)
    Ws = [img.words(ca, ce) for ca, ce in ranges]       # one word tuple per code section
    rstarts = [ca for ca, _ in ranges]
    pd = {}
    for b, ln in img.pdata_entries():
        pd[b] = ln
    blref = collections.Counter()
    for (ca, _ce), W in zip(ranges, Ws):
        for i, w in enumerate(W):
            if (w >> 26) == 18 and w & 1 and not w & 2:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                blref[(ca + i * 4 + li) & 0xFFFFFFFF] += 1
    # function starts: pdata + any bl target in code
    starts = set(b for b in pd if img.is_code(b)) | set(t for t in blref if img.is_code(t))
    rows = []
    for s in starts:
        k = bisect.bisect_right(rstarts, s) - 1
        ca, W = ranges[k][0], Ws[k]
        n = len(W)
        i = (s - ca) // 4
        j = i
        feat = collections.Counter()
        limit = min(n, i + 1024)
        # walk to the end of the function: first blr/bctr not covered by a forward branch
        maxfwd = 0
        while j < limit:
            w = W[j]
            op = w >> 26
            pc = ca + j * 4
            if op == 31:
                xo = (w >> 1) & 0x3FF
                if xo == 278: feat['dcbt'] += 1
                if xo == 1014: feat['dcbz'] += 1
                if xo == 339 and (w >> 11 & 0x3FF) == 8: feat['mflr'] += 1
            if op == 18 and w & 1: feat['bl'] += 1
            if (op == 16 or op == 18) and not (w & 3):
                bd = (w & 0xFFFC) - (0x10000 if w & 0x8000 else 0) if op == 16 else \
                     (w & 0x03FFFFFC) - (0x04000000 if w & 0x02000000 else 0)
                if bd > 0 and not (op == 18 and w & 1):
                    maxfwd = max(maxfwd, pc + bd)
            j += 1
            if w == 0:
                break
            if (w == 0x4E800020 or w == 0x4E800420) and maxfwd <= pc:
                break
        size = (j - i) * 4
        rows.append((s, size, feat, blref.get(s, 0)))
    def show(title, sel):
        print('== ' + title)
        for s, size, f, x in sel[:a.top]:
            print('  0x%08X xrefs=%-5d scan=%-5d dcbt=%-2d dcbz=%-2d bl=%-2d mflr=%d pdata=%s' % (
                s, x, size, f['dcbt'], f['dcbz'], f['bl'], f['mflr'], pd.get(s)))
    cand = [r for r in rows if r[2]['dcbt'] and r[2]['bl'] == 0 and r[2]['mflr'] == 0]
    cand.sort(key=lambda r: -r[3])
    show('leaf with dcbt (memcpy/memmove/XMemCpy candidates), by bl xref count', cand)
    cand = [r for r in rows if r[2]['dcbz'] and r[2]['bl'] == 0 and r[2]['mflr'] == 0]
    cand.sort(key=lambda r: -r[3])
    show('leaf with dcbz/dcbz128 (memset/XMemSet candidates)', cand)
    if a.leaf_xrefs:
        cand = [r for r in rows if r[2]['bl'] == 0 and r[2]['mflr'] == 0 and r[1] > 4]
        cand.sort(key=lambda r: (-r[3], r[0]))
        print('== leaf functions (no bl/mflr, > 1 insn) by bl xref count')
        for s, size, f, x in cand[:a.leaf_xrefs]:
            print('  0x%08X xrefs=%-5d scan=%-5d dcbt=%-2d dcbz=%-2d pdata=%s' % (s, x, size, f['dcbt'], f['dcbz'], pd.get(s)))
    # setjmp/longjmp, helpers by signature.  An item is None (any word), an int (exact word) or a predicate.
    d_lo = min((s.va for s in img.data_sections if s.flags & 0x80000000), default=0) >> 16   # writable data pages
    d_hi = max((s.end for s in img.data_sections if s.flags & 0x80000000), default=0) >> 16
    lis_r4_data = lambda w: (w & 0xFFFF0000) == 0x3C800000 and d_lo <= (w & 0xFFFF) <= d_hi
    sig = {
        # CRT bodies are identical across games built with the same XDK
        'memcpy(std r3,-8(r1);rlwinm r6,r3,..;dcbt;cmplwi)': (0xF861FFF8, 0x5466077E, 0x7C00222C, 0x28060000),
        'memset(addi r0,r5,1;mtctr;ori r6,r3,0;b)': (0x38050001, 0x7C0903A6, 0x60660000, 0x48000010),
        'savegprlr_14': (0xF9C1FF68,), 'restgprlr_14': (0xE9C1FF68,),
        'savefpr_14': (0xD9CCFF70,), 'restfpr_14': (0xC9CCFF70,),
        # li r11,-0x120; stvx v14,r11,r12 / lvx v14,r11,r12 ...   and the v64.. block: li r11,-0x400; stvx128/lvx128
        'savevmx_14': (0x3960FEE0, 0x7DCB61CE, 0x3960FEF0, 0x7DEB61CE),
        'restvmx_14': (0x3960FEE0, 0x7DCB60CE, 0x3960FEF0, 0x7DEB60CE),
        'savevmx_64': (0x3960FC00, 0x100B61CB, 0x3960FC10, 0x102B61CB),
        'restvmx_64': (0x3960FC00, 0x100B60CB, 0x3960FC10, 0x102B60CB),
        'setjmp(lis r4,<data page>;lwz r0,..;cmpwi;mtctr;bnectr)': (lis_r4_data, None, 0x2C000000, 0x7C0903A6, 0x4C820420),
        'longjmp(mflr;stwu -0x50;stw r0)': (0x7C0802A6, 0x9421FFB0, 0x90010008, 0x7C862378),
    }
    def match(p, w):
        return p is None or (p(w) if callable(p) else w == p)
    print('== setjmp/longjmp/save-rest helpers (exact signature matches)')
    for name, pat in sig.items():
        hits = []
        for (ca, _ce), W in zip(ranges, Ws):
            for i in range(len(W) - len(pat)):
                if all(match(p, W[i + k]) for k, p in enumerate(pat)):
                    hits.append(ca + i * 4)
        print('  %-60s %s' % (name, ' '.join('0x%08X(bl xrefs %d)' % (h, blref.get(h, 0)) for h in hits[:8])))


if __name__ == '__main__':
    main()
