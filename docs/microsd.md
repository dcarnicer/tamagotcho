# Decks on the microSD card: design and decisions

> **Status: released in v0.7.0 (2026-10-24), checked on a Cardputer ADV
> (docs/releases/v0.7.0.md, §8).** The maintainer chose D2, D3
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
  G14, CS G12. The display is on another bus (M5GFX puts it on `SPI2_HOST`,
  MOSI G35, SCLK G36), so the card takes `SPI3_HOST` (an `SPIClass` on
  `HSPI`, not Arduino's global `SPI`, which is SPI2) and
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

## 8. As built (2026-10-24)

What the code does, where it differs from §1-§6 or fills a gap they left
(docs/decisions.md, "microSD: as built"). Not yet run on a Cardputer ADV:
the checks of §6 are the hardware test.

**Code.**
- `storage/DeckFiles` (pure, `test_storage`): the deck paths of the two
  places (`/packs/<file>` and `/sd/tamagotcho/decks/<file>`; `placeOf()` reads
  the place from the path), the listing filter (`*.tgc`, not hidden: no temp,
  no Mac `._` file), the merge by file name (`mergeDecks()`), the move's room
  rule (`moveFit()`), the reserves (`kCardReserveBytes` 256 KB: the progress
  copy plus cluster rounding; LittleFS keeps its 96 KB) and the GB of About
  (`formatGb()`, GiB rounded down).
- `board::sdPins()`: the Cardputer ADV's SCK 40, MISO 39, MOSI 14, CS 12;
  false on the StickS3 and the T-Display-S3, and in the `m5stack` image the
  board found at boot answers. M5GFX puts the panel on `SPI2_HOST` (its
  ESP32-S3 default; the `SPI3_HOST` line in its source is the classic ESP32's
  branch), so the card is on SPI3, `platform/Storage`'s own `SPIClass(HSPI)`:
  on Arduino's global `SPI` (SPI2) it never answered (2026-10-24, on the
  device). A mount that finds no card takes about 1 s, so the deck list and
  Get decks retry at most every 20 s after one (the boot always tries).
- **The EXT header shares the card's lines.** The Cap LoRa-1262 (Meshtastic's
  radio for this board) sits on SCK G40, MOSI G14, MISO G39 with its own select
  on G5; with the cap on and G5 left floating, the radio answered with the card
  and the card's init read noise ("crc error" on `GO_IDLE_STATE`). `SdPins`
  has an `otherCs` the storage drives high before the bus starts (G5 on the
  Cardputer ADV); harmless without the cap. After both fixes a 32 GB SDHC FAT32
  card mounted in 8 ms and took 15 KB of internal heap (2026-10-24).
