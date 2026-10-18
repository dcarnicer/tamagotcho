#pragma once
// The flat components of the UX refresh (docs/ux-refresh.md §2.4 and §6.1):
// header, hint bar, option tile, list row, pill, progress bar and a scrolling
// list, drawn into any LovyanGFX target (the full-screen sprite). Colours and
// spacing come from ui/Theme.h, icons from ui/Icons.h and the list state from
// ui/ListView.h. Labels use the UI font and captions the built-in Font0, both
// through the Renderer, so every widget works with no pack loaded. No rounded
// corners. Arduino/ESP32 only.
#include <cstdint>
#include <functional>

#include "board/Board.h"
#include "render/Renderer.h"
#include "ui/Icons.h"
#include "ui/ListView.h"
#include "ui/Theme.h"

namespace tgc {
namespace widgets {

// `icon` with its top-left at (x, y) in `ink`. Returns its width.
int drawIcon(lgfx::LovyanGFX& d, const Icon& icon, int x, int y, uint16_t ink);

// Captions: Font0 at 1x (6 px a character, cap height 7, an 8 px cell drawn
// opaque on `bg`), for status, metadata, values and hints.
int captionWidth(const char* s);
// Draws `s` with the top of its cell at y. Returns the width drawn.
int drawCaption(lgfx::LovyanGFX& d, Renderer& r, const char* s, int x, int y, uint16_t fg,
                uint16_t bg);

// ---------------------------------------------------------------- header
// The 16 px status bar: a title (text-dim caption) or a pill on the left; on
// the right an optional value and the battery. Its bottom is a 2 px progress
// bar (the quiz) or a 1 px hairline.
struct Header {
    const char* title = nullptr;
    const char* pill = nullptr;  // drawn instead of the title when set
    uint16_t pillFg = theme::kTextDim;
    uint16_t pillBg = theme::kSurfaceHi;
    const Icon* pillIcon = nullptr;  // e.g. the check in "✓ GOOD · 4d"
    const char* right = nullptr;     // value left of the battery (cards left, date)
    bool wifi = false;               // the radio-on icon left of the value
    int battery = -1;                // percent; < 0 leaves the icon out
    int progressNum = -1;            // < 0: the hairline instead of the bar
    int progressDen = 0;
};
void drawHeader(lgfx::LovyanGFX& d, Renderer& r, int w, const Header& h);
// 13 x 7: outline, nub and a fill of the charge; text-dim, `warn` under 20 %,
// `err-hi` under 10 %.
void drawBattery(lgfx::LovyanGFX& d, int x, int y, int pct);

// ---------------------------------------------------------------- hint bar
// The 12 px bar at the bottom: the front button's actions on the left as a
// mark and a word (• tap, ▬ hold), the side button's on the right as an icon,
// with its word only when that fits. `holding` lights the hold item while a
// hold is in progress.
struct Hints {
    const char* tap = nullptr;
    const char* hold = nullptr;
    const Icon* side = nullptr;
    const char* sideWord = nullptr;
    bool holding = false;
};
void drawHintBar(lgfx::LovyanGFX& d, Renderer& r, int w, int h, const Hints& hints);

// ---------------------------------------------------------------- tile
// An answer tile. Idle: `surface`. Focus: `accent`. Holding: `accent-press`
// filling from the left by `fill` (0..1), the rest `accent`. Correct: `ok` and
// a check. Wrong: `err-bg`, a 2 px `err` outline and a cross. Dimmed: no fill,
// `text-faint`. The label is left-aligned 8 px in (or centred) and keeps clear
// of the right 20 px, where the check/cross goes.
enum class TileState : uint8_t { Idle, Focus, Holding, Correct, Wrong, Dimmed };
void drawTile(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, TileState state,
              const char* label, float fill = 0.0f, uint8_t set = Renderer::kTextUi,
              Align align = Align::Left);

// ---------------------------------------------------------------- row
// A list row: `surface`, or `accent` when focused. From the left an optional
// radio (the saved / loaded choice) or 9 x 9 icon, the label (UI font); on the
// right an optional value (caption) and a chevron when the row opens another
// screen. With a `caption` it is a two-line row: the caption goes under the
// label, from the row's left padding.
enum class Radio : uint8_t { None, Off, On };
struct Row {
    const char* label = "";
    const char* value = nullptr;
    const char* caption = nullptr;
    const Icon* icon = nullptr;
    Radio radio = Radio::None;
    bool chevron = false;
    bool focused = false;
    bool keepLabel = false;  // cut the value rather than the label
    uint16_t labelColor = theme::kText;  // while not focused
    // The value only when the label and the whole value fit (else none), for
    // values that are a nicety (the settings home's "10/day", "87%").
    bool valueIfFits = false;
    const char* tag = nullptr;  // a caption pill right after the label ("EN")
    // Not focused: no fill, a 1 px `line` outline and faint text (a catalog
    // deck already on the device).
    bool muted = false;
    const Icon* mark = nullptr;  // an icon at the right end, in place of the chevron
    uint16_t markColor = theme::kTextDim;
    // 0..100: a progress bar in place of the caption (a two-line row) or along
    // the row's bottom; < 0 for none.
    int progress = -1;
};
void drawRow(lgfx::LovyanGFX& d, Renderer& r, int x, int y, int w, int h, const Row& row);

// ---------------------------------------------------------------- pill, bar
// An 11 px caption pill (mode, result, ON/KUN, EN/ES). Returns its width.
int pillWidth(const char* text, const Icon* icon = nullptr);
int drawPill(lgfx::LovyanGFX& d, Renderer& r, int x, int y, const char* text, uint16_t fg,
             uint16_t bg, const Icon* icon = nullptr);
// A flat bar: `track`, then `num / den` of it in `fill`.
void drawProgress(lgfx::LovyanGFX& d, int x, int y, int w, int h, int num, int den,
                  uint16_t fill = theme::kAccent, uint16_t track = theme::kSurfaceHi);

// ---------------------------------------------------------------- list
// Draws the visible rows of `list` (its geometry set) in [x, x + w) through
// `row`; when the list overflows, a 2 px scroll bar 1 px right of the rows,
// and a small up / down marker centred in the band above / below the rows
// while rows are hidden there.
using RowDrawer = std::function<void(int index, int x, int y, int w, int h, bool focused)>;
void drawList(lgfx::LovyanGFX& d, const ListView& list, int x, int w, const RowDrawer& row);

// ---------------------------------------------------------------- frame
// Pushes only the rectangle (x, y, w, h) of the full-screen `canvas` to the
// panel: LovyanGFX's pushImage() clips to the destination's clip rect and
// sends just that window.
void presentRegion(lgfx::LGFX_Sprite& canvas, lgfx::LovyanGFX& display, int x, int y, int w,
                   int h);

}  // namespace widgets
}  // namespace tgc
