"""Licence-compatible alternatives to JMdict's Spanish glosses for the example
words of decision 9 (docs/example-words.md, "Alternatives"). Nothing here
changes a pack.

It takes the words scripts/example_words_report.py picks (one per asked
reading of every card of the published decks) and, for each, the Spanish gloss
each candidate source would give:

- A. Open Multilingual Wordnet 1.4: the Japanese WordNet (NICT) gives the
  word's synsets; the one whose English (Princeton WordNet) lemmas contain the
  word's first JMdict_e gloss is taken, the most frequent English sense first;
  the Spanish gloss is that synset's first Spanish lemma (MCR 3.0).
- B1. Spanish Wiktionary's Japanese entries (kaikki.org's wiktextract data):
  the first Spanish definition of the word, read as given.
- B2. English Wiktionary's Japanese entries (kaikki.org): the first English
  gloss, then translated as in C. FreeDict's eng-spa dictionary *is* English
  Wiktionary's translation tables (WikDict, via DBnary), so this is the
  "Wiktionary glosses plus translation tables" route.
- C. The first JMdict_e gloss translated with FreeDict's eng-spa dictionary
  (the one `--meaning-order` pins): the first Spanish translation of the first
  sense of the entry with the word's part of speech.
- D. Wikidata lexemes (CC0): a Japanese lexeme with the word as its lemma
  (and the reading as its hiragana lemma, when it has one), its first sense
  linked to an item (P5137), and that item's Spanish label.
- E. Composition: the first Spanish meaning of each of the word's kanji, as
  the published Spanish packs store it (休日 = descanso + día).

It prints, as Markdown: per source and deck, how many words get a Spanish
gloss and a short one (≤ --max-gloss characters); a 30-word N5-N3 spot check
with every source side by side; what E needs beyond the pack; and the number
of words a project-authored table (F) would have to review per level.

Downloads (--fetch) go to cache/research/ and are checked against SHA256:
the OMW release is versioned; the kaikki.org files and the Wikidata query
result are regenerated upstream, so a later fetch fails the check until the
pins below are reviewed and updated.

Usage (after `tamagotcho-pack fetch --readings --meanings` and
`scripts/example_words_report.py --fetch`):

    uv run python scripts/example_words_alternatives.py [--fetch] --packs DIR [--tsv out/alternatives.tsv]
"""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import pickle
import random
import re
import sys
import tarfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

from tamagotcho_pack import paths
from tamagotcho_pack.meanings import TEI, norm_en
from tamagotcho_pack.pack import MEANING_SEP, read_pack
from tamagotcho_pack.readings import is_kanji, to_hiragana

sys.path.insert(0, str(Path(__file__).resolve().parent))
from example_words_report import DECK_TITLE, DECKS, Picks, Row, compute_picks, pct  # noqa: E402

RESEARCH = paths.CACHE / "research"
OMW_URL = "https://github.com/omwn/omw-data/releases/download/v1.4/omw-1.4.tar.xz"
KAIKKI_EN_URL = "https://kaikki.org/dictionary/Japanese/kaikki.org-dictionary-Japanese.jsonl.gz"
KAIKKI_ES_URL = "https://kaikki.org/eswiktionary/raw-wiktextract-data.jsonl.gz"
WIKIDATA_SPARQL = "https://query.wikidata.org/sparql"
# Japanese lexemes, their lemmas, and their senses linked to an item, with the
# item's Spanish label. (Only 12 Japanese lexemes have a Spanish gloss of
# their own, 2026-10-23.)
WIKIDATA_QUERY = (
    'SELECT ?lexeme ?lemma ?sense ?item ?es WHERE { ?lexeme dct:language wd:Q5287 ; '
    'wikibase:lemma ?lemma ; ontolex:sense ?sense . ?sense wdt:P5137 ?item . '
    'OPTIONAL { ?item rdfs:label ?es FILTER(LANG(?es) = "es") } }')
