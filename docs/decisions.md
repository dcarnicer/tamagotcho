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
  register, not a free-running counter. **The StickS3 has no RTC chip at all**
  (the official pinmap lists only the BMI270 and the M5PM1 on the internal I2C,
  and M5Unified's RTC probe reports absent), so there is no accessible wall
  clock. Real time would need Wi-Fi/NTP (phase 4) or a user-set date; the
  session-day heuristic stays and the prototype was removed.
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

## 2026-10-17 – Settings menu hierarchy

- **The menu is now settings, not a deck list.** `Screen::Menu` is a five-row
  settings home (Seguir / Mazos / Fecha / Nuevas/día / Velocidad), still reached
  by holding the side button from the quiz. The deck list moved to its own
  `Screen::Decks`, and the date row to the "Fecha" setting, so new settings no
  longer pile rows onto the deck list.
- **One shared `Screen::Choice`** draws a title and four value rows; the open
  picker and its candidate values are held in `gChoice`/`gChoiceValues[]`. It
  serves both the daily new-card cap (3/5/10/20) and the Hard threshold
  (1.5/3/5 s or "no" = 0xFFFF to disable Hard). Holding a row applies the value,
  saves the state file and rebuilds the queue when the new-card cap changed.
- **Rows use the built-in 2x font** (`drawCentred` with `kTextBuiltin`) so the
  accented labels render; deck rows stay on the 1x `Font0` and only the loaded
  pack shows `due/new` counts (counting every pack would mean opening each one).

## 2026-10-17 – On-device localisation follows the pack language

- Every on-device string lives in a `UiStrings` table reached through `ui()`;
  `loadPack` calls `setUiForLang(gPack.header().lang)`, so "es" selects Spanish
  and any other code (or no pack yet) falls back to English. The error screen
  runs before a pack opens, so it always draws the English defaults.
- The pack's `lang` is the only signal for now; letting the user override the UI
  language (independently of the meaning language) is still open (roadmap).
- Header hints and the date rows draw through the built-in GFX font
  (`drawCentred` with `kTextBuiltinSmall`/`kTextBuiltin`) so accented Spanish
  ("atrás", "día") renders. The Font0 `smallText` path stays ASCII-only, so the
  done screen uses plain "dia".

## 2026-10-17 – On-device statistics screen

- `SrsStore::stats()` sums the state table once into `Stats`
  (`seen`/`learned`/`answers`/`reps`/`lapses`, pure and native-tested). A card is
  **learned** once its stability reaches 21 days (`stabilityCds / 100 >=
  kLearnedStabilityDays`), i.e. a ~3-week interval; the threshold is inclusive
  and in whole days.
- The settings home gained a sixth row ("Statistics"/"Estadisticas") opening
  `Screen::Stats`: the internal date (reusing `formatDate`), seen/pack total,
  the loaded pack's due/new (`packDueNew`), lifetime answers, accuracy
  `(reps - lapses) / reps` as a percentage (guarded for a zero divisor) and the
  learned count. All labels come from the `UiStrings` en/es table; the side
  button returns to the settings home, as on the other settings subscreens.
- Accuracy is computed in the UI from `Stats`, not stored: the header already
  keeps `reps`/`lapses` per card and `answered_total`, so no format change was
  needed (v1-compatible).

## 2026-10-17 – Landscape orientation option

- The panel rotation is a user setting ("Orientation"/"Orientación", appended to
  the settings home) persisted in NVS (`Preferences` namespace `tangocho`, key
  `land`, default false = portrait). **Portrait is untouched by default**: the
  landscape branches are guarded by `W > H`, so a bug in landscape cannot change
  the portrait rendering or the boot path.
- The HAL gained `setOrientation(bool)`/`landscape()`; `width()`/`height()` read
  the display and therefore follow the rotation. `setup()` applies the saved
  value right after `board::begin()` (which sets portrait) and before the canvas,
  so the back buffer matches the panel from the first frame.
- `applyOrientation()` (called when the setting is confirmed) rotates, picks up
  the new `W`/`H`, deletes and recreates `gCanvas` (PSRAM, internal-RAM
  fallback), persists the choice and redraws.
- Landscape quiz (240x135) is two columns: the prompt (kanji, or the meaning for
  meaning→kanji) is centred in the left column `max(largePx, W*2/5)` wide, and
  the existing option layout (rows / 2×2) is bounded to the right column. Lists
  (settings home, deck list, date, choice, stats) compute their row step from the
  available height and drop to the built-in 1x font when 2x no longer fits;
  portrait keeps its original steps and 2x font exactly.

## 2026-10-17 – Mistake detail card

- After a **wrong** answer the reveal is no longer "tap → next": a tap (or hold)
  now opens a new `Screen::Detail` card and the **next** input starts the
  following question. A correct answer keeps the old one-tap flow. The last
  answer's correctness lives in `gLastCorrect`; `afterReveal()` decides.
- The card shows the kanji (set 0, large), all meanings, all on/kun readings (the
  okurigana `.` markers stripped by `joinReadings`), the stroke count and the
  card's own accuracy `(reps - lapses)/reps` from the SRS record when it has one
  ("correct 3/5"). No new storage: everything is already in the card + record.
- Portrait: kanji centred in the upper ~45% (the quiz's `min(large_px + 6, 45%)`
  box), details below. Landscape: two columns mirroring the quiz (kanji left,
  details right). Latin values use the built-in font when it covers them (2x,
  shrunk to 1x), else the pack glyphs; readings use **set 2**, falling back to
  set 3 only when set 2 overflows and set 3 fits (kana are short and set 2 keeps
  the card compact). The meanings block takes the height left after reserving a
  label + one line per optional reading and one line each for strokes/accuracy;
  it ellipsises rather than clipping, as elsewhere in `main.cpp`.
- Input: tap/hold/side tap → next question; side hold → the settings home.
  `TGC_QUIZ_LOG` gains a `D U+xxxx` line when the card is shown.

## 2026-10-17 – Streak and review time in the SRS header

- The day streak and the lifetime review time reuse the header's reserved bytes
  32..47 (`streak_current`, `streak_best`, `last_study_day`, `review_ms`); see
  docs/srs.md §4.1. **No version bump**: the bytes were documented reserved, are
  not validated, and `0` means "absent", so an old file loads as zeros and an old
  reader still validates the CRC over the whole (unchanged, 48-byte) header.
- The streak is advanced inside `grade()` on the first answer of a new SRS day
  (exactly the next day → +1, any other day → 1); same-day answers never change
  it; both counters saturate at `0xFFFF`. Known quirk: `last_study_day == 0`
  doubles as the "never studied" sentinel, so the very first answer on day index
  `0` (the fresh-store day) does not start a streak; it disappears after the
  first day change. Documented rather than special-cased to keep the header
  field's "0 = absent" meaning.
- `addReviewMs()` is called from `holdAction()` with the same `rt_ms` that grades
  the answer, so there is no new timing source. `Stats` grows `streakCurrent`/
  `streakBest`/`reviewMs` and `Screen::Stats` gains "streak C/B" and "time Xh Ym"
  lines (localised; the duration picks h/m, m/s or s).
- Per-kanji accuracy stays derivable from the record (`(reps - lapses) / reps`)
  and is surfaced by the detail card; no per-card storage was added.

## 2026-10-17 – On-device UI language picker

- The UI language used to be purely pack-driven (`setUiForLang` on the pack's
  `lang`). A new NVS preference (`tangocho`/`ui`, int) adds **Auto (0, the
  default)**, English (1) and Español (2). The settings home gains a
  "Language"/"Idioma" row that opens the shared `Screen::Choice` with the three
  endonyms (Auto / English / Español); applying persists the value and re-renders
  immediately. Auto is byte-for-byte today's behaviour ("es" → Spanish, anything
  else → English), so nothing changes unless the user chooses.
