"""Example words for the learning mode's intro card (`build --example-words`).

Per card, one common word that uses the card's asked on'yomi (its first) and
one for its asked kun'yomi, each with its reading and a short gloss in the
deck's language; pack format v3 stores them (docs/pack-format.md,
"Example-words section"). How a word is picked was measured in
docs/example-words.md (scripts/example_words_report.py, which imports this
module):

1. **Candidates.** Every JMdict (kanji form, reading) pair, weighted for a
   learner as `--common-readings` weighs words (readings.weighted_pairs:
   wordfreq shared by Tatoeba and the JMdict priorities, times the JLPT list
   multiplier). A word uses a reading of a kanji when its JmdictFurigana part
   for that kanji matches it, by the attribution that scores the readings
   (readings.match_level: okurigana, sequential voicing, gemination).
2. **Worth showing** (usable): on a JLPT list, or with a JMdict priority tag
   on both its form and its reading; and not "usually written using kana
   alone" in its first sense.
3. **The pick**: a JLPT list word at or below the kanji's own level first
   (a kanji with no level takes any list word), then the highest weight. The
   project's Spanish table can mark a word `skip`, and the next one is taken
   (in every language: the words are the same in every deck).
4. **Glosses.** English: the first gloss of the word's first JMdict_e sense
   that is not a transliteration of its reading, if it has at most MAX_GLOSS
   characters. Spanish: only reviewed (`ok`) rows of the project's table
   `data/example_glosses_es.tsv`, never JMdict's own Spanish glosses (their
   licence: docs/example-words.md). Other languages, and a word with no
   gloss: an empty gloss (the device shows the word and its reading).

The Spanish table is filled by `tamagotcho-pack sync-glosses`: picked words it
lacks are appended as `draft` rows with a suggestion from FreeDict (the
JMdict_e gloss translated by the dictionary `--meaning-order` pins; wrong for
about one word in five, hence the review).
"""

from __future__ import annotations

import io
import re
import tarfile
import unicodedata
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from .meanings import TEI, norm_en
from .readings import (KanjiReadings, WordPair, _read_maybe_gz, is_kanji, match_level,
                       pair_weight, priority_rank, segments, to_hiragana)

MAX_GLOSS = 24          # a gloss longer than this (characters) is not shown (English)
KIND_ON, KIND_KUN = 0, 1
KIND_NAME = {KIND_ON: "on", KIND_KUN: "kun"}
XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"


# --- JMdict: what a pair is worth showing, and its glosses -------------------------

@dataclass
class Info:
    """One (kanji form, reading) pair of JMdict."""
    weight: float     # JMdict priority (readings.pair_weight)
    tagged: bool      # form and reading both carry a priority tag (news/ichi/spec/gai/nf)
    uk: bool          # the first sense is "usually written using kana alone" (為る, 沢山)
    eng: list[str] = field(default_factory=list)    # the first English sense's glosses
    other: list[str] = field(default_factory=list)  # every gloss in `other_lang`, in order
    pos: tuple[str, ...] = ()                       # the first sense's parts of speech


_ENTITY = re.compile(rb'<!ENTITY\s+([\w-]+)\s+"([^"]*)">')


def load_word_info(source: str | Path | bytes, other_lang: str | None = None
                   ) -> dict[tuple[str, str], Info]:
    """Every (kanji form, reading) pair of JMdict with its priority, its
    "usually kana" mark, its first English sense's glosses and its first
    sense's parts of speech.

    The builder reads `JMdict_e` (English only). `other_lang` ("spa") also
    collects that language's glosses, for scripts/example_words_report.py's
    measurement of the multilingual file; no pack uses them. Senses restricted
    (stagk/stagr) to other forms are skipped. A pair in several entries takes
    the entry with the highest JMdict priority, the first on a tie."""
    data = _read_maybe_gz(source)
    end = data.find(b"<JMdict>")
    names = {v.decode(): k.decode() for k, v in _ENTITY.findall(data[:end if end > 0 else 300_000])}
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
                    if other_lang:
                        g.other += [t for lang, t in glosses if lang == other_lang]
                old = out.get((keb, reb))
                if old is None or g.weight > old.weight:
                    out[(keb, reb)] = g
        el.clear()
    return out


# A loanword's source note inside a gloss: "neurona (eng: neuron)".
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


def english_gloss(info: Info | None, reading: str, max_len: int = MAX_GLOSS) -> str:
    """An English pack's gloss for a word: the shown gloss of its first
    JMdict_e sense when it is short, else "" (none)."""
    g = shown_gloss(info.eng, reading) if info else None
    return g if g and len(g) <= max_len else ""


