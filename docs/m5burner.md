# M5Burner listing

Tamagotcho is listed on [M5Burner](https://burner.m5stack.com/) once, for
the StickS3 and the Cardputer ADV (the T-Display-S3 is a LilyGO board, which
M5Burner does not list). A version carries one `.bin`, written at `0x0`, so
the entry takes the **merged `m5stack` image** of the GitHub release, the one
build that runs on both boards (the board is found at boot; docs/decisions.md,
"One image for the StickS3 and the Cardputer ADV"). This page keeps what the
publish form asks for, to add a version with each release.

The package helper lives in `m5burner/`: `make dist VERSION=vX.Y.Z` builds the
full `dist/` and the M5Burner package, and `make m5burner VERSION=vX.Y.Z
MERGED=path` packages an existing merged image without rebuilding. Both write
`dist/m5burner/Tamagotcho-<version>.bin` (the Publish dialog), a `.zip` for the
M5Stack-Firmware route, and the unpacked package with `m5burner.json`, cover,
README and licence ([m5burner/README.md](../m5burner/README.md)).

The entry: <https://burner.m5stack.com/firmware/2112925841991487490>, account
`dabidesu`. v0.4.1 was submitted as Public on 2026-10-21 and approved on
2026-10-22 (published at 04:16 CEST); v0.4.0, submitted first, was withdrawn
the same day (back to a private draft) for the download bug v0.4.1 fixes.
v0.4.2 and v0.5.0 (both 2026-10-22) were never added; v0.5.1 (submitted
2026-10-23) never appeared in the public list either. **v0.6.0** was published
on 2026-10-26 and is the version the listing shows; its description gives
v0.6.0's news, then sums up v0.4.2 to v0.5.1 in one line for those still on
v0.4.1. **v0.7.0** was submitted as Public on 2026-10-24 and **M5Stack
rejected it**: "after multiple tests on actual devices, the firmware you
uploaded failed to boot the device" (reported by the maintainer on
2026-11-06). Because v0.7.1 shares the flash layout and most of the firmware,
the merged images were tested on hardware on 2026-11-06: they boot (table
below), so the rejection is not reproducible with
`tamagotcho-m5stack-merged.bin`. Check the uploaded file first: the entry
needs that merged image (1,647,968 bytes for v0.7.0, 1,648,672 for v0.7.1),
not the app-only `tamagotcho-m5stack.bin` (1,582,432 and 1,583,136 bytes),
which goes at `0x10000`, not `0x0`. If the merged image was attached, upload
it again (a truncated upload is the other possibility) and keep the boot log.

| test (2026-11-06) | result |
|---|---|
| Cardputer ADV, `erase_flash`, v0.7.1 merged at `0x0` | boots; card mounted, deck opened |
| Cardputer ADV, over M5Launcher, v0.7.1 merged, no erase | boots; LittleFS formatted |
| StickS3, over v0.6.0, v0.7.1 merged, no erase | boots; LittleFS formatted |
| StickS3, over the new layout, v0.7.1 merged, no erase | boots |
| StickS3, v0.7.0 merged, no erase | boots |

The shared description below is the live text,
updated on 2026-10-24 (learning mode, example words, JMdict); the v0.7.x
features are still missing from it (firmware updates over Wi-Fi, decks on the
microSD card, the new-firmware notice). A new version:
`/developer/firmware/2112925841991487490/versions/new` (My homepage › the
entry › View all versions); it keeps the entry's name, categories, devices and
description, and asks for the version, its notes, the release's
`tamagotcho-m5stack-merged.bin`, the visibility and the cover.

**v0.7.1** (2026-10-27) is ready to submit with the merged image: the v0.7.0
rejection is not reproducible, and v0.7.1 booted on real hardware on
2026-11-06. The published v0.7.1 also installed over the air on a StickS3 that
day (1,577,440 B into `app1`, marked valid after 10 s, N5 deck and 51 records
intact). It adds the card-full notice and is the first release that arrives
over the air: a device on v0.7.0 installs it from **Settings › Device ›
Firmware**, keeping settings, decks and progress. For a device on v0.6.0 or
older, burning it is still a **fresh install**, because v0.7.0's two-slot
layout comes with it. v0.7.1 also fixes v0.7.0's card-full boot screen: if the
card filled during the move, v0.7.0 could stay on "Moving decks to the
card..." instead of reaching the next screen, which can look like a failed
boot on a Cardputer with a full card.

