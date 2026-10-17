"""KanjiVG parsing and per-stroke 1bpp mask rasterisation.

KanjiVG (https://kanjivg.tagaini.net, CC BY-SA 3.0, (c) Ulrich Apel) stores each
kanji as an SVG whose ``<path>`` elements are, in document order, the strokes.
A path is an open *centre line* (the group sets ``stroke-width:3`` in a
``109x109`` viewBox), not a filled outline, so we reproduce it by stroking the
flattened polyline with a width derived from that ratio rather than filling the
polyline as a polygon (which a straight horizontal stroke would have drawn as
nothing). A ``Z`` subpath is closed before it is stroked.

Output masks and their placement match docs/pack-format.md: 1bpp, bit 7 =
leftmost pixel, row stride ``ceil(w/8)``, 1 = ink, position relative to the
top-left of the ``px`` x ``px`` em square the viewBox is mapped onto.
"""

from __future__ import annotations

import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from xml.etree import ElementTree as ET

from PIL import Image, ImageDraw

from . import paths

# KanjiVG's nominal stroke width and viewBox size (see the module docstring).
_STROKE_WIDTH = 3.0
_DEFAULT_VIEWBOX = (0.0, 0.0, 109.0, 109.0)
# Line segments per Bezier when flattening curves.
_CURVE_STEPS = 8

_NUM = r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?"
_TOKEN = re.compile(rf"([MmLlHhVvCcSsQqTtAaZz])|({_NUM})")
_STEPS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2}

Point = tuple[float, float]


@dataclass
class StrokeMask:
    x_off: int
    y_off: int
    w: int
    h: int
    data: bytes  # 1bpp, row stride ceil(w/8), bit 7 = leftmost pixel


# ------------------------------------------------------------- SVG sources

class KanjiVGError(RuntimeError):
    pass


class KanjiVGDir:
    """Stroke SVGs from a directory of ``{codepoint:05x}.svg`` files."""

    def __init__(self, root: str | Path):
        self.root = Path(root)

    def svg(self, cp: int) -> bytes | None:
        f = self.root / f"{cp:05x}.svg"
        return f.read_bytes() if f.exists() else None

    def close(self) -> None:  # symmetry with KanjiVGZip
        pass


