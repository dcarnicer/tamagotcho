# Tamagotcho pack format (v3)

A **pack** is a single binary file (`*.tgc`) that holds one deck of kanji cards
plus every pre-rendered glyph the device needs to show them. The device never
rasterises fonts: if a character is not in the pack, it cannot be drawn
(except plain ASCII, which falls back to the built-in GFX font).

This document is the contract between `tools/pack-builder` (writer) and
`firmware` (reader). Change it here first, bump `version` on any
incompatible change.

**Version 2** added compressed glyph bitmaps (encoding 1) and a new stroke
block layout with compressed stroke masks. **Version 3** adds a header
extension (`header_size` 80) and, in it, the offset of an optional **example
words** section: per card, a common word for its asked on'yomi and one for its
asked kun'yomi, each with its reading and a gloss (docs/learning-mode.md
§3.11), plus the words' kanji in glyph set 2. Everything else is unchanged,
and readers keep accepting version 1 and 2 packs: the differences are marked
*v1* / *v2* / *v3* below and summarised under [Versioning](#versioning).

The builder writes **version 2** by default, byte for byte as before, and
**version 3 only with `build --example-words`** (the one v3 feature), so the
decks released firmware reads stay v2 ([Rollout](#rollout-of-v3)).

All integers are **little-endian**. All offsets are **absolute byte offsets
from the start of the file**. Strings are UTF-8, NUL-terminated, stored in the
string section.

## Layout

```
+---------------------+  0
| Header (64 bytes)   |  v3: 80 bytes (header_size), the last 16 the extension
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
+---------------------+  words_off (v3; 0 = no example words)
| Example words       |  optional, v3 only, see below
+---------------------+
```

Sections are 4-byte aligned. Readers must use the offsets, not assume order.
Individual bitmaps inside the glyph bitmap section (and, in v2, the stroke
masks) are not aligned: they are read byte by byte.

The **stroke section** is optional: `stroke_off` is 0 for every pack built
without `--strokes`, which is what all packs written before this extension have.
A reader that does not know about strokes ignores `stroke_off` and treats the
pack exactly as before, so it was added to v1 as a compatible extension (no
version bump). Version 2 changed the stroke block layout (see below).

The **example-words section** is optional too, and only a v3 pack has one:
`words_off`, in the v3 header extension, is 0 when the pack has none.

## Header (64 bytes; v3: 80)

| off | type     | field            | notes                                          |
|-----|----------|------------------|------------------------------------------------|
| 0   | char[4]  | magic            | `"TNGC"`                                       |
| 4   | u16      | version          | `3` with example words, else `2` (`1` for older packs, still readable) |
| 6   | u16      | header_size      | `64` in v1 and v2; `80` in v3 (readers accept more and skip it) |
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

**Header extension (v3).** A v3 header is 80 bytes: the 64 above, then:

| off | type     | field            | notes                                          |
|-----|----------|------------------|------------------------------------------------|
| 64  | u32      | words_off        | absolute offset of the example-words section, 0 = none |
| 68  | u32[3]   | reserved         | 0; for later optional sections (0 = absent, no version bump) |

`header_size` says how long the header is: a v3 reader requires at least 80,
reads the fields it knows and skips the rest (a later extension may grow it;
`cards_off` and every other offset stay absolute, so nothing moves). A v1 or
v2 header is 64 bytes, and a reader never looks for `words_off` in it.

The magic `TNGC` and the `.tgc` extension come from the project's former name,
Tangochō, and stay as they are after the rename to Tamagotcho, as does the
deck catalog's tag `#tangocho-catalog` (`tools/pack-builder/README.md`): they
are part of the formats the published packs and catalogs carry
(docs/decisions.md, 2026-10-21).

## Card (32 bytes)

| off | type | field         | notes                                                      |
|-----|------|---------------|------------------------------------------------------------|
| 0   | u32  | codepoint     | the kanji (Unicode scalar). Stable card identity.          |
| 4   | u32  | meanings      | absolute file offset into the string pool. Meanings separated by `"; "`, most common first |
| 8   | u32  | onyomi        | string offset, katakana, readings separated by `"、"`, most useful first (see Readings). May be empty `""` |
| 12  | u32  | kunyomi       | string offset, hiragana, separated by `"、"`, most useful first; okurigana marked with `.` as in KANJIDIC (`た.べる`). May be empty |
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

**Readings.** Within each kind (`onyomi`, `kunyomi`) the readings are ordered
**most useful first**, and a reading question asks the **first** reading of the
kind (dots removed). **Every listed reading counts as a valid answer**: no
option of a question may be another reading of the target, of either kind, so
readings that are not worth asking stay in the list, after the others, to keep
distractors out. **A kind may be empty**: the device then asks the other kind,
and a card with neither kind gets no reading questions. An empty kind is the
offset of an empty string (`""`) inside the string pool, **never 0** (readers
reject an offset outside the pool). A non-empty list has no empty item (no
leading, trailing or doubled `、`) and no reading that is only okurigana dots.
The builder by default
keeps KANJIDIC2's order; `build --common-readings` puts common readings first
and leaves a kind empty when none of its readings is worth asking
(docs/common-readings.md). A card with no meaning and no reading is never
written.

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
| 4   | u8   | set       | 0 = large kanji (quiz prompt), 1 = small kanji (answer options / lists), 2 = text (kana, Latin, punctuation for meanings and readings; in v3, also the example words' kanji), 3 = large option text (kana for reading answers) |
| 5   | u8   | encoding  | 0 = raw 4bpp, 1 = RLE 4bpp (v2 and v3). Others: invalid |
| 6   | u8   | width     | bitmap width, px (may be 0 for space)                  |
| 7   | u8   | height    | bitmap height, px                                      |
| 8   | i8   | x_off     | left bearing: pen x → bitmap left                      |
| 9   | i8   | y_off     | baseline → bitmap top (negative = above baseline)      |
| 10  | u8   | advance   | pen advance, px                                        |
| 11  | u8   | reserved  | 0                                                      |
| 12  | u32  | data_off  | absolute offset of the bitmap data (encoded as `encoding` says) |

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

Sets 2 and 3 also hold the **hiragana form of every on'yomi character**: the
katakana ァ..ヶ and ヽヾ shifted down by 0x60 (U+3041..U+3096, ゝゞ); other
characters, such as ー, have no other form. The device can show on'yomi answers
in hiragana (Settings › Study › On'yomi) while the strings stay katakana. A
v2 or v3 pack **must** carry these glyphs; v1 packs predate the setting.

**Set 2 and the example words (v3).** A pack with an example-words section
**must** have a set-2 glyph for every character of every word, reading and gloss
in it (ASCII may still fall back to the built-in font, as everywhere). Without
words, set 2 holds no kanji; with them it holds every kanji the words are
written with: the card's own kanji and the words' other ones (毎日 adds 毎 to
日's deck), rendered at `text_px` as normal text like the kana (pen and
baseline relative, `text_ascent`), not centred in an em square like sets 0
and 1. So a word, its reading and its gloss are drawn with set 2 like any
meaning, and a reader's text routine needs nothing new. Set 3 gets nothing
for the words.

## Bitmap encoding 0: raw 4bpp

Row-major, top to bottom. Two pixels per byte, **high nibble first**. Each
row starts on a byte boundary (row stride = `ceil(width/2)`). Value 0 =
background, 15 = full ink. The device alpha-blends ink colour over
background with `v/15`. A row of odd width ends in a padding nibble, which is 0.

## Bitmap encoding 1: RLE 4bpp (v2)

A compressed form of the same bitmap: decoding it yields exactly the encoding 0
bytes (same stride, padding nibbles 0). Most kanji pixels are 0 or 15 and many
rows repeat the row above, so the code has runs of 0, runs of 15, copies from
the row above, and single anti-aliased pixels.

**Pixel order.** The `width x height` pixels are one row-major sequence: a run
or copy that reaches the end of a row continues at the start of the next one
(rows are not padded in the stream).

**Codes.** The stream is a sequence of 4-bit codes, two per byte, **high nibble
first**. Tokens, by their first code:

| codes      | meaning                                                         |
|------------|-----------------------------------------------------------------|
| `v`        | `v` = 1..14: one pixel of value `v`                              |
| `0 L`      | `L` pixels of value 0                                           |
| `F L`      | `L` pixels of value 15                                          |
| `0 0 L`    | copy: each of the next `L` pixels takes the value of the pixel one row above it (pixel index `i` copies `i - width`). Every copied pixel must be in row 1 or later |
| `F 0`      | reserved: invalid in v2                                         |

`L` is a **length code** of one to three codes. Its first code `c` is never 0
(that is what tells `0 0 L` from `0 L`):

| first code `c` | codes      | length                                  |
|----------------|------------|-----------------------------------------|
| 1..10          | `c`        | `c` (1..10)                             |
| 11..14         | `c n`      | `11 + (c - 11) * 16 + n` (11..74)       |
| 15             | `F n1 n2`  | `75 + n1 * 16 + n2` (75..330)           |

A longer run or copy is written as several tokens (400 zeros: `0 F F F` = 330,
then `0 E B` = 70).

**End.** Decoding stops after exactly `width * height` pixels. A token that
would produce more pixels than remain is an error. If the last code is a high
nibble, the low nibble of that byte is padding and is ignored. The stream
length is **not stored**: it is implied by the codes. The builder uses encoding
1 only when it is strictly smaller than encoding 0, and readers must reject a
stream longer than `ceil(width/2) * height` bytes, so a reader may fetch that
many bytes (clamped to the end of the file) from `data_off` and decode from
them. Glyphs with no pixels (`width` or `height` 0) are always encoding 0.

**Example.** A 5x3 glyph:

```
row 0:   0  0  7 15 15
row 1:   0  0  7 15 15
row 2:  15 15 15 15  3
```

Encoding 0 is `00 7F F0 00 7F F0 FF FF 30` (9 bytes). Encoding 1:

```
0 2      two 0 pixels
7        one pixel of 7
F 2      two 15 pixels (end of row 0)
0 0 5    copy five pixels from above (all of row 1)
F 4      four 15 pixels
3        one pixel of 3
(0)      padding
```

which packs to `02 7F 20 05 F4 30` (6 bytes). Longer lengths: 40 zeros are
`0 C D` (`11 + 1*16 + 13`), 100 zeros are `0 F 1 9` (`75 + 25`).

The builder's encoder is greedy (at each pixel: a copy when it covers at least
3 pixels and more than a run would, else a run of 0/15, else a literal); any
stream that follows the rules above is valid.

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
| Stroke masks                     |
+----------------------------------+
```

The index is the same in both versions; the block and entry layouts differ.

**Per-card stroke block, v2** (always starts 4-byte aligned; `8 + 8*stroke_count`
bytes):

| off | type     | field        | notes                                             |
|-----|----------|--------------|---------------------------------------------------|
| 0   | u16      | stroke_count |                                                   |
| 2   | u16      | px           | em-square size the masks are drawn in (`== header large_px`) |
| 4   | u32      | data_off     | absolute offset of the first stroke's mask data   |
| 8   | entry[]  | strokes      | `stroke_count` entries, 8 bytes each (below)      |

A card's masks are stored **back to back in stroke order**, with no padding:
mask `k` starts at `data_off + size_0 + ... + size_(k-1)`.

**Stroke entry, v2** (8 bytes):

| off | type | field    | notes                                                       |
|-----|------|----------|-------------------------------------------------------------|
| 0   | i8   | x_off    | mask left, relative to the em-square left (0..px-1)         |
| 1   | i8   | y_off    | mask top, relative to the em-square top (0..px-1)           |
| 2   | u8   | w        | mask width, px (`> 0`)                                      |
| 3   | u8   | h        | mask height, px (`> 0`)                                     |
| 4   | u8   | encoding | 0 = raw 1bpp, 1 = runs, 2 = runs of the row XOR (below). Others: invalid |
| 5   | u8   | reserved | 0                                                           |
| 6   | u16  | size     | bytes of mask data. Encoding 0: exactly `ceil(w/8)*h`; 1 and 2: at most that |

**v1 layout.** In version 1 the block's bytes 4..7 are `reserved` (0) and each
stroke entry ends in `u32 data_off` (absolute offset of that mask, at bytes 4..7)
instead of `encoding`, `reserved` and `size`. Every v1 mask is raw 1bpp, its
size is `ceil(w/8)*h`, and the v1 writer started each mask on a 4-byte boundary.

**Mask encoding 0: raw 1bpp.** Row-major, top to bottom, **bit 7 = leftmost pixel**,
row stride `ceil(w/8)`, 1 = ink, 0 = nothing; the padding bits at the end of a
row are 0. A mask is the stroke's **own ink only**: the earlier strokes are
never baked in. To show stroke `i` of a kanji the device composites strokes
`0..i-1` in full ink and stroke `i` in a highlight colour; strokes after `i` are
simply not drawn.

**Mask encoding 1: runs (v2).** The `w x h` pixels are one row-major sequence
(rows run on into the next, as in glyph encoding 1), coded as **alternating run
lengths, starting with a run of empty pixels** (which may have length 0), then
ink, then empty, and so on. The stream is 4-bit codes, two per byte, high nibble
first; each run length is a length code with first code `c`:

| first code `c` | codes      | length                         |
|----------------|------------|--------------------------------|
| 0..13          | `c`        | `c` (0..13)                    |
| 14             | `E n`      | `14 + n` (14..29)              |
| 15             | `F n1 n2`  | `30 + n1 * 16 + n2` (30..285)  |

A run longer than 285 is written as 285, a 0-length run of the other colour,
then the rest. The stream ends with the mask's `size` bytes: **pixels after the
last run are empty**, so the builder leaves out a final empty run. A last padding
nibble 0 is just a 0-length run. A run that goes past `w*h` pixels, or a length
code cut off by the end of the data, is an error.

**Mask encoding 2: runs of the row XOR (v2).** The same run stream, but it codes
each row XOR the row above it (row 0 as is). To decode, decode as encoding 1,
then for `y = 1 .. h-1`, top to bottom, XOR row `y` with the already decoded
row `y-1` (byte by byte over the row stride). A vertical stroke becomes a few
runs. The builder stores each mask in whichever of encodings 0, 1 and 2 is
smallest (0 on a tie).

**Examples.** A 16x2 mask whose rows are `...##########...` and
`....##########..` is `1F F8 0F FC` raw (4 bytes). As runs it is 3 empty, 10
ink, 7 empty (3 + 4 across the row end), 10 ink, and the final 2 empty pixels
are left out: codes `3 A 7 A`, bytes `3A 7A` (encoding 1, 2 bytes). A 4x6
vertical stroke whose rows are all `.##.` is `60 60 60 60 60 60` raw; its row
XOR is `.##.` followed by five empty rows, so encoding 2 is the runs 1 empty, 2
ink: one byte, `12`.

The em square is the same one `Renderer::drawKanji` uses for set 0: a `px x px`
square whose top-left is `(boxX + (boxW - px)/2, boxY + (boxH - px)/2)`. A mask
is drawn at `(emX + x_off, emY + y_off)`. The em square (not the font glyph's
cropped ink box) is the reference frame, so stroke masks line up for any card of
the pack regardless of the font used.

## Example-words section (v3, optional)

Enabled by the builder's `--example-words` flag, which is what makes a pack v3;
`words_off` is 0 otherwise. It gives each card up to two common words that
show its readings in use (docs/learning-mode.md §3.11; how the words are
picked: docs/example-words.md): one that uses the card's **asked on'yomi**
(the first of `onyomi`) and one for its **asked kun'yomi** (the first of
`kunyomi`), each with its reading and a short gloss in the pack's language.
All offsets are absolute file offsets, as everywhere else, and the section is
4-byte aligned.

```
+----------------------------------+  words_off
| Section header (8 bytes)         |
+----------------------------------+  words_off + 8
| Card word index                  |  (card_count + 1) x u32
+----------------------------------+  words_off + 8 + 4 * (card_count + 1)
| Word table                       |  word_count x entry_size bytes
+----------------------------------+
```

**Section header** (8 bytes):

| off | type | field      | notes                                                   |
|-----|------|------------|---------------------------------------------------------|
| 0   | u16  | entry_size | bytes per word entry: `16`. A reader requires at least 16, reads the first 16 and skips the rest, so a field can be appended later without a version bump |
| 2   | u16  | reserved   | 0                                                       |
| 4   | u32  | word_count | entries in the word table                               |

**Card word index**: `card_count + 1` u32 word-table indices, `first[0..card_count]`,
in card-table order. The words of card `i` are the entries `first[i]` ..
`first[i+1] - 1` (none when the two are equal). `first[0]` is 0, the list
never decreases, and `first[card_count]` is `word_count`. The word table
starts right after the index (so it is 4-byte aligned too).

**Word entry** (`entry_size` bytes; 16 in v3):

| off | type | field         | notes                                                  |
|-----|------|---------------|--------------------------------------------------------|
| 0   | u32  | word          | string offset: the word as written, kanji with any okurigana (`毎日`, `食べる`). Never empty |
| 4   | u32  | reading       | string offset: its reading in kana, as JMdict writes it (`まいにち`; hiragana, katakana in loanword parts). Never empty |
| 8   | u32  | gloss         | string offset: a short meaning in the pack's `lang`; `""` when the pack has none for this word (see below) |
| 12  | u8   | kind          | 0 = a word for the card's asked on'yomi, 1 = for its asked kun'yomi. Others: invalid |
| 13  | u8   | reserved      | 0                                                      |
| 14  | u8   | reading_start | byte offset, inside `reading`, of the part the card's kanji reads (`にち` in `まいにち`: 6) |
| 15  | u8   | reading_len   | that part's length in bytes (6); 0 = unknown, and then `reading_start` is 0 |

Rules:

- A card has **at most one word of each kind, the on'yomi word first**. A kind
  that is empty on the card has no word, and an asked reading may have none
  when no common word uses it (天 あめ). A card may have no words at all.
- `word` contains the card's kanji. The span `reading_start`/`reading_len`
  lies inside `reading` on character boundaries (sequential voicing and
  gemination included: `がっ` in `がっこう` for 学 ガク); a reader treats a
  span that does not fit as unknown.
- The strings live in the **string pool** with every other string,
  deduplicated (`外国` serves 外 and 国); an empty gloss is the offset of an
  empty string, **never 0**, as for an empty reading kind. Every offset must
  lie inside the pool.
- **Glosses.** English packs take the first gloss of the word's first JMdict_e
  sense that is not a transliteration of its reading, when it has at most 24
  characters (else none). Spanish packs take
  only reviewed glosses from the project's table
  (`tools/pack-builder/data/example_glosses_es.tsv`, rows marked `ok`); a word
  without one has an empty gloss, as in packs of other languages, and the
  device shows its word and reading only. A gloss is **never in another
  language** than `lang`. The format sets no length limit; this builder keeps
  glosses within 24 characters (Unicode code points) except where a reviewed
  Spanish gloss is longer.
- Every character of the section's strings has a set-2 glyph (see the glyph
  table above).

A reader reads the section **on demand**, in either memory mode, as it reads
stroke data: a card's words cost one read of 8 index bytes and one of its
entries, and their strings come from the pool like the card's own (from
memory, or through the streamed string cache). It validates the header, the
index and every entry at open, like the other tables.

**Example** (the N5 card 日, asked on'yomi ニチ, kun'yomi ひ, English): two
entries, `毎日` / `まいにち` / `every day` / kind 0 / span 6, 6, and `日` / `ひ` /
`day` / kind 1 / span 0, 3.

## Limits and expectations

- `card_count` ≤ 65535 (distractor indices are u16).
- `large_px` ≤ 128 when the optional stroke section is present: a stroke entry's
  `x_off`/`y_off` are i8, so a larger em square cannot be represented.
- Expected size (v2, builder defaults `large_px=96`, `small_px=40`, with
  `--strokes`): **about 1.7–1.8 KB per kanji** with the Klee One font and
  1.4–1.5 KB with Noto Sans JP, whose straight strokes compress better. Per kanji
  (2,136-kanji Jōyō build, Klee One / Noto): set-0 glyph ~880 / 640 B, set-1
  glyph ~340 / 260 B, stroke section ~340 B, card and glyph table entries,
  strings and distractors ~160 B; the text sets add ~20–30 KB per pack. JLPT N1
  (1,232 kanji) is 2.2 / 1.8 MB, a Jōyō-sized pack 3.7 / 3.0 MB, both under the
  device's 5 MB per-pack cap. The same packs in v1 were about 3–4 times
  larger (5.4–5.8 KB per kanji: 6.8 MB for N1, 11.6 MB for the Jōyō set).
- The optional stroke section costs **about 340 bytes per kanji** at
  `large_px=96` in v2 (it was ~800 bytes in v1): ~100 bytes of index, block and
  entries (8 bytes per stroke), ~240 bytes of masks (about 34 % of their raw
  size). A mask's pixel count scales with the em square's *area*, so the raw
  cost grows quadratically with `large_px`.
- The device reads packs through the filesystem with random access; the
  builder must not rely on the device loading the whole file in RAM, although
  the device may cache the header, card table and glyph table in PSRAM. Both v2
  encodings decode front to back into the caller's bitmap buffer, reading the
  stream through a small window (the firmware reads 256 bytes at a time), so a
  reader needs one decoded bitmap (`ceil(large_px/2) * large_px` bytes at most:
  4.6 KB at 96 px) and no other memory.

## Versioning

- Unknown `version` → device refuses the pack and shows an error. Firmware
  that only knows version 1 therefore refuses a v2 pack cleanly ("unsupported
  pack version"); it never misreads compressed data as raw.
- New fields go into `reserved` bytes when possible (no version bump needed if
  0 means "absent"). The stroke section follows this rule: it reuses the old
  `reserved` u32 at offset 60 and 0 means "no stroke data".
- **Version 2** (compression) needed a bump, because a v1 reader would draw
  compressed bytes as raw ones:
  - glyph `encoding` 1 (RLE 4bpp) is allowed; encoding 0 stays valid, and the
    builder picks, per glyph, whichever is smaller;
  - the per-card stroke block's bytes 4..7 hold the `data_off` of the card's
    first mask, and each stroke entry holds `encoding`/`reserved`/`size`
    instead of an absolute `data_off`; masks are stored back to back, in
    encoding 0, 1 or 2;
  - everything else (header, cards, distractors, strings, glyph table layout,
    stroke index) is identical to v1.
- A reader for version 2 must also accept version 1 (devices keep their old
  packs): in a v1 pack every glyph must be encoding 0 and the stroke blocks use
  the v1 layout.
- **Version 3** (example words) is a bump although the section itself is
  optional: the 64-byte header had no free field left, so v3 grows it to 80
  bytes (`header_size`), and a v3 pack is meant for firmware that can show its
  words. Everything else is identical to v2:
  - the header extension (`words_off` and three reserved u32) and the optional
    example-words section;
  - set 2 may hold kanji (the words' ones) when the section is present.
- A reader for version 3 must also accept versions 1 and 2, exactly as before:
  it reads no extension from a v1 or v2 header and reports no example words for
  such a pack.
- Released firmware (v0.5.1 and older) refuses a v3 pack cleanly by its
  version ("unsupported pack version"). Its header check alone would have
  accepted an 80-byte header (it only requires `header_size >= 64`), so the
  extension could have been a compatible v2 change; the bump is deliberate
  (docs/decisions.md, 2026-10-23 "Example words in packs: format v3").

## Rollout of v3

Published decks must stay readable by released firmware: v0.5.1 reads v2 packs
from the `decks-v2` release (its default catalog,
`.../releases/download/decks-v2/catalog-v2.txt`) and refuses v3 packs. So the
steps are, in this order:

1. **Default builds stay v2.** Without `--example-words` the builder writes v2,
   byte for byte as before, and `decks.yml` keeps building `decks-v2`
   unchanged. Merging the builder and the reader changes no published deck.
2. **A `decks-v3` release**, built by `decks.yml` with `--example-words` added
   to its build lines (Spanish glosses as far as the table is reviewed). The
   workflow names the release after the packs' version (`decks-v<N>`), so
   these go to `decks-v3`, next to `decks-v2`, which stays as it is. Nothing
   released reads `decks-v3` yet, so publishing it early is harmless, and it
   must exist before step 3. Its catalog is still catalog format v2
   (`decks-v3/catalog-v2.txt`): the two version numbers are independent.
3. **A firmware release that reads v3** (this reader) and shows the words (the
   intro screen, learning-mode phase 2), whose default catalog URL
   (`HomeWifi::kDefaultCatalogUrl`) points at `decks-v3`. It still opens v2
   packs exactly as before, so decks already on a device keep working; Get
   decks and the update check offer the v3 ones (their SHA-256 differs). A
   device that saved its own catalog URL (an override in NVS) keeps it.
4. **`decks-v2` is frozen**, kept for v0.5.1 and older, and rebuilt only if
   they need a fix; under the project's no-backward-compatibility rule
   (docs/decisions.md) it can also just be left as it is.

The builder and reader change does none of this by itself: no deck release
and no firmware default changes with it.
