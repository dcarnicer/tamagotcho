"""Well-known paths inside tools/pack-builder."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "cache"
FIXTURES = ROOT / "fixtures"
KANJIDIC_GZ = CACHE / "kanjidic2.xml.gz"
KANJI_JSON = CACHE / "kanji.json"
NOTO_DIR = CACHE / "noto"
DEFAULT_FONT = NOTO_DIR / "NotoSansJP-Regular.otf"
NOTO_LICENCE = NOTO_DIR / "LICENSE"
# Klee One, the default font before 2026-10-18: only `fetch --klee-one` gets it,
# and `build --font cache/fonts/KleeOne-SemiBold.ttf` uses it.
FONT_DIR = CACHE / "fonts"
KLEE_ONE = FONT_DIR / "KleeOne-SemiBold.ttf"
KANJIVG_DIR = CACHE / "kanjivg"
KANJIVG_ZIP = KANJIVG_DIR / "kanjivg-20260714-main.zip"
KANJIVG_LICENCE = KANJIVG_DIR / "COPYING"
# Inputs of `build --common-readings` (docs/common-readings.md): only
# `fetch --readings` gets them.
READINGS_DIR = CACHE / "readings"
JMDICT_GZ = READINGS_DIR / "JMdict_e.gz"
JMDICT_FURIGANA = READINGS_DIR / "JmdictFurigana.txt"
WORDFREQ_JA = READINGS_DIR / "large_ja.msgpack.gz"
TATOEBA_JPN = READINGS_DIR / "jpn_indices.tar.bz2"
# Jonathan Waller's JLPT vocabulary lists (tanos.co.uk), N5..N1, as Anki 1.x decks.
JLPT_VOCAB = {n: READINGS_DIR / "jlpt" / f"n{n}-vocab-kanji-hiragana.anki" for n in (5, 4, 3, 2, 1)}
# The official Jōyō table, read only by scripts/common_readings_report.py.
JOYO_PDF = READINGS_DIR / "joyokanjihyo_20101130.pdf"
# Input of `build --meaning-order` (docs/meaning-order.md): only
# `fetch --meanings` gets it. FreeDict's English-Spanish dictionary (WikDict).
MEANINGS_DIR = CACHE / "meanings"
FREEDICT_ENG_SPA = MEANINGS_DIR / "freedict-eng-spa-2025.11.23.src.tar.xz"
# Project-authored data, committed (not downloaded): the hand-checked meaning
# overrides of `build --meaning-order`.
DATA = ROOT / "data"
MEANING_OVERRIDES = DATA / "meaning_overrides.tsv"
