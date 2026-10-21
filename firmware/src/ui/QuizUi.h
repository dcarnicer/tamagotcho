#pragma once
// Pure logic of the quiz screens of the UX refresh (docs/ux-refresh.md §2.4,
// §2.5 and §6): the session progress in the header, option navigation, the
// hint bar policy, hold progress, auto-advance timing, the result pill's
// text within its pixel budget, and the detail card's reading rows. Pure C++,
// no Arduino (native-tested in test_quizui); the drawing is in main.cpp with
// render/Widgets.
//
// Pixel budgets assume the header and hint bar geometry of render/Widgets:
// captions are Font0 at 1x (6 px a character), a pill is its text plus 6 px of
// padding plus its icon and 3 px, and the hint bar puts a mark, 3 px, the word
// and 6 px per front-button item from the left gutter.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

}  // namespace tgc
