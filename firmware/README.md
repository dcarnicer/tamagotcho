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

Use `-e tdisplay-s3` for the T-Display-S3.

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
is revealed, a tap goes to the next card. The side button is a tap shortcut;
**holding it opens the settings home** (Continue / Decks / Date / New per day /
Speed / Statistics / Wi-Fi / Orientation / Language), where a tap moves the
selection, a hold activates the row, and the side button goes back.

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
