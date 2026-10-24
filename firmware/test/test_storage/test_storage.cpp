// Native tests for where decks live (src/storage/DeckFiles: docs/microsd.md):
// the deck paths of the two places, the folder listing's filter, the merge of
// LittleFS's and the card's decks by file name (the device's copy wins), the
// move's free-space rule and the card sizes of Settings > Device > About.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "storage/DeckFiles.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

namespace {

DeckFile deck(DeckPlace p, const char* name, uint32_t bytes = 1000) {
    DeckFile d;
    d.name = name;
    d.path = deckPath(p, name);
    d.bytes = bytes;
    d.place = p;
    return d;
}

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

}  // namespace

// A path says its filesystem: LittleFS keeps the old "/packs/..." (NVS `pack`),
// the card's are its VFS paths under "/sd/tamagotcho/decks".
void test_paths() {
    TEST_ASSERT_EQUAL_STRING("/packs", decksFolder(DeckPlace::Device).c_str());
    TEST_ASSERT_EQUAL_STRING("/sd/tamagotcho/decks", decksFolder(DeckPlace::Card).c_str());
    TEST_ASSERT_EQUAL_STRING("/packs/n5-en.tgc", deckPath(DeckPlace::Device, "n5-en.tgc").c_str());
    TEST_ASSERT_EQUAL_STRING("/sd/tamagotcho/decks/n5-en.tgc",
                             deckPath(DeckPlace::Card, "n5-en.tgc").c_str());
    TEST_ASSERT_TRUE(placeOf("/sd/tamagotcho/decks/n5-en.tgc") == DeckPlace::Card);
    TEST_ASSERT_TRUE(placeOf(deckPath(DeckPlace::Card, kMoveTmpName)) == DeckPlace::Card);
    TEST_ASSERT_TRUE(placeOf("/packs/n5-en.tgc") == DeckPlace::Device);
    // Only the mount itself counts: a LittleFS name that starts like it does not.
    TEST_ASSERT_TRUE(placeOf("/sdcard/n5.tgc") == DeckPlace::Device);
    TEST_ASSERT_TRUE(placeOf("/sd") == DeckPlace::Device);
    TEST_ASSERT_TRUE(placeOf("") == DeckPlace::Device);
    TEST_ASSERT_EQUAL_STRING("n5-en.tgc", fileNameOf("/sd/tamagotcho/decks/n5-en.tgc").c_str());
    TEST_ASSERT_EQUAL_STRING("n5-en.tgc", fileNameOf("/packs/n5-en.tgc").c_str());
    TEST_ASSERT_EQUAL_STRING("n5-en.tgc", fileNameOf("n5-en.tgc").c_str());
    // The card's folder is the one the design names (docs/microsd.md, D2), and
    // the card's deck paths are its filesystem's paths under the mount.
    TEST_ASSERT_EQUAL_STRING(decksFolder(DeckPlace::Card).c_str(),
                             (std::string(kCardMount) + kCardDecksDir).c_str());
    TEST_ASSERT_EQUAL(0, std::strncmp(kCardDecksDir, kCardAppDir, std::strlen(kCardAppDir)));
}

// What a folder listing keeps: decks, not temps, not hidden files.
void test_deck_file_names() {
    TEST_ASSERT_TRUE(isDeckFileName("n5-en.tgc"));
    TEST_ASSERT_TRUE(isDeckFileName("a.tgc"));
    TEST_ASSERT_FALSE(isDeckFileName(".tgc"));
    TEST_ASSERT_FALSE(isDeckFileName("tgc"));
    TEST_ASSERT_FALSE(isDeckFileName("n5-en.tgc.txt"));
    TEST_ASSERT_FALSE(isDeckFileName("n5-en.TGC"));
    TEST_ASSERT_FALSE(isDeckFileName("._n5-en.tgc"));  // a Mac's resource fork on the card
    for (const char* tmp : {kFetchTmpName, kUploadTmpName, kMoveTmpName})
        TEST_ASSERT_FALSE_MESSAGE(isDeckFileName(tmp), tmp);
    // The progress copy and its temp are outside the decks folder.
    TEST_ASSERT_NOT_EQUAL(0, std::strncmp(kCardProgressCopy, kCardDecksDir,
                                          std::strlen(kCardDecksDir)));
    TEST_ASSERT_EQUAL(0, std::strncmp(kCardProgressCopy, kCardAppDir, std::strlen(kCardAppDir)));
    TEST_ASSERT_EQUAL('.', fileNameOf(kCardProgressTmp)[0]);
}

