// The pickers; see Choice.h.
#include "app/Choice.h"

#include <Arduino.h>

#include <string>
#include <vector>

#include "app/App.h"
#include "app/Display.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "app/WifiMenu.h"
#include "board/Board.h"
#include "platform/HomeWifi.h"
#include "render/Widgets.h"
#include "ui/InputPrefs.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/UiLang.h"

namespace tgc {
namespace app {

// Generic choice screen: a title and the candidate values. A radio marks the
// saved value and the accent fill the focus. Hold applies the focused value;
// the side button cancels back to where the picker came from.
tgc::ListView gChoiceList;

namespace {

Choice gChoice = Choice::NewPerDay;
int gChoiceCount = 0;
uint16_t gChoiceValues[4] = {0, 0, 0, 0};
int gChoiceSaved = -1;  // row of the saved value, -1 when none (Forget Wi-Fi)

}  // namespace

// The Hard threshold as the Study row and its picker show it: "3 s", "off".
void hardAfterText(uint16_t ms, char* out, size_t n) {
    if (ms == 0xFFFF)
        snprintf(out, n, "%s", ui().hardAfterOff);
    else
        st::formatSeconds(ms, ui().decimalPoint, out, n);
}

namespace {

void choiceLabel(int i, char* out, size_t n) {
    const uint16_t v = gChoiceValues[i];
    if (gChoice == Choice::Orientation) {
        snprintf(out, n, "%s", ui().orientRows[v % 3]);
    } else if (gChoice == Choice::ForgetWifi) {
        snprintf(out, n, "%s", ui().forgetRows[i % 2]);
    } else if (gChoice == Choice::UiLang) {
        snprintf(out, n, "%s", ui().langRows[i]);
    } else if (gChoice == Choice::Brightness) {
        snprintf(out, n, "%u%%", (unsigned)(v * 100u / 255u));
    } else if (gChoice == Choice::SlowMs) {
        hardAfterText(v, out, n);
    } else if (gChoice == Choice::AutoAdvance) {
        snprintf(out, n, "%s", ui().advanceRows[v % tgc::kAdvanceChoices]);
    } else if (gChoice == Choice::HoldTime) {
        snprintf(out, n, "%s", ui().holdRows[v % tgc::kHoldChoices]);
    } else if (gChoice == Choice::OnKana) {
        snprintf(out, n, "%s", ui().onKanaRows[v % 2]);
    } else {
        snprintf(out, n, "%u", (unsigned)v);
    }
}

// A picker row's value: the duration a timing choice stands for, else
// "default" on the default choice; "" for none.
void choiceValue(int i, char* out, size_t n) {
    const uint16_t v = gChoiceValues[i];
    out[0] = 0;
    if (gChoice == Choice::AutoAdvance) {
        if (v != tgc::kAdvanceOff)
            st::formatSeconds(tgc::advanceMsFor(v), ui().decimalPoint, out, n);
    } else if (gChoice == Choice::HoldTime) {
        st::formatSeconds(tgc::holdMsFor(v), ui().decimalPoint, out, n);
    } else if ((gChoice == Choice::NewPerDay && v == tgc::kSrsDefaultNewPerDay) ||
               (gChoice == Choice::SlowMs && v == tgc::kSrsDefaultSlowMs) ||
               (gChoice == Choice::Brightness && v == kBrightnessDefault)) {
        snprintf(out, n, "%s", ui().choiceDefault);
    }
}

const char* choiceTitle() {
    switch (gChoice) {
        case Choice::NewPerDay: return ui().newPerDayTitle;
        case Choice::SlowMs: return ui().hardAfterTitle;
        case Choice::Orientation: return ui().orientTitle;
        case Choice::Brightness: return ui().brightTitle;
        case Choice::ForgetWifi: return ui().forgetTitle;
        case Choice::AutoAdvance: return ui().advanceTitle;
        case Choice::HoldTime: return ui().holdTitle;
        case Choice::OnKana: return ui().onKanaTitle;
        case Choice::UiLang: break;
    }
    return ui().langTitle;
}

// The one-line explanation under a picker's header; null for none.
const char* choiceDescription() {
    switch (gChoice) {
        case Choice::NewPerDay: return ui().descNewPerDay;
        case Choice::SlowMs: return ui().descHardAfter;
        case Choice::AutoAdvance: return ui().descAdvance;
        case Choice::HoldTime: return ui().descHold;
        case Choice::OnKana: return ui().descOnKana;
        case Choice::Orientation: return tgc::board::hasAccel() ? ui().descOrient : nullptr;
        case Choice::Brightness: return ui().descBright;
        case Choice::UiLang: return ui().descLang;
        case Choice::ForgetWifi: break;
    }
    return nullptr;
}

}  // namespace

void drawChoice() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, choiceTitle());
    int top = kHeaderH;
    const int rowH = layout().row1H;
    // "Forget": which network, between the header and the rows.
    if (gChoice == Choice::ForgetWifi) {
        gRender.drawLabel(d, gWifiMenuSsid.c_str(), kGutter, kHeaderH + 2, W - 2 * kGutter, 20,
                          tgc::theme::kWarn, kBlack, tgc::Renderer::kTextUi, tgc::Align::Centre);
        top += 22;
    }
    // The description, wrapped in the caption font, when every row still fits
    // under it (the four-row pickers in landscape go without).
    if (const char* desc = choiceDescription()) {
        const std::vector<tgc::TextSpan> lines = tgc::wrapText(desc, (W - 2 * kGutter) / 6);
        const int descH = 3 + static_cast<int>(lines.size()) * 10;
        const int rowsH = gChoiceCount * (rowH + tgc::theme::kGap) - tgc::theme::kGap +
                          2 * tgc::theme::kMarkerBand;
        if (top + descH + rowsH <= H - kHintH) {
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string line(desc + lines[i].begin, lines[i].len);
                const int y = top + 3 + 10 * static_cast<int>(i);
                wg::drawCaption(d, gRender, line.c_str(), kGutter, y, tgc::theme::kTextDim, kBlack);
            }
            top += descH;
        }
    }
    gChoiceList.setCount(gChoiceCount);
    layoutList(gChoiceList, top, H - kHintH, rowH);
    wg::drawList(d, gChoiceList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        char label[24], value[24];
        choiceLabel(i, label, sizeof label);
        choiceValue(i, value, sizeof value);
        wg::Row row;
        row.label = label;
        row.value = value;
        row.keepLabel = true;
        row.focused = f;
        if (gChoiceSaved >= 0) row.radio = i == gChoiceSaved ? wg::Radio::On : wg::Radio::Off;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintChoose);
    presentList(gChoiceList);
}

