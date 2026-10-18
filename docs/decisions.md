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
- The settings home now has **9 rows** (the Wi-Fi row was added later);
  `drawMenu`'s existing step/`labelFont` fallback already shrinks the rows to fit
  both orientations (portrait keeps the 2x font for short labels and drops the
  long ones to 1x, landscape uses 1x).

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
  stroke and compositing on device costs ~1.1-1.2 KB per kanji at 96 px
  (`--strokes` adds ~90-95 KB to the 79-card N5 pack; a mask's pixel count, and
  so its cost, scales with the em square's area -- see docs/pack-format.md
  "Limits") and lets the device choose
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

## 2026-10-17 – On-device Wi-Fi portal (phase 4)

- **A small `platform/WifiPortal` class owns the AP and an `esp32 WebServer`**
  (`begin`/`poll`/`end`), started from the new settings-home row "Wi-Fi" (or at
  boot when there is no pack) and stopped on exit. It is decoupled from
  `main.cpp` by a two-callback `Hooks` struct: one fills a `PortalStats` value
  (SRS counters + loaded pack card count) and one is called after `/packs`
  changes so the app rescans the folder and reloads the live pack (the
  boot-only upload-ready handling is described below).
- **The access point is open (no password), SSID `Tangocho-XXXX`** from the AP
  MAC's last two bytes, IP 192.168.4.1. Deliberate trade-off: joining is
  trivial, but anyone in radio range can list, upload or delete packs and read
  the statistics. The portal is meant to be started for a moment and stopped;
  this is stated on the page, in README and in the header.
- **One self-contained HTML page, no external assets or JS**, at `/`, with the
  pack list (size + delete), an upload form (`multipart/form-data`,
  `accept=".tgc"`), a URL-download form, the SRS statistics and the free space.
  `/api/stats` returns the same counters and the pack list as JSON. No auth.
