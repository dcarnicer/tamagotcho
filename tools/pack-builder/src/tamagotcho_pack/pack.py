"""Pack writer (v2, or v3 with example words) and reader (v1 to v3). See
docs/pack-format.md."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from . import rle
from .glyphs import Glyph, unpack_4bpp
from .kanjidic import Card

if TYPE_CHECKING:  # avoid a runtime import cycle (kanjivg imports paths only)
    from .kanjivg import StrokeMask
    from .words import ExampleWord

MAGIC = b"TNGC"
VERSION = 2  # what the writer emits without example words
VERSION_WORDS = 3  # ... and with them (the header extension and the words section)
READ_VERSIONS = (1, 2, 3)  # what the reader accepts (devices keep their v1 and v2 packs)
HEADER_SIZE = 64
HEADER_SIZE_V3 = 80  # the 64 bytes, then words_off and three reserved u32
HEADER_EXT_FMT = "<IIII"  # v3: words_off, reserved x3
WORDS_HEADER_FMT = "<HHI"  # entry_size, reserved, word_count
WORD_ENTRY_FMT = "<IIIBBBB"  # word, reading, gloss, kind, reserved, reading_start, reading_len
WORD_ENTRY_SIZE = 16
CARD_SIZE = 32
GLYPH_SIZE = 16
HEADER_FMT = "<4sHHIIIIIIIII4sBBBBBBBBII"
CARD_FMT = "<IIIIIBBBBHHI"
GLYPH_FMT = "<IBBBBbbBBI"
STROKE_ENTRY_FMT = "<bbBBBBH"  # v2: x_off, y_off, w, h, encoding, reserved, size
STROKE_ENTRY_V1_FMT = "<bbBBI"  # v1: x_off, y_off, w, h, data_off
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(CARD_FMT) == CARD_SIZE
assert struct.calcsize(GLYPH_FMT) == GLYPH_SIZE
assert HEADER_SIZE + struct.calcsize(HEADER_EXT_FMT) == HEADER_SIZE_V3
assert struct.calcsize(WORD_ENTRY_FMT) == WORD_ENTRY_SIZE

MEANING_SEP = "; "
READING_SEP = "、"


def align4(n: int) -> int:
    return (n + 3) & ~3


class _Strings:
    def __init__(self) -> None:
        self.buf = bytearray()
        self.index: dict[str, int] = {}

    def add(self, s: str) -> int:
        """Pool-relative offset (deduplicated)."""
        if s not in self.index:
            self.index[s] = len(self.buf)
            self.buf += s.encode("utf-8") + b"\0"
        return self.index[s]


def on_hiragana(s: str) -> str:
    """An on'yomi as the device writes it in hiragana (Settings > Study >
    On'yomi): katakana ァ..ヶ and ヽヾ shifted to hiragana, anything else (ー)
    unchanged. Packs carry these glyphs too (docs/pack-format.md)."""
    return "".join(chr(ord(ch) - 0x60) if "ァ" <= ch <= "ヶ" or ch in "ヽヾ" else ch for ch in s)


def card_strings(c: Card) -> tuple[str, str, str]:
    return (
        MEANING_SEP.join(c.meanings),
        READING_SEP.join(c.onyomi),
        READING_SEP.join(c.kunyomi),
    )


def write_pack(
    cards: list[Card],
    glyphs: list[Glyph],
    deck_name: str,
    lang: str,
    large_px: int,
    small_px: int,
    text_px: int,
    text_line_height: int,
    text_ascent: int,
    option_px: int = 0,
    option_line_height: int = 0,
    option_ascent: int = 0,
    build_time: int | None = None,
    strokes: list[list["StrokeMask"]] | None = None,
    strokes_px: int = 0,
    words: list[list["ExampleWord"]] | None = None,
) -> bytes:
    """The pack's bytes: v2, or v3 when `words` (aligned with `cards`, each
    card's example words, possibly none) is given."""
    if len(cards) > 65535:
        raise ValueError("too many cards (u16 distractor indices)")
    if strokes is not None:
        # The stroke block stores x_off/y_off as i8, so a mask placed anywhere in
        # the em square needs large_px <= 128; the reader also rejects a block
        # whose px disagrees with the header, hence the two checks.
        if large_px > 128:
            raise ValueError(f"large_px ({large_px}) > 128 cannot carry stroke masks: "
                             "stroke x_off/y_off are signed bytes (max 127)")
        if strokes_px != large_px:
            raise ValueError(f"strokes_px ({strokes_px}) must equal large_px ({large_px}): "
                             "the masks share the em square the glyphs use")
        if len(strokes) != len(cards):
            raise ValueError("strokes must be index-aligned with cards")
    if words is not None and len(words) != len(cards):
        raise ValueError("words must be index-aligned with cards")
    glyphs = sorted(glyphs, key=lambda g: (g.set, g.codepoint))

    strings = _Strings()
    deck_rel = strings.add(deck_name)
    card_rel = [tuple(strings.add(s) for s in card_strings(c)) for c in cards]
    # v3: the words' strings join the pool after every card's (so a v2 pack's
    # pool is unchanged), deduplicated with them.
    word_rel = [[tuple(strings.add(s) for s in (w.text, w.reading, w.gloss)) for w in ws]
                for ws in words] if words is not None else None

    header_size = HEADER_SIZE if words is None else HEADER_SIZE_V3
    cards_off = header_size
    glyphs_off = cards_off + CARD_SIZE * len(cards)
    distract_off = glyphs_off + GLYPH_SIZE * len(glyphs)
    dis = bytearray()
    dis_rel = []
    for c in cards:
        dis_rel.append(len(dis))
        dis += struct.pack(f"<H{len(c.distractors)}H", len(c.distractors), *c.distractors)
    read_off = distract_off + len(dis)
    dis_r = bytearray()
    dis_r_rel = []
    for c in cards:
        if c.distractors_reading:
            dis_r_rel.append(read_off + len(dis_r))
            dis_r += struct.pack(f"<H{len(c.distractors_reading)}H", len(c.distractors_reading),
                                 *c.distractors_reading)
        else:
            dis_r_rel.append(0)  # no separate list -> use the meaning one
    strings_off = align4(read_off + len(dis_r))
    strings_size = len(strings.buf)
    glyph_data_off = align4(strings_off + strings_size)

    data = bytearray()
    gtable = bytearray()
    for g in glyphs:
        if g.encoding != rle.GLYPH_RAW or len(g.data) != rle.raw4_size(g.width, g.height):
            raise ValueError(f"glyph U+{g.codepoint:04X} set {g.set}: expected a raw 4bpp bitmap")
        # Per glyph, whichever is smaller: RLE (encoding 1) or raw (encoding 0).
        enc, blob = rle.best_glyph(g.data, g.width, g.height)
        off = glyph_data_off + len(data)
        gtable += struct.pack(
            GLYPH_FMT, g.codepoint, g.set, enc, g.width, g.height,
            g.x_off, g.y_off, g.advance, 0, off,
        )
        data += blob
    data += b"\0" * (align4(len(data)) - len(data))

    ctable = bytearray()
    for c, (m, o, k), d, dr in zip(cards, card_rel, dis_rel, dis_r_rel):
        ctable += struct.pack(
            CARD_FMT, c.codepoint, strings_off + m, strings_off + o, strings_off + k,
            distract_off + d, c.jlpt, c.grade, c.strokes, 1 if c.fallback_en else 0,
            c.freq, c.level, dr,
        )

    # Optional stroke section, appended after the glyph data.
    stroke_off = 0
    stroke_sec = b""
    if strokes is not None:
        stroke_off = align4(glyph_data_off + len(data))
        stroke_sec = _stroke_section(strokes, strokes_px, stroke_off)

    # Optional example words (v3), after everything else.
    words_off = 0
    words_sec = b""
    if word_rel is not None:
        words_off = align4(glyph_data_off + len(data) + len(stroke_sec))
        words_sec = _words_section(words, word_rel, strings_off)

    header = struct.pack(
        HEADER_FMT, MAGIC, VERSION if words is None else VERSION_WORDS, header_size,
        len(cards), len(glyphs),
        cards_off, glyphs_off, distract_off, strings_off, strings_size, glyph_data_off,
        strings_off + deck_rel, lang.encode("ascii").ljust(4, b"\0"),
        large_px, small_px, text_px, text_line_height, text_ascent,
        option_px, option_line_height, option_ascent,
        int(time.time()) if build_time is None else build_time, stroke_off,
    )
    if words is not None:
        header += struct.pack(HEADER_EXT_FMT, words_off, 0, 0, 0)
    out = bytearray(header)
    out += ctable
    out += gtable
    out += dis
    out += dis_r
    out += b"\0" * (strings_off - len(out))
    out += strings.buf
    out += b"\0" * (glyph_data_off - len(out))
    out += data
    out += stroke_sec
    if words_sec:
        out += b"\0" * (words_off - len(out))
        out += words_sec
    return bytes(out)


