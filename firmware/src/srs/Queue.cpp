// Session queue: classification, ordering, the daily new cap, per-item mode
// choice, the in-session relearn path and the learning steps. Design:
// docs/srs.md section 6 and docs/learning-mode.md section 3.4.
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

// The modes `c` can be asked in, indexed by QuizMode.
void askable(const Card& c, bool can[3]) {
    const bool hasMeaning = c.meanings && c.meanings[0];
    can[static_cast<int>(QuizMode::Meaning)] = hasMeaning;
    // One kind is enough: buildQuestion() asks a card without the wanted kind
    // its other one. Only a card with neither is never asked a reading.
    can[static_cast<int>(QuizMode::Reading)] = readingKindOf(c) != ReadingKind::None;
    can[static_cast<int>(QuizMode::MeaningToKanji)] = hasMeaning;
}

// Step 1 asks kanji -> meaning (recognition first), step 2 a reading, the
// harder half (docs/learning-mode.md section 3.4, O3).
int8_t stepMode(uint8_t step) {
    return static_cast<int8_t>(step <= 1 ? QuizMode::Meaning : QuizMode::Reading);
}

}  // namespace

void Queue::setLearning(const LearnOptions& opts) {
    learn_ = opts;
    if (learn_.steps < 1) learn_.steps = 1;
    if (learn_.steps > kMaxSteps) learn_.steps = kMaxSteps;
    if (learn_.gap < 1) learn_.gap = 1;
}

void Queue::rebuild(const PackReader& pack, const SrsStore& srs, XorShiftRng& rng, uint16_t extraNew) {
    (void)rng;  // ordering is fully specified; kept for the injected-RNG API
    pack_ = &pack;
    srs_ = &srs;
    items_.clear();
    pos_ = 0;
    answered_ = 0;
    prevMode_ = -1;
    cur_ = Item{};
    open_ = false;

    const uint32_t n = pack.cardCount();
    seen_.assign(n, 0);
    relearned_.assign(n, 0);

    // A learning record (an intro whose steps a rebuild dropped) is due
    // whatever its due day, which is 0 for an intro on SRS day 0
    // (docs/learning-mode.md section 3.5).
    const uint16_t today = srs.today();
    const auto isDue = [today](const Record& r) { return isLearning(r) || r.dueDay <= today; };
    std::vector<DueEntry> due;
    std::vector<NewEntry> fresh;
    // Their sizes first (the code points are in memory however the pack is
    // read), so neither list grows by doubling: without PSRAM a Joyo-sized
    // deck would otherwise need about three times its final size for a moment.
    size_t nDue = 0, nFresh = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const Record* r = srs.find(pack.codepointAt(i));
        if (isFresh(r))
            ++nFresh;
        else if (isDue(*r))
            ++nDue;
    }
    due.reserve(nDue);
    fresh.reserve(nFresh);
    uint16_t introduced = 0;  // this deck's cards introduced today
    for (uint32_t i = 0; i < n; ++i) {
        Card c;
        if (!pack.card(i, &c, /*text=*/false)) continue;
        const Record* r = srs.find(c.codepoint);
        if (r && introducedOn(*r, today) && introduced < 0xFFFF) ++introduced;
        if (isFresh(r)) {
            fresh.push_back({i, c.level, c.codepoint});
        } else if (isDue(*r)) {
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

    // The session cap counts what the session will show: with learning on,
    // an intro and the steps it will add.
    size_t scheduled = 0;
    for (const DueEntry& e : due) {
        if (scheduled >= kSessionCap) break;
        if (seen_[e.idx]) continue;
        seen_[e.idx] = 1;
        items_.push_back(Item{e.idx});
        ++scheduled;
    }

    // New cap, per deck: new_per_day minus this deck's cards introduced today
    // (docs/srs.md section 6.3). Another deck's new cards do not use it up.
    // With learning on, each new card is an intro (docs/learning-mode.md 3.4).
    const uint16_t perDay = static_cast<uint16_t>(srs.newPerDay() + extraNew);
    uint16_t allowed = perDay > introduced ? static_cast<uint16_t>(perDay - introduced) : 0;
    const size_t cost = learn_.on ? 1u + learn_.steps : 1u;
    for (const NewEntry& e : fresh) {
        if (allowed == 0 || scheduled + cost > kSessionCap) break;
        if (seen_[e.idx]) continue;
        seen_[e.idx] = 1;
        Item it{e.idx};
        if (learn_.on) it.kind = ItemKind::Intro;
        items_.push_back(it);
        scheduled += cost;
        --allowed;
    }
}

int Queue::chooseMode(const Card& c, int prev) const {
    bool can[3];
    askable(c, can);

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
    open_ = false;
    while (pos_ < items_.size()) {
        const Item it = items_[pos_++];
        Card c;
        if (!pack_->card(it.cardIndex, &c)) continue;
        // A step (or a step's relearn) asks its own mode when the card has
        // it, any other item the least used one. A card with neither a meaning
        // nor a reading is never asked, so it gets no intro either.
        bool can[3];
        askable(c, can);
        const int m = it.mode >= 0 && can[it.mode] ? it.mode : chooseMode(c, prevMode_);
        if (m < 0) continue;
        out->cardIndex = it.cardIndex;
        out->kind = it.kind;
        out->step = it.step;
        if (it.kind == ItemKind::Intro) {
            out->mode = QuizMode::Meaning;  // no question: the mode rotation is left alone
        } else {
            out->mode = static_cast<QuizMode>(m);
            prevMode_ = m;
        }
        cur_ = it;
        if (it.mode >= 0) cur_.mode = static_cast<int8_t>(m);
        open_ = true;
        return true;
    }
    return false;
}

size_t Queue::stepsAfter(const Item& it) const {
    if (it.kind == ItemKind::Intro) return learn_.steps;
    if (it.kind == ItemKind::Step && it.step < learn_.steps) return learn_.steps - it.step;
    return 0;
}

size_t Queue::remaining() const {
    size_t n = 0;
    for (size_t i = pos_; i < items_.size(); ++i) n += 1 + stepsAfter(items_[i]);
    if (open_) n += stepsAfter(cur_);
    return n;
}

void Queue::insertAhead(size_t gap, const Item& it) {
    size_t at = pos_ + gap;
    if (at > items_.size()) at = items_.size();
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(at), it);
}

