# Tangochō 単語帳

Kanji flashcards on your wrist-sized ESP32. Tangochō turns an
[M5Stack StickS3](https://docs.m5stack.com/) into a ring-bound kanji card deck
with **multiple-choice reviews** and spaced repetition, fully offline.

> Status: early development. See [docs/roadmap.md](docs/roadmap.md).

## How it works

1. `tools/pack-builder` builds a **pack** (`.tgc`) from open data
   (KANJIDIC2), choosing a level (e.g. JLPT N5), meaning language and font.
   All glyphs are pre-rendered, so the device needs no Japanese font.
2. Upload the pack to the device.
3. Review: a kanji appears, pick the right meaning or reading from four
   options. Distractors are chosen to be genuinely confusable. A wrong answer
   opens a detail card that shows all meanings and readings — and animates the
   kanji's stroke order from KanjiVG, if the pack was built with `--strokes`.

## Repository layout

| path                  | what                                    |
|-----------------------|-----------------------------------------|
| `firmware/`           | PlatformIO firmware (StickS3, T-Display-S3) |
| `tools/pack-builder/` | Python CLI that builds `.tgc` packs     |
| `docs/`               | architecture, pack format, decisions, roadmap |

## Licence

Code: MIT. Generated packs contain data from KANJIDIC2 (© EDRDG, CC BY-SA 4.0)
and glyphs from the selected font (default Klee One, SIL OFL 1.1). See
[docs/data-sources.md](docs/data-sources.md).
