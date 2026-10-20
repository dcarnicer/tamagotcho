"""Prototype report for `build --common-readings` (docs/common-readings.md).

For every kanji of JLPT N5-N1 and the Jōyō list, orders its KANJIDIC2 readings
the way `build --common-readings` does (tangocho_pack.readings) and prints, as
Markdown:

- how many kanji change the first on'yomi / kun'yomi (the reading the device
  asks) with each weighting: JMdict priorities only, + wordfreq, + Tatoeba,
  + the Jōyō table's approved readings first (the builder's order);
- how many readings no word uses;
- the class (approved / special / not in the table) of the asked reading of
  the Jōyō kanji, before and after;
- the kanji with no reading of a kind worth asking (input for a pack-format
  "do not ask" flag);
- a sanity table for well-known kanji.

Usage (after `tangocho-pack fetch --readings`):

    uv run python scripts/common_readings_report.py [--kanji 九生日] [--tsv out/changes.tsv]
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from tangocho_pack import paths
from tangocho_pack.joyo_table import classify, load_pdf
from tangocho_pack.kanjidic import JOYO_GRADES, load_new_jlpt, parse_kanjidic
from tangocho_pack.readings import bound_forms, compute_scores, reorder_raw, share

SANITY = "九生日下上行人大何今明重楽方間"


def fmt_order(readings, scores, bound, entry):
    """`キュウ 86 · ク 14`: readings in order with their share (%) of the kind's
    score; `*` = indented in the Jōyō table, `†` = not in it, `ᵇ` = bound form."""
    sh = share(readings, scores)
    out = []
    for r in readings:
        mark = {"joyo": "", "special": "*", "other": "†"}[classify(r, entry)] if entry else ""
        b = "ᵇ" if r in bound else ""
        out.append(f"{r}{mark}{b} {sh[r] * 100:.0f}")
    return " · ".join(out) if out else "—"


def first(old, new):
    if not old:
        return "—"
    return old[0] if old[0] == new[0] else f"{old[0]} → **{new[0]}**"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kanji", default=SANITY, help="kanji for the sanity table")
    ap.add_argument("--tsv", help="write every kanji whose first reading changes to this file")
    a = ap.parse_args()

    for p in (paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.JMDICT_GZ, paths.JMDICT_FURIGANA,
              paths.WORDFREQ_JA, paths.TATOEBA_JPN, paths.JOYO_PDF):
        if not p.exists():
            print(f"missing {p}: run `tangocho-pack fetch --readings`", file=sys.stderr)
            return 2
    raw_all = parse_kanjidic(paths.KANJIDIC_GZ)
    new_jlpt = load_new_jlpt(paths.KANJI_JSON)
    raw = [k for k in raw_all if new_jlpt.get(k.codepoint) or k.grade in JOYO_GRADES]
    by_cp = {k.codepoint: k for k in raw}
    joyo = load_pdf(paths.JOYO_PDF)

    jm = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA)
    wo = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA)
    wf = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA,
                        paths.TATOEBA_JPN)
    variants = [
        ("JMdict priorities", reorder_raw(raw, jm)),
        ("+ wordfreq", reorder_raw(raw, wo)),
        ("+ Tatoeba", reorder_raw(raw, wf)),
        ("+ Jōyō approved first", reorder_raw(raw, wf, joyo)),
    ]
    freq_only = {k.codepoint: k for k in variants[2][1][0]}
    final = {k.codepoint: k for k in variants[3][1][0]}
    st = variants[3][1][1]

    n_jlpt = sum(1 for k in raw if new_jlpt.get(k.codepoint))
    n_joyo = sum(1 for k in raw if k.grade in JOYO_GRADES)
    print(f"## Set\n\n{len(raw)} kanji: JLPT N5-N1 ({n_jlpt}) ∪ Jōyō ({n_joyo}). "
          f"Jōyō table: {len(joyo)} kanji, {sum(len(v) for v in joyo.values())} readings, "
          f"{sum(1 for v in joyo.values() for r in v if r.indented)} indented.\n")
    print(f"Per-kanji furigana parts matched to a KANJIDIC2 reading: {wf.matched:,}; "
          f"unmatched: {wf.unmatched:,} "
          f"({wf.unmatched / max(1, wf.matched + wf.unmatched) * 100:.1f} %).\n")

    print("## First reading changes (each column adds to the previous)\n")
    print("| | " + " | ".join(n for n, _ in variants) + " |")
    print("|---|" + "---:|" * len(variants))
    with_on = sum(1 for k in raw if k.onyomi)
    with_kun = sum(1 for k in raw if k.kunyomi)
    print(f"| first on'yomi changes (of {with_on}) | "
          + " | ".join(str(s.first_on_changed) for _, (_, s) in variants) + " |")
    print(f"| first kun'yomi changes (of {with_kun}) | "
          + " | ".join(str(s.first_kun_changed) for _, (_, s) in variants) + " |")
    print(f"\nReadings no word uses: {st.unscored_on} of {st.readings_on} on'yomi, "
          f"{st.unscored_kun} of {st.readings_kun} kun'yomi. They stay in the card, last.")
    all_unscored = Counter()
    for k in raw:
        sc = wf.score.get(k.codepoint, {})
        for kind, lst in (("on", k.onyomi), ("kun", k.kunyomi)):
            if lst and not any(sc.get(r) for r in lst):
                all_unscored[kind] += 1
    print(f"Kanji with no used reading of a kind (KANJIDIC2 order kept): "
          f"{all_unscored['on']} on, {all_unscored['kun']} kun.\n")

    print("## The asked reading against the Jōyō table (Jōyō kanji only)\n")
    jk = [k for k in raw if chr(k.codepoint) in joyo]
    print("| kind | order | approved | special (indented) | not in the table |")
    print("|---|---|---:|---:|---:|")
    for kind in ("on", "kun"):
        attr = "onyomi" if kind == "on" else "kunyomi"
        for label, src in (("KANJIDIC2", by_cp), ("frequency only", freq_only),
                           ("`--common-readings`", final)):
            c = Counter()
            for k in jk:
                lst = getattr(src[k.codepoint], attr)
                if lst:
                    c[classify(lst[0], joyo[chr(k.codepoint)])] += 1
            print(f"| {kind} | {label} | {c['joyo']} | {c['special']} | {c['other']} |")

    print("\n## Kinds with nothing worth asking\n")
    no = {}
    for kind in ("on", "kun"):
        attr = "onyomi" if kind == "on" else "kunyomi"
        no[kind] = [k for k in jk if getattr(k, attr) and not any(
            classify(r, joyo[chr(k.codepoint)]) == "joyo" for r in getattr(k, attr))]
    print(f"Jōyō kanji whose KANJIDIC2 readings of a kind include none the table approves: "
          f"on'yomi {len(no['on'])}, kun'yomi {len(no['kun'])}. With a \"do not ask\" flag "
          f"the device would ask the other kind for them. Kun'yomi, by school grade:\n")
    by_grade = Counter(k.grade for k in no["kun"])
    print("| grade | " + " | ".join(str(g) for g in sorted(by_grade)) + " |")
    print("|---|" + "---:|" * len(by_grade))
    print("| kanji | " + " | ".join(str(by_grade[g]) for g in sorted(by_grade)) + " |")
    print("\nExamples (asked kun'yomi today): " + "、".join(
        f"{chr(k.codepoint)} {k.kunyomi[0]}" for k in no["kun"] if k.grade <= 3) + "\n")

    print("## Sanity table\n")
    print("Order and shares of `--common-readings`; a share is the reading's % of its "
          "kind's score. `*` indented in the Jōyō table (special), `†` not in the table, "
          "`ᵇ` bound form (prefix/suffix, bare stem or rendaku form; ordered at a quarter "
          "of its score).\n")
    print("| kanji | asked on | asked kun | on'yomi | kun'yomi |")
    print("|---|---|---|---|---|")
    for ch in a.kanji:
        k = by_cp.get(ord(ch))
        if k is None:
            continue
        n = final[k.codepoint]
        sc = wf.score.get(k.codepoint, {})
        entry = joyo.get(ch)
        bound = bound_forms(k.kunyomi, k.kun_bound)
        print(f"| {ch} | {first(k.onyomi, n.onyomi)} | {first(k.kunyomi, n.kunyomi)} | "
              f"{fmt_order(n.onyomi, sc, set(), entry)} | "
              f"{fmt_order(n.kunyomi, sc, bound, entry)} |")

    if a.tsv:
        out = Path(a.tsv)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            f.write("kanji\tjlpt\tgrade\tkind\tkanjidic2_first\tcommon_first\tcommon_order\n")
            for k in raw:
                n = final[k.codepoint]
                for kind, old, new in (("on", k.onyomi, n.onyomi), ("kun", k.kunyomi, n.kunyomi)):
                    if old and old[0] != new[0]:
                        f.write(f"{chr(k.codepoint)}\t{new_jlpt.get(k.codepoint, 0)}\t{k.grade}\t"
                                f"{kind}\t{old[0]}\t{new[0]}\t{'、'.join(new)}\n")
        print(f"\n(wrote {out})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
