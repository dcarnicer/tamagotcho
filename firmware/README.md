# Tangochō firmware

PlatformIO project for the M5Stack StickS3 (primary) and the LilyGO
T-Display-S3 (dev stand-in). Phase 2: open the saved pack in `/packs` and run a
multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji) scheduled
from persisted FSRS review state (`/state/srs.bin`).

## Build and test

```sh
cd firmware
pio test -e native            # unit tests for pack/, quiz/ and srs/ (host, no Arduino)
pio run -e sticks3            # build for StickS3
pio run -e tdisplay-s3        # build for T-Display-S3
```

## Flash

```sh
pio run -e sticks3 -t upload      # firmware
pio run -e sticks3 -t uploadfs    # LittleFS image built from firmware/data/
pio device monitor
```

Use `-e tdisplay-s3` for the T-Display-S3. `-e sticks3-dev` is the StickS3 build
with the quiz trace (`TGC_QUIZ_LOG`) on the serial monitor: every question,
answer and grade, plus Wi-Fi and power events.

### Prebuilt images from a release

A GitHub release carries, per board, the app-only `tangocho-<env>.bin` (the same
file `pio run` produces, e.g. for M5Launcher) and a **merged**
`tangocho-<env>-merged.bin` that `esptool write_flash 0x0 …` or a browser (ESP
Web Tools, via the release's `manifest.json`) can flash in one go. A merged image
flashes an **empty filesystem**, so a fresh device boots into the Wi-Fi upload
screen and a pack can be sent over the air. Offsets and the exact
`esptool merge_bin` command are in the root README; the merged image is **not
verified on hardware** yet (docs/decisions.md).

## Putting a pack on the device

Build a pack with `tools/pack-builder` (see its README), copy the `.tgc` into
`firmware/data/packs/` (packs are git-ignored) and run `uploadfs`. The firmware
opens the deck saved in NVS, or the first `*.tgc` if there is none; hold the
side button to switch decks. `uploadfs` replaces the whole filesystem, so keep
every pack you want in `data/packs/`, and note that it also clears review
progress (`/state/srs.bin`).

Alternatively, copy packs over Wi-Fi with no `uploadfs`: open **Settings →
Wi-Fi** (hold the side button, then hold the row) and join the device's open
access point from a phone or laptop, then open `http://192.168.4.1` to upload or
delete packs, download one from a URL and read the statistics. A device with no
pack at all boots straight into this screen. The uploaded file is validated
before it is kept, and the side button stops the portal. See the root README for
the security trade-off (the AP is open).

## Flash layout

No OTA: one 2 MB app slot, everything else is LittleFS (partition label
`spiffs`, see `partitions/`).

| board         | flash | app    | LittleFS         |
|---------------|-------|--------|------------------|
| StickS3       | 8 MB  | 2 MB   | 0x5F0000 = 5.94 MB |
| T-Display-S3  | 16 MB | 2 MB   | 0xDF0000 = 13.94 MB |

## Controls

The front (confirm) button drives the quiz: a **tap** moves the highlight to the
next option, a **hold** (~600 ms) confirms the highlighted one. Once the answer
is revealed, a tap advances: for a correct answer straight to the next card, for
a wrong answer it first opens the mistake **detail card** (meanings, readings,
stroke order), and the next tap starts the following question. The side button is
a tap shortcut; **holding it opens the settings home** (Continue / Decks / Date /
New per day / Speed / Statistics / Wi-Fi / Orientation / Language / Brightness),
where a tap moves the selection, a hold activates the row, and the side button
goes back.

## Power

After 30 s without a button press the backlight dims (the next press brightens
it and acts as usual). After 2 min (5 min on the Wi-Fi screen, counting phone
requests as activity) progress is saved and, **on battery, the device
deep-sleeps**; the **front button wakes it** into a fresh question. The wake press
itself is ignored. Time spent asleep is measured by the ESP32's RTC timer and
advances the SRS day, so the "how many days?" question only appears after a real
power-off. **On USB power** it only turns the panel off (the serial port stays up
for flashing and logs); any press turns it back on, and unplugging lets it sleep.
To flash a sleeping device on battery, wake it first. The CPU runs at 80 MHz
(240 MHz while the Wi-Fi portal is up).

| button            | StickS3          | T-Display-S3 |
|-------------------|------------------|--------------|
| confirm (hold)    | front button (A) | GPIO0 (BOOT) |
| next / tap        | side button (B)  | GPIO14       |
| deck menu (hold)  | side button (B)  | GPIO14       |

## Layout

`src/board` HAL (one impl per board), `src/pack` pack reader (pure C++),
`src/quiz` question builder (pure C++), `src/render` glyph/text drawing,
`src/platform` LittleFS-backed pack source and SRS file plus the on-device Wi-Fi
portal (`WifiPortal`: open AP + HTTP upload/delete/download/stats),
`src/main.cpp` Phase 0 app.