def _words_section(words: list[list["ExampleWord"]], rel: list[list[tuple[int, int, int]]],
                   strings_off: int) -> bytes:
    """Serialise the v3 example-words section: header, the card word index
    (card_count + 1 entries), then the word table."""
    count = sum(len(ws) for ws in words)
    sec = bytearray(struct.pack(WORDS_HEADER_FMT, WORD_ENTRY_SIZE, 0, count))
    first = 0
    for ws in words:
        sec += struct.pack("<I", first)
        first += len(ws)
    sec += struct.pack("<I", first)
    for ws, rs in zip(words, rel):
        if [w.kind for w in ws] != sorted(set(w.kind for w in ws)):
            raise ValueError("a card's words must be one per kind at most, on'yomi first")
        for w, (t, r, g) in zip(ws, rs):
            start, length = w.span
            if not (0 <= start <= 255 and 0 <= length <= 255) or (length == 0 and start):
                raise ValueError(f"word {w.text}: bad reading span {w.span}")
            sec += struct.pack(WORD_ENTRY_FMT, strings_off + t, strings_off + r, strings_off + g,
                               w.kind, 0, start, length)
    return bytes(sec)


def _stroke_section(card_strokes: list[list["StrokeMask"]], px: int, base_off: int) -> bytes:
    """Serialise the v2 stroke section: index, blocks, then each card's masks back
    to back (each in the smallest of mask encodings 0, 1, 2)."""
    n = len(card_strokes)
    blocks_start = align4(base_off + 4 * n)
    block_off: list[int] = []
    cursor = blocks_start
    for s in card_strokes:
        if not s:
            block_off.append(0)
            continue
        block_off.append(cursor)
        cursor += 8 + 8 * len(s)
    masks_start = align4(cursor)
    encoded = [[rle.best_mask(m.data, m.w, m.h) for m in s] for s in card_strokes]
    first_off: list[int] = []
    cursor = masks_start
    for e in encoded:
        first_off.append(cursor)
        cursor += sum(len(d) for _, d in e)

    sec = bytearray(struct.pack(f"<{n}I", *block_off))
    sec += b"\0" * (blocks_start - (base_off + len(sec)))
    for ci, s in enumerate(card_strokes):
        if not s:
            continue
        assert base_off + len(sec) == block_off[ci]
        sec += struct.pack("<HHI", len(s), px, first_off[ci])
        for m, (enc, d) in zip(s, encoded[ci]):
            sec += struct.pack(STROKE_ENTRY_FMT, m.x_off, m.y_off, m.w, m.h, enc, 0, len(d))
    sec += b"\0" * (masks_start - (base_off + len(sec)))
    for e in encoded:
        for _, d in e:
            sec += d
    sec += b"\0" * (align4(len(sec)) - len(sec))
    return bytes(sec)


