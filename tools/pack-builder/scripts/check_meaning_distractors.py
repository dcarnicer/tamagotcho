"""Check the meaning-distractor invariant of built packs.

The device shows one meaning per option, the card's first (an option of a
meaning question, or the prompt of a meaning -> kanji question). For every
card of every pack given:

- the pack validates (`read_pack`);
- no card in its precomputed meaning-distractor list shows, as its first
  meaning, a meaning of the target, and the target's first meaning is not a
  meaning of the distractor card. With the KANJIDIC2 cache present a card's
  meanings are KANJIDIC2's full list in the pack's language (English for a card
  with the fallback flag), so the meanings the builder's caps leave out
  (`--meaning-order` can change which ones) are checked too;
- how often the list is short: fewer than the 3 wrong options a question
  needs (distinct first meanings that are not the target's); the device then
  fills from a random walk over the pack.

Usage: uv run python scripts/check_meaning_distractors.py out/*.tgc
Exit status 1 if any pack fails.
"""

from __future__ import annotations

import sys
from pathlib import Path

from tamagotcho_pack import paths
from tamagotcho_pack.pack import MEANING_SEP, read_pack

WRONG_OPTIONS = 3


def items(s: str) -> list[str]:
    return [m for m in s.split(MEANING_SEP) if m] if s else []


def main(argv: list[str]) -> int:
    full: dict[int, dict[str, list[str]]] = {}
    if paths.KANJIDIC_GZ.exists():
        from tamagotcho_pack.kanjidic import parse_kanjidic

        full = {k.codepoint: k.meanings for k in parse_kanjidic(paths.KANJIDIC_GZ)}
    else:
        print("(no KANJIDIC2 in the cache: checking against the meanings in each pack only)")
    failed = False
    for name in argv:
        p = read_pack(Path(name).read_bytes())
        lang = p.header["lang"]

        def every(c) -> set[str]:
            got = {m.lower() for m in items(c.meanings)}
            src = full.get(c.codepoint, {})
            got |= {m.lower() for m in src.get("en" if c.flags & 1 else lang, [])}
            return got

        alls = [every(c) for c in p.cards]
        firsts = [(items(c.meanings) or [""])[0].lower() for c in p.cards]
        bad: list[str] = []
        short = asked = 0
        for i, c in enumerate(p.cards):
            if not firsts[i]:
                continue
            asked += 1
            seen = {firsts[i]}
            for j in c.distractors:
                o = p.cards[j]
                if firsts[j] and firsts[j] in alls[i]:
                    bad.append(f"{chr(c.codepoint)}: {chr(o.codepoint)} shows {firsts[j]!r}, "
                               f"a meaning of {chr(c.codepoint)}")
                if firsts[i] in alls[j]:
                    bad.append(f"{chr(c.codepoint)}: its {firsts[i]!r} is a meaning of "
                               f"{chr(o.codepoint)}")
                if firsts[j] and firsts[j] not in alls[i]:
                    seen.add(firsts[j])
            short += len(seen) - 1 < WRONG_OPTIONS
        status = "OK" if not (p.errors or bad) else "FAIL"
        print(f"{name}: {status}, {len(p.cards)} cards ({lang}); {short} of {asked} "
              f"({short / max(asked, 1) * 100:.1f} %) have fewer than {WRONG_OPTIONS} options in "
              "the precomputed meaning list")
        for e in (p.errors + bad)[:20]:
            print("  -", e)
        failed |= bool(p.errors or bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
