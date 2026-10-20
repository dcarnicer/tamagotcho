#include "quiz/Quiz.h"

#include <cstring>
#include <utility>
#include <vector>

namespace tgc {

namespace {
const char kReadingSep[] = "\xE3\x80\x81";  // U+3001 "、"
}

int meaningCount(const char* m) {
    if (!m || !*m) return 0;
    int n = 1;
    for (const char* p = m; (p = std::strstr(p, "; ")) != nullptr; ++p) ++n;
    return n;
}

std::string meaningAt(const char* m, int index) {
    const char* p = m;
    for (int i = 0; i < index; ++i) {
        p = std::strstr(p, "; ");
        if (!p) return std::string();
        p += 2;
    }
    const char* e = std::strstr(p, "; ");
    return e ? std::string(p, e - p) : std::string(p);
}

std::string firstReadingDisplay(const char* r) {
    const char* e = std::strstr(r, kReadingSep);
    std::string s = e ? std::string(r, e - r) : std::string(r);
    std::string out;
    for (char ch : s)
        if (ch != '.') out += ch;
    return out;
}

std::string readingsDisplay(const char* r) {
    std::string out;
    if (!r) return out;
    for (; *r; ++r)
        if (*r != '.') out += *r;
    return out;
}

ReadingKind readingKindOf(const Card& c) {
    if (c.onyomi[0]) return ReadingKind::On;
    if (c.kunyomi[0]) return ReadingKind::Kun;
    return ReadingKind::None;
}

bool hasReading(const Card& c, ReadingKind kind) {
    switch (kind) {
        case ReadingKind::On: return c.onyomi[0] != 0;
        case ReadingKind::Kun: return c.kunyomi[0] != 0;
        default: return false;
    }
}

namespace {

// Only this many of a card's meanings are offered as options. 1 = always the
// first (most typical) one: the lists in other languages carry derived or wrong
// extra senses (母: "madre", "base", "cimientos"), so for now we keep to the
// canonical first gloss and revisit with a better meaning source later.
constexpr int kOptionMeanings = 1;

// Reading display for a card: the asked kind if the card has it, else the
// card's primary kind (for the relaxed pass). Empty if it has no reading.
std::string readingDisplay(const Card& c, ReadingKind kind) {
    ReadingKind k = hasReading(c, kind) ? kind : readingKindOf(c);
    if (k == ReadingKind::On) return firstReadingDisplay(c.onyomi);
    if (k == ReadingKind::Kun) return firstReadingDisplay(c.kunyomi);
    return std::string();
}

// One of the card's (first kOptionMeanings) meanings at random.
std::string randomMeaning(const char* m, Rng& rng) {
    int n = meaningCount(m);
    if (n > kOptionMeanings) n = kOptionMeanings;
    return n ? meaningAt(m, static_cast<int>(rng.below(static_cast<uint32_t>(n)))) : std::string();
}

// A random meaning (among the first kOptionMeanings) not already among `used`
// (so options never share a gloss); "" if every meaning is taken.
std::string pickMeaning(const char* m, Rng& rng, const std::string* used, int usedN) {
    int n = meaningCount(m);
    if (n > kOptionMeanings) n = kOptionMeanings;
    if (n == 0) return std::string();
    int start = static_cast<int>(rng.below(static_cast<uint32_t>(n)));
    for (int k = 0; k < n; ++k) {
        std::string s = meaningAt(m, (start + k) % n);
        bool clash = false;
        for (int i = 0; i < usedN; ++i)
            if (used[i] == s) { clash = true; break; }
        if (!clash) return s;
    }
    return std::string();
}

// True if `want` is one of the card's readings of the given list (dots removed).
// Used so a distractor is never another valid reading of the target, which would
// make two options correct at once.
bool containsReading(const char* readings, const std::string& want) {
    if (!readings || !*readings || want.empty()) return false;
    const size_t sep = std::strlen(kReadingSep);
    const char* p = readings;
    while (*p) {
        const char* e = std::strstr(p, kReadingSep);
        std::string s(p, e ? static_cast<size_t>(e - p) : std::strlen(p));
        std::string cleaned;
        for (char ch : s)
            if (ch != '.') cleaned += ch;
        if (cleaned == want) return true;
        if (!e) break;
        p = e + sep;
    }
    return false;
}

// True if `want` is any of the card's meanings.
bool containsMeaning(const char* meanings, const std::string& want) {
    if (!meanings || !*meanings || want.empty()) return false;
    const char* p = meanings;
    while (*p) {
        const char* e = std::strstr(p, "; ");
        std::string s(p, e ? static_cast<size_t>(e - p) : std::strlen(p));
        if (s == want) return true;
        if (!e) break;
        p = e + 2;
    }
    return false;
}

}  // namespace

bool buildQuestion(const PackReader& pack, uint32_t cardIndex, QuizMode mode, Rng& rng, Question* out,
                   ReadingKind readingKind) {
    Card target;
    if (!pack.card(cardIndex, &target)) return false;

    ReadingKind kind = ReadingKind::None;
    if (mode == QuizMode::Reading) {
        kind = hasReading(target, readingKind) ? readingKind : readingKindOf(target);
        if (kind == ReadingKind::None) return false;
    }
    std::string answer =
        (mode == QuizMode::Reading) ? readingDisplay(target, kind) : randomMeaning(target.meanings, rng);
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
        for (int i = 0; i < n; ++i)
            if (q.optionCard[i] == idx) return;  // card already an option
        Card c;
        if (!pack.card(idx, &c)) return;
        std::string s;
        if (mode != QuizMode::Reading) {
            s = pickMeaning(c.meanings, rng, q.options, n);
            if (containsMeaning(target.meanings, s)) return;  // also a meaning of the target
            // Meaning -> kanji shows the kanji, not `s`: a candidate that also
            // means the prompt (any of its glosses) would be a second answer.
            if (mode == QuizMode::MeaningToKanji && containsMeaning(c.meanings, answer)) return;
        } else {
            if (strict && !hasReading(c, kind)) return;
            s = readingDisplay(c, kind);
            // Also a reading of the target: checked against both kinds, since
            // the relaxed pass offers a card's other kind (an on'yomi in a
            // kun'yomi question) and that may be one of the target's own.
            if (containsReading(target.onyomi, s) || containsReading(target.kunyomi, s)) return;
        }
        if (s.empty()) return;
        for (int i = 0; i < n; ++i)
            if (q.options[i] == s) return;
        q.options[n] = s;
        q.optionCard[n] = static_cast<uint16_t>(idx);
        ++n;
    };

    std::vector<uint16_t> list(64);
    uint16_t total = 0;
    uint16_t got = pack.distractors(target, list.data(), static_cast<uint16_t>(list.size()), &total,
                                    mode == QuizMode::Reading);
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
