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
