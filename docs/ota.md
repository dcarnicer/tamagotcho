# OTA firmware updates: design and decisions

> **Status: released in v0.7.0 (2026-10-24); v0.7.1 (2026-10-27) is the
> first release that arrives over the air.** The install path was checked on
> a Cardputer ADV with a temporary release: manifest and image over HTTPS,
> install into `app1`, the image marked valid after 10 s, and decks and
> progress intact. The v0.7.1 assets and manifest were verified, and the
> install page serves v0.7.1; installing from the published release on a
> device is the remaining check. The maintainer took the recommendation of
> every decision of §8 (D1, D2, D3 and D6/D8 asked explicitly; D4, D5 and D7
> as recommended). §3.1 is the manifest's spec, the contract between
> `release.yml` and the firmware. Phases 1 (the tables, `TGC_ENV`, the
> manifest in `release.yml`, the docs) and 2 (the device side, §9 "As built")
> of §6 are built and integrated.
> §1 describes the layout before phase 1. Roadmap: "OTA firmware updates"
> (`docs/roadmap.md`, idea of 2026-10-21).

The device should update its own firmware over the home Wi-Fi: it finds out
that a newer release exists, says so, and installs it when the user holds a
button. Settings, decks and progress stay. A bad image must never leave the
device unable to boot.

