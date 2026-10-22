"""KANJIDIC2 parsing and the card model."""

from __future__ import annotations

import gzip
import json
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

MAX_MEANINGS = 3

# Fallback when kanji.json (new JLPT levels) is unavailable. KANJIDIC2 only has
# the pre-2010 four-level scale; old level 2 spans new N3 and N2, so it is
# mapped to N3 (lossy). See docs/data-sources.md.
OLD_TO_NEW_JLPT = {4: 5, 3: 4, 2: 3, 1: 1}

# The Jōyō kanji (2010 list, 2,136 characters) as KANJIDIC2 school grades:
# 1-6 are the kyōiku kanji taught in elementary school (1,026) and 8 the rest of
# the list, taught in secondary school (1,110). Grades 9 and 10 are jinmeiyō
# (name) kanji, not Jōyō.
JOYO_GRADES = frozenset({1, 2, 3, 4, 5, 6, 8})

# Card orders for make_cards(). The order becomes each card's `level` hint, the
# order in which the device introduces new cards (docs/srs.md, "New cards").
#   jlpt:  N5 first .. N1, then kanji with no JLPT level; within a level by
#          school grade, then newspaper frequency.
#   grade: school order, grade 1 .. 6, then 8 (secondary school); within a grade
#          by newspaper frequency, kanji with no frequency rank last (fewer
#          strokes first). The Jōyō deck uses it.
#   file:  the input order (the --kanji-file lines), unchanged.
ORDERS = ("jlpt", "grade", "file")


@dataclass
class Card:
    codepoint: int
    meanings: list[str]  # already in the final language, max MAX_MEANINGS
    onyomi: list[str]
    kunyomi: list[str]
    jlpt: int = 0
    grade: int = 0
    strokes: int = 0
    freq: int = 0
    # Classical KangXi radical numbers (1..214) from KANJIDIC2, used only to
    # rank meaning distractors; empty when the source has no radical data.
    radicals: list[int] = field(default_factory=list)
    fallback_en: bool = False
    level: int = 0
    distractors: list[int] = field(default_factory=list)
    distractors_reading: list[int] = field(default_factory=list)
    # Every reading the kanji has, when `onyomi`/`kunyomi` hold fewer (a kind
    # emptied by `--common-readings`): reading distractors are kept away from
    # these too. Empty = the same as `onyomi`/`kunyomi`. Not stored in the pack.
    valid_onyomi: list[str] = field(default_factory=list)
    valid_kunyomi: list[str] = field(default_factory=list)
    # Every meaning the kanji has in the card's language, before the builder's
    # caps (`--meaning-order`, which can also change the ones kept): meaning
    # distractors are kept away from these too. Empty = `meanings`. Not stored.
    valid_meanings: list[str] = field(default_factory=list)

    @property
    def char(self) -> str:
        return chr(self.codepoint)


@dataclass
class RawKanji:
    codepoint: int
    meanings: dict[str, list[str]]
    onyomi: list[str]
    kunyomi: list[str]
    jlpt_old: int
    grade: int
    strokes: int
    freq: int
    radicals: list[int] = field(default_factory=list)
    # Kun'yomi (as cleaned, in `kunyomi`) that KANJIDIC2 only lists as a prefix
    # or suffix (`うわ-`, `-び`): bound forms, used only inside compounds. Only
    # `--common-readings` looks at it (readings.py); the pack does not store it.
    kun_bound: frozenset[str] = frozenset()
    # Set by readings.reorder_raw when it empties a kind: the readings before.
    all_onyomi: list[str] | None = None
    all_kunyomi: list[str] | None = None


def clean_kun(r: str) -> str:
    """Drop the '-' prefix/suffix markers, keep the '.' okurigana marker."""
    return r.replace("-", "")


def _dedupe(items: list[str]) -> list[str]:
    """Distinct readings in order, without empty ones (or ones that are only
    okurigana dots): a pack's reading list never has an empty item."""
    return list(dict.fromkeys(i for i in items if i.replace(".", "")))


def parse_kanjidic(source: str | Path | bytes) -> list[RawKanji]:
    """Parse KANJIDIC2 from a path (.xml or .xml.gz) or raw XML bytes."""
    if isinstance(source, bytes):
        data = source
    else:
        p = Path(source)
        data = gzip.decompress(p.read_bytes()) if p.suffix == ".gz" else p.read_bytes()
    root = ET.fromstring(data)  # KANJIDIC2 DTD is internal-subset only; stdlib is fine
    out: list[RawKanji] = []
    for ch in root.iter("character"):
        cp = None
        for c in ch.iter("cp_value"):
            if c.get("cp_type") == "ucs" and c.text and c.text.strip():
                cp = int(c.text.strip(), 16)
        if cp is None:
            continue
        # Classical KangXi radical numbers only (rad_type="classical"); the
        # Nelson reclassifications are another classification and not used.
        # Real entries always carry a <radical> block, but treat a missing or
        # empty one as "no radical data" rather than failing.
        radicals: list[int] = []
        for rv in ch.iter("rad_value"):
            text = rv.text.strip() if rv.text else ""
            if rv.get("rad_type") == "classical" and text:
                radicals.append(int(text))

        meanings: dict[str, list[str]] = {}
        on: list[str] = []
        kun: list[str] = []
        free: set[str] = set()
        for rm in ch.iter("reading_meaning"):
            for r in rm.iter("reading"):
                text = (r.text or "").strip()
                if r.get("r_type") == "ja_on":
                    on.append(text)
                elif r.get("r_type") == "ja_kun":
                    kun.append(clean_kun(text))
                    if "-" not in text:
                        free.add(text)
            for m in rm.iter("meaning"):
                meanings.setdefault(m.get("m_lang", "en"), []).append((m.text or "").strip())

        def num(path: str) -> int:
            e = ch.find(path)
            text = e.text.strip() if e is not None and e.text else ""
            return int(text) if text else 0

        out.append(
            RawKanji(
                codepoint=cp,
                meanings=meanings,
                onyomi=_dedupe(on),
                kunyomi=_dedupe(kun),
                jlpt_old=num("misc/jlpt"),
                grade=num("misc/grade"),
                strokes=num("misc/stroke_count"),
                freq=num("misc/freq"),
                radicals=radicals,
                kun_bound=frozenset(k for k in kun if k and k not in free),
            )
        )
    return out


