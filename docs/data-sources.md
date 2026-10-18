# Data sources and licences

Code in this repository is MIT. **Generated packs (`*.tgc`) contain data
derived from KANJIDIC2** and, when built with `--strokes`, from **KanjiVG**,
and are licensed **CC BY-SA** (KANJIDIC2 is CC BY-SA 4.0, KanjiVG is CC BY-SA
3.0 and ShareAlike permits redistribution of the combined work as a later
version). Anything downloaded by `tangocho-pack fetch` lands in
`tools/pack-builder/cache/` (git-ignored) and is never committed. The only
third-party-derived files in the repo are the three kanji in
`tools/pack-builder/tests/data/mini_kanjidic2.xml` (a KANJIDIC2 excerpt), the
three KanjiVG SVG excerpts in `tools/pack-builder/tests/data/kanjivg/`, the
fixture pack built from them, and the hand-written WaniKani API response
fixtures in `tools/pack-builder/tests/data/wanikani/` (a handful of kanji and
their English glosses, © Tofugu LLC, used only to test the importer offline),
and the UX mockups in `docs/ux/*.png`. The mockups render five N5 kanji with
their KANJIDIC2 glosses and readings (hard-coded in
`tools/ux-mockups/render.py`), Klee One glyphs and the KanjiVG stroke shapes of
one kanji, so they carry the same CC BY-SA terms and attribution as a pack.

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

## WaniKani (optional; user's own token)

- URL: https://api.wanikani.com/v2/ — the required `Wanikani-Revision` header is
  `20170710`, the current (and so far only) documented revision,
  https://docs.api.wanikani.com/. Endpoints used: `/assignments` (which kanji
  the user has unlocked) and `/subjects` (their characters, meanings, readings).
- Owner: **Tofugu LLC. WaniKani is a trademark of Tofugu LLC.** Tangochō is an
  unofficial third-party tool; it is not affiliated with, endorsed by or
  sponsored by WaniKani or Tofugu.
- Licence/terms: WaniKani's content (subject meanings, readings, mnemonics…)
  is © Tofugu LLC and is governed by the WaniKani terms of service and the API
  terms. **Tangochō ships and redistributes no WaniKani content**: nothing is
  fetched unless the user runs the importer with their *own* personal API token,
  and a pack built this way is **the user's own responsibility**. It is intended
  for that user's personal study, must not be redistributed, and must respect
  the account's subscription (`max_level_granted`). The API rules apply: no
  login/password authentication (tokens only) and no for-profit use of the
  content.
- Token handling: supplied at build time with `--wanikani-token` or the
  `WANIKANI_TOKEN` environment variable. It is sent only in the request's
  `Authorization: Bearer` header and is **never** logged, printed, committed or
  written to `cache/`.
- Used for: cards from the kanji the user has **unlocked** only — assignments
  with `data.unlocked_at` set, `data.hidden` false and `data.subject_type ==
  "kanji"` — joined by subject id to the kanji subjects. Locked and hidden
  kanji are never imported, and nothing is ever written back to WaniKani.
- Caveats:
  - **Meanings are English-only.** WaniKani has no Spanish/French/Portuguese
    meanings, so a non-English `--lang` falls back to English **and sets the
    card's fallback flag** (bit0, the same convention as KANJIDIC2).
  - On'yomi come back in hiragana and are shifted to **katakana** so the pack
    contract holds (`docs/pack-format.md`: on'yomi katakana, kun'yomi hiragana);
    kun'yomi stay hiragana. `nanori` readings are dropped.
  - `jlpt`, `grade`, `strokes` and `freq` are 0 (unknown): WaniKani has no
    equivalent. The ordering hint is WaniKani level, then subject id.
  - The importer stays under the API's 60-requests/minute limit and reports a
    clear, token-free error on 401/403/429.
- Fixture provenance: `tools/pack-builder/tests/data/wanikani/*.json` are
  hand-written, minimal API responses (a few subjects/assignments, including a
  locked one) so the importer's tests run offline. They contain no real account
  data.

## KanjiVG (stroke order)

- URL: https://github.com/KanjiVG/kanjivg/releases/tag/r20260714 (asset
  `kanjivg-20260714-main.zip`, published 2026-07-14; pinned in `fetch.py`).
- Owner: Ulrich Apel.
- Licence: Creative Commons Attribution-ShareAlike 3.0
  (https://creativecommons.org/licenses/by-sa/3.0/; the release ships its
  `COPYING`, fetched to `cache/kanjivg/COPYING`).
- Required attribution: "Stroke order data from KanjiVG, (c) Ulrich Apel,
  CC BY-SA 3.0, https://kanjivg.tagaini.net."
- Used for: the optional per-stroke masks in a pack's stroke section
  (`--strokes`). Each SVG `<path>`, in document order, is one stroke. The
  centre line is flattened into polylines and stroked into a 1bpp mask at
  `--strokes-px` (default: `--large-px`); KanjiVG's `109x109` viewBox is mapped
  onto the `px` em square (see docs/pack-format.md).
- Rasterisation note: KanjiVG paths are *centre lines* (the group carries
  `stroke-width:3`), not filled outlines, so the builder strokes the polyline
  with a width proportional to `3/109` of `px` (`ImageDraw.line` with round
  caps) rather than filling it as a polygon. A path that uses an unsupported
  command (arcs, `A`) is skipped with a warning; coordinates outside the viewBox
  are clamped.
- `--strokes` is off by default: a pack without it has no stroke section and
  carries no KanjiVG data (none is redistributed unless asked for).
- Fixture provenance: `tools/pack-builder/tests/data/kanjivg/065e5.svg`,
  `06708.svg` and `0706b.svg` are verbatim excerpts from the r20260714 release
  (the three mini kanji 日/月/火). They are committed, with this attribution, so
  the `mini.tgc` fixture and its tests need no network.

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

## Root CA certificates (firmware)

`firmware/src/platform/RootCerts.h` embeds eleven public root CA certificates
(ISRG, USERTrust/Sectigo, DigiCert, Google Trust Services, Amazon) so the
device can verify HTTPS for Get decks. They are copied verbatim from Mozilla's
root store as packaged by certifi (MPL-2.0 for the bundle); CA certificates are
published by their issuers for exactly this use. Not data in any pack. The list,
expiry dates and the update procedure are in docs/decisions.md.

Packs the device downloads through Get decks are the release's prebuilt packs
(CC BY-SA, attribution in the release notes and below); nothing is bundled with
the firmware.

## Combined attribution for a pack

> Kanji data from KANJIDIC2 (c) EDRDG, CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html.
> JLPT levels from Jonathan Waller's JLPT Resources (tanos.co.uk) via
> davidluzgouveia/kanji-data (MIT). Glyphs rendered from Klee One
> (c) Fontworks Inc., SIL OFL 1.1. Stroke order data from KanjiVG
> (c) Ulrich Apel, CC BY-SA 3.0, https://kanjivg.tagaini.net (present only in
> packs built with `--strokes`). This pack is licensed CC BY-SA.

A pack built with `--wanikani-token` is different: it contains WaniKani-derived
content (© Tofugu LLC) and must not be redistributed; the CC BY-SA licence above
does not apply to it. WaniKani is a trademark of Tofugu LLC.
