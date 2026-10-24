#include "render/Widgets.h"

#include <algorithm>
#include <cmath>

#include "pack/Utf8.h"
#include "ui/HintBar.h"
#include "ui/UiFace.h"
#include "ui/UiFont.h"

namespace tgc {
namespace widgets {

namespace {
using namespace theme;

constexpr uint8_t kCaption = Renderer::kTextBuiltinSmall;
constexpr int kCaptionCap = 7;  // Font0: cap height, the 8th row is the descender

// Top of an icon / caption vertically centred in a band of height `h` at y.
inline int centreY(int y, int h, int inkH) { return y + (h - inkH + 1) / 2; }

// Clips `d` to (x, y, w, h) within its current clip rect and puts that one
// back on exit, so a widget drawn under an outer clip (a hold-progress frame
// redraws its screen clipped to one row) stays inside it. empty(): nothing of
// the widget is inside the outer clip.
class ClipScope {
public:
    ClipScope(lgfx::LovyanGFX& d, int x, int y, int w, int h) : d_(d) {
        d_.getClipRect(&ox_, &oy_, &ow_, &oh_);
        const int32_t x0 = std::max<int32_t>(x, ox_);
        const int32_t y0 = std::max<int32_t>(y, oy_);
        const int32_t x1 = std::min<int32_t>(x + w, ox_ + ow_);
        const int32_t y1 = std::min<int32_t>(y + h, oy_ + oh_);
        empty_ = x1 <= x0 || y1 <= y0;
        if (empty_)
            d_.setClipRect(0, 0, 0, 0);
        else
            d_.setClipRect(x0, y0, x1 - x0, y1 - y0);
    }
    ~ClipScope() { d_.setClipRect(ox_, oy_, ow_, oh_); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;
    bool empty() const { return empty_; }

private:
    lgfx::LovyanGFX& d_;
    int32_t ox_ = 0, oy_ = 0, ow_ = 0, oh_ = 0;
    bool empty_ = false;
};

float gPress = -1.0f;      // setPressFill()
bool gFocusNoted = false;  // takeFocusRow()
Rect gFocusRow{};

// Width of the holding part of a `w` px tile or row filled by `fill`.
inline int pressSplit(int w, float fill) {
    if (fill < 0.0f) fill = 0.0f;
    if (fill > 1.0f) fill = 1.0f;
    return static_cast<int>(std::lround(w * fill));
}
}  // namespace

void setPressFill(float fill) { gPress = fill; }
float pressFill() { return gPress; }

bool takeFocusRow(Rect* out) {
    const bool had = gFocusNoted;
    if (had && out) *out = gFocusRow;
    gFocusNoted = false;
    return had;
}

int drawIcon(lgfx::LovyanGFX& d, const Icon& icon, int x, int y, uint16_t ink) {
    drawBitmap1bpp(d, x, y, icon.bits, icon.w, icon.h, ink);
    return icon.w;
}

int captionWidth(const char* s) {
    int n = 0;
    if (s)
        while (utf8Next(&s)) ++n;
    return n * 6;
}

int drawCaption(lgfx::LovyanGFX& d, Renderer& r, const char* s, int x, int y, uint16_t fg,
                uint16_t bg) {
    return s ? r.drawText(d, s, x, y, fg, bg, kCaption) : 0;
}

// ---------------------------------------------------------------- header

void drawBattery(lgfx::LovyanGFX& d, int x, int y, int pct) {
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    const uint16_t c = pct < 10 ? kErrHi : (pct < 20 ? kWarn : kTextDim);
    d.drawRect(x, y, 12, 7, c);
    d.fillRect(x + 12, y + 2, 1, 3, c);
    const int fill = std::max(1, (8 * pct + 50) / 100);
    d.fillRect(x + 2, y + 2, fill, 3, c);
}

int pillWidth(const char* text, const Icon* icon) {
    return captionWidth(text) + 6 + (icon ? icon->w + 3 : 0);
}

// A header title: a text-dim caption, or, for a title made only of kana the
// Kana face has (the app's name, たまごっちょ: Font0 has no kana), that face
// with its ink centred in the bar above the hairline. Returns the width drawn.
static int drawHeaderTitle(lgfx::LovyanGFX& d, Renderer& r, int w, const char* title) {
    if (*title && uiCanDraw(uifont::kKana, title))
        return r.drawLabel(d, title, kGutter, 0, w - 2 * kGutter, kHeaderH - 1, kTextDim, kBg,
                           Renderer::kTextUiKana);
    return drawCaption(d, r, title, kGutter, 4, kTextDim, kBg);
}

int drawPill(lgfx::LovyanGFX& d, Renderer& r, int x, int y, const char* text, uint16_t fg,
             uint16_t bg, const Icon* icon) {
    const int w = pillWidth(text, icon);
    d.fillRect(x, y, w, kPillH, bg);
    int tx = x + 3;
    if (icon) {
        drawIcon(d, *icon, tx, centreY(y, kPillH, icon->h), fg);
        tx += icon->w + 3;
    }
    drawCaption(d, r, text, tx, y + 2, fg, bg);
    return w;
}

void drawHeader(lgfx::LovyanGFX& d, Renderer& r, int w, const Header& h) {
    d.fillRect(0, 0, w, kHeaderH, kBg);
    int left = kGutter;
    if (h.pill)
        left += drawPill(d, r, kGutter, 2, h.pill, h.pillFg, h.pillBg, h.pillIcon);
    else if (h.title)
        left += drawHeaderTitle(d, r, w, h.title);
    int rx = w - kGutter;
    if (h.battery >= 0) {
        rx -= 13;
        drawBattery(d, rx, 4, h.battery);
        rx -= 5;
    }
    if (h.wifi) {
        rx -= kIconWifi.w;
        drawIcon(d, kIconWifi, rx, 4, kAccentHi);
        rx -= 5;
    }
    if (h.right) {
        // Cut from the left edge rather than run into the title.
        const int room = rx - left - 6;
        if (room > 6)
            r.drawLabel(d, h.right, rx - room, 4, room, kCaptionCap, kTextDim, kBg, kCaption,
                        Align::Right);
    }
    if (h.progressNum >= 0 && h.progressDen > 0) {
        drawProgress(d, 0, kHeaderH - 2, w, 2, h.progressNum, h.progressDen);
    } else {
        d.fillRect(0, kHeaderH - 1, w, 1, kLine);
    }
}

// ---------------------------------------------------------------- hint bar

namespace {
HintSpec hintSpec(const Hints& h) {
    if (h.keys)
        return keyHintSpec(h.tap, h.tapKey, h.hold, h.side ? &h.sideKey : nullptr, h.sideWord,
                           h.sideHold);
    HintSpec s;
    s.tap = h.tap;
    s.hold = h.hold;
    s.tapMarkW = kIconTap.w;
    s.holdMarkW = kIconHold.w;
    s.sideIconW = h.side ? h.side->w : 0;
    s.sideWord = h.sideWord;
    s.sideHold = h.sideHold;
    return s;
}
}  // namespace

int hintBarHeight(int w, const Hints& hints) { return planHints(w, hintSpec(hints)).height; }

void drawHintBar(lgfx::LovyanGFX& d, Renderer& r, int w, int h, const Hints& hints) {
    // ui/HintBar.h decides where the side group goes; the front row is the
    // bottom 12 px on every screen.
    const HintPlan plan = planHints(w, hintSpec(hints));
    const int y = h - kHintH;
    d.fillRect(0, h - plan.height, w, plan.height, kBg);
    auto item = [&](int x, int row, const Icon& mark, const char* word, uint16_t markInk,
                    uint16_t wordInk) {
        drawIcon(d, mark, x, row + 2 + (7 - mark.h) / 2, markInk);
        if (word) drawCaption(d, r, word, x + mark.w + kHintMarkGap, row + 2, wordInk, kBg);
        return x + hintItemW(mark.w, word) + kHintItemGap;
    };
    // A keycap mark (keyboard boards): its box from 1 px under the row's top,
    // the icon centred in it or the caption on the row's text line.
    auto keyItem = [&](int x, int row, const KeyCap& key, const char* word, uint16_t markInk,
                       uint16_t wordInk) {
        const int w = keyCapW(key);
        d.fillRect(x, row + 1, w, kKeyCapH, kSurfaceHi);
        if (key.icon)
            drawIcon(d, *key.icon, x + kKeyCapPadX, row + 1 + (kKeyCapH - key.icon->h) / 2, markInk);
        else
            drawCaption(d, r, key.text, x + kKeyCapPadX, row + 2, markInk, kSurfaceHi);
        if (word) drawCaption(d, r, word, x + w + kHintMarkGap, row + 2, wordInk, kBg);
        return x + hintItemW(w, word) + kHintItemGap;
    };
    const int sy = plan.twoRows ? y - kHintH : y;
    const bool lit = hints.holding || gPress >= 0.0f;
    const uint16_t sideInk = hints.sideDim ? kTextFaint : kText;
    const uint16_t sideWordInk = hints.sideDim ? kTextFaint : kTextDim;
    const char* sideWord = plan.sideWord ? hints.sideWord : nullptr;
    int x = kGutter;
    if (hints.keys) {
        if (hints.tap) x = keyItem(x, y, hints.tapKey, hints.tap, kText, kTextDim);
        if (hints.hold)
            keyItem(x, y, kKeyEnter, hints.hold, lit ? kAccentHi : kText, lit ? kText : kTextDim);
        if (hints.side) keyItem(plan.sideX, sy, hints.sideKey, sideWord, sideInk, sideWordInk);
        if (hints.sideHold) keyItem(plan.sideHoldX, sy, kKeyEsc, hints.sideHold, kText, kTextDim);
        return;
    }
    if (hints.tap) x = item(x, y, kIconTap, hints.tap, kText, kTextDim);
    if (hints.hold)
        item(x, y, kIconHold, hints.hold, lit ? kAccentHi : kText, lit ? kText : kTextDim);
    if (hints.side) item(plan.sideX, sy, *hints.side, sideWord, sideInk, sideWordInk);
    if (hints.sideHold) item(plan.sideHoldX, sy, kIconHold, hints.sideHold, kText, kTextDim);
}

// ---------------------------------------------------------------- tile

namespace {
// The tile's background and ink for `state`, its fill (and the wrong tile's
// outline), then `content(fg, bg)` once per colour part, clipped to it, so the
// anti-aliased ink blends against the colour under it. The holding tile is
// `accent-press` from the left by `fill`, `accent` after it.
template <typename Content>
void tileBody(lgfx::LovyanGFX& d, int x, int y, int w, int h, TileState state, float fill,
              Content&& content) {
    uint16_t bg = kSurface;
    uint16_t fg = kText;
    switch (state) {
        case TileState::Idle: bg = kSurface; break;
        case TileState::Focus:
        case TileState::Holding: bg = kAccent; break;
        case TileState::Correct: bg = kOk; break;
        case TileState::Wrong: bg = kErrBg; break;
        case TileState::Dimmed:
            bg = kBg;
            fg = kTextFaint;
            break;
    }
    d.fillRect(x, y, w, h, bg);
    if (state == TileState::Wrong) {
        d.drawRect(x, y, w, h, kErr);
        d.drawRect(x + 1, y + 1, w - 2, h - 2, kErr);
    }
    if (state != TileState::Holding) {
        ClipScope clip(d, x, y, w, h);
        content(fg, bg);
        return;
    }
    const int split = pressSplit(w, fill);
    if (split > 0) {
        d.fillRect(x, y, split, h, kAccentPress);
        ClipScope clip(d, x, y, split, h);
        content(fg, kAccentPress);
    }
    if (split < w) {
        ClipScope clip(d, x + split, y, w - split, h);
        content(fg, bg);
    }
}

// The check or cross of a correct or wrong tile: right of the label,
// vertically centred, or (`corner`, grid cells) in the top-right corner.
void tileMark(lgfx::LovyanGFX& d, int x, int y, int w, int h, TileState state, bool corner) {
    if (state == TileState::Correct) {
        const Icon& ic = kIconCheck;
        drawIcon(d, ic, x + w - ic.w - (corner ? 4 : 6), corner ? y + 4 : centreY(y, h, ic.h), kText);
    } else if (state == TileState::Wrong) {
        const Icon& ic = kIconCross;
        drawIcon(d, ic, x + w - ic.w - (corner ? 5 : 7), corner ? y + 5 : centreY(y, h, ic.h),
                 kErrHi);
    }
}
}  // namespace

void drawTile(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, TileState state,
              const char* label, float fill, uint8_t set, Align align) {
    ClipScope outer(d, x, y, w, h);
    if (outer.empty()) return;
    const bool marked = state == TileState::Correct || state == TileState::Wrong;
    const bool grid = align == Align::Centre;
    // Label box: a grid cell centres in the whole cell; a row starts 8 px in
    // and keeps clear of the mark only when there is one.
    const int lx = grid ? x + 2 : x + kTilePadX;
    const int lw = grid ? w - 4 : w - kTilePadX - (marked ? kTileMarkW : 6);
    const int lh = r.lineHeight(set);
    const bool twoLines = !grid && label && h >= 2 * lh + 4 && r.textWidth(label, set) > lw;
    tileBody(d, x, y, w, h, state, fill, [&](uint16_t fg, uint16_t bg) {
        if (!label || !*label) return;
        if (twoLines)
            r.drawWrapped(d, label, lx, y + (h - 2 * lh) / 2, lw, 2, fg, bg, set);
        else
            r.drawLabel(d, label, lx, y, lw, h, fg, bg, set, align);
    });
    tileMark(d, x, y, w, h, state, grid);
}

void drawKanjiTile(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, TileState state,
                   uint32_t cp, uint8_t set, float fill) {
    ClipScope outer(d, x, y, w, h);
    if (outer.empty()) return;
    tileBody(d, x, y, w, h, state, fill,
             [&](uint16_t fg, uint16_t bg) { r.drawKanji(d, set, cp, x, y, w, h, fg, bg); });
    tileMark(d, x, y, w, h, state, true);
}

// ---------------------------------------------------------------- row

namespace {
// A row's content on `bg` (drawRow() fills the row first).
void rowContent(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, const Row& row,
                uint16_t bg) {
    const bool f = row.focused;
    const bool muted = row.muted && !f;
    // A two-line row keeps its last 13 px for the caption.
    const bool twoLines = row.caption != nullptr;
    const int lineH = twoLines ? h - 13 : h;
    int lx = x + kRowPadX;
    if (row.radio != Radio::None) {
        const bool on = row.radio == Radio::On;
        drawIcon(d, on ? kIconRadioOn : kIconRadioOff, lx, centreY(y, lineH, 7),
                 f ? kText : (on ? kAccentHi : kTextFaint));
        lx += 7 + 5;
    }
    if (row.icon) {
        drawIcon(d, *row.icon, lx, centreY(y, lineH, row.icon->h), f ? kText : row.iconColor);
        lx += row.icon->w + 5;
    }
    int rx = x + w - kRowPadX;
    if (row.mark) {
        drawIcon(d, *row.mark, rx - row.mark->w, centreY(y, lineH, row.mark->h),
                 f ? kText : row.markColor);
        rx -= row.mark->w + 4;
    } else if (row.chevron) {
        drawIcon(d, kIconChevron, rx - kIconChevron.w, centreY(y, lineH, kIconChevron.h),
                 f ? kText : kTextFaint);
        rx -= kIconChevron.w + 4;
    }
    const uint16_t valueFg = f ? kText : kTextDim;
    const int valueY = centreY(y, lineH, kCaptionCap);
    const bool hasValue = row.value && *row.value;
    // A tag pill follows the label, 4 px after it.
    const int tagW = row.tag ? pillWidth(row.tag) + 4 : 0;
    int labelRoom = rx - lx;
    if (hasValue && row.keepLabel) {
        // The label as it is; the value gets what is left, cut if needed.
        const int labelW =
            std::min(r.textWidth(row.label, Renderer::kTextUi) + tagW, labelRoom);
        const int room = rx - lx - labelW - 5;
        if (room >= 12)
            r.drawLabel(d, row.value, rx - room, valueY, room, kCaptionCap, valueFg, bg, kCaption,
                        Align::Right);
    } else if (hasValue && row.valueIfFits) {
        const int vw = captionWidth(row.value);
        if (r.textWidth(row.label, Renderer::kTextUi) + tagW + 5 + vw <= labelRoom) {
            drawCaption(d, r, row.value, rx - vw, valueY, valueFg, bg);
            labelRoom -= vw + 5;
        }
    } else if (hasValue) {
        const int vw = captionWidth(row.value);
        drawCaption(d, r, row.value, rx - vw, valueY, valueFg, bg);
        labelRoom -= vw + 5;
    }
    const int labelW = r.drawLabel(d, row.label, lx, y, labelRoom - tagW, lineH,
                                   f ? kText : (muted ? kTextFaint : row.labelColor), bg,
                                   Renderer::kTextUi);
    if (row.tag)
        drawPill(d, r, lx + labelW + 4, centreY(y, lineH, kPillH), row.tag, f ? kText : kTextDim,
                 kSurfaceHi);
    const int barX = x + kRowPadX;
    const int barW = w - 2 * kRowPadX;
    const uint16_t barFill = f ? kText : kAccent;
    if (twoLines && row.progress >= 0)
        drawProgress(d, barX, y + h - 9, barW, 4, row.progress, 100, barFill, kSurfaceHi);
    else if (twoLines)
        r.drawLabel(d, row.caption, barX, y + h - 11, barW, kCaptionCap,
                    f ? kText : (muted ? kTextFaint : kTextDim), bg, kCaption);
    else if (row.progress >= 0)
        drawProgress(d, barX, y + h - 4, barW, 2, row.progress, 100, barFill, kSurfaceHi);
}
}  // namespace

void drawRow(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, const Row& row) {
    ClipScope clip(d, x, y, w, h);
    if (clip.empty()) return;
    const bool muted = row.muted && !row.focused;
    const uint16_t bg = row.focused ? kAccent : (muted ? kBg : kSurface);
    d.fillRect(x, y, w, h, bg);
    if (muted) d.drawRect(x, y, w, h, kLine);
    if (!row.focused || gPress < 0.0f) {
        rowContent(d, r, x, y, w, h, row, bg);
        return;
    }
    // Holding: accent-press from the left, the content drawn once per part.
    const int split = pressSplit(w, gPress);
    if (split > 0) {
        d.fillRect(x, y, split, h, kAccentPress);
        ClipScope part(d, x, y, split, h);
        rowContent(d, r, x, y, w, h, row, kAccentPress);
    }
    if (split < w) {
        ClipScope part(d, x + split, y, w - split, h);
        rowContent(d, r, x, y, w, h, row, bg);
    }
}

// ---------------------------------------------------------------- progress

void drawProgress(lgfx::LovyanGFX& d, int x, int y, int w, int h, int num, int den, uint16_t fill,
                  uint16_t track) {
    d.fillRect(x, y, w, h, track);
    if (den <= 0 || num <= 0) return;
    if (num > den) num = den;
    const int fw = static_cast<int>(static_cast<int64_t>(w) * num / den);
    if (fw > 0) d.fillRect(x, y, fw, h, fill);
}

// ---------------------------------------------------------------- list

void drawList(lgfx::LovyanGFX& d, const ListView& list, int x, int w, const RowDrawer& row) {
    const int first = list.first();
    const int n = list.visible();
    for (int i = first; i < first + n; ++i) {
        const bool focused = i == list.focus();
        row(i, x, list.rowY(i), w, list.rowH(), focused);
        if (focused) {
            gFocusRow = Rect{static_cast<int16_t>(x), static_cast<int16_t>(list.rowY(i)),
                             static_cast<int16_t>(w), static_cast<int16_t>(list.rowH())};
            gFocusNoted = true;
        }
    }
    if (list.overflows()) {
        const ListView::Thumb t = list.thumb();
        d.fillRect(x + w + 1, t.trackY, kScrollW, t.trackH, kSurface);
        d.fillRect(x + w + 1, t.y, kScrollW, t.h, kTextFaint);
    }
    const int mx = x + (w - kIconMoreUp.w) / 2;
    if (list.moreAbove()) drawIcon(d, kIconMoreUp, mx, list.top() - kIconMoreUp.h - 1, kTextDim);
    if (list.moreBelow()) drawIcon(d, kIconMoreDown, mx, list.rowsBottom() + 1, kTextDim);
}

// ---------------------------------------------------------------- frame

void presentRegion(lgfx::LGFX_Sprite& canvas, lgfx::LovyanGFX& display, int x, int y, int w,
                   int h) {
    display.setClipRect(x, y, w, h);
    canvas.pushSprite(&display, 0, 0);
    display.clearClipRect();
}

}  // namespace widgets
}  // namespace tgc
