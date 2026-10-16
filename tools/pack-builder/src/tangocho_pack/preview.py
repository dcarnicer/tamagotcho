"""Mock of the firmware quiz screen drawn ONLY from glyph bitmaps in a pack."""

from __future__ import annotations

import random

from PIL import Image

from .pack import Pack

BG = (16, 18, 24)
INK = (240, 240, 240)
BOX = (40, 46, 60)
BOX_OK = (30, 90, 50)
BORDER = (90, 100, 125)
DIM = (140, 140, 150)


def _blit(img: Image.Image, pack: Pack, g, pen_x: int, baseline: int, ink, clip_right: int) -> None:
    bmp = pack.bitmap(g)
    px = img.load()
    for y in range(g.height):
        for x in range(g.width):
            v = bmp[y * g.width + x]
            if not v:
                continue
            X, Y = pen_x + g.x_off + x, baseline + g.y_off + y
            if 0 <= X < clip_right and 0 <= Y < img.height:
                bg = px[X, Y]
                px[X, Y] = tuple(round(bg[i] + (ink[i] - bg[i]) * v / 15) for i in range(3))


def text_width(pack: Pack, s: str) -> int:
    w = 0
    for ch in s:
        g = pack.find_glyph(2, ord(ch))
        w += g.advance if g else 0
    return w


def draw_text(img, pack, s, x, baseline, max_x, ink=INK) -> list[str]:
    """Draw one line, clipping at max_x. Returns characters missing from set 2."""
    missing = []
    for ch in s:
        g = pack.find_glyph(2, ord(ch))
        if g is None:
            missing.append(ch)
            continue
        if x + g.advance > max_x:
            break
        _blit(img, pack, g, x, baseline, ink, max_x)
        x += g.advance
    return missing


def fill_rect(img, x0, y0, x1, y1, color) -> None:
    for y in range(max(y0, 0), min(y1, img.height)):
        for x in range(max(x0, 0), min(x1, img.width)):
            img.putpixel((x, y), color)


def option_text(pack: Pack, idx: int, mode: str) -> str:
    c = pack.cards[idx]
    if mode == "meaning":
        return c.meanings.split("; ")[0] if c.meanings else "?"
    parts = [p for p in (c.onyomi.split("、")[0], c.kunyomi.split("、")[0]) if p]
    return "、".join(parts) or "?"


def render_preview(
    pack: Pack, cp: int, mode: str, width: int = 135, height: int = 240,
    scale: int = 3, seed: int = 1, reveal: bool = False,
) -> tuple[Image.Image, dict]:
    idx = pack.find_card(cp)
    if idx is None:
        raise ValueError(f"U+{cp:04X} not in pack")
    card = pack.cards[idx]
    h = pack.header
    rng = random.Random(seed * 7919 + cp)
    options = [idx] + card.distractors[:3]
    rng.shuffle(options)
    correct_slot = options.index(idx)

    img = Image.new("RGB", (width, height), BG)
    missing: list[str] = []
    margin = 4
    # prompt: em square (large_px) centred horizontally; baseline = bottom edge
    lp = h["large_px"]
    top = margin + 2
    g0 = pack.find_glyph(0, cp)
    if g0 is None:
        missing.append(chr(cp))
    else:
        _blit(img, pack, g0, (width - lp) // 2, top + lp, INK, width)
    area_top = top + lp + margin
    n = len(options)
    gap = 3
    box_h = (height - area_top - margin - gap * (n - 1)) // n
    lh, asc = h["text_line_height"], h["text_ascent"]
    for slot, o in enumerate(options):
        y0 = area_top + slot * (box_h + gap)
        color = BOX_OK if reveal and slot == correct_slot else BOX
        fill_rect(img, margin, y0, width - margin, y0 + box_h, color)
        txt = option_text(pack, o, mode)
        baseline = y0 + (box_h - lh) // 2 + asc
        missing += draw_text(img, pack, txt, margin + 4, baseline, width - margin - 2)
    info = {"options": [pack.cards[o].codepoint for o in options], "correct_slot": correct_slot,
            "missing": missing, "box_h": box_h, "line_height": lh}
    return img.resize((width * scale, height * scale), Image.NEAREST), info
