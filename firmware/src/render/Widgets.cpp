#include "render/Widgets.h"

#include <algorithm>
#include <cmath>

#include "pack/Utf8.h"

namespace tgc {
namespace widgets {

namespace {
using namespace theme;

constexpr uint8_t kCaption = Renderer::kTextBuiltinSmall;
constexpr int kCaptionCap = 7;  // Font0: cap height, the 8th row is the descender

// Top of an icon / caption vertically centred in a band of height `h` at y.
inline int centreY(int y, int h, int inkH) { return y + (h - inkH + 1) / 2; }
}  // namespace

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
        left += drawCaption(d, r, h.title, kGutter, 4, kTextDim, kBg);
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

void drawHintBar(lgfx::LovyanGFX& d, Renderer& r, int w, int h, const Hints& hints) {
    const int y = h - kHintH;
    d.fillRect(0, y, w, kHintH, kBg);
    int x = kGutter;
    auto item = [&](const Icon& mark, const char* word, bool lit) {
        drawIcon(d, mark, x, y + 2 + (7 - mark.h) / 2, lit ? kAccentHi : kText);
        x += mark.w + 3;
        x += drawCaption(d, r, word, x, y + 2, lit ? kText : kTextDim, kBg) + 6;
    };
    if (hints.tap) item(kIconTap, hints.tap, false);
    if (hints.hold) item(kIconHold, hints.hold, hints.holding);
    if (!hints.side) return;
    const Icon& ic = *hints.side;
    const int iy = y + 2 + (7 - ic.h) / 2;
    const int wordW = hints.sideWord ? captionWidth(hints.sideWord) : 0;
    // The word only when the two sides stay as far apart as the front items
    // (6 px); else the icon alone.
    const int withWord = w - kGutter - wordW - ic.w - 3;
    if (wordW && withWord >= x) {
        drawIcon(d, ic, withWord, iy, kText);
        drawCaption(d, r, hints.sideWord, withWord + ic.w + 3, y + 2, kTextDim, kBg);
    } else {
        drawIcon(d, ic, w - kGutter - ic.w, iy, kText);
    }
}

// ---------------------------------------------------------------- tile

void drawTile(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, TileState state,
              const char* label, float fill, uint8_t set, Align align) {
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
    const int lx = x + kTilePadX;
    const int lw = w - kTilePadX - kTileMarkW;
    auto drawText = [&](uint16_t on, int cx, int cw) {
        d.setClipRect(cx, y, cw, h);
        r.drawLabel(d, label, lx, y, lw, h, fg, on, set, align);
        d.clearClipRect();
    };
    if (state == TileState::Holding) {
        // The press fills from the left; the label is drawn once per part,
        // clipped to it, so its anti-aliasing blends against each colour.
        if (fill < 0.0f) fill = 0.0f;
        if (fill > 1.0f) fill = 1.0f;
        const int split = static_cast<int>(std::lround(w * fill));
        if (split > 0) d.fillRect(x, y, split, h, kAccentPress);
        if (label && *label) {
            if (split > 0) drawText(kAccentPress, x, split);
            if (split < w) drawText(bg, x + split, w - split);
        }
    } else if (label && *label) {
        drawText(bg, x, w);
    }
    if (state == TileState::Correct)
        drawIcon(d, kIconCheck, x + w - kIconCheck.w - 6, centreY(y, h, kIconCheck.h), kText);
    else if (state == TileState::Wrong)
        drawIcon(d, kIconCross, x + w - kIconCross.w - 7, centreY(y, h, kIconCross.h), kErrHi);
}

// ---------------------------------------------------------------- row

void drawRow(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, const Row& row) {
    const bool f = row.focused;
    const bool muted = row.muted && !f;
    const uint16_t bg = f ? kAccent : (muted ? kBg : kSurface);
    d.fillRect(x, y, w, h, bg);
    if (muted) d.drawRect(x, y, w, h, kLine);
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
        drawIcon(d, *row.icon, lx, centreY(y, lineH, row.icon->h), f ? kText : kTextDim);
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
    d.setClipRect(x, y, w, h);
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
    d.clearClipRect();
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
    for (int i = first; i < first + n; ++i)
        row(i, x, list.rowY(i), w, list.rowH(), i == list.focus());
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
