"""Pack v2 bitmap compression (docs/pack-format.md, "Bitmap encoding 1" and
"Stroke mask encodings").

Both schemes write a stream of 4-bit codes packed two per byte, high nibble
first (the same order as the raw 4bpp pixels), and both read the bitmap as one
row-major pixel sequence that runs on from the end of a row into the next.

- Glyphs (4bpp, encoding 1): a code 1..14 is one pixel of that value; ``0 L``
  and ``F L`` are runs of 0 (background) and 15 (full ink); ``0 0 L`` copies
  ``L`` pixels from the row above. Length codes are 1-3 nibbles (1..330).
- Stroke masks (1bpp, encodings 1 and 2): alternating run lengths, empty
  first; encoding 2 codes each row XOR the row above (vertical strokes become
  a few runs). Lengths are 1-3 nibbles (0..285).

The encoders are greedy (one pass, no tables) and the decoders are the
reference for the firmware's ``pack/Rle.cpp``; both sides are cross-checked
with ``fixtures/rle_vectors.bin`` (``tamagotcho-pack fixture``).
"""

from __future__ import annotations

from collections.abc import Sequence

# Glyph encodings (glyph table `encoding` byte).
GLYPH_RAW, GLYPH_RLE = 0, 1
# Stroke mask encodings (v2 stroke entry `encoding` byte).
MASK_RAW, MASK_RUNS, MASK_XOR_RUNS = 0, 1, 2

GLYPH_MAX_LEN = 330  # longest run/copy one glyph token can carry
MASK_MAX_LEN = 285   # longest run one mask length code can carry
_COPY_MIN = 3        # shorter copies never beat literals or a run


def raw4_size(w: int, h: int) -> int:
    return ((w + 1) // 2) * h


def raw1_size(w: int, h: int) -> int:
    return ((w + 7) // 8) * h


def _nibbles_to_bytes(nib: list[int]) -> bytes:
    if len(nib) % 2:
        nib.append(0)  # padding: ignored by the glyph decoder, a 0-length run for masks
    return bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))


