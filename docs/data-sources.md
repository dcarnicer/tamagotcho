# Data sources and licences

Code in this repository is MIT. **Generated packs (`*.tgc`) contain data
derived from KANJIDIC2 and are licensed CC BY-SA 4.0**, regardless of the
licence of the code that produced them. Anything downloaded by
`tangocho-pack fetch` lands in `tools/pack-builder/cache/` (git-ignored) and is
never committed. The only third-party-derived files in the repo are the three
kanji in `tools/pack-builder/tests/data/mini_kanjidic2.xml` (a KANJIDIC2
excerpt) and the fixture pack built from it.

## KANJIDIC2

- URL: https://www.edrdg.org/kanjidic/kanjidic2.xml.gz
- Owner: Electronic Dictionary Research and Development Group (EDRDG)
- Licence: Creative Commons Attribution-ShareAlike 4.0
  (https://www.edrdg.org/edrdg/licence.html)
- Required attribution: "This package uses the KANJIDIC2 file. These files are
  the property of the Electronic Dictionary Research and Development Group,
  and are used in conformance with the Group's licence."
- Used for: codepoint, meanings (en/es/fr/pt), on'yomi, kun'yomi, stroke
  count, school grade, newspaper frequency, the legacy 4-level JLPT field
  (fallback only, see below), and the classical KangXi radical numbers
  (`rad_value rad_type="classical"`, 1..214) used to rank meaning distractors.
  Radicals are a builder-side selection signal only: they are **not** stored in
  the pack and the pack format is unchanged (see docs/decisions.md).

### Meaning and reading conventions

- Meanings: first 3 in the requested language (`m_lang`; no attribute = English).
  If the language has none for a kanji, the first 3 English meanings are used
  and card flag bit0 is set. KANJIDIC2 has no meanings at all for some kanji;
  cards with neither meanings nor readings are dropped.
- On'yomi: katakana as in KANJIDIC2. Kun'yomi: hiragana as in KANJIDIC2.
- Kun'yomi keep the `.` okurigana marker (`た.べる`). The `-` prefix/suffix
  markers (`-ず`, `ひ.-`) are **dropped**: they carry little value on a 135 px
  screen and would otherwise create near-duplicate entries; duplicates created
  by dropping are removed.
- Readings are not capped by the builder; the firmware decides how many fit.
- Joined with `; ` (meanings) and `、` (readings) per the pack spec.

## New JLPT levels (N5-N1): davidluzgouveia/kanji-data

KANJIDIC2 only carries the old four-level JLPT (4 = easiest ... 1 = hardest),
which does not map one-to-one onto N5-N1. We use `jlpt_new` from:

- URL: https://github.com/davidluzgouveia/kanji-data (`kanji.json`)
- Repository licence: MIT (Copyright (c) 2019 David Gouveia).
- That file is itself derived from KANJIDIC and from Jonathan Waller's JLPT
  Resources page (http://www.tanos.co.uk/jlpt/), which publishes its lists
  under Creative Commons Attribution. The file also contains WaniKani-derived
  fields (`wk_*`); **we read none of them** and redistribute nothing from it.
- Required attribution: "JLPT levels from Jonathan Waller's JLPT Resources
  (tanos.co.uk), via davidluzgouveia/kanji-data."
- Used for: `jlpt_new` only, stored in the card `jlpt` byte (1..5).
- Caveat: JLPT no longer publishes official kanji lists since 2010, so any
  N5-N1 kanji list is community-reconstructed. N5 yields 79 kanji.
- Fallback if `kanji.json` is not in the cache: old level 4 -> N5, 3 -> N4,
  2 -> N3 (lossy: old 2 spans N3 and N2, so N2 is never produced), 1 -> N1.
  The builder prints a warning in that case.

## Klee One (font)

- URL: https://github.com/fontworks-fonts/Klee (`fonts/ttf/KleeOne-SemiBold.ttf`,
  `KleeOne-Regular.ttf`, `OFL.txt`)
- Licence: SIL Open Font License 1.1. Copyright Fontworks Inc.
- Used for: rasterising kanji and text glyphs into the packs. The default is
  SemiBold (better legibility at small sizes on a low-density LCD).
- OFL notes: the OFL permits use, embedding and redistribution of the font and
  of output made with it; it does not require the licence of rendered output
  to change. The pre-rendered 4bpp bitmaps in a pack are such output (they are
  not the font software and cannot be used to reconstruct a usable font file).
  We nevertheless credit the font in README/attribution screens as a courtesy,
  and we never redistribute the `.ttf` files (they stay in `cache/`). The
  reserved font name restrictions apply only to modified/redistributed font
  files, which we do not ship. A user may substitute any font with `--font`;
  its licence must then be checked by whoever distributes the resulting pack.
- Suggested credit: "Glyphs rendered from Klee One, (c) Fontworks Inc.,
  SIL Open Font License 1.1."

## Adafruit GLCD font (built-in UI font)

- The firmware's small text and the meaning options use the built-in GLCD font
  (Font0) from LovyanGFX/M5GFX: the classic Adafruit 5x7 font, BSD-2-Clause
  (Copyright (c) 2012 Adafruit Industries). It is not content of a pack; no
  attribution is required in generated packs.
- The pack builder vendors a copy of its 256-glyph bitmap in `glcd.py` so
  `tangocho-pack preview` can render the same font as the device.

## Combined attribution for a pack

> Kanji data from KANJIDIC2 (c) EDRDG, CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html.
> JLPT levels from Jonathan Waller's JLPT Resources (tanos.co.uk) via
> davidluzgouveia/kanji-data (MIT). Glyphs rendered from Klee One
> (c) Fontworks Inc., SIL OFL 1.1. This pack is licensed CC BY-SA 4.0.
