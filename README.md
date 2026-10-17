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
   options. Distractors are chosen to be genuinely confusable.

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
   and the pack list are also available as JSON at `/api/stats`.
4. Tap the side button to leave the screen and stop the portal.

Notes and limits:

- The access point is **intentionally open**: anyone in radio range can list,
  upload or delete packs and read the statistics. Start the portal when you
  need it and stop it afterwards; do not leave it running untrusted.
- An uploaded file is streamed straight to `/packs` (a multi-MB pack never has
  to fit in RAM), then checked for the `TNGC` magic and a valid pack header. A
  file that is not a pack is deleted again, so a bad upload cannot strand the
  device, and there must be enough free space (the portal keeps a reserve).
- **URL download is HTTP only.** TLS to arbitrary hosts is untested and may
  fail on flash/memory, so `https://` is refused rather than silently tried; a
  failed download leaves the existing packs untouched.
- On a fresh device (or one whose filesystem a launcher formatted) with no
  packs, the firmware boots straight into the Wi-Fi screen with instructions
  and continues into the quiz as soon as a pack arrives.
- Everything works offline as before; the portal is only started on the Wi-Fi
  screen.

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

## Repository layout

| path                  | what                                    |
|-----------------------|-----------------------------------------|
| `firmware/`           | PlatformIO firmware (StickS3, T-Display-S3) |
| `tools/pack-builder/` | Python CLI that builds `.tgc` packs     |
| `docs/`               | architecture, pack format, decisions, roadmap |

## Licence

Code: MIT. Generated packs contain data from KANJIDIC2 (© EDRDG, CC BY-SA 4.0)
and glyphs from the selected font (default Klee One, SIL OFL 1.1). See
[docs/data-sources.md](docs/data-sources.md).
