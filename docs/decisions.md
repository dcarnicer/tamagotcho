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

## 2026-10-17 – On/kun readings and random meanings

- **Reading questions ask both on'yomi and kun'yomi.** Before, any kanji with
  on'yomi was always asked with it, so kun readings only appeared on kun-only
  kanji. `buildQuestion` now takes the kind to ask for and the app alternates
  on and kun; a requested kind falls back to the card's primary kind for
  single-kind kanji. Distractors are matched by "has a reading of the asked
  kind", not by the distractor's own primary kind.
- **Meaning options pick one meaning at random** from the card's list (up to 3
  per pack) instead of always the first, so a multi-meaning kanji surfaces all
  of them over time. Each option is picked so it does not repeat a gloss already
  shown; a card with no free meaning is dropped as a distractor.
- **Distractors sharing *any* meaning with the target are excluded** by the
  builder (`same_level_similar_strokes`), not just those sharing the first,
  since the random pick could otherwise make two options correct at once.
- Options are deduplicated by card index, not by display string: with random
  meanings the same card could otherwise arrive twice and produce a different
  gloss the second time.

## 2026-10-17 – Quiz layout: per-mode options and large kana

- **Reading answers: a 2×2 grid.** Readings are short (1–3 kana) and wasted the
  full width; the grid gives each cell about 65×52 px and fits larger kana.
- **Meaning answers: one per row, full width**, drawn with the built-in GFX font
  (Font0) at 2× so Latin text is legible, matching the header and labels. The
  font has no glyph for some characters (e.g. uppercase-accented letters); such
  a question falls back to the pack's set-2 glyphs (never mixing fonts inside a
  word). `render/GlcdCp437.h` maps the Latin-1 letters Font0 keeps in its CP437
  slots and is unit-tested natively.
- **New glyph set 3 = large option text**, rendered at `--option-px` (default
  20) and described by header bytes 53–55 (`option_px`, `option_line_height`,
  `option_ascent`; 0 = absent, so no version bump). Reading answers use set 3
  when the whole question fits the cell, otherwise the small text. Readers fall
  back from set 3 to set 2 when a code point is absent from set 3.

## 2026-10-17 – One-button quiz control

- **The front button drives everything**: a tap cycles the highlight, a ~600 ms
  hold confirms it; once revealed, a tap moves to the next card. The side button
  stays as a tap shortcut (and is kept free for the deck menu).
- Tap and hold are separated by acting on release: the tap runs when the button
  goes up and is suppressed if a hold already fired, so a long press is never
  also counted as a tap. `main.cpp` keeps its own press timing (`isHeld` +
  `millis()`); the HAL still exposes press/hold edges.

## 2026-10-17 – Typical options for beginners

- **Meaning options use only a card's first gloss** (`kOptionMeanings = 1` in
  `Quiz.cpp`, correct option and distractors alike): even the first two surfaced
  too many odd senses, so for now the canonical first gloss is used until a
  better meaning source is found.
- **The builder caps non-English gloss lists to the English sense count.**
  KANJIDIC2's Spanish lists are not a translation of the English ones and often
  add derived or wrong senses (母: "madre", "base", "cimientos"; 火: "fuego",
  "flama", "llama"; 39 of 79 N5 cards have more Spanish senses than English).
  The English list is the canonical count, so `make_cards` keeps at most
  `len(en)` entries per language. 月 keeps ["mes", "luna"]; 母 becomes ["madre"].
  Caveat: this does not fix every odd 2nd gloss (日 still gives "día", "solar").
- Reading options already use each card's first reading, which is the typical
  one. Caveat: KANJIDIC's reading order is not frequency-based (九's first kun
  reading is the rare ここの), so a per-reading frequency source may be worth
  adding later (roadmap, phase 3).

## 2026-10-17 – Deck selector

- The firmware lists the `*.tgc` files in `/packs` (sorted) and opens the one
  saved in NVS (`Preferences`, namespace `tangocho`), falling back to the first.
  A new pack replaces the live one only after it validates, so a bad file cannot
  strand the device without a deck.
