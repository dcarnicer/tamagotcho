"""How common each KANJIDIC2 reading of a kanji is, from word frequencies.

KANJIDIC2 lists a kanji's on'yomi and kun'yomi in no order of frequency (九:
kun ここの before ここの.つ), and the device asks the *first* reading of each
kind. This module scores every reading by how often the words that use it
occur, so the builder can put the common reading first (`build
--common-readings`, docs/common-readings.md):

1. **JMdict** (EDRDG, CC BY-SA 4.0) lists the words (kanji form + reading) and
   their priority tags: `nfXX` is the word's 500-word band in a newspaper
   frequency list, `news1/ichi1/spec1/gai1` mark common words, the `2`
   variants less common ones.
2. **wordfreq** (Robyn Speer, data CC BY-SA 4.0) gives how often each written
   form occurs in Japanese text (Wikipedia, subtitles, web, Twitter, Reddit),
   as MeCab/IPAdic tokens, so a verb is counted through its inflection stems
   (行か, 行き, 行く, 行け, 行こ, 行っ). Without wordfreq the JMdict priority
   alone is the weight, which undercounts verbs (行く has only `ichi1`).
3. **Tatoeba**'s Japanese sentence indices (CC BY 2.0 FR) name the reading of
   every homograph in ~150k sentences (上(うえ) vs 上(じょう)). A token claimed
   by several JMdict words or readings is shared by these counts, smoothed
   with the JMdict priorities.
4. **JmdictFurigana** (CC BY-SA, as JMdict) splits each word into per-kanji
   readings (学校 = 学:がっ + 校:こう).
5. Each per-kanji reading is matched to one of that kanji's KANJIDIC2 readings,
   allowing for okurigana (生き物 -> い.きる), sequential voicing (人々 ひとびと ->
   ひと) and gemination (学校 がっ -> ガク), and the word's weight is added to
   that reading's score.

The order then puts the readings the official Jōyō table approves first
(joyo_table.py: 弟 テイ before the indented ダイ, 兄弟's reading) and, within a
tier, the highest score first; bound forms (prefixes, suffixes, bare stems,
rendaku forms: 日 -び, 九 ここの, 殻 がら) count a quarter of their score.

A reading no word uses scores 0. Nothing is removed: rare readings stay in the
card (they are still valid answers, so they still keep distractors out), they
just stop being first.
"""

from __future__ import annotations

import gzip
import io
import math
import re
import struct
import xml.etree.ElementTree as ET
from collections import defaultdict
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field, replace
from pathlib import Path

# --- JMdict priority -------------------------------------------------------------

NF_BAND = 500           # words per nfXX band (JMdict DTD, ke_pri)
RANK_COMMON = 12_000    # news1/ichi1/spec1/gai1 without an nf band
RANK_UNCOMMON = 30_000  # news2/ichi2/spec2/gai2
RANK_NONE = 100_000     # in JMdict but in none of the frequency lists
# A word whose first sense is marked "usually written using kana alone" (為る,
# 有る) is seldom seen with its kanji, so it teaches its reading less.
UK_FACTOR = 0.1
# With wordfreq, a word's weight is its corpus frequency plus this much of its
# JMdict priority weight, so words wordfreq lacks (multi-token compounds) still
# count a little: an nf01 word adds ~4e-6 (a Zipf 3.6 word), an untagged one
# 1e-8 (wordfreq's floor).
JMDICT_BLEND = 1e-3

# Kanji/reading elements that are not how the word is normally written or read.
SKIP_KE_INF = frozenset({"rK", "sK", "oK", "iK"})
SKIP_RE_INF = frozenset({"rk", "sk", "ok", "ik"})


def priority_rank(tags: Iterable[str]) -> float | None:
    """Approximate frequency rank of a JMdict ke_pri/re_pri tag set (None: no tag)."""
    tags = list(tags)
    nf = [int(t[2:]) for t in tags if t.startswith("nf") and t[2:].isdigit()]
    if nf:
        return NF_BAND * min(nf) - NF_BAND / 2
    if any(t in ("news1", "ichi1", "spec1", "gai1") for t in tags):
        return RANK_COMMON
    if any(t in ("news2", "ichi2", "spec2", "gai2") for t in tags):
        return RANK_UNCOMMON
    return None


def pair_weight(ke_pri: Iterable[str], re_pri: Iterable[str], usually_kana: bool = False) -> float:
    """Weight of one written form + reading of a word: 1/rank, where the pair is
    only as common as the rarer of its two halves (a common word's rare reading,
    今日 こんち, or a rare spelling of a common word, gets RANK_NONE)."""
    rk = priority_rank(ke_pri) or RANK_NONE
    rr = priority_rank(re_pri) or RANK_NONE
    w = 1.0 / max(rk, rr)
    return w * UK_FACTOR if usually_kana else w


