# Tangochō pack format (v1)

A **pack** is a single binary file (`*.tgc`) that holds one deck of kanji cards
plus every pre-rendered glyph the device needs to show them. The device never
rasterises fonts: if a character is not in the pack, it cannot be drawn
(except plain ASCII, which falls back to the built-in GFX font).

This document is the contract between `tools/pack-builder` (writer) and
`firmware` (reader). Change it here first, bump `version` on any
incompatible change.

All integers are **little-endian**. All offsets are **absolute byte offsets
from the start of the file**. Strings are UTF-8, NUL-terminated, stored in the
string section.

## Layout

```
+---------------------+  0
| Header (64 bytes)   |
+---------------------+  cards_off
| Card table          |  card_count * 32 bytes
+---------------------+  glyphs_off
| Glyph table         |  glyph_count * 16 bytes, sorted by (set, codepoint)
+---------------------+  distract_off
| Distractor lists    |  u16 arrays
+---------------------+  strings_off
| String pool         |  strings_size bytes
+---------------------+  glyph_data_off
| Glyph bitmaps       |
+---------------------+
```

Sections are 4-byte aligned. Readers must use the offsets, not assume order.

## Header (64 bytes)

| off | type     | field            | notes                                          |
|-----|----------|------------------|------------------------------------------------|
| 0   | char[4]  | magic            | `"TNGC"`                                       |
| 4   | u16      | version          | `1`                                            |
| 6   | u16      | header_size      | `64`                                           |
| 8   | u32      | card_count       |                                                |
| 12  | u32      | glyph_count      |                                                |
| 16  | u32      | cards_off        |                                                |
| 20  | u32      | glyphs_off       |                                                |
| 24  | u32      | distract_off     |                                                |
| 28  | u32      | strings_off      |                                                |
| 32  | u32      | strings_size     |                                                |
| 36  | u32      | glyph_data_off   |                                                |
| 40  | u32      | deck_name        | string offset, e.g. `"JLPT N5"`                |
| 44  | char[4]  | lang             | meaning language, NUL padded: `"en\0\0"`, `"es\0\0"` |
| 48  | u8       | large_px         | nominal pixel size of set 0 glyphs             |
| 49  | u8       | small_px         | nominal pixel size of set 1 glyphs             |
| 50  | u8       | text_px          | nominal pixel size of set 2 glyphs             |
| 51  | u8       | text_line_height | recommended line height for set 2, px          |
| 52  | u8       | text_ascent      | baseline offset from line top for set 2, px    |
| 53  | u8       | option_px        | nominal pixel size of set 3 glyphs, 0 = no set 3 |
| 54  | u8       | option_line_height | recommended line height for set 3, px        |
| 55  | u8       | option_ascent    | baseline offset from line top for set 3, px    |
| 56  | u32      | build_time       | unix seconds                                   |
| 60  | u32      | reserved         | 0                                              |

## Card (32 bytes)

| off | type | field         | notes                                                      |
|-----|------|---------------|------------------------------------------------------------|
| 0   | u32  | codepoint     | the kanji (Unicode scalar). Stable card identity.          |
| 4   | u32  | meanings      | absolute file offset into the string pool. Meanings separated by `"; "`, most common first |
| 8   | u32  | onyomi        | string offset, katakana, readings separated by `"、"`. May be empty `""` |
| 12  | u32  | kunyomi       | string offset, hiragana, separated by `"、"`; okurigana marked with `.` as in KANJIDIC (`た.べる`). May be empty |
| 16  | u32  | distractors   | absolute file offset of this card's list inside the distractor section (see below) |
| 20  | u8   | jlpt          | 1..5 (N1..N5), 0 = unknown                                 |
| 21  | u8   | grade         | KANJIDIC school grade 1..10, 0 = unknown                   |
| 22  | u8   | strokes       |                                                            |
| 23  | u8   | flags         | bit0: meaning fell back to English (requested lang missing) |
| 24  | u16  | freq          | newspaper frequency rank, 0 = unknown                      |
| 26  | u16  | level         | deck-local ordering hint (lower = learn earlier)           |
| 28  | u32  | reserved      | 0                                                          |

The kanji's own glyphs are looked up by `codepoint` in sets 0 and 1 (every card
**must** have a set-0 and a set-1 glyph).

## Distractor list

At `distractors`: `u16 count`, then `count` × `u16` card **indices** (into
the card table of this pack), best distractor first. Lists are precomputed by
the builder (visual similarity, shared radicals, confusable readings, same
level...). The device may pick any subset and fill with random cards if the
list is short. `count` may be 0.

## Glyph table entry (16 bytes)

Sorted ascending by `(set, codepoint)` so the device can binary-search.

| off | type | field     | notes                                                  |
|-----|------|-----------|--------------------------------------------------------|
| 0   | u32  | codepoint |                                                        |
| 4   | u8   | set       | 0 = large kanji (quiz prompt), 1 = small kanji (answer options / lists), 2 = text (kana, Latin, punctuation for meanings and readings), 3 = large option text (kana for reading answers) |
| 5   | u8   | encoding  | 0 = raw 4bpp. (1 = RLE 4bpp, reserved for v2)           |
| 6   | u8   | width     | bitmap width, px (may be 0 for space)                  |
| 7   | u8   | height    | bitmap height, px                                      |
| 8   | i8   | x_off     | left bearing: pen x → bitmap left                      |
| 9   | i8   | y_off     | baseline → bitmap top (negative = above baseline)      |
| 10  | u8   | advance   | pen advance, px                                        |
| 11  | u8   | reserved  | 0                                                      |
| 12  | u32  | data_off  | absolute offset of bitmap                              |

For sets 0 and 1 the builder centres each kanji's ink box in a
`large_px`/`small_px` square and stores the cropped bitmap. The **baseline is
the bottom edge of that square**, so `y_off = top_in_square - px` (always
negative) and `x_off` is the left offset inside the square. To draw a kanji
in a box whose top-left is `(x, y)`: bitmap top-left = `(x + x_off, y + px + y_off)`.
All kanji of one set share the same visual box.

Sets 2 and 3 are normal text: `y_off` and `x_off` are relative to the pen
position and the baseline (`text_ascent` / `option_ascent` from the line top).
Set 3 holds the same kind of glyphs as set 2, rendered larger, for kana used in
reading answers where legibility matters. A reader that wants a set-3 glyph
**must fall back to set 2** if the code point is not present in set 3 (packs may
omit set 3 entirely: `option_px = 0`).

## Bitmap encoding 0: raw 4bpp

Row-major, top to bottom. Two pixels per byte, **high nibble first**. Each
row starts on a byte boundary (row stride = `ceil(width/2)`). Value 0 =
background, 15 = full ink. The device alpha-blends ink colour over
background with `v/15`.

## Limits and expectations

- `card_count` ≤ 65535 (distractor indices are u16).
- Expected size: ~650 kanji (N5–N3) at large_px=96 fits well under 3 MB.
- The device reads packs through the filesystem with random access; the
  builder must not rely on the device loading the whole file in RAM, although
  the device may cache the header, card table and glyph table in PSRAM.

## Versioning

- Unknown `version` → device refuses the pack and shows an error.
- New fields go into `reserved` bytes when possible (no version bump needed if
  0 means "absent").
