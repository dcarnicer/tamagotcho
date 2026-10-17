# Roadmap

Scope for now: **kanji only** (no vocabulary decks).

- [x] **Phase 0 – Bring-up.** PlatformIO project for StickS3 + T-Display-S3,
      board HAL, LittleFS, read a pack, draw a kanji glyph, buttons.
      Validated on a StickS3 (2026-10-17).
- [x] **Phase 1 – Minimal quiz.** Pack builder from KANJIDIC2 (JLPT N5),
      kanji→meaning and kanji→reading questions with 4 options, random
      distractors, correct/incorrect feedback. Validated on a StickS3
      (2026-10-17).
- [ ] **Phase 2 – SRS.** FSRS on device, persisted state in LittleFS,
      review/learn queues, grade from correctness + response time.
- [ ] **Phase 3 – Better cards.** Smart distractors (visual similarity,
      shared radicals, confusable readings), meaning→kanji questions, detail
      card on mistakes, stroke order from KanjiVG.
      Also: prefer *common* readings — KANJIDIC2's reading order is not by
      frequency (e.g. 九's first kun reading is the rare ここの), so this needs a
      reading-frequency source or an on'yomi preference for some kanji.
- [ ] **Phase 4 – Connectivity.** Wi-Fi portal on device (upload packs, view
      stats), WaniKani importer (user's own token, unlocked kanji only), IMU
      input (tilt to choose).
- [ ] **Phase 5 – Release.** README with photos, licences/attribution, GitHub
      releases, ESP Web Tools browser flasher, prebuilt packs.

## Before publishing

- [ ] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
- [ ] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in README and on device).
