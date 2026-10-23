// Statistics; see Stats.h.
#include "app/Stats.h"

#include <algorithm>
#include <cstring>

#include "app/App.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "render/Widgets.h"
#include "ui/DeckSummary.h"
#include "ui/Icons.h"
#include "ui/ScreenLayout.h"
#include "ui/SettingsTree.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace {

// "2h 5m", "5m 30s" or "30s" depending on the magnitude; `firstUnit` keeps
// only the largest unit ("2h", "5m"). The unit strings come from UiStrings so
// a language can change them.
void formatReviewTime(uint32_t ms, char* out, size_t n, bool firstUnit = false) {
    const uint32_t totalS = ms / 1000u;
    const uint32_t mins = totalS / 60u;
    const uint32_t hours = mins / 60u;
    if (hours > 0)
        snprintf(out, n, ui().statsTimeHM, (unsigned)hours, (unsigned)(mins % 60u));
    else if (mins > 0)
        snprintf(out, n, ui().statsTimeMS, (unsigned)mins, (unsigned)(totalS % 60u));
    else
        snprintf(out, n, ui().statsTimeS, (unsigned)totalS);
    if (firstUnit) {
        char* space = std::strchr(out, ' ');
        if (space) *space = 0;
    }
}

// One statistics tile: the value in the label face (after an optional icon)
// over its caption.
void drawStatTile(lgfx::LovyanGFX& d, const tgc::layout::Rect& t, const char* value,
                  const char* caption, const tgc::Icon* icon = nullptr) {
    using namespace tgc::theme;
    // 4 px in from the sides: a 62 px tile keeps 54 px, nine caption
    // characters ("dominadas").
    constexpr int kPad = 4;
    d.fillRect(t.x, t.y, t.w, t.h, kSurface);
    int x = t.x + kPad;
    // The value (an 18 px box) and the caption under it, centred as a pair.
    const int valueY = t.y + std::max(3, (t.h - 29) / 2);
    if (icon) {
        wg::drawIcon(d, *icon, x, valueY + (18 - icon->h) / 2, kWarn);
        x += icon->w + 3;
    }
    gRender.drawLabel(d, value, x, valueY, t.right() - kPad - x, 18, kText, kSurface);
    gRender.drawLabel(d, caption, t.x + kPad, valueY + 19, t.w - 2 * kPad, 8, kTextDim, kSurface,
                      tgc::Renderer::kTextBuiltinSmall);
}

}  // namespace

// Statistics (docs/ux-refresh.md, stats mockup; opened from the settings
// home): the accuracy over every answer as a ring, six tiles -- the day streak
// (a flame) and the best one, the loaded deck's due and new cards, the learned
// cards and the time on questions -- and how much of the deck has been seen.
// Read-only; the side button goes back.
void drawStats() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().statsTitle);
    const tgc::layout::Stats l = tgc::layout::statsFor(W, H);
    const tgc::Stats stats = gSrs.stats();
    const tgc::DeckSummary s = deckCounts();
    const int acc = st::accuracyPercent(stats.answers, stats.again);

    // The ring: a track, the accuracy clockwise from 12 o'clock, the number in
    // the hole ("87" in the numeral face, "%" in the label face, on one
    // baseline), else the label face alone when that does not fit ("100%").
    d.fillArc(l.cx, l.cy, l.rOuter, l.rInner, 0.0f, 360.0f, kSurfaceHi);
    if (acc > 0)
        d.fillArc(l.cx, l.cy, l.rOuter, l.rInner, tgc::layout::kRingStartDeg,
                  tgc::layout::ringEndDeg(acc), kAccentHi);
    char digits[8];
    snprintf(digits, sizeof digits, acc >= 0 ? "%d" : "--", acc);
    const int dw = gRender.textWidth(digits, tgc::Renderer::kTextUiNumeral);
    const int pw = acc >= 0 ? gRender.textWidth("%", tgc::Renderer::kTextUi) : 0;
    if (tgc::layout::ringFits(dw + pw, l.rInner)) {
        const int x = l.cx - (dw + pw) / 2;
        gRender.drawLabel(d, digits, x, l.cy - 12, dw, 24, kText, kBg,
                          tgc::Renderer::kTextUiNumeral);
        if (pw) gRender.drawLabel(d, "%", x + dw, l.cy - 2, pw, 14, kTextDim, kBg);
    } else {
        char label[8];
        snprintf(label, sizeof label, "%d%%", acc);
        gRender.drawLabel(d, label, l.cx - l.rInner, l.cy - 7, 2 * l.rInner, 14, kText, kBg,
                          tgc::Renderer::kTextUi, tgc::Align::Centre);
    }
    const int colW = l.seenStacked ? 2 * l.cx : W;
    gRender.drawLabel(d, ui().statsAccuracy, 0, l.captionY, colW, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);

    // The tiles, in pairs.
    char v[6][16];
    snprintf(v[0], sizeof v[0], "%u", (unsigned)currentStreak());
    snprintf(v[1], sizeof v[1], "%u", (unsigned)stats.streakBest);
    snprintf(v[2], sizeof v[2], "%u", (unsigned)s.due);
    snprintf(v[3], sizeof v[3], "%u", (unsigned)s.fresh);
    snprintf(v[4], sizeof v[4], "%u", (unsigned)stats.learned);
    formatReviewTime(stats.reviewMs, v[5], sizeof v[5]);
    if (gRender.textWidth(v[5], tgc::Renderer::kTextUi) > l.tiles[5].w - 8)
        formatReviewTime(stats.reviewMs, v[5], sizeof v[5], true);
    for (int i = 0; i < 6; ++i)
        drawStatTile(d, l.tiles[i], v[i], ui().statsTiles[i], i == 0 ? &tgc::kIconFlame : nullptr);

    // Seen / total of the loaded deck: one line across (portrait), or the
    // caption over the bar in the left column (landscape).
    char nums[24];
    snprintf(nums, sizeof nums, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
    const tgc::layout::Rect& sn = l.seen;
    if (!l.seenStacked) {
        const int ww = wg::captionWidth(ui().statsSeen);
        const int nw = wg::captionWidth(nums);
        wg::drawCaption(d, gRender, ui().statsSeen, sn.x, sn.y, kTextDim, kBg);
        wg::drawCaption(d, gRender, nums, sn.right() - nw, sn.y, kTextDim, kBg);
        wg::drawProgress(d, sn.x + ww + 6, sn.y + 2, sn.w - ww - nw - 12, 3,
                         static_cast<int>(s.seen()), static_cast<int>(s.total));
    } else {
        char line[40];
        snprintf(line, sizeof line, ui().summarySeenFmt, (unsigned)s.seen(), (unsigned)s.total);
        if (wg::captionWidth(line) > sn.w) snprintf(line, sizeof line, "%s", nums);
        gRender.drawLabel(d, line, sn.x, sn.y, sn.w, 8, kTextDim, kBg,
                          tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
        wg::drawProgress(d, sn.x, sn.y + 10, sn.w, 3, static_cast<int>(s.seen()),
                         static_cast<int>(s.total));
    }
    backHints(d);
    present();
}

void openStats() {
    gScreen = Screen::Stats;
    drawScreen();
}

// loop() on the statistics (handleInput()).
void statsLoop(int b) {
    if (b) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

}  // namespace app
}  // namespace tgc