- The decision is a tiny pure helper, `ui/UiLang.h`'s
  `uiLangCodeFor(pref, packLang)` (header-only, native-tested through `-Isrc`).
  English/Spanish force the table regardless of the pack, so an explicit choice
  survives a pack in another language; an unknown preference behaves like Auto.
- The settings home now has **8 rows**; `drawMenu`'s existing step/`labelFont`
  fallback already shrinks the rows to fit both orientations (portrait keeps the
  2x font for short labels and drops the long ones to 1x, landscape uses 1x).

## 2026-10-17 – Meaning distractors prefer a shared classical radical

- **KANJIDIC2 classical radicals are parsed** into `RawKanji`/`Card`
  (`radicals: list[int]`, the `rad_value rad_type="classical"` numbers, 1..214).
  Nelson reclassifications are ignored; names/Unicode are not imported. A
  missing/empty `<radical>` block yields an empty list. Radicals are a
  **builder-side ranking signal only** — the pack format and `docs/pack-format.md`
  are unchanged (v1, no field, no version bump).
- **Scoring key for the meaning list** (best first): same JLPT level, then
  shared classical radical, then visual (ink-vector cosine) similarity, then
  closest stroke count, then the seeded random tie-break and card index. Level
  stays the *first* key so a radical never lifts a candidate above one at the
  target's own level; within a level a shared radical outranks raw ink
  similarity. **Why radical before similarity:** a shared radical is a
  deliberate, explainable confusion axis (same dictionary family — 氵, 木, 口…)
  and is less noise-prone than the cosine of two glyph bitmaps; the point of the
  feature is exactly to surface those siblings when the ink score alone would
  miss them. Same-level membership is what counts as "otherwise plausible".