# ---------------------------------------------------------------- reader

@dataclass
class PCard:
    codepoint: int
    meanings: str
    onyomi: str
    kunyomi: str
    distractor_off: int
    distractors: list[int]
    jlpt: int
    grade: int
    strokes: int
    flags: int
    freq: int
    level: int
    distractor_read_off: int = 0
    distractors_reading: list[int] = field(default_factory=list)
    string_offs: tuple[int, int, int] = (0, 0, 0)


@dataclass
class PGlyph:
    codepoint: int
    set: int
    encoding: int
    width: int
    height: int
    x_off: int
    y_off: int
    advance: int
    data_off: int
    size: int = 0  # bytes of stored (possibly encoded) data
    bits: bytes = field(default=b"", repr=False)  # decoded raw 4bpp bitmap


@dataclass
class PStroke:
    x_off: int
    y_off: int
    w: int
    h: int
    data_off: int
    data: bytes = b""  # decoded raw 1bpp mask
    encoding: int = 0
    size: int = 0  # bytes of stored (possibly encoded) data


@dataclass
class PWord:
    """An example word of a v3 pack (docs/pack-format.md)."""
    kind: int          # 0 = for the asked on'yomi, 1 = for the asked kun'yomi
    word: str
    reading: str
    gloss: str
    reading_start: int
    reading_len: int
    string_offs: tuple[int, int, int] = (0, 0, 0)


