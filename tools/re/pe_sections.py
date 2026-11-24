#!/usr/bin/env python3
"""Section table of a decrypted Xbox 360 image (stdlib only).

The image is the PE that tools/re/xex_image.py writes from a default.xex ($WORK/re/default.pe): laid out at
virtual addresses, so the guest address D is at byte `D - 0x82000000` (`image.word(D)`). Every tool that
needs "where is the code / .pdata / .rdata" asks the section table here instead of hard-coding ranges:
MCLA has more code than `.text` (the `BINK` section).

    from pe_sections import PEImage
    img = PEImage("C:/Users/Ali/Development/mcla-x86-work/re/default.pe")
    img.code_ranges                  # [(0x82130000, 0x827BDCE4), (0x827BDE00, 0x827CD054)]
    img.pdata_entries()              # [(start, length_in_bytes), ...]
    img.rdata_range, img.pdata_range, img.text_range

CLI:  python tools/re/pe_sections.py IMAGE [--json]

A section is "code" when its flags say CNT_CODE (0x20) or MEM_EXECUTE (0x20000000). "Data" (for pointer
scans) is an initialised, non-code, non-discardable section other than `.pdata`.

Ported from the owner's iOS port (tools/core/pe_sections.py).
"""
import bisect
import json
import struct
import sys
from pathlib import Path
from typing import NamedTuple

BASE = 0x82000000

SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SCN_MEM_DISCARDABLE = 0x02000000
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000


class PEError(ValueError):
    pass


class Section(NamedTuple):
    name: str
    va: int          # guest virtual address (image base + RVA)
    size: int        # VirtualSize (RawSize when VirtualSize is 0)
    raw_size: int    # SizeOfRawData in the original file (the image keeps the full virtual size)
    flags: int

    @property
    def end(self):
        return self.va + self.size

    @property
    def init_size(self):
        """Bytes backed by file data (the rest is zero fill)."""
        return min(self.size, self.raw_size) if self.raw_size else self.size

    @property
    def is_code(self):
        return bool(self.flags & (SCN_CNT_CODE | SCN_MEM_EXECUTE))

    @property
    def is_discardable(self):
        return bool(self.flags & SCN_MEM_DISCARDABLE)

    @property
    def is_data(self):
        return (bool(self.flags & SCN_CNT_INITIALIZED_DATA) and not self.is_code
                and not self.is_discardable and self.name != ".pdata")

    def contains(self, va):
        return self.va <= va < self.end

    def to_dict(self):
        return {"name": self.name, "va": "0x%08X" % self.va, "end": "0x%08X" % self.end, "size": self.size,
                "raw_size": self.raw_size, "flags": "0x%08X" % self.flags, "code": self.is_code}


