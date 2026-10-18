"""Pack v2 compression: glyph RLE (encoding 1), stroke mask runs (encodings 1/2),
and their use by the writer, reader and fixtures (docs/pack-format.md)."""

import random
import struct

import pytest

from tangocho_pack import fixture, paths, rle
from tangocho_pack.glyphs import Glyph
from tangocho_pack.kanjidic import Card
from tangocho_pack.kanjivg import StrokeMask
from tangocho_pack.pack import GLYPH_SIZE, read_pack, write_pack

needs_font = pytest.mark.skipif(not paths.DEFAULT_FONT.exists(), reason="run `tangocho-pack fetch`")
# mini_v1.tgc was frozen when Klee One was the default font.
needs_klee = pytest.mark.skipif(not paths.KLEE_ONE.exists(), reason="run `tangocho-pack fetch --klee-one`")


def g4(pix, w, h):
    return rle.pack_4bpp_nibbles(pix, w, h)


def roundtrip_glyph(raw, w, h):
    enc = rle.encode_glyph(raw, w, h)
    back, used = rle.decode_glyph(enc, w, h)
    assert back == raw
    assert used == len(enc)
    return enc


# ------------------------------------------------------------ glyph RLE

def test_spec_example():
    raw = g4([0, 0, 7, 15, 15, 0, 0, 7, 15, 15, 15, 15, 15, 15, 3], 5, 3)
    assert raw.hex() == "007ff0007ff0ffff30"
    assert roundtrip_glyph(raw, 5, 3).hex() == "027f2005f430"


def test_length_codes():
    # 1..10 one code, 11..74 two, 75..330 three; the first code is never 0.
    assert rle._glyph_len(1) == [1] and rle._glyph_len(10) == [10]
    assert rle._glyph_len(11) == [11, 0] and rle._glyph_len(40) == [12, 13]
    assert rle._glyph_len(74) == [14, 15]
    assert rle._glyph_len(75) == [15, 0, 0] and rle._glyph_len(100) == [15, 1, 9]
    assert rle._glyph_len(330) == [15, 15, 15]
    for n in range(1, 331):
        codes = rle._glyph_len(n)
        assert codes[0] != 0
        nb = rle._Nibbles(rle._nibbles_to_bytes(codes[1:] + [0]))
        assert rle._read_glyph_len(nb, codes[0]) == n
    with pytest.raises(ValueError):
        rle._glyph_len(331)


@pytest.mark.parametrize("w,h", [(1, 1), (2, 1), (3, 5), (7, 7), (16, 3), (33, 9)])
def test_random_bitmaps_roundtrip(w, h):
    rng = random.Random(w * 100 + h)
    for _ in range(20):
        pix = [rng.choice([0, 0, 0, 15, 15, rng.randrange(16)]) for _ in range(w * h)]
        roundtrip_glyph(g4(pix, w, h), w, h)


def test_empty_and_degenerate():
    # No pixels: encoding 1 has nothing to say, the builder keeps encoding 0.
    assert rle.encode_glyph(b"", 0, 5) == b""
    assert rle.decode_glyph(b"", 0, 5) == (b"", 0)
    assert rle.best_glyph(b"", 0, 0) == (rle.GLYPH_RAW, b"")
    assert rle.best_glyph(b"", 6, 0) == (rle.GLYPH_RAW, b"")
    # An all-zero glyph is a single run: 0, length 45 = 11 + 2*16 + 2 -> "0 D 2" + pad.
    assert roundtrip_glyph(g4([0] * 45, 9, 5), 9, 5).hex() == "0d20"


def test_odd_width_padding_nibble_stays_zero():
    # Odd widths leave a padding nibble per row; decoding must keep it 0.
    raw = g4([15] * 15, 3, 5)
    enc = roundtrip_glyph(raw, 3, 5)
    assert all(b & 0x0F == 0 for b in raw[1::2])
    assert rle.decode_glyph(enc, 3, 5)[0] == raw


