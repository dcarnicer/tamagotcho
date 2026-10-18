#!/usr/bin/env python3
"""Tangochō UX mockups: "before" and "after" PNGs of the device screens.

Run it inside the pack builder's environment (Pillow, fontTools and the glyph /
GLCD / KanjiVG code come from there):

    cd tools/pack-builder && uv run python ../ux-mockups/render.py

Options: ``--cache DIR`` (default ``tools/pack-builder/cache``; needs
``fonts/KleeOne-SemiBold.ttf`` and the KanjiVG zip, i.e. ``uv run tangocho-pack
fetch``), ``--out DIR`` (default ``docs/ux``) and ``--tokens`` (print the colour
token table with contrast ratios used in docs/ux-refresh.md, then exit).

* **Before** screens are line-by-line ports of the ``draw*`` functions in
  ``firmware/src/main.cpp`` (main @ 6a5d0b8): same geometry, the built-in 6x8
  GLCD font (Font0, all 8 rows) at 1x/2x, pack glyphs rendered from Klee One at
  the builder defaults (large 96, small 40, text 18, option 20 px) and the
  ``Renderer`` wrap/centre rules.
* **After** screens implement the proposal in ``docs/ux-refresh.md`` (design
  tokens, header, hint bar, tiles, rows, icons). Elements that depend on the
  proposed firmware UI font (typography option U3) are drawn with Klee One at
  the proposed sizes.

Every pixel is RGB565 (colours quantised like ``color565()``, glyph edges blended
with ``blend565`` against the flat background, as the firmware does), written at
native resolution and upscaled 3x with nearest neighbour, plus one contact sheet
``overview.png``. The output is deterministic: fixed sample data, no randomness,
no timestamps.

Sample card data comes from KANJIDIC2 (CC BY-SA 4.0) and the stroke shapes from
KanjiVG (CC BY-SA 3.0); the glyphs are Klee One (SIL OFL 1.1). See
docs/data-sources.md.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from tangocho_pack import glcd
from tangocho_pack.glyphs import FontRenderer, unpack_4bpp
from tangocho_pack.kanjivg import KanjiVGDir, KanjiVGZip, stroke_masks

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CACHE = REPO / "tools" / "pack-builder" / "cache"
DEFAULT_OUT = REPO / "docs" / "ux"

# Builder defaults (tangocho_pack.build.build_pack) = what a default pack holds.
LARGE_PX, SMALL_PX, TEXT_PX, OPTION_PX = 96, 40, 18, 20


# =============================================================== RGB565 helpers

def c565(r: int, g: int, b: int) -> int:
    """color565(): 8-bit RGB -> RGB565."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb(c: int) -> tuple[int, int, int]:
    """RGB565 -> the 8-bit RGB the panel shows (bit replication)."""
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def blend565(fg: int, bg: int, a: int) -> int:
    """Port of firmware/src/render/Blend565.h (alpha a/15)."""
    if a >= 15:
        return fg
    if a == 0:
        return bg
    ia = 15 - a
    r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * ia + 7) // 15
    g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * ia + 7) // 15
    b = ((fg & 0x1F) * a + (bg & 0x1F) * ia + 7) // 15
    return (r << 11) | (g << 5) | b


