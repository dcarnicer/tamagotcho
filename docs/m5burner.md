# M5Burner listing

Tamagotcho is listed on [M5Burner](https://burner.m5stack.com/) for the
StickS3 and the Cardputer ADV (the T-Display-S3 is a LilyGO board, which
M5Burner does not list). M5Burner writes one file at `0x0`, so each entry
carries the board's **merged** image from the GitHub release. This page keeps
what the publish form asks for, to update the entries with each release.

| field | StickS3 entry | Cardputer ADV entry |
|---|---|---|
| Name | `Tamagotcho` | `Tamagotcho` |
| Version | the release tag, e.g. `v0.3.0` | the same |
| Device | StickS3 | Cardputer ADV |
| Categories | Development & Learning, Productivity | the same |
| Source (GitHub) | `https://github.com/dcarnicer/tamagotcho` | the same |
| Firmware file | `tamagotcho-sticks3-merged.bin` | `tamagotcho-cardputer-adv-merged.bin` |
| Cover | `docs/images/cover.png` (1280x720) | the same, or a Cardputer one |
| Description | the text below | the text below, Cardputer controls |
| Version notes | the first lines of `docs/releases/<tag>.md` | the same |

## Description

Paste between the lines; M5Burner renders Markdown.

---

**たまごっちょ (Tamagotcho)**: a 電子単語帳 (electronic flashcard book) for
kanji. Multiple-choice reviews with spaced repetition, fully offline.

A kanji appears; pick its meaning or its reading from four options and hold
the button to answer. Spaced repetition (FSRS) decides when you see it again,
and the wrong options are ones you could really confuse. Get one wrong and a
card shows its meanings, readings and stroke order.

**Features**

- Kanji → meaning, kanji → reading, meaning → kanji
- Spaced repetition on the device, a daily cap of new cards
- Stroke order animated from KanjiVG after a mistake
- 12 free decks: JLPT N5 to N1 and the 2,136 Jōyō kanji, in English and
  Spanish, downloaded over Wi-Fi
- A web portal on your phone: decks, statistics, uploads
- The real date from your phone or NTP; dims and sleeps to save the battery
- Screens in English or Spanish

**First start**

1. Choose the language (a tap moves, a hold chooses).
2. Scan the QR code on the screen with your phone to join `Tamagotcho-XXXX`,
   open the page, enter your home Wi-Fi and press **Switch to my Wi-Fi**.
3. Scan the new QR code and download a deck, e.g. JLPT N5.

**Controls (StickS3)**: tap the front button to move, **hold it to answer**;
hold the side button for the settings, tap it to go back.

**Controls (Cardputer ADV)**: `;` `.` `,` `/` (the arrows) move, **Enter
answers**, Tab opens the settings, Esc goes back.

Burning erases the settings (home Wi-Fi, preferences). Code MIT; decks CC
BY-SA (KANJIDIC2, KanjiVG). Source, manual and other boards:
https://github.com/dcarnicer/tamagotcho

---
