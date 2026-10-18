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
