# Roadmap

Scope for now: **kanji only** (no vocabulary decks).

- [x] **Phase 0 – Bring-up.** PlatformIO project for StickS3 + T-Display-S3,
      board HAL, LittleFS, read a pack, draw a kanji glyph, buttons.
      Validated on a StickS3 (2026-10-17).
- [x] **Phase 1 – Minimal quiz.** Pack builder from KANJIDIC2 (JLPT N5),
      kanji→meaning and kanji→reading questions with 4 options, random
      distractors, correct/incorrect feedback. Validated on a StickS3
      (2026-10-17).
- [~] **Phase 2 – SRS.** FSRS-5 on device (`firmware/src/srs/`, pure and
      native-tested), `/state/srs.bin` persisted per-codepoint state, review/
      learn queues, grade from correctness + response time. Implemented and
      committed; persistence and the session day clock verified on the StickS3.
      Pending: tune `slow_ms` and the daily new cap with real use, and the
      "study more" override check.
- [~] **Phase 3 – Better cards.** Distractors done for visual similarity and
      confusable readings (shared radicals pending); meaning→kanji questions
      done. Pending: detail card on mistakes, stroke order from KanjiVG, and
      preferring *common* readings — KANJIDIC2's reading order is not by
      frequency (e.g. 九's first kun reading is the rare ここの), so this needs a
      reading-frequency source or an on'yomi preference for some kanji.
      Invariant enforced: **no option may be another valid reading or meaning of
      the target** (otherwise two options are correct); checked in `Quiz.cpp`
      and in the builder's distractor lists.
- [ ] **Phase 4 – Connectivity.** Wi-Fi portal on device (upload packs, view
      stats), WaniKani importer (user's own token, unlocked kanji only), IMU
      input (tilt to choose). Also: sync NTP time while connected and store an
      epoch in the SRS state, so the scheduler finally has real days (the
      StickS3 has no RTC; see docs/decisions.md).
- [ ] **Phase 5 – Release.** README with photos, licences/attribution, GitHub
      releases, ESP Web Tools browser flasher, prebuilt packs.

## Cross-cutting

- [ ] **Statistics.** Per-kanji accuracy, current streak, review/seen counts and
      time spent; a small stats screen on device and richer views in the Wi-Fi
      portal. Depends on the SRS state (phase 2).
- [ ] **Multilingual.** Localise the on-device strings (currently English) and
      let the user pick the meaning language on device; packs already carry
      `lang`, and the builder supports en/es/fr/pt.
- [ ] **Menu and settings redesign.** The deck menu now also carries the date
      row, and more settings will follow (new/day, slow threshold, language).
      Reconsider navigation and give settings their own screen instead of adding
      rows to the deck list.
- [ ] **Landscape orientation.** The panel is natively portrait (135x240); a
      horizontal layout (240x135) gives much more room, especially for menus and
      the four options. Needs layout work: `main.cpp` computes everything from
      portrait `W x H` and the HAL returns portrait.

## Before publishing

- [ ] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
- [ ] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in README and on device).
