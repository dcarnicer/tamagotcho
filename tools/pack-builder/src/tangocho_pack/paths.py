"""Well-known paths inside tools/pack-builder."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "cache"
FIXTURES = ROOT / "fixtures"
KANJIDIC_GZ = CACHE / "kanjidic2.xml.gz"
KANJI_JSON = CACHE / "kanji.json"
FONT_DIR = CACHE / "fonts"
DEFAULT_FONT = FONT_DIR / "KleeOne-SemiBold.ttf"