def test_all_ink_and_long_runs():
    raw = g4([15] * 256, 16, 16)
    enc = roundtrip_glyph(raw, 16, 16)
    assert enc.hex() == "ffb5"  # F, length 256 = 75 + 0xB5
    # Runs past the 330 maximum are split into several tokens.
    raw = g4([0] * 1019 + [5], 255, 4)
    enc = roundtrip_glyph(raw, 255, 4)
    assert enc.hex() == "0fff" * 3 + "0c2" + "5"  # 3 x 330, then 29 = 11 + 16 + 2, then 5


def test_copy_from_above():
    # Rows repeating the one above become one copy, also across row ends.
    row = [0, 3, 15, 15, 15, 9, 0, 0, 7, 15, 15, 1]
    raw = g4(row * 40, 12, 40)  # 39 rows to copy = 468 px > 330
    enc = roundtrip_glyph(raw, 12, 40)
    # row 0 as runs and literals, then copies of 330 and 138 (75 + 0x3F) pixels
    assert enc.hex() == "013f39027f21" + "00fff" + "00f3f"
    # The first row has nothing above it: a repeat there is not a copy.
    assert roundtrip_glyph(g4([1, 2, 3, 1, 2, 3], 6, 1), 6, 1).hex() == "123123"


@pytest.mark.parametrize("name,kind,enc,w,h,stream", fixture.MALFORMED)
def test_malformed_streams_rejected(name, kind, enc, w, h, stream):
    with pytest.raises(ValueError):
        if kind == fixture.KIND_GLYPH:
            rle.decode_glyph(stream, w, h)
        else:
            rle.decode_mask_any(enc, stream, w, h)


def test_best_glyph_never_grows():
    rng = random.Random(7)
    noisy = g4([rng.randrange(1, 15) for _ in range(64)], 8, 8)  # literals only
    enc, data = rle.best_glyph(noisy, 8, 8)
    assert enc == rle.GLYPH_RAW and data == noisy
    flat = g4([0] * 64, 8, 8)
    enc, data = rle.best_glyph(flat, 8, 8)
    assert enc == rle.GLYPH_RLE and len(data) < len(flat)


# ------------------------------------------------------------- mask runs

def m1(rows):
    raw, w, h = fixture._m(rows)
    return raw, w, h


def test_mask_spec_examples():
    raw, w, h = m1(["...##########...", "....##########.."])
    assert raw.hex() == "1ff80ffc"
    assert rle.encode_mask(raw, w, h).hex() == "3a7a"
    assert rle.best_mask(raw, w, h) == (rle.MASK_RUNS, bytes.fromhex("3a7a"))
    raw, w, h = m1([".##."] * 6)
    assert rle.encode_mask(raw, w, h, xor=True).hex() == "12"
    assert rle.best_mask(raw, w, h) == (rle.MASK_XOR_RUNS, b"\x12")


def test_mask_length_codes():
    for n in range(0, 286):
        codes = rle._mask_len(n)
        nb = rle._Nibbles(rle._nibbles_to_bytes(list(codes)))
        assert rle._read_mask_len(nb) == n
    with pytest.raises(ValueError):
        rle._mask_len(286)


@pytest.mark.parametrize("rows", [
    ["#"], ["."], ["########"] * 8, ["##.", ".##", "..#"] * 7, ["#" + "." * 127, "." * 128, "." * 127 + "#"],
    [".#.#.#.#.", "#.#.#.#.#"] * 3,
])
def test_mask_roundtrip(rows):
    raw, w, h = m1(rows)
    for xor in (False, True):
        enc = rle.encode_mask(raw, w, h, xor)
        assert rle.decode_mask(enc, w, h, xor) == raw
    enc, data = rle.best_mask(raw, w, h)
    assert rle.decode_mask_any(enc, data, w, h) == raw
    assert len(data) <= len(raw)


