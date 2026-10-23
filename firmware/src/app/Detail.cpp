// The detail card; see Detail.h.
#include "app/Detail.h"

#include <Arduino.h>

#include <algorithm>

#include "app/App.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/KanjiCard.h"
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

// The detail card's header: "DETAIL", then the stroke counter while the
// strokes animate ("stroke 4/6", or "4/13" when the worded counter of the last
// stroke would not fit; decided per card, ui/QuizUi.h) or the stroke count.
void drawDetailHeader(lgfx::LovyanGFX& d, const tgc::Card& card) {
    wg::Header h;
    h.title = ui().detailTitle;
    h.battery = tgc::board::batteryPercent();
    const int room =
        tgc::headerPillRoom(W, 0, h.battery >= 0) - wg::captionWidth(h.title) - 6;
    char counter[32];
    strokeCounter(counter, sizeof counter, room, card.strokes);
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
// the kanji animates (app/KanjiCard, the Screen::Detail branch of loop()).
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
    drawKanjiBox(d, card.codepoint, g.px, g.py, g.pw, g.ph);

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
    const int tagW = readingTagW();
    for (int i = 0; i < readingRows && y + lh <= bottom + 2; ++i) {
        drawReadingRow(d, x, y, w, lh, tagW, readings[i].kind, readings[i].text.c_str());
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
    const QuizGeom& g = gQuizG;
    drawDetailHeader(d, card);
    drawKanjiBox(d, card.codepoint, g.px, g.py, g.pw, g.ph);
    presentKanjiFrame(g.px, g.py, g.pw, g.ph);
}

}  // namespace

void showDetail() {
    tgc::Card c{};  // stays zero when there is no card (drawDetail() then says so)
    questionCard(&c);
    // Animate the stroke order when the pack has stroke data for this card.
    if (startStrokeAnim(gQ.cardIndex, millis()))
        QLOG("D U+%04X strokes=%u animating\n", (unsigned)c.codepoint, (unsigned)gAnim.strokes);
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
    if (strokeAnimTick(t)) drawDetailFrame();
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
