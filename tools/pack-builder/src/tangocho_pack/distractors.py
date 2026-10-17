"""Distractor selection. Strategies are plain callables so smarter ones
(visual similarity, shared radicals, confusable readings) can be plugged in.

A strategy receives the ordered card list and returns, for each card, a list
of other card *indices*, best distractor first.
"""

from __future__ import annotations

import random
from typing import Callable

from .kanjidic import Card

DistractorStrategy = Callable[[list[Card]], list[list[int]]]
MAX_DISTRACTORS = 12


def same_level_similar_strokes(
    cards: list[Card], k: int = MAX_DISTRACTORS, seed: int = 1
) -> list[list[int]]:
    """Prefer same JLPT level, then closest stroke count; seeded tie-break.

    Candidates sharing *any* meaning with the target are skipped: the quiz picks
    one of a card's meanings at random, so an overlap could make two options
    correct at once.
    """
    result: list[list[int]] = []
    for i, c in enumerate(cards):
        rng = random.Random(seed * 1_000_003 + c.codepoint)
        mine = {m.lower() for m in c.meanings}
        scored = []
        for j, o in enumerate(cards):
            if j == i:
                continue
            if mine & {m.lower() for m in o.meanings}:
                continue
            scored.append(((o.jlpt != c.jlpt), abs(o.strokes - c.strokes), rng.random(), j))
        scored.sort()
        result.append([s[3] for s in scored[:k]])
    return result


DEFAULT_STRATEGY: DistractorStrategy = same_level_similar_strokes
