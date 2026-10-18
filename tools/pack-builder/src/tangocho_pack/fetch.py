"""Download third-party data into the (git-ignored) cache."""

from __future__ import annotations

import urllib.request
from pathlib import Path

from . import paths

# Noto Sans JP (the default font) is pinned to the noto-cjk "Sans2.004" release
# tag: the Japanese subset OTF, plus the repository's LICENSE (SIL OFL 1.1, the
# licence of every Noto CJK font).
NOTO_RELEASE = "Sans2.004"
NOTO = f"https://raw.githubusercontent.com/notofonts/noto-cjk/{NOTO_RELEASE}"
KLEE = "https://github.com/fontworks-fonts/Klee/raw/master"
# KanjiVG is pinned to a dated release (tag + asset name carry the same date).
KANJIVG_RELEASE = "r20260714"
KANJIVG_ZIP_URL = (
    "https://github.com/KanjiVG/kanjivg/releases/download/"
    f"{KANJIVG_RELEASE}/kanjivg-20260714-main.zip"
)
KANJIVG_COPYING_URL = f"https://raw.githubusercontent.com/KanjiVG/kanjivg/{KANJIVG_RELEASE}/COPYING"
SOURCES: list[tuple[str, Path]] = [
    ("https://www.edrdg.org/kanjidic/kanjidic2.xml.gz", paths.KANJIDIC_GZ),
    (
        "https://raw.githubusercontent.com/davidluzgouveia/kanji-data/master/kanji.json",
        paths.KANJI_JSON,
    ),
    (f"{NOTO}/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf", paths.DEFAULT_FONT),
    (f"{NOTO}/LICENSE", paths.NOTO_LICENCE),
    (KANJIVG_ZIP_URL, paths.KANJIVG_ZIP),
    (KANJIVG_COPYING_URL, paths.KANJIVG_LICENCE),
]
# Klee One, the default font before 2026-10-18 (textbook style). Opt-in with
# `fetch --klee-one`; build with it through `--font`.
KLEE_SOURCES: list[tuple[str, Path]] = [
    (f"{KLEE}/fonts/ttf/KleeOne-SemiBold.ttf", paths.KLEE_ONE),
    (f"{KLEE}/fonts/ttf/KleeOne-Regular.ttf", paths.FONT_DIR / "KleeOne-Regular.ttf"),
    (f"{KLEE}/OFL.txt", paths.FONT_DIR / "OFL.txt"),
]


def fetch_all(force: bool = False, klee_one: bool = False) -> None:
    for url, dest in SOURCES + (KLEE_SOURCES if klee_one else []):
        if dest.exists() and not force:
            print(f"have  {dest.relative_to(paths.ROOT)}")
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"get   {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "tangocho-pack"})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        tmp = dest.with_suffix(dest.suffix + ".part")
        tmp.write_bytes(data)
        tmp.replace(dest)
        print(f"  -> {dest.relative_to(paths.ROOT)} ({len(data):,} bytes)")
