// The quiz screen and its answers; see Quiz.h.
#include "app/Quiz.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>

#include "app/App.h"
#include "app/Detail.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/Intro.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "render/Widgets.h"
#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

tgc::Question gQ;
int gSel = 0;
bool gRevealed = false;
// The quiz's hint bar policy and the auto-advance after a correct answer
// (ui/QuizUi.h; docs/ux-refresh.md §6.3, §6.5).
tgc::QuizHints gQuizHints;
tgc::AutoAdvance gAdvance;
uint32_t gShownAt = 0;     // millis() when the question was drawn
int gNavTaps = 0;          // navigation taps on the current question
uint32_t gLastAnswerAt = 0;
// "Study more" override for the daily new-card cap (the done screen's tap), and
// the SRS day it was granted on: it only applies on that day (extraNewToday()).
uint16_t gExtraNew = 0;
uint16_t gExtraNewDay = 0;

int gShownBattery = -2;
uint32_t gBatteryCheckAt = 0;

namespace {

bool gLastCorrect = false;  // outcome of the last answer (drives the detail card)
tgc::ReadingTurn gReadingTurn;  // on'yomi and kun'yomi questions take turns
// The last answer's grade and the days to the card's next review (record
// dueDay - today), for the result pill.
tgc::Grade gLastGrade = tgc::Grade::Good;
int gLastInterval = 0;
uint32_t gAdvanceFrameAt = 0;  // millis() of the last countdown frame

// Response time credited back per navigation tap: with one button, reaching
// the 4th option costs three taps that are UI cost, not recall time.
constexpr uint32_t kNavAllowanceMs = 400;

const char* gradeName(tgc::Grade g) {
    switch (g) {
        case tgc::Grade::Again: return "again";
        case tgc::Grade::Hard: return "hard";
        case tgc::Grade::Good: return "good";
    }
    return "?";
}

}  // namespace

// The "study more" override in force. It belongs to the day it was granted, so
// any change of the day index (uptime rollover, sleep time, a manual date, a
// real-time sync) drops it without each of those paths having to.
uint16_t extraNewToday() {
    if (gExtraNewDay != gSrs.today()) gExtraNew = 0;
    return gExtraNew;
}

// Pops the next queue item (see Quiz.h). An intro card is handed to
// app/Intro with no question built, and leaves the on/kun turn alone. A
// question tries the item's mode first -- a learning step's is fixed: kanji
// -> meaning, then a reading -- and then the other two (a card may lack
// readings or meanings); a card asked in no mode is skipped.
Screen nextItem() {
    static const tgc::QuizMode kModes[] = {tgc::QuizMode::Meaning, tgc::QuizMode::Reading,
                                           tgc::QuizMode::MeaningToKanji};
    constexpr int kN = 3;
    for (;;) {
        tgc::QueueItem item;
        if (!gQueue.next(&item)) return Screen::Done;
        if (item.kind == tgc::ItemKind::Intro) {
            if (prepareIntro(item.cardIndex)) return Screen::Intro;
            continue;  // unreadable card: the next item
        }
        // Ask for on'yomi and kun'yomi in turn, so both kinds show up.
        const tgc::ReadingKind want = gReadingTurn.want();
        for (int k = 0; k < kN; ++k) {
            const tgc::QuizMode mode = kModes[(static_cast<int>(item.mode) + k) % kN];
            if (!tgc::buildQuestion(*gPack, item.cardIndex, mode, gRng, &gQ, want, gOnKana))
                continue;
            if (gQ.mode == tgc::QuizMode::Reading) gReadingTurn.asked(gQ.readingKind);
            gSel = 0;
            gRevealed = false;
            gShownAt = millis();
            gNavTaps = 0;
            gQuizHints.question(gShownAt);
            gAdvance.cancel();
            tgc::Card c;
            gPack->card(gQ.cardIndex, &c);
            const char* mk = gQ.mode == tgc::QuizMode::Reading
                                 ? (gQ.readingKind == tgc::ReadingKind::Kun ? "read-kun" : "read-on")
                                 : (gQ.mode == tgc::QuizMode::MeaningToKanji ? "k2m" : "mean");
            QLOG("Q %s mode=%d U+%04X", mk, static_cast<int>(gQ.mode), (unsigned)c.codepoint);
            if (item.kind == tgc::ItemKind::Step) QLOG(" step=%u", (unsigned)item.step);
            for (int i = 0; i < gQ.count; ++i)
                QLOG(" |%s %s", i == gQ.correct ? "*" : " ", gQ.options[i].c_str());
            QLOG("\n");
            return Screen::Quiz;
        }
        // Card cannot be asked in any mode: skip it and try the next item.
    }
}

