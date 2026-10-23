// The date screens; see DateScreens.h.
#include "app/DateScreens.h"

#include <Arduino.h>

#include <cstring>

#include "app/Clock.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/Net.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "render/Widgets.h"
#include "srs/TimeZone.h"
#include "srs/WallClock.h"
#include "ui/ScreenLayout.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

Screen gAfterDate = Screen::Quiz;  // where the date screen returns to
// Days from the current day's date to the proposed "today" (negative =
// earlier), within gSrs.manualStepsMin()..manualStepsMax().
int32_t gDateSteps = 0;

// Date screen (docs/srs.md section 5, power-cut rule): the last-used date and a
// proposed "today" that starts at the current day. A tap adds a day, a side
// tap takes one away, a hold confirms, a side hold cancels. Below the current
// day the index is kept and the anchor moves back (SrsStore::setDateManual(),
// only on a store with an anchor). Asked on a power-on boot (where the network
// time comes first when it can, see dateSyncBegin()) and opened from Settings >
// Device > Date for manual corrections.

namespace {

// The proposed "today" as a calendar day (days since 1970-01-01).
int32_t dateTarget() { return gSrs.epochDayOf(gSrs.today()) + gDateSteps; }

const char* weekdayName(uint16_t day) {
    return ui().weekdays[tgc::weekdayFromDays(gSrs.epochDayOf(day))];
}

}  // namespace

void openDate(Screen after) {
    gAfterDate = after;
    gDateSteps = 0;
    gScreen = Screen::Date;
    drawScreen();
}