- **Uploads stream to `/packs/.upload.tmp`** with `server.upload()` and are
  renamed over the destination only after they validate (see the later "Uploads
  are atomic" entry); the body never has to fit in RAM. The filename is reduced
  to a basename and restricted to `[A-Za-z0-9._-]`, must end `.tgc`, and is
  length-capped, so a write can never escape `/packs`. A whole-file size cap, a
  running cap and a **96 KB reserve** (against the ~5.9 MB StickS3 filesystem)
  reject an upload that would fill the disk. Before the rename the first 4 bytes
  must be `"TNGC"` and a cheap `PackReader` probe (PSRAM-backed) must open it;
  otherwise only the temp is deleted, so a bad upload cannot strand the device.
- **URL download is staged through `poll()`** so the on-device screen paints
  "downloading..." before the (blocking) fetch starts; downloads stream via
  `HTTPClient::getStream()` to `/packs/.fetch.tmp` and are only renamed over the
  target after the same magic/probe validation, so a failed download leaves the
  existing packs untouched. **Plain `http://` only**: TLS to arbitrary hosts is
  untested and may fail on flash/memory, so `https://` is refused rather than
  silently attempted. No resume yet.
- **Trace channel:** the portal always logs over serial (`[wifi] ...`) because
  upload/fetch results matter even when `TGC_QUIZ_LOG` is off; the app also logs
  portal start/stop and the SSID/URL behind `TGC_QUIZ_LOG`.

## 2026-10-17 – Empty filesystem boots into the Wi-Fi upload screen

- A device whose filesystem has no pack no longer shows "No pack found" and
  halts; it starts the portal and shows the Wi-Fi screen with instructions
  ("upload-ready"). `loop()` polls the portal and, as soon as an upload (or URL
  download) has loaded a pack, rebuilds the queue and goes to the quiz. This is
  the practical answer to a fresh device and to a launcher-formatted filesystem,
  where `uploadfs` is not available. `LittleFS.begin()` failing is still fatal
  (the portal cannot help without a filesystem); so is an AP that will not
  start, which keeps the old error screen.
- In upload-ready mode the Wi-Fi screen is the **only** screen: the side button
  is ignored, because there is no quiz to return to. This is the *boot-time*
  state only: a portal opened later from Settings and then emptied stays
  leavable (see the later "upload-ready is a boot-only flag" entry), and
  uploading a new pack on the boot screen continues into the quiz.

## 2026-10-17 – M5Launcher and the app-only `.bin` (documented, not verified)

- `pio run -e sticks3` produces `.pio/build/sticks3/firmware.bin`, an
  **app-only** image (ESP image magic `0xE9`, no bootloader, no partition
  table), as before; USB flashing with `-t upload`/`-t uploadfs` is unchanged.
- Per the M5Launcher wiki, Launcher installs a normal app-only `.bin` into an
  OTA app partition and manages **its own** partition table, attaching a
  SPIFFS/LittleFS data partition (its docs mention a 448 KB minimum for an empty
  data partition). Our `partitions/sticks3_8MB.csv` (2 MB app slot, `spiffs` at
  `0x210000`) is not the table the launcher uses.
- **Conclusion, stated honestly:** installing the app under M5Launcher is
  plausible — our ~1.2 MB build should fit a launcher-created app partition —
  but **we have not verified it on hardware**, and we cannot claim that the
  launcher's filesystem partition is the one our `uploadfs` layout writes or
  that it is large enough for multi-MB packs. Open question to settle on a real
  StickS3. Until then, the **Wi-Fi upload is the reliable way** to populate
  `/packs` once the firmware is running, because it does not depend on who owns
  the filesystem partition. Written up in README.md.

## 2026-10-17 – CI scope

- **Two jobs, on push to `main`, PRs and manual dispatch.** Firmware: install
  PlatformIO on Ubuntu, run `pio test -e native` and build `sticks3` and
  `tdisplay-s3`. Pack builder: `uv run pytest`. The pure logic (`pack/`, `quiz/`,
  `srs/`) is native-tested, so the host job is meaningful without a device; the
  board builds prove both HALs still compile. A `concurrency` group cancels a
  superseded run of the same ref.
- **`~/.platformio` is cached, keyed on the PlatformIO version and a hash of
  `firmware/platformio.ini`.** The version is read after `pip install` (a cache
  restore key must be known first), and the ini hash covers the pinned platform
  (`espressif32@6.12.0`) and the exact `lib_deps` commits, so a dependency bump
  invalidates the cache. Action majors are pinned (`checkout@v4`,
  `setup-python@v5`, `cache@v4`, `setup-uv@v5`); no secrets are used.
- **CI does not build packs.** KANJIDIC2/Klee One/KanjiVG downloads are slow and
  produce multi-MB CC BY-SA artifacts, so prebuilt packs belong in the release
  workflow and only when the maintainer asks (below). CI also does not touch
  hardware: everything device-specific stays "verified by hand, documented" as
  before.

## 2026-10-17 – Release artifacts: merged image and ESP Web Tools (unverified)

- **Two images per board.** `pio run -e <env>` still yields the app-only
  `firmware.bin` (published as `tangocho-<env>.bin`; what M5Launcher installs and
  what `pio run -t upload` writes to the app slot). A **merged single-file
  image** (`tangocho-<env>-merged.bin`, flashed at offset 0) is added for ESP Web
  Tools, which wants one file rather than an offset list.
- **The merged image mirrors what PlatformIO flashes, and no `boot_app0` is
  included.** `tools/release/merge_image.py` reads the offsets from
  `firmware/partitions/*.csv` and merges bootloader `0x0`, partition table
  `0x8000` and the table's `app` partition. `boot_app0.bin` (the Arduino "which
  OTA slot" record at `0xe000`) is included **only if the table has a `data,ota`
  partition**; ours have none (single factory app, no OTA), so it is omitted —
  which is exactly what PlatformIO does for the same table, and avoids writing
  `boot_app0` over our `phy_init` partition at `0xe000`. Writing it anyway would
  be harmless on the current board (phy_init is unused) but wrong.
- **A merged image flashes an empty filesystem**, so the natural first-boot path
  is the existing Wi-Fi upload screen rather than `uploadfs`.
- **Least privilege.** The workflow defaults to `contents: read`; only the
  publish job sets `contents: write`, and it is the only job with the token.
  Triggers are tag push `v*` and `workflow_dispatch` (with an explicit tag
  input, validated to start with `v`).
- **Unverified on hardware.** The offsets match `pio run -t upload` and were
  checked by running the merge locally against the real partition tables, but no
  one has flashed a merged image from a browser (ESP Web Tools) or with
  `esptool write_flash` on a real StickS3. Stated as such in README and in the
  release notes.
- **Prebuilt packs are opt-in.** A dispatch input `build_packs` (with
  `packs_jlpt`/`packs_langs`) builds N5 (and, on request, deeper levels) for en
  and es with `--strokes`, writes the catalog, and attaches them. Off by default
  because redistributing CC BY-SA data is a maintainer decision ("Before
  publishing"); when packs are attached, the release notes carry the combined
  KANJIDIC2/KanjiVG/Klee One attribution automatically.

## 2026-10-17 – Device catalog format: line-oriented text

- **The catalog is TAB-separated text, not JSON.** The device has no JSON
  library and this file is small: one `#` comment line with the format version,
  one documented `#` column line, then one record per line
  `name<TAB>lang<TAB>cards<TAB>bytes<TAB>sha256<TAB>url`. A firmware parser is
  then a line split plus `strtoul`/hex, with no allocation beyond the line
  buffer. It is also greppable and diffable for humans. An optional
  `catalog.json` is written for release pages but is never read on device.
- **`sha256` per file** so the device can verify integrity after download (the
  check itself is a follow-up). `inspect` now prints the same digest.
- **Deterministic**: records sorted by file name; `--base-url` makes URLs
  absolute (with an optional `{file}` template) and defaults to the bare file
  name, meaning "relative to the catalog's own URL". `name` is the file name
  without `.tgc`, i.e. the name the device saves the download as.
- **`catalog` refuses an empty directory or an invalid `.tgc`** (exit 1) so a
  broken build cannot silently enter a release.
- **The device-side fetch/parse is not implemented** (roadmap phase 5,
  follow-up); the format is the contract to implement against and is kept
  minimal for that reason.

## 2026-10-17 – Optional WaniKani importer (phase 4)

- **An importer, not a content source.** `tools/pack-builder/src/tangocho_pack/
  wanikani.py` builds cards from the kanji a user has **unlocked** on WaniKani,
  using the user's own API v2 token. No WaniKani content is shipped and nothing
  is fetched unless the user explicitly passes `--wanikani-token` (or sets
  `WANIKANI_TOKEN`) to `build`. The importer then hands the cards to the
  unchanged `build_pack`, so glyphs, distractors (including the radical ranking)
  and `--strokes` all apply as usual. Reserve from the start: WaniKani is a
  trademark of Tofugu LLC; the content is theirs; packs built this way are the
  user's own responsibility and are not redistributed (docs/data-sources.md).
- **Unlocked only.** `/assignments` says what the user can study: an assignment
  must have `unlocked_at`, not be `hidden`, and have `subject_type == "kanji"`.
  The request asks the API for exactly that (`subject_types=kanji&unlocked=true
  &hidden=false`, plus an optional `levels=` filter) **and** re-checks the flags
  locally, so a locked/hidden kanji in the data can never slip through — this is
  what the canned fixture pins. Subjects are fetched with `types=kanji` and
  joined by subject id; nothing is written back to WaniKani.
- **Token handling.** The token is read from the flag or the environment and
  used only in the `Authorization: Bearer` header; it is never logged, printed
  in `--help`/output, cached to disk or committed. HTTP 401/403/429 become
  clear, token-free `WaniKaniError` messages, and requests are spaced to stay
  under the documented 60-requests/minute limit. The network layer is a
  `transport(url, token)` callable so tests inject canned pages and never touch
  the network.
- **English-only meanings.** WaniKani ships English meanings only; there is no
  honest translation, so a non-English `--lang` **falls back to English and sets
  the card's fallback flag** rather than refusing the build. This matches the
  KANJIDIC2 importer's convention (flag bit0), so the device and the detail card
  behave identically for both sources. Chosen over "refuse" because it keeps the
  common `--lang es` pipeline working and the fallback is already surfaced.
- **Readings.** The API returns all readings in hiragana; on'yomi are shifted to
  **katakana** so the pack contract (`onyomi` katakana, `kunyomi` hiragana) holds
  for both importers and the reading distractors compare like with like. Readings
  are primary-first then accepted-first, deduped; `nanori` are dropped. Meaning
  and reading lists are capped/ordered the same way as KANJIDIC2 (meanings to
  `MAX_MEANINGS`).
- **Unknown metadata is 0.** `jlpt`/`grade`/`strokes`/`freq` have no WaniKani
  equivalent, so they stay 0; the ordering hint is WaniKani level then subject
  id, which the builder assigns as the deck-local `level`.

## 2026-10-17 – Uploads are atomic: never destroy the pack they replace

- The upload handler used to open (and delete) the final `/packs/<name>` at
  `UPLOAD_FILE_START` and delete it again in `abortUpload()`, so a dropped
  connection, a size cap or a 0-byte body destroyed a working pack and left the
  loaded pack a dangling file handle. It now streams into `/packs/.upload.tmp`
  (hidden, so the listers skip it) and removes the destination and renames the
  temp over it **only after** the whole body arrived, passed the size caps and the
  magic/`PackReader` probe -- exactly the URL-download scheme. Any abort removes
  only the temp.
- The dead early checks (`up.totalSize` is always 0 in this WebServer core) are
  replaced by the request's declared length via `WebServer::clientContentLength()`
  (0 = unknown/chunked -> the write-time cap decides), with a small multipart
  envelope slack so a pack that only just fits is not refused. "Pack too large"
  and "Not enough space" stay reachable at the early and the write-time checks.
- `onPacksChanged()` now fires whenever `/packs` may have changed: a successful
  upload, a successful fetch, and a fetch whose rename failed after the old
  destination was already removed (delete already fired it unconditionally).

## 2026-10-17 – The upload-ready state is a boot-only flag

- `gWifiUploadReady` doubled as "booted with no pack" and "the folder is empty
  now", so deleting every pack from a portal opened via Settings stranded the user
  on the Wi-Fi screen (the side button was inert). Renamed to `gWifiBootUpload` and
  set **only** at boot (cleared when the app advances to the quiz); opening the
  portal from Settings no longer touches it, and `portalPacksChanged()` no longer
  sets it. The side button is inert only in the genuine boot-time state.

## 2026-10-17 – Streak "has studied" flag (header `flags` bit1)

- `noteStudyDay()` used `last_study_day == 0` both as "never" and as the valid day
  index 0, so no answer on day 0 started a streak. First-vs-consecutive now uses a
  persisted "has studied" bit (header `flags` bit1, `kSrsFlagHasStudied`); day 0
  starts a streak of 1 like any other first answer. A file written before the bit
  recovers it from a non-zero `last_study_day` on load, so no v1 file resets a
  running streak. Header bytes only, so still v1-compatible.

## 2026-10-17 – Review follow-ups: stroke limits and small fixes

- `--large-px > 128` with `--strokes` used to truncate the i8 stroke offsets;
  `build_pack` now refuses it before rendering, and `write_pack` rejects both
  `large_px > 128` and `strokes_px != large_px` so the public writer cannot emit a
  pack the reader rejects (docs/pack-format.md "Limits").
- The stroke-section size estimate was much too optimistic: masks scale with the
  em square's area, so the committed 32 px fixture (~130 B/card) implies
  ~1.1-1.2 KB/card at 96 px (~90-95 KB for the 79 N5 cards), not 0.4-0.6 KB.
- `parse_kanjidic` strips (and treats whitespace-only as absent) numeric values
  instead of letting `int()` raise on them; `merge_image.py` reads one byte of the
  app image for the magic check instead of the whole file; the dead
  `PortalStats::packName` field/assignment is removed; the firmware README's
  tap-after-wrong-answer wording and the stale "8 rows" settings-home count are
  corrected.

## 2026-10-18 – Power management and the backlight buzz

- **The firmware never slept.** `loop()` ran every 10 ms at 240 MHz with the
  backlight at 50 %, forever, and the Wi-Fi portal had no timeout, so the
  battery drained in hours even when the device sat unused.
- **Idle policy (in `main.cpp`):** dim after 30 s without input; after 2 min
  (5 min while the portal is up, where any HTTP request counts as activity) save
  the SRS, stop the portal and, **on battery, deep-sleep** with the front button
  (Select) as the ext0 wake source. On **USB power** only the panel goes dark:
  deep sleep would drop the USB serial port and make flashing awkward, and there
  is no battery to save; unplugging is polled every 5 s and then sleeps. The
  press that wakes a dark panel (or the chip) is swallowed until released; a
  press on a merely dimmed panel brightens it and acts normally.
- **Deep sleep feeds the SRS clock.** Before sleeping the app stores the RTC
  timer (`esp_rtc_get_time_us()`) and a magic in `RTC_DATA_ATTR` memory; a wake
  adds the difference through the new pure `SrsStore::addElapsedMs()`. Only a
  real power-off now leads to the "how many days?" screen, because `RTC_NOINIT`
  survives deep sleep. A store that failed to load and has no answers is not
  written on idle, so a damaged `srs.bin`/`.bak` is not overwritten unasked.
- **CPU at 80 MHz** (240 MHz while the portal runs). Rendering is light and the
  SPI/LEDC clocks come from the 80 MHz APB either way.
- **Brightness setting** (settings row 10, NVS `tangocho`/`bri`, 12/25/50/100 %,
  default 50 %). The settings rows keep a 1 px gap on the short landscape screen
  so ten rows still fit the 1x font.
- **Backlight PWM moved from 256 Hz to 25 kHz.** M5GFX drives the StickS3
  backlight (GPIO38, LEDC ch 7, 9-bit) at 256 Hz; at partial duty the current
  pulses made an audible buzz on USB power (through the charger's inductor),
  which stopped when unplugged. `BoardSticks3::begin()` calls
  `ledcChangeFrequency(7, 25000, 9)` after `M5.begin()`. Diagnosis by reading the
  M5GFX source. **Hardware check (same day): the buzz is still audible with
  an ear close to the device on USB**, so the 256 Hz PWM was not (or not the
  only) cause. The change stays (25 kHz is harmless and removes an audible
  frequency); the buzz is an open item on the roadmap.
- **Unverified on hardware:** whether the M5PM1 keeps the rail up through a
  long ESP32 deep sleep (if it cut power, the next start would be a power-on and
  show the date screen: degraded but safe) and the deep-sleep current.

## 2026-10-18 – Review fixes: response time, meaning→kanji, pack swap, logs

- **Response time** is now question shown → hold press-down minus 400 ms per
  navigation tap, instead of restarting the clock on every tap, which ignored
  the thinking time before the first tap (docs/srs.md §3).
- **Meaning→kanji options** also reject a candidate whose *own* glosses contain
  the prompt: it would be a second correct kanji. The builder already excluded
  these from the precomputed list; the random-walk fallback did not.
- **`loadPack` opens the new reader next to the live one** and swaps it in only
  after it validated (`gPack` is now a `unique_ptr`), instead of closing the
  live pack first; the separate probe open is gone. A glyph-cache allocation
  failure leaves a consistent "no pack" state.
- **`TGC_QUIZ_LOG` is off in `sticks3`** (and so in releases); the new
  `sticks3-dev` env turns it on.
- The quiz header re-reads the battery every 30 s and redraws when it changed.
- `docs/srs.md` §5 described a boot rule the code no longer had; rewritten.

## 2026-10-18 – Power button sleeps, "Sleep" row, critical battery

- **Why.** On the StickS3 the M5PM1 owns the power button. A click "resets" the
  ESP32 and a double click powers it off, and both cut its rails (M5PM1
  datasheet v1.9, §V.3: reset = shutdown, 500 ms, power on), which clears the RTC
  timer and RTC memory and so leads to the "how many days?" screen. That was
  the main way the day clock got lost.
- **Registers** (I2C 0x6E, through M5Unified's `M5.Power.M5pm1`; the pinned
  M5PM1 library is not needed): `Device_ID` 0x00 = 0x50 and `Device_Model`
  0x01 = 0x20 identify the chip. Both registers arrived with PM1 firmware 6,
  the release that added `BTN_STATUS` (older firmware has a UID there), so
  they double as the version check. `BTN_CFG_1` 0x49 bit0 `SINGLE_RESET_DIS`
  (factory 0x2A: double-click gap 250 ms, long press 2 s, click 250 ms,
  `DL_LOCK` 0), `BTN_CFG_2` 0x4A bit0 `DOUBLE_POWEROFF_DIS`, `BTN_STATUS` 0x48
  bit7 "pressed since the last read" (the read clears it), bit0 "down now".
  Only bit0 of the config registers is changed (read-modify-write), and each
  write is read back.
- **What the firmware does.** At every boot `board::begin()` checks the
  board, the PMIC type and the ID/model. It enables the double-click
  power-off, then sets `SINGLE_RESET_DIS` and reads `BTN_STATUS` once to drop
  a stale press. `board::powerButtonClicked()` polls `BTN_STATUS` every 100 ms
  (a 1-byte read; M5Unified already reads `IRQ_STATUS3` on every
  `M5.update()`). The app treats a press like "sleep now": save the SRS, stop
  the portal, show "Sleeping / progress saved, the front button wakes me" for
  1.5 s, store the RTC timestamp as the idle path does, deep-sleep. On a dark
  panel (idle on USB) the press only turns the panel back on, like the other
  buttons. The error screen (`halt()`) sleeps on a press too.
- **The double-click power-off is kept on purpose: it is the escape hatch.**
  A long press is not a force-off. It restarts the ESP32 into **download
  mode** (rails cycled with BOOT held low; the green LED blinks every 500 ms).
  The register map lists `BTN_CFG_1`/`BTN_CFG_2` as untouched by reset,
  download mode and power-off. With both bits set, a long press (or a hung
  app) would leave a device that no button can get back into the app: a click
  no longer resets, a double click no longer powers off. Only a host reset
  over USB or a flat cell would help. So a double click stays a real power-off,
  which still forgets the day: a deliberate act, and the first press of it
  already starts the (atomic) SRS save. The boot re-enables it if another
  firmware turned it off, and it only disables the click reset once that
  worked.
- **Persistence.** The PM1 runs from the cell while the device is "off", so
  the setting survives ESP32 resets, deep sleep, download mode and PM1
  power-off. It returns to the factory value only on a PM1 cold start (cell
  disconnected or flat with no USB). It is re-applied at every boot anyway.
  Side effect: other firmware (another M5Launcher app, a reflash) inherits
  "click does nothing" until it clears the bit. A build with
  `-DTGC_PM1_CLICK_SLEEP=0` puts the factory button back at boot.
- **No power-button wake.** `IRQ_STATUS1..3` only latch while a PM1 GPIO is in
  IRQ mode, and M5Unified sets none on the StickS3. The StickS3 pin map shows
  PM1 G1 wired to ESP32 G13 (RTC-capable), so a power-button wake from deep
  sleep looks possible: PM1 G1 as the IRQ output, every IRQ but the click
  masked, ext0 on G13. It is **not done**. The datasheet says that with the
  click reset disabled and an IRQ GPIO, the PM1 LED blinks every 200 ms, and
  it is unclear whether that is continuous. Every unmasked charge, VBUS or IMU
  event would also wake the chip. Neither can be checked without hardware. The
  front button stays the only wake source; a power-button press during sleep
  does nothing and is dropped at the next boot.
- **"Sleep" settings row** (row 11, "Sleep"/"Suspender", not "Power
  off"/"Apagar"). It does the same as the button: a deep sleep that keeps the
  day clock, not a power-off. Both explicit paths deep-sleep **on USB power
  too**. They are an explicit request, unlike the idle timeout (which only
  blanks the panel on USB to keep the serial port). Charging carries on in the
  PM1. To flash, wake the device with the front button, or long-press for
  download mode. Eleven rows fit: portrait 135x240 steps 19 px (was 21),
  landscape 240x135 hits the 10 px floor and drops the 1 px gap so the
  highlight still clears the 8 px font, T-Display-S3 steps 27 px (2x font
  kept) and 13 px in landscape.
- **Critical battery.** M5Unified maps the PM1's cell voltage linearly,
  `(mV - 3300) / 8`, so 3 % is about 3.32 V and the figure is noisy under load.
  `power/BatteryGuard.h` is pure and native-tested. It takes a reading every
  10 s on every screen. Three readings at or below 3 %, with no reading of 6 %
  or more in between, trigger the sleep: save, then a localised "Battery low /
  Charge me" notice, then deep sleep with a flag in RTC memory. USB/charging is
  never critical. On a wake after such a sleep the app takes the best of three
  readings first. It carries on only at 6 % or more (hysteresis) or on USB.
  Otherwise it shows the notice again and goes back to sleep **without**
  touching the stored RTC timestamp, so the next real wake adds the whole
  span. There is no timer wake, so it cannot flap on its own. This buys time
  to reach a charger, no more: the PM1's own low-voltage cut (`BATT_LVP`,
  factory 0x40 = 2.5 V) still powers off a cell left flat, and that loses the
  day.
- **Fail-safe.** No StickS3/PM1, an I2C error or an unknown ID/model: nothing
  is written and `powerButtonClicked()` stays false (if an earlier boot had
  disabled the click reset and the chip now fails to answer, the click does
  nothing until it does; the double click still powers off). A failed write
  puts the factory bits back (best effort). The T-Display-S3 has no PM1:
  always false.
- **Unverified on hardware** (no device attached): that the PM1 honours
  `SINGLE_RESET_DIS` and still reports the press in `BTN_STATUS`; that a
  click still **powers on** a device that is off with the bit set (the
  datasheet treats "reset" and power-on as separate wake sources but does not
  say); that a double click still works from download mode; the sleep notice
  layout; the battery thresholds against a real discharge.
