#pragma once
// Geometry of the settings home, the statistics, the date stepper and the done
// screen (docs/ux-refresh.md §2.1, §3 and the mockups in docs/ux/), for any
// w x h panel: StickS3 135x240 / 240x135 and T-Display-S3 170x320 / 320x170.
// Portrait stacks the parts, landscape puts the summary card, the ring and the
// calendar tile in a left column. Every part stays between the 16 px header
// and the 12 px hint bar. Flat: no rounded corners. Pure C++, no Arduino
// (native-tested in test_settings); main.cpp draws into these rectangles.
#include <cstdint>

#include "ui/Theme.h"

namespace tgc {
namespace layout {

struct Rect {
    int16_t x, y, w, h;
    constexpr int right() const { return x + w; }
    constexpr int bottom() const { return y + h; }
};

constexpr Rect rect(int x, int y, int w, int h) {
    return Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                static_cast<int16_t>(h)};
}

constexpr int imin(int a, int b) { return a < b ? a : b; }
constexpr int imax(int a, int b) { return a > b ? a : b; }

// ---------------------------------------------------------------- settings home
// The summary card (deck name, due/new, coverage bar) and the band the group
// rows scroll in. Portrait: the card is a 46 px band over the rows (all five
// fit). Landscape: the card is a left column and the rows go right of it (four
// visible, the list scrolls), as in the landscape mockup.
struct Home {
    Rect card;
    bool cardColumn;     // landscape: the card is a column left of the rows
    int16_t rowsX, rowsW;
    int16_t rowsTop, rowsBottom;  // the band layoutList() fits the rows into
};

constexpr int kHomeCardH = 46;  // portrait card: name line, counts, bar

constexpr Home homeFor(int w, int h) {
    using namespace theme;
    Home l{};
    const int hintY = h - kHintH;
    if (w > h) {
        const int cw = w * 35 / 100;  // 84 px of 240, 112 of 320
        const int top = kHeaderH + kMarkerBand;
        l.cardColumn = true;
        l.card = rect(kGutter, top, cw, hintY - kMarkerBand - top);
        l.rowsX = static_cast<int16_t>(kGutter + cw + kGutter);
        l.rowsW = static_cast<int16_t>(w - l.rowsX - kGutter);
        l.rowsTop = kHeaderH;
    } else {
        l.cardColumn = false;
        l.card = rect(kGutter, kHeaderH + 3, w - 2 * kGutter, kHomeCardH);
        l.rowsX = kGutter;
        l.rowsW = static_cast<int16_t>(w - 2 * kGutter);
        l.rowsTop = static_cast<int16_t>(l.card.bottom());
    }
    l.rowsBottom = static_cast<int16_t>(hintY);
    return l;
}

// ---------------------------------------------------------------- statistics
// The accuracy ring with its caption, six tiles in two columns (streak, best;
// due, new; learned, time) and the seen/total line. Portrait: ring, tiles,
// then the seen line across. Landscape: the ring, its caption and the seen
// line (caption over the bar) in a left column, the tiles right of it.
struct Stats {
    int16_t cx, cy, rOuter, rInner;  // the ring
    int16_t captionY;                // top of the caption cell under the ring
    Rect tiles[6];
    Rect seen;         // portrait: one caption line; landscape: caption + bar
    bool seenStacked;  // landscape: the caption sits over the bar
};

constexpr int kRingMaxR = 48;
constexpr int ringThickness(int r) { return r >= 40 ? 7 : 6; }

constexpr Stats statsFor(int w, int h) {
    using namespace theme;
    Stats s{};
    const int hintY = h - kHintH;
    const int top = kHeaderH + 3;
    if (w > h) {
        const int lw = w * 40 / 100;  // the left column: 96 of 240, 128 of 320
        // Under the ring: its caption (3 + 8), the seen caption (2 + 8) and
        // the bar (2 + 3), and 4 px to the hint bar.
        const int r = imin(imin(lw / 2 - 6, (hintY - top - 30) / 2), kRingMaxR);
        s.rOuter = static_cast<int16_t>(r);
        s.cx = static_cast<int16_t>(lw / 2);
        s.cy = static_cast<int16_t>(top + r);
        s.captionY = static_cast<int16_t>(s.cy + r + 3);
        s.seen = rect(kGutter, s.captionY + 10, lw - 2 * kGutter, 13);
        s.seenStacked = true;
        const int tx = lw + kGutter;
        const int tw = w - kGutter - tx;
        const int colW = (tw - kGap) / 2;
        const int tileH = (hintY - 2 - top - 2 * kGap) / 3;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 2, row = i / 2;
            const int x = col == 0 ? tx : tx + colW + kGap;
            s.tiles[i] = rect(x, top + row * (tileH + kGap), col == 0 ? colW : tw - colW - kGap,
                              tileH);
        }
    } else {
        const int r = imin((w - 63) / 2, kRingMaxR);  // 36 of 135: the hole fits "87" + "%"
        s.rOuter = static_cast<int16_t>(r);
        s.cx = static_cast<int16_t>(w / 2);
        s.cy = static_cast<int16_t>(kHeaderH + 4 + r);
        s.captionY = static_cast<int16_t>(s.cy + r + 3);
        const int tilesTop = s.captionY + 8 + 4;
        const int tileH = h >= 300 ? 40 : 34;
        const int tw = w - 2 * kGutter;
        const int colW = (tw - kGap) / 2;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 2, row = i / 2;
            const int x = col == 0 ? kGutter : kGutter + colW + kGap;
            s.tiles[i] = rect(x, tilesTop + row * (tileH + kGap),
                              col == 0 ? colW : tw - colW - kGap, tileH);
        }
        s.seen = rect(kGutter, tilesTop + 3 * tileH + 2 * kGap + 4, tw, 8);
        s.seenStacked = false;
    }
    s.rInner = static_cast<int16_t>(s.rOuter - ringThickness(s.rOuter));
    return s;
}

