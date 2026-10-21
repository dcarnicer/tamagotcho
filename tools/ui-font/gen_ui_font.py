#!/usr/bin/env python3
"""Generate the firmware's UI font, ``firmware/src/ui/UiFont.h``.

The firmware draws its own text (labels, values, the date numerals, the app's
name in kana) with glyphs pre-rendered from Noto Sans JP, the font the packs
already use, so that text works with no pack loaded and Spanish keeps its
accents (docs/ux-refresh.md §2.3 "U3", §6.2). Run it inside the pack builder's environment, like the UX
mockup script; it reuses the builder's ``FontRenderer``, so the bitmaps are the
same 4bpp format, quantisation and placement as a pack's text glyphs
(docs/pack-format.md):

    cd tools/pack-builder && uv run tamagotcho-pack fetch    # once: Noto Sans JP -> cache/
    cd tools/pack-builder && uv run python ../ui-font/gen_ui_font.py

Options: ``--font FILE`` (default: the builder's default font,
``cache/noto/NotoSansJP-Regular.otf``), ``--out FILE`` (default:
``firmware/src/ui/UiFont.h``) and ``--check`` (exit 1 when the output would
differ from the file on disk, without writing it).

The output is committed and deterministic: fixed code point lists, no
timestamps. It depends only on the font file and on the Pillow/FreeType
versions locked in ``tools/pack-builder/uv.lock`` (both are named in the
header). The firmware build never runs this script.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from dataclasses import dataclass
from pathlib import Path

import PIL
from PIL import features

from tamagotcho_pack import paths
from tamagotcho_pack.glyphs import FontRenderer, Glyph

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "firmware" / "src" / "ui" / "UiFont.h"


@dataclass(frozen=True)
class FaceSpec:
    name: str        # C++ identifier stem: kLabel, kLabelGlyphs, kLabelBits
    role: str        # one line for the header comment
    px: int          # em size
    pitch: int       # compact line pitch for one-script UI lines (ux-refresh §2.3)
    cap_char: str    # whose ink top gives the cap height
    required: tuple[int, ...]
    optional: tuple[int, ...] = ()  # included only when the font has them


def _range(a: int, b: int) -> tuple[int, ...]:
    return tuple(range(a, b + 1))


# The app's name, たまごっちょ (Tamagotcho), as the device shows it; the
# firmware's copy is tgc::credits::kAppName (firmware/src/Credits.h).
APP_NAME = "たまごっちょ"
APP_NAME_KANA = tuple(sorted({ord(c) for c in APP_NAME}))

FACES = (
    FaceSpec(
        name="Label",
        role="answers, row labels, values, the About title (docs/ux-refresh.md §2.3 Label)",
        px=18,
        pitch=20,
        cap_char="H",
        # ASCII, Latin-1 and the punctuation the UI uses: · (in Latin-1),
        # … › ‹ « » – —; and the kana of the app's name, たまごっちょ (the
        # About title).
        required=_range(0x20, 0x7E) + _range(0xA0, 0xFF)
        + (0x2013, 0x2014, 0x2026, 0x2039, 0x203A) + APP_NAME_KANA,
        # ✓ ✗ ▲ ▼ when the font has them (Noto Sans JP has no ✗; the widgets
        # draw all four as 1bpp icons from ui/Icons.h anyway).
        optional=(0x2713, 0x2717, 0x25B2, 0x25BC),
    ),
    FaceSpec(
        name="Numeral",
        role="the date's day, statistics heroes (docs/ux-refresh.md §2.3 Numeral)",
        px=32,
        pitch=36,
        cap_char="0",
        required=(0x20,) + _range(0x30, 0x39) + tuple(ord(c) for c in "%:/+-."),
    ),
    FaceSpec(
        name="Kana",
        role="the app's name in the 16 px header, where Font0 has no kana",
        px=12,
        pitch=14,
        # "Cap height" here is the ink top of a full-size kana.
        cap_char="た",
        required=APP_NAME_KANA,
    ),
)


@dataclass
class Face:
    spec: FaceSpec
    glyphs: list[Glyph]
    offsets: list[int]
    bits: bytes
    ascent: int
    line_height: int
    cap_height: int


def render_face(font: FontRenderer, spec: FaceSpec) -> Face:
    missing = [cp for cp in spec.required if not font.has(cp)]
    if missing:
        raise SystemExit(
            f"{spec.name}: the font has no glyph for "
            + ", ".join(f"U+{cp:04X}" for cp in missing))
    cps = sorted(set(spec.required) | {cp for cp in spec.optional if font.has(cp)})
    glyphs: list[Glyph] = []
    offsets: list[int] = []
    bits = bytearray()
    for cp in cps:
        g = font.text_glyph(cp, spec.px)
        if not (0 <= g.width <= 255 and 0 <= g.height <= 255 and 0 <= g.advance <= 255
                and -128 <= g.x_off <= 127 and -128 <= g.y_off <= 127):
            raise SystemExit(f"{spec.name}: U+{cp:04X} does not fit the glyph table")
        glyphs.append(g)
        offsets.append(len(bits))
        bits += g.data
    if len(bits) > 0xFFFF:
        raise SystemExit(f"{spec.name}: {len(bits)} bytes of bitmaps, offsets are 16-bit")
    line_height, ascent = font.text_metrics(spec.px)
    cap = next(g for g in glyphs if g.codepoint == ord(spec.cap_char))
    return Face(spec, glyphs, offsets, bytes(bits), ascent, line_height, -cap.y_off)


def _char_comment(cp: int) -> str:
    # Printable ASCII is shown; a backslash at the end of a // comment would
    # continue it onto the next line, so it is left out.
    if 0x21 <= cp <= 0x7E and cp != 0x5C:
        return f"U+{cp:04X} '{chr(cp)}'"
    return f"U+{cp:04X}"


GLYPH_TABLE_ENTRY = 10  # sizeof(UiGlyph): 2 + 2 + 4 x 1 + 1, padded to 2


def emit(faces: list[Face], font_path: Path) -> str:
    sha = hashlib.sha256(font_path.read_bytes()).hexdigest()
    total_bits = sum(len(f.bits) for f in faces)
    total_tables = sum(len(f.glyphs) * GLYPH_TABLE_ENTRY for f in faces)
    out: list[str] = []
    w = out.append
    w("#pragma once")
    w("// Tamagotcho firmware UI font: Noto Sans JP glyphs pre-rendered to 4bpp bitmaps")
    w("// (docs/ux-refresh.md §2.3 and §6.2, docs/decisions.md).")
    w("//")
    w("// GENERATED by tools/ui-font/gen_ui_font.py -- do not edit by hand. Regenerate:")
    w("//   cd tools/pack-builder && uv run tamagotcho-pack fetch   # once: the font")
    w("//   cd tools/pack-builder && uv run python ../ui-font/gen_ui_font.py")
    w("//")
    w(f"// Font: {font_path.name}, sha256 {sha[:16]}..., (c) Adobe,")
    w("// SIL Open Font License 1.1 (docs/data-sources.md). Rasterised by the pack")
    w(f"// builder's FontRenderer (Pillow {PIL.__version__}, FreeType "
      f"{features.version('freetype2')}): the format and")
    w("// placement of pack text glyphs (docs/pack-format.md), see ui/UiFace.h.")
    for f in faces:
        s = f.spec
        w(f"// {s.name} {s.px} px: {len(f.glyphs)} glyphs, {len(f.bits)} bytes; {s.role}.")
    w(f"// Flash: {total_bits} bytes of bitmaps + {total_tables} bytes of glyph tables.")
    w('#include "ui/UiFace.h"')
    w("")
    w("namespace tgc {")
    w("namespace uifont {")
    for f in faces:
        s = f.spec
        w("")
        w(f"inline constexpr uint8_t k{s.name}Bits[] = {{")
        for g, off in zip(f.glyphs, f.offsets):
            if not g.data:
                continue
            w(f"    // {_char_comment(g.codepoint)} {g.width}x{g.height} at {off}")
            for i in range(0, len(g.data), 16):
                w("    " + " ".join(f"0x{b:02x}," for b in g.data[i:i + 16]))
        w("};")
        w("")
        w(f"inline constexpr UiGlyph k{s.name}Glyphs[] = {{")
        w("    // cp, offset, width, height, xOff, yOff, advance")
        for g, off in zip(f.glyphs, f.offsets):
            w(f"    {{0x{g.codepoint:04X}, {off}, {g.width}, {g.height}, {g.x_off}, {g.y_off}, "
              f"{g.advance}}},  // {_char_comment(g.codepoint)}")
        w("};")
        w("")
        w(f"inline constexpr UiFace k{s.name} = {{")
        w(f"    k{s.name}Glyphs, {len(f.glyphs)}, k{s.name}Bits, {len(f.bits)},")
        w(f"    /* px */ {s.px}, /* ascent */ {f.ascent}, /* lineHeight */ {f.line_height},")
        w(f"    /* capHeight */ {f.cap_height}, /* pitch */ {s.pitch},")
        w("};")
    w("")
    w("}  // namespace uifont")
    w("}  // namespace tgc")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", type=Path, default=paths.DEFAULT_FONT)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the output differs from --out (nothing is written)")
    args = ap.parse_args()
    if not args.font.exists():
        print(f"{args.font} not found: run `uv run tamagotcho-pack fetch` first", file=sys.stderr)
        return 2
    font = FontRenderer(args.font)
    faces = [render_face(font, spec) for spec in FACES]
    text = emit(faces, args.font)
    if args.check:
        same = args.out.exists() and args.out.read_text(encoding="utf-8") == text
        print(f"{args.out}: {'up to date' if same else 'OUT OF DATE'}")
        return 0 if same else 1
    args.out.write_text(text, encoding="utf-8")
    bits = sum(len(f.bits) for f in faces)
    tables = sum(len(f.glyphs) * GLYPH_TABLE_ENTRY for f in faces)
    for f in faces:
        print(f"{f.spec.name} {f.spec.px} px: {len(f.glyphs)} glyphs, {len(f.bits)} bytes, "
              f"cap height {f.cap_height}, ascent {f.ascent}, line {f.line_height}")
    print(f"wrote {args.out} ({bits} bytes of bitmaps + {tables} bytes of tables)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
