# tamagotcho-pack

Builds Tamagotcho (たまごっちょ) `.tgc` packs (format: `docs/pack-format.md`)
from KANJIDIC2 and a font. Python 3.12+, managed with
[uv](https://docs.astral.sh/uv/).

```sh
cd tools/pack-builder
uv sync
uv run tamagotcho-pack fetch            # KANJIDIC2, JLPT N5-N1 table, Noto Sans JP, KanjiVG -> cache/
uv run tamagotcho-pack build --jlpt 5 --lang es -o out/n5-es.tgc
uv run tamagotcho-pack build --jlpt 5 --lang es --strokes -o out/n5-es-strokes.tgc
uv run tamagotcho-pack fetch --klee-one # optional: Klee One (textbook style) -> cache/fonts/
uv run tamagotcho-pack build --jlpt 5 --font cache/fonts/KleeOne-SemiBold.ttf -o out/n5-en-klee.tgc
uv run tamagotcho-pack build --joyo --lang es --strokes -o out/joyo-es.tgc
printf '日\n月\n火\n' > my-kanji.txt
uv run tamagotcho-pack build --kanji-file my-kanji.txt --lang es -o out/my-kanji.tgc
uv run tamagotcho-pack fetch --readings # optional: word data for --common-readings -> cache/readings/
uv run tamagotcho-pack build --jlpt 5 --lang es --common-readings -o out/n5-es-common.tgc
uv run python scripts/common_readings_report.py   # what --common-readings changes
uv run tamagotcho-pack fetch --meanings # optional: English-Spanish dictionary for --meaning-order (with --readings)
uv run tamagotcho-pack build --jlpt 5 --lang es --meaning-order -o out/n5-es-meanings.tgc
uv run python scripts/meaning_order_report.py     # what --meaning-order changes
uv run python scripts/example_words_report.py --fetch --packs DIR  # example-word measurement (docs/example-words.md)
uv run tamagotcho-pack build --wanikani-token "$WANIKANI_TOKEN" -o out/wanikani.tgc
uv run tamagotcho-pack build --wanikani-level 1 --wanikani-level 2 -o out/wanikani-l1-2.tgc
uv run tamagotcho-pack inspect out/n5-es.tgc
uv run tamagotcho-pack preview out/n5-es.tgc --card 水 --mode meaning -o out/preview.png
uv run tamagotcho-pack preview out/n5-es-strokes.tgc --card 水 --mode strokes --stroke 2 -o out/strokes.png
uv run tamagotcho-pack catalog out --base-url "https://example.com/v1" -o out/catalog-v2.txt
uv run pytest
```

## Commands

- `fetch [--force] [--klee-one] [--readings] [--meanings]` downloads into `cache/` (git-ignored):
  KANJIDIC2, the JLPT table, Noto Sans JP Regular (the default font, pinned to
  the noto-cjk `Sans2.004` release, with its OFL `LICENSE`, in `cache/noto/`)
  and the pinned KanjiVG release zip. `--klee-one` also gets Klee One (the
  textbook-style font that was the default before 2026-10-18) into
  `cache/fonts/`, for `build --font cache/fonts/KleeOne-SemiBold.ttf`.
  `--readings` also gets, into `cache/readings/` (~37 MB), what
  `build --common-readings` reads: JMdict, JmdictFurigana, wordfreq's Japanese
  list, Tatoeba's Japanese sentence indices, the official Jōyō kanji table
  (PDF) and the tanos.co.uk JLPT vocabulary lists (Internet Archive copies,
  pinned and checked against SHA256). `--meanings` also gets, into
  `cache/meanings/` (3.7 MB), what `build --meaning-order` reads: FreeDict's
  English-Spanish dictionary (WikDict, from Wiktionary; the `2025.11.23`
  release, checked against SHA256). Licences in docs/data-sources.md.
- `build (--jlpt N [--jlpt M ...] [--grade G ... | --joyo] | --kanji-file PATH)
  [--order jlpt|grade|file]
  --lang en|es|fr|pt
  [--font PATH] [--large-px 96] [--small-px 40] [--text-px 18] [--option-px 20]
  [--strokes] [--strokes-px N] [--name NAME] -o FILE`. `--font` takes any
  TTF/OTF (default `cache/noto/NotoSansJP-Regular.otf`); a card whose kanji the
  font lacks is dropped with a warning, and a missing text glyph is an error.
  `--jlpt` and `--grade` filters are AND-ed when both are given. `--joyo`
  selects the 2,136 **Jōyō** kanji (KANJIDIC2 grades 1-6, the elementary-school
  kyōiku kanji, and 8, the rest of the list) and names the deck `Joyo kanji`.
  `--kanji-file` builds a deck from **your own list**: a UTF-8 text file with
  one kanji per non-blank line. Surrounding whitespace and an optional UTF-8
  BOM are ignored, and a line whose first non-space character is `#` is a
  comment. Duplicates keep their first occurrence; a line that is not exactly
  one Unicode character is an error naming the line, and a character KANJIDIC2
  does not have is skipped with a warning. The file order is the introduction
  order (the device's new-card order) unless `--order jlpt` or `--order grade`
  overrides it; `--order file` is only accepted with `--kanji-file`. The
  default deck name is the file stem, made safe for the
  header and catalog (non-ASCII characters become spaces; an empty stem is
  `Kanji`); `--name` overrides it. It cannot be combined with `--jlpt`,
  `--grade`, `--joyo` or `--wanikani-token`/`WANIKANI_TOKEN`.
  ```
  printf '日\n月\n火\n' > my-kanji.txt
  uv run tamagotcho-pack build --kanji-file my-kanji.txt --lang es -o out/my-kanji.tgc
  ```
  The card order is the pack's `level` hint, i.e. the order the device
  introduces new cards in (docs/srs.md): `--order jlpt` is
  easiest first (N5 before N1, then lower grade, then frequency); `--order
  grade`, the default with `--joyo`, is school order (grade 1..6, then 8; within
  a grade by newspaper frequency, kanji with no frequency rank last, fewer
  strokes first); `--order file`, the default with `--kanji-file`, is the file
  order. Deck names should be ASCII (`Joyo`, not `Jōyō`): the device
  draws the name in its built-in ASCII font, and the builder warns otherwise.
  `--strokes` adds the optional KanjiVG stroke-order section (off by default).
  `--strokes-px` sets the em square for the masks and defaults to `--large-px`.
  Packs are written in format **v2**: each glyph bitmap and stroke mask is
  stored compressed when that is smaller (`docs/pack-format.md`), which makes a
  pack about a third to a quarter of its v1 size. Firmware older than v2
  support refuses these packs ("unsupported pack version"); it keeps reading its
  v1 packs.
  `--common-readings` (needs `fetch --readings`, adds ~10 s) reorders each
  kanji's on'yomi and kun'yomi so the device, which asks the first reading of a
  kind, asks a useful one: the Jōyō table's approved readings first, then the
  readings of the easiest JLPT vocabulary, then by word frequency
  (docs/common-readings.md). A kind with nothing worth asking (no approved
  reading for a Jōyō kanji, no used one for another kanji: 気 いき, 百 もも) is
  left empty, so the device asks the other kind; never both kinds. A kind that
  stays keeps every reading, and reading distractors are kept away from the
  emptied kind's readings too. Without the flag the output is unchanged. It
  cannot be combined with `--wanikani-token` (WaniKani already lists its
  primary reading first). `scripts/common_readings_report.py` prints what it
  changes for JLPT N5-N1 and the Jōyō kanji; `scripts/check_reading_distractors.py
  out/*.tgc` checks built packs (the reading-distractor invariant against the
  full KANJIDIC2 readings, and how often a precomputed list is short).
  `--meaning-order` (needs `fetch --readings --meanings`, adds ~2 s) puts each
  card's most useful meaning first, the one the device shows
  (docs/meaning-order.md). English: each KANJIDIC2 meaning is scored by the
  JLPT vocabulary words that contain the kanji, through their JMdict glosses
  (easier levels and words that are more of the kanji count more), so 私
  "private; I; me" becomes "I; me; private". Spanish: each gloss is matched to
  the English gloss it translates (FreeDict's dictionary, then related forms: a
  shared stem, a cognate, head words, a near-synonym) and follows the new
  English order, unmatched glosses last, ties in KANJIDIC2's order (持
  "duración; propiedad" becomes "sostener; mantener"). Last, the project's
  `data/meaning_overrides.tsv` puts a hand-checked KANJIDIC2 meaning first
  where both still go wrong (機 "machine"); the builder refuses an override that
  names a meaning KANJIDIC2 lacks. The caps (as many meanings as English, at
  most three) apply afterwards. `fr`/`pt` have no dictionary: their order is
  unchanged (a warning says so). In every language the flag also keeps meaning
  distractors away from all of a card's KANJIDIC2 meanings, not only the stored
  ones. Without the flag the output is unchanged; it cannot be combined with
  `--wanikani-token`.
  `scripts/meaning_order_report.py` prints what it changes for JLPT N5-N1 and
  the Jōyō kanji (totals per language and level, a sanity table, every N5-N3
  change, the overrides, the cases to double-check); `scripts/check_meaning_distractors.py out/*.tgc` checks built
  packs (no meaning distractor shows a meaning of its card, or has the card's
  first meaning among its own, against KANJIDIC2's full lists).
- `build --wanikani-token TOKEN [--wanikani-level N ...]` builds from **your own
  WaniKani account** instead of KANJIDIC2 (so no `--jlpt`/`--grade`, and
  `fetch` is only needed for the font). It imports only the kanji you have
  **unlocked** (WaniKani API
  v2, `/assignments` joined to `/subjects`), reads the token from the flag or
  the `WANIKANI_TOKEN` environment variable, and never logs, prints or caches
  it. `--wanikani-level` (repeatable) limits the import to those WaniKani levels;
  without it every unlocked level is imported, ordered by level then subject id.
  `--lang` still applies, but WaniKani has English meanings only, so any other
  language falls back to English with the card's fallback flag set. On'yomi are
  converted from the API's hiragana to katakana. WaniKani content is © Tofugu
  LLC; a pack built this way is your own responsibility and must not be
  redistributed (docs/data-sources.md).
- `inspect FILE` validates (magic, version, offsets, alignment, glyph table
  order, set 0/1 glyph per card, every string character present in set 2, every
  reading character in set 3, and every compressed glyph and stroke mask decodes
  within its size) and prints sizes, raw vs stored bytes per glyph set and for
  the stroke masks, and the file's `sha256`; exit code 1 when invalid. It reads
  v1 and v2 packs.
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
  native tests, plus `fixtures/rle_vectors.bin`, the decoder test vectors the
  firmware cross-checks against this encoder (format in `fixture.py`).
  `mini.json` lists the expected decoded fields; all offsets in it are absolute
  file offsets. `fixtures/mini_v1.tgc` is the last v1 fixture, kept frozen (not
  regenerated) to test that v1 packs stay readable.
- `catalog DIRECTORY [-o FILE| -] [--base-url URL] [--json PATH] [--order NAMES]`
  scans a directory of `*.tgc` and writes the line-oriented **catalog** the
  device reads, format version 2 (see below; publish it as `catalog-v2.txt`). It
  refuses an empty directory, a file that is not a valid pack, and a pack whose
  deck name cannot be a catalog title (over 64 bytes, or a control character),
  so a broken build cannot silently enter a release.
  `--json` also writes a human-readable JSON companion (`catalog-v2.json` in the
  decks release; never read on device).
  Records are sorted by file name unless `--order` (pack names without `.tgc`,
  comma or space separated) lists some first: Get decks shows decks in catalog
  order, so the decks release puts N5 first.

## Catalog format

The roadmap wants prebuilt packs hosted with "a small catalog the device can
read". The catalog is deliberately trivial to parse on an ESP32 without a JSON
library: one record per line, TAB-separated, `#` comments to skip. This is
**version 2**; the exact bytes, one example per line:

```
#tangocho-catalog<TAB>2
# name<TAB>title<TAB>lang<TAB>cards<TAB>bytes<TAB>sha256<TAB>url
n5-en<TAB>JLPT N5<TAB>en<TAB>79<TAB>703123<TAB>ab12cd…<TAB>https://example.com/v1/n5-en.tgc
```

- The **first line** is a `#` comment that also carries the format version, so a
  reader can detect the format and refuse an unknown version before parsing.
  Its tag, `#tangocho-catalog`, keeps the project's former name (Tangochō) on
  purpose: it is part of the format, and firmware in the field checks it.
- Every line starting with `#` is a comment; blank lines are ignored.
- A **record** is exactly seven TAB-separated fields:
  - `name` — the file name without `.tgc`; the name the device should save the
    download as (`<name>.tgc`);
  - `title` — the deck's human-readable name, the pack header's `deck_name`
    (e.g. `JLPT N5`, `Joyo kanji`): UTF-8, at most **64 bytes**, may contain
    spaces but no TAB or other control character (U+0000..U+001F, U+007F). It
    may be empty, and a reader then shows `name`. The device draws it in its
    built-in font, so keep deck names ASCII (`build` warns otherwise);
  - `lang` — the pack's meaning language (the header's `lang`);
  - `cards` — its card count;
  - `bytes` — the exact file size;
  - `sha256` — lowercase hex digest of the whole file; the device checks it
    (and `bytes`) before it keeps a download;
  - `url` — absolute when `--base-url` is given, otherwise the bare file name,
    meaning "relative to the URL the catalog was fetched from". `--base-url` may
    contain `{file}`, which is replaced by the file name.
- `title` is the second column so that a person reading the file sees the file
  name and the deck name side by side, and `url`, the longest field, stays last.
- Records are sorted by file name, so the output is deterministic and safe to
  diff.
- The device reads this file for **Get decks** and the portal page lists it
  (`firmware/src/catalog/`): it refuses any other version, skips a record that
  does not validate (and keeps the others), caps the list at 32 decks, and
  resolves a bare-file-name `url` against the catalog URL it was configured
  with (before any redirect).

**File name and versions.** The catalog's format version (the `N` of
`#tangocho-catalog<TAB>N`) and the pack format version (`docs/pack-format.md`)
are independent. The published file is named after the **catalog** format,
`catalog-v<N>.txt` (`catalog-v2.txt` now, with `catalog-v2.json` for people);
the decks release tag `decks-v<N>` is named after the **pack** format. So the
firmware's default catalog,
`https://github.com/dcarnicer/tamagotcho/releases/download/decks-v2/catalog-v2.txt`,
is catalog format v2 listing packs of format v2, and either version can change
without the other.

**Version 1** (`catalog.txt`, the same columns without `title`) is no longer
written or read: the builder writes only v2, and the firmware refuses a v1 file
as an unsupported version ("old catalog format"). There is no backward
compatibility while the maintainer is the only user (docs/decisions.md,
2026-10-18 "Catalog v2: deck titles").

## Layout

- `kanjidic.py` parsing and card selection; `wanikani.py` the optional
  WaniKani importer (own token, unlocked kanji only); `glyphs.py` rasterisation
  and 4bpp; `pack.py` writer, reader and validator; `kanjivg.py` stroke masks
  (KanjiVG); `distractors.py` distractor strategies (visual similarity for
  meanings, confusable readings for readings); `preview.py` UI mock;
  `catalog.py` the line-oriented device catalog; `build.py` orchestration;
  `readings.py` reading scores and order for `--common-readings`, with
  `joyo_table.py` the parse of the official Jōyō table PDF.

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