# --- JMdict ----------------------------------------------------------------------

@dataclass
class WordPair:
    text: str            # kanji form (keb)
    reading: str         # kana reading (reb)
    weight: float        # JMdict priority weight, then (with wordfreq) the corpus weight
    pos: tuple[str, ...] = ()  # part-of-speech entity names of the first sense (v5k, adj-i...)


_ENTITY = re.compile(rb'<!ENTITY\s+([\w-]+)\s+"([^"]*)">')


def _read_maybe_gz(source: str | Path | bytes) -> bytes:
    data = source if isinstance(source, bytes) else Path(source).read_bytes()
    return gzip.decompress(data) if data[:2] == b"\x1f\x8b" else data


def parse_jmdict(source: str | Path | bytes) -> Iterator[WordPair]:
    """Every usable (kanji form, reading) pair of JMdict with its priority weight.

    Entries with no kanji form, kanji forms marked rare/search-only/outdated/
    irregular, readings marked likewise, readings marked `re_nokanji` and
    readings restricted (`re_restr`) to other kanji forms are skipped."""
    data = _read_maybe_gz(source)
    # The DTD's entities (&uk; ...) expand to their descriptions; map them back.
    end = data.find(b"<JMdict>")
    names = {v.decode(): k.decode() for k, v in _ENTITY.findall(data[:end if end > 0 else 300_000])}

    def ent(e) -> str:
        return names.get(e.text or "", e.text or "")

    for _, el in ET.iterparse(io.BytesIO(data), events=("end",)):
        if el.tag != "entry":
            continue
        kanji = []
        for k in el.findall("k_ele"):
            if {ent(i) for i in k.findall("ke_inf")} & SKIP_KE_INF:
                continue
            kanji.append((k.findtext("keb") or "", [p.text or "" for p in k.findall("ke_pri")]))
        first = el.find("sense")
        if kanji and first is not None:
            uk = any(ent(m) == "uk" for m in first.findall("misc"))
            pos = tuple(ent(p) for p in first.findall("pos"))
            for r in el.findall("r_ele"):
                if r.find("re_nokanji") is not None:
                    continue
                if {ent(i) for i in r.findall("re_inf")} & SKIP_RE_INF:
                    continue
                restr = {x.text for x in r.findall("re_restr")}
                re_pri = [p.text or "" for p in r.findall("re_pri")]
                reb = r.findtext("reb") or ""
                for keb, ke_pri in kanji:
                    if restr and keb not in restr:
                        continue
                    yield WordPair(keb, reb, pair_weight(ke_pri, re_pri, uk), pos)
        el.clear()


# --- wordfreq ---------------------------------------------------------------------

def _unpack(buf: bytes, i: int = 0):
    """Decode one MessagePack value at buf[i:] -> (value, next index). Only the
    types wordfreq's lists use (arrays, maps, strings, ints, nil, bools)."""
    b = buf[i]
    i += 1
    if b <= 0x7F:
        return b, i
    if 0x90 <= b <= 0x9F or b in (0xDC, 0xDD):
        if b <= 0x9F:
            n = b & 0x0F
        elif b == 0xDC:
            n, = struct.unpack_from(">H", buf, i)
            i += 2
        else:
            n, = struct.unpack_from(">I", buf, i)
            i += 4
        out = []
        for _ in range(n):
            v, i = _unpack(buf, i)
            out.append(v)
        return out, i
    if 0x80 <= b <= 0x8F or b in (0xDE, 0xDF):
        if b <= 0x8F:
            n = b & 0x0F
        elif b == 0xDE:
            n, = struct.unpack_from(">H", buf, i)
            i += 2
        else:
            n, = struct.unpack_from(">I", buf, i)
            i += 4
        d = {}
        for _ in range(n):
            k, i = _unpack(buf, i)
            v, i = _unpack(buf, i)
            d[k] = v
        return d, i
    if 0xA0 <= b <= 0xBF or b in (0xD9, 0xDA, 0xDB):
        if b <= 0xBF:
            n = b & 0x1F
        else:
            size = {0xD9: 1, 0xDA: 2, 0xDB: 4}[b]
            n = int.from_bytes(buf[i:i + size], "big")
            i += size
        return buf[i:i + n].decode("utf-8"), i + n
    if b in (0xCC, 0xCD, 0xCE, 0xCF):
        size = {0xCC: 1, 0xCD: 2, 0xCE: 4, 0xCF: 8}[b]
        return int.from_bytes(buf[i:i + size], "big"), i + size
    if b == 0xC0:
        return None, i
    if b in (0xC2, 0xC3):
        return b == 0xC3, i
    raise ValueError(f"unsupported MessagePack byte 0x{b:02x} at {i - 1}")


