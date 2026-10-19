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
      - [~] Make real power-offs rare (docs/decisions.md, 2026-10-18).
        - [x] The M5PM1's click reset is disabled (`BTN_CFG_1`) and its press
          read from `BTN_STATUS`. A click saves and deep-sleeps instead of
          power-cycling the chip. Any PM1 check or I2C failure keeps the
          factory button.
        - [x] A "Sleep"/"Suspender" settings row does the same (also on USB).
        - [x] Save and sleep at critical battery (3 %, three readings, resume
          only at 6 % or on USB).
        - Deliberately **not** done: the double-click shutdown stays enabled.
          A long press only enters download mode and the PM1 keeps its button
          config through resets, so the double click is the only hardware
          escape hatch.
        - [ ] Hardware checks: the PM1 honours `SINGLE_RESET_DIS` and reports
          the press; a click still powers on a device that is off; a double
          click still leaves download mode; sleep notice layout; battery
          thresholds on a real discharge.
        - [ ] Maybe: wake from deep sleep with the power button through the
          PM1 IRQ output (PM1 G1 is wired to ESP32 G13). Held back because the
          datasheet says the PM1 LED then blinks while the click reset is off.
      - [x] Real date from the phone (2026-10-18): opening the portal page posts
        the browser's clock (`POST /time`: `Date.now()` and the UTC offset); the
        SRS header stores an absolute anchor (epoch day of SRS day 0, byte 38,
        v1-compatible) and the day rolls over at 04:00 local, as Anki, instead of
        every 24 h from an arbitrary origin. The statistics screen, the portal
        page and `/api/stats` show the real date. `applyWallClock()` is the entry
        point the NTP item below reuses; the offset is kept in NVS (`tz`).
        Native-tested; not yet verified with a phone on hardware.
      - [x] Real date from NTP over the home Wi-Fi (2026-10-18). Get decks syncs
        the clock once connected (3 s NTP timeout, a failure never stops it).
        After a power cut, the date question joins the home Wi-Fi in the
        background and answers itself from NTP, with a status line ("Syncing
        via <ssid>..." / "No network: set it by hand"); the stepper stays usable
        and is the fallback, and any manual answer turns the radio off. Uses the
        UTC offset stored by the last phone sync and is skipped without it.
        (Since then: a visible network-first screen and a zone rule, below.)
        **Not verified on hardware yet.**
      - [x] Better manual fallback (2026-10-18): the date screen shows the
        last-used date and a proposed "today" with its weekday (tap +1 day, hold
        confirms, side hold -1 day, side tap cancels) instead of the 0/1/2-7/14+
        buckets. A sync from the phone answers it instead.
      - [x] Time zone rules, manual zone, network first (2026-10-18, v0.1.0
        hardware feedback): the page sends the browser's zone as a POSIX TZ
        rule, so the 04:00 rollover follows DST changes (23/25 h study days)
        without another visit; Settings › Device › Date › Time zone offers "From
        phone" and 22 zones for a device no phone has set; after a power cut
        with a home Wi-Fi and a zone, "Getting the date via <network>" comes
        before the stepper (a tap skips it); the stepper's hints spell out
        both directions. Native-tested (`test_tz`), the page's rule checked
        against tzdata for 597 zones (`scripts/check_tz_rules.py`).
        **Not verified on hardware yet.**
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
      - [x] **Home Wi-Fi (station mode).** The SSID/password are entered once on
        the portal page (with nearby-network suggestions) and stored in NVS
        `net`; the password is never logged or shown back. The device joins the
        home network only on demand -- while Get decks runs, and briefly for the
        date after a power cut (NTP, phase 2) -- with a 25 s join timeout, the
        radio off afterwards and the CPU at 240 MHz only meanwhile, so the
        battery policy holds.
        **Not verified on hardware yet** (built and reviewed only).
      - [x] **One-button deck download.** Decks -> "+ Get decks" joins the home
        Wi-Fi, reads the release catalog over **HTTPS** (verified against a small
        embedded root-CA set, redirects followed), lists its decks marked new /
        already here / other size, and a hold downloads one into `/packs` through
        the shared temp-file + validate + rename path, checking the catalog's
        size and `sha256`, with a progress bar and cancel. A fresh device offers
        it on the boot-time upload screen once a home Wi-Fi is saved. The default
        catalog is in the dedicated decks release
        (`releases/download/decks-v2/catalog-v2.txt`, never marked "latest";
        catalog format v2 with deck titles since 2026-10-18, published by
        `decks.yml`); the URL can be changed on the portal page. **Not verified on hardware yet**: TLS
        heap use, download speed and the GitHub redirect chain on a real
        StickS3 are to be confirmed (`[net]` serial lines log the heap).
      - [x] **Portal over the home Wi-Fi** (2026-10-18). With a home network
        saved, the portal joins it and serves the page on the station IP (QR
        code on the device, `tangocho.local` over mDNS), so the phone keeps its
        internet; the page lists the catalog and downloads decks itself, with a
        progress view. The open access point stays as *setup mode* (QR code to
        join), whose page saves the network and switches to home mode. The page
        is reordered around the task (first setup / Get decks / Your decks /
        Statistics / Advanced; Statistics left Advanced on 2026-10-19) and follows the device or browser language (en/es). Settings
        gains a **Wi-Fi »** submenu: Web portal, Home network (test), Setup
        mode, Forget network. **Not verified on hardware yet.**
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
        decks without `uploadfs` and without a computer. Plain `http://` only on
        the portal (its own AP has no internet; Get decks above does HTTPS over
        the home Wi-Fi); no resume yet.
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
      The **device-side catalog fetch/parse** is done (`firmware/src/catalog/`,
      native-tested against the builder's output; used by Get decks).
      **Prebuilt decks** have their own workflow, `.github/workflows/decks.yml`
      (manual dispatch): JLPT N5-N1 and the full Jōyō deck (school order), in
      es and en, with stroke order, published with the catalog to the fixed
      release `decks-v<pack format>` (never "latest", replaced in place), which
      is the firmware's default Get decks catalog; firmware releases no longer
      carry packs (docs/decisions.md, 2026-10-18 "Decks release").
      The **first decks run** published `decks-v2` on 2026-10-18 (12 packs,
      105 KB to 3.0 MB in pack format v2; Get decks and the portal download them
      on a real StickS3). The **first firmware release, v0.1.0**, followed on
      2026-10-18; its StickS3 merged image was verified with esptool and from
      the browser (esptool-js). The **second, v0.2.0** (2026-10-19), ships the
      UX refresh (phases 1-3), catalog v2 and the per-deck new-card cap; it is
      the first that reads `catalog-v2.txt`, so v0.1.0's Get decks stopped
      working when the decks release dropped `catalog.txt` (docs/decisions.md,
      2026-10-19). Pending: **README photos**, and the T-Display-S3 images on
      real hardware.

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
      applied as a pure `uiLangCodeFor` helper. The first boot of a device
      with no decks and no stored preference asks once (English / Español)
      and stores an explicit choice. Pending: more languages (the
      meaning language is a property of the pack, so switching packs already
      switches the meanings).
- [x] **Menu and settings redesign.** The settings home (Seguir / Mazos / Fecha /
      Nuevas/día / Velocidad) replaces the single deck list, with a shared
      four-row choice screen for the new/day cap and the Hard threshold. The deck
      list moved to its own screen and shows `due/new` for the loaded pack.
      Since the UX refresh's phase 3 (`feature/ux-settings`) the home is a
      summary card and five groups -- Decks, Study, Stats, Wi-Fi, Device --
      with their values; new settings go into a group, never onto the home.
- [x] **Landscape orientation.** The panel is natively portrait (135x240); a
      horizontal layout (240x135) gives much more room, especially for menus and
      the four options. Selectable from the settings home and persisted in NVS
      (portrait is the default and is unchanged). Landscape puts the quiz prompt
      in a left column and the options on the right, and shrinks the list rows to
      fit the shorter screen.

- [~] **Power management** (2026-10-18). Done: dim after 30 s, deep sleep
      after 2 min on battery (front button wakes; panel-off only on USB), 5 min
      Wi-Fi portal timeout, CPU at 80 MHz (240 while the portal or the home
      Wi-Fi runs), a brightness setting, power button and "Sleep" row = save +
      deep sleep, a critical-battery sleep. Pending: hardware checks (does the
      M5PM1 keep the rail up through deep sleep, sleep current, battery life),
      and the **USB buzz**: still audible up close after moving the backlight
      PWM to 25 kHz, so the backlight was not (or not the only) cause; to
      investigate.
- [ ] **UX refresh.** A review to give the device UI a more modern look and feel:
      typography and spacing, a consistent visual language across quiz, menus
      and detail card, clearer feedback and transitions on answers, a less
      text-heavy settings home (fewer rows or grouping), and an on-screen hint of
      what tap/hold do. Start with an audit and mockups before code.
      **Audit and mockups done** (2026-10-18): `docs/ux-refresh.md`
      (screen-by-screen audit, design tokens, type scale, components, regrouped
      settings tree, phased plan) and before/after PNGs in `docs/ux/`, generated
      by `tools/ux-mockups/render.py`. **Approved with changes** the same day
      (§6 there, docs/decisions.md "UX refresh decisions"): flat tiles, a
      firmware UI font from Noto Sans JP, auto-advance after a correct answer,
      a mode pill in the quiz header, five settings groups, a hold-duration
      setting, side tap = previous option, catalog v2 with deck titles.
      **Phase 1 implemented** (`feature/ui-foundation`, verified on the
      StickS3): design tokens, icons, the Noto Sans JP UI font
      (`tools/ui-font/`), flat widgets and the menu screens as scrolling lists
      with a hint bar (ux-refresh §4). **Catalog v2 done** (`feature/catalog-v2`):
      a `title` column (the pack's deck name) in `catalog-v2.txt`, shown by Get
      decks and the portal page; v1 is no longer written or read.
      **Phase 2 implemented** (`feature/ux-quiz`): quiz header with
      mode/result pill, cards left and session bar; flat answer tiles in the
      UI font / option kana / kanji grid; reveal with ✓/✗ and `GOOD · 4d`-style
      grades; the hint bar policy; side tap = previous option; hold progress on
      quiz tiles and list rows; auto-advance after a correct answer; the detail
      card with the pick, ON/KUN pills and ghost strokes. **Phase 3
      implemented** (`feature/ux-settings`): the settings tree (summary card,
      five groups, Study and Device pages with the Auto-advance and Hold time
      pickers), the deck list from pack headers, and the date, stats, done,
      Get decks, Wi-Fi portal, About and error screens in the flat style.
      Phases 1-3 are on `main`, checked on the StickS3 (2026-10-18). Left:
      phase 5 polish (kanji fade-in, maybe slides).
      Later: calibrate the "Hard after" thresholds from `TGC_QUIZ_LOG` logs
      (picking an option on the device takes longer than in Anki).
- [ ] **Learning mode** (idea, 2026-10-18). A mode for learning kanji, not
      only reviewing them: for example, introducing a new kanji (meanings,
      readings, stroke order) before its first multiple-choice question.
      Scope to be defined.

## Before publishing

- [x] Verify licence of tanos.co.uk JLPT lists (used via kanji-data `jlpt_new`).
      Verified 2026-10-18: the site's "Use my data!" page licenses everything
      not for sale under Creative Commons BY (credit the site), which may be
      included in our CC BY-SA packs with attribution. The site no longer
      resolves; the evidence is an archived copy (see docs/data-sources.md).
- [x] Decide whether prebuilt packs are distributed (CC BY-SA 4.0 attribution in
      README and on device). Decided and done: `decks-v2` was published on
      2026-10-18 and is the device's default catalog. Packs built with
      KANJIDIC2/KanjiVG are licensed CC BY-SA; `.github/workflows/decks.yml`
      (manual dispatch only) builds the default set and publishes it, with the
      combined attribution in the notes, to the release `decks-v<pack format>`
      (`decks-v2`), which is never marked "latest" so firmware releases cannot
      hide it. The device's default Get decks catalog is now
      `releases/download/decks-v2/catalog-v2.txt` on `dcarnicer/tangocho`
      (catalog format v2, 2026-10-18); the first run published the v1
      `catalog.txt`, the second (2026-10-18) published `catalog-v2.txt` and
      deleted `catalog.txt`, which only v0.1.0 reads. The former blockers (the
      JLPT lists' licence, the 5 MiB cap in pack format v1) were resolved first.
- [x] Attribution for redistributed data: README's "Releases" section,
      `docs/data-sources.md` and the release notes all carry the combined
      KANJIDIC2 / KanjiVG / Noto Sans JP credit. (On the device too since
      2026-10-18: Settings › Device › About, and the portal page's footer.)
- [x] Flash a **merged** image on a real StickS3 — in a browser and with
      `esptool write_flash 0x0 …` — to confirm the documented offsets. Done
      2026-10-18 for v0.1.0 (esptool, and esptool-js after Erase Flash); an
      ESP Web Tools install page is not set up (it needs a page serving the image
      and the manifest together).
