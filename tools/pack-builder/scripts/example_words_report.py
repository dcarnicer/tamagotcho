"""Measurement for decision 9 of docs/learning-mode.md: example words on the
new-kanji intro card, and whether JMdict has Spanish glosses for them
(docs/example-words.md). Nothing here changes a pack.

For every kanji of the published decks (JLPT N5..N1 and the Jōyō deck) it
takes the readings the device asks (the first on'yomi and the first kun'yomi
after `--common-readings`; a kind may be empty) and picks, per asked reading,
one example word: among the JMdict words whose per-kanji reading
(JmdictFurigana) is that reading, a JLPT list word at or below the kanji's
level first, then the highest weight `--common-readings` gives a word
(tamagotcho_pack.readings: wordfreq frequency shared by Tatoeba and the JMdict
priorities, times the JLPT list multiplier). It then looks each word up in the
multilingual JMdict (`JMdict.gz`, every language, not `JMdict_e`) and prints,
as Markdown:

- per deck: the asked kinds, the words found, and how many have a Spanish and
  an English gloss, and a short one: the first gloss that is not a
  transliteration of the reading, at most --max-gloss characters;
- a random spot check of N5-N3 words with their Spanish glosses;
- a size estimate per deck for words with glosses (b) and words with readings
  only (c): strings, a per-card index and the 18 px set-2 glyphs of the words'
  kanji, against the published packs (--packs: a directory with the decks-v2
  `.tgc` files, e.g. from `gh release download decks-v2`).

The multilingual JMdict is read here only to measure: the EDRDG licence (CC BY-SA
4.0) covers JMdict's Japanese and English parts, and says the translations in
other languages "are covered by separate copyright held by the compilers of
that material" (docs/example-words.md, "Licence"). Keep the --tsv output, which
holds those glosses, out of the repository.

Usage (after `tamagotcho-pack fetch --readings`; --fetch first downloads
JMdict.gz, every language, 22 MB, into cache/readings/):

    uv run python scripts/example_words_report.py [--fetch] [--packs DIR] [--tsv out/example-words.tsv]
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import random
import re
import statistics
import sys
import unicodedata
import urllib.request
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from tamagotcho_pack import paths
from tamagotcho_pack.glyphs import SET_TEXT, FontRenderer
from tamagotcho_pack.joyo_table import load_pdf
from tamagotcho_pack.kanjidic import JOYO_GRADES, load_new_jlpt, parse_kanjidic
from tamagotcho_pack.pack import GLYPH_SIZE, READING_SEP, read_pack
from tamagotcho_pack.readings import (LEVEL_MULT, KanjiReadings, WordPair, compute_scores,
                                      corpus_weights, is_kanji, load_jlpt_vocab,
                                      load_tatoeba_counts, load_wordfreq, match_level, pair_weight,
                                      parse_furigana, parse_jmdict, priority_rank, reorder_raw,
                                      segments, to_hiragana)
from tamagotcho_pack.rle import best_glyph

# The multilingual JMdict (EDRDG): the same entries as JMdict_e, plus glosses
# in other languages as extra senses (only the Japanese and English parts are
# under the EDRDG's CC BY-SA 4.0). Rebuilt daily, like JMdict_e, so it has no
# versioned URL; the report prints the file's date and SHA256.
JMDICT_FULL_URL = "https://www.edrdg.org/pub/Nihongo/JMdict.gz"
JMDICT_FULL = paths.READINGS_DIR / "JMdict.gz"

DECKS = ("n5", "n4", "n3", "n2", "n1", "joyo")
DECK_TITLE = {"n5": "N5", "n4": "N4", "n3": "N3", "n2": "N2", "n1": "N1", "joyo": "Jōyō"}
XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"
# Per card in a words section: a (word, reading[, gloss]) string offset, u32
# each, per asked kind; plus a header extension (docs/learning-mode.md §3.11).
INDEX_PER_SLOT = 4
HEADER_EXT = 16


# --- fetch ------------------------------------------------------------------------

def fetch_jmdict(force: bool = False) -> None:
    """Download the multilingual JMdict into the cache, the way fetch.py does."""
    if JMDICT_FULL.exists() and not force:
        print(f"have  {JMDICT_FULL}", file=sys.stderr)
        return
    print(f"get   {JMDICT_FULL_URL}", file=sys.stderr)
    req = urllib.request.Request(JMDICT_FULL_URL, headers={"User-Agent": "tamagotcho-pack"})
    with urllib.request.urlopen(req, timeout=300) as r:
        data = r.read()
    tmp = JMDICT_FULL.with_suffix(".gz.part")
    tmp.write_bytes(data)
    tmp.replace(JMDICT_FULL)
    print(f"  -> {JMDICT_FULL} ({len(data):,} bytes)", file=sys.stderr)


def jmdict_created(data: bytes) -> str:
    m = re.search(rb"<!-- JMdict created: ([0-9-]+) -->", data[:400_000])
    return m.group(1).decode() if m else "?"


# --- glosses ----------------------------------------------------------------------

@dataclass
class Info:
    """One (kanji form, reading) pair of the multilingual JMdict."""
    weight: float     # JMdict priority (readings.pair_weight)
    tagged: bool      # form and reading both carry a priority tag (news/ichi/spec/gai/nf)
    uk: bool          # the first sense is "usually written using kana alone" (為る, 沢山)
    eng: list[str] = field(default_factory=list)   # the first English sense's glosses
    spa: list[str] = field(default_factory=list)   # every Spanish gloss, in order
    pos: tuple[str, ...] = ()                      # the first sense's parts of speech (n, v5k...)


_ENTITY = re.compile(rb'<!ENTITY\s+([\w-]+)\s+"([^"]*)">')


def load_jmdict_info(data: bytes) -> dict[tuple[str, str], Info]:
    """Every (kanji form, reading) pair of the multilingual JMdict with its
    priority, its "usually kana" mark and its English and Spanish glosses.

    Spanish glosses sit in senses of their own (no part of speech, no link to
    an English sense). Senses restricted (stagk/stagr) to other forms are
    skipped. A pair in several entries takes the entry with the highest JMdict
    priority, the first on a tie."""
    end = data.find(b"<JMdict>")
    names = {v.decode(): k.decode() for k, v in _ENTITY.findall(data[:end])}
    out: dict[tuple[str, str], Info] = {}
    for _, el in ET.iterparse(io.BytesIO(data), events=("end",)):
        if el.tag != "entry":
            continue
        kebs = {k.findtext("keb"): [p.text for p in k.findall("ke_pri")] for k in el.findall("k_ele")}
        senses = [({x.text for x in s.findall("stagk")}, {x.text for x in s.findall("stagr")},
                   {names.get(m.text or "", m.text) for m in s.findall("misc")},
                   [(g.get(XML_LANG, "eng"), g.text or "") for g in s.findall("gloss")])
                  for s in el.findall("sense")]
        first = el.find("sense")
        pos = tuple(names.get(p.text or "", p.text) for p in first.findall("pos")) \
            if first is not None else ()
        for r in el.findall("r_ele"):
            reb = r.findtext("reb")
            restr = {x.text for x in r.findall("re_restr")}
            re_pri = [p.text for p in r.findall("re_pri")]
            for keb, ke_pri in kebs.items():
                if restr and keb not in restr:
                    continue
                g = Info(pair_weight(ke_pri, re_pri),
                         priority_rank(ke_pri) is not None and priority_rank(re_pri) is not None,
                         bool(senses) and "uk" in senses[0][2], pos=pos)
                for stagk, stagr, _, glosses in senses:
                    if (stagk and keb not in stagk) or (stagr and reb not in stagr):
                        continue
                    eng = [t for lang, t in glosses if lang == "eng"]
                    if eng and not g.eng:
                        g.eng = eng
                    g.spa += [t for lang, t in glosses if lang == "spa"]
                old = out.get((keb, reb))
                if old is None or g.weight > old.weight:
                    out[(keb, reb)] = g
        el.clear()
    return out


# A loanword's source note inside a Spanish gloss: "neurona (eng: neuron)".
_LANG_NOTE = re.compile(r"\s*\((?:eng|fre|fr|ger|ita|lat|wasei|por|dut|lit|[a-z]{2,5}):[^)]*\)")


def clean(g: str) -> str:
    return " ".join(_LANG_NOTE.sub("", g).split())


# Hepburn romanisation, enough to recognise a transliterated gloss ("sake").
_ROMAJI = dict(zip(
    "あいうえおかきくけこがぎぐげごさしすせそざじずぜぞたちつてとだぢづでどなにぬねの"
    "はひふへほばびぶべぼぱぴぷぺぽまみむめもやゆよらりるれろわゐゑをんゔぁぃぅぇぉゎ",
    "a i u e o ka ki ku ke ko ga gi gu ge go sa shi su se so za ji zu ze zo ta chi tsu te to "
    "da ji zu de do na ni nu ne no ha hi fu he ho ba bi bu be bo pa pi pu pe po ma mi mu me mo "
    "ya yu yo ra ri ru re ro wa i e o n vu a i u e o wa".split()))
_SMALL_Y = {"ゃ": "ya", "ゅ": "yu", "ょ": "yo"}


def romaji(reading: str) -> str:
    out: list[str] = []
    double = False
    for ch in to_hiragana(reading):
        if ch == "っ":
            double = True
            continue
        if ch in _SMALL_Y and out:
            # きゃ kya, しゃ sha, ちゃ cha, じゃ ja (っちゃ tcha)
            prev = out.pop()
            palatal = prev.endswith(("shi", "chi", "ji"))
            syl = prev[:-1] + (_SMALL_Y[ch][1:] if palatal else _SMALL_Y[ch])
        elif ch == "ー" and out:
            syl = out[-1][-1]
        else:
            syl = _ROMAJI.get(ch, "")
        if double and syl:
            syl = ("t" if syl.startswith("ch") else syl[0]) + syl
        double = False
        out.append(syl)
    return "".join(out)


def romaji_variants(reading: str) -> set[str]:
    """The romanisations a transliterated gloss may use: Hepburn with long
    vowels written out (toukyou) or not (tokyo), and m before b/m/p."""
    h = romaji(reading)
    short = h
    for a, b in (("ou", "o"), ("oo", "o"), ("uu", "u"), ("ii", "i"), ("aa", "a"), ("ee", "e")):
        short = short.replace(a, b)
    out = {h, short}
    out |= {re.sub(r"n(?=[bmp])", "m", v) for v in out}
    return {v for v in out if v}


def letters(s: str) -> str:
    s = unicodedata.normalize("NFKD", s.lower())
    return "".join(c for c in s if "a" <= c <= "z")


def is_transliteration(gloss: str, reading: str) -> bool:
    """A gloss that is just the word in romaji ("sake", "kimono"), or that leads
    with it in quotes ('"oden": un tipo de cocido'). A capitalised one is a
    proper name ("Chiba (city, prefecture)"), for which the romaji is the
    translation, so it does not count."""
    variants = romaji_variants(reading)
    bare = re.sub(r"\([^)]*\)", "", gloss).strip()
    if letters(bare) in variants:
        return not bare[:1].isupper()
    m = re.match(r'\s*["“]([^"”]+)["”]', gloss)
    return bool(m and letters(m.group(1)) in variants)


def shown_gloss(glosses: list[str], reading: str) -> str | None:
    """The gloss an intro would show: the first that is not a transliteration."""
    for g in glosses:
        c = clean(g)
        if c and not is_transliteration(c, reading):
            return c
    return None


# --- words ------------------------------------------------------------------------

@dataclass
class Word:
    text: str
    reading: str
    weight: float   # the learner-weighted corpus weight of `--common-readings`
    level: int      # JLPT list level of (text, reading), 5 = N5; 0 = on no list


def learner_pairs(vocab: dict[tuple[str, str], int]) -> list[WordPair]:
    """JMdict pairs weighted as readings.compute_scores weighs them."""
    pairs = list(parse_jmdict(paths.JMDICT_GZ))
    pairs = corpus_weights(pairs, load_wordfreq(paths.WORDFREQ_JA),
                           load_tatoeba_counts(paths.TATOEBA_JPN))
    return [WordPair(p.text, p.reading,
                     p.weight * LEVEL_MULT.get(vocab.get((p.text, p.reading), 0), 1.0), p.pos)
            for p in pairs]


def words_by_reading(pairs: list[WordPair], kanji: dict[int, KanjiReadings], furi,
                     vocab: dict[tuple[str, str], int]) -> dict[tuple[int, str], dict]:
    """(codepoint, KANJIDIC2 reading) -> {(text, reading): Word} for the words
    whose per-kanji furigana matches that reading, by the attribution of
    readings.score_readings (the best match level; ties go to every reading)."""
    out: dict[tuple[int, str], dict] = {}
    for p in pairs:
        segs = segments(p.text, p.reading, furi)
        if not segs:
            continue
        for start, end, rt in segs:
            if start != end or start >= len(p.text):
                continue
            ch = p.text[start]
            if ch == "々" and start > 0:
                ch = p.text[start - 1]
            if not is_kanji(ch):
                continue
            kr = kanji.get(ord(ch))
            if kr is None:
                continue
            rt_h, following = to_hiragana(rt), p.text[end + 1:]
            best, hits = None, []
            for r in kr.onyomi + kr.kunyomi:
                lv = match_level(rt_h, following, r)
                if lv is None:
                    continue
                if best is None or lv < best:
                    best, hits = lv, [r]
                elif lv == best and r not in hits:
                    hits.append(r)
            for r in hits:
                d = out.setdefault((ord(ch), r), {})
                key = (p.text, p.reading)
                if key not in d or d[key].weight < p.weight:
                    d[key] = Word(p.text, p.reading, p.weight, vocab.get(key, 0))
    return out


def usable(w: Word, info: dict[tuple[str, str], Info]) -> bool:
    """A word worth showing: on a JLPT list, or common by JMdict's priority
    tags (not 天山 あめやま, archaic); and not one JMdict says is usually written
    in kana (沢山 たくさん), which a learner seldom sees with its kanji."""
    i = info.get((w.text, w.reading))
    return i is not None and (bool(w.level) or i.tagged) and not i.uk


def pick(cands: dict | None, level: int, info: dict | None = None) -> Word | None:
    """A JLPT list word at or below `level` (5 = N5) first, then the weight;
    with `info`, only the usable() words."""
    if not cands:
        return None
    pool = [w for w in cands.values() if info is None or usable(w, info)]
    if not pool:
        return None
    return min(pool, key=lambda w: (not (w.level and w.level >= level), -w.weight,
                                    w.text, w.reading))


def is_own_form(w: Word, ch: str) -> bool:
    """The word is the kanji alone or with kana only (本, 食べる): it repeats
    the intro's kanji and reading rather than showing a compound."""
    return w.text[0] == ch and all(not is_kanji(c) for c in w.text[1:])


