# Tangochō firmware

PlatformIO project for the M5Stack StickS3 (primary) and the LilyGO
T-Display-S3 (dev stand-in). Phase 2: open the saved pack in `/packs` and run a
multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji) scheduled
from persisted FSRS review state (`/state/srs.bin`).

## Build and test

```sh
cd firmware
pio test -e native            # unit tests for pack/, quiz/, srs/, imu/, catalog/, portal/ (host, no Arduino)
pio run -e sticks3            # build for StickS3
pio run -e tdisplay-s3        # build for T-Display-S3
```

Each build prints `Tangocho version: ...`: `scripts/version.py` takes
`$TGC_VERSION` (the release workflow sets the tag), else `git describe --tags
--always --dirty --match "v[0-9]*"`, else `dev`, and compiles it into
`src/Version.cpp` only (a new version recompiles one file). The device logs it
on boot (`Tangocho <version>`) and shows it under Settings › Device › About.

## Flash

```sh
pio run -e sticks3 -t upload      # firmware
pio run -e sticks3 -t uploadfs    # LittleFS image built from firmware/data/
pio device monitor
```

Use `-e tdisplay-s3` for the T-Display-S3. `-e sticks3-dev` is the StickS3 build
with the quiz trace (`TGC_QUIZ_LOG`) on the serial monitor: every question,
answer and grade, plus Wi-Fi and power events. `-e sticks3-shots` is the
StickS3 build with the serial remote (`TGC_REMOTE`): over the USB cable,
`tools/screenshots/remote.py` presses its buttons, saves its screen as PNG and
runs test hooks (the power-cut date question, a home Wi-Fi join that fails);
see [tools/screenshots/README.md](../tools/screenshots/README.md). Never flash
it as a release: anything on the cable can press the buttons.

### Prebuilt images from a release