def cdiv(a: int, b: int) -> int:
    """C/C++ integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def luminance(c: int) -> float:
    def lin(v: int) -> float:
        s = v / 255
        return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4
    r, g, b = rgb(c)
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a: int, b: int) -> float:
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


# Proposed additions to firmware/src/render/GlcdCp437.h: Font0 already has these
# glyphs in its CP437 slots, only the Unicode mapping is missing. No "before"
# string uses them, so the before-ports are unaffected.
GLCD_PROPOSED = {0x00B7: 0xF9, 0x2022: 0x07, 0x00B0: 0xF8}


def glcd_index(cp: int) -> int:
    idx = glcd.glcd_index(cp)
    return idx if idx >= 0 else GLCD_PROPOSED.get(cp, -1)


# =============================================================== text faces

@dataclass(frozen=True)
class Face:
    """A text style: the built-in GLCD font at `scale`, or pack-style glyphs at `px`."""
    kind: str          # "glcd" or "pack"
    scale: int = 1     # glcd only
    px: int = 0        # pack only
    gset: int = 2      # pack glyph set the face stands for (2 text, 3 option, 4 UI)


GLCD1 = Face("glcd", scale=1)          # Renderer::kTextBuiltinSmall
GLCD2 = Face("glcd", scale=2)          # Renderer::kTextBuiltin
GLCD3 = Face("glcd", scale=3)
TEXT = Face("pack", px=TEXT_PX, gset=2)      # pack set 2
OPTION = Face("pack", px=OPTION_PX, gset=3)  # pack set 3
UI_XL = Face("pack", px=32, gset=4)          # proposed UI font, large digits (U3)


class Glyphs:
    """Pack glyphs rendered on demand with the builder's FontRenderer (same
    rasteriser, quantisation and placement rules as a real pack)."""

    def __init__(self, font_path: Path):
        self.font = FontRenderer(font_path)
        self._cache: dict[tuple[int, int, int], tuple[object, list[int]]] = {}
        self._metrics: dict[int, tuple[int, int]] = {}

    def glyph(self, gset: int, cp: int, px: int):
        key = (gset, cp, px)
        if key not in self._cache:
            if gset in (0, 1):
                g = self.font.kanji_glyph(cp, px, gset)
            else:
                g = self.font.text_glyph(cp, px, gset)
            self._cache[key] = (g, unpack_4bpp(g.data, g.width, g.height))
        return self._cache[key]

    def metrics(self, px: int) -> tuple[int, int]:
        """(line_height, ascent) as the builder stores them in the header."""
        if px not in self._metrics:
            self._metrics[px] = self.font.text_metrics(px)
        return self._metrics[px]


# =============================================================== canvas

ICONS = {
    # 1bpp pixel icons for the proposed ui/Icons.h ('#' = ink).
    "check": ["........#", ".......##", "#.....##.", "##...##..", ".##.##...", "..###....",
              "...#....."],
    "cross": ["#.....#", "##...##", ".##.##.", "..###..", ".##.##.", "##...##", "#.....#"],
    "chev_r": ["#...", "##..", ".##.", "..##", ".##.", "##..", "#..."],
    "back": ["...#", "..##", ".###", "####", ".###", "..##", "...#"],
    "tap": [".###.", "#####", "#####", "#####", ".###."],
    "hold": [".#########.", "###########", "###########", "###########", ".#########."],
    "menu": ["#######", ".......", "#######", ".......", "#######"],
    "wifi": [".#######.", "#.......#", "..#####..", ".#.....#.", "...###...", ".........",
             "....#...."],
    "plus": ["..#..", "..#..", "#####", "..#..", "..#.."],
    "down": ["...#...", "...#...", "...#...", "#..#..#", ".#.#.#.", "..###..", "...#...",
             "#######"],
    "radio_on": ["..###..", ".#...#.", "#.###.#", "#.###.#", "#.###.#", ".#...#.", "..###.."],
    "radio_off": ["..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.."],
    "cards": ["..######.", "..#....#.", "######.#.", "#....#.#.", "#....#.#.", "#....###.",
              "#....#...", "######...", "........."],
    "study": [".##...##.", "#..#.#..#", "#...#...#", "#...#...#", "#...#...#", "#...#...#",
              "##..#..##", "..##.##..", "........."],
    "chart": [".......##", ".......##", "....##.##", "....##.##", ".##.##.##", ".##.##.##",
              ".##.##.##", "#########", "........."],
    "wifi9": ["..#####..", ".#.....#.", "#..###..#", "..#...#..", ".#..#..#.", "...#.#...",
              "....#....", "....#....", "........."],
    "gear": ["...#.#...", ".#######.", ".##...##.", "##.....##", ".#..#..#.", "##.....##",
             ".##...##.", ".#######.", "...#.#..."],
    "flame": ["...#...", "..##...", "..###..", ".####.#", ".######", "#######", ".#####."],
}


class Canvas:
    """A W x H RGB565 frame with the drawing primitives the firmware uses."""

    def __init__(self, w: int, h: int, glyphs: Glyphs):
        self.w, self.h = w, h
        self.g = glyphs
        self.img = Image.new("RGB", (w, h), (0, 0, 0))
        self.px = self.img.load()
        self.draw = ImageDraw.Draw(self.img)
        self.clip: tuple[int, int, int, int] | None = None  # x0, y0, x1, y1 (exclusive)

    # ---------------------------------------------------------- primitives
    def put(self, x: int, y: int, c: int) -> None:
        if self.clip and not (self.clip[0] <= x < self.clip[2] and self.clip[1] <= y < self.clip[3]):
            return
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[x, y] = rgb(c)

    def fill(self, c: int) -> None:
        self.rect(0, 0, self.w, self.h, c)

    def rect(self, x: int, y: int, w: int, h: int, c: int) -> None:
        if w > 0 and h > 0:
            self.draw.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb(c))

    def rrect(self, x: int, y: int, w: int, h: int, r: int, c: int) -> None:
        if w > 0 and h > 0:
            self.draw.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=rgb(c))

    def rrect_line(self, x: int, y: int, w: int, h: int, r: int, c: int, width: int = 1) -> None:
        if w > 0 and h > 0:
            self.draw.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r,
                                        outline=rgb(c), width=width)

    def icon(self, name: str, x: int, y: int, c: int) -> int:
        rows = ICONS[name]
        for yy, row in enumerate(rows):
            for xx, ch in enumerate(row):
                if ch == "#":
                    self.put(x + xx, y + yy, c)
        return len(rows[0])

    def bmp4(self, x: int, y: int, bits: list[int], w: int, h: int, fg: int, bg: int) -> None:
        """drawBitmap4bpp: blend against the flat `bg`, skip zero alpha."""
        for row in range(h):
            for col in range(w):
                a = bits[row * w + col]
                if a:
                    self.put(x + col, y + row, blend565(fg, bg, a))

    def bmp1(self, x: int, y: int, data: bytes, w: int, h: int, ink: int) -> None:
        stride = (w + 7) // 8
        for row in range(h):
            for col in range(w):
                if data[row * stride + (col >> 3)] & (0x80 >> (col & 7)):
                    self.put(x + col, y + row, ink)

    # ---------------------------------------------------------- GLCD font
    def glcd_char(self, idx: int, x: int, y: int, scale: int, fg: int) -> None:
        cols = glcd.glyph_columns(idx)
        for cx in range(5):
            for ry in range(8):  # Font0 is 5x8 (bit 7 = descender row)
                if (cols[cx] >> ry) & 1:
                    self.rect(x + cx * scale, y + ry * scale, scale, scale, fg)

    def glcd_text(self, s: str, x: int, y: int, fg: int, scale: int = 1, datum: str = "tl") -> int:
        """smallText()/drawString with Font0; datum tl, tr or tc."""
        w = glcd.GLYPH_W * scale * len(s)
        if datum == "tr":
            x -= w
        elif datum == "tc":
            x -= w // 2
        pen = x
        for ch in s:
            idx = glcd_index(ord(ch))
            if idx >= 0:
                self.glcd_char(idx, pen, y, scale, fg)
            pen += glcd.GLYPH_W * scale
        return w

    # ---------------------------------------------------------- Renderer port
    def line_height(self, face: Face) -> int:
        if face.kind == "glcd":
            return glcd.GLYPH_H * face.scale
        return self.g.metrics(face.px)[0]

    def ascent(self, face: Face) -> int:
        if face.kind == "glcd":
            return glcd.GLYPH_ASCENT * face.scale
        return self.g.metrics(face.px)[1]

    def adv(self, cp: int, face: Face) -> int:
        if face.kind == "glcd":
            return glcd.GLYPH_W * face.scale
        g, _ = self.g.glyph(face.gset, cp, face.px)
        return g.advance

    def text_width(self, s: str, face: Face) -> int:
        return sum(self.adv(ord(ch), face) for ch in s)

    def draw_text(self, s: str, x: int, y: int, fg: int, bg: int, face: Face) -> int:
        """Renderer::drawText: y is the top of the line."""
        pen = x
        for ch in s:
            cp = ord(ch)
            if face.kind == "glcd":
                idx = glcd_index(cp)
                if idx >= 0:
                    self.glcd_char(idx, pen, y, face.scale, fg)
                pen += glcd.GLYPH_W * face.scale
                continue
            g, bits = self.g.glyph(face.gset, cp, face.px)
            self.bmp4(pen + g.x_off, y + self.ascent(face) + g.y_off, bits, g.width, g.height,
                      fg, bg)
            pen += g.advance
        return pen - x

    def wrap(self, s: str, max_w: int, max_lines: int, face: Face) -> list[tuple[str, bool]]:
        """Renderer::wrap: greedy, at spaces or after any CJK character; the
        last line is ellipsised on overflow."""
        lines: list[tuple[str, bool]] = []
        p, n = 0, len(s)
        ell_w = self.text_width("...", face)
        while p < n and len(lines) < max_lines and len(lines) < 8:
            last = len(lines) == max_lines - 1
            q, last_break, after_break, fit_end, ell_end, w, overflow = p, None, None, p, p, 0, False
            while q < n:
                start = q
                cp = ord(s[q])
                q += 1
                a = self.adv(cp, face)
                if w + a > max_w and start != p:
                    overflow = True
                    break
                if cp == 0x20 and w > 0:
                    last_break, after_break = start, q
                w += a
                fit_end = q
                if w + ell_w <= max_w:
                    ell_end = q
                if cp >= 0x2E80:
                    last_break, after_break = q, q
            if not overflow:
                lines.append((s[p:fit_end], False))
                break
            if last:
                lines.append((s[p:ell_end], True))
                break
            use_break = last_break is not None and last_break > p
            lines.append((s[p:last_break if use_break else fit_end], False))
            p = after_break if use_break else fit_end
            while p < n and s[p] == " ":
                p += 1
        return lines

    def draw_wrapped(self, s: str, x: int, y: int, max_w: int, max_lines: int, fg: int, bg: int,
                     face: Face, pitch: int | None = None) -> int:
        lh = pitch or self.line_height(face)
        lines = self.wrap(s, max_w, max_lines, face)
        for i, (t, ell) in enumerate(lines):
            dw = self.draw_text(t, x, y + i * lh, fg, bg, face)
            if ell:
                self.draw_text("...", x + dw, y + i * lh, fg, bg, face)
        return len(lines)

    def draw_centred(self, s: str, x: int, y: int, w: int, h: int, max_lines: int, fg: int,
                     bg: int, face: Face) -> int:
        lines = self.wrap(s, w, max_lines, face)
        lh = self.line_height(face)
        top = y + cdiv(h - len(lines) * lh, 2)
        for i, (t, ell) in enumerate(lines):
            dw = self.text_width(t, face) + (self.text_width("...", face) if ell else 0)
            tx = max(x, x + cdiv(w - dw, 2))
            drawn = self.draw_text(t, tx, top + i * lh, fg, bg, face)
            if ell:
                self.draw_text("...", tx + drawn, top + i * lh, fg, bg, face)
        return len(lines)

    def draw_kanji(self, gset: int, cp: int, bx: int, by: int, bw: int, bh: int, fg: int,
                   bg: int, px: int | None = None) -> None:
        """Renderer::drawKanji: em square centred in the box, baseline = its bottom."""
        px = px or (LARGE_PX if gset == 0 else SMALL_PX)
        g, bits = self.g.glyph(gset, cp, px)
        pen_x = bx + cdiv(bw - px, 2)
        base_y = by + cdiv(bh - px, 2) + px
        self.bmp4(pen_x + g.x_off, base_y + g.y_off, bits, g.width, g.height, fg, bg)

    # ---------------------------------------------------------- proposal helpers
    def cap_height(self, face: Face) -> int:
        if face.kind == "glcd":
            return 7 * face.scale
        g, _ = self.g.glyph(face.gset, ord("H"), face.px)
        return -g.y_off

    def label(self, s: str, x: int, y: int, w: int, h: int, fg: int, bg: int, face: Face,
              align: str = "l") -> int:
        """Proposed Renderer::drawLabel: one line, ellipsised, centred on the cap
        height (not on the font's tall line box). Returns the width drawn."""
        lines = self.wrap(s, w, 1, face)
        if not lines:
            return 0
        t, ell = lines[0]
        shown = t + ("..." if ell else "")
        tw = self.text_width(shown, face)
        tx = x if align == "l" else (x + w - tw if align == "r" else x + (w - tw) // 2)
        if face.kind == "glcd":
            ty = y + (h - 7 * face.scale + 1) // 2
        else:
            ty = y + (h + self.cap_height(face)) // 2 - self.ascent(face)
        self.draw_text(shown, tx, ty, fg, bg, face)
        return tw


# =============================================================== sample data

@dataclass
class Card:
    char: str
    meanings: str  # "; "-separated, as stored in a pack (max 3)
    on: str        # "、"-separated, katakana
    kun: str       # "、"-separated, okurigana marked with '.'
    strokes: int

    @property
    def cp(self) -> int:
        return ord(self.char)


# KANJIDIC2 (CC BY-SA 4.0), English glosses capped at 3 as the builder does.
CARDS = {
    "電": Card("電", "electricity", "デン", "", 13),
    "休": Card("休", "rest; day off; retire", "キュウ", "やす.む、やす.まる、やす.める", 6),
    "体": Card("体", "body; substance; object", "タイ、テイ", "からだ、かたち", 7),
    "木": Card("木", "tree; wood", "ボク、モク", "き、こ", 4),
    "本": Card("本", "book; present; main", "ホン", "もと", 5),
}


@dataclass
class Question:
    card: Card
    mode: str                  # "meaning", "reading" or "kanji" (meaning -> kanji)
    options: list[str]
    correct: int
    sel: int = 0
    revealed: bool = False
    option_cards: list[str] = field(default_factory=list)  # kanji mode
    kind: str = ""             # reading: "on" / "kun"


DECK = "JLPT N5"
BATTERY = 87
REMAINING = 12
SESSION_DONE = 9  # answered this session, for the proposed progress bar


def q_electricity(sel: int = 1, revealed: bool = False) -> Question:
    return Question(CARDS["電"], "meaning", ["rain", "electricity", "snow", "car"], 1, sel, revealed)


def q_rest(sel: int = 0, revealed: bool = False) -> Question:
    return Question(CARDS["休"], "meaning", ["body", "rest", "tree", "book"], 1, sel, revealed)


def q_rest_reading(sel: int = 0) -> Question:
    return Question(CARDS["休"], "reading", ["タイ", "キュウ", "ボク", "ホン"], 1, sel, kind="on")


def q_rest_kanji(sel: int = 2) -> Question:
    return Question(CARDS["休"], "kanji", ["body", "tree", "rest", "book"], 2, sel,
                    option_cards=["体", "木", "休", "本"])


def join_readings(s: str) -> str:
    return "、".join(r.replace(".", "") for r in s.split("、")) if s else ""


def stroke_source(cache: Path):
    """KanjiVG from the cache: the release zip if fetched, else an extracted dir."""
    kvg = cache / "kanjivg"
    zips = sorted(kvg.glob("kanjivg-*.zip"))
    if zips:
        return KanjiVGZip(zips[-1])
    if (kvg / "kanji").is_dir():
        return KanjiVGDir(kvg / "kanji")
    return None


# =============================================================== BEFORE (ports)

class Before:
    """Faithful ports of firmware/src/main.cpp @ 6a5d0b8."""

    K_BLACK, K_WHITE = 0x0000, 0xFFFF
    K_GREY = c565(60, 60, 70)
    K_HIGHLIGHT = c565(30, 60, 150)
    K_GREEN = c565(0, 130, 40)
    K_RED = c565(170, 20, 20)
    K_YELLOW = c565(255, 210, 60)

    def __init__(self, c: Canvas, strokes):
        self.c, self.W, self.H, self.strokes = c, c.w, c.h, strokes

    def small(self, s, x, y, fg, datum="tl"):
        self.c.glcd_text(s, x, y, fg, 1, datum)

    def header(self, title: str, right: str | None = None) -> None:
        self.c.rect(0, 0, self.W, 14, self.K_GREY)
        self.small(title, 3, 3, self.K_WHITE)
        if right is not None:
            self.small(right, self.W - 3, 3, self.K_WHITE, "tr")

    def back_hint(self, s: str = "side: back") -> None:
        tw = self.c.text_width(s, GLCD1)
        self.c.draw_centred(s, self.W - 3 - tw, 3, tw, 8, 1, self.K_WHITE, self.K_GREY, GLCD1)

    def label_font(self, label: str, h: int, min_h: int, w: int) -> Face:
        if h < min_h or self.c.text_width(label, GLCD2) > w:
            return GLCD1
        return GLCD2

    def row(self, label: str, y: int, h: int, sel: bool) -> None:
        bg = self.K_HIGHLIGHT if sel else self.K_BLACK
        self.c.rrect(2, y, self.W - 4, h, 5, bg)
        if sel:
            self.c.rrect_line(2, y, self.W - 4, h, 5, self.K_WHITE)
        face = self.label_font(label, h, 22, self.W - 10)
        self.c.draw_centred(label, 4, y + 1, self.W - 8, h - 2, 1, self.K_WHITE, bg, face)

    # ---------------------------------------------------------- drawQuiz
    def quiz(self, q: Question) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        header_h = 14
        self.header(DECK)
        self.small(f"{BATTERY}%", W - 3, 3, self.K_WHITE, "tr")
        reading, k2m = q.mode == "reading", q.mode == "kanji"
        landscape = W > H
        left_w = max(LARGE_PX, W * 2 // 5)
        col_x = left_w if landscape else 0
        col_w = W - left_w if landscape else W
        kanji_h = min(LARGE_PX + 6, H * 45 // 100)
        if k2m:
            prompt = q.options[q.correct]
            p_w = left_w if landscape else col_w
            if not glcd.can_draw(prompt):
                face = TEXT
            else:
                face = GLCD2 if c.text_width(prompt, GLCD2) <= p_w - 8 else GLCD1
            box_y = header_h if landscape else header_h + 2
            box_h = H - header_h if landscape else kanji_h
            ml = max(1, min(4, cdiv(box_h - 4, c.line_height(face))))
            c.draw_centred(prompt, 3, box_y, p_w - 6, box_h - (2 if landscape else 0), ml,
                           self.K_WHITE, self.K_BLACK, face)
        elif landscape:
            c.draw_kanji(0, q.card.cp, 0, header_h, left_w, H - header_h, self.K_WHITE,
                         self.K_BLACK)
        else:
            c.draw_kanji(0, q.card.cp, 0, header_h + 2, W, kanji_h, self.K_WHITE, self.K_BLACK)
        label = "KANJI" if k2m else ("READING" if reading else "MEANING")
        if landscape:
            self.small(label, left_w // 2, H - 11, self.K_YELLOW, "tc")
        else:
            self.small(label, W // 2, header_h + 2 + kanji_h, self.K_YELLOW, "tc")

        top = header_h + 2 if landscape else header_h + kanji_h + 14
        area = H - top - 2
        margin, gap = 1, 3

        def option_bg(i: int) -> int:
            if q.revealed:
                return self.K_GREEN if i == q.correct else (self.K_RED if i == q.sel else self.K_BLACK)
            return self.K_HIGHLIGHT if i == q.sel else self.K_BLACK

        def option_box(i, x, y, w, h, face, max_lines):
            bg = option_bg(i)
            c.rrect(x, y, w, h, 4, bg)
            if not q.revealed and i == q.sel:
                c.rrect_line(x, y, w, h, 4, self.K_WHITE)
            c.draw_centred(q.options[i], x + 2, y + 1, w - 4, h - 2, max_lines, self.K_WHITE, bg,
                           face)

        n = len(q.options)
        if k2m or reading:
            cols, rows = 2, (n + 1) // 2
            cell_w = cdiv(col_w - 2 * margin - (cols - 1) * gap, cols)
            cell_h = cdiv(area - (rows - 1) * gap, rows)
            face = OPTION
            if reading and any(c.text_width(o, OPTION) > cell_w - 4 for o in q.options):
                face = TEXT
            for i in range(n):
                x = col_x + margin + (i % cols) * (cell_w + gap)
                y = top + (i // cols) * (cell_h + gap)
                if k2m:
                    bg = option_bg(i)
                    c.rrect(x, y, cell_w, cell_h, 4, bg)
                    if not q.revealed and i == q.sel:
                        c.rrect_line(x, y, cell_w, cell_h, 4, self.K_WHITE)
                    c.draw_kanji(1, ord(q.option_cards[i]), x, y, cell_w, cell_h, self.K_WHITE, bg)
                else:
                    option_box(i, x, y, cell_w, cell_h, face, 1)
        else:
            w = col_w - 2 * margin
            face = GLCD2
            if any(c.text_width(o, GLCD2) > w - 4 for o in q.options):
                face = GLCD1
            row_h = area // n
            box_h = row_h - gap
            max_lines = max(1, min(2, cdiv(box_h - 2, c.line_height(face))))
            for i in range(n):
                option_box(i, col_x + margin, top + i * row_h, w, box_h, face, max_lines)

    # ---------------------------------------------------------- drawMenu
    MENU_ROWS = ["Continue", "Decks", "Date", "New/day", "Speed", "Statistics", "Wi-Fi",
                 "Orientation", "Language", "Brightness"]

    def menu(self, sel: int = 1) -> None:
        c, H = self.c, self.H
        c.fill(self.K_BLACK)
        self.header("SETTINGS")
        self.back_hint()
        top, step = 20, 36
        n = len(self.MENU_ROWS)
        if top + step * n > H:
            step = max(10, (H - top - 2) // n)
        pad = 4 if step >= 20 else 1
        row_h = min(step - pad, 32)
        for i, label in enumerate(self.MENU_ROWS):
            self.row(label, top + i * step, row_h, i == sel)

    # ---------------------------------------------------------- drawDecks
    def decks(self, packs: list[str], current: int, sel: int) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        self.header("DECKS")
        self.back_hint()
        top, row_h = 18, 20
        if packs:
            row_h = min(row_h, (H - top - 2) // len(packs))
        row_h = max(row_h, 10)
        for i, name in enumerate(packs):
            y = top + i * row_h
            if y + row_h - 2 > H:
                break
            bg = self.K_HIGHLIGHT if i == sel else self.K_BLACK
            c.rrect(1, y, W - 2, row_h - 2, 4, bg)
            if i == sel:
                c.rrect_line(1, y, W - 2, row_h - 2, 4, self.K_WHITE)
            label = name
            if i == current:
                label = f"* {name}  {REMAINING}/5"
            self.small(label, 4, y + (row_h - 2 - 8) // 2, self.K_WHITE)

    # ---------------------------------------------------------- drawChoice
    def choice(self, title: str, labels: list[str], sel: int) -> None:
        c, H = self.c, self.H
        c.fill(self.K_BLACK)
        self.header(title)
        self.back_hint()
        top, step = 40, 40
        if top + step * len(labels) > H:
            step = max(10, (H - top - 2) // len(labels))
        row_h = min(step - 4, 32)
        for i, label in enumerate(labels):
            self.row(label, top + i * step, row_h, i == sel)

    # ---------------------------------------------------------- drawDate
    def date(self, sel: int = 0) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        self.header("DATE")
        self.back_hint()
        c.draw_centred("2026-10-18", 2, 18, W - 4, 18, 1, self.K_YELLOW, self.K_BLACK, GLCD2)
        rows = ["today (0)", "1 day", "2-7 days", "more (14)"]
        top, row_h = 44, 32
        if top + row_h * 4 > H - 2:
            top, row_h = 38, max(12, (H - 38 - 2) // 4)
        for i, r in enumerate(rows):
            y = top + i * row_h
            bg = self.K_HIGHLIGHT if i == sel else self.K_BLACK
            c.rrect(2, y, W - 4, row_h - 4, 5, bg)
            if i == sel:
                c.rrect_line(2, y, W - 4, row_h - 4, 5, self.K_WHITE)
            face = self.label_font(r, row_h - 4, 22, W - 10)
            c.draw_centred(r, 4, y + 2, W - 8, row_h - 8, 1, self.K_WHITE, bg, face)

    # ---------------------------------------------------------- drawStats
    def stats(self) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        self.header("STATISTICS")
        self.back_hint()
        lines = ["2026-10-18", "streak 3/7", "time 1h 5m", "seen 45 / 79", f"due {REMAINING}  new 5",
                 "answers 312", "accuracy 87%", "learned 12"]
        top, step = 26, 24
        if top + step * len(lines) > H:
            step = max(10, (H - top - 2) // len(lines))
        for i, line in enumerate(lines):
            face = self.label_font(line, step - 2, 18, W - 4)
            c.draw_centred(line, 2, top + i * step, W - 4, step - 4, 1,
                           self.K_YELLOW if i == 0 else self.K_WHITE, self.K_BLACK, face)

    # ---------------------------------------------------------- drawWifi
    def wifi(self) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        self.header("WI-FI")
        self.back_hint()
        lines = ["Tangocho-1A2B", "http://192.168.4.1",
                 "Open the URL on your phone and upload a .tgc", "free 5047 / 6080 KB",
                 "uploaded: 0", "ready"]
        top = 18
        step = max(10, min(22, (H - top - 2) // len(lines)))
        for i, line in enumerate(lines):
            c.draw_centred(line, 2, top + i * step, W - 4, step - 2, 2,
                           self.K_YELLOW if i == 1 else self.K_WHITE, self.K_BLACK, GLCD1)

    # ---------------------------------------------------------- drawDone
    def done(self) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        self.header(DECK, "DONE")
        self.small("All reviewed for now", W // 2, H // 2 - 24, self.K_WHITE, "tc")
        self.small("day 1  45 cards", W // 2, H // 2 - 6, self.K_YELLOW, "tc")
        self.small("due 0  new 0", W // 2, H // 2 + 8, self.K_WHITE, "tc")
        self.small("tap/hold: +10 new", W // 2, H // 2 + 30, self.K_WHITE, "tc")
        self.small("side hold: menu", W // 2, H // 2 + 44, self.K_WHITE, "tc")

    # ---------------------------------------------------------- drawDetail
    def detail(self, card: Card, frame: int, correct: int = 3, total: int = 5) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(self.K_BLACK)
        header_h = 14
        self.header(DECK, "DETAIL")
        landscape = W > H
        if landscape:
            left_w = max(LARGE_PX, W * 2 // 5)
            kx, ky, kw, kh = 0, header_h, left_w, H - header_h
            x, w, top, bottom = left_w + 3, W - left_w - 6, header_h + 2, H - 2
        else:
            kanji_h = min(LARGE_PX + 6, H * 45 // 100)
            kx, ky, kw, kh = 0, header_h + 2, W, kanji_h
            x, w, top, bottom = 3, W - 6, header_h + 2 + kanji_h, H - 2
        masks = self.strokes(card.cp)
        if masks:
            em_x, em_y = kx + cdiv(kw - LARGE_PX, 2), ky + cdiv(kh - LARGE_PX, 2)
            for i, m in enumerate(masks[: frame + 1]):
                c.bmp1(em_x + m.x_off, em_y + m.y_off, m.data, m.w, m.h,
                       self.K_YELLOW if i == frame else self.K_WHITE)
        else:
            c.draw_kanji(0, card.cp, kx, ky, kw, kh, self.K_WHITE, self.K_BLACK)

        lh = c.line_height(TEXT)
        on, kun = join_readings(card.on), join_readings(card.kun)

        def kana_face(s: str) -> Face:
            if c.text_width(s, TEXT) > w and c.text_width(s, OPTION) <= w:
                return OPTION
            return TEXT

        def latin_face(s: str) -> Face:
            return GLCD2 if c.text_width(s, GLCD2) <= w else GLCD1

        reserve = 11 + (8 + lh if on else 0) + (8 + lh if kun else 0) + 11
        meaning_lines = max(1, min(3, cdiv(bottom - top - reserve - 8, lh)))
        y = top
        mface = latin_face(card.meanings)
        self.small("MEANING", x, y, self.K_YELLOW)
        y += 8
        y += c.draw_wrapped(card.meanings, x, y, w, meaning_lines, self.K_WHITE, self.K_BLACK,
                            mface) * c.line_height(mface)
        y += 3
        for label, s in (("ON", on), ("KUN", kun)):
            if not s or y + 8 + lh > bottom:
                continue
            face = kana_face(s)
            self.small(label, x, y, self.K_YELLOW)
            y += 8
            y += c.draw_wrapped(s, x, y, w, 1, self.K_WHITE, self.K_BLACK, face) * c.line_height(face)
            y += 3
        if y + 11 <= bottom:
            self.small(f"strokes {card.strokes}", x, y, self.K_WHITE)
            y += 11
        if y + 11 <= bottom:
            self.small(f"correct {correct}/{total}", x, y, self.K_GREEN)


# =============================================================== AFTER (proposal)

class T:
    """Design tokens (docs/ux-refresh.md, "Colour tokens"). RGB565 values."""
    BG = c565(0, 0, 0)
    SURFACE = c565(24, 28, 36)
    SURFACE_HI = c565(40, 46, 60)
    LINE = c565(56, 64, 80)
    GHOST = c565(44, 50, 64)
    TEXT = c565(255, 255, 255)
    TEXT_DIM = c565(164, 172, 188)
    TEXT_FAINT = c565(100, 108, 124)
    ACCENT = c565(40, 96, 216)
    ACCENT_PRESS = c565(96, 156, 255)
    ACCENT_HI = c565(120, 170, 255)
    OK = c565(0, 128, 64)
    OK_HI = c565(90, 220, 140)
    OK_BG = c565(0, 48, 24)
    ERR = c565(200, 40, 40)
    ERR_HI = c565(255, 110, 110)
    ERR_BG = c565(56, 16, 16)
    WARN = c565(255, 196, 48)
    MODE_READING = c565(110, 220, 200)
    MODE_KANJI = c565(200, 160, 255)

    TABLE = [  # name, value, role (for --tokens and the doc)
        ("bg", "BG", "screen background; glyph blend background"),
        ("surface", "SURFACE", "idle tile / row"),
        ("surface-hi", "SURFACE_HI", "pills, progress track"),
        ("line", "LINE", "hairlines, wrong-tile outline base"),
        ("ghost", "GHOST", "unwritten strokes on the detail card"),
        ("text", "TEXT", "primary text, kanji"),
        ("text-dim", "TEXT_DIM", "secondary text, header titles, hint labels"),
        ("text-faint", "TEXT_FAINT", "disabled, dimmed options after reveal"),
        ("accent", "ACCENT", "focus fill (white text on it)"),
        ("accent-press", "ACCENT_PRESS", "hold-progress fill (transient)"),
        ("accent-hi", "ACCENT_HI", "accent text/icons on black, meaning mode"),
        ("ok", "OK", "correct tile fill (white text on it)"),
        ("ok-hi", "OK_HI", "correct text/icons on black"),
        ("ok-bg", "OK_BG", "correct result pill"),
        ("err", "ERR", "wrong-tile outline"),
        ("err-hi", "ERR_HI", "wrong text/icons on black"),
        ("err-bg", "ERR_BG", "wrong-tile fill, wrong result pill"),
        ("warn", "WARN", "current stroke, low battery, date notice, Hard"),
        ("mode-reading", "MODE_READING", "reading-mode pill text"),
        ("mode-kanji", "MODE_KANJI", "kanji-mode pill text"),
    ]


GUT = 4         # screen gutter
GAP = 4         # gap between tiles/rows
RADIUS = 4      # tile corner radius
HEADER_H = 16   # header incl. the 2 px progress bar / hairline
HINT_H = 12     # hint bar
UI_PITCH = 20   # line pitch for one-script Latin/kana lines of the 18 px text face


class After:
    def __init__(self, c: Canvas, strokes):
        self.c, self.W, self.H, self.strokes = c, c.w, c.h, strokes

    # ---------------------------------------------------------- components
    def battery(self, x: int, y: int, pct: int) -> None:
        c = self.c
        col = T.TEXT_DIM if pct >= 20 else (T.WARN if pct >= 10 else T.ERR_HI)
        c.rrect_line(x, y, 12, 7, 1, col)
        c.rect(x + 12, y + 2, 1, 3, col)
        c.rect(x + 2, y + 2, max(1, round(8 * pct / 100)), 3, col)

    def header(self, left: str | None = None, *, pill: tuple[str, int, int] | None = None,
               icon: tuple[str, int] | None = None, right: str | None = None,
               progress: float | None = None, wifi: bool = False) -> None:
        """Status bar: title or pill left, optional counter/wifi + battery right,
        a 2 px progress bar (quiz) or a hairline at the bottom."""
        c, W = self.c, self.W
        c.rect(0, 0, W, HEADER_H, T.BG)
        x = GUT
        if pill:
            text, fg, bg = pill
            iw = 0
            if icon:
                iw = len(ICONS[icon[0]][0]) + 3
            pw = c.text_width(text, GLCD1) + 8 + iw
            c.rrect(x, 2, pw, 11, 3, bg)
            px = x + 4
            if icon:
                c.icon(icon[0], px, 4 if len(ICONS[icon[0]]) >= 7 else 5, icon[1])
                px += iw
            c.glcd_text(text, px, 4, fg)
        elif left:
            c.glcd_text(left, x, 4, T.TEXT_DIM)
        bx = W - GUT - 13
        self.battery(bx, 4, BATTERY)
        rx = bx - 5
        if wifi:
            rx -= c.icon("wifi", rx - 9, 4, T.ACCENT_HI) + 1
            rx -= 4
        if right:
            c.glcd_text(right, rx, 4, T.TEXT_DIM, 1, "tr")
        if progress is not None:
            c.rect(0, 14, W, 2, T.SURFACE_HI)
            c.rect(0, 14, round(W * progress), 2, T.ACCENT)
        else:
            c.rect(0, 15, W, 1, T.LINE)

    def hint(self, items: list[tuple[str, str]], side: tuple[str, str] | None = None,
             strong: int = -1) -> None:
        """Hint bar: front-button actions left (dot = tap, bar = hold), the side
        button's action right-aligned. `strong` brightens one item (held)."""
        c, W, H = self.c, self.W, self.H
        y = H - HINT_H
        c.rect(0, y, W, HINT_H, T.BG)
        x = GUT
        for i, (ic, label) in enumerate(items):
            col_i = T.TEXT if i != strong else T.ACCENT_HI
            col_l = T.TEXT_DIM if i != strong else T.TEXT
            rows = ICONS[ic]
            x += c.icon(ic, x, y + 2 + (7 - len(rows)) // 2, col_i) + 3
            x += c.glcd_text(label, x, y + 2, col_l) + 6
        if side:
            ic, label = side
            tw = c.text_width(label, GLCD1) if label else 0
            iw = len(ICONS[ic][0])
            sx = W - GUT - tw - (iw + 3 if label else iw)
            rows = ICONS[ic]
            c.icon(ic, sx, y + 2 + (7 - len(rows)) // 2, T.TEXT)
            if label:
                c.glcd_text(label, sx + iw + 3, y + 2, T.TEXT_DIM)

    def tile(self, x, y, w, h, state: str) -> int:
        """Option tile. States: idle, focus, ok, wrong, dim. Returns the text colour."""
        c = self.c
        if state == "focus":
            c.rrect(x, y, w, h, RADIUS, T.ACCENT)
            return T.TEXT
        if state == "ok":
            c.rrect(x, y, w, h, RADIUS, T.OK)
            return T.TEXT
        if state == "wrong":
            c.rrect(x, y, w, h, RADIUS, T.ERR_BG)
            c.rrect_line(x, y, w, h, RADIUS, T.ERR, 2)
            return T.TEXT
        if state == "dim":
            return T.TEXT_FAINT
        c.rrect(x, y, w, h, RADIUS, T.SURFACE)
        return T.TEXT

    @staticmethod
    def tile_bg(state: str) -> int:
        return {"focus": T.ACCENT, "ok": T.OK, "wrong": T.ERR_BG, "dim": T.BG}.get(state,
                                                                                T.SURFACE)

    def result_icon(self, x, y, w, h, state: str) -> None:
        if state == "ok":
            self.c.icon("check", x + w - 9 - 6, y + (h - 7) // 2, T.TEXT)
        elif state == "wrong":
            self.c.icon("cross", x + w - 7 - 7, y + (h - 7) // 2, T.ERR_HI)

    def row(self, x, y, w, h, label: str, *, sel=False, value: str = "", icon: str | None = None,
            chevron=False, label_col: int | None = None, radio: bool | None = None) -> None:
        """List row: optional icon/radio, label (text face), right value, chevron."""
        c = self.c
        bg = T.ACCENT if sel else T.SURFACE
        c.rrect(x, y, w, h, RADIUS, bg)
        lx = x + 6
        if radio is not None:
            c.icon("radio_on" if radio else "radio_off", lx, y + (h - 7) // 2,
                   T.TEXT if sel else (T.ACCENT_HI if radio else T.TEXT_FAINT))
            lx += 7 + 6
        if icon:
            c.icon(icon, lx, y + (h - 9) // 2, T.TEXT if sel else T.TEXT_DIM)
            lx += 9 + 5
        rx = x + w - 6
        if chevron:
            c.icon("chev_r", rx - 4, y + (h - 7) // 2, T.TEXT if sel else T.TEXT_FAINT)
            rx -= 4 + 4
        if value:
            vw = c.text_width(value, GLCD1)
            c.glcd_text(value, rx - vw, y + (h - 7) // 2, T.TEXT if sel else T.TEXT_DIM)
            rx -= vw + 5
        c.label(label, lx, y, rx - lx, h, label_col if label_col is not None else T.TEXT, bg, TEXT)

    def pill(self, text: str, x: int, y: int, fg: int, bg: int, align: str = "l") -> int:
        c = self.c
        w = c.text_width(text, GLCD1) + 6
        if align == "c":
            x -= w // 2
        elif align == "r":
            x -= w
        c.rrect(x, y, w, 11, 3, bg)
        c.glcd_text(text, x + 3, y + 2, fg)
        return w

    def scrollbar(self, x: int, y: int, h: int, first: int, shown: int, total: int) -> None:
        if total <= shown:
            return
        self.c.rect(x, y, 2, h, T.SURFACE)
        th = max(6, h * shown // total)
        ty = y + (h - th) * first // max(1, total - shown)
        self.c.rect(x, ty, 2, th, T.TEXT_FAINT)

    def mode_pill(self, q: Question) -> tuple[str, int, int]:
        if q.mode == "reading":
            return ("READING " + q.kind.upper(), T.MODE_READING, T.SURFACE_HI)
        if q.mode == "kanji":
            return ("KANJI", T.MODE_KANJI, T.SURFACE_HI)
        return ("MEANING", T.ACCENT_HI, T.SURFACE_HI)

    # ---------------------------------------------------------- quiz
    def quiz(self, q: Question, *, hold: float | None = None, grade: str = "GOOD",
             interval: str = "4d", hints: bool = True) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(T.BG)
        landscape = W > H
        progress = (SESSION_DONE + (1 if q.revealed else 0)) / (SESSION_DONE + REMAINING)
        if q.revealed:
            ok = q.sel == q.correct
            pill = ((f"{grade} · {interval}", T.OK_HI, T.OK_BG) if ok else
                    ("AGAIN", T.ERR_HI, T.ERR_BG))
            icon = ("check", T.OK_HI) if ok else ("cross", T.ERR_HI)
            self.header(pill=pill, icon=icon, right=str(REMAINING), progress=progress)
        else:
            self.header(pill=self.mode_pill(q), right=str(REMAINING), progress=progress)

        bottom = H - (HINT_H if hints else 2)
        if landscape:
            left_w = 100
            prompt_box = (0, HEADER_H, left_w, bottom - HEADER_H)
            ox, ow, oy, oh = left_w + GUT, W - left_w - 2 * GUT, HEADER_H + 3, bottom - HEADER_H - 4
        else:
            kanji_h = LARGE_PX + 6
            prompt_box = (0, HEADER_H, W, kanji_h)
            ox, ow, oy = GUT, W - 2 * GUT, HEADER_H + kanji_h + 2
            oh = bottom - oy - 4
        bx, by, bw, bh = prompt_box
        if q.mode == "kanji":
            # Say what is asked: a caption above the meaning prompt.
            c.glcd_text("which kanji means", bx + bw // 2, by + bh // 2 - 22, T.TEXT_FAINT, 1, "tc")
            c.draw_centred(q.options[q.correct], bx + 8, by + bh // 2 - 10, bw - 16, 28, 2, T.TEXT,
                           T.BG, TEXT)
        else:
            c.draw_kanji(0, q.card.cp, bx, by, bw, bh, T.TEXT, T.BG)

        def state(i: int) -> str:
            if q.revealed:
                if i == q.correct:
                    return "ok"
                return "wrong" if i == q.sel else "dim"
            return "focus" if i == q.sel else "idle"

        n = len(q.options)
        if q.mode in ("reading", "kanji"):
            cw, ch = (ow - GAP) // 2, (oh - GAP) // 2
            for i in range(n):
                x, y = ox + (i % 2) * (cw + GAP), oy + (i // 2) * (ch + GAP)
                st = state(i)
                fg = self.tile(x, y, cw, ch, st)
                bg = self.tile_bg(st)
                if q.mode == "kanji":
                    c.draw_kanji(1, ord(q.option_cards[i]), x, y, cw, ch, fg, bg)
                else:
                    self.hold_fill(x, y, cw, ch, hold if st == "focus" else None, lambda b:
                                   c.label(q.options[i], x + 2, y, cw - 4, ch, fg, b, OPTION, "c"),
                                   bg)
                if st == "wrong":
                    c.icon("cross", x + cw - 11, y + 4, T.ERR_HI)
                if st == "ok":
                    c.icon("check", x + cw - 13, y + 4, T.TEXT)
        else:
            th = (oh - (n - 1) * GAP) // n
            for i in range(n):
                y = oy + i * (th + GAP)
                st = state(i)
                fg = self.tile(ox, y, ow, th, st)
                self.hold_fill(ox, y, ow, th, hold if st == "focus" else None, lambda b:
                               c.label(q.options[i], ox + 8, y, ow - 8 - 20, th, fg, b, TEXT),
                               self.tile_bg(st))
                self.result_icon(ox, y, ow, th, st)

        if hints:
            if q.revealed:
                ok = q.sel == q.correct
                self.hint([("tap", "next" if ok else "details")], ("menu", ""))
            else:
                self.hint([("tap", "next"), ("hold", "answer")], ("menu", ""),
                          strong=1 if hold is not None else -1)

    def hold_fill(self, x, y, w, h, frac: float | None, draw_label, bg: int) -> None:
        """Hold progress: the focused tile fills with accent-press from the left
        while the button is down; the label is drawn twice, clipped to each part,
        so its anti-aliasing blends against the right background (setClipRect on
        device)."""
        if frac is None:
            draw_label(bg)
            return
        split = x + round(w * frac)
        self.c.rrect(x, y, w, h, RADIUS, T.ACCENT_PRESS)
        if split < x + w:  # the rest keeps the focus colour, square edge at the split
            self.c.draw.rounded_rectangle([split, y, x + w - 1, y + h - 1], radius=RADIUS,
                                          fill=rgb(bg), corners=(False, True, True, False))
        for x0, x1, b in ((x, split, T.ACCENT_PRESS), (split, x + w, bg)):
            self.c.clip = (x0, y, x1, y + h)
            draw_label(b)
        self.c.clip = None

    # ---------------------------------------------------------- detail
    def detail(self, card: Card, frame: int, picked: str) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        masks = self.strokes(card.cp)
        self.header("DETAIL", right=f"stroke {frame + 1}/{len(masks)}" if masks else None)
        kanji_h = LARGE_PX + 6
        kx, ky, kw, kh = 0, HEADER_H, W, kanji_h
        if masks:
            # Ghost of the whole kanji, then the written strokes, current in amber.
            c.draw_kanji(0, card.cp, kx, ky, kw, kh, T.GHOST, T.BG)
            em_x, em_y = kx + (kw - LARGE_PX) // 2, ky + (kh - LARGE_PX) // 2
            for i, m in enumerate(masks[: frame + 1]):
                c.bmp1(em_x + m.x_off, em_y + m.y_off, m.data, m.w, m.h,
                       T.WARN if i == frame else T.TEXT)
        else:
            c.draw_kanji(0, card.cp, kx, ky, kw, kh, T.TEXT, T.BG)

        x, w = GUT, W - 2 * GUT
        y = HEADER_H + kanji_h + 2
        # Your pick vs the card's own record, in one meta line (the stroke count
        # is in the header counter).
        c.icon("cross", x, y + 1, T.ERR_HI)
        c.glcd_text(picked, x + 10, y + 1, T.ERR_HI)
        c.glcd_text("3/5 correct", W - GUT, y + 1, T.TEXT_DIM, 1, "tr")
        y += 13
        n = c.draw_wrapped(card.meanings, x, y, w, 2, T.TEXT, T.BG, TEXT, pitch=UI_PITCH)
        y += n * UI_PITCH + 4
        for tag, s in (("ON", join_readings(card.on)), ("KUN", join_readings(card.kun))):
            if not s:
                continue
            self.pill(tag, x, y + 4, T.TEXT_DIM, T.SURFACE_HI)
            c.label(s, x + 28, y, w - 28, UI_PITCH, T.TEXT, T.BG, TEXT)
            y += UI_PITCH + 3
        self.hint([("tap", "next")], ("menu", ""))

    # ---------------------------------------------------------- settings home
    HOME = [("cards", "Decks", "3"), ("study", "Study", "10/day"), ("chart", "Stats", "87%"),
            ("wifi9", "Wi-Fi", "off"), ("gear", "Device", "")]

    def summary(self, x: int, y: int, w: int, h: int, stacked: bool = False) -> None:
        """At-a-glance card on the settings home: deck, due/new, coverage."""
        c = self.c
        c.rrect(x, y, w, h, RADIUS, T.SURFACE)
        c.label(DECK, x + 6, y + 3, w - 12, 20, T.TEXT, T.SURFACE, TEXT)
        due = f"{REMAINING} due"
        if stacked:
            yy = y + 28
            c.glcd_text(due, x + 6, yy, T.ACCENT_HI)
            c.glcd_text("5 new", x + 6, yy + 12, T.TEXT_DIM)
            c.glcd_text("seen 45/79", x + 6, yy + 24, T.TEXT_DIM)
            c.rect(x + 6, yy + 36, w - 12, 3, T.SURFACE_HI)
            c.rect(x + 6, yy + 36, (w - 12) * 45 // 79, 3, T.ACCENT)
            return
        c.glcd_text("45/79", x + w - 6, y + 9, T.TEXT_FAINT, 1, "tr")
        c.glcd_text(due, x + 6, y + 25, T.ACCENT_HI)
        c.glcd_text("· 5 new", x + 6 + c.text_width(due + " ", GLCD1), y + 25, T.TEXT_DIM)
        c.rect(x + 6, y + h - 8, w - 12, 3, T.SURFACE_HI)
        c.rect(x + 6, y + h - 8, (w - 12) * 45 // 79, 3, T.ACCENT)

    def home(self, sel: int = 0) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(T.BG)
        self.header("SETTINGS", right="18 Oct")
        if W > H:
            lw = 92
            self.summary(GUT, HEADER_H + 3, lw, H - HEADER_H - HINT_H - 5, stacked=True)
            rx, rw = GUT + lw + GAP, W - lw - 2 * GUT - GAP - 4
            rh, shown = 22, 4
            for i in range(shown):
                ic, label, value = self.HOME[i]
                self.row(rx, HEADER_H + 3 + i * (rh + GAP), rw, rh, label, sel=i == sel,
                         value=value, icon=ic, chevron=True)
            self.scrollbar(W - GUT - 1, HEADER_H + 3, shown * (rh + GAP) - GAP, 0, shown,
                           len(self.HOME))
        else:
            self.summary(GUT, HEADER_H + 4, W - 2 * GUT, 44)
            y0, rh = HEADER_H + 4 + 44 + 6, 26
            for i, (ic, label, value) in enumerate(self.HOME):
                self.row(GUT, y0 + i * (rh + GAP), W - 2 * GUT, rh, label, sel=i == sel,
                         value=value, icon=ic, chevron=True)
        self.hint([("tap", "move"), ("hold", "open")], ("back", "back"))

    # ---------------------------------------------------------- choice
    def choice(self) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("STUDY › NEW/DAY".replace("›", ">"))
        c.draw_wrapped("New kanji added to each day's reviews.", GUT, HEADER_H + 4, W - 2 * GUT,
                       2, T.TEXT_DIM, T.BG, GLCD1, pitch=10)
        y0, rh = HEADER_H + 30, 26
        values, saved, focus = ["3", "5", "10", "20"], 2, 3
        for i, v in enumerate(values):
            self.row(GUT, y0 + i * (rh + GAP), W - 2 * GUT, rh, v, sel=i == focus,
                     radio=i == saved, value="default" if i == saved else "")
        self.hint([("tap", "move"), ("hold", "set")], ("back", "back"))

    # ---------------------------------------------------------- date stepper
    def date(self, days: int = 1) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("DATE")
        c.glcd_text("Device was off.", W // 2, HEADER_H + 5, T.WARN, 1, "tc")
        c.glcd_text("Last used Sat 17 Oct", W // 2, HEADER_H + 16, T.TEXT_DIM, 1, "tc")
        # Calendar tile: month band, big day number (UI-XL digits), weekday.
        tw, th = 76, 84
        tx, ty = (W - tw) // 2, HEADER_H + 32
        c.rrect(tx, ty, tw, th, 6, T.SURFACE)
        c.rrect(tx, ty, tw, 16, 6, T.ACCENT)
        c.rect(tx, ty + 10, tw, 6, T.ACCENT)
        c.glcd_text("OCT 2026", tx + tw // 2, ty + 4, T.TEXT, 1, "tc")
        c.label("18", tx, ty + 18, tw, 44, T.TEXT, T.SURFACE, UI_XL, "c")
        c.label("Sunday", tx, ty + 60, tw, 20, T.TEXT_DIM, T.SURFACE, TEXT, "c")
        self.pill(f"+{days} day" if days == 1 else f"+{days} days", W // 2, ty + th + 8,
                  T.WARN, T.SURFACE_HI, "c")
        c.glcd_text("Tap adds a day,", W // 2, ty + th + 26, T.TEXT_FAINT, 1, "tc")
        c.glcd_text("hold when it's today.", W // 2, ty + th + 36, T.TEXT_FAINT, 1, "tc")
        self.hint([("tap", "+1 day"), ("hold", "confirm")], ("back", ""))

    # ---------------------------------------------------------- decks
    def decks(self, sel: int = 3) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("DECKS")
        rows = [("JLPT N5", "EN", f"79 kanji · {REMAINING} due", True),
                ("JLPT N5", "ES", "79 kanji", False),
                ("JLPT N4", "ES", "167 kanji", False)]
        y, rh = HEADER_H + 4, 36
        for i, (name, lang, meta, loaded) in enumerate(rows):
            s = i == sel
            bg = T.ACCENT if s else T.SURFACE
            c.rrect(GUT, y, W - 2 * GUT, rh, RADIUS, bg)
            c.icon("radio_on" if loaded else "radio_off", GUT + 7, y + 7,
                   T.TEXT if s else (T.ACCENT_HI if loaded else T.TEXT_FAINT))
            nw = c.label(name, GUT + 20, y + 1, W - 2 * GUT - 50, 20, T.TEXT, bg, TEXT)
            self.pill(lang, GUT + 20 + nw + 5, y + 5, T.TEXT_DIM, T.SURFACE_HI)
            c.glcd_text(meta, GUT + 20, y + 23, T.TEXT if s else T.TEXT_DIM)
            y += rh + GAP
        # "Get decks" (home Wi-Fi catalog) closes the list.
        s = sel == len(rows)
        bg = T.ACCENT if s else T.BG
        c.rrect(GUT, y, W - 2 * GUT, 26, RADIUS, bg)
        if not s:
            c.rrect_line(GUT, y, W - 2 * GUT, 26, RADIUS, T.LINE)
        c.icon("down", GUT + 7, y + 9, T.TEXT if s else T.ACCENT_HI)
        c.label("Get decks", GUT + 20, y, W - 2 * GUT - 28, 26, T.TEXT if s else T.ACCENT_HI, bg,
                TEXT)
        c.icon("chev_r", W - GUT - 12, y + 10, T.TEXT if s else T.TEXT_FAINT)
        self.hint([("tap", "move"), ("hold", "open")], ("back", "back"))

    # ---------------------------------------------------------- get decks
    def get_decks(self) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("GET DECKS", wifi=True)
        c.glcd_text("Home Wi-Fi · 4 decks", GUT, HEADER_H + 4, T.TEXT_DIM)
        # Catalog lines carry name, lang, cards and bytes (tangocho-pack catalog).
        rows = [("n3-es", "ES · 367 kanji", "1.9 MB", "dl", 0.45),
                ("n4-en", "EN · 167 kanji", "0.9 MB", "", 0),
                ("n4-es", "ES · on device", "", "have", 0),
                ("n5-es", "ES · on device", "", "have", 0)]
        y, rh = HEADER_H + 16, 36
        for i, (name, meta, size, st, frac) in enumerate(rows):
            s = i == 0
            have = st == "have"
            bg = T.ACCENT if s else (T.BG if have else T.SURFACE)
            c.rrect(GUT, y, W - 2 * GUT, rh, RADIUS, bg)
            if have:
                c.rrect_line(GUT, y, W - 2 * GUT, rh, RADIUS, T.SURFACE_HI)
            col = T.TEXT_FAINT if have else T.TEXT
            c.label(name, GUT + 6, y + 1, W - 2 * GUT - 50, 20, col, bg, TEXT)
            if st == "dl":
                c.glcd_text(f"{round(frac * 100)}%", W - GUT - 6, y + 7, T.TEXT, 1, "tr")
                bx, bw = GUT + 6, W - 2 * GUT - 12
                c.rect(bx, y + 25, bw, 4, T.SURFACE_HI)
                c.rect(bx, y + 25, round(bw * frac), 4, T.TEXT)
            else:
                c.glcd_text(meta, GUT + 6, y + 23, T.TEXT_FAINT if have else T.TEXT_DIM)
                if have:
                    c.icon("check", W - GUT - 15, y + 8, T.OK_HI)
                else:
                    c.glcd_text(size, W - GUT - 6, y + 8, T.TEXT_DIM, 1, "tr")
            y += rh + GAP
        self.hint([("tap", "move"), ("hold", "get")], ("back", "back"))

    # ---------------------------------------------------------- stats
    def stats(self) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("STATS")
        # Accuracy ring (fillArc on device) with the value inside.
        cx, cy, r = W // 2, HEADER_H + 34, 28
        box = [cx - r, cy - r, cx + r, cy + r]
        c.draw.ellipse(box, outline=rgb(T.SURFACE_HI), width=6)
        c.draw.arc(box, start=-90, end=-90 + round(360 * 0.87), fill=rgb(T.ACCENT_HI), width=6)
        c.label("87%", cx - r, cy - 10, 2 * r, 20, T.TEXT, T.BG, TEXT, "c")
        c.glcd_text("accuracy", cx, cy + r + 4, T.TEXT_DIM, 1, "tc")
        tiles = [("3", "streak", "flame", T.WARN), ("7", "best", None, 0),
                 (str(REMAINING), "due", None, 0), ("5", "new", None, 0),
                 ("12", "learned", None, 0), ("1h 5m", "time", None, 0)]
        tw, th = (W - 2 * GUT - GAP) // 2, 34
        y0 = cy + r + 16
        for i, (v, label, ic, icol) in enumerate(tiles):
            x, y = GUT + (i % 2) * (tw + GAP), y0 + (i // 2) * (th + GAP)
            c.rrect(x, y, tw, th, RADIUS, T.SURFACE)
            vx = x + 6
            if ic:
                vx += c.icon(ic, vx, y + 7, icol) + 4
            c.label(v, vx, y + 1, tw - (vx - x) - 1, 20, T.TEXT, T.SURFACE, TEXT)
            c.glcd_text(label, x + 6, y + 22, T.TEXT_DIM)
        yb = y0 + 3 * (th + GAP)
        c.glcd_text("seen", GUT, yb, T.TEXT_DIM)
        c.glcd_text("45/79", W - GUT, yb, T.TEXT_DIM, 1, "tr")
        c.rect(GUT + 30, yb + 2, W - 2 * GUT - 66, 4, T.SURFACE_HI)
        c.rect(GUT + 30, yb + 2, (W - 2 * GUT - 66) * 45 // 79, 4, T.ACCENT)
        self.hint([], ("back", "back"))

    # ---------------------------------------------------------- wi-fi
    def qr(self, text: str, x: int, y: int, mod: int = 2) -> int:
        """Illustrative QR-like block (finder patterns + hashed modules). The
        device would draw a real code with LovyanGFX's qrcode()."""
        n, quiet = 25, 2
        size = (n + 2 * quiet) * mod
        self.c.rect(x, y, size, size, T.TEXT)
        bits = hashlib.sha256(text.encode()).digest() * 3

        def finder(fx: int, fy: int) -> bool:
            return 0 <= fx < 7 and 0 <= fy < 7 and (fx in (0, 6) or fy in (0, 6) or
                                                     (2 <= fx <= 4 and 2 <= fy <= 4))
        k = 0
        for my in range(n):
            for mx in range(n):
                corners = [(0, 0), (n - 7, 0), (0, n - 7)]
                in_f = [(mx - ax, my - ay) for ax, ay in corners
                        if -1 <= mx - ax <= 7 and -1 <= my - ay <= 7]
                if in_f:
                    on = finder(*in_f[0])
                elif mx == 6 or my == 6:
                    on = (mx + my) % 2 == 0
                else:
                    on = (bits[k // 8] >> (k % 8)) & 1 == 1
                    k += 1
                if on:
                    self.c.rect(x + (quiet + mx) * mod, y + (quiet + my) * mod, mod, mod, T.BG)
        return size

    def wifi(self) -> None:
        c, W = self.c, self.W
        c.fill(T.BG)
        self.header("WI-FI", wifi=True)
        y = HEADER_H + 4
        size = self.qr("WIFI:T:nopass;S:Tangocho-1A2B;;", GUT, y)
        self.qr("http://192.168.4.1", W - GUT - size, y)
        for i, (n, label) in enumerate((("1", "join"), ("2", "open"))):
            x = GUT if i == 0 else W - GUT - size
            c.rrect(x + size // 2 - 22, y + size + 3, 11, 11, 5, T.ACCENT)
            c.glcd_text(n, x + size // 2 - 19, y + size + 5, T.TEXT)
            c.glcd_text(label, x + size // 2 - 8, y + size + 5, T.TEXT_DIM)
        y += size + 20
        # The SSID is picked from the phone's list (recognised: small is fine);
        # the URL may have to be typed (large).
        c.glcd_text("network", GUT, y, T.TEXT_FAINT)
        c.glcd_text("Tangocho-1A2B", W - GUT, y, T.TEXT, 1, "tr")
        c.glcd_text("then open", GUT, y + 13, T.TEXT_FAINT)
        c.label("192.168.4.1", GUT, y + 23, W - 2 * GUT, 20, T.ACCENT_HI, T.BG, TEXT, "c")
        y += 50
        c.rrect(GUT, y, W - 2 * GUT, 46, RADIUS, T.SURFACE)
        c.glcd_text("free", GUT + 6, y + 6, T.TEXT_DIM)
        c.glcd_text("4.9 / 5.9 MB", W - GUT - 6, y + 6, T.TEXT_DIM, 1, "tr")
        c.rect(GUT + 6, y + 17, W - 2 * GUT - 12, 3, T.SURFACE_HI)
        c.rect(GUT + 6, y + 17, (W - 2 * GUT - 12) * 49 // 59, 3, T.ACCENT)
        c.icon("check", GUT + 6, y + 29, T.OK_HI)
        c.glcd_text("time synced 09:12", GUT + 18, y + 29, T.TEXT)
        self.hint([], ("back", "stop"))

    # ---------------------------------------------------------- done
    def done(self) -> None:
        c, W, H = self.c, self.W, self.H
        c.fill(T.BG)
        self.header(DECK)
        cx, cy, r = W // 2, HEADER_H + 36, 22
        c.draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=rgb(T.OK_BG),
                       outline=rgb(T.OK), width=2)
        for yy, row in enumerate(ICONS["check"]):  # the 9x7 check icon at 3x
            for xx, ch in enumerate(row):
                if ch == "#":
                    c.rect(cx - 13 + xx * 3, cy - 10 + yy * 3, 3, 3, T.OK_HI)
        c.label("All caught up", 0, cy + r + 8, W, 22, T.TEXT, T.BG, TEXT, "c")
        c.glcd_text("tomorrow: 8 due", W // 2, cy + r + 34, T.TEXT_DIM, 1, "tc")
        fw = c.icon("flame", W // 2 - 40, cy + r + 49, T.WARN)
        c.glcd_text("3-day streak", W // 2 - 40 + fw + 4, cy + r + 49, T.WARN)
        by = H - HINT_H - 34
        c.rrect(GUT, by, W - 2 * GUT, 28, RADIUS, T.ACCENT)
        c.label("+10 new", GUT, by, W - 2 * GUT, 28, T.TEXT, T.ACCENT, TEXT, "c")
        self.hint([("hold", "study more")], ("menu", ""))


# =============================================================== screens

PORTRAIT, LANDSCAPE = (135, 240), (240, 135)


@dataclass
class Screen:
    name: str
    title: str
    size: tuple[int, int]
    before: object | None  # callable(Before) or None (new screen)
    after: object | None   # callable(After)


SCREENS = [
    Screen("quiz-meaning", "Quiz, kanji → meaning", PORTRAIT,
           lambda b: b.quiz(q_electricity()), lambda a: a.quiz(q_electricity())),
    Screen("quiz-hold", "Holding to answer (new)", PORTRAIT,
           None, lambda a: a.quiz(q_electricity(), hold=0.6)),
    Screen("quiz-reading-landscape", "Quiz, kanji → reading, landscape", LANDSCAPE,
           lambda b: b.quiz(q_rest_reading()), lambda a: a.quiz(q_rest_reading())),
    Screen("quiz-kanji", "Quiz, meaning → kanji", PORTRAIT,
           lambda b: b.quiz(q_rest_kanji()), lambda a: a.quiz(q_rest_kanji())),
    Screen("feedback-correct", "Correct answer", PORTRAIT,
           lambda b: b.quiz(q_electricity(1, True)), lambda a: a.quiz(q_electricity(1, True))),
    Screen("feedback-wrong", "Wrong answer", PORTRAIT,
           lambda b: b.quiz(q_rest(0, True)), lambda a: a.quiz(q_rest(0, True))),
    Screen("detail", "Mistake detail card (stroke 4 of 6)", PORTRAIT,
           lambda b: b.detail(CARDS["休"], 3), lambda a: a.detail(CARDS["休"], 3, "body")),
    Screen("settings-home", "Settings home", PORTRAIT,
           lambda b: b.menu(1), lambda a: a.home(0)),
    Screen("settings-home-landscape", "Settings home, landscape", LANDSCAPE,
           lambda b: b.menu(1), lambda a: a.home(0)),
    Screen("choice", "Choice picker (new cards per day)", PORTRAIT,
           lambda b: b.choice("NEW/DAY", ["3", "5", "10", "20"], 3), lambda a: a.choice()),
    Screen("date", "Date after a power-off", PORTRAIT,
           lambda b: b.date(0), lambda a: a.date(1)),
    Screen("decks", "Deck list", PORTRAIT,
           lambda b: b.decks(["n4-es", "n5-en", "n5-es"], 1, 1), lambda a: a.decks(3)),
    Screen("get-decks", "Get decks over home Wi-Fi (new)", PORTRAIT,
           None, lambda a: a.get_decks()),
    Screen("stats", "Statistics", PORTRAIT, lambda b: b.stats(), lambda a: a.stats()),
    Screen("wifi", "Wi-Fi portal", PORTRAIT, lambda b: b.wifi(), lambda a: a.wifi()),
    Screen("done", "Done for today", PORTRAIT, lambda b: b.done(), lambda a: a.done()),
]


def render_all(cache: Path, out: Path) -> list[Path]:
    font = cache / "fonts" / "KleeOne-SemiBold.ttf"
    if not font.exists():
        sys.exit(f"error: {font} not found (run `uv run tangocho-pack fetch` in "
                 "tools/pack-builder, or pass --cache)")
    glyphs = Glyphs(font)
    source = stroke_source(cache)
    if source is None:
        print("warning: no KanjiVG data in the cache; the detail card shows a static kanji",
              file=sys.stderr)
    stroke_cache: dict[int, list] = {}

    def strokes(cp: int) -> list:
        if source is None:
            return []
        if cp not in stroke_cache:
            svg = source.svg(cp)
            stroke_cache[cp] = stroke_masks(svg, LARGE_PX) if svg else []
        return stroke_cache[cp]

    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    pairs: list[tuple[Screen, Image.Image | None, Image.Image | None]] = []
    for s in SCREENS:
        imgs: dict[str, Image.Image | None] = {"before": None, "after": None}
        for which, fn in (("before", s.before), ("after", s.after)):
            if fn is None:
                continue
            c = Canvas(*s.size, glyphs)
            fn(Before(c, strokes) if which == "before" else After(c, strokes))
            imgs[which] = c.img
            for scale, suffix in ((1, ""), (3, "@3x")):
                path = out / f"{s.name}-{which}{suffix}.png"
                im = c.img if scale == 1 else c.img.resize((s.size[0] * 3, s.size[1] * 3),
                                                           Image.NEAREST)
                im.save(path, optimize=True)
                written.append(path)
        pairs.append((s, imgs["before"], imgs["after"]))
    if source is not None:
        source.close()
    written.append(contact_sheet(pairs, font, out / "overview.png"))
    return written


def contact_sheet(pairs, font_path: Path, path: Path) -> Path:
    """All before/after pairs at 2x on one sheet, labelled."""
    scale, pad, gap = 2, 24, 12
    title_f = ImageFont.truetype(str(font_path), 22)
    head_f = ImageFont.truetype(str(font_path), 18)
    cap_f = ImageFont.truetype(str(font_path), 14)
    max_w = 1900
    cells = []
    for s, before, after in pairs:
        w, h = s.size[0] * scale, s.size[1] * scale
        cw = w * 2 + gap
        cells.append((s, before, after, w, h, cw, h + 58))
    # Flow cells into rows.
    rows, cur, cur_w = [], [], 0
    for cell in cells:
        if cur and cur_w + cell[5] + pad > max_w - pad:
            rows.append(cur)
            cur, cur_w = [], 0
        cur.append(cell)
        cur_w += cell[5] + pad
    if cur:
        rows.append(cur)
    sheet_w = max_w
    sheet_h = 70 + sum(max(c[6] for c in r) + pad for r in rows) + pad
    sheet = Image.new("RGB", (sheet_w, sheet_h), (18, 20, 26))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 20), "Tangochō UX refresh — before / after (proposal, see docs/ux-refresh.md)",
           font=title_f, fill=(235, 235, 240))
    y = 70
    for r in rows:
        x = pad
        for s, before, after, w, h, cw, ch in r:
            d.text((x, y), s.title, font=head_f, fill=(235, 235, 240))
            for k, (label, im) in enumerate((("before", before), ("after", after))):
                ix = x + k * (w + gap)
                iy = y + 26
                d.text((ix, iy), label.upper(), font=cap_f,
                       fill=(150, 156, 170) if label == "before" else (120, 170, 255))
                if im is None:
                    d.rectangle([ix, iy + 22, ix + w - 1, iy + 22 + h - 1], outline=(60, 64, 76))
                    d.text((ix + 10, iy + 32), "new screen", font=cap_f, fill=(110, 116, 130))
                else:
                    sheet.paste(im.resize((w, h), Image.NEAREST), (ix, iy + 22))
            x += cw + pad
        y += max(c[6] for c in r) + pad
    sheet.save(path, optimize=True)
    return path


def print_tokens() -> None:
    bg = T.BG
    print("| token | RGB565 | RGB (as shown) | on bg | white on it | role |")
    print("|---|---|---|---|---|---|")
    for name, attr, role in T.TABLE:
        v = getattr(T, attr)
        r, g, b = rgb(v)
        print(f"| `{name}` | `0x{v:04X}` | `#{r:02X}{g:02X}{b:02X}` | {contrast(v, bg):.1f} | "
              f"{contrast(T.TEXT, v):.1f} | {role} |")
    print()
    print("Text pairs:")
    for fg, bgc, label in ((T.TEXT, T.ACCENT, "text on accent"), (T.TEXT, T.OK, "text on ok"),
                           (T.TEXT, T.ERR_BG, "text on err-bg"),
                           (T.TEXT_DIM, T.SURFACE, "text-dim on surface"),
                           (T.TEXT_FAINT, T.BG, "text-faint on bg"),
                           (T.OK_HI, T.OK_BG, "ok-hi on ok-bg"),
                           (T.ERR_HI, T.ERR_BG, "err-hi on err-bg"),
                           (T.TEXT, Before.K_GREEN, "before: white on green"),
                           (T.TEXT, Before.K_RED, "before: white on red"),
                           (T.TEXT, Before.K_HIGHLIGHT, "before: white on highlight"),
                           (T.TEXT, Before.K_GREY, "before: white on header grey"),
                           (Before.K_YELLOW, Before.K_BLACK, "before: yellow on black")):
        print(f"- {label}: {contrast(fg, bgc):.1f}:1")
    print()
    print(f"before: green vs red luminance contrast {contrast(Before.K_GREEN, Before.K_RED):.2f}:1"
          f"; after: ok vs err-bg {contrast(T.OK, T.ERR_BG):.2f}:1")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE,
                    help="pack-builder cache with fonts/ and kanjivg/ (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output dir (default: %(default)s)")
    ap.add_argument("--tokens", action="store_true", help="print the colour token table and exit")
    a = ap.parse_args()
    if a.tokens:
        print_tokens()
        return 0
    for p in render_all(a.cache, a.out):
        print(p.relative_to(REPO) if p.is_relative_to(REPO) else p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
