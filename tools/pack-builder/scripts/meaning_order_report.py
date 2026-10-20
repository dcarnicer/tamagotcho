"""Report for `build --meaning-order` (docs/meaning-order.md).

For every kanji of JLPT N5-N1 and the Jōyō list, orders its KANJIDIC2 Spanish
meanings the way `build --meaning-order` does (tangocho_pack.meanings) and
prints, as Markdown:

- how many glosses match an English gloss (by translation or a related form),
  how many kanji change their first meaning (the one the device shows) and how
  many store a different set of meanings (the builder keeps as many as English
  has, at most three), overall and per JLPT level;
- a sanity table for well-known kanji;
- the full list of N5 changes;
- the changes to double-check: the new first meaning matches only through a
  related form, or the old first matched nothing (a gap in the dictionary?)
  and the new one translates a later English gloss.

Usage (after `tangocho-pack fetch --meanings`):

    uv run python scripts/meaning_order_report.py [--kanji 持界題] [--tsv out/changes.tsv]
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from tangocho_pack import paths
from tangocho_pack.kanjidic import JOYO_GRADES, MAX_MEANINGS, load_new_jlpt, parse_kanjidic
from tangocho_pack.meanings import MeaningOrder, load_freedict

SANITY = "持界題院安重生行明気日上下大人中女子書毎聞見食小天先半後前"
LEVELS = (5, 4, 3, 2, 1, 0)
LANG = "es"


def stored(glosses: list[str], en: list[str]) -> list[str]:
    """What make_cards keeps: as many as English has, at most MAX_MEANINGS."""
    return glosses[: min(len(en), MAX_MEANINGS)]


def marked(ranked, en) -> str:
    """`sostener¹ · mantener¹ · duración`: each gloss with the (1-based) English
    gloss it matched, `~` for a related form, nothing when unmatched."""
    out = []
    for g, m in ranked:
        if not m:
            out.append(g)
        elif m[1] == "translation":
            out.append(f"{g}<sup>{m[0] + 1}</sup>")
        else:
            out.append(f"{g}<sup>~{m[0]}</sup>")
    return " · ".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kanji", default=SANITY, help="kanji for the sanity table")
    ap.add_argument("--tsv", help="write every kanji whose first meaning changes to this file")
    a = ap.parse_args()
    for p in (paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.FREEDICT_ENG_SPA):
        if not p.exists():
            print(f"missing {p}: run `tangocho-pack fetch --meanings`", file=sys.stderr)
            return 2
    new_jlpt = load_new_jlpt(paths.KANJI_JSON)
    raw = [k for k in parse_kanjidic(paths.KANJIDIC_GZ)
           if new_jlpt.get(k.codepoint) or k.grade in JOYO_GRADES]
    order = MeaningOrder(load_freedict(paths.FREEDICT_ENG_SPA))

    rows = {}
    st = Counter()
    by_level = {lv: Counter() for lv in LEVELS}
    for k in raw:
        es, en = k.meanings.get(LANG, []), k.meanings.get("en", [])
        lv = by_level[new_jlpt.get(k.codepoint, 0)]
        lv["kanji"] += 1
        if not es or not en:
            st["no es" if not es else "no en"] += 1
            continue
        ranked = order.ranked(es, en)
        after = [g for g, _ in ranked]
        st["kanji"] += 1
        st["glosses"] += len(es)
        for _, m in ranked:
            st["unmatched" if not m else m[1]] += 1
        st["nothing matched"] += all(m is None for _, m in ranked)
        changed = after[0] != es[0]
        st["first changed"] += changed
        st["stored set changed"] += set(stored(after, en)) != set(stored(es, en))
        lv["first changed"] += changed
        lv["stored set changed"] += set(stored(after, en)) != set(stored(es, en))
        rows[k.codepoint] = (es, after, en, ranked)

    print(f"## Totals ({len(raw)} kanji: JLPT N5-N1 ∪ Jōyō)\n")
    print(f"- with Spanish and English meanings: {st['kanji']} (no Spanish: {st['no es']}, "
          "these fall back to English)")
    g = st["glosses"]
    print(f"- Spanish glosses: {g}; matched by translation {st['translation']} "
          f"({st['translation'] / g * 100:.0f} %), by a related form "
          f"{g - st['translation'] - st['unmatched']} (stem {st['stem']}, cognate {st['cognate']}, "
          f"head words {st['head']}, synonym {st['synonym']}); unmatched {st['unmatched']} "
          f"({st['unmatched'] / g * 100:.0f} %)")
    print(f"- kanji with no gloss matched (order unchanged): {st['nothing matched']}")
    print(f"- first meaning changed: {st['first changed']}; stored meanings (at most as many "
          f"as English, at most {MAX_MEANINGS}) changed: {st['stored set changed']}\n")
    print("| | N5 | N4 | N3 | N2 | N1 | no level |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for key in ("kanji", "first changed", "stored set changed"):
        print(f"| {key} | " + " | ".join(str(by_level[lv][key]) for lv in LEVELS) + " |")

    print("\n## Sanity table\n")
    print("Superscript: the English gloss a Spanish gloss translates (1 = first); `~n`: a "
          "related form of English gloss n, which counts one position later; none: unmatched. "
          "*Stored*: what the pack keeps.\n")
    print("| kanji | English | stored before | stored after | full order |")
    print("|---|---|---|---|---|")
    for ch in a.kanji:
        if ord(ch) not in rows:
            continue
        es, after, en, ranked = rows[ord(ch)]
        b, f = "; ".join(stored(es, en)), "; ".join(stored(after, en))
        print(f"| {ch} | {'; '.join(en)} | {b} | {f if f != b else '='} | {marked(ranked, en)} |")

    def change_line(cp):
        es, after, en, ranked = rows[cp]
        return f"{chr(cp)} {es[0]} → **{after[0]}** (en {'; '.join(en[:3])})"

    print("\n## JLPT N5, every first-meaning change\n")
    n5 = [cp for cp in rows if new_jlpt.get(cp) == 5 and rows[cp][1][0] != rows[cp][0][0]]
    for cp in sorted(n5, key=lambda c: rows[c][0][0]):
        print(f"- {change_line(cp)}")

    print("\n## To double-check\n")
    for lv in LEVELS:
        weak, gap = [], []
        for cp, (es, after, en, ranked) in rows.items():
            if new_jlpt.get(cp, 0) != lv or after[0] == es[0]:
                continue
            m_new = ranked[0][1]
            m_old = next(m for g2, m in ranked if g2 == es[0])
            if m_new and m_new[1] != "translation":
                weak.append(cp)
            elif m_old is None and m_new and m_new[0] > 0:
                gap.append(cp)
        name = f"N{lv}" if lv else "no level"
        if weak:
            print(f"- {name}, new first by a related form: " + "; ".join(change_line(c) for c in weak))
        if gap:
            print(f"- {name}, old first unmatched, new first translates a later English gloss: "
                  + "; ".join(change_line(c) for c in gap))

    if a.tsv:
        out = Path(a.tsv)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            f.write("kanji\tjlpt\tfirst before\tfirst after\tenglish\tspanish before\tspanish after\n")
            for cp, (es, after, en, _) in sorted(rows.items(), key=lambda r: (-new_jlpt.get(r[0], 0), r[0])):
                if after[0] != es[0]:
                    f.write(f"{chr(cp)}\t{new_jlpt.get(cp, 0)}\t{es[0]}\t{after[0]}\t{'; '.join(en)}\t"
                            f"{'; '.join(es)}\t{'; '.join(after)}\n")
        print(f"\n(wrote {out})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
