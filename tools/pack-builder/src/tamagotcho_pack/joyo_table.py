"""The official Jōyō kanji table (常用漢字表, Cabinet notice No. 2 of 2010):
which readings it approves for each kanji, and which of those it marks as
special or of very narrow use.

The table's 音訓 column lists every approved reading (on'yomi in katakana,
kun'yomi in hiragana, without an okurigana split). Readings "indented by one
character" (１字下げ) are, in the notice's words, 特別なものか，又は用法のごく狭いもの
(special, or of very narrow use: 行 アン, 上 ショウ/うわ, 弟 ダイ/デ). The PDF from
the Agency for Cultural Affairs (`fetch --readings`) is read with pypdf: its
text has one table row per line, cells separated by tabs, and an indented
reading starts with an ideographic space (U+3000). The parse finds the 2,136
kanji and 4,388 readings the notice lists, 178 of them indented (checked
against a parse of the same PDF by glyph position with poppler's `pdftotext
-bbox`). Used by `build --common-readings` (readings.py) and
`scripts/common_readings_report.py` (docs/common-readings.md).
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

_KANA = re.compile("^　?[ぁ-ゖァ-ヶー]+$")
TABLE_KANJI = 2136
TABLE_READINGS = 4388
LAST_KANJI = "腕"  # the table is in reading order; 腕 (ワン) is its last entry


@dataclass(frozen=True)
class JoyoReading:
    reading: str     # as printed: katakana on'yomi, hiragana kun'yomi, no okurigana mark
    indented: bool   # 1字下げ: special, or of very narrow use


def _is_kanji(c: str) -> bool:
    return "一" <= c <= "鿿" or ord(c) >= 0x20000


def _variant(cell: str) -> bool:
    """Cells beside the kanji that are not readings: the old form （亞）, an
    allowed form ［遡］, or an old-form kanji on its own (弁: 辨 瓣 辯)."""
    return cell[0] in "（［" or (len(cell) == 1 and _is_kanji(cell))


def parse_pages(pages: Iterable[str]) -> dict[str, list[JoyoReading]]:
    """Parse the text of the PDF's pages (pypdf `extract_text()`) into kanji ->
    readings, in the table's order. Pages before the first entry (亜) are the
    preface; parsing stops with the page of the last entry (腕), after which
    the appendix (付表) starts. A reading continued on the next page stays with
    the last kanji."""
    table: dict[str, list[JoyoReading]] = {}
    cur: str | None = None
    started = False
    for text in pages:
        if not started:
            if not re.search(r"(^|\t) ?亜\t", text, re.MULTILINE):
                continue
            started = True
        for line in text.split("\n"):
            fields = line.split("\t")
            if len(fields) < 3:
                continue  # page headers, footers, notes
            cells = [c for c in (f.strip(" ") for f in fields) if c.strip()]
            if not cells:
                continue
            head = len(cells[0]) == 1 and _is_kanji(cells[0])
            i = 1 if head else 0
            while i < len(cells) and _variant(cells[i]):
                i += 1
            if i >= len(cells) or not _KANA.match(cells[i]):
                continue  # an old-form line, a blank row or a note
            if head:
                cur = cells[0]
                table.setdefault(cur, [])
            if cur is not None:
                cell = cells[i]
                table[cur].append(JoyoReading(cell.lstrip("　"), cell.startswith("　")))
        if cur == LAST_KANJI:
            break
    return table


def load_pdf(pdf: str | Path) -> dict[str, list[JoyoReading]]:
    """Read the table PDF (joyokanjihyo_20101130.pdf) and parse it; raises
    ValueError if the result does not have the notice's 2,136 kanji and
    4,388 readings (a different file, or a pypdf change)."""
    from pypdf import PdfReader

    table = parse_pages(page.extract_text() for page in PdfReader(str(pdf)).pages)
    n = sum(len(v) for v in table.values())
    if len(table) != TABLE_KANJI or n != TABLE_READINGS:
        raise ValueError(f"{pdf}: parsed {len(table)} kanji / {n} readings, expected "
                         f"{TABLE_KANJI} / {TABLE_READINGS}")
    return table


def plain(reading: str) -> str:
    """A KANJIDIC2 reading as the table prints it: no okurigana dot, no hyphen."""
    return reading.replace(".", "").replace("-", "")


def classify(reading: str, entry: list[JoyoReading] | None) -> str:
    """'joyo' (approved), 'special' (indented) or 'other' (not in the table)
    for a KANJIDIC2 reading of a kanji whose table entry is `entry`.

    The table sometimes prints only the stem and shows the okurigana in its
    examples (異: こと, 異なる; 幾: いく, 幾つ), so a reading with okurigana also
    matches a table reading equal to its stem."""
    if entry is None:
        return "other"
    p = plain(reading)
    stem = reading.partition(".")[0] if "." in reading else None
    for r in entry:
        if r.reading == p or (stem is not None and r.reading == stem):
            return "special" if r.indented else "joyo"
    return "other"
