# tangocho-pack

Builds Tangochō `.tgc` packs (format: `docs/pack-format.md`) from KANJIDIC2 and
a font. Python 3.12+, managed with [uv](https://docs.astral.sh/uv/).

```sh
cd tools/pack-builder
uv sync
uv run tangocho-pack fetch            # KANJIDIC2, JLPT N5-N1 table, Klee One, KanjiVG -> cache/
uv run tangocho-pack build --jlpt 5 --lang es -o out/n5-es.tgc
uv run tangocho-pack build --jlpt 5 --lang es --strokes -o out/n5-es-strokes.tgc
uv run tangocho-pack inspect out/n5-es.tgc
uv run tangocho-pack preview out/n5-es.tgc --card 水 --mode meaning -o out/preview.png
uv run tangocho-pack preview out/n5-es-strokes.tgc --card 水 --mode strokes --stroke 2 -o out/strokes.png
uv run pytest
```

## Commands

- `fetch [--force]` downloads into `cache/` (git-ignored): KANJIDIC2, the
  JLPT table, Klee One and the pinned KanjiVG release zip.
- `build --jlpt N [--jlpt M ...] [--grade G ...] --lang en|es|fr|pt
  [--font PATH] [--large-px 96] [--small-px 40] [--text-px 18] [--option-px 20]
  [--strokes] [--strokes-px N] [--name NAME] -o FILE`. `--jlpt` and `--grade`
  filters are AND-ed when both are given. Cards are ordered easiest first (N5
  before N1, lower grade, frequency). `--strokes` adds the optional KanjiVG
  stroke-order section (off by default; a pack without it is byte-identical to a
  KANJIDIC2-only v1 pack). `--strokes-px` sets the em square for the masks and
  defaults to `--large-px`.
- `inspect FILE` validates (magic, version, offsets, alignment, glyph table
  order, set 0/1 glyph per card, every string character present in set 2, every
  reading character in set 3) and prints sizes; exit code 1 when invalid.
- `preview FILE --card 日 --mode meaning|reading|kanji|strokes [--kind on|kun]
  -o out.png [--width 135 --height 240 --scale 3 --seed 1 --reveal]
  [--stroke N]` draws a mock quiz screen that mirrors the device: reading answers
  as a 2x2 grid of large kana (set 3), meaning answers one per row in the
  built-in GLCD font, first gloss only. `--reveal` tints the correct option (and
  draws nothing else differently). `--mode strokes` draws the pack's stroke
  masks over the kanji area (all in white, or strokes `0..N` with `--stroke N`
  highlighting the last, mirroring the device animation).
- `fixture` regenerates `fixtures/mini.tgc` and `fixtures/mini.json` (3 cards,
  large 32 / small 16 / text 12 / option 18, build_time 0) for the firmware's
  native tests. `mini.json` lists the expected decoded fields; all offsets in it
  are absolute file offsets.

## Layout

- `kanjidic.py` parsing and card selection; `glyphs.py` rasterisation and 4bpp;
  `pack.py` writer, reader and validator; `kanjivg.py` stroke masks (KanjiVG);
  `distractors.py` distractor strategies (visual similarity for meanings,
  confusable readings for readings); `preview.py` UI mock; `build.py`
  orchestration.

## Conventions (where the spec leaves room)

- Set 0/1 glyphs: the ink bounding box (after 4bpp quantisation) is centred in
  the `px` x `px` em square; the **baseline is the bottom edge of that square**,
  so `y_off = top_in_square - px` (negative). A reader draws the square with
  its top-left at `(pen_x, baseline_y - px)`.
- Set 2: normal font baseline; `text_ascent` / `text_line_height` come from
  the font metrics at `text_px`.
- Set 3: the same readings rendered at `option_px` (`option_ascent` /
  `option_line_height`), for legible kana in reading answers. Readers must fall
  back to set 2 when a code point is absent from set 3; `--option-px 0` omits it.
- String and distractor offsets in cards and the header are absolute file
  offsets, consistent with the spec's global rule.

Data sources and licences: `docs/data-sources.md`.