// The ring's filled sweep for `percent` (0..100), clockwise from 12 o'clock,
// in LovyanGFX's fillArc() degrees (0 = 3 o'clock, clockwise): the end angle
// of a sweep that starts at 270. 0 % draws nothing; 100 % is the full circle.
constexpr float kRingStartDeg = 270.0f;
constexpr float ringEndDeg(int percent) {
    return kRingStartDeg + 3.6f * static_cast<float>(imax(0, imin(100, percent)));
}

// True when a value `textW` px wide fits inside the ring's hole with 3 px to
// spare on either side.
constexpr bool ringFits(int textW, int rInner) { return textW <= 2 * rInner - 6; }

// ---------------------------------------------------------------- date stepper
// The calendar tile (an accent month band, the day in the 32 px numeral, the
// weekday), the "+N days" pill under it, the two notice lines ("Device was
// off.", "Last used ...") and the explanation / network status lines.
// Portrait: notice, tile, pill, explanation top to bottom. Landscape: tile and
// pill on the left, the text right of them.
struct Date {
    Rect notice;   // two caption lines
    Rect tile;
    int16_t bandH;            // the month band at the top of the tile
    int16_t pillY, pillCx;    // the pill is centred on pillCx
    Rect explain;  // caption lines, top-aligned
};

constexpr int kDateTileH = 76;  // band 13, day 38, weekday 21, 4 px under it
constexpr int kDateTileMaxW = 100;  // "Wednesday" in the label face (96 px) and 2 px a side

constexpr Date dateFor(int w, int h) {
    using namespace theme;
    Date d{};
    const int hintY = h - kHintH;
    const int top = kHeaderH + 3;
    d.bandH = 13;
    if (w > h) {
        const int tw = kDateTileMaxW;
        d.tile = rect(kGutter, top, tw, kDateTileH);
        d.pillY = static_cast<int16_t>(d.tile.bottom() + 5);
        d.pillCx = static_cast<int16_t>(kGutter + tw / 2);
        const int tx = kGutter + tw + 6;
        d.notice = rect(tx, top, w - kGutter - tx, 20);
        d.explain = rect(tx, d.notice.bottom() + 4, w - kGutter - tx,
                         hintY - 2 - (d.notice.bottom() + 4));
    } else {
        const int tw = imin(w - 2 * kGutter, kDateTileMaxW);
        d.notice = rect(kGutter, top, w - 2 * kGutter, 20);
        d.tile = rect((w - tw) / 2, d.notice.bottom() + 3, tw, kDateTileH);
        d.pillY = static_cast<int16_t>(d.tile.bottom() + 6);
        d.pillCx = static_cast<int16_t>(w / 2);
        const int ey = d.pillY + kPillH + 6;
        d.explain = rect(kGutter, ey, w - 2 * kGutter, hintY - 4 - ey);
    }
    return d;
}

// ---------------------------------------------------------------- done screen
// A check in a circle, "All caught up", tomorrow's due count and the streak
// (two caption lines), and the "+10 new" button over the hint bar. Portrait
// centres the parts in a column; landscape puts the circle left of the text.
struct Done {
    int16_t iconCx, iconCy, iconR;
    Rect title;               // label face, one line
    int16_t line1Y, line2Y;   // caption cells
    int16_t textX, textW;     // the column the title and the lines centre in
    Rect button;
};

constexpr Done doneFor(int w, int h) {
    using namespace theme;
    Done d{};
    const int hintY = h - kHintH;
    d.iconR = 22;
    if (w > h) {
        d.button = rect(kGutter, hintY - 2 - 24, w - 2 * kGutter, 24);
        const int mid = kHeaderH + (d.button.y - 2 - kHeaderH) / 2;
        d.iconCx = static_cast<int16_t>(kGutter + 8 + d.iconR);
        d.iconCy = static_cast<int16_t>(mid);
        d.textX = static_cast<int16_t>(d.iconCx + d.iconR + 10);
        d.textW = static_cast<int16_t>(w - kGutter - d.textX);
        d.title = rect(d.textX, mid - 22, d.textW, 20);
    } else {
        d.button = rect(kGutter, hintY - 4 - 28, w - 2 * kGutter, 28);
        d.iconCx = static_cast<int16_t>(w / 2);
        d.iconCy = static_cast<int16_t>(kHeaderH + 10 + d.iconR);
        d.textX = kGutter;
        d.textW = static_cast<int16_t>(w - 2 * kGutter);
        d.title = rect(kGutter, d.iconCy + d.iconR + 6, w - 2 * kGutter, 22);
    }
    d.line1Y = static_cast<int16_t>(d.title.bottom() + 3);
    d.line2Y = static_cast<int16_t>(d.line1Y + 13);
    return d;
}

}  // namespace layout
}  // namespace tgc
