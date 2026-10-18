#pragma once
// Design tokens of the UX refresh (docs/ux-refresh.md §2.1, §2.2 and §6.1):
// RGB565 colours, spacing and the per-panel layout grid. Flat: no rounded
// corners anywhere (kRadius 0) and 2 px between tiles and rows. Pure C++, no
// Arduino (native-tested in test_ui).
//
// The colours are RGB565 exactly as the panel gets them (`render.py --tokens`
// in tools/ux-mockups prints them with their contrast ratios). Pass them as
// uint16_t: LovyanGFX reads a 32-bit colour argument as RGB888.
#include <cstdint>

namespace tgc {
namespace theme {

// color565(): 8-bit RGB to RGB565, usable in constant expressions.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---------------------------------------------------------------- colours
constexpr uint16_t kBg = 0x0000;           // background; glyph blend background
constexpr uint16_t kSurface = 0x18E4;      // idle tile / row
constexpr uint16_t kSurfaceHi = 0x2967;    // pills, progress track
constexpr uint16_t kLine = 0x3A0A;         // hairlines, outlines
constexpr uint16_t kGhost = 0x2988;        // unwritten strokes on the detail card
constexpr uint16_t kText = 0xFFFF;         // primary text, kanji
constexpr uint16_t kTextDim = 0xA577;      // titles, secondary values, hint labels
constexpr uint16_t kTextFaint = 0x636F;    // de-emphasised (dimmed options after reveal)
constexpr uint16_t kAccent = 0x2B1B;       // focus fill
constexpr uint16_t kAccentPress = 0x64FF;  // hold-progress fill (transient)
constexpr uint16_t kAccentHi = 0x7D5F;     // accent text/icons on black; meaning mode
constexpr uint16_t kOk = 0x0408;           // correct tile
constexpr uint16_t kOkHi = 0x5EF1;         // correct text/icons on black
constexpr uint16_t kOkBg = 0x0183;         // correct result pill
constexpr uint16_t kErr = 0xC945;          // wrong-tile outline (2 px)
constexpr uint16_t kErrHi = 0xFB6D;        // wrong text/icons on black
constexpr uint16_t kErrBg = 0x3882;        // wrong-tile fill, wrong result pill
constexpr uint16_t kWarn = 0xFE26;         // current stroke, low battery, date notice, Hard
constexpr uint16_t kModeReading = 0x6EF9;  // reading-mode pill
constexpr uint16_t kModeKanji = 0xCD1F;    // kanji-mode pill

// ---------------------------------------------------------------- spacing
constexpr int kGutter = 4;  // screen edge to content
constexpr int kGap = 2;     // between tiles and between list rows (§6.1: was 4)
constexpr int kRadius = 0;  // corners: flat everywhere (§6.1)

constexpr int kHeaderH = 16;   // header incl. its bottom 2 px progress / 1 px hairline
constexpr int kHintH = 12;     // hint bar at the bottom
constexpr int kPillH = 11;     // caption pill
constexpr int kRowPadX = 6;    // list row: left/right padding
constexpr int kTilePadX = 8;   // option tile: label padding on the left
constexpr int kTileMarkW = 20; // option tile: room kept on the right for the check/cross
constexpr int kScrollW = 2;    // list scroll bar
constexpr int kMarkerBand = 4; // above/below a list: room for the small up/down markers

// ---------------------------------------------------------------- layout
// Where things go on one panel orientation (§2.1). y/x ranges are half-open:
// `promptY`..`promptY + promptH`. Portrait stacks prompt over answers;
// landscape puts the prompt in a left column.
struct Layout {
    int16_t w, h;
    bool landscape;
    int16_t row1H;  // list row, one line
    int16_t row2H;  // list row, two lines (label + caption)
    // Quiz (used from phase 2 on): the prompt box and the answer area.
    int16_t promptX, promptY, promptW, promptH;
    int16_t answersX, answersY, answersW, answersH;
    // Hint bar top (h - kHintH).
    int16_t hintY;
};

// The grid for a w x h panel: StickS3 135x240 / 240x135 and T-Display-S3
// 170x320 come from the table in §2.1; the T-Display's 320x170 landscape and
// any other size are derived the same way (portrait when h > w).
constexpr Layout layoutFor(int w, int h) {
    const bool land = w > h;
    Layout l{};
    l.w = static_cast<int16_t>(w);
    l.h = static_cast<int16_t>(h);
    l.landscape = land;
    l.hintY = static_cast<int16_t>(h - kHintH);
    if (!land) {
        // Portrait: rows 26/36 (30/40 from 300 px up), the 96 px kanji box
        // under the header, the answers between it and the hint bar.
        const bool tall = h >= 300;
        l.row1H = tall ? 30 : 26;
        l.row2H = tall ? 40 : 36;
        l.promptX = 0;
        l.promptY = kHeaderH;
        l.promptW = static_cast<int16_t>(w);
        l.promptH = 102;  // y 16..117: a 96 px em and 3 px either side
        l.answersX = kGutter;
        l.answersY = 120;
        l.answersW = static_cast<int16_t>(w - 2 * kGutter);
        l.answersH = static_cast<int16_t>(l.hintY - 4 - l.answersY);  // 120..223 / 120..303
    } else {
        // Landscape: rows 22/32 (26/36 from 160 px up), the kanji in a left
        // column of 100 px (120 on wider panels), the answers right of it.
        const bool tall = h >= 160;
        l.row1H = tall ? 26 : 22;
        l.row2H = tall ? 36 : 32;
        l.promptX = 0;
        l.promptY = kHeaderH;
        l.promptW = static_cast<int16_t>(w >= 300 ? 120 : 100);
        l.promptH = static_cast<int16_t>(l.hintY - kHeaderH);
        l.answersX = static_cast<int16_t>(l.promptW + kGutter);
        l.answersY = static_cast<int16_t>(kHeaderH + 3);
        l.answersW = static_cast<int16_t>(w - l.answersX - kGutter);
        l.answersH = static_cast<int16_t>(l.hintY - 1 - l.answersY);
    }
    return l;
}

}  // namespace theme
}  // namespace tgc
