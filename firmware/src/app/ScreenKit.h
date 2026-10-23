#pragma once
// The parts the screens are drawn from (docs/ux-refresh.md §2): the header,
// the hint bar in this board's terms, caption lines, a list's geometry and its
// note line, and the error and notice screens.
#include <cstdint>

#include "app/Display.h"
#include "render/Renderer.h"
#include "render/Widgets.h"
#include "ui/HintBar.h"
#include "ui/ListView.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace wg = tgc::widgets;
using tgc::theme::kGutter;
using tgc::theme::kHeaderH;
using tgc::theme::kHintH;

// Error screen (defined with the menu helpers): what happened, the one thing
// to do, and the hint bar when a button does something here.
void showError(const char* title, const char* action, const tgc::widgets::Hints* hints = nullptr);

// The hint bar in this board's terms (keycaps on a board with a keyboard).
tgc::widgets::Hints boardHints(tgc::widgets::Hints h, const tgc::KeyCap& tapKey = tgc::kKeyMove);
void drawHints(lgfx::LovyanGFX& d, const tgc::widgets::Hints& h,
               const tgc::KeyCap& tapKey = tgc::kKeyMove);

// --- Menu screens (docs/ux-refresh.md §2.4) ---------------------------------
// The list screens are built from render/Widgets: a header (title, battery,
// hairline), rows of the layout's height that scroll instead of shrinking
// (ui/ListView, a 2 px scroll bar and small markers when rows are hidden), and
// the hint bar. Labels use the UI font, so they need no pack.

tgc::theme::Layout layout();  // the grid of the panel's current W x H

void menuHeader(lgfx::LovyanGFX& d, const char* title, const char* right = nullptr,
                bool wifi = false);
void menuHints(lgfx::LovyanGFX& d, const char* holdWord);
void backHints(lgfx::LovyanGFX& d, const char* tap = nullptr, const char* hold = nullptr);
int drawCaptionLines(lgfx::LovyanGFX& d, const char* text, int x, int y, int w, int maxLines,
                     uint16_t color, tgc::Align align = tgc::Align::Centre);

void layoutList(tgc::ListView& list, int top, int bottom, int rowH);
inline int listX() { return kGutter; }
inline int listW() { return W - 2 * kGutter; }  // the scroll bar goes in the right gutter

// Lists whose rows carry a second text (deck counts, zone offsets) put it on
// a caption line under the label on narrow panels, and right of the label as
// the row's value where a row is wide enough for both (landscape).
inline bool wideRows() { return W >= 200; }

// The menus' one-line note above the hint bar (what the last action did).
constexpr int kNoteH = 12;
void menuNote(lgfx::LovyanGFX& d, const char* text, uint16_t color);

void showNotice(const char* title, const char* hint, uint16_t titleColor);

}  // namespace app
}  // namespace tgc
