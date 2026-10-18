"""Write a small, line-oriented catalog of built packs (see README.md).

The roadmap wants prebuilt packs hosted with "a small catalog the device can
read". This module produces that file. The format is deliberately trivial to
parse on an ESP32 without a JSON library: one record per line, TAB-separated
fields, `#` comments and blank lines to be skipped.

    #tangocho-catalog<TAB>1
    # name<TAB>lang<TAB>cards<TAB>bytes<TAB>sha256<TAB>url
    n5-en<TAB>en<TAB>79<TAB>703123<TAB>ab12...<TAB>https://.../n5-en.tgc

- The first line is a version tag (`#` comment that also carries the format
  version), so a reader can detect the format and a future v2 can be refused.
- The second line documents the columns; readers skip every `#` line.
- `name` is the file's basename without `.tgc`, i.e. the name the device should
  save the downloaded file as (`<name>.tgc`).
- `lang` is the pack's meaning language, `cards` its card count and `bytes` its
  exact file size.
- `sha256` is the lowercase hex digest of the whole file; the device checks it
  (and `bytes`) before it keeps a download (firmware/src/catalog/, Get decks).
- `url` is either absolute (`--base-url`) or the bare file name, meaning
  "relative to the URL this catalog was fetched from".

Records are sorted by file name, so the output is deterministic for a given
directory. The `catalog` command refuses to write a catalog for an empty
directory or a file that is not a valid pack, so a broken build cannot silently
enter a release.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .pack import read_pack

VERSION = 1
# `#`-prefixed lines are comments for any reader. The first line carries the
# version so a reader can refuse an unknown format before parsing records.
HEADER = f"#tangocho-catalog\t{VERSION}"
COLUMNS = "# name\tlang\tcards\tbytes\tsha256\turl"


@dataclass(frozen=True)
class Record:
    name: str
    lang: str
    cards: int
    size: int
    sha256: str
    url: str

    def line(self) -> str:
        return "\t".join(
            [self.name, self.lang, str(self.cards), str(self.size), self.sha256, self.url]
        )

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "lang": self.lang,
            "cards": self.cards,
            "bytes": self.size,
            "sha256": self.sha256,
            "url": self.url,
        }


def _url(base_url: str | None, filename: str) -> str:
    if not base_url:
        # Bare file name: resolve relative to the catalog's own URL on device.
        return filename
    if "{file}" in base_url:
        return base_url.replace("{file}", filename)
    return f"{base_url.rstrip('/')}/{filename}"


def scan(directory: Path, base_url: str | None = None) -> list[Record]:
    """Read every ``*.tgc`` in *directory* (sorted) into catalog records.

    Raises ValueError for a ``.tgc`` that is not a valid pack, so a malformed
    artifact is reported instead of silently entering a release.
    """
    records: list[Record] = []
    for path in sorted(directory.glob("*.tgc")):
        raw = path.read_bytes()
        try:
            pack = read_pack(raw)
        except ValueError as e:  # wrong magic / truncated header
            raise ValueError(f"{path.name}: {e}") from e
        if pack.errors:
            raise ValueError(f"{path.name}: invalid pack: {pack.errors[0]}")
        records.append(
            Record(
                name=path.stem,
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