namespace {

// Shows the picker set up by openChoice(): the saved value's row (or none,
// -1) is marked and focused.
void showChoice(int saved) {
    gChoiceSaved = saved;
    gChoiceList.setCount(gChoiceCount);
    gChoiceList.reset(saved >= 0 ? saved : 0);
    gScreen = Screen::Choice;
    drawScreen();
}

}  // namespace

// Opens one of the Screen::Choice pickers (2/3/4 rows) with the saved value
// marked and focused when it is one of the candidates.
void openChoice(Choice which) {
    gChoice = which;
    int saved = -1;
    if (which == Choice::Orientation) {
        // Auto first, as in the language picker; only with an accelerometer.
        gChoiceCount = 0;
        if (tgc::board::hasAccel()) gChoiceValues[gChoiceCount++] = kOrientAuto;
        gChoiceValues[gChoiceCount++] = kOrientPortrait;
        gChoiceValues[gChoiceCount++] = kOrientLandscape;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == shownOrientPref()) saved = i;
        showChoice(saved);
        return;
    }
    if (which == Choice::ForgetWifi) {
        // Cancel first and focused, no saved value: forgetting takes a tap and
        // a hold.
        gChoiceCount = 2;
        gChoiceValues[0] = 0;
        gChoiceValues[1] = 1;
        showChoice(-1);
        return;
    }
    if (which == Choice::UiLang) {
        gChoiceCount = 3;
        gChoiceValues[0] = tgc::kUiLangAuto;
        gChoiceValues[1] = tgc::kUiLangEnglish;
        gChoiceValues[2] = tgc::kUiLangSpanish;
        saved = gUiLangPref;  // rows are in preference order
        if (saved < 0 || saved >= gChoiceCount) saved = -1;
        showChoice(saved);
        return;
    }
    if (which == Choice::OnKana) {
        // The row is the stored tgc::OnKana.
        gChoiceCount = 2;
        gChoiceValues[0] = static_cast<uint16_t>(tgc::OnKana::Katakana);
        gChoiceValues[1] = static_cast<uint16_t>(tgc::OnKana::Hiragana);
        showChoice(static_cast<int>(gOnKana));
        return;
    }
    if (which == Choice::AutoAdvance || which == Choice::HoldTime) {
        // ui/InputPrefs.h: the row is the stored index.
        gChoiceCount = 3;
        for (int i = 0; i < gChoiceCount; ++i) gChoiceValues[i] = static_cast<uint16_t>(i);
        saved = which == Choice::AutoAdvance ? gAdvancePref : gHoldPref;
        if (saved >= gChoiceCount) saved = -1;
        showChoice(saved);
        return;
    }
    gChoiceCount = 4;
    if (which == Choice::Brightness) {
        gChoiceValues[0] = 32;
        gChoiceValues[1] = 64;
        gChoiceValues[2] = 128;
        gChoiceValues[3] = 255;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gBrightness) saved = i;
    } else if (which == Choice::NewPerDay) {
        gChoiceValues[0] = 3;
        gChoiceValues[1] = 5;
        gChoiceValues[2] = 10;
        gChoiceValues[3] = 20;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.newPerDay()) saved = i;
    } else {
        gChoiceValues[0] = 1500;
        gChoiceValues[1] = 3000;
        gChoiceValues[2] = 5000;
        gChoiceValues[3] = 0xFFFF;  // disables Hard
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.slowMs()) saved = i;
    }
    showChoice(saved);
}

