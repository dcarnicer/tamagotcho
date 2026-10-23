#!/usr/bin/env python3
"""Learning-mode mockups: a new kanji's first contact today, and the proposed
intro card (docs/learning-mode.md).

Run it inside the pack builder's environment, like ``render.py`` next to it,
whose canvas, glyph and stroke helpers it reuses:

    cd tools/pack-builder && uv run python ../ux-mockups/learning.py

Options: ``--cache DIR`` (default ``tools/pack-builder/cache``; needs
``noto/NotoSansJP-Regular.otf`` and the KanjiVG zip, i.e. ``uv run
tamagotcho-pack fetch``) and ``--out DIR`` (default ``docs/learning-mode``).

Unlike ``render.py``, whose "after" images predate the maintainer's answers in
docs/ux-refresh.md §6, every screen here is drawn in the style the firmware
has now: flat tiles and rows (no radius, 2 px gaps), the Noto Sans JP UI font
(Label 18 px on a 20 px pitch, centred on its cap height), Font0 captions, the
header and hint bar of ``render/Widgets.cpp`` and the quiz geometry of
``quizGeom()`` in ``main.cpp``. Pack glyphs are rendered from Noto Sans JP at
the builder defaults (96/40/18/20 px), the stroke masks from KanjiVG.

* **before**: what the device draws today when a new kanji comes up (its first
  question) and after it is missed (the mistake detail card).
* **after**: the proposed intro card, portrait and landscape (StickS3), the
  Cardputer ADV's keyboard hints, the T-Display-S3, and the example-word
  variant of decision 9.

Output: native-resolution PNGs, ``@3x`` nearest-neighbour upscales and one
contact sheet ``overview.png``. Deterministic: fixed sample data, no
timestamps. Sample card data is KANJIDIC2 (CC BY-SA 4.0), the example words
JMdict (CC BY-SA 4.0), the strokes KanjiVG (CC BY-SA 3.0), the glyphs Noto
Sans JP (SIL OFL 1.1); see docs/data-sources.md.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from tamagotcho_pack.kanjivg import stroke_masks

import render as R  # tools/ux-mockups/render.py
from render import CARDS, ICONS, T, Canvas, Card, Face, Glyphs, stroke_source

REPO = R.REPO
DEFAULT_CACHE = R.DEFAULT_CACHE
DEFAULT_OUT = REPO / "docs" / "learning-mode"

LARGE_PX, SMALL_PX, TEXT_PX, OPTION_PX = R.LARGE_PX, R.SMALL_PX, R.TEXT_PX, R.OPTION_PX

# ui/Theme.h (flat, §6.1 of docs/ux-refresh.md)
GUT, GAP = 4, 2
HEADER_H, HINT_H, PILL_H = 16, 12, 11
TILE_PAD_X, TILE_MARK_W = 8, 20
# ui/UiFont.h kLabel: 18 px, pitch 20; the Renderer's ascent for a UI face is
# (pitch + capHeight + 1) / 2 (Renderer::ascent()).
UI_PX, UI_PITCH = 18, 20

PORTRAIT, LANDSCAPE, TDISPLAY = (135, 240), (240, 135), (170, 320)

# Icons the hint bar's keycaps use (ui/Icons.h), next to render.py's ICONS.
KEY_ICONS = {
    "key_updown": ["..#.....#..", ".###....#..", "#.#.#...#..", "..#.....#..", "..#...#.#.#",
                   "..#....###.", "..#.....#.."],
    "key_right": ["....#..", ".....#.", "#######", ".....#.", "....#.."],
    "key_left": ["..#....", ".#.....", "#######", ".#.....", "..#...."],
    "key_enter": ["......#", "......#", "..#...#", ".#....#", "#######", ".#.....", "..#...."],
}
ICONS.update(KEY_ICONS)

BATTERY = 87


# =============================================================== sample data

@dataclass
class Word:
    """An example word (decision 9): JMdict form, reading and first gloss."""
    form: str
    reading: str
    gloss: str


# JMdict (CC BY-SA 4.0): common words with 休's on'yomi and kun'yomi.
WORDS = {"休": [Word("休日", "きゅうじつ", "holiday"), Word("休む", "やすむ", "to rest")]}


@dataclass
class Q:
    card: Card
    mode: str             # "meaning", "reading"
    options: list[str]
    correct: int
    sel: int = 0
    kind: str = ""        # reading: "ON" / "KUN"


def q_rest_meaning() -> Q:
    return Q(CARDS["休"], "meaning", ["body", "rest", "tree", "book"], 1, 0)


def q_rest_on() -> Q:
    return Q(CARDS["休"], "reading", ["タイ", "キュウ", "ボク", "ホン"], 1, 0, "ON")


# =============================================================== flat widgets

class Flat:
    """The firmware's current widgets on a render.py Canvas."""

    def __init__(self, c: Canvas, strokes):
        self.c, self.W, self.H, self.strokes = c, c.w, c.h, strokes
        self.ui = Face("pack", px=UI_PX, gset=2)
        self.opt = Face("pack", px=OPTION_PX, gset=3)
        h, _ = c.g.glyph(2, ord("H"), UI_PX)
        self.cap = -h.y_off
        self.asc = (UI_PITCH + self.cap + 1) // 2

    # ---------------------------------------------------------- text
    def adv(self, s: str) -> int:
        return self.c.text_width(s, self.ui)

    def ui_text(self, s: str, x: int, top: int, fg: int, bg: int) -> int:
        """Renderer::drawRange for the UI face (kana from the pack's set 2)."""
        pen = x
        for ch in s:
            g, bits = self.c.g.glyph(2, ord(ch), UI_PX)
            self.c.bmp4(pen + g.x_off, top + self.asc + g.y_off, bits, g.width, g.height, fg, bg)
            pen += g.advance
        return pen - x

    def ui_label(self, s: str, x: int, y: int, w: int, h: int, fg: int, bg: int,
                 align: str = "l") -> int:
        """Renderer::drawLabel: one line, '...' when cut, centred on the cap height."""
        lines = self.c.wrap(s, w, 1, self.ui)
        if not lines:
            return 0
        t, ell = lines[0]
        if ell:
            t = t.rstrip(" ")
        shown = t + ("..." if ell else "")
        tw = self.adv(shown)
        tx = x if align == "l" else (x + w - tw if align == "r" else x + (w - tw) // 2)
        tx = max(tx, x)
        self.ui_text(shown, tx, y + (h + self.cap) // 2 - self.asc, fg, bg)
        return tw

    def ui_wrapped(self, s: str, x: int, y: int, w: int, max_lines: int, fg: int, bg: int) -> int:
        lines = self.c.wrap(s, w, max_lines, self.ui)
        for i, (t, ell) in enumerate(lines):
            dw = self.ui_text(t, x, y + i * UI_PITCH, fg, bg)
            if ell:
                self.ui_text("...", x + dw, y + i * UI_PITCH, fg, bg)
        return len(lines)

    def caption(self, s: str, x: int, y: int, fg: int) -> int:
        return self.c.glcd_text(s, x, y, fg)

    @staticmethod
    def caption_w(s: str) -> int:
        return 6 * len(s)

    # ---------------------------------------------------------- header
    def battery(self, x: int, y: int) -> None:
        c = self.c
        c.draw.rectangle([x, y, x + 11, y + 6], outline=R.rgb(T.TEXT_DIM))
        c.rect(x + 12, y + 2, 1, 3, T.TEXT_DIM)
        c.rect(x + 2, y + 2, max(1, (8 * BATTERY + 50) // 100), 3, T.TEXT_DIM)

    def pill(self, text: str, x: int, y: int, fg: int, bg: int, icon: str | None = None) -> int:
        iw = len(ICONS[icon][0]) + 3 if icon else 0
        w = self.caption_w(text) + 6 + iw
        self.c.rect(x, y, w, PILL_H, bg)
        tx = x + 3
        if icon:
            rows = ICONS[icon]
            self.c.icon(icon, tx, y + (PILL_H - len(rows) + 1) // 2, fg)
            tx += iw
        self.caption(text, tx, y + 2, fg)
        return w

    def header(self, *, title: str | None = None, pill: tuple[str, int, int] | None = None,
               right: str | None = None, progress: tuple[int, int] | None = None) -> None:
        c, W = self.c, self.W
        c.rect(0, 0, W, HEADER_H, T.BG)
        left = GUT
        if pill:
            left += self.pill(pill[0], GUT, 2, pill[1], pill[2])
        elif title:
            left += self.caption(title, GUT, 4, T.TEXT_DIM)
        rx = W - GUT - 13
        self.battery(rx, 4)
        rx -= 5
        if right:
            self.caption(right, rx - self.caption_w(right), 4, T.TEXT_DIM)
        if progress:
            num, den = progress
            c.rect(0, HEADER_H - 2, W, 2, T.SURFACE_HI)
            c.rect(0, HEADER_H - 2, W * num // den, 2, T.ACCENT)
        else:
            c.rect(0, HEADER_H - 1, W, 1, T.LINE)

    # ---------------------------------------------------------- hint bar
    def hints(self, tap: str | None, hold: str | None = None, *, side: str | None = "menu",
              side_word: str | None = None, keys: bool = False, tap_key: str = "key_updown",
              side_key: str = "tab") -> None:
        """widgets::drawHintBar for one row: front items from the left, the side
        group right-aligned (its icon, or on a keyboard its keycap, and a word)."""
        c, W, H = self.c, self.W, self.H
        y = H - HINT_H
        c.rect(0, y, W, HINT_H, T.BG)

        def mark(x: int, name: str, ink: int) -> int:
            if not keys:
                rows = ICONS[name]
                c.icon(name, x, y + 2 + (7 - len(rows)) // 2, ink)
                return len(rows[0])
            if name in ICONS:
                w = len(ICONS[name][0]) + 4
                c.rect(x, y + 1, w, 10, T.SURFACE_HI)
                rows = ICONS[name]
                c.icon(name, x + 2, y + 1 + (10 - len(rows)) // 2, ink)
                return w
            w = self.caption_w(name) + 4
            c.rect(x, y + 1, w, 10, T.SURFACE_HI)
            c.glcd_text(name, x + 2, y + 2, ink)
            return w

        def mark_w(name: str) -> int:
            if name in ICONS:
                return len(ICONS[name][0]) + (4 if keys else 0)
            return self.caption_w(name) + 4

        x = GUT
        if tap:
            x += mark(x, tap_key if keys else "tap", T.TEXT) + 3
            x += self.caption(tap, x, y + 2, T.TEXT_DIM) + 6
        if hold:
            x += mark(x, "key_enter" if keys else "hold", T.TEXT) + 3
            x += self.caption(hold, x, y + 2, T.TEXT_DIM) + 6
        front_end = x - 6
        if side or (keys and side_key):
            name = side_key if keys else side
            mw = mark_w(name)
            with_word = mw + 3 + self.caption_w(side_word) if side_word else mw
            right = W - GUT
            show_word = side_word and right - with_word >= front_end + 6
            sx = right - (with_word if show_word else mw)
            mark(sx, name, T.TEXT)
            if show_word:
                self.caption(side_word, sx + mw + 3, y + 2, T.TEXT_DIM)

    # ---------------------------------------------------------- quiz (today)
    def quiz(self, q: Q, left: int, done: int) -> None:
        """drawQuiz(): header with the mode pill, the prompt kanji, flat tiles,
        the hint bar (shown: the first 50 answers, or after 8 s idle)."""
        c, W, H = self.c, self.W, self.H
        c.fill(T.BG)
        land = W > H
        if q.mode == "reading":
            pill = (q.kind, T.MODE_READING, T.SURFACE_HI)
        else:
            pill = ("MEANING", T.ACCENT_HI, T.SURFACE_HI)
        self.header(pill=pill, right=str(left), progress=(done, done + left))
        hint_y = H - HINT_H
        if not land:
            px, py, pw, ph = 0, HEADER_H, W, 102
            ax, ay, aw = GUT, 120, W - 2 * GUT
            ah = hint_y - 4 - ay
        else:
            pw = 120 if W >= 300 else 100
            px, py, ph = 0, HEADER_H, hint_y - HEADER_H
            ax, ay = pw + GUT, HEADER_H + 3
            aw, ah = W - ax - GUT, hint_y - 1 - ay
        c.draw_kanji(0, q.card.cp, px, py, pw, ph, T.TEXT, T.BG)
        n = len(q.options)
        if q.mode == "reading":
            gx, gw = ax - 2, aw + 4
            cw, ch = (gw - GAP) // 2, (ah - GAP) // 2
            for i, s in enumerate(q.options):
                x, y = gx + (i % 2) * (cw + GAP), ay + (i // 2) * (ch + GAP)
                bg = T.ACCENT if i == q.sel else T.SURFACE
                c.rect(x, y, cw, ch, bg)
                c.label(s, x + 2, y, cw - 4, ch, T.TEXT, bg, self.opt, "c")
        else:
            th = (ah - (n - 1) * GAP) // n
            for i, s in enumerate(q.options):
                y = ay + i * (th + GAP)
                bg = T.ACCENT if i == q.sel else T.SURFACE
                c.rect(ax, y, aw, th, bg)
                self.ui_label(s, ax + TILE_PAD_X, y, aw - TILE_PAD_X - 6, th, T.TEXT, bg)
        self.hints("move", "answer")

    # ---------------------------------------------------------- kanji box
    def kanji_box(self, cp: int, box: tuple[int, int, int, int], frame: int | None) -> int:
        """drawDetailKanji(): the static glyph, or the strokes so far in text,
        the current one in warn and the rest as a ghost. Returns the stroke count."""
        c = self.c
        bx, by, bw, bh = box
        masks = self.strokes(cp) if frame is not None else []
        if not masks:
            c.draw_kanji(0, cp, bx, by, bw, bh, T.TEXT, T.BG)
            return 0
        ex, ey = bx + (bw - LARGE_PX) // 2, by + (bh - LARGE_PX) // 2
        for i, m in enumerate(masks):
            if i > frame:
                c.bmp1(ex + m.x_off, ey + m.y_off, m.data, m.w, m.h, T.GHOST)
        for i, m in enumerate(masks[: frame + 1]):
            c.bmp1(ex + m.x_off, ey + m.y_off, m.data, m.w, m.h, T.WARN if i == frame else T.TEXT)
        return len(masks)

    def body_box(self) -> tuple[tuple[int, int, int, int], int, int, int]:
        """The kanji box (quizGeom's prompt box) and the text column x, w, top."""
        W, H = self.W, self.H
        if W > H:
            pw = 120 if W >= 300 else 100
            box = (0, HEADER_H, pw, H - HINT_H - HEADER_H)
            return box, pw + GUT, W - pw - 2 * GUT, HEADER_H + 3
        box = (0, HEADER_H, W, 102)
        return box, GUT, W - 2 * GUT, HEADER_H + 102 + 2

    # ---------------------------------------------------------- detail (today)
    def detail(self, card: Card, frame: int, picked: str, acc: str) -> None:
        """drawDetail(): what was picked, the meanings, every reading."""
        c, H = self.c, self.H
        c.fill(T.BG)
        box, x, w, y = self.body_box()
        n = len(self.strokes(card.cp))
        self.header(title="DETAIL", right=f"stroke {frame + 1}/{n}" if n else None)
        self.kanji_box(card.cp, box, frame)
        bottom = H - HINT_H - 2
        c.icon("cross", x, y + (UI_PITCH - 7 + 1) // 2, T.ERR_HI)
        aw = self.caption_w(acc)
        self.ui_label(picked, x + 11, y, w - 11 - aw - 6, UI_PITCH, T.ERR_HI, T.BG)
        self.caption(acc, x + w - aw, y + (UI_PITCH - 7 + 1) // 2, T.TEXT_DIM)
        y += UI_PITCH + 2
        rows = [(k, R.join_readings(s)) for k, s in (("ON", card.on), ("KUN", card.kun)) if s]
        lines = max(1, min(3, (bottom - y - len(rows) * (UI_PITCH + 2)) // UI_PITCH))
        y += self.ui_wrapped(card.meanings, x, y, w, lines, T.TEXT, T.BG) * UI_PITCH + 2
        tag_w = max(self.caption_w("ON"), self.caption_w("KUN")) + 6
        for tag, s in rows:
            self.pill(tag, x, y + (UI_PITCH - PILL_H + 1) // 2, T.MODE_READING, T.SURFACE_HI)
            self.ui_label(s, x + tag_w + 5, y, w - tag_w - 5, UI_PITCH, T.TEXT, T.BG)
            y += UI_PITCH + 2
        self.hints("next")

    # ---------------------------------------------------------- intro (proposed)
    def emphasised(self, items: list[str], x: int, y: int, w: int) -> None:
        """One line: the asked item in text, the others in text-dim after it
        when they all fit, else a "+N" caption for them."""
        fw = self.ui_label(items[0], x, y, w, UI_PITCH, T.TEXT, T.BG)
        if len(items) < 2:
            return
        rest = "、" + "、".join(items[1:])
        if fw + self.adv(rest) <= w:
            self.ui_label(rest, x + fw, y, w - fw, UI_PITCH, T.TEXT_DIM, T.BG)
        else:
            more = f"+{len(items) - 1}"
            self.caption(more, x + fw + 5, y + (UI_PITCH - 7 + 1) // 2, T.TEXT_DIM)

    def intro(self, card: Card, frame: int, left: int, done: int, *, keys: bool = False,
              words: list[Word] | None = None) -> None:
        c, H = self.c, self.H
        c.fill(T.BG)
        box, x, w, y = self.body_box()
        n = len(self.strokes(card.cp))
        self.header(pill=("NEW", T.ACCENT_HI, T.SURFACE_HI),
                    right=f"stroke {frame + 1}/{n}" if n else None, progress=(done, done + left))
        self.kanji_box(card.cp, box, frame)

        meanings = card.meanings.split("; ")
        self.ui_label(meanings[0], x, y, w, UI_PITCH, T.TEXT, T.BG)
        y += UI_PITCH
        if len(meanings) > 1:
            self.ui_label("; ".join(meanings[1:]), x, y, w, UI_PITCH, T.TEXT_DIM, T.BG)
            y += UI_PITCH
        y += 2

        tag_w = max(self.caption_w("ON"), self.caption_w("KUN")) + 6
        for tag, raw in (("ON", card.on), ("KUN", card.kun)):
            if not raw:
                continue
            items = [r.replace(".", "") for r in raw.split("、")]
            self.pill(tag, x, y + (UI_PITCH - PILL_H + 1) // 2, T.MODE_READING, T.SURFACE_HI)
            self.emphasised(items, x + tag_w + 5, y, w - tag_w - 5)
            y += UI_PITCH + 2

        if words:
            # Decision 9, option (b): the words a pack would carry, one per row:
            # the word, then its reading and gloss in captions underneath when
            # the panel has room, else beside it.
            for wd in words:
                if y + UI_PITCH > H - HINT_H:
                    break
                fw = self.ui_label(wd.form, x, y, w, UI_PITCH, T.TEXT, T.BG)
                self.ui_label(wd.reading, x + fw + 6, y, w - fw - 6, UI_PITCH, T.TEXT_DIM, T.BG)
                y += UI_PITCH
                if y + 9 <= H - HINT_H - 1:
                    self.caption(wd.gloss, x, y, T.TEXT_DIM)
                    y += 11

        self.intro_hints(keys)

    def intro_hints(self, keys: bool) -> None:
        if keys:
            self.hints("next", None, side_word="known", keys=True, tap_key="key_enter",
                       side_key="key_left")
        else:
            self.hints("next", None, side="check", side_word="known")

    def intro_words_page(self, card: Card, words: list[Word], left: int, done: int) -> None:
        """Decision 9 (b) on a 135 px panel: the words do not fit under the
        readings, so they get a second page (a front tap turns to it, the next
        one goes on): the kanji small with its asked meaning, then each word,
        its reading and its gloss."""
        c, W, H = self.c, self.W, self.H
        c.fill(T.BG)
        self.header(pill=("NEW", T.ACCENT_HI, T.SURFACE_HI), right="2/2",
                    progress=(done, done + left))
        x, w = GUT, W - 2 * GUT
        y = HEADER_H + 4
        c.draw_kanji(1, card.cp, x, y, SMALL_PX, SMALL_PX, T.TEXT, T.BG)
        self.ui_label(card.meanings.split("; ")[0], x + SMALL_PX + 8, y, w - SMALL_PX - 8,
                      SMALL_PX, T.TEXT, T.BG)
        y += SMALL_PX + 6
        c.rect(x, y, w, 1, T.LINE)
        y += 5
        for wd in words:
            if y + 2 * UI_PITCH + 11 > H - HINT_H:
                break
            self.ui_label(wd.form, x, y, w, UI_PITCH, T.TEXT, T.BG)
            y += UI_PITCH
            self.ui_label(wd.reading, x, y, w, UI_PITCH, T.TEXT_DIM, T.BG)
            y += UI_PITCH + 1
            self.caption(wd.gloss, x, y, T.TEXT_DIM)
            y += 15
        self.intro_hints(False)


# =============================================================== screens

@dataclass
class Shot:
    name: str
    title: str
    size: tuple[int, int]
    draw: object  # callable(Flat)


SHOTS = [
    Shot("first-contact-before", "Today: a new kanji's first contact is a question",
         PORTRAIT, lambda f: f.quiz(q_rest_meaning(), left=8, done=12)),
    Shot("detail-before", "Today: the detail card, after missing it", PORTRAIT,
         lambda f: f.detail(CARDS["休"], 2, "body", "correct 0/1")),
    Shot("first-contact-landscape-before", "Today: first contact, landscape", LANDSCAPE,
         lambda f: f.quiz(q_rest_on(), left=8, done=12)),
    Shot("intro-after", "Proposed: intro card (stroke 3 of 6)", PORTRAIT,
         lambda f: f.intro(CARDS["休"], 2, left=24, done=12)),
    Shot("intro-landscape-after", "Proposed: intro card, landscape", LANDSCAPE,
         lambda f: f.intro(CARDS["休"], 2, left=24, done=12)),
    Shot("intro-cardputer-after", "Proposed: intro, Cardputer ADV keys", LANDSCAPE,
         lambda f: f.intro(CARDS["休"], 5, left=24, done=12, keys=True)),
    Shot("intro-tdisplay-after", "Proposed: intro, T-Display-S3, with example words (decision 9)",
         TDISPLAY, lambda f: f.intro(CARDS["休"], 5, left=24, done=12, words=WORDS["休"])),
    Shot("intro-words-after", "Decision 9 (b): example words, a second page on the StickS3",
         PORTRAIT, lambda f: f.intro_words_page(CARDS["休"], WORDS["休"], left=24, done=12)),
]


def render_all(cache: Path, out: Path) -> list[Path]:
    font = cache / "noto" / "NotoSansJP-Regular.otf"
    if not font.exists():
        sys.exit(f"error: {font} not found (run `uv run tamagotcho-pack fetch` in "
                 "tools/pack-builder, or pass --cache)")
    glyphs = Glyphs(font)
    source = stroke_source(cache)
    if source is None:
        print("warning: no KanjiVG data in the cache; the kanji are static", file=sys.stderr)
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
    images: list[tuple[Shot, Image.Image]] = []
    for s in SHOTS:
        c = Canvas(*s.size, glyphs)
        s.draw(Flat(c, strokes))
        images.append((s, c.img))
        for scale, suffix in ((1, ""), (3, "@3x")):
            path = out / f"{s.name}{suffix}.png"
            im = c.img if scale == 1 else c.img.resize((s.size[0] * 3, s.size[1] * 3),
                                                       Image.NEAREST)
            im.save(path, optimize=True)
            written.append(path)
    if source is not None:
        source.close()
    written.append(contact_sheet(images, font, out / "overview.png"))
    return written


def contact_sheet(images, font_path: Path, path: Path) -> Path:
    """Every shot at 2x on one sheet, labelled, flowed into rows."""
    scale, pad = 2, 24
    title_f = ImageFont.truetype(str(font_path), 22)
    cap_f = ImageFont.truetype(str(font_path), 15)
    max_w = 1500
    rows, cur, cur_w = [], [], pad
    for s, im in images:
        w = s.size[0] * scale
        if cur and cur_w + w + pad > max_w:
            rows.append(cur)
            cur, cur_w = [], pad
        cur.append((s, im))
        cur_w += w + pad
    if cur:
        rows.append(cur)
    row_h = [max(s.size[1] * scale for s, _ in r) + 70 for r in rows]
    sheet = Image.new("RGB", (max_w, 70 + sum(row_h) + pad), (18, 20, 26))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 20), "Tamagotcho learning mode: today's first contact and the proposed intro "
           "(docs/learning-mode.md)", font=title_f, fill=(235, 235, 240))
    y = 70
    for r, rh in zip(rows, row_h):
        x = pad
        for s, im in r:
            w = s.size[0] * scale
            colour = (150, 156, 170) if s.name.endswith("before") else (120, 170, 255)
            words = s.title.split(": ", 1)
            d.text((x, y), words[0].upper(), font=cap_f, fill=colour)
            d.text((x, y + 20), words[1] if len(words) > 1 else "", font=cap_f,
                   fill=(220, 222, 228))
            sheet.paste(im.resize((w, s.size[1] * scale), Image.NEAREST), (x, y + 46))
            x += w + pad
        y += rh
    sheet.save(path, optimize=True)
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE,
                    help="pack-builder cache with noto/ and kanjivg/ (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output dir (default: %(default)s)")
    a = ap.parse_args()
    for p in render_all(a.cache, a.out):
        print(p.relative_to(REPO) if p.is_relative_to(REPO) else p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