namespace {

// "Sat 17 Oct": a day as the date screen's "last used" line writes it.
void weekdayDayMonth(uint16_t day, char* out, size_t n) {
    char dm[16];
    shortDate(day, dm, sizeof dm);
    snprintf(out, n, "%s %s", weekdayName(day), dm);
}

// The calendar tile (§2.4): an accent band with "OCT 2026", the day in the
// numeral face and the weekday under it, for calendar day `epochDay`.
void drawCalendarTile(lgfx::LovyanGFX& d, const tgc::layout::Date& l, int32_t epochDay) {
    using namespace tgc::theme;
    const tgc::layout::Rect& t = l.tile;
    int y = 0;
    unsigned m = 1, dd = 1;
    tgc::civilFromDays(epochDay, &y, &m, &dd);
    d.fillRect(t.x, t.y, t.w, t.h, kSurface);
    d.fillRect(t.x, t.y, t.w, l.bandH, kAccent);
    char band[16], num[4];
    snprintf(band, sizeof band, "%s %d", ui().months[(m + 11) % 12], y);
    for (char* p = band; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
    gRender.drawLabel(d, band, t.x, t.y, t.w, l.bandH, kText, kAccent,
                      tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
    snprintf(num, sizeof num, "%u", dd);
    gRender.drawLabel(d, num, t.x, t.y + l.bandH + 2, t.w, 36, kText, kSurface,
                      tgc::Renderer::kTextUiNumeral, tgc::Align::Centre);
    gRender.drawLabel(d, ui().weekdaysLong[tgc::weekdayFromDays(epochDay)], t.x,
                      t.y + l.bandH + 40, t.w, 20, kTextDim, kSurface, tgc::Renderer::kTextUi,
                      tgc::Align::Centre);
}

// The boot-time question's network attempt (gDateSync == Joining): what it is
// doing, over which network, and that a tap gives up and shows the stepper.
void drawDateNet() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
    gRender.drawCentred(d, ui().dateNetTitle, 4, mid - 36, W - 8, 40, 2, kText, kBg,
                        tgc::Renderer::kTextUi);
    char line[64];
    snprintf(line, sizeof line, ui().dateNetViaFmt, gHome.ssid());
    drawCaptionLines(d, line, 4, mid + 8, W - 8, 2, kTextDim);
    wg::Hints h;
    h.tap = ui().hintByHand;
    drawHints(d, h, tgc::kKeyEnter);
    present();
}

}  // namespace

// The stepper (docs/ux-refresh.md, date mockup): at boot "Device was off." and
// the last-used day, then the calendar tile of the proposed day, the "+N days"
// / "-N days" pill, why it is asked by hand when the network time failed, and
// what to step to. The hint bar has both buttons' two actions: "• +1 day ▬ OK"
// and "◂ -1 day ▬ cancel", the side ones on a row of their own when the panel
// is too narrow for one row (the portraits); "-1 day" is dimmed at the floor.
void drawDate() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().dateTitle, nullptr, gDateSync == DateSync::Joining);
    if (gDateSync == DateSync::Joining) {
        drawDateNet();
        return;
    }
    wg::Hints h;
    h.tap = ui().dateOneDay;
    h.hold = ui().firstLangHold;  // "OK"
    h.side = &tgc::kIconBack;
    h.sideWord = ui().dateBackOneDay;
    h.sideDim = gDateSteps <= gSrs.manualStepsMin();
    h.sideHold = ui().hintCancel;
    const tgc::layout::Date l =
        tgc::layout::dateFor(W, H, wg::hintBarHeight(W, boardHints(h, tgc::kKeyRight)));
    const bool land = W > H;
    const tgc::Align align = land ? tgc::Align::Left : tgc::Align::Centre;
    char line[64], day[32];

    // At boot: the device was off; always: the day it last ran.
    const bool boot = gAfterDate == Screen::Quiz;
    weekdayDayMonth(gBootDay, day, sizeof day);
    snprintf(line, sizeof line, "%s %s", ui().dateLastUsed, day);
    const tgc::layout::Rect& n = l.notice;
    if (boot)
        gRender.drawLabel(d, ui().dateWasOff, n.x, n.y, n.w, 8, kWarn, kBg,
                          tgc::Renderer::kTextBuiltinSmall, align);
    gRender.drawLabel(d, line, n.x, boot ? n.y + 10 : n.y + 5, n.w, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, align);

    // The proposed "today": this is what the buttons change.
    drawCalendarTile(d, l, dateTarget());
    if (gDateSteps == 0)
        snprintf(line, sizeof line, "%s", ui().dateToday);
    else if (gDateSteps == 1)
        snprintf(line, sizeof line, "%s", ui().dateOneDay);
    else if (gDateSteps == -1)
        snprintf(line, sizeof line, "%s", ui().dateBackOneDay);
    else if (gDateSteps > 0)
        snprintf(line, sizeof line, ui().dateDaysFmt, static_cast<unsigned>(gDateSteps));
    else
        snprintf(line, sizeof line, ui().dateBackDaysFmt, static_cast<unsigned>(-gDateSteps));
    const int pw = wg::pillWidth(line);
    wg::drawPill(d, gRender, l.pillCx - pw / 2, l.pillY, line, gDateSteps ? kWarn : kTextDim,
                 kSurfaceHi);

    // Why it is asked by hand after all (the network time failed), then what
    // to step to (the hint bar has the buttons).
    const tgc::layout::Rect& e = l.explain;
    const int maxLines = e.h / 10;
    int lines = 0;
    if (gDateSync == DateSync::NoNetwork || gDateSync == DateSync::NoTime) {
        const char* status =
            gDateSync == DateSync::NoNetwork ? ui().dateNoNetwork : ui().dateNoTime;
        lines = drawCaptionLines(d, status, e.x, e.y, e.w, maxLines, kWarn, align);
        ++lines;  // a blank line under it
    }
    drawCaptionLines(d, keysOr(ui().dateExplain, ui().dateExplainKeys), e.x, e.y + 10 * lines,
                     e.w, maxLines - lines, kTextDim, align);

    drawHints(d, h, tgc::kKeyRight);
    present();
}

// --- Settings > Device > Date ------------------------------------------------

