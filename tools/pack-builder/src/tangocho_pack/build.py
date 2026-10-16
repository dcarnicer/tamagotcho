"""Orchestration: cards + font -> pack bytes."""

from __future__ import annotations

import sys
from pathlib import Path

from .distractors import DEFAULT_STRATEGY, DistractorStrategy
from .glyphs import SET_LARGE, SET_SMALL, FontRenderer, Glyph
from .kanjidic import Card
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
    strategy: DistractorStrategy = DEFAULT_STRATEGY,
    build_time: int | None = None,
) -> bytes:
    font = FontRenderer(font_path)
    kept = []
    for c in cards:
        if font.has(c.codepoint):
            kept.append(c)
        else:
            print(f"warning: font lacks U+{c.codepoint:04X} {c.char}; card dropped", file=sys.stderr)
    cards = kept
    for c, d in zip(cards, strategy(cards)):
        c.distractors = d

    glyphs: list[Glyph] = []
    for c in cards:
        glyphs.append(font.kanji_glyph(c.codepoint, large_px, SET_LARGE))
        glyphs.append(font.kanji_glyph(c.codepoint, small_px, SET_SMALL))

    text_cps = set(range(0x20, 0x7F))
    for s in [deck_name] + [x for c in cards for x in card_strings(c)]:
        text_cps.update(ord(ch) for ch in s)
    for cp in sorted(text_cps):
        if not font.has(cp):
            raise ValueError(f"font lacks text glyph U+{cp:04X} {chr(cp)!r}")
        glyphs.append(font.text_glyph(cp, text_px))
    line_h, ascent = font.text_metrics(text_px)
    return write_pack(
        cards, glyphs, deck_name, lang, large_px, small_px, text_px, line_h, ascent, build_time
    )
