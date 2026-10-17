import random
from pathlib import Path

import pytest

from tangocho_pack import fixture, paths
from tangocho_pack.build import build_pack
from tangocho_pack.glyphs import pack_4bpp, unpack_4bpp
from tangocho_pack.kanjidic import RawKanji
from tangocho_pack.kanjidic import make_cards, parse_kanjidic
from tangocho_pack.pack import read_pack
from tangocho_pack.preview import render_preview

XML = Path(__file__).parent / "data" / "mini_kanjidic2.xml"
JLPT = {0x65E5: 5, 0x6708: 5, 0x706B: 5}
needs_font = pytest.mark.skipif(not paths.DEFAULT_FONT.exists(), reason="run `tangocho-pack fetch`")


def mini_cards(lang="en"):
    return make_cards(parse_kanjidic(XML.read_bytes()), lang, {5}, None, JLPT)


def test_4bpp_packing():
    # 3 px wide -> stride 2; high nibble first; rows byte aligned
    px = [0, 255, 128, 255, 0, 17]
    assert pack_4bpp(px, 3, 2) == bytes([0x0F, 0x80, 0xF0, 0x10])


def test_4bpp_roundtrip_and_quantise():
    rng = random.Random(0)
    for w, h in [(1, 1), (5, 3), (8, 2), (7, 7)]:
        vals = [rng.randrange(256) for _ in range(w * h)]
        out = unpack_4bpp(pack_4bpp(vals, w, h), w, h)
        assert out == [(v * 15 + 127) // 255 for v in vals]
    assert pack_4bpp([255, 0], 2, 1) == b"\xf0"


def test_parse_kanjidic_fields():
    raw = {k.codepoint: k for k in parse_kanjidic(XML.read_bytes())}
    nichi = raw[0x65E5]
    assert "Sun" in nichi.meanings["en"] or "sun" in [m.lower() for m in nichi.meanings["en"]]
    assert all("-" not in k for k in nichi.kunyomi)
    assert "ニチ" in nichi.onyomi
    assert nichi.strokes == 4


def test_meaning_language_capped_to_english():
    # 母 has a single English sense; the Spanish list's extra senses are dropped.
    mother = RawKanji(0x6BCD, {"en": ["mother"], "es": ["madre", "base", "cimientos"]},
                      ["ボ"], ["はは"], 4, 2, 5, 0)
    cards = make_cards([mother], "es", {5}, None, {0x6BCD: 5})
    assert cards[0].meanings == ["madre"]
    # 月 has two English senses; both Spanish ones are kept.
    moon = RawKanji(0x6708, {"en": ["month", "moon"], "es": ["mes", "luna", "extra"]},
                    ["ゲツ"], ["つき"], 4, 2, 4, 0)
    cards = make_cards([moon], "es", {5}, None, {0x6708: 5})
    assert cards[0].meanings == ["mes", "luna"]


def test_glcd_cp437_map():
    from tangocho_pack import glcd
    assert glcd.glcd_index(ord("A")) == 0x41
    assert glcd.glcd_index(0x00E9) == 0x82  # é
    assert glcd.glcd_index(0x00E1) == 0xA0  # á
    assert glcd.glcd_index(0x00F1) == 0xA4  # ñ
    assert glcd.glcd_index(0x00BF) == 0xA8  # ¿
    assert glcd.glcd_index(0x00C1) == -1    # Á (not in Font0)
    assert glcd.can_draw("día") and not glcd.can_draw("Á")


def test_meaning_distractors_use_visual_and_skip_glosses():
    from tangocho_pack.distractors import meaning_distractors
    from tangocho_pack.kanjidic import Card
    cards = [Card(1, ["a"], [], [], jlpt=5, strokes=1),
             Card(2, ["b"], [], [], jlpt=5, strokes=1),
             Card(3, ["a"], [], [], jlpt=5, strokes=1),  # shares a gloss with card 0
             Card(4, ["d"], [], [], jlpt=5, strokes=1)]
    visual = [(1.0, 0.0), (0.9, 0.1), (1.0, 0.0), (0.0, 1.0)]
    order = meaning_distractors(cards, visual)[0]
    assert 2 not in order      # card 2 shares a gloss with card 0 -> excluded
    assert order[0] == 1       # most visually similar (cosine ~0.99)


def test_reading_distractors_pick_confusable():
    from tangocho_pack.distractors import reading_distractors
    from tangocho_pack.kanjidic import Card
    cards = [Card(1, ["x"], ["ショウ"], [], jlpt=5, strokes=1),
             Card(2, ["y"], ["ジョウ"], [], jlpt=5, strokes=2),  # one kana apart
             Card(3, ["z"], [], ["つき"], jlpt=5, strokes=3)]     # no on'yomi to compare
    order = reading_distractors(cards)[0]
    assert order == [1]


def test_reading_distractors_skip_target_readings():
    from tangocho_pack.distractors import reading_distractors
    from tangocho_pack.kanjidic import Card
    cards = [Card(1, ["x"], ["ニチ", "ジツ"], ["ひ", "び"], jlpt=5, strokes=1),
             Card(2, ["y"], [], ["ひ"], jlpt=5, strokes=2),     # kun ひ is a target reading
             Card(3, ["z"], ["ジツ"], [], jlpt=5, strokes=3),   # on ジツ is a target reading
             Card(4, ["w"], ["ゲツ"], [], jlpt=5, strokes=4)]   # safe
    assert reading_distractors(cards)[0] == [3]


def test_spanish_fallback_flag():
    cards = mini_cards("fr")  # fixture only guarantees some languages
    for c in cards:
        assert c.meanings  # always falls back to English at worst
        assert len(c.meanings) <= 3


@needs_font
def test_roundtrip_and_sorted_glyphs():
    cards = mini_cards()
    raw = build_pack(cards, paths.DEFAULT_FONT, "en", "JLPT N5", 32, 16, 12, build_time=123)
    p = read_pack(raw)
    assert p.errors == []
    assert p.header["lang"] == "en" and p.header["build_time"] == 123
    assert (p.header["large_px"], p.header["small_px"], p.header["text_px"]) == (32, 16, 12)
    assert p.deck_name == "JLPT N5"
    assert [c.codepoint for c in p.cards] == [c.codepoint for c in cards]
    for pc, c in zip(p.cards, cards):
        assert pc.meanings == "; ".join(c.meanings)
        assert pc.onyomi == "、".join(c.onyomi)
        assert pc.kunyomi == "、".join(c.kunyomi)
        assert pc.jlpt == 5 and pc.flags == 0
        assert pc.distractors == c.distractors and len(pc.distractors) == 2
        assert p.find_glyph(0, pc.codepoint) and p.find_glyph(1, pc.codepoint)
    keys = [(g.set, g.codepoint) for g in p.glyphs]
    assert keys == sorted(keys) and len(set(keys)) == len(keys)
    for cp in range(0x20, 0x7F):
        assert p.find_glyph(2, cp) is not None
    assert p.find_glyph(2, 0x3042 + 0x1000) is None
    # same input, same bytes
    assert raw == build_pack(mini_cards(), paths.DEFAULT_FONT, "en", "JLPT N5", 32, 16, 12, build_time=123)


@needs_font
def test_kanji_glyph_centred_in_square():
    p = read_pack(fixture.build_mini(paths.DEFAULT_FONT))
    for gs, px in ((0, 32), (1, 16)):
        for c in p.cards:
            g = p.find_glyph(gs, c.codepoint)
            assert 0 < g.width <= px and 0 < g.height <= px
            assert g.advance == px
            top = g.y_off + px  # offset from square top, baseline = square bottom
            assert abs(g.x_off - (px - g.width - g.x_off)) <= 1
            assert abs(top - (px - g.height - top)) <= 1


@needs_font
def test_inspect_detects_corruption():
    raw = bytearray(fixture.build_mini(paths.DEFAULT_FONT))
    assert read_pack(bytes(raw)).errors == []
    p = read_pack(bytes(raw))
    # swap two glyph table entries -> sort violation
    o = p.header["glyphs_off"]
    a, b = bytes(raw[o:o + 16]), bytes(raw[o + 16:o + 32])
    raw[o:o + 16], raw[o + 16:o + 32] = b, a
    assert any("sorted" in e for e in read_pack(bytes(raw)).errors)
    with pytest.raises(ValueError):
        read_pack(b"XXXX" + bytes(raw[4:]))


@needs_font
def test_preview_uses_pack_only():
    p = read_pack(fixture.build_mini(paths.DEFAULT_FONT))
    img, info = render_preview(p, 0x65E5, "meaning", 135, 240, 2)
    assert img.size == (270, 480) and info["missing"] == []
    img, info = render_preview(p, 0x6708, "reading", 135, 240, 1)
    assert info["missing"] == []


@needs_font
def test_fixture_is_deterministic(tmp_path):
    a = fixture.build_mini(paths.DEFAULT_FONT)
    assert a == fixture.build_mini(paths.DEFAULT_FONT)
    committed = paths.FIXTURES / "mini.tgc"
    if committed.exists():
        assert a == committed.read_bytes(), "run `tangocho-pack fixture`"
