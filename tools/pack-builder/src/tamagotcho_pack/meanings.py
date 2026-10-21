"""Meanings in a useful order (`build --meaning-order`, docs/meaning-order.md).

The device shows a card's first meaning: as the quiz option, in the "which
kanji means ..." prompt and first on the detail card. KANJIDIC2's order is not
always the useful one: its English list has quirks (機 "loom; mechanism;
machine", 私 "private; I; me", 原 "meadow; original"), and its Spanish list
often follows neither (持: "duración; propiedad; cargo; sostener; mantener"
against "hold; have"). Three steps:

A. **English, by a learner signal** (EnglishOrder): every KANJIDIC2 English
   meaning is scored by the JLPT vocabulary words that contain the kanji
   (Jonathan Waller's lists, as for the readings), through those words'
   JMdict glosses, weighted by the word's level (N5 most), by how much of the
   word is the kanji, and by the gloss's place in the entry. 私: the N5 word
   私 "I; me" puts "I" first; 機: 飛行機 "airplane" and 機械 "machine". The
   list is sorted by score, KANJIDIC2's order breaking ties; a meaning with too
   little evidence scores 0.
B. **Spanish, by the English order** (MeaningOrder), below: each Spanish gloss
   is matched to the English gloss it translates.
C. **Overrides**: a short project-authored table
   (`data/meaning_overrides.tsv`) puts one of KANJIDIC2's own meanings first
   where A and B still go wrong (機 máquina). Never new text.

Matching Spanish to English (B):

1. **Normalisation**, both languages: lower case, no parentheses, no leading
   "to"/"to be" (English) or article (Spanish), accents ignored; a Spanish
   gloss is also tried without a reflexive "-se" and without a trailing
   preposition (debajo de -> debajo), an English one by its lemma (going ->
   go) when the dictionary lacks the form.
2. **Translation** (strong evidence): the Spanish gloss is the English one
   (hospital, China) or one of its translations in FreeDict's English-Spanish
   dictionary (WikDict, built from Wiktionary; CC BY-SA 3.0).
3. **Related forms** (weak evidence), only for a gloss with no translation
   match against that English gloss: a word sharing a long stem with one of its
   translations (préstamo / prestar for "lend"), a cognate (matrimonio /
   matrimony), the head words of multi-word glosses (pájaro grande / large
   bird), or a translation of an English near-synonym, one that shares
   translations with it (debajo is a translation of "under", which shares bajo
   and abajo with "below").

Each Spanish gloss scores the index of the first English gloss it matches; a
weak match counts one position later (it ties with a translation of the next
English gloss). The list is sorted by score, ties and unmatched glosses keeping
KANJIDIC2's order, unmatched glosses last. Nothing is added or removed: the
builder's caps (as many as English has, at most three) then apply to the
reordered list, so the glosses that match the English list are the ones kept.
"""

from __future__ import annotations

import io
import re
import tarfile
import unicodedata
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field, replace
from pathlib import Path

TEI = "{http://www.tei-c.org/ns/1.0}"

# Languages with a dictionary. fr/pt: no pinned English-French/Portuguese
# release (docs/meaning-order.md); they keep KANJIDIC2's order.
LANGS = frozenset({"es"})

# Penalty, in English positions, of a match through a related form.
WEAK = 1
UNMATCHED = 1_000_000

# Words that are never a head word.
EN_STOP = frozenset({"a", "an", "the", "of", "for", "to", "be", "in", "on", "at", "with", "one's",
                     "oneself", "and", "or", "as", "by", "up", "out", "off", "into", "from",
                     "something", "someone", "etc"})
ES_STOP = frozenset({"de", "del", "la", "el", "los", "las", "un", "una", "y", "o", "a", "al", "en",
                     "con", "por", "para", "se", "lo", "su", "sus", "que"})
_ES_ARTICLE = re.compile(r"^(el|la|los|las|un|una|lo|unos|unas) ")
_EN_PREFIX = re.compile(r"^(to be |to |be )")
_PARENS = re.compile(r"\([^)]*\)")
_OPEN_PAREN = re.compile(r"\(.*$")   # KANJIDIC2 splits a few glosses inside parentheses
_SPACES = re.compile(r"\s+")
_WORD = re.compile(r"[\w'-]+")


