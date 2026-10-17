"""Off-device mock of the quiz screen, drawn from a pack's own glyphs.

Mirrors `firmware/src/main.cpp`: reading answers are a 2x2 grid using the large
text set (3) when the whole question fits; meaning answers are one per row in
the built-in GLCD font (drawn at 2x, shrunk to 1x) and use only each card's
first gloss. The selected reading kind (on/kun) is explicit.
"""

from __future__ import annotations

import random
from math import gcd

from PIL import Image

from . import glcd
from .pack import Pack

BG = (16, 18, 24)
HEADER = (46, 48, 56)
INK = (240, 240, 240)
BOX = (40, 46, 60)
BOX_OK = (30, 90, 50)
LABEL = (255, 210, 60)

MARGIN, GAP = 1, 3
HEADER_H = 14


def _blend(bg, ink, a):  # a in 0..15
    return tuple(round(bg[i] + (ink[i] - bg[i]) * a / 15) for i in range(3))


def fill_rect(img, x0, y0, x1, y1, color) -> None:
    for y in range(max(y0, 0), min(y1, img.height)):
        for x in range(max(x0, 0), min(x1, img.width)):
            img.putpixel((x, y), color)


def _blit_pack(img, pack: Pack, g, pen_x: int, baseline: int, ink, clip_right: int) -> None:
    bmp = pack.bitmap(g)
    px = img.load()
    for y in range(g.height):
        for x in range(g.width):
            v = bmp[y * g.width + x]
            if not v:
                continue
            X, Y = pen_x + g.x_off + x, baseline + g.y_off + y
            if 0 <= X < clip_right and 0 <= Y < img.height:
                px[X, Y] = _blend(px[X, Y], ink, v)


# ------------------------------------------------------------- text metrics

def pack_advance(pack: Pack, cp: int, gset: int) -> int:
    g = pack.find_glyph(gset, cp)
    if g is None and gset != 2:
        g = pack.find_glyph(2, cp)
    return g.advance if g else 0


def pack_width(pack: Pack, s: str, gset: int) -> int:
    return sum(pack_advance(pack, ord(ch), gset) for ch in s)


def draw_pack(img, pack: Pack, s: str, x: int, y_top: int, gset: int, ascent: int, ink,
              clip_right: int) -> int:
    pen = x
    for ch in s:
        g = pack.find_glyph(gset, ord(ch)) or pack.find_glyph(2, ord(ch))
        if g is None:
            continue
        _blit_pack(img, pack, g, pen, y_top + ascent, ink, clip_right)
        pen += g.advance
    return pen - x


def glcd_width(s: str, scale: int) -> int:
    return glcd.GLYPH_W * scale * len(s)


def draw_glcd(img, s: str, x: int, y_top: int, scale: int, ink) -> int:
    pen = x
    for ch in s:
        idx = glcd.glcd_index(ord(ch))
        if idx >= 0:
            cols = glcd.glyph_columns(idx)
            for c in range(5):
                for r in range(7):
                    if (cols[c] >> r) & 1:
                        fill_rect(img, pen + c * scale, y_top + r * scale,
                                  pen + (c + 1) * scale, y_top + (r + 1) * scale, ink)
        pen += glcd.GLYPH_W * scale
    return pen - x


# ------------------------------------------------------------- question model

def _first_reading(txt: str) -> str:
    return txt.split("、")[0].replace(".", "") if txt else ""


def _disp(card, mode: str, kind: str) -> str:
    if mode != "reading":
        return card.meanings.split("; ")[0] if card.meanings else ""
    return _first_reading(card.onyomi if kind == "on" else card.kunyomi)


def reading_kind(pack: Pack, idx: int, kind: str) -> str:
    """The requested kind if the card has it, else its primary kind."""
    c = pack.cards[idx]
    has_on, has_kun = bool(c.onyomi), bool(c.kunyomi)
    if kind == "on" and has_on:
        return "on"
    if kind == "kun" and has_kun:
        return "kun"
    return "on" if has_on else ("kun" if has_kun else "")


