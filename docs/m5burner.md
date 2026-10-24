# M5Burner listing

Tamagotcho is listed on [M5Burner](https://burner.m5stack.com/) once, for
the StickS3 and the Cardputer ADV (the T-Display-S3 is a LilyGO board, which
M5Burner does not list). A version carries one `.bin`, written at `0x0`, so
the entry takes the **merged `m5stack` image** of the GitHub release, the one
build that runs on both boards (the board is found at boot; docs/decisions.md,
"One image for the StickS3 and the Cardputer ADV"). This page keeps what the
publish form asks for, to add a version with each release.

The entry: <https://burner.m5stack.com/firmware/2112925841991487490>, account
`dabidesu`. v0.4.1 was submitted as Public on 2026-10-21 and approved on
2026-10-22 (published at 04:16 CEST); v0.4.0, submitted first, was withdrawn
the same day (back to a private draft) for the download bug v0.4.1 fixes.
v0.4.2 and v0.5.0 (both 2026-10-22) were never added. **v0.5.1** was
submitted as Public on 2026-10-23 with the release's
`tamagotcho-m5stack-merged.bin` and is pending review; since M5Burner users
come from v0.4.1, its version description sums up everything since then
(deck update checks, progress backup, the weakest kanji, texts that fit) and
links the full notes. **v0.6.0** was submitted as Public on 2026-10-24, with
v0.5.1 still pending review; its description gives v0.6.0's news, then sums
up v0.4.2 to v0.5.1 in one line for those still on v0.4.1. The shared
description below was updated the same day (learning mode, example words,
JMdict) and is under review as metadata. A new version: `/developer/firmware/2112925841991487490/versions/new`
(My homepage › the entry › View all versions); it keeps the entry's name,
categories, devices and description, and asks for the version, its notes,
the release's `tamagotcho-m5stack-merged.bin`, the visibility and the cover.

| field | value |
|---|---|
| Name | `Tamagotcho` |
| Version | the release tag, e.g. `v0.4.2` |
| Devices | StickS3, Cardputer ADV |
| Categories | Development & Learning, Productivity |
| Project link | `https://github.com/dcarnicer/tamagotcho` |
| Firmware package | `tamagotcho-m5stack-merged.bin` |
| Cover | `docs/images/cover.png` (1280x720); `cover-cardputer.png` is the Cardputer's |
| Description | the text below (shared by all versions) |
| Version description | the first lines of `docs/releases/<tag>.md` |

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