# --- normalisation ----------------------------------------------------------------

def strip_accents(s: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFD", s) if unicodedata.category(c) != "Mn")


def _clean(s: str) -> str:
    s = _OPEN_PAREN.sub(" ", _PARENS.sub(" ", s.lower()))
    return _SPACES.sub(" ", s).strip(" .-")


def norm_en(s: str) -> str:
    """`To hold (in one's hand)` -> `hold`; `Inst.` -> `inst`."""
    return _EN_PREFIX.sub("", _clean(s))


def norm_es(s: str) -> str:
    """`La línea (texto)` -> `linea`: no article, no accents."""
    return strip_accents(_ES_ARTICLE.sub("", _clean(s)))


def es_variants(n: str) -> list[str]:
    """A normalised Spanish gloss, then without a reflexive `-se` (levantarse
    -> levantar) and without a trailing preposition (debajo de -> debajo)."""
    out = [n]
    m = re.match(r"^(\w+(?:ar|er|ir))se\b(.*)$", n)
    if m:
        out.append((m.group(1) + m.group(2)).strip())
    m = re.match(r"^(.*) (de|a|en|con|por)$", n)
    if m:
        out.append(m.group(1))
    return list(dict.fromkeys(out))


def en_variants(n: str, known) -> list[str]:
    """A normalised English gloss, then (when `known`, the dictionary's
    headwords, lacks it) the lemmas it has: going -> go, places -> place."""
    if n in known:
        return [n]
    cands = []
    if n.endswith("ing"):
        b = n[:-3]
        cands += [b, b + "e"]
        if len(b) > 2 and b[-1] == b[-2]:
            cands.append(b[:-1])
    if n.endswith("ies"):
        cands.append(n[:-3] + "y")
    elif n.endswith("es"):
        cands += [n[:-2], n[:-1]]
    elif n.endswith("s"):
        cands.append(n[:-1])
    if n.endswith("ed"):
        cands += [n[:-2], n[:-1]]
    return [n] + [c for c in dict.fromkeys(cands) if c in known]


def _words(s: str, stop) -> list[str]:
    return [w for w in _WORD.findall(s) if w not in stop]


def en_heads(n: str) -> set[str]:
    """Head-word candidates of an English gloss: the first word before `of` /
    `for` (counter for days), else the first and last (black ink, take hold)."""
    ws = _words(n, EN_STOP)
    if not ws:
        return set()
    return {ws[0]} if re.search(r" (of|for) ", n) else {ws[0], ws[-1]}


def es_heads(n: str) -> set[str]:
    """Head-word candidates of a Spanish gloss: the first word before `de` /
    `para` (contador para casas), else the first and last (oficial militar)."""
    ws = _words(n, ES_STOP)
    if not ws:
        return set()
    return {ws[0]} if re.search(r" (de|del|para) ", n) else {ws[0], ws[-1]}


def _prefix(a: str, b: str) -> int:
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


def _levenshtein(a: str, b: str) -> int:
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def same_stem(a: str, b: str) -> bool:
    """Two words of one family: a common prefix of at least 5 letters and 3/4 of
    the shorter word (prestamo / prestar, eterno / eternidad)."""
    if len(a) < 5 or len(b) < 5:
        return False
    p = _prefix(a, b)
    return p >= 5 and p >= 0.75 * min(len(a), len(b))


def cognate(a: str, b: str) -> bool:
    """A Spanish and an English word that look alike (matrimonio / matrimony):
    5+ letters each, the first 3 shared, edit distance at most 40 %."""
    a, b = strip_accents(a), strip_accents(b)
    if len(a) < 5 or len(b) < 5 or _prefix(a, b) < 3:
        return False
    return 1 - _levenshtein(a, b) / max(len(a), len(b)) >= 0.6


# --- the dictionary ---------------------------------------------------------------

@dataclass
class Bilingual:
    """English headword (normalised) -> its Spanish translations (normalised,
    with their es_variants)."""
    trans: dict[str, frozenset[str]]
    _rev: dict[str, frozenset[str]] | None = field(default=None, repr=False)

    def get(self, en: str) -> frozenset[str]:
        return self.trans.get(en, frozenset())

    def __contains__(self, en: str) -> bool:
        return en in self.trans

    @property
    def rev(self) -> dict[str, frozenset[str]]:
        """Spanish translation -> the English headwords that list it."""
        if self._rev is None:
            rev: dict[str, set[str]] = defaultdict(set)
            for e, tr in self.trans.items():
                for s in tr:
                    rev[s].add(e)
            self._rev = {s: frozenset(es) for s, es in rev.items()}
        return self._rev


def parse_freedict_tei(source) -> Bilingual:
    """Read a FreeDict TEI dictionary (a file object or path): each entry's
    `form/orth` and the `quote`s of its `cit type="trans"` elements."""
    trans: dict[str, set[str]] = defaultdict(set)
    for _, el in ET.iterparse(source, events=("end",)):
        if el.tag != TEI + "entry":
            continue
        orth = el.find(f"{TEI}form/{TEI}orth")
        if orth is not None and orth.text:
            k = norm_en(orth.text)
            for cit in el.iter(TEI + "cit"):
                if cit.get("type") != "trans":
                    continue
                for q in cit.iter(TEI + "quote"):
                    if q.text and q.text.strip():
                        trans[k].update(es_variants(norm_es(q.text)))
        el.clear()
    trans.pop("", None)
    return Bilingual({k: frozenset(v - {""}) for k, v in trans.items()})


def load_freedict(source: str | Path | bytes) -> Bilingual:
    """FreeDict's English-Spanish dictionary from its source release
    (`freedict-eng-spa-<version>.src.tar.xz`, the TEI inside) or a bare TEI file."""
    data = source if isinstance(source, bytes) else Path(source).read_bytes()
    if data[:6] == b"\xfd7zXZ\x00":
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tar:
            member = next((m for m in tar.getmembers() if m.name.endswith(".tei") and m.isfile()),
                          None)
            if member is None:
                raise ValueError("no .tei file in the FreeDict archive")
            f = tar.extractfile(member)
            assert f is not None
            return parse_freedict_tei(f)
    return parse_freedict_tei(io.BytesIO(data))


# --- matching ---------------------------------------------------------------------

class MeaningOrder:
    """Orders a kanji's glosses in one language by its English glosses."""

    def __init__(self, dic: Bilingual):
        self.dic = dic

    def _keys(self, en: str) -> list[str]:
        return en_variants(norm_en(en), self.dic.trans)

    def _translation(self, vs: list[str], keys: list[str]) -> bool:
        for k in keys:
            ka = strip_accents(k)
            tr = self.dic.get(k)
            if ka in vs or any(v in tr for v in vs):
                return True
        return False

    def _related(self, vs: list[str], keys: list[str]) -> str | None:
        """Why a gloss with no translation match is related to the English one."""
        dic = self.dic
        single = [v for v in vs if " " not in v]
        for k in keys:
            if any(" " not in t and any(same_stem(v, t) for v in single) for t in dic.get(k)):
                return "stem"
        sh = {w for v in vs for w in es_heads(v)}
        if any(cognate(a, b) for a in sh for k in keys for b in en_heads(k)):
            return "cognate"
        eh = {x for k in keys for w in en_heads(k) for x in en_variants(w, dic.trans)}
        if (eh - set(keys) or sh - set(vs)) and any(x in dic.get(w) for w in eh for x in sh):
            return "head"
        for k in keys:
            mine = dic.get(k)
            if len(mine) < 2:
                continue
            for v in vs:
                for e2 in dic.rev.get(v, ()):
                    if e2 == k:
                        continue
                    shared = mine & dic.get(e2)
                    if len(shared) >= 2 and len(shared) >= 0.3 * min(len(mine), len(dic.get(e2))):
                        return "synonym"
        return None

    def match(self, gloss: str, english: list[str]) -> tuple[int, str] | None:
        """(score, how) of the first English gloss `gloss` matches: its index for
        a translation, index + WEAK for a related form. None: no match."""
        vs = [v for v in es_variants(norm_es(gloss)) if v]
        if not vs:
            return None
        best: tuple[int, str] | None = None
        for i, e in enumerate(english):
            if best is not None and best[0] <= i:
                break  # a later English gloss cannot score better
            keys = [k for k in self._keys(e) if k]
            if not keys:
                continue
            if self._translation(vs, keys):
                return (i, "translation")
            if best is None:
                how = self._related(vs, keys)
                if how:
                    best = (i + WEAK, how)
        return best

    def ranked(self, glosses: list[str], english: list[str]) -> list[tuple[str, tuple | None]]:
        """The glosses in their new order, each with its match (or None)."""
        ms = [self.match(g, english) for g in glosses]
        key = sorted(range(len(glosses)), key=lambda j: (ms[j][0] if ms[j] else UNMATCHED, j))
        return [(glosses[j], ms[j]) for j in key]

    def order(self, glosses: list[str], english: list[str]) -> list[str]:
        if not glosses or not english:
            return list(glosses)
        return [g for g, _ in self.ranked(glosses, english)]


# --- English: the learner signal (A) ------------------------------------------------

# A word's weight by its JLPT list (5 = N5), as for the readings' learner key.
LEVEL_WEIGHT = {5: 16.0, 4: 8.0, 3: 4.0, 2: 2.0, 1: 1.5}
SENSES = 3          # a word's first senses that count; sense s weighs 1/(1+s)
GLOSS_STEP = 0.2    # the p-th gloss of a sense weighs 1/(1 + 0.2 p)
PARTIAL = 0.25      # the meaning is part of a longer gloss (next <- next week)
STEM = 0.5          # one-word meaning and gloss of one family (original <- origin)
MIN_SCORE = 2.0     # less evidence than this counts as none (an N3 compound's first gloss: 2)


def _stem(w: str) -> str:
    """A crude English stem, applied to both sides of a comparison: going/go,
    studies/study, comes/come, places/place."""
    if w.endswith("ing") and len(w) >= 5:
        w = w[:-3]
    elif w.endswith(("ied", "ies")) and len(w) >= 5:
        w = w[:-3] + "y"
    elif w.endswith("ed") and len(w) >= 5:
        w = w[:-2]
    elif w.endswith("es") and len(w) >= 5:
        w = w[:-2]
    elif w.endswith("s") and len(w) >= 4 and not w.endswith("ss"):
        w = w[:-1]
    return w[:-1] if w.endswith("e") and len(w) > 3 else w


def gloss_tokens(s: str) -> tuple[str, ...]:
    """The stemmed content words of an English meaning or gloss ("to come
    (spatially or temporally)" -> ("com",)); a meaning made only of small
    words keeps them ("in")."""
    ws = _WORD.findall(norm_en(s))
    return tuple(_stem(w) for w in ws if w not in EN_STOP) or tuple(ws)


def gloss_match(meaning: tuple[str, ...], gloss: tuple[str, ...]) -> float:
    """How well a KANJIDIC2 meaning matches a word's gloss (token tuples)."""
    if not meaning or not gloss:
        return 0.0
    if meaning == gloss:
        return 1.0
    if set(meaning) <= set(gloss):
        return PARTIAL / max(1, len(gloss) - len(meaning))
    if len(meaning) == 1 and len(gloss) == 1 and same_stem(meaning[0], gloss[0]):
        return STEM
    return 0.0


@dataclass(frozen=True)
class VocabWord:
    word: str                         # as written, with kanji (飛行機)
    reading: str
    level: int                        # 5 = N5 .. 1 = N1
    senses: tuple[tuple[str, ...], ...]  # JMdict's English glosses, by sense


def parse_jmdict_glosses(source: str | Path | bytes, kebs: set[str] | None = None
                         ) -> dict[tuple[str, str], list[list[str]]]:
    """(kanji form, reading) -> the English glosses of its senses, in order,
    from JMdict (all entries with that pair; senses restricted to other forms
    or readings left out). Only the forms in `kebs`, when given."""
    from .readings import _read_maybe_gz

    data = _read_maybe_gz(source)
    out: dict[tuple[str, str], list[list[str]]] = defaultdict(list)
    for _, el in ET.iterparse(io.BytesIO(data), events=("end",)):
        if el.tag != "entry":
            continue
        forms = [k.findtext("keb") or "" for k in el.findall("k_ele")]
        if kebs is not None:
            forms = [f for f in forms if f in kebs]
        if forms:
            rebs = [(r.findtext("reb") or "", {x.text for x in r.findall("re_restr")})
                    for r in el.findall("r_ele") if r.find("re_nokanji") is None]
            senses = []
            for s in el.findall("sense"):
                gl = [g.text for g in s.findall("gloss") if g.text]
                if gl:
                    senses.append(({x.text for x in s.findall("stagk")},
                                   {x.text for x in s.findall("stagr")}, gl))
            for keb in forms:
                for reb, restr in rebs:
                    if restr and keb not in restr:
                        continue
                    ss = [gl for sk, sr, gl in senses
                          if (not sk or keb in sk) and (not sr or reb in sr)]
                    if ss:
                        out[(keb, reb)].extend(ss)
        el.clear()
    return dict(out)


def load_learner_vocab(jlpt_vocab: dict[tuple[str, str], int],
                       jmdict: str | Path | bytes) -> list[VocabWord]:
    """The JLPT vocabulary words written with kanji, with their JMdict glosses
    (by word and reading; a word whose reading JMdict does not pair with it
    takes its form's first entry)."""
    from .readings import is_kanji

    words = {w for w, _ in jlpt_vocab if any(is_kanji(c) for c in w)}
    glosses = parse_jmdict_glosses(jmdict, words)
    by_form: dict[str, list[list[str]]] = {}
    for (keb, _), ss in glosses.items():
        by_form.setdefault(keb, ss)
    out = []
    for (w, r), level in sorted(jlpt_vocab.items()):
        if w not in words:
            continue
        ss = glosses.get((w, r)) or by_form.get(w)
        if ss:
            out.append(VocabWord(w, r, level, tuple(tuple(s) for s in ss)))
    return out


class EnglishOrder:
    """Orders a kanji's KANJIDIC2 English meanings by the learner signal."""

    def __init__(self, vocab: list[VocabWord]):
        from .readings import is_kanji

        self.by_kanji: dict[str, list[tuple[VocabWord, int]]] = defaultdict(list)
        for v in vocab:
            ks = {c for c in v.word if is_kanji(c)}
            for c in ks:
                self.by_kanji[c].append((v, len(ks)))
        self._tokens: dict[str, tuple[str, ...]] = {}

    def _tok(self, s: str) -> tuple[str, ...]:
        t = self._tokens.get(s)
        if t is None:
            t = self._tokens[s] = gloss_tokens(s)
        return t

    def scores(self, kanji: str, english: list[str]) -> list[float]:
        """Each meaning's score: over the JLPT words with the kanji, the word's
        level weight / its number of kanji x its best match (sense and gloss
        position weighted). Below MIN_SCORE: 0."""
        mts = [self._tok(m) for m in english]
        sc = [0.0] * len(english)
        for v, nk in self.by_kanji.get(kanji, ()):
            best = [0.0] * len(english)
            for s, gl in enumerate(v.senses[:SENSES]):
                for p, g in enumerate(gl):
                    w = 1.0 / ((1 + s) * (1 + GLOSS_STEP * p))
                    gt = self._tok(g)
                    for i, mt in enumerate(mts):
                        x = w * gloss_match(mt, gt)
                        if x > best[i]:
                            best[i] = x
            weight = LEVEL_WEIGHT.get(v.level, 1.0) / nk
            for i in range(len(english)):
                sc[i] += weight * best[i]
        return [x if x >= MIN_SCORE else 0.0 for x in sc]

    def order(self, kanji: str, english: list[str]) -> list[str]:
        sc = self.scores(kanji, english)
        return [english[i] for i in sorted(range(len(english)), key=lambda i: (-sc[i], i))]


# --- overrides (C) -------------------------------------------------------------------

@dataclass(frozen=True)
class Override:
    kanji: str
    lang: str
    order: tuple[str, ...]   # these KANJIDIC2 meanings first, in this order
    reason: str
    line: int


def load_overrides(source: str | Path) -> dict[tuple[str, str], Override]:
    """The override table: tab-separated `kanji, lang, first, [order], reason`;
    `order` (optional) lists more meanings, `; `-separated, to follow `first`.
    `#` starts a comment line. Raises ValueError on a malformed line."""
    out: dict[tuple[str, str], Override] = {}
    for n, line in enumerate(Path(source).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != 5:
            raise ValueError(f"meaning overrides line {n}: {len(cols)} columns, expected 5")
        kanji, lang, first, rest, reason = (c.strip() for c in cols)
        if len(kanji) != 1 or lang not in ("en", "es", "fr", "pt") or not first or not reason:
            raise ValueError(f"meaning overrides line {n}: bad kanji, language, meaning or reason")
        order = (first,) + tuple(m.strip() for m in rest.split(";") if m.strip())
        if len(set(order)) != len(order):
            raise ValueError(f"meaning overrides line {n}: a meaning is listed twice")
        if (kanji, lang) in out:
            raise ValueError(f"meaning overrides line {n}: {kanji} {lang} is already overridden")
        out[(kanji, lang)] = Override(kanji, lang, order, reason, n)
    return out


def check_overrides(overrides: dict[tuple[str, str], Override], raw: list) -> list[str]:
    """Every override names a kanji of KANJIDIC2 and only meanings it has in
    that language (no new text)."""
    meanings = {chr(k.codepoint): k.meanings for k in raw}
    errors = []
    for (kanji, lang), ov in sorted(overrides.items(), key=lambda x: x[1].line):
        have = meanings.get(kanji, {}).get(lang)
        if not have:
            errors.append(f"line {ov.line}: {kanji} has no {lang} meanings in KANJIDIC2")
            continue
        missing = [m for m in ov.order if m not in have]
        if missing:
            errors.append(f"line {ov.line}: {kanji} {lang}: not a KANJIDIC2 meaning: "
                          + ", ".join(repr(m) for m in missing))
    return errors


def apply_override(glosses: list[str], ov: Override | None) -> list[str]:
    if ov is None:
        return list(glosses)
    return list(ov.order) + [g for g in glosses if g not in ov.order]


# --- all together ---------------------------------------------------------------------

@dataclass
class MeaningStats:
    kanji: int = 0           # kanji with meanings in the language (and English)
    first_changed: int = 0   # ... whose first meaning changed
    glosses: int = 0
    matched: int = 0         # glosses matched to an English gloss (B)
    overridden: int = 0


def reorder_raw(raw: list, lang: str, english: EnglishOrder | None = None,
                spanish: MeaningOrder | None = None,
                overrides: dict[tuple[str, str], Override] | None = None
                ) -> tuple[list, MeaningStats]:
    """Copies of the RawKanji with their English meanings in the learner order
    (A, when `english` is given) and their `lang` meanings in the order of the
    (new) English ones (B, when `spanish` is given), overrides applied (C)."""
    overrides = overrides or {}
    st = MeaningStats()
    out = []
    for k in raw:
        ch = chr(k.codepoint)
        ms = dict(k.meanings)
        en = ms.get("en", [])
        if en:
            new_en = english.order(ch, en) if english else list(en)
            ov = overrides.get((ch, "en"))
            ms["en"] = apply_override(new_en, ov)
            if lang == "en":
                st.kanji += 1
                st.first_changed += ms["en"][0] != en[0]
                st.overridden += ov is not None
        xs = ms.get(lang, [])
        if lang != "en" and xs:
            st.kanji += 1
            new = list(xs)
            if spanish and ms.get("en"):
                ranked = spanish.ranked(xs, ms["en"])
                new = [g for g, _ in ranked]
                st.glosses += len(xs)
                st.matched += sum(1 for _, m in ranked if m)
            ov = overrides.get((ch, lang))
            ms[lang] = apply_override(new, ov)
            st.overridden += ov is not None
            st.first_changed += ms[lang][0] != xs[0]
        out.append(replace(k, meanings=ms) if ms != k.meanings else k)
    return out, st
