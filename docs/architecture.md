# Architecture

```
 data sources              tools/pack-builder (Python)          device (firmware/)
 ─────────────             ──────────────────────────           ──────────────────
 KANJIDIC2 ─┐                                                   LittleFS
 KanjiVG ───┼──► importers ─► deck model ─► distractors ─┐      ├─ packs/*.tgc ──► PackReader
 WaniKani* ─┤                                            ├─► .tgc ──► upload ──►├─ state/srs.bin ──► SRS (FSRS)
 CSV/Anki* ─┘                 font (Klee One) ─► glyphs ─┘      └─ config         Quiz engine ─► UI
                                                                                  Board HAL (StickS3 / T-Display-S3)
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
| `board/`      | HAL: display handle, buttons, battery, sleep. One impl per board |
| `pack/`       | `PackReader`: parse header/tables, glyph lookup, string access, stroke masks. Pure C++, unit tested natively |
| `render/`     | Draw 4bpp glyphs, 1bpp stroke masks and UTF-8 text from pack glyphs onto a GFX canvas |
| `quiz/`       | Build a question (prompt + 4 options) from a card and its distractors. Pure C++ |
| `srs/`        | Scheduling (FSRS, phase 2) and persisted review state         |
| `ui/`         | Screens and input state machine                              |

Pure-logic modules (`pack`, `quiz`, `srs`) must build in the `native`
PlatformIO env with no Arduino headers.

## Target hardware

- **M5Stack StickS3** (primary): ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM,
  135×240 ST7789, buttons A/B, IMU, speaker. Via M5Unified/M5GFX.
- **LilyGO T-Display-S3** (dev stand-in): ESP32-S3R8, 16 MB flash, 170×320
  parallel panel, 2 buttons, no IMU. Via LovyanGFX.

UI layout is computed from the panel size, portrait orientation.
