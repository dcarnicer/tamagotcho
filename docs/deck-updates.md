# Deck update checks

Status (2026-10-22): design and first implementation on `main`. The device
checks the release catalog for newer builds of the decks it already has, says
so, and lets the user update them from the deck list. It never downloads a
pack on its own.

Roadmap: Phase 4, "Deck update checks over the home Wi-Fi"
(`docs/roadmap.md`).

## Goal

Today the user has to open **Decks > + Get decks** and compare the catalog's
decks with the installed ones to notice that a published deck was rebuilt. The
device should notice by itself, within the battery and radio policy, and the
update should be one hold away. SRS progress survives an update: it is keyed
by kanji, not by pack file (`docs/srs.md` section 2).

Non-goal: downloading or installing an update without the user asking. The
check only tells; the user chooses the deck and holds to update it.

## What counts as an update

The catalog (`docs/pack-format.md`, `catalog-v2.txt`) has one record per
published deck: `name`, `title`, `lang`, `cards`, exact `bytes`, `sha256` and
`url`. An installed file `/packs/<name>.tgc` whose catalog record has a
different `bytes` is an **update**, and so is one with the same size but a
different SHA-256 (`tgc::DeckState::Update`, the same state Get decks already
shows as "update"). Consequences:

- A catalog entry with a name that is not installed is **not** an update; Get
  decks lists it as new.
- A pack installed from outside the catalog (a WaniKani build, a file uploaded
  from the portal) is ignored: the catalog has no record for it.
- **Size then SHA-256.** The installed file is hashed only when its size equals
  the catalog's; a file whose size already differs is an update without reading
  it. The digest catches a rebuild that keeps the exact same byte count.
- **No false update on a read failure.** If the file cannot be opened or read
  for hashing, the comparison falls back to size only and reports the deck as
  installed, as before. The next check tries the hash again; no digest is
  cached.
- The check never trusts the local file's contents: the download still checks
  the catalog's size and SHA-256 before the atomic rename (`NetFetch`).

## When it checks

Automatically, at boot, after the pack, the SRS state and the queue are
loaded, and before the first question:

| condition | rule |
|---|---|
| home Wi-Fi | a network is saved (`HomeWifi::hasCredentials()`) |
| clock | the SRS has a real wall clock (`hasWallClock()`), so "today" is known |
| external power | once per calendar day |
| battery | once per 7 days, and only at 50 % or more |
| never checked | due on the first boot that meets the other conditions |
| power-cut boot | skipped: the date screen already uses the radio; the next normal boot checks |
| no packs | skipped (the boot upload screen has its own Get decks path) |

