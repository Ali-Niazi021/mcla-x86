#!/usr/bin/env python3
"""Generate a codegen [functions] table (huecos.toml schema) for the game's PE (--game mcla).

ReXGlue registers every .pdata entry as a function (size = FunctionLength*4,
extended by SEH/C++ EH info), then GapFill splits the remaining non-zero code
runs on `blr` / `b <known function>`.  Functions are missed when a code run
holds several functions with no such terminator between them (leaf thunks that
end in `bctr`, `b <non-function>`, ...), or when a pdata function is cut short
and its tail (reached by conditional branches) falls in the gap.

Mode A (static, default): analyse the PE image.
  * nonzero runs inside the gaps between consecutive .pdata ranges are split on
    blr / b-to-known-function / bctr / b-to-outside (more aggressive than the
    SDK, so thunks that the SDK would glue together get their own entry);
  * a segment is emitted as a standalone start ("0xADDR" = { }) when it is
    targeted by a direct bl/b from elsewhere, or (--all-segments) always;
  * a gap whose code is reached by a conditional branch from the preceding
    pdata function (or branches back into it) is a continuation: emitted as
    "0xPARENT" = { end = 0xEND } instead.
Mode B (--log LOG): parse codegen log lines
    "Unresolved conditional branch to 0xX from 0xY"  (and "UnresolvedCall: 0xX - target not in any function")
  and emit a fix: standalone start at X when X is outside every pdata range
  or sits at a gap segment start; otherwise extend the function containing
  Y to cover X's segment ("{ end = }").  Addresses already declared in
  --existing TOML files are skipped.

Output: TOML on stdout or -o, sorted, de-duplicated, "# N bytes" comments.

Ranges (code, .pdata, .rdata, data) come from the PE section table (tools/re/pe_sections.py), never
from constants: every code section is analysed (MCLA: .text + `BINK`).
Default image: $WORK/re/default.pe (--work / MCLA_WORK).

Ported from the owner's iOS port (tools/carbon/gen_huecos.py).
"""
import argparse, os, re, struct, sys, bisect

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_sections import PEImage, BASE  # noqa: E402
from game_config import add_game_argument, get_game  # noqa: E402

BLR = 0x4E800020


class Image:
    def __init__(self, path):
        self.pe = PEImage(path)
        self.d = self.pe.d
        self.code = self.pe.code_ranges                       # every code section, not just .text
        self.rdata = self.pe.rdata_range
        if not self.code or not self.pdata_range() or not self.rdata:
            raise SystemExit('%s: no code, .pdata or .rdata section in the section table' % path)
        # initialised data that can hold code pointers (vtables, callback tables)
        self.data = [(s.va, min(s.end, self.pe.image_end)) for s in self.pe.data_sections]

    def pdata_range(self):
        return self.pe.pdata_range

    def w(self, va):
        return struct.unpack_from('>I', self.d, va - BASE)[0]

    def is_code(self, va):
        return self.pe.is_code(va)

    def pdata(self):
        return self.pe.pdata_entries()


def branch_target(pc, w):
    op = w >> 26
    if op == 18:  # b/bl/ba/bla
        if w & 2:
            return None
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (pc + li) & 0xFFFFFFFF
    if op == 16:  # bc
        if w & 2:
            return None
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (pc + bd) & 0xFFFFFFFF
    return None


def is_exception_data(img, a):
    """handler ptr in code + scope ptr in .rdata (SDK looksLikeExceptionData)."""
    cr = img.pe.code_range_at(a)
    if cr is None or a + 8 > cr[1]:
        return False
    w0, w1 = img.w(a), img.w(a + 4)
    return img.is_code(w0) and img.rdata[0] <= w1 < img.rdata[1]