def parse_kanji_file(text: str, known: set[int] | None = None) -> tuple[list[int], list[str]]:
    """Parse a `--kanji-file` list: one kanji per line, in file order.

    Blank lines and lines whose first non-space character is `#` are ignored,
    as is an optional UTF-8 BOM and whitespace around a line. Duplicates are
    removed, keeping the first occurrence. A line that does not hold exactly
    one Unicode character raises ValueError with its line number.

    When `known` is given, codepoints absent from it are skipped and reported
    in the returned warnings (the kanji KANJIDIC2 does not have).
    """
    if text.startswith("\ufeff"):
        text = text[1:]
    cps: list[int] = []
    first_line: dict[int, int] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if len(line) != 1:
            raise ValueError(f"line {lineno}: expected one kanji character, got {line!r} "
                             f"({len(line)} characters)")
        cp = ord(line)
        if cp not in first_line:
            first_line[cp] = lineno
            cps.append(cp)
    warnings: list[str] = []
    if known is not None:
        kept = []
        for cp in cps:
            if cp in known:
                kept.append(cp)
            else:
                warnings.append(f"line {first_line[cp]}: {chr(cp)!r} (U+{cp:04X}) is not in "
                                "KANJIDIC2; skipped")
        cps = kept
    return cps, warnings


def load_new_jlpt(path: str | Path) -> dict[int, int]:
    """Map codepoint -> new JLPT level (5..1) from davidluzgouveia/kanji-data."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return {
        ord(k): v["jlpt_new"]
        for k, v in data.items()
        if len(k) == 1 and v.get("jlpt_new")
    }


def make_cards(
    raw: list[RawKanji],
    lang: str,
    jlpt_levels: set[int] | None = None,
    grades: set[int] | None = None,
    new_jlpt: dict[int, int] | None = None,
    order: str = "jlpt",
    keep_all_meanings: bool = False,
) -> list[Card]:
    """Select and convert kanji to cards, ordered easiest-first (level hint).

    `order` is one of ORDERS (see there); it decides each card's `level`.
    `order="file"` keeps the input order, for `--kanji-file`.
    `keep_all_meanings` (`--meaning-order`) records each card's uncapped list
    in `Card.valid_meanings`.
    """
    if order not in ORDERS:
        raise ValueError(f"unknown card order {order!r} (expected one of {', '.join(ORDERS)})")
    cards: list[Card] = []
    for k in raw:
        jlpt = new_jlpt.get(k.codepoint, 0) if new_jlpt is not None else OLD_TO_NEW_JLPT.get(k.jlpt_old, 0)
        if jlpt_levels and jlpt not in jlpt_levels:
            continue
        if grades and k.grade not in grades:
            continue
        en = k.meanings.get("en", [])
        ms = list(en) if lang == "en" else list(k.meanings.get(lang, []))
        fallback = False
        if not ms:
            ms = list(en)
            fallback = lang != "en"
        every = list(ms)
        # The non-English lists can carry extra, derived senses the canonical
        # English list does not (母: "madre", "base", "cimientos"); keep at most
        # as many as English has.
        if lang != "en" and en:
            ms = ms[: len(en)]
        if not ms and not (k.onyomi or k.kunyomi):
            continue
        cards.append(
            Card(
                codepoint=k.codepoint,
                meanings=ms[:MAX_MEANINGS],
                onyomi=k.onyomi,
                kunyomi=k.kunyomi,
                jlpt=jlpt,
                grade=min(k.grade, 255),
                strokes=min(k.strokes, 255),
                freq=min(k.freq, 65535),
                radicals=list(k.radicals),
                fallback_en=fallback,
                valid_onyomi=list(k.all_onyomi or []),
                valid_kunyomi=list(k.all_kunyomi or []),
                valid_meanings=every if keep_all_meanings else [],
            )
        )
    if order == "grade":
        # school order: lower grade, then frequency, then fewer strokes, then codepoint
        cards.sort(key=lambda c: (c.grade or 99, c.freq or 9999, c.strokes or 99, c.codepoint))
    elif order == "jlpt":
        # easiest first: higher N number, then lower grade, then frequency, then codepoint
        cards.sort(key=lambda c: (-c.jlpt, c.grade or 99, c.freq or 9999, c.codepoint))
    # order == "file": the input order is the introduction order
    for i, c in enumerate(cards):
        c.level = min(i, 65535)
    return cards
