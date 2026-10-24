# Data sources and licences

Code in this repository is MIT. **Generated packs (`*.tgc`) contain data
derived from KANJIDIC2**, when built with `--strokes` from **KanjiVG**, and
when built with `--example-words` words from **JMdict**,
and are licensed **CC BY-SA** (KANJIDIC2 and JMdict are CC BY-SA 4.0, KanjiVG is CC BY-SA
3.0 and ShareAlike permits redistribution of the combined work as a later
version). Anything downloaded by `tamagotcho-pack fetch` lands in
`tools/pack-builder/cache/` (git-ignored) and is never committed. The only
third-party-derived files in the repo are the three kanji in
`tools/pack-builder/tests/data/mini_kanjidic2.xml` (a KANJIDIC2 excerpt), the
three KanjiVG SVG excerpts in `tools/pack-builder/tests/data/kanjivg/`, the
fixture packs built from them (glyphs rendered from Noto Sans JP), and the
hand-written WaniKani API response fixtures in
`tools/pack-builder/tests/data/wanikani/` (a handful of kanji and
their English glosses, © Tofugu LLC, used only to test the importer offline),
and the UX mockups in `docs/ux/*.png`. The mockups render five N5 kanji with
their KANJIDIC2 glosses and readings (hard-coded in
`tools/ux-mockups/render.py`), Klee One glyphs (the default font when they
were drawn) and the KanjiVG stroke shapes of one kanji, so they carry the same
CC BY-SA terms and attribution as a pack. The learning-mode mockups in
`docs/learning-mode/*.png` (`tools/ux-mockups/learning.py`) are the same kind:
the same KANJIDIC2 sample data, Noto Sans JP glyphs, the KanjiVG strokes of 休,
and two JMdict words with their readings and English glosses (休日, 休む;
CC BY-SA 4.0, EDRDG), hard-coded in the script. The firmware itself embeds glyph
bitmaps rendered from Noto Sans JP (`firmware/src/ui/UiFont.h`, see
[Noto Sans JP](#noto-sans-jp-default-font) below); it contains no dictionary
or stroke data.

## KANJIDIC2

- URL: https://www.edrdg.org/kanjidic/kanjidic2.xml.gz
- Owner: Electronic Dictionary Research and Development Group (EDRDG)
- Licence: Creative Commons Attribution-ShareAlike 4.0
  (https://www.edrdg.org/edrdg/licence.html)
- Required attribution: "This package uses the KANJIDIC2 file. These files are
  the property of the Electronic Dictionary Research and Development Group,
  and are used in conformance with the Group's licence."
- Used for: codepoint, meanings (en/es/fr/pt), on'yomi, kun'yomi, stroke
  count, school grade (also the Jōyō selection: grades 1-6 and 8, `--joyo`),
  newspaper frequency, the legacy 4-level JLPT field
  (fallback only, see below), and the classical KangXi radical numbers
  (`rad_value rad_type="classical"`, 1..214) used to rank meaning distractors.
  It is also the source of the entries selected by a `--kanji-file` list.
  Radicals are a builder-side selection signal only: they are **not** stored in
  the pack and the pack format is unchanged (see docs/decisions.md).

### Meaning and reading conventions

- Meanings: the first ones in the requested language (`m_lang`; no attribute =
  English), at most as many as English has and at most 3. If the language has
  none for a kanji, the first 3 English meanings are used and card flag bit0 is
  set. KANJIDIC2 has no meanings at all for some kanji; cards with neither
  meanings nor readings are dropped. With `--meaning-order` the English list
  is first ordered by a learner signal, the Spanish list by the new English
  one, and a few overrides apply (see
  [Meaning order](#meaning-order-optional-fetch---readings---meanings-build---meaning-order)),
  so the caps keep the most useful meanings.
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
  under Creative Commons Attribution. Verified on 2026-10-18 from the site's
  "Use my data!" page (https://www.tanos.co.uk/jlpt/sharing/, updated
  2011-10-12): "Everything on this site (that I'm not selling), is licenced
  under Creative Commons "BY". Basically this means... use anything here
  however you like (commercial or non-commercial), but credit my site. (A link
  would be nice.)" The site itself no longer resolves (DNS, 2026-10-18); the
  quote is from the Wayback Machine copy
  https://web.archive.org/web/20250117052055/https://www.tanos.co.uk/jlpt/sharing/.
  CC BY material can be included in a CC BY-SA work as long as it is credited,
  so the packs stay CC BY-SA with the attribution below. The file also contains WaniKani-derived
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
- Owner: **Tofugu LLC. WaniKani is a trademark of Tofugu LLC.** Tamagotcho is an
  unofficial third-party tool; it is not affiliated with, endorsed by or
  sponsored by WaniKani or Tofugu.
- Licence/terms: WaniKani's content (subject meanings, readings, mnemonics…)
  is © Tofugu LLC and is governed by the WaniKani terms of service and the API
  terms. **Tamagotcho ships and redistributes no WaniKani content**: nothing is
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

## Noto Sans JP (default font)

- URL: https://github.com/notofonts/noto-cjk, release tag `Sans2.004`
  (Noto Sans CJK 2.004, 2022-01-27; pinned in `fetch.py`): the Japanese subset
  `Sans/SubsetOTF/JP/NotoSansJP-Regular.otf` and the repository's `LICENSE`,
  fetched to `cache/noto/`.
- Licence: SIL Open Font License 1.1. © 2014-2021 Adobe
  (http://www.adobe.com/). Noto is a trademark of Google Inc.
- Used for: rendering the kanji and text glyphs of a pack (sets 0-3) into 4bpp
  bitmaps at build time. Only the Regular weight is used: Medium clogged dense
  kanji at the 40 px option size (docs/decisions.md, 2026-10-18).
- Also used for: **the firmware's UI font**. `tools/ui-font/gen_ui_font.py`
  renders ASCII, Latin-1, `· … › ‹ « » – — ✓ ▲ ▼` and the six kana of the
  app's name (たまごっちょ) at 18 px, the digits with `%:/+-.` at 32 px and
  those six kana again at 12 px (the header) from the same Regular OTF, with
  the builder's `FontRenderer`, into `firmware/src/ui/UiFont.h` (228 glyphs,
  ~16 KB of 4bpp bitmaps). That header is committed and compiled into every
  firmware image, so the firmware distributes these bitmaps; they carry the
  font's credit (README, Settings › Device › About "UI font").
- OFL notes: the OFL permits use, embedding and redistribution of the font and
  of output made with it; it does not require the licence of rendered output
  to change. The pre-rendered 4bpp bitmaps in a pack, and those in the
  firmware's `UiFont.h`, are such output (they are not the font software and
  cannot be used to reconstruct a usable font file: fixed sizes, no outlines,
  no hinting, a few hundred characters).
  We nevertheless credit the font in README/attribution screens as a courtesy,
  and we never redistribute the `.otf` file (it stays in `cache/`). The
  reserved font name restrictions apply only to modified/redistributed font
  files, which we do not ship. A user may substitute any font with `--font`;
  its licence must then be checked by whoever distributes the resulting pack.
- Suggested credit: "Glyphs rendered from Noto Sans JP, (c) Adobe,
  SIL Open Font License 1.1."

## Klee One (optional font)

- URL: https://github.com/fontworks-fonts/Klee (`fonts/ttf/KleeOne-SemiBold.ttf`,
  `KleeOne-Regular.ttf`, `OFL.txt`), fetched to `cache/fonts/` only with
  `tamagotcho-pack fetch --klee-one`.
- Licence: SIL Open Font License 1.1. Copyright Fontworks Inc. The OFL notes
  above apply in the same way.
- Used for: nothing by default since 2026-10-18 (it was the default font
  before). A pack built with `--font cache/fonts/KleeOne-SemiBold.ttf` renders
  its glyphs from it, and the UX mockups in `docs/ux/` were drawn with it.
- Suggested credit for such a pack: "Glyphs rendered from Klee One,
  (c) Fontworks Inc., SIL Open Font License 1.1."

## Adafruit GLCD font (built-in caption font)

- The firmware's captions (headers, hints, values, metadata) and, until the
  quiz is restyled, its small text and meaning options use the built-in GLCD
  font (Font0) from LovyanGFX/M5GFX: the classic Adafruit 5x7 font,
  BSD-2-Clause (Copyright (c) 2012 Adafruit Industries). It is not content of
  a pack; no attribution is required in generated packs.
- The pack builder vendors a copy of its 256-glyph bitmap in `glcd.py` so
  `tamagotcho-pack preview` can render the same font as the device.

## Reading order (optional: `fetch --readings`, `build --common-readings`)

`build --common-readings` reorders each card's on'yomi and kun'yomi so the
reading the device asks is a useful one, and leaves a kind with nothing worth
asking empty (docs/common-readings.md). The six inputs below are downloaded
only by `tamagotcho-pack fetch --readings`, into `cache/readings/`, and read only
with that flag and `--example-words` (and by the scripts in
`tools/pack-builder/scripts/`). A pack built with `--common-readings` alone
contains no word data from them, only KANJIDIC2's own readings in a different
order (some kinds left out), but it is derived from them, so it carries their
attribution (below); a pack built with `--example-words` also stores JMdict
words, readings and English glosses ([Example words](#example-words-optional-fetch---readings-build---example-words)).
`decks.yml` builds the published decks with both (`decks-v3`, from
2026-10-24); `decks-v2`, which v0.5.1 and older read, keeps its packs
without words.

### JMdict

- URL: https://www.edrdg.org/pub/Nihongo/JMdict_e.gz (rebuilt daily; not pinned).
- Owner: EDRDG. Licence: CC BY-SA 4.0 (https://www.edrdg.org/edrdg/licence.html),
  the same terms and attribution as KANJIDIC2 ("This package uses the JMdict
  and KANJIDIC2 files…").
- Used for: the list of words (kanji form + reading) and their priority tags
  (`nfXX`, `news1`, `ichi1`…), as a prior for how common each word is; and,
  with `--meaning-order`, the English glosses of the JLPT vocabulary words
  (docs/meaning-order.md); with `--example-words`, the example words
  themselves, their readings and English glosses, stored in the pack. Only the
  English file is read (its Japanese and English parts are what the EDRDG
  licence covers).

### JmdictFurigana

- URL: https://github.com/Doublevil/JmdictFurigana, release
  `2.3.1+2026-09-25`, asset `JmdictFurigana.txt` (pinned in `fetch.py`).
- Owner: Doublevil. Licence: "distributed under the same licence as JMDict
  (Creative Commons Attribution-ShareAlike Licence)" (README); the code is MIT.
- Used for: which part of each JMdict word's reading belongs to each kanji
  (学校 = 学:がっ + 校:こう).

### wordfreq (Japanese list)

- URL: https://github.com/rspeer/wordfreq, tag `v3.2`, file
  `wordfreq/data/large_ja.msgpack.gz` (pinned in `fetch.py`).
- Owner: Robyn Speer. Licence: the code is Apache-2.0; the data files "may be
  redistributed under a Creative Commons Attribution-ShareAlike 4.0 license"
  (README, "License"). The Japanese list is built from Wikipedia, OpenSubtitles
  2018 ("may be used with attribution to OpenSubtitles"), OSCAR web text,
  Twitter and Reddit, tokenised with MeCab/IPAdic. A frozen snapshot (~2021).
- Required attribution: wordfreq by Robyn Speer, CC BY-SA 4.0, which includes
  data from OpenSubtitles (http://www.opensubtitles.org/).
- Used for: how often each word's written form (and a verb's inflection stems)
  occurs. Read with a small MessagePack decoder in `readings.py`; the
  `wordfreq` package is not a dependency.

### Tatoeba Japanese indices

- URL: https://downloads.tatoeba.org/exports/jpn_indices.tar.bz2 (weekly
  export; not pinned).
- Owner: the Tatoeba Project (the indices of the Tanaka Corpus, which Tatoeba
  maintains). Licence: CC BY 2.0 FR (https://tatoeba.org/en/downloads: "These
  files are released under CC BY 2.0 FR"; the corpus moved from the public
  domain to CC BY in 2009, https://www.edrdg.org/wiki/Tanaka_Corpus.html).
  CC BY material may be used in a CC BY-SA work with attribution.
- Required attribution: "Sentence indices from the Tatoeba Project
  (https://tatoeba.org), CC BY 2.0 FR."
- Used for: counts of each JMdict headword per reading in ~150,000 sentences
  (上(うえ) vs 上(じょう)), to split a homograph's frequency among its readings.
  No sentence text is used.

### Jōyō kanji table (常用漢字表)

- URL: https://www.bunka.go.jp/kokugo_nihongo/sisaku/joho/joho/kijun/naikaku/pdf/joyokanjihyo_20101130.pdf
  (Agency for Cultural Affairs; the table of Cabinet notice No. 2 of
  2010-11-30).
- Licence: not subject to copyright in Japan: Copyright Act art. 13(ii)
  excludes "notifications, instructions, circular notices, and other similar
  materials issued by a national or local government agency"
  (https://www.japaneselawtranslation.go.jp/en/laws/view/4207). The Agency's
  site also puts its content under the MEXT terms (公共データ利用規約 1.0,
  compatible with CC BY 4.0, https://www.mext.go.jp/b_menu/1351168.htm).
- Suggested credit: "Jōyō kanji table (常用漢字表), Agency for Cultural
  Affairs, 2010."
- Used for: which readings the table approves for each kanji and which it
  indents as special or of narrow use (`joyo_table.py`, read with pypdf):
  approved readings come first, and a Jōyō kanji's kind with no approved
  reading is left empty. The PDF stays in `cache/`.

### JLPT vocabulary lists (tanos.co.uk)

- URLs: the Internet Archive's captures of
  `http://www.tanos.co.uk/jlpt/jlpt{N}/vocab/n{N}-vocab-kanji-hiragana.anki`
  for N = 5..1, taken 2011-07-22, fetched through Wayback `id_` URLs (which
  return the archived bytes unchanged) and pinned in `fetch.py` by capture
  timestamp and SHA256. tanos.co.uk itself no longer resolves; the 2025
  captures of the N1-N3 files have the same digests.
- Owner: Jonathan Waller (JLPT Resources). Licence: CC BY, from the same "Use
  my data!" page as the JLPT kanji levels above ("Everything on this site (that
  I'm not selling), is licenced under Creative Commons "BY"…"); the vocabulary
  lists were free downloads, not sold.
- Required attribution: "JLPT vocabulary lists from Jonathan Waller's JLPT
  Resources (tanos.co.uk), CC BY."
- Format: Anki 1.x decks (SQLite files; `Front` = the word in kanji, `Back` =
  its reading), read with Python's `sqlite3`. 7,270 entries, 7,224 (word,
  reading) pairs after expanding alternatives and skipping notes.
- Used for: the learner signal: a reading met in an easier JLPT list's word
  comes first (docs/common-readings.md); and, with `--meaning-order`, a meaning
  of the words met in easier lists comes first (docs/meaning-order.md).

Test fixtures: `tools/pack-builder/tests/test_readings.py` holds hand-written
miniatures of each format (a few JMdict-style entries such as 九つ/ここのつ, a
furigana line per word, a three-token wordfreq list, two Tatoeba index lines,
a few rows shaped like the Jōyō table's text and two-table Anki decks with a
handful of words). They are not copied from the files above.

## Meaning order (optional: `fetch --readings --meanings`, `build --meaning-order`)

`build --meaning-order` puts each card's most useful meaning first, the one the
device shows (docs/meaning-order.md): the English meanings by a learner signal
(the JLPT vocabulary words that use the kanji, through their JMdict glosses:
both inputs of `--common-readings` above), the Spanish ones in the order of
the English ones they translate (the dictionary below), then a short
project-authored override table. The dictionary is downloaded only by
`tamagotcho-pack fetch --meanings`, into `cache/meanings/`, and read only with
that flag. A pack built with it contains no word or dictionary data, only
KANJIDIC2's own meanings in another order (the caps then keep the first ones),
but the order is derived from these sources, so the pack carries their
attribution (below). `decks.yml` is prepared to build the published decks with
it; the decks published so far use none of it.

### FreeDict English-Spanish dictionary (WikDict)

- URL: https://download.freedict.org/dictionaries/eng-spa/2025.11.23/freedict-eng-spa-2025.11.23.src.tar.xz
  (FreeDict release `2025.11.23`, listed with its SHA512 in
  https://freedict.org/freedict-database.json; pinned in `fetch.py` by SHA256).
  The archive holds the TEI dictionary (64,258 English headwords, their
  Spanish translations per sense) and the licence (`COPYING`); the builder
  reads the TEI from the archive.
- Owner: Karl Bartel (WikDict, maintainer of the FreeDict release), from
  Wiktionary via DBnary (Gilles Sérasset).
- Licence: Creative Commons Attribution-ShareAlike 3.0 Unported (the TEI
  header: "Licensed under the Creative Commons Attribution-ShareAlike 3.0
  Unported license"; https://creativecommons.org/licenses/by-sa/3.0/legalcode).
  DBnary: "Dbnary is derived from Wiktionary and is distributed under Creative
  Commons Attribution-ShareAlike 3.0" (https://kaiko.getalp.org/about-dbnary/);
  WikDict: "Data from Wiktionary via DBnary licensed under the Creative Commons
  Attribution-ShareAlike License" (https://www.wikdict.com/page/about). CC BY-SA
  3.0 §4(b) lets an adaptation use a later version with the same licence
  elements, so packs stay CC BY-SA 4.0 (as with KanjiVG).
- Required attribution: "Spanish meaning order from FreeDict's English-Spanish
  dictionary (WikDict, from Wiktionary via DBnary), CC BY-SA 3.0."
- Used for: which English gloss each Spanish gloss translates (with
  normalisation and related forms, docs/meaning-order.md).
- Not used, by licence: the full multilingual JMdict's Spanish glosses. The
  EDRDG licence covers "the Japanese and English components (the
  translational equivalents in other languages, e.g. German, French, Dutch,
  etc. are covered by separate copyright held by the compilers of that
  material)" (https://www.edrdg.org/edrdg/licence.html).
  `scripts/example_words_report.py --fetch` downloads `JMdict.gz` into the
  cache only to measure how many example words have a Spanish gloss
  (docs/example-words.md); no pack contains them.

### Meaning overrides (project-authored)

- File: `tools/pack-builder/data/meaning_overrides.tsv`, committed in this
  repository (not downloaded): a short hand-checked table (39 lines) that puts
  one of KANJIDIC2's own meanings first for a kanji and language where the
  steps above go wrong (機 "machine", 申 "decir"), with the reason.
- Authored by the Tamagotcho project. Licence: it is distributed with the packs
  under CC BY-SA (and with the code, MIT); it holds no third-party text beyond
  the KANJIDIC2 meanings it names, each of which the builder checks against
  KANJIDIC2 (an override can only choose among them, never add text).
- Used for: the last step of `--meaning-order`.

Test fixtures: `tools/pack-builder/tests/test_meanings.py` holds hand-written
miniatures in the TEI, JMdict and Anki-deck formats (a dozen English words
with Spanish translations, four JMdict entries, a handful of vocabulary words)
and KANJIDIC2-shaped entries with invented glosses; they are not copied from
the sources or KANJIDIC2.

## Example words (optional: `fetch --readings`, `build --example-words`)

`build --example-words` puts **JMdict word data in the pack** (pack format
v3, docs/pack-format.md "Example-words section"): per card, up to two common
words that use its asked readings (docs/example-words.md), each with its
reading, and a gloss in the deck's language. It reads the inputs of
`--common-readings` above (JMdict, JmdictFurigana, wordfreq, Tatoeba, the JLPT
vocabulary lists) and the table below; `sync-glosses` also reads FreeDict
(`fetch --meanings`). Packs built without the flag contain no word data, as
before. With it:

- **The words and their readings** are JMdict's (`JMdict_e`, its Japanese
  part): CC BY-SA 4.0 under the EDRDG licence, with the same attribution as
  KANJIDIC2 ("This package uses the JMdict and KANJIDIC2 files…"), which the
  device already shows for both files (`credits::kKanjidic`). The part of a
  reading each kanji reads comes from JmdictFurigana (CC BY-SA). The choice
  of the word is derived from wordfreq (CC BY-SA 4.0), Tatoeba (CC BY 2.0 FR)
  and the JLPT lists (CC BY), already credited for the reading order; no data
  of theirs is stored.
- **English glosses** are JMdict_e's own (its English part, CC BY-SA 4.0,
  EDRDG): the first gloss of the word's first sense that is not a
  transliteration, when it has at most 24 characters (if need be without its
  parenthesised notes).
- **Spanish glosses** come only from the project's table below, rows marked
  `ok`. Never from the multilingual JMdict, whose Spanish glosses are outside
  the EDRDG's grant (above).
- **French and Portuguese packs** get the words and readings with no gloss.
- The words' kanji are rendered from the pack's font into glyph set 2, like
  every other glyph (Noto Sans JP by default).

Packs stay CC BY-SA 4.0. The table's `skip` rows choose which words are
shown, in every language.

### Spanish example glosses (project-authored)

- File: `tools/pack-builder/data/example_glosses_es.tsv`, committed in this
  repository: one row per example word (word, reading) with its Spanish
  gloss and a review status (`draft`, `ok`, `skip`). `build --example-words
  --lang es` uses only `ok` rows and leaves every other word without a gloss;
  `skip` rows are never an example (in any language). `sync-glosses` appends
  the words a deck picks and the table lacks as `draft` rows, pre-filled with
  FreeDict's translation of the JMdict_e gloss, for review. Reviewed so far:
  N5 (2026-10-23).
- Authored by the Tamagotcho project, distributed with the packs under CC BY-SA
  4.0. Derived from JMdict_e's English glosses (EDRDG, CC BY-SA 4.0) and, where
  a draft kept its suggestion, FreeDict eng-spa (CC BY-SA 3.0, above). No text
  from JMdict's Spanish glosses (docs/example-words.md, "Alternatives").

Test fixtures: `tools/pack-builder/tests/data/mini_jmdict.xml`,
`mini_furigana.txt` and `mini_glosses_es.tsv` are hand-written miniatures in
the JMdict, JmdictFurigana and table formats (nine words with 日, 月 and 火,
glosses written for the tests), not copied from the sources; the v3 fixture
pack `fixtures/mini_words.tgc` is built from them and the mini KANJIDIC2
excerpt.

### Sources evaluated (no pack uses them)

`tools/pack-builder/scripts/example_words_alternatives.py --fetch` downloads
these into `cache/research/` (git-ignored, SHA256-pinned in the script as
fetched on 2026-10-23) to compare Spanish glosses for the example words of
decision 9 (docs/example-words.md, "Alternatives"). FreeDict eng-spa (above)
and JMdict (all languages, above) were compared too.

- **Open Multilingual Wordnet 1.4** data release,
  https://github.com/omwn/omw-data/releases/download/v1.4/omw-1.4.tar.xz
  (49 MB). Read: the Japanese WordNet (NICT; licence: "Permission to use,
  copy, modify and distribute this software and database … provided that …
  the same appear on ALL copies", `omw-ja/LICENSE`), the Princeton WordNet 3.0
  (the same terms, `omw-en/LICENSE`) and the Spanish wordnet of the
  Multilingual Central Repository 3.0 ("distributed under Attribution 3.0
  Unported (CC BY 3.0)", `omw-es/LICENSE`, https://adimen.si.ehu.es/web/MCR).
  Compatible with CC BY-SA 4.0 packs with attribution and the notices.
- **Wiktionary via kaikki.org** (wiktextract, Tatu Ylonen):
  https://kaikki.org/dictionary/Japanese/kaikki.org-dictionary-Japanese.jsonl.gz
  (English Wiktionary's Japanese entries, 49 MB) and
  https://kaikki.org/eswiktionary/raw-wiktextract-data.jsonl.gz (Spanish
  Wiktionary, 103 MB). Unversioned, regenerated weekly. Wiktionary text is CC
  BY-SA 4.0 (and GFDL): "Permission is granted to copy, distribute and/or
  modify the text of all Wiktionary entries under the terms of the Creative
  Commons Attribution-ShareAlike 4.0 International License"
  (https://en.wiktionary.org/wiki/Wiktionary:Copyrights); attribution:
  Wiktionary contributors, with a link. kaikki.org asks for a link and a
  citation, not a licence.
- **Wikidata lexemes**: a SPARQL query (in the script) for Japanese lexemes,
  their item-linked senses (P5137) and the items' Spanish labels, from
  https://query.wikidata.org/sparql. CC0: "All structured data (i.e. the
  main, Property, Lexeme, and EntitySchema namespaces) is released into the
  public domain" (https://www.wikidata.org/wiki/Wikidata:Licensing).
- **JMdict's Spanish glosses**, for the record: WWWJDIC's JSPANISH file, which
  combines HISPADIC (a collaborative Spanish translation of EDICT, "licencia
  creative commons" of unstated version) and RUI (Francisco Barberan,
  "included with his permission"); https://www.edrdg.org/wwwjdic/wwwjdicinf.html.

## Root CA certificates (firmware)

`firmware/src/platform/RootCerts.h` embeds eleven public root CA certificates
(ISRG, USERTrust/Sectigo, DigiCert, Google Trust Services, Amazon) so the
device can verify HTTPS for Get decks. They are copied verbatim from Mozilla's
root store as packaged by certifi (MPL-2.0 for the bundle); CA certificates are
published by their issuers for exactly this use. Not data in any pack. The list,
expiry dates and the update procedure are in docs/decisions.md.

Packs the device downloads through Get decks are the prebuilt packs of the
dedicated decks release (`decks-v<pack format>`, published by
`.github/workflows/decks.yml`; CC BY-SA, attribution in that release's notes and
below); nothing is bundled with the firmware.

## Combined attribution for a pack

> Kanji data from KANJIDIC2 (c) EDRDG, CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html.
> JLPT levels from Jonathan Waller's JLPT Resources (tanos.co.uk) via
> davidluzgouveia/kanji-data (MIT). Glyphs rendered from Noto Sans JP
> (c) Adobe, SIL OFL 1.1. Stroke order data from KanjiVG
> (c) Ulrich Apel, CC BY-SA 3.0, https://kanjivg.tagaini.net (present only in
> packs built with `--strokes`). This pack is licensed CC BY-SA.

A pack built with another `--font` credits that font instead of Noto Sans JP.
A pack built with `--common-readings` says "JLPT levels and vocabulary lists
from Jonathan Waller's JLPT Resources (tanos.co.uk, CC BY)" and adds:

> Reading order from JMdict (EDRDG, CC BY-SA 4.0), JmdictFurigana (CC BY-SA),
> wordfreq (Robyn Speer, CC BY-SA 4.0, with data from OpenSubtitles), the
> Tatoeba Project's sentence indices (CC BY 2.0 FR) and the Jōyō kanji table
> (Agency for Cultural Affairs, 2010).

A pack built with `--meaning-order` adds (the FreeDict part for Spanish):

> Meaning order from Jonathan Waller's JLPT vocabulary lists (tanos.co.uk,
> CC BY) and JMdict (EDRDG, CC BY-SA 4.0); Spanish meaning order from FreeDict's
> English-Spanish dictionary (WikDict, from Wiktionary via DBnary), CC BY-SA
> 3.0; and the Tamagotcho project's meaning overrides.

A pack built with `--example-words` (pack format v3) adds (the second
sentence for Spanish):

> Example words, their readings and English glosses from JMdict (EDRDG, CC
> BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html), with JmdictFurigana
> (CC BY-SA); chosen with wordfreq, the Tatoeba indices and the JLPT
> vocabulary lists above. Spanish glosses by the Tamagotcho project, CC BY-SA
> 4.0, translated from JMdict's English glosses with suggestions from
> FreeDict's English-Spanish dictionary (WikDict, from Wiktionary via DBnary,
> CC BY-SA 3.0).

The decks release notes in `decks.yml` carry this text (the example-words
paragraph from the first `decks-v3` release on, docs/pack-format.md
"Rollout of v3").

The firmware shows a short form of this attribution on the device
(**Settings › Device › About**: the loaded deck's name, then KANJIDIC2 and JMdict, KanjiVG
when the deck has stroke order, the JLPT levels, the other reading-order sources
(`credits::kReadings`, "Readings:"), the meaning-order source
(`credits::kMeanings`, "Meanings:") and the glyph font, each with its licence;
"Firmware: MIT", "Decks: CC BY-SA", "UI font: Noto Sans JP, SIL OFL 1.1" and
the repository) and in the footer of the Wi-Fi portal page, which lists the
same sources. Both read the strings in
`firmware/src/Credits.h`: change them together with this file. Packs carry no
list of their sources, so the device shows these credits for any deck,
including one built with another font or from WaniKani (docs/decisions.md,
2026-10-18 "About screen and firmware version").

Example words need **no new device credit**: their data are JMdict (already
in `credits::kKanjidic`, "KANJIDIC2, JMdict (c) EDRDG"), JmdictFurigana,
wordfreq, Tatoeba and the JLPT lists (`kReadings`, `kJlpt`), and FreeDict plus
the project's own glosses (`kMeanings`, whose source the Spanish table's
suggestions share; the project's work is covered by "Decks: CC BY-SA"). The
comments in `Credits.h` say so. If the intro screen labels the words, the
About page could name them ("Words: JMdict"), which is a UI string, not a new
source.

A pack built with `--wanikani-token` is different: it contains WaniKani-derived
content (© Tofugu LLC) and must not be redistributed; the CC BY-SA licence above
does not apply to it. WaniKani is a trademark of Tofugu LLC.