The automatic check runs on a small screen of its own ("Checking deck
updates..." and the network name) so the user knows why the boot is waiting.
Any press skips it; a failed join gives up after 30 s (the date screen allows
40 s). The result (updates found / all up to date / the failure) stays on
screen for about two seconds, or until a press. The radio is off again before
the quiz appears.

A manual check is always available: **Decks > Updates** opens the same
catalog read on demand, on battery too, and lists only the installed decks
with a newer build.

Why not every boot on battery: joining the home network costs up to 40 s of
radio at 240 MHz. Once a week at 50 % keeps the feature useful without
turning it into a battery tax; the manual row covers the rest. This is the
first policy; the roadmap keeps the battery thresholds open to tuning.

## UI

- **Settings home, Decks row**: its value changes from the pack count to
  "N updates" while an update is known. With none, it stays the pack count.
- **Deck list**: a new row above **+ Get decks**, "Updates", with the count,
  "check" (never checked) or "up to date" (checked, nothing found). A hold
  opens the updates list.
- **Updates list**: the Get decks screen in update mode. It joins the home
  Wi-Fi, reads the catalog and lists only installed decks whose size or SHA-256
  differs, with the existing update arrow and size; a hold downloads the new
  build, checks its size and SHA-256, renames it over the old file and removes
  the row when it is done. With nothing to update it says "All decks are up to
  date". The screen keeps the usual side-button back, retry on error and cancel
  on a download.
- **Boot check screen**: header "DECK UPDATES"; while joining, "Checking deck
  updates..." and the network name; then "N deck updates available" or "All
  decks are up to date", or the reason it failed. The hint bar offers skip /
  continue.
- The app does not pop a dialog over a review: the automatic check only runs
  at boot.

## State

NVS namespace `tangocho`:

| key | type | meaning |
|---|---|---|
| `updday` | u32 | epoch day of the last completed check, success or attempt (0 = never) |
| `updcount` | u8 | installed decks with an update (the last read, minus the ones updated since) |
| `updok` | u8 | 1 once a catalog read has succeeded; the Updates row says "up to date" only then |

The count is a badge, not a cache of the catalog: the updates list always
reads the catalog again, so a stale count corrects itself there. A failed or
skipped automatic check still records the day (the device tried; the next
automatic try is tomorrow or next week), but leaves the count and `updok` as
they were: the row says "check" only while no read has ever succeeded.
Opening Get decks, normal or update mode, refreshes all three when it reads a
catalog.

## Out of scope (follow-ups)

- A battery-based automatic check more often than weekly (or a scheduled
  wake-up; the device has no RTC alarm, only the RTC timer of a deep sleep).
- Showing the state on the Wi-Fi portal page or in `/api/stats`.
- Update channels (stable / beta) or per-deck pins; the catalog has one
  build per deck.
- Automatic download of security fixes; the user stays in control.
- M5Launcher-installed builds: the launcher keeps its own update path.

## Verification

- Native tests: `catalogDeckState()` covers the New / OnDevice / Update
  classification, size-only and size + SHA-256 (`test/test_catalog`);
  `deckUpdateCheckDue()` covers the daily / weekly / 50 % policy. The update
  count and the boot screen are device-only code.
- Both boards build (`pio run -e sticks3`, `-e cardputer-adv`).
- **Same-size rebuild, StickS3, 2026-10-22.** A local catalog declared `n5-en`
  with the installed file's exact size (106,124 B) but another SHA-256. The
  portal marked it **Update**, and the device's **Decks > Updates** list showed
  **JLPT N5 · update** after hashing one pack in 73 ms. Holding it downloaded
  the published deck (106,124 B, SHA-256 checked), removed the row, and kept
  the deck at 5 due / 67 new. With the published catalog, the other installed
  decks also showed as updates: their older builds have the same size but a
  different digest, which is exactly what the new check catches.
- **StickS3, 2026-10-22 (v0.4.2, size-only).** The boot check joined the saved
  home Wi-Fi ("Shimokitazawa"), read the catalog and showed "All decks are up
  to date"; the radio was off before the quiz. A deliberately replaced `n4-en.tgc`
  (189,660 B, built without stroke order) was detected by the **Updates** row
  and the filtered list showed "JLPT N4 · update"; a hold downloaded the
  catalog build (238,688 B, size and SHA-256 checked), removed the row and
  left the loaded N5 at 5 due / 67 new. The device ran the normal `m5stack`
  build afterwards.
- **Cardputer ADV, 2026-10-22.** The keyboard drove the settings, the deck
  list and Get decks (N4, 238,688 B); the Updates row and the filtered list
  worked, and Enter downloaded the catalog build after a deliberately
  replaced N4 (189,660 B, no strokes). The automatic boot check ran with that
  update (the badge showed 1; its two-second result screen passed before the
  remote could capture it). The device was restored to its original
  M5Launcher flash afterwards.
- **Backup before testing a device that is not ours.** The Cardputer's
  M5Launcher partition table has a 1,344 MB app0, so our 1.48 MB app-only
  image does not fit; the test used a merged image with the project's table.
  The whole 8 MB flash was read first (`esptool read_flash 0x0 0x800000`) and
  written back afterwards (`write_flash 0x0`, hash verified): bootloader,
  partition table, NVS, app slots and SPIFFS are all inside that image.
- Not exercised: capturing the automatic result screen with N > 0 (the check
  itself ran and the badge counted it), the skip and failure paths, and the
  battery thresholds. The detection and count path is the one the manual list
  used; the boot screen with zero updates was seen.