class KanjiVGZip:
    """Stroke SVGs read straight from the KanjiVG release zip (``kanji/*.svg``)."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._zip: zipfile.ZipFile | None = None
        self._names: dict[str, str] = {}

    def _ensure(self) -> None:
        if self._zip is None:
            if not self.path.exists():
                raise KanjiVGError(f"KanjiVG zip not found: {self.path}")
            self._zip = zipfile.ZipFile(self.path)
            self._names = {Path(n).name: n for n in self._zip.namelist() if n.endswith(".svg")}

    def svg(self, cp: int) -> bytes | None:
        self._ensure()
        assert self._zip is not None
        name = self._names.get(f"{cp:05x}.svg")
        return self._zip.read(name) if name else None

    def close(self) -> None:
        if self._zip is not None:
            self._zip.close()
            self._zip = None


def default_source() -> KanjiVGDir | KanjiVGZip:
    """The cache: the release zip if fetched, else an extracted ``kanji/`` dir."""
    if paths.KANJIVG_ZIP.exists():
        return KanjiVGZip(paths.KANJIVG_ZIP)
    extracted = paths.KANJIVG_DIR / "kanji"
    if extracted.is_dir():
        return KanjiVGDir(extracted)
    raise KanjiVGError(
        f"no KanjiVG data in {paths.KANJIVG_DIR} (run `tangocho-pack fetch`)"
    )


# ------------------------------------------------------------- path parsing

def flatten_path(d: str) -> list[list[Point]]:
    """Flatten an SVG path `d` into subpaths of line points (absolute)."""
    groups: list[tuple[str, list[float]]] = []
    cur: tuple[str, list[float]] | None = None
    for m in _TOKEN.finditer(d):
        letter, num = m.group(1), m.group(2)
        if letter:
            cur = (letter, [])
            groups.append(cur)
        elif num is not None:
            if cur is None:
                continue
            cur[1].append(float(num))

    subpaths: list[list[Point]] = []
    pts: list[Point] = []
    x = y = 0.0
    sx = sy = 0.0
    prev_c: Point | None = None  # last cubic control, for S
    prev_q: Point | None = None  # last quadratic control, for T

    def flush(close: bool) -> None:
        nonlocal pts
        if close and pts:
            pts.append((sx, sy))
        if pts:
            subpaths.append(pts)
        pts = []

    for cmd, args in groups:
        up = cmd.upper()
        rel = cmd.islower()
        if up == "Z":
            flush(close=True)
            x, y = sx, sy
            prev_c = prev_q = None
            continue
        step = _STEPS.get(up)
        if step is None:
            print(f"warning: kanjivg: unsupported path command {cmd!r}, path skipped",
                  file=sys.stderr)
            return []
        k = 0
        first = True
        while k + step <= len(args):
            a = args[k:k + step]
            k += step
            ox, oy = (x, y) if rel else (0.0, 0.0)
            if up == "M":
                x, y = a[0] + ox, a[1] + oy
                if first:
                    flush(close=False)
                    sx, sy = x, y
                    pts = [(x, y)]
                    first = False
                else:  # implicit lineto
                    pts.append((x, y))
                prev_c = prev_q = None
            elif up == "L":
                x, y = a[0] + ox, a[1] + oy
                pts.append((x, y))
                prev_c = prev_q = None
            elif up == "H":
                x = a[0] + ox
                pts.append((x, y))
                prev_c = prev_q = None
            elif up == "V":
                y = a[0] + oy
                pts.append((x, y))
                prev_c = prev_q = None
            elif up == "C":
                p1, p2, p = (a[0] + ox, a[1] + oy), (a[2] + ox, a[3] + oy), (a[4] + ox, a[5] + oy)
                pts += _cubic((x, y), p1, p2, p)
                x, y = p
                prev_c, prev_q = p2, None
            elif up == "S":
                p1 = _reflect(prev_c, (x, y)) if prev_c is not None else (x, y)
                p2, p = (a[0] + ox, a[1] + oy), (a[2] + ox, a[3] + oy)
                pts += _cubic((x, y), p1, p2, p)
                x, y = p
                prev_c, prev_q = p2, None
            elif up == "Q":
                q, p = (a[0] + ox, a[1] + oy), (a[2] + ox, a[3] + oy)
                pts += _quad((x, y), q, p)
                x, y = p
                prev_q, prev_c = q, None
            elif up == "T":
                q = _reflect(prev_q, (x, y)) if prev_q is not None else (x, y)
                p = (a[0] + ox, a[1] + oy)
                pts += _quad((x, y), q, p)
                x, y = p
                prev_q, prev_c = q, None
    flush(close=False)
    return subpaths


def _reflect(ctrl: Point | None, cur: Point) -> Point:
    if ctrl is None:
        return cur
    return (2 * cur[0] - ctrl[0], 2 * cur[1] - ctrl[1])


def _cubic(p0: Point, p1: Point, p2: Point, p3: Point) -> list[Point]:
    out = []
    for i in range(1, _CURVE_STEPS + 1):
        t = i / _CURVE_STEPS
        mt = 1 - t
        a, b = 3 * mt * mt * t, 3 * mt * t * t
        c, d = mt ** 3, t ** 3
        out.append((c * p0[0] + a * p1[0] + b * p2[0] + d * p3[0],
                    c * p0[1] + a * p1[1] + b * p2[1] + d * p3[1]))
    return out


def _quad(p0: Point, p1: Point, p2: Point) -> list[Point]:
    out = []
    for i in range(1, _CURVE_STEPS + 1):
        t = i / _CURVE_STEPS
        mt = 1 - t
        out.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                    mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))
    return out


# ------------------------------------------------------------- rasterising

def _viewbox(root: ET.Element) -> tuple[float, float, float, float]:
    vb = root.get("viewBox") or root.get("viewbox")
    if not vb:
        return _DEFAULT_VIEWBOX
    parts = re.split(r"[,\s]+", vb.strip())
    if len(parts) != 4:
        return _DEFAULT_VIEWBOX
    return tuple(float(p) for p in parts)  # type: ignore[return-value]


def _path_data(root: ET.Element) -> list[str]:
    out: list[str] = []
    for el in root.iter():  # document order == stroke order
        if el.tag.rsplit("}", 1)[-1] != "path":
            continue
        d = el.get("d")
        if d:
            out.append(d)
    return out


def _clampi(v: float, lo: int, hi: int) -> int:
    return max(lo, min(hi, int(round(v))))


def _rasterise(polylines: list[list[Point]], px: int,
               viewbox: tuple[float, float, float, float]) -> Image.Image:
    minx, miny, vbw, vbh = viewbox
    sx = px / vbw if vbw else 1.0
    sy = px / vbh if vbh else 1.0
    img = Image.new("L", (px, px), 0)
    drw = ImageDraw.Draw(img)
    width = max(1, round(_STROKE_WIDTH * (sx + sy) / 2))
    for line in polylines:
        pts = [(_clampi(min(x_ - minx, vbw) * sx, 0, px - 1),
                _clampi(min(y_ - miny, vbh) * sy, 0, px - 1)) for x_, y_ in line]
        if not pts:
            continue
        if len(pts) == 1:
            r = width / 2
            drw.ellipse([pts[0][0] - r, pts[0][1] - r, pts[0][0] + r, pts[0][1] + r], fill=255)
            continue
        drw.line(pts, fill=255, width=width, joint="curve")
        r = width / 2
        for p in (pts[0], pts[-1]):  # round caps, as the source specifies
            drw.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)
    return img


def _pack_1bpp(img: Image.Image) -> bytes:
    w, h = img.size
    stride = (w + 7) // 8
    src = img.tobytes()
    out = bytearray(stride * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            if src[row + x]:
                out[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def stroke_masks(svg: bytes, px: int) -> list[StrokeMask]:
    """Rasterise one kanji SVG into per-stroke 1bpp masks placed in a px square."""
    root = ET.fromstring(svg)
    viewbox = _viewbox(root)
    masks: list[StrokeMask] = []
    for d in _path_data(root):
        polylines = flatten_path(d)
        if not polylines:
            print(f"warning: kanjivg: empty path skipped: {d[:40]!r}", file=sys.stderr)
            continue
        img = _rasterise(polylines, px, viewbox)
        bbox = img.getbbox()
        if bbox is None:
            continue  # stroke fell outside the box (clamped away)
        crop = img.crop(bbox)
        masks.append(StrokeMask(bbox[0], bbox[1], crop.width, crop.height, _pack_1bpp(crop)))
    return masks


def masks_for(cp: int, px: int, source: KanjiVGDir | KanjiVGZip | None = None) -> list[StrokeMask]:
    """Masks for a codepoint; empty when the source has no SVG for it."""
    own = source is None
    source = source or default_source()
    try:
        svg = source.svg(cp)
    finally:
        if own:
            source.close()
    return stroke_masks(svg, px) if svg else []