void showNextItem() {
    const Screen s = nextItem();
    if (s == Screen::Done) {
        showDone();
        return;
    }
    gScreen = s;
    drawScreen();
}

void refreshAfterRebuild(Screen& s, bool keepQuestion) {
    const bool open = s == Screen::Intro ||
                      (!keepQuestion && ((s == Screen::Quiz && !gRevealed) || s == Screen::Done));
    if (open) s = nextItem();
}

// The card of the question on screen. False when there is none to draw or
// grade: no pack open, no question built yet, or an index the open pack does
// not have. (A question left over from a pack the portal or Get decks replaced
// is replaced itself, in portalPacksChanged().)
bool questionCard(tgc::Card* out) {
    return gPack->isOpen() && gQ.count > 0 && gPack->card(gQ.cardIndex, out);
}

// Stand-in for the quiz or detail screen when questionCard() fails (a deck that
// could not be loaded): says so and how to get out, since a side hold opens
// the settings home on both screens.
void drawNoQuestion() {
    tgc::widgets::Hints h;
    h.side = &tgc::kIconMenu;
    h.sideWord = ui().hintSettings;
    const tgc::widgets::Hints keys = boardHints(h);
    showError(ui().errOpenPack, ui().errOpenPackMenu, &keys);
}

namespace {

// The quiz prompt's set-0 kanji. With TGC_QUIZ_LOG it also traces the draw
// cost: the whole call and the bitmap fetch inside it (file read plus the v2
// decode; 0 when the renderer still held this glyph from the last redraw).
void drawPromptKanji(lgfx::LovyanGFX& d, uint32_t cp, int x, int y, int w, int h) {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, x, y, w, h, kWhite, kBlack);
#if defined(TGC_QUIZ_LOG)
    const uint32_t us = micros() - t0;
    const tgc::Glyph* g = gPack->findGlyph(tgc::kGlyphSetLarge, cp);
    QLOG("R U+%04X kanji %ux%u enc=%u draw %u us (fetch %u us)\n", (unsigned)cp,
         g ? (unsigned)g->width : 0u, g ? (unsigned)g->height : 0u, g ? (unsigned)g->encoding : 0u,
         (unsigned)us, (unsigned)gRender.lastFetchUs());
#endif
}

}  // namespace

// --- Quiz screen (docs/ux-refresh.md §2.4, §6) ------------------------------
// Header: the mode pill (the result pill once answered), the cards left, the
// battery and the 2 px session bar. Prompt: the kanji, or for meaning -> kanji
// a caption and the meaning. Options: flat tiles (render/Widgets). The hint bar
// follows ui/QuizUi.h's policy (QuizHints); without it the tiles take its space.

QuizGeom gQuizG;  // as the quiz or the detail card was last drawn