class PEImage:
    """The decrypted image and its section table. `source` is a path or the raw bytes."""

    def __init__(self, source, base=BASE):
        if isinstance(source, (bytes, bytearray, memoryview)):
            self.path = None
            self.d = bytes(source)
        else:
            self.path = Path(source)
            self.d = self.path.read_bytes()
        self.base = base
        d = self.d
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise PEError("not a PE image (no MZ header)")
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if e + 24 > len(d) or d[e:e + 4] != b"PE\0\0":
            raise PEError("not a PE image (no PE signature)")
        self.machine, nsec, _ts, _sym, _nsym, optsize, _chars = struct.unpack_from("<HHIIIHH", d, e + 4)
        opt = e + 24
        if opt + 60 > len(d):
            raise PEError("truncated optional header")
        self.entry = base + struct.unpack_from("<I", d, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        secs = []
        for i in range(nsec):
            off = opt + optsize + 40 * i
            if off + 40 > len(d):
                raise PEError("truncated section table")
            nm, vs, va, rs, _rp, _a, _b, _c, _d, fl = struct.unpack_from("<8sIIIIIIHHI", d, off)
            secs.append(Section(nm.rstrip(b"\0").decode("latin-1"), base + va, vs or rs, rs, fl))
        self.sections = tuple(secs)

    # ---- raw access ----
    @property
    def image_end(self):
        return self.base + len(self.d)

    def word(self, va):
        return struct.unpack_from(">I", self.d, va - self.base)[0]

    def words(self, start, end):
        """Big-endian u32 words of [start, end) as a tuple."""
        return struct.unpack_from(">%dI" % ((end - start) // 4), self.d, start - self.base)

    def read(self, va, n):
        return self.d[va - self.base:va - self.base + n]

    # ---- sections ----
    def sections_named(self, name):
        return [s for s in self.sections if s.name == name]

    def section(self, name):
        """The first section with this name, or None."""
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def section_at(self, va):
        for s in self.sections:
            if s.contains(va):
                return s
        return None

    @property
    def code_sections(self):
        return [s for s in self.sections if s.is_code]

    @property
    def code_ranges(self):
        """[(start, end)] of every code section, sorted by address."""
        return sorted((s.va, s.end) for s in self.code_sections)

    @property
    def code_span(self):
        """(lowest code address, end of the highest code section)."""
        r = self.code_ranges
        return (r[0][0], max(e for _, e in r)) if r else (0, 0)

    def is_code(self, va):
        r = self._starts_ends()
        i = bisect.bisect_right(r[0], va) - 1
        return i >= 0 and va < r[1][i]

    def code_range_at(self, va):
        """The (start, end) code range containing va, or None."""
        r = self._starts_ends()
        i = bisect.bisect_right(r[0], va) - 1
        return (r[0][i], r[1][i]) if i >= 0 and va < r[1][i] else None

    def _starts_ends(self):
        c = getattr(self, "_se", None)
        if c is None:
            rr = self.code_ranges
            c = self._se = ([a for a, _ in rr], [b for _, b in rr])
        return c

    @property
    def text(self):
        return self.section(".text")

    @property
    def text_range(self):
        s = self.text
        return (s.va, s.end) if s else None

    @property
    def pdata(self):
        return self.section(".pdata")

    @property
    def pdata_range(self):
        s = self.pdata
        return (s.va, s.end) if s else None

    @property
    def rdata(self):
        return self.section(".rdata")

    @property
    def rdata_range(self):
        s = self.rdata
        return (s.va, s.end) if s else None

    @property
    def data_sections(self):
        """Initialised non-code, non-discardable sections except .pdata (pointer-scan candidates)."""
        return [s for s in self.sections if s.is_data]

    def pdata_entries(self, skip_zero=False, min_len=4):
        """[(start, length_in_bytes)] from .pdata, sorted by start. Each 8-byte entry is
        BeginAddress + (PrologLength:8, FunctionLength:22, 32bit:1, Exception:1); the length is
        FunctionLength * 4, at least `min_len`."""
        s = self.pdata
        if s is None:
            return []
        out = []
        for p in range(s.va, s.va + (s.size & ~7), 8):
            b, x = struct.unpack_from(">II", self.d, p - self.base)
            if skip_zero and not b:
                continue
            out.append((b, max(((x >> 8) & 0x3FFFFF) * 4, min_len)))
        out.sort()
        return out

    def describe(self):
        lines = ["%-10s %-10s %-10s %8s  %-10s %s" % ("name", "start", "end", "size", "flags", "kind")]
        for s in self.sections:
            kind = "code" if s.is_code else ("data" if s.is_data else "other")
            lines.append("%-10s 0x%08X 0x%08X %8d  0x%08X %s" % (s.name, s.va, s.end, s.size, s.flags, kind))
        pd = self.pdata_entries()
        lines.append("pdata entries: %d" % len(pd))
        lines.append("code ranges: " + ", ".join("%08X-%08X" % r for r in self.code_ranges))
        return "\n".join(lines)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    as_json = "--json" in argv
    args = [a for a in argv if a != "--json"]
    if len(args) != 1:
        print("usage: pe_sections.py IMAGE [--json]", file=sys.stderr)
        return 2
    img = PEImage(args[0])
    if as_json:
        print(json.dumps({"sections": [s.to_dict() for s in img.sections],
                          "code_ranges": ["0x%08X-0x%08X" % r for r in img.code_ranges],
                          "pdata_entries": len(img.pdata_entries()),
                          "entry": "0x%08X" % img.entry}, indent=1))
    else:
        print(img.describe())
    return 0


if __name__ == "__main__":
    sys.exit(main())
