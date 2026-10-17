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
      learn queues, grade from correctness + response time. Validated on the
      StickS3. The device has no clock, so days come from a user-reported gap on
      each power-on plus a manual date (NTP in phase 4). `slow_ms`, the daily new
      cap and the "study more" override are all adjustable/working. Pending: tune
      the defaults with real use.
- [~] **Phase 3 – Better cards.** Distractors done for visual similarity and
      confusable readings (shared radicals pending); meaning→kanji questions
      done; the mistake detail card is done (a tap after a wrong answer opens a
      read-only card with the kanji, all meanings/readings, strokes and the
      card's own accuracy). Pending: stroke order from KanjiVG, and preferring
      *common* readings — KANJIDIC2's reading order is not by
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
      - **Download packs over Wi-Fi** from a URL (or a small catalog/JSON),
        with progress and resume, so a fresh device gets decks without
        `uploadfs` and without a computer.
      - **M5Launcher compatibility.** Ship the app-only `.bin` so it can be
        installed as an app (the launcher attaches its own SPIFFS), and provide
        a way to put packs into that launcher-managed filesystem — the Wi-Fi
        pack download above is the intended answer, since the launcher's SPIFFS
        starts empty and our `uploadfs` layout is not the one in use.
- [ ] **Phase 5 – Release.** README with photos, licences/attribution, GitHub
      releases, ESP Web Tools browser flasher, prebuilt packs. Host prebuilt
      packs (with a small catalog the device can read) so the Wi-Fi download has
      something to point at.

## Cross-cutting

- [~] **Statistics.** Per-kanji accuracy, current streak, review/seen counts and
      time spent; a small stats screen on device and richer views in the Wi-Fi
      portal. Depends on the SRS state (phase 2). On-device screen done:
      `SrsStore::stats()` + `Screen::Stats` show the date, seen/pack total,
      due/new, lifetime answers, accuracy and learned count. Pending: per-kanji
      accuracy, streak, time spent and the portal views.
- [~] **Multilingual.** On-device strings are localised to the pack language
      (`lang`: en/es today, anything else falls back to English). Pending: a
      language picker on device (the meaning language is a property of the pack,
      so switching packs already switches the meanings).
- [x] **Menu and settings redesign.** The settings home (Seguir / Mazos / Fecha /
      Nuevas/día / Velocidad) replaces the single deck list, with a shared
      four-row choice screen for the new/day cap and the Hard threshold. The deck
      list moved to its own screen and shows `due/new` for the loaded pack.
- [x] **Landscape orientation.** The panel is natively portrait (135x240); a
      horizontal layout (240x135) gives much more room, especially for menus and
      the four options. Selectable from the settings home and persisted in NVS
      (portrait is the default and is unchanged). Landscape puts the quiz prompt
      in a left column and the options on the right, and shrinks the list rows to
      fit the shorter screen.

## Before publishing

- [ ] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
- [ ] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in README and on device).
