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
- **Prebuilt packs are opt-in.** (Superseded 2026-10-18: packs come only from
  `decks.yml`, see "Decks release" below.) A dispatch input `build_packs` (with
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

## 2026-10-18 – Auto-rotation from the IMU

- **Setting.** Orientation gains **Auto** (first row, as in the language
  picker) on boards with an accelerometer; the T-Display-S3 keeps Portrait and
  Landscape only. It is stored as NVS `tangocho`/`orient` (0 portrait,
  1 landscape, 2 auto), replacing the bool `land`: the first boot reads `land`
  once, writes `orient` and removes `land`, so landscape users keep landscape.
  **New users still get portrait**: the axis mapping below is unconfirmed on
  hardware, and portrait is what the docs describe. The rotation Auto picks is
  never persisted. Auto on a board whose IMU did not start behaves as portrait.
- **Rotations.** Auto uses 0 (portrait) and **both landscape directions** (1
  and 3), so the text is upright whichever way the stick is turned, but **not
  2** (portrait upside down): with the front button under the thumb below the
  screen, upside-down poses are mostly transient (tipping the device toward
  you, lying down), and phones leave it out for the same reason. That sector
  keeps the current layout; the filter can allow it with one mask bit. The
  fixed Landscape choice stays rotation 1.
- **The buttons do not move, only the picture.** Every on-screen hint names
  the button ("side: back", "tap/hold", "side hold: menu") instead of pointing
  at it, and options are reached by cycling, not by position, so no hint
  becomes wrong in rotation 1 or 3. In rotation 1 the front button is to the
  right of the screen, in rotation 3 to the left.
- **Filter (pure `imu/OrientationFilter`, native tests in `test_imu`).** "Up"
  in the screen plane is θ = atan2(x, y), clockwise from the panel's top edge;
  rotation r is centred on r·90°. A different rotation needs the pose within
  30° of its centre (±15° hysteresis on each 45° boundary, so a turn of at
  least 60° from the current centre) held continuously for **500 ms**. Samples
  are ignored when the screen is within 25° of horizontal (face up or down) or
  when |a| is outside 1 ± 0.3 g (shaking, taps), and an ignored or dead-band
  sample, or a gap of more than 1 s between samples, restarts the settle time.
  At boot one sample picks the nearest allowed sector at once (no settle,
  nothing is on screen yet), falling back to portrait when flat or unclear.
- **App.** The accelerometer is polled every 100 ms, only while Auto is on and
  the panel is on (not while it is off on USB idle). A settled change goes
  through the same path as the setting (rotate, new canvas, redraw the active
  screen) but **only while no button is down**: a change that settles during a
  press waits for the release, so tap/hold timing and the highlighted option
  never change under the finger. Selecting Auto keeps the current picture (the
  hold that selected it is still down); the IMU takes over after the release.
- **A rotation is not user activity.** It does not brighten a dimmed panel or
  reset the idle timers, as on phones: a stick turning over in a bag or pocket
  must still dim and sleep. The picture does rotate while dimmed.
- **Axis mapping, to be confirmed on hardware.** M5Unified remaps the StickS3's
  BMI270 (`IMU_Class.inl`: "X=+Y, Y=-X, Z=+Z"). Applied to M5Stack's StickS3
  axis diagram (chip X toward the USB end, Y toward the screen's right edge, Z
  out of the screen) this gives X = screen right, Y = top (HAT) end, Z = out of
  the screen, with the front button below the screen: exactly the frame
  M5Stack documents for the StickC Plus2, whose MPU6886 M5Unified does not
  remap. LovyanGFX rotation r puts the picture's top at the panel's top, right,
  bottom or left edge (read from its software rotation in `Panel_HasBuffer`,
  which its MADCTL tables are meant to match), so the picture is upright when
  the +1 g reaction points r·90° clockwise from the top edge. Hence the HAL's
  panel frame is M5Unified's frame unchanged. Confidence is moderate (two
  diagrams and the source agree; nothing measured); the swap/sign constants
  live only in `BoardSticks3.cpp`, and the `sticks3-dev` log prints each
  rotation with its sample.
- **IMU power.** M5Unified starts the BMI270 with the accelerometer and
  gyroscope in normal mode and never suspends it: its BMI270 driver has no
  `sleep()` and `M5.Power` does not touch the IMU. So it kept running through
  the ESP32's deep sleep, if the M5PM1 keeps its rail up. `board::deepSleep()`
  now writes PWR_CTRL = 0 and PWR_CONF = adv_power_save (the chip's power-on
  reset state) first; `M5.begin()` soft-resets and re-initialises it on the
  next boot, exactly as after a power-on. Unmeasured: the current saved, and
  whether the rail stays up at all. The gyroscope also runs while awake for
  nothing; turning it off is a possible follow-up.

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

## 2026-10-18 – Real date from the phone, 04:00 rollover, stepped manual date

- **An anchor, not a clock.** The SRS keeps its monotonic day index and due days
  stay in index space; the real date is an **anchor**: header byte 38 (u16,
  unused and zero in every existing file) holds the days since 1970-01-01 of SRS
  day 0, `0` = never synced. Display = anchor + `now_day`, else the old fixed
  2026-10-17 base, so v1 files load unchanged with no version bump
  (docs/srs.md §4.1). The anchor only moves on a sync, so every date the device
  shows (last used, today, statistics) derives from one number. Bytes 44..47
  stay unused.
- **The phone's browser is the source.** Opening `http://192.168.4.1` posts
  `Date.now()` and the offset (`POST /time`, form-encoded: the portal has no
  JSON parser). The offset travels as **minutes east of UTC**
  (`-getTimezoneOffset()`), the ISO sign, everywhere in the code. The portal only
  parses; the pure `srs/WallClock` decides plausibility (2026-01-01 <= t < 2100,
  offset -12..+14 h). The new hook is `bool onTime(int64_t utcMs, int16_t
  tzOffsetMin)` -- a `bool` rather than `void` so the page can say whether the
  device accepted the clock.
- **`applyWallClock(int64_t utcMs, int tzOffsetMin)` in `main.cpp`** is the one
  entry point for a real time; the NTP item will call it too. It stores the
  offset in NVS (`tangocho`/`tz`, int16, minutes east, absent until the first
  sync) because NTP only yields UTC. It saves the SRS only when the anchor or
  the day changed, so reloading the page (every upload/delete redirects to it)
  does not rewrite the 40 KB state file. A real time applied before `setup()`
  decides on the power-cut question (`gWallClockSynced`) skips the question.
- **Rollover at 04:00 local**, as in Anki, so a late session still counts for
  the evening's day. A sync sets `ms_carry` to the time since the last local
  04:00 and the existing uptime/sleep rules do the rest, unchanged. The offset is
  a snapshot: DST or travel shifts the rollover until the next sync.
- **Never rewind the index.** Real day ahead -> `advanceDays` (the new-card
  counter resets as on any day change; the streak follows the index). Real day
  behind (an overshot manual answer, or the RC oscillator running fast through
  a long sleep) -> re-anchor the display, keep the index; rewinding would put
  past reviews in the future. Re-anchoring was chosen over "freeze the index
  until real time catches up" because the uptime/sleep rules cannot know when
  that happens after a power cut.
- **First sync of a never-synced store** anchors the current index as today: it
  cannot measure a power-off gap that happened before it. This can only lose one
  gap once per device, and a sync dismisses the boot-time question (the user
  should not have to answer a question the device can answer itself).
- **Manual fallback: a stepped date.** The bucket rows are gone: the screen
  shows the last-used date (the persisted day at boot) and a proposed today with
  its weekday. Tap +1 day, hold confirms, **side hold -1 day** (never below the
  current day), side tap cancels. Stepping back was chosen over wrapping after
  +N: a wrap either caps how long an absence can be entered or makes an overshoot
  cost a full cycle of taps. On the other sub-screens any side press is "back";
  here the side hold is the minus key, and the bottom hint line says so.
- **A manual advance restarts the carry** (`advanceDaysManual`): after a power
  cut the time of day is unknown, and carry 0 rolls over a full day after the
  answer -- late, never early. An early rollover would hand out a day the index
  can never give back; a late one is fixed by the next answer or sync. "Same
  day" (+0) leaves the carry alone.
- **A sync while the quiz is up.** Today the portal only runs on the Wi-Fi
  screen, so a sync there just rebuilds the queue when the day changed. For a
  sync on other screens (NTP later): an unanswered question gives way to the new
  queue, an answered one or the detail card continues into it, the done screen
  re-checks. As with a date set from the settings, the question left behind the
  menu is still shown on return and may come up once more.