SOURCES = {  # name: (url, file, sha256 as fetched 2026-10-23)
    "omw": (OMW_URL, RESEARCH / "omw-1.4.tar.xz",
            "28be99fcb038171403cd057f62e7213d1ea4ea1cf00ab2fb99af190f39bb1d99"),
    "kaikki-en": (KAIKKI_EN_URL, RESEARCH / "kaikki-en-Japanese.jsonl.gz",
                  "28be495b8a72e6a7882a6b9a7fc402445a1b68d22546c82c7cb5a98b913888b0"),
    "kaikki-es": (KAIKKI_ES_URL, RESEARCH / "kaikki-eswiktionary-raw.jsonl.gz",
                  "4f253676b74773020379f6136c39af681993b3db52fe16861328e2d70f57adc2"),
    "wikidata": (WIKIDATA_SPARQL + "?" + urllib.parse.urlencode({"query": WIKIDATA_QUERY}),
                 RESEARCH / "wikidata-ja-lexemes.tsv",
                 "bf6557ebf602a4561057bc8ef130e6b30d162d7c9093a9e9ff86542327cf8dac"),
}
METHODS = ("A", "B1", "B2", "C", "D", "E")
METHOD_NAME = {
    "A": "A. Wordnet (wn-ja → PWN → MCR)",
    "B1": "B1. Spanish Wiktionary",
    "B2": "B2. English Wiktionary → FreeDict",
    "C": "C. JMdict_e → FreeDict",
    "D": "D. Wikidata lexemes",
    "E": "E. Composition (kanji meanings)",
}


def fetch(force: bool = False) -> None:
    RESEARCH.mkdir(parents=True, exist_ok=True)
    for name, (url, dest, want) in SOURCES.items():
        if dest.exists() and not force:
            print(f"have  {dest}", file=sys.stderr)
            continue
        print(f"get   {url[:100]}", file=sys.stderr)
        headers = {"User-Agent": "tamagotcho-pack-research"}
        if name == "wikidata":
            headers["Accept"] = "text/tab-separated-values"
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers),
                                    timeout=900) as r:
            data = r.read()
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            raise SystemExit(f"{name}: SHA256 {got}, pinned {want}: the source changed upstream; "
                             "review it and update the pin in this script")
        tmp = dest.with_suffix(dest.suffix + ".part")
        tmp.write_bytes(data)
        tmp.replace(dest)


def check_pins() -> list[str]:
    bad = []
    for name, (_, dest, want) in SOURCES.items():
        if not dest.exists():
            bad.append(f"missing {dest}: run with --fetch")
        elif hashlib.sha256(dest.read_bytes()).hexdigest() != want:
            bad.append(f"{dest}: SHA256 differs from the pin")
    return bad


# --- C. FreeDict, in order ---------------------------------------------------------

@dataclass
class FdEntry:
    pos: str
    senses: list[list[str]]   # Spanish quotes per sense, in order


def load_freedict_ordered(path: Path) -> dict[str, list[FdEntry]]:
    """FreeDict eng-spa as headword -> entries (one per part of speech), each
    with its senses' Spanish quotes in the dictionary's order (the builder's
    meanings.load_freedict keeps sets, normalised, for matching)."""
    out: dict[str, list[FdEntry]] = defaultdict(list)
    with tarfile.open(path, "r:xz") as tar:
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


def split_gloss(gloss: str) -> list[str]:
    """English Wiktionary's "novel, story, fiction" / "to look for; to seek"
    as separate glosses (commas inside parentheses stay)."""
    parts = re.split(r"[;,]\s*(?![^(]*\))", gloss)
    return [p.strip() for p in parts if p.strip()]


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


# --- A. Open Multilingual Wordnet ----------------------------------------------------

@dataclass
class Wordnets:
    ja: dict[str, list[str]]          # Japanese lemma -> ILIs (wn-ja order)
    en_members: dict[str, list[str]]  # ILI -> English lemmas (synset order)
    en_rank: dict[tuple[str, str], int]  # (English lemma, ILI) -> sense rank (0 = most frequent)
    es_members: dict[str, list[str]]  # ILI -> Spanish lemmas (synset order)


