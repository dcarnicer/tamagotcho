// The "All caught up" screen; see Done.h.
#include "app/Done.h"

#include "app/App.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Quiz.h"
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

// An icon drawn `s` times larger (each pixel an s x s block).
void drawIconScaled(lgfx::LovyanGFX& d, const tgc::Icon& ic, int x, int y, int s, uint16_t ink) {
    for (int yy = 0; yy < ic.h; ++yy)
        for (int xx = 0; xx < ic.w; ++xx)
            if (tgc::icons::pixel(ic, xx, yy)) d.fillRect(x + xx * s, y + yy * s, s, s, ink);
}

}  // namespace

// The day streak as it stands today (ui/SettingsTree.h).
uint32_t currentStreak() {
    return st::liveStreak(gSrs.streakCurrent(), gSrs.hasStudied(), gSrs.lastStudyDay(),
                          gSrs.today());
}

namespace {

// "All done" screen (docs/ux-refresh.md, done mockup): no card is due and no
// new card is left today. A check in a circle, "All caught up", tomorrow's due
// count, the streak and a "+10 new" button. A tap, hold or side tap studies 10
// more (rebuilds the queue and tries again); the side hold opens the settings.
// A deck whose cards have all been studied has nothing to add: no button, no
// hold hint, and those presses do nothing.
bool doneCanStudyMore() { return deckCounts().fresh > 0; }

}  // namespace

void drawDone() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, gPack->isOpen() ? gPack->deckName() : ui().appName);
    const tgc::layout::Done l = tgc::layout::doneFor(W, H);
    d.fillCircle(l.iconCx, l.iconCy, l.iconR, kOkBg);
    d.drawCircle(l.iconCx, l.iconCy, l.iconR, kOk);
    d.drawCircle(l.iconCx, l.iconCy, l.iconR - 1, kOk);
    constexpr int kScale = 3;
    drawIconScaled(d, tgc::kIconCheck, l.iconCx - tgc::kIconCheck.w * kScale / 2,
                   l.iconCy - tgc::kIconCheck.h * kScale / 2, kScale, kOkHi);
    const tgc::Align align = W > H ? tgc::Align::Left : tgc::Align::Centre;
    gRender.drawLabel(d, ui().doneTitle, l.textX, l.title.y, l.textW, l.title.h, kText, kBg,
                      tgc::Renderer::kTextUi, align);
    char line[48];
    snprintf(line, sizeof line, ui().doneTomorrowFmt, (unsigned)deckCounts().dueTomorrow);
    gRender.drawLabel(d, line, l.textX, l.line1Y, l.textW, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, align);
    const uint32_t streak = currentStreak();
    if (streak > 0) {
        if (streak == 1)
            snprintf(line, sizeof line, "%s", ui().doneStreakOne);
        else
            snprintf(line, sizeof line, ui().doneStreakFmt, (unsigned)streak);
        const int w = tgc::kIconFlame.w + 4 + wg::captionWidth(line);
        const int x = align == tgc::Align::Left ? l.textX : l.textX + (l.textW - w) / 2;
        wg::drawIcon(d, tgc::kIconFlame, x, l.line2Y, kWarn);
        wg::drawCaption(d, gRender, line, x + tgc::kIconFlame.w + 4, l.line2Y, kWarn, kBg);
    }
    wg::Hints h;
    if (doneCanStudyMore()) {
        const tgc::layout::Rect& b = l.button;
        d.fillRect(b.x, b.y, b.w, b.h, kAccent);
        gRender.drawLabel(d, ui().doneMore, b.x, b.y, b.w, b.h, kText, kAccent,
                          tgc::Renderer::kTextUi, tgc::Align::Centre);
        h.hold = ui().hintStudyMore;
    }
    h.side = &tgc::kIconMenu;
    drawHints(d, h);
    present();
}

void showDone() {
    gScreen = Screen::Done;
    drawDone();
}

// loop() on the done screen (handleInput()).
void doneLoop(int a, int b) {
    if (b == 2) {
        openMenu();
    } else if ((a || b == 1) && doneCanStudyMore()) {
        // Nothing due and the daily new cap is spent: study 10 more today.
        gExtraNew = tgc::studyMoreExtra(gSrs.newPerDay(), deckCounts().introducedToday,
                                        extraNewToday());
        gExtraNewDay = gSrs.today();
        gQueue.rebuild(*gPack, gSrs, gRng, gExtraNew);
        showNextItem();  // with learning on, the first of ten intros
    }
}

}  // namespace app
}  // namespace tgc