def test_mask_random_roundtrip():
    rng = random.Random(3)
    for _ in range(200):
        w, h = rng.randint(1, 40), rng.randint(1, 12)
        rows = ["".join(rng.choice("..#") for _ in range(w)) for _ in range(h)]
        test_mask_roundtrip(rows)


def test_mask_trailing_empty_pixels_are_implicit():
    raw, w, h = m1(["#......."])
    assert rle.encode_mask(raw, w, h).hex() == "01"  # 0 empty, 1 ink; 7 empty left out
    assert rle.decode_mask(b"", w, h) == b"\x00"  # an empty stream is an empty mask


# ------------------------------------------------------------ packs

def _cards():
    return [Card(0x65E5, ["day"], [], [], jlpt=5, strokes=4)]


def _kanji_glyphs():
    big = [0] * 30 + ([0, 0, 4, 15, 15, 15, 15, 11, 0, 0] * 18)
    return [Glyph(0x65E5, 0, 10, 21, 1, -22, 32, g4(big, 10, 21)),
            # 12 literal pixels: 6 bytes either way, so it stays raw
            Glyph(0x65E5, 1, 4, 3, 0, -3, 16, g4(list(range(1, 13)), 4, 3))]


def test_writer_picks_encoding_per_glyph():
    glyphs = _kanji_glyphs()
    raw = write_pack(_cards(), glyphs, "mini", "en", 32, 16, 12, 12, 10, build_time=1)
    p = read_pack(raw)
    assert p.errors == [] and p.header["version"] == 2
    big, small = p.find_glyph(0, 0x65E5), p.find_glyph(1, 0x65E5)
    assert big.encoding == rle.GLYPH_RLE and big.size < len(glyphs[0].data)
    assert small.encoding == rle.GLYPH_RAW and small.size == len(glyphs[1].data)
    assert p.raw_bitmap(big) == glyphs[0].data and p.raw_bitmap(small) == glyphs[1].data
    assert p.bitmap(big) == rle.unpack_4bpp_nibbles(glyphs[0].data, 10, 21)


def test_reader_rejects_corrupt_rle_and_v1_rle():
    raw = bytearray(write_pack(_cards(), _kanji_glyphs(), "mini", "en", 32, 16, 12, 12, 10,
                               build_time=1))
    p = read_pack(bytes(raw))
    big = p.find_glyph(0, 0x65E5)
    idx = p.glyphs.index(big)
    # A stream cut short by a run that overflows the bitmap.
    bad = bytearray(raw)
    bad[big.data_off] = 0x0F  # "0 F n n" = 75+ zeros; then garbage
    bad[big.data_off + 1] = 0xFF
    assert any("bad RLE stream" in e for e in read_pack(bytes(bad)).errors)
    # An unknown encoding.
    bad = bytearray(raw)
    bad[p.header["glyphs_off"] + idx * GLYPH_SIZE + 5] = 2
    assert any("unsupported encoding 2" in e for e in read_pack(bytes(bad)).errors)
    # Encoding 1 is not allowed in a v1 pack.
    bad = bytearray(raw)
    struct.pack_into("<H", bad, 4, 1)
    assert any("unsupported encoding 1 in a v1 pack" in e for e in read_pack(bytes(bad)).errors)
    # Unknown versions are refused outright.
    bad = bytearray(raw)
    struct.pack_into("<H", bad, 4, 3)
    with pytest.raises(ValueError, match="version"):
        read_pack(bytes(bad))


