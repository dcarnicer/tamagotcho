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
      StickS3. The device has no RTC chip: days come from uptime plus the time
      spent in deep sleep (measured by the ESP32's RTC timer, 2026-10-18), and
      only a real power-off asks the user for the gap. `slow_ms`, the daily new
      cap and the "study more" override are all adjustable/working. Pending: tune
      the defaults with real use, and **better day detection**:
      - [ ] Make real power-offs rare: the M5PM1 power button can have its
        double-click shutdown and click reset disabled (`BTN_CFG_1/2`) and its
        clicks read (`BTN_STATUS`), so a press saves and deep-sleeps instead; a
        "Power off" settings row does the same; save and sleep at critical
        battery (~3 %) before the cell dies. Check on hardware that the PM1
        honours the registers.
      - [ ] Real date from the phone: the portal page posts `Date.now()` and the
        browser's time zone (`POST /time`); store an absolute epoch day in the
        SRS header's reserved bytes (v1-compatible) and roll the day over at a
        fixed local hour (04:00, as Anki) instead of every 24 h from an arbitrary
        origin.
      - [ ] Real date from NTP over the home Wi-Fi (phase 4) after a power cut,
        with a short timeout and the manual question as the fallback.
      - [ ] Better manual fallback: show the last-used date and step a proposed
        "today" (tap +1 day, hold confirms) instead of the 0/1/2-7/14+ buckets.
- [~] **Phase 3 – Better cards.** Distractors done for visual similarity,
      shared classical radicals and confusable readings; meaning→kanji questions
      done; the mistake detail card is done (a tap after a wrong answer opens a
      read-only card with the kanji, all meanings/readings, strokes and the
      card's own accuracy), and it now plays the kanji's **stroke order from
      KanjiVG** when the pack was built with `--strokes` (a v1-compatible
      optional pack section; packs without it behave exactly as before).
      Pending: preferring *common* readings — KANJIDIC2's reading order is not by
      frequency (e.g. 九's first kun reading is the rare ここの), so this needs a
      reading-frequency source or an on'yomi preference for some kanji.
      Invariant enforced: **no option may be another valid reading or meaning of
      the target** (otherwise two options are correct); checked in `Quiz.cpp`
      and in the builder's distractor lists.
- [~] **Phase 4 – Connectivity.** The on-device **Wi-Fi portal** is done: it
      serves a single self-contained page from a phone/laptop on its own open
      access point (`Tangocho-XXXX`, `http://192.168.4.1`) to upload, delete and
      download packs and to read the SRS statistics (`/`, `/api/stats`); it also
      lets a fresh, pack-less device boot into an "upload a `.tgc`" state and
      continue into the quiz once a pack arrives. Pending:
      - [ ] **Home Wi-Fi (station mode).** Enter the SSID/password once from the
        portal page (stored in NVS, never logged or shown back); the device then
        joins the home network on demand, not permanently, so the battery
        policy still holds. Used for NTP time (phase 2) and the catalog below.
      - [ ] **One-button deck download.** A "Get decks" screen fetches the
        release catalog (`tangocho-pack catalog`, line-oriented, phase 5) over
        the home Wi-Fi, lists the decks not yet on the device with their size,
        and downloads the chosen one into `/packs` with the existing
        temp-file + validate + rename path, checking the catalog's `sha256`.
        Needs **HTTPS**: GitHub release assets are `https://` with redirects,
        and the current fetch is plain `http://` only, so TLS (root CA bundle,
        redirect following, memory on the StickS3) has to be added and tested.
      - [x] **Auto-rotation from the IMU** (2026-10-18). Use the StickS3's BMI270
        accelerometer to switch portrait/landscape as the device is turned, with
        hysteresis and a short settle time so it does not flicker; the
        Orientation setting gains "Auto" next to the fixed choices. The
        T-Display-S3 has no IMU and keeps the fixed setting. Auto uses portrait
        and both landscape directions (pure `imu/OrientationFilter`). Pending:
        confirm the accelerometer axis mapping on hardware.
      - [ ] IMU input (tilt to choose).
      - [x] **WaniKani importer** (user's own token, unlocked kanji only). The
        builder can build a pack from the kanji a user has unlocked on their own
        WaniKani account instead of (or as well as) KANJIDIC2, via
        `tangocho-pack build --wanikani-token ... [--wanikani-level N]`. Nothing
        is shipped, nothing is fetched without the flag, and the token is never
        logged or cached; a WaniKani-derived pack is the user's own
        responsibility and is not redistributed (docs/data-sources.md).
      - [x] **Download packs over Wi-Fi** from a URL, so a fresh device gets
        decks without `uploadfs` and without a computer. Plain `http://` only
        (TLS to arbitrary hosts is untested); no resume yet.
      - [~] **M5Launcher compatibility.** The app-only `.bin` that
        `pio run -e sticks3` produces can be installed as a launcher app, and
        the Wi-Fi upload is the intended way to populate `/packs` once it runs
        (the launcher's SPIFFS starts empty and our `uploadfs` layout is not the
        one in use). Documented in README.md; **not verified on hardware**, and
        whether the launcher-created filesystem partition is large enough for
        packs is an open question.
- [~] **Phase 5 – Release.** Done so far: the app-only image and a **merged
      single-file image** per board plus an **ESP Web Tools `manifest.json`**,
      published by `.github/workflows/release.yml` on tags `v*` (and manual
      dispatch); CI (`.github/workflows/ci.yml`) runs the native tests and both
      board builds plus the pack-builder tests; a `tangocho-pack catalog` command
      that writes the small line-oriented catalog the device is meant to read;
      README with the licence/attribution and flashing instructions.
      Pending: **README photos**; the **device-side catalog fetch/parse** (the
      catalog format is defined and tested, the firmware does not read it yet);
      and hardware verification of the merged-image/ESP Web Tools path (written,
      **not flashed on a real device**). Prebuilt packs can be attached to a
      release, but only on request — redistributing CC BY-SA data is a
      maintainer decision (see "Before publishing").

## Cross-cutting

- [~] **Statistics.** Per-kanji accuracy, current streak, review/seen counts and
      time spent; a small stats screen on device and richer views in the Wi-Fi
      portal. Depends on the SRS state (phase 2). On-device screen done:
      `SrsStore::stats()` + `Screen::Stats` show the date, current/best streak,
      time on questions, seen/pack total, due/new, lifetime answers, accuracy and
      learned count. Single-card accuracy is on the mistake detail card. The
      Wi-Fi portal shows the same counters on its page and as JSON at
      `/api/stats`. Pending: an aggregate per-kanji accuracy view.
- [~] **Multilingual.** On-device strings are localised to the pack language
      (`lang`: en/es today, anything else falls back to English). The on-device
      language picker is done: a "Language"/"Idioma" setting chooses Auto (follow
      the pack, the default), English or Español, persisted in NVS (`ui`) and
      applied as a pure `uiLangCodeFor` helper. Pending: more languages (the
      meaning language is a property of the pack, so switching packs already
      switches the meanings).
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

- [~] **Power management** (2026-10-18). Done: dim after 30 s, deep sleep
      after 2 min on battery (front button wakes; panel-off only on USB), 5 min
      Wi-Fi portal timeout, CPU at 80 MHz (240 while the portal runs), a
      brightness setting. Pending: hardware checks (does the M5PM1 keep the rail
      up through deep sleep, sleep current, battery life), and the **USB buzz**:
      still audible up close after moving the backlight PWM to 25 kHz, so the
      backlight was not (or not the only) cause; to investigate.
- [ ] **UX refresh.** A review to give the device UI a more modern look and feel:
      typography and spacing, a consistent visual language across quiz, menus
      and detail card, clearer feedback and transitions on answers, a less
      text-heavy settings home (fewer rows or grouping), and an on-screen hint of
      what tap/hold do. Start with an audit and mockups before code.

## Before publishing

- [ ] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
      Not verified; left open.
- [~] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in
      README and on device). Progress: packs built with KANJIDIC2/KanjiVG are
      licensed CC BY-SA and the release workflow adds the combined attribution to
      the release notes automatically, but attaching them stays **opt-in** (a
      dispatch input) until this is a standing decision.
- [x] Attribution for redistributed data: README's "Releases" section,
      `docs/data-sources.md` and the release notes all carry the combined
      KANJIDIC2 / KanjiVG / Klee One credit. (Still open: an attribution line on
      the device itself.)
- [ ] Flash a **merged** image on a real StickS3 — ESP Web Tools in a browser and
      `esptool write_flash 0x0 …` — to confirm the documented offsets. The image
      and manifest are written and locally checked, **not verified on hardware**.
