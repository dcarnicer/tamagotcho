# Decisions

Short log of non-obvious choices. Newest last.

## 2026-10-16 – Project scope and shape

- **Name:** Tangochō (単語帳), after the ring-bound flashcards the StickS3 resembles.
- **Kanji only** for now; vocabulary may come later.
- **Multiple choice, not typed answers.** Does not sync progress to WaniKani.
- **No hard dependency on WaniKani.** Default content from KANJIDIC2 (EDRDG,
  CC BY-SA 4.0). WaniKani becomes an optional importer using the user's own
  API token, so no WaniKani content is redistributed.
- **Pre-rendered glyphs in packs** instead of a CJK font in firmware: a full
  Japanese font is several MB and only a few hundred glyphs are needed per deck.
- **Default font: Klee One** (Fontworks, SIL OFL 1.1). Textbook (教科書体)
  style, closer to handwritten stroke shapes than a gothic font, which matters
  for learners. User-selectable in the builder.
- **SRS: FSRS** (open algorithm, as in Anki). Multiple choice gives weaker
  signal than recall, so grade = Again on miss, Hard on slow correct, Good on
  fast correct.
- **Repo language: English** for docs and code; it is meant to be published.

## 2026-10-16 – Phase 0 implementation choices

- **No OTA partitions.** Single 2 MB factory app, rest of flash to LittleFS
  (~5.9 MB on StickS3, ~13.9 MB on T-Display-S3). Firmware is ~590 KB, so
  OTA can be added later by splitting the app space.
- **Reading questions use one reading**: first on'yomi, else first kun'yomi,
  with the okurigana dot removed for display (`た.べる` → `たべる`).
  Distractors use the same reading kind when possible.
- **Kanji square baseline is its bottom edge**; all pack offsets are absolute
  (clarified in the spec after the first implementation pass).
- **M5Unified/M5PM1 pinned to git commits** for reproducible builds.

## 2026-10-17 – Phase 0/1 validated on hardware

- **On-device check done** on a StickS3 with the N5 Spanish pack: LittleFS,
  pack open/validation, kanji + text glyphs, buttons (B moves, A selects),
  reveal feedback, next card, and meaning/reading alternation all work.
- **Header uses the built-in GFX font.** The deck name is now drawn with
  `Font0`, same size as the battery indicator (left-aligned), matching the
  MEANING/READING label. Trade-off: a non-ASCII deck name would no longer
  render; default names are ASCII, so accepted.
- **Quiz tracing over serial** behind `-DTGC_QUIZ_LOG` (enabled in
  `platformio.ini` for now): prints each question (mode, codepoint, options,
  correct) and each answer. Cheap and useful while the UI and scheduling settle.
- **Option text size is a builder/authoring choice**, not a firmware one
  (glyphs are pre-rendered): 18 px (`--text-px`) looked too large for four
  options on the 135 px panel, 14 px is preferred on device. Builder default
  left at 18 pending a decision.