def load_wordfreq(source: str | Path | bytes) -> dict[str, float]:
    """wordfreq's `cBpack` list (`large_ja.msgpack.gz`): a header, then one list
    of tokens per centibel bin, bin i holding the tokens of frequency 10^(-i/100)."""
    value, _ = _unpack(_read_maybe_gz(source))
    if not isinstance(value, list) or not value or not isinstance(value[0], dict) \
            or value[0].get("format") != "cB":
        raise ValueError("not a wordfreq cBpack list")
    freq: dict[str, float] = {}
    for i, tokens in enumerate(value[1:]):
        f = 10 ** (-i / 100)
        for t in tokens:
            freq.setdefault(t, f)
    return freq


# Godan endings: (dictionary-form kana, the a/i/e/o-row kana, the onbin stem kana).
_GODAN = {
    "く": ("かきけこ", "い"), "ぐ": ("がぎげご", "い"), "す": ("さしせそ", ""),
    "つ": ("たちてと", "っ"), "ぬ": ("なにねの", "ん"), "ぶ": ("ばびべぼ", "ん"),
    "む": ("まみめも", "ん"), "る": ("らりれろ", "っ"), "う": ("わいえお", "っ"),
}


def surface_forms(text: str, pos: Iterable[str]) -> set[str]:
    """The MeCab/IPAdic tokens a word's kanji form shows up as: the form itself,
    and for verbs and i-adjectives their inflection stems (生まれる: 生まれ;
    行く: 行か 行き 行け 行こ 行っ; 高い: 高く 高かっ 高けれ)."""
    out = {text}
    pos = set(pos)
    if len(text) < 2:
        return out
    base, last = text[:-1], text[-1]
    if pos & {"v1", "v1-s", "vk"} and last == "る":
        out |= {base, base + "れ", base + "ろ", base + "よ"}
    if any(p.startswith("v5") for p in pos) and last in _GODAN:
        rows, onbin = _GODAN[last]
        out |= {base + k for k in rows}
        if onbin:
            out.add(base + onbin)
        if "v5k-s" in pos:
            out.add(base + "っ")  # 行く: 行った
    if pos & {"adj-i", "adj-ix"} and last == "い":
        out |= {base + "く", base + "かっ", base + "けれ", base + "かろ"}
    return out


# --- Tatoeba ----------------------------------------------------------------------

# Pseudo-counts of the JMdict prior when Tatoeba counts split a form's readings.
TATOEBA_ALPHA = 2.0
# A one-character form takes a corpus share only with this much Tatoeba evidence.
TATOEBA_MIN_SINGLE = 5

_B_ITEM = re.compile(r"^([^(\[{~|]+)(?:\(([^)]*)\))?")


def load_tatoeba_counts(source: str | Path | bytes) -> dict[tuple[str, str | None], int]:
    """Count the JMdict headwords of Tatoeba's Japanese sentence indices
    (`jpn_indices.csv`, the Tanaka Corpus "B lines": `上(うえ)[01]{上に}`).

    The reading in parentheses is given when several JMdict entries share the
    headword (上: うえ / じょう / かみ), so these counts tell which reading a
    homograph has in real sentences; None counts an item without one.
    `(#1234)` entry numbers are not readings and count as None."""
    if isinstance(source, (str, Path)) and str(source).endswith((".tar.bz2", ".tbz2")):
        import tarfile

        with tarfile.open(source, "r:bz2") as tf:
            member = next(m for m in tf.getmembers() if m.name.endswith(".csv"))
            text = tf.extractfile(member).read().decode("utf-8")
    else:
        text = _read_maybe_gz(source).decode("utf-8")
    counts: dict[tuple[str, str | None], int] = defaultdict(int)
    for line in text.splitlines():
        cols = line.split("\t")
        if len(cols) < 3:
            continue
        for item in cols[2].split(" "):
            m = _B_ITEM.match(item)
            if not m:
                continue
            reading = m.group(2)
            if reading is not None and (not reading or reading.startswith("#")):
                reading = None
            counts[(m.group(1), reading)] += 1
    return dict(counts)


