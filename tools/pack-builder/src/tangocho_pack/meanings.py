"""Spanish meanings in the order of the English ones (`build --meaning-order`).

KANJIDIC2 lists a kanji's English meanings most common first, but its Spanish
list often is not (持: "duración; propiedad; cargo; sostener; mantener" against
"hold; have"), and the device shows the first meaning: as the quiz option, in
the "which kanji means ..." prompt and first on the detail card. This module
matches each Spanish gloss to the English gloss it translates and sorts the
Spanish list by the English order (docs/meaning-order.md):

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


@dataclass
class MeaningStats:
    kanji: int = 0           # kanji with meanings in the language and in English
    first_changed: int = 0   # ... whose first meaning changed
    glosses: int = 0
    matched: int = 0         # glosses matched to an English gloss


def reorder_raw(raw: list, order: MeaningOrder, lang: str) -> tuple[list, MeaningStats]:
    """Copies of the RawKanji with their `lang` meanings in the English order."""
    st = MeaningStats()
    out = []
    for k in raw:
        xs, en = k.meanings.get(lang, []), k.meanings.get("en", [])
        if not xs or not en:
            out.append(k)
            continue
        ranked = order.ranked(xs, en)
        new = [g for g, _ in ranked]
        st.kanji += 1
        st.first_changed += new[0] != xs[0]
        st.glosses += len(xs)
        st.matched += sum(1 for _, m in ranked if m)
        out.append(replace(k, meanings={**k.meanings, lang: new}))
    return out, st