# --- report -----------------------------------------------------------------------

@dataclass
class Row:
    ch: str
    kind: str
    asked: str
    word: Word
    gl: Info | None

    def es(self) -> str | None:
        return shown_gloss(self.gl.spa, self.word.reading) if self.gl else None

    def en(self) -> str | None:
        return shown_gloss(self.gl.eng, self.word.reading) if self.gl else None


def pct(n: int, d: int) -> str:
    return f"{n / d * 100:.0f} %" if d else "–"


@dataclass
class Picks:
    """The picked words of every deck (compute_picks), for this report and
    scripts/example_words_alternatives.py."""
    decks: dict[str, list[int]]                       # deck -> card codepoints
    packs: dict[tuple[str, str], Path]                # (deck, lang) -> published pack
    mismatch: list[str]                               # asked readings that differ from a pack
    new_jlpt: dict[int, int]
    picks: dict[int, list[tuple[str, str, Word | None]]]  # cp -> (kind, asked, word) per asked kind
    unfiltered: dict[int, int]                        # asked kinds with some word before usable()
    rows: dict[int, list[Row]]                        # cp -> the picked words
    all_cps: list[int]
    full: bytes                                       # JMdict.gz as read
    xml: bytes                                        # ... decompressed
    info: dict[tuple[str, str], Info]