QuizGeom quizGeom(bool hints) {
    using namespace tgc::theme;
    const Layout l = layoutFor(W, H);
    QuizGeom g;
    g.hints = hints;
    g.px = l.promptX;
    g.py = l.promptY;
    g.pw = l.promptW;
    g.ph = l.promptH;
    int ax = l.answersX, ay = l.answersY, aw = l.answersW, ah = l.answersH;
    const int extra = hints ? 0 : kHintH;
    if (!l.landscape) {
        // A larger em (--large-px) gets a taller kanji box, up to 45 % of H.
        const int largePx = gPack->header().largePx ? gPack->header().largePx : 64;
        const int want = std::min(largePx + 6, H * 45 / 100);
        if (want > g.ph) {
            ay += want - g.ph;
            ah -= want - g.ph;
            g.ph = want;
        }
        ah += extra;
    } else {
        g.ph += extra;
        ah += extra ? extra - 3 : 0;  // 4 px from the bottom edge, as on the right
    }
    const int n = gQ.count > 0 ? std::min(gQ.count, tgc::kMaxOptions) : 1;
    // The grid keeps 2 px to the panel edges (rows keep the 4 px gutter): a
    // portrait cell then holds three option kana.
    const int gx = ax - 2, gw = aw + 4;
    const int rows = (n + 1) / 2;
    const int cw = (gw - kGap) / 2;
    const int ch = (ah - (rows - 1) * kGap) / rows;
    auto allFit = [&](uint8_t set, int w) {
        for (int i = 0; i < n; ++i)
            if (gRender.textWidth(gQ.options[i].c_str(), set) > w) return false;
        return true;
    };
    if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
        g.grid = true;
        g.set = tgc::kGlyphSetSmall;
    } else if (gQ.mode == tgc::QuizMode::Reading) {
        g.set = tgc::kGlyphSetOption;
        g.grid = allFit(g.set, cw - 4);
        if (!g.grid && allFit(tgc::kGlyphSetText, cw - 4)) {
            g.set = tgc::kGlyphSetText;
            g.grid = true;
        }
        if (!g.grid && !allFit(g.set, aw - kTilePadX - 6)) g.set = tgc::kGlyphSetText;
    }
    if (g.grid) {
        g.tw = cw;
        g.th = ch;
        for (int i = 0; i < n; ++i) {
            g.tx[i] = gx + (i % 2) * (cw + kGap);
            g.ty[i] = ay + (i / 2) * (ch + kGap);
        }
    } else {
        g.tw = aw;
        g.th = (ah - (n - 1) * kGap) / n;
        for (int i = 0; i < n; ++i) {
            g.tx[i] = ax;
            g.ty[i] = ay + i * (g.th + kGap);
        }
    }
    return g;
}

bool quizHintsShown() { return gQuizHints.shown(gSrs.answeredTotal()); }

// Hold progress on the quiz (see holdTick()): a fill frame is on screen, and
// its share, so a full redraw during the hold keeps it.
bool gHoldOn = false;
float gHoldShare = 0.0f;
uint32_t gHoldFrameAt = 0;  // millis() of the last fill frame

namespace {

wg::TileState quizTileState(int i) {
    if (gRevealed) {
        if (i == gQ.correct) return wg::TileState::Correct;
        return i == gSel ? wg::TileState::Wrong : wg::TileState::Dimmed;
    }
    return i == gSel ? wg::TileState::Focus : wg::TileState::Idle;
}

}  // namespace

// Option tile `i` where gQuizG puts it; `fill` >= 0 draws the focused tile
// holding (hold progress).
void drawQuizTile(lgfx::LovyanGFX& d, int i, float fill) {
    const QuizGeom& g = gQuizG;
    wg::TileState st = quizTileState(i);
    if (fill >= 0.0f && st == wg::TileState::Focus) st = wg::TileState::Holding;
    if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
        // The code point only (a streamed pack would read the card's text too,
        // on every hold-fill frame).
        const uint32_t cp =
            gQ.optionCard[i] < gPack->cardCount() ? gPack->codepointAt(gQ.optionCard[i]) : '?';
        wg::drawKanjiTile(d, gRender, g.tx[i], g.ty[i], g.tw, g.th, st, cp, g.set, fill);
        return;
    }
    wg::drawTile(d, gRender, g.tx[i], g.ty[i], g.tw, g.th, st, gQ.options[i].c_str(), fill, g.set,
                 g.grid ? tgc::Align::Centre : tgc::Align::Left);
}

