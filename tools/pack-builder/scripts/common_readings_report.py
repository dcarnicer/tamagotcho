"""Prototype report for `build --common-readings` (docs/common-readings.md).

For every kanji of JLPT N5-N1 and the Jōyō list, orders its KANJIDIC2 readings
the way `build --common-readings` does (tamagotcho_pack.readings) and prints, as
Markdown:

- how many kanji change the first on'yomi / kun'yomi (the reading the device
  asks) as each signal is added: JMdict priorities, wordfreq, Tatoeba, the
  Jōyō table's approved readings first, the JLPT vocabulary lists;
- the kinds `--common-readings` empties (nothing worth asking), per JLPT
  level, and the cards where both kinds would have gone;
- the class (approved / special / not in the table) of the asked reading of
  the Jōyō kanji, before and after;
- a sanity table for well-known kanji and the full list of N5 changes.

Usage (after `tamagotcho-pack fetch --readings`):

    uv run python scripts/common_readings_report.py [--kanji 九生日] [--tsv out/changes.tsv]
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from tamagotcho_pack import paths
from tamagotcho_pack.joyo_table import classify, load_pdf
from tamagotcho_pack.kanjidic import JOYO_GRADES, load_new_jlpt, parse_kanjidic
from tamagotcho_pack.readings import bound_forms, compute_scores, load_jlpt_vocab, reorder_raw, share

SANITY = "九生日下上行出明人大何今重楽方間食入四一"
LEVELS = (5, 4, 3, 2, 1, 0)


def fmt_order(readings, scores, levels, bound, entry):
    """`キュウ 86 · ク 14`: readings in order with their share (%) of the kind's
    score and, as `N5`, the easiest JLPT list level of a word using it; `*` =
    indented in the Jōyō table, `†` = not in it, `ᵇ` = bound form."""
    sh = share(readings, scores)
    out = []
    for r in readings:
        mark = {"joyo": "", "special": "*", "other": "†"}[classify(r, entry)] if entry else ""
        b = "ᵇ" if r in bound else ""
        lv = f" N{int(levels[r])}" if levels.get(r) else ""
        out.append(f"{r}{mark}{b} {sh[r] * 100:.0f}{lv}")
    return " · ".join(out) if out else "—"


def first(old, new):
    if not old:
        return "—"
    if not new:
        return f"~~{old[0]}~~ (dropped)"
    return old[0] if old[0] == new[0] else f"{old[0]} → **{new[0]}**"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kanji", default=SANITY, help="kanji for the sanity table")
    ap.add_argument("--tsv", help="write every kanji whose first reading changes to this file")
    a = ap.parse_args()

    need = [paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.JMDICT_GZ, paths.JMDICT_FURIGANA,
            paths.WORDFREQ_JA, paths.TATOEBA_JPN, paths.JOYO_PDF, *paths.JLPT_VOCAB.values()]
    for p in need:
        if not p.exists():
            print(f"missing {p}: run `tamagotcho-pack fetch --readings`", file=sys.stderr)
            return 2
    raw_all = parse_kanjidic(paths.KANJIDIC_GZ)
    new_jlpt = load_new_jlpt(paths.KANJI_JSON)
    raw = [k for k in raw_all if new_jlpt.get(k.codepoint) or k.grade in JOYO_GRADES]
    by_cp = {k.codepoint: k for k in raw}
    joyo = load_pdf(paths.JOYO_PDF)
    vocab = load_jlpt_vocab(paths.JLPT_VOCAB)
    level_of = {k.codepoint: new_jlpt.get(k.codepoint, 0) for k in raw}

    args = (raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA)
    jm = compute_scores(*args)
    wo = compute_scores(*args, paths.WORDFREQ_JA)
    wf = compute_scores(*args, paths.WORDFREQ_JA, paths.TATOEBA_JPN)
    fl = compute_scores(*args, paths.WORDFREQ_JA, paths.TATOEBA_JPN, vocab)
    variants = [
        ("JMdict priorities", reorder_raw(raw, jm)),
        ("+ wordfreq", reorder_raw(raw, wo)),
        ("+ Tatoeba", reorder_raw(raw, wf)),
        ("+ Jōyō approved first", reorder_raw(raw, wf, joyo)),
        ("+ JLPT lists", reorder_raw(raw, fl, joyo)),
        ("+ drop (`--common-readings`)", reorder_raw(raw, fl, joyo, drop=True)),
    ]
    freq_only = {k.codepoint: k for k in variants[2][1][0]}
    final, st = variants[-1][1]
    final = {k.codepoint: k for k in final}

    n_jlpt = sum(1 for k in raw if new_jlpt.get(k.codepoint))
    n_joyo = sum(1 for k in raw if k.grade in JOYO_GRADES)
    print(f"## Set\n\n{len(raw)} kanji: JLPT N5-N1 ({n_jlpt}) ∪ Jōyō ({n_joyo}). "
          f"Jōyō table: {len(joyo)} kanji, {sum(len(v) for v in joyo.values())} readings, "
          f"{sum(1 for v in joyo.values() for r in v if r.indented)} indented. JLPT "
          f"vocabulary: {len(vocab):,} (word, reading) pairs.\n")
    print(f"Per-kanji furigana parts matched to a KANJIDIC2 reading: {fl.matched:,}; "
          f"unmatched: {fl.unmatched:,} "
          f"({fl.unmatched / max(1, fl.matched + fl.unmatched) * 100:.1f} %).\n")

    print("## First reading changes (each column adds to the previous)\n")
    print("| | " + " | ".join(n for n, _ in variants) + " |")
    print("|---|" + "---:|" * len(variants))
    with_on = sum(1 for k in raw if k.onyomi)
    with_kun = sum(1 for k in raw if k.kunyomi)
    print(f"| first on'yomi changes (of {with_on}) | "
          + " | ".join(str(s.first_on_changed) for _, (_, s) in variants) + " |")
    print(f"| first kun'yomi changes (of {with_kun}) | "
          + " | ".join(str(s.first_kun_changed) for _, (_, s) in variants) + " |")
    print("\nThe last column counts only kinds that stay; dropped kinds are below.")
    print(f"\nReadings no word uses (in the kinds that stay): {st.unscored_on} of "
          f"{st.readings_on} on'yomi, {st.unscored_kun} of {st.readings_kun} kun'yomi. "
          "They stay in the card, last.\n")

    print("## Dropped kinds\n")
    drop_on, drop_kun = Counter(), Counter()
    for cp in st.dropped_on:
        drop_on[level_of[cp]] += 1
    for cp in st.dropped_kun:
        drop_kun[level_of[cp]] += 1
    chg_on, chg_kun = Counter(), Counter()
    for k in raw:
        n = final[k.codepoint]
        chg_on[level_of[k.codepoint]] += bool(n.onyomi) and n.onyomi[0] != k.onyomi[0]
        chg_kun[level_of[k.codepoint]] += bool(n.kunyomi) and n.kunyomi[0] != k.kunyomi[0]
    cards = Counter(level_of[k.codepoint] for k in raw)
    print("By JLPT level (no level: Jōyō kanji outside the JLPT lists):\n")
    print("| | " + " | ".join(f"N{n}" if n else "no level" for n in LEVELS) + " | all |")
    print("|---|" + "---:|" * (len(LEVELS) + 1))
    for name, c in (("kanji", cards), ("first on'yomi changed", chg_on),
                    ("first kun'yomi changed", chg_kun), ("on'yomi dropped", drop_on),
                    ("kun'yomi dropped", drop_kun)):
        print(f"| {name} | " + " | ".join(str(c[n]) for n in LEVELS) + f" | {sum(c.values())} |")
    print(f"\nBoth kinds had nothing worth asking for {len(st.both_failed)} kanji (the kind "
          "with the higher total score stays): "
          + "、".join(f"{chr(cp)} (keeps {kind}, N{level_of[cp]})" for cp, kind in st.both_failed)
          + ".")
    print(f"\nThe only kind of {len(st.only_kind_kept)} kanji had nothing worth asking and stays "
          "(a card keeps its readings): "
          + "、".join(f"{chr(cp)} ({kind}, N{level_of[cp]})" for cp, kind in st.only_kind_kept)
          + ".\n")

    print("## The asked reading against the Jōyō table (Jōyō kanji only)\n")
    jk = [k for k in raw if chr(k.codepoint) in joyo]
    print("| kind | order | approved | special (indented) | not in the table | dropped |")
    print("|---|---|---:|---:|---:|---:|")
    for kind in ("on", "kun"):
        attr = "onyomi" if kind == "on" else "kunyomi"
        for label, src in (("KANJIDIC2", by_cp), ("frequency only", freq_only),
                           ("`--common-readings`", final)):
            c = Counter()
            for k in jk:
                lst = getattr(src[k.codepoint], attr)
                if lst:
                    c[classify(lst[0], joyo[chr(k.codepoint)])] += 1
                elif getattr(k, attr):
                    c["dropped"] += 1
            print(f"| {kind} | {label} | {c['joyo']} | {c['special']} | {c['other']} | "
                  f"{c['dropped']} |")

    print("\n## Sanity table\n")
    print("Order of `--common-readings` with each reading's share (%) of its kind's score "
          "and the easiest JLPT list level of a word using it. `*` indented in the Jōyō "
          "table, `†` not in it, `ᵇ` bound form (no level, a quarter of its score).\n")
    print("| kanji | asked on | asked kun | on'yomi | kun'yomi |")
    print("|---|---|---|---|---|")
    for ch in a.kanji:
        k = by_cp.get(ord(ch))
        if k is None:
            continue
        n = final[k.codepoint]
        sc = fl.score.get(k.codepoint, {})
        lv = fl.level.get(k.codepoint, {})
        entry = joyo.get(ch)
        bound = bound_forms(k.kunyomi, k.kun_bound)
        print(f"| {ch} | {first(k.onyomi, n.onyomi)} | {first(k.kunyomi, n.kunyomi)} | "
              f"{fmt_order(n.onyomi, sc, lv, set(), entry)} | "
              f"{fmt_order(n.kunyomi or k.kunyomi, sc, lv, bound, entry)} |")

    print("\n## JLPT N5: every change\n")
    for k in raw:
        if level_of[k.codepoint] != 5:
            continue
        n = final[k.codepoint]
        for kind, old, new in (("on", k.onyomi, n.onyomi), ("kun", k.kunyomi, n.kunyomi)):
            if old and (not new or new[0] != old[0]):
                print(f"- {chr(k.codepoint)} {kind}: {first(old, new)}")

    if a.tsv:
        out = Path(a.tsv)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            f.write("kanji\tjlpt\tgrade\tkind\tkanjidic2_first\tcommon_first\tcommon_order\n")
            for k in raw:
                n = final[k.codepoint]
                for kind, old, new in (("on", k.onyomi, n.onyomi), ("kun", k.kunyomi, n.kunyomi)):
                    if old and (not new or old[0] != new[0]):
                        f.write(f"{chr(k.codepoint)}\t{level_of[k.codepoint]}\t{k.grade}\t"
                                f"{kind}\t{old[0]}\t{new[0] if new else '(dropped)'}\t"
                                f"{'、'.join(new)}\n")
        print(f"\n(wrote {out})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
