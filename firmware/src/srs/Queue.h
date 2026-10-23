#pragma once
// Session queue: which card to ask next and in which of the three quiz modes.
// Pure C++, no Arduino; depends on pack/ and quiz/ for the card table and the
// mode enum, and on srs/ for the state and the day index. Design: docs/srs.md
// section 6; learning mode: docs/learning-mode.md section 3.4.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pack/PackReader.h"
#include "quiz/Quiz.h"
#include "srs/Srs.h"
#include "srs/SrsStore.h"

namespace tgc {

// What a queue item asks the app to show (docs/learning-mode.md section 3.4).
enum class ItemKind : uint8_t {
    Ask,    // a question in `mode`: a review, a new card with learning off, a relearn
    Intro,  // a never-seen kanji's intro card, no question; the app reports
            // introduced() or known()
    Step,   // a learning step after an intro: a question in `mode`
};

struct QueueItem {
    uint32_t cardIndex = 0;
    QuizMode mode = QuizMode::Meaning;  // unused for an Intro
    ItemKind kind = ItemKind::Ask;
    uint8_t step = 0;  // 1 or 2 for a Step, else 0
};

// Learning mode (docs/learning-mode.md sections 3.4 and 6, decision 5): an
// intro card for every never-seen kanji, then `steps` questions `gap` items
// apart (step 1 kanji -> meaning, step 2 a reading). Off by default: the queue
// is then exactly what it was before learning mode.
struct LearnOptions {
    bool on = false;
    uint8_t steps = 2;  // 1..Queue::kMaxSteps (clamped)
    uint8_t gap = 3;    // items between an intro and its step 1, and between steps (>= 1)
};

class Queue {
public:
    // Relearn pushes: insert the card this many items after the cursor, at
    // most this many times per card per session (section 6.6).
    static constexpr size_t kRelearnGap = 5;
    static constexpr uint8_t kMaxRelearnPushes = 3;
    // Soft session cap: at most this many scheduled items per rebuild. With
    // learning on, an intro takes 1 + its steps of it.
    static constexpr size_t kSessionCap = 200;
    // Learning steps: step 1 asks kanji -> meaning, step 2 a reading.
    static constexpr uint8_t kMaxSteps = 2;

    // Learning mode for the next rebuild() (the queue built is left as it is):
    // the app sets it at boot and when the setting changes, then rebuilds.
    void setLearning(const LearnOptions& opts);
    const LearnOptions& learning() const { return learn_; }

    // Rebuilds the session list for `pack` from `srs`. `rng` is accepted for
    // determinism/future fuzz; ordering here is fully specified by the design,
    // so it is currently unused. `extraNew` raises the daily new-card allowance
    // (the "study more" override on the done screen). Call on boot and on every
    // pack change. With learning on, the new cards are Intro items; learning
    // records (intros whose steps a rebuild dropped) are due reviews.
    void rebuild(const PackReader& pack, const SrsStore& srs, XorShiftRng& rng, uint16_t extraNew = 0);

    // Pops the next item (and chooses its mode). False when the queue is done.
    bool next(QueueItem* out);

    // Items left, including pending relearn pushes, and the learning steps the
    // intros and steps still to come will add (those of the item on screen
    // too), so the count does not jump up after each intro.
    size_t remaining() const;
    // Items done since the last rebuild: answers, and intros gone on from
    // (introduced() or known()). The quiz header's session progress:
    // done / (done + left).
    size_t answeredCount() const { return answered_; }

    // Reports the grade for `cardIndex`. Again re-inserts the card
    // kRelearnGap items ahead of the cursor, up to kMaxRelearnPushes times.
    // After a learning step, the next step goes `gap` items on, whatever the
    // grade; a missed step's relearn asks the step's mode again.
    void answered(uint32_t cardIndex, Grade g);

    // The learner went on from the intro of `cardIndex` (the item on screen):
    // step 1 goes `gap` items on (fewer items left: at the end). The app
    // records the exposure with SrsStore::introduce(). False, nothing changed,
    // when that intro is not the item on screen.
    bool introduced(uint32_t cardIndex);
    // "Known" on the intro of `cardIndex`: the card is asked at once as an
    // ordinary item (its first answer is a first review), with no steps.
    // False, nothing changed, when that intro is not the item on screen.
    bool known(uint32_t cardIndex);

private:
    struct Item {
        uint32_t cardIndex = 0;
        ItemKind kind = ItemKind::Ask;
        uint8_t step = 0;  // 1..kMaxSteps for a Step
        int8_t mode = -1;  // a fixed QuizMode (a step, a step's relearn); -1 = chosen when served
    };

    // Mode with the lowest modeSeen usage, ties broken toward a mode other
    // than `prev` and rotated cyclically from it. -1 if the card is unaskable
    // (no meaning and no reading).
    int chooseMode(const Card& c, int prev) const;
    // Learning steps still to come after `it`.
    size_t stepsAfter(const Item& it) const;
    // Inserts `it` `gap` items after the cursor, or at the end.
    void insertAhead(size_t gap, const Item& it);

    const PackReader* pack_ = nullptr;
    const SrsStore* srs_ = nullptr;
    LearnOptions learn_;

    std::vector<Item> items_;
    size_t pos_ = 0;
    size_t answered_ = 0;             // items done since rebuild()
    std::vector<uint8_t> seen_;      // per card index: already scheduled
    std::vector<uint8_t> relearned_;  // per card index: relearn pushes so far
    int prevMode_ = -1;               // mode of the last served question
    Item cur_;                        // the last item next() served, mode as served
    bool open_ = false;               // cur_ waits for answered(), introduced() or known()
};

}  // namespace tgc
