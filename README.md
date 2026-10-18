# Tangochō 単語帳

Kanji flashcards on your wrist-sized ESP32. Tangochō turns an
[M5Stack StickS3](https://docs.m5stack.com/) into a ring-bound kanji card deck
with **multiple-choice reviews** and spaced repetition, fully offline.

> Status: early development. See [docs/roadmap.md](docs/roadmap.md).

## How it works

1. `tools/pack-builder` builds a **pack** (`.tgc`) from open data
   (KANJIDIC2), choosing a level (e.g. JLPT N5), meaning language and font.
   All glyphs are pre-rendered, so the device needs no Japanese font.
2. Upload the pack to the device.
3. Review: a kanji appears, pick the right meaning or reading from four
   options. Distractors are chosen to be genuinely confusable. A wrong answer
   opens a detail card that shows all meanings and readings — and animates the
   kanji's stroke order from KanjiVG, if the pack was built with `--strokes`.

## Loading packs over Wi-Fi

The firmware includes an **on-device Wi-Fi portal**, so a phone or laptop can
copy a pack onto the device without a computer running `uploadfs`:

1. On the device, hold the side button to open **Settings** and pick
   **Wi-Fi** (the portal also starts by itself when the device boots with no
   pack at all). The screen shows the access point name (`Tangocho-XXXX`, the
   last bytes of the device MAC) and `http://192.168.4.1`.
2. Join that access point from the phone (it is **open**, no password) and open
   `http://192.168.4.1` in a browser.
3. Upload a `.tgc` with the file picker. The same page deletes a pack, downloads
   one from a plain `http://` URL, and shows the SRS statistics; the counters
   and the pack list are also available as JSON at `/api/stats`. Opening the
   page also sets the device's date from the phone's clock and time zone, so the
   review day rolls over at 04:00 local. Once the date has been set this way,
   a power-off can be handled by skipping the on-device date question (side tap)
   and opening this page again.
4. Tap the side button to leave the screen and stop the portal.

Notes and limits:

- The access point is **intentionally open**: anyone in radio range can list,
  upload or delete packs and read the statistics. Start the portal when you
  need it and stop it afterwards; do not leave it running untrusted.
- An uploaded file is streamed straight to `/packs` (a multi-MB pack never has
  to fit in RAM), then checked for the `TNGC` magic and a valid pack header. A
  file that is not a pack is deleted again, so a bad upload cannot strand the
  device, and there must be enough free space (the portal keeps a reserve).
- **The page's URL download is plain `http://` only**: on its own access point
  the device has no internet, so this is for a server on the phone/laptop side.
  For decks from the internet use **Get decks** below. A failed download leaves
  the existing packs untouched.
- On a fresh device (or one whose filesystem a launcher formatted) with no
  packs, the firmware boots straight into the Wi-Fi screen with instructions
  and continues into the quiz as soon as a pack arrives.
- Everything works offline as before; the portal is only started on the Wi-Fi
  screen.

## Getting decks with one button (home Wi-Fi)

Once the device knows your home Wi-Fi it can download decks from the release
catalog by itself:

1. Open the Wi-Fi portal as above and, on the `http://192.168.4.1` page, fill in
   **Home Wi-Fi** (network name -- nearby networks are suggested -- and
   password; empty for an open network) and press **Save**. Stop the portal.
2. On the device: hold the side button -> **Decks** -> the last row,
   **+ Get decks** (**+ Descargar** in Spanish), and hold.
3. It joins the home network, reads the catalog over HTTPS and lists the decks:
   `+` not on the device yet, `=` already here, `~` here but a different size
   (another build). Tap moves, **hold downloads** (with a progress bar; the side
   button cancels), and a hold on a deck that is already here starts studying it.
   The side button goes back.

Notes and limits:

- The radio is on **only while this screen is open**, and the device sleeps as
  usual when idle. A fresh device without packs also offers it on its boot
  screen ("hold: get decks") once a home Wi-Fi is saved.
- Every download is checked against the catalog's **size and SHA-256** and must
  be a valid pack before it replaces anything; a failure leaves `/packs` as it
  was. HTTPS is verified against a small embedded set of root certificates
  (Let's Encrypt, Sectigo/USERTrust, DigiCert, Google Trust Services, Amazon);
  see `docs/decisions.md`.
- The password is stored on the device (NVS, **not encrypted**) and is never
  shown again; the page only shows the saved network name and a "forget"
  button. While the portal runs, anyone in range could replace (not read) it.
- The default catalog is
  `https://github.com/dcarnicer/tangocho/releases/latest/download/catalog.txt`,
  which only works once the newest release of a **public** repository carries
  prebuilt packs and `catalog.txt` (a maintainer decision, see "Before
  publishing" in the roadmap); until then the device says no catalog is
  published. The portal page can point it at another catalog (any `http(s)://`
  URL, e.g. a release pinned to a tag or your own server). **Not verified on
  hardware yet.**

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
  unless enlarged). Our app slot is 2 MB and the build is ~1.2 MB, so it should
  fit a launcher-created app partition, but we have not confirmed it.
- Our LittleFS partition as built by `uploadfs` (the partition labelled
  `spiffs`, 5.94 MB at `0x210000` on the StickS3) is **not necessarily the same
  partition** a launcher-managed app sees, and 448 KB is far too small for a
  multi-MB pack. Whether the launcher's partition carries a label our firmware
  mounts and is large enough to hold packs is an **open question** to settle on
  hardware.
- Because of that, the reliable way to populate `/packs` under a
  launcher-installed app is the **Wi-Fi upload above**, once the firmware is
  running — it does not depend on who owns the filesystem.
- On the StickS3 the firmware turns the power button's **click into "save and
  sleep"** by switching off the M5PM1's click reset (see
  [firmware/README.md](firmware/README.md#power)). The PM1 keeps that setting
  after you leave the app, so under the launcher a click no longer restarts the
  device. A double click still powers it off, and a click (or plugging in USB)
  should power it back on. Not yet verified on hardware.

## Releases

Every push to `main` and every pull request runs
[`.github/workflows/ci.yml`](.github/workflows/ci.yml): a firmware job (native
tests plus StickS3 and T-Display-S3 builds, with `~/.platformio` cached and the
cache keyed on the PlatformIO version) and a pack-builder job (`uv run pytest`).
A newer push on the same ref cancels the superseded run.

[`.github/workflows/release.yml`](.github/workflows/release.yml) runs on tags
`v*` and on manual dispatch. It builds, per board, an **app-only** image and a
**merged, single-file** image, writes an **ESP Web Tools** `manifest.json`, and
publishes a GitHub release with those artifacts. Prebuilt packs and a catalog are
**opt-in** (a dispatch input), because they redistribute CC BY-SA data; when they
are attached, the attribution is added to the release notes automatically.

### Flash over USB

```sh
cd firmware
pio run -e sticks3 -t upload      # app (also -e tdisplay-s3)
pio run -e sticks3 -t uploadfs    # LittleFS image built from data/
```

### Flash a merged image (esptool or a browser)

A release's `tangocho-sticks3-merged.bin` (and the `tdisplay-s3` one) is one file
flashed at offset 0, so it needs no offset list:

```sh
esptool --chip esp32s3 write_flash 0x0 tangocho-sticks3-merged.bin
```

It is produced from the PlatformIO build by
[`tools/release/merge_image.py`](tools/release/merge_image.py), which runs the
equivalent of (StickS3):

```sh
esptool --chip esp32s3 merge_bin -o tangocho-sticks3-merged.bin \
  --flash_mode qio --flash_freq 80m --flash_size 8MB \
  0x0     .pio/build/sticks3/bootloader.bin \
  0x8000  .pio/build/sticks3/partitions.bin \
  0x10000 .pio/build/sticks3/firmware.bin
```

The offsets are the ESP32-S3 bootloader (`0x0`), the partition table (`0x8000`)
and the `app0` offset from `firmware/partitions/sticks3_8MB.csv`. Our partition
tables have **no OTA data partition**, so no `boot_app0.bin` is added — exactly
what PlatformIO flashes for the same table (see `docs/decisions.md`).

For a **browser flash**, open an ESP Web Tools page (e.g.
<https://esphome.github.io/esp-web-tools/>) and point it at the release's
`tangocho-sticks3-manifest.json`. A merged image flashes an **empty
filesystem**, so a fresh device boots straight into the Wi-Fi upload screen and
a pack can be dropped in over the air.

> Honest status — **not verified on hardware.** We have not flashed a merged
> image through ESP Web Tools or `esptool write_flash` on a real StickS3. The
> offsets come from the partition table and the build output and mirror
> `pio run -t upload`, but the browser path is unconfirmed until someone tries
> it.

### Prebuilt packs and their licence

When the maintainer asks for them (a release-workflow input), a release can
attach `*.tgc` packs built with `--strokes` (for example JLPT N5 in English and
Spanish) together with a line-oriented `catalog.txt` — the small catalog the
device reads for **Get decks** (format: `tools/pack-builder/README.md`). A pack
contains **KANJIDIC2** data (CC BY-SA 4.0) and, with `--strokes`, **KanjiVG**
data (CC BY-SA 3.0), so prebuilt packs are licensed **CC BY-SA**. The combined attribution is in the release notes and
in [docs/data-sources.md](docs/data-sources.md).

## Repository layout

| path                  | what                                    |
|-----------------------|-----------------------------------------|
| `firmware/`           | PlatformIO firmware (StickS3, T-Display-S3) |
| `tools/pack-builder/` | Python CLI that builds `.tgc` packs     |
| `tools/release/`      | release helper: merge a build into one flashable image |
| `.github/workflows/`  | CI and release automation               |
| `docs/`               | architecture, pack format, decisions, roadmap |

## Licence

Code: MIT. Generated packs contain data from KANJIDIC2 (© EDRDG, CC BY-SA 4.0)
and glyphs from the selected font (default Klee One, SIL OFL 1.1). See
[docs/data-sources.md](docs/data-sources.md).