- **No-radical data degrades exactly to the old ordering:** a pair with no
  shared radical makes the new key term constant, so hand-written fixtures and
  the mini fixture are byte-identical (the committed `fixtures/mini.tgc` needs no
  regeneration; its three kanji have distinct radicals anyway).
- **Existing hard rules and the contract are unchanged:** gloss-sharing
  candidates and `j == i` are still excluded, the result is still deterministic
  for a seed, and `MAX_DISTRACTORS` / best-first still hold. Packs built before
  this change **still play correctly and need no rebuild**; only future builds
  compute different (better) distractor lists.

## 2026-10-17 – Stroke order from KanjiVG

- **Format: a new optional section, not a version bump.** The header's old
  `reserved` u32 at offset 60 becomes `stroke_off`; `0` means "no stroke data",
  which is every pack built before this change and every pack built without
  `--strokes`. Old readers ignore offset 60, so the extension is v1-compatible
  (the same pattern as the reading-distractor and set-3 extensions). The section
  carries a per-card index, per-card blocks and 1bpp masks; the full layout is
  in docs/pack-format.md.
- **Per-stroke masks, not pre-rendered frames.** Storing one 1bpp mask per
  stroke and compositing on device costs ~0.4-0.6 KB per kanji at 96 px
  (`--strokes` adds ~50 KB to the 79-card N5 pack) and lets the device choose
  colour, pacing and orientation. Pre-rendering N frames per kanji
  (or one 4bpp image per frame) would multiply the size and freeze the look;
  the strip is also only loaded on demand, like glyph bitmaps.
- **1bpp, own ink only.** A mask is one stroke's ink (not the earlier strokes
  baked in), so the device composites `0..i-1` in full ink and `i` highlighted.
  Full coverage is the right model for a hand-drawn stroke mask; 4bpp would only
  add antialiasing bytes the 135 px panel cannot show.
- **The em square is the reference, not the glyph's ink box.** Masks are placed
  in the same `large_px` square `Renderer::drawKanji` uses for set 0
  (`(boxX + (boxW - px)/2, boxY + (boxH - px)/2)`), so stroke data is
  independent of how the font cropped a particular kanji and of the font in use.
  `blocks.px` is required to equal `large_px`; the reader rejects a mismatch.
- **KanjiVG paths are centre lines, so they are stroked, not filled.** The task
  sketch said "polygon"; KanjiVG actually ships open centre-line paths
  (`stroke-width:3` in a `109x109` viewBox), and filling a straight stroke as a
  polygon would draw nothing. The builder flattens M/L/H/V/C/S/Q/T/Z into
  polylines and strokes them with `ImageDraw.line(width ≈ 3*px/109, round caps)`,
  then crops each stroke to its bounding box. Unsupported commands (arcs) skip
  that path with a warning.
- **`--strokes` is opt-in.** Default packs stay small and need no KanjiVG
  download; with the flag omitted the emitted bytes are identical to the v1
  writer's for the same inputs (asserted by a test against a committed baseline).
- **Animation** runs only on the mistake detail card, only when the loaded pack
  has stroke data for the shown card: one stroke every ~550 ms with the current
  one in yellow, a ~1.2 s hold on the last, then loop; the frame is redrawn only
  when it changes. With no stroke data the card is byte-for-byte today's card
  (no input-handling change).

