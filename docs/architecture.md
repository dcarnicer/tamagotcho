# Architecture

```
 data sources              tools/pack-builder (Python)          device (firmware/)
 ─────────────             ──────────────────────────           ──────────────────
 KANJIDIC2 ─┐                                                   LittleFS
 KanjiVG ───┼──► importers ─► deck model ─► distractors ─┐      ├─ packs/*.tgc ──► PackReader
 WaniKani* ─┤                                            ├─► .tgc ──► upload ──►├─ state/srs.bin ──► SRS (FSRS)
 CSV/Anki* ─┘             font (Noto Sans JP) ─► glyphs ─┘      └─ config         Quiz engine ─► UI
                                                                                  Board HAL (StickS3 / T-Display-S3 / Cardputer ADV)
        phone/laptop ──(setup mode: open AP 192.168.4.1 | home mode: home Wi-Fi, http://<ip>/)──► Wi-Fi portal (HTTP) ──► /packs
        decks-v<format> release: catalog-v2.txt + packs ──(home Wi-Fi, HTTPS: Get decks, portal, boot check)──► Get decks / portal page ──► /packs
 * later phases
```

The boot-time check is the only automatic catalog read (docs/deck-updates.md):
it runs once a day on external power, or once a week on battery from 50 %.

## Principles

1. **Content and device are decoupled.** The device only understands the pack
   format (`docs/pack-format.md`). Every data source is an importer in the
   builder, so WaniKani is optional and no third-party content is shipped in
   the firmware.
2. **No font rasterisation on device.** The builder renders exactly the glyphs
   a deck needs (and, with `--strokes`, the KanjiVG stroke masks). Keeps
   firmware small and lets users pick the font. The firmware's own UI text
   (Latin-1 labels at 18 px, numerals at 32 px, and the app's name,
   たまごっちょ, in kana) is pre-rendered the same way by `tools/ui-font/` into
   a committed header, so the build needs no Python.
3. **Offline first.** Reviews never need a network. Wi-Fi is only for loading
   packs and (later) syncing.
4. **Multiple choice, done well.** The value of the app depends on distractor
   quality, so distractors are a first-class, precomputed part of the pack.

## Firmware modules (`firmware/src/`)