def _form_priors(pairs: list[WordPair],
                 counts: dict[tuple[str, str | None], int] | None) -> tuple[list[float], set[str]]:
    """Each pair's share of its written form, times the form's JMdict weight.

    Without Tatoeba counts the share is the pair's JMdict priority among the
    form's pairs; with them it is the smoothed Tatoeba frequency of the
    reading (an item without a reading is spread by the JMdict prior). Also
    returns the forms Tatoeba has at least TATOEBA_MIN_SINGLE items for."""
    by_text: dict[str, list[int]] = defaultdict(list)
    for i, p in enumerate(pairs):
        by_text[p.text].append(i)
    prior = [p.weight for p in pairs]
    known: set[str] = set()
    if not counts:
        return prior, known
    for text, idx in by_text.items():
        mass = sum(pairs[i].weight for i in idx)
        pi = {i: pairs[i].weight / mass for i in idx}
        # Mass of each reading (one reading can be in several entries).
        r_mass: dict[str, float] = defaultdict(float)
        for i in idx:
            r_mass[pairs[i].reading] += pi[i]
        none = counts.get((text, None), 0)
        n = none + sum(counts.get((text, r), 0) for r in r_mass)
        if n == 0:
            continue
        if n >= TATOEBA_MIN_SINGLE:
            known.add(text)
        for i in idx:
            r = pairs[i].reading
            c = counts.get((text, r), 0) * pi[i] / r_mass[r] + none * pi[i]
            prior[i] = mass * (c + TATOEBA_ALPHA * pi[i]) / (n + TATOEBA_ALPHA)
    return prior, known


def corpus_weights(pairs: Iterable[WordPair], freq: dict[str, float],
                   counts: dict[tuple[str, str | None], int] | None = None) -> list[WordPair]:
    """Re-weight JMdict pairs by corpus frequency.

    Every token's frequency is shared among all the pairs that claim it (as
    their form or an inflection stem), in proportion to their prior (see
    _form_priors), so 生まれ goes to both 生まれ and 生まれる, and with Tatoeba
    counts the token 上 goes mostly to うえ.

    A one-character form (下, 重, 生) takes no corpus share unless Tatoeba has
    evidence for its readings, and keeps only its JMdict weight: as a token, a
    lone kanji is often a piece MeCab split off a longer word (影響下, 九重町),
    and JMdict's tags cannot say which reading it had (下 has `nf07` on もと),
    so sharing its large count by them would hand it to whichever reading
    JMdict happens to tag."""
    pairs = list(pairs)
    prior, known = _form_priors(pairs, counts)
    forms = [surface_forms(p.text, p.pos) if len(p.text) > 1 or p.text in known else set()
             for p in pairs]
    claim: dict[str, float] = defaultdict(float)
    for i, fs in enumerate(forms):
        for t in fs:
            claim[t] += prior[i]
    out = []
    for i, (p, fs) in enumerate(zip(pairs, forms)):
        f = sum(freq.get(t, 0.0) * prior[i] / claim[t] for t in fs)
        out.append(WordPair(p.text, p.reading, f + JMDICT_BLEND * p.weight, p.pos))
    return out


# --- JmdictFurigana -----------------------------------------------------------------

# One furigana part: characters text[start..end] (inclusive) read as `rt`.
Segment = tuple[int, int, str]


def parse_furigana(source: str | Path | bytes) -> dict[tuple[str, str], list[Segment]]:
    """JmdictFurigana.txt: `text|reading|0:がっ;1:こう` (indices into text,
    `0-1:おとな` for a reading that spans several characters)."""
    data = _read_maybe_gz(source).decode("utf-8-sig")
    out: dict[tuple[str, str], list[Segment]] = {}
    for line in data.splitlines():
        parts = line.split("|")
        if len(parts) != 3:
            continue
        text, reading, furi = parts
        segs: list[Segment] = []
        for item in furi.split(";"):
            idx, _, rt = item.partition(":")
            if not rt:
                continue
            a, _, b = idx.partition("-")
            try:
                segs.append((int(a), int(b or a), rt))
            except ValueError:
                segs = []
                break
        if segs:
            out[(text, reading)] = segs
    return out


# --- JLPT vocabulary (tanos.co.uk) ---------------------------------------------------

_ALT_SPLIT = re.compile(r"[\s/／]+")
_KANA_ONLY = re.compile(r"^[ぁ-ゖァ-ヶー]+$")


