#pragma once
// Session queue: which card to ask next and in which of the three quiz modes.
// Pure C++, no Arduino; depends on pack/ and quiz/ for the card table and the
// mode enum, and on srs/ for the state and the day index. Design: docs/srs.md
// section 6.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pack/PackReader.h"
#include "quiz/Quiz.h"
#include "srs/Srs.h"
#include "srs/SrsStore.h"

namespace tgc {

struct QueueItem {
    uint32_t cardIndex = 0;
    QuizMode mode = QuizMode::Meaning;
};

class Queue {
public:
    // Relearn pushes: insert the card this many items after the cursor, at
    // most this many times per card per session (section 6.6).
    static constexpr size_t kRelearnGap = 5;
    static constexpr uint8_t kMaxRelearnPushes = 3;
    // Soft session cap: at most this many scheduled items per rebuild.
    static constexpr size_t kSessionCap = 200;

    // Rebuilds the session list for `pack` from `srs`. `rng` is accepted for
    // determinism/future fuzz; ordering here is fully specified by the design,
    // so it is currently unused. `extraNew` raises the daily new-card allowance
    // (the "study more" override on the done screen). Call on boot and on every
    // pack change.
    void rebuild(const PackReader& pack, const SrsStore& srs, XorShiftRng& rng, uint16_t extraNew = 0);

    // Pops the next item (and chooses its mode). False when the queue is done.
    bool next(QueueItem* out);

    // Items left, including pending relearn pushes.
    size_t remaining() const { return pos_ < items_.size() ? items_.size() - pos_ : 0; }
    // Answers reported since the last rebuild (the quiz header's session
    // progress: answered / (answered + left)).
    size_t answeredCount() const { return answered_; }

    // Reports the grade for `cardIndex`. Again re-inserts the card
    // kRelearnGap items ahead of the cursor, up to kMaxRelearnPushes times.
    void answered(uint32_t cardIndex, Grade g);

private:
    struct Item {
        uint32_t cardIndex = 0;
    };

    // Mode with the lowest modeSeen usage, ties broken toward a mode other
    // than `prev` and rotated cyclically from it. -1 if the card is unaskable
    // (no meaning and no reading).
    int chooseMode(const Card& c, int prev) const;

    const PackReader* pack_ = nullptr;
    const SrsStore* srs_ = nullptr;

    std::vector<Item> items_;
    size_t pos_ = 0;
    size_t answered_ = 0;             // answered() calls since rebuild()
    std::vector<uint8_t> seen_;      // per card index: already scheduled
    std::vector<uint8_t> relearned_;  // per card index: relearn pushes so far
    int prevMode_ = -1;               // mode of the last served item
};

}  // namespace tgc
