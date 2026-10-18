"""Write a small, line-oriented catalog of built packs (see README.md).

The roadmap wants prebuilt packs hosted with "a small catalog the device can
read". This module produces that file, catalog format version 2. The format is
deliberately trivial to parse on an ESP32 without a JSON library: one record
per line, TAB-separated fields, `#` comments and blank lines to be skipped.

    #tangocho-catalog<TAB>2
    # name<TAB>title<TAB>lang<TAB>cards<TAB>bytes<TAB>sha256<TAB>url
    n5-en<TAB>JLPT N5<TAB>en<TAB>79<TAB>703123<TAB>ab12...<TAB>https://.../n5-en.tgc

- The first line is a version tag (`#` comment that also carries the format
  version), so a reader can detect the format and refuse any other version
  (the firmware reads only v2; v1 had no `title` column).
- The second line documents the columns; readers skip every `#` line.
- `name` is the file's basename without `.tgc`, i.e. the name the device should
  save the downloaded file as (`<name>.tgc`).
- `title` is the pack header's deck name (UTF-8, at most TITLE_MAX_BYTES bytes,
  no TAB or other control character; may be empty, then readers show `name`).
- `lang` is the pack's meaning language, `cards` its card count and `bytes` its
  exact file size.
- `sha256` is the lowercase hex digest of the whole file; the device checks it
  (and `bytes`) before it keeps a download (firmware/src/catalog/, Get decks).
- `url` is either absolute (`--base-url`) or the bare file name, meaning
  "relative to the URL this catalog was fetched from".

The file is published as `catalog-v2.txt` (FILE_NAME): named after the catalog
format version, whereas the decks release tag `decks-v<N>` names the pack
format version.

Records are sorted by file name, so the output is deterministic for a given
directory; `--order` lists names to put first, in that order (the device lists
decks in catalog order, so a release puts N5 before N1). The `catalog` command
refuses to write a catalog for an empty directory, a file that is not a valid
pack, or a pack whose deck name is not a valid title, so a broken build cannot
silently enter a release.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .pack import read_pack

VERSION = 2
# The published names: after the catalog format version, not the pack format.
FILE_NAME = f"catalog-v{VERSION}.txt"
JSON_FILE_NAME = f"catalog-v{VERSION}.json"
# `#`-prefixed lines are comments for any reader. The first line carries the
# version so a reader can refuse an unknown format before parsing records.
HEADER = f"#tangocho-catalog\t{VERSION}"
COLUMNS = "# name\ttitle\tlang\tcards\tbytes\tsha256\turl"
# Byte cap of the title column (UTF-8); the firmware's kCatalogMaxTitleLen.
TITLE_MAX_BYTES = 64


@dataclass(frozen=True)
class Record:
    name: str
    title: str
    lang: str
    cards: int
    size: int
    sha256: str
    url: str

    def line(self) -> str:
        return "\t".join(
            [self.name, self.title, self.lang, str(self.cards), str(self.size), self.sha256,
             self.url]
        )

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "title": self.title,
            "lang": self.lang,
            "cards": self.cards,
            "bytes": self.size,
            "sha256": self.sha256,
            "url": self.url,
        }


def title_problem(title: str) -> str | None:
    """Why *title* cannot be a catalog title, or None when it can."""
    size = len(title.encode("utf-8"))
    if size > TITLE_MAX_BYTES:
        return f"{size} bytes, over the catalog's {TITLE_MAX_BYTES}-byte title cap"
    for ch in title:
        if ord(ch) < 0x20 or ord(ch) == 0x7F:
            return f"control character U+{ord(ch):04X}"
    return None


def _url(base_url: str | None, filename: str) -> str:
    if not base_url:
        # Bare file name: resolve relative to the catalog's own URL on device.
        return filename
    if "{file}" in base_url:
        return base_url.replace("{file}", filename)
    return f"{base_url.rstrip('/')}/{filename}"


def scan(directory: Path, base_url: str | None = None,
         order: list[str] | None = None) -> list[Record]:
    """Read every ``*.tgc`` in *directory* (sorted) into catalog records.

    *order* names packs (file names without ``.tgc``) that come first, in that
    order; the rest follow sorted by file name. A name with no pack is an error.
    Raises ValueError for a ``.tgc`` that is not a valid pack or whose deck name
    is not a valid title (title_problem), so a malformed artifact is reported
    instead of silently entering a release.
    """
    paths = sorted(directory.glob("*.tgc"))
    if order:
        by_name = {p.stem: p for p in paths}
        missing = [n for n in order if n not in by_name]
        if missing:
            raise ValueError(f"--order names packs that are not in {directory}: {', '.join(missing)}")
        first = list(dict.fromkeys(order))
        paths = [by_name[n] for n in first] + [p for p in paths if p.stem not in first]
    records: list[Record] = []
    for path in paths:
        raw = path.read_bytes()
        try:
            pack = read_pack(raw)
        except ValueError as e:  # wrong magic / truncated header
            raise ValueError(f"{path.name}: {e}") from e
        if pack.errors:
            raise ValueError(f"{path.name}: invalid pack: {pack.errors[0]}")
        problem = title_problem(pack.deck_name)
        if problem:
            raise ValueError(f"{path.name}: deck name {pack.deck_name!r} cannot be a catalog "
                             f"title: {problem}")
        records.append(
            Record(
                name=path.stem,
                title=pack.deck_name,
                lang=pack.header["lang"],
                cards=pack.header["card_count"],
                size=len(raw),
                sha256=hashlib.sha256(raw).hexdigest(),
                url=_url(base_url, path.name),
            )
        )
    return records


def render(records: list[Record]) -> str:
    """The line-oriented catalog text (the format the device is meant to read)."""
    return "\n".join([HEADER, COLUMNS] + [r.line() for r in records]) + "\n"


def render_json(records: list[Record]) -> str:
    """A human-readable companion (release pages, scripts). Never read on device."""
    return (
        json.dumps(
            {
                "_comment": "Human-readable companion to the line-oriented Tangocho pack "
                            "catalog. The device reads the text catalog, not this file.",
                "format": "tangocho-catalog",
                "version": VERSION,
                "packs": [r.as_dict() for r in records],
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n"
    )
