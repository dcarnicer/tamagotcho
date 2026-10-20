// Which records go when a deck's progress is deleted. Design: docs/srs.md
// section 2.1.
#include "srs/DeckProgress.h"

#include <algorithm>
#include <iterator>

namespace tgc {

void sortUnique(std::vector<uint32_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

std::vector<uint32_t> forgetOnDelete(const std::vector<uint32_t>& deck,
                                     const std::vector<uint32_t>& keep) {
    std::vector<uint32_t> out;
    std::set_difference(deck.begin(), deck.end(), keep.begin(), keep.end(),
                        std::back_inserter(out));
    return out;
}

DeckReset deckResetPreview(const SrsStore& srs, const std::vector<uint32_t>& deck,
                           const std::vector<uint32_t>& others) {
    DeckReset r;
    for (uint32_t cp : deck) {
        if (!srs.find(cp)) continue;
        ++r.records;
        if (std::binary_search(others.begin(), others.end(), cp)) ++r.shared;
    }
    return r;
}

}  // namespace tgc