class _Nibbles:
    """Sequential nibble reader over a byte string."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # in nibbles

    def at_end(self) -> bool:
        return self.pos >= 2 * len(self.data)

    def next(self) -> int:
        if self.at_end():
            raise ValueError("truncated stream")
        b = self.data[self.pos >> 1]
        n = b & 15 if self.pos & 1 else b >> 4
        self.pos += 1
        return n

    def bytes_used(self) -> int:
        return (self.pos + 1) // 2


def unpack_4bpp_nibbles(data: bytes, w: int, h: int) -> list[int]:
    stride = (w + 1) // 2
    out: list[int] = []
    for y in range(h):
        row = data[y * stride:(y + 1) * stride]
        for x in range(w):
            b = row[x >> 1]
            out.append(b & 15 if x & 1 else b >> 4)
    return out


def pack_4bpp_nibbles(pix: Sequence[int], w: int, h: int) -> bytes:
    stride = (w + 1) // 2
    out = bytearray(stride * h)
    for y in range(h):
        base = y * stride
        for x in range(w):
            v = pix[y * w + x]
            if v:
                out[base + (x >> 1)] |= v if x & 1 else v << 4
    return bytes(out)


# ------------------------------------------------------------ glyph (4bpp)

def _glyph_len(n: int) -> list[int]:
    """Length code for 1..330: 1..10 in one nibble, 11..74 in two, 75..330 in three.
    The first nibble is never 0 (``0 0`` introduces a copy)."""
    if not 1 <= n <= GLYPH_MAX_LEN:
        raise ValueError(f"glyph run length {n} out of range")
    if n <= 10:
        return [n]
    if n <= 74:
        m = n - 11
        return [11 + (m >> 4), m & 15]
    m = n - 75
    return [15, m >> 4, m & 15]


def _read_glyph_len(nb: _Nibbles, c: int) -> int:
    if c <= 10:
        return c
    if c <= 14:
        return 11 + ((c - 11) << 4) + nb.next()
    hi = nb.next()
    return 75 + (hi << 4) + nb.next()


def encode_glyph(raw: bytes, w: int, h: int) -> bytes:
    """Encode a raw 4bpp bitmap (encoding 0 layout) as encoding 1."""
    pix = unpack_4bpp_nibbles(raw, w, h)
    n = len(pix)
    nib: list[int] = []
    i = 0
    while i < n:
        v = pix[i]
        run = 1
        while i + run < n and run < GLYPH_MAX_LEN and pix[i + run] == v:
            run += 1
        copy = 0
        if i >= w:
            while i + copy < n and copy < GLYPH_MAX_LEN and pix[i + copy] == pix[i + copy - w]:
                copy += 1
        is_run = v in (0, 15)
        if copy >= _COPY_MIN and (not is_run or copy > run):
            nib += [0, 0, *_glyph_len(copy)]
            i += copy
        elif is_run:
            nib += [v, *_glyph_len(run)]
            i += run
        else:
            nib.append(v)
            i += 1
    return _nibbles_to_bytes(nib)


def decode_glyph(data: bytes, w: int, h: int) -> tuple[bytes, int]:
    """Decode an encoding-1 stream. Returns (raw 4bpp bitmap, bytes consumed).
    Raises ValueError on a malformed or truncated stream."""
    total = w * h
    nb = _Nibbles(data)
    pix: list[int] = []
    while len(pix) < total:
        t = nb.next()
        if 1 <= t <= 14:
            pix.append(t)
            continue
        c = nb.next()
        left = total - len(pix)
        if t == 0 and c == 0:
            first = nb.next()
            if first == 0:
                raise ValueError("length code starting with 0")
            n = _read_glyph_len(nb, first)
            if len(pix) < w:
                raise ValueError("copy from above in the first row")
            if n > left:
                raise ValueError("copy past the end of the bitmap")
            while n:  # chunks of <= w so the source is already decoded
                k = min(n, w)
                s = len(pix) - w
                pix.extend(pix[s:s + k])
                n -= k
            continue
        if c == 0:
            raise ValueError("reserved code F 0")
        n = _read_glyph_len(nb, c)
        if n > left:
            raise ValueError("run past the end of the bitmap")
        pix.extend([t] * n)
    return pack_4bpp_nibbles(pix, w, h), nb.bytes_used()


def best_glyph(raw: bytes, w: int, h: int) -> tuple[int, bytes]:
    """(encoding, data): encoding 1 only when strictly smaller than the raw bitmap."""
    if w and h:
        enc = encode_glyph(raw, w, h)
        if len(enc) < len(raw):
            return GLYPH_RLE, enc
    return GLYPH_RAW, raw


# ------------------------------------------------------------- masks (1bpp)

def _mask_len(n: int) -> list[int]:
    """Length code for 0..285: 0..13 in one nibble, 14..29 in two, 30..285 in three."""
    if not 0 <= n <= MASK_MAX_LEN:
        raise ValueError(f"mask run length {n} out of range")
    if n < 14:
        return [n]
    if n < 30:
        return [14, n - 14]
    m = n - 30
    return [15, m >> 4, m & 15]


def _read_mask_len(nb: _Nibbles) -> int:
    c = nb.next()
    if c < 14:
        return c
    if c == 14:
        return 14 + nb.next()
    hi = nb.next()
    return 30 + (hi << 4) + nb.next()


def _mask_bits(raw: bytes, w: int, h: int) -> list[int]:
    stride = (w + 7) // 8
    return [(raw[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1 for y in range(h) for x in range(w)]


def _xor_rows(raw: bytes, w: int, h: int, undo: bool) -> bytes:
    """Row XOR filter: encode (row ^= row above, bottom up) or undo (top down)."""
    stride = (w + 7) // 8
    out = bytearray(raw)
    rows = range(1, h) if undo else range(h - 1, 0, -1)
    for y in rows:
        for b in range(stride):
            out[y * stride + b] ^= out[(y - 1) * stride + b]
    return bytes(out)


def encode_mask(raw: bytes, w: int, h: int, xor: bool = False) -> bytes:
    """Encode a raw 1bpp mask as encoding 1 (runs) or 2 (runs of the row XOR)."""
    if xor:
        raw = _xor_rows(raw, w, h, undo=False)
    runs: list[int] = []
    colour, n = 0, 0
    for b in _mask_bits(raw, w, h):
        if b == colour:
            n += 1
        else:
            runs.append(n)
            colour, n = b, 1
    runs.append(n)
    if len(runs) % 2:  # ends on an empty run: implicit (the decoder leaves 0)
        runs.pop()
    nib: list[int] = []
    for r in runs:
        while r > MASK_MAX_LEN:  # split: max, 0 of the other colour, rest
            nib += _mask_len(MASK_MAX_LEN) + _mask_len(0)
            r -= MASK_MAX_LEN
        nib += _mask_len(r)
    return _nibbles_to_bytes(nib)


def decode_mask(data: bytes, w: int, h: int, xor: bool = False) -> bytes:
    """Decode mask encoding 1 or 2 (the stream is exactly `data`). Raises ValueError."""
    total = w * h
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    nb = _Nibbles(data)
    i, colour = 0, 0
    while not nb.at_end():
        n = _read_mask_len(nb)
        if n > total - i:
            raise ValueError("mask run past the end of the bitmap")
        if colour:
            for p in range(i, i + n):
                y, x = divmod(p, w)
                out[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
        i += n
        colour ^= 1
    raw = bytes(out)
    return _xor_rows(raw, w, h, undo=True) if xor else raw


def best_mask(raw: bytes, w: int, h: int) -> tuple[int, bytes]:
    """(encoding, data): the smallest of raw, runs and row-XOR runs (ties: lower number)."""
    best = (MASK_RAW, raw)
    for enc, xor in ((MASK_RUNS, False), (MASK_XOR_RUNS, True)):
        d = encode_mask(raw, w, h, xor)
        if len(d) < len(best[1]):
            best = (enc, d)
    return best


def decode_mask_any(enc: int, data: bytes, w: int, h: int) -> bytes:
    if enc == MASK_RAW:
        if len(data) != raw1_size(w, h):
            raise ValueError("raw mask size mismatch")
        return bytes(data)
    if enc in (MASK_RUNS, MASK_XOR_RUNS):
        return decode_mask(data, w, h, enc == MASK_XOR_RUNS)
    raise ValueError(f"unknown mask encoding {enc}")