def load_jlpt_vocab(files: dict[int, str | Path]) -> dict[tuple[str, str], int]:
    """(word, reading) -> new JLPT level (5 = N5, the easiest; a word in two
    lists keeps the easier level), from Jonathan Waller's vocabulary lists in
    their Anki 1.x deck form (`n5-vocab-kanji-hiragana.anki`: an SQLite file
    whose facts have a `Front` (the word in kanji) and a `Back` (its reading)).

    Entries listing alternatives are expanded: `川/河`, `しち / なな`, and
    `お・金持ち` (with and without the part before `・`). Readings that are
    not plain kana (grammar notes such as `（感）`) are skipped."""
    import sqlite3

    out: dict[tuple[str, str], int] = {}
    for level, path in files.items():
        con = sqlite3.connect(f"file:{Path(path)}?mode=ro", uri=True)
        try:
            names = {i: name for i, name in con.execute("select id, name from fieldModels")}
            facts: dict[int, dict[str, str]] = {}
            for fid, mid, value in con.execute("select factId, fieldModelId, value from fields"):
                facts.setdefault(fid, {})[names.get(mid, "")] = (value or "").strip()
        finally:
            con.close()
        for f in facts.values():
            texts = []
            for t in _ALT_SPLIT.split(f.get("Front", "")):
                if "・" in t:
                    texts += [t.replace("・", ""), t.split("・", 1)[1]]
                elif t:
                    texts.append(t)
            readings = [r for r in _ALT_SPLIT.split(f.get("Back", "")) if _KANA_ONLY.match(r)]
            for t in texts:
                for r in readings:
                    if out.get((t, r), 0) < level:
                        out[(t, r)] = level
    return out


# --- kana and matching ---------------------------------------------------------------

def to_hiragana(s: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in s)


def is_hiragana(c: str) -> bool:
    return "ぁ" <= c <= "ゖ"


def is_kanji(c: str) -> bool:
    o = ord(c)
    return 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF or 0x20000 <= o <= 0x2FFFF or c == "々"


_VOICED = {
    "か": "が", "き": "ぎ", "く": "ぐ", "け": "げ", "こ": "ご",
    "さ": "ざ", "し": "じ", "す": "ず", "せ": "ぜ", "そ": "ぞ",
    "た": "だ", "ち": "ぢじ", "つ": "づず", "て": "で", "と": "ど",
    "は": "ばぱ", "ひ": "びぴ", "ふ": "ぶぷ", "へ": "べぺ", "ほ": "ぼぽ",
}
_GEMINATING = "つくちき"


def sound_variants(form: str) -> set[str]:
    """Forms a reading takes inside a compound, besides itself: initial voicing
    (rendaku, 人々 ひとびと; h -> p, 一本 いっぽん) and a final っ (学校 がっこう)."""
    out = {form}
    if form and form[0] in _VOICED:
        out |= {v + form[1:] for v in _VOICED[form[0]]}
    if len(form) > 1 and form[-1] in _GEMINATING:
        out |= {x[:-1] + "っ" for x in list(out)}
    out.discard(form)
    return out


# Match quality, lower is better. A sound change costs SOUND on top.
EXACT, PARTIAL, STEM_ONLY, SOUND = 0, 1, 4, 2


def match_level(rt: str, following: str, reading: str) -> tuple[int, int] | None:
    """How well a per-kanji furigana `rt` (hiragana) fits one KANJIDIC2 reading.

    `reading` is an on'yomi (katakana) or a kun'yomi with an optional `.`
    okurigana marker (`い.きる`); `following` is the word's text after the
    kanji (`きる` in 生きる), used to tell い.きる from い.ける. Returns
    (level, -okurigana matched), so a full okurigana match beats a shorter one,
    or None when the reading does not fit."""
    stem, _, okuri = to_hiragana(reading).partition(".")
    best: tuple[int, int] | None = None
    forms = [(stem, None)] + [(stem + okuri[:j], j) for j in range(1, len(okuri) + 1)]
    for form, absorbed in forms:
        if rt == form:
            penalty = 0
        elif rt in sound_variants(form):
            penalty = SOUND
        else:
            continue
        if absorbed is not None:
            # 生花 いけばな: the okurigana is inside the furigana.
            cand = (PARTIAL + penalty, -absorbed)
        elif not okuri:
            # A reading without okurigana; if kana follow, a reading with
            # okurigana that matches them is the better explanation (九つ).
            level = EXACT if not following or not is_hiragana(following[0]) else PARTIAL
            cand = (level + penalty, 0)
        elif following.startswith(okuri):
            cand = (EXACT + penalty, -len(okuri))
        elif len(okuri) > 1 and following.startswith(okuri[:-1]):
            # 生き物: the stem form of い.きる.
            cand = (PARTIAL + penalty, -(len(okuri) - 1))
        else:
            cand = (STEM_ONLY + penalty, 0)
        if best is None or cand < best:
            best = cand
    return best


# --- scoring -----------------------------------------------------------------------

