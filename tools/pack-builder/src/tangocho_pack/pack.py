"""Pack v1 writer and reader. See docs/pack-format.md."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from .glyphs import Glyph, unpack_4bpp
from .kanjidic import Card

if TYPE_CHECKING:  # avoid a runtime import cycle (kanjivg imports paths only)
    from .kanjivg import StrokeMask

MAGIC = b"TNGC"
VERSION = 1
HEADER_SIZE = 64
CARD_SIZE = 32
GLYPH_SIZE = 16
HEADER_FMT = "<4sHHIIIIIIIII4sBBBBBBBBII"
CARD_FMT = "<IIIIIBBBBHHI"
GLYPH_FMT = "<IBBBBbbBBI"
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(CARD_FMT) == CARD_SIZE
assert struct.calcsize(GLYPH_FMT) == GLYPH_SIZE

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
) -> bytes:
    if len(cards) > 65535:
        raise ValueError("too many cards (u16 distractor indices)")
    if strokes is not None and len(strokes) != len(cards):
        raise ValueError("strokes must be index-aligned with cards")
    glyphs = sorted(glyphs, key=lambda g: (g.set, g.codepoint))

    strings = _Strings()
    deck_rel = strings.add(deck_name)
    card_rel = [tuple(strings.add(s) for s in card_strings(c)) for c in cards]

    cards_off = HEADER_SIZE
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
        off = glyph_data_off + len(data)
        gtable += struct.pack(
            GLYPH_FMT, g.codepoint, g.set, g.encoding, g.width, g.height,
            g.x_off, g.y_off, g.advance, 0, off,
        )
        data += g.data
    data += b"\0" * (align4(len(data)) - len(data))

    ctable = bytearray()
    for c, (m, o, k), d, dr in zip(cards, card_rel, dis_rel, dis_r_rel):
        ctable += struct.pack(
            CARD_FMT, c.codepoint, strings_off + m, strings_off + o, strings_off + k,
            distract_off + d, c.jlpt, c.grade, c.strokes, 1 if c.fallback_en else 0,
            c.freq, c.level, dr,
        )

    # Optional stroke section, appended after the glyph data (so packs built
    # without strokes stay byte-identical to v1 output).
    stroke_off = 0
    stroke_sec = b""
    if strokes is not None:
        stroke_off = align4(glyph_data_off + len(data))
        stroke_sec = _stroke_section(strokes, strokes_px, stroke_off)

    header = struct.pack(
        HEADER_FMT, MAGIC, VERSION, HEADER_SIZE, len(cards), len(glyphs),
        cards_off, glyphs_off, distract_off, strings_off, strings_size, glyph_data_off,
        strings_off + deck_rel, lang.encode("ascii").ljust(4, b"\0"),
        large_px, small_px, text_px, text_line_height, text_ascent,
        option_px, option_line_height, option_ascent,
        int(time.time()) if build_time is None else build_time, stroke_off,
    )
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
    return bytes(out)


def _stroke_section(card_strokes: list[list["StrokeMask"]], px: int, base_off: int) -> bytes:
    """Serialise the stroke section: index, blocks and 1bpp masks (4-byte aligned)."""
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
    mask_off: list[list[int]] = []
    for s in card_strokes:
        offs = []
        for m in s:
            cursor = align4(cursor)
            offs.append(cursor)
            cursor += len(m.data)
        mask_off.append(offs)

    sec = bytearray(struct.pack(f"<{n}I", *block_off))
    sec += b"\0" * (blocks_start - (base_off + len(sec)))
    for ci, s in enumerate(card_strokes):
        if not s:
            continue
        assert base_off + len(sec) == block_off[ci]
        sec += struct.pack("<HHI", len(s), px, 0)
        for off, m in zip(mask_off[ci], s):
            sec += struct.pack("<bbBBI", m.x_off, m.y_off, m.w, m.h, off)
    for ci, s in enumerate(card_strokes):
        for off, m in zip(mask_off[ci], s):
            sec += b"\0" * (off - (base_off + len(sec)))
            sec += m.data
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


@dataclass
class PStroke:
    x_off: int
    y_off: int
    w: int
    h: int
    data_off: int
    data: bytes = b""


@dataclass
class Pack:
    raw: bytes
    header: dict
    deck_name: str
    cards: list[PCard] = field(default_factory=list)
    glyphs: list[PGlyph] = field(default_factory=list)
    strokes: list[list[PStroke]] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

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

    def bitmap(self, g: PGlyph) -> list[int]:
        n = ((g.width + 1) // 2) * g.height
        return unpack_4bpp(self.raw[g.data_off : g.data_off + n], g.width, g.height)


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
    if h["version"] != VERSION:
        raise ValueError(f"unsupported version {h['version']}")
    if h["header_size"] != HEADER_SIZE:
        errs.append(f"header_size {h['header_size']} != 64")
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
        if enc != 0:
            errs.append(f"glyph {i}: unsupported encoding {enc}")
        elif w and ht:
            n = ((w + 1) // 2) * ht
            if doff < h["glyph_data_off"] or doff + n > size:
                errs.append(f"glyph {i} (U+{cp:04X} set {gs}): bitmap out of range")
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
                cnt, px, _res = struct.unpack_from("<HHI", raw, blk)
                if px == 0 or px != h["large_px"]:
                    errs.append(f"card {i}: stroke block px {px} != large_px {h['large_px']}")
                if blk + 8 + 8 * cnt > size:
                    errs.append(f"card {i}: {cnt} stroke entries out of range")
                    continue
                for k in range(cnt):
                    xo, yo, w, ht, doff = struct.unpack_from("<bbBBI", raw, blk + 8 + 8 * k)
                    if w == 0 or ht == 0:
                        errs.append(f"card {i} stroke {k}: empty mask")
                        continue
                    nbytes = ((w + 7) // 8) * ht
                    if doff + nbytes > size:
                        errs.append(f"card {i} stroke {k}: mask out of range")
                        continue
                    pack.strokes[i].append(
                        PStroke(xo, yo, w, ht, doff, raw[doff:doff + nbytes]))

    present = {(g.set, g.codepoint) for g in pack.glyphs}
    for i, c in enumerate(pack.cards):
        for s in (0, 1):
            if (s, c.codepoint) not in present:
                errs.append(f"card {i} U+{c.codepoint:04X}: missing set {s} glyph")
        for what, txt in (("meanings", c.meanings), ("onyomi", c.onyomi), ("kunyomi", c.kunyomi)):
            for ch in txt:
                if 0x20 <= ord(ch) <= 0x7E:
                    continue
                if (2, ord(ch)) not in present:
                    errs.append(f"card {i} {what}: U+{ord(ch):04X} {ch!r} missing from set 2")
    for ch in pack.deck_name:
        if not 0x20 <= ord(ch) <= 0x7E and (2, ord(ch)) not in present:
            errs.append(f"deck name: U+{ord(ch):04X} missing from set 2")
    # If the pack ships large option text, every reading character must be in it.
    if h["option_px"]:
        for i, c in enumerate(pack.cards):
            for what, txt in (("onyomi", c.onyomi), ("kunyomi", c.kunyomi)):
                for ch in txt:
                    if (3, ord(ch)) not in present:
                        errs.append(f"card {i} {what}: U+{ord(ch):04X} {ch!r} missing from set 3")
    return pack