namespace {

// "+1", "-3", "+5:30", "0": one offset for the zone list's right column.
void formatOffsetShort(int offsetMin, char* out, size_t n) {
    if (offsetMin == 0) {
        snprintf(out, n, "0");
        return;
    }
    const int a = offsetMin < 0 ? -offsetMin : offsetMin;
    if (a % 60)
        snprintf(out, n, "%c%d:%02d", offsetMin < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(out, n, "%c%d", offsetMin < 0 ? '-' : '+', a / 60);
}

// A zone's offsets: "+1/+2" (short) or "UTC+1, summer UTC+2" (footer); one
// offset alone without daylight time.
void tzOffsets(const tgc::TzRule& r, bool longForm, char* out, size_t n) {
    char a[16], b[16];
    if (longForm) {
        tgc::formatUtcOffset(r.stdOffsetSec / 60, a, sizeof a);
        tgc::formatUtcOffset(r.dstOffsetSec / 60, b, sizeof b);
    } else {
        formatOffsetShort(r.stdOffsetSec / 60, a, sizeof a);
        formatOffsetShort(r.dstOffsetSec / 60, b, sizeof b);
    }
    if (!r.hasDst)
        snprintf(out, n, "%s", a);
    else if (longForm)
        snprintf(out, n, ui().tzSummerFmt, a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

// The zone in use for the Date submenu: its city, else its offset (an old page
// left only that), else "none".
void currentZoneLabel(char* out, size_t n) {
    if (!gTzKnown)
        snprintf(out, n, "%s", ui().tzNone);
    else if (gTzName[0])
        tgc::tzZoneCity(gTzName, out, n);
    else
        tgc::formatUtcOffset(gTz.stdOffsetSec / 60, out, n);
}

}  // namespace

// Settings > Device > Date: the date (the stepper) and the time zone (the
// list), in the Wi-Fi submenu's look: the row's value is the date or the
// zone's city. The side button goes back to the Device page.
tgc::ListView gDateMenuList;
char gDateMenuNote[48] = "";

namespace {

constexpr int kDateMenuRows = 2;
uint16_t gDateMenuNoteColor = 0;

}  // namespace

void drawDateMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().dateTitle);
    char date[16], zone[48];
    formatDate(gSrs.today(), date, sizeof date);
    currentZoneLabel(zone, sizeof zone);
    const char* values[kDateMenuRows] = {date, zone};
    // Narrow rows put the value under the label: beside it, portrait cut the
    // date to "2026-..." (2026-10-20).
    const bool wide = wideRows();
    gDateMenuList.setCount(kDateMenuRows);
    layoutList(gDateMenuList, kHeaderH, H - kHintH - kNoteH,
               wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gDateMenuList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().dateMenuRows[i];
        if (wide)
            row.value = values[i];
        else
            row.caption = values[i];
        row.keepLabel = true;
        row.chevron = true;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gDateMenuNote, gDateMenuNoteColor);
    menuHints(d, ui().hintOpen);
    present();  // a move clears the note: the whole frame
}

// Settings > Device > Date > Zone: "From phone" (the zone the portal page last
// sent), then tgc::kTzZones. Row 0 is "From phone", row i is kTzZones[i - 1].
tgc::ListView gTzListView;

namespace {

int tzListRows() { return 1 + static_cast<int>(tgc::kTzZoneCount); }

// The row of the zone in use: the list entry chosen by hand, else "From phone".
int tzCurrentRow() {
    if (gTzManual)
        for (size_t i = 0; i < tgc::kTzZoneCount; ++i)
            if (std::strcmp(tgc::kTzZones[i].name, gTzName) == 0) return static_cast<int>(i) + 1;
    return 0;
}

}  // namespace

// Each row: the city and its offsets ("UTC+1, summer UTC+2" under it in
// portrait, or "+1/+2" when that does not fit; "+1/+2" right of it in
// landscape); "From phone" with the page's city and offsets, or how to get
// them. A radio marks the zone in use.
void drawTzList() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().tzTitle);
    gTzListView.setCount(tzListRows());
    const bool wide = wideRows();
    layoutList(gTzListView, kHeaderH, H - kHintH, wide ? layout().row1H : layout().row2H);
    const int current = tzCurrentRow();
    wg::drawList(d, gTzListView, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        char label[40], caption[64];
        if (i == 0) {
            snprintf(label, sizeof label, "%s", ui().tzFromPhone);
            if (gPhoneTzKnown && gPhoneTzName[0]) {
                char city[40], offsets[24];
                tgc::tzZoneCity(gPhoneTzName, city, sizeof city);
                tzOffsets(gPhoneTz, false, offsets, sizeof offsets);
                snprintf(caption, sizeof caption, "%s · %s", city, offsets);
            } else if (gPhoneTzKnown) {
                tzOffsets(gPhoneTz, !wide, caption, sizeof caption);
            } else {
                snprintf(caption, sizeof caption, "%s", ui().tzPhoneNone);
            }
        } else {
            const tgc::TzZone& z = tgc::kTzZones[i - 1];
            tgc::tzZoneCity(z.name, label, sizeof label);
            tgc::TzRule r;
            caption[0] = 0;
            if (tgc::parseTzRule(z.rule, &r)) {
                tzOffsets(r, !wide, caption, sizeof caption);
                if (!wide && wg::captionWidth(caption) > w - 2 * tgc::theme::kRowPadX)
                    tzOffsets(r, false, caption, sizeof caption);
            }
        }
        wg::Row row;
        row.label = label;
        if (wide) {
            row.value = caption;
            row.keepLabel = i == 0;  // "From phone" stays whole, the city is cut
        } else {
            row.caption = caption;
        }
        row.radio = i == current ? wg::Radio::On : wg::Radio::Off;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintChoose);
    presentList(gTzListView);
}

