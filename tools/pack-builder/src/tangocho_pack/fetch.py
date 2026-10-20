"""Download third-party data into the (git-ignored) cache."""

from __future__ import annotations

import hashlib
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


# Word data for `build --common-readings` (docs/common-readings.md), opt-in with
# `fetch --readings`. JMdict and Tatoeba's indices have no versioned URL (the
# EDRDG file is rebuilt daily, like KANJIDIC2; Tatoeba exports weekly);
# JmdictFurigana is pinned to a monthly release built from JMdict, wordfreq to
# its last tag (its data is frozen since 2021). The Jōyō table PDF is the
# official notice. The JLPT vocabulary lists come from the Internet Archive's
# copies of tanos.co.uk (the site no longer resolves), pinned to the 2011-07-22
# snapshots and checked against SHA256.
JMDICT_FURIGANA_RELEASE = "2.3.1%2B2026-09-25"
WORDFREQ_TAG = "v3.2"
JLPT_VOCAB_SNAPSHOTS = {  # level: (Wayback timestamp, sha256)
    5: ("20110722124915", "b304d00ead721eba3db54644386405442623776b1b53bc5fa82a406cfa46f0ab"),
    4: ("20110722125438", "f39a6ba94b357a020bda63fa483d233b331fa281084857ee90466fe19452d57d"),
    3: ("20110722125127", "212fb7c83b5cf476d92a995506bd36ce9c0ec74599e956c0ed6a8ba346c16cdf"),
    2: ("20110722104109", "a21442ee7de29845925ed9086edb621e15a3a6937402e0dfca193f0d8ed1b7df"),
    1: ("20110722095128", "1e6296ae1a93091fd48ef33628c0cf445754964673f5b13842f5113c5fc648f0"),
}
SHA256: dict[Path, str] = {paths.JLPT_VOCAB[n]: h for n, (_, h) in JLPT_VOCAB_SNAPSHOTS.items()}
READINGS_SOURCES: list[tuple[str, Path]] = [
    ("https://www.edrdg.org/pub/Nihongo/JMdict_e.gz", paths.JMDICT_GZ),
    (
        "https://github.com/Doublevil/JmdictFurigana/releases/download/"
        f"{JMDICT_FURIGANA_RELEASE}/JmdictFurigana.txt",
        paths.JMDICT_FURIGANA,
    ),
    (
        f"https://raw.githubusercontent.com/rspeer/wordfreq/{WORDFREQ_TAG}/"
        "wordfreq/data/large_ja.msgpack.gz",
        paths.WORDFREQ_JA,
    ),
    ("https://downloads.tatoeba.org/exports/jpn_indices.tar.bz2", paths.TATOEBA_JPN),
    (
        "https://www.bunka.go.jp/kokugo_nihongo/sisaku/joho/joho/kijun/naikaku/pdf/"
        "joyokanjihyo_20101130.pdf",
        paths.JOYO_PDF,
    ),
] + [
    (
        f"https://web.archive.org/web/{ts}id_/http://www.tanos.co.uk/jlpt/jlpt{n}/vocab/"
        f"n{n}-vocab-kanji-hiragana.anki",
        paths.JLPT_VOCAB[n],
    )
    for n, (ts, _) in JLPT_VOCAB_SNAPSHOTS.items()
]


class ChecksumError(RuntimeError):
    pass


def fetch_all(force: bool = False, klee_one: bool = False, readings: bool = False) -> None:
    extra = (KLEE_SOURCES if klee_one else []) + (READINGS_SOURCES if readings else [])
    for url, dest in SOURCES + extra:
        want = SHA256.get(dest)
        if dest.exists() and not force:
            if want and hashlib.sha256(dest.read_bytes()).hexdigest() != want:
                print(f"bad   {dest.relative_to(paths.ROOT)} (checksum): downloading again")
            else:
                print(f"have  {dest.relative_to(paths.ROOT)}")
                continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"get   {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "tangocho-pack"})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        if want and hashlib.sha256(data).hexdigest() != want:
            raise ChecksumError(f"{url}: SHA256 {hashlib.sha256(data).hexdigest()}, "
                                f"expected {want}")
        tmp = dest.with_suffix(dest.suffix + ".part")
        tmp.write_bytes(data)
        tmp.replace(dest)
        print(f"  -> {dest.relative_to(paths.ROOT)} ({len(data):,} bytes)")
