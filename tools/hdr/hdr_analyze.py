#!/usr/bin/env python3
"""hdr_analyze.py - verify an HDR (or SDR) frame dump numerically, without eyes on an HDR screen.

    python tools/hdr/hdr_analyze.py FILE [FILE ...] [--out-dir DIR] [--no-images]
            [--colorspace hdr10|scrgb|sdr] [--paper-white NITS] [--peak NITS] [--min-nits NITS]
            [--sdr-eotf srgb|gamma22] [--sdr-white NITS]
            [--rect x,y,w,h]... [--ui-mask FILE] [--ui-layer FILE]
            [--band-rect x,y,w,h]... [--compare-sdr FILE]
    python tools/hdr/hdr_analyze.py --selftest

Input
  FILE is a DDS written by the game (DX10 extended header, no mips, one slice, rows tightly packed; a header pitch / linear
  size that is larger than width*bytes_per_pixel is honoured) or a plain PNG / BMP (8-bit SDR). DXGI formats:
      24 R10G10B10A2_UNORM     HDR10: PQ-encoded BT.2020 RGB, 10-bit codes (word bits R 0-9, G 10-19, B 20-29, A 30-31)
      10 R16G16B16A16_FLOAT    scRGB: linear BT.709, 1.0 = 80 nits, values may be negative or > 1
       2 R32G32B32A32_FLOAT    same meaning as 10
      28 R8G8B8A8_UNORM, 87 B8G8R8A8_UNORM   SDR, 8-bit display-referred
  Any other format (or a DDS without the DX10 header) is refused with a message.
  The optional sidecar <name>.json next to the input (same stem, .json) carries
      {"colorspace": "hdr10"|"scrgb"|"sdr", "paper_white_nits", "peak_nits", "min_nits", "frame", "t", "kind", "width", "height"}.
  Precedence per value: command-line flag, then sidecar, then default (colorspace: from the DXGI format; paper white 200;
  peak 1000). A colorspace that cannot hold the format (hdr10 in an 8-bit file, scrgb in an integer file) is refused.
  R10G10B10A2 with colorspace sdr is accepted (10-bit codes through the SDR EOTF).

Decoding (float64 throughout)
  hdr10: N = code/1023, PQ (SMPTE ST 2084) EOTF to nits per channel (BT.2020 linear), Y = 0.2627 R + 0.6780 G + 0.0593 B on the
         BT.2020 values (exact), then the inverse of the BT.709->BT.2020 matrix gives the BT.709 linear nits.
  scrgb: nits = value * 80 per channel (BT.709 linear); Y = 0.2126 R + 0.7152 G + 0.0722 B. NaN / Inf channels are counted and
         then decoded as 0.
  sdr:   --sdr-eotf srgb (piecewise, default) or gamma22 per channel, 1.0 = --sdr-white nits (default: paper white), Y with the
         BT.709 weights. "Y" below always means this luminance in nits; BT.709 linear "RGB" is the three channels in nits.
  Percentiles use numpy's linear interpolation. Everything is vectorised: a 3840x2160 frame takes a few seconds.

What is reported (printed as text lines and written as <name>.analysis.json)
  1 luminance   Y min, p0.1, p0.5, p1, p5, p25, p50, p75, p95, p99, p99.9, max, mean; histogram as fractions over the edges
                0 .001 .01 .05 .1 .5 1 2 5 10 20 50 100 150 200 300 400 500 700 1000 1500 2000 4000 10000 (23 bins, plus a 24th
                bin [10000, inf); JSON: edges (24 values), labels and fraction (24 each); Y < 0 falls in the first bin); pixels with Y > peak*1.01 (over_peak); for hdr10 / sdr the
                pixels with any / all channels at the maximum code (1023 / 255; null for scrgb).
  2 black       fraction of pixels with Y < 0.01 and < 0.1 nits; black floor = p0.5 of Y; RAISED_BLACKS when the floor is above
                0.05 nits and the image has dark content (p5 < 5 nits); NaN/Inf pixel count; pixels with a negative channel
                (float formats).
  3 gamut       fraction of pixels whose BT.709 linear has a channel below -0.002*Y - 1e-4 (all in nits: outside BT.709 = wide
                gamut content), the largest negative channel (nits) and the largest -channel/Y (pixels with Y >= 0.01); the
                pixels with Y > 10000 nits (what PQ cannot encode).
  4 UI white    With --rect (repeatable) and/or --ui-mask (PNG / DDS: its alpha where the alpha is not constant, else its
                non-zero pixels) and/or --ui-layer (a DDS / PNG of the UI layer: alpha > 0.5; premultiplied or straight does not
                matter; a size that differs from the frame is resized with nearest): every region and the union of them gets Y
                percentiles (min p5 p25 p50 p75 p95 p99 max) and the "white" UI luminance = the median Y of the pixels with
                saturation (max-min)/max < 0.1 (on BT.709 linear, negatives clamped to 0) that are in the top 20 % of the
                region by Y (Y >= its p80); white_ratio = that / paper white.
                Without any of them a HEURISTIC runs: pixels with saturation < 0.08, Y > 0.5 * paper white and a 3x3
                neighbourhood of identical values; the peak of their Y histogram at 1-nit resolution is
                ui_white_estimate_nits (the median of the pixels in that bin; the 3 strongest bins are listed). It is a
                heuristic: any big flat grey area (sky, a wall) competes with the UI.
  5 banding     For each region (--band-rect, repeatable; default the top 25 % rows = sky; plus always the full image) the
                encoded code values: the 10-bit PQ code for hdr10; for scrgb the 10-bit PQ code of the BT.2020 green resp. of Y;
                the 8-bit (or 10-bit) code for sdr. Along rows |diff| of adjacent green codes (and of luma codes: PQ code of Y
                for hdr10 / scrgb, BT.709-weighted code for sdr) where 0 < |diff| <= 16: histogram of steps 1..16, the median
                nonzero step, the fraction of steps that are 1 code. Plateau length = mean (and median) run length of equal green
                codes along rows in smooth places (runs whose pixels all have a centred 5-px window spanning <= 8 codes; runs
                touching the left / right 2 px of the region do not count). A smooth 10-bit PQ gradient has mostly 1-code steps;
                a gradient that went through an 8-bit SDR intermediate shows steps of ~3-6 PQ codes in the midtones. Flag
                BANDING_8BIT_UPSTREAM when a region (judged on green) has >= 2000 small steps and a median step >= 3. The full
                image scan includes textured content, so it can flag on a busy frame: look at the sky / --band-rect regions.
  6 images      <name>.preview.png: SDR rendering (x = BT.709 linear nits / paper white; identity up to 0.75, then a smooth
                Reinhard-type rolloff that reaches 1.0 at peak/paper white, applied to luminance with the chroma kept by
                scaling RGB by Y_out/Y_in; clip, sRGB, 8-bit). <name>.nits.png: false-colour luminance map (<0.1 black, <1 dark
                blue, <10 blue, <50 cyan, <0.9 paper white green, 0.9-1.1x paper white white, <500 yellow, <1000 orange,
                >=1000 red, > peak magenta) with a legend strip appended at the bottom. --no-images skips both.
  7 --compare-sdr FILE  an 8-bit SDR capture of the same frame (nearest-resized to the frame if the size differs), decoded with
                the SDR EOTF and 1.0 = paper white: per band of SDR relative luminance (0-0.05, 0.05-0.2, 0.2-0.4, 0.4-0.6,
                0.6-0.8, 0.8-0.95, 0.95-1.0) the median / p10 / p90 of HDR_Y / SDR_Y and the fraction of pixels where HDR is
                darker than SDR by > 10 % (ratio < 0.9). Pixels with SDR relative luminance < 0.001 are left out (ratio
                undefined near black). Ideally the ratio is ~1.0 up to ~0.6.
  8 summary     one line per file, printed last (all inputs analysed first):
                  hdr-analyze <name>: peak=<max nits> p99=.. p50=.. black_p0.5=.. ui_white=.. banding_step=.. gamut_out=..% flags=[...]
                (ui_white n/a when nothing was found; banding_step = the largest median step over the regions that have >= 2000
                small steps, n/a if none).

Flags: RAISED_BLACKS, BANDING_8BIT_UPSTREAM, OVER_PEAK (>= 0.01 % of pixels above peak*1.01), CLIPPED_CODES (hdr10 only: >= 0.1 %
of pixels with a channel at code 1023), NONFINITE (NaN / Inf in the file), ABOVE_PQ_RANGE (Y > 10000 nits somewhere).

--selftest  synthesises HDR10 / 8-bit-intermediate / scRGB gradients in a temp dir, analyses them and asserts the numbers above
            (PASS / FAIL per assert; exit 1 on any FAIL). write_dds() in this file is the DDS layout the game writes.
Exit code: 0; 2 when an input (or --ui-mask / --ui-layer / --compare-sdr) cannot be read; --selftest: 1 on a FAIL.
Output files go next to the input, or into --out-dir: <name>.analysis.json, <name>.preview.png, <name>.nits.png (name = file stem).
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import math
import struct
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# --------------------------------------------------------------------------------------------------------------------
# constants

PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 4096.0 * 128.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0

M709_TO_2020 = np.array([[0.6274, 0.3293, 0.0433],
                         [0.0691, 0.9195, 0.0114],
                         [0.0164, 0.0880, 0.8956]], dtype=np.float64)
M2020_TO_709 = np.linalg.inv(M709_TO_2020)
W709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
W2020 = np.array([0.2627, 0.6780, 0.0593], dtype=np.float64)
SCRGB_NITS_PER_UNIT = 80.0

# dxgi -> (name, bytes per pixel, storage kind)
DXGI_FORMATS = {
    2: ("R32G32B32A32_FLOAT", 16, "f32"),
    10: ("R16G16B16A16_FLOAT", 8, "f16"),
    24: ("R10G10B10A2_UNORM", 4, "u10"),
    28: ("R8G8B8A8_UNORM", 4, "u8"),
    87: ("B8G8R8A8_UNORM", 4, "u8"),
}
DDS_FLAGS = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8      # CAPS | HEIGHT | WIDTH | PIXELFORMAT | PITCH

HIST_EDGES = np.array([0, 0.001, 0.01, 0.05, 0.1, 0.5, 1, 2, 5, 10, 20, 50, 100, 150, 200, 300, 400, 500, 700, 1000, 1500,
                       2000, 4000, 10000], dtype=np.float64)
LUM_PERCENTILES = [0.1, 0.5, 1, 5, 25, 50, 75, 95, 99, 99.9]
UI_PERCENTILES = [5, 25, 50, 75, 95, 99]
SDR_COMPARE_BANDS = [0.0, 0.05, 0.2, 0.4, 0.6, 0.8, 0.95, 1.0]
PREVIEW_KNEE = 0.75

DEFAULT_PAPER_WHITE = 200.0
DEFAULT_PEAK = 1000.0
BANDING_MIN_STEPS = 2000
BANDING_MEDIAN_FLAG = 3
RAISED_BLACK_NITS = 0.05
OVER_PEAK_FLAG_FRACTION = 1e-4
CLIPPED_FLAG_FRACTION = 1e-3


class InputError(Exception):
    """A file that cannot be read or a request that cannot be honoured; main() turns it into exit code 2."""


# --------------------------------------------------------------------------------------------------------------------
# transfer functions

def pq_encode(nits):
    """Inverse PQ EOTF: nits (0..10000) -> N in 0..1."""
    y = np.clip(np.asarray(nits, dtype=np.float64), 0.0, 10000.0) / 10000.0
    yp = np.power(y, PQ_M1)
    return np.power((PQ_C1 + PQ_C2 * yp) / (1.0 + PQ_C3 * yp), PQ_M2)


def pq_decode(n):
    """PQ EOTF: N in 0..1 -> nits."""
    n = np.clip(np.asarray(n, dtype=np.float64), 0.0, 1.0)
    p = np.power(n, 1.0 / PQ_M2)
    return 10000.0 * np.power(np.maximum(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1)


HDR10_LUT = pq_decode(np.arange(1024) / 1023.0)                         # nits per 10-bit code
PQ_BOUNDS = pq_decode((np.arange(1023) + 0.5) / 1023.0)                 # nits between code k and k+1


def pq_code10(nits):
    """The 10-bit PQ code of nits (same as rint(pq_encode(nits)*1023), by a search on the code boundaries: faster)."""
    return np.searchsorted(PQ_BOUNDS, nits, side="right").astype(np.int16)


def srgb_eotf(v):
    v = np.asarray(v, dtype=np.float64)
    return np.where(v <= 0.04045, v / 12.92, np.power((np.maximum(v, 0.0) + 0.055) / 1.055, 2.4))


def srgb_oetf(lin):
    lin = np.clip(np.asarray(lin, dtype=np.float64), 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1.0 / 2.4) - 0.055)


def sdr_eotf(v, kind: str):
    if kind == "gamma22":
        return np.power(np.clip(np.asarray(v, dtype=np.float64), 0.0, 1.0), 2.2)
    return srgb_eotf(v)


SRGB8_LUT = np.rint(srgb_oetf(np.arange(65536) / 65535.0) * 255.0).astype(np.uint8)


def cmin(a):
    """Per-pixel minimum over the last axis of 3 (faster than a.min(axis=-1) for big images)."""
    return np.minimum(np.minimum(a[..., 0], a[..., 1]), a[..., 2])


def cmax(a):
    return np.maximum(np.maximum(a[..., 0], a[..., 1]), a[..., 2])


def any3(m):
    return m[..., 0] | m[..., 1] | m[..., 2]


def all3(m):
    return m[..., 0] & m[..., 1] & m[..., 2]


# --------------------------------------------------------------------------------------------------------------------
# DDS / image IO

def write_dds(path, dxgi: int, pixels, row_pitch: int | None = None) -> None:
    """Write a DX10 DDS: 'DDS ', 124-byte header (flags CAPS|HEIGHT|WIDTH|PIXELFORMAT|PITCH, pitch = row bytes, ddspf size 32,
    flags FOURCC, fourcc 'DX10', caps TEXTURE), 20-byte DXT10 header (TEXTURE2D, 1 slice), tightly packed rows.
    pixels: dxgi 24 -> uint32 (H, W) words (R bits 0-9, G 10-19, B 20-29, A 30-31); 10 -> float16 (H, W, 4);
    2 -> float32 (H, W, 4); 28 / 87 -> uint8 (H, W, 4) in the format's memory order. row_pitch pads the rows (tests)."""
    if dxgi not in DXGI_FORMATS:
        raise InputError("write_dds: unsupported DXGI format %d" % dxgi)
    name, bpp, storage = DXGI_FORMATS[dxgi]
    dtype = {"u10": "<u4", "u8": "u1", "f16": "<f2", "f32": "<f4"}[storage]
    arr = np.ascontiguousarray(pixels, dtype=np.dtype(dtype))
    want_dims = 2 if storage == "u10" else 3
    if arr.ndim != want_dims or (want_dims == 3 and arr.shape[2] != 4):
        raise InputError("write_dds: %s wants an array of shape %s" % (name, "(H, W)" if want_dims == 2 else "(H, W, 4)"))
    h, w = arr.shape[:2]
    row_bytes = w * bpp
    rows = arr.reshape(h, -1).view(np.uint8).reshape(h, row_bytes)
    pitch = row_bytes if row_pitch is None else int(row_pitch)
    if pitch < row_bytes:
        raise InputError("write_dds: row_pitch smaller than a row")
    if pitch > row_bytes:
        padded = np.zeros((h, pitch), dtype=np.uint8)
        padded[:, :row_bytes] = rows
        rows = padded
    header = [124, DDS_FLAGS, h, w, pitch, 0, 1] + [0] * 11
    header += [32, 0x4, struct.unpack("<I", b"DX10")[0], 0, 0, 0, 0, 0]
    header += [0x1000, 0, 0, 0, 0]
    dxt10 = [dxgi, 3, 0, 1, 0]
    blob = b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", *dxt10) + rows.tobytes()
    Path(path).write_bytes(blob)