namespace {

// The header: before the answer the mode pill in its colour -- MEANING, the
// reading asked for (ON / KUN) or KANJI -- and after it the result: a check
// and "GOOD · 4d" / "HARD · 2d", or a cross and "AGAIN", in the longest form
// that fits next to the cards left (ui/QuizUi.h). Right: the cards left and
// the battery; bottom: answered / (answered + left).
void drawQuizHeader(lgfx::LovyanGFX& d) {
    using namespace tgc::theme;
    const tgc::SessionProgress p =
        tgc::sessionProgress(static_cast<uint32_t>(gQueue.answeredCount()),
                             static_cast<uint32_t>(gQueue.remaining()), !gRevealed);
    char left[12];
    snprintf(left, sizeof left, "%u", (unsigned)p.left);
    const int bat = tgc::board::batteryPercent();
    gShownBattery = bat;
    wg::Header h;
#if defined(TGC_BATT_TEST)
    // The discharge test needs the figure, not just the icon: show it where
    // the quiz otherwise says how many cards are left.
    char batText[8];
    snprintf(batText, sizeof batText, "%d%%", bat);
    h.right = batText;
#else
    h.right = left;
#endif
    h.battery = bat;
    h.progressNum = static_cast<int>(p.done);
    h.progressDen = static_cast<int>(p.total);
    h.pillBg = kSurfaceHi;
    char pill[40];
    if (!gRevealed) {
        if (gQ.mode == tgc::QuizMode::Reading) {
            h.pill = gQ.readingKind == tgc::ReadingKind::Kun  ? ui().detailKun
                     : gQ.readingKind == tgc::ReadingKind::On ? ui().detailOn
                                                              : ui().quizReading;
            h.pillFg = kModeReading;
        } else if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
            h.pill = ui().quizKanji;
            h.pillFg = kModeKanji;
        } else {
            h.pill = ui().quizMeaning;
            h.pillFg = kAccentHi;
        }
    } else {
        const char* word = ui().pillGood;
        const tgc::Icon* icon = &tgc::kIconCheck;
        int days = gLastInterval > 0 ? gLastInterval : 0;
        if (gLastGrade == tgc::Grade::Again) {
            word = ui().pillAgain;
            icon = &tgc::kIconCross;
            days = -1;
            h.pillFg = kErrHi;
            h.pillBg = kErrBg;
        } else if (gLastGrade == tgc::Grade::Hard) {
            word = ui().pillHard;
            h.pillFg = kWarn;
        } else {
            h.pillFg = kOkHi;
            h.pillBg = kOkBg;
        }
        tgc::resultPillText(pill, sizeof pill, word, days, ui().pillDaysUnit, icon->w,
                            tgc::headerPillRoom(W, tgc::captionChars(h.right), bat >= 0));
        h.pill = pill;
        h.pillIcon = icon;
    }
    wg::drawHeader(d, gRender, W, h);
}

// The prompt: the kanji, or (meaning -> kanji) a caption saying what is asked
// and the meaning under it (UI font, up to three lines), centred as a block.
void drawQuizPrompt(lgfx::LovyanGFX& d, const tgc::Card& card) {
    const QuizGeom& g = gQuizG;
    if (gQ.mode != tgc::QuizMode::MeaningToKanji) {
        drawPromptKanji(d, card.codepoint, g.px, g.py, g.pw, g.ph);
        return;
    }
    constexpr uint8_t kCap = tgc::Renderer::kTextBuiltinSmall;
    constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
    const char* cap = ui().quizK2mCaption;
    const char* prompt = gQ.options[gQ.correct].c_str();
    const int bx = g.px + kGutter;
    const int bw = g.pw - 2 * kGutter;
    const int lh = gRender.lineHeight(kLabel);
    const int capLines = std::max(1, gRender.lineCount(cap, bw, 2, kCap));
    const int capH = capLines * gRender.lineHeight(kCap);
    constexpr int kGapY = 6;
    const int maxLines = std::max(1, std::min(3, (g.ph - capH - kGapY - 4) / lh));
    const int lines = std::max(1, gRender.lineCount(prompt, bw, maxLines, kLabel));
    const int top = g.py + (g.ph - (capH + kGapY + lines * lh)) / 2;
    gRender.drawCentred(d, cap, bx, top, bw, capH, capLines, tgc::theme::kTextDim, kBlack, kCap);
    gRender.drawCentred(d, prompt, bx, top + capH + kGapY, bw, lines * lh, lines, kWhite, kBlack,
                        kLabel);
}

}  // namespace