## 2026-10-18 – The date question survives a sleep

- Found while integrating the power-button and day-detection branches: after a
  power cut the boot shows the date question, but the idle timeout (2 min on
  battery), a power-button click or the Sleep row could deep-sleep the device
  before it was answered, and the wake then went straight to the quiz -- the
  unknown gap was silently treated as zero days.
- Every deep sleep now goes through `armSleep()`, which stores the sleep
  timestamp (as before) and `gDatePending` in RTC memory when the boot-time
  question is on screen. A wake from deep sleep with the flag set asks again.
  The proposed date starts from the current index, which by then includes the
  slept time, so stepping to the real date does not count the sleep twice. A
  critical-battery re-sleep at boot leaves the flag untouched.

## 2026-10-18 – UX refresh: generated mockups, proposal first

- The UX refresh starts as a **proposal** (`docs/ux-refresh.md`), not code: an
  audit of every screen, design tokens, a type scale, components, a regrouped
  settings tree and a phased plan, all awaiting the maintainer's approval.
- **Mockups are generated, not drawn.** `tools/ux-mockups/render.py` runs in the
  pack builder's environment and reuses its `FontRenderer`, Font0 table and
  KanjiVG rasteriser. "Before" screens are line-by-line ports of the `draw*`
  functions in `main.cpp`, rendered in RGB565 with `blend565`, so the audit's
  numbers (row heights, fallbacks to the 6×8 font) come from the real layout
  code, not from screenshots. Output is deterministic; the PNGs (native + 3x +
  a contact sheet, ~0.6 MB) are committed so the proposal can be reviewed
  without a device. The script is not part of the builder and does not change
  its behaviour.

## 2026-10-18 – Home Wi-Fi: station mode on demand, credentials in NVS `net`

- **Entered on the portal page, stored in NVS namespace `net`** (`ssid`,
  `pass`, and `cat` for an optional catalog URL override), separate from the
  app's `tangocho` namespace. The page has a "Home Wi-Fi" form (SSID with
  suggestions from a scan, password, "forget") and a catalog URL field.
- **The password is write-only.** It is never logged, never put in a page or in
  `/api/stats`, and `HomeWifi` has no getter for it: the page shows only the
  saved SSID. `WiFi.persistent(false)` keeps the driver from writing its own copy
  into its NVS area. NVS is **not encrypted** (no flash encryption on this
  device), so anyone holding it with a USB cable can read the flash -- the usual
  trade-off for a stored Wi-Fi password. Because the portal's AP is open, anyone
  in range while it runs can *replace* (not read) the home network or the catalog
  URL; a pack fetched from a hostile catalog still has to pass the same checks as
  an upload (size, SHA-256 from that catalog, magic, `PackReader` probe), so this
  is the exposure the upload form already has.
