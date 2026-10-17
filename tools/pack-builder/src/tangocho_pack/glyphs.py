"""Glyph rasterisation (Pillow) and 4bpp packing."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

SET_LARGE, SET_SMALL, SET_TEXT, SET_OPTION = 0, 1, 2, 3


@dataclass
class Glyph:
    codepoint: int
    set: int
    width: int
    height: int
    x_off: int
    y_off: int
    advance: int
    data: bytes  # raw 4bpp, row stride ceil(width/2)
    encoding: int = 0


def quantize(v: int) -> int:
    return (v * 15 + 127) // 255


def pack_4bpp(pixels: list[int] | bytes, width: int, height: int) -> bytes:
    """Pack 8-bit coverage (row-major) into 4bpp, high nibble first,
    byte-aligned rows. Values are quantised to 0..15."""
    stride = (width + 1) // 2
    out = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            q = quantize(pixels[y * width + x])
            out[y * stride + x // 2] |= q << 4 if x % 2 == 0 else q
    return bytes(out)


def unpack_4bpp(data: bytes, width: int, height: int) -> list[int]:
    """Inverse of pack_4bpp; returns nibble values 0..15."""
    stride = (width + 1) // 2
    out = []
    for y in range(height):
        for x in range(width):
            b = data[y * stride + x // 2]
            out.append(b >> 4 if x % 2 == 0 else b & 15)
    return out


class FontRenderer:
    def __init__(self, path: str | Path):
        self.path = str(path)
        self.cmap = set(TTFont(self.path).getBestCmap())
        self._fonts: dict[int, ImageFont.FreeTypeFont] = {}

    def font(self, px: int) -> ImageFont.FreeTypeFont:
        if px not in self._fonts:
            self._fonts[px] = ImageFont.truetype(self.path, px)
        return self._fonts[px]

    def has(self, cp: int) -> bool:
        return cp in self.cmap

    def _render(self, cp: int, px: int) -> tuple[Image.Image, int, int]:
        """Render onto a generous canvas; returns (image, origin_x, baseline_y)."""
        w, h = px * 3, px * 3
        img = Image.new("L", (w, h), 0)
        ox, oy = px, px * 2
        ImageDraw.Draw(img).text((ox, oy), chr(cp), fill=255, font=self.font(px), anchor="ls")
        return img, ox, oy

    @staticmethod
    def _crop(img: Image.Image) -> tuple[Image.Image, int, int] | None:
        """Crop to the ink that survives 4bpp quantisation."""
        q = img.point([quantize(v) * 17 for v in range(256)])
        box = q.getbbox()
        if box is None:
            return None
        return q.crop(box), box[0], box[1]

    def kanji_glyph(self, cp: int, px: int, gset: int) -> Glyph:
        """Ink box centred in a px*px em square whose bottom edge is the
        baseline (so y_off = square_top_offset - px, always negative)."""
        img, _, _ = self._render(cp, px)
        c = self._crop(img)
        if c is None:
            return Glyph(cp, gset, 0, 0, 0, 0, px, b"")
        crop, _, _ = c
        w, h = crop.size
        left = (px - w) // 2
        top = (px - h) // 2
        return Glyph(cp, gset, w, h, left, top - px, px, pack_4bpp(_vals(crop), w, h))

    def text_glyph(self, cp: int, px: int, gset: int = SET_TEXT) -> Glyph:
        img, ox, oy = self._render(cp, px)
        adv = int(round(self.font(px).getlength(chr(cp))))
        c = self._crop(img)
        if c is None:
            return Glyph(cp, gset, 0, 0, 0, 0, adv, b"")
        crop, bx, by = c
        w, h = crop.size
        return Glyph(cp, gset, w, h, bx - ox, by - oy, adv, pack_4bpp(_vals(crop), w, h))

    def text_metrics(self, px: int) -> tuple[int, int]:
        """(line_height, ascent) for set 2."""
        asc, desc = self.font(px).getmetrics()
        return asc + desc, asc


def _vals(img: Image.Image) -> list[int]:
    # crop() was quantised to multiples of 17; quantize() maps back exactly.
    return list(img.tobytes())
