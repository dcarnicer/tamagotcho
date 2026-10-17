# Tangochō firmware

PlatformIO project for the M5Stack StickS3 (primary) and the LilyGO
T-Display-S3 (dev stand-in). Phase 0: open the first pack in `/packs` and run a
random multiple-choice quiz (kanji -> meaning / reading).

## Build and test

```sh
cd firmware
pio test -e native            # unit tests for pack/ and quiz/ (host, no Arduino)
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
opens the first `*.tgc` it finds in `/packs`. `uploadfs` replaces the whole
filesystem, so keep every pack you want in `data/packs/`.

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
is revealed, a tap goes to the next card. The side button is a tap shortcut.

| button            | StickS3          | T-Display-S3 |
|-------------------|------------------|--------------|
| confirm (hold)    | front button (A) | GPIO0 (BOOT) |
| next / tap        | side button (B)  | GPIO14       |

## Layout

`src/board` HAL (one impl per board), `src/pack` pack reader (pure C++),
`src/quiz` question builder (pure C++), `src/render` glyph/text drawing,
`src/platform` LittleFS-backed pack source, `src/main.cpp` Phase 0 app.
