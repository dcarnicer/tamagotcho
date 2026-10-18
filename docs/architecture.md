# Architecture

```
 data sources              tools/pack-builder (Python)          device (firmware/)
 ─────────────             ──────────────────────────           ──────────────────
 KANJIDIC2 ─┐                                                   LittleFS
 KanjiVG ───┼──► importers ─► deck model ─► distractors ─┐      ├─ packs/*.tgc ──► PackReader
 WaniKani* ─┤                                            ├─► .tgc ──► upload ──►├─ state/srs.bin ──► SRS (FSRS)
 CSV/Anki* ─┘                 font (Klee One) ─► glyphs ─┘      └─ config         Quiz engine ─► UI
                                                                                  Board HAL (StickS3 / T-Display-S3)
        phone/laptop ──(open AP 192.168.4.1)──► Wi-Fi portal (HTTP) ──► /packs
        release catalog.txt + packs ──(home Wi-Fi, HTTPS, on demand)──► Get decks ──► /packs
 * later phases
```

## Principles

1. **Content and device are decoupled.** The device only understands the pack
   format (`docs/pack-format.md`). Every data source is an importer in the
   builder, so WaniKani is optional and no third-party content is shipped in
   the firmware.
2. **No font rasterisation on device.** The builder renders exactly the glyphs
   a deck needs (and, with `--strokes`, the KanjiVG stroke masks). Keeps
   firmware small and lets users pick the font.
3. **Offline first.** Reviews never need a network. Wi-Fi is only for loading
   packs and (later) syncing.
4. **Multiple choice, done well.** The value of the app depends on distractor
   quality, so distractors are a first-class, precomputed part of the pack.

## Firmware modules (`firmware/src/`)

| module        | role                                                         |
|---------------|--------------------------------------------------------------|
| `board/`      | HAL: display handle and rotation, buttons, accelerometer, battery, sleep. One impl per board |
| `pack/`       | `PackReader`: parse header/tables, glyph lookup, string access, stroke masks. Pure C++, unit tested natively |
| `render/`     | Draw 4bpp glyphs, 1bpp stroke masks and UTF-8 text from pack glyphs onto a GFX canvas |
| `quiz/`       | Build a question (prompt + 4 options) from a card and its distractors. Pure C++ |
| `catalog/`    | Parse the line-oriented deck catalog (`tangocho-pack catalog`): version check, per-record validation (safe file name, numbers, SHA-256, URL), bare-file-name URLs resolved against the catalog URL, entry cap. Pure C++, unit tested natively |
| `srs/`        | Scheduling (FSRS, phase 2) and persisted review state         |
| `imu/`        | `OrientationFilter`: accelerometer samples to a stable display rotation for the Auto orientation (hysteresis, settle time, flat and motion rejection). Pure C++, unit tested natively |
| `power/`      | Critical-battery guard (`BatteryGuard.h`, header-only). Pure C++, unit tested natively |
| `platform/`   | LittleFS-backed pack source and SRS state file; the on-device **Wi-Fi portal** (access point + HTTP server) for uploading, deleting and downloading packs, reading the statistics and saving the home Wi-Fi (its page also posts the phone's clock, `POST /time`, which anchors the SRS date); `HomeWifi` (home network in NVS, station mode on demand, SNTP helper); `NetFetch` (atomic HTTP(S) pack download with size/SHA-256 checks, shared by the portal and Get decks) with the embedded root CAs in `RootCerts.h`. Arduino only |
| `ui/`         | Screens and input state machine                              |

Pure-logic modules (`pack`, `quiz`, `srs`, `imu`, `power`, `catalog`) must build
in the `native` PlatformIO env with no Arduino headers.

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