@dataclass
class KanjiReadings:
    onyomi: list[str]
    kunyomi: list[str]


@dataclass
class ReadingScores:
    """score[codepoint][reading]: summed word weight, for on and kun readings
    (readings spelled exactly as in the card lists), plus match statistics."""
    score: dict[int, dict[str, float]] = field(default_factory=dict)
    # level[codepoint][reading]: the easiest JLPT level (5 = N5) of a list word
    # using the reading; empty without the JLPT vocabulary lists.
    level: dict[int, dict[str, float]] = field(default_factory=dict)
    matched: int = 0      # per-kanji furigana parts attributed to a reading
    unmatched: int = 0    # parts of a known kanji that fit none of its readings
    unmatched_by_kanji: dict[int, dict[str, float]] = field(default_factory=dict)

    def of(self, cp: int, reading: str) -> float:
        return self.score.get(cp, {}).get(reading, 0.0)


def segments(text: str, reading: str,
             furigana: dict[tuple[str, str], list[Segment]]) -> list[Segment] | None:
    """The per-kanji furigana of a word: JmdictFurigana's, or for a word it
    lacks that is one kanji plus kana (明い あかるい, a typo in a JLPT list), the
    reading without the kana ending."""
    segs = furigana.get((text, reading))
    if segs:
        return segs
    if text and is_kanji(text[0]) and all(is_hiragana(c) for c in text[1:]):
        h = to_hiragana(reading)
        tail = text[1:]
        if h.endswith(tail) and len(h) > len(tail):
            return [(0, 0, h[:len(h) - len(tail)])]
    return None


def score_readings(kanji: dict[int, KanjiReadings], pairs: Iterable[WordPair],
                   furigana: dict[tuple[str, str], list[Segment]],
                   combine: str = "sum") -> ReadingScores:
    """Attribute every word's weight to the readings of the kanji it is written
    with: summed (`combine="sum"`) or the largest (`"max"`, for JLPT levels)."""
    res = ReadingScores()
    unmatched = defaultdict(lambda: defaultdict(float))
    for p in pairs:
        segs = segments(p.text, p.reading, furigana)
        if not segs:
            continue
        for start, end, rt in segs:
            if start != end or start >= len(p.text):
                continue  # jukujikun (今日 きょう) or a bad index: no per-kanji reading
            ch = p.text[start]
            if ch == "々" and start > 0:
                ch = p.text[start - 1]  # 人々: the repeated kanji
            if not is_kanji(ch):
                continue
            kr = kanji.get(ord(ch))
            if kr is None:
                continue
            rt_h = to_hiragana(rt)
            following = p.text[end + 1:]
            best: tuple[int, int] | None = None
            hits: list[str] = []
            for r in kr.onyomi + kr.kunyomi:
                lv = match_level(rt_h, following, r)
                if lv is None:
                    continue
                if best is None or lv < best:
                    best, hits = lv, [r]
                elif lv == best and r not in hits:
                    hits.append(r)
            if not hits:
                res.unmatched += 1
                unmatched[ord(ch)][rt_h] += p.weight
                continue
            res.matched += 1
            per = res.score.setdefault(ord(ch), {})
            for r in hits:
                if combine == "max":
                    per[r] = max(per.get(r, 0.0), p.weight)
                else:
                    per[r] = per.get(r, 0.0) + p.weight / len(hits)
    res.unmatched_by_kanji = {k: dict(v) for k, v in unmatched.items()}
    return res


# A bound form counts this much of its score when ordering (see bound_forms).
BOUND_FACTOR = 0.25


def bound_forms(kunyomi: list[str], marked: Iterable[str] = ()) -> set[str]:
    """Kun'yomi that only occur inside compounds, which make poor answers:
    those KANJIDIC2 only lists as a prefix/suffix (`marked`: 日 -び, 上 うわ-);
    a bare stem listed next to the same stem with okurigana (九 ここの next to
    ここの.つ, 重 おも next to おも.い: 九日, 重み); and a voiced or geminated
    form of another of the kanji's kun'yomi (殻 がら next to から, 舟 ぶね next
    to ふね: rendaku inside compounds)."""
    out = set(marked) & set(kunyomi)
    stems = {r.partition(".")[0] for r in kunyomi if "." in r}
    out |= {r for r in kunyomi if "." not in r and r in stems}
    plain = {r: r.replace(".", "") for r in kunyomi}
    for r in kunyomi:
        if any(plain[r] in sound_variants(plain[o]) for o in kunyomi if o != r):
            out.add(r)
    return out