def _read_lmf(f) -> tuple[dict[str, list[str]], dict[str, list[str]], dict[str, str]]:
    """A WN-LMF lexicon: lemma -> synset ids (in entry order), synset id ->
    member lemmas (in the synset's order), synset id -> ILI."""
    lemma_syn: dict[str, list[str]] = defaultdict(list)
    sense_lemma: dict[str, str] = {}
    members: dict[str, list[str]] = {}
    ili: dict[str, str] = {}
    lemma = ""
    for _, el in ET.iterparse(f, events=("end",)):
        tag = el.tag
        if tag == "Lemma":
            lemma = el.get("writtenForm", "")
        elif tag == "Sense":
            sense_lemma[el.get("id", "")] = lemma
            lemma_syn[lemma].append(el.get("synset", ""))
        elif tag == "LexicalEntry":
            el.clear()
        elif tag == "Synset":
            sid = el.get("id", "")
            ili[sid] = el.get("ili", "")
            members[sid] = [sense_lemma.get(m, "") for m in (el.get("members") or "").split()]
            el.clear()
    return lemma_syn, members, ili


def load_wordnets(path: Path) -> Wordnets:
    parts = {}
    with tarfile.open(path, "r:xz") as tar:
        for m in tar:
            for lang in ("ja", "en", "es"):
                if m.name.endswith(f"omw-{lang}/omw-{lang}.xml"):
                    parts[lang] = _read_lmf(tar.extractfile(m))
    ja_l, _, ja_ili = parts["ja"]
    en_l, en_m, en_ili = parts["en"]
    _, es_m, es_ili = parts["es"]
    ja = {lem.replace("+", ""): [ja_ili[s] for s in syns if ja_ili.get(s)]
          for lem, syns in ja_l.items()}
    en_rank = {}
    for lem, syns in en_l.items():
        for i, s in enumerate(syns):
            en_rank.setdefault((lem.replace("_", " ").lower(), en_ili[s]), i)
    return Wordnets(
        ja,
        {en_ili[s]: [x.replace("_", " ") for x in v] for s, v in en_m.items() if en_ili.get(s)},
        en_rank,
        {es_ili[s]: [x.replace("_", " ") for x in v] for s, v in es_m.items() if es_ili.get(s)})


def wordnet_gloss(text: str, english: list[str], wn: Wordnets) -> tuple[str | None, str]:
    """(Spanish, how): the word's synset whose English lemmas contain the
    earliest of its JMdict_e glosses (most frequent English sense first), and
    that synset's first Spanish lemma. how: "no synset", "no English match",
    "no Spanish lemma" or "ok"."""
    ilis = wn.ja.get(text)
    if not ilis:
        return None, "no synset"
    keys = [k.lower() for g in english for k in en_keys(g)]
    best = None
    for ili in ilis:
        members = {m.lower() for m in wn.en_members.get(ili, [])}
        for gi, k in enumerate(keys):
            if k in members:
                cand = (gi, wn.en_rank.get((k, ili), 99), ili)
                if best is None or cand < best:
                    best = cand
                break
    if best is None:
        return None, "no English match"
    es = wn.es_members.get(best[2])
    return (es[0], "ok") if es else (None, "no Spanish lemma")


# --- B. Wiktionary (kaikki.org) ------------------------------------------------------

def ruby_reading(form: str, ruby: list) -> str | None:
    """The reading of a written form from its ruby ([[base, reading], ...]),
    kana outside the ruby read as themselves."""
    out, i = [], 0
    for base, rt in ruby:
        j = form.find(base, i)
        if j < 0:
            return None
        out.append(form[i:j])
        out.append(rt)
        i = j + len(base)
    out.append(form[i:])
    return to_hiragana("".join(out))


def load_en_wiktionary(path: Path, wanted: set[str]) -> dict[str, list[tuple[set[str], str]]]:
    """English Wiktionary's Japanese entries for the wanted words: word ->
    [(its readings, its first English gloss)], skipping senses that only
    point to another form ("alternative spelling of ...")."""
    out: dict[str, list[tuple[set[str], str]]] = defaultdict(list)
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            e = json.loads(line)
            w = e.get("word", "")
            if w not in wanted:
                continue
            readings = set()
            for fm in e.get("forms", []):
                if fm.get("ruby"):
                    r = ruby_reading(fm.get("form", ""), fm["ruby"])
                    if r:
                        readings.add(r)
                elif "hiragana" in fm.get("tags", []):
                    readings.add(to_hiragana(fm.get("form", "")))
            gloss = next((s["glosses"][0] for s in e.get("senses", [])
                          if s.get("glosses") and not s.get("form_of") and not s.get("alt_of")),
                         None)
            if gloss:
                out[w].append((readings, gloss))
    return out