def build_options(pack: Pack, idx: int, mode: str, kind: str, seed: int):
    """(option card indices, option texts, correct slot) — first gloss only."""
    card = pack.cards[idx]
    answer = _disp(card, mode, kind)
    opts = [idx]
    used = {answer}
    for j in card.distractors:
        if len(opts) >= 4:
            break
        if j == idx:
            continue
        s = _disp(pack.cards[j], mode, kind)
        if s and s not in used:
            used.add(s)
            opts.append(j)
    n = len(pack.cards)
    if n > 1:
        start = (seed * 7919 + idx) % n
        step = 1 + (seed * 31 + idx) % (n - 1)
        while gcd(step, n) != 1:
            step += 1
        i = 0
        while len(opts) < 4 and i < n:
            j = (start + i * step) % n
            i += 1
            s = _disp(pack.cards[j], mode, kind)
            if j == idx or s in used or not s:
                continue
            used.add(s)
            opts.append(j)
    correct = opts.index(idx)
    rng = random.Random(seed * 7919 + idx)
    rng.shuffle(opts)
    return opts, [_disp(pack.cards[j], mode, kind) for j in opts], opts.index(idx)


# ------------------------------------------------------------- rendering

def _wrap(text: str, char_w, max_w: int, max_lines: int):
    """Greedy wrap (at spaces, else anywhere). Returns [(text, ellipsis)]."""
    lines, i, n = [], 0, len(text)
    while i < n and len(lines) < max_lines:
        last = len(lines) == max_lines - 1
        w, j, last_space = 0, i, -1
        while j < n:
            cw = char_w(ord(text[j]))
            if w + cw > max_w and j > i:
                break
            if text[j] == " ":
                last_space = j
            w += cw
            j += 1
        if j >= n:
            lines.append((text[i:j], False))
            break
        if last:
            ell_w = sum(char_w(ord(c)) for c in "...")
            w, k = 0, i
            while k < n and w + char_w(ord(text[k])) + ell_w <= max_w:
                w += char_w(ord(text[k]))
                k += 1
            lines.append((text[i:k], True))
            break
        cut = last_space if last_space > i else j
        lines.append((text[i:cut], False))
        i = cut + (1 if last_space > i else 0)
    return lines


def _lexer(lexicon, text, max_w, max_lines, line_h, top, h, ink):
    lines = lexicon(text, max_w, max_lines)
    y = top + (h - len(lines) * line_h) // 2
    for s, ell in lines:
        yield s, ell, y
        y += line_h


def _draw_glcd_lines(img, text, max_w, max_lines, line_h, x, w, top, h, scale, ink):
    def lex(t, mw, ml):
        return _wrap(t, lambda cp: glcd.GLYPH_W * scale, mw, ml)
    for s, ell, y in _lexer(lex, text, max_w, max_lines, line_h, top, h, ink):
        shown = s + ("..." if ell else "")
        tx = x + (w - glcd_width(shown, scale)) // 2
        draw_glcd(img, shown, tx, y, scale, ink)


def _draw_pack_lines(img, pack, text, gset, max_w, max_lines, line_h, ascent, x, w, top, h, clip):
    for s, ell, y in _lexer(
            lambda t, mw, ml: _wrap(t, lambda cp: pack_advance(pack, cp, gset), mw, ml),
            text, max_w, max_lines, line_h, top, h, INK):
        shown = s + ("..." if ell else "")
        tx = x + (w - pack_width(pack, shown, gset)) // 2
        draw_pack(img, pack, shown, tx, y, gset, ascent, INK, clip)


def _draw_kanji(img, pack: Pack, cp: int, box_x: int, box_y: int, box_w: int, box_h: int,
                px: int, missing: list) -> None:
    g = pack.find_glyph(1, cp)
    if g is None:
        missing.append(chr(cp))
        return
    pen_x = box_x + (box_w - px) // 2
    base_y = box_y + (box_h - px) // 2 + px
    _blit_pack(img, pack, g, pen_x, base_y, INK, box_x + box_w)


