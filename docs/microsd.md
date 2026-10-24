# Decks on the microSD card: design and decisions

> **Status: approved (2026-10-24), being built.** The maintainer chose D2, D3
> as recommended and, for D4, that decks on LittleFS **move to the card by
> themselves** (§3.6); D1, D5 and D6 as recommended. Roadmap: "Decks on the Cardputer ADV's microSD card" (idea of
> 2026-10-21). The maintainer's brief (2026-10-24): on the Cardputer ADV, use
> the card for storage as far as possible. Built on top of the OTA work
> (docs/ota.md), whose two app slots left the Cardputer 4.44 MiB of LittleFS.

The Cardputer ADV has a microSD slot. Today every deck lives on LittleFS
(`/packs`), 4.44 MiB on the 8 MB boards since the OTA layout: the Jōyō deck
fits with N5 and N4, not with N1, and both languages of everything never fit.
A card holds every deck the device can read, and decks can be copied onto it
from a computer.

Non-goals: SD support on boards without a slot (StickS3, T-Display-S3);
exFAT (§1); hot-plug detection (§3.5).

## 1. Facts

- **Pins** (Cardputer ADV): SD on its own SPI bus, SCK G40, MISO G39, MOSI
  G14, CS G12. The display is on another bus (M5GFX puts it on `SPI3_HOST`,
  MOSI G35, SCLK G36), so the card takes `SPI2_HOST` (Arduino's `SPI`) and
  the two never share a bus or a lock. No card-detect pin.
- **Filesystem**: Arduino-ESP32 2.0.17's FatFs is built with
  `FF_FS_EXFAT 0`: **FAT16/FAT32 only**. Cards up to 32 GB come as FAT32;
  larger ones (SDXC) come as exFAT and must be reformatted as FAT32 (most
  formatters do it for 64 GB; the SD Association's tool does not, others
  do). Long file names are on (`CONFIG_FATFS_LFN_STACK`, 255 characters).
- **Arduino's `SD` library** gives an `fs::FS`, the same `File` API as
  `LittleFS`: `FsPackSource` (the pack reader's file source), the SHA-256 of
  installed decks (`PackHash`) and the download sink can take either.
- **Memory**: the Cardputer has no PSRAM and its pack tables are read on
  demand from the file (`PackMemory::Streamed`). FatFs keeps a 4 KB cache per
  open file (`CONFIG_FATFS_PER_FILE_CACHE`) plus the volume's buffers, about
  6-8 KB of internal RAM with the deck open; the heap after a deck opens is
  about 150 KB, and TLS takes about 50 KB of it.
- **Speed**: SPI at 20-25 MHz reads about 1-2 MB/s with a few ms per random
  access; a streamed pack reads a handful of small records and one glyph per
  drawn character, so the card's latency adds to every new question's draw.
  To measure (§6).
- **Other boards**: the Waveshare ESP32-S3-Touch-AMOLED-2.06 (the watch of
  the roadmap) has a slot too (SPI: MOSI G1, SCK G2, MISO G3, CS G17), so the
  board layer gets the pins, not the app.

## 2. What goes on the card

| what | today | proposed |
|---|---|---|
| decks (`.tgc`) | LittleFS `/packs` | **on the card** when one is mounted (D1, D2) |
| progress (`/state/srs.bin` and its `.bak`) | LittleFS | **stays on LittleFS**, plus a copy on the card (D3) |
| settings, Wi-Fi | NVS | NVS |
| download and upload temps | next to their destination | next to their destination |
| the firmware (OTA) | streamed into the other app slot | unchanged |

**Decks.** The card's folder **`/tamagotcho/decks/`** (created on first use)
holds decks like `/packs` does. Get decks, the portal upload and the portal
URL download write to the card when it is mounted, else to LittleFS. Decks on
LittleFS move to the card by themselves when one is mounted (§3.6, D4), so
with a card the device normally has every deck on it and LittleFS holds only
the progress. The deck list shows both places while a deck is still on
LittleFS (a card that was full, or a deck fetched while no card was in), the
card's decks marked as such (an "SD" pill).

**Progress stays on LittleFS** (recommended, D3): it is small (a few KB, 40 KB
for every Jōyō kanji), written every 8 answers, and must survive a card that
is pulled, full, slow or corrupt. The device also writes
**`/tamagotcho/progress.bin`** on the card (the portal's backup format,
docs/srs.md "Backup and restore from the portal") when it goes to sleep and
after a restore, so the card always carries a recent backup that a computer
can read and the portal can restore. The alternative, progress on the card,
would make it follow the card between devices, but a lost card would lose it
and every write would wait on the card.

## 3. Design

### 3.1 Mounting

At boot, after LittleFS: `SPI.begin(40, 39, 14, 12)`, then
`SD.begin(12, SPI, 20 MHz, "/sd", 2 files)`. Results:
- **No card** (the init fails): LittleFS only, as today; nothing on screen.
- **A card that does not mount** (not FAT, damaged): LittleFS only, and the
  deck list and the portal say "Card not readable: format it as FAT32".
- **Mounted**: the decks folder is created if missing; the card's size and
  free space go to the portal and Settings › Device › About.

Mounting happens again when the deck list or Get decks opens with no card
mounted, so a card inserted after boot is picked up without a restart.

### 3.2 Paths

Deck paths name their filesystem: `/packs/<name>.tgc` on LittleFS (as today,
so NVS `pack` keeps working) and `/sd/tamagotcho/decks/<name>.tgc` on the
card. One small helper (`platform/Storage`) maps a path to its `fs::FS` and
its relative path, and every place that opens, lists, hashes, renames or
removes a deck goes through it instead of calling `LittleFS` directly
(`app/Decks`, `app/Progress`, `platform/NetFetch`, `platform/PackHash`,
`platform/WifiPortal`, `app/Boot`'s temp cleanup). The pure code does not
change.

### 3.3 Writing decks to the card

The download and upload paths keep their rules (a temp next to the
destination, checked, then renamed over it; the loaded deck closed first;
docs/decisions.md 2026-10-24 "The portal can replace and delete the loaded
deck"). FAT renames do not overwrite, so the old file is removed first, as on
LittleFS. The free-space check reads the card's free space; the per-pack cap
(`kCatalogMaxPackBytes`, 5 MiB) stays, since it is about the reader, not the
storage.

### 3.4 Reading from the card

`FsPackSource` reads the card's file like a LittleFS one. On the Cardputer
the tables are streamed already; the glyph cache (`gRender`) keeps the
recent glyphs in RAM, so a question's second draw does not touch the card.

### 3.5 A card pulled out

There is no card-detect pin. A read error from the open deck (the reader's
`ok()` turns false, or a glyph cannot be read) is treated like the loaded
deck disappearing: `portalPacksChanged()` rescans, the deck list opens with
"Card removed" when no deck is left, a LittleFS deck loads otherwise.
Progress is not at risk (it is on LittleFS). A write in progress when the
card is pulled fails like a full filesystem.

### 3.6 Moving LittleFS decks to the card (D4)

Every time a card mounts (at boot, or when the deck list or Get decks mounts
it later), each `/packs/<name>.tgc` on LittleFS moves to
`/sd/tamagotcho/decks/<name>.tgc`, before any deck is opened:
1. Copy into the card's temp (`.move.tmp` in the decks folder), with the
   card's free space checked first (the size plus a small reserve). A deck
   that does not fit stays on LittleFS and the move stops there (later decks
   are not tried either: the card is full).
2. Check the copy: the same size and SHA-256 as the source (`PackHash`), and
   the pack probe (`validatePackFile`).
3. Rename the temp over the card's `<name>.tgc` (removing a card copy of the
   same name first: the device's own copy is the one it used last, e.g. an
   update fetched while the card was out), then remove the LittleFS file.
4. If the moved deck is the one NVS `pack` names, `pack` is rewritten to its
   new path, so the same deck opens.

A failure at any step leaves the LittleFS deck as it was and removes the
temp. The screen shows "Moving decks to the card... 2/5" with a bar while it
runs (about 1 MB/s; a full 4.4 MiB LittleFS takes a few seconds); the side
button does not cancel it (each deck is atomic, and the next boot finishes the
rest anyway). The serial log lists each deck moved or kept and why.

Progress records are keyed by kanji, not by file (docs/srs.md §2), so a moved
deck keeps its progress.

### 3.7 The portal

The page lists the decks of both places (with "card" or "device" next to
each), shows the card's free space and deletes from either. The progress
backup link stays; the page says that the card holds a copy.

## 4. Screens

- **Decks**: an `SD` pill on the card's decks; a top line "Card not readable:
  format it as FAT32" or "Card removed" when that is the case.
- **Settings › Device › About**: "Card: 29.7 GB, 28.1 free" (or "No card").
- **Get decks**: "on the card" next to the free space.

## 5. Code

- `board/`: `bool sdPins(SdPins&)` (Cardputer ADV: 40/39/14/12; others none).
- `platform/Storage.{h,cpp}`: mount, status, free space, the path mapping,
  listing the decks of both places, the progress copy.
- The callers of §3.2 move to it; `platform/FsQuiet.h` gets the FS argument.
- Native tests for the pure parts that change (the deck list merge by name
  and the "which copy wins" rule, if it lands in pure code).
- `m5stack` image: the StickS3 has no slot, so `sdPins()` says none at run
  time (the board is found at boot, docs/decisions.md "One image for the
  StickS3 and the Cardputer ADV").

## 6. Plan and hardware checks

1. Mount, status, About line, the deck list of both places, reading a deck
   from the card (copied there by a computer). Measure the draw time of a
   new question and of the intro card with a Jōyō deck on the card vs on
   LittleFS (`TGC_QUIZ_LOG`), and the heap with the card mounted, during TLS.
2. Writes: Get decks, the portal upload and URL download to the card; the
   loaded deck replaced on the card; delete. The move of §3.6 with the loaded
   deck among the moved ones, with a card that fills up half-way, and with a
   deck of the same name already on the card.
3. The progress copy on the card; a pulled card (reading, writing); an exFAT
   card (the message); no card (as today).
4. The `m5stack` image on the StickS3 (no slot) and on the Cardputer.

## 7. Decisions

Taken on 2026-10-24: the "recommended" column, except D4 (marked).

| # | question | recommended | alternatives |
|---|---|---|---|
| D1 | Where new decks go when a card is mounted | the card | ask each time; LittleFS first, the card when it is full |
| D2 | The card's folder | `/tamagotcho/decks/` | `/decks/`; the card's root |
| D3 | Progress | LittleFS, with a copy on the card at sleep and after a restore | on the card only; LittleFS only (no copy) |
| D4 | LittleFS decks when a card is mounted | **chosen: moved to the card by themselves** (§3.6) | (recommended) stay, the portal moves one; hidden while a card is mounted |
| D5 | A card that does not mount | say "format it as FAT32", carry on with LittleFS | silent |
| D6 | Which boards | the Cardputer ADV now, the watch when it is ported (pins in the board layer) | Cardputer only |
