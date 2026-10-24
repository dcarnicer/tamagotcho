// The parts the detail card and the intro card share; see KanjiCard.h.
#include "app/KanjiCard.h"

#include <Arduino.h>

#include <algorithm>

#include "app/App.h"
#include "app/Display.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "render/Widgets.h"
#include "ui/QuizUi.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

StrokeAnim gAnim;

namespace {

constexpr uint32_t kStrokeMs = 550;       // one stroke step
constexpr uint32_t kStrokePauseMs = 1200; // hold on the last stroke before looping

// The em square of the set-0 kanji in its box (drawKanji() centres it).
int largePx() { return gPack->header().largePx ? gPack->header().largePx : 64; }

}  // namespace

bool startStrokeAnim(uint32_t cardIndex, uint32_t now) {
    uint16_t sc = 0;
    gAnim.on = gPack->strokeInfo(cardIndex, &sc, nullptr) && sc > 0;
    gAnim.cardIndex = cardIndex;
    gAnim.strokes = sc;
    gAnim.frame = 0;
    gAnim.next = now + kStrokeMs;
    return gAnim.on;
}

bool strokeAnimTick(uint32_t t) {
    if (!gAnim.on || gDimmed || gScreenOff || static_cast<int32_t>(t - gAnim.next) < 0)
        return false;
    const int count = static_cast<int>(gAnim.strokes);
    gAnim.frame = (gAnim.frame + 1) % count;
    gAnim.next = t + (gAnim.frame == count - 1 ? kStrokePauseMs : kStrokeMs);
    return true;
}

// The static set-0 glyph, or -- while the pack's stroke data animates --
// every stroke: those written so far in `text`, the current one in `warn`,
// the rest as a `ghost`. The masks live in the em square drawKanji() uses.
void drawKanjiBox(lgfx::LovyanGFX& d, uint32_t cp, int x, int y, int w, int h) {
    d.fillRect(x, y, w, h, kBlack);
    if (!gAnim.on) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, x, y, w, h, kWhite, kBlack);
        return;
    }
    const int px = largePx();
    const int emX = x + (w - px) / 2;
    const int emY = y + (h - px) / 2;
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    const int n = static_cast<int>(gAnim.strokes);
    for (int i = gAnim.frame + 1; i < n; ++i)
        gRender.drawStroke(d, gAnim.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           tgc::theme::kGhost);
    for (int i = 0; i <= gAnim.frame; ++i)
        gRender.drawStroke(d, gAnim.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           i == gAnim.frame ? tgc::theme::kWarn : tgc::theme::kText);
#if defined(TGC_QUIZ_LOG)
    // One line per animation loop (every frame draws every stroke now).
    if (gAnim.frame == n - 1)
        QLOG("R U+%04X strokes %d draw %u us\n", (unsigned)cp, n, (unsigned)(micros() - t0));
#endif
}

void presentKanjiFrame(int x, int y, int w, int h) {
    const int px = largePx();
    const int ex = std::max(x, x + (w - px) / 2 - 1);
    const int ey = std::max(y, y + (h - px) / 2 - 1);
    presentRegion(0, 0, W, kHeaderH);
    presentRegion(ex, ey, std::min(px + 2, w), std::min(px + 2, h));
}

void strokeCounter(char* out, size_t n, int room, unsigned cardStrokes) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (gAnim.on) {
        const bool worded = tgc::strokeCounterWorded(ui().detailStrokeFmt, gAnim.strokes, room);
        snprintf(out, n, worded ? ui().detailStrokeFmt : "%u/%u", (unsigned)(gAnim.frame + 1),
                 (unsigned)gAnim.strokes);
    } else if (cardStrokes) {
        snprintf(out, n, ui().detailStrokesFmt, cardStrokes);
    }
}

int readingTagW() { return std::max(wg::pillWidth(ui().detailOn), wg::pillWidth(ui().detailKun)); }

void drawReadingRow(lgfx::LovyanGFX& d, int x, int y, int w, int lh, int tagW,
                    tgc::ReadingKind kind, const char* first, const char* rest, int more) {
    using namespace tgc::theme;
    constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
    const char* tag = kind == tgc::ReadingKind::Kun ? ui().detailKun : ui().detailOn;
    wg::drawPill(d, gRender, x, y + (lh - kPillH + 1) / 2, tag, kModeReading, kSurfaceHi);
    const int tx = x + tagW + 5;
    const int tw = w - tagW - 5;
    const int drawn = gRender.drawLabel(d, first, tx, y, tw, lh, kText, kBg, kLabel);
    if (rest && *rest && drawn < tw) {
        gRender.drawLabel(d, rest, tx + drawn, y, tw - drawn, lh, kTextDim, kBg, kLabel);
    } else if (more > 0) {
        char cap[12];
        snprintf(cap, sizeof cap, "+%d", more);
        wg::drawCaption(d, gRender, cap, tx + drawn + tgc::kIntroMoreGap, y + (lh - 7 + 1) / 2,
                        kTextDim, kBg);
    }
}

}  // namespace app
}  // namespace tgc