// One row per file name, sorted by it; the device's copy wins a tie.
void test_merge() {
    const std::vector<DeckFile> device = {deck(DeckPlace::Device, "n5-en.tgc", 700),
                                          deck(DeckPlace::Device, "joyo-en.tgc", 4000)};
    const std::vector<DeckFile> card = {deck(DeckPlace::Card, "n4-en.tgc"),
                                        deck(DeckPlace::Card, "n5-en.tgc", 650),
                                        deck(DeckPlace::Card, "a-es.tgc")};
    const std::vector<DeckFile> m = mergeDecks(device, card);
    TEST_ASSERT_EQUAL(4, (int)m.size());
    TEST_ASSERT_EQUAL_STRING("a-es.tgc", m[0].name.c_str());
    TEST_ASSERT_TRUE(m[0].place == DeckPlace::Card);
    TEST_ASSERT_EQUAL_STRING("joyo-en.tgc", m[1].name.c_str());
    TEST_ASSERT_TRUE(m[1].place == DeckPlace::Device);
    TEST_ASSERT_EQUAL_STRING("n4-en.tgc", m[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING("n5-en.tgc", m[3].name.c_str());
    TEST_ASSERT_TRUE(m[3].place == DeckPlace::Device);
    TEST_ASSERT_EQUAL_UINT32(700, m[3].bytes);
    TEST_ASSERT_EQUAL_STRING("/packs/n5-en.tgc", m[3].path.c_str());
    // Either side alone, and neither.
    TEST_ASSERT_EQUAL(3, (int)mergeDecks({}, card).size());
    TEST_ASSERT_EQUAL_STRING("a-es.tgc", mergeDecks({}, card)[0].name.c_str());
    TEST_ASSERT_EQUAL(2, (int)mergeDecks(device, {}).size());
    TEST_ASSERT_TRUE(mergeDecks({}, {}).empty());
}

// The move's free-space rule (docs/microsd.md §3.6): the deck plus the card's
// reserve; a same-name copy on the card counts once it is removed first.
void test_move_fit() {
    const uint64_t deckB = 2 * kMiB;
    TEST_ASSERT_TRUE(moveFit(deckB, 30 * kGiB, 0) == MoveFit::Fits);
    TEST_ASSERT_TRUE(moveFit(deckB, deckB + kCardReserveBytes, 0) == MoveFit::Fits);
    TEST_ASSERT_TRUE(moveFit(deckB, deckB + kCardReserveBytes - 1, 0) == MoveFit::NoRoom);
    TEST_ASSERT_TRUE(moveFit(deckB, 0, 0) == MoveFit::NoRoom);
    // An old copy of the same deck: fits as it is when there is room for both,
    // else only in its place, else not at all.
    TEST_ASSERT_TRUE(moveFit(deckB, 30 * kGiB, deckB) == MoveFit::Fits);
    TEST_ASSERT_TRUE(moveFit(deckB, kCardReserveBytes, deckB) == MoveFit::FitsInPlaceOfOld);
    TEST_ASSERT_TRUE(moveFit(deckB, kCardReserveBytes, deckB - 1) == MoveFit::NoRoom);
    TEST_ASSERT_TRUE(moveFit(deckB, 0, deckB) == MoveFit::NoRoom);  // the reserve is not there
    // The reserve holds the progress copy, the largest SRS file (128 KB) plus
    // the cluster rounding of a few files.
    TEST_ASSERT_TRUE(kCardReserveBytes >= 48u + 8192u * 16u + 4u + 2u * 32u * 1024u);
    TEST_ASSERT_TRUE(reserveFor(DeckPlace::Card) == kCardReserveBytes);
    TEST_ASSERT_TRUE(reserveFor(DeckPlace::Device) == kDeviceReserveBytes);
}

// Settings > Device > About's "29.7 GB, 28.1 free": GiB, one decimal, rounded
// down (a full card never shows as having room), the language's mark.
void test_format_gb() {
    char buf[16];
    // A "32 GB" card: 31,914,983,424 bytes, about 29.7 GiB.
    TEST_ASSERT_EQUAL_STRING("29.7", formatGb(31914983424ull, '.', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("29,7", formatGb(31914983424ull, ',', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("0.0", formatGb(0, '.', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("0.0", formatGb(100 * kMiB, '.', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("0.9", formatGb(kGiB - 1, '.', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("1.0", formatGb(kGiB, '.', buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("119.2", formatGb(128000000000ull, '.', buf, sizeof buf));
    // A cut buffer stays terminated.
    char small[3];
    formatGb(31914983424ull, '.', small, sizeof small);
    TEST_ASSERT_EQUAL_STRING("29", small);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_paths);
    RUN_TEST(test_deck_file_names);
    RUN_TEST(test_merge);
    RUN_TEST(test_move_fit);
    RUN_TEST(test_format_gb);
    return UNITY_END();
}
