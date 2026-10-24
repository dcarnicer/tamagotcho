#pragma once
// Where decks live (docs/microsd.md): the device's LittleFS (`/packs`) and, on
// a board with a microSD slot, the card's `/tamagotcho/decks/`. The path rules
// both sides share, the merge of the two folders by file name (which copy the
// deck list shows), whether a deck moving to the card fits there, and the
// card's sizes as Settings > Device > About shows them. Pure C++, no Arduino:
// unit tested natively in test/test_storage. The filesystems themselves, the
// mount and the listing are platform/Storage.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tgc {

enum class DeckPlace : uint8_t { Device, Card };

// A deck path names its filesystem. "/packs/<file>" is LittleFS: the path the
// LittleFS API takes, as NVS `pack` has always held it. "/sd/tamagotcho/
// decks/<file>" is the card: its VFS path (the card is mounted at "/sd"), so a
// path alone says where a deck is.
constexpr const char* kDeviceDecksDir = "/packs";
constexpr const char* kCardMount = "/sd";
// On the card's filesystem (the paths its fs::FS takes).
constexpr const char* kCardAppDir = "/tamagotcho";
constexpr const char* kCardDecksDir = "/tamagotcho/decks";
// The progress copy (docs/microsd.md §2, D3): the portal's backup file as it
// is, written when the device sleeps and after a restore; its temp next to it,
// hidden like every temp.
constexpr const char* kCardProgressCopy = "/tamagotcho/progress.bin";
constexpr const char* kCardProgressTmp = "/tamagotcho/.progress.tmp";
// The temps of the deck writers, each in the folder of its destination (a
// rename never crosses filesystems). Hidden names: the deck listers skip them.
constexpr const char* kFetchTmpName = ".fetch.tmp";
constexpr const char* kUploadTmpName = ".upload.tmp";
constexpr const char* kMoveTmpName = ".move.tmp";

// Free space every deck writer leaves on the destination: on LittleFS for the
// progress file and the temps (platform/NetFetch.h kPackFsReserveBytes is this
// one), on the card for the progress copy (up to 128 KB) and the rounding of
// every file to whole clusters (32 KB on a 32 GB card).
constexpr uint64_t kDeviceReserveBytes = 96u * 1024u;
constexpr uint64_t kCardReserveBytes = 256u * 1024u;
inline uint64_t reserveFor(DeckPlace p) {
    return p == DeckPlace::Card ? kCardReserveBytes : kDeviceReserveBytes;
}

// The folder of deck paths in `p`: "/packs" or "/sd/tamagotcho/decks".
std::string decksFolder(DeckPlace p);
// "/packs/n5-en.tgc" or "/sd/tamagotcho/decks/n5-en.tgc".
std::string deckPath(DeckPlace p, const std::string& file);
// Where the deck at `path` is: the card for a path under "/sd/", else LittleFS.
DeckPlace placeOf(const std::string& path);
// What follows the last '/' ("n5-en.tgc").
std::string fileNameOf(const std::string& path);
// A deck file in a folder listing: "<name>.tgc", not hidden (a temp, or a
// computer's "._n5-en.tgc" on the card).
bool isDeckFileName(const std::string& name);

struct DeckFile {
    std::string path;  // its deck path
    std::string name;  // its file name ("n5-en.tgc")
    uint32_t bytes = 0;
    DeckPlace place = DeckPlace::Device;
};

// The deck list from the two folders' decks: one entry per file name, sorted
// by it. With the same name in both places the device's copy is the one
// listed (and loaded): it is the one the device wrote last (a download made
// while the card was out, or a deck the card had no room for), and the next
// move replaces the card's copy with it (docs/microsd.md §3.6).
std::vector<DeckFile> mergeDecks(const std::vector<DeckFile>& device,
                                 const std::vector<DeckFile>& card);

// Whether a deck of `bytes` moving from LittleFS fits the card, which has
// `cardFree` bytes free and `oldBytes` in a copy of the same name (0 without
// one), the card's reserve kept. In place of the old copy: it fits only once
// that copy is gone, so the move removes it first (the device's copy, the one
// that wins, is still on LittleFS until the new one checked).
enum class MoveFit : uint8_t { Fits, FitsInPlaceOfOld, NoRoom };
MoveFit moveFit(uint64_t bytes, uint64_t cardFree, uint64_t oldBytes);

// `bytes` in GB (GiB, as a computer shows a card) with one decimal and `point`
// as the decimal mark, rounded down: "29.7", "0.0". Returns `out`.
const char* formatGb(uint64_t bytes, char point, char* out, size_t n);

}  // namespace tgc
