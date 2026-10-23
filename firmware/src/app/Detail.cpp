// The detail card; see Detail.h.
#include "app/Detail.h"

#include <Arduino.h>

#include <algorithm>

#include "app/App.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "render/Widgets.h"
#include "ui/Icons.h"
#include "ui/QuizUi.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace {

// Stroke-order animation on the mistake detail card. Only active while the
// shown card's pack carries stroke data (docs/pack-format.md); otherwise the
// card is drawn exactly as before.
bool gAnimOn = false;
uint16_t gAnimStrokes = 0;
int gAnimFrame = 0;
uint32_t gAnimNext = 0;
constexpr uint32_t kStrokeMs = 550;       // one stroke step
constexpr uint32_t kStrokePauseMs = 1200; // hold on the last stroke before looping

// The detail card's kanji box (gQuizG's prompt box, where the quiz showed the
// kanji): the static set-0 glyph, or -- while the pack's stroke data animates
// -- every stroke: those written so far in `text`, the current one in `warn`,
// the rest as a `ghost`, so the final shape shows throughout. The masks live
// in the em square drawKanji() uses.
void drawDetailKanji(lgfx::LovyanGFX& d, uint32_t cp) {
    const QuizGeom& g = gQuizG;
    d.fillRect(g.px, g.py, g.pw, g.ph, kBlack);
    if (!gAnimOn) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, g.px, g.py, g.pw, g.ph, kWhite, kBlack);
        return;
    }
    const int px = gPack->header().largePx ? gPack->header().largePx : 64;
    const int emX = g.px + (g.pw - px) / 2;
    const int emY = g.py + (g.ph - px) / 2;
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    const int n = static_cast<int>(gAnimStrokes);
    for (int i = gAnimFrame + 1; i < n; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY, tgc::theme::kGhost);
    for (int i = 0; i <= gAnimFrame; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           i == gAnimFrame ? tgc::theme::kWarn : tgc::theme::kText);
#if defined(TGC_QUIZ_LOG)
    // One line per animation loop (every frame draws every stroke now).
    if (gAnimFrame == n - 1)
        QLOG("R U+%04X strokes %d draw %u us\n", (unsigned)cp, n, (unsigned)(micros() - t0));
#endif
}

// The detail card's header: "DETAIL", then the stroke counter while the
// strokes animate ("stroke 4/6", or "4/13" when the worded counter of the last
// stroke would not fit; decided per card, ui/QuizUi.h) or the stroke count.
void drawDetailHeader(lgfx::LovyanGFX& d, const tgc::Card& card) {
    wg::Header h;
    h.title = ui().detailTitle;
    h.battery = tgc::board::batteryPercent();
    char counter[32] = "";
    if (gAnimOn) {
        const int room = tgc::headerPillRoom(W, 0, h.battery >= 0) -
                         wg::captionWidth(h.title) - 6;
        const bool worded = tgc::strokeCounterWorded(ui().detailStrokeFmt, gAnimStrokes, room);
        snprintf(counter, sizeof counter, worded ? ui().detailStrokeFmt : "%u/%u",
                 (unsigned)(gAnimFrame + 1), (unsigned)gAnimStrokes);
    } else if (card.strokes) {
        snprintf(counter, sizeof counter, ui().detailStrokesFmt, (unsigned)card.strokes);
    }
    if (counter[0]) h.right = counter;
    wg::drawHeader(d, gRender, W, h);
}

}  // namespace