def order_by_score(readings: list[str], scores: dict[str, float],
                   bound: Iterable[str] = (), tier: dict[str, int] | None = None,
                   level: dict[str, float] | None = None) -> list[str]:
    """Most common reading first; unscored and tied readings keep their order.

    The key is, in order: `tier` (reading -> 0, 1, ...; missing = 0: the
    builder puts the readings the Jōyō table approves in tier 0, the rest in
    tier 1); `level`, the easiest JLPT level of a list word using the reading
    (5 = N5 first; missing = 0, last); then the score. Readings in `bound` count
    BOUND_FACTOR of their score and no level: a bound form (九日's ここの, 日 -び)
    is never the answer because a list word happens to use it."""
    bound = set(bound)
    tier = tier or {}
    level = level or {}

    def key(r: str) -> tuple[float, float]:
        s = scores.get(r, 0.0)
        if r in bound:
            return 0.0, s * BOUND_FACTOR
        return level.get(r, 0.0), s

    return [r for *_, r in sorted((tier.get(r, 0), -key(r)[0], -key(r)[1], i, r)
                                  for i, r in enumerate(readings))]


# Common readings the Jōyō table does not approve, but that are the everyday
# reading of the kanji and worth asking (docs/common-readings.md, "Extra common
# readings"). Project-authored and hand-checked; keyed by kanji, values in
# KANJIDIC2 notation. They are ordered as if the table approved them, so they
# can be the asked reading; `classify()` still reports what the table says, so
# the reports keep counting them as not approved.
EXTRA_COMMON = {
    "等": ("など",),
    "経": ("た.つ",),
    "被": ("かぶ.る",),
}


def is_extra_common(cp: int, reading: str) -> bool:
    """True for a reading in EXTRA_COMMON, matched with or without okurigana
    (経 た.つ and a hypothetical たつ are the same reading)."""
    extra = EXTRA_COMMON.get(chr(cp))
    if not extra:
        return False
    from .joyo_table import plain

    p = plain(reading)
    return any(plain(r) == p for r in extra)


def joyo_tiers(cp: int, readings: list[str], joyo: dict | None) -> dict[str, int] | None:
    """Tier 0 for the readings the Jōyō table approves for this kanji (or that
    EXTRA_COMMON adds), 1 for the rest (indented or not in the table). None
    (one tier) for a kanji the table does not list, or without a table."""
    if not joyo:
        return None
    from .joyo_table import classify

    entry = joyo.get(chr(cp))
    if entry is None:
        return None
    return {r: 0 if classify(r, entry) == "joyo" or is_extra_common(cp, r) else 1
            for r in readings}


def share(readings: list[str], scores: dict[str, float]) -> dict[str, float]:
    """Each reading's fraction of its kind's total score (0 when nothing scored)."""
    total = sum(scores.get(r, 0.0) for r in readings)
    return {r: (scores.get(r, 0.0) / total if total else 0.0) for r in readings}


def zipf(x: float) -> float:
    """A readable magnitude for reports: the Zipf value of a frequency
    (log10 of occurrences per billion words; 0 for nothing)."""
    return math.log10(x * 1e9) if x > 0 else 0.0


# --- builder entry points ----------------------------------------------------------

# A word on a JLPT list counts this many times its frequency (by its level), so
# among readings whose easiest list word has the same level, the one in more
# and easier list words wins (出: 出る beats the compound verbs in 〜出す).
LEVEL_MULT = {5: 16.0, 4: 8.0, 3: 4.0, 2: 2.0, 1: 1.5}


def compute_scores(raw: list, jmdict: str | Path | bytes, furigana: str | Path | bytes,
                   wordfreq: str | Path | bytes | None = None,
                   tatoeba: str | Path | bytes | None = None,
                   jlpt_vocab: dict[tuple[str, str], int] | None = None) -> ReadingScores:
    """Scores for the readings of every kanji in `raw` (KANJIDIC2 RawKanji).

    Without `wordfreq` the JMdict priorities alone weigh the words; `tatoeba`
    (only used with `wordfreq`) splits a form's frequency among its readings.
    With `jlpt_vocab` ((word, reading) -> level, load_jlpt_vocab) each word's
    weight is multiplied by LEVEL_MULT for its level, and `level` holds each
    reading's easiest list level."""
    kanji = {k.codepoint: KanjiReadings(k.onyomi, k.kunyomi) for k in raw}
    furi = parse_furigana(furigana)
    pairs: list[WordPair] = list(parse_jmdict(jmdict))
    if wordfreq is not None:
        counts = load_tatoeba_counts(tatoeba) if tatoeba is not None else None
        pairs = corpus_weights(pairs, load_wordfreq(wordfreq), counts)
    if jlpt_vocab:
        pairs = [WordPair(p.text, p.reading,
                          p.weight * LEVEL_MULT.get(jlpt_vocab.get((p.text, p.reading), 0), 1.0),
                          p.pos) for p in pairs]
    res = score_readings(kanji, pairs, furi)
    if jlpt_vocab:
        lv = score_readings(kanji, [WordPair(t, r, float(n)) for (t, r), n in jlpt_vocab.items()],
                            furi, combine="max")
        res.level = lv.score
    return res


