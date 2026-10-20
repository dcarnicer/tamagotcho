#pragma once
// Which records go when a deck's progress is deleted (docs/srs.md section 2.1).
// Records are keyed by code point and shared by every deck with that kanji, so
// the two ways of deleting a deck's progress differ in what they leave:
//   - deleting a deck file forgets only the kanji no deck still installed has
//     (N5's kanji stay while the Jouyou deck is on the device);
//   - resetting a deck (Settings > Study > Delete progress > This deck) forgets
//     all of its kanji, the shared ones too; its preview says how many of them
//     other installed decks also have.
// The caller reads the decks' code points (readPackCodepoints()) and applies the
// result with SrsStore::forget(). Pure C++, no Arduino (native-tested in
// test_srs).
#include <cstdint>
#include <vector>

#include "srs/SrsStore.h"

namespace tgc {

// Sorts `v` ascending and drops duplicates (a pack's code points, or the union
// of several packs').
void sortUnique(std::vector<uint32_t>& v);

// Deleting a deck file: the code points of `deck` that `keep` (the decks still
// installed) does not have. Both sorted and unique; so is the result.
std::vector<uint32_t> forgetOnDelete(const std::vector<uint32_t>& deck,
                                     const std::vector<uint32_t>& keep);

// What resetting one deck would forget: the deck's kanji (unique) that have a
// record, and how many of those `others` (other installed decks, sorted and
// unique) also have, whose progress goes with them.
struct DeckReset {
    uint32_t records = 0;
    uint32_t shared = 0;
};
DeckReset deckResetPreview(const SrsStore& srs, const std::vector<uint32_t>& deck,
                           const std::vector<uint32_t>& others);

}  // namespace tgc