// The quiz's hint bar: "• move ▬ answer" on a question, "• next" after a
// correct answer, "• details" after a wrong one; the side button's icon is
// the settings one (side hold). `holding` lights the hold item. Keys: up/down
// move, Enter answers (and goes on: any key does), Tab the settings.
void drawQuizHints(lgfx::LovyanGFX& d, bool holding) {
    wg::Hints h;
    if (!gRevealed) {
        h.tap = ui().hintMove;
        h.hold = ui().hintAnswer;
        h.holding = holding;
    } else {
        h.tap = gLastCorrect ? ui().hintNext : ui().hintDetails;
    }
    h.side = &tgc::kIconMenu;
    drawHints(d, h, gRevealed ? tgc::kKeyEnter : tgc::kKeyMove);
}

namespace {

// The auto-advance countdown: a 2 px bar along the bottom edge that drains
// (below the tiles, or in the hint bar's empty bottom rows).
constexpr int kAdvanceBarH = 2;
void drawAdvanceBar(lgfx::LovyanGFX& d, uint32_t now) {
    const int y = H - kAdvanceBarH;
    d.fillRect(0, y, W, kAdvanceBarH, kBlack);
    const int w = static_cast<int>(std::lround(W * gAdvance.left(now)));
    if (w > 0) d.fillRect(0, y, w, kAdvanceBarH, tgc::theme::kOkHi);
}

}  // namespace

void drawQuiz() {
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    gQuizG = quizGeom(quizHintsShown());
    drawQuizHeader(d);
    drawQuizPrompt(d, card);
    const bool holding = gHoldOn && !gRevealed;
    for (int i = 0; i < gQ.count; ++i)
        drawQuizTile(d, i, holding && i == gSel ? gHoldShare : -1.0f);
    if (gQuizG.hints) drawQuizHints(d, holding);
    if (gRevealed && gAdvance.active()) drawAdvanceBar(d, millis());
    present();
}