# --- candidates and the pick ---------------------------------------------------------

@dataclass
class Word:
    text: str
    reading: str
    weight: float   # the learner-weighted corpus weight of `--common-readings`
    level: int      # JLPT list level of (text, reading), 5 = N5; 0 = on no list
    # Bytes of `reading` the kanji reads (start, length); (0, 0) when unknown.
    span: tuple[int, int] = (0, 0)


def reading_span(text: str, reading: str, segs: list, index: int) -> tuple[int, int]:
    """(byte offset, byte length) inside `reading` of furigana segment
    `segs[index]` (毎日 まいにち, segment 1 にち: (6, 6)); (0, 0) when the
    segments do not spell the reading or a value does not fit a byte."""
    by_start = {s[0]: (k, s) for k, s in enumerate(segs)}
    built: list[str] = []
    start = length = -1
    i = 0
    while i < len(text):
        if i in by_start:
            k, (a, b, rt) = by_start[i]
            if b < a:
                return 0, 0
            if k == index:
                start, length = len("".join(built)), len(rt)
            built.append(to_hiragana(rt))
            i = b + 1
        else:
            built.append(to_hiragana(text[i]))
            i += 1
    if start < 0 or "".join(built) != to_hiragana(reading):
        return 0, 0
    b0 = len(reading[:start].encode("utf-8"))
    bl = len(reading[start:start + length].encode("utf-8"))
    return (b0, bl) if 0 < bl and b0 + bl <= 255 else (0, 0)


def words_by_reading(pairs: list[WordPair], kanji: dict[int, KanjiReadings], furi,
                     vocab: dict[tuple[str, str], int]) -> dict[tuple[int, str], dict]:
    """(codepoint, KANJIDIC2 reading) -> {(text, reading): Word} for the words
    whose per-kanji furigana matches that reading, by the attribution of
    readings.score_readings (the best match level; ties go to every reading).
    `kanji` holds every reading of each kanji (an emptied kind's too)."""
    out: dict[tuple[int, str], dict] = {}
    for p in pairs:
        segs = segments(p.text, p.reading, furi)
        if not segs:
            continue
        for k, (start, end, rt) in enumerate(segs):
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
                    d[key] = Word(p.text, p.reading, p.weight, vocab.get(key, 0),
                                  reading_span(p.text, p.reading, segs, k))
    return out


def usable(w: Word, info: dict[tuple[str, str], Info]) -> bool:
    """A word worth showing: on a JLPT list, or common by JMdict's priority
    tags (not 天山 あめやま, archaic); and not one JMdict says is usually written
    in kana (沢山 たくさん), which a learner seldom sees with its kanji."""
    i = info.get((w.text, w.reading))
    return i is not None and (bool(w.level) or i.tagged) and not i.uk


def pick(cands: dict | None, level: int, info: dict | None = None,
         skip: frozenset | set = frozenset()) -> Word | None:
    """A JLPT list word at or below `level` (5 = N5) first, then the weight;
    with `info`, only the usable() words; never a (text, reading) in `skip`."""
    if not cands:
        return None
    pool = [w for w in cands.values()
            if (info is None or usable(w, info)) and (w.text, w.reading) not in skip]
    if not pool:
        return None
    return min(pool, key=lambda w: (not (w.level and w.level >= level), -w.weight,
                                    w.text, w.reading))


def is_own_form(w: Word, ch: str) -> bool:
    """The word is the kanji alone or with kana only (本, 食べる): it repeats
    the intro's kanji and reading rather than showing a compound."""
    return w.text[0] == ch and all(not is_kanji(c) for c in w.text[1:])


def asked_readings(card) -> list[tuple[int, str]]:
    """(kind, reading) for each kind the device asks a card: the first on'yomi
    and the first kun'yomi, as stored (a kind may be empty)."""
    return [(kind, rs[0]) for kind, rs in ((KIND_ON, card.onyomi), (KIND_KUN, card.kunyomi)) if rs]


def card_readings(card) -> KanjiReadings:
    """Every reading of a card's kanji, for the attribution: an emptied kind
    (`--common-readings`) still claims its words (Card.valid_*)."""
    return KanjiReadings(list(card.valid_onyomi or card.onyomi),
                         list(card.valid_kunyomi or card.kunyomi))


@dataclass
class Pick:
    kind: int
    asked: str        # the card's reading the word shows (KANJIDIC2 notation)
    word: Word | None


def word_candidates(cards: list, pairs: list[WordPair], furi,
                    vocab: dict[tuple[str, str], int]) -> dict[tuple[int, str], dict]:
    """words_by_reading for the cards' kanji (every reading of each)."""
    return words_by_reading(pairs, {c.codepoint: card_readings(c) for c in cards}, furi, vocab)