// Detail card (roadmap phase 3, restyled in docs/ux-refresh.md §4 phase 2): a
// tap after a wrong reveal opens it. Under the kanji: what was picked (a cross
// and the option; for meaning -> kanji the picked kanji's meaning) and the
// card's own accuracy, the meanings (UI font, what room is left, at most three
// lines), and the readings, one line each behind an ON / KUN pill (the UI
// font draws kana from the pack's text set). Landscape mirrors the quiz:
// kanji left, details right. When the pack carries stroke data for the card,
// the kanji animates (drawDetailFrame(), the Screen::Detail branch of loop()).
void drawDetail() {
    // The card keeps the pack's katakana, the pick included: Settings > Study >
    // On'yomi changes the answers only. Read before questionCard(): a streamed
    // pack's string cache would not keep the card's strings past these reads.
    tgc::Question asPack = gQ;
    if (gOnKana != tgc::OnKana::Katakana)
        tgc::restyleReadings(*gPack, tgc::OnKana::Katakana, &asPack);
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBg);
    gQuizG = quizGeom(quizHintsShown());
    const QuizGeom& g = gQuizG;
    drawDetailHeader(d, card);
    drawDetailKanji(d, card.codepoint);

    constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
    const int lh = gRender.lineHeight(kLabel);
    const bool landscape = W > H;
    const int x = landscape ? g.pw + kGutter : kGutter;
    const int w = W - x - kGutter;
    int y = landscape ? kHeaderH + 3 : g.py + g.ph + 2;
    const int bottom = (g.hints ? H - kHintH : H) - 2;

    // What was picked, and "correct 3/5" (the bare "3/5" when the pick
    // needs the room).
    const tgc::Record* rec = gSrs.find(card.codepoint);
    char accFull[32] = "", accBare[16] = "";
    if (rec && rec->reps > 0) {
        const unsigned total = rec->reps;
        const unsigned correct = rec->reps > rec->lapses ? rec->reps - rec->lapses : 0;
        snprintf(accFull, sizeof accFull, ui().detailAccuracyFmt, correct, total);
        snprintf(accBare, sizeof accBare, "%u/%u", correct, total);
    }
    const char* picked = gSel >= 0 && gSel < asPack.count ? asPack.options[gSel].c_str() : "";
    const int markW = tgc::kIconCross.w + 4;
    const int pickedW = gRender.textWidth(picked, kLabel);
    const char* acc = nullptr;
    if (accFull[0])
        acc = markW + pickedW + 6 + wg::captionWidth(accFull) <= w ? accFull : accBare;
    const int accW = acc ? wg::captionWidth(acc) : 0;
    wg::drawIcon(d, tgc::kIconCross, x, y + (lh - tgc::kIconCross.h + 1) / 2, kErrHi);
    gRender.drawLabel(d, picked, x + markW, y, w - markW - (acc ? accW + 6 : 0), lh, kErrHi, kBg,
                      kLabel);
    if (acc) wg::drawCaption(d, gRender, acc, x + w - accW, y + (lh - 7 + 1) / 2, kTextDim, kBg);
    y += lh + 2;

    // Only the kinds the card has get a row (ui/QuizUi.h).
    tgc::DetailReading readings[2];
    const int readingRows = tgc::detailReadings(card, readings);
    const int lines = std::max(1, std::min(3, (bottom - y - readingRows * (lh + 2)) / lh));
    y += gRender.drawWrapped(d, card.meanings, x, y, w, lines, kText, kBg, kLabel) * lh + 2;

    // Both readings start at the same x, after the wider tag.
    const int tagW = std::max(wg::pillWidth(ui().detailOn), wg::pillWidth(ui().detailKun));
    for (int i = 0; i < readingRows && y + lh <= bottom + 2; ++i) {
        const char* tag = readings[i].kind == tgc::ReadingKind::Kun ? ui().detailKun : ui().detailOn;
        wg::drawPill(d, gRender, x, y + (lh - kPillH + 1) / 2, tag, kModeReading, kSurfaceHi);
        gRender.drawLabel(d, readings[i].text.c_str(), x + tagW + 5, y, w - tagW - 5, lh, kText, kBg,
                          kLabel);
        y += lh + 2;
    }

    if (g.hints) {
        wg::Hints hints;
        hints.tap = ui().hintNext;
        hints.side = &tgc::kIconMenu;
        drawHints(d, hints, tgc::kKeyEnter);
    }
    present();
}

namespace {

// One stroke-animation step: only the header (its counter) and the kanji's
// em square are redrawn and pushed.
void drawDetailFrame() {
    tgc::Card card;
    if (!questionCard(&card)) return;
    auto& d = *gCanvas;
    drawDetailHeader(d, card);
    drawDetailKanji(d, card.codepoint);
    const QuizGeom& g = gQuizG;
    const int px = gPack->header().largePx ? gPack->header().largePx : 64;
    const int ex = std::max(g.px, g.px + (g.pw - px) / 2 - 1);
    const int ey = std::max(g.py, g.py + (g.ph - px) / 2 - 1);
    presentRegion(0, 0, W, kHeaderH);
    presentRegion(ex, ey, std::min(px + 2, g.pw), std::min(px + 2, g.ph));
}

}  // namespace

void showDetail() {
    tgc::Card c{};  // stays zero when there is no card (drawDetail() then says so)
    questionCard(&c);
    // Start the stroke animation when the pack has stroke data for this card.
    uint16_t sc = 0;
    gAnimOn = gPack->strokeInfo(gQ.cardIndex, &sc, nullptr) && sc > 0;
    gAnimStrokes = sc;
    gAnimFrame = 0;
    gAnimNext = millis() + kStrokeMs;
    if (gAnimOn)
        QLOG("D U+%04X strokes=%u animating\n", (unsigned)c.codepoint, (unsigned)sc);
    else
        QLOG("D U+%04X\n", (unsigned)c.codepoint);
    gScreen = Screen::Detail;
    drawDetail();
}

// loop() on the detail card (handleInput()).
void detailLoop(int a, int b, uint32_t t) {
    // Animate the stroke order while idle: one stroke every ~550 ms, a
    // ~1.2 s hold on the last one, then loop; it stops while the panel is
    // dim or dark (§2.6). Only the counter and the kanji are redrawn.
    if (gAnimOn && !gDimmed && !gScreenOff && static_cast<int32_t>(t - gAnimNext) >= 0) {
        const int count = static_cast<int>(gAnimStrokes);
        gAnimFrame = (gAnimFrame + 1) % count;
        gAnimNext = t + (gAnimFrame == count - 1 ? kStrokePauseMs : kStrokeMs);
        drawDetailFrame();
    }
    // Any input (tap, hold or side tap) starts the next question; a side hold
    // opens the menu, as on the quiz.
    if (a == 1 || a == 2 || b == 1) {
        if (newQuestion()) {
            gScreen = Screen::Quiz;
            drawScreen();
        } else {
            showDone();
        }
    } else if (b == 2) {
        openMenu();
    }
}

}  // namespace app
}  // namespace tgc
