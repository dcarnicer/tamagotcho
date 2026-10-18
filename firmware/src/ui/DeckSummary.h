#pragma once
// The loaded deck's counts, as the settings summary card, the deck list, the
// statistics and the done screen show them: cards in the deck, never reviewed
// ("new"), due by today and by tomorrow, and from those the coverage (seen /
// total). Progress is global (SRS records are keyed by code point), so a deck
// counts the records of its own cards only. One pass over the card table with
// a binary search per card. Pure C++, no Arduino (native-tested in
// test_settings).
#include <cstdint>

#include "pack/PackReader.h"
#include "srs/SrsStore.h"

namespace tgc {

struct DeckSummary {
    uint32_t total = 0;        // cards in the deck
    uint32_t fresh = 0;        // no record, or a record never scheduled (due day 0)
    uint32_t due = 0;          // reviewed and due today or earlier
    uint32_t dueTomorrow = 0;  // reviewed and due by tomorrow (`due` included)
    uint32_t introducedToday = 0;  // first reviewed today: what the new-card cap counts
    uint32_t seen() const { return total - fresh; }
};

inline DeckSummary deckSummary(const PackReader& pack, const SrsStore& srs) {
    DeckSummary s;
    if (!pack.isOpen()) return s;
    const uint32_t today = srs.today();
    for (uint32_t i = 0; i < pack.cardCount(); ++i) {
        Card c;
        if (!pack.card(i, &c)) continue;
        ++s.total;
        const Record* r = srs.find(c.codepoint);
        if (r && introducedOn(*r, static_cast<uint16_t>(today))) ++s.introducedToday;
        if (!r || r->dueDay == 0) {
            ++s.fresh;
            continue;
        }
        if (r->dueDay <= today) ++s.due;
        if (r->dueDay <= today + 1) ++s.dueTomorrow;
    }
    return s;
}

// The "+N new" override of the done screen: the extra new cards for today such
// that `step` cards beyond what this deck already introduced today come up.
// The extra in force resets when the deck changes while today's introductions
// stay, so adding `step` to it alone could still leave nothing to study.
inline uint16_t studyMoreExtra(uint16_t perDay, uint32_t introducedToday, uint16_t extra,
                               uint16_t step = 10) {
    const uint32_t over = introducedToday > perDay ? introducedToday - perDay : 0;
    const uint32_t v = (over > extra ? over : extra) + static_cast<uint32_t>(step);
    return v > 0xFFFFu ? 0xFFFF : static_cast<uint16_t>(v);
}

}  // namespace tgc