**v0.7.2 is the latest release** (2026-11-06): it adds the full-screen notice
when a newer firmware is found. Submit **v0.7.2** instead of v0.7.1: the
release carries `Tamagotcho-0.7.2.bin` (the merged image for the Publish
dialog) and `tamagotcho-m5burner-0.7.2.zip` (the M5Stack-Firmware package).
Its version description is the "since v0.6.0" block from v0.7.1 plus the
notice bullet; the v0.7.1 assets remain on their release.

**v0.7.0's new layout** moves the deck storage, so Burning v0.7.0 or v0.7.1
over v0.6.0 or older loses the decks and the progress as well as the settings.
The version description has to say so and link the move (docs/user-guide.md,
"Moving to v0.7.0": the progress backup on the portal first). A device burned
with v0.7.0 or later runs the `m5stack` build and takes the `m5stack` row of the
release's `tamagotcho-firmware.txt` from then on; M5Burner stays the way to
install it, and to start again from scratch. v0.7.0's version description
gives its news, the fresh-install warning with the progress backup, and one
line for everything since v0.4.1.

For v0.7.1 or v0.7.2 use the first lines of `docs/releases/v0.7.1.md` plus
this paragraph (M5Burner users are coming from v0.6.0 or older); for v0.7.2
add the notice bullet from `docs/releases/v0.7.2.md`:

> **Coming from v0.6.0 or older?** v0.7.0 changed the flash layout, so burning
> v0.7.1 or v0.7.2 is a fresh install: the settings, decks and progress in the
> device's memory are erased. Before flashing, open the portal page ›
> **Advanced › Progress backup** and download the file; after, download the
> decks again and restore the progress. Guide:
> https://github.com/dcarnicer/tamagotcho/blob/main/docs/user-guide.md#moving-to-v070-once

| field | value |
|---|---|
| Name | `Tamagotcho` |
| Version | the release tag, e.g. `v0.7.1` |
| Devices | StickS3, Cardputer ADV |
| Categories | Development & Learning, Productivity |
| Project link | `https://github.com/dcarnicer/tamagotcho` |
| Firmware package | `tamagotcho-m5stack-merged.bin` |
| Cover | `docs/images/cover.png` (1280x720); `cover-cardputer.png` is the Cardputer's |
| Description | the text below (shared by all versions) |
| Version description | the first lines of `docs/releases/<tag>.md`; for v0.7.1 add the fresh-install paragraph above |

## Description

Paste between the lines; M5Burner renders Markdown.

---

**たまごっちょ (Tamagotcho)**: a 電子単語帳 (electronic flashcard book) for kanji. Multiple-choice reviews with spaced repetition, fully offline. One image for the **StickS3** and the **Cardputer ADV**: it finds the board at boot.

A new kanji is introduced first: its stroke order, meanings, readings and two example words. Then it is asked: pick its meaning or its reading from four options and answer. Spaced repetition (FSRS) decides when you see it again, and the wrong options are ones you could really confuse. Get one wrong and a card shows its meanings, readings and stroke order.

**Features**

- Kanji → meaning, kanji → reading, meaning → kanji
- Learning mode: each new kanji gets an intro card, then is asked again a few cards later
- Spaced repetition on the device, a daily cap of new cards
- Stroke order animated from KanjiVG on new kanji and after a mistake
- 12 free decks: JLPT N5 to N1 and the 2,136 Jōyō kanji, in English and Spanish, with example words, downloaded over Wi-Fi
- A web portal on your phone: decks, statistics, uploads
- The real date from your phone or NTP; dims and sleeps to save the battery
- Screens in English or Spanish

**First start**

1. Choose the language.
2. Scan the QR code on the screen with your phone to join `Tamagotcho-XXXX`, open the page, enter your home Wi-Fi and press **Switch to my Wi-Fi**.
3. Scan the new QR code and download a deck, e.g. JLPT N5.

**Controls**

- StickS3: tap the front button to move, **hold it to answer**; hold the side button for the settings, tap it to go back. On a new kanji's card, a side tap says you already know it.
- Cardputer ADV: `;` `.` `,` `/` (the arrows) move, **Enter answers**, Tab opens the settings, Esc goes back. On a new kanji's card, ← says you already know it.

Burning erases the settings (home Wi-Fi, preferences). Code MIT; decks CC BY-SA (KANJIDIC2, KanjiVG, JMdict). Source, manual and other boards: https://github.com/dcarnicer/tamagotcho

---
