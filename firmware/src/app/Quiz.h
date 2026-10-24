#pragma once
// The quiz (docs/ux-refresh.md §2.4, §6): the question on screen and its
// answer, the "study more" override, the quiz screen with the geometry and the
// tiles the detail card and the hold progress share, and its input.
#include <cstdint>

#include "app/App.h"
#include "board/Board.h"
#include "pack/PackReader.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "ui/KeyInput.h"
#include "ui/QuizUi.h"

namespace tgc {
namespace app {

// The question on screen, the focused option and whether it was answered.
extern tgc::Question gQ;
extern int gSel;
extern bool gRevealed;
// The quiz's hint bar policy and the auto-advance after a correct answer.
extern tgc::QuizHints gQuizHints;
extern tgc::AutoAdvance gAdvance;
extern uint32_t gShownAt;  // millis() when the question was drawn
extern int gNavTaps;       // navigation taps on the current question
extern uint32_t gLastAnswerAt;
// "Study more" override for the daily new-card cap and the SRS day it was
// granted on (extraNewToday()).
extern uint16_t gExtraNew;
extern uint16_t gExtraNewDay;
// The quiz header's battery figure, re-read every kBatteryCheckMs.
constexpr uint32_t kBatteryCheckMs = 30000;
extern int gShownBattery;
extern uint32_t gBatteryCheckAt;

uint16_t extraNewToday();
// Takes the next queue item and makes it ready, without drawing it: a
// question (Screen::Quiz; a review, a new card with learning off, a relearn
// or a learning step, asked in the item's mode), a never-seen kanji's intro
// card (Screen::Intro, app/Intro.h; no question is built), or Screen::Done
// when the queue is done. The caller sets gScreen (or gBeforeMenu) to it.
Screen nextItem();
// nextItem() on screen: the question or the intro, else the done screen.
void showNextItem();
// After a rebuild: `s` (the screen on top, or gBeforeMenu) gives way to the
// new queue's next item when it holds an item of the old queue still open --
// an intro card, an unanswered question -- or is the done screen. An intro's
// introduced() / known() would be refused by the new queue. `keepQuestion`
// leaves an unanswered question as it is (only an intro is replaced): a
// rebuild for New per day or a date set by hand, as before learning mode.
void refreshAfterRebuild(Screen& s, bool keepQuestion = false);
bool questionCard(tgc::Card* out);
void drawNoQuestion();

// Where the quiz puts its parts (drawQuiz(), the focus moves and hold frames,
// the detail card's kanji box). Meanings are rows in the UI font. Readings are
// a 2x2 grid when every option fits a cell -- in the option kana (set 3), else
// the small kana (set 2) -- otherwise rows, in set 3 when every option fits a
// row. Kanji options are a 2x2 grid of set-1 glyphs.
struct QuizGeom {
    int px = 0, py = 0, pw = 0, ph = 0;  // prompt box
    int tx[tgc::kMaxOptions] = {}, ty[tgc::kMaxOptions] = {};
    int tw = 0, th = 0;  // every tile's size
    uint8_t set = tgc::Renderer::kTextUi;
    bool grid = false;
    bool hints = false;  // the hint bar is shown
};
extern QuizGeom gQuizG;  // as the quiz or the detail card was last drawn
QuizGeom quizGeom(bool hints);
bool quizHintsShown();

// Hold progress on the quiz (holdTick()): a fill frame is on screen, and its
// share, so a full redraw during the hold keeps it.
extern bool gHoldOn;
extern float gHoldShare;
extern uint32_t gHoldFrameAt;
void drawQuizTile(lgfx::LovyanGFX& d, int i, float fill);
void drawQuizHints(lgfx::LovyanGFX& d, bool holding);

void drawQuiz();
bool quizGridKey(tgc::Key k, uint32_t now);
void quizLoop(int a, int b, uint32_t pressedAt, uint32_t now, bool down);

}  // namespace app
}  // namespace tgc