def read_dds(path) -> dict:
    data = Path(path).read_bytes()
    if len(data) < 148 or data[:4] != b"DDS ":
        raise InputError("not a DX10 DDS file (bad magic or too short)")
    size, flags, height, width, pitch_or_linear = struct.unpack_from("<5I", data, 4)
    if size != 124:
        raise InputError("DDS_HEADER size is %d, not 124" % size)
    pf_flags, fourcc = struct.unpack_from("<I4s", data, 80)
    if not (pf_flags & 0x4) or fourcc != b"DX10":
        raise InputError("DDS has no DX10 extended header (fourCC %r): only DX10 DDS files are supported" % fourcc)
    dxgi, res_dim, _misc, array_size, _misc2 = struct.unpack_from("<5I", data, 128)
    if dxgi not in DXGI_FORMATS:
        raise InputError("unsupported DXGI format %d (supported: %s)" % (
            dxgi, ", ".join("%d %s" % (k, v[0]) for k, v in sorted(DXGI_FORMATS.items()))))
    if res_dim != 3:
        raise InputError("DDS resourceDimension is %d, only 3 (TEXTURE2D) is supported" % res_dim)
    if width < 1 or height < 1 or width > 65536 or height > 65536:
        raise InputError("DDS size %dx%d is not plausible" % (width, height))
    name, bpp, storage = DXGI_FORMATS[dxgi]
    row_bytes = width * bpp
    offset = 148
    pitch = row_bytes
    if (flags & 0x8) and pitch_or_linear >= row_bytes:
        pitch = pitch_or_linear
    elif (flags & 0x80000) and pitch_or_linear // height >= row_bytes:
        pitch = pitch_or_linear // height
    need = (height - 1) * pitch + row_bytes
    if len(data) - offset < need and pitch != row_bytes:
        pitch = row_bytes
        need = (height - 1) * pitch + row_bytes
    if len(data) - offset < need:
        raise InputError("DDS is truncated: %d pixel bytes present, %d needed for %dx%d %s" % (
            len(data) - offset, need, width, height, name))
    rows = np.ndarray((height, row_bytes), dtype=np.uint8, buffer=data, offset=offset, strides=(pitch, 1))
    rows = np.ascontiguousarray(rows)
    if storage == "u10":
        words = rows.view("<u4").reshape(height, width)
        px = np.empty((height, width, 4), dtype=np.uint16)
        px[..., 0] = words & 0x3FF
        px[..., 1] = (words >> 10) & 0x3FF
        px[..., 2] = (words >> 20) & 0x3FF
        px[..., 3] = words >> 30
    elif storage == "f16":
        px = rows.view("<f2").reshape(height, width, 4)
    elif storage == "f32":
        px = rows.view("<f4").reshape(height, width, 4)
    else:
        px = rows.reshape(height, width, 4)
        if dxgi == 87:
            px = px[..., [2, 1, 0, 3]]
    notes = []
    if array_size > 1:
        notes.append("DDS has %d array slices; only the first is analysed" % array_size)
    return {"path": str(path), "width": width, "height": height, "dxgi": dxgi, "format_name": name,
            "storage": storage, "px": px, "notes": notes}