- Hold the side button to open a `DECKS` menu: tap moves, hold opens, the side
  button goes back; the loaded deck is marked `*`. NVS survives `uploadfs` (only
  LittleFS is replaced), so the choice sticks across firmware/fs updates.

## 2026-10-17 – Distractors: visual similarity and confusable readings

- Each card now has **two distractor lists**. The **meaning** list ranks
  candidates by visual similarity computed from the glyphs themselves: a 16×16
  average-ink vector of the em square, compared by cosine (so 日/百, 水/木,
  一/午 rank together). Same JLPT level first, then closest stroke count; cards
  sharing a gloss with the target are excluded.
- The **reading** list ranks candidates by how confusable their reading *of the
  same kind* is: Levenshtein distance 1–3 between first on/kun readings (e.g.
  カ/ゴ/フ, ショウ/ジョウ); identical readings are skipped (the quiz dedupes
  them anyway).
- Pack format: the reading list offset reuses the card's reserved u32
  (`distract_read`, offset 28; 0 = reuse the meaning list), so it is a
  backwards-compatible v1 extension. The firmware asks for it in Reading mode
  and falls back to the meaning list when absent.

## 2026-10-17 – Meaning-to-kanji questions

- A third question type: the prompt is the card's first gloss and the options
  are kanji in the 2x2 grid (set 1). This is the one place where confusable
  *kanji* are actually shown, so it uses the meaning list (ranked by visual
  similarity) as its distractors.
- `QuizMode::MeaningToKanji` reuses the meaning path in `buildQuestion` (first
  gloss, gloss-deduped options, so no two options can mean the same). The app
  rotates Meaning -> Reading -> MeaningToKanji and falls back to another mode
  when a card cannot be asked one way. `preview.py` gained `--mode kanji`.

## 2026-10-17 – No wall clock on the StickS3; day rule tightened

- The M5PM1 (I2C 0x6E) has an RTC domain with 32 B of retained RAM and a "timer
  counter" (0x38..0x3B), but after arming it the read-back is the programmed
  value (2 000 000 s) on every boot, including across a power-off: it is a reload
  register, not a free-running clock. So the StickS3 has no accessible wall
  clock and the session-day heuristic stays; real time needs Wi-Fi/NTP (phase 4)
  or a user-set date. The prototype was removed.
- The boot rule was too loose anyway: `beginSession()` advanced the day whenever
  `answered_total > 0` (lifetime), so *every* reboot after the first answer
  advanced a day. It now advances only when the *previous session* had an
  answer, tracked by a persisted `flags` bit that is set on every answer and
  cleared on boot (a reboot with no answers no longer moves the day).

## 2026-10-17 – Options must not duplicate the target's readings/meanings

- A reading option that is *another* reading of the prompt kanji (日 = ニチ/ジツ;
  a distractor showing ジツ) is a second correct answer. `Quiz.cpp` now rejects
  any candidate whose asked-kind reading is one of the target's readings of that
  kind, and any candidate whose gloss is one of the target's meanings.
- The builder excludes the same candidates from the precomputed reading list, so
  the pack carries no dead entries. The runtime check is the safety net (it also
  covers the random-walk fallback); packs built earlier still play correctly, so
  rebuilding them is optional.

## 2026-10-17 – Phase 2 SRS design (see docs/srs.md)

- **FSRS-5, fixed default weights, three grades** (Again on a miss, Hard on a
  slow correct, Good on a fast correct), day granularity. At 90% retention the
  constants cancel and the interval is `round(S)`, clamped to [1, 365] days,
  with ±5% fuzz. No on-device optimisation.
- **State is keyed by codepoint and shared across packs** (knowing 日 does not
  reset when the deck changes). A 16-byte record; the file is `/state/srs.bin`
  with a CRC-32 and a temp+rename atomic write, flushed every 8 answers or 120 s.
- **No wall clock**: a persisted monotonic *day index*, +1 per boot that follows
  a session with answers (a `flags` bit) and +1 per 24 h of uptime. Only
  differences matter; intervals therefore mean "sessions", not wall-clock days
  (documented trade-off; the M5PM1 has no readable clock).
- Pure logic in `firmware/src/srs/` (no Arduino, native-tested); LittleFS I/O in
  `firmware/src/platform/`; wiring in `main.cpp`.