// Rebuilds the queue and shows the quiz (or the done screen if nothing is due).
// Used after the date screen so newly due cards appear.
void leaveDate() {
    dateSyncCancel();
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

namespace {

// Applies the stepped days (setDateManual: later dates advance the index,
// earlier ones move the anchor back; either restarts the day carry, see
// docs/srs.md section 5), persists them, rebuilds the queue (cards may now be
// due) and goes back to where the date screen was opened from: Settings >
// Device > Date, or the quiz on a power-off boot.
void applyDate() {
    dateSyncCancel();
    const tgc::ManualDate r = gSrs.setDateManual(gDateSteps);
    char date[16];
    formatDate(gSrs.today(), date, sizeof date);
    Serial.printf("clock: set by hand, %+ld days: %s, day %u (%s)\n",
                  static_cast<long>(gDateSteps),
                  r == tgc::ManualDate::Advanced     ? "advanced"
                  : r == tgc::ManualDate::Reanchored ? "re-anchored"
                  : r == tgc::ManualDate::Same       ? "same day"
                                                     : "rejected",
                  (unsigned)gSrs.today(), date);
    gSrs.save(gSrsIo);
    gDirty = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gAfterDate != Screen::Quiz) {
        openDateMenu(false);
        return;
    }
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// --- Settings > Device > Date: loop ------------------------------------------

void dateMenuNote(const char* text, uint16_t color) {
    snprintf(gDateMenuNote, sizeof gDateMenuNote, "%s", text ? text : "");
    gDateMenuNoteColor = color;
}

}  // namespace

// Opens the Date submenu: `fresh` from the settings home (first row, no note),
// else back from the stepper or the zone list (selection and note kept).
void openDateMenu(bool fresh) {
    if (fresh) {
        gDateMenuList.setCount(kDateMenuRows);
        gDateMenuList.reset(0);
        gDateMenuNote[0] = 0;
    }
    gScreen = Screen::DateMenu;
    drawScreen();
}

namespace {

void openTzList() {
    gTzListView.setCount(tzListRows());
    gTzListView.reset(tzCurrentRow());
    gScreen = Screen::TzList;
    drawScreen();
}

}  // namespace

void dateMenuLoop(int a, int b) {
    if (a == 1) {
        gDateMenuList.next();
        gDateMenuNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        if (gDateMenuList.focus() == 0)
            openDate(Screen::DateMenu);
        else
            openTzList();
    }
    if (b && gScreen == Screen::DateMenu) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

namespace {

// A hold on the zone list: the zone chosen by hand (NVS `tzm`), or "From
// phone", which drops that choice. It takes effect for the day lengths now and
// for the next real time (NTP or the page); the day and the rollover stay
// where they are until then.
void applyTzChoice() {
    const int sel = gTzListView.focus();
    if (sel == 0) {
        if (gPrefs.isKey(kPrefTzManual)) gPrefs.remove(kPrefTzManual);
    } else {
        gPrefs.putString(kPrefTzManual, tgc::kTzZones[sel - 1].name);
    }
    loadTimeZone();
    if (gTzKnown)
        dateMenuNote(ui().tzSaved, tgc::theme::kOkHi);
    else
        dateMenuNote(ui().tzPhoneNone, tgc::theme::kWarn);
    openDateMenu(false);
}

}  // namespace

void tzListLoop(int a, int b) {
    if (a == 1) {
        moveListFocus(gTzListView);
        drawScreen();
    } else if (a == 2) {
        applyTzChoice();
    }
    if (b && gScreen == Screen::TzList) openDateMenu(false);  // back, nothing changed
}

// loop() on the date stepper (handleInput()).
void dateLoop(int a, int b) {
    // Network first (boot-time question): "Getting the date via ..." until
    // the network time answers (on the quiz then) or fails (the stepper with
    // the reason); any press gives up on it, radio off, and shows the
    // stepper. Presses are handled before the poll, which may block for the
    // NTP request.
    // The stepper: tap one more day, side tap one day less (as the quiz's
    // side tap is the previous option; below the current day only with an
    // anchor, see SrsStore::manualStepsMin()), hold confirm, side hold
    // cancel.
    if (gDateSync == DateSync::Joining) {
        if (a || b) {
            dateSyncCancel();
            drawScreen();
        } else {
            dateSyncPoll();
        }
    } else if (a == 1) {
        if (gDateSteps < gSrs.manualStepsMax()) ++gDateSteps;
        drawScreen();
    } else if (a == 2) {
        applyDate();
    } else if (b == 1) {
        if (gDateSteps > gSrs.manualStepsMin()) {
            --gDateSteps;
            drawScreen();
        }
    } else if (b == 2) {  // cancel: back to the Date submenu, or straight into the quiz
        if (gAfterDate != Screen::Quiz)
            openDateMenu(false);
        else
            leaveDate();
    }
}

}  // namespace app
}  // namespace tgc
