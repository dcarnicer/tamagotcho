// Tamagotcho (たまごっちょ): the Arduino entry points. setup() runs the boot
// (app/Boot.cpp); loop() turns the buttons into tap and hold events and hands
// them, and the keyboard's keys, to the screen on top (handleInput()), which
// drawScreen() draws. Every screen lives in its own file under app/
// (docs/architecture.md).
#include <Arduino.h>

#include "app/About.h"
#include "app/App.h"
#include "app/Boot.h"
#include "app/Choice.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Detail.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/GetDecks.h"
#include "app/Input.h"
#include "app/Intro.h"
#include "app/Power.h"
#include "app/Progress.h"
#include "app/Quiz.h"
#include "app/Remote.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/Stats.h"
#include "app/Wifi.h"
#include "app/WifiMenu.h"
#include "board/Board.h"
#include "ui/InputPrefs.h"

using tgc::board::Button;

namespace tgc {
namespace app {

void drawScreen() {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    if (gScreen == Screen::Menu) drawMenu();
    else if (gScreen == Screen::Decks) drawDecks();
    else if (gScreen == Screen::Choice) drawChoice();
    else if (gScreen == Screen::Done) drawDone();
    else if (gScreen == Screen::Date) drawDate();
    else if (gScreen == Screen::DateMenu) drawDateMenu();
    else if (gScreen == Screen::TzList) drawTzList();
    else if (gScreen == Screen::Stats) drawStats();
    else if (gScreen == Screen::Detail) drawDetail();
    else if (gScreen == Screen::Intro) drawIntro();
    else if (gScreen == Screen::Wifi) drawWifi();
    else if (gScreen == Screen::GetDecks) drawGetDecks();
    else if (gScreen == Screen::UpdateCheck) drawUpdateCheck();
    else if (gScreen == Screen::WifiMenu) drawWifiMenu();
    else if (gScreen == Screen::NetTest) drawNetTest();
    else if (gScreen == Screen::About) drawAbout();
    else if (gScreen == Screen::FirstLang) drawFirstLang();
    else if (gScreen == Screen::Reset) drawReset();
    else if (gScreen == Screen::ResetConfirm) drawResetConfirm();
    else drawQuiz();
    // Frame cost (docs/ux-refresh.md §4, phase 1): the whole redraw and, in
    // it, the push to the panel (rows: H, or fewer for a partial push). A hold
    // frame logs its own line (drawHoldFrame()).
    if (!gQuietFrame)
        QLOG("F screen %d: %u us, push %u us (%d rows)\n", static_cast<int>(gScreen),
             (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

// One turn of the current screen's own loop with this turn's events: `a`
// the front button's, `b` the side button's (1 tap, 2 hold, 0 none),
// `pressedAt` when the front press went down (the quiz grades from it), `t`
// now and `down` whether a button is down. loop() calls it once a turn with
// the buttons' events; a key calls it once more with the events it stands
// for (keyAction()).
void handleInput(int a, int b, uint32_t pressedAt, uint32_t t, bool down) {
    if (gScreen == Screen::Menu) {
        menuLoop(a, b);
    } else if (gScreen == Screen::Decks) {
        decksLoop(a, b);
    } else if (gScreen == Screen::Choice) {
        choiceLoop(a, b);
    } else if (gScreen == Screen::Date) {
        dateLoop(a, b);
    } else if (gScreen == Screen::DateMenu) {
        dateMenuLoop(a, b);
    } else if (gScreen == Screen::TzList) {
        tzListLoop(a, b);
    } else if (gScreen == Screen::Stats) {
        statsLoop(b);
    } else if (gScreen == Screen::About) {
        aboutLoop(a, b);
    } else if (gScreen == Screen::Detail) {
        detailLoop(a, b, t);
    } else if (gScreen == Screen::Intro) {
        introLoop(a, b, t, down);
    } else if (gScreen == Screen::Done) {
        doneLoop(a, b);
    } else if (gScreen == Screen::Wifi) {
        // The page may leave this screen (Get decks), switch to home mode or
        // download; the boot-time upload screen moves on to the quiz as soon as
        // a pack arrived in setup mode (see wifiLoop()).
        wifiLoop(a, b);
    } else if (gScreen == Screen::UpdateCheck) {
        updateCheckInput(a, b);
    } else if (gScreen == Screen::GetDecks) {
        getDecksLoop(a, b);
    } else if (gScreen == Screen::WifiMenu) {
        wifiMenuLoop(a, b);
    } else if (gScreen == Screen::NetTest) {
        netTestLoop(a, b);
    } else if (gScreen == Screen::FirstLang) {
        firstLangLoop(a);
    } else if (gScreen == Screen::Reset) {
        resetLoop(a, b);
    } else if (gScreen == Screen::ResetConfirm) {
        resetConfirmLoop(a, b);
    } else {
        quizLoop(a, b, pressedAt, t, down);
    }
}

}  // namespace app
}  // namespace tgc

using namespace tgc::app;

namespace {

// Tap/hold detection: 1 = tap (on release), 2 = hold (at the threshold).
struct Key {
    bool down = false;
    bool longFired = false;
    uint32_t downAt = 0;
    int poll(bool isDown, uint32_t now, uint32_t holdMs) {
        if (isDown && !down) {
            down = true;
            longFired = false;
            downAt = now;
        }
        if (isDown && !longFired && now - downAt >= holdMs) {
            longFired = true;
            return 2;
        }
        if (!isDown && down) {
            down = false;
            if (!longFired) return 1;
        }
        return 0;
    }
};

}  // namespace

void setup() {
    boot();
}

void loop() {
    tgc::board::update();
    const uint32_t t = millis();
    gSrs.tick(t);
    // Input dropped after a blocking step covers the keys read this turn too
    // (the buttons below clear the flag once they are up).
    const bool swallowKeys = gSwallowInput;
    static Key keyA, keyB;
#if defined(TGC_REMOTE)
    remotePoll(t);
    const bool downA = tgc::board::isHeld(Button::Select) || remoteDown(0, t);
    const bool downB = tgc::board::isHeld(Button::Next) || remoteDown(1, t);
#else
    const bool downA = tgc::board::isHeld(Button::Select);
    const bool downB = tgc::board::isHeld(Button::Next);
#endif
    const uint32_t holdMs = tgc::holdMsFor(gHoldPref);
    int a = keyA.poll(downA, t, holdMs);
    int b = keyB.poll(downB, t, holdMs);
#if defined(TGC_REMOTE)
    remoteKeyEvent(0, a, t);
    remoteKeyEvent(1, b, t);
#endif
    if (downA || downB || a || b) {
        gLastInputAt = t;
        // A dark panel only wakes up: that press (through its release) is
        // dropped. A dimmed panel brightens and the press acts as usual.
        if (gScreenOff) gSwallowInput = true;
        if (gScreenOff || gDimmed) wakeScreen();
    }
    if (gSwallowInput) {
        a = b = 0;
        if (!downA && !downB) gSwallowInput = false;
    }
    // The power button (StickS3 M5PM1, click reset disabled) means "sleep now";
    // on a dark panel (idle on USB) it only wakes the panel, like the others.
    if (tgc::board::powerButtonClicked()) {
        gLastInputAt = t;
        if (!gScreenOff) sleepNow(false);
        wakeScreen();
    }
    // Hold progress first, so the fill never outlives its press; then the
    // input. A fill that ended needs a frame without it: the handler's own
    // (plus the hint bar, whose hold item was lit, after a partial push), or
    // a redraw when the press did nothing.
    const bool fillEnded = holdTick(t, keyA.down, keyA.longFired, keyA.downAt, a, holdMs);
    const uint32_t framesBefore = gFrames;
    handleInput(a, b, keyA.downAt, t, downA || downB);
    if (fillEnded) {
        if (gFrames == framesBefore)
            drawScreen();
        else if (gLastPushPartial)
            presentRegion(0, H - kHintH, W, kHintH);
    }
    keyTurn(t, swallowKeys || gSwallowInput);
    if (gDirty >= 8 || (gDirty > 0 && t - gLastAnswerAt > 120000)) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    if ((gScreen == Screen::Quiz || gScreen == Screen::Intro) && !gScreenOff &&
        t - gBatteryCheckAt >= kBatteryCheckMs) {
        gBatteryCheckAt = t;
        if (tgc::board::batteryPercent() != gShownBattery) drawScreen();
    }
    orientTick(t, downA || downB);
#if defined(TGC_BATT_TEST)
    if (t - gBattLogAt >= 60000) {
        gBattLogAt = t;
        battLog("tick");
        if (Serial) battLogPrint();
    }
#endif
    // Save and sleep before a nearly flat cell browns out and takes the RTC
    // timer (the SRS day clock) with it.
    if (batteryCritical(t)) sleepNow(true);
    updateCheckTick(t);
    powerTick(t);
    delay(10);
}
