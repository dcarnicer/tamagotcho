import struct
from pathlib import Path

import pytest

from tangocho_pack import fixture, paths
from tangocho_pack.glyphs import Glyph
from tangocho_pack.kanjidic import Card
from tangocho_pack.kanjivg import KanjiVGDir, StrokeMask, flatten_path, stroke_masks
from tangocho_pack.pack import read_pack, write_pack

KANJIVG = Path(__file__).parent / "data" / "kanjivg"
needs_font = pytest.mark.skipif(not paths.DEFAULT_FONT.exists(), reason="run `tangocho-pack fetch`")


def mini_src() -> KanjiVGDir:
    return KanjiVGDir(KANJIVG)


# ------------------------------------------------------------ ftixture SVGs

def test_fixture_svgs_parse_in_document_order():
    src = mini_src()
    assert src.svg(0x65E5) is not None and src.svg(0x4E00) is None
    masks = stroke_masks(src.svg(0x65E5), 32)
    assert len(masks) == 4  # 日 has four strokes; document order is stroke order
    # Stroke 1 is the tall left vertical, stroke 4 the bottom horizontal.
    first, last = masks[0], masks[-1]
    assert first.h > first.w
    assert last.w > last.h
    for m in masks:
        assert m.w > 0 and m.h > 0
        assert 0 <= m.x_off < 32 and 0 <= m.y_off < 32
        assert m.x_off + m.w <= 32 and m.y_off + m.h <= 32
        assert any(m.data)  # every mask has ink
        assert len(m.data) == ((m.w + 7) // 8) * m.h


def test_stroke_masks_scale_with_px():
    svg = mini_src().svg(0x706B)
    small, large = stroke_masks(svg, 32), stroke_masks(svg, 96)
    assert len(small) == len(large) == 4
    assert large[0].w >= small[0].w and large[0].h >= small[0].h


def test_unsupported_command_skips_path(capsys):
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 109 109">'
        '<path d="M10,10 L90,10"/>'
        '<path d="M10,20 A5,5 0 0 1 90,20"/>'  # arcs are unsupported -> skipped
        "</svg>"
    ).encode()
    masks = stroke_masks(svg, 32)
    assert len(masks) == 1
    assert "unsupported" in capsys.readouterr().err


def test_flatten_curves_and_close():
    subs = flatten_path("M0,0 L10,0 h5 v5 c1,1,2,2,3,3 z")
    assert len(subs) == 1 and len(subs[0]) >= 8
    assert subs[0][0] == (0.0, 0.0) and subs[0][-1] == (0.0, 0.0)  # z closes back


# ------------------------------------------------------------ pack writer

def _card() -> Card:
    return Card(0x65E5, ["day"], [], [], jlpt=5, strokes=4)


def _glyphs() -> list[Glyph]:
    return [Glyph(0x65E5, 0, 4, 4, 0, -4, 32, b"\x00" * 8),
            Glyph(0x65E5, 1, 4, 4, 0, -4, 16, b"\x00" * 8)]


def test_stroke_section_roundtrip():
    masks = [StrokeMask(2, 3, 9, 4, bytes([0xAA, 0x80] * 4)),
             StrokeMask(1, 0, 1, 1, b"\x80")]
    raw = write_pack([_card()], _glyphs(), "mini", "en", 32, 16, 12, 12, 10, build_time=1,
                     strokes=[masks], strokes_px=32)
    p = read_pack(raw)
    assert p.errors == []
    so = p.header["stroke_off"]
    assert so != 0 and so % 4 == 0
    st = p.strokes_of(0)
    assert [(s.x_off, s.y_off, s.w, s.h) for s in st] == [(2, 3, 9, 4), (1, 0, 1, 1)]
    assert st[0].data == masks[0].data
    assert st[0].data_off % 4 == 0
    # Same inputs without strokes: no section, and byte-identical to the v1 writer.
    plain = write_pack([_card()], _glyphs(), "mini", "en", 32, 16, 12, 12, 10, build_time=1)
    p2 = read_pack(plain)
    assert p2.errors == []
    assert p2.header["stroke_off"] == 0
    assert p2.strokes_of(0) == []
    assert plain == write_pack([_card()], _glyphs(), "mini", "en", 32, 16, 12, 12, 10, build_time=1)


def test_index_entry_zero_for_unstroked_card():
    masks = [StrokeMask(1, 1, 2, 2, b"\xc0\xc0")]
    raw = write_pack([_card(), Card(0x6708, ["moon"], [], [], jlpt=5, strokes=4)],
                     _glyphs() + [Glyph(0x6708, 0, 4, 4, 0, -4, 32, b"\x00" * 8),
                                  Glyph(0x6708, 1, 4, 4, 0, -4, 16, b"\x00" * 8)],
                     "mini", "en", 32, 16, 12, 12, 10, build_time=1,
                     strokes=[masks, []], strokes_px=32)
    p = read_pack(raw)
    assert p.errors == []
    so = p.header["stroke_off"]
    assert struct.unpack_from("<2I", raw, so)[1] == 0  # card 1: no data
    assert len(p.strokes_of(0)) == 1 and p.strokes_of(1) == []


def test_read_pack_rejects_bad_stroke_offsets():
    base = (paths.FIXTURES / "mini.tgc").read_bytes()
    so = read_pack(base).header["stroke_off"]
    assert so
    # stroke_off out of bounds
    bad = bytearray(base)
    struct.pack_into("<I", bad, 60, 0x7FFFFFFF)
    assert any("stroke" in e for e in read_pack(bytes(bad)).errors)
    # a mask data_off past the end
    bad = bytearray(base)
    blk = struct.unpack_from("<I", bad, so)[0]
    struct.pack_into("<I", bad, blk + 8 + 4, 0x7FFFFFFF)
    assert any("out of range" in e for e in read_pack(bytes(bad)).errors)
    # a block px that disagrees with the header
    bad = bytearray(base)
    struct.pack_into("<H", bad, blk + 2, 99)
    assert any("large_px" in e for e in read_pack(bytes(bad)).errors)


@needs_font
def test_build_rejects_strokes_px_mismatch():
    from tangocho_pack.build import build_pack
    with pytest.raises(ValueError, match="em square"):
        build_pack([_card()], paths.DEFAULT_FONT, "en", "mini", 32, 16, 12, 12,
                   strokes=True, strokes_px=16, kanjivg=mini_src())


@needs_font
def test_no_strokes_build_is_byte_identical_to_v1():
    baseline = (Path(__file__).parent / "data" / "mini_v1_baseline.tgc").read_bytes()
    assert fixture.build_mini(paths.DEFAULT_FONT, strokes=False) == baseline


@needs_font
def test_mini_fixture_carries_strokes():
    p = read_pack((paths.FIXTURES / "mini.tgc").read_bytes())
    assert p.errors == []
    assert p.header["stroke_off"] != 0
    assert [len(p.strokes_of(i)) for i in range(3)] == [4, 4, 4]
    for s in p.strokes_of(0):
        assert 0 <= s.x_off < 32 and 0 <= s.y_off < 32
        assert s.w > 0 and s.h > 0


@needs_font
def test_preview_strokes_mode():
    from tangocho_pack.preview import render_preview
    p = read_pack((paths.FIXTURES / "mini.tgc").read_bytes())
    img, info = render_preview(p, 0x65E5, "strokes", 135, 240, 2)
    assert img.size == (270, 480)
    assert info["stroke_count"] == 4 and info["missing"] == []
