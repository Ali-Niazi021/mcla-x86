#!/usr/bin/env python3
"""XEX -> decrypted, decompressed PE image (the RE input), stdlib only.

    python tools/re/xex_image.py [XEX [OUT]] [--game DIR] [--work DIR] [--no-verify]

Defaults: XEX = $GAME/game/default.xex, else $GAME/default.xex; OUT = $WORK/re/default.pe.
($GAME: --game / env MCLA_GAME, default C:/Users/Ali/Development/dumps/mcla-x360.
 $WORK: --work / env MCLA_WORK, default C:/Users/Ali/Development/mcla-x86-work.)

It is what the SDK does when it loads the game (src/system/xex_module.cpp): decrypt the session key with the
retail key, decrypt the data with AES-128-CBC, then expand it (MCLA's XEX is "basic" compressed: data blocks and
zero fill, no LZX; the LZX path is kept for other XEXs). The image keeps the layout it has when loaded at
0x82000000: the guest address D is at byte D - 0x82000000.

Prints the SHA-256 of the XEX and of the image and the section table. With verification (the default) the XEX must
be the supported build (SHA-256 c386f400...a432) and the image must hash to 5a9de5cd...f319
(5a9de5cd05f32ae66d2a7fd3b09a569e18239ffa258bb1a0efb9126de1cff319, 10,354,688 bytes): otherwise it prints
`FAIL <what> expected=<x> got=<y>`, writes nothing and exits 2. On success the last line is
`OK pe sha256=<hex>`. Pure Python AES: about 4-5 s.

The image IS the game: it goes to $WORK/re/, never into the repo (.gitignore: *.pe).

Ported from the owner's iOS port (tools/ediciones/imagen.py).
"""

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_config  # noqa: E402

BASE = 0x82000000
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")


# ---------------------------------------------------------------------------
# AES-128, decrypt only (equivalent inverse cipher, with tables)
# ---------------------------------------------------------------------------

