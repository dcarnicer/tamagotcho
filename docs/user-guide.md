# Tamagotcho user guide

The [README](../README.md) is the overview; this is the whole manual: how the
device works, the Wi-Fi portal, flashing, M5Launcher, the prebuilt decks and
how releases are made. The firmware side (build, flash layout, every control,
power and date handling) is in [firmware/README.md](../firmware/README.md).

## How it works

1. `tools/pack-builder` builds a **pack** (`.tgc`) from open data
   (KANJIDIC2), choosing a level (e.g. JLPT N5), meaning language and font
   (default: Noto Sans JP, a gothic like the one in printed texts). All glyphs
   are pre-rendered, so the device needs no Japanese font.
2. Upload the pack to the device.
3. Review: a kanji appears, pick the right meaning or reading from four
   options. Distractors are chosen to be genuinely confusable. A wrong answer
   opens a detail card that shows all meanings and readings — and animates the
   kanji's stroke order from KanjiVG, if the pack was built with `--strokes`.
   Controls: a tap of the front button moves the highlight down, a tap of the
   side button moves it up, and **holding the front button answers** (the
   option fills up while you hold); holding the side button opens the
   settings. After a correct answer the next card follows by itself.
4. Hold the side button for the **settings**: a summary of the loaded deck
   (due and new cards, how much of it you have seen) and five groups --
   **Decks** (switch decks, Get decks), **Study** (new cards per day, Hard
   after, auto-advance, on'yomi answers in katakana or hiragana, delete
   progress), **Stats**, **Wi-Fi** and **Device** (orientation,
   brightness, language, hold time, date and time zone, sleep, About). In
   every menu a tap moves, a hold opens or chooses, and the side button goes
   back; the bar at the bottom of the screen says what each does. The date
   screen is the one exception: there the front tap adds a day, the side tap
   takes one away, and holding the side button cancels.


## Getting decks onto the device (Wi-Fi portal)

The firmware has an **on-device web portal**: one page, opened from a phone or
laptop, to download decks, upload or delete them and read the statistics -- no
computer running `uploadfs`. **Wi-Fi is off by default**: the radio runs only
while the device's Wi-Fi screen is open (it stops after 5 min without use),
during Get decks, and for one network-time sync after a power cut.

The portal has two modes:

- **Home mode** (the usual one): the device joins **your Wi-Fi** and serves the
  page there, so the phone stays on its normal network. The device screen
  shows the network, the address (`http://<ip>/`) and a **QR code** of it;
  `http://tamagotcho.local/` usually works too.
- **Setup mode**: the device's own **open** network `Tamagotcho-XXXX` (the last
  bytes of its MAC) at `http://192.168.4.1/`, for the first setup or to change
  networks. The screen shows a QR code that joins it; a tap of the front button
  swaps it for one that opens the page.

### First setup

1. A brand-new device first asks for its **language**: **English** or
   **Español** (tap moves, hold chooses). The device screens and the portal
   page then stay in that language, whatever the language of the decks you
   download; **Settings → Device → Language** changes it later (Auto follows
   the loaded deck).
2. A device with no decks starts the portal by itself (later: hold the side
   button for **Settings → Wi-Fi → Setup mode**). Scan the QR code on the
   screen with the phone camera to join `Tamagotcho-XXXX`, then open
   `http://192.168.4.1` (or tap the device's front button and scan the second
   QR code).
3. The page's first step is **Connect the device to your Wi-Fi**: pick the
   network (nearby ones are suggested), type its password (empty for an open
   network) and press **Save**.
4. Press **Switch to my Wi-Fi**. The device leaves its own network and joins
   yours. Put the phone back on your Wi-Fi and scan the new QR code on the
   device screen (or type the address it shows).
5. The page now lists the decks of the catalog, **your language first**: press
   **Download** (or **Update** for a deck that is on the device in another
   build). The page and the device screen show the progress; the device's side
   button cancels. When you are done, the device's side button starts studying.