def _blit_stroke(img, s, pen_x: int, pen_y: int, ink) -> None:
    """Blit a 1bpp stroke mask (bit 7 = leftmost pixel, rows byte-aligned)."""
    stride = (s.w + 7) // 8
    for y in range(s.h):
        row = y * stride
        for x in range(s.w):
            if s.data[row + (x >> 3)] & (0x80 >> (x & 7)):
                X, Y = pen_x + s.x_off + x, pen_y + s.y_off + y
                if 0 <= X < img.width and 0 <= Y < img.height:
                    img.putpixel((X, Y), ink)


def _render_strokes(pack: Pack, idx: int, width: int, height: int, scale: int,
                    stroke: int) -> tuple[Image.Image, dict]:
    """Detail-card kanji area drawn from the pack's stroke masks (mock animation).

    `stroke` < 0 draws every stroke in ink; otherwise strokes 0..stroke are drawn
    with the current one highlighted, as the device animates them.
    """
    h = pack.header
    px = h["large_px"]
    img = Image.new("RGB", (width, height), BG)
    fill_rect(img, 0, 0, width, HEADER_H, HEADER)
    draw_glcd(img, pack.deck_name, 3, 3, 1, INK)
    lw = glcd_width("STROKES", 1)
    draw_glcd(img, "STROKES", (width - lw) // 2, HEADER_H + 2, 1, LABEL)
    missing: list[str] = []
    masks = pack.strokes_of(idx)
    if not masks:
        missing.append(chr(pack.cards[idx].codepoint))
        return img.resize((width * scale, height * scale), Image.NEAREST), {
            "options": [], "correct_slot": -1, "reading_kind": None, "missing": missing,
            "stroke_count": 0, "row_h": 0,
        }
    box_h = min(px + 6, height * 45 // 100)
    em_x = (width - px) // 2
    em_y = HEADER_H + 2 + (box_h - px) // 2
    last = len(masks) - 1 if stroke < 0 else min(stroke, len(masks) - 1)
    for i, s in enumerate(masks[: last + 1]):
        ink = LABEL if (stroke >= 0 and i == last) else INK
        _blit_stroke(img, s, em_x, em_y, ink)
    return img.resize((width * scale, height * scale), Image.NEAREST), {
        "options": [], "correct_slot": -1, "reading_kind": None, "missing": missing,
        "stroke_count": len(masks), "row_h": 0,
    }


def render_preview(pack: Pack, cp: int, mode: str, width: int = 135, height: int = 240,
                   scale: int = 3, seed: int = 1, reveal: bool = False,
                   kind: str = "on", stroke: int = -1) -> tuple[Image.Image, dict]:
    idx = pack.find_card(cp)
    if idx is None:
        raise ValueError(f"U+{cp:04X} not in pack")
    if mode == "strokes":
        return _render_strokes(pack, idx, width, height, scale, stroke)
    h = pack.header
    if mode == "reading":
        kind = reading_kind(pack, idx, kind)
        if not kind:
            raise ValueError(f"U+{cp:04X} has no reading")

    opts, texts, correct = build_options(pack, idx, mode, kind, seed)
    img = Image.new("RGB", (width, height), BG)
    missing: list[str] = []

    # Header (deck name, built-in font) and the MEANING/READING label.
    fill_rect(img, 0, 0, width, HEADER_H, HEADER)
    draw_glcd(img, pack.deck_name, 3, 3, 1, INK)
    large_px = h["large_px"]
    kanji_h = min(large_px + 6, height * 45 // 100)
    label = "KANJI" if mode == "kanji" else ("READING" if mode == "reading" else "MEANING")
    lw = glcd_width(label, 1)
    draw_glcd(img, label, (width - lw) // 2, HEADER_H + 2 + kanji_h, 1, LABEL)

    if mode == "kanji":
        # Prompt is the meaning; the options are kanji.
        prompt = texts[correct]
        if glcd.can_draw(prompt):
            gscale = 2 if glcd_width(prompt, 2) <= width - 8 else 1
            _draw_glcd_lines(img, prompt, width - 6, 4, glcd.GLYPH_H * gscale, 3, width - 6,
                             HEADER_H + 2, kanji_h, gscale, INK)
        else:
            for ch in prompt:
                if pack.find_glyph(2, ord(ch)) is None:
                    missing.append(ch)
            _draw_pack_lines(img, pack, prompt, 2, width - 6, 4, h["text_line_height"],
                             h["text_ascent"], 3, width - 6, HEADER_H + 2, kanji_h, width)
    else:
        # Prompt kanji: em square centred horizontally, baseline = square bottom.
        g0 = pack.find_glyph(0, cp)
        if g0 is None:
            missing.append(chr(cp))
        else:
            pen_x = (width - large_px) // 2
            base_y = HEADER_H + 2 + (kanji_h - large_px) // 2 + large_px
            _blit_pack(img, pack, g0, pen_x, base_y, INK, width)

    top = HEADER_H + kanji_h + 14
    area = height - top - 2
    fill_rect(img, MARGIN, top, width - MARGIN, height - 2, BG)

    if mode == "kanji":
        cols, rows = 2, (len(opts) + 1) // 2
        cell_w = (width - 2 * MARGIN - (cols - 1) * GAP) // cols
        cell_h = (area - (rows - 1) * GAP) // rows
        for i, o in enumerate(opts):
            x = MARGIN + (i % cols) * (cell_w + GAP)
            y = top + (i // cols) * (cell_h + GAP)
            box = BOX_OK if (reveal and i == correct) else BOX
            fill_rect(img, x, y, x + cell_w, y + cell_h, box)
            _draw_kanji(img, pack, pack.cards[o].codepoint, x, y, cell_w, cell_h, h["small_px"],
                        missing)
    elif mode == "reading":
        cols, rows = 2, (len(opts) + 1) // 2
        cell_w = (width - 2 * MARGIN - (cols - 1) * GAP) // cols
        cell_h = (area - (rows - 1) * GAP) // rows
        gset = 3
        if any(pack_width(pack, t, 3) > cell_w - 4 for t in texts):
            gset = 2
        for i, t in enumerate(texts):
            x = MARGIN + (i % cols) * (cell_w + GAP)
            y = top + (i // cols) * (cell_h + GAP)
            box = BOX_OK if (reveal and i == correct) else BOX
            fill_rect(img, x, y, x + cell_w, y + cell_h, box)
            for ch in t:
                if pack.find_glyph(3, ord(ch)) is None and pack.find_glyph(2, ord(ch)) is None:
                    missing.append(ch)
            _draw_pack_lines(img, pack, t, gset, cell_w - 4, 1, h["option_line_height"],
                             h["option_ascent"] if gset == 3 else h["text_ascent"],
                             x + 2, cell_w - 4, y + 1, cell_h - 2, x + cell_w)
    else:
        rows = len(opts)
        row_h = area // rows
        box_h = row_h - GAP
        inner_w = width - 2 * MARGIN - 4
        use_glcd = all(glcd.can_draw(t) for t in texts)
        if use_glcd:
            big = all(glcd_width(t, 2) <= inner_w for t in texts)
            gl_scale, line_h = (2, glcd.GLYPH_H * 2) if big else (1, glcd.GLYPH_H)
        else:
            gl_scale, line_h = 0, h["text_line_height"]
        max_lines = max(1, min(2, (box_h - 2) // line_h))
        for i, t in enumerate(texts):
            y = top + i * row_h
            box = BOX_OK if (reveal and i == correct) else BOX
            fill_rect(img, MARGIN, y, width - MARGIN, y + box_h, box)
            if use_glcd:
                _draw_glcd_lines(img, t, inner_w, max_lines, line_h, MARGIN + 2, inner_w, y + 1,
                                 box_h - 2, gl_scale, INK)
            else:
                for ch in t:
                    if pack.find_glyph(2, ord(ch)) is None:
                        missing.append(ch)
                _draw_pack_lines(img, pack, t, 2, inner_w, max_lines, line_h, h["text_ascent"],
                                 MARGIN + 2, inner_w, y + 1, box_h - 2, width - MARGIN)

    info = {
        "options": [pack.cards[o].codepoint for o in opts],
        "correct_slot": correct,
        "reading_kind": kind if mode == "reading" else None,
        "missing": missing,
        "row_h": (area // len(opts)) if mode == "meaning" else 0,
    }
    return img.resize((width * scale, height * scale), Image.NEAREST), info
