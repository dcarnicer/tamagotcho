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
        - [x] Hardware checks (StickS3, 2026-10-18/19): the PM1 honours
          `SINGLE_RESET_DIS` and reports the press (a click sleeps, the front
          button wakes it, `power: woke after 45 s`); a click powers on a
          device that is off; a long press enters download mode and a double
          click leaves it; the sleep notice reads well.
        - [x] Battery thresholds on a real discharge (3 % sleep, resume at 6 %
          or on USB). StickS3, 2026-10-23, with `sticks3-batt` (awake, full
          brightness, no dim/sleep): 100 % → 0 % in about 2 h; the guard slept
          after three 0 % readings, but the gauge jumped 6 % → 0 %, so the
          exact 3 % was not seen (the rule is native-tested). USB + the front
          button resumed at 14 % (`boot after critical (usb)`). The 6 %
          no-USB resume is accepted as working: the gauge is unreliable at
          that level, and the USB resume plus the native `canResume` rule are
          the evidence.
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
        Verified with a phone on the StickS3 (2026-10-19: the page posts the
        date, offset and zone rule on every load).
      - [x] Real date from NTP over the home Wi-Fi (2026-10-18). Get decks syncs
        the clock once connected (3 s NTP timeout, a failure never stops it).
        After a power cut, the date question joins the home Wi-Fi in the
        background and answers itself from NTP, with a status line ("Syncing
        via <ssid>..." / "No network: set it by hand"); the stepper stays usable
        and is the fallback, and any manual answer turns the radio off. Uses the
        UTC offset stored by the last phone sync and is skipped without it.
        (Since then: a visible network-first screen and a zone rule, below.)
        Verified on the StickS3 (2026-10-19): Get decks and the portal sync
        the clock from NTP once joined.
      - [x] Better manual fallback (2026-10-18): the date screen shows the
        last-used date and a proposed "today" with its weekday (tap +1 day, hold
        confirms; then side hold -1 day and side tap cancel, swapped on
        2026-10-20, below) instead of the 0/1/2-7/14+ buckets. A sync from the
        phone answers it instead.
      - [x] Time zone rules, manual zone, network first (2026-10-18, v0.1.0
        hardware feedback): the page sends the browser's zone as a POSIX TZ
        rule, so the 04:00 rollover follows DST changes (23/25 h study days)
        without another visit; Settings › Device › Date › Time zone offers "From
        phone" and 22 zones for a device no phone has set; after a power cut
        with a home Wi-Fi and a zone, "Getting the date via <network>" comes
        before the stepper (a tap skips it); the stepper's hints spell out
        both directions. Native-tested (`test_tz`), the page's rule checked
        against tzdata for 597 zones (`scripts/check_tz_rules.py`).
        Verified on the StickS3 (2026-10-18/19): the zone rule from the phone
        (Europe/Madrid, +120 min in October), and after a real power-off the
        network-first screen answers by itself, or a tap leads to the stepper.
        Choosing a zone by hand checked on the StickS3 too (2026-10-20).
      - [x] Date stepper in both directions (2026-10-20, StickS3 feedback: with
        the network time failing there was no visible way to go back a day,
        and the side tap cancelled): side tap -1 day, side hold cancel, both
        in the hint bar ("• +1 day ▬ OK", "◂ -1 day ▬ cancel", the side items
        on a second row in portrait). Below the current day the index is kept
        and the anchor moves back, like a `Reanchored` sync; only on a device
        that has had a real-time date once (docs/srs.md §5). Native-tested
        (`test_srs`, `test_quizui`, `test_settings`). Verified on the StickS3
        (2026-10-20): through the serial remote in portrait and Spanish (from
        Settings, and the boot-time question with the join failing), then by
        hand in landscape and English.
- [~] **Phase 3 – Better cards.** Distractors done for visual similarity,
      shared classical radicals and confusable readings; meaning→kanji questions
      done; the mistake detail card is done (a tap after a wrong answer opens a
      read-only card with the kanji, all meanings/readings, strokes and the
      card's own accuracy), and it now plays the kanji's **stroke order from
      KanjiVG** when the pack was built with `--strokes` (a v1-compatible
      optional pack section; packs without it behave exactly as before).
      *Common* readings (done 2026-10-20, docs/common-readings.md): KANJIDIC2's
      reading order is not by frequency (e.g. 九's first kun reading is the rare
      ここの), so `build --common-readings` puts the Jōyō table's approved
      readings first, then those of the easiest
      JLPT vocabulary, then the most frequent (JMdict, JmdictFurigana,
      wordfreq, Tatoeba), and leaves a kind with nothing worth asking empty,
      so 気 and 百 are asked their on'yomi; the firmware keeps the on/kun turn
      and the detail card right for such cards (checked on the StickS3,
      2026-10-20). The N5 list was reviewed by the maintainer; the reading
      order's credits are on About and the portal footer.
      - [x] Rebuild the published decks with it: `decks.yml` replaced
        `decks-v2` in place on 2026-10-20, and again on 2026-10-21 (hiragana
        glyphs, the new repository name in the catalog's URLs).
      - [x] Follow-up: let a few common readings the Jōyō table does not
        approve be asked (等 など, 経 た.つ, 被 かぶ.る), e.g. for learners outside
        the school curriculum. Done (2026-10-22): the project-authored
        `EXTRA_COMMON` table in `readings.py` treats them as approved for the
        reading order, while `classify()` still reports what the table says;
        N3 asks 等 など and 経 た.つ, N2 asks 被 かぶ.る
        (docs/common-readings.md, "Extra common readings"). Published on
        2026-10-22 (`decks.yml` run 39746603559).
      - [x] Rebuild the published decks after the extra common readings
        (2026-10-22): done, `decks.yml` run 39746603559 rebuilt the 12 packs;
        the published N3/N2 packs were checked and their catalog SHA-256
        verified.
      - [x] **Meanings in a useful order** (2026-10-20). KANJIDIC2's Spanish
        meanings are often ordered worse than its English ones, and the device
        shows the first ones: 持 "duración; propiedad" (en "hold; have"), 界
        "borde; frontera" (en "world; boundary"), 題 "objeto; tema" (en
        "topic; subject"); and its English order has quirks of its own (機
        "loom", 私 "private", 原 "meadow"). Done (docs/meaning-order.md):
        `build --meaning-order`
        orders the English meanings by the JLPT vocabulary words that use the
        kanji (their JMdict glosses), the Spanish ones by the English ones they
        translate (FreeDict's English-Spanish dictionary, CC BY-SA 3.0), then
        applies a 39-line project-authored override table; 524 English and 707
        Spanish first meanings change (N5: 14 and 14), English decks included,
        and in every language meaning distractors also avoid the meanings a
        pack leaves out. The maintainer reviewed the N5-N3 doubtful cases
        (2026-10-20). On `main` and in the published decks since 2026-10-20.
      - [ ] Follow-up: French and Portuguese meanings in the English order too
        (about as many changes as Spanish), once a pinned English-French /
        English-Portuguese dictionary under a compatible licence exists.
      - [ ] Follow-up: hand-check the N2-N1 meaning changes the report flags.
      Invariant enforced: **no option may be another valid reading or meaning of
      the target** (otherwise two options are correct); checked in `Quiz.cpp`
      and in the builder's distractor lists.
- [~] **Phase 4 – Connectivity.** The on-device **Wi-Fi portal** is done: it
      serves a single self-contained page from a phone/laptop on its own open
      access point (`Tamagotcho-XXXX`, `http://192.168.4.1`) to upload, delete and
      download packs and to read the SRS statistics (`/`, `/api/stats`); it also
      lets a fresh, pack-less device boot into an "upload a `.tgc`" state and
      continue into the quiz once a pack arrives. Pending:
      - [x] **Home Wi-Fi (station mode).** The SSID/password are entered once on
        the portal page (with nearby-network suggestions) and stored in NVS
        `net`; the password is never logged or shown back. The device joins the
        home network only on demand -- while Get decks runs, and briefly for the
        date after a power cut (NTP, phase 2) -- with a 40 s join timeout, the
        radio off afterwards and the CPU at 240 MHz only meanwhile, so the
        battery policy holds. Verified on the StickS3 (2026-10-19), which
        found that a network with several access points needs the join pinned
        to the strongest one (docs/decisions.md, 2026-10-19). The step that
        falls back to any access point after 25 s ran on hardware on
        2026-10-20 (serial remote hook `f`): it joined the strongest one at
        31 s and NTP answered.
        - [x] Once, an NTP request right after a join got no answer in 3 s
          (2026-10-19): since 2026-10-21 a second try follows (6 s at most).
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
        `decks.yml`); the URL can be changed on the portal page. Verified on
        the StickS3 (2026-10-19): the GitHub redirect chain works, a TLS
        session costs about 45 KB of internal heap (248 KB free before, 164 KB
        largest block after), and N3 (541 KB) downloaded in 11.8 s, about
        46 KB/s.
      - [x] **Portal over the home Wi-Fi** (2026-10-18). With a home network
        saved, the portal joins it and serves the page on the station IP (QR
        code on the device, `tangocho.local` over mDNS), so the phone keeps its
        internet; the page lists the catalog and downloads decks itself, with a
        progress view. The open access point stays as *setup mode* (QR code to
        join), whose page saves the network and switches to home mode. The page
        is reordered around the task (first setup / Get decks / Your decks /
        Statistics / Advanced; Statistics left Advanced on 2026-10-19) and
        follows the device or browser language (en/es). Settings gains a
        **Wi-Fi »** submenu: Web portal, Home network (test), Setup mode,
        Forget network. Verified on the StickS3 (2026-10-19): the page by IP
        and by `tangocho.local`, the catalog with deck titles.
      - [x] **Auto-rotation from the IMU** (2026-10-18). Use the StickS3's BMI270
        accelerometer to switch portrait/landscape as the device is turned, with
        hysteresis and a short settle time so it does not flicker; the
        Orientation setting gains "Auto" next to the fixed choices. The
        T-Display-S3 has no IMU and keeps the fixed setting. Auto uses portrait
        and both landscape directions (pure `imu/OrientationFilter`); upside
        down keeps the layout, as phones do. Axis mapping confirmed on the
        StickS3 (2026-10-19), and Auto is now the default there.
      - [ ] IMU input (tilt to choose).
      - [x] **WaniKani importer** (user's own token, unlocked kanji only). The
        builder can build a pack from the kanji a user has unlocked on their own
        WaniKani account instead of (or as well as) KANJIDIC2, via
        `tamagotcho-pack build --wanikani-token ... [--wanikani-level N]`. Nothing
        is shipped, nothing is fetched without the flag, and the token is never
        logged or cached; a WaniKani-derived pack is the user's own
        responsibility and is not redistributed (docs/data-sources.md).
      - [x] **Your own kanji list** (2026-10-22). `build --kanji-file PATH`
        builds a deck from a UTF-8 text file with one kanji per line, for a
        textbook chapter or a class list. Comments and blank lines are allowed,
        duplicates keep their first occurrence, and unknown kanji are skipped
        with a warning. The file order is the introduction order unless
        `--order jlpt|grade` says otherwise; the deck name defaults to the file
        stem. It uses KANJIDIC2 and is `--jlpt`/`--grade`/`--joyo`/WaniKani
        exclusive. One character per line only: no CSV or Anki import.
      - [x] **Download packs over Wi-Fi** from a URL, so a fresh device gets
        decks without `uploadfs` and without a computer. Plain `http://` only on
        the portal (its own AP has no internet; Get decks above does HTTPS over
        the home Wi-Fi); no resume yet.
      - [x] **Progress backup and restore** (2026-10-22). Advanced gains a
        **Progress backup** section: **Download progress file** streams
        `/state/srs.bin` as `tangocho-progress.bin` (read-only, no deck needed),
        and **Restore progress** uploads a file through a size-capped temp that
        is CRC/format-validated before the swap. The previous state is kept as
        `/state/srs.before-restore.bin`; a failed upload or restore leaves it
        loaded. The page's clock post after the redirect aligns the restored
        day index. Settings and deck files are not in the backup. Design:
        docs/srs.md section 4, docs/decisions.md 2026-10-22.
      - [~] **Deck update checks over the home Wi-Fi** (2026-10-22; design
        docs/deck-updates.md). v1 on `main`: at boot, once a day on USB power
        (or once a week on battery from 50 %), the device joins the home
        network, reads the catalog and compares the installed files' sizes and,
        when they match, SHA-256 (2026-10-22). Decks shows "N updates" and a new
        Updates row opens the filtered Get decks list, where a hold replaces a
        deck in place (progress survives). The check result is a two-second,
        skippable screen; it never downloads by itself. Checked on a StickS3 and
        a Cardputer ADV (2026-10-22): boot check, Updates row, detection,
        download and progress preservation, with the keyboard on the Cardputer.
        - [x] Automatic result screen with N > 0, the tap-to-skip path and the
          catalog-failure path, captured on a StickS3 (2026-10-23, 6 updates;
          docs/deck-updates.md). Two UI issues found: the Spanish result line
          is cut ("6 actualizacion..."), and the home Decks row hides the
          update badge in portrait (it showed "6 act." in landscape).
        - [x] Fix the Spanish result line and the portrait home badge
          (2026-10-23). The result now reads "6 mazos por / actualizar" and the
          home row shows "Mazos 6 act" in portrait ("Decks 6 upd" in English);
          `test_ui` checks both widths.
        - [x] Shorten the updates and Get decks headers to "%s · %u"
          (2026-10-23): with "Shimokitazawa" the old lines were 150/138 px in
          127 and were cut; the headers now show the network and the count.
        - [x] Refresh the update count from any home-Wi-Fi session
          (2026-10-23): Get decks already did; the portal page, the network
          test and the boot-time date sync now do too. The date sync only reads
          the catalog when the policy says due; the standalone boot check stays
          as the fallback. The English result line was cut too and is now
          "6 updates".
        - [ ] Tune the battery policy (the daily / weekly / 50 % rule is
          native-tested; a real battery run is still pending).
      - [~] **M5Launcher compatibility.** The app-only `.bin` that
        `pio run -e sticks3` produces can be installed as a launcher app, and
        the Wi-Fi upload is the intended way to populate `/packs` once it runs
        (the launcher's SPIFFS starts empty and our `uploadfs` layout is not the
        one in use). Documented in README.md. The Cardputer ADV's launcher
        table, seen on 2026-10-22, has app0 1,344 KB (test), firmw1 2,496 KB
        (the OTA app slot) and a 448 KB SPIFFS; the 1.48 MB app-only image does
        not fit app0, so a launcher install would have to use firmw1 (not
        tried), and 448 KB cannot hold a normal deck set. The Wi-Fi portal
        remains the deck path; the hardware test used a merged image with the
        project's table.
- [x] **Phase 5 – Release.** Done so far: the app-only image and a **merged
      single-file image** per board plus an **ESP Web Tools `manifest.json`**,
      published by `.github/workflows/release.yml` on tags `v*` (and manual
      dispatch); CI (`.github/workflows/ci.yml`) runs the native tests and both
      board builds plus the pack-builder tests; a `tamagotcho-pack catalog` command
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
      2026-10-19). **v0.2.1** (2026-10-19) has the fixes from that day's
      hardware test: home Wi-Fi joins pinned to the strongest access point,
      statistics in their own section of the page, Auto orientation by default.
      **v0.3.0** (2026-10-21) is the first as たまごっちょ (Tamagotcho) and the
      first with Cardputer ADV images; it adds on'yomi answers in hiragana,
      Delete progress and the new date screen buttons, and goes with the
      `decks-v2` rebuild of that day (reading and meaning order, hiragana
      glyphs). Its "What's new" comes from `docs/releases/v0.3.0.md`.
      **v0.4.0** (2026-10-21) adds the `m5stack` image, one build for the
      StickS3 and the Cardputer ADV, which is the one on M5Burner, and small
      fixes (NTP's second try, values cut short in portrait). **v0.4.1**
      (2026-10-21) fixes the deck that went unreadable after the second
      download of a session (the TLS client's `close(0)`).
      **v0.4.2** (2026-10-22) adds the deck update checks and the portal's
      weakest-kanji view, and goes with the `decks-v2` rebuild of that day;
      its notes are `docs/releases/v0.4.2.md`.
      **v0.5.0** (2026-10-22) adds SHA-256 to the deck update check, progress
      backup and restore in the portal, `--kanji-file` in the pack builder, and
      the browser install page on GitHub Pages. It was checked on a StickS3
      (same-size update detection and update, backup/restore, a custom pack
      upload/load/delete, release image boot), a T-Display-S3 (release image
      boot, N4 deck and one record, quiz/settings/setup portal on 170×320) and
      a Cardputer ADV (release `m5stack` image boot, board and TCA8418 keyboard
      detected, first-boot language picker; the `cardputer-adv-shots` setup
      portal on 240×135; original M5Launcher flash restored). Its notes are
      `docs/releases/v0.5.0.md`.
      **v0.5.1** (2026-10-23) refreshes the deck update count from any
      home-Wi-Fi session (Get decks, the portal, the network test, the
      boot-time date sync) and fits the update texts on the narrow screen,
      all checked on a StickS3; its notes are `docs/releases/v0.5.1.md`.
      It is the first version on M5Burner after v0.4.1 (submitted
      2026-10-23, pending review; v0.4.2 and v0.5.0 were skipped there).
      **v0.6.0** (2026-10-24) adds learning mode (the intro card, learning
      steps, example words from the new `decks-v3` release) and lets the
      portal replace and delete the loaded deck; checked on the StickS3, the
      T-Display-S3 and the Cardputer ADV; its notes are
      `docs/releases/v0.6.0.md` (M5Burner: submitted, pending review).
      The **README** is a landing page since 2026-10-21: a cover and a
      gallery of real screens (`tools/screenshots/readme_images.py`, shots
      from a StickS3 through the serial remote), with the long manual moved
      to `docs/user-guide.md`. The v0.3.0 merged images of the StickS3 and
      the T-Display-S3 booted as new devices on real hardware (2026-10-21).
      The Cardputer ADV's v0.3.0 merged image booted on hardware the same
      day. **Published on M5Burner** on 2026-10-22, when v0.4.1 passed
      M5Stack's review: one entry for both M5 boards with the `m5stack` image
      (docs/m5burner.md).
      - [x] `release.yml` puts `docs/releases/<tag>.md` in the notes as the
        "What's new" (from v0.3.0; before, it was added by hand).
      - [x] CI: also build `sticks3-shots` and `cardputer-adv-shots` (the
        serial remote), so its code keeps compiling (2026-10-21).

## Cross-cutting

- [x] **Statistics.** Per-kanji accuracy, current streak, review/seen counts and
      time spent; a small stats screen on device and richer views in the Wi-Fi
      portal. Depends on the SRS state (phase 2). On-device screen done:
      `SrsStore::stats()` + `Screen::Stats` show the date, current/best streak,
      time on questions, seen/pack total, due/new, lifetime answers, accuracy and
      learned count. Single-card accuracy is on the mistake detail card. The
      Wi-Fi portal shows the same counters on its page and as JSON at
      `/api/stats`. An aggregate per-kanji accuracy view is done (2026-10-22):
      the portal lists the ten kanji with the worst accuracy among those with
      at least 3 reviews and one miss (worst first, with right/review counts),
      and `/api/stats` carries the same `trouble` array.
      Deleting progress (2026-10-20, `feature/delete-deck-progress`): deleting
      a deck on the portal forgets its kanji that no other installed deck has;
      Settings › Study › Delete progress resets the loaded deck or everything,
      statistics included; the accuracy is a lifetime counter now (docs/srs.md
      §2.1). Verified on the StickS3 (2026-10-20, portrait and landscape,
      en/es): this deck, everything, "Nothing to delete"; the portal's delete
      was not run on hardware (native-tested, same forget code).
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
      (portrait was the default until Auto took over, below). Landscape puts the quiz prompt
      in a left column and the options on the right, and shrinks the list rows to
      fit the shorter screen.

- [~] **Third board: M5Stack Cardputer ADV** (2026-10-21, released in
      v0.3.0). Envs `cardputer-adv` and `cardputer-adv-shots`:
      landscape only (no Orientation row, no Auto), the keyboard drives every
      screen (arrows, Enter, Esc/del, Tab as the menu key; arrows repeat on
      lists) and the hint bar names keys; G0 is the front button; deep sleep
      wakes on G0 or any key. No PSRAM: packs open streamed (a few KB instead
      of up to 237 KB), the glyph cache has a 16 KB budget, the SRS table
      grows by steps (docs/architecture.md, "Memory without PSRAM").
      Checked on a Cardputer ADV (2026-10-21): orientation, the key map, the
      Tab menu, G0, sleep and wake on a key, the battery icon; the arrows on
      the 2x2 kanji grid move by row and column. Get decks downloads work
      without PSRAM (2026-10-21): N1 (1.8 MB) and N2 (532 KB, 11.3 s, as on
      the StickS3); about 107 KB of internal heap free after the TLS
      handshake, 67 KB the largest block, 62 KB the lowest since boot (after
      a portal session); a 1,232-kanji deck opens streamed in 10 KB.
      - [x] **Bug: a deck unreadable after the second download of a boot**
        (2026-10-21, found on the Cardputer ADV, on every board): its name
        read "" and its reads failed (EBADF) until a reboot. Cause: the
        core's `WiFiClientSecure` called `close(0)` at the end of every TLS
        request, first closing stdin and then whatever file had taken
        descriptor 0, which after a download is the reloaded deck. Fixed in
        `platform/NetFetch.cpp` (`TlsClient`); docs/decisions.md has the
        hunt.

- [~] **Power management** (2026-10-18). Done: dim after 30 s, deep sleep
      after 2 min on battery (front button wakes; panel-off only on USB), 5 min
      Wi-Fi portal timeout, CPU at 80 MHz (240 while the portal or the home
      Wi-Fi runs), a brightness setting, power button and "Sleep" row = save +
      deep sleep, a critical-battery sleep. Verified on the StickS3
      (2026-10-18/19): the dim, the 2 min sleep on battery and the wake with
      the front button where it left off. Pending: sleep current and battery
      life (no battery figure is shown, only the header icon), and the **USB
      buzz**: still audible up close after moving the backlight
      PWM to 25 kHz, so the backlight was not (or not the only) cause; to
      investigate.
- [~] **UX refresh.** A review to give the device UI a more modern look and feel:
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
      phase 5 polish (kanji fade-in, maybe slides). The two values cut short
      in portrait seen on 2026-10-20 are fixed (2026-10-21): narrow screens
      show the Date page's values under their labels, and the Spanish Wi-Fi
      menu's "Modo configuración" row is "Configurar"; the English "Forget
      network" (cut to "Forget net...") is "Forget Wi-Fi".
      Later: calibrate the "Hard after" thresholds from `TGC_QUIZ_LOG` logs
      (picking an option on the device takes longer than in Anki).
- [x] **Learning mode** (idea, 2026-10-18; approved 2026-10-23; released in
      v0.6.0, 2026-10-24). A mode for
      learning kanji, not only reviewing them: for example, introducing a new
      kanji (meanings, readings, stroke order) before its first
      multiple-choice question. **Design proposal** (2026-10-23):
      `docs/learning-mode.md` (today's flow of a new card, options, a
      recommended design: an intro card and two same-day learning steps inside
      the daily session, the FSRS state it records, mockups in
      `docs/learning-mode/`, a phased plan) with ten open decisions, all
      answered with the recommendations on 2026-10-23 (docs/decisions.md).
      Built on top of the `main.cpp` split: the SRS learning steps, the intro
      card (Settings › Study › Introduce), and example words (pack format v3)
      with English glosses from JMdict_e and Spanish ones from a reviewed
      project table (docs/example-words.md), published in `decks-v3`.
      Later, on hold: the Spanish glosses beyond N5 (reviewed level by level,
      then `decks-v3` rebuilt in place) and the long-run checks of
      docs/learning-mode.md §5.
- [x] **OTA firmware updates** (idea, 2026-10-21; design approved 2026-10-24
      in `docs/ota.md`; released in v0.7.0, 2026-10-24). Update the firmware over
      the home Wi-Fi, from the device, instead of over USB: for example a
      Settings › Device › Update row that reads the latest GitHub release
      (as Get decks reads the decks catalog: HTTPS, size and SHA-256 checked),
      says what is new and installs the board's app-only image, keeping
      settings, decks and progress. Decided (docs/ota.md §8):
      - The flash layout: two app slots of 1.75 MiB on the 8 MB boards
        (LittleFS 5.94 → 4.44 MiB: the Jōyō deck and N1 no longer fit
        together), 2 MiB on the T-Display-S3 (11.94 MiB left); `nvs` and
        `app0` keep their offsets. The move is one USB flash, by hand:
        progress backup, flash, Get decks, restore.
      - Which image: the running build's environment (`TGC_ENV`), its row of
        the release's manifest `tamagotcho-firmware.txt`; development builds
        never update.
      - Safety: battery and free heap checks before starting (TLS without
        PSRAM on the Cardputer ADV), the bootloader's rollback if the new
        image does not boot, never in the middle of a review.
      - M5Launcher installs keep their own update path.
      - Status: phase 1 (docs/ota.md §6) built 2026-10-24: the two tables,
        `TGC_ENV`, the manifest and its slot-size check in `release.yml`,
        `boot_app0.bin` as a release asset for app-only USB updates, the
        docs. Not yet flashed on the boards.
- [x] **One image for the StickS3 and the Cardputer ADV** (2026-10-21).
      The `m5stack` env compiles both boards and picks one at boot (PSRAM or
      not), so one M5Burner entry lists both devices; checked on both, and as
      a merged image on a blank Cardputer (docs/decisions.md, "One image for
      the StickS3 and the Cardputer ADV"). The single-board envs stay.
- [x] **Decks on the Cardputer ADV's microSD card** (idea, 2026-10-21;
      design approved 2026-10-24 in `docs/microsd.md`; released in v0.7.0,
      2026-10-24). The
      Cardputer has a microSD slot (CS G12, MOSI G14, CLK G40, MISO G39):
      decks copied there from a computer, beside or instead of the 4.44 MiB
      LittleFS (5.94 MiB before v0.7.0). Decided: a FAT32 card's
      `/tamagotcho/decks/` holds the decks, new ones go there, the LittleFS
      decks move there by themselves whenever a card mounts, the progress
      stays on LittleFS with a copy on the card at every sleep.
      - Status: built 2026-10-24 on `feature/microsd` (docs/microsd.md §8:
        the board's pins, `platform/Storage`, the move and its screen, a
        pulled card, the deck list's SD pill, About, the portal); not yet
        checked on a Cardputer ADV (the checks of docs/microsd.md §6).
- [ ] **A watch: Waveshare ESP32-S3-Touch-AMOLED-2.06** (idea, 2026-10-24;
      after OTA). ESP32-S3R8 (8 MB PSRAM), 32 MB flash, 2.06" AMOLED
      410×502 (CO5300 over QSPI, which LovyanGFX 1.2.34 drives:
      `Panel_CO5300`), FT3168 touch (`Touch_FT5x06`), QMI8658 IMU, PCF85063
      RTC on a backup battery, AXP2101 PMU, microSD, speaker and microphones,
      BOOT (GPIO0) and PWR (read on GPIO10) buttons. Its own environment and
      image, not the shared `m5stack` one: the maintainer is fine with a
      separate firmware. Steps: a bring-up with today's UI drawn at 205×251
      and doubled (every screen works at once; the 4bpp glyphs stay legible),
      then native layouts at 410×502 (about 315 ppi, so 18 px text is too
      small: decks built with larger `--large-px`/`--text-px`, a larger UI
      font) and a touch-first answer (tap the option); **watch functions**
      (a clock face when idle, with the RTC's time and date, the battery and
      the day's due count; raise to wake with the IMU; maybe a timer or an
      alarm on the speaker); decks on its microSD, shared with the Cardputer
      item above. Its factory flash is backed up in
      `~/Downloads/tangocho-backups/` (2026-10-24).

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
      `releases/download/decks-v2/catalog-v2.txt` on `dcarnicer/tamagotcho`
      (`dcarnicer/tangocho` before the rename of 2026-10-21; catalog format v2,
      2026-10-18); the first run published the v1
      `catalog.txt`, the second (2026-10-18) published `catalog-v2.txt` and
      deleted `catalog.txt`, which only v0.1.0 reads. The former blockers (the
      JLPT lists' licence, the 5 MiB cap in pack format v1) were resolved first.
- [x] Attribution for redistributed data: README's "Releases" section,
      `docs/data-sources.md` and the release notes all carry the combined
      KANJIDIC2 / KanjiVG / Noto Sans JP credit. (On the device too since
      2026-10-18: Settings › Device › About, and the portal page's footer.)
- [x] Flash a **merged** image on a real StickS3 — in a browser and with
      `esptool write_flash 0x0 …` — to confirm the documented offsets. Done
      2026-10-18 for v0.1.0 (esptool, and esptool-js after Erase Flash).
- [x] **ESP Web Tools install page** (2026-10-22). Published at
      <https://dcarnicer.github.io/tamagotcho/>: `.github/workflows/pages.yml`
      downloads each release's merged images and manifests, renders
      `tools/install-page/index.html`, and deploys them together, because the
      page and the assets must share an origin (release assets send no CORS
      headers). GitHub Pages was enabled on 2026-10-22 and the v0.4.2 page
      deployed (run 39805320148, HTTP 200, manifest and merged image checked);
      every later Release republishes it. It is a **first-install** path only:
      the merged image erases the settings, while updates stay on the app-only
      image.
