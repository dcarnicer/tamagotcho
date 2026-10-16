# Roadmap

Scope for now: **kanji only** (no vocabulary decks).

- [~] **Phase 0 – Bring-up.** PlatformIO project for StickS3 + T-Display-S3,
      board HAL, LittleFS, read a pack, draw a kanji glyph, buttons.
      Code done, native tests pass, both envs build. **Pending: on-device check.**
- [~] **Phase 1 – Minimal quiz.** (builder done for N5 en/es; quiz loop written, untested on device) Pack builder from KANJIDIC2 (JLPT N5),
      kanji→meaning and kanji→reading questions with 4 options, random
      distractors, correct/incorrect feedback.
- [ ] **Phase 2 – SRS.** FSRS on device, persisted state in LittleFS,
      review/learn queues, grade from correctness + response time.
- [ ] **Phase 3 – Better cards.** Smart distractors (visual similarity,
      shared radicals, confusable readings), meaning→kanji questions, detail
      card on mistakes, stroke order from KanjiVG.
- [ ] **Phase 4 – Connectivity.** Wi-Fi portal on device (upload packs, view
      stats), WaniKani importer (user's own token, unlocked kanji only), IMU
      input (tilt to choose).
- [ ] **Phase 5 – Release.** README with photos, licences/attribution, GitHub
      releases, ESP Web Tools browser flasher, prebuilt packs.

## Before publishing

- [ ] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
- [ ] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in README and on device).