def pick_words(cards: list, cands: dict[tuple[int, str], dict], info: dict[tuple[str, str], Info],
               skip: frozenset | set = frozenset()) -> list[list[Pick]]:
    """Each card's picks from its candidates (word_candidates), one per asked
    kind, in card order (`word` None when no usable word uses the reading). A
    card's level is its JLPT level (no level: any list word)."""
    out = []
    for c in cards:
        level = c.jlpt or 1
        out.append([Pick(kind, r, pick(cands.get((c.codepoint, r)), level, info, skip))
                    for kind, r in asked_readings(c)])
    return out


@dataclass
class ExampleWord:
    """One word as a v3 pack stores it (docs/pack-format.md)."""
    kind: int                      # KIND_ON / KIND_KUN
    text: str                      # 毎日
    reading: str                   # まいにち
    gloss: str = ""                # in the pack's language; "" = none
    span: tuple[int, int] = (0, 0)  # reading_start, reading_len (bytes); (0, 0) = unknown


def example_words(picks: list[list[Pick]], info: dict[tuple[str, str], Info], lang: str,
                  table: dict | None = None) -> list[list[ExampleWord]]:
    """The packed words of each card, with the deck language's glosses:
    English from JMdict_e (english_gloss), Spanish from the `ok` rows of the
    table (load_gloss_table), none otherwise."""
    out = []
    for ps in picks:
        row = []
        for p in ps:
            w = p.word
            if w is None:
                continue
            if lang == "en":
                gloss = english_gloss(info.get((w.text, w.reading)), w.reading)
            elif lang == "es" and table:
                t = table.get((w.text, w.reading))
                gloss = t.es if t is not None and t.status == "ok" else ""
            else:
                gloss = ""
            row.append(ExampleWord(p.kind, w.text, w.reading, gloss, w.span))
        out.append(row)
    return out


# --- the Spanish gloss table ------------------------------------------------------------

TABLE_COLUMNS = ("level", "kanji", "word", "reading", "en", "es", "status", "note")
STATUSES = ("draft", "ok", "skip")


@dataclass(frozen=True)
class GlossRow:
    level: str
    kanji: str
    word: str
    reading: str
    en: str
    es: str
    status: str
    note: str
    line: int = 0

    def key(self) -> tuple[str, str]:
        return self.word, self.reading


def load_gloss_table(path: str | Path) -> dict[tuple[str, str], GlossRow]:
    """The project's Spanish gloss table: `#` comment lines, a header row
    naming the columns (TABLE_COLUMNS, in any order), then one row per (word,
    reading). An absent file is an empty table. Raises ValueError on a
    malformed row, an unknown status, an `ok` row without a gloss or a
    (word, reading) listed twice."""
    p = Path(path)
    if not p.exists():
        return {}
    out: dict[tuple[str, str], GlossRow] = {}
    cols: dict[str, int] | None = None
    for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cells = line.split("\t")
        if cols is None:
            names = [c.strip() for c in cells]
            missing = [c for c in TABLE_COLUMNS if c not in names]
            if missing:
                raise ValueError(f"{p.name} line {n}: header lacks {', '.join(missing)}")
            cols = {c: names.index(c) for c in TABLE_COLUMNS}
            continue
        if len(cells) < len(cols) - 1 or len(cells) > max(cols.values()) + 1:
            raise ValueError(f"{p.name} line {n}: {len(cells)} columns, expected "
                             f"{max(cols.values()) + 1}")
        v = {c: (cells[i].strip() if i < len(cells) else "") for c, i in cols.items()}
        if not v["word"] or not v["reading"]:
            raise ValueError(f"{p.name} line {n}: empty word or reading")
        if v["status"] not in STATUSES:
            raise ValueError(f"{p.name} line {n}: status {v['status']!r}, expected one of "
                             + ", ".join(STATUSES))
        if v["status"] == "ok" and not v["es"]:
            raise ValueError(f"{p.name} line {n}: an ok row needs a gloss (es)")
        row = GlossRow(**v, line=n)
        if row.key() in out:
            raise ValueError(f"{p.name} line {n}: {row.word} {row.reading} is already on line "
                             f"{out[row.key()].line}")
        out[row.key()] = row
    return out


def skip_words(table: dict[tuple[str, str], GlossRow]) -> frozenset[tuple[str, str]]:
    """The (word, reading) pairs the table marks `skip`: never an example."""
    return frozenset(k for k, r in table.items() if r.status == "skip")


