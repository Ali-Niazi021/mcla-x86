#!/usr/bin/env python3
"""xiso.py - Xbox 360 XDVDFS disc-image tool for the MCLA x86 port (PLAN 2, 6 G0, Appendix C).

Stdlib only (Python 3.9+). Nothing is decrypted or decompressed: the tool walks the XDVDFS directory
tree, hashes or copies files, and parses the XEX2 header of default.xex.

Commands
    verify <iso>          locate default.xex in the image, stream its SHA-256, parse the XEX2 header and compare
                          with the pinned identity (Midnight Club: Los Angeles, Complete Edition, media 5940C9DB).
                          Prints exactly one line and exits 0 only on a full match:
                              OK default.xex sha256=<64 hex> title=<8 hex> media=<8 hex> version=<n> disc=<i>/<n>
                          Otherwise one line per mismatching field and exit 2:
                              FAIL <field> expected=<x> got=<y>
    verify-xex <xex>      same check for a loose default.xex (for example $GAME/default.xex), same output
    list <iso>            every file in the image: size and path, then a summary line
    extract <iso> <dest>  copy the game files into <dest>. Skips $SystemUpdate/ and *.pad. Streams in 8 MiB
                          chunks, writes <name>.part then renames, skips files whose size already matches
                          (unless --force). Prints, as its last line:
                              extracted <n> files, <bytes> bytes
                          (n and bytes count every wanted file now present in <dest>, written or skipped)

Exit codes: 0 ok, 1 operational error (unreadable or not an XDVDFS image, short read, disk full ...), 2 identity
mismatch (verify, verify-xex), 130 interrupted.

Contract lines go to stdout; progress and diagnostics go to stderr, so `... | grep` and `$(...)` stay clean.

Paths: <iso> for verify/list and <xex> for verify-xex may be omitted; the tool then looks in the game directory
(--game DIR, else $MCLA_GAME, else C:/Users/Ali/Development/dumps/mcla-x360) for the single *.iso or default.xex.

Format notes (XDVDFS): the volume descriptor sits at sector 32 of the partition (magic MICROSOFT*XBOX*MEDIA at +0
and +0x7EC, root sector and size as little-endian u32 at +0x14/+0x18). A directory is a binary tree of
{u16 left, u16 right, u32 sector, u32 size, u8 attrs, u8 name_len, name}; left/right are in 4-byte units,
0 and 0xFFFF mean "no child". The partition starts at 0x0FD90000 (XGD2, this disc), 0x02080000 (XGD3),
0x18300000 (XGD1) or 0 (already trimmed). XEX2 headers are big-endian; optional header 0x00040006 holds the
execution info (media id, version, base version, title id, platform, executable type, disc number, disc count).
"""

import argparse
import hashlib
import os
import shutil
import struct
import sys
import time
from typing import Iterator, List, NamedTuple, Optional, Tuple

# --- Pinned identity (PLAN 2: default.xex of the Complete Edition, redump 64472) ---------------------------------
PIN_SHA256 = "c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432"
PIN_TITLE_ID = 0x545407F8
PIN_MEDIA_ID = 0x5940C9DB
PIN_VERSION = 8
PIN_BASE_VERSION = 8
PIN_DISC_NUMBER = 1
PIN_DISC_COUNT = 1
PIN_XEX_NAME = "default.xex"

# --- Defaults (overridable by --game / MCLA_GAME) ----------------------------------------------------------------
DEFAULT_GAME = "C:/Users/Ali/Development/dumps/mcla-x360"

# --- XDVDFS constants --------------------------------------------------------------------------------------------
SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_BASES = (
    (0x0FD90000, "XGD2"),
    (0x02080000, "XGD3"),
    (0x18300000, "XGD1"),
    (0x00000000, "raw/trimmed"),
)
ATTR_DIRECTORY = 0x10
DIR_ENTRY = struct.Struct("<HHIIBB")  # left, right, sector, size, attrs, name_len (14 bytes)
MAX_DIR_BYTES = 64 << 20
MAX_DEPTH = 32