def read_pil(path) -> dict:
    with Image.open(path) as im:
        im = im.convert("RGBA")
        px = np.asarray(im).copy()
    h, w = px.shape[:2]
    return {"path": str(path), "width": w, "height": h, "dxgi": None, "format_name": "8-bit RGBA (PNG/BMP)",
            "storage": "u8", "px": px, "notes": []}


def load_image(path) -> dict:
    p = Path(path)
    try:
        if p.suffix.lower() == ".dds":
            return read_dds(p)
        return read_pil(p)
    except InputError:
        raise
    except (OSError, ValueError, struct.error) as e:
        raise InputError("%s" % e)


def unit_rgba(frame: dict):
    """The frame as float32 (H, W, 4) with RGB and alpha scaled to 0..1 for the integer formats (floats as they are)."""
    px, st = frame["px"], frame["storage"]
    if st == "u8":
        return px.astype(np.float32) / 255.0
    if st == "u10":
        out = px.astype(np.float32)
        out[..., :3] /= 1023.0
        out[..., 3] /= 3.0
        return out
    return px.astype(np.float32)


def fit_mask(mask, h: int, w: int):
    if mask.shape == (h, w):
        return mask
    im = Image.fromarray(mask.astype(np.uint8) * 255).resize((w, h), Image.NEAREST)
    return np.asarray(im) > 0


def mask_from_mask_image(frame: dict):
    a = unit_rgba(frame)
    alpha = a[..., 3]
    if float(alpha.max()) > float(alpha.min()):
        return alpha > 0
    return (a[..., :3] != 0).any(axis=2)


def mask_from_layer(frame: dict):
    return unit_rgba(frame)[..., 3] > 0.5


# --------------------------------------------------------------------------------------------------------------------
# metadata and decoding