def test_stroke_masks_compressed_in_pack():
    vertical = StrokeMask(4, 2, 4, 20, bytes([0x60] * 20))
    spec, w, h = fixture._m(["...##########...", "....##########.."])  # runs: 3A 7A
    horizontal = StrokeMask(1, 10, w, h, spec)
    tiny = StrokeMask(0, 0, 1, 1, b"\x80")
    raw = write_pack(_cards(), _kanji_glyphs(), "mini", "en", 32, 16, 12, 12, 10, build_time=1,
                     strokes=[[vertical, horizontal, tiny]], strokes_px=32)
    p = read_pack(raw)
    assert p.errors == []
    st = p.strokes_of(0)
    assert [s.encoding for s in st] == [rle.MASK_XOR_RUNS, rle.MASK_RUNS, rle.MASK_RAW]
    assert [s.data for s in st] == [vertical.data, horizontal.data, tiny.data]
    assert st[0].size < 20 and st[1].size == 2 and st[2].size == 1


def test_v1_fixture_still_reads():
    # The last v1 mini.tgc, frozen: v1 packs on devices must keep working.
    p = read_pack((paths.FIXTURES / "mini_v1.tgc").read_bytes())
    assert p.errors == []
    assert p.header["version"] == 1
    assert all(g.encoding == 0 for g in p.glyphs)
    assert [len(p.strokes_of(i)) for i in range(3)] == [4, 4, 4]
    assert all(m.encoding == 0 and m.data_off % 4 == 0 for s in p.strokes for m in s)


@needs_klee
def test_v2_fixture_matches_v1_pixels():
    # Same font and inputs as the frozen v1 fixture: identical pixels and masks.
    v1 = read_pack((paths.FIXTURES / "mini_v1.tgc").read_bytes())
    raw = fixture.build_mini(paths.KLEE_ONE)
    v2 = read_pack(raw)
    assert v2.errors == [] and v2.header["version"] == 2
    assert [g.bits for g in v1.glyphs] == [g.bits for g in v2.glyphs]
    assert [[m.data for m in s] for s in v1.strokes] == [[m.data for m in s] for s in v2.strokes]
    assert sum(g.size for g in v2.glyphs) < sum(g.size for g in v1.glyphs)
    assert len(raw) < len(v1.raw)


@needs_font
def test_rle_vectors_are_up_to_date():
    committed = (paths.FIXTURES / "rle_vectors.bin").read_bytes()
    assert committed == fixture.rle_vectors(fixture.build_mini(paths.DEFAULT_FONT)), \
        "run `tangocho-pack fixture`"


def test_rle_vectors_decode():
    data = (paths.FIXTURES / "rle_vectors.bin").read_bytes()
    assert data[:4] == b"TRLV"
    fmt, count = struct.unpack_from("<HH", data, 4)
    assert fmt == 1
    pos, seen = 8, set()
    for _ in range(count):
        src, kind, enc, w, h, _r, ns, nr = struct.unpack_from("<BBBBBBHH", data, pos)
        pos += 10
        stored, plain = data[pos:pos + ns], data[pos + ns:pos + ns + nr]
        pos += ns + nr
        seen.add(src)
        if src == fixture.SRC_MALFORMED:
            with pytest.raises(ValueError):
                if kind == fixture.KIND_GLYPH:
                    rle.decode_glyph(stored, w, h)
                else:
                    rle.decode_mask_any(enc, stored, w, h)
        elif kind == fixture.KIND_GLYPH:
            got = rle.decode_glyph(stored, w, h)[0] if enc else stored
            assert got == plain
        else:
            assert rle.decode_mask_any(enc, stored, w, h) == plain
    assert pos == len(data) and seen == {0, 1, 2, 3}


@needs_klee
def test_visual_distractors_unchanged_by_compression():
    # Distractors are chosen from the raw bitmaps before the writer encodes them,
    # so a v2 pack has the same lists as the v1 fixture built from the same input.
    v1 = read_pack((paths.FIXTURES / "mini_v1.tgc").read_bytes())
    v2 = read_pack(fixture.build_mini(paths.KLEE_ONE))
    assert [g.bits for g in v1.glyphs] == [g.bits for g in v2.glyphs]
    assert [c.distractors for c in v1.cards] == [c.distractors for c in v2.cards]
    assert [c.distractors_reading for c in v1.cards] == [c.distractors_reading for c in v2.cards]
