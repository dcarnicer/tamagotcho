#include "quiz/Quiz.h"

#include <cstring>
#include <utility>
#include <vector>

namespace tgc {

namespace {
const char kReadingSep[] = "\xE3\x80\x81";  // U+3001 "、"
}

std::string firstMeaning(const char* m) {
    const char* e = std::strstr(m, "; ");
    return e ? std::string(m, e - m) : std::string(m);
}

std::string firstReadingDisplay(const char* r) {
    const char* e = std::strstr(r, kReadingSep);
    std::string s = e ? std::string(r, e - r) : std::string(r);
    std::string out;
    for (char ch : s)
        if (ch != '.') out += ch;
    return out;
}

ReadingKind readingKindOf(const Card& c) {
    if (c.onyomi[0]) return ReadingKind::On;
    if (c.kunyomi[0]) return ReadingKind::Kun;
    return ReadingKind::None;
}

namespace {

// Display text for `c` in this mode/kind; empty if not available.
std::string displayFor(const Card& c, QuizMode mode, ReadingKind kind) {
    if (mode == QuizMode::Meaning) return firstMeaning(c.meanings);
    if (kind == ReadingKind::On) return firstReadingDisplay(c.onyomi);
    if (kind == ReadingKind::Kun) return firstReadingDisplay(c.kunyomi);
    return std::string();
}

}  // namespace

bool buildQuestion(const PackReader& pack, uint32_t cardIndex, QuizMode mode, Rng& rng, Question* out) {
    Card target;
    if (!pack.card(cardIndex, &target)) return false;

    ReadingKind kind = ReadingKind::None;
    if (mode == QuizMode::Reading) {
        kind = readingKindOf(target);
        if (kind == ReadingKind::None) return false;
    }
    std::string answer = displayFor(target, mode, kind);
    if (answer.empty()) return false;

    Question q;
    q.cardIndex = cardIndex;
    q.mode = mode;
    q.readingKind = kind;
    q.options[0] = answer;
    q.optionCard[0] = static_cast<uint16_t>(cardIndex);
    int n = 1;

    // strict: in Reading mode only accept cards of the same reading kind.
    auto tryAdd = [&](uint32_t idx, bool strict) {
        if (n >= kMaxOptions || idx == cardIndex || idx >= pack.cardCount()) return;
        Card c;
        if (!pack.card(idx, &c)) return;
        ReadingKind k = readingKindOf(c);
        if (mode == QuizMode::Reading && strict && k != kind) return;
        std::string s = displayFor(c, mode, k);
        if (s.empty()) return;
        for (int i = 0; i < n; ++i)
            if (q.options[i] == s) return;
        q.options[n] = s;
        q.optionCard[n] = static_cast<uint16_t>(idx);
        ++n;
    };

    std::vector<uint16_t> list(64);
    uint16_t total = 0;
    uint16_t got = pack.distractors(target, list.data(), static_cast<uint16_t>(list.size()), &total);
    const uint32_t numCards = pack.cardCount();

    // Passes: strict (same reading kind) first, then relaxed. In each, the
    // precomputed list (best first) comes before a random walk over all cards.
    for (int pass = 0; pass < 2 && n < kMaxOptions; ++pass) {
        const bool strict = (pass == 0);
        if (!strict && mode == QuizMode::Meaning) break;  // nothing new to relax
        for (uint16_t i = 0; i < got; ++i) tryAdd(list[i], strict);
        // Random walk: start + i*step mod N with gcd(step, N) == 1 visits every card once.
        uint32_t start = rng.below(numCards);
        uint32_t step = 1 + rng.below(numCards > 1 ? numCards - 1 : 1);
        for (;; --step) {
            uint32_t x = numCards, y = step;
            while (y) { uint32_t t = x % y; x = y; y = t; }
            if (x == 1) break;
        }
        for (uint32_t i = 0; i < numCards && n < kMaxOptions; ++i)
            tryAdd(static_cast<uint32_t>((start + static_cast<uint64_t>(i) * step) % numCards), strict);
    }

    if (n < 2) return false;

    // Fisher-Yates shuffle.
    for (int i = n - 1; i > 0; --i) {
        int j = static_cast<int>(rng.below(static_cast<uint32_t>(i + 1)));
        std::swap(q.options[i], q.options[j]);
        std::swap(q.optionCard[i], q.optionCard[j]);
    }
    q.count = n;
    for (int i = 0; i < n; ++i)
        if (q.optionCard[i] == cardIndex) q.correct = i;
    *out = q;
    return true;
}

}  // namespace tgc