def compute_picks(packs_dir: str | Path | None = None) -> Picks:
    """Every deck's asked readings and their picked words (see the docstring)."""
    # The asked readings, exactly as `build --common-readings` orders them.
    raw = parse_kanjidic(paths.KANJIDIC_GZ)
    new_jlpt = load_new_jlpt(paths.KANJI_JSON)
    vocab = load_jlpt_vocab(paths.JLPT_VOCAB)
    scores = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA,
                            paths.TATOEBA_JPN, vocab)
    ordered, _ = reorder_raw(raw, scores, load_pdf(paths.JOYO_PDF), drop=True)
    asked = {k.codepoint: (k.onyomi[:1], k.kunyomi[:1]) for k in ordered}

    decks: dict[str, list[int]] = {}
    for n in (5, 4, 3, 2, 1):
        decks[f"n{n}"] = [k.codepoint for k in raw if new_jlpt.get(k.codepoint) == n]
    decks["joyo"] = [k.codepoint for k in raw if k.grade in JOYO_GRADES]
    packs: dict[tuple[str, str], Path] = {}
    if packs_dir:
        for d in DECKS:
            for lang in ("es", "en"):
                f = Path(packs_dir) / f"{d}-{lang}.tgc"
                if f.exists():
                    packs[(d, lang)] = f
    # The packs' own card lists win (a card the font lacks is dropped), and
    # their first readings check the asked ones above.
    mismatch = []
    for (d, lang), f in packs.items():
        if lang != "es":
            continue
        p = read_pack(f.read_bytes())
        decks[d] = [c.codepoint for c in p.cards]
        for c in p.cards:
            on, kun = (s.split(READING_SEP)[:1] if s else [] for s in (c.onyomi, c.kunyomi))
            if (on, kun) != asked[c.codepoint]:
                mismatch.append(f"{d} {chr(c.codepoint)}: pack {on}/{kun}, here "
                                f"{asked[c.codepoint]}")

    # Candidate words and the pick per asked reading.
    full = JMDICT_FULL.read_bytes()
    xml = gzip.decompress(full)
    info = load_jmdict_info(xml)
    kanji = {k.codepoint: KanjiReadings(k.onyomi, k.kunyomi) for k in raw}
    furi = parse_furigana(paths.JMDICT_FURIGANA)
    cands = words_by_reading(learner_pairs(vocab), kanji, furi, vocab)
    all_cps = sorted({cp for cps in decks.values() for cp in cps})
    picks: dict[int, list[tuple[str, str, Word | None]]] = {}
    unfiltered: dict[int, int] = {}
    for cp in all_cps:
        level = new_jlpt.get(cp) or 1   # no JLPT level (Jōyō only): any list word
        on, kun = asked[cp]
        kinds = [(kind, r[0]) for kind, r in (("on", on), ("kun", kun)) if r]
        picks[cp] = [(kind, r, pick(cands.get((cp, r)), level, info)) for kind, r in kinds]
        unfiltered[cp] = sum(1 for _, r in kinds if cands.get((cp, r)))
    rows: dict[int, list[Row]] = {
        cp: [Row(chr(cp), kind, r, w, info.get((w.text, w.reading))) for kind, r, w in ps if w]
        for cp, ps in picks.items()}
    return Picks(decks, packs, mismatch, new_jlpt, picks, unfiltered, rows, all_cps, full, xml,
                 info)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true", help="download JMdict.gz (all languages) first")
    ap.add_argument("--force", action="store_true", help="with --fetch: download again")
    ap.add_argument("--packs", help="directory with the published decks (n5-es.tgc ... joyo-en.tgc)")
    ap.add_argument("--max-gloss", type=int, default=24,
                    help="longest gloss (characters) that counts as short (default 24)")
    ap.add_argument("--sample", type=int, default=30, help="spot-check size (N5-N3 words)")
    ap.add_argument("--seed", type=int, default=9, help="spot-check random seed")
    ap.add_argument("--tsv", help="write every picked word with its glosses to this file")
    a = ap.parse_args()

    if a.fetch:
        fetch_jmdict(a.force)
    need = [paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.JMDICT_GZ, paths.JMDICT_FURIGANA,
            paths.WORDFREQ_JA, paths.TATOEBA_JPN, paths.JOYO_PDF, JMDICT_FULL,
            *paths.JLPT_VOCAB.values()]
    for p in need:
        if not p.exists():
            hint = "--fetch" if p == JMDICT_FULL else "`tamagotcho-pack fetch --readings`"
            print(f"missing {p}: run {hint}", file=sys.stderr)
            return 2
    mx = a.max_gloss

    s = compute_picks(a.packs)
    decks, packs, mismatch, new_jlpt = s.decks, s.packs, s.mismatch, s.new_jlpt
    picks, unfiltered, rows, all_cps = s.picks, s.unfiltered, s.rows, s.all_cps
    full, xml = s.full, s.xml

    print("## Inputs\n")
    print(f"- JMdict (all languages): created {jmdict_created(xml)}, "
          f"{len(full):,} bytes, SHA256 `{hashlib.sha256(full).hexdigest()}`.")
    n_entries = xml.count(b"<entry>")
    n_spa = xml.count(b'<gloss xml:lang="spa">')
    spa_entries = sum(1 for e in xml.split(b"<entry>")[1:] if b'<gloss xml:lang="spa">' in e)
    print(f"- {n_entries:,} entries, {spa_entries:,} of them with a Spanish gloss "
          f"({spa_entries / n_entries * 100:.1f} %; {n_spa:,} Spanish glosses).")
    print(f"- JMdict_e (word choice, as `--common-readings`): "
          f"{jmdict_created(gzip.decompress(paths.JMDICT_GZ.read_bytes()))}.")
    if packs:
        print(f"- Asked readings checked against the published packs: {len(mismatch)} "
              "mismatches." + ("" if not mismatch else " " + "; ".join(mismatch[:20])))
    print()

    print(f"## Per deck (short gloss: the first that is not a transliteration, "
          f"≤ {mx} characters)\n")
    print("| deck | kanji | asked on | on word | asked kun | kun word | words | Spanish gloss "
          f"| Spanish ≤ {mx} | English gloss | English ≤ {mx} | kanji with a short Spanish "
          "gloss word |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    skipped = Counter()
    for d in DECKS:
        cps = decks[d]
        c = Counter()
        for cp in cps:
            ps = picks[cp]
            for kind, _, w in ps:
                c[f"asked {kind}"] += 1
                c[f"word {kind}"] += w is not None
            skipped[d] += unfiltered[cp] - sum(1 for *_, w in ps if w)
            rs = rows[cp]
            c["words"] += len(rs)
            c["es"] += sum(1 for r in rs if r.gl and r.gl.spa)
            c["es short"] += sum(1 for r in rs if (g := r.es()) and len(g) <= mx)
            c["en"] += sum(1 for r in rs if r.gl and r.gl.eng)
            c["en short"] += sum(1 for r in rs if (g := r.en()) and len(g) <= mx)
            c["kanji es"] += any((g := r.es()) and len(g) <= mx for r in rs)
        n = c["words"]
        print(f"| {DECK_TITLE[d]} | {len(cps):,} | {c['asked on']:,} | {c['word on']:,} | "
              f"{c['asked kun']:,} | {c['word kun']:,} | {n:,} | {c['es']:,} ({pct(c['es'], n)}) | "
              f"{c['es short']:,} ({pct(c['es short'], n)}) | {c['en']:,} ({pct(c['en'], n)}) | "
              f"{c['en short']:,} ({pct(c['en short'], n)}) | {c['kanji es']:,} "
              f"({pct(c['kanji es'], len(cps))}) |")
    print("\nAsked kinds that have JMdict words with the reading, but none on a JLPT list or "
          "with a priority tag, or only usually-kana ones (no word picked): "
          + ", ".join(f"{DECK_TITLE[d]} {skipped[d]}" for d in DECKS) + ".")

    # Breakdowns over the union (N5-N1 ∪ Jōyō).
    every = [r for cp in all_cps for r in rows[cp]]
    print("\n## Breakdowns (every picked word, N5-N1 ∪ Jōyō)\n")
    print(f"| words | count | Spanish gloss | Spanish ≤ {mx} | English ≤ {mx} |")
    print("|---|---:|---:|---:|---:|")
    groups = [
        ("on'yomi words", lambda r: r.kind == "on"),
        ("kun'yomi words", lambda r: r.kind == "kun"),
        ("on a JLPT list at or below the kanji's level",
         lambda r: r.word.level and r.word.level >= (new_jlpt.get(ord(r.ch)) or 1)),
        ("on no JLPT list (or a harder one)",
         lambda r: not (r.word.level and r.word.level >= (new_jlpt.get(ord(r.ch)) or 1))),
        ("the kanji's own form (本, 食べる)", lambda r: is_own_form(r.word, r.ch)),
        ("compounds", lambda r: not is_own_form(r.word, r.ch)),
    ] + [(f"kanji of JLPT N{n}", lambda r, n=n: new_jlpt.get(ord(r.ch)) == n) for n in
         (5, 4, 3, 2, 1)] + [("Jōyō kanji with no JLPT level", lambda r: not new_jlpt.get(ord(r.ch)))]
    for label, f in groups:
        g = [r for r in every if f(r)]
        es = sum(1 for r in g if r.gl and r.gl.spa)
        es_s = sum(1 for r in g if (x := r.es()) and len(x) <= mx)
        en_s = sum(1 for r in g if (x := r.en()) and len(x) <= mx)
        print(f"| {label} | {len(g):,} | {pct(es, len(g))} | {pct(es_s, len(g))} | "
              f"{pct(en_s, len(g))} |")

    with_es = [r for r in every if r.gl and r.gl.spa]
    firsts = [clean(r.gl.spa[0]) for r in with_es]
    translit = sum(1 for r in with_es if r.es() != clean(r.gl.spa[0]))
    no_shown = sum(1 for r in with_es if r.es() is None)
    longer = sum(1 for r in with_es if (x := r.es()) and len(x) > mx)
    print(f"\nOf the {len(with_es):,} words with a Spanish gloss: the first is a transliteration "
          f"for {translit} ({no_shown} have nothing else); the shown gloss is longer than {mx} "
          f"characters for {longer}. First Spanish gloss length: median "
          f"{statistics.median(len(x) for x in firsts):.0f}, 90th percentile "
          f"{statistics.quantiles([len(x) for x in firsts], n=10)[-1]:.0f} characters.")
    lens = [len(x) for r in every if (x := r.es())]
    for cut in (14, 20, 24, 32):
        print(f"- shown Spanish gloss ≤ {cut}: {sum(1 for x in lens if x <= cut):,} of "
              f"{len(every):,} words ({pct(sum(1 for x in lens if x <= cut), len(every))})")
    fallback = sum(1 for r in every if not ((x := r.es()) and len(x) <= mx)
                   and (y := r.en()) and len(y) <= mx)
    print(f"\nWords with no short Spanish gloss but a short English one: {fallback:,} of "
          f"{len(every):,}.")

    # Spot check.
    pool = sorted({(r.ch, r.kind, r.word.text, r.word.reading): r
                   for d in ("n5", "n4", "n3") for cp in decks[d] for r in rows[cp]
                   if r.gl and r.gl.spa}.items())
    sample = random.Random(a.seed).sample(pool, min(a.sample, len(pool)))
    print(f"\n## Spot check ({len(sample)} random N5-N3 words with a Spanish gloss, seed {a.seed})\n")
    print("| # | kanji | kind | word | reading | English (first sense) | Spanish (every gloss) "
          "| shown |")
    print("|---:|---|---|---|---|---|---|---|")
    for i, (_, r) in enumerate(sorted(sample, key=lambda x: x[0]), 1):
        shown = r.es()
        if shown and len(shown) > mx:
            shown += f" (over {mx})"
        print(f"| {i} | {r.ch} | {r.kind} {r.asked} | {r.word.text} | {r.word.reading} | "
              f"{'; '.join(r.gl.eng[:3])} | {'; '.join(clean(x) for x in r.gl.spa[:6])} | "
              f"{shown or '—'} |")

    # Size estimate.
    if packs:
        print("\n## Size estimate\n")
        font = FontRenderer(paths.DEFAULT_FONT)
        cost: dict[tuple[int, int], int] = {}

        def glyph_bytes(cp: int, px: int) -> int:
            if (cp, px) not in cost:
                g = font.text_glyph(cp, px, SET_TEXT)
                cost[(cp, px)] = GLYPH_SIZE + len(best_glyph(g.data, g.width, g.height)[1])
            return cost[(cp, px)]

        print("Per deck and language: the published pack, then what (c) words with readings "
              "and (b) words with readings and a short gloss in the deck's language would add "
              f"(strings, a {2 * INDEX_PER_SLOT} / {3 * INDEX_PER_SLOT} B index per asked kind, "
              f"a {HEADER_EXT} B header extension, and the set-2 glyphs the words need).\n")
        print("| pack | today | set-2 glyphs today | new set-2 kanji (deck / other) | glyphs "
              "| strings (c) | (c) total | strings (b) | (b) total |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for d in DECKS:
            for lang in ("es", "en"):
                f = packs.get((d, lang))
                if f is None:
                    continue
                raw_p = f.read_bytes()
                p = read_pack(raw_p)
                px = p.header["text_px"]
                set2 = {g.codepoint for g in p.glyphs if g.set == 2}
                deck_cps = {c.codepoint for c in p.cards}
                rs = [r for c in p.cards for r in rows.get(c.codepoint, [])]
                kinds = sum(len(picks.get(c.codepoint, [])) for c in p.cards)
                glosses = [x for r in rs if (x := (r.es() if lang == "es" else r.en()))
                           and len(x) <= mx]
                s_c = {r.word.text for r in rs} | {r.word.reading for r in rs}
                s_b = s_c | set(glosses)
                b_c = sum(len(s.encode()) + 1 for s in s_c)
                b_b = sum(len(s.encode()) + 1 for s in s_b)
                new_c = {ord(ch) for s in s_c for ch in s} - set2
                new_b = {ord(ch) for s in s_b for ch in s} - set2
                g_c = sum(glyph_bytes(cp, px) for cp in new_c)
                g_b = sum(glyph_bytes(cp, px) for cp in new_b)
                kj = {cp for cp in new_c if is_kanji(chr(cp))}
                tot_c = b_c + g_c + kinds * 2 * INDEX_PER_SLOT + HEADER_EXT
                tot_b = b_b + g_b + kinds * 3 * INDEX_PER_SLOT + HEADER_EXT
                print(f"| {d}-{lang} | {len(raw_p) / 1024:,.0f} KB | {len(set2)} | "
                      f"{len(kj & deck_cps)} / {len(kj - deck_cps)} | {g_c / 1024:,.1f} KB"
                      f"{'' if g_b == g_c else f' (b: {g_b / 1024:,.1f})'} | "
                      f"{b_c / 1024:,.1f} KB | **+{tot_c / 1024:,.0f} KB "
                      f"({tot_c / len(raw_p) * 100:.0f} %)** | {b_b / 1024:,.1f} KB | "
                      f"**+{tot_b / 1024:,.0f} KB ({tot_b / len(raw_p) * 100:.0f} %)** |")
        kj = [v for (cp, _), v in cost.items() if is_kanji(chr(cp))]
        if kj:
            print(f"\nAn 18 px kanji in set 2 costs {statistics.mean(kj):.0f} B on average "
                  f"({GLYPH_SIZE} B table entry + the bitmap; median {statistics.median(kj):.0f} B, "
                  f"{len(kj)} kanji rendered).")
        e2 = read_pack(packs[("n5", "es")].read_bytes()) if ("n5", "es") in packs else None
        if e2:
            adv = [g.advance for g in e2.glyphs if g.set == 2 and "a" <= chr(g.codepoint) <= "z"]
            print(f"A lowercase Latin letter in set 2 advances {statistics.mean(adv):.1f} px on "
                  "average (Noto Sans JP, 18 px).")

    if a.tsv:
        out = Path(a.tsv)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            f.write("kanji\tjlpt\tjoyo\tkind\tasked\tword\treading\tlist_level\tweight\t"
                    "spanish_shown\tenglish_shown\tspanish_all\tenglish_first_sense\n")
            joyo_set = set(decks["joyo"])
            for cp in all_cps:
                for r in rows[cp]:
                    f.write(f"{r.ch}\t{new_jlpt.get(cp, 0)}\t{int(cp in joyo_set)}\t{r.kind}\t"
                            f"{r.asked}\t{r.word.text}\t{r.word.reading}\t{r.word.level}\t"
                            f"{r.word.weight:.3g}\t{r.es() or ''}\t{r.en() or ''}\t"
                            f"{' | '.join(clean(x) for x in r.gl.spa) if r.gl else ''}\t"
                            f"{' | '.join(r.gl.eng) if r.gl else ''}\n")
        print(f"\n(wrote {out})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