@dataclass
class ReorderStats:
    kanji: int = 0
    first_on_changed: int = 0
    first_kun_changed: int = 0
    readings_on: int = 0
    readings_kun: int = 0
    unscored_on: int = 0     # on'yomi that no word in the data uses
    unscored_kun: int = 0
    dropped_on: list[int] = field(default_factory=list)    # codepoints
    dropped_kun: list[int] = field(default_factory=list)
    # Both kinds had nothing worth asking: (codepoint, kind kept). Or the only
    # kind the card has: (codepoint, kind kept), kept so the card keeps a reading.
    both_failed: list[tuple[int, str]] = field(default_factory=list)
    only_kind_kept: list[tuple[int, str]] = field(default_factory=list)


def worth_asking(cp: int, readings: list[str], scores: dict[str, float],
                 joyo: dict | None) -> bool:
    """Whether a kind of a kanji has a reading worth asking: for a kanji in the
    Jōyō table, one the table approves (not indented) or EXTRA_COMMON adds; for
    any other kanji, one that some word uses (score > 0)."""
    if not readings:
        return False
    entry = joyo.get(chr(cp)) if joyo else None
    if entry is not None:
        from .joyo_table import classify

        return any(classify(r, entry) == "joyo" or is_extra_common(cp, r) for r in readings)
    return any(scores.get(r, 0.0) > 0 for r in readings)


def reorder_raw(raw: list, scores: ReadingScores, joyo: dict | None = None,
                drop: bool = False) -> tuple[list, ReorderStats]:
    """A copy of `raw` (KANJIDIC2 RawKanji) with each kanji's on'yomi and
    kun'yomi ordered most useful first: with `joyo`, the table's approved
    readings before the rest; then by the easiest JLPT list level (if
    `scores.level`); then by score.

    With `drop`, a kind with nothing worth asking (worth_asking) is emptied, so
    the device asks the other kind. Never both: if both kinds would go, the one
    with the higher total score stays; a card's only kind always stays. A kind
    that stays keeps all its readings."""
    st = ReorderStats()
    out = []
    for k in raw:
        cp = k.codepoint
        sc = scores.score.get(cp, {})
        lv = scores.level.get(cp, {})
        on = order_by_score(k.onyomi, sc, tier=joyo_tiers(cp, k.onyomi, joyo), level=lv)
        kun = order_by_score(k.kunyomi, sc, bound_forms(k.kunyomi, k.kun_bound),
                             joyo_tiers(cp, k.kunyomi, joyo), lv)
        all_on = all_kun = None
        if drop:
            all_on, all_kun = list(on), list(kun)
            bad_on = bool(on) and not worth_asking(cp, on, sc, joyo)
            bad_kun = bool(kun) and not worth_asking(cp, kun, sc, joyo)
            if bad_on and bad_kun:
                keep = "on" if sum(sc.get(r, 0.0) for r in on) >= sum(
                    sc.get(r, 0.0) for r in kun) else "kun"
                st.both_failed.append((cp, keep))
                bad_on, bad_kun = keep == "kun", keep == "on"
            if bad_on and not kun:
                st.only_kind_kept.append((cp, "on"))
                bad_on = False
            if bad_kun and not on:
                st.only_kind_kept.append((cp, "kun"))
                bad_kun = False
            if bad_on:
                st.dropped_on.append(cp)
                on = []
            if bad_kun:
                st.dropped_kun.append(cp)
                kun = []
        st.kanji += 1
        st.first_on_changed += bool(on) and on[0] != k.onyomi[0]
        st.first_kun_changed += bool(kun) and kun[0] != k.kunyomi[0]
        st.readings_on += len(on)
        st.readings_kun += len(kun)
        st.unscored_on += sum(1 for r in on if not sc.get(r))
        st.unscored_kun += sum(1 for r in kun if not sc.get(r))
        # With `drop`, the full lists ride along (RawKanji.all_*), so reading
        # distractors are kept away from an emptied kind's readings too.
        out.append(replace(k, onyomi=on, kunyomi=kun, all_onyomi=all_on, all_kunyomi=all_kun))
    return out, st