A GitHub release carries, per board, the app-only `tangocho-<env>.bin` (the same
file `pio run` produces, e.g. for M5Launcher) and a **merged**
`tangocho-<env>-merged.bin` that `esptool write_flash 0x0 …` or a browser (ESP
Web Tools, via the release's `manifest.json`) can flash in one go. A merged image
carries **no filesystem**: the first boot formats the data partition when it
holds no LittleFS (erased or foreign), so a fresh device asks for its language
(English / Español) and boots into the Wi-Fi portal, where a pack can be sent
over the air. Offsets and the exact
`esptool merge_bin` command are in the root README; the merged image is **not
verified on hardware** yet (docs/decisions.md).

## Putting a pack on the device

Build a pack with `tools/pack-builder` (see its README), copy the `.tgc` into
`firmware/data/packs/` (packs are git-ignored) and run `uploadfs`. The firmware
opens the deck saved in NVS, or the first `*.tgc` if there is none; hold the
side button to switch decks. `uploadfs` replaces the whole filesystem, so keep
every pack you want in `data/packs/`, and note that it also clears review
progress (`/state/srs.bin`).

Alternatively, use the **Wi-Fi portal** with no `uploadfs` (root README, "Getting
decks onto the device"). **Settings → Wi-Fi »** (hold the side button, then hold
the row) has four rows: **Web portal** joins the saved home Wi-Fi and serves the
page there (home mode; the screen shows the address and a QR code of it), or
starts the device's own open access point when no network is saved; **Home:
<network>** tests the saved network (IP, signal, catalog, network time, then
radio off); **Setup mode** forces the access point (QR code to join it, a tap
for the page's QR, `http://192.168.4.1`); **Forget network** asks first. A
device with no pack boots straight into the portal: home mode with a saved
network (setup mode if the join fails), else setup mode. In setup mode the page
saves the home Wi-Fi and then offers **Switch to my Wi-Fi**; in home mode it
lists the deck catalog and downloads a deck with a progress view, and in both it
uploads or deletes packs, downloads one from an `http://` URL and shows the
statistics. Uploads and downloads are validated before they are kept, and the
side button stops the portal. See the root README for the security trade-off
(no password: anyone in range, or on the home network, while the screen is
open).

Or fetch decks on the device itself: **Decks -> + Get decks** joins the home
Wi-Fi only while that screen is open, reads the release catalog over HTTPS
(verified, redirects followed), lists the decks by title (`+` new, `=` here,
`~` other size) and downloads the one you hold, checked against the catalog's
size and SHA-256. The page's downloads use the same code. The side button
cancels a download; a click of the power button or a critical battery cancels
it and sleeps, and on battery below 6 % it does not start. A fresh device
offers the same from its boot screen once a home network is saved. The default
catalog (`releases/download/decks-v2/catalog-v2.txt`, catalog format v2) only
answers once the decks release carries it; the page can set another URL. A v1
`catalog.txt` is refused as an old format. Serial `[wifi]` and `[net]` lines
report each step and the free heap around TLS and around a download from the
page.

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
a tap shortcut; **holding it opens the settings home** (Continue / Decks / Date » /
New per day / Speed / Statistics / Wi-Fi » / Orientation / Language / Brightness
/ Sleep / About), where a tap moves the selection, a hold activates the row, and
the side button goes back (in landscape the list scrolls with the selection).
The screen headers no longer say so: going back is always the side button.
**Wi-Fi »** opens its own list (Web portal / Home network / Setup mode / Forget
network) that works the same way, and so does **Date »** (Date / Time zone, see
[Date](#date)). **About** shows the
firmware version and licence, the decks' licence, the repository, and the loaded
deck's name with its data credits; a tap turns the page when it needs two, the
side button goes back. The deck list's last row, **+ Get decks**, downloads decks
over the home Wi-Fi (tap moves, hold downloads or studies, side button
cancels/back).

**Language.** The screens are in English or Spanish. The first boot of a device
with no decks and no saved language asks once (**English** preselected /
**Español**; tap moves, hold chooses) and stores that as the Language setting,
so a deck in another language does not change it. **Settings → Language**
offers Auto (follow the loaded deck's language), English and Español. A device
that keeps its decks through a merged-image flash, which erases the settings,
starts in Auto.

## Orientation

Settings › Device › Orientation: on the StickS3, **Auto** (the default),
which follows the accelerometer: upright for portrait, turned either way for
landscape (the text is upright in both directions); or a fixed **Portrait**
or **Landscape**.
Upside down and lying flat keep the current layout. A turn takes effect after
about half a second, never in the middle of a button press, and does not count
as activity for dimming or sleep. Only the picture turns; the buttons keep their
roles. The T-Display-S3 has no IMU and offers only the fixed choices (portrait
by default).

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
while the portal in home mode, Get decks or the network test is open, or for a
few seconds while the date question after a power-off fetches the network time;
a download or a join counts as activity).

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
the Wi-Fi portal page from a phone (**Settings → Wi-Fi » → Web portal**, then
the address on the screen) sets the real date from the phone's clock and time
zone; from then on the day rolls over at 04:00 local and the statistics show the
real date (docs/srs.md §5.1). The page sends the zone as a rule with its
daylight-saving changes, so the rollover stays at 04:00 across them without
another visit.

After a real power-off the device has to find out the date again:

- **With a home Wi-Fi and a time zone**, it first tries the network: "Getting
  the date via <network>..." (up to 40 s to join, 3 s for NTP), then straight
  on to the quiz. If that fails, or you **tap** to skip it, the date screen
  comes up (with the reason when it failed).
- **Otherwise**, or then, the **date screen**: the last-used date and a
  proposed "today" with its weekday. **Tap: +1 day, side tap: -1 day, hold:
  OK, side hold: cancel** (keep the day as it is); the hint bar says so (in
  portrait the side button's two actions take a second row).

Going below the current day fixes a device that is ahead of the real date
(say, a day too many confirmed earlier): the reviews stay where they are, only
the date shown moves back, as when the network time finds the device ahead
(docs/srs.md §5). That needs a date the device has had from a real clock once
(the portal page or NTP); until then -1 day only undoes your own +1 days, and
"-1 day" is dimmed at today.

**Settings → Device → Date** has the same date screen for manual corrections and
**Time zone**: **From phone** (the zone the portal page last sent, shown with
its city) or one of 22 zones (UTC, London, Lisbon, Madrid, Athens, New York,
Chicago, Denver, Los Angeles, Mexico City, Bogota, Lima, Caracas, Santiago,
Buenos Aires, Sao Paulo, Kolkata, Shanghai, Seoul, Tokyo, Sydney, Auckland),
each with its offsets ("+1/+2": winter/summer); the zone in use is green. Use
it when no phone has set one, or the phone's is wrong. The next portal page
visit replaces a zone chosen here with the phone's. A new zone counts from the
next network time or page visit; it does not move the date by itself.

**Network time.** With a home Wi-Fi saved, the date also comes from NTP: the
portal in home mode, Get decks and the network test sync it once connected (at
most 3 s, a failure is only logged), and the power-off date question tries it
first as above. NTP gives UTC only, so all of these need the time zone and skip
the sync without one.

## Layout

`src/board` HAL (one impl per board), `src/pack` pack reader (pure C++),
`src/quiz` question builder (pure C++), `src/imu` orientation filter for the
Auto rotation (pure C++), `src/power` critical-battery guard (pure C++),
`src/catalog` deck-catalog parser (pure C++), `src/portal` the portal's pure
helpers (Wi-Fi QR text, QR size, page language) and its page strings in en/es,
`src/ui` the UX design tokens, icons, list state and the generated UI font
(pure C++), `src/render` glyph/text drawing and the flat widgets (header, hint
bar, tiles, rows, lists), `src/platform` LittleFS-backed pack source and
SRS file, the on-device Wi-Fi portal (`WifiPortal`: setup mode = open AP, home
mode = the home network; one HTTP page for the catalog, upload/delete/download,
stats, home Wi-Fi and the phone clock), the home network (`HomeWifi`) and the
shared HTTP(S) pack and catalog download (`NetFetch`, roots in `RootCerts.h`),
`src/main.cpp` the app.
