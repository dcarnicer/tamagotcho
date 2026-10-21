"""Check the reading-distractor invariant of built packs.

For every card of every pack given:

- the pack validates (`read_pack`: offsets, glyphs, reading lists without
  empty items);
- no card in its precomputed reading-distractor list shows, as its first
  reading of a kind, a reading the target has of that kind. With the KANJIDIC2
  cache present the target's readings are KANJIDIC2's full lists, so a kind
  that `--common-readings` emptied is checked too;
- how often the list is short: the device takes its options from the list
  first (cards with the asked kind, a first reading that is not one of the
  target's, no repeats) and fills the rest by a random walk over the pack
  (docs/common-readings.md). A kind is "short" when the list gives fewer than
  the 3 wrong options a question needs.

Usage: uv run python scripts/check_reading_distractors.py out/*.tgc
Exit status 1 if any pack fails.
"""

from __future__ import annotations

import sys
from pathlib import Path

from tamagotcho_pack import paths
from tamagotcho_pack.pack import READING_SEP, read_pack

WRONG_OPTIONS = 3


def first(s: str) -> str:
    return s.split(READING_SEP)[0].replace(".", "") if s else ""


def all_of(s: str) -> set[str]:
    return {r.replace(".", "") for r in s.split(READING_SEP)} if s else set()


def main(argv: list[str]) -> int:
    full: dict[int, tuple[set[str], set[str]]] = {}
    if paths.KANJIDIC_GZ.exists():
        from tamagotcho_pack.kanjidic import parse_kanjidic

        for k in parse_kanjidic(paths.KANJIDIC_GZ):
            full[k.codepoint] = ({r.replace(".", "") for r in k.onyomi},
                                 {r.replace(".", "") for r in k.kunyomi})
    else:
        print("(no KANJIDIC2 in the cache: checking against the readings in each pack only)")
    failed = False
    for name in argv:
        p = read_pack(Path(name).read_bytes())
        bad: list[str] = []
        short = asked = 0
        for i, c in enumerate(p.cards):
            mine = {"on": all_of(c.onyomi), "kun": all_of(c.kunyomi)}
            if c.codepoint in full:
                mine["on"] |= full[c.codepoint][0]
                mine["kun"] |= full[c.codepoint][1]
            for j in c.distractors_reading:
                o = p.cards[j]
                for kind, theirs in (("on", o.onyomi), ("kun", o.kunyomi)):
                    if theirs and first(theirs) in mine[kind]:
                        bad.append(f"{chr(c.codepoint)}: {chr(o.codepoint)}'s {first(theirs)}")
            for kind, own in (("on", c.onyomi), ("kun", c.kunyomi)):
                if not own:
                    continue
                asked += 1
                seen = {first(own)}
                for j in c.distractors_reading:
                    s = first(p.cards[j].onyomi if kind == "on" else p.cards[j].kunyomi)
                    if s and s not in all_of(own) and s not in seen:
                        seen.add(s)
                short += len(seen) - 1 < WRONG_OPTIONS
        status = "OK" if not (p.errors or bad) else "FAIL"
        print(f"{name}: {status}, {len(p.cards)} cards; {short} of {asked} asked kinds "
              f"({short / max(asked, 1) * 100:.1f} %) have fewer than {WRONG_OPTIONS} options "
              f"in the precomputed list")
        for e in (p.errors + bad)[:20]:
            print("  -", e)
        failed |= bool(p.errors or bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