@dataclass
class Pack:
    raw: bytes
    header: dict
    deck_name: str
    cards: list[PCard] = field(default_factory=list)
    glyphs: list[PGlyph] = field(default_factory=list)
    strokes: list[list[PStroke]] = field(default_factory=list)
    words: list[list[PWord]] = field(default_factory=list)  # per card; empty lists without
    errors: list[str] = field(default_factory=list)

    def words_of(self, card_idx: int) -> list[PWord]:
        """Example words of a card (empty when the pack carries none for it)."""
        return self.words[card_idx] if 0 <= card_idx < len(self.words) else []

    def find_card(self, cp: int) -> int | None:
        for i, c in enumerate(self.cards):
            if c.codepoint == cp:
                return i
        return None

    def strokes_of(self, card_idx: int) -> list[PStroke]:
        """Stroke masks of a card (empty when the pack carries none for it)."""
        return self.strokes[card_idx] if 0 <= card_idx < len(self.strokes) else []

    def find_glyph(self, gset: int, cp: int) -> PGlyph | None:
        lo, hi = 0, len(self.glyphs)
        key = (gset, cp)
        while lo < hi:  # binary search, as the device does
            mid = (lo + hi) // 2
            g = self.glyphs[mid]
            if (g.set, g.codepoint) < key:
                lo = mid + 1
            else:
                hi = mid
        if lo < len(self.glyphs) and (self.glyphs[lo].set, self.glyphs[lo].codepoint) == key:
            return self.glyphs[lo]
        return None

    def raw_bitmap(self, g: PGlyph) -> bytes:
        """Decoded bitmap in the encoding 0 layout (empty if it could not be read)."""
        return g.bits

    def bitmap(self, g: PGlyph) -> list[int]:
        return unpack_4bpp(g.bits, g.width, g.height) if g.bits else [0] * (g.width * g.height)


def _read_words(raw: bytes, h: dict, pack: Pack, string, errs: list[str]) -> None:
    """Parse and validate the v3 example-words section into pack.words."""
    size, cc, wo = len(raw), h["card_count"], h["words_off"]
    if wo % 4:
        errs.append(f"words_off {wo} not 4-byte aligned")
        return
    if wo + 8 + 4 * (cc + 1) > size:
        errs.append(f"words section header and index [{wo}, {wo + 8 + 4 * (cc + 1)}) "
                    f"exceed file size {size}")
        return
    entry_size, reserved, count = struct.unpack_from(WORDS_HEADER_FMT, raw, wo)
    if entry_size < WORD_ENTRY_SIZE:
        errs.append(f"words section: entry_size {entry_size} < {WORD_ENTRY_SIZE}")
        return
    if reserved:
        errs.append("words section: reserved field not 0")
    first = list(struct.unpack_from(f"<{cc + 1}I", raw, wo + 8))
    table = wo + 8 + 4 * (cc + 1)
    if first[0] != 0 or first[-1] != count or any(b < a for a, b in zip(first, first[1:])):
        errs.append("words section: the card word index is not 0..word_count, non-decreasing")
        return
    if table + entry_size * count > size:
        errs.append(f"words section: {count} entries exceed file size {size}")
        return
    for i in range(cc):
        ws = []
        for k in range(first[i], first[i + 1]):
            t, r, g, kind, res, start, length = struct.unpack_from(
                WORD_ENTRY_FMT, raw, table + k * entry_size)
            where = f"card {i} word {k - first[i]}"
            w = PWord(kind, string(t, f"{where} word"), string(r, f"{where} reading"),
                      string(g, f"{where} gloss"), start, length, (t, r, g))
            if kind > 1:
                errs.append(f"{where}: bad kind {kind}")
            if res:
                errs.append(f"{where}: reserved byte not 0")
            if not w.word or not w.reading:
                errs.append(f"{where}: empty word or reading")
            rb = w.reading.encode("utf-8")
            if length == 0 and start:
                errs.append(f"{where}: reading_start {start} with no length")
            if length:
                try:
                    rb[start:start + length].decode("utf-8")
                    rb[:start].decode("utf-8")
                    ok = start + length <= len(rb)
                except UnicodeDecodeError:
                    ok = False
                if not ok:
                    errs.append(f"{where}: reading span {start}+{length} not inside "
                                f"{w.reading!r} on character boundaries")
            if i < len(pack.cards) and chr(pack.cards[i].codepoint) not in w.word:
                errs.append(f"{where}: {w.word!r} does not contain the card's kanji")
            ws.append(w)
        kinds = [w.kind for w in ws]
        if kinds != sorted(set(kinds)):
            errs.append(f"card {i}: more than one word of a kind, or kun'yomi before on'yomi")
        pack.words[i] = ws