def load_sidecar(path):
    side_path = Path(path).with_suffix(".json")
    if not side_path.is_file():
        return {}, None
    try:
        data = json.loads(side_path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("not a JSON object")
        return data, str(side_path)
    except (OSError, ValueError) as e:
        print("hdr-analyze: warning: ignoring sidecar %s (%s)" % (side_path, e), file=sys.stderr)
        return {}, None


def pick(*values):
    for v in values:
        if v is not None:
            return v
    return None


def resolve_meta(path, frame: dict, args) -> dict:
    side, side_path = load_sidecar(path)
    st = frame["storage"]
    inferred = {"u10": "hdr10", "f16": "scrgb", "f32": "scrgb", "u8": "sdr"}[st]
    cs = pick(args.colorspace, side.get("colorspace"), inferred)
    allowed = {"u10": ("hdr10", "sdr"), "f16": ("scrgb",), "f32": ("scrgb",), "u8": ("sdr",)}[st]
    if cs not in allowed:
        raise InputError("colorspace %r cannot be decoded from %s (allowed: %s)" % (cs, frame["format_name"], "/".join(allowed)))
    pw = float(pick(args.paper_white, side.get("paper_white_nits"), DEFAULT_PAPER_WHITE))
    peak = float(pick(args.peak, side.get("peak_nits"), DEFAULT_PEAK))
    min_nits = pick(args.min_nits, side.get("min_nits"))
    if pw <= 0 or peak <= 0:
        raise InputError("paper white and peak must be positive (got %g, %g)" % (pw, peak))
    sdr_white = float(pick(args.sdr_white, pw))
    notes = []
    sw, sh = side.get("width"), side.get("height")
    if (sw is not None and sw != frame["width"]) or (sh is not None and sh != frame["height"]):
        notes.append("sidecar says %sx%s but the file is %dx%d" % (sw, sh, frame["width"], frame["height"]))
    return {"colorspace": cs, "paper_white_nits": pw, "peak_nits": peak,
            "min_nits": None if min_nits is None else float(min_nits),
            "frame": side.get("frame"), "t": side.get("t"), "kind": side.get("kind"),
            "sdr_eotf": args.sdr_eotf, "sdr_white_nits": sdr_white, "sidecar": side_path, "notes": notes}


def decode(frame: dict, meta: dict) -> dict:
    """Decode to BT.709 linear nits (rgb, float64 H x W x 3) and luminance (y, float64 H x W), plus per-format counters."""
    px, st, cs = frame["px"], frame["storage"], meta["colorspace"]
    dec = {"max_code_any": None, "max_code_all": None, "nonfinite_pixels": 0, "nonfinite_samples": 0, "negative_pixels": 0}
    if st in ("f16", "f32"):
        rgb = px[..., :3].astype(np.float64)
        bad = ~np.isfinite(rgb)
        n_bad = int(bad.sum())
        if n_bad:
            dec["nonfinite_samples"] = n_bad
            dec["nonfinite_pixels"] = int(any3(bad).sum())
            rgb[bad] = 0.0
        rgb *= SCRGB_NITS_PER_UNIT
        dec["negative_pixels"] = int(any3(rgb < 0).sum())
        y = rgb @ W709
    else:
        codes = px[..., :3]
        code_max = 1023 if st == "u10" else 255
        at_max = codes == code_max
        dec["max_code_any"] = int(any3(at_max).sum())
        dec["max_code_all"] = int(all3(at_max).sum())
        if cs == "hdr10":
            lin2020 = HDR10_LUT[codes]
            y = lin2020 @ W2020
            rgb = lin2020 @ M2020_TO_709.T
        else:
            lut = sdr_eotf(np.arange(code_max + 1) / code_max, meta["sdr_eotf"]) * meta["sdr_white_nits"]
            rgb = lut[codes]
            y = rgb @ W709
    dec["rgb"], dec["y"] = rgb, y
    return dec


def banding_codes(frame: dict, meta: dict, dec: dict):
    """(green, luma) int16 code planes in the encoded domain: 10-bit PQ for the HDR formats, 8/10-bit for sdr."""
    px, cs = frame["px"], meta["colorspace"]
    if cs == "hdr10":
        return px[..., 1].astype(np.int16), pq_code10(dec["y"])
    if cs == "scrgb":
        green2020 = dec["rgb"] @ M709_TO_2020[1]
        return pq_code10(green2020), pq_code10(dec["y"])
    c = px[..., :3].astype(np.int32)
    luma = (c[..., 0] * 2126 + c[..., 1] * 7152 + c[..., 2] * 722 + 5000) // 10000
    return px[..., 1].astype(np.int16), luma.astype(np.int16)


# --------------------------------------------------------------------------------------------------------------------
# analysis sections

def saturation(rgb):
    """(max - min) / max on BT.709 linear, negatives clamped to 0 (0 where max <= 0). rgb: (..., 3)."""
    mx = cmax(rgb)
    mn = np.maximum(cmin(rgb), 0.0)
    return np.where(mx > 0, (mx - mn) / np.where(mx > 0, mx, 1.0), 0.0)


def hist_label(i: int) -> str:
    if i == len(HIST_EDGES) - 1:
        return "[%g,inf)" % HIST_EDGES[i]
    return "[%g,%g)" % (HIST_EDGES[i], HIST_EDGES[i + 1])


def luminance_section(dec: dict, meta: dict) -> dict:
    y = dec["y"].ravel()
    n = y.size
    pcts = np.percentile(y, LUM_PERCENTILES)
    stats = {"min": float(y.min())}
    stats.update({"p%g" % p: float(v) for p, v in zip(LUM_PERCENTILES, pcts)})
    stats["max"] = float(y.max())
    stats["mean"] = float(y.mean())
    idx = np.searchsorted(HIST_EDGES, y, side="right") - 1
    np.clip(idx, 0, len(HIST_EDGES) - 1, out=idx)
    counts = np.bincount(idx, minlength=len(HIST_EDGES))
    over_thr = meta["peak_nits"] * 1.01
    over = int((y > over_thr).sum())
    out = {"stats": stats,
           "histogram": {"edges": HIST_EDGES.tolist(), "labels": [hist_label(i) for i in range(len(HIST_EDGES))],
                         "fraction": (counts / n).tolist()},
           "over_peak_threshold_nits": over_thr, "over_peak_pixels": over, "over_peak_fraction": over / n,
           "max_code_any_pixels": dec["max_code_any"], "max_code_all_pixels": dec["max_code_all"],
           "max_code_any_fraction": None if dec["max_code_any"] is None else dec["max_code_any"] / n,
           "pixels": n}
    return out


def black_section(dec: dict, lum: dict) -> dict:
    y = dec["y"]
    n = y.size
    st = lum["stats"]
    floor = st["p0.5"]
    return {"fraction_below_0.01_nits": float((y < 0.01).sum() / n), "fraction_below_0.1_nits": float((y < 0.1).sum() / n),
            "black_floor_nits": floor, "raised_blacks": bool(floor > RAISED_BLACK_NITS and st["p5"] < 5.0),
            "nonfinite_pixels": dec["nonfinite_pixels"], "nonfinite_samples": dec["nonfinite_samples"],
            "negative_pixels": dec["negative_pixels"]}


def gamut_section(dec: dict) -> dict:
    rgb, y = dec["rgb"], dec["y"]
    n = y.size
    minc = cmin(rgb)
    out = minc < (-0.002 * np.maximum(y, 0.0) - 1e-4)
    count = int(out.sum())
    excursion = float(-minc[out].min()) if count else 0.0
    rel_mask = out & (y >= 0.01)
    rel = float((-minc[rel_mask] / y[rel_mask]).max()) if rel_mask.any() else 0.0
    above = int((y > 10000.0).sum())
    return {"out_of_bt709_pixels": count, "out_of_bt709_fraction": count / n, "max_excursion_nits": excursion,
            "max_excursion_relative_to_y": rel, "above_pq_range_pixels": above, "above_pq_range_fraction": above / n}


def ui_region_stats(rgbv, yv, paper_white: float) -> dict:
    n = int(yv.size)
    if n == 0:
        return {"pixels": 0, "y_nits": None, "white_pixels": 0, "white_nits": None, "white_ratio": None}
    p = np.percentile(yv, UI_PERCENTILES)
    ypct = {"min": float(yv.min())}
    ypct.update({"p%g" % q: float(v) for q, v in zip(UI_PERCENTILES, p)})
    ypct["max"] = float(yv.max())
    thr = float(np.percentile(yv, 80))
    white = (saturation(rgbv) < 0.1) & (yv >= thr)
    wy = yv[white]
    wn = int(wy.size)
    out = {"pixels": n, "y_nits": ypct, "white_pixels": wn, "white_nits": None, "white_nits_p5": None, "white_nits_p95": None,
           "white_ratio": None}
    if wn:
        med = float(np.median(wy))
        out.update({"white_nits": med, "white_nits_p5": float(np.percentile(wy, 5)), "white_nits_p95": float(np.percentile(wy, 95)),
                    "white_ratio": med / paper_white})
    return out


def ui_heuristic(dec: dict, paper_white: float) -> dict:
    rgb, y = dec["rgb"], dec["y"]
    h, w = y.shape
    mx = cmax(rgb)
    mn = np.maximum(cmin(rgb), 0.0)
    cand = (mx > 0) & ((mx - mn) < 0.08 * mx) & (y > 0.5 * paper_white) & (y <= 10000.0)
    cand[0, :] = cand[-1, :] = False
    cand[:, 0] = cand[:, -1] = False
    idx = np.flatnonzero(cand)
    flat, yflat = rgb.reshape(-1, 3), y.ravel()
    offsets = [dy * w + dx for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx]
    for o in offsets:                   # equal Y is necessary for equal RGB: a cheap first pass on one channel
        idx = idx[yflat[idx + o] == yflat[idx]]
    for o in offsets:
        idx = idx[all3(flat[idx + o] == flat[idx])]
    out = {"method": "heuristic", "heuristic": True, "candidate_pixels": int(idx.size), "ui_white_estimate_nits": None,
           "modes": [],
           "note": "heuristic: flat near-achromatic bright plateaus (3x3 identical, saturation < 0.08, Y > 0.5 x paper white); "
                   "big flat grey areas other than the UI compete"}
    if idx.size:
        yv = y.ravel()[idx]
        bins = np.floor(yv).astype(np.int64)
        counts = np.bincount(bins)
        order = np.argsort(counts)[::-1][:3]
        for b in order:
            if counts[b] == 0:
                continue
            out["modes"].append({"nits": float(np.median(yv[bins == b])), "pixels": int(counts[b])})
        out["ui_white_estimate_nits"] = out["modes"][0]["nits"]
        out["white_ratio"] = out["ui_white_estimate_nits"] / paper_white
    out["ui_white_nits"] = out["ui_white_estimate_nits"]
    return out


def clamp_rect(rect, w: int, h: int):
    x, y, rw, rh = rect
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + rw, w), min(y + rh, h)
    if x1 <= x0 or y1 <= y0:
        return None
    return x0, y0, x1, y1


def ui_section(dec: dict, meta: dict, args, shared: dict) -> dict:
    y = dec["y"]
    h, w = y.shape
    pw = meta["paper_white_nits"]
    regions = []        # (label, source, rect-or-mask)
    for r in args.rect or []:
        c = clamp_rect(r, w, h)
        label = "rect %d,%d,%d,%d" % r
        if c is None:
            raise InputError("%s lies outside the %dx%d frame" % (label, w, h))
        regions.append((label, c, None))
    if shared["ui_mask"] is not None:
        regions.append(("ui-mask", None, fit_mask(mask_from_mask_image(shared["ui_mask"]), h, w)))
    if shared["ui_layer"] is not None:
        regions.append(("ui-layer", None, fit_mask(mask_from_layer(shared["ui_layer"]), h, w)))
    if not regions:
        return ui_heuristic(dec, pw)

    def gather(rect, mask):
        if rect is not None:
            x0, y0, x1, y1 = rect
            return dec["rgb"][y0:y1, x0:x1].reshape(-1, 3), y[y0:y1, x0:x1].ravel()
        return dec["rgb"][mask], y[mask]

    out_regions, union = [], np.zeros((h, w), dtype=bool)
    for label, rect, mask in regions:
        rgbv, yv = gather(rect, mask)
        st = ui_region_stats(rgbv, yv, pw)
        st["name"] = label
        out_regions.append(st)
        if rect is not None:
            union[rect[1]:rect[3], rect[0]:rect[2]] = True
        else:
            union |= mask
    if len(regions) == 1:
        overall = dict(out_regions[0])
    else:
        rgbv, yv = dec["rgb"][union], y[union]
        overall = ui_region_stats(rgbv, yv, pw)
    overall["name"] = "all regions"
    return {"method": "mask", "heuristic": False, "regions": out_regions, "overall": overall,
            "ui_white_nits": overall["white_nits"], "white_ratio": overall["white_ratio"]}


