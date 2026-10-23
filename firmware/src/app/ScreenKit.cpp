// The parts the screens are drawn from; see ScreenKit.h.
#include "app/ScreenKit.h"

#include <algorithm>
#include <string>
#include <vector>

#include "app/App.h"
#include "app/UiStrings.h"
#include "ui/Icons.h"
#include "ui/TextLayout.h"

namespace tgc {
namespace app {

namespace {

void smallText(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t fg, uint16_t bg,
               lgfx::textdatum_t datum = lgfx::top_left) {
    d.setFont(&lgfx::fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(fg, bg);
    d.setTextDatum(datum);
    d.drawString(s, x, y);
}

}  // namespace

// The hint bar in this board's terms. On a board with a keyboard the words
// stay and the marks become keys (ui/HintBar.h keycaps, docs/decisions.md
// 2026-10-21): `tapKey` for the front tap (kKeyMove where it moves a focus,
// kKeyRight where it steps or turns a page, kKeyEnter where any key goes on),
// Enter for the hold, esc for the side's back or cancel, the menu key and the
// word "settings" for the side's settings icon, and on the date stepper (the
// only bar with a side hold) left for the side tap and esc for the side hold.
// The buttons' boards get `h` as it is.
tgc::widgets::Hints boardHints(tgc::widgets::Hints h, const tgc::KeyCap& tapKey) {
    if (!tgc::board::hasKeyboard()) return h;
    h.keys = true;
    h.tapKey = tapKey;
    if (h.sideHold) {
        h.sideKey = tgc::kKeyLeft;
    } else if (h.side == &tgc::kIconMenu) {
        h.sideKey = tgc::kKeyMenu;
        if (!h.sideWord) h.sideWord = ui().hintSettings;
    } else {
        h.sideKey = tgc::kKeyEsc;
    }
    return h;
}

void drawHints(lgfx::LovyanGFX& d, const tgc::widgets::Hints& h,
               const tgc::KeyCap& tapKey) {
    tgc::widgets::drawHintBar(d, gRender, W, H, boardHints(h, tapKey));
}

tgc::theme::Layout layout() { return tgc::theme::layoutFor(W, H); }

// The menus' header: the title, an optional value (the date, a page number)
// and the battery; the radio icon while the Wi-Fi is on.
void menuHeader(lgfx::LovyanGFX& d, const char* title, const char* right,
                bool wifi) {
    wg::Header h;
    h.title = title;
    h.right = right;
    h.wifi = wifi;
    h.battery = tgc::board::batteryPercent();
    wg::drawHeader(d, gRender, W, h);
}

// Tap moves the focus, a hold does `holdWord`, the side button goes back.
void menuHints(lgfx::LovyanGFX& d, const char* holdWord) {
    wg::Hints h;
    h.tap = ui().hintMove;
    h.hold = holdWord;
    h.side = &tgc::kIconBack;
    h.sideWord = ui().hintBack;
    drawHints(d, h);
}

// The hint bar of a screen whose side button only goes back. Its tap (About's
// next page) is the right arrow on a keyboard.
void backHints(lgfx::LovyanGFX& d, const char* tap, const char* hold) {
    wg::Hints h;
    h.tap = tap;
    h.hold = hold;
    h.side = &tgc::kIconBack;
    h.sideWord = ui().hintBack;
    drawHints(d, h, tgc::kKeyRight);
}

// Lines of `text` wrapped in the caption font to `w` px, drawn from y one
// every 10 px, centred or from x; at most `maxLines`. Returns the lines drawn.
int drawCaptionLines(lgfx::LovyanGFX& d, const char* text, int x, int y, int w, int maxLines,
                     uint16_t color, tgc::Align align) {
    if (!text || !*text) return 0;
    const std::vector<tgc::TextSpan> lines = tgc::wrapText(text, w / 6);
    int n = 0;
    for (const tgc::TextSpan& l : lines) {
        if (n >= maxLines) break;
        const std::string s(text + l.begin, l.len);
        gRender.drawLabel(d, s.c_str(), x, y + 10 * n, w, 8, color, kBlack,
                          tgc::Renderer::kTextBuiltinSmall, align);
        ++n;
    }
    return n;
}

void showError(const char* title, const char* action, const wg::Hints* hints) {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().appName);
    // The title (two label lines at most) and the action under it, centred
    // in the space between the header and the hint bar.
    const int lines = static_cast<int>(tgc::wrapText(action ? action : "", (W - 8) / 6).size());
    const int blockH = 44 + 10 * std::min(lines, 6);
    const int top = kHeaderH + std::max(4, (H - kHeaderH - kHintH - blockH) / 2);
    gRender.drawCentred(d, title, 4, top, W - 8, 40, 2, tgc::theme::kWarn, kBlack,
                        tgc::Renderer::kTextUi);
    drawCaptionLines(d, action, 4, top + 44, W - 8, 6, kWhite);
    if (hints) wg::drawHintBar(d, gRender, W, H, *hints);
    present();
}

// Fits `list` (rows of `rowH`) between y `top` and `bottom`, keeping a band
// above and below for the scroll markers. Called on every draw: the window
// follows the orientation.
void layoutList(tgc::ListView& list, int top, int bottom, int rowH) {
    using tgc::theme::kMarkerBand;
    list.setGeometry(top + kMarkerBand, bottom - top - 2 * kMarkerBand, rowH, tgc::theme::kGap);
}

void menuNote(lgfx::LovyanGFX& d, const char* text, uint16_t color) {
    if (text && *text)
        gRender.drawLabel(d, text, kGutter, H - kHintH - kNoteH + 1, W - 2 * kGutter, 7, color,
                          kBlack, tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
}

// Full-screen notice (sleep, battery, first start): `title` in the label face
// (two lines at most), `hint` under it in captions. The UI fonts need no pack.
void showNotice(const char* title, const char* hint, uint16_t titleColor) {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int mid = H / 2;
    gRender.drawCentred(d, title, 4, mid - 44, W - 8, 40, 2, titleColor, kBlack,
                        tgc::Renderer::kTextUi);
    drawCaptionLines(d, hint, 4, mid + 2, W - 8, 4, tgc::theme::kTextDim);
    present();
}

}  // namespace app
}  // namespace tgc
