#pragma once
// Multiple-choice question builder. Pure C++ (no Arduino).
#include <cstdint>
#include <string>

#include "pack/PackReader.h"

namespace tgc {

enum class QuizMode : uint8_t { Meaning, Reading };

// Which kind of reading a Reading question uses.
enum class ReadingKind : uint8_t { None, On, Kun };

constexpr int kMaxOptions = 4;

class Rng {
public:
    virtual ~Rng() = default;
    virtual uint32_t next() = 0;
    // Uniform-ish value in [0, n). n must be > 0.
    uint32_t below(uint32_t n) { return next() % n; }
};

// Small deterministic generator (xorshift32); seed the device one from esp_random().
class XorShiftRng : public Rng {
public:
    explicit XorShiftRng(uint32_t seed = 0x2545F491u) : s_(seed ? seed : 0x2545F491u) {}
    uint32_t next() override {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }

private:
    uint32_t s_;
};

struct Question {
    uint32_t cardIndex = 0;
    QuizMode mode = QuizMode::Meaning;
    ReadingKind readingKind = ReadingKind::None;  // only for Reading
    int count = 0;                                // 2..kMaxOptions (fewer only for tiny packs)
    std::string options[kMaxOptions];             // display strings
    uint16_t optionCard[kMaxOptions] = {0, 0, 0, 0};  // card index each option belongs to
    int correct = 0;                              // index into options
};

// Display helpers (exposed for tests).
// Meanings are "; "-separated ("day; sun; Japan"): how many, and the i-th
// (0-based) one. Empty list -> 0 / "".
int meaningCount(const char* meanings);
std::string meaningAt(const char* meanings, int index);
// First reading of a "、"-separated list with '.' okurigana markers removed
// ("た.べる、く.う" -> "たべる"). Empty if the list is empty.
std::string firstReadingDisplay(const char* readings);

// True if the card has a reading of this kind (None -> false).
bool hasReading(const Card& c, ReadingKind kind);

// The reading a card offers when no kind is requested: On if it has on'yomi,
// else Kun, else None.
ReadingKind readingKindOf(const Card& c);

// Builds a question for `cardIndex`. Returns false if the card cannot be asked
// in this mode (no meaning / no reading) or fewer than 2 distinct options exist.
// Distractors: the card's precomputed list first (cards with a reading of the
// asked kind in Reading mode), then random cards of the same kind, then random
// cards of any kind.
// For Reading mode, `readingKind` is the kind to ask for; if the card has none
// of that kind, its primary kind is used instead (so kanji with only on'yomi or
// only kun'yomi can still be asked). None means the card's primary kind.
bool buildQuestion(const PackReader& pack, uint32_t cardIndex, QuizMode mode, Rng& rng, Question* out,
                   ReadingKind readingKind = ReadingKind::None);

}  // namespace tgc
