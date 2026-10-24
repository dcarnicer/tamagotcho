#pragma once
// The decks' two places and the microSD card (docs/microsd.md): LittleFS's
// `/packs` and, on a board with a slot (board::sdPins()), a FAT16/FAT32 card's
// `/tamagotcho/decks/`. Mounts the card at boot and when the deck list or Get
// decks opens with none mounted, says what state it is in, maps a deck path to
// its filesystem (storage/DeckFiles.h: "/packs/..." is LittleFS, "/sd/..." the
// card), lists the decks of both places, and writes the progress copy on the
// card. Every place that opens, lists, hashes, renames or removes a deck goes
// through here instead of calling LittleFS. Arduino/ESP32 only; not part of
// the native build.
#include <Arduino.h>
#include <FS.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "storage/DeckFiles.h"

namespace tgc {
namespace storage {

enum class CardState : uint8_t {
    NoSlot,      // the board has no microSD slot: LittleFS only, nothing said
    None,        // no card answered the last mount: LittleFS only, nothing said
    Unreadable,  // a card answered but has no FAT16/FAT32 volume (exFAT, unformatted,
                 // damaged), or kept failing reads: "format it as FAT32"
    Mounted,
    Removed,     // a mounted card stopped answering (docs/microsd.md §3.5): "Card removed",
                 // until a card mounts again
};

// At boot, after LittleFS: the card's bus and the mount. The card's folders
// are created and its stale temps removed whenever it mounts.
void begin();
// The deck list and Get decks, on opening: mounts the card again when none is
// mounted (a card put in after boot, or another one). True when a card
// mounted just now; the caller then moves the device's decks to it.
bool remount();
// A read from a deck on the card failed and its file is closed: the card is
// unmounted and mounted again. Removed when no card answers any more; Mounted
// when it does (the deck may be read again).
CardState readFailed();
// The card keeps failing reads although it answers: unmounted, Unreadable.
void giveUp();
// A mounted card still answers (there is no card-detect pin): a look at its
// decks folder, a few ms. False without a card mounted.
bool cardAnswers();

CardState cardState();
inline bool cardMounted() { return cardState() == CardState::Mounted; }
// The board has a slot (the UI and the page mention the card only then).
bool hasSlot();
// The mounted card's FAT volume: its size and free bytes (0 without one).
uint64_t cardTotalBytes();
uint64_t cardFreeBytes();

// --- deck paths --------------------------------------------------------------
// A path on the card while no card is mounted fails here at once (no
// filesystem error in the log).
bool onCard(const String& path);
fs::FS& fsFor(const String& path);
String fsPath(const String& path);  // the path on fsFor()'s filesystem
File openFile(const String& path, const char* mode = "r");
long fileSize(const String& path);  // -1 when there is no such file
inline bool exists(const String& path) { return fileSize(path) >= 0; }
bool removeFile(const String& path);
bool removeIfExists(const String& path);  // true when a file was removed
// Within one filesystem only. A FAT rename does not overwrite: the caller
// removes the destination first, as on LittleFS.
bool renameFile(const String& from, const String& to);
bool sha256(const String& path, uint8_t out[32]);
// Copies `from` to `to` (any two places) in 4 KB chunks. `digest` (optional)
// gets the SHA-256 of the bytes read, `progress` (optional) the bytes so far
// after each chunk. True when every byte arrived; on failure `to` may be left
// partial, and the caller removes it.
using CopyProgressFn = void (*)(size_t done, void* ctx);
bool copyFile(const String& from, const String& to, uint8_t digest[32] = nullptr,
              CopyProgressFn progress = nullptr, void* ctx = nullptr);

// Where a new deck goes (docs/microsd.md D1): the card while one is mounted,
// else LittleFS.
DeckPlace writePlace();
String deckPathFor(DeckPlace place, const String& file);
String decksDir(DeckPlace place);
bool makeDecksDir(DeckPlace place);
// Free bytes for decks in `place`, clamped to 4 GB - 1 (every caller compares
// it with a deck of at most 5 MB); 0 for the card while none is mounted.
size_t freeBytes(DeckPlace place);
// Removes the deck writers' temps in `place` (a power cut or a pulled card
// leaves them, up to 5 MB each).
void removeTemps(DeckPlace place);

// The deck of file name `file` the deck list shows: LittleFS's copy, else the
// card's; "" when neither place has it.
String installedPath(const String& file);
// The decks of `place`, sorted by file name (readdir() and stat(): no file is
// opened, which the card's two open files could not afford).
void listFiles(DeckPlace place, std::vector<DeckFile>& out);
// The deck list: one path per file name, LittleFS's copy over the card's
// (mergeDecks()).
void listDecks(std::vector<String>& out);
// Every deck file of both places, duplicates included (which kanji another
// deck still has, docs/srs.md §2.1).
void listAllDecks(std::vector<String>& out);
// After `path` was stored: removes the copy of the same file name in the other
// place, so that the new one is the one listed. True when one was removed.
bool removeOtherCopy(const String& path);

// The progress copy, /tamagotcho/progress.bin on the card (docs/microsd.md §2,
// D3): the state file as the portal's backup serves it, written through a temp.
// Nothing without a card. True when it was written.
bool writeProgressCopy();

}  // namespace storage
}  // namespace tgc
