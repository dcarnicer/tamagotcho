"""Orchestration: cards + font -> pack bytes."""

from __future__ import annotations

import sys
from pathlib import Path

from .distractors import meaning_distractors, reading_distractors, square_vector
from .glyphs import SET_LARGE, SET_OPTION, SET_SMALL, SET_TEXT, FontRenderer, Glyph
from .kanjidic import Card
from .kanjivg import KanjiVGDir, KanjiVGZip, default_source, masks_for
from .pack import card_strings, write_pack


def default_deck_name(jlpt: set[int], grades: set[int]) -> str:
    parts = []
    if jlpt:
        parts.append("JLPT " + "+".join(f"N{n}" for n in sorted(jlpt, reverse=True)))
    if grades:
        parts.append("Grade " + "+".join(str(g) for g in sorted(grades)))
    return " ".join(parts) or "Kanji"


def build_pack(
    cards: list[Card],
    font_path: str | Path,
    lang: str,
    deck_name: str,
    large_px: int = 96,
    small_px: int = 40,
    text_px: int = 18,
    option_px: int = 20,
    build_time: int | None = None,
    strokes: bool = False,
    strokes_px: int | None = None,
    kanjivg: KanjiVGDir | KanjiVGZip | None = None,
) -> bytes:
    # The stroke masks store per-stroke placement as i8, so they only work up to
    # a 128 px em square (docs/pack-format.md "Limits"). Fail before rendering the
    # font or downloading KanjiVG.
    if strokes and large_px > 128:
        raise ValueError(f"large_px ({large_px}) > 128 cannot be used with --strokes: "
                         "stroke x_off/y_off are signed bytes (max 127)")
    font = FontRenderer(font_path)
    kept = []
    for c in cards:
        if font.has(c.codepoint):
            kept.append(c)
        else:
            print(f"warning: font lacks U+{c.codepoint:04X} {c.char}; card dropped", file=sys.stderr)
    cards = kept

    # Kanji glyphs first: the meaning distractors are picked from how similar
    # the glyphs look, so we need the bitmaps before choosing them.
    large = [font.kanji_glyph(c.codepoint, large_px, SET_LARGE) for c in cards]
    small = [font.kanji_glyph(c.codepoint, small_px, SET_SMALL) for c in cards]
    visual = [square_vector(g, small_px) for g in small]
    for c, d in zip(cards, meaning_distractors(cards, visual)):
        c.distractors = d
    for c, d in zip(cards, reading_distractors(cards)):
        c.distractors_reading = d

    glyphs: list[Glyph] = large + small

    text_cps = set(range(0x20, 0x7F))
    for s in [deck_name] + [x for c in cards for x in card_strings(c)]:
        text_cps.update(ord(ch) for ch in s)
    for cp in sorted(text_cps):
        if not font.has(cp):
            raise ValueError(f"font lacks text glyph U+{cp:04X} {chr(cp)!r}")
        glyphs.append(font.text_glyph(cp, text_px, SET_TEXT))
    line_h, ascent = font.text_metrics(text_px)

    # Set 3: the same readings rendered larger, for legible kana in answers.
    option_cps: set[int] = set()
    for c in cards:
        _, o, k = card_strings(c)
        option_cps.update(ord(ch) for ch in o)
        option_cps.update(ord(ch) for ch in k)
    for cp in sorted(option_cps):
        if not font.has(cp):
            raise ValueError(f"font lacks option glyph U+{cp:04X} {chr(cp)!r}")
        glyphs.append(font.text_glyph(cp, option_px, SET_OPTION))
    option_line_h, option_ascent = font.text_metrics(option_px)

    # Optional stroke order from KanjiVG. Masks are drawn in the same em square
    # as the set-0 glyphs, so strokes_px must equal large_px (docs/pack-format.md).
    px = strokes_px or large_px
    if strokes and px != large_px:
        raise ValueError(f"strokes_px ({px}) must equal large_px ({large_px}): "
                         "the masks share the em square the glyphs use")
    card_strokes = None
    if strokes:
        src = kanjivg
        own = kanjivg is None
        if src is None:
            src = default_source()
        try:
            card_strokes = [masks_for(c.codepoint, px, src) for c in cards]
        finally:
            if own:
                src.close()
        no_strokes = sum(1 for s in card_strokes if not s)
        if no_strokes:
            print(f"warning: no KanjiVG strokes for {no_strokes}/{len(cards)} cards",
                  file=sys.stderr)

    return write_pack(
        cards, glyphs, deck_name, lang, large_px, small_px, text_px, line_h, ascent,
        option_px, option_line_h, option_ascent, build_time,
        strokes=card_strokes, strokes_px=px,
    )
