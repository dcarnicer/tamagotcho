# Tangochō firmware

PlatformIO project for the M5Stack StickS3 (primary) and the LilyGO
T-Display-S3 (dev stand-in). Phase 2: open the saved pack in `/packs` and run a
multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji) scheduled
from persisted FSRS review state (`/state/srs.bin`).

## Build and test

```sh
cd firmware
pio test -e native            # unit tests for pack/, quiz/, srs/ and catalog/ (host, no Arduino)
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

Or let the device fetch decks itself over your **home Wi-Fi**: save the network
once in the **Home Wi-Fi** form on that same page, then pick **Decks -> + Get
decks** on the device. It joins the network only while that screen is open,
reads the release catalog over HTTPS (verified, redirects followed), lists the
decks (`+` new, `=` here, `~` other size) and downloads the one you hold, checked
against the catalog's size and SHA-256. A fresh device offers the same from its
boot screen once a home network is saved. The default catalog only exists once
a public release carries packs + `catalog.txt`; the page can set another URL.
Serial `[net]` lines report each step and the free heap around TLS.

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
New per day / Speed / Statistics / Wi-Fi / Orientation / Language / Brightness /
Sleep), where a tap moves the selection, a hold activates the row, and the side
button goes back. The deck list's last row, **+ Get decks**, downloads decks
over the home Wi-Fi (tap moves, hold downloads or studies, side button
cancels/back).

## Orientation

Settings → Orientation: **Portrait** (default), **Landscape**, or, on the
StickS3, **Auto**, which follows the accelerometer: upright for portrait,
turned either way for landscape (the text is upright in both directions).
Upside down and lying flat keep the current layout. A turn takes effect after
about half a second, never in the middle of a button press, and does not count
as activity for dimming or sleep. Only the picture turns; the buttons keep their
roles. The T-Display-S3 has no IMU and offers only the fixed choices.

## Power

After 30 s without a button press the backlight dims (the next press brightens
it and acts as usual). After 2 min (5 min on the Wi-Fi screen, counting phone
requests as activity) progress is saved and, **on battery, the device
deep-sleeps**; the **front button wakes it** into a fresh question. The wake press
itself is ignored. Time spent asleep is measured by the ESP32's RTC timer and
advances the SRS day, so the date question (below) only appears after a real
power-off. **On USB power** it only turns the panel off (the serial port stays up
for flashing and logs); any press turns it back on, and unplugging lets it sleep.
To flash a sleeping device on battery, wake it first. The CPU runs at 80 MHz
(240 MHz while the Wi-Fi portal or the home Wi-Fi is up; the home Wi-Fi only
while Get decks is open, and a download counts as activity).

**Sleep on demand.** On the StickS3 a **click of the power button** (the M5PM1's
button, which used to restart the device) saves and deep-sleeps right away, after
a short "Sleeping" notice; the **Sleep** row in the settings does the same on
both boards. Both sleep even on USB power. The front button wakes it, the power
button does not. A **double click** still really powers the device off: it is
the hardware way out if the app ever hangs, and the next start asks for the
date. A **long press** still restarts into download mode for flashing; leave it
with a double click (off), then a click (on). The firmware changes this in the
PM1 at every boot and the PM1 keeps it, also for other firmware, until it loses
power (battery disconnected or flat). Flash a build with
`-DTGC_PM1_CLICK_SLEEP=0` (e.g.
`PLATFORMIO_BUILD_FLAGS=-DTGC_PM1_CLICK_SLEEP=0 pio run -e sticks3 -t upload`)
to put the factory click reset back. If the PM1 is not recognised, the button
keeps its factory behaviour (see the `pm1:` lines in the serial log). The
power-button handling is **not verified on hardware yet** (docs/decisions.md).

**Critical battery.** On battery at 3 % (three readings 10 s apart) the device
saves, shows "Battery low" and deep-sleeps, before the cell dies and takes the
day clock with it. Woken while still below 6 % and unplugged, it shows the
notice again and goes back to sleep. Plug in USB to use it.

| button            | StickS3            | T-Display-S3 |
|-------------------|--------------------|--------------|
| confirm (hold)    | front button (A)   | GPIO0 (BOOT) |
| next / tap        | side button (B)    | GPIO14       |
| deck menu (hold)  | side button (B)    | GPIO14       |
| sleep now (click) | power button (PM1) | --           |

## Date

There is no RTC chip: the SRS day advances with uptime and deep sleep. Opening
the Wi-Fi portal page from a phone (**Settings → Wi-Fi**, then
`http://192.168.4.1`) sets the real date from the phone's clock and time zone;
from then on the day rolls over at 04:00 local and the statistics show the real
date (docs/srs.md §5.1). After a real power-off the device asks on the **date
screen**: it shows the last-used date and a proposed "today" with its weekday. A
tap adds a day, a hold confirms, a side-button hold takes a day back (never
before the current day) and a side tap skips the question. The same screen is
**Settings → Date** for manual corrections.

## Layout

`src/board` HAL (one impl per board), `src/pack` pack reader (pure C++),
`src/quiz` question builder (pure C++), `src/imu` orientation filter for the
Auto rotation (pure C++), `src/power` critical-battery guard (pure C++),
`src/catalog` deck-catalog parser (pure C++), `src/render` glyph/text drawing,
`src/platform` LittleFS-backed pack source and SRS file, the on-device Wi-Fi
portal (`WifiPortal`: open AP + HTTP upload/delete/download/stats/home Wi-Fi/
phone clock), the home network (`HomeWifi`) and the shared HTTP(S) pack
download (`NetFetch`, roots in `RootCerts.h`), `src/main.cpp` the app.