namespace {

// Applies the focused choice, persists it and returns to the settings page it
// came from (the Wi-Fi submenu for "Forget").
void applyChoice() {
    const int sel = gChoiceList.focus();
    if (gChoice == Choice::Orientation) {
        gOrientPref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar("orient", gOrientPref);
        gScreen = Screen::Menu;
        if (orientAuto()) {
            // The hold is still down: keep the picture; orientTick() takes over
            // from it after the release and the settle time.
            gOrient.reset(tgc::board::rotation());
            drawScreen();
        } else {
            applyRotation(fixedRotation());  // redraws the active screen
        }
        return;
    }
    if (gChoice == Choice::ForgetWifi) {
        if (gChoiceValues[sel] == 1) {
            tgc::HomeWifi::forgetCredentials();
            wifiMenuNote(ui().wifiForgotten, tgc::theme::kOkHi);
        }
        openWifiMenu(false);
        return;
    }
    if (gChoice == Choice::UiLang) {
        gUiLangPref = static_cast<int>(gChoiceValues[sel]);
        gPrefs.putInt("ui", gUiLangPref);
        setUiForLang(gPack->header().lang);  // re-render in the chosen language
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (gChoice == Choice::Brightness) {
        gBrightness = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar("bri", gBrightness);
        tgc::board::setBrightness(gBrightness);
        gDimmed = false;
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (gChoice == Choice::AutoAdvance) {
        gAdvancePref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar(tgc::kPrefAdvance, gAdvancePref);
        backToMenu();
        return;
    }
    if (gChoice == Choice::OnKana) {
        gOnKana = static_cast<tgc::OnKana>(gChoiceValues[sel]);
        gPrefs.putUChar("onkana", static_cast<uint8_t>(gOnKana));
        // The question on screen changes too, not only the next one.
        tgc::restyleReadings(*gPack, gOnKana, &gQ);
        backToMenu();
        return;
    }
    if (gChoice == Choice::HoldTime) {
        // Read by loop() from the next press on; the hold that chose it has
        // fired already.
        gHoldPref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar(tgc::kPrefHold, gHoldPref);
        backToMenu();
        return;
    }
    const uint16_t v = gChoiceValues[sel];
    if (gChoice == Choice::NewPerDay) {
        gSrs.setNewPerDay(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
        gQueue.rebuild(*gPack, gSrs, gRng);  // the daily new-card cap changed
    } else {
        gSrs.setSlowMs(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gScreen = Screen::Menu;
    drawScreen();
}

}  // namespace

// loop() on a picker (handleInput()).
void choiceLoop(int a, int b) {
    if (a == 1) {
        moveListFocus(gChoiceList);
        drawScreen();
    } else if (a == 2) {
        applyChoice();
    }
    if (b && gScreen == Screen::Choice) {  // cancel: back to where the picker came from
        if (gChoice == Choice::ForgetWifi) {
            openWifiMenu(false);
        } else {
            gScreen = Screen::Menu;
            drawScreen();
        }
    }
}

}  // namespace app
}  // namespace tgc