def hist_median(hist, values):
    """Median of a distribution given as counts per value (lower/upper middle averaged)."""
    n = int(hist.sum())
    if n == 0:
        return None
    cum = np.cumsum(hist)
    lo = values[int(np.searchsorted(cum, (n - 1) // 2, side="right"))]
    hi = values[int(np.searchsorted(cum, n // 2, side="right"))]
    return float(lo + hi) / 2.0


def step_stats(codes) -> dict:
    d = np.abs(np.diff(codes, axis=1))
    small = d[(d > 0) & (d <= 16)]
    hist = np.bincount(small, minlength=17)[1:17]
    n = int(hist.sum())
    return {"small_steps": n, "hist_1_to_16": hist.tolist(), "median_step": hist_median(hist, np.arange(1, 17)),
            "fraction_step_1": (float(hist[0]) / n) if n else None}


def plateau_stats(codes) -> dict:
    h, w = codes.shape
    if w < 8:
        return {"runs": 0, "mean": None, "median": None}
    mx = mn = codes[:, :w - 4]
    for k in range(1, 5):
        seg = codes[:, k:w - 4 + k]
        mx = np.maximum(mx, seg)
        mn = np.minimum(mn, seg)
    smooth = np.zeros((h, w), dtype=bool)
    smooth[:, 2:w - 2] = (mx - mn) <= 8
    starts = np.ones((h, w), dtype=bool)
    starts[:, 1:] = codes[:, 1:] != codes[:, :-1]
    s_idx = np.flatnonzero(starts.ravel())
    lengths = np.diff(np.append(s_idx, h * w))
    cs = np.concatenate(([0], np.cumsum(smooth.ravel(), dtype=np.int64)))
    in_smooth = (cs[s_idx + lengths] - cs[s_idx]) == lengths
    sel = lengths[in_smooth]
    if sel.size == 0:
        return {"runs": 0, "mean": None, "median": None}
    return {"runs": int(sel.size), "mean": float(sel.mean()), "median": float(np.median(sel))}


BANDING_TEXT = ("a smooth 10-bit PQ gradient has mostly 1-code steps; a gradient that went through an 8-bit SDR intermediate "
                "shows steps of ~3-6 PQ codes in the midtones (BANDING_8BIT_UPSTREAM: a region with >= %d small steps and a "
                "median step >= %d)" % (BANDING_MIN_STEPS, BANDING_MEDIAN_FLAG))


def banding_section(frame: dict, meta: dict, dec: dict, args) -> dict:
    h, w = dec["y"].shape
    regions = []
    if args.band_rect:
        for r in args.band_rect:
            c = clamp_rect(r, w, h)
            if c is None:
                raise InputError("band-rect %d,%d,%d,%d lies outside the %dx%d frame" % (r + (w, h)))
            regions.append(("band-rect %d,%d,%d,%d" % r, c))
    else:
        regions.append(("sky (top 25% rows)", (0, 0, w, max(1, h // 4))))
    regions.append(("full image", (0, 0, w, h)))
    green, luma = banding_codes(frame, meta, dec)
    out, medians, flagged = [], [], []
    for name, (x0, y0, x1, y1) in regions:
        g, lu = green[y0:y1, x0:x1], luma[y0:y1, x0:x1]
        gs, ls, pl = step_stats(g), step_stats(lu), plateau_stats(g)
        flag = bool(gs["small_steps"] >= BANDING_MIN_STEPS and gs["median_step"] is not None
                    and gs["median_step"] >= BANDING_MEDIAN_FLAG)
        if gs["small_steps"] >= BANDING_MIN_STEPS and gs["median_step"] is not None:
            medians.append(gs["median_step"])
        if flag:
            flagged.append(name)
        out.append({"name": name, "rect": [x0, y0, x1 - x0, y1 - y0], "green": gs, "luma": ls, "plateau": pl, "flag": flag})
    return {"regions": out, "step_median": max(medians) if medians else None, "flagged_regions": flagged,
            "flag": bool(flagged), "interpretation": BANDING_TEXT}


def compare_section(dec: dict, meta: dict, sdr_frame: dict):
    if sdr_frame["storage"] != "u8":
        raise InputError("--compare-sdr wants an 8-bit capture (PNG / BMP / R8G8B8A8 / B8G8R8A8), got %s" % sdr_frame["format_name"])
    h, w = dec["y"].shape
    px = sdr_frame["px"]
    resized = px.shape[:2] != (h, w)
    if resized:
        px = np.asarray(Image.fromarray(np.ascontiguousarray(px)).resize((w, h), Image.NEAREST))
    lut = sdr_eotf(np.arange(256) / 255.0, meta["sdr_eotf"])
    rel = lut[px[..., :3]] @ W709
    pw = meta["paper_white_nits"]
    keep = rel >= 0.001
    ratio = dec["y"][keep] / (rel[keep] * pw)
    rel_k = rel[keep]
    bands = []
    edges = SDR_COMPARE_BANDS
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        m = (rel_k >= lo) & ((rel_k <= hi) if i == len(edges) - 2 else (rel_k < hi))
        r = ratio[m]
        b = {"sdr_relative_luminance": [lo, hi], "pixels": int(r.size), "median_ratio": None, "p10_ratio": None, "p90_ratio": None,
             "fraction_hdr_darker_10pct": None}
        if r.size:
            p10, med, p90 = np.percentile(r, [10, 50, 90])
            b.update({"median_ratio": float(med), "p10_ratio": float(p10), "p90_ratio": float(p90),
                      "fraction_hdr_darker_10pct": float((r < 0.9).mean())})
        bands.append(b)
    return {"resized_sdr_to_frame": resized, "sdr_size": [int(sdr_frame["width"]), int(sdr_frame["height"])],
            "excluded_pixels_below_0.001": int((~keep).sum()), "bands": bands,
            "fraction_hdr_darker_10pct": float((ratio < 0.9).mean()) if ratio.size else None}


# --------------------------------------------------------------------------------------------------------------------
# images

def tonemap_preview(dec: dict, meta: dict):
    """SDR preview (H, W, 3 uint8): identity up to PREVIEW_KNEE x paper white, then a smooth Reinhard-type rolloff reaching
    1.0 at peak / paper white, applied to luminance; chroma kept by scaling RGB by Y_out / Y_in."""
    pw = meta["paper_white_nits"]
    lw = max(meta["peak_nits"] / pw, 1.0)
    k = PREVIEW_KNEE
    a = (lw - 1.0) / (1.0 - k)
    rgb, y = dec["rgb"], dec["y"]
    h, w = y.shape
    out = np.empty((h, w, 3), dtype=np.uint8)
    for r0 in range(0, h, 256):
        r1 = min(r0 + 256, h)
        yy = y[r0:r1] / pw
        t = np.maximum(yy - k, 0.0) / (lw - k)
        y_out = np.where(yy <= k, yy, k + (1.0 - k) * (1.0 + a) * t / (1.0 + a * t))
        scale = np.where(yy > 1e-9, y_out / np.maximum(yy, 1e-9), 1.0)
        lin = np.clip(rgb[r0:r1] / pw * scale[..., None], 0.0, 1.0)
        out[r0:r1] = SRGB8_LUT[np.rint(lin * 65535.0).astype(np.int32)]
    return out


def nits_map(dec: dict, meta: dict):
    """(image H x W x 3 uint8, legend items [(colour, label)])."""
    pw, peak = meta["paper_white_nits"], meta["peak_nits"]
    bands = [(0.1, (0, 0, 0), "<0.1"), (1.0, (0, 0, 110), "<1"), (10.0, (0, 70, 255), "<10"), (50.0, (0, 220, 255), "<50"),
             (0.9 * pw, (0, 170, 0), "<%g (0.9 PW)" % (0.9 * pw)), (1.1 * pw, (255, 255, 255), "PW %g (0.9-1.1x)" % pw),
             (500.0, (255, 230, 0), "<500"), (1000.0, (255, 140, 0), "<1000")]
    bands.sort(key=lambda b: b[0])
    thresholds = np.array([b[0] for b in bands])
    palette = np.array([b[1] for b in bands] + [(230, 0, 0)], dtype=np.uint8)
    y = dec["y"]
    img = palette[np.searchsorted(thresholds, y, side="right")]
    img[y > peak] = (255, 0, 255)
    legend = [(b[1], b[2]) for b in bands] + [((230, 0, 0), ">=%g" % thresholds[-1]), ((255, 0, 255), ">peak %g" % peak)]
    return img, legend


def with_legend(img, legend):
    h, w = img.shape[:2]
    box, pad, gap, row_h = 12, 6, 14, 16
    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    x, y, placed = pad, pad, []
    for colour, label in legend:
        item_w = box + 4 + int(math.ceil(probe.textlength(label)))
        if x + item_w + pad > w and x > pad:
            x, y = pad, y + row_h
        placed.append((x, y, colour, label))
        x += item_w + gap
    strip_h = y + row_h + pad
    canvas = Image.new("RGB", (w, h + strip_h), (24, 24, 24))
    canvas.paste(Image.fromarray(img), (0, 0))
    d = ImageDraw.Draw(canvas)
    for x, y, colour, label in placed:
        top = h + y + 1
        d.rectangle([x, top, x + box, top + box], fill=tuple(colour), outline=(140, 140, 140))
        d.text((x + box + 4, top), label, fill=(225, 225, 225))
    return canvas


# --------------------------------------------------------------------------------------------------------------------
# per-file driver and report

def fn(v, digits: int = 5) -> str:
    if v is None:
        return "n/a"
    return "%.*g" % (digits, v)


def pct(v, digits: int = 3) -> str:
    return "n/a" if v is None else "%.*g %%" % (digits, v * 100.0)


def to_jsonable(o):
    if isinstance(o, dict):
        return {str(k): to_jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [to_jsonable(v) for v in o]
    if isinstance(o, np.ndarray):
        return to_jsonable(o.tolist())
    if isinstance(o, np.bool_):
        return bool(o)
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, (np.floating, float)):
        f = float(o)
        return f if math.isfinite(f) else None
    return o


def analyze_file(path, args, shared: dict) -> dict:
    t0 = time.perf_counter()
    frame = load_image(path)
    meta = resolve_meta(path, frame, args)
    dec = decode(frame, meta)
    p = Path(path)
    out_dir = Path(args.out_dir) if args.out_dir else p.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    name = p.stem

    lum = luminance_section(dec, meta)
    black = black_section(dec, lum)
    gamut = gamut_section(dec)
    ui = ui_section(dec, meta, args, shared)
    banding = banding_section(frame, meta, dec, args)
    compare = compare_section(dec, meta, shared["compare"]) if shared["compare"] is not None else None

    images = {"preview": None, "nits": None}
    if not args.no_images:
        prev_path, nits_path = out_dir / (name + ".preview.png"), out_dir / (name + ".nits.png")
        Image.fromarray(tonemap_preview(dec, meta)).save(prev_path, compress_level=1)
        nm, legend = nits_map(dec, meta)
        with_legend(nm, legend).save(nits_path, compress_level=1)
        images = {"preview": str(prev_path), "nits": str(nits_path)}

    flags = []
    if black["raised_blacks"]:
        flags.append("RAISED_BLACKS")
    if banding["flag"]:
        flags.append("BANDING_8BIT_UPSTREAM")
    if lum["over_peak_fraction"] >= OVER_PEAK_FLAG_FRACTION:
        flags.append("OVER_PEAK")
    if meta["colorspace"] == "hdr10" and lum["max_code_any_fraction"] is not None \
            and lum["max_code_any_fraction"] >= CLIPPED_FLAG_FRACTION:
        flags.append("CLIPPED_CODES")
    if dec["nonfinite_pixels"]:
        flags.append("NONFINITE")
    if gamut["above_pq_range_pixels"]:
        flags.append("ABOVE_PQ_RANGE")

    st = lum["stats"]
    summary = "hdr-analyze %s: peak=%s p99=%s p50=%s black_p0.5=%s ui_white=%s banding_step=%s gamut_out=%s%% flags=[%s]" % (
        name, fn(st["max"]), fn(st["p99"]), fn(st["p50"]), fn(black["black_floor_nits"]), fn(ui.get("ui_white_nits")),
        fn(banding["step_median"]), "%.3g" % (gamut["out_of_bt709_fraction"] * 100.0), ",".join(flags))
    notes = list(frame["notes"]) + list(meta["notes"])
    meta_out = {k: v for k, v in meta.items() if k != "notes"}
    report = {"tool": "hdr_analyze", "input": str(path), "name": name, "width": frame["width"], "height": frame["height"],
              "format": frame["format_name"], "dxgi_format": frame["dxgi"], "meta": meta_out, "luminance": lum, "black": black,
              "gamut": gamut, "ui": ui, "banding": banding, "compare_sdr": compare, "images": images, "flags": flags,
              "summary": summary, "notes": notes}
    json_path = out_dir / (name + ".analysis.json")
    report["analysis_json"] = str(json_path)
    report["elapsed_s"] = time.perf_counter() - t0
    json_path.write_text(json.dumps(to_jsonable(report), indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


def format_report(rep: dict) -> list:
    m, lum, blk, gam, ui, ban = rep["meta"], rep["luminance"], rep["black"], rep["gamut"], rep["ui"], rep["banding"]
    L = []
    extra = ", ".join("%s %s" % (k, m[k]) for k in ("frame", "t", "kind") if m.get(k) is not None)
    L.append("== %s  %dx%d  %s  colorspace=%s  paper_white=%g peak=%g min=%s%s" % (
        rep["input"], rep["width"], rep["height"], rep["format"], m["colorspace"], m["paper_white_nits"], m["peak_nits"],
        fn(m["min_nits"]), ("  (" + extra + ")") if extra else ""))
    if m["colorspace"] == "sdr":
        L.append("   sdr decode: %s, 1.0 = %g nits" % (m["sdr_eotf"], m["sdr_white_nits"]))
    for n in rep["notes"]:
        L.append("   note: " + n)
    st = lum["stats"]
    L.append("luminance Y (nits): min=%s p0.1=%s p1=%s p5=%s p25=%s p50=%s p75=%s p95=%s p99=%s p99.9=%s max=%s mean=%s" % tuple(
        fn(st[k]) for k in ("min", "p0.1", "p1", "p5", "p25", "p50", "p75", "p95", "p99", "p99.9", "max", "mean")))
    L.append("histogram (fraction of pixels per Y bin, nits):")
    for label, frac in zip(lum["histogram"]["labels"], lum["histogram"]["fraction"]):
        if frac > 0:
            L.append("   %-14s %9.4f %%" % (label, frac * 100.0))
    L.append("over peak: %d px (%s) above %g nits (peak*1.01)" % (lum["over_peak_pixels"], pct(lum["over_peak_fraction"]),
                                                                  lum["over_peak_threshold_nits"]))
    if lum["max_code_any_pixels"] is None:
        L.append("max code: n/a (float format)")
    else:
        L.append("at max code: %d px (%s) with any channel, %d px with all channels" % (
            lum["max_code_any_pixels"], pct(lum["max_code_any_fraction"]), lum["max_code_all_pixels"]))
    L.append("black: %s of pixels < 0.01 nit, %s < 0.1 nit; black floor (p0.5) = %s nits%s; NaN/Inf pixels %d; negative pixels %d" % (
        pct(blk["fraction_below_0.01_nits"]), pct(blk["fraction_below_0.1_nits"]), fn(blk["black_floor_nits"]),
        "  RAISED_BLACKS" if blk["raised_blacks"] else "", blk["nonfinite_pixels"], blk["negative_pixels"]))
    L.append("gamut: %s of pixels outside BT.709 (%d px), max negative channel %s nits, max -channel/Y %s; Y > 10000 nits: %d px" % (
        pct(gam["out_of_bt709_fraction"]), gam["out_of_bt709_pixels"], fn(gam["max_excursion_nits"]),
        fn(gam["max_excursion_relative_to_y"]), gam["above_pq_range_pixels"]))
    if ui["method"] == "heuristic":
        L.append("ui white (HEURISTIC, flat near-achromatic bright plateaus): estimate %s nits (%s of paper white), %d candidate px" % (
            fn(ui["ui_white_estimate_nits"]), pct(ui.get("white_ratio"), 4), ui["candidate_pixels"]))
        for mode in ui["modes"]:
            L.append("   mode %s nits: %d px" % (fn(mode["nits"]), mode["pixels"]))
    else:
        regs = ui["regions"] + ([ui["overall"]] if len(ui["regions"]) > 1 else [])
        for r in regs:
            if not r["pixels"]:
                L.append("ui %s: no pixels" % r["name"])
                continue
            yp = r["y_nits"]
            L.append("ui %s: %d px, Y min=%s p5=%s p50=%s p95=%s max=%s; white: %d px, median %s nits (p5 %s, p95 %s), ratio to paper white %s" % (
                r["name"], r["pixels"], fn(yp["min"]), fn(yp["p5"]), fn(yp["p50"]), fn(yp["p95"]), fn(yp["max"]), r["white_pixels"],
                fn(r["white_nits"]), fn(r["white_nits_p5"]), fn(r["white_nits_p95"]), fn(r["white_ratio"], 4)))
    L.append("banding (steps of 1..16 codes between adjacent pixels along rows; green code, luma code):")
    for r in ban["regions"]:
        g, lu, pl = r["green"], r["luma"], r["plateau"]
        L.append("   %-22s %s: green small steps %d, median %s, 1-code %s%s; luma median %s; plateau mean %s median %s (%d runs)" % (
            r["name"], "x,y,w,h=%d,%d,%d,%d" % tuple(r["rect"]), g["small_steps"], fn(g["median_step"]),
            pct(g["fraction_step_1"], 3), "  BANDING_8BIT_UPSTREAM" if r["flag"] else "", fn(lu["median_step"]),
            fn(pl["mean"], 4), fn(pl["median"], 4), pl["runs"]))
        L.append("      green step histogram 1..16: " + " ".join(str(v) for v in g["hist_1_to_16"]))
    L.append("   " + ban["interpretation"])
    cmp_ = rep["compare_sdr"]
    if cmp_ is not None:
        L.append("compare-sdr (HDR_Y / SDR_Y, SDR 1.0 = paper white%s; excluded %d px below 0.001):" % (
            ", SDR resized" if cmp_["resized_sdr_to_frame"] else "", cmp_["excluded_pixels_below_0.001"]))
        for b in cmp_["bands"]:
            L.append("   sdr %.2f-%.2f: %9d px  median %s  p10 %s  p90 %s  HDR darker >10%%: %s" % (
                b["sdr_relative_luminance"][0], b["sdr_relative_luminance"][1], b["pixels"], fn(b["median_ratio"], 4),
                fn(b["p10_ratio"], 4), fn(b["p90_ratio"], 4), pct(b["fraction_hdr_darker_10pct"])))
        L.append("   overall HDR darker than SDR by > 10%%: %s" % pct(cmp_["fraction_hdr_darker_10pct"]))
    if rep["images"]["preview"]:
        L.append("wrote %s" % rep["images"]["preview"])
        L.append("wrote %s" % rep["images"]["nits"])
    L.append("wrote %s  (%.2f s)" % (rep["analysis_json"], rep["elapsed_s"]))
    return L


# --------------------------------------------------------------------------------------------------------------------
# command line

def parse_rect(text: str):
    parts = text.replace(" ", "").split(",")
    try:
        vals = tuple(int(p) for p in parts)
    except ValueError:
        vals = ()
    if len(vals) != 4 or vals[2] <= 0 or vals[3] <= 0:
        raise argparse.ArgumentTypeError("expected x,y,w,h with w,h > 0, got %r" % text)
    return vals


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", metavar="FILE", help="DDS (DX10: R10G10B10A2, R16G16B16A16F, R32G32B32A32F, RGBA8, BGRA8) or PNG/BMP")
    ap.add_argument("--selftest", action="store_true", help="synthesise gradients, analyse them, assert (exit 1 on a FAIL)")
    ap.add_argument("--out-dir", metavar="DIR", help="write <name>.analysis.json / .preview.png / .nits.png here (default: next to the input)")
    ap.add_argument("--no-images", action="store_true", help="skip the preview and nits PNGs")
    ap.add_argument("--colorspace", choices=("hdr10", "scrgb", "sdr"), help="override the sidecar / the format's default")
    ap.add_argument("--paper-white", type=float, metavar="NITS", help="paper white in nits (sidecar, else %g)" % DEFAULT_PAPER_WHITE)
    ap.add_argument("--peak", type=float, metavar="NITS", help="display peak in nits (sidecar, else %g)" % DEFAULT_PEAK)
    ap.add_argument("--min-nits", type=float, metavar="NITS", help="display minimum in nits (reported only)")
    ap.add_argument("--sdr-eotf", choices=("srgb", "gamma22"), default="srgb", help="EOTF of SDR input (default srgb)")
    ap.add_argument("--sdr-white", type=float, metavar="NITS", help="nits of SDR 1.0 (default: paper white)")
    ap.add_argument("--rect", type=parse_rect, action="append", metavar="x,y,w,h", help="a UI region (repeatable)")
    ap.add_argument("--ui-mask", metavar="FILE", help="UI mask image (PNG/DDS): alpha where it varies, else non-zero pixels")
    ap.add_argument("--ui-layer", metavar="FILE", help="UI layer texture (DDS/PNG): alpha > 0.5 is UI")
    ap.add_argument("--band-rect", type=parse_rect, action="append", metavar="x,y,w,h",
                    help="banding region (repeatable; default the top 25%% rows); the full image is always scanned too")
    ap.add_argument("--compare-sdr", metavar="FILE", help="an 8-bit SDR capture of the same frame (PNG/BMP/DDS)")
    return ap


def load_shared(args) -> dict:
    shared = {"ui_mask": None, "ui_layer": None, "compare": None}
    for key, flag in (("ui_mask", args.ui_mask), ("ui_layer", args.ui_layer), ("compare", args.compare_sdr)):
        if flag:
            shared[key] = load_image(flag)
    return shared


def main(argv=None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.inputs:
        ap.error("no input files (give one or more FILEs, or --selftest)")
    try:
        shared = load_shared(args)
    except InputError as e:
        print("hdr-analyze: cannot read an auxiliary input: %s" % e, file=sys.stderr)
        return 2
    rc, reports = 0, []
    for path in args.inputs:
        try:
            rep = analyze_file(path, args, shared)
        except InputError as e:
            print("hdr-analyze: cannot analyse %s: %s" % (path, e), file=sys.stderr)
            rc = 2
            continue
        for line in format_report(rep):
            print(line)
        reports.append(rep)
    if reports:
        print()
        for rep in reports:
            print(rep["summary"])
    return rc


# --------------------------------------------------------------------------------------------------------------------
# self test

def synth_hdr10(w: int, h: int, pw: float, rect, lift: float = 0.0, quantise8: bool = False, noise_rng=None):
    """HDR10 words of a 0..1000 nit BT.709-grey horizontal gradient (the bottom 8 % of the rows pure black, a letterbox),
    a flat rectangle at exactly paper white, optionally through an 8-bit sRGB intermediate / with +-1 code noise."""
    nits = _gradient_nits(w, h, pw, rect, lift, quantise8)
    codes = np.rint(pq_encode(nits) * 1023.0).astype(np.int64)
    if noise_rng is not None:
        noise = noise_rng.integers(-1, 2, size=codes.shape)
        x, y, rw, rh = rect
        noise[y:y + rh, x:x + rw] = 0
        codes = np.clip(codes + noise, 0, 1023)
    c = codes.astype(np.uint32)
    return c | (c << 10) | (c << 20) | (np.uint32(3) << 30)


def _gradient_nits(w, h, pw, rect, lift, quantise8):
    row = np.linspace(0.0, 1000.0, w)
    nits = np.tile(row, (h, 1))
    if quantise8:
        c8 = np.rint(srgb_oetf(nits / 1000.0) * 255.0)
        nits = srgb_eotf(c8 / 255.0) * 1000.0
    nits[int(h * 0.92):, :] = 0.0
    x, y, rw, rh = rect
    nits[y:y + rh, x:x + rw] = pw
    return nits + lift


def synth_scrgb(w: int, h: int, pw: float, rect):
    nits = _gradient_nits(w, h, pw, rect, 0.0, False)
    v = (nits / SCRGB_NITS_PER_UNIT).astype(np.float16)
    out = np.empty((h, w, 4), dtype=np.float16)
    out[..., 0] = out[..., 1] = out[..., 2] = v
    out[..., 3] = 1.0
    return out


def selftest() -> int:
    fails = 0

    def check(label, ok, detail=""):
        nonlocal fails
        print("%s  %s%s" % ("PASS" if ok else "FAIL", label, ("  (" + detail + ")") if detail else ""))
        if not ok:
            fails += 1

    # transfer function and container checks
    nits = np.geomspace(0.001, 10000.0, 4000)
    err = float(np.max(np.abs(pq_decode(pq_encode(nits)) / nits - 1.0)))
    check("PQ encode/decode round trip error < 0.1 %", err < 1e-3, "max rel error %.2e" % err)
    anchors = pq_encode([100.0, 1000.0, 10000.0])
    check("PQ anchors: 100 nits -> 0.5081, 1000 nits -> 0.7518, 10000 nits -> 1",
          abs(anchors[0] - 0.5081) < 5e-4 and abs(anchors[1] - 0.7518) < 5e-4 and abs(anchors[2] - 1.0) < 1e-9,
          "%.4f %.4f %.4f" % tuple(anchors))
    probe = np.geomspace(0.001, 10000.0, 20000)
    check("pq_code10 equals rint(pq_encode * 1023)", int((pq_code10(probe) != np.rint(pq_encode(probe) * 1023)).sum()) == 0)

    rng = np.random.default_rng(1234)
    with tempfile.TemporaryDirectory(prefix="hdr_analyze_selftest_") as td:
        td = Path(td)
        formats = {24: rng.integers(0, 2 ** 32, size=(5, 7), dtype=np.uint32),
                   10: rng.random((5, 7, 4)).astype(np.float16), 2: rng.random((5, 7, 4)).astype(np.float32),
                   28: rng.integers(0, 256, size=(5, 7, 4), dtype=np.uint8), 87: rng.integers(0, 256, size=(5, 7, 4), dtype=np.uint8)}
        ok = True
        for dxgi, arr in formats.items():
            for pitch in (None, 7 * DXGI_FORMATS[dxgi][1] + 12):
                write_dds(td / "rt.dds", dxgi, arr, row_pitch=pitch)
                fr = read_dds(td / "rt.dds")
                if dxgi == 24:
                    back = fr["px"][..., 0].astype(np.uint32) | (fr["px"][..., 1].astype(np.uint32) << 10) | \
                           (fr["px"][..., 2].astype(np.uint32) << 20) | (fr["px"][..., 3].astype(np.uint32) << 30)
                    same = np.array_equal(back, arr)
                elif dxgi == 87:
                    same = np.array_equal(fr["px"], arr[..., [2, 1, 0, 3]])
                else:
                    same = np.array_equal(fr["px"], arr)
                ok = ok and same and (fr["width"], fr["height"]) == (7, 5)
        check("DDS write/read round trip for dxgi 24, 10, 2, 28, 87 (tight and padded rows)", ok)
        blob = bytearray((td / "rt.dds").read_bytes())
        struct.pack_into("<I", blob, 128, 71)           # BC1
        (td / "bad.dds").write_bytes(bytes(blob))
        try:
            read_dds(td / "bad.dds")
            check("unsupported DXGI format is refused", False)
        except InputError as e:
            check("unsupported DXGI format is refused", "71" in str(e))

        # synthetic frames
        w, h, pw = 1280, 720, 200.0
        rect = (100, 100, 200, 100)
        side = {"colorspace": "hdr10", "paper_white_nits": pw, "peak_nits": 1000.0, "min_nits": 0.0, "frame": 1, "t": 0.5,
                "kind": "backbuffer", "width": w, "height": h}
        dark_band = "0,0,%d,%d" % (w // 4, h)
        rect_arg = "%d,%d,%d,%d" % rect

        def make(name, dxgi, pixels, sidecar):
            write_dds(td / (name + ".dds"), dxgi, pixels)
            (td / (name + ".json")).write_text(json.dumps(sidecar), encoding="utf-8")
            return str(td / (name + ".dds"))

        def run(path, extra):
            args = build_parser().parse_args([path] + extra)
            with contextlib.redirect_stdout(io.StringIO()):
                rep = analyze_file(path, args, {"ui_mask": None, "ui_layer": None, "compare": None})
            print("   " + rep["summary"])
            return rep

        a_path = make("a_clean_hdr10", 24, synth_hdr10(w, h, pw, rect), side)
        b_path = make("b_8bit_upstream", 24, synth_hdr10(w, h, pw, rect, quantise8=True), side)
        c_path = make("c_scrgb", 10, synth_scrgb(w, h, pw, rect), dict(side, colorspace="scrgb"))
        d_path = make("d_noisy", 24, synth_hdr10(w, h, pw, rect, noise_rng=rng), side)
        e_path = make("e_raised", 24, synth_hdr10(w, h, pw, rect, lift=0.2), side)
        common = ["--rect", rect_arg, "--band-rect", dark_band]

        a = run(a_path, common)
        peak = a["luminance"]["stats"]["max"]
        check("(a) HDR10: decoded peak within 1 % of 1000 nits", abs(peak / 1000.0 - 1.0) < 0.01, "%.2f nits" % peak)
        uw = a["ui"]["ui_white_nits"]
        check("(a) HDR10: UI white (--rect) within 2 % of paper white", uw is not None and abs(uw / pw - 1.0) < 0.02, "%s nits" % fn(uw))
        check("(a) HDR10: not flagged for banding", "BANDING_8BIT_UPSTREAM" not in a["flags"],
              "median steps %s" % ", ".join(fn(r["green"]["median_step"]) for r in a["banding"]["regions"]))
        check("(a) HDR10: black floor is 0", a["black"]["black_floor_nits"] == 0.0, "%s nits" % fn(a["black"]["black_floor_nits"]))
        check("(a) HDR10: not flagged RAISED_BLACKS", "RAISED_BLACKS" not in a["flags"])
        check("(a) HDR10: grey content has no pixel outside BT.709", a["gamut"]["out_of_bt709_pixels"] == 0)
        outs = [Path(a["images"]["preview"]), Path(a["images"]["nits"]), Path(a["analysis_json"])]
        okf = all(p.is_file() for p in outs)
        if okf:
            with Image.open(outs[0]) as im0, Image.open(outs[1]) as im1:
                px = im0.convert("RGB").getpixel((rect[0] + 100, rect[1] + 50))
                okf = im0.size == (w, h) and im1.size[0] == w and im1.size[1] > h and px[0] == px[1] == px[2] >= 200
            okf = okf and json.loads(outs[2].read_text(encoding="utf-8"))["name"] == "a_clean_hdr10"
        check("(a) preview.png, nits.png (with legend strip) and analysis.json are written", okf)

        b = run(b_path, common)
        check("(b) 8-bit sRGB intermediate: flagged BANDING_8BIT_UPSTREAM", "BANDING_8BIT_UPSTREAM" in b["flags"],
              "median steps %s" % ", ".join("%s=%s" % (r["name"], fn(r["green"]["median_step"])) for r in b["banding"]["regions"]))
        ub = b["ui"]["ui_white_nits"]
        check("(b) 8-bit intermediate: UI white (--rect) within 2 % of paper white", ub is not None and abs(ub / pw - 1.0) < 0.02)

        c = run(c_path, common)
        pc = c["luminance"]["stats"]["max"]
        check("(c) scRGB: decoded peak within 1 % of 1000 nits", abs(pc / 1000.0 - 1.0) < 0.01, "%.2f nits" % pc)
        uc = c["ui"]["ui_white_nits"]
        check("(c) scRGB: UI white (--rect) within 2 % of paper white", uc is not None and abs(uc / pw - 1.0) < 0.02, "%s nits" % fn(uc))
        check("(c) scRGB: not flagged for banding", "BANDING_8BIT_UPSTREAM" not in c["flags"],
              "median steps %s" % ", ".join(fn(r["green"]["median_step"]) for r in c["banding"]["regions"]))
        check("(c) scRGB: black floor is 0", c["black"]["black_floor_nits"] == 0.0)

        d = run(d_path, ["--no-images"])
        ud = d["ui"].get("ui_white_estimate_nits")
        check("(d) heuristic UI white estimate (no mask, +-1 code noise elsewhere) within 2 % of paper white",
              d["ui"]["method"] == "heuristic" and ud is not None and abs(ud / pw - 1.0) < 0.02, "%s nits" % fn(ud))

        e = run(e_path, ["--no-images"])
        check("(e) lifted blacks (+0.2 nit): flagged RAISED_BLACKS", "RAISED_BLACKS" in e["flags"],
              "floor %s nits" % fn(e["black"]["black_floor_nits"]))
    print("selftest: %s" % ("all PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