If the device cannot join your Wi-Fi, its screen says why ("Network not
found", "Wrong password?", "Could not connect"); on a device without decks it
goes back to setup mode by itself, otherwise a hold of the front button opens
setup mode and a tap tries again.

### Daily use

- **Settings → Wi-Fi → Web portal** joins your Wi-Fi and shows the address
  and QR code; open it from any phone or laptop on that network. The page has
  **Get decks** (download or update), **Your decks** (the one being studied is
  marked; delete, which deletes its progress too: see [Progress](#progress)),
  **Statistics** (including the ten kanji you miss most often: at least 3
  reviews and one miss, with their right/review counts) and, folded under
  **Advanced**: upload a
  `.tgc` file, download one from a plain `http://` URL, the deck catalog URL,
  the home Wi-Fi (change, forget, reconnect), the free space and the JSON at
  `/api/stats` and `/api/status`. The side button on the device stops the portal.
- Without the phone: **Settings → Decks → Get decks** joins the home Wi-Fi and
  lists the catalog's decks by title on the device screen (a check marks a deck
  that is here, a download mark one that is here in another build; tap moves,
  **hold downloads** or, on a deck that is here, studies it; the side button
  goes back or cancels a download). In setup mode the page also offers **Get
  decks on the device screen**, and a fresh device's boot screen offers it with
  a hold.
- With a home Wi-Fi saved, the device also **checks for newer builds of your
  decks**: at boot, once a day on USB power (or once a week on battery from
  50 %). A short "Checking deck updates..." screen can be skipped; the radio is
  off again afterwards. When something is new, **Decks** shows "N updates" and
  its **Updates** row opens the list of installed decks with a newer build;
  **hold** one to download and replace it. Progress is kept (it is per kanji),
  and the device never downloads an update by itself. The same row checks on
  demand when you want, on battery too.
- **Settings → Wi-Fi → Home** tests the saved network: it joins,
  shows the IP address, the signal, whether the deck catalog answers and
  whether the network time works, then turns the radio off again.
- Opening the page sets the device's **date** from the phone's clock and **time
  zone**, so the review day rolls over at 04:00 local -- daylight-saving changes
  included: the page sends the zone's rules, not just today's offset. With a
  home Wi-Fi saved the device also takes the time from NTP whenever it joins,
  and after a power-off it **tries the network first** ("Getting the date via
  <network>...", about 40 s at most; a tap skips it) before asking for the
  date by hand. NTP needs the time zone: the one the page last sent, or one
  chosen in **Settings → Device → Date → Zone** (a short list of zones, for when
  no phone has set one; the next page visit replaces it with the phone's).

### Progress

Progress belongs to each **kanji**, not to a deck: decks that share a kanji
share its progress, switching decks keeps it, and so does updating a deck.
**Deleting a deck** on the page also deletes the progress of its kanji, except
those another deck on the device has (N5's kanji keep theirs while the Jōyō
deck is installed), so downloading it again starts from scratch; the answers,
accuracy, time and streaks stay. On the device, **Settings → Study → Delete
progress** resets **This deck** (all of its kanji, shared ones too: the
confirmation says how many other decks share) or **Everything** (all progress
and the statistics; decks and settings stay). Both ask first, and only a hold
on the confirmation deletes.

### Changing networks

- Still at home: **Web portal → Advanced → Home Wi-Fi**, save the new network,
  then **Reconnect**.
- Elsewhere: **Settings → Wi-Fi → Setup mode**, join `Tamagotcho-XXXX`, save the
  new network on the page and press **Switch to my Wi-Fi**.
- **Settings → Wi-Fi → Forget Wi-Fi** asks first (Cancel is selected; tap to
  Forget, then hold).

### Notes and limits

- **Neither mode has a password.** In setup mode anyone in radio range, in home
  mode anyone on your home network, can list, upload or delete decks, read the
  statistics and replace the saved network or catalog URL **while the Wi-Fi
  screen is open** on the device. Open it when you need it and leave it
  afterwards (it also stops by itself after 5 min without use).
- The home Wi-Fi password is stored on the device (NVS, **not encrypted**) and
  is never shown again, logged or sent back to a page.
- Every catalog download is checked against the catalog's **size and SHA-256**
  and must be a valid pack before it replaces anything; a failure leaves
  `/packs` as it was. HTTPS is verified against a small embedded set of root
  certificates (Let's Encrypt, Sectigo/USERTrust, DigiCert, Google Trust
  Services, Amazon); see `docs/decisions.md`. A download is refused on battery
  below 6 %, and an update needs room for the old and the new copy at once.
- An uploaded file streams straight to the filesystem (a multi-MB pack never has
  to fit in RAM) and is kept only if it is a valid pack, with room to spare.
  The URL download is **plain `http://` only** (e.g. a server on your computer).
- The page is in the device's language (the Language setting, chosen on the
  first boot, or the loaded deck's under Auto); a device set to Auto with no
  deck yet follows the browser's language, and the page offers English /
  Español links.
- The default catalog is
  `https://github.com/dcarnicer/tamagotcho/releases/download/decks-v2/catalog-v2.txt`,
  in the dedicated **decks release** for the pack format this firmware reads (see
  [Prebuilt decks](#prebuilt-decks-and-their-licence) below). The page can point it at
  another catalog (any `http(s)://` URL, e.g. your own server); it must be
  catalog format v2 (with deck titles), and an older `catalog.txt` is refused as
  an old format.
- Everything else works offline.

## Flashing

### Flash over USB

```sh
cd firmware
pio run -e sticks3 -t upload      # app (also -e tdisplay-s3, -e cardputer-adv)
pio run -e sticks3 -t uploadfs    # LittleFS image built from data/
```

### Flash a merged image (esptool or a browser)

A release's `tamagotcho-sticks3-merged.bin` (and the `tdisplay-s3`,
`cardputer-adv` and, from v0.4.0, `m5stack` ones; `m5stack` runs on the
StickS3 and the Cardputer ADV alike and is the one on M5Burner) is one file
flashed at offset 0, so it needs no offset list (releases up to v0.2.1 name
their files `tangocho-…`):

```sh
esptool --chip esp32s3 write_flash 0x0 tamagotcho-sticks3-merged.bin
```

**What it overwrites:** everything from `0x0` to the end of the app, including
the **settings** (the NVS partition: home Wi-Fi, preferences, time zone), which
the merge pads with `0xFF`. The filesystem with the decks and the study
progress starts after the 2 MB app slot and is not touched. To **update** a
device and keep its settings, flash the app-only image at the app offset
instead:

```sh
esptool --chip esp32s3 write_flash 0x10000 tamagotcho-sticks3.bin
```

It is produced from the PlatformIO build by
[`tools/release/merge_image.py`](../tools/release/merge_image.py), which runs the
equivalent of (StickS3):

```sh
esptool --chip esp32s3 merge_bin -o tamagotcho-sticks3-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     .pio/build/sticks3/bootloader.bin \
  0x8000  .pio/build/sticks3/partitions.bin \
  0x10000 .pio/build/sticks3/firmware.bin
```

The offsets are the ESP32-S3 bootloader (`0x0`), the partition table (`0x8000`)
and the `app0` offset from `firmware/partitions/sticks3_8MB.csv`. Our partition
tables have **no OTA data partition**, so no `boot_app0.bin` is added.
PlatformIO's USB upload does write `boot_app0.bin` at `0xe000`, over the unused
`phy_init` partition; leaving it out is right for a table without OTA. The
bootloader header is written in DIO, as PlatformIO's upload does; the app
switches the flash to QIO itself (see `docs/decisions.md`).

For a **browser flash** (Chrome or Edge on a desktop), open Espressif's web
flasher <https://espressif.github.io/esptool-js/>, press **Connect** and pick
the StickS3's port, add `tamagotcho-sticks3-merged.bin` at address `0x0` and
press **Program**. The release also carries ESP Web Tools manifests
(`tamagotcho-*-manifest.json`) for a future install page; ESP Web Tools needs a
page that serves the image and the manifest together, so it cannot be pointed
at a release on its own. A merged image carries **no filesystem**:
on the first boot the firmware finds no LittleFS on the data partition (erased,
or left by another firmware), formats it ("Preparing storage..."), asks for the
language (English / Español) and boots into the Wi-Fi portal's setup mode,
where decks can be fetched or uploaded over the air. A device that still has
its decks after a merged-image flash does not ask: its settings are gone, so
the language follows the loaded deck (Auto) until you change it. The flash
counts as a power cut, so it does ask for the **date**, by hand: with the home
Wi-Fi and the time zone erased it cannot get it from the network first. Set it
with the stepper (or skip), then open the portal page once to save the Wi-Fi
and the zone again.

> Status: verified on a real StickS3 (2026-10-18) both ways. With **esptool**,
> a v0.1.0 candidate's merged image written at `0x0` booted, kept its decks and
> progress, and started with the settings erased. From the **browser**
> (esptool-js), Erase Flash and then the merged image at `0x0` gave a new
> device: language picker, setup mode, portal. The v0.3.0 merged images of the
> StickS3 and the T-Display-S3, written with esptool after an Erase Flash,
> booted as new devices on 2026-10-21 (`Tamagotcho v0.3.0`, storage formatted,
> language picker). The Cardputer ADV image is built but not verified on
> hardware; its firmware is, flashed with PlatformIO (2026-10-21).

## M5Launcher and app-only `.bin`s

`pio run -e sticks3` (or `-e tdisplay-s3`) produces an **app-only** image at
`.pio/build/sticks3/firmware.bin` (StickS3; `tdisplay-s3` for the other board).
Like any Arduino/PlatformIO app it begins with the ESP32 image magic `0xE9` and
contains no bootloader and no partition table; `esptool`/PlatformIO write it to
the app slot together with the bootloader and `partitions.bin`. The normal USB
path (`pio run -t upload` and `-t uploadfs`) is unchanged.

[M5Launcher](https://github.com/bmorcelli/Launcher) can install such app-only
binaries: copy `firmware.bin` to the SD card (or upload it through the
launcher's WebUI) and install it as an app. Launcher places the app image in an
OTA app partition and can attach its own SPIFFS/LittleFS data partition.

Honest status — **not verified on hardware**:

- We have not installed this firmware under M5Launcher on a real StickS3. The
  statements below come from the launcher's documentation, not from a test.
- Launcher manages **its own partition table**, not our
  `partitions/sticks3_8MB.csv`. An app-only binary carries no partition table,
  so the launcher decides the app offset/size and the data partition's
  label/size (its wiki says empty data partitions start at a 448 KB minimum
  unless enlarged). The Cardputer ADV's launcher table, seen on 2026-10-22,
  has a 1,344 KB app0 and a 2,496 KB `firmw1` OTA app slot: the 1.48 MB image
  does not fit app0, so it would have to be installed in `firmw1` (not tried).
- Our LittleFS partition as built by `uploadfs` (the partition labelled
  `spiffs`, 5.94 MB at `0x210000` on the StickS3) is **not necessarily the same
  partition** a launcher-managed app sees. The Cardputer's launcher data
  partition was 448 KB, far too small for a multi-MB pack: the N5 deck might
  fit, the larger decks will not. Whether the launcher's partition carries a
  label our firmware mounts is still an **open question** to settle on
  hardware.
- Because of that, the reliable way to populate `/packs` under a
  launcher-installed app is the **Wi-Fi portal above**, once the firmware is
  running — it does not depend on who owns the filesystem.
- On the StickS3 the firmware turns the power button's **click into "save and
  sleep"** by switching off the M5PM1's click reset (see
  [firmware/README.md](../firmware/README.md#power)). The PM1 keeps that setting
  after you leave the app, so under the launcher a click no longer restarts the
  device. A double click still powers it off, and a click (or plugging in USB)
  should power it back on. Not yet verified on hardware.

## Your own decks

`tools/pack-builder` can also build a deck from a list of kanji you choose, for
a textbook chapter, a class list or a set you are studying. Write one kanji per
line to a UTF-8 text file (blank lines and `#` comments are fine), then build:

```sh
printf '日\n月\n火\n' > my-kanji.txt
cd tools/pack-builder
uv run tamagotcho-pack build --kanji-file my-kanji.txt --lang es -o out/my-kanji.tgc
```

The lines are the order the device introduces new cards in; `--order jlpt` or
`--order grade` changes that. Duplicates are ignored, a line that is not
exactly one character stops the build with its line number, and a kanji
KANJIDIC2 does not have is skipped with a warning. The deck name defaults to
the file name without its extension (made safe for the device's ASCII header;
`--name` overrides it). Upload the resulting `.tgc` through the portal
(Advanced) like any other deck. The
[pack builder's README](../tools/pack-builder/README.md) documents every option
and the licence of the data.

## Prebuilt decks and their licence

Prebuilt decks live in their own GitHub release with a fixed tag,
**`decks-v<pack format version>`** (`decks-v2`), published by
[`.github/workflows/decks.yml`](../.github/workflows/decks.yml) when the maintainer
runs it (manual dispatch only). That release is **never marked "latest"**, so a
firmware release never hides it, and a rebuild replaces its files in place. It
holds `*.tgc` packs built with `--strokes` — by default JLPT N5 to N1 and the
full **Jōyō** deck (2,136 kanji, in school order), each in Spanish and English
(`n5-es.tgc` ... `n1-en.tgc`, `joyo-es.tgc`, `joyo-en.tgc`) — and the
line-oriented `catalog-v2.txt` the device reads for **Get decks**, with each
deck's title (format: `tools/pack-builder/README.md`), plus a `catalog-v2.json`
for people. The `v2` in the catalog's name is the *catalog* format; the one in
the tag is the *pack* format. Firmware reads the release of its own pack
format, so a future format gets a new `decks-v<N>` release and older firmware
keeps the decks it can open.

The workflow's inputs are the JLPT levels (`5 4 3 2 1`), the meaning languages
(`es en`), whether to build the Jōyō deck, and whether to publish (off: build,
check and keep the packs as a workflow artifact). It derives the tag from the
packs' format version and fails if the packs disagree on it or if any pack is
over the device's 5 MiB per-pack limit.

To install a deck by hand, download a `.tgc` from that release and upload it
from the Wi-Fi portal page (Advanced). A pack contains **KANJIDIC2** data (CC
BY-SA 4.0) and, with `--strokes`, **KanjiVG** data (CC BY-SA 3.0), so prebuilt
packs are licensed **CC BY-SA**. The combined attribution is in the decks release notes
and in [docs/data-sources.md](data-sources.md); the device shows it under
**Settings › Device › About** (with the loaded deck's name) and the portal page
in its footer.

## Releases

Every push to `main` and every pull request runs
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml): a firmware job (native
tests plus StickS3, T-Display-S3 and Cardputer ADV builds, with `~/.platformio` cached and the
cache keyed on the PlatformIO version) and a pack-builder job (`uv run pytest`).
A newer push on the same ref cancels the superseded run.

[`.github/workflows/release.yml`](../.github/workflows/release.yml) runs on tags
`v*` and on manual dispatch. It builds, per board, an **app-only** image and a
**merged, single-file** image, writes an **ESP Web Tools** `manifest.json`, and
publishes a GitHub release with those artifacts. Firmware releases carry **no
packs**; their notes link to the decks release below.

**Version.** Every firmware build carries a version string, shown under
**Settings › Device › About**, in the portal page's footer, as `"version"` in
`/api/stats` and `/api/status`, and as the first boot log line
(`Tamagotcho <version>`). [`firmware/scripts/version.py`](../firmware/scripts/version.py)
sets it: the `TGC_VERSION` environment variable if present (the release
workflow passes the tag), else `git describe --tags --always --dirty --match
"v[0-9]*"` (e.g. `v0.1.0-3-gabc1234-dirty`, or just the commit id when no `v*`
tag is reachable, as in CI's shallow checkout), else `dev`. To label a local
build by hand: `TGC_VERSION=v0.1.0-rc1 pio run -e sticks3`.

**Making a release** (the maintainer):

1. Write `docs/releases/vX.Y.Z.md`, the "What's new" the workflow puts in the
   notes.
2. Merge into `main`, push, then tag `vX.Y.Z` and push the tag; `release.yml`
   builds and publishes the images.
3. **Publish it on M5Burner**: add version `vX.Y.Z` to the Tamagotcho entry
   with the release's `tamagotcho-m5stack-merged.bin` (StickS3 and Cardputer
   ADV), as [docs/m5burner.md](m5burner.md) describes. M5Burner does not
   follow GitHub: without this step it keeps offering the previous version.