Non-goals: installing without the user asking; updating devices that run
under M5Launcher (they keep the launcher's own path); signing images (the
trust is the same as Get decks': GitHub over HTTPS, §3.7).

This document covers: the facts that constrain the design (§1), the flash
layout and the one-time move to it (§2), the design (§3), what the screens
show (§4), the code (§5), a phased plan (§6), what only hardware can answer
(§7), the decisions to take (§8) and what building the device side changed
(§9).

## 1. Today

**Flash layout** (`firmware/partitions/*.csv`): one app slot and no OTA data,
so nearly all flash is LittleFS.

| | 8 MB (StickS3, Cardputer ADV, `m5stack`) | 16 MB (T-Display-S3) |
|---|---|---|
| `nvs` | `0x9000`, 20 KB | same |
| `phy_init` (unused) | `0xE000`, 4 KB | same |
| `app0` (factory) | `0x10000`, 2 MiB | same |
| `spiffs` (LittleFS) | `0x210000`, 5.94 MiB | `0x210000`, 13.94 MiB |

**App sizes** (v0.6.0): `m5stack` 1,508,720 B, `sticks3` 1,503,216,
`cardputer-adv` 1,451,184, `tdisplay-s3` 1,344,128. The 2 MiB slot has
about 0.5 MiB free.

**What is where.** Settings and the home Wi-Fi are in NVS (namespace
`tangocho`); decks (`/packs`) and progress (`/state/srs.bin`) are on
LittleFS. Writing a merged image at `0x0` erases NVS too: `esptool merge_bin`
fills the gap between the partition table and the app with `0xFF` (hence
M5Burner's "Burning erases the settings"). Writing the parts at their own
offsets, as `pio run -t upload` does, keeps NVS.

**The framework already has what OTA needs.** Arduino-ESP32 2.0.17 (IDF 4.4,
`espressif32@6.12.0`):
- Its precompiled bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a
  new image boots in the `PENDING_VERIFY` state and, if it is not marked
  valid before the next reset, the bootloader goes back to the previous one.
- `initArduino()` marks a pending image valid at once, unless the app
  defines `verifyRollbackLater()` to return true; then the app decides when
  (`esp_ota_mark_app_valid_cancel_rollback()`).
- The `Update` library writes the next OTA slot sector by sector (erasing as
  it goes), writes the image's first 16 bytes last so a half-written image
  can never boot, checks the size and the image's magic byte and calls
  `esp_ota_set_boot_partition()`. The bootloader then checks the image's own
  checksum and SHA-256 (every build carries both) before running it.
  `Update.canRollBack()` / `rollBack()` switch back to the other slot.
- `esp_ota_get_last_invalid_partition()` tells, after a rollback, that one
  happened.

**The network side exists too.** `platform/NetFetch` downloads over HTTPS
against the root CAs of `RootCerts.h`, follows GitHub's redirects, checks
size and SHA-256 while streaming, reports progress and can be cancelled; it
writes into a temp file only. Measured on the Cardputer ADV (no PSRAM),
v0.6.0: 278 KB in 9.6 s including the TLS handshake (about 30 KB/s), so a
1.5 MB image would take about a minute there.

**GitHub gives a fixed URL for the latest release:**
`https://github.com/dcarnicer/tamagotcho/releases/latest/download/<asset>`
answers `302` to `/releases/download/<tag>/<asset>`, which answers `302` to
`release-assets.githubusercontent.com`, the same host Get decks already
downloads from (checked 2026-10-24 with the v0.6.0 assets). The decks
releases are never marked latest, so "latest" is always a firmware release.

**Updating today** is USB only: `pio run -t upload` (all parts), `esptool
write_flash 0x10000 tamagotcho-<board>.bin` (app only; keeps everything),
the merged image from the install page or M5Burner (erases settings). PIO's
upload already writes `boot_app0.bin` (the OTA data record) at `0xE000`, over
the unused `phy_init`.

## 2. Flash layout with two app slots

OTA needs two app slots (one runs, the other receives) and an `otadata`
partition (which slot boots). The space comes from LittleFS; the app slots
stay together at the start, so the data partition moves.

**Proposed 8 MB table** (StickS3, Cardputer ADV, `m5stack`):

| name | type | offset | size |
|---|---|---|---|
| `nvs` | data, nvs | `0x9000` | `0x5000` (20 KB, unchanged) |
| `otadata` | data, ota | `0xE000` | `0x2000` |
| `app0` | app, ota_0 | `0x10000` | `0x1C0000` (1.75 MiB) |
| `app1` | app, ota_1 | `0x1D0000` | `0x1C0000` (1.75 MiB) |
| `spiffs` | data, spiffs | `0x390000` | `0x470000` (4.44 MiB) |

**Proposed 16 MB table** (T-Display-S3): `app0` and `app1` of 2 MiB at
`0x10000` and `0x210000`, `spiffs` at `0x410000` with `0xBF0000`
(11.94 MiB).

`nvs` keeps its offset and size, so settings and the home Wi-Fi survive the
change. `app0` keeps its offset, so an app-only image written at `0x10000`
is still slot 0. `phy_init` goes: the framework builds the PHY init data into
the app (`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` is off) and keeps the
calibration in NVS.

**Slot size** (decision D1). 1.75 MiB leaves 326 KB over today's largest
app, before the OTA code (an estimated 20-40 KB: `Update` and the screen).
The slot size can only change with another USB flash, so it errs on the
large side. The alternatives:

| slot | LittleFS (8 MB) | headroom over 1.51 MB | lost vs today |
|---|---|---|---|
| 1.625 MiB | 4.69 MiB | 195 KB | 1.25 MiB |
| **1.75 MiB** | **4.44 MiB** | **326 KB** | **1.50 MiB** |
| 1.875 MiB | 4.19 MiB | 457 KB | 1.75 MiB |

**What still fits on an 8 MB board** with 4.44 MiB (`decks-v3` sizes; the
96 KB write reserve and the state file aside): N5 to N1 in one language
(3.59 MiB); the Jōyō deck (3.27 MiB) with N5 and N4; N5 to N3 in both
languages. What no longer fits: the Jōyō deck with N1 (5.27 MiB), which fits
today. The T-Display-S3 keeps room for every deck of one language.

### 2.1 Moving to the new layout (once, over USB)

The first release with the new table (call it v0.7.0) cannot arrive over
the air: the partition table, the bootloader's view of the slots and
LittleFS's position all change. It is flashed over USB once; every later
release can come over the air.

LittleFS records its size in its superblock, and the partition now starts
elsewhere, so the old filesystem does not mount in the new partition: the
firmware formats it (as on a blank device). **Decks and progress are lost
unless saved first; settings and Wi-Fi stay** when the parts are written at
their offsets (`pio run -t upload`, or `esptool write_flash` with the
bootloader, partition table, `boot_app0.bin` and app), and are lost with a
merged image.

Two ways to keep them (decision D2):
- **(a) By hand, with what exists.** Before flashing: the portal's
  **Advanced → Progress backup**. After: Get decks again, then restore the
  backup on the portal. No code; a few minutes for the maintainer, the only
  user (docs/decisions.md, no backward compatibility).
- **(b) A host tool** (`tools/ota-migrate`): reads the old filesystem over
  USB (`esptool read_flash`), copies its files into a new LittleFS image of
  the new size with `littlefs-python` (dropping decks that no longer fit,
  never the state), and writes table, `boot_app0.bin`, app and filesystem.
  Keeps everything, but needs `esp_littlefs`'s exact geometry and a careful
  hardware test; worth it only if there are other users to migrate.

M5Burner and the install page carry the merged image, which erases
everything anyway; their first OTA-capable version is a fresh install.

## 3. Design

### 3.1 The release side

Each firmware release gets one more asset, **`tamagotcho-firmware.txt`**, a
line-oriented manifest in the spirit of the deck catalog. The device reads it
at `https://github.com/dcarnicer/tamagotcho/releases/latest/download/tamagotcho-firmware.txt`
(the default; §3.8 for the override). `release.yml` writes it from the
app-only images it already builds, and fails if an image is larger than its
slot (read from the partition CSV, as `decks.yml` reads the per-pack cap from
`Catalog.h`).

```
#tamagotcho-firmware	1
version	v0.7.0
app	sticks3	tamagotcho-sticks3.bin	1503216	<sha256 hex>
app	tdisplay-s3	tamagotcho-tdisplay-s3.bin	1344128	<sha256 hex>
app	cardputer-adv	tamagotcho-cardputer-adv.bin	1451184	<sha256 hex>
app	m5stack	tamagotcho-m5stack.bin	1508720	<sha256 hex>
```

**Manifest format, version 1** (spec):
- UTF-8, lines end in LF (a CR before it is ignored), fields separated by one
  TAB. At most 8 KiB; a longer body is not a manifest.
- **Line 1** is the header: `#tamagotcho-firmware`, TAB, the format version.
  A reader of version 1 refuses any other version ("unsupported manifest
  version") and anything without the header ("not a firmware manifest").
- After it, blank lines and lines starting with `#` are ignored.
- **`version`**, exactly once, before any `app` line: the release tag,
  `v<major>.<minor>.<patch>` with three non-negative decimal integers and
  nothing else. A manifest whose version does not parse that way is refused.
- **`app`**, one per build environment: `app`, the environment name
  (`[a-z0-9-]{1,31}`, the PlatformIO env, e.g. `m5stack`), the image's file
  name or URL, its exact size in bytes (decimal, greater than 0), and its
  SHA-256 as 64 hexadecimal characters (either case). A file name without a
  scheme is resolved against the manifest's own URL, as the catalog's `url`
  field is (`.../download/v0.7.0/tamagotcho-firmware.txt` gives
  `.../download/v0.7.0/<file>`); an `http://` or `https://` URL is used as it
  is. A second `app` line for the same environment makes the manifest
  invalid.
- Any other first field is ignored, so later versions of the format can add
  record types without a version bump; a malformed `version` or `app` line
  makes the whole manifest invalid (no partial reads).
- The image a row names is the app-only image (`firmware.bin` of that
  environment, ESP image magic `0xE9`), the same file the release publishes
  as `tamagotcho-<env>.bin`.

### 3.2 Which image a device installs

The one its own build came from: every environment compiles in its name
(`TGC_ENV`, e.g. `"m5stack"`), and the device takes that row; no row for it
means no update. A StickS3 installed from M5Burner runs `m5stack` and stays
on it; one flashed with `pio run -e sticks3` stays on `sticks3`.

Development builds (a version that is not a plain `vX.Y.Z`, e.g.
`v0.6.0-12-gabc1234`, or `dev`) never read the default manifest: their image
would drop the serial remote or the test code. They read a manifest only
when its URL has been overridden (§3.8), which is how updates are tested;
then any manifest version different from the running one is offered, since a
dev build has no order to compare.

### 3.3 When the device looks

Decision D3. Recommended: **with the deck update checks**
(docs/deck-updates.md): every home-Wi-Fi session that reads the deck catalog
(the boot-time check by its battery policy, Get decks, the portal page, the
network test, the date sync) also reads the firmware manifest, one more
small HTTPS request in the same Wi-Fi session (a few seconds, most of it the
TLS handshake). The result is a badge, as for
decks: **Device** and its **Firmware** row show "v0.7.0". A manual **Check
now** on the Firmware screen covers the rest. Alternative: only the manual
check (simpler, but nobody would ever notice an update).

Version order: `vMAJOR.MINOR.PATCH` compared numerically; a manifest version
that does not parse is ignored. Only a newer version is offered; no
downgrades (the other slot's rollback, §3.6, is the way back).

### 3.4 Installing

On **Settings › Device › Firmware**:
1. The screen shows the running version, the available one and its size
   (or "up to date", or the last check's error).
2. **Preconditions**, checked before anything is written: the home Wi-Fi is
   saved; the battery is at least at the level the deck downloads require
   (`BatteryGuard::canResume`) or the device is on USB; the partition table
   is ours (§3.7). The download is refused otherwise, with the reason.
3. **Hold to install.** The screen keeps the panel on and shows a progress
   bar; the side button (Esc on the Cardputer) cancels until the last
   moment; a click of the power button cancels and sleeps, as a deck
   download does.
4. The body streams from NetFetch into `Update` (no file on LittleFS, so no
   free space is needed); size and SHA-256 are checked against the manifest
   while it streams; `Update.end()` checks the image and switches the boot
   slot only when everything matched.
5. "Restarting into v0.7.0..." and a restart. Any failure before the switch
   leaves the running slot as the boot slot and shows the reason; the
   half-written slot is simply overwritten next time.

The quiz is never interrupted: the install starts only from this screen, by
hand.

### 3.5 The first boot of a new image

The image defines `verifyRollbackLater()` to return true, so it boots
pending. It marks itself valid once it has shown it works (decision D5):
`setup()` returned (LittleFS mounted, a deck opened or the no-deck screens
reached) and then the loop has run for 10 s. A crash or a watchdog reset
before that sends the next boot back to the previous image (the bootloader's
rollback). A deep sleep before the 10 s (a click of the power button) is not
a failure: the image stays pending and the 10 s start again at the wake.

The previous image, booted again, sees `esp_ota_get_last_invalid_partition()`
and says once: "The update to v0.7.0 failed; back on v0.6.x". It then stops
offering that version (it remembers it in NVS), so a broken release does not
loop.

### 3.6 Going back by hand

After a good update the other slot still holds the previous version.
**Firmware › Previous version** (only when `Update.canRollBack()`) switches
back to it and restarts, for a release that boots but misbehaves (decision
D6; optional). Progress is not an obstacle while the SRS file format stays
the same; a release that changes it must say so, and the rollback row then
warns or is hidden.

### 3.7 Trust and safety

- **Integrity and origin**: the manifest comes over HTTPS from GitHub (the
  root CAs of Get decks); the image's size and SHA-256 come from the
  manifest and are checked before the slot switches. No signing: anyone who
  can publish a release on the repository can already publish decks.
- **Only our layout**: OTA is offered only when the running partition is
  `app0` or `app1` of our table (labels and sizes) and the next update slot
  is the other one. Under M5Launcher, or with any other table, the Firmware
  screen says to update the way the device was installed.
- **Power**: the battery precondition, the panel kept on, the power-button
  cancel. A power cut while writing leaves the running slot bootable.
- **Memory**: TLS without PSRAM already works for Get decks on the
  Cardputer ADV; `Update` needs one 4 KB sector buffer.
- **The USB paths keep working**, with one change to the instructions: after
  an OTA the device may boot `app1`, so an app-only `write_flash 0x10000`
  must also write `boot_app0.bin` at `0xE000` (or erase `otadata`), or the
  new image is not the one that boots. `pio run -t upload` and the merged
  images already do; the release publishes `boot_app0.bin` for the rest
  (docs/user-guide.md, "Flash a merged image").

### 3.8 The portal and development

- The portal page shows the firmware version and, when known, the available
  one, with "install it from the device" (decision D7: the page does not
  start the install; the device's screen and buttons do).
- **The manifest URL can be overridden** on the portal's Advanced section,
  like the catalog URL (NVS `fwurl`; empty = the default), with `http://`
  allowed there, so updates can be tested from a manifest and images served
  on the computer before anything is published. This is part of the first
  version: phase 2's hardware tests need it.
- **For development** (decision D8, a later phase): the portal accepts an app
  image upload (`POST /firmware`, the same `Update` path, with the page's
  size and an optional SHA-256), so a build can be installed over the Wi-Fi
  without a cable, e.g. `curl -F firmware=@.pio/build/sticks3/firmware.bin
  http://<device>/firmware`.

## 4. Screens

Settings › Device gains one row, **Firmware**, before About, with the
running version as its value and an "update" badge like the Decks row's.
The Firmware screen (StickS3 portrait, 135 px wide; sketches, not mockups):

```
FIRMWARE                       FIRMWARE          v0.7.0
                               Downloading...
v0.6.0  ->  v0.7.0             [##########......] 61 %
1.5 MB, from GitHub            0.9 / 1.5 MB
                               
Install                        
Check now                      
Previous version (v0.5.1)      
                               
o move  = install  < back      < cancel
```

Spanish texts and the Cardputer's keycaps follow the existing tables.

## 5. Code

Following CLAUDE.md's split:
- **Pure, native-tested** (`firmware/src/ota/`): the manifest parser
  (format errors, unknown lines, the row for an environment, URLs relative
  to the manifest), the version comparison, the "offer this?" rule (newer,
  not a dev build, not remembered as failed), the slot-size check the
  workflow also does.
- **Platform**: NetFetch's body loop takes a sink, a file (today's packs) or
  an OTA slot (`Update`), so redirects, TLS, timeouts, size, SHA-256 and
  progress stay in one place; `platform/Ota` wraps `Update`, the partition
  checks, `verifyRollbackLater()`, marking valid and the rollback queries.
- **App**: `app/Firmware.{h,cpp}` (the screen, the install), the Device row,
  the manifest read added to the deck update sessions (`app/DeckUpdates`,
  `app/Net`), the mark-valid step in `app/Boot` and the main loop.
- **Release**: `release.yml` writes the manifest and checks the slot sizes;
  `tools/release/merge_image.py` already writes `boot_app0.bin` when the
  table has `otadata`.
- **Docs**: the manifest format, the layout (architecture.md), the update
  paths (user-guide.md, firmware/README.md, the release notes template),
  m5burner.md (the merged image is a fresh install).

## 6. Plan

1. **Layout and release side** (S): the two partition tables, the manifest
   in `release.yml` with the size check, docs. Flash it over USB on the
   three boards (the move of §2.1) and check that everything still works,
   including `pio run -t upload` and an app-only write after `boot_app0.bin`.
2. **The device side** (M-L): the pure module and its tests, the NetFetch
   sink, `platform/Ota`, the Firmware screen, the checks with the deck
   updates, mark-valid and the rollback notice. Tested with a manifest
   override pointing at the computer, and with an image built to crash in
   `setup()` to see the rollback.
3. **The first OTA release**: v0.7.0 is installed over USB; v0.7.1, even a
   tiny one, is the first to arrive over the air on all three boards.
4. **Optional**: the portal upload for development (§3.8), Previous version
   (§3.6).

## 7. What only the hardware will tell

- Download and write time on each board, and whether `Update`'s erases slow
  the stream enough to hit NetFetch's idle timeout.
- Heap during TLS plus `Update` on the Cardputer ADV.
- That the precompiled bootloader really rolls back (a crashing test image).
- That LittleFS formats cleanly in its new place and that the first boot
  after the move behaves as a fresh device with settings kept.

## 8. Decisions

All taken on 2026-10-24 as recommended (the "recommended" column is the
decision).

| # | question | recommended | alternatives |
|---|---|---|---|
| D1 | App slot size on 8 MB boards | 1.75 MiB (LittleFS 4.44 MiB; Jōyō + N1 no longer fit) | 1.625 MiB (4.69 MiB, tight), 1.875 MiB (4.19 MiB) |
| D2 | Moving to the new layout | (a) by hand: progress backup, USB flash, Get decks, restore | (b) a host migration tool |
| D3 | When to look for an update | with the deck update checks, plus Check now | only Check now |
| D4 | Which image | the running build's environment; dev builds never update | always `m5stack` on M5 boards |
| D5 | When a new image counts as good | after setup, the filesystem and a deck (or the no-deck screens), plus a few seconds | at once (no rollback protection after `setup()`) |
| D6 | Previous version row | later, optional | in the first version; never |
| D7 | Install from the portal page | no: the page shows, the device installs | a portal button that starts it |
| D8 | Development upload over Wi-Fi | later phase | with phase 2; never |

## 9. As built (phase 2, the device side)

Built on 2026-10-24 (`feature/ota-device`; docs/decisions.md, "OTA: device
side as built"). The code is where §5 puts it: `ota/Manifest` and `ota/Offer`
(pure, `test_ota`), `fetchToSink()` in `platform/NetFetch` (the deck downloads
use it through a file sink), `platform/Ota`, `app/Firmware` (the screen, the
checks, the first boot). What differs from the design above:

- **Deep sleep and the first boot** (§3.5). The framework's bootloader is built
  with `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`: a deep-sleep wake boots
  the slot it slept in without reading `otadata`, so a pending image most
  likely stays pending through a sleep, as designed (to confirm on hardware).
  The app does not depend on it: every deep sleep of the running app
  (`prepareDeepSleep()`: the power button, Sleep, the idle timer, unplugging a
  dark panel) first marks a pending image valid once the loop has started,
  which is right whichever way the wake goes. Because a wake would not roll it
  back, a pending image that fails inside its boot -- `halt()` (no deck opens,
  the access point does not start) or the damaged-storage question -- rolls
  back at once (`esp_ota_mark_app_invalid_rollback_and_reboot()`) after 3 s of
  its error screen. Otherwise valid after `setup()` and 10 s of the loop, as
  designed, and also before an install (which overwrites the slot it would
  roll back to).
- **Other layouts keep the framework's verdict**: `verifyRollbackLater()`
  returns true only on our layout, so an app that M5Launcher put into one of its
  own slots is still marked valid at once by `initArduino()`.
- **How a rollback is told.** Once the image is in and matched the manifest,
  and before `Update.end()` switches the boot slot, the install stores the
  version and the slot it wrote (NVS `otanew`, `otaslot`; no record, no switch);
  a pending boot keeps them until it is marked valid, which clears them. A boot that is not pending
  and runs the other slot is a rollback: that version goes to NVS `otabad`
  (never offered again, release or development build) and a notice says once
  "Update failed / v0.7.0 did not start: back on v0.6.0", for 4 s or until a
  press, after the deck is open (in its language). `esp_ota_get_last_invalid_partition()`
  is only logged: on a device that never updated (erased `otadata`) it names
  `ota_0`, the running slot. The images' app description has the IDF's version,
  not the tag.
- **The badge**: NVS `fwver` and `fwsize`. The home's **Device** row has no room
  for a version next to "Dispositivo" (92 of its 93 px in portrait), so it
  shows the update arrow Get decks uses in place of its chevron; the
  **Firmware** row reads "v0.7.0 available" (else the running version). A read
  that parsed sets or clears it, a failed one leaves it; it goes once the
  device runs that version.
- **Install reads the manifest again** in its own Wi-Fi session, so the row is
  fresh (the badge may be from an earlier boot); then the download, "Checking
  the image..." while `Update.end()` reads it back, "Restarting into v0.7.0...".
  `Update.end()` is called without `evenIfRemaining`, so an image shorter than
  the size it began with is refused there too. A power cut in the second
  between the record and the switch makes the next boot report that version as
  failed (and not offer it again); the reverse order would let a failed one
  roll back unnoticed.
- **The sessions' manifest read** (§3.3) is skipped when the catalog read got
  no answer from any server (connect or TLS failure: the manifest would wait
  for the same timeouts); its body has 10 s. Failures are only logged
  (`[ota] manifest ...`). Development builds without a URL of their own log that
  they read none.
- **Relative image names** resolve against the manifest URL as requested, so
  the default gives `releases/latest/download/<file>`: a release published
  between the manifest read and the download fails that download's size or
  SHA-256 check, and the next check reads the new manifest.
- **The Firmware screen** (sketch of §4): the running version with the build
  environment right of it, a status line (an offer, "Up to date" with the
  latest version, "No update" and why, "Development build", "Not available"
  with the USB hint, or the last error), its detail, and the rows Install (only
  with an offer) and Check now (only where a manifest may be read). Its TLS
  failure reads "Secure link failed": "Secure connection failed" takes three
  lines in the 135 px portrait.
- **The portal**: Advanced → **Device firmware** (the version and build, a found
  update, or that this layout does not update itself) with the manifest URL
  field (`POST /fwurl`; in home mode saving it reads the catalog, and with it
  the new manifest, again), and a line at the top while an update is known.
- Not built (later, §6 step 4): **Previous version** (D6) and the development
  upload over Wi-Fi (D8).