def analyse(img, include_all=False, min_size=4, only_glued=False):
    pd = img.pdata()
    starts = [b for b, _ in pd]
    startset = set(starts)
    stats = dict(pdata=len(pd))
    # xref table: direct bl/b targets over all code sections
    refs = {}   # target -> count (bl + data pointers)
    brefs = []
    cond = []   # (pc, target) for bc
    d = img.d
    span_lo, span_hi = img.pe.code_span
    lis_lo, lis_hi = span_lo >> 16, (span_hi >> 16) + 1   # `lis` immediates that can reach code
    lisaddr = {}
    for ca, ce in img.code:
        n = (ce - ca) // 4
        words = struct.unpack_from('>%dI' % n, d, ca - BASE)
        for i, w in enumerate(words):
            op = w >> 26
            if op == 18 or op == 16:
                pc = ca + i * 4
                t = branch_target(pc, w)
                if t is None:
                    continue
                if op == 18:
                    if words[i] & 1:
                        refs[t] = refs.get(t, 0) + 1     # bl
                    else:
                        brefs.append((pc, t))            # b (tail call or internal jump)
                else:
                    cond.append((pc, t))
        # lis rX,hi ; addi/ori rY,rX,lo within 8 insns: code addresses taken in code
        # (vtable-less callbacks, indirectly called functions)
        for i, w in enumerate(words):
            if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and lis_lo <= (w & 0xFFFF) <= lis_hi:
                rt = (w >> 21) & 31
                for j in range(i + 1, min(i + 9, n)):
                    x = words[j]
                    if (x >> 26) in (14, 24) and ((x >> 16) & 31) == rt:   # addi / ori
                        lo = x & 0xFFFF
                        if (x >> 26) == 14 and lo & 0x8000:
                            lo -= 0x10000
                        v = ((w & 0xFFFF) << 16) + lo
                        if img.is_code(v) and not v & 3:
                            lisaddr[v] = lisaddr.get(v, 0) + 1
    # absolute function pointers in data (vtables, callbacks, jump-table-adjacent)
    for da, de in img.data:
        for v in struct.unpack_from('>%dI' % ((de - da) // 4), d, da - BASE):
            if not v & 3 and img.is_code(v):
                refs[v] = refs.get(v, 0) + 1
    # gaps: the space between .pdata functions, per code section
    gaps = []
    for cs, ce in img.code:
        prev_end = cs
        prev_start = None
        for k in range(bisect.bisect_left(starts, cs), bisect.bisect_left(starts, ce)):
            b, sz = pd[k]
            if b > prev_end:
                gaps.append((prev_end, b, prev_start))
            prev_end = max(prev_end, b + sz)
            prev_start = b
        if prev_end < ce:
            gaps.append((prev_end, ce, prev_start))
    stats['gaps'] = len(gaps)
    # nonzero runs per gap, split into segments
    segs = []   # (start, end, parent_start)
    for gs, ge, par in gaps:
        a = gs
        while a < ge:
            if img.w(a) == 0:
                a += 4
                continue
            if is_exception_data(img, a):
                a += 8
                continue
            rs = a
            while a < ge and img.w(a) != 0:
                a += 4
            # split run on terminators, but never inside a forward-branch closure
            # (a mid-function early `blr` must not cut the function in two)
            s = rs
            sk = 'run'
            maxfwd = 0
            for p in range(rs, a, 4):
                w = img.w(p)
                op = w >> 26
                term = None
                if op in (16, 18) and not (op == 18 and w & 1):
                    t = branch_target(p, w)
                    if t is not None and p < t < a:
                        maxfwd = max(maxfwd, t)
                if w == BLR:
                    term = 'blr'
                elif (w & 0xFC00FFFF) == 0x4C000420 and (w >> 21 & 31) == 20:  # bctr
                    term = 'bctr'
                elif op == 18 and not (w & 1):
                    t = branch_target(p, w)
                    if t is not None and t != s and (t in startset or t < s or t >= a):
                        term = 'tail' if t in startset else 'b'
                if term and maxfwd <= p:
                    segs.append((s, p + 4, par, sk))
                    s = p + 4
                    sk = term
            if s < a:
                segs.append((s, a, par, sk))
    stats['gap_segments'] = len(segs)
    # continuation detection
    pend = {b: b + sz for b, sz in pd}
    fn_by_start = dict(pd)
    seg_starts = [s[0] for s in segs]
    ext = {}  # parent -> end
    cont_segs = set()
    seg_of = lambda x: (lambda i: segs[i] if i >= 0 and segs[i][0] <= x < segs[i][1] else None)(bisect.bisect_right(seg_starts, x) - 1)
    for pc, t in cond:
        sp = seg_of(t)
        spc = seg_of(pc)
        if sp is not None and sp[2] is not None and pc >= sp[2] and sp[2] <= pc < pend[sp[2]] and pc < sp[0]:
            # pdata function branches (conditionally) forward into gap segment
            ext[sp[2]] = max(ext.get(sp[2], 0), sp[1])
            cont_segs.add(sp[0])
        elif spc is not None and spc[2] is not None and spc[2] <= t < pend[spc[2]]:
            # gap code branches back into preceding pdata function
            ext[spc[2]] = max(ext.get(spc[2], 0), spc[1])
            cont_segs.add(spc[0])
    # propagate: a continuation extends over the whole contiguous chain of segments
    entries = {}
    for par, end in ext.items():
        # also cover segments adjacent up to end, already included by max
        entries[par] = ('end', end, end - par)
    standalone = 0
    segstart = {x[0]: x for x in segs}
    for pc, t in brefs:
        x = segstart.get(t)
        if x is not None and not (x[0] <= pc < x[1]):
            refs[t] = refs.get(t, 0) + 1
    # indirectly-referenced addresses inside a gap segment, right after a terminator
    def prev_is_term(t):
        pw = img.w(t - 4)
        return pw == 0 or pw == BLR or (pw & 0xFC00FFFF) == 0x4C000420 or ((pw >> 26) == 18 and not pw & 1)
    extra = {}
    for t in list(lisaddr) + [t for t in refs if t in ()]:
        x = seg_of(t)
        if x is not None and t != x[0] and prev_is_term(t) and t not in startset:
            extra[t] = x[1] - t
    stats['indirect_split_points'] = len(extra)
    glued = 0
    for s, e, par, sk in segs:
        if s in cont_segs or e - s < min_size:
            continue
        if any(par is not None and par in ext and s < ext[par] and s >= par for _ in (0,)):
            continue
        g = sk in ('bctr', 'b')   # SDK GapFill does not split here (upstream PR #554)
        glued += g
        if include_all or g or (refs.get(s) and not only_glued):
            if s not in entries and s not in startset:
                entries[s] = ('start', None, e - s)
                standalone += 1
    for t, sz in extra.items():
        if t not in entries and sz >= min_size:
            entries[t] = ('start', None, sz)
            standalone += 1
    stats['continuations'] = len(ext)
    stats['standalone'] = standalone
    stats['segments_after_bctr_or_b'] = glued
    stats['segments_skipped'] = len(segs) - standalone - len(cont_segs)
    return entries, stats, pd, segs


LOGRE = [re.compile(r'Unresolved conditional branch to 0x([0-9A-Fa-f]+) from 0x([0-9A-Fa-f]+)'),
         re.compile(r'Unresolved branch from 0x([0-9A-Fa-f]+) to 0x([0-9A-Fa-f]+)')]
# UnresolvedCall: the old one-line form, and the SDK v0.10.0 report line
# "  0xTARGET from 0xSITE: bl 0xTARGET from 0xSITE - target not in any function"
CALLRE = re.compile(r'UnresolvedCall: 0x([0-9A-Fa-f]+)|'
                    r'0x([0-9A-Fa-f]{8}) from 0x[0-9A-Fa-f]{8}: (?:bl|b) 0x[0-9A-Fa-f]+ from 0x[0-9A-Fa-f]+ '
                    r'- target not in any function')


def from_log(img, log, include_all):
    pd = img.pdata()
    starts = [b for b, _ in pd]
    ends = {b: b + s for b, s in pd}
    entries = {}
    _, _, _, segs = analyse(img, include_all=True)
    seg_starts = [s[0] for s in segs]
    def seg_of(x):
        i = bisect.bisect_right(seg_starts, x) - 1
        return segs[i] if i >= 0 and segs[i][0] <= x < segs[i][1] else None
    def pdata_of(x):
        i = bisect.bisect_right(starts, x) - 1
        return starts[i] if i >= 0 and x < ends[starts[i]] else None
    for line in open(log, encoding='utf-8', errors='replace'):
        m = LOGRE[0].search(line)
        if m:
            tgt, src = int(m.group(1), 16), int(m.group(2), 16)
        elif LOGRE[1].search(line):
            m = LOGRE[1].search(line); src, tgt = int(m.group(1), 16), int(m.group(2), 16)
        else:
            m = CALLRE.search(line)
            if m:
                t = int(m.group(1) or m.group(2), 16)
                if pdata_of(t) is None:
                    entries[t] = ('start', None, (seg_of(t) or (t, t + 4, 0))[1] - t)
            continue
        if pdata_of(tgt) is not None and pdata_of(tgt) != tgt and pdata_of(src) == pdata_of(tgt):
            continue
        sp = seg_of(tgt)
        par = pdata_of(src)
        if par is None:
            par = next((s[2] for s in segs if s[0] <= src < s[1]), None)
        if par is not None and par < tgt:
            e = sp[1] if sp else tgt + 4
            if tgt >= ends.get(par, 0) or sp:
                cur = entries.get(par)
                e = max(e, cur[1] if cur and cur[0] == 'end' else 0)
                entries[par] = ('end', e, e - par)
        elif par is not None and tgt < par and sp is None:
            entries[tgt] = ('start', None, 4)
        else:
            entries[tgt] = ('start', None, (sp[1] - tgt) if sp else 4)
    return entries


def emit(entries, existing, out, header):
    out.write(header)
    out.write('[functions]\n\n')
    n = 0
    for a in sorted(entries):
        if a in existing:
            continue
        kind, end, size = entries[a]
        if kind == 'end':
            out.write('"0x%08X" = { end = 0x%08X }   # %d bytes (continuation)\n' % (a, end, size))
        else:
            out.write('"0x%08X" = { }   # %d bytes\n' % (a, size))
        n += 1
    return n


def load_existing(paths):
    keys = set()
    for p in paths:
        for l in open(p, encoding='utf-8'):
            m = re.match(r'\s*"0x([0-9A-Fa-f]+)"\s*=', l)
            if m:
                keys.add(int(m.group(1), 16))
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_game_argument(ap)
    ap.add_argument('--pe', help='decrypted PE image (base 0x82000000); default: $WORK/re/default.pe')
    ap.add_argument('--log', help='codegen log: Mode B (unresolved-branch fixes) instead of Mode A')
    ap.add_argument('--existing', action='append', default=[], help='TOML whose keys are skipped (repeatable)')
    ap.add_argument('--all-segments', action='store_true', help='Mode A: emit every gap segment, not only those with a direct bl/b xref')
    ap.add_argument('--only-glued', action='store_true', help='Mode A: only segments the SDK GapFill cannot split off (after bctr / non-function b) plus indirect-address split points')
    ap.add_argument('--min-size', type=int, default=8, help='Mode A: minimum segment size in bytes (default 8)')
    ap.add_argument('--show-ranges', action='store_true', help='print the code / .pdata / .rdata / data ranges taken from the section table to stderr')
    ap.add_argument('-o', '--output', help='output file (default stdout)')
    a = ap.parse_args()
    game = get_game(a.game, a.work)
    img = Image(a.pe or str(game.pe))
    if a.show_ranges:
        print('game %s, image %s' % (game.name, a.pe or game.pe), file=sys.stderr)
        print('code:  ' + ', '.join('%08X-%08X' % r for r in img.code), file=sys.stderr)
        print('pdata: %08X-%08X (%d entries)  rdata: %08X-%08X' % (img.pe.pdata_range + (len(img.pdata()),) + tuple(img.rdata)), file=sys.stderr)
        print('data:  ' + ', '.join('%08X-%08X' % r for r in img.data), file=sys.stderr)
    existing = load_existing(a.existing)
    who = 'tools/re/gen_huecos.py'
    if a.log:
        entries = from_log(img, a.log, a.all_segments)
        hdr = '# Generated by %s (mode B, log %s)\n' % (who, a.log)
        stats = {}
    else:
        entries, stats, _, _ = analyse(img, a.all_segments, a.min_size, a.only_glued)
        hdr = ('# Generated by %s (mode A, static).\n'
               '# stats: %s\n' % (who, ', '.join('%s=%s' % kv for kv in stats.items())))
    out = open(a.output, 'w', newline='\n') if a.output else sys.stdout
    n = emit(entries, existing, out, hdr)
    if a.output:
        out.close()
    print('entries: %d  %s' % (n, stats), file=sys.stderr)


if __name__ == '__main__':
    main()