- **A pulled card keeps its deck remembered.** When the loaded deck was on a
  card that was pulled and no deck is left, NVS `pack` stays, and the rescan
  after the card is back opens it again (it used to forget it and open the
  first deck of the list). Checked on the device: N3 studied, card out ("Card
  removed"), card in, N3 again.
- `platform/Storage`: the mount (an `fs::SDFS` subclass whose mount tells "no
  card" from "a card without a FAT volume": `SD.begin()` is false for both),
  the states (no slot, none, unreadable, mounted, removed), the path mapping,
  the listings (readdir() and stat(), no file opened), the copy with its
  SHA-256, the progress copy. `FsQuiet.h` and `PackHash.h` take the
  filesystem; `FsPackSource` can stop at its first read error and says it had
  one.
- `app/CardMove`: the move of §3.6 and its screen ("SD CARD", "2/5" right of
  it, "Moving decks to the card..." over a bar of the bytes moved and the
  deck's file name). Its callers: `app/Boot` (right after LittleFS and the
  card mount, before the deck opens) and `mountCardOnOpen()` (the deck list and
  Get decks).

**Mounting.** 20 MHz and two open files (§3.1). This framework's FatFs has
4 KB sector buffers (FF_MAX_SS is 4096 for the flash's wear levelling), one per
open file and one for the volume, so the mount takes about 13 KB of internal
heap, not 6-8 KB; the `[sd] boot: mounted ...` line logs the heap before and
after. Two files are enough because nothing keeps a second card file open:
the loaded deck plus one temp, hash, probe or copy at a time, and the
listings open none (the core's `openNextFile()` opens every file it lists).
The core gives a FAT file a 4 KB stdio buffer; a deck opened for reading on
the card gets 512 bytes (one sector: less heap, no 4 KB read behind each
glyph). The first mount of a card whose FSINFO has no free count reads the
whole FAT once (logged in ms); FatFs writes the count back on the next write.

**Which copy wins.** With the same file name in both places, the deck list
shows and loads LittleFS's copy (§3.6: the device's copy is the one it used
last). Storing a deck (Get decks, an upload, a URL download) removes its copy
in the other place once the new one is in, so in normal use a name is in one
place only; the move removes the card's old copy itself. "The loaded deck"
means the same file name in either place for closing it before a replace,
and a rescan that no longer finds its path opens the same file name in the
other place first (`sameDeckIn()`), at boot too. A saved deck on a card that
is not in stays saved in NVS, so the card's return opens it again.

**The move** follows §3.6, with two additions. The room check counts a
same-name card copy: a deck that fits only in its place removes it first (the
LittleFS copy, the one that wins, is still there), otherwise the old copy goes
right before the rename. The copy is hashed while it is read, and the card's
temp hashed again from the card. A LittleFS file that will not go after the
rename (never seen) leaves both copies; the device's still wins and the next
mount finishes. The move runs at boot and when the deck list or Get decks
mounts a card later, not after a pulled card mounts again (§3.5): no deck can
have reached LittleFS meanwhile. A later mount with the loaded deck among the
moved ones closes it, points `gCurrentPack` (and NVS `pack`) at the card and
rescans, which reopens it and gives the settings home's Continue a fresh
question.

**Writes.** New decks go to the card while one is mounted (D1), also an update
of a deck that sits on LittleFS (the card was full): with no room on the card
it fails as "not enough space", as a full LittleFS did. The free-space checks
and the reserve are the destination's. The per-pack cap stays 5 MiB.

**A pulled card (§3.5).** The loaded deck's source stops at its first failed
read (the SD driver spends about a second of timeouts on each read without a
card, and a screen is dozens of reads). `deckReadTick()` in the loop then
closes the deck, unmounts the card and mounts it again: no answer is
"Card removed" (it stays until a card mounts), an answer reopens the deck.
The decks are rescanned; the quiz, the detail and intro cards and the done
screen get a fresh item from a LittleFS deck, or the deck list when none is
left, and other screens are drawn again. Three failures within a minute with
the card answering give it up (unmounted, "Card not readable: format it as
FAT32"), so a bad sector cannot loop. A card pulled while no deck of it is
open is noticed when the deck list or Get decks opens (a mounted card is asked
whether it still answers, a few ms) or when a deck of it is chosen (its file
is gone): the same unmount and mount again. Until then the Wi-Fi portal's
uploads and downloads fail on it ("cannot create the file"). A write cut by a
pulled card fails like a full filesystem and removes its temp when it can.

**Screens and page.** As §4, plus: Get decks' note line shows "SD: 28.1 GB
free" while nothing else is there; the Wi-Fi screen's free-space line becomes
"SD 28.1 / 29.7 GB" over its bar while a card is mounted; About shows "No
card", "Card removed" or the unreadable line too. The deck list's top line is
one caption line in landscape. The page: the status line gives the card's free
space ("28.1 GB free on the card"), a red line says unreadable or removed,
*Your decks* lists every file of both places with "card" or "device" (only on
a board with a slot) and deletes the one chosen (`where=card|device`),
Advanced gives both free spaces and, with a card in, the note on the progress
copy, and `/api/stats` has `card` (state, free and total bytes) and a
`place` per pack.

**Progress copy.** Written whenever the device saves for a sleep (the idle
and explicit deep sleeps, the critical battery, and the dark panel on USB)
and after a restore, through `/tamagotcho/.progress.tmp`; `srs.bin`, else the
`.bak` a cut save left, as the portal's download serves.

**Sizes** (`TGC_VERSION=v0.0.0-cmp`, against `c8d2e0b`): see
docs/decisions.md, "microSD: as built".