def load_es_wiktionary(path: Path, wanted: set[str]) -> dict[str, list[tuple[set[str], str]]]:
    """Spanish Wiktionary's Japanese entries (lang_code ja) for the wanted
    words: word -> [(its hiragana readings, its first Spanish definition)]."""
    out: dict[str, list[tuple[set[str], str]]] = defaultdict(list)
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            if '"lang_code": "ja"' not in line:
                continue
            e = json.loads(line)
            w = e.get("word", "")
            if w not in wanted:
                continue
            readings = {to_hiragana(s["alternative"]) for s in e.get("sounds", [])
                        if s.get("note") == "hiragana" and s.get("alternative")}
            gloss = next((s["glosses"][0] for s in e.get("senses", []) if s.get("glosses")), None)
            if gloss:
                out[w].append((readings, gloss))
    return out


def wiktionary_gloss(entries: list[tuple[set[str], str]] | None, reading: str) -> str | None:
    """The gloss of the entry with the word's reading, else of an entry that
    lists no reading."""
    if not entries:
        return None
    for readings, g in entries:
        if reading in readings:
            return g
    return next((g for readings, g in entries if not readings), None)


def es_definition(g: str) -> str:
    """"Comer." -> "Comer": a Spanish Wiktionary definition as a gloss."""
    return g.strip().rstrip(".").strip()


# --- D. Wikidata lexemes ---------------------------------------------------------------

def load_wikidata(path: Path) -> dict[str, list[tuple[set[str], str]]]:
    """Japanese lexemes: kanji lemma -> [(hiragana lemmas, the Spanish label of
    the item of the lexeme's first sense that has one)]."""
    lemmas: dict[str, dict[str, set[str]]] = defaultdict(lambda: defaultdict(set))
    senses: dict[str, list[tuple[int, str]]] = defaultdict(list)
    with path.open(encoding="utf-8") as f:
        rd = csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)
        next(rd, None)
        for row in rd:
            if len(row) < 5:
                continue
            lex = row[0].rsplit("/", 1)[-1].rstrip(">")
            m = re.match(r'^"(.*)"@([\w-]+)$', row[1])
            if m:
                lemmas[lex][m.group(2)].add(m.group(1))
            s = re.search(r"-S(\d+)>$", row[2])
            e = re.match(r'^"(.*)"@es$', row[4])
            if s and e:
                senses[lex].append((int(s.group(1)), e.group(1)))
    out: dict[str, list[tuple[set[str], str]]] = defaultdict(list)
    for lex, by_lang in lemmas.items():
        if not senses.get(lex):
            continue
        es = min(senses[lex])[1]
        hira = {to_hiragana(x) for x in by_lang.get("ja-hira", set()) | by_lang.get("ja-kana", set())}
        for t in by_lang.get("ja", set()) | by_lang.get("ja-hani", set()):
            out[t].append((hira, es))
    return out


# --- E. Composition ----------------------------------------------------------------------

def pack_meanings(packs: dict[tuple[str, str], Path]) -> tuple[dict[int, str], dict[str, set[int]]]:
    """The first Spanish meaning of every card of the published Spanish packs
    (marked "(en)" when the card falls back to English), and each deck's
    codepoints."""
    first: dict[int, str] = {}
    deck_cps: dict[str, set[int]] = {}
    for (d, lang), f in packs.items():
        if lang != "es":
            continue
        p = read_pack(f.read_bytes())
        deck_cps[d] = {c.codepoint for c in p.cards}
        for c in p.cards:
            m = c.meanings.split(MEANING_SEP)[0]
            first.setdefault(c.codepoint, m + (" (en)" if c.flags & 1 else ""))
    return first, deck_cps


def composition(text: str, first: dict[int, str]) -> str | None:
    parts = [first.get(ord(ch)) for ch in text if is_kanji(ch) and ch != "々"]
    return " + ".join(parts) if parts and all(parts) else None