TABLE_HEADER = """\
# Spanish glosses for the example words on the new-kanji intro card
# (docs/learning-mode.md, decision 9; docs/example-words.md).
# Authored by the Tamagotcho project; distributed with the packs under CC BY-SA
# 4.0 (docs/data-sources.md). Derived from JMdict_e's English glosses (JMdict,
# EDRDG, CC BY-SA 4.0) and, where a draft kept its suggestion, from FreeDict's
# eng-spa dictionary (WikDict, from Wiktionary via DBnary, CC BY-SA 3.0). No
# text comes from JMdict's own Spanish glosses.
#
# Columns (tab-separated), one row per (word, reading): level, kanji, word,
# reading, en, es, status (draft, ok or skip), note. Only ok rows reach a pack.
"""


def format_row(row: GlossRow) -> str:
    cells = [getattr(row, c) for c in TABLE_COLUMNS]
    for c in cells:
        if "\t" in c or "\n" in c:
            raise ValueError(f"a table cell cannot hold a tab or a newline: {c!r}")
    return "\t".join(cells)


def append_rows(path: str | Path, rows: list[GlossRow]) -> None:
    """Appends rows at the end of the table, leaving every existing byte as it
    is; creates the file with a header first when it does not exist."""
    p = Path(path)
    if not rows:
        return
    if not p.exists():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(TABLE_HEADER + "\t".join(TABLE_COLUMNS) + "\n", encoding="utf-8")
    text = p.read_bytes()
    with p.open("a", encoding="utf-8", newline="\n") as f:
        if text and not text.endswith(b"\n"):
            f.write("\n")
        for r in rows:
            f.write(format_row(r) + "\n")


# --- FreeDict: the draft suggestion for a Spanish gloss ("C") --------------------------

@dataclass
class FdEntry:
    pos: str
    senses: list[list[str]]   # Spanish quotes per sense, in order


def load_freedict_ordered(source: str | Path) -> dict[str, list[FdEntry]]:
    """FreeDict eng-spa as headword -> entries (one per part of speech), each
    with its senses' Spanish quotes in the dictionary's order (meanings.load_freedict
    keeps normalised sets, for matching)."""
    out: dict[str, list[FdEntry]] = defaultdict(list)
    with tarfile.open(source, "r:xz") as tar:
        member = next(m for m in tar.getmembers() if m.name.endswith(".tei") and m.isfile())
        f = tar.extractfile(member)
        for _, el in ET.iterparse(f, events=("end",)):
            if el.tag != TEI + "entry":
                continue
            orth = el.findtext(f"{TEI}form/{TEI}orth")
            pos = el.findtext(f"{TEI}gramGrp/{TEI}pos") or ""
            senses = []
            for s in el.findall(f"{TEI}sense"):
                qs = [q.text.strip() for c in s.findall(f"{TEI}cit") if c.get("type") == "trans"
                      for q in c.findall(f"{TEI}quote") if q.text and q.text.strip()]
                if qs:
                    senses.append(qs)
            if orth and senses:
                out[orth.strip()].append(FdEntry(pos, senses))
            el.clear()
    return out


def fd_pos(jm_pos: tuple[str, ...]) -> str:
    """FreeDict's part of speech for a JMdict one (v5k -> v, adj-i -> adj)."""
    for p in jm_pos:
        if p.startswith("v"):
            return "v"
        if p.startswith("adj"):
            return "adj"
        if p.startswith("adv"):
            return "adv"
        if p == "num":
            return "numeral"
        if p.startswith("n") or p == "pn":
            return "n"
    return ""


def en_keys(gloss: str) -> list[str]:
    """Headwords to try for an English gloss: as written, without a
    parenthesis or a leading "to"/"to be"/article, lowercased."""
    g = re.sub(r"\s*\([^)]*\)", "", gloss).strip()
    out = [g, g.lower(), norm_en(gloss)]
    m = re.match(r"^(?:to be|to|a|an|the) (.+)$", g, re.IGNORECASE)
    if m:
        out.append(m.group(1))
    return [k for k in dict.fromkeys(out) if k]


def translate(glosses: list[str], jm_pos: tuple[str, ...], fd: dict[str, list[FdEntry]]
              ) -> tuple[str, str] | None:
    """(Spanish, the English gloss used): the first English gloss FreeDict
    has, its entry with the word's part of speech (else its first entry), the
    first translation of that entry's first sense."""
    want = fd_pos(jm_pos)
    for g in glosses:
        for k in en_keys(g):
            entries = fd.get(k)
            if not entries:
                continue
            e = next((x for x in entries if x.pos == want), entries[0])
            return e.senses[0][0], g
    return None