namespace {

// A focus move (front tap: down, side tap: up, wrapping): only the two tiles
// change, so only they are redrawn and pushed. The hint bar is redrawn into
// the buffer too (unlit): loop() pushes it when a hold fill ended with this tap.
// `to(sel, count)` picks the new focus (the arrow keys on the 2x2 grid move by
// row and column, see quizGridKey()).
template <typename To>
void moveQuizFocusTo(To to) {
    tgc::Card card;
    if (!questionCard(&card)) return;  // also keeps gQ.count from being 0 below
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    const int old = gSel;
    gSel = to(gSel, gQ.count);
    ++gNavTaps;
    auto& d = *gCanvas;
    drawQuizTile(d, old, -1.0f);
    drawQuizTile(d, gSel, -1.0f);
    if (gQuizG.hints) drawQuizHints(d, false);
    const QuizGeom& g = gQuizG;
    const int x0 = std::min(g.tx[old], g.tx[gSel]);
    const int y0 = std::min(g.ty[old], g.ty[gSel]);
    const int x1 = std::max(g.tx[old], g.tx[gSel]) + g.tw;
    const int y1 = std::max(g.ty[old], g.ty[gSel]) + g.th;
    presentRegion(x0, y0, x1 - x0, y1 - y0);
    QLOG("F quiz focus %d -> %d: %u us, push %u us (%d rows)\n", old, gSel,
         (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

void moveQuizFocus(bool down) {
    moveQuizFocusTo([down](int sel, int count) {
        return down ? tgc::nextOption(sel, count) : tgc::prevOption(sel, count);
    });
}

}  // namespace

// An arrow key on the quiz's 2x2 answer grid moves by row (up, down) and by
// column (left, right), as the tiles look; keys elsewhere, and the arrows on
// the one-column layout, go through keyAction() (previous / next). True when
// the key was used here.
bool quizGridKey(tgc::Key k, uint32_t now) {
    if (gScreen != Screen::Quiz || gRevealed || !gQuizG.grid) return false;
    const int dx = k == tgc::Key::Left ? -1 : k == tgc::Key::Right ? 1 : 0;
    const int dy = k == tgc::Key::Up ? -1 : k == tgc::Key::Down ? 1 : 0;
    if (!dx && !dy) return false;
    gQuizHints.input(now);
    moveQuizFocusTo([dx, dy](int sel, int count) { return tgc::gridOption(sel, count, 2, dx, dy); });
    return true;
}

// The quiz's input (docs/ux-refresh.md §6.7). On a question: front tap = the
// next option (down), side tap = the previous one (up), both wrapping; front
// hold = answer; side hold = settings. A tap acts on release and a hold fires
// at holdMsFor(gHoldPref), so a long press is never also a tap. Revealed: any
// press but the side hold goes on (after a wrong answer to the detail card
// first); after a correct one the auto-advance countdown may do it instead.

namespace {

// The input after the answer has been revealed: a correct answer advances as
// before; a wrong one opens the detail card first, and the next input advances.
void afterReveal() {
    gAdvance.cancel();
    if (!gLastCorrect) {
        showDetail();
        return;
    }
    showNextItem();
}

void holdAction(uint32_t pressedAt) {
    if (gRevealed) {
        afterReveal();
        return;
    }
    // No card, no grade: a record must never be written for a card that is not
    // the one on screen (see questionCard()).
    tgc::Card card;
    if (!questionCard(&card)) return;
    gRevealed = true;

    const bool correct = gSel == gQ.correct;
    gLastCorrect = correct;
    // From the question being drawn to the hold's press-down (not its fire),
    // minus a fixed allowance per navigation tap. Thinking time before and
    // between taps counts, the mechanical cost of reaching the option does not.
    const uint32_t raw = pressedAt - gShownAt;
    const uint32_t nav = static_cast<uint32_t>(gNavTaps) * kNavAllowanceMs;
    const uint32_t rt_ms = raw > nav ? raw - nav : 0;
    tgc::Grade grade;
    if (!correct)
        grade = tgc::Grade::Again;
    else if (rt_ms >= gSrs.slowMs())
        grade = tgc::Grade::Hard;
    else
        grade = tgc::Grade::Good;

    const float u01 = gRng.below(1000) / 1000.0f;
    gSrs.grade(card.codepoint, grade, gSrs.today(), static_cast<uint8_t>(gQ.mode), u01);
    gSrs.addReviewMs(rt_ms);
    gQueue.answered(gQ.cardIndex, grade);
    ++gDirty;
    gLastAnswerAt = millis();

    const tgc::Record* r = gSrs.find(card.codepoint);
    const int interval = r ? static_cast<int>(r->dueDay) - static_cast<int>(gSrs.today()) : 0;
    gLastGrade = grade;
    gLastInterval = interval;
    QLOG("A sel=%d correct=%d %s grade=%s rt=%ums S=%.2f D=%.1f I=%d\n", gSel, gQ.correct,
         correct ? "OK" : "MISS", gradeName(grade), (unsigned)rt_ms,
         r ? r->stabilityCds / 100.0f : 0.0f, r ? r->difficulty10 / 10.0f : 0.0f, interval);
    // A correct answer moves on by itself after the auto-advance time; a
    // wrong one never does (the detail card follows a press).
    if (correct)
        gAdvance.start(gLastAnswerAt, tgc::advanceMsFor(gAdvancePref));
    else
        gAdvance.cancel();
    gAdvanceFrameAt = gLastAnswerAt;
    drawScreen();
}

}  // namespace

// loop() on the quiz screen (see above). `down`: a button is down now.
void quizLoop(int a, int b, uint32_t pressedAt, uint32_t now, bool down) {
    if (!gRevealed) {
        if (down || a || b) gQuizHints.input(now);
        if (a == 1)
            moveQuizFocus(true);
        else if (a == 2)
            holdAction(pressedAt);
        else if (b == 1)
            moveQuizFocus(false);
        else if (b == 2)
            openMenu();
        else if (!gScreenOff && gQuizHints.tick(now, gSrs.answeredTotal(), true))
            drawScreen();  // the idle hint bar appears: the tiles make room
        return;
    }
    if (a || b == 1) {
        afterReveal();
    } else if (b == 2) {
        openMenu();
    } else if (gAdvance.active()) {
        if (gAdvance.due(now, down)) {
            afterReveal();
        } else if (!gScreenOff && now - gAdvanceFrameAt >= tgc::kHoldFrameMs) {
            gAdvanceFrameAt = now;
            drawAdvanceBar(*gCanvas, now);
            presentRegion(0, H - kAdvanceBarH, W, kAdvanceBarH);
        }
    }
}

}  // namespace app
}  // namespace tgc