# --- report ------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true", help="download the sources into cache/research/")
    ap.add_argument("--force", action="store_true", help="with --fetch: download again")
    ap.add_argument("--packs", required=True, help="directory with the published decks")
    ap.add_argument("--max-gloss", type=int, default=24)
    ap.add_argument("--sample", type=int, default=30)
    ap.add_argument("--seed", type=int, default=9)
    ap.add_argument("--tsv", help="write every word with each source's gloss to this file")
    ap.add_argument("--picks-cache", help="(development) pickle the picked words here and reuse them")
    a = ap.parse_args()
    if a.fetch:
        fetch(a.force)
    bad = check_pins()
    if bad:
        print("\n".join(bad), file=sys.stderr)
        return 2
    mx = a.max_gloss

    if a.picks_cache and Path(a.picks_cache).exists():
        s: Picks = pickle.loads(Path(a.picks_cache).read_bytes())
    else:
        s = compute_picks(a.packs)
        s.full = s.xml = b""
        if a.picks_cache:
            Path(a.picks_cache).write_bytes(pickle.dumps(s))
    every: list[Row] = [r for cp in s.all_cps for r in s.rows[cp]]
    words = {(r.word.text, r.word.reading): r for r in every}
    texts = {t for t, _ in words}

    fd = load_freedict_ordered(paths.FREEDICT_ENG_SPA)
    wn = load_wordnets(SOURCES["omw"][1])
    en_wk = load_en_wiktionary(SOURCES["kaikki-en"][1], texts)
    es_wk = load_es_wiktionary(SOURCES["kaikki-es"][1], texts)
    wd = load_wikidata(SOURCES["wikidata"][1])
    first, deck_cps = pack_meanings(s.packs)

    # Each source's Spanish gloss per (word, reading).
    out: dict[tuple[str, str], dict[str, str | None]] = {}
    a_how: Counter = Counter()
    b2_en = 0
    for key, r in words.items():
        text, reading = key
        eng = r.gl.eng if r.gl else []
        pos = r.gl.pos if r.gl else ()
        g: dict[str, str | None] = {}
        g["A"], how = wordnet_gloss(text, eng, wn)
        a_how[how] += 1
        g["B1"] = wiktionary_gloss(es_wk.get(text), reading)
        if g["B1"]:
            g["B1"] = es_definition(g["B1"])
        wen = wiktionary_gloss(en_wk.get(text), reading)
        b2_en += wen is not None
        t = translate(split_gloss(wen), pos, fd) if wen else None
        g["B2"] = t[0] if t else None
        t = translate(eng, pos, fd)
        g["C"] = t[0] if t else None
        g["D"] = wiktionary_gloss(wd.get(text), reading)
        g["E"] = composition(text, first)
        g["en"] = eng[0] if eng else None
        g["J"] = r.es()
        out[key] = g

    print("## Sources\n")
    for name, (url, dest, sha) in SOURCES.items():
        print(f"- {name}: `{dest.name}`, {dest.stat().st_size:,} bytes, SHA256 `{sha}`")
    print(f"- FreeDict eng-spa: `{paths.FREEDICT_ENG_SPA.name}` (pinned by `fetch --meanings`)")
    print(f"\n{len(words):,} distinct picked words (N5-N1 ∪ Jōyō). Wordnet (A): "
          + ", ".join(f"{k} {v:,}" for k, v in a_how.most_common()) + f". English Wiktionary "
          f"has a gloss for {b2_en:,} of them (B2 before translation).\n")

    print(f"## Coverage per deck (words with a Spanish gloss / with one ≤ {mx} characters)\n")
    print("| source | " + " | ".join(DECK_TITLE[d] for d in DECKS) + " |")
    print("|---|" + "---:|" * len(DECKS))
    for m in ("JMdict",) + METHODS:
        cells = []
        for d in DECKS:
            ks = [(r.word.text, r.word.reading) for cp in s.decks[d] for r in s.rows[cp]]
            if m == "JMdict":
                vals = [words[k].es() for k in ks]
            else:
                vals = [out[k][m] for k in ks]
            n_any = sum(1 for v in vals if v)
            n_short = sum(1 for v in vals if v and len(v) <= mx)
            cells.append(f"{pct(n_any, len(ks))} / {pct(n_short, len(ks))}")
        label = "JMdict Spanish (not licensed, for reference)" if m == "JMdict" else METHOD_NAME[m]
        print(f"| {label} | " + " | ".join(cells) + " |")

    # C confirmed by an independent source (A or D; B2 shares FreeDict with C).
    def same(x: str | None, y: str | None) -> bool:
        return bool(x and y) and x.strip().lower() == y.strip().lower()

    print("\nC agrees with A or D (the same Spanish word, ignoring case): "
          + ", ".join(f"{DECK_TITLE[d]} " + pct(
              sum(1 for k in ks if same(out[k]["C"], out[k]["A"]) or same(out[k]["C"], out[k]["D"])),
              len(ks))
              for d in DECKS
              for ks in [[(r.word.text, r.word.reading) for cp in s.decks[d] for r in s.rows[cp]]])
          + ".")

    # E: what the pack lacks.
    print("\n## E. Composition: meanings outside the deck\n")
    print("| deck | words | all kanji in the deck | other kanji needed | their meaning strings |")
    print("|---|---:|---:|---:|---:|")
    for d in DECKS:
        ks = [r.word.text for cp in s.decks[d] for r in s.rows[cp]]
        own = deck_cps.get(d, set())
        other = {ord(ch) for t in ks for ch in t if is_kanji(ch) and ch != "々"} - own
        inside = sum(1 for t in ks if all(ord(ch) in own for ch in t if is_kanji(ch) and ch != "々"))
        size = sum(len(first.get(cp, "").encode()) + 1 + 8 for cp in other)
        print(f"| {DECK_TITLE[d]} | {len(ks):,} | {inside:,} ({pct(inside, len(ks))}) | "
              f"{len(other):,} | {size / 1024:.1f} KB |")

    # F: words to review.
    print("\n## F. Words a project-authored table would review\n")
    seen: set[tuple[str, str]] = set()
    print("| deck | words | new (not in an easier deck) | cumulative |")
    print("|---|---:|---:|---:|")
    for d in ("n5", "n4", "n3", "n2", "n1", "joyo"):
        ks = {(r.word.text, r.word.reading) for cp in s.decks[d] for r in s.rows[cp]}
        new = ks - seen
        seen |= ks
        print(f"| {DECK_TITLE[d]} | {len(ks):,} | {len(new):,} | {len(seen):,} |")

    # Spot check: the same 30 words for every source.
    pool = sorted({(r.word.text, r.word.reading) for d in ("n5", "n4", "n3")
                   for cp in s.decks[d] for r in s.rows[cp]})
    sample = sorted(random.Random(a.seed).sample(pool, min(a.sample, len(pool))))
    print(f"\n## Spot check ({len(sample)} random N5-N3 words, seed {a.seed})\n")
    print("| # | word | reading | JMdict_e | JMdict es (ref.) | " + " | ".join(METHODS) + " |")
    print("|---:|---|---|---|---|" + "---|" * len(METHODS))
    for i, k in enumerate(sample, 1):
        g = out[k]
        print(f"| {i} | {k[0]} | {k[1]} | {g['en'] or '—'} | {g['J'] or '—'} | "
              + " | ".join(g[m] or "—" for m in METHODS) + " |")

    # F draft sample: 30 N5 words with the English and C's suggestion.
    n5 = sorted({(r.word.text, r.word.reading) for cp in s.decks["n5"] for r in s.rows[cp]})
    fs = sorted(random.Random(a.seed).sample(n5, min(a.sample, len(n5))))
    print(f"\n## F. Sample for a draft ({len(fs)} random N5 words, seed {a.seed})\n")
    print("| # | word | reading | JMdict_e (first sense) | C suggestion |")
    print("|---:|---|---|---|---|")
    for i, k in enumerate(fs, 1):
        r = words[k]
        print(f"| {i} | {k[0]} | {k[1]} | {'; '.join(r.gl.eng[:3]) if r.gl else '—'} | "
              f"{out[k]['C'] or '—'} |")

    if a.tsv:
        p = Path(a.tsv)
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("w", encoding="utf-8") as f:
            f.write("word\treading\tdecks\tenglish\t" + "\t".join(METHODS) + "\n")
            in_deck = defaultdict(list)
            for d in DECKS:
                for cp in s.decks[d]:
                    for r in s.rows[cp]:
                        in_deck[(r.word.text, r.word.reading)].append(d)
            for k, g in sorted(out.items()):
                f.write(f"{k[0]}\t{k[1]}\t{','.join(dict.fromkeys(in_deck[k]))}\t{g['en'] or ''}\t"
                        + "\t".join(g[m] or "" for m in METHODS) + "\n")
        print(f"\n(wrote {p})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
