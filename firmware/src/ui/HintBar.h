#pragma once
// The hint bar's layout (docs/ux-refresh.md §2.4): the front button's items
// (• tap, ▬ hold) from the left, the side button's right-aligned (its icon,
// optionally a word, and optionally a side-hold item: ▬ and a word). When a
// screen gives the side button a tap and a hold word and the two groups do
// not fit one row, the side group gets a second row above the front one.
// Pure C++, no Arduino: render/Widgets draws from planHints(), and the native
// suite test_quizui checks every string's fit.
//
// Captions are Font0 at 1x: 6 px a character, whatever its UTF-8 length.
#include "pack/Utf8.h"
#include "ui/Theme.h"

namespace tgc {

// ---------------------------------------------------------------- captions
constexpr int kCaptionCharW = 6;  // Font0 at 1x

// Code points in UTF-8 `s` (a caption's width is this times kCaptionCharW).
inline int captionChars(const char* s) {
    int n = 0;
    if (s)
        while (utf8Next(&s)) ++n;
    return n;
}

// ---------------------------------------------------------------- items
constexpr int kHintMarkGap = 3;  // a mark or the side icon to its word
constexpr int kHintItemGap = 6;  // between items, and the least between the two groups

// One item: its mark, then 3 px and the word (no word: the mark alone).
inline int hintItemW(int markW, const char* word) {
    return markW + (word ? kHintMarkGap + captionChars(word) * kCaptionCharW : 0);
}

// Where the front-button items end (the x after the last word, without its
// trailing 6 px) for a tap word and an optional hold word, with the tap mark
// `tapW` and hold mark `holdW` px wide. kGutter - 6 when there are none.
inline int hintFrontEnd(const char* tap, const char* hold, int tapW, int holdW) {
    int x = theme::kGutter;
    if (tap) x += hintItemW(tapW, tap) + kHintItemGap;
    if (hold) x += hintItemW(holdW, hold) + kHintItemGap;
    return x - kHintItemGap;
}

// ---------------------------------------------------------------- plan
// What a bar shows: the front tap and hold words with their marks' widths,
// the side tap's icon width (0: no side tap item) and optional word, and the
// side hold's optional word (drawn with the hold mark).
struct HintSpec {
    const char* tap = nullptr;
    const char* hold = nullptr;
    int tapMarkW = 0;
    int holdMarkW = 0;
    int sideIconW = 0;
    const char* sideWord = nullptr;
    const char* sideHold = nullptr;
};

// Where the side group goes. The front row is always the bottom kHintH px;
// with twoRows the side group is on the row above it, and the bar is `height`
// px tall. sideX is the side icon's x, sideHoldX the side-hold mark's x.
struct HintPlan {
    bool twoRows = false;
    bool sideWord = false;  // the side tap's word is drawn (else its icon alone)
    int sideX = 0;
    int sideHoldX = 0;
    int height = theme::kHintH;
};

inline HintPlan planHints(int w, const HintSpec& s) {
    HintPlan p;
    const int right = w - theme::kGutter;
    const int frontEnd = hintFrontEnd(s.tap, s.hold, s.tapMarkW, s.holdMarkW);
    const bool sideTap = s.sideIconW > 0;
    const int iconOnly = sideTap ? s.sideIconW : 0;
    const int withWord = sideTap ? hintItemW(s.sideIconW, s.sideWord) : 0;

    if (!s.sideHold) {
        // One row; the side word only when it keeps 6 px clear of the front
        // items, else the icon alone.
        p.sideWord = sideTap && s.sideWord && right - withWord >= frontEnd + kHintItemGap;
        p.sideX = right - (p.sideWord ? withWord : iconOnly);
        return p;
    }

    // Two side actions: the side tap (its words included), 6 px, the hold item.
    const int holdW = hintItemW(s.holdMarkW, s.sideHold);
    auto groupW = [&](int tapW) { return tapW + (tapW > 0 ? kHintItemGap : 0) + holdW; };
    p.sideWord = sideTap && s.sideWord;
    int start = right - groupW(p.sideWord ? withWord : iconOnly);
    if (start < frontEnd + kHintItemGap) {
        // A row of its own; only if even that is too narrow, the side tap
        // goes without its word.
        p.twoRows = true;
        p.height = 2 * theme::kHintH;
        if (start < theme::kGutter && p.sideWord) {
            p.sideWord = false;
            start = right - groupW(iconOnly);
        }
    }
    p.sideX = start;
    p.sideHoldX = right - holdW;
    return p;
}

}  // namespace tgc
