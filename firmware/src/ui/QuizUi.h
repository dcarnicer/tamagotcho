#pragma once
// Pure logic of the quiz screens of the UX refresh (docs/ux-refresh.md §2.4,
// §2.5 and §6): the session progress in the header, option navigation, the
// hint bar policy, hold progress, auto-advance timing, the result pill's
// text within its pixel budget, the detail card's reading rows, and the
// learning mode's intro card (docs/learning-mode.md §3.2, §3.11): its
// meanings, reading rows and example words within their pixel budgets. Pure
// C++, no Arduino (native-tested in test_quizui); the drawing is in app/
// (Quiz, Detail, Intro) with render/Widgets.
//
// Pixel budgets assume the header and hint bar geometry of render/Widgets:
// captions are Font0 at 1x (6 px a character), a pill is its text plus 6 px of
// padding plus its icon and 3 px, and the hint bar puts a mark, 3 px, the word
// and 6 px per front-button item from the left gutter.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "pack/PackReader.h"
#include "pack/Utf8.h"
#include "quiz/Quiz.h"   // ReadingKind, readingsDisplay()
#include "ui/HintBar.h"  // captionChars(), kCaptionCharW, hintFrontEnd()
#include "ui/Theme.h"

namespace tgc {

// ---------------------------------------------------------------- pills
// Width of a caption pill with `text` and an icon `iconW` px wide (0: none),
// as widgets::pillWidth() draws it.
inline int pillWidthFor(const char* text, int iconW) {
    return captionChars(text) * kCaptionCharW + 6 + (iconW > 0 ? iconW + 3 : 0);
}

// Room for the pill on the left of a `w` px wide header whose right side holds
// `rightChars` caption characters and, if `battery`, the battery icon
// (widgets::drawHeader(): the right value keeps 6 px clear of the pill).
inline int headerPillRoom(int w, int rightChars, bool battery) {
    int rx = w - theme::kGutter;
    if (battery) rx -= 13 + 5;
    const int rightW = rightChars * kCaptionCharW;
    return rx - (rightW > 0 ? rightW + 6 : 0) - theme::kGutter;
}

// ---------------------------------------------------------------- progress
// The quiz header's counter and its 2 px session bar (§2.4): answered /
// (answered + left). `answered` counts the answers since the queue was built,
// `queued` is Queue::remaining() (the items after the one on screen) and `open`
// says the question on screen is not answered yet, so it still counts as left.
struct SessionProgress {
    uint32_t left;   // cards left, the open one included
    uint32_t done;   // the bar: done of total
    uint32_t total;
};

inline SessionProgress sessionProgress(uint32_t answered, uint32_t queued, bool open) {
    SessionProgress p{};
    p.left = queued + (open ? 1u : 0u);
    p.done = answered;
    p.total = answered + p.left;
    return p;
}

// ---------------------------------------------------------------- navigation
// §6.7: the front tap moves the focus to the next option (down), the side tap
// to the previous one (up); both wrap. An out-of-range `sel` is taken modulo.
inline int nextOption(int sel, int count) {
    if (count <= 0) return 0;
    return ((sel % count + count) % count + 1) % count;
}
inline int prevOption(int sel, int count) {
    if (count <= 0) return 0;
    return ((sel % count + count) % count + count - 1) % count;
}

// The arrow keys on options laid out in a grid of `cols` columns, row-major
// (the 2x2 tiles): dy -1/+1 changes row and dx -1/+1 column, each wrapping
// within its line, so a keyboard moves the way the tiles look, not in the
// buttons' order. A move into the missing cells of a short last row lands on
// the last option.
inline int gridOption(int sel, int count, int cols, int dx, int dy) {
    if (count <= 0 || cols <= 0) return 0;
    sel = (sel % count + count) % count;
    const int rows = (count + cols - 1) / cols;
    int r = sel / cols, c = sel % cols;
    if (dy) r = ((r + dy) % rows + rows) % rows;
    if (dx) {
        const int inRow = std::min(cols, count - (sel / cols) * cols);
        c = ((c + dx) % inRow + inRow) % inRow;
    }
    return std::min(r * cols + c, count - 1);
}

// ---------------------------------------------------------------- hint bar
// §6.5: the menus always show the hint bar. The quiz shows it while the user
// has fewer than kHintLearnAnswers lifetime answers (SrsStore's answer count);
// afterwards only once an unanswered question has gone kHintIdleMs without
// input, and from then on for the rest of that question -- its answer and
// detail card included -- so the tiles do not jump while the user reads the
// hints, moves and answers. Without the bar the tiles take its space.
constexpr uint32_t kHintLearnAnswers = 50;
constexpr uint32_t kHintIdleMs = 8000;

class QuizHints {
public:
    // A question came on screen: the idle clock starts, any idle bar goes.
    void question(uint32_t now) {
        quietSince_ = now;
        idle_ = false;
    }
    // Button activity: the idle clock starts again (a bar already shown stays).
    void input(uint32_t now) { quietSince_ = now; }
    // Whether the quiz shows the bar for `answers` lifetime answers.
    bool shown(uint32_t answers) const { return answers < kHintLearnAnswers || idle_; }
    // Advances the idle clock to `now`; only an unanswered (`open`) question
    // can go idle. True when shown() changed: a redraw with the bar is due.
    bool tick(uint32_t now, uint32_t answers, bool open) {
        const bool before = shown(answers);
        if (open && !idle_ && now - quietSince_ >= kHintIdleMs) idle_ = true;
        return shown(answers) != before;
    }

private:
    uint32_t quietSince_ = 0;
    bool idle_ = false;
};

// ---------------------------------------------------------------- hold progress
// §2.5: kHoldShowMs after the front button went down, the focused tile or row
// starts filling from the left and is full when the hold fires at `holdMs`
// (ui/InputPrefs.h holdMsFor()). Releasing earlier is a tap and the fill goes.
// It is redrawn every kHoldFrameMs.
constexpr uint32_t kHoldShowMs = 120;
constexpr uint32_t kHoldFrameMs = 40;

// The filled share (0..1) after a press of `heldMs`; negative before
// kHoldShowMs (nothing shown yet).
inline float holdFill(uint32_t heldMs, uint32_t holdMs) {
    if (heldMs < kHoldShowMs) return -1.0f;
    if (holdMs <= kHoldShowMs || heldMs >= holdMs) return 1.0f;
    return static_cast<float>(heldMs - kHoldShowMs) / static_cast<float>(holdMs - kHoldShowMs);
}

// ---------------------------------------------------------------- auto-advance
// §6.3: after a correct answer the next card follows by itself `ms` after the
// reveal (ui/InputPrefs.h advanceMsFor(); 0 = off, wait for a press). Any
// press skips the wait, and the wait never ends while a button is down: that
// press advances by itself, so a card is never skipped twice.
class AutoAdvance {
public:
    void start(uint32_t now, uint32_t ms) {
        on_ = ms > 0;
        at_ = now;
        ms_ = ms;
    }
    void cancel() { on_ = false; }
    bool active() const { return on_; }
    // The share of the wait still to go: 1 at the reveal, 0 at its end.
    float left(uint32_t now) const {
        if (!on_ || ms_ == 0) return 0.0f;
        const uint32_t e = now - at_;
        return e >= ms_ ? 0.0f : 1.0f - static_cast<float>(e) / static_cast<float>(ms_);
    }
    // True once, when the wait is over and no button is down; it then stops.
    bool due(uint32_t now, bool buttonDown) {
        if (!on_ || buttonDown || now - at_ < ms_) return false;
        on_ = false;
        return true;
    }

private:
    bool on_ = false;
    uint32_t at_ = 0;
    uint32_t ms_ = 0;
};

// ---------------------------------------------------------------- result pill
// The header pill after an answer (§6.4): the grade word and, for Good and
// Hard, the next interval ("GOOD · 4d"); the check or cross is the pill's
// icon. Writes the longest form whose pill fits `room` px -- "WORD · Nd",
// "WORD·Nd", then the word alone -- into `out` and returns that pill's width.
// `days` < 0 leaves the interval out (Again). `unit` follows the number ("d").
inline int resultPillText(char* out, size_t n, const char* word, int days, const char* unit,
                          int iconW, int room) {
    if (!out || n == 0) return 0;
    if (days >= 0) {
        snprintf(out, n, "%s \xC2\xB7 %d%s", word, days, unit);
        int pw = pillWidthFor(out, iconW);
        if (pw <= room) return pw;
        snprintf(out, n, "%s\xC2\xB7%d%s", word, days, unit);
        pw = pillWidthFor(out, iconW);
        if (pw <= room) return pw;
    }
    snprintf(out, n, "%s", word);
    return pillWidthFor(out, iconW);
}

// ---------------------------------------------------------------- detail card
// The stroke counter in the detail card's header ("stroke 4/13"): the worded
// `fmt` (two %u, the stroke and the count) when its widest value -- the last
// stroke -- fits `room` caption px, else the caller shows the bare "4/13". The
// choice is made once per card, so the word does not vanish at stroke 10.
inline bool strokeCounterWorded(const char* fmt, unsigned strokes, int room) {
    char buf[40];
    snprintf(buf, sizeof buf, fmt, strokes, strokes);
    return captionChars(buf) * kCaptionCharW <= room;
}

// One reading row of the detail card: its ON / KUN pill and every reading of
// that kind ("、"-separated, okurigana dots removed).
struct DetailReading {
    ReadingKind kind = ReadingKind::None;
    std::string text;
};

// The detail card's reading rows, top to bottom: ON, then KUN, each only when
// the card has that kind. A card without kun'yomi (or on'yomi) gets one row,
// not a pill beside a blank line, and the meanings above get the room; one
// with neither gets none. Returns the row count, 0..2.
inline int detailReadings(const Card& c, DetailReading out[2]) {
    int n = 0;
    const ReadingKind kinds[2] = {ReadingKind::On, ReadingKind::Kun};
    for (ReadingKind k : kinds) {
        std::string s = readingsDisplay(k == ReadingKind::On ? c.onyomi : c.kunyomi);
        if (s.empty()) continue;
        out[n].kind = k;
        out[n].text = std::move(s);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- intro card
// The intro card of learning mode (docs/learning-mode.md §3.2): a never-seen
// kanji's meanings, readings and example words before its first question.
// Text widths come from the caller as a per-code-point advance, `adv(cp)`:
// the renderer's charAdvance() in the UI font on the device (Latin from the
// Label face, kana and kanji from the pack's text set), a model of it in
// test_quizui, so the same decisions are checked natively.

// The intro's header: the NEW pill, then (decided per card, as on the detail
// card) the stroke counter in what is left of the header.
inline int introCounterRoom(int w, const char* pill, bool battery) {
    return headerPillRoom(w, 0, battery) - pillWidthFor(pill, 0) - 6;
}

// Width of UTF-8 `s` with the advance `adv` (0 for null).
template <typename Adv>
int textWidthBy(const char* s, Adv adv) {
    int w = 0;
    if (s)
        while (uint32_t cp = utf8Next(&s)) w += adv(cp);
    return w;
}

// Lines `s` takes when Renderer::wrap() breaks it at `maxW` px, at most
// `maxLines` (the last one then ends in "..."): the same rules, a break at a
// space, after any CJK character, else where the line is full. 0 for an empty
// text.
template <typename Adv>
int wrapLines(const char* s, int maxW, int maxLines, Adv adv) {
    int lines = 0;
    const char* p = s ? s : "";
    while (*p && lines < maxLines) {
        const char* q = p;
        const char* lastBreak = nullptr;
        const char* afterBreak = nullptr;
        const char* fitEnd = p;
        int w = 0;
        bool overflow = false;
        while (*q) {
            const char* start = q;
            const uint32_t cp = utf8Next(&q);
            const int a = adv(cp);
            if (w + a > maxW && start != p) {
                overflow = true;
                break;
            }
            if (cp == ' ' && w > 0) {
                lastBreak = start;
                afterBreak = q;
            }
            w += a;
            fitEnd = q;
            if (cp >= 0x2E80) {
                lastBreak = q;
                afterBreak = q;
            }
        }
        ++lines;
        if (!overflow) break;
        const bool useBreak = lastBreak && lastBreak > p;
        p = useBreak ? afterBreak : fitEnd;
        while (*p == ' ') ++p;
    }
    return lines;
}

// The meanings: the first, the one the quiz asks, on its own line in `text`;
// the others ("; "-separated as in the pack) on the next in `text-dim`.
struct IntroMeanings {
    std::string first;
    std::string rest;  // "" when the card has one meaning
};

inline IntroMeanings introMeanings(const char* m) {
    IntroMeanings out;
    if (!m) return out;
    const char* e = std::strstr(m, "; ");
    if (!e) {
        out.first = m;
        return out;
    }
    out.first.assign(m, static_cast<size_t>(e - m));
    out.rest = e + 2;
    return out;
}

// One reading row of the intro (§3.2): the first reading of the kind (the
// one the quiz asks) in `text`, then the others in `text-dim` after "、" when
// they all fit, else a "+N" caption (Font0) kIntroMoreGap px after it.
// On'yomi in the script of Settings > Study > On'yomi.
struct IntroReading {
    ReadingKind kind = ReadingKind::None;
    std::string first;
    std::string rest;  // "、" and the others, or "" (none, or they did not fit)
    int more = 0;      // the others' count for the "+N" caption, 0 for none
};
constexpr int kIntroMoreGap = 4;

// The intro's reading rows, ON then KUN, each only when the card has the
// kind (as the detail card's). `w` is the room for the text after the pill.
// When even "+N" does not fit after the first reading, the row shows the
// first alone (the reading wins over the count). Returns the row count, 0..2.
template <typename Adv>
int introReadings(const Card& c, bool onHiragana, int w, Adv adv, IntroReading out[2]) {
    static const char kSep[] = "\xE3\x80\x81";  // U+3001 "、"
    int n = 0;
    const ReadingKind kinds[2] = {ReadingKind::On, ReadingKind::Kun};
    for (ReadingKind k : kinds) {
        std::string all = readingsDisplay(k == ReadingKind::On ? c.onyomi : c.kunyomi);
        if (all.empty()) continue;
        if (k == ReadingKind::On && onHiragana) all = toHiragana(all);
        IntroReading& r = out[n++];
        r = IntroReading{};
        r.kind = k;
        const size_t sep = all.find(kSep);
        if (sep == std::string::npos) {
            r.first = all;
            continue;
        }
        r.first = all.substr(0, sep);
        if (textWidthBy(all.c_str(), adv) <= w) {
            r.rest = all.substr(sep);
            continue;
        }
        int others = 0;
        for (size_t p = sep; p != std::string::npos; p = all.find(kSep, p + 1)) ++others;
        char cap[12];
        snprintf(cap, sizeof cap, "+%d", others);
        if (textWidthBy(r.first.c_str(), adv) + kIntroMoreGap +
                captionChars(cap) * kCaptionCharW <=
            w)
            r.more = others;
    }
    return n;
}

// The example words (docs/learning-mode.md §3.11, phase 4). Each is its form
// (`text`), its reading (`text-dim`, the part the card's kanji reads in
// `text`) and its gloss (`text-dim`, up to kIntroGlossLines lines; none when
// the pack has no gloss for it), every line `lh` px, kIntroWordsGap px
// between two words.
struct IntroWordText {
    const char* word;
    const char* reading;
    const char* gloss;  // "" for none
};
struct IntroWordLines {
    bool oneLine = false;  // the form and the reading side by side
    int glossLines = 0;
    int height = 0;  // px
};
constexpr int kIntroWordGap = 6;   // the form to its reading on one line
constexpr int kIntroWordsGap = 2;  // between two words
constexpr int kIntroGlossLines = 2;

// Lays out up to two words in a column `w` px wide and `h` px tall: the form
// and the reading side by side when `sideBySide` and both fit, else on two
// lines, and the gloss under them. When they do not all fit, a gloss gives up
// its second line, the last word's first; then the last word goes. Returns
// how many words to draw (the first ones), with their lines in `out`.
template <typename Adv>
int planIntroWords(const IntroWordText* words, int n, int w, int h, int lh, bool sideBySide,
                   Adv adv, IntroWordLines out[2]) {
    if (n > 2) n = 2;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) {
        IntroWordLines& o = out[i];
        o.oneLine = sideBySide && textWidthBy(words[i].word, adv) + kIntroWordGap +
                                          textWidthBy(words[i].reading, adv) <=
                                      w;
        const char* g = words[i].gloss;
        o.glossLines = g && *g ? wrapLines(g, w, kIntroGlossLines, adv) : 0;
    }
    auto total = [&](int k) {
        int t = 0;
        for (int i = 0; i < k; ++i) {
            out[i].height = ((out[i].oneLine ? 1 : 2) + out[i].glossLines) * lh;
            t += out[i].height + (i > 0 ? kIntroWordsGap : 0);
        }
        return t;
    };
    for (int i = n - 1; i >= 0 && total(n) > h; --i)
        if (out[i].glossLines > 1) out[i].glossLines = 1;
    while (n > 0 && total(n) > h) --n;
    total(n);
    return n;
}

// Whether the words go under the reading rows on the card itself (the
// T-Display-S3's 170 px portrait) rather than on a second page (every 135 px
// panel, and the landscape forms): when the column, below its fullest rows
// (two meaning lines and two reading rows, `rowH` px each, from `columnTop`),
// still holds two words of a line and a gloss line each before `bottom`.
constexpr bool introWordsInline(int columnTop, int bottom, int lh, int rowH) {
    return bottom - (columnTop + 4 * rowH) >= 2 * (2 * lh) + kIntroWordsGap + 2;
}

// Where the intro puts its parts on a `w` x `h` panel (the hint bar always
// shown; `largePx` / `smallPx` the pack's set-0 / set-1 em, `lh` the UI font's
// line pitch). Page 1 is the detail card's frame: the kanji box (the quiz's
// prompt box) and the text column under it (portrait) or right of it
// (landscape), rows `lh` + 2 px apart down to `bottom`; the example words
// follow the reading rows when wordsInline. Otherwise they get page 2: a head
// (portrait: the set-1 kanji beside its first meaning, as the mockup;
// landscape: the kanji and the meaning on one text line), a rule, then the
// words across the panel, down to pageBottom: in portrait the form, the
// reading and the gloss each on its line (the mockup); in landscape the form
// and the reading side by side over a gloss line, which the 135 px height
// holds twice and where every N5 gloss fits one line.
struct IntroLayout {
    int px = 0, py = 0, pw = 0, ph = 0;  // the kanji box
    int colX = 0, colW = 0, colTop = 0;  // the text column
    int bottom = 0;                      // page 1: the rows end above it
    bool wordsInline = false;
    // Page 2.
    bool headKanji = false;  // the set-1 kanji (a box smallPx square) beside the meaning
    int headY = 0, headH = 0;
    int ruleY = 0;
    int wordsY = 0;
    int wordsW = 0;
    bool wordsSideBySide = false;
    int pageBottom = 0;  // the words' room ends here (inline words too)
};

inline IntroLayout introLayout(int w, int h, int largePx, int smallPx, int lh) {
    const theme::Layout l = theme::layoutFor(w, h);
    IntroLayout o;
    o.px = l.promptX;
    o.py = l.promptY;
    o.pw = l.promptW;
    o.ph = l.promptH;
    if (!l.landscape) {
        // As quizGeom(): a larger em gets a taller box, up to 45 % of h.
        const int want = std::min(largePx + 6, h * 45 / 100);
        if (want > o.ph) o.ph = want;
        o.colX = theme::kGutter;
        o.colTop = o.py + o.ph + 2;
    } else {
        o.colX = o.pw + theme::kGutter;
        o.colTop = theme::kHeaderH + 3;
    }
    o.colW = w - o.colX - theme::kGutter;
    o.bottom = h - theme::kHintH - 2;
    o.pageBottom = h - theme::kHintH - 1;
    o.wordsInline = introWordsInline(o.colTop, o.bottom, lh, lh + 2);
    o.headKanji = !l.landscape;
    o.wordsW = w - 2 * theme::kGutter;
    o.wordsSideBySide = l.landscape;
    if (o.headKanji) {
        o.headY = theme::kHeaderH + 2;
        o.headH = smallPx;
        o.ruleY = o.headY + o.headH + 2;
        o.wordsY = o.ruleY + 3;
    } else {
        o.headY = theme::kHeaderH + 1;
        o.headH = lh;
        o.ruleY = o.headY + o.headH + 1;
        o.wordsY = o.ruleY + 2;
    }
    return o;
}

// Page 1 of the intro as drawn in the text column of `l`: the asked meaning
// (two lines when it is the only one), the others on one dim line, the
// reading rows, and the example words when they all fit under them (the
// panel allows it, IntroLayout::wordsInline, and none would be lost);
// otherwise the words get page 2.
struct IntroCardPlan {
    int firstLines = 0;   // the asked meaning's lines (0: no meaning)
    bool restLine = false;
    int rows = 0;         // reading rows drawn
    IntroReading readings[2];
    int wordsY = 0;       // the inline words' top
    int words = 0;        // words drawn inline
    IntroWordLines wordLines[2];
    bool wordsPage = false;
};

// `tagW` is the wider reading pill, `lh` the line pitch; the words are the
// card's (`nWords`, none for a v1/v2 pack).
template <typename Adv>
IntroCardPlan planIntroCard(const IntroLayout& l, const Card& c, const IntroWordText* words,
                            int nWords, bool onHiragana, int tagW, int lh, Adv adv) {
    IntroCardPlan p;
    const IntroMeanings m = introMeanings(c.meanings);
    int y = l.colTop;
    if (!m.first.empty()) {
        p.firstLines = wrapLines(m.first.c_str(), l.colW, m.rest.empty() ? 2 : 1, adv);
        y += p.firstLines * lh + 2;
    }
    if (!m.rest.empty()) {
        p.restLine = true;
        y += lh + 2;
    }
    const int n = introReadings(c, onHiragana, l.colW - tagW - 5, adv, p.readings);
    while (p.rows < n && y + lh <= l.bottom + 2) {
        ++p.rows;
        y += lh + 2;
    }
    p.wordsY = y + 2;
    if (nWords <= 0) return p;
    if (l.wordsInline) {
        const int k = planIntroWords(words, nWords, l.colW, l.pageBottom - p.wordsY, lh, true, adv,
                                     p.wordLines);
        if (k == std::min(nWords, 2)) {
            p.words = k;
            return p;
        }
    }
    p.wordsPage = true;
    return p;
}

// The part of a word's reading the card's kanji reads (`readingStart`,
// `readingLen` in bytes, docs/pack-format.md): false, and the reading drawn
// as one dim range, when it is unknown (length 0) or does not lie inside
// `reading` on character boundaries.
inline bool readingSpanValid(const char* reading, unsigned start, unsigned len) {
    if (!reading || len == 0) return false;
    const size_t n = std::strlen(reading);
    if (start + len > n) return false;
    auto boundary = [&](size_t i) {
        return i >= n || (static_cast<uint8_t>(reading[i]) & 0xC0) != 0x80;
    };
    return boundary(start) && boundary(start + len);
}

}  // namespace tgc
