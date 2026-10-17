"""tangocho-pack command line."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import paths


def cmd_fetch(a) -> int:
    from .fetch import fetch_all
    fetch_all(force=a.force)
    return 0


def cmd_build(a) -> int:
    from .build import build_pack, default_deck_name
    from .kanjidic import load_new_jlpt, make_cards, parse_kanjidic
    from .pack import read_pack

    jlpt, grades = set(a.jlpt or []), set(a.grade or [])
    if not jlpt and not grades:
        print("error: give at least one --jlpt or --grade", file=sys.stderr)
        return 2
    if not paths.KANJIDIC_GZ.exists():
        print("error: run `tangocho-pack fetch` first", file=sys.stderr)
        return 2
    new = None
    if paths.KANJI_JSON.exists():
        new = load_new_jlpt(paths.KANJI_JSON)
    else:
        print("warning: kanji.json missing, mapping old 4-level JLPT (lossy)", file=sys.stderr)
    cards = make_cards(parse_kanjidic(paths.KANJIDIC_GZ), a.lang, jlpt, grades, new)
    if not cards:
        print("error: no cards selected", file=sys.stderr)
        return 1
    name = a.name or default_deck_name(jlpt, grades)
    raw = build_pack(cards, a.font, a.lang, name, a.large_px, a.small_px, a.text_px, a.option_px)
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(raw)
    p = read_pack(raw)
    fb = sum(1 for c in p.cards if c.flags & 1)
    print(f"{out}: {len(p.cards)} cards ({fb} with English fallback), "
          f"{len(p.glyphs)} glyphs, {len(raw):,} bytes")
    if p.errors:
        print("pack validation errors:", *p.errors[:10], sep="\n  ", file=sys.stderr)
        return 1
    return 0


def cmd_inspect(a) -> int:
    from .pack import CARD_SIZE, GLYPH_SIZE, read_pack
    raw = Path(a.file).read_bytes()
    try:
        p = read_pack(raw)
    except ValueError as e:
        print(f"INVALID: {e}")
        return 1
    h = p.header
    sets = [sum(1 for g in p.glyphs if g.set == s) for s in (0, 1, 2, 3)]
    data_bytes = sum(((g.width + 1) // 2) * g.height for g in p.glyphs)
    print(f"file        {a.file}")
    print(f"version     {h['version']}   lang {h['lang']}   deck {p.deck_name!r}")
    print(f"cards       {h['card_count']} ({sum(1 for c in p.cards if c.flags & 1)} English fallback)")
    print(f"glyphs      {h['glyph_count']} (set0 {sets[0]}, set1 {sets[1]}, set2 {sets[2]}, set3 {sets[3]})")
    print(f"px          large {h['large_px']}  small {h['small_px']}  text {h['text_px']} "
          f"(line {h['text_line_height']}, ascent {h['text_ascent']})  option {h['option_px']} "
          f"(line {h['option_line_height']}, ascent {h['option_ascent']})")
    print("sections    bytes")
    print(f"  header          {h['header_size']:>9,}")
    print(f"  cards           {h['card_count'] * CARD_SIZE:>9,}")
    print(f"  glyph table     {h['glyph_count'] * GLYPH_SIZE:>9,}")
    print(f"  distractors     {h['strings_off'] - h['distract_off']:>9,}")
    print(f"  strings         {h['strings_size']:>9,}")
    print(f"  glyph bitmaps   {data_bytes:>9,}")
    print(f"  total           {len(raw):>9,}")
    if p.errors:
        print(f"INVALID: {len(p.errors)} problem(s)")
        for e in p.errors[:50]:
            print("  -", e)
        return 1
    print("OK")
    return 0


def cmd_preview(a) -> int:
    from .pack import read_pack
    from .preview import render_preview
    if len(a.card) != 1:
        print("error: --card must be a single character", file=sys.stderr)
        return 2
    p = read_pack(Path(a.file).read_bytes())
    img, info = render_preview(p, ord(a.card), a.mode, a.width, a.height, a.scale, a.seed, a.reveal,
                               a.kind)
    img.save(a.output)
    print(f"{a.output}: options {[chr(c) for c in info['options']]} correct slot {info['correct_slot']}")
    if info.get("reading_kind"):
        print(f"  reading kind: {info['reading_kind']}")
    if info["missing"]:
        print("MISSING glyphs:", "".join(info["missing"]), file=sys.stderr)
        return 1
    return 0


def cmd_fixture(a) -> int:
    from .fixture import write_fixture
    for f in write_fixture(Path(a.font) if a.font else None):
        print(f"wrote {f} ({f.stat().st_size:,} bytes)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="tangocho-pack")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("fetch", help="download KANJIDIC2, JLPT levels and Klee One into cache/")
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_fetch)

    s = sub.add_parser("build", help="build a .tgc pack")
    s.add_argument("--jlpt", type=int, action="append", choices=range(1, 6), metavar="N")
    s.add_argument("--grade", type=int, action="append", choices=range(1, 11), metavar="N")
    s.add_argument("--lang", default="en", choices=["en", "es", "fr", "pt"])
    s.add_argument("--font", default=str(paths.DEFAULT_FONT))
    s.add_argument("--large-px", type=int, default=96)
    s.add_argument("--small-px", type=int, default=40)
    s.add_argument("--text-px", type=int, default=18)
    s.add_argument("--option-px", type=int, default=20,
                   help="size of set 3 (large text for reading answers); 0 to omit")
    s.add_argument("--name", help="deck name (default derived from filters)")
    s.add_argument("-o", "--output", required=True)
    s.set_defaults(fn=cmd_build)

    s = sub.add_parser("inspect", help="validate a pack and print a summary")
    s.add_argument("file")
    s.set_defaults(fn=cmd_inspect)

    s = sub.add_parser("preview", help="render a simulated device screen from a pack")
    s.add_argument("file")
    s.add_argument("--card", required=True)
    s.add_argument("--mode", choices=["meaning", "reading"], default="meaning")
    s.add_argument("--kind", choices=["on", "kun"], default="on",
                   help="reading kind to ask for in reading mode")
    s.add_argument("--width", type=int, default=135)
    s.add_argument("--height", type=int, default=240)
    s.add_argument("--scale", type=int, default=3)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--reveal", action="store_true", help="highlight the correct option")
    s.add_argument("-o", "--output", required=True)
    s.set_defaults(fn=cmd_preview)

    s = sub.add_parser("fixture", help="regenerate fixtures/mini.tgc and mini.json")
    s.add_argument("--font")
    s.set_defaults(fn=cmd_fixture)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
