"""Report for `build --meaning-order` (docs/meaning-order.md).

For every kanji of JLPT N5-N1 and the Jōyō list, orders its KANJIDIC2 meanings
the way `build --meaning-order` does (tangocho_pack.meanings): English by the
learner signal, Spanish by the new English order, then the overrides. Prints,
as Markdown:

- how many glosses match, how many kanji change their first meaning (the one
  the device shows) and how many store a different set of meanings (the
  builder keeps as many as English has, at most three), per language and JLPT
  level, and against the first version (Spanish by KANJIDIC2's English order);
- a sanity table for well-known kanji;
- every first-meaning change in N5, N4 and N3, English and Spanish;
- the overrides and what they change;
- the changes to double-check: a new Spanish first meaning that matches only
  through a related form, or an old first that matched nothing (a gap in the
  dictionary?) next to a new one that translates a later English gloss.

Usage (after `tangocho-pack fetch --readings --meanings`):

    uv run python scripts/meaning_order_report.py [--kanji 持界題] [--tsv out/changes.tsv]
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from tangocho_pack import paths
from tangocho_pack.kanjidic import JOYO_GRADES, MAX_MEANINGS, load_new_jlpt, parse_kanjidic
from tangocho_pack.meanings import (EnglishOrder, MeaningOrder, load_freedict, load_learner_vocab,
                                    load_overrides, reorder_raw)
from tangocho_pack.readings import load_jlpt_vocab

SANITY = ("持界題院安重生行明気日上下大人中女子書毎聞見食小天先半後前"
          "私機原験計申背支頂更御処積言帰教失難種眠来")
LEVELS = (5, 4, 3, 2, 1, 0)


def stored(glosses: list[str], en: list[str]) -> list[str]:
    """What make_cards keeps: as many as English has, at most MAX_MEANINGS."""
    return glosses[: min(len(en), MAX_MEANINGS)]


def lv_name(lv: int) -> str:
    return f"N{lv}" if lv else "no level"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kanji", default=SANITY, help="kanji for the sanity table")
    ap.add_argument("--tsv", help="write every kanji whose first meaning changes to this file")
    a = ap.parse_args()
    need = [paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.FREEDICT_ENG_SPA, paths.JMDICT_GZ,
            *paths.JLPT_VOCAB.values()]
    for p in need:
        if not p.exists():
            print(f"missing {p}: run `tangocho-pack fetch --readings --meanings`", file=sys.stderr)
            return 2
    new_jlpt = load_new_jlpt(paths.KANJI_JSON)
    raw = [k for k in parse_kanjidic(paths.KANJIDIC_GZ)
           if new_jlpt.get(k.codepoint) or k.grade in JOYO_GRADES]
    english = EnglishOrder(load_learner_vocab(load_jlpt_vocab(paths.JLPT_VOCAB), paths.JMDICT_GZ))
    spanish = MeaningOrder(load_freedict(paths.FREEDICT_ENG_SPA))
    overrides = load_overrides(paths.MEANING_OVERRIDES)

    v1, _ = reorder_raw(raw, "es", None, spanish)                    # Spanish by KANJIDIC2's English
    plain_es, _ = reorder_raw(raw, "es", english, spanish)            # no overrides
    plain_en, _ = reorder_raw(raw, "en", english, None)
    v2_es, st_es = reorder_raw(raw, "es", english, spanish, overrides)
    v2_en, st_en = reorder_raw(raw, "en", english, None, overrides)

    rows = {}
    by_level = {lv: Counter() for lv in LEVELS}
    for k, a1, pe, pn, e2, n2 in zip(raw, v1, plain_es, plain_en, v2_es, v2_en):
        en, es = k.meanings.get("en", []), k.meanings.get("es", [])
        if not en:
            continue
        lv = by_level[new_jlpt.get(k.codepoint, 0)]
        lv["kanji"] += 1
        r = dict(en=en, en2=n2.meanings["en"], en_plain=pn.meanings["en"], es=es,
                 es1=a1.meanings.get("es", []), es2=e2.meanings.get("es", []),
                 es_plain=pe.meanings.get("es", []), level=new_jlpt.get(k.codepoint, 0))
        rows[k.codepoint] = r
        lv["en first"] += r["en2"][0] != en[0]
        lv["en stored"] += set(stored(r["en2"], en)) != set(stored(en, en))
        if es:
            lv["with es"] += 1
            lv["es first"] += r["es2"][0] != es[0]
            lv["es stored"] += set(stored(r["es2"], en)) != set(stored(es, en))
            lv["es first vs v1"] += r["es2"][0] != r["es1"][0]

    tot = Counter()
    for c in by_level.values():
        tot.update(c)
    print(f"## Totals ({len(rows)} kanji: JLPT N5-N1 ∪ Jōyō)\n")
    print(f"- English: first meaning changed for {tot['en first']} kanji, stored meanings "
          f"(at most {MAX_MEANINGS}) for {tot['en stored']}; {st_en.overridden} English overrides.")
    print(f"- Spanish ({tot['with es']} kanji with Spanish meanings): {st_es.matched} of "
          f"{st_es.glosses} glosses matched to an English gloss; first meaning changed for "
          f"{tot['es first']} (against KANJIDIC2), stored meanings for {tot['es stored']}; "
          f"{st_es.overridden} Spanish overrides. Against the first version (Spanish by "
          f"KANJIDIC2's English order) the first Spanish meaning differs for "
          f"{tot['es first vs v1']}.\n")
    print("| | N5 | N4 | N3 | N2 | N1 | no level |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for key, label in (("kanji", "kanji"), ("en first", "English first meaning changed"),
                       ("en stored", "English stored meanings changed"),
                       ("es first", "Spanish first meaning changed"),
                       ("es stored", "Spanish stored meanings changed"),
                       ("es first vs v1", "Spanish first differs from the first version")):
        print(f"| {label} | " + " | ".join(str(by_level[lv][key]) for lv in LEVELS) + " |")

    print("\n## Sanity table\n")
    print("First meaning, KANJIDIC2 → `--meaning-order` (= unchanged); *v1*: the first "
          "version's Spanish (by KANJIDIC2's English order); the stored lists in full.\n")
    print("| kanji | English | Spanish | v1 | stored English | stored Spanish |")
    print("|---|---|---|---|---|---|")
    for ch in a.kanji:
        r = rows.get(ord(ch))
        if not r:
            continue
        en = r["en"][0] if r["en2"][0] == r["en"][0] else f"{r['en'][0]} → **{r['en2'][0]}**"
        es = ("—" if not r["es"] else r["es"][0] if r["es2"][0] == r["es"][0]
              else f"{r['es'][0]} → **{r['es2'][0]}**")
        print(f"| {ch} | {en} | {es} | {r['es1'][0] if r['es1'] else '—'} | "
              f"{'; '.join(stored(r['en2'], r['en']))} | {'; '.join(stored(r['es2'], r['en']))} |")

    def changes(lv: int, lang: str) -> list[str]:
        out = []
        for cp, r in rows.items():
            old, new = r[lang], r[lang + "2"]
            if r["level"] == lv and old and new[0] != old[0]:
                ov = " (override)" if (chr(cp), lang) in overrides else ""
                out.append(f"{chr(cp)} {old[0]} → {new[0]}{ov}")
        return out

    for lv in (5, 4, 3):
        print(f"\n## JLPT N{lv}: every first-meaning change (KANJIDIC2 → new)\n")
        for lang, label in (("en", "English"), ("es", "Spanish")):
            ch = changes(lv, lang)
            print(f"- {label} ({len(ch)}): " + ", ".join(ch) + ".")

    print("\n## Overrides\n")
    print("| kanji | language | first | without the override | reason |")
    print("|---|---|---|---|---|")
    for (ch, lang), ov in sorted(overrides.items(), key=lambda x: x[1].line):
        r = rows.get(ord(ch))
        before = (r["en_plain"] if lang == "en" else r["es_plain"])[0] if r else "?"
        print(f"| {ch} | {lang} | {'; '.join(ov.order)} | {before} | {ov.reason} |")

    print("\n## To double-check (Spanish)\n")
    for lv in LEVELS:
        weak, gap = [], []
        for cp, r in rows.items():
            if r["level"] != lv or not r["es"] or r["es2"][0] == r["es"][0] \
                    or (chr(cp), "es") in overrides:
                continue
            m_new = spanish.match(r["es2"][0], r["en2"])
            m_old = spanish.match(r["es"][0], r["en2"])
            line = f"{chr(cp)} {r['es'][0]} → **{r['es2'][0]}** (en {'; '.join(r['en2'][:3])})"
            if m_new and m_new[1] != "translation":
                weak.append(line)
            elif m_old is None and m_new and m_new[0] > 0:
                gap.append(line)
        if weak:
            print(f"- {lv_name(lv)}, new first by a related form: " + "; ".join(weak))
        if gap:
            print(f"- {lv_name(lv)}, old first unmatched, new first translates a later English "
                  "gloss: " + "; ".join(gap))

    if a.tsv:
        out = Path(a.tsv)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            f.write("kanji\tjlpt\tlang\tfirst before\tfirst after\tfirst v1\toverride\t"
                    "before\tafter\n")
            for cp, r in sorted(rows.items(), key=lambda x: (-x[1]["level"], x[0])):
                for lang in ("en", "es"):
                    old, new = r[lang], r[lang + "2"]
                    if old and new[0] != old[0]:
                        v1 = r["es1"][0] if lang == "es" else old[0]
                        f.write(f"{chr(cp)}\t{r['level']}\t{lang}\t{old[0]}\t{new[0]}\t{v1}\t"
                                f"{'yes' if (chr(cp), lang) in overrides else ''}\t"
                                f"{'; '.join(old)}\t{'; '.join(new)}\n")
        print(f"\n(wrote {out})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
