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
+---------------------+  stroke_off (0 = no stroke data)
| Stroke section      |  optional, see below
+---------------------+
```

Sections are 4-byte aligned. Readers must use the offsets, not assume order.

The **stroke section** is optional: `stroke_off` is 0 for every pack built
without `--strokes`, which is what all packs written before this extension have.
A reader that does not know about strokes ignores `stroke_off` and treats the
pack exactly as before, so this is a v1-compatible extension (no version bump).

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
| 60  | u32      | stroke_off       | absolute offset of the stroke section, 0 = none. Was `reserved` in earlier packs |

## Card (32 bytes)

| off | type | field         | notes                                                      |
|-----|------|---------------|------------------------------------------------------------|
| 0   | u32  | codepoint     | the kanji (Unicode scalar). Stable card identity.          |
| 4   | u32  | meanings      | absolute file offset into the string pool. Meanings separated by `"; "`, most common first |
| 8   | u32  | onyomi        | string offset, katakana, readings separated by `"、"`. May be empty `""` |
| 12  | u32  | kunyomi       | string offset, hiragana, separated by `"、"`; okurigana marked with `.` as in KANJIDIC (`た.べる`). May be empty |
| 16  | u32  | distractors   | absolute file offset of this card's meaning distractor list (see below) |
| 20  | u8   | jlpt          | 1..5 (N1..N5), 0 = unknown                                 |
| 21  | u8   | grade         | KANJIDIC school grade 1..10, 0 = unknown                   |
| 22  | u8   | strokes       |                                                            |
| 23  | u8   | flags         | bit0: meaning fell back to English (requested lang missing) |
| 24  | u16  | freq          | newspaper frequency rank, 0 = unknown                      |
| 26  | u16  | level         | deck-local ordering hint (lower = learn earlier)           |
| 28  | u32  | distract_read | reading distractor list offset; 0 = use `distractors`      |

The kanji's own glyphs are looked up by `codepoint` in sets 0 and 1 (every card
**must** have a set-0 and a set-1 glyph).

## Distractor lists

Each card has up to two lists: the **meaning** list at `distractors` and the
**reading** list at `distract_read`. Both share the format: `u16 count`, then
`count` × `u16` card **indices** (into the card table of this pack), best
distractor first. `distract_read` may be 0, in which case the meaning list is
used for reading questions too. Either list may have `count` 0.

Lists are precomputed by the builder: the meaning list favours visually similar
kanji (compared from their own bitmaps) so the wrong options are the ones a
learner might confuse; the reading list favours cards whose reading of the same
kind is confusable (one kana apart, e.g. ショウ/ジョウ). The device may pick any
subset and fill with random cards if a list is short.

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

## Stroke order section (optional)

Enabled by the builder's `--strokes` flag; absent (offset 60 = 0) otherwise.
It lets the device animate a kanji stroke by stroke, from KanjiVG data. All
offsets are absolute file offsets, as everywhere else, and the section is
4-byte aligned.

```
+----------------------------------+  stroke_off
| Card stroke index                |  card_count x u32 absolute offsets, 0 = no data for that card
+----------------------------------+  (4-byte aligned)
| Per-card stroke blocks (aligned) |
+----------------------------------+  (4-byte aligned)
| 1bpp stroke masks                |
+----------------------------------+
```

**Per-card stroke block** (always starts 4-byte aligned; `8 + 8*stroke_count`
bytes):

| off | type     | field        | notes                                             |
|-----|----------|--------------|---------------------------------------------------|
| 0   | u16      | stroke_count |                                                   |
| 2   | u16      | px           | em-square size the masks are drawn in (`== header large_px`) |
| 4   | u32      | reserved     | 0 (keeps the stroke entries 8-byte aligned)       |
| 8   | entry[]  | strokes      | `stroke_count` entries, 8 bytes each (below)      |

**Stroke entry** (8 bytes):

| off | type | field    | notes                                                       |
|-----|------|----------|-------------------------------------------------------------|
| 0   | i8   | x_off    | mask left, relative to the em-square left (0..px-1)         |
| 1   | i8   | y_off    | mask top, relative to the em-square top (0..px-1)           |
| 2   | u8   | w        | mask width, px (`> 0`)                                      |
| 3   | u8   | h        | mask height, px (`> 0`)                                     |
| 4   | u32  | data_off | absolute offset of the 1bpp mask                            |

**Mask encoding: 1bpp.** Row-major, top to bottom, **bit 7 = leftmost pixel**,
row stride `ceil(w/8)`, 1 = ink, 0 = nothing. A mask is the stroke's **own ink
only**: the earlier strokes are never baked in. To show stroke `i` of a kanji
the device composites strokes `0..i-1` in full ink and stroke `i` in a highlight
colour; strokes after `i` are simply not drawn.

The em square is the same one `Renderer::drawKanji` uses for set 0: a `px x px`
square whose top-left is `(boxX + (boxW - px)/2, boxY + (boxH - px)/2)`. A mask
is drawn at `(emX + x_off, emY + y_off)`. The em square (not the font glyph's
cropped ink box) is the reference frame, so stroke masks line up for any card of
the pack regardless of the font used.

## Limits and expectations

- `card_count` ≤ 65535 (distractor indices are u16).
- Expected size: ~650 kanji (N5–N3) at large_px=96 fits well under 3 MB.
- The optional stroke section costs roughly 0.4–0.6 KB per kanji at
  `large_px=96` (~50 KB for the 79 N5 cards), a small fraction of the glyph data.
- The device reads packs through the filesystem with random access; the
  builder must not rely on the device loading the whole file in RAM, although
  the device may cache the header, card table and glyph table in PSRAM.

## Versioning

- Unknown `version` → device refuses the pack and shows an error.
- New fields go into `reserved` bytes when possible (no version bump needed if
  0 means "absent"). The stroke section follows this rule: it reuses the old
  `reserved` u32 at offset 60 and 0 means "no stroke data".
