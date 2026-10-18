// Session queue: classification, ordering, the daily new cap, per-item mode
// choice and the in-session relearn path. Design: docs/srs.md section 6.
#include "srs/Queue.h"

#include <algorithm>

namespace tgc {
namespace {

struct DueEntry {
    uint32_t idx;
    uint16_t due;
    uint16_t stab;
    uint32_t cp;
};

struct NewEntry {
    uint32_t idx;
    uint16_t level;
    uint32_t cp;
};

}  // namespace

void Queue::rebuild(const PackReader& pack, const SrsStore& srs, XorShiftRng& rng, uint16_t extraNew) {
    (void)rng;  // ordering is fully specified; kept for the injected-RNG API
    pack_ = &pack;
    srs_ = &srs;
    items_.clear();
    pos_ = 0;
    answered_ = 0;
    prevMode_ = -1;

    const uint32_t n = pack.cardCount();
    seen_.assign(n, 0);
    relearned_.assign(n, 0);

    const uint16_t today = srs.today();
    std::vector<DueEntry> due;
    std::vector<NewEntry> fresh;
    for (uint32_t i = 0; i < n; ++i) {
        Card c;
        if (!pack.card(i, &c)) continue;
        const Record* r = srs.find(c.codepoint);
        if (!r || r->dueDay == 0) {
            fresh.push_back({i, c.level, c.codepoint});
        } else if (r->dueDay <= today) {
            due.push_back({i, r->dueDay, r->stabilityCds, c.codepoint});
        }
    }

    // Reviews: most overdue, then weakest, then codepoint (section 6.2).
    std::sort(due.begin(), due.end(), [](const DueEntry& a, const DueEntry& b) {
        if (a.due != b.due) return a.due < b.due;
        if (a.stab != b.stab) return a.stab < b.stab;
        return a.cp < b.cp;
    });
    // New cards: pack level hint, then codepoint (section 6.3).
    std::sort(fresh.begin(), fresh.end(), [](const NewEntry& a, const NewEntry& b) {
        if (a.level != b.level) return a.level < b.level;
        return a.cp < b.cp;
    });

    for (const DueEntry& e : due) {
        if (items_.size() >= kSessionCap) break;
        if (seen_[e.idx]) continue;
        seen_[e.idx] = 1;
        items_.push_back(Item{e.idx});
    }

    // New cap: new_per_day minus what was introduced today. The store resets
    // the counter on a day change; the day check here is belt and braces.
    const uint16_t introduced = srs.newIntroducedDay() == today ? srs.newIntroducedCount() : 0;
    const uint16_t perDay = static_cast<uint16_t>(srs.newPerDay() + extraNew);
    uint16_t allowed = perDay > introduced ? static_cast<uint16_t>(perDay - introduced) : 0;
    for (const NewEntry& e : fresh) {
        if (allowed == 0 || items_.size() >= kSessionCap) break;
        if (seen_[e.idx]) continue;
        seen_[e.idx] = 1;
        items_.push_back(Item{e.idx});
        --allowed;
    }
}

int Queue::chooseMode(const Card& c, int prev) const {
    const bool hasMeaning = c.meanings && c.meanings[0];
    const bool hasReading = (c.onyomi && c.onyomi[0]) || (c.kunyomi && c.kunyomi[0]);
    bool can[3];
    can[static_cast<int>(QuizMode::Meaning)] = hasMeaning;
    can[static_cast<int>(QuizMode::Reading)] = hasReading;
    can[static_cast<int>(QuizMode::MeaningToKanji)] = hasMeaning;

    bool seen[3] = {false, false, false};
    if (srs_) {
        if (const Record* r = srs_->find(c.codepoint)) {
            seen[0] = (r->modeSeen & 0x1u) != 0;
            seen[1] = (r->modeSeen & 0x2u) != 0;
            seen[2] = (r->modeSeen & 0x4u) != 0;
        }
    }

    // Four passes over the mode ring, starting just after `prev`, so ties
    // rotate per card instead of globally (section 6.4): unused and different
    // from the previous item first, then unused, then anything different.
    const int start = prev >= 0 ? (prev + 1) % 3 : 0;
    for (int pass = 0; pass < 4; ++pass) {
        for (int k = 0; k < 3; ++k) {
            const int m = (start + k) % 3;
            if (!can[m]) continue;
            if (pass == 0 && (seen[m] || m == prev)) continue;
            if (pass == 1 && seen[m]) continue;
            if (pass == 2 && m == prev) continue;
            return m;
        }
    }
    return -1;  // no meaning and no reading: cannot be asked
}

bool Queue::next(QueueItem* out) {
    if (!out || !pack_) return false;
    while (pos_ < items_.size()) {
        const uint32_t idx = items_[pos_++].cardIndex;
        Card c;
        if (!pack_->card(idx, &c)) continue;
        const int m = chooseMode(c, prevMode_);
        if (m < 0) continue;
        prevMode_ = m;
        out->cardIndex = idx;
        out->mode = static_cast<QuizMode>(m);
        return true;
    }
    return false;
}

void Queue::answered(uint32_t cardIndex, Grade g) {
    ++answered_;
    if (g != Grade::Again) return;
    if (cardIndex >= relearned_.size()) return;
    if (relearned_[cardIndex] >= kMaxRelearnPushes) return;
    ++relearned_[cardIndex];

    size_t at = pos_ + kRelearnGap;
    if (at > items_.size()) at = items_.size();
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(at), Item{cardIndex});
}

}  // namespace tgc