def _cstr(raw: bytes, off: int) -> str:
    end = raw.index(b"\0", off)
    return raw[off:end].decode("utf-8")


def read_pack(raw: bytes) -> Pack:
    """Parse and validate. Structural problems are collected in `.errors`."""
    errs: list[str] = []
    if len(raw) < HEADER_SIZE:
        raise ValueError("file shorter than header")
    f = struct.unpack_from(HEADER_FMT, raw, 0)
    names = ["magic", "version", "header_size", "card_count", "glyph_count", "cards_off",
             "glyphs_off", "distract_off", "strings_off", "strings_size", "glyph_data_off",
             "deck_name", "lang", "large_px", "small_px", "text_px", "text_line_height",
             "text_ascent", "option_px", "option_line_height", "option_ascent",
             "build_time", "stroke_off"]
    h = dict(zip(names, f))
    h["lang"] = h["lang"].rstrip(b"\0").decode("ascii", "replace")
    if h["magic"] != MAGIC:
        raise ValueError(f"bad magic {h['magic']!r}")
    if h["version"] not in READ_VERSIONS:
        raise ValueError(f"unsupported version {h['version']}")
    v2 = h["version"] >= 2
    v3 = h["version"] >= 3
    h["words_off"] = 0
    if v3:
        # The header extension: a v3 header is at least 80 bytes.
        if h["header_size"] < HEADER_SIZE_V3 or h["header_size"] > len(raw):
            errs.append(f"header_size {h['header_size']} < {HEADER_SIZE_V3} in a v3 pack")
        else:
            words_off, *reserved = struct.unpack_from(HEADER_EXT_FMT, raw, HEADER_SIZE)
            h["words_off"] = words_off
            if any(reserved):
                errs.append("header extension: reserved fields not 0")
    elif h["header_size"] != HEADER_SIZE:
        errs.append(f"header_size {h['header_size']} != 64")
    if h["cards_off"] < h["header_size"]:
        errs.append(f"cards_off {h['cards_off']} inside the {h['header_size']}-byte header")
    size = len(raw)
    cc, gc = h["card_count"], h["glyph_count"]

    def section(name: str, off: int, length: int) -> None:
        if off % 4:
            errs.append(f"{name} offset {off} not 4-byte aligned")
        if off + length > size:
            errs.append(f"{name} [{off}, {off + length}) exceeds file size {size}")

    section("cards", h["cards_off"], cc * CARD_SIZE)
    section("glyphs", h["glyphs_off"], gc * GLYPH_SIZE)
    section("distractors", h["distract_off"], 0)
    section("strings", h["strings_off"], h["strings_size"])
    section("glyph_data", h["glyph_data_off"], 0)
    if errs:
        return Pack(raw, h, "", errors=errs)

    s_lo, s_hi = h["strings_off"], h["strings_off"] + h["strings_size"]

    def string(off: int, what: str) -> str:
        if not (s_lo <= off < s_hi):
            errs.append(f"{what}: string offset {off} outside string pool")
            return ""
        try:
            return _cstr(raw, off)
        except (ValueError, UnicodeDecodeError):
            errs.append(f"{what}: unterminated or invalid UTF-8 string at {off}")
            return ""

    pack = Pack(raw, h, string(h["deck_name"], "deck_name"), errors=errs)
    d_lo = h["distract_off"]

    def read_list(off: int, what: str) -> list[int]:
        if off == 0:
            return []
        if not (d_lo <= off and off + 2 <= size):
            errs.append(f"{what}: distractor offset {off} out of range")
            return []
        (n,) = struct.unpack_from("<H", raw, off)
        if off + 2 + 2 * n > h["strings_off"]:
            errs.append(f"{what}: distractor list overruns section")
            return []
        dl = list(struct.unpack_from(f"<{n}H", raw, off + 2))
        if any(x >= cc for x in dl):
            errs.append(f"{what}: distractor index out of range")
        return dl

    for i in range(cc):
        t = struct.unpack_from(CARD_FMT, raw, h["cards_off"] + i * CARD_SIZE)
        cp, mo, oo, ko, do, jlpt, grade, strokes, flags, freq, level, dro = t
        dl = read_list(do, f"card {i} meaning")
        if i in dl:
            errs.append(f"card {i}: lists itself as a meaning distractor")
        dlr = read_list(dro, f"card {i} reading")
        if i in dlr:
            errs.append(f"card {i}: lists itself as a reading distractor")
        pack.cards.append(PCard(
            cp, string(mo, f"card {i} meanings"), string(oo, f"card {i} onyomi"),
            string(ko, f"card {i} kunyomi"), do, dl, jlpt, grade, strokes, flags, freq,
            level, dro, dlr, (mo, oo, ko),
        ))
    prev = None
    for i in range(gc):
        t = struct.unpack_from(GLYPH_FMT, raw, h["glyphs_off"] + i * GLYPH_SIZE)
        cp, gs, enc, w, ht, xo, yo, adv, _, doff = t
        g = PGlyph(cp, gs, enc, w, ht, xo, yo, adv, doff)
        if prev is not None and (gs, cp) <= prev:
            errs.append(f"glyph table not strictly sorted at index {i}")
        prev = (gs, cp)
        if gs > 3:
            errs.append(f"glyph {i}: bad set {gs}")
        n = rle.raw4_size(w, ht)
        where = f"glyph {i} (U+{cp:04X} set {gs})"
        if enc == rle.GLYPH_RAW:
            g.size = n
            if n and (doff < h["glyph_data_off"] or doff + n > size):
                errs.append(f"{where}: bitmap out of range")
            else:
                g.bits = raw[doff:doff + n]
        elif enc == rle.GLYPH_RLE and v2:
            if not n:
                errs.append(f"{where}: RLE glyph without pixels")
            elif doff < h["glyph_data_off"] or doff >= size:
                errs.append(f"{where}: bitmap out of range")
            else:
                # The stream may not be longer than the raw bitmap, so a reader
                # can fetch at most that many bytes (clamped to the file end).
                try:
                    g.bits, g.size = rle.decode_glyph(raw[doff:min(doff + n, size)], w, ht)
                except ValueError as e:
                    errs.append(f"{where}: bad RLE stream: {e}")
        else:
            errs.append(f"{where}: unsupported encoding {enc} in a v{h['version']} pack")
        pack.glyphs.append(g)

    # Stroke section (optional): validate the index, blocks and masks.
    pack.strokes = [[] for _ in range(cc)]
    so = h["stroke_off"]
    if so:
        if so % 4:
            errs.append(f"stroke_off {so} not 4-byte aligned")
        elif so + 4 * cc > size:
            errs.append(f"stroke index [{so}, {so + 4 * cc}) exceeds file size {size}")
        else:
            for i, blk in enumerate(struct.unpack_from(f"<{cc}I", raw, so)):
                if blk == 0:
                    continue
                if blk % 4 or blk + 8 > size:
                    errs.append(f"card {i}: stroke block offset {blk} out of range")
                    continue
                cnt, px, first = struct.unpack_from("<HHI", raw, blk)
                if px == 0 or px != h["large_px"]:
                    errs.append(f"card {i}: stroke block px {px} != large_px {h['large_px']}")
                if blk + 8 + 8 * cnt > size:
                    errs.append(f"card {i}: {cnt} stroke entries out of range")
                    continue
                cursor = first  # v2: masks back to back from the block's data_off
                for k in range(cnt):
                    where = f"card {i} stroke {k}"
                    if v2:
                        xo, yo, w, ht, enc, _r, n = struct.unpack_from(
                            STROKE_ENTRY_FMT, raw, blk + 8 + 8 * k)
                        doff, cursor = cursor, cursor + n
                    else:
                        xo, yo, w, ht, doff = struct.unpack_from(
                            STROKE_ENTRY_V1_FMT, raw, blk + 8 + 8 * k)
                        enc, n = rle.MASK_RAW, rle.raw1_size(w, ht)
                    if w == 0 or ht == 0:
                        errs.append(f"{where}: empty mask")
                        continue
                    nraw = rle.raw1_size(w, ht)
                    if doff + n > size:
                        errs.append(f"{where}: mask out of range")
                        continue
                    if enc == rle.MASK_RAW and n != nraw:
                        errs.append(f"{where}: raw mask size {n} != {nraw}")
                        continue
                    if n > nraw:
                        errs.append(f"{where}: encoded mask ({n} B) larger than raw ({nraw} B)")
                        continue
                    try:
                        bits = rle.decode_mask_any(enc, raw[doff:doff + n], w, ht)
                    except ValueError as e:
                        errs.append(f"{where}: bad mask data (encoding {enc}): {e}")
                        continue
                    pack.strokes[i].append(PStroke(xo, yo, w, ht, doff, bits, enc, n))

    # Example words (v3, optional): the index, every entry and its strings.
    pack.words = [[] for _ in range(cc)]
    wo = h["words_off"]
    if wo:
        _read_words(raw, h, pack, string, errs)

    present = {(g.set, g.codepoint) for g in pack.glyphs}

    # Every reading string a set must draw: v2 packs also carry each on'yomi in
    # hiragana. v1 packs predate the setting that writes them so (a device
    # draws a box for a missing glyph).
    def readings(c: PCard) -> tuple[tuple[str, str], ...]:
        both = (("onyomi", c.onyomi), ("kunyomi", c.kunyomi))
        return both + ((("onyomi in hiragana", on_hiragana(c.onyomi)),) if v2 else ())

    for i, c in enumerate(pack.cards):
        for s in (0, 1):
            if (s, c.codepoint) not in present:
                errs.append(f"card {i} U+{c.codepoint:04X}: missing set {s} glyph")
        for what, txt in (("meanings", c.meanings),) + readings(c):
            for ch in txt:
                if 0x20 <= ord(ch) <= 0x7E:
                    continue
                if (2, ord(ch)) not in present:
                    errs.append(f"card {i} {what}: U+{ord(ch):04X} {ch!r} missing from set 2")
        # A reading list is empty, or has no empty item and a first reading
        # that is more than okurigana dots (the device asks the first one).
        for what, txt in (("onyomi", c.onyomi), ("kunyomi", c.kunyomi)):
            items = txt.split(READING_SEP) if txt else []
            if any(not r.replace(".", "") for r in items):
                errs.append(f"card {i} {what}: empty reading in {txt!r}")
    for ch in pack.deck_name:
        if not 0x20 <= ord(ch) <= 0x7E and (2, ord(ch)) not in present:
            errs.append(f"deck name: U+{ord(ch):04X} missing from set 2")
    # Every character of the example words is drawn from set 2.
    for i, ws in enumerate(pack.words):
        for w in ws:
            for what, txt in (("word", w.word), ("reading", w.reading), ("gloss", w.gloss)):
                for ch in txt:
                    if not 0x20 <= ord(ch) <= 0x7E and (2, ord(ch)) not in present:
                        errs.append(f"card {i} example {what} {txt!r}: U+{ord(ch):04X} "
                                    "missing from set 2")
    # If the pack ships large option text, every reading character must be in it.
    if h["option_px"]:
        for i, c in enumerate(pack.cards):
            for what, txt in readings(c):
                for ch in txt:
                    if (3, ord(ch)) not in present:
                        errs.append(f"card {i} {what}: U+{ord(ch):04X} {ch!r} missing from set 3")
    return pack
