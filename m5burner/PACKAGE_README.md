# Tamagotcho — M5Burner package

Kanji multiple-choice reviews with spaced repetition for the **M5Stack
StickS3** and the **Cardputer ADV**. One image for both boards; the board is
found at boot.

Burn `firmware/Tamagotcho_0x0.bin` (or `Tamagotcho-<version>.bin`) at `0x0`.
It contains the bootloader, the partition table, the OTA boot record and the
application. Do not use `tamagotcho-m5stack.bin`: that is the app-only image
for `0x10000`.

**Coming from v0.6.0 or older? This is a fresh install.** v0.7.0 changed the
flash layout, so burning this image erases the settings, decks and progress
stored in the device's memory. Before flashing, open the portal page ›
**Advanced › Progress backup** and download the file. After flashing, download
the decks again (Get decks; put a FAT32 card in the Cardputer ADV first) and
restore the progress on the portal page.

Code: MIT. Decks: CC BY-SA (KANJIDIC2, KanjiVG, JMdict), downloaded over
Wi-Fi; none are included in this package. Source, manual and other boards:
<https://github.com/dcarnicer/tamagotcho>

See `VERSION_DESCRIPTION.md` for this version's changes.
