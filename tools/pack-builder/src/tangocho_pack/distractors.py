"""Distractor selection for meaning and reading questions.

Two lists per card, both plain lists of card indices, best first:

- **meaning**: visually similar kanji, compared from their own bitmaps, so the
  wrong options are the ones a learner might actually confuse; same JLPT level
  first, then shared classical radical, then visual similarity, then closest
  stroke count.
- **reading**: cards whose reading *of the same kind* is confusable (a kana or
  two apart, e.g. ショウ / ジョウ), same level first.

Cards sharing a meaning with the target are excluded from the meaning list: the
quiz shows one gloss per option, so an overlap would make two options correct.
"""

from __future__ import annotations

import random

from .glyphs import Glyph, unpack_4bpp

MAX_DISTRACTORS = 12
GRID = 16  # similarity ink-mask resolution


def levenshtein(a: str, b: str) -> int:
    if a == b:
        return 0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def display_reading(readings) -> str:
    """First reading of a card's list (or a '、'-joined string), dots removed."""
    if not readings:
        return ""
    first = readings[0] if isinstance(readings, list) else readings.split("、")[0]
    return first.replace(".", "")


def square_vector(glyph: Glyph, px: int, grid: int = GRID) -> tuple[float, ...]:
    """A grid x grid vector of average ink coverage (0..15) for the glyph placed
    in its px x px em square. Cosine similarity on this ranks like-looking kanji
    (日/目, 水/木) well above unrelated ones."""
    ink = [[0] * px for _ in range(px)]
    if glyph.width and glyph.height:
        vals = unpack_4bpp(glyph.data, glyph.width, glyph.height)
        top = glyph.y_off + px
        for y in range(glyph.height):
            for x in range(glyph.width):
                yy, xx = top + y, glyph.x_off + x
                if 0 <= yy < px and 0 <= xx < px:
                    ink[yy][xx] = vals[y * glyph.width + x]
    step = px / grid
    out = []
    for gy in range(grid):
        y0 = int(gy * step)
        y1 = min(max(int((gy + 1) * step), y0 + 1), px)
        for gx in range(grid):
            x0 = int(gx * step)
            x1 = min(max(int((gx + 1) * step), x0 + 1), px)
            s = sum(ink[y][x] for y in range(y0, y1) for x in range(x0, x1))
            out.append(s / ((y1 - y0) * (x1 - x0)))
    return tuple(out)


def _similarity(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    na = sum(x * x for x in a) ** 0.5
    nb = sum(x * x for x in b) ** 0.5
    return dot / (na * nb) if na and nb else 0.0


def meaning_distractors(cards: list, visual: list[int], k: int = MAX_DISTRACTORS,
                        seed: int = 1) -> list[list[int]]:
    result: list[list[int]] = []
    for i, c in enumerate(cards):
        rng = random.Random(seed * 1_000_003 + c.codepoint)
        mine = {m.lower() for m in c.meanings}
        my_radicals = set(c.radicals)
        scored = []
        for j, o in enumerate(cards):
            if j == i:
                continue
            if mine & {m.lower() for m in o.meanings}:
                continue  # would make two options correct
            # Scoring key, best (smallest) first:
            #   1. same JLPT level first        -> level always outranks radicals
            #   2. shared classical radical     -> same dictionary family (氵, 木, 口…)
            #   3. visual similarity (cosine)   -> how alike the glyphs look
            #   4. closest stroke count         -> a mild, human-plausible tie-break
            #   5. seeded random, then index    -> deterministic tie-break
            # Sharing a radical is a deliberate, explainable confusion axis, so it
            # is preferred over the noisier ink cosine for candidates that are
            # otherwise plausible (same level). A card with no radical data shares
            # nothing, so `not shared` is constant and the key degrades exactly to
            # the pre-radical ordering.
            shared = bool(my_radicals & set(o.radicals))
            scored.append(((o.jlpt != c.jlpt), not shared,
                           -_similarity(visual[i], visual[j]),
                           abs(o.strokes - c.strokes), rng.random(), j))
        scored.sort(key=lambda s: s[:5])
        result.append([s[5] for s in scored[:k]])
    return result


def _readings(readings) -> set[str]:
    """All readings of a card's list (or a '、'-joined string), dots removed."""
    if not readings:
        return set()
    items = readings if isinstance(readings, list) else readings.split("、")
    return {r.replace(".", "") for r in items if r}


def reading_distractors(cards: list, k: int = MAX_DISTRACTORS, seed: int = 1) -> list[list[int]]:
    result: list[list[int]] = []
    for i, c in enumerate(cards):
        rng = random.Random(seed * 1_000_003 + c.codepoint)
        target = {"on": display_reading(c.onyomi), "kun": display_reading(c.kunyomi)}
        t_sets = {"on": _readings(c.onyomi), "kun": _readings(c.kunyomi)}
        scored = []
        for j, o in enumerate(cards):
            if j == i:
                continue
            other = {"on": display_reading(o.onyomi), "kun": display_reading(o.kunyomi)}
            # A candidate whose reading is *another valid reading of the target*
            # would make two options correct: never offer it.
            if any(other[kd] and other[kd] in t_sets[kd] for kd in ("on", "kun")):
                continue
            dists = [levenshtein(target[kd], other[kd]) for kd in ("on", "kun")
                     if target[kd] and other[kd]]
            dists = [d for d in dists if d > 0]  # identical readings are deduped anyway
            if not dists:
                continue
            best = min(dists)
            if best > 3:
                continue  # not confusable
            scored.append(((o.jlpt != c.jlpt), best, abs(o.strokes - c.strokes), rng.random(), j))
        scored.sort(key=lambda s: s[:4])
        result.append([s[4] for s in scored[:k]])
    return result