void Queue::answered(uint32_t cardIndex, Grade g) {
    ++answered_;
    // The question on screen (in the app always this card's).
    const bool onScreen = open_ && cur_.kind != ItemKind::Intro && cur_.cardIndex == cardIndex;
    if (onScreen) open_ = false;

    // After a learning step, the next one `gap` items on, whatever the grade
    // (docs/learning-mode.md section 3.4). Inserted before a relearn push, so
    // both keep their gaps.
    if (onScreen && cur_.kind == ItemKind::Step && cur_.step < learn_.steps) {
        Item s{cardIndex, ItemKind::Step, static_cast<uint8_t>(cur_.step + 1)};
        s.mode = stepMode(s.step);
        insertAhead(learn_.gap, s);
    }

    if (g != Grade::Again) return;
    if (cardIndex >= relearned_.size()) return;
    if (relearned_[cardIndex] >= kMaxRelearnPushes) return;
    ++relearned_[cardIndex];

    // A missed step is relearned in the mode it missed (meaning -> kanji
    // waits for the next day); any other card's mode is chosen when served.
    Item r{cardIndex};
    if (onScreen) r.mode = cur_.mode;
    insertAhead(kRelearnGap, r);
}

bool Queue::introduced(uint32_t cardIndex) {
    if (!open_ || cur_.kind != ItemKind::Intro || cur_.cardIndex != cardIndex) return false;
    open_ = false;
    ++answered_;  // the intro is a done item in the session bar
    Item s{cardIndex, ItemKind::Step, 1};
    s.mode = stepMode(1);
    insertAhead(learn_.gap, s);
    return true;
}

bool Queue::known(uint32_t cardIndex) {
    if (!open_ || cur_.kind != ItemKind::Intro || cur_.cardIndex != cardIndex) return false;
    open_ = false;
    ++answered_;
    // Asked at once as an ordinary item: a first answer, no steps.
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(pos_), Item{cardIndex});
    return true;
}

}  // namespace tgc