# --- XEX2 constants ----------------------------------------------------------------------------------------------
XEX_MAGIC = b"XEX2"
XEX_OPT_EXECUTION_INFO = 0x00040006
XEX_OPT_ENTRY_POINT = 0x00010100
XEX_OPT_IMAGE_BASE = 0x00010201
XEX_MAX_OPT_HEADERS = 0x100
XEX_HEAD_BYTES = 1 << 20  # the header (0x3000 bytes for this title) is parsed from the first MiB of the file

CHUNK = 8 << 20
PROGRESS_SECONDS = 15.0

# Extraction filter (PLAN 2: 12 game files; the disc has 15)
SKIP_DIR_PREFIX = "$systemupdate/"
SKIP_SUFFIX = ".pad"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_MISMATCH = 2
EXIT_INTERRUPTED = 130


class XiError(Exception):
    """Operational error (bad image, short read, unsafe name ...): exit 1."""


class XexError(Exception):
    """The bytes are not a usable XEX2 header. The message is a single token (it ends up in a FAIL line)."""


def warn(msg: str) -> None:
    print("xiso: warning: " + msg, file=sys.stderr, flush=True)


def info(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# ================================================================================================================
# XDVDFS
# ================================================================================================================

class Entry(NamedTuple):
    path: str  # '/'-separated, relative to the partition root
    sector: int
    size: int
    is_dir: bool


class Image:
    """A read-only XDVDFS image. Not thread-safe (one file position)."""

    def __init__(self, path: str) -> None:
        self.path = path
        self._f = open(path, "rb", buffering=0)
        try:
            self.image_size = os.fstat(self._f.fileno()).st_size
            self.base, self.layout = self._probe()
            self.root_sector, self.root_size = self._read_descriptor()
        except BaseException:
            self._f.close()
            raise

    def __enter__(self) -> "Image":
        return self

    def __exit__(self, *exc) -> None:
        self._f.close()

    # -- low level ---------------------------------------------------------------------------------------------
    def _read_at(self, offset: int, n: int) -> bytes:
        self._f.seek(offset)
        parts = []
        got = 0
        while got < n:
            b = self._f.read(n - got)
            if not b:
                break
            parts.append(b)
            got += len(b)
        return b"".join(parts)

    def _probe(self) -> Tuple[int, str]:
        for base, layout in PARTITION_BASES:
            at = base + 32 * SECTOR
            if at + SECTOR > self.image_size:
                continue
            if self._read_at(at, len(XDVDFS_MAGIC)) == XDVDFS_MAGIC:
                return base, layout
        tried = ", ".join("0x%08X" % b for b, _ in PARTITION_BASES)
        raise XiError("%s: no XDVDFS volume descriptor (%s at sector 32) at partition offsets %s; "
                      "not an Xbox 360 ISO, or a compressed container (CCI/GOD/ZAR)"
                      % (self.path, XDVDFS_MAGIC.decode(), tried))

    def _read_descriptor(self) -> Tuple[int, int]:
        vd = self._read_at(self.base + 32 * SECTOR, SECTOR)
        if len(vd) < SECTOR or vd[:20] != XDVDFS_MAGIC:
            raise XiError("%s: invalid XDVDFS volume descriptor" % self.path)
        if vd[0x7EC:0x7EC + 20] != XDVDFS_MAGIC:
            warn("closing magic missing at +0x7EC of the volume descriptor (truncated image?)")
        root_sector, root_size = struct.unpack_from("<II", vd, 0x14)
        return root_sector, root_size

    # -- directory tree ----------------------------------------------------------------------------------------
    def _read_dir(self, sector: int, size: int, where: str) -> List[Entry]:
        if size == 0:
            return []
        if size > MAX_DIR_BYTES:
            raise XiError("directory %s: table size %d is implausible" % (where, size))
        table = self._read_at(self.base + sector * SECTOR, size)
        if len(table) != size:
            raise XiError("directory %s: table truncated (%d of %d bytes); image incomplete?"
                          % (where, len(table), size))
        if table[:8] == b"\xff" * 8:  # no real entry has sector 0xFFFFFFFF: an empty directory marked by 0xFF fill
            return []
        out: List[Entry] = []
        seen = set()
        stack = [0]
        while stack:
            off = stack.pop()
            if off in seen:
                raise XiError("directory %s: entry offset 0x%X visited twice (corrupt tree)" % (where, off))
            if off + DIR_ENTRY.size > len(table):
                raise XiError("directory %s: entry offset 0x%X is outside the %d byte table" % (where, off, len(table)))
            seen.add(off)
            left, right, esector, esize, attrs, nlen = DIR_ENTRY.unpack_from(table, off)
            end = off + DIR_ENTRY.size + nlen
            if nlen == 0 or end > len(table):
                raise XiError("directory %s: bad name length %d at offset 0x%X" % (where, nlen, off))
            name = table[off + DIR_ENTRY.size:end].decode("latin-1")
            if name in (".", "..") or any(c in name for c in "/\\\0"):
                raise XiError("directory %s: unsafe entry name %r" % (where, name))
            for child in (left, right):
                if child not in (0, 0xFFFF):
                    stack.append(child * 4)
            out.append(Entry(name, esector, esize, bool(attrs & ATTR_DIRECTORY)))
        out.sort(key=lambda e: e.path.lower())  # here Entry.path is the bare name; walk() makes it a full path
        return out

    def walk(self) -> Iterator[Entry]:
        """Every entry (directories included), depth first, siblings sorted case-insensitively."""
        seen_dirs = set()

        def rec(sector: int, size: int, prefix: str, depth: int) -> Iterator[Entry]:
            if depth > MAX_DEPTH:
                raise XiError("directory nesting deeper than %d at %s" % (MAX_DEPTH, prefix or "/"))
            if sector in seen_dirs:
                raise XiError("directory loop at %s" % (prefix or "/"))
            seen_dirs.add(sector)
            for e in self._read_dir(sector, size, prefix or "/"):
                full = prefix + e.path
                yield Entry(full, e.sector, e.size, e.is_dir)
                if e.is_dir:
                    yield from rec(e.sector, e.size, full + "/", depth + 1)

        return rec(self.root_sector, self.root_size, "", 0)

    def files(self) -> List[Entry]:
        return [e for e in self.walk() if not e.is_dir]

    def find_root_file(self, name: str) -> Optional[Entry]:
        want = name.lower()
        for e in self._read_dir(self.root_sector, self.root_size, "/"):
            if not e.is_dir and e.path.lower() == want:
                return e
        return None

    # -- file data ---------------------------------------------------------------------------------------------
    def end_of_file(self, e: Entry) -> int:
        return self.base + e.sector * SECTOR + e.size

    def chunks(self, e: Entry, buf: bytearray) -> Iterator[memoryview]:
        """Yield the file's bytes as views of `buf` (valid until the next iteration; no per-chunk allocation)."""
        if self.end_of_file(e) > self.image_size:
            raise XiError("%s ends at byte %d, past the end of the image (%d bytes): image incomplete?"
                          % (e.path, self.end_of_file(e), self.image_size))
        self._f.seek(self.base + e.sector * SECTOR)
        mv = memoryview(buf)
        remaining = e.size
        while remaining:
            got = self._f.readinto(mv[:min(len(buf), remaining)])
            if not got:
                raise XiError("unexpected end of image while reading %s (%d bytes missing)" % (e.path, remaining))
            remaining -= got
            yield mv[:got]


# ================================================================================================================
# XEX2
# ================================================================================================================

class XexInfo(NamedTuple):
    module_flags: int
    pe_offset: int
    media_id: int
    version: int
    base_version: int
    title_id: int
    platform: int
    executable_type: int
    disc_number: int
    disc_count: int
    entry_point: Optional[int]
    image_base: Optional[int]


def parse_xex(head: bytes) -> XexInfo:
    """Parse the identity fields out of the first bytes of an XEX2 file (big-endian)."""
    if len(head) < 0x18:
        raise XexError("truncated-header")
    if head[:4] != XEX_MAGIC:
        raise XexError("bad-magic-" + head[:4].hex().upper())
    module_flags, pe_offset, _reserved, _security, count = struct.unpack_from(">5I", head, 4)
    if count > XEX_MAX_OPT_HEADERS or 0x18 + count * 8 > len(head):
        raise XexError("bad-optional-header-count-%d" % count)
    opts = {}
    for i in range(count):
        key, value = struct.unpack_from(">II", head, 0x18 + i * 8)
        opts[key] = value
    ei = opts.get(XEX_OPT_EXECUTION_INFO)
    if ei is None:
        raise XexError("no-execution-info")
    if ei + 24 > len(head):
        raise XexError("execution-info-outside-header")
    media_id, version, base_version, title_id = struct.unpack_from(">4I", head, ei)
    platform, exe_type, disc_number, disc_count = struct.unpack_from(">4B", head, ei + 16)
    return XexInfo(module_flags, pe_offset, media_id, version, base_version, title_id, platform, exe_type,
                   disc_number, disc_count, opts.get(XEX_OPT_ENTRY_POINT), opts.get(XEX_OPT_IMAGE_BASE))


def identity_failures(sha_hex: str, head: bytes) -> Tuple[Optional[XexInfo], List[Tuple[str, str, str]]]:
    """Compare against the pinned identity. Returns (parsed header or None, [(field, expected, got)])."""
    fails: List[Tuple[str, str, str]] = []
    if sha_hex != PIN_SHA256:
        fails.append(("sha256", PIN_SHA256, sha_hex))
    try:
        x = parse_xex(head)
    except XexError as e:
        fails.append(("xex", "XEX2", str(e)))
        return None, fails
    if x.title_id != PIN_TITLE_ID:
        fails.append(("title", "%08X" % PIN_TITLE_ID, "%08X" % x.title_id))
    if x.media_id != PIN_MEDIA_ID:
        fails.append(("media", "%08X" % PIN_MEDIA_ID, "%08X" % x.media_id))
    if x.version != PIN_VERSION:
        fails.append(("version", str(PIN_VERSION), str(x.version)))
    if x.base_version != PIN_BASE_VERSION:
        fails.append(("base_version", str(PIN_BASE_VERSION), str(x.base_version)))
    if (x.disc_number, x.disc_count) != (PIN_DISC_NUMBER, PIN_DISC_COUNT):
        fails.append(("disc", "%d/%d" % (PIN_DISC_NUMBER, PIN_DISC_COUNT), "%d/%d" % (x.disc_number, x.disc_count)))
    return x, fails


def hash_chunks(chunks: Iterator[memoryview]) -> Tuple[str, bytes]:
    """SHA-256 of the whole stream, plus its first XEX_HEAD_BYTES bytes."""
    h = hashlib.sha256()
    head = bytearray()
    for c in chunks:
        h.update(c)
        if len(head) < XEX_HEAD_BYTES:
            head += c[:XEX_HEAD_BYTES - len(head)]
    return h.hexdigest(), bytes(head)


def report_identity(sha_hex: str, head: bytes, verbose: bool) -> int:
    x, fails = identity_failures(sha_hex, head)
    if verbose and x is not None:
        info("xex: flags=0x%08X pe_offset=0x%X base_version=%d platform=%d executable_type=%d entry=%s image_base=%s"
             % (x.module_flags, x.pe_offset, x.base_version, x.platform, x.executable_type,
                "none" if x.entry_point is None else "0x%08X" % x.entry_point,
                "none" if x.image_base is None else "0x%08X" % x.image_base))
    if fails:
        for field, expected, got in fails:
            print("FAIL %s expected=%s got=%s" % (field, expected, got))
        return EXIT_MISMATCH
    assert x is not None
    print("OK %s sha256=%s title=%08X media=%08X version=%d disc=%d/%d"
          % (PIN_XEX_NAME, sha_hex, x.title_id, x.media_id, x.version, x.disc_number, x.disc_count))
    return EXIT_OK


# ================================================================================================================
# Commands
# ================================================================================================================

def game_dir(args: argparse.Namespace) -> str:
    return getattr(args, "game", None) or os.environ.get("MCLA_GAME") or DEFAULT_GAME


def resolve_iso(args: argparse.Namespace) -> str:
    if args.iso:
        return args.iso
    d = game_dir(args)
    try:
        isos = sorted(n for n in os.listdir(d) if n.lower().endswith(".iso"))
    except OSError as e:
        raise XiError("no <iso> given and the game directory is unreadable: %s" % e)
    if len(isos) != 1:
        raise XiError("no <iso> given and %s holds %d *.iso files (need exactly 1)" % (d, len(isos)))
    return os.path.join(d, isos[0])


def describe_open(img: Image, verbose: bool) -> None:
    if verbose:
        info("image: %s, %d bytes, layout %s, partition base 0x%08X, root sector %d, root size %d"
             % (img.path, img.image_size, img.layout, img.base, img.root_sector, img.root_size))


def cmd_verify(args: argparse.Namespace) -> int:
    with Image(resolve_iso(args)) as img:
        describe_open(img, args.verbose)
        entry = img.find_root_file(PIN_XEX_NAME)
        if entry is None:
            print("FAIL %s expected=present got=missing" % PIN_XEX_NAME)
            return EXIT_MISMATCH
        if args.verbose:
            info("%s: sector %d, %d bytes" % (entry.path, entry.sector, entry.size))
        sha_hex, head = hash_chunks(img.chunks(entry, bytearray(CHUNK)))
    return report_identity(sha_hex, head, args.verbose)


def cmd_verify_xex(args: argparse.Namespace) -> int:
    path = args.xex or os.path.join(game_dir(args), PIN_XEX_NAME)
    buf = bytearray(CHUNK)
    mv = memoryview(buf)
    h = hashlib.sha256()
    head = bytearray()
    with open(path, "rb", buffering=0) as f:
        while True:
            n = f.readinto(buf)
            if not n:
                break
            h.update(mv[:n])
            if len(head) < XEX_HEAD_BYTES:
                head += mv[:min(n, XEX_HEAD_BYTES - len(head))]
    return report_identity(h.hexdigest(), bytes(head), args.verbose)


def is_skipped(path: str) -> bool:
    low = path.lower()
    return low.startswith(SKIP_DIR_PREFIX) or low.endswith(SKIP_SUFFIX)


def cmd_list(args: argparse.Namespace) -> int:
    rc = EXIT_OK
    with Image(resolve_iso(args)) as img:
        describe_open(img, args.verbose)
        n = total = wanted_n = wanted_total = 0
        for e in img.files():
            print("%13d  %s" % (e.size, e.path))
            n += 1
            total += e.size
            if not is_skipped(e.path):
                wanted_n += 1
                wanted_total += e.size
            if img.end_of_file(e) > img.image_size:
                warn("%s extends past the end of the image (image incomplete?)" % e.path)
                rc = EXIT_ERROR
    print("%d files, %d bytes (extract: %d files, %d bytes)" % (n, total, wanted_n, wanted_total))
    return rc


_BAD_NAME_CHARS = set('<>:"|?*') | {chr(c) for c in range(32)}


def local_path(dest: str, rel: str) -> str:
    """Join a disc path onto dest, refusing anything that could escape it or that Windows cannot store."""
    parts = rel.split("/")
    for p in parts:
        if (not p or p in (".", "..") or p.endswith((" ", ".")) or any(c in _BAD_NAME_CHARS for c in p)):
            raise XiError("refusing to extract %r: component %r is not a safe file name" % (rel, p))
    return os.path.join(dest, *parts)


def human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0
    raise AssertionError


def write_entry(img: Image, e: Entry, target: str, buf: bytearray, label: str) -> None:
    part = target + ".part"
    os.makedirs(os.path.dirname(target), exist_ok=True)
    done = 0
    last = time.monotonic()
    try:
        with open(part, "wb", buffering=0) as out:
            for c in img.chunks(e, buf):
                out.write(c)  # raw FileIO: loop on partial writes
                done += len(c)
                now = time.monotonic()
                if now - last >= PROGRESS_SECONDS:
                    last = now
                    info("  %s %s: %s of %s (%d%%)" % (label, e.path, human(done), human(e.size),
                                                       100 * done // max(e.size, 1)))
        os.replace(part, target)
    except BaseException:
        try:
            os.remove(part)
        except OSError:
            pass
        raise


def cmd_extract(args: argparse.Namespace) -> int:
    dest = os.path.abspath(args.dest)
    with Image(args.iso) as img:
        describe_open(img, args.verbose)
        wanted = [e for e in img.files() if not is_skipped(e.path)]
        plan = []  # (entry, target, needs_write)
        to_write = 0
        for e in wanted:
            target = local_path(dest, e.path)
            if os.path.isdir(target):
                raise XiError("%s exists and is a directory" % target)
            stale = target + ".part"
            if os.path.exists(stale):
                os.remove(stale)
            same = (not args.force) and os.path.isfile(target) and os.path.getsize(target) == e.size
            plan.append((e, target, not same))
            if not same:
                to_write += e.size
        os.makedirs(dest, exist_ok=True)
        free = shutil.disk_usage(dest).free
        if free < to_write:
            raise XiError("not enough free space in %s: need %s, have %s" % (dest, human(to_write), human(free)))

        buf = bytearray(CHUNK)
        written = skipped = 0
        t0 = time.monotonic()
        for i, (e, target, needs_write) in enumerate(plan, 1):
            label = "[%d/%d]" % (i, len(plan))
            if not needs_write:
                info("%s skip %s (%d bytes, size matches)" % (label, e.path, e.size))
                skipped += 1
                continue
            info("%s extract %s (%d bytes)" % (label, e.path, e.size))
            write_entry(img, e, target, buf, label)
            written += 1
        elapsed = max(time.monotonic() - t0, 1e-9)
        info("wrote %d files (%s) in %.1f s, skipped %d already complete"
             % (written, human(to_write), elapsed, skipped))
    print("extracted %d files, %d bytes" % (len(plan), sum(e.size for e, _, _ in plan)))
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="xiso.py", description="Xbox 360 XDVDFS image tool (verify, list, extract).")
    sub = p.add_subparsers(dest="cmd", required=True, metavar="{verify,verify-xex,list,extract}")

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-v", "--verbose", action="store_true", help="print layout and header details to stderr")
    gamearg = argparse.ArgumentParser(add_help=False)
    gamearg.add_argument("--game", metavar="DIR", help="game directory used when the file argument is omitted "
                                                       "(default: $MCLA_GAME or %s)" % DEFAULT_GAME)

    s = sub.add_parser("verify", parents=[common, gamearg], help="check default.xex inside the ISO against the pin")
    s.add_argument("iso", nargs="?")
    s.set_defaults(func=cmd_verify)

    s = sub.add_parser("verify-xex", parents=[common, gamearg], help="same check for a loose default.xex")
    s.add_argument("xex", nargs="?")
    s.set_defaults(func=cmd_verify_xex)

    s = sub.add_parser("list", parents=[common, gamearg], help="list every file in the ISO")
    s.add_argument("iso", nargs="?")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("extract", parents=[common], help="extract the game files (no $SystemUpdate/, no *.pad)")
    s.add_argument("iso")
    s.add_argument("dest")
    s.add_argument("--force", action="store_true", help="rewrite files even when the size already matches")
    s.set_defaults(func=cmd_extract)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("xiso: interrupted", file=sys.stderr)
        return EXIT_INTERRUPTED
    except (XiError, OSError) as e:
        print("xiso: error: %s" % e, file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
