# Architecture

```
 data sources              tools/pack-builder (Python)          device (firmware/)
 ─────────────             ──────────────────────────           ──────────────────
 KANJIDIC2 ─┐                                                   LittleFS
 KanjiVG ───┼──► importers ─► deck model ─► distractors ─┐      ├─ packs/*.tgc ──► PackReader
 WaniKani* ─┤                                            ├─► .tgc ──► upload ──►├─ state/srs.bin ──► SRS (FSRS)
 CSV/Anki* ─┘             font (Noto Sans JP) ─► glyphs ─┘      └─ config         Quiz engine ─► UI
                                                                                  Board HAL (StickS3 / T-Display-S3)
        phone/laptop ──(setup mode: open AP 192.168.4.1 | home mode: home Wi-Fi, http://<ip>/)──► Wi-Fi portal (HTTP) ──► /packs
        decks-v<format> release: catalog.txt + packs ──(home Wi-Fi, HTTPS, on demand)──► Get decks / portal page ──► /packs
 * later phases
```

## Principles

1. **Content and device are decoupled.** The device only understands the pack
   format (`docs/pack-format.md`). Every data source is an importer in the
   builder, so WaniKani is optional and no third-party content is shipped in
   the firmware.
2. **No font rasterisation on device.** The builder renders exactly the glyphs
   a deck needs (and, with `--strokes`, the KanjiVG stroke masks). Keeps
   firmware small and lets users pick the font. The firmware's own UI text
   (Latin-1 labels at 18 px, numerals at 32 px) is pre-rendered the same way
   by `tools/ui-font/` into a committed header, so the build needs no Python.
3. **Offline first.** Reviews never need a network. Wi-Fi is only for loading
   packs and (later) syncing.
4. **Multiple choice, done well.** The value of the app depends on distractor
   quality, so distractors are a first-class, precomputed part of the pack.

## Firmware modules (`firmware/src/`)

| module        | role                                                         |
|---------------|--------------------------------------------------------------|
| `board/`      | HAL: display handle and rotation, buttons, accelerometer, battery, sleep. One impl per board |
| `pack/`       | `PackReader`: parse header/tables (pack v1 and v2), glyph lookup, string access, stroke masks; `Rle` decodes v2's compressed glyph bitmaps and stroke masks on read, through a small window over the file. Pure C++, unit tested natively |
| `render/`     | Draw 4bpp glyphs, 1bpp stroke masks and UTF-8 text onto a GFX canvas: pack glyphs, the built-in Font0 (captions) and the UI font (`Renderer::kTextUi` / `kTextUiNumeral`, no pack needed), with `drawLabel` (one line, cap-height centred, ellipsis). `Widgets`: the flat UX components (header, hint bar, option tile, list row, pill, progress bar, a scrolling list drawer) and `presentRegion` (a partial push: `setClipRect` + `pushSprite`) |
| `quiz/`       | Build a question (prompt + 4 options) from a card and its distractors. Pure C++ |
| `catalog/`    | Parse the line-oriented deck catalog (`tangocho-pack catalog`): version check, per-record validation (safe file name, numbers, SHA-256, URL), bare-file-name URLs resolved against the catalog URL, entry cap; own-language ordering and a deck's state against `/packs` (new / on device / update). Pure C++, unit tested natively |
| `portal/`     | The Wi-Fi portal's pure helpers: the `WIFI:` QR text for the open access point (escaping), the QR version for a text, the page language (device UI language, else the browser's Accept-Language) and the deck-list language, RSSI quality words; and every page string in English and Spanish (`PortalText`). Pure C++, unit tested natively |
| `srs/`        | Scheduling (FSRS, phase 2) and persisted review state; the day clock and the real date (`WallClock`: the 04:00 study day); time zones (`TimeZone`: POSIX TZ rules with daylight saving time, the curated zone list), so study days are 23/25 h on a change day. Pure C++, unit tested natively |
| `imu/`        | `OrientationFilter`: accelerometer samples to a stable display rotation for the Auto orientation (hysteresis, settle time, flat and motion rejection). Pure C++, unit tested natively |
| `power/`      | Critical-battery guard (`BatteryGuard.h`, header-only). Pure C++, unit tested natively |
| `platform/`   | LittleFS-backed pack source and SRS state file; the on-device **Wi-Fi portal** (`WifiPortal`: an HTTP server in *setup mode* on its own open access point, or in *home mode* on the home network the app joined) with one task-oriented page: the deck catalog and its downloads (home mode, through the app), the decks on the device, and under Advanced uploading, URL downloads, the catalog URL, the home Wi-Fi and the statistics (the page also posts the phone's clock and time zone as a POSIX rule, `POST /time`, which anchors the SRS date); `HomeWifi` (home network in NVS, station mode on demand, link/IP/RSSI, SNTP time for the SRS date); `NetFetch` (atomic HTTP(S) pack download with size/SHA-256 checks and the catalog read, shared by the portal, Get decks and the network test) with the embedded root CAs in `RootCerts.h`. Arduino only |
| `ui/`         | Screens and input state machine. Header-only pure helpers, unit tested natively: `UiLang.h` (the UI language, and when the first boot asks for it), `TextLayout.h` (line wrapping and paging for the built-in font, used by Settings › About), `Theme.h` (the UX refresh's RGB565 colour tokens, spacing and per-panel layout grid), `Icons.h` (1bpp icons, art strings packed at compile time), `ListView.h` (a scrolling list's focus, window, markers and scroll thumb), `UiFace.h` + the generated `UiFont.h` (the firmware's UI font: Noto Sans JP glyph tables in the pack 4bpp format, from `tools/ui-font/gen_ui_font.py`) |
| `Version.*`, `Credits.h` | `tgc::firmwareVersion()`: the release tag, `git describe` or `"dev"`, defined for `Version.cpp` only by `scripts/version.py`; the attribution strings Settings › About and the portal footer share (docs/data-sources.md) |

Pure-logic modules (`pack`, `quiz`, `srs`, `imu`, `power`, `catalog`, `portal`, and the
helpers in `ui`) must build in the `native` PlatformIO env with no Arduino headers.

The menu screens follow the UX refresh's design system (docs/ux-refresh.md
§2, §6): a 16 px header, rows of the layout's height that scroll through a
`ListView` instead of shrinking, and a 12 px hint bar (• tap, ▬ hold, the side
button's icon). The quiz, the detail card and the other screens still use the
older drawing until their phases (§4).

## Target hardware

- **M5Stack StickS3** (primary): ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM,
  135×240 ST7789, buttons A/B, IMU, speaker. Via M5Unified/M5GFX.
- **LilyGO T-Display-S3** (dev stand-in): ESP32-S3R8, 16 MB flash, 170×320
  parallel panel, 2 buttons, no IMU. Via LovyanGFX.

UI layout is computed from the panel size (`W` x `H`), so every screen has a
portrait and a landscape form. The Orientation setting is Portrait (rotation
0, default), Landscape (rotation 1) or, on boards with an accelerometer, Auto:
`board::readAccel()` gives gravity in the panel frame, `imu/OrientationFilter`
turns it into rotation 0, 1 or 3, and the app applies a settled change only
while no button is down (docs/decisions.md, 2026-10-18).
