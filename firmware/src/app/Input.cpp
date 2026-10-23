// The hold progress and the keyboard; see Input.h.
#include "app/Input.h"

#include <Arduino.h>

#include "app/App.h"
#include "app/Boot.h"
#include "app/Choice.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/GetDecks.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Progress.h"
#include "app/Quiz.h"
#include "app/Remote.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/WifiMenu.h"
#include "board/Board.h"
#include "render/Widgets.h"
#include "ui/QuizUi.h"

namespace tgc {
namespace app {

tgc::KeyRepeat gKeyRepeat;      // the held arrow of a keyboard (keyTurn())

// --- Hold progress (docs/ux-refresh.md §2.5) --------------------------------
// kHoldShowMs after the front button went down on the quiz's focused tile or
// on the focused row of a list screen, the target fills with accent-press from
// the left; it is full when the hold fires (holdMsFor(gHoldPref)), and
// releasing earlier is a tap and the fill goes. Every kHoldFrameMs only the
// target is redrawn and pushed: the quiz draws its tile directly; a list
// screen is redrawn by its own draw function under a clip rect on the row,
// with the widgets' press fill set, so every drawList() screen gets it with no
// code of its own. The first frame also lights the hint bar's hold item (the
// quiz pushes the bar, a list pushes that one frame whole). Grading still uses
// the press time.

namespace {

struct HoldTarget {
    bool quiz;
    wg::Rect r;
};

bool holdTarget(HoldTarget* out) {
    if (gScreen == Screen::Quiz) {
        tgc::Card c;
        if (gRevealed || !questionCard(&c) || gSel < 0 || gSel >= gQ.count) return false;
        out->quiz = true;
        out->r = wg::Rect{static_cast<int16_t>(gQuizG.tx[gSel]), static_cast<int16_t>(gQuizG.ty[gSel]),
                          static_cast<int16_t>(gQuizG.tw), static_cast<int16_t>(gQuizG.th)};
        return true;
    }
    if (!gFrameFocusValid) return false;
    out->quiz = false;
    out->r = gFrameFocus;
    return true;
}

void drawHoldFrame(const HoldTarget& tg, float fill, bool first) {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    auto& d = *gCanvas;
    if (tg.quiz) {
        gHoldShare = fill;
        drawQuizTile(d, gSel, fill);
        presentRegion(tg.r.x, tg.r.y, tg.r.w, tg.r.h);
        if (first && gQuizG.hints) {
            drawQuizHints(d, true);
            presentRegion(0, H - kHintH, W, kHintH);
        }
    } else {
        wg::setPressFill(fill);
        gQuietFrame = true;
        if (first) {
            drawScreen();
        } else {
            gPushOnly = true;
            gPushRect = tg.r;
            d.setClipRect(tg.r.x, tg.r.y, tg.r.w, tg.r.h);
            drawScreen();
            d.clearClipRect();
            gPushOnly = false;  // in case the screen pushed nothing
        }
        gQuietFrame = false;
    }
    QLOG("H hold %s %d%%%s: %u us, push %u us (%d rows)\n", tg.quiz ? "tile" : "row",
         static_cast<int>(fill * 100.0f + 0.5f), first ? " (first)" : "",
         (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

}  // namespace

// Called by loop() with the front button's key and event before the input is
// handled. True when a fill was on screen and its press just ended (a tap, a
// hold, or a press that was dropped): loop() then makes sure a frame without
// the fill follows.
bool holdTick(uint32_t now, bool down, bool longFired, uint32_t downAt, int event,
              uint32_t holdMs) {
    const bool pressing = down && !longFired && event == 0 && !gSwallowInput && !gScreenOff;
    const float fill = pressing ? tgc::holdFill(now - downAt, holdMs) : -1.0f;
    HoldTarget tg{};
    if (fill < 0.0f || !holdTarget(&tg)) {
        if (!gHoldOn) return false;
        gHoldOn = false;
        wg::setPressFill(-1.0f);
        return true;
    }
    if (gHoldOn && now - gHoldFrameAt < tgc::kHoldFrameMs) return false;
    const bool first = !gHoldOn;
    gHoldOn = true;
    gHoldFrameAt = now;
    drawHoldFrame(tg, fill, first);
    return false;
}

// --- Keyboard (board::hasKeyboard(): the Cardputer ADV) -------------------------
// Each key press is one action (ui/KeyInput.h: next, previous, confirm, back,
// menu) on the screen it lands on. keyAction() is the one place that turns an
// action into the screen's own: mostly the button event it stands for, through
// handleInput(), so every screen keeps a single behaviour; the rest is the
// lists' focus up (listFocusUp()) and the menu key's way to the settings home.
// Held arrows repeat (gKeyRepeat) on the lists and the date stepper.

namespace {

// A confirmation that deletes (Delete progress) ignores an Enter this soon
// after it opened: the Enter that opened it, pressed twice, must not also
// confirm it. A hold of the front button takes longer anyway.
constexpr uint32_t kConfirmGuardMs = 500;

// The list screens' focus up (the up and left keys), wrapping as the front tap
// wraps down, with what a move does there (a note goes, a partial push). False
// on a screen that is not a list.
bool listFocusUp() {
    switch (gScreen) {
        case Screen::Menu: {
            const bool note = gMenuNote[0] != 0;
            gMenuNote[0] = 0;
            moveListFocus(menuList(), -1);
            if (note) dropPartialPush();
            break;
        }
        case Screen::Decks:
            gDeckList.setCount(static_cast<int>(gPacks.size()) + 2);  // + "Updates" and "Get decks"
            moveListFocus(gDeckList, -1);
            break;
        case Screen::Choice:
            moveListFocus(gChoiceList, -1);
            break;
        case Screen::DateMenu:
            gDateMenuList.prev();
            gDateMenuNote[0] = 0;
            break;
        case Screen::TzList:
            moveListFocus(gTzListView, -1);
            break;
        case Screen::WifiMenu:
            gWifiMenuList.prev();
            gWifiMenuNote[0] = 0;
            break;
        case Screen::FirstLang:
            gFirstLangList.prev();
            break;
        case Screen::Reset: {
            const bool note = gResetNote[0] != 0;
            gResetNote[0] = 0;
            moveListFocus(gResetList, -1);
            if (note) dropPartialPush();
            break;
        }
        case Screen::GetDecks: {
            if (gNetStep != NetStep::List || gCatalog.empty() || gNetBusy) return false;
            const int n = static_cast<int>(gCatalog.size());
            gNetSel = (gNetSel + n - 1) % n;
            gNetNote[0] = 0;
            break;
        }
        default:
            return false;
    }
    drawScreen();
    return true;
}

// The screens a held arrow repeats on.
bool keyRepeatsHere() {
    switch (gScreen) {
        case Screen::Menu:
        case Screen::Decks:
        case Screen::Choice:
        case Screen::DateMenu:
        case Screen::TzList:
        case Screen::WifiMenu:
        case Screen::FirstLang:
        case Screen::Reset:
            return true;
        case Screen::GetDecks:
            return gNetStep == NetStep::List;
        case Screen::Date:
            return gDateSync != DateSync::Joining;
        default:
            return false;
    }
}

// The screens of the settings tree the menu key leaves for its home: those
// without a radio to stop (the Wi-Fi screens leave with esc) and not the
// boot-time date question.
bool menuKeyGoesHome() {
    switch (gScreen) {
        case Screen::Decks:
        case Screen::Choice:
        case Screen::Stats:
        case Screen::About:
        case Screen::DateMenu:
        case Screen::TzList:
        case Screen::WifiMenu:
        case Screen::Reset:
        case Screen::ResetConfirm:
            return true;
        case Screen::Date:
            return gAfterDate != Screen::Quiz;
        default:
            return false;
    }
}

// One key action on the current screen (see above). The quiz, the detail card
// and the done screen have no "back": there esc opens the settings home, as
// the menu key does (the side hold). The date stepper's esc cancels (its side
// hold); a picker's, a list's or a page's goes back (the side tap).
void keyAction(tgc::KeyAction act, uint32_t now) {
    using A = tgc::KeyAction;
    const bool quizFlow =
        gScreen == Screen::Quiz || gScreen == Screen::Detail || gScreen == Screen::Done;
    const bool stepper = gScreen == Screen::Date && gDateSync != DateSync::Joining;
    switch (act) {
        case A::Next:
            handleInput(1, 0, now, now, false);  // the front tap
            break;
        case A::Confirm:
            if (gScreen == Screen::ResetConfirm && now - gResetConfirmAt < kConfirmGuardMs) break;
            handleInput(2, 0, now, now, false);  // the front hold, at once
            break;
        case A::Prev:
            // The side tap where it is "previous": the quiz's option up, the
            // stepper's day less (and any key gives up the network time).
            if ((gScreen == Screen::Quiz && !gRevealed) || gScreen == Screen::Date)
                handleInput(0, 1, now, now, false);
            else
                listFocusUp();  // a list's focus up; elsewhere nothing
            break;
        case A::Back:
            if (gScreen == Screen::FirstLang) break;  // nothing to go back to
            handleInput(0, quizFlow || stepper ? 2 : 1, now, now, false);
            break;
        case A::Menu:
            if (quizFlow) {
                handleInput(0, 2, now, now, false);  // the settings home
            } else if (gScreen == Screen::Menu) {
                handleInput(0, 1, now, now, false);  // a page: its home; the home: out
            } else if (menuKeyGoesHome()) {
                gMenuPage = st::Page::Home;
                gMenuNote[0] = 0;
                gScreen = Screen::Menu;
                drawScreen();
            }
            break;
        case A::None:
            break;
    }
}

// The next key event: the board's, then (TGC_REMOTE) the serial remote's.
bool nextKey(tgc::KeyEvent* ev, uint32_t now) {
    if (tgc::board::nextKeyEvent(ev)) return true;
#if defined(TGC_REMOTE)
    return remoteNextKey(ev, now);
#else
    (void)now;
    return false;
#endif
}

}  // namespace

// loop()'s keyboard turn, after the buttons'. A press is input for the idle
// timers and wakes the panel as a button does: on a dark panel that press only
// wakes it (dropped, and it does not repeat), on a dim one it also acts.
// Presses read while `swallow` (input dropped after a blocking step, or a
// button woke the panel this turn) are dropped too.
void keyTurn(uint32_t t, bool swallow) {
    tgc::KeyEvent ev;
    while (nextKey(&ev, t)) {
        if (!ev.down) {
            gKeyRepeat.up(ev.key);
            continue;
        }
        gLastInputAt = t;
        if (gScreenOff) {
            wakeScreen();
            continue;
        }
        if (gDimmed) wakeScreen();
        if (swallow) continue;
        gKeyRepeat.down(ev.key, t);
        if (quizGridKey(ev.key, t)) continue;
        keyAction(tgc::keyAction(ev.key), t);
    }
    const tgc::Key r = gKeyRepeat.poll(t);
    if (r != tgc::Key::None && !gScreenOff) {
        gLastInputAt = t;
        if (keyRepeatsHere()) keyAction(tgc::keyAction(r), t);
    }
}
}  // namespace app
}  // namespace tgc