def _aes_tables():
    def xtime(a):
        a <<= 1
        return (a ^ 0x11B) & 0xFF if a & 0x100 else a

    def mul(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = xtime(a)
            b >>= 1
        return r

    # S-box: multiplicative inverse and affine transform
    s = [0] * 256
    p = q = 1
    while True:
        p = p ^ xtime(p)                       # p times 3
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09                          # q divided by 3
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        s[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    s[0] = 0x63
    si = [0] * 256
    for i, v in enumerate(s):
        si[v] = i
    td0 = [(mul(v, 14) << 24) | (mul(v, 9) << 16) | (mul(v, 13) << 8) | mul(v, 11) for v in si]
    td1 = [((t >> 8) | (t << 24)) & 0xFFFFFFFF for t in td0]
    td2 = [((t >> 16) | (t << 16)) & 0xFFFFFFFF for t in td0]
    td3 = [((t >> 24) | (t << 8)) & 0xFFFFFFFF for t in td0]
    return s, si, td0, td1, td2, td3


_S, _SI, _TD0, _TD1, _TD2, _TD3 = _aes_tables()


def _decrypt_keys(key):
    w = list(struct.unpack(">4I", key))
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (_S[t >> 24] << 24) | (_S[(t >> 16) & 255] << 16) | (_S[(t >> 8) & 255] << 8) | _S[t & 255]
            t ^= rcon << 24
            rcon = ((rcon << 1) ^ 0x11B) & 0xFF if rcon & 0x80 else rcon << 1
        w.append(w[i - 4] ^ t)
    # rounds in reverse order, with InvMixColumns on the middle ones
    rk = []
    for rnd in range(10, -1, -1):
        for x in w[4 * rnd:4 * rnd + 4]:
            if 0 < rnd < 10:
                x = (_TD0[_S[x >> 24]] ^ _TD1[_S[(x >> 16) & 255]] ^
                     _TD2[_S[(x >> 8) & 255]] ^ _TD3[_S[x & 255]])
            rk.append(x)
    return rk


def aes_cbc_decrypt(key, data):
    """AES-128-CBC with a zero IV. A tail that does not fill a block is left as it is."""
    rk = _decrypt_keys(key)
    td0, td1, td2, td3, si = _TD0, _TD1, _TD2, _TD3, _SI
    n = len(data) // 16
    words = struct.unpack(">%dI" % (n * 4), data[:n * 16])
    out_words = [0] * (n * 4)
    p0 = p1 = p2 = p3 = 0
    k0, k1, k2, k3 = rk[0:4]
    middle = [tuple(rk[4 * r:4 * r + 4]) for r in range(1, 10)]
    f0, f1, f2, f3 = rk[40:44]
    for b in range(0, n * 4, 4):
        c0, c1, c2, c3 = words[b:b + 4]
        s0, s1, s2, s3 = c0 ^ k0, c1 ^ k1, c2 ^ k2, c3 ^ k3
        for r0, r1, r2, r3 in middle:
            t0 = td0[s0 >> 24] ^ td1[(s3 >> 16) & 255] ^ td2[(s2 >> 8) & 255] ^ td3[s1 & 255] ^ r0
            t1 = td0[s1 >> 24] ^ td1[(s0 >> 16) & 255] ^ td2[(s3 >> 8) & 255] ^ td3[s2 & 255] ^ r1
            t2 = td0[s2 >> 24] ^ td1[(s1 >> 16) & 255] ^ td2[(s0 >> 8) & 255] ^ td3[s3 & 255] ^ r2
            t3 = td0[s3 >> 24] ^ td1[(s2 >> 16) & 255] ^ td2[(s1 >> 8) & 255] ^ td3[s0 & 255] ^ r3
            s0, s1, s2, s3 = t0, t1, t2, t3
        out_words[b] = ((si[s0 >> 24] << 24) | (si[(s3 >> 16) & 255] << 16) |
                        (si[(s2 >> 8) & 255] << 8) | si[s1 & 255]) ^ f0 ^ p0
        out_words[b + 1] = ((si[s1 >> 24] << 24) | (si[(s0 >> 16) & 255] << 16) |
                            (si[(s3 >> 8) & 255] << 8) | si[s2 & 255]) ^ f1 ^ p1
        out_words[b + 2] = ((si[s2 >> 24] << 24) | (si[(s1 >> 16) & 255] << 16) |
                            (si[(s0 >> 8) & 255] << 8) | si[s3 & 255]) ^ f2 ^ p2
        out_words[b + 3] = ((si[s3 >> 24] << 24) | (si[(s2 >> 16) & 255] << 16) |
                            (si[(s1 >> 8) & 255] << 8) | si[s0 & 255]) ^ f3 ^ p3
        p0, p1, p2, p3 = c0, c1, c2, c3
    return struct.pack(">%dI" % (n * 4), *out_words) + data[n * 16:]


# ---------------------------------------------------------------------------
# LZX (the .cab one), after the SDK's thirdparty/libmspack
# ---------------------------------------------------------------------------

_SLOTS = [30, 32, 34, 36, 38, 42, 50, 66, 98, 162, 290]   # by window bits, from 15
_FRAME = 32768


def _huffman_table(lengths, n, may_be_empty=False):
    """Direct decoding table: index = the next `max_len` bits."""
    max_len = max(lengths[:n])
    if max_len == 0:
        if may_be_empty:
            return None, 0
        raise ValueError("LZX: empty Huffman tree")
    table = [0] * (1 << max_len)
    pos = 0
    for length in range(1, max_len + 1):
        run = 1 << (max_len - length)
        for sym in range(n):
            if lengths[sym] == length:
                table[pos:pos + run] = [(sym << 5) | length] * run
                pos += run
    if pos != len(table):
        raise ValueError("LZX: incomplete Huffman tree")
    return table, max_len


def lzx_decompress(data, out_size, window_bits):
    if not 15 <= window_bits <= 25:
        raise ValueError("LZX: window of %d bits" % window_bits)
    n_main = 256 + (_SLOTS[window_bits - 15] << 3)
    extra_bits = [0, 0, 0, 0] + [min(i // 2 - 1, 17) for i in range(4, 290)]
    base_pos = [0]
    for i in range(1, 290):
        base_pos.append(base_pos[-1] + (1 << extra_bits[i - 1]))

    data = bytes(data) + bytes(64)
    out = bytearray(out_size)
    # bit stream state: little-endian 16-bit words, read from the high bit down
    pos = 0          # next unread byte
    buf = 0          # bits read and not yet consumed (the low nb)
    nb = 0

    def bits(n):
        nonlocal pos, buf, nb
        while nb < n:
            buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
            pos += 2
            nb += 16
        nb -= n
        return (buf >> nb) & ((1 << n) - 1)

    def symbol(table, max_len):
        nonlocal pos, buf, nb
        while nb < max_len:
            buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
            pos += 2
            nb += 16
        e = table[(buf >> (nb - max_len)) & ((1 << max_len) - 1)]
        nb -= e & 31
        return e >> 5

    def read_lengths(lengths, start, stop):
        pre = [bits(4) for _ in range(20)]
        table, max_len = _huffman_table(pre, 20)
        x = start
        while x < stop:
            z = symbol(table, max_len)
            if z == 17:
                y = bits(4) + 4
                value = 0
            elif z == 18:
                y = bits(5) + 20
                value = 0
            elif z == 19:
                y = bits(1) + 4
                value = (lengths[x] - symbol(table, max_len)) % 17
            else:
                y = 1
                value = (lengths[x] - z) % 17
            for _ in range(y):
                if x < len(lengths):
                    lengths[x] = value
                x += 1

    if bits(1):
        if (bits(16) << 16) | bits(16):
            # x86 call translation (E8). A PowerPC executable does not use it.
            raise ValueError("LZX: stream with x86 preprocessing, not supported")

    len_main = [0] * (n_main + 64)
    len_long = [0] * (249 + 64)
    len_aligned = [0] * 8
    t_main = t_long = t_aligned = None
    m_main = m_long = m_aligned = 0
    r0 = r1 = r2 = 1
    block_type = 0
    block_len = remaining = 0
    wp = 0

    while wp < out_size:
        frame_end = min(wp + _FRAME, out_size)
        while wp < frame_end:
            if remaining == 0:
                if block_type == 3 and block_len & 1:
                    pos += 1                       # padding after an odd-sized uncompressed block
                block_type = bits(3)
                block_len = remaining = (bits(16) << 8) | bits(8)
                if block_type in (1, 2):
                    if block_type == 2:
                        for i in range(8):
                            len_aligned[i] = bits(3)
                        t_aligned, m_aligned = _huffman_table(len_aligned, 8)
                    read_lengths(len_main, 0, 256)
                    read_lengths(len_main, 256, n_main)
                    t_main, m_main = _huffman_table(len_main, n_main)
                    read_lengths(len_long, 0, 249)
                    t_long, m_long = _huffman_table(len_long, 249, True)
                elif block_type == 3:
                    # align to 16 bits; if already aligned, skip a whole word
                    consumed = pos * 8 - nb
                    consumed += 16 - consumed % 16
                    pos, buf, nb = consumed // 8, 0, 0
                    r0, r1, r2 = struct.unpack_from("<3I", data, pos)
                    pos += 12
                else:
                    raise ValueError("LZX: block type %d" % block_type)

            span = min(remaining, frame_end - wp)
            remaining -= span
            end = wp + span
            if block_type == 3:
                out[wp:end] = data[pos:pos + span]
                pos += span
                wp = end
                continue

            aligned = block_type == 2
            mask = (1 << m_main) - 1
            while wp < end:
                if nb < 16:
                    buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
                    pos += 2
                    nb += 16
                e = t_main[(buf >> (nb - m_main)) & mask]
                nb -= e & 31
                e >>= 5
                if e < 256:
                    out[wp] = e
                    wp += 1
                    continue
                e -= 256
                length = e & 7
                if length == 7:
                    if t_long is None:
                        raise ValueError("LZX: the length tree is needed and it is empty")
                    if nb < 16:
                        buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
                        pos += 2
                        nb += 16
                    x = t_long[(buf >> (nb - m_long)) & ((1 << m_long) - 1)]
                    nb -= x & 31
                    length += x >> 5
                length += 2
                slot = e >> 3
                if slot == 0:
                    dist = r0
                elif slot == 1:
                    dist = r1
                    r1 = r0
                    r0 = dist
                elif slot == 2:
                    dist = r2
                    r2 = r0
                    r0 = dist
                else:
                    extra = extra_bits[slot]
                    dist = base_pos[slot] - 2
                    if extra >= 3 and aligned:
                        if extra > 3:
                            n = extra - 3
                            while nb < n:
                                buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
                                pos += 2
                                nb += 16
                            nb -= n
                            dist += ((buf >> nb) & ((1 << n) - 1)) << 3
                        if nb < 16:
                            buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
                            pos += 2
                            nb += 16
                        x = t_aligned[(buf >> (nb - m_aligned)) & ((1 << m_aligned) - 1)]
                        nb -= x & 31
                        dist += x >> 5
                    elif extra:
                        while nb < extra:
                            buf = ((buf & ((1 << nb) - 1)) << 16) | data[pos] | (data[pos + 1] << 8)
                            pos += 2
                            nb += 16
                        nb -= extra
                        dist += (buf >> nb) & ((1 << extra) - 1)
                    r2 = r1
                    r1 = r0
                    r0 = dist
                src = wp - dist
                if src < 0:
                    raise ValueError("LZX: copy from before the start")
                if dist >= length:
                    out[wp:wp + length] = out[src:src + length]
                else:
                    for i in range(length):
                        out[wp + i] = out[src + i]
                wp += length
            if wp > end:
                # the last copy ran past the span: it comes out of the rest of the block
                remaining -= wp - end
                if remaining < 0 or wp > frame_end:
                    raise ValueError("LZX: a copy runs out of the block")

        # every 32 KB frame starts on a 16-bit word
        if block_type != 3:
            consumed = pos * 8 - nb
            if consumed % 16:
                consumed += 16 - consumed % 16
            pos, buf, nb = consumed // 8, 0, 0
    return bytes(out[:out_size])


# ---------------------------------------------------------------------------
# XEX2
# ---------------------------------------------------------------------------

def _optional_headers(xex):
    n = struct.unpack_from(">I", xex, 0x14)[0]
    return dict(struct.unpack_from(">II", xex, 0x18 + i * 8) for i in range(n))


def image_from_xex(xex):
    """The image (bytes) of an XEX2 read whole into memory."""
    if xex[:4] != b"XEX2":
        raise ValueError("does not start with 'XEX2': not an Xbox 360 executable")
    data_off, sec_off = struct.unpack_from(">I", xex, 0x08)[0], struct.unpack_from(">I", xex, 0x10)[0]
    image_size = struct.unpack_from(">I", xex, sec_off + 0x004)[0]
    file_key = xex[sec_off + 0x150:sec_off + 0x160]
    ffi = _optional_headers(xex).get(0x000003FF)
    if ffi is None:
        raise ValueError("XEX without 'file format info'")
    info_size, encryption, compression = struct.unpack_from(">IHH", xex, ffi)

    data = xex[data_off:]
    if encryption == 1:
        session_key = aes_cbc_decrypt(RETAIL_KEY, file_key)
        data = aes_cbc_decrypt(session_key, data)
    elif encryption != 0:
        raise ValueError("unknown encryption %d" % encryption)

    if compression == 0:
        img = data[:image_size]
    elif compression == 1:
        # basic: pairs (data bytes, zero bytes)
        chunks, p = [], 0
        for i in range((info_size - 8) // 8):
            n_data, n_zeros = struct.unpack_from(">II", xex, ffi + 8 + i * 8)
            chunks.append(data[p:p + n_data])
            chunks.append(bytes(n_zeros))
            p += n_data
        img = b"".join(chunks)[:image_size]
    elif compression == 2:
        window, block_size = struct.unpack_from(">II", xex, ffi + 8)
        digest = xex[ffi + 16:ffi + 36]
        compressed, p = [], 0
        while block_size:
            block = data[p:p + block_size]
            if hashlib.sha1(block).digest() != digest:
                raise ValueError("a block digest does not match: damaged XEX, or encrypted with "
                                 "another key (a devkit XEX, for example)")
            next_size = struct.unpack_from(">I", block, 0)[0]
            digest = block[4:24]
            q = 24
            while True:
                n = (block[q] << 8) | block[q + 1]
                q += 2
                if not n:
                    break
                compressed.append(block[q:q + n])
                q += n
            p += block_size
            block_size = next_size
        img = lzx_decompress(b"".join(compressed), image_size, window.bit_length() - 1)
    else:
        raise ValueError("compression %d not supported (3 is a delta patch)" % compression)

    if img[:2] != b"MZ":
        raise ValueError("the decrypted data is not a PE: wrong key or format")
    return img.ljust(image_size, b"\0")


def sections(img):
    """[(name, start address, end address)] from the PE section table."""
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    n = struct.unpack_from("<H", img, pe + 6)[0]
    opt = struct.unpack_from("<H", img, pe + 20)[0]
    out = []
    for i in range(n):
        at = pe + 24 + opt + i * 40
        name = img[at:at + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr = struct.unpack_from("<II", img, at + 8)
        out.append((name, BASE + vaddr, BASE + vaddr + vsize))
    return out


def find_xex(game):
    for cand in (game / "game" / "default.xex", game / "default.xex"):
        if cand.is_file():
            return cand
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex", nargs="?", help="default.xex (default: $GAME/game/default.xex, else $GAME/default.xex)")
    ap.add_argument("out", nargs="?", help="image to write (default: $WORK/re/default.pe)")
    ap.add_argument("--game", metavar="DIR", help="$GAME, the game files (default: env MCLA_GAME, else %s)"
                    % game_config.DEFAULT_GAME)
    ap.add_argument("--work", metavar="DIR", help="$WORK (default: env MCLA_WORK, else %s)"
                    % game_config.DEFAULT_WORK)
    ap.add_argument("--no-verify", action="store_true",
                    help="do not require the supported build (XEX and image hashes); write whatever it decodes to")
    args = ap.parse_args(argv)

    if args.xex:
        xex_path = Path(args.xex)
    else:
        game = game_config.game_dir(args.game)
        xex_path = find_xex(game)
        if xex_path is None:
            print(f"error: no default.xex in {game / 'game'} or {game} (--game / MCLA_GAME, or pass the path)",
                  file=sys.stderr)
            return 2
    if not xex_path.is_file():
        print(f"error: {xex_path} does not exist", file=sys.stderr)
        return 2
    out_path = Path(args.out) if args.out else game_config.work_root(args.work) / "re" / "default.pe"

    xex = xex_path.read_bytes()
    xex_hash = hashlib.sha256(xex).hexdigest()
    print(f"XEX    {xex_path}")
    print(f"XEX    sha256 {xex_hash}")
    if not args.no_verify and xex_hash != game_config.XEX_SHA256:
        print(f"FAIL xex sha256 expected={game_config.XEX_SHA256} got={xex_hash}")
        return 2
    try:
        img = image_from_xex(xex)
    except ValueError as e:
        print(f"FAIL decode: {e}")
        return 2
    img_hash = hashlib.sha256(img).hexdigest()
    print(f"image  {len(img)} bytes, sha256 {img_hash}")
    for name, start, end in sections(img):
        print("  %-8s %08X - %08X" % (name, start, end))
    if not args.no_verify and img_hash != game_config.PE_SHA256:
        print(f"FAIL pe sha256 expected={game_config.PE_SHA256} got={img_hash}")
        return 2

    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_name(out_path.name + ".tmp")
    tmp.write_bytes(img)
    os.replace(tmp, out_path)
    print(f"wrote  {out_path}")
    print(f"OK pe sha256={img_hash}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