| module        | role                                                         |
|---------------|--------------------------------------------------------------|
| `board/`      | HAL: display handle and rotation, buttons, accelerometer, battery, sleep; the board's capabilities (`fixedLandscape()`, `hasKeyboard()`) and its key events (`nextKeyEvent()`; the Cardputer ADV's TCA8418 driver). One impl per board |
| `pack/`       | `PackReader`: parse header/tables (pack v1 to v3), glyph lookup, string access, stroke masks, and a v3 pack's example words (`exampleWords()`: word, reading, gloss and the reading span its kanji reads; their strings in the pool, their glyphs in set 2), read on demand and validated at open; `Rle` decodes v2's compressed glyph bitmaps and stroke masks on read, through a small window over the file; `peekPackInfo()` reads a pack's deck name, language and card count without opening it (the deck list); `readPackCodepoints()` reads only its cards' code points (deleting a deck's progress). The three tables either in memory or, without PSRAM, read on demand through small caches (`PackMemory::Streamed`, "Memory without PSRAM" below). Pure C++, unit tested natively in both modes |
| `render/`     | Draw 4bpp glyphs, 1bpp stroke masks and UTF-8 text onto a GFX canvas (decoded small glyphs cached: without a limit in PSRAM, within a byte budget otherwise): pack glyphs, the built-in Font0 (captions) and the UI font (`Renderer::kTextUi` / `kTextUiNumeral` / `kTextUiKana`, no pack needed), with `drawLabel` (one line, cap-height centred, ellipsis). `Widgets`: the flat UX components (header, hint bar, option tile with a label or a kanji, list row, pill, progress bar, a scrolling list drawer), the press fill of hold progress, and `presentRegion` (a partial push: `setClipRect` + `pushSprite`) |
| `quiz/`       | Build a question (prompt + 4 options) from a card and its distractors, on'yomi options in katakana or hiragana (`restyleReadings()` rewrites the one on screen when the setting changes); `ReadingTurn`, the on/kun turn of reading questions. Pure C++ |
| `catalog/`    | Parse the line-oriented deck catalog (`tamagotcho-pack catalog`, format v2 only, `catalog-v2.txt`): version check, per-record validation (safe file name, deck title as UTF-8, numbers, SHA-256, URL), bare-file-name URLs resolved against the catalog URL, entry cap; own-language ordering and a deck's state against `/packs` (new / on device / update). Pure C++, unit tested natively |
| `portal/`     | The Wi-Fi portal's pure helpers: the `WIFI:` QR text for the open access point (escaping), the QR version for a text, the page language (device UI language, else the browser's Accept-Language) and the deck-list language, RSSI quality words; and every page string in English and Spanish (`PortalText`). Pure C++, unit tested natively |
| `srs/`        | Scheduling (FSRS, phase 2) and persisted review state, and which records go when a deck is deleted or its progress reset (`DeckProgress`, docs/srs.md §2.1); the day clock and the real date (`WallClock`: the 04:00 study day); time zones (`TimeZone`: POSIX TZ rules with daylight saving time, the curated zone list), so study days are 23/25 h on a change day. Pure C++, unit tested natively |
| `imu/`        | `OrientationFilter`: accelerometer samples to a stable display rotation for the Auto orientation (hysteresis, settle time, flat and motion rejection). Pure C++, unit tested natively |
| `power/`      | Critical-battery guard (`BatteryGuard.h`, header-only). Pure C++, unit tested natively |
| `platform/`   | LittleFS-backed pack source and SRS state file; the on-device **Wi-Fi portal** (`WifiPortal`: an HTTP server in *setup mode* on its own open access point, or in *home mode* on the home network the app joined) with one task-oriented page: the deck catalog and its downloads (home mode, through the app), the decks on the device (deleting one deletes its progress too, through the app: `Hooks::onDeleteDeck`), and under Advanced uploading, URL downloads (either one of the loaded deck's file name has the app close that deck first, `Hooks::onBeforeReplace`, since LittleFS neither removes nor renames over an open file), the catalog URL, the home Wi-Fi and the statistics (the page also posts the phone's clock and time zone as a POSIX rule, `POST /time`, which anchors the SRS date); `HomeWifi` (home network in NVS, station mode on demand, link/IP/RSSI, SNTP time for the SRS date); `NetFetch` (atomic HTTP(S) pack download with size/SHA-256 checks and the catalog read, shared by the portal, Get decks and the network test) with the embedded root CAs in `RootCerts.h`. Arduino only |
| `ui/`         | The screens' pure logic (the screens themselves are in `app/`, below). Header-only pure helpers, unit tested natively: `UiLang.h` (the UI language, and when the first boot asks for it), `TextLayout.h` (line wrapping and paging for the built-in font, used by Settings › Device › About and the captions), `Theme.h` (the UX refresh's RGB565 colour tokens, spacing and per-panel layout grid), `Icons.h` (1bpp icons, art strings packed at compile time), `ListView.h` (a scrolling list's focus, window, markers and scroll thumb), `HintBar.h` (the hint bar's layout: caption widths, the front and side groups, and the second row a screen with a side tap and a side hold needs when they do not fit one; on a keyboard board the marks are keycaps), `KeyInput.h` (keyboard input: the Cardputer ADV's TCA8418 key numbers on its key grid, the keys the app tells apart, their five actions and the arrows' auto-repeat), `QuizUi.h` (the quiz screens' logic: session progress, option navigation, the hint bar policy, hold-progress and auto-advance timing, the result pill within its pixel budget, the detail card's reading rows, and the intro card's layout: meanings, reading rows with `+N`, example words, page 1 and page 2 per panel), `UiFace.h` + the generated `UiFont.h` (the firmware's UI font: Noto Sans JP glyph tables in the pack 4bpp format, from `tools/ui-font/gen_ui_font.py`), `SettingsTree.h` (the settings home's five groups and the Study / Device pages, Study › Introduce among them as item tables, chevrons, icons and row values; a landscape-only board's Device page has no Orientation row), `ScreenLayout.h` (where the summary card, the stats ring and tiles, the calendar tile and the done screen go on each panel), `DeckSummary.h` (the loaded deck's total / new / due today and tomorrow), `InputPrefs.h` (auto-advance and hold-time choices) |
| `app/`        | The app: every screen, the quiz loop, the settings, Get decks and the deck updates, the Wi-Fi screen and the portal's hooks, the clock, the boot and the power handling, one file pair per screen or area ("The app layer" below). Arduino only |
| `main.cpp`    | The Arduino entry points: `setup()` (runs `app::boot()`), `loop()` (the buttons' tap and hold events) and the two screen dispatchers, `drawScreen()` and `handleInput()` |
| `Version.*`, `Credits.h` | `tgc::firmwareVersion()`: the release tag, `git describe` or `"dev"`, defined for `Version.cpp` only by `scripts/version.py`; the attribution strings Settings › Device › About and the portal footer share (docs/data-sources.md), and the app's name (`kAppName` たまごっちょ on screen, `kAppNameLatin` Tamagotcho for the serial log, SSID and mDNS host) |

Pure-logic modules (`pack`, `quiz`, `srs`, `imu`, `power`, `catalog`, `portal`, and the
helpers in `ui`) must build in the `native` PlatformIO env with no Arduino headers.

### The app layer (`firmware/src/app/`)

Until 2026-10-23 all of this was `main.cpp` (6,832 lines, docs/decisions.md).
Each file pair below is one screen or one area, in namespace `tgc::app`. What
other files use is declared in the module's header; the rest of a `.cpp` is in
an anonymous namespace.

| file          | owns |
|---------------|------|
| `App`         | The shared context: the open deck (`gPack`, its file `gSource`, `gRender`), `gRng`, the SRS store, its file and the queue (`gSrs`, `gSrsIo`, `gQueue`, `gDirty`, `gSrsLoaded`), the `Screen` enum with `gScreen` and `gBeforeMenu`, the NVS namespace (`gPrefs`), `QLOG`, and the declarations of `drawScreen()` / `handleInput()` (defined in `main.cpp`) |
| `UiStrings`   | `UiStrings` (every on-device string), the English and Spanish tables, `ui()`, `keysOr()`, the UI-language preference |
| `Display`     | The back buffer (`gCanvas`, `W`, `H`), `present()` / `presentRegion()`, a list's focus move and partial push, the rotation and the Auto orientation |
| `ScreenKit`   | What the screens are drawn from: the header, the hint bar in the board's terms (`boardHints()`), caption lines, a list's geometry and note line, the error and notice screens |
| `Quiz`        | The next queue item (`nextItem()`: a question, an intro card or the done screen; `showNextItem()`, `refreshAfterRebuild()`), the quiz screen and its geometry and tiles, the answer and its grade, `quizLoop()`, the "study more" override |
| `Detail`      | The detail card after a wrong answer |
| `KanjiCard`   | The parts the detail card and the intro share, keyed to a card index: the kanji box with the stroke-order animation (`gAnim`), the header's stroke counter, the reading rows |
| `Intro`       | Learning mode's intro card before a never-seen kanji's first question (docs/learning-mode.md): its two pages, "go on" (`SrsStore::introduce()`, `Queue::introduced()`) and "known" (`Queue::known()`), the 400 ms press guard |
| `Done`        | "All caught up" and "+10 new" |
| `Settings`    | The settings tree: the home with the summary card, the Study and Device pages, the `MenuItem` table (one entry per `ui/SettingsTree.h` item: label, value, action), the settings' values |
| `Choice`      | The pickers (`Screen::Choice`) and applying a choice |
| `Stats`, `About` | Statistics; Settings › Device › About |
| `Decks`       | `/packs` and the loaded deck (`loadPack()`, `portalPacksChanged()`), its counts (`deckCounts()`), the deck list |
| `GetDecks`    | Get decks and the updates list |
| `DeckUpdates` | The update count, the automatic check's policy and its boot screen |
| `Net`         | What the Wi-Fi sessions share: the station (`gHome`), the radio off, why a join failed, a catalog deck's state on disk, why a catalog read failed, the checked deck download and its progress |
| `Wifi`        | The Wi-Fi screen, the portal's two modes, the boot-time upload screen's portal, the portal's hooks (`kPortalHooks`) |
| `WifiMenu`    | Settings › Wi-Fi and the home network test |
| `Progress`    | A deck deleted from the portal with its progress, a restore from the portal, Settings › Study › Delete progress |
| `Clock`       | The SRS day as a date, the time zone, a real time from the phone or NTP, the boot-time network date |
| `DateScreens` | The date stepper, Settings › Device › Date and the zone list |
| `Power`       | The dim, the idle and explicit sleep, the wake, the critical-battery guard, the time slept, the CPU clocks, the battery test log |
| `Input`       | The hold progress and the keyboard (`keyAction()`, `keyTurn()`) |
| `Remote`      | The serial remote of the `-shots` envs (`TGC_REMOTE`) |
| `Boot`        | `boot()` (what `setup()` runs), the filesystem mount and format, the first-boot language picker, the boot-time upload screen |

**Where new code goes.** A new screen is a `Screen` value in `App.h`, its own
`app/X.h` / `app/X.cpp` with its state, its draw function and its loop function
(`void xLoop(int a, int b)`: the front and side buttons' events, 1 tap, 2
hold), and one line in each of `drawScreen()` and `handleInput()` in
`main.cpp`. A list screen, or one in the settings tree, also gets its cases in
`app/Input.cpp` (`listFocusUp()`, `keyRepeatsHere()`, `menuKeyGoesHome()`).
Its strings go in `UiStrings` (the struct and both tables); a settings row is a
`ui/SettingsTree.h` item plus its `MenuItem` entry in `app/Settings.cpp`. Logic
that needs no Arduino and can be tested natively goes in a pure module instead
(`ui/`, `srs/`, ...). Code shared by two screens goes in the module that owns
the concept (`Net`, `Clock`, `Decks`), not in `main.cpp`.

The menu and secondary screens follow the UX refresh's design system
(docs/ux-refresh.md §2, §6): a 16 px header, rows of the layout's height that
scroll through a `ListView` instead of shrinking, and a 12 px hint bar (• tap,
▬ hold, the side button's icon; a second row for the side button where its tap
and hold both need a word and the panel is too narrow, as on the date stepper). The settings are a tree (§3): a home with a
summary card and five groups, two of which (Study, Device) are pages of rows;
`app/Settings.cpp` maps each `SettingsTree.h` item to its label, value and
action in one table. The quiz and the detail card follow it too (phase 2: header pills,
flat answer tiles, hold progress, auto-advance).

## Target hardware

- **M5Stack StickS3** (primary): ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM,
  135×240 ST7789, buttons A/B, IMU, speaker. Via M5Unified/M5GFX.
- **LilyGO T-Display-S3** (dev stand-in): ESP32-S3R8, 16 MB flash, 170×320
  parallel panel, 2 buttons, no IMU. Via LovyanGFX.
- **M5Stack Cardputer ADV** (2026-10-21, not verified on hardware yet):
  Stamp-S3A (ESP32-S3FN8), 8 MB flash, **no PSRAM**, 1.14" ST7789V2 used
  landscape only (240×135, rotation 1, over the keyboard), 56-key keyboard
  behind a TCA8418 on the internal I2C bus (INT on G11), the G0 button,
  battery on G10, a power switch. Via M5Unified/M5GFX (autodetected) and a
  small TCA8418 driver in `board/BoardCardputerAdv.cpp`.

UI layout is computed from the panel size (`W` x `H`), so every screen has a
portrait and a landscape form. The Orientation setting is Portrait (rotation
0, default), Landscape (rotation 1) or, on boards with an accelerometer, Auto:
`board::readAccel()` gives gravity in the panel frame, `imu/OrientationFilter`
turns it into rotation 0, 1 or 3, and the app applies a settled change only
while no button is down (docs/decisions.md, 2026-10-18). A board whose
`board::fixedLandscape()` is true (the Cardputer ADV) has no Orientation row
and is always in rotation 1.

**Input.** Every board has the front button (`Button::Select`, tap and hold);
the StickS3 and the T-Display-S3 also the side button (`Button::Next`).
`loop()` turns them into tap/hold events and hands them to the screen's loop
(`handleInput()`). A board with a keyboard (`board::hasKeyboard()`) also
delivers key events (`board::nextKeyEvent()`); `ui/KeyInput.h` maps each key
to one of five actions (next, previous, confirm, back, menu) and `keyAction()`
in `app/Input.cpp` is the one place that turns an action into the screen's own:
next is the front tap, confirm (Enter) the front hold at once, previous the
side tap where that means previous or a list's focus up, back the screen's
back or cancel, menu the settings home. The hint bar then names keys
(keycaps) instead of the buttons' marks (docs/decisions.md, 2026-10-21).

## Memory without PSRAM

The StickS3 and the T-Display-S3 keep the big buffers in PSRAM: the canvas,
the packs' card table, glyph table and string pool, the renderer's glyph
cache, the SRS table (and, by the framework's settings, Wi-Fi's buffers and
every allocation over 4 KB). The Cardputer ADV has only the ESP32-S3's
internal SRAM, which Wi-Fi and TLS need too. Numbers of 2026-10-21 (the
`cardputer-adv` build and the twelve published decks; the heap figures are an
estimate until the device logs them):

- **Static:** `.dram0.data` 23,692 B + `.dram0.bss` 31,392 B = 55,084 B;
  IRAM code 75,838 B shares the same SRAM (the heap starts 5 KB lower than
  the StickS3's, whose IRAM has the PSRAM workarounds).
- **Fixed:** the canvas, 240 × 135 × 2 = 64,800 B; the loop task's 16 KB stack.
- **Per deck** (English decks; Spanish ones within 4 KB). In memory: the three
  tables plus the renderer's pointer per glyph. Streamed
  (`PackMemory::Streamed`): two card blocks, six glyph blocks, sixteen string
  slots, 4 bytes a card and 8 bytes per 32 glyphs (ESP32 sizes, after every
  card's text was read once):

  | deck  | cards | glyphs | in memory       | streamed |
  |-------|------:|-------:|----------------:|---------:|
  | N5    |    79 |    474 |  14.3 + 1.9 KB  |   6.9 KB |
  | N4    |   166 |    669 |  24.4 + 2.7 KB  |   7.5 KB |
  | N3    |   367 |  1,101 |  49.8 + 4.4 KB  |   8.6 KB |
  | N2    |   367 |  1,088 |  45.2 + 4.4 KB  |   8.3 KB |
  | N1    | 1,232 |  2,833 | 130.8 + 11.3 KB |  12.3 KB |
  | Jōyō  | 2,136 |  4,647 | 232.4 + 18.6 KB |  17.0 KB |

  The renderer's cache of decoded small glyphs has no limit with PSRAM (every
  small kanji of a Jōyō deck drawn once would be 1.5 MB); without it a 16 KB
  budget, plus 1.2 KB of slots and the 4.6 KB set-0 scratch. The queue holds
  2 bytes a card and its items; a rebuild needs 12 bytes per due or new card
  for a moment (reserved exactly, not grown by doubling). The SRS table is 16
  bytes a record (34 KB for all of Jōyō), grown 64 records at a time; a save
  serializes it into a second buffer of the same size for a moment.
  Distractor lists are read on demand; the deck summary needs nothing.
  A v3 deck with example words (docs/pack-format.md) costs streamed about
  0.5 KB more (the glyph index of the words' set-2 kanji); in memory its
  glyph table and string pool are larger (Jōyō: about +34 KB of glyph entries
  and +74 KB of strings, plus the renderer's pointer per glyph). The words
  themselves are read on demand, nothing is kept.
- **Wi-Fi and TLS.** Measured on the StickS3 (2026-10-19): 248 KB of internal
  heap free with the home Wi-Fi up, a TLS session about 45 KB (164 KB largest
  block during it). Without PSRAM, Wi-Fi's static buffers (8 RX and 8 TX of
  about 1.6 KB) and the packets in flight come from the internal heap too:
  about 35 KB more, so some 220 KB free with Wi-Fi up before the app's own.
- **Budget, a Get decks download** (the worst moment: Wi-Fi, TLS and a deck
  loaded): 220 − 65 (canvas) − 7..17 (deck, streamed) − 22 (renderer) −
  5..34 (SRS, 300 to 2,136 records) − 5 (queue) − 45 (TLS) leaves about 30 to
  70 KB. With the tables in memory N1 (142 KB) and Jōyō (251 KB) would not
  even open next to the canvas and Wi-Fi, and N2-N3 (about 54 KB) would leave
  25-65 KB with TLS before a download's probe opens a second copy. So a board
  without PSRAM always opens packs streamed (`platform/FsPackSource.h`
  `packMemory()`, also for that probe); a board with PSRAM keeps them in
  memory and streams only if they ever do not fit.

What to watch on the device: the `pack ...: tables streamed (N bytes);
internal heap F free, L largest` line after a deck loads and the
`[net] before TLS` / `after TLS connect` / `home portal up` lines (free,
largest block, minimum since boot).