- **On demand, never left on.** `HomeWifi::begin()` joins only when the Get decks
  screen opens; the join times out after 15 s with a reason from the driver's
  disconnect event (`NO_AP_FOUND` -> "network not found"; `AUTH_FAIL`, 4-way or
  handshake timeout, 802.1X, MIC -> "wrong password?"; else "could not
  connect"). Leaving the screen, an error, or the idle timeout calls
  `WiFi.disconnect(true)` + `WIFI_OFF`. The CPU runs at 240 MHz only while the
  radio is up (the portal's rule). Download progress counts as activity for the
  dim/sleep timers (as HTTP requests do for the portal); an idle list screen
  still sleeps after 2 min, and a USB-powered panel that went dark rejoins on the
  next press.
- **SSID suggestions:** the portal scans once when it starts, in station mode
  *before* the AP comes up (~1.5 s, 100 ms per channel), so no phone is
  connected yet and the AP never shares the radio with a scan. Up to 12 names,
  strongest first, go into a `<datalist>`.
- **NTP helper only.** `HomeWifi::fetchUtcMs()` runs SNTP (`configTime`, UTC,
  pool.ntp.org / time.google.com / time.cloudflare.com) until the first sync or
  a timeout, stops SNTP again and refuses times before 2026-01-01. It is **not
  wired into the SRS**: the wall-clock integration belongs with the separate
  `applyWallClock()` change (roadmap phase 2, "real date from NTP").

## 2026-10-18 – HTTPS for Get decks: embedded root CAs

- **`WiFiClientSecure` + `HTTPClient`, redirects followed**
  (`HTTPC_STRICT_FOLLOW_REDIRECTS`): `github.com/.../releases/latest/download/x`
  answers 302 to `github.com/.../releases/download/<tag>/x`, which answers 302 to
  `release-assets.githubusercontent.com` (signed, short-lived URL). Each hop is a
  new handshake verified against the same roots. **No `setInsecure()`**
  anywhere; plain `http://` still works (the portal's URL download, a LAN
  catalog).
- **Chains on 2026-10-18** (`openssl s_client -showcerts`): `github.com` ->
  Sectigo DV E36 -> Sectigo Public Server Authentication Root E46, served
  cross-signed by USERTrust ECC; `release-assets` / `objects` /
  `raw.githubusercontent.com` -> Let's Encrypt YR1 -> ISRG Root YR, served
  cross-signed by ISRG Root X1.
- **The embedded set** (`firmware/src/platform/RootCerts.h`, 11 roots, 14.6 KB
  of PEM, copied verbatim from Mozilla's root store via certifi 2026.07.22):

  | root | key | expires | why |
  |---|---|---|---|
  | ISRG Root X1 | RSA 4096 | 2035-06-04 | today's anchor for the release-asset hosts |
  | ISRG Root X2 | ECDSA P-384 | 2040-09-17 | Let's Encrypt ECDSA |
  | USERTrust ECC Certification Authority | ECDSA P-384 | 2038-01-18 | today's anchor for github.com |
  | USERTrust RSA Certification Authority | RSA 4096 | 2038-01-18 | Sectigo RSA |
  | Sectigo Public Server Authentication Root E46 | ECDSA P-384 | 2046-03-21 | github.com once the cross-sign goes |
  | Sectigo Public Server Authentication Root R46 | RSA 4096 | 2046-03-21 | Sectigo RSA, newer |
  | DigiCert Global Root G2 | RSA 2048 | 2038-01-15 | earlier GitHub asset hosts, Azure |
  | DigiCert Global Root G3 | ECDSA P-384 | 2038-01-15 | DigiCert ECC |
  | GTS Root R1 | RSA 4096 | 2036-06-22 | Google, many Cloudflare sites |
  | GTS Root R4 | ECDSA P-384 | 2036-06-22 | Cloudflare ECDSA |
  | Amazon Root CA 1 | RSA 2048 | 2038-01-17 | S3 / CloudFront |

  Checked with only these roots (`openssl s_client -CAfile roots.pem -no-CApath
  -no-CAstore -verify_return_error`): github.com, release-assets / objects /
  raw.githubusercontent.com, pages.github.com, www.cloudflare.com,
  s3.amazonaws.com, gitlab.com, codeberg.org and storage.googleapis.com all
  verify. A host under another CA needs its root added (below).
- **No clock, no date check.** The device has no RTC and this mbedTLS build has
  no `MBEDTLS_HAVE_TIME_DATE`, so certificate validity dates are not checked;
  signatures, chain and host name are. A pack's integrity is anchored in the
  catalog's SHA-256, and the catalog itself comes over this verified TLS.
- **How to update the roots:** (1) look at the chains with `openssl s_client
  -connect HOST:443 -servername HOST -showcerts`; (2) copy the root PEM blocks
  verbatim from Mozilla's store (e.g. certifi's `cacert.pem`); (3) record
  `openssl x509 -noout -subject -enddate -fingerprint -sha256` above each block
  in `RootCerts.h` and in this table; (4) re-run the `-CAfile` check above.
  Watch: Let's Encrypt's self-signed ISRG Root YR/YE (not in Mozilla's store yet)
  once the X1 cross-sign stops; the USERTrust and DigiCert G2/G3 roots expire in
  January 2038.
- **Memory.** This mbedTLS build allocates from internal RAM only
  (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`): 16 KB in + 4 KB out record buffers plus
  handshake state, and ~16 KB while the 11 roots are parsed (freed when the
  handshake ends) -- roughly 40-60 KB of internal heap per connection, one at a
  time. Static RAM is ~51 KB of 320 KB and the canvas and pack tables live in
  PSRAM, so this fits; `[net] before/after TLS` lines log the free, largest and
  minimum internal heap and the free PSRAM so it can be confirmed on hardware.
  The handshake runs on the Arduino loop task, whose stack goes from 8 to 16 KB
  (`SET_LOOP_TASK_STACK_SIZE` in `NetFetch.cpp`, +8 KB internal RAM).
- **HTTP/1.0 for Get decks.** The body is read raw from the stream, so a chunked
  answer would corrupt it; HTTP/1.0 rules chunking out. GitHub's whole redirect
  chain answers HTTP/1.0 requests with a `Content-Length` (checked with
  `curl --http1.0`). The portal's request is unchanged (HTTP/1.1).
- **Flash:** the StickS3 build grows from 1,248,709 to 1,292,181 bytes (+43 KB,
  61.6 % of the 2 MB app slot), TLS included.

## 2026-10-18 – Get decks: the catalog, the download and where it lives

- **The last row of the Decks screen** ("+ Get decks" / "+ Descargar"), not a
  settings-home row: the home has 10 rows (another is on the way) and this is a
  deck action. A fresh device on the boot-time upload screen also offers it (a
  hold) once a home Wi-Fi is saved: the portal stops, Get decks runs, and the
  side button returns to the upload screen -- or into the quiz if a deck is now
  loaded.
- **Default catalog URL** (superseded the same day by `decks-v2`, see "Decks
  release" below)
  `https://github.com/dcarnicer/tangocho/releases/latest/download/catalog.txt`.
  It works only once the newest (non-pre-)release of a **public** repo carries
  `catalog.txt` and the packs, which the release workflow attaches only on
  request ("Before publishing"). A release without them makes "latest" answer
  404, which the device reports as "no catalog published at this URL yet". The
  portal's catalog field overrides it (e.g. a tag-pinned
  `releases/download/vX.Y.Z/catalog.txt`, or any http(s) host).
- **Catalog rules** (pure `catalog/` module, native-tested against the builder's
  output): no `#tangocho-catalog<TAB>N` first line -> not a catalog (an HTML
  error page, say); an unknown version -> refused ("update the firmware");
  otherwise each record is validated on its own and a bad one is skipped and
  counted, so one bad line does not hide the rest. A name must be safe as
  `/packs/<name>.tgc` with the portal's character set and no hidden names; sizes
  are 64 B..5 MB, cards 1..65535, `sha256` 64 hex digits. A bare-file-name `url`
  resolves against the **configured** catalog URL, not the post-redirect one
  (for GitHub that is a signed, expiring blob URL). At most 32 entries and a
  32 KB catalog body.
- **Download** (`NetFetch`, shared with the portal): free space is checked
  against the catalog size plus the portal's 96 KB reserve *before* connecting;
  the `Content-Length` must equal the catalog size; SHA-256 is computed while
  streaming (mbedTLS, hardware SHA) and compared, with the byte count, before
  the usual magic + `PackReader` probe and the rename of `/packs/.fetch.tmp` over
  the destination. Any failure removes only the temp. Afterwards `/packs` is
  rescanned through `portalPacksChanged()`, the portal's path.
- **"Already here"** is decided by existence and size: the same size marks a
  deck as on the device (a hold studies it), another size as an update (a hold
  downloads it, replacing the file). Replacing the loaded deck closes it first
  and reopens it afterwards, also when the download failed. A same-size file
  with other content is not detected (hashing the local file is a follow-up).
- **The portal's "download from a URL" now uses the same code.** Its messages
  are unchanged; one behaviour changed: a body shorter than its announced
  `Content-Length` is now "download failed" instead of being probed and possibly
  kept as a truncated pack.

## 2026-10-18 – NTP over the home Wi-Fi

- **The stored offset, or nothing.** NTP yields UTC; the study day needs the
  local offset. The NTP paths use the `tz` key the last phone sync left in NVS
  and **skip the sync** (logged) when it is absent rather than assume UTC: a
  wrong offset can move the 04:00 rollover across midnight and put the day off
  by one, which the index can never take back. In practice the offset is known,
  because the portal page that saves the home Wi-Fi also posts the browser's
  clock. It stays a snapshot: DST or travel is only corrected by opening the
  portal page again.
- **No radio just for the clock, except after a power cut.** NTP runs (3 s
  timeout, `HomeWifi::fetchUtcMs()`) when Get decks has connected, before the
  catalog read; a failure is logged and Get decks goes on. And when the boot
  shows the date question (power cut, or a wake that re-asks it) with a home
  Wi-Fi and the offset saved, the date screen joins in the background, once per
  boot: it is the one moment the day is unknown. Every other boot keeps the
  radio off.
- **The manual answer stays first-class.** The stepper works while the screen
  says "Syncing via <ssid>..."; confirming or skipping stops the join, radio
  off. Connected -> NTP (the loop blocks for at most 3 s, presses made meanwhile
  are dropped) -> radio off -> `applyWallClock()`, which dismisses the question
  as a phone sync does. A failed join or NTP leaves "No network (time): set it
  by hand". Joining in the background was chosen over a blocking "syncing"
  screen before the question: without the network that would cost the 15 s
  join timeout every time.
- **Power.** The CPU runs at 240 MHz only while the radio is on (`netRadioOff()`
  on every exit). The join and its outcome count as activity for the idle
  timers, so neither the 30 s dim nor the 2 min sleep can cut the 15 s join +
  3 s NTP short, and the status line gets a full timeout to be read. An idle
  timeout or an explicit sleep turns the radio off as for Get decks; a sleep
  while the question is open still re-asks it (and retries NTP) on the wake.
- **A new day under the settings.** `applyWallClock()` already gave an
  unanswered question (or the done screen) way to the new queue; on the portal
  and Get decks screens it now does the same for the screen the settings home's
  "Continue" returns to, so a sync there does not leave a stale "all reviewed".
- **Flash:** the StickS3 build grows by ~4 KB (the SNTP client is linked now).
  **Not verified on hardware.**

## 2026-10-18 – Integration review fixes

Found by a read-only review of the merged phase 2/4 branches (`integrate/2026-10-18`).

- **A catalog deck name may not end in '.'.** `x.` was accepted and saved as
  `/packs/x..tgc`, which the portal's delete refuses (it rejects any ".."), so
  the deck could never be removed again. `catalogNameIsSafe()` now refuses a
  trailing '.' too (native test).
- **Stale download/upload temps are removed.** `fetchPackFile()` measured the
  free space *before* removing a `/packs/.fetch.tmp` left by an interrupted
  download, and nothing else removed it, so a power cut mid-download kept up to
  5 MB occupied and every later download or upload failed "not enough space".
  `setup()` now removes `/packs/.fetch.tmp` and `/packs/.upload.tmp` right after
  mounting LittleFS (nothing can be in flight at boot), and the download and the
  portal upload discard their temp before measuring the space. `kFetchTmp`
  moved to `NetFetch.h` for that.
- **Unplugging a dark panel saves before sleeping.** On USB power the idle
  timeout saves and only blanks the panel; the uptime rule keeps advancing the
  day carry in RAM, and the unplug path went straight to `armSleep()` +
  `deepSleep()`, so a night on the charger lost those hours (04:00 rollover
  included) on the next wake. Every sleep of the running app now goes through
  `prepareDeepSleep()`: save (with the usual guard: a store that failed to load
  and has no answers is not written), portal and home Wi-Fi off, then
  `armSleep()`. The explicit sleep calls it before its notice, so the saved
  carry and the RTC timestamp describe the same moment. The critical-battery
  re-sleep at boot (keeps the first sleep's timestamp) and `halt()` (SRS maybe
  not loaded) stay as they were.
- **A pack change under the settings refreshes "Continue".** The portal (upload,
  delete, URL download) and Get decks reload, swap or close the live pack while
  the settings home waits to "Continue" into the quiz, detail or done screen,
  but only `netDownload()` refreshed the question kept for it, and only when
  it had closed the live deck itself to replace it (or none was open).
  Deleting every pack and pressing Continue drew a closed pack (crash);
  replacing or deleting the live pack showed the old question and a hold graded
  whatever card sat at that index in the new pack. `portalPacksChanged()`, the
  path both share, now notices when the live reader was replaced or closed:
  with a pack open it rebuilds the queue and builds the question Continue
  returns to (or the done screen); with none, Continue opens the deck list,
  whose "+ Get decks" row still works. The deck list was chosen over the
  boot-time upload screen because that one deliberately makes the side button
  inert, and the portal opened from the settings must stay leavable. A failed
  reload that kept the old reader keeps its question. Continue also restarts
  the answer clock of an unrevealed question, as a wake from a dark panel does:
  the question was hidden (or built) behind the settings, and that time must
  not grade it Hard. As a last line of defence, `drawQuiz()`, `drawDetail()`,
  `tapAction()` and `holdAction()` check `questionCard()` (pack open, a question
  built, its card readable) and otherwise draw "Cannot open pack / side hold:
  menu" or do nothing, so nothing is ever graded for a card that is not on
  screen.
- **A download no longer blocks the power button and the battery guard.** Get
  decks downloads block `loop()` for up to 10 min at 240 MHz, and the progress
  callback only watched the side button. It now also feeds the critical-battery
  guard every 10 s (`batteryCritical()`, shared with `loop()`) and polls the
  power button; either one cancels the download and records the sleep request,
  and `netDownload()` calls `sleepNow()` only after `fetchPackFile()` returned,
  so the temp is already removed and the old deck untouched (the wake reopens
  a deck that was closed to be replaced). A download is also refused on battery
  below the guard's resume level (6 %, `BatteryGuard::canResume()`), with
  "Battery low" in the footer. The portal's URL download has no progress
  callback and is unchanged (it is capped at 60 s).
- **"Study more" is for today only.** The done screen's +10 new-card override
  (`gExtraNew`) was never reset on a day change, so the next day started with
  yesterday's raised cap (and every further tap stacked on it). It now carries
  the day it was granted, and `extraNewToday()` drops it once the day index
  differs: one check at the point of use covers the uptime rollover, the sleep
  time, a manual date and a wall-clock sync alike, instead of a reset in each
  of those paths. Loading another deck still resets it, now also when the
  portal or Get decks swaps the live deck. Verified on the way: the NTP merge's
  `applyWallClock()` already gives the settings home's "Continue" a fresh
  question when a sync on the Wi-Fi or Get decks screen starts a new day.
- **An update that does not fit says what to do.** A download keeps the old
  copy until the new one validated, so updating a big deck needs room for both,
  and the footer only said "Not enough space". For an update (`~` row) that
  fails that way, Get decks now shows its error screen: "Not enough space" and
  "An update needs room for both copies. Delete the old one first: Settings >
  Wi-Fi, 192.168.4.1" (es: "Hace falta sitio para las dos copias...").

## 2026-10-18 – First boot formats a data partition with no LittleFS

- **Found while preparing an onboarding test.** The merged release image
  carries no filesystem (earlier docs said "an empty filesystem", which was
  wrong): the data partition is whatever was there before -- erased by a browser
  flasher, or another firmware's data. `LittleFS.begin(false)` fails on both, and
  the app stopped on "Filesystem not found / Run: pio run -t uploadfs", so no new
  user could ever reach the Wi-Fi upload screen.
- **Format only what is not ours.** `mountFilesystem()` formats the partition
  (with a "Preparing storage..." notice) only when it holds no LittleFS at all:
  neither block of the root metadata pair has the superblock's `"littlefs"`
  magic at byte 8. A LittleFS that is there but will not mount is damage, not a
  fresh device, and is never erased silently: the error screen ("Storage
  damaged") erases it only on a long press of the front button, after the press
  that woke the chip was released, then restarts. `LittleFS.begin(true)`
  (format on any failure) was rejected because it would wipe decks and progress
  on a transient or partial corruption without asking.

## 2026-10-18 – Default font: Noto Sans JP

- **Noto Sans JP Regular replaces Klee One SemiBold as the builder's default
  font** (decided with the maintainer; supersedes the "Default font: Klee One"
  bullet of the 2026-10-16 entry). The app is about *recognising* kanji in
  print, so the glyphs should look like the gothic learners meet in real texts
  (and in WaniKani) rather than Klee One's textbook (教科書体) style. Stroke
  shapes for writing are still shown by the KanjiVG stroke animation, which
  does not depend on the font. Klee One stays available: `fetch --klee-one`,
  then `build --font cache/fonts/KleeOne-SemiBold.ttf`.
- **Regular, not Medium**, after comparing previews: Medium clogs dense kanji
  at the 40 px option size, and light ink on the dark panel already looks
  heavier on the real screen than in a preview.
- **M5GFX's built-in `lgfxJapanGothic_*` fonts were rejected**: they stop at
  40 px (the prompt is 96 px), are 1bpp with no antialiasing, and cost ~534 KB
  of app flash per size. Packs keep their pre-rendered glyphs.
- **Pinned download.** `fetch` gets the Japanese subset OTF and the OFL
  `LICENSE` from the noto-cjk `Sans2.004` release tag (the latest Sans release,
  byte-identical to `main` today) into `cache/noto/`.
- **No format or layout change.** Both fonts have the same vertical metrics
  (1160/-288 per 1000), so text line heights and ascents in the header are
  unchanged. N5 builds for en/es/fr/pt and N4-N1 for es keep every card and
  every text glyph (œ, ç, ã, õ included). Packs grow ~4-7 % because gothic ink
  fills more of the em square (N5 es 375,356 -> 401,816 bytes; N3 es 1,738,280
  -> 1,829,012; N1 es 5,765,932 -> 6,026,068, which still fits the StickS3's
  6,225,920-byte LittleFS partition, but with little room to spare). Meaning
  distractors are ranked from the font's own bitmaps, so they shift too (N5 es:
  the first distractor changes on 28 of 79 cards).
- The UX mockups (`tools/ux-mockups/`, `docs/ux/`, `docs/ux-refresh.md`) were
  rendered with Klee One and are left as they are: they show layout, not the
  final typeface, and `render.py` still reads `cache/fonts/KleeOne-SemiBold.ttf`
  (fetch it with `--klee-one`).

## 2026-10-18 – Decks release: `decks-v<format>` instead of `latest`

- **Why.** The default catalog, `releases/latest/download/catalog.txt`, needed
  the newest release to carry the packs, but `release.yml` attached them only on
  an opt-in dispatch: any tag-push firmware release (never with packs) would
  become "latest" and break Get decks. There was no release yet, so it 404'd.
- **The decks get their own release with a fixed tag, `decks-v<pack format
  version>`** (`decks-v2` for format v2), created with `--latest=false` so it is
  never "latest" and no firmware release can hide it. `.github/workflows/decks.yml`
  (manual dispatch only) builds, checks and publishes it; a rebuild replaces the
  assets in place (`gh release upload --clobber`, packs first and the catalog
  last, so a device reading the new catalog finds the new packs) and refreshes
  the title and notes. A run publishes its whole set: `catalog.txt` lists exactly
  the packs it built, and `.tgc` assets left by an earlier run that it did not
  build are deleted, so the release and its catalog always agree. A format bump
  publishes a new `decks-v<N+1>` release and old firmware keeps reading
  `decks-v<N>`, so it never downloads a pack it cannot open.
- **Firmware default** (supersedes the "Default catalog URL" bullet of "Get
  decks: the catalog, the download and where it lives"):
  `HomeWifi::kDefaultCatalogUrl =
  https://github.com/dcarnicer/tangocho/releases/download/decks-v2/catalog.txt`,
  to change together with the pack reader's version. The portal's override field
  stays. Until `decks-v2` exists the URL answers 404, which the device reports as
  "No catalog published at this URL yet" -- still accurate, and also right for a
  future firmware whose `decks-v<N>` is not out yet.
- **Redirects and roots still hold.** `releases/download/<tag>/<asset>` is one
  hop shorter than `latest`: `github.com` answers 302 straight to
  `release-assets.githubusercontent.com` (signed URL), which answers 200 with a
  `Content-Length` to an HTTP/1.0 request (`curl -sIL --http1.0` on KanjiVG's
  r20260714 asset, 2026-10-18). Same hosts, so the embedded roots ("HTTPS for Get
  decks") are unchanged.
- **The tag is derived, not typed.** The workflow reads every pack's format
  version from `tangocho-pack inspect`, fails if they disagree, and publishes to
  `decks-v<that>`. It reads the per-pack cap `kCatalogMaxPackBytes` (5 MiB) from
  `firmware/src/catalog/Catalog.h` and fails if a pack is over it (Get decks
  would skip that record). It also fails unless the builder's default font is
  Noto Sans JP, the font the notes credit, so the attribution cannot go stale.
- **Default set**: JLPT N5..N1 and the full Jōyō deck, each in es and en, all
  with `--strokes` (12 packs: `n5-es.tgc` .. `n1-en.tgc`, `joyo-es.tgc`,
  `joyo-en.tgc`). Levels, languages and the Jōyō deck are dispatch inputs
  (validated against fixed lists and passed through the environment), plus
  `publish` (off: build, check and keep a workflow artifact only).
- **The Jōyō deck** (`tangocho-pack build --joyo`) is KANJIDIC2 grades 1-6 and 8
  = 2,136 kanji, named `Joyo kanji`: deck names stay ASCII because the device
  draws them in its built-in font, which has no "ō" (the builder now warns about
  a non-ASCII name). Its cards are in **school order** -- grade 1..6, then 8, each
  by newspaper frequency, kanji with no frequency rank last (fewer strokes first)
  -- because a pack's card order is its `level` hint, the order the device
  introduces new cards in (docs/srs.md). A JLPT order would only replay the N5..N1
  decks back to back; school order is the curriculum Japanese learners follow and
  starts with the simplest, most frequent kanji (日一人年大十二本中出...).
  `--order jlpt|grade` overrides either default.
- **Sizes in pack format v1** (Klee One, `--strokes`, built 2026-10-18): N5 0.42
  MB, N4 0.91, N3 2.03, N2 2.02, N1 6.80, Jōyō 11.62 (es; en within 0.1 %). N1
  and Jōyō are over the 5 MiB cap and the StickS3's 5.9 MB filesystem, so the
  default set only passes the workflow's check with pack format v2's compression;
  until then a run with those decks fails before publishing anything.
- **`release.yml` no longer builds packs** (supersedes the "Prebuilt packs are
  opt-in" bullet of "Release artifacts"): the `build_packs`/`packs_jlpt`/
  `packs_langs` inputs and the packs job are gone, the rest is unchanged. The
  firmware notes link to the decks release named in that firmware's own
  `kDefaultCatalogUrl` (read from `HomeWifi.h` when the release is made), for
  Get decks and for manual download.
- **Still open:** running the decks workflow is a maintainer decision, blocked
  on the licence of the tanos.co.uk JLPT lists ("Before publishing" in the
  roadmap).
- **Catalog order = learning order** (added at integration). Get decks lists
  decks in catalog order (own language first), and `catalog` sorted by file
  name put `joyo-*` first and N5 last. `tangocho-pack catalog --order NAMES`
  lists named packs first; `decks.yml` passes its build order, so the catalog
  reads N5, N4, N3, N2, N1, Joyo per language and a new device's selection
  starts on N5.

## 2026-10-18 – Get decks from the portal page; own language first

- **One step from the page.** With a home Wi-Fi saved, the portal page's Home
  Wi-Fi section has a **Get decks now** button (`POST /getdecks`); saving the
  network redirects to `/?wifi=saved#home`, which confirms the save and points
  at it. The device stops the portal and opens Get decks: the boot-time upload
  screen's hold, now one function (`wifiToGetDecks()`), from the boot-time
  portal and the Settings one alike. Its exit is the existing one: from the
  boot-time state the upload screen again, or the quiz once a deck is loaded;
  from Settings the deck list, whose side button goes to the settings home. The
  boot-only upload-ready flag is not touched. No home Wi-Fi saved: no button,
  and `POST /getdecks` answers 400.
- **The response goes out before the AP goes down.** As with the URL download,
  the handler only records the request and sends the page; `poll()` calls the
  new `Hooks::onGetDecks` 1 s later, so `WifiPortal` still never reaches into
  `main.cpp`. The WebServer closes the connection right after `send()` and lwIP
  delivers the buffered page in the background, so leaving on the very next
  poll could pull the AP from under it; a second is ample on a local link. A
  side press, an upload that starts the quiz or the idle timeout within that
  second stops the portal, and `end()` drops the request. `loop()` ignores the
  Wi-Fi screen's input once `poll()` has left it, so a press in that iteration
  cannot `closeWifi()` over the Get decks screen. The page says the device
  leaves this Wi-Fi to download over the home network, that the phone can go
  back to its own network, and to follow the device screen; the switch undoes
  an idle dim, since typing on the phone is not button input. A network
  forgotten within that second gets Get decks' own "No home Wi-Fi saved".
- **Own language first.** Get decks orders the catalog so that the decks whose
  `lang` is the screen's language come first, in catalog order, followed by all
  the others in catalog order (a stable partition, `orderCatalogForLang()` in
  the pure `catalog/` module, native-tested); nothing is hidden. The language is
  the string table the screens are drawn with: the Language setting, or the
  pack's language under Auto (`uiLangCodeFor()` through `setUiForLang()`).
  After every pack was deleted under Auto it stays the last pack's language,
  which is what the screen still shows. The primary subtag is compared without
  case (`es-MX` counts as Spanish). The selection still starts on the first
  deck not on the device, so now in the user's language when there is one. A
  fresh device has no pack, hence an English UI under Auto; so the page's button
  also posts the phone's `navigator.language` (hidden field, filtered to
  letters, digits and `-`), and when the setting is Auto **and no pack is
  open** that tag orders the list instead (also after every pack was deleted).
  The screens stay in their own language; the hint lives until Get decks closes
  and the boot-time hold sends none.
- **Flash:** +3.8 KB on the StickS3 build. **Not verified on hardware.**

## 2026-10-18 – Pack format v2: compressed glyphs and strokes

- **Why.** Raw 4bpp bitmaps made packs ~5.5 KB per kanji: JLPT N1 with
  `--strokes` was 6.8 MB and a Jōyō-sized pack 11.6 MB, over the 5 MB per-pack
  cap and the StickS3's 5.9 MB filesystem. Set 0 (96 px) alone was two thirds
  of a pack, and most of its pixels are 0 or 15.
- **Glyphs: encoding 1, a nibble-code RLE** (docs/pack-format.md). The stream is
  4-bit codes, high nibble first like the pixels: `1..14` one anti-aliased
  pixel, `0 L` / `F L` runs of 0 / 15, `0 0 L` copy the next `L` pixels from the
  row above; lengths are 1–3 codes (1..330). Pixels run on across row ends. The
  length is implied by the codes and never exceeds the raw size, so the glyph
  table did not change. The builder keeps encoding 0 whenever RLE is not
  strictly smaller (it never is for an empty glyph), so the worst case never
  grows. The encoder is greedy (copy if it covers >= 3 pixels and more than a
  run, else a run, else a literal); an optimal parse was only 0.1–0.2 points
  better.
- **Measured before choosing** (N4, 166 kanji, set 0 / set 1 as % of raw, Klee
  One | Noto Sans JP Medium): the crude byte RLE 31/70 | 26/59; a byte-oriented
  code (2-bit op + 6-bit count: zero run, ink run, literal block) 35/70 |
  31/69, because two thirds of the anti-aliased runs are a single pixel and a
  byte-aligned code pays a whole byte for each; the nibble code without copies
  23/50 | 20/43; **with copies 21/46 | 14/32**; zlib per glyph 25/54 | 15/37.
  Copies from up-left/up-right gained nothing (< 0.1 point). The brief asked
  for a byte-oriented code; the nibble one was kept because it is a third
  smaller and decodes with the same "next nibble" step the raw format already
  uses (no tables, no bit fields across bytes).
- **Stroke masks: encodings 1 and 2.** Alternating run lengths, empty first,
  1–3 nibble lengths (0..285), trailing empty pixels implied; encoding 2 codes
  each row XOR the row above (a vertical stroke becomes two runs). Per mask the
  builder stores the smallest of raw, 1 and 2. On N4: plain runs alone ~50 %,
  row XOR alone ~43 %, the per-mask best 33 % (zlib per mask 56 %, over a card's
  whole mask data 37 %).
- **Stroke layout change.** A stroke entry had no free byte, so v2 reuses the
  block's reserved u32 for the offset of the card's first mask and replaces each
  entry's absolute `data_off` with `encoding`, `reserved` and a u16 `size`; masks
  are back to back in stroke order. Entries stay 8 bytes, sizes are exact (the
  reader validates and fetches exactly), and the v1 per-mask 4-byte padding
  goes. `strokeAt(i)` sums the sizes of the entries before `i` in one read.
- **Why not deflate (miniz / the ROM's tinfl).** Per bitmap it is not smaller
  (numbers above), and compressing across bitmaps, where it would win, breaks
  the random access the reader relies on. Inflate also needs an ~11 KB
  decompressor state plus a dictionary window or the whole output, more flash,
  and a second implementation to cross-check in tests; the RLE decoders are
  ~200 lines with no state beyond the caller's bitmap and a 256-byte read
  window.
- **Results** (`--lang es --strokes`, defaults 96/40/18/20 px; v1 → v2 bytes):

  | deck | kanji | Klee One v1 → v2 | Noto Sans JP v1 → v2 |
  |------|------:|------------------|----------------------|
  | N5   |    79 | 424,906 → 119,944 | 459,906 → 107,440 |
  | N4   |   166 | 908,352 → 280,944 | 975,980 → 238,980 |
  | N3   |   367 | 2,027,180 → 644,120 | 2,150,336 → 539,576 |
  | N2   |   367 | 2,019,558 → 643,092 | 2,142,434 → 533,712 |
  | N1   | 1,232 | 6,795,127 → 2,236,624 | 7,160,003 → 1,837,008 |
  | Jōyō (grades 1–6 + 8) | 2,136 | 11,619,375 → 3,691,532 | 12,271,851 → 3,022,048 |

  Ratios in the Jōyō build: set 0 23 % (Klee) / 16 % (Noto), set 1 50 / 36 %,
  text sets 58–65 %, masks 34 %; the stroke section is ~340 B per kanji (was
  ~800 B). Every pack fits the 5 MB cap; the Jōyō pack with strokes is 3.7 MB
  with Klee One, a little over the ~3.5 MB target, and 3.0 MB with Noto. Pixels
  are unchanged: the firmware reader decodes the v2 N1 and Jōyō packs to the
  same bitmaps as the v1 builds (checked by hashing every glyph and mask).
- **Compatibility.** `version` is 2; readers accept 1 and 2 (in a v1 pack every
  glyph must be encoding 0 and blocks use the v1 layout), so the packs already
  on devices keep working. Firmware from before this change checks
  `version != 1` and refuses a v2 pack with "unsupported pack version" before
  reading any bitmap. The builder writes v2 only.
- **Firmware.** `pack/Rle.{h,cpp}` (pure, native-tested) decodes through a
  `ByteIn`: memory, or a 256-byte window refilled from the `PackSource`.
  `readGlyphBitmap`/`readStrokeBitmap` decode transparently, so the renderer
  and the stroke animation draw decoded data as before. The renderer now also
  keeps the decoded set-0 glyph in its scratch buffer, so the quiz's redraws
  (every button press) skip the read and decode. +1.9 KB of flash. With
  `TGC_QUIZ_LOG` (`sticks3-dev`) an `R` line gives the prompt kanji's draw
  time and the fetch (read + decode) time inside it, and the full stroke frame's
  draw time; not yet measured on hardware.
- **Tests.** `tangocho-pack fixture` also writes `fixtures/rle_vectors.bin`
  (synthetic edge cases, every glyph and mask of `mini.tgc` with its decoded
  bitmap, malformed streams); the firmware's `test_rle` suite decodes all of it
  and reads the v2 `mini.tgc` against it. `fixtures/mini_v1.tgc` is the last v1
  fixture, frozen, for the v1 reader test. The byte-identity baseline of the
  no-strokes writer is now `tests/data/mini_v2_baseline.tgc`.

## 2026-10-18 – Portal over the home Wi-Fi; task-oriented page; Wi-Fi menu

- **Why.** Tested on hardware the same day: everything worked, but (1) the
  page was ordered packs → upload → URL download → home Wi-Fi → catalog →
  statistics, which reads backwards for a first setup; (2) using the page put
  the phone on the device's open access point, without internet; (3) Settings
  had a single "Wi-Fi" row. Wi-Fi stays **off by default**: the radio runs only
  while the Wi-Fi screen is open (5 min idle timeout, HTTP requests count),
  during Get decks and the new network test, and for the one NTP sync after a
  power cut.
- **Two modes, one page** (`WifiPortal::Mode`). *Setup mode* is the open
  access point of before (`begin()`). *Home mode* (`beginHome()`) serves the
  same WebServer on the station IP after the app joined the saved network with
  `HomeWifi` (its 15 s timeout and failure reasons) and ran NTP as Get decks
  does. The portal never owns the station: `end()` leaves it up and the app
  turns it off (`stopPortal()` → `netRadioOff()`), so the radio and CPU rules
  stay in main.cpp (240 MHz only while the radio is on, 80 MHz on every exit).
  "Web portal" opens home mode when a network is saved, else setup mode;
  "Setup mode" forces the access point. A failed join shows why with "tap: try
  again, hold: setup mode" (a hold during the join skips it). At boot with no
  deck: no network → setup mode as before; a network → home mode, **falling
  back to setup mode by itself** when the join fails, the reason on screen,
  because that screen has nothing else to offer.
- **Switching.** Once a network is saved, the setup-mode page offers **Switch
  to my Wi-Fi** (`POST /home`), staged like "Get decks now" (the 1 s grace is
  now a shared `Leave` stage): the response page tells the user to go back to
  their Wi-Fi and open the address or QR code on the device, then
  `Hooks::onSwitchToHome` → `openPortal(Home)`. In home mode the same POST is
  **Reconnect** (after saving another network). **Get decks on the device
  screen** (`POST /getdecks`) stays in setup mode, unchanged; home mode lists
  the decks on the page instead.
- **QR codes** with M5GFX/LovyanGFX's own `qrcode()`, no new library. The
  version comes from `qrVersionForBytes()` (byte mode, ECC level L, which is
  what `qrcode()` uses), and the code is drawn exactly `modules × px` wide
  inside a white square with a **2-module quiet zone** (the spec's 4 would cut
  the modules from 4 to 3 px on the 135 px screen; phone cameras read 2 fine
  against the black surround -- to confirm on hardware). Setup mode:
  `WIFI:T:nopass;S:<ssid>;;` with `\ ; , : "` escaped (`wifiQrOpenNetwork()`,
  native-tested); a tap swaps it for `http://192.168.4.1/`. Home mode:
  `http://<ip>/`. Portrait stacks the QR (116 px for version 2) over the text,
  landscape puts it on the left; a URL too wide for the 1x line drops
  `http://` and the slash.
- **The catalog on the page.** Home mode reads it from `poll()` right after the
  server starts (one iteration to paint "reading catalog...", then the blocking
  `fetchCatalog()`, now shared with Get decks and the test; the page
  meta-refreshes every 2 s meanwhile), with "Try again" on a failure and a new
  read when the catalog URL changes. Rows are ordered by `orderCatalogForLang()`
  with `catalogOrderLang()`: the device UI language, else the page's `?lang=`
  choice, else the browser's top Accept-Language tag (any language, as Get
  decks uses `navigator.language`). Each row's state (Download / Update / On
  the device) is `catalogDeckState()`, the size rule the device list now shares.
- **Downloads from the page.** `POST /deck` (by name; only a catalog deck that is
  not already on the device) is staged: one poll paints the device screen, the
  next runs `Hooks::onDeckDownload` → `downloadCatalogDeck()` in main.cpp, the
  function Get decks now uses too: refused on battery below 6 %, the live deck
  closed and reopened, `fetchPackFile()` with size, SHA-256, free space + the
  96 KB reserve, magic + probe and the atomic rename, then
  `portalPacksChanged()`. The side button cancels; the power button and a
  critical battery cancel and sleep, as before. **One download at a time**:
  while one is staged or running, another download, a delete, an upload, a URL
  download and a switch are refused ("busy"). The progress callback
  (`netProgress()`) redraws the Wi-Fi screen and calls
  `WifiPortal::serviceDownload()`, which answers the page at most every 100 ms:
  `GET /` is then a small progress view (meta-refresh 2 s, no script) and
  `/api/status` gives the same as JSON. Accepted gap: an upload posted from
  another tab during a download is read (and discarded) by the WebServer inside
  that callback and can stall the TLS stream past its 15 s idle timeout; the
  progress view itself has no forms.
- **Memory.** In home mode a TLS download runs while the WebServer is up: still
  one TLS connection at a time, plus at most one short plain-HTTP client served
  between body chunks. New `[net] home portal up` and `[net] page download
  start/end` heap lines join the existing `before/after TLS` ones. **Not
  measured on hardware.**
- **mDNS.** Home mode also answers as `http://tangocho.local/` (ESPmDNS, started
  and stopped with the server). Cost **+25.9 KB of flash** (1,386,821 vs
  1,360,889 bytes on the StickS3) and an mDNS task while home mode runs. Kept
  because a bookmark survives a new DHCP address on iOS/macOS/Windows; Android
  resolves `.local` unevenly, so the QR code and the IP stay primary.
  `-DTGC_PORTAL_MDNS=0` drops it.
- **The page**, one self-contained HTML, no external assets: a status line
  (decks on the device, free space, "via <home network>" or "on the device's
  own network <AP>"), the clock line, a one-shot notice, then the task: *first
  setup* (no network saved): step 1 "Connect the device to your Wi-Fi" (SSID
  with the scan's suggestions, password), step 2 what comes next; *setup mode
  with a network*: Get decks = Switch to my Wi-Fi / Get decks on the device
  screen; *home mode*: the catalog. Then **Your decks** (the loaded one marked,
  delete), then **Advanced** as a closed `<details>` (upload, URL download,
  catalog URL, home Wi-Fi change/forget/Reconnect, statistics, JSON links),
  opened when the last action was one of these. POST handlers now redirect
  (303) with a one-shot notice instead of separate result pages; the leave
  pages and the upload result stay standalone (after a boot-time upload the
  device moves into the quiz and stops the portal right after its response). Script stays minimal: the `POST /time` clock script
  (its messages now come from data attributes), `confirm()` on delete/forget
  and `navigator.language` for Get decks; every form works without it.
- **Language.** `portalPageLang()`: the device UI language (the Language
  setting, or the loaded deck's under Auto); when Auto has no deck, the page's
  `?lang=` link (kept for the session), else the highest-q English or Spanish
  Accept-Language entry, else English. The strings live in `portal/PortalText`
  (English defaults, Spanish overriding each by name; a native test checks the
  Spanish table is complete). The device screen's status line from the portal
  (`lastMessage()`) stays English, as before.
- **Settings › Wi-Fi.** Row 6 becomes **"Wi-Fi »"** (Font0 has `»`, CP437 0xAF,
  not `›`), so the settings home keeps 11 rows; it opens a list in the
  settings style: **Web portal**, **Home: <network>|none** (a test: join, IP,
  RSSI with a word from `rssiQuality()`, the catalog -- deck count or the
  error, which is the end-to-end check that the host is reachable, TLS
  included -- and the network time when the offset is saved, applied like any
  NTP sync; then radio off; a hold repeats), **Setup mode** and **Forget
  network** (a Cancel/Forget picker with Cancel selected, so it takes a tap and
  a hold; with nothing saved, a note). The side button goes back to the list,
  and from there to the settings home.
- **The boot-only upload flag is unchanged** (set only at boot; the side button
  is inert while there is no deck). Setup mode still moves into the quiz as
  soon as an upload brings a deck. Home mode does not: its page is where the
  decks come from, and leaving under it would cut the page off mid-use; once a
  deck is loaded the screen says "side: start studying" and the side button
  stops the portal and starts the quiz.
- **Security.** Home mode exposes the unauthenticated page to the whole home
  network while the Wi-Fi screen is open -- the same trade-off as the open
  access point (anyone in radio range), stated on the page, in README and in
  `WifiPortal.h`. The password stays write-only: never in a page, the JSON or a
  log.
- **Kept:** the dim/sleep timers and their activity accounting, radio off and
  80 MHz on every exit, the battery and power-button guards during downloads,
  the boot-only upload flag, the already-here/update rule, NTP on connect.
- **Flash:** StickS3 1,319,353 → 1,386,821 bytes (+67.5 KB, 66.1 % of the
  2 MB slot): mDNS 25.9 KB, the rest the QR encoder, the page code and the two
  string tables. Static RAM 51,164 → 54,148 bytes. Pure helpers in
  `portal/PortalLogic` with native tests (`test_portal`). **Not verified on
  hardware.**

## 2026-10-18 – JLPT level lists: licence verified (CC BY)

- **The open "Before publishing" item is closed.** The N5-N1 level of each
  kanji (`jlpt_new` from davidluzgouveia/kanji-data, MIT) comes from Jonathan
  Waller's JLPT Resources (tanos.co.uk), whose "Use my data!" page licenses
  everything not for sale under **Creative Commons BY**: use freely, credit the
  site. CC BY content may go into our CC BY-SA packs with that credit, which the
  release notes, README and docs/data-sources.md already give.
- **Evidence is archived, not live:** tanos.co.uk no longer resolves (checked
  2026-10-18), so docs/data-sources.md quotes the page and links the Wayback
  Machine copy (2025-01-17 capture of a page updated 2011-10-12).
- **No other source needed.** JLPT has published no official kanji lists since
  2010; every N-level list is a community reconstruction, and the open ones
  found (e.g. OpenJLPT, CC BY-SA 4.0, 2,211 kanji, the same counts per level)
  are themselves derived from the tanos lists. KANJIDIC2's own `jlpt` field is
  the pre-2010 four-level scale (old level 2 spans N3 and N2), so it stays the
  fallback only.

## 2026-10-18 – Long readings: one row per option instead of the 2x2 grid

- **Found on hardware:** 「ちいさい」 was cut with an ellipsis. A portrait grid
  cell leaves 61 px for text, i.e. 3 kana at either kana size (18 or 20 px,
  full-width advances), and 406 of the 1,777 kun readings the Jōyō deck asks
  are 4-6 kana. Not a regression of the font change: Klee One has the same
  advances.
- **Rule:** the grid stays when every option fits a cell (large kana, else the
  small text, as before); otherwise all four options become full-width rows
  (the meaning layout), in large kana when they fit the row, which holds 6 (the
  longest first reading in Jōyō, e.g. うけたまわる). A row is 24 px tall in
  portrait; the 20 px kana ink fits it although its 30 px line box does not.
  `tangocho-pack preview` mirrors the rule.

## 2026-10-18 – About screen and firmware version

- **Why.** Before the first firmware release (v0.1.0): the packs are CC BY-SA
  and redistribute KANJIDIC2, KanjiVG, the tanos.co.uk JLPT lists and glyphs
  rendered from Noto Sans JP, but the device itself showed no attribution (the
  last open item of "Attribution for redistributed data" in the roadmap); and
  nothing told which build a device or a portal page runs.
- **The version string.** `firmware/scripts/version.py`, a PlatformIO `pre:`
  script on every board env: `$TGC_VERSION` if set, else `git describe --tags
  --always --dirty --match "v[0-9]*"`, else `"dev"`; characters outside
  `[0-9A-Za-z._+~-]` become `_`, 40 at most. The `--match` matters: the nearest
  tag in this repo is the decks release `decks-v2`, which would otherwise name
  firmware builds `decks-v2-9-g5be0ac9`.
  - **One file sees it.** A global `-D` changes every compile command, so each
    commit would rebuild the Arduino core and every library. The define goes to
    `src/Version.cpp` only (`env.AddBuildMiddleware()`), and the rest of the
    firmware calls `tgc::firmwareVersion()`; a new version recompiles that file
    and relinks (checked). `Version.cpp` falls back to `"dev"` without the
    define, and the native env never compiles it, so the pure modules and their
    tests do not depend on it.
  - **Per context.** Local: `git describe` (`v0.1.0-3-gabc1234-dirty`, or the
    commit id while no `v*` tag exists). CI (`ci.yml`): the shallow checkout has
    no tags, so the commit id. Release (`release.yml`): the firmware job exports
    `TGC_VERSION` = the resolved tag as job env (never interpolated into a
    script), and a new step fails the job when `strings firmware.bin` does not
    contain it (a tag with characters the script replaces). Permissions are
    unchanged. A manual dispatch builds the dispatched ref and labels it with
    the input tag, as its release name already did.
  - **Where it shows:** the first boot log line `Tangocho <version>`,
    `"version"` first in `/api/stats` and `/api/status`, Settings › About and
    the portal footer.
- **Settings › About.** A 12th settings row, "About" / "Acerca de", after
  Sleep (the group refactor of docs/ux-refresh.md moves it under `Device ›`).
  Built-in Font0 only, so it works with no deck. Two groups:
  - *Firmware:* "Tangochō" in the 2x font (Font0 has no "ō": a 6x2 px macron
    is drawn over the "o", whose rows 0-1 are blank), the version, "Firmware:
    MIT", "Decks: CC BY-SA", `github.com/dcarnicer/tangocho`.
  - *Deck:* "Deck: <name>" ("none" with no deck) and its credits:
    KANJIDIC2 (c) EDRDG, CC BY-SA 4.0; KanjiVG (c) Ulrich Apel, CC BY-SA 3.0,
    only when the loaded pack has a stroke section (or no deck is loaded);
    Jonathan Waller (tanos.co.uk), CC BY; Noto Sans JP, SIL OFL 1.1. "(c)"
    because Font0 has no copyright sign. The strings live in `src/Credits.h`,
    shared with the portal; labels are in the `ui()` tables (yellow, the value
    follows on the same line).
  - *Layout:* `ui/TextLayout.h`, pure and native-tested: wraps by code points
    (Font0 is 6 px monospace) at spaces; U+00A0 in Credits.h keeps "CC BY-SA
    4.0", "Ulrich Apel" or "Noto Sans JP" on one line (Font0 draws it blank);
    a word longer than a line breaks after '/' or '-' (the URL, a long `git
    describe`). Paging keeps a group together when it fits an empty page, and
    everything goes on one page when it fits. Checked with a host build of the
    same calls rendered with the vendored Font0: StickS3 portrait 1 page in
    English (content to y = 233 of 240), 2 in Spanish; landscape 2 in both;
    T-Display 1 in both orientations. When paged, the footer says "1/2  tap:
    more".
  - *Input:* as Statistics: the side button returns to the settings home; a
    front tap turns the page (after the last, back to the first). Presses count
    for the dim and sleep timers as on every screen.
  - *Spanish header:* "ACERCA DE" and "lateral: atrás" (54 + 84 px) do not fit
    135 px, so on that screen the hint moves to the footer. Other Spanish
    headers (VELOCIDAD, ESTADISTICAS...) already overlap the hint in portrait;
    left to the UX refresh's hint bar.
- **The settings home scrolls in landscape.** 12 rows at the 10 px floor below
  the 20 px top need 140 px of 135. Squeezing the top margin would fit exactly
  once and never again, so the list shows 11 rows and follows the highlight as
  the Get decks list does, with a 2 px scroll bar in the right margin. Portrait
  (18 px steps) and the T-Display (12 px in landscape) still show all 12.
- **Portal footer.** The main page ends with one small `<footer>`:
  "Tangochō <version> · MIT-licensed firmware · Decks: CC BY-SA. Kanji data: …;
  stroke order: …; JLPT levels: …; glyphs: …" and the repository link, from
  the same Credits.h strings ("(c)" becomes ©). Two new PortalText strings in
  English and Spanish; a native test checks both templates take exactly the
  arguments passed.
- **Accepted gap:** a pack records no list of its sources, so the device shows
  these credits for any deck, including one built with another `--font` or
  from WaniKani. A pack-format field (e.g. an attribution string) would fix
  that; the published decks need none.
- **Flash:** StickS3 1,386,965 → 1,395,241 bytes (+8.1 KB: version 0.4 KB,
  About and TextLayout 5.9 KB, portal footer 1.8 KB), static RAM 54,148 →
  54,156 bytes. **Not verified on hardware.**

## 2026-10-18 – v0.1.0: how the release images are flashed

- **The merged image is flashed in DIO.** `pio run -t upload -v` writes these
  boards with `--flash_mode dio` (the app switches the flash to QIO itself);
  `merge_image.py` used QIO, so its default is now DIO to mirror the USB upload.
- **Correction:** the 2026-10-17 "Release artifacts" entry says PlatformIO
  omits `boot_app0.bin` for our table. It does not: its upload writes
  `boot_app0.bin` at `0xe000`, over the unused `phy_init` partition. The merged
  image leaves it out, which is what a table without OTA needs.
- **A merged image erases the settings.** `merge_bin` pads the gaps with
  `0xFF`, so writing it at `0x0` clears NVS (home Wi-Fi, preferences, time
  zone) up to the end of the app; LittleFS starts after the 2 MB app slot and
  keeps the decks and the study progress. Updates should therefore use the
  app-only image at `0x10000`; README and the release notes say so.
- **Browser path:** ESP Web Tools needs a page that serves the image and the
  manifest together, so "point a page at the release's manifest" (as the docs
  said) does not work. The documented browser path is Espressif's esptool-js
  flasher with the merged image at `0x0`; the manifests stay in the release for
  a future install page.
- **Verified on hardware** (v0.1.0-rc1, StickS3): the esptool path — the merged
  image booted, kept 30 SRS records and the decks, came up with no settings,
  opened setup mode with its QR and served the portal. Not yet tried: the
  browser path and the T-Display-S3 merged image.

## 2026-10-18 – First-boot language picker

- **Why.** The Language setting defaults to Auto (follow the loaded deck), so
  a brand-new device, which has no deck, showed its first screens (the setup
  screen's "Scan to join" and the rest) in English, and Settings cannot be
  reached before a deck is loaded. Agreed for v0.1.0.
- **When.** Once, at boot, when NVS `tangocho`/`ui` is **absent**
  (`Preferences::isKey()`: "never chosen" is not a stored Auto) **and**
  `/packs` holds no deck: a new device, or one whose flash was erased. Not
  after a merged-image reflash of a device that still has decks (the merge
  pads the NVS, the decks stay): Auto follows the deck there, as before. Not
  on any later boot, since the choice is stored. The rule is
  `firstBootLangPicker(prefStored, hasDecks)` in `ui/UiLang.h`, native-tested.
  Accepted side effect: a device that never stored the preference (Auto was the
  silent default) and whose decks were all deleted asks once on its next boot.
- **What.** `Screen::FirstLang`, in the choice-screen look (`drawRow()`, top
  40, 40 px step, shrunk above the hints in landscape): "English", preselected
  as the default that needs no browser to guess it, and "Español"; a neutral
  header "LANGUAGE / IDIOMA" made of both tables' `langTitle`; the button hints
  of both tables, each in its own language ("tap: other  hold: OK", "toque:
  otro  larga: OK"), one line each in landscape and on the T-Display, two each
  in the StickS3 portrait, where the Spanish line is 1 px too wide. Tap moves,
  hold chooses, the side button does nothing (nothing to go back to, as on the
  boot screen that follows). It runs in `loop()` like every screen, so presses
  count for the dim and sleep timers and Auto orientation turns it.
- **Then.** The row becomes an **explicit** preference
  (`firstBootLangPref()`: English or Spanish, never Auto), stored in `ui` and
  applied with `setUiForLang()`; the boot goes on with `openBootUploadScreen()`
  (the code that was inline in `setup()`): home mode with a saved network,
  else setup mode, now in that language. Explicit rather than Auto so that a
  Spanish speaker who downloads an English deck to learn the English meanings
  keeps Spanish screens. Settings › Language still offers Auto / English /
  Español. The portal page follows the setting as before: `portalUiLang()`
  returns it, it wins over `?lang=` and Accept-Language, and the page's
  English / Español links (shown only while the device has no language of its
  own) no longer appear; the page's catalog and Get decks list that language's
  decks first.
- **Order and sleep.** With no deck there is no date question, so the picker
  is the first screen that needs reading. The "Preparing storage..." notice of
  an erased data partition (a few seconds, no input) still comes first, in
  English: the device only knows it has no deck once the filesystem is mounted.
  The SRS session starts before the picker, as it did before the setup screen.
  A power-button sleep, the idle sleep on battery or a critical-battery sleep
  while the picker is open leaves `ui` unset, so the wake shows the picker
  again (no RTC flag needed); on USB the idle timeout darkens the panel and
  the next press redraws it.
- **Flash:** StickS3 1,395,225 → 1,396,205 bytes (+1.0 KB), static RAM 54,156
  → 54,164 bytes (both built with `TGC_VERSION=dev`). **Not verified on
  hardware.**


## 2026-10-18 – No back hint in the screen headers

- **Why.** Hardware test of the v0.1.0 candidate: in Spanish the right-aligned
  "lateral: atrás" overlapped header titles in the 135 px portrait (DESCARGAR,
  VELOCIDAD, ESTADISTICAS; About had already moved it to its footer). The side
  button is "back" on every settings screen, a global convention the README
  states, so the hint goes instead of being squeezed.
- **What.** `drawBackHint()` and the `backHint` strings are gone from every
  header (settings home, decks, pickers, statistics, About, Wi-Fi screens, Get
  decks, network test, date). About no longer keeps a footer copy: its footer
  is only the page number ("1/2  tap: more") when it needs two pages, and the
  freed line goes to the content. Screens where the side button does something
  else keep saying so in their own hints (the Wi-Fi screen's "side: start
  studying", and its "side: cancel" during a download from the page).
