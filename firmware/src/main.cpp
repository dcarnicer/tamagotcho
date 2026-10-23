// Tamagotcho Phase 2: open the saved pack in /packs and run a spaced-repetition
// multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board/Board.h"
#include "catalog/Catalog.h"
#include "imu/OrientationFilter.h"
#include "pack/PackReader.h"
#include "pack/Utf8.h"
#include "platform/FsPackSource.h"
#include "platform/FsQuiet.h"
#include "platform/FsSrsFile.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/PackHash.h"
#include "platform/WifiPortal.h"
#include "portal/PortalLogic.h"
#include "power/BatteryGuard.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "render/Widgets.h"
#include "srs/DeckProgress.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"
#include "srs/WallClock.h"
#include "ui/DeckSummary.h"
#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/ListView.h"
#include "ui/QuizUi.h"
#include "ui/ScreenLayout.h"
#include "ui/SettingsTree.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/UiLang.h"
#include "Credits.h"
#include "Version.h"
#include "app/About.h"
#include "app/App.h"
#include "app/Boot.h"
#include "app/Choice.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Detail.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/GetDecks.h"
#include "app/Input.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Progress.h"
#include "app/Quiz.h"
#include "app/Remote.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/Stats.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "app/WifiMenu.h"

using tgc::board::Button;

// The StickS3 has no RTC and the ESP32's RTC domain does not survive a power-off
// (verified: RTC memory is cleared when the device is switched off). RTC_NOINIT
// memory does survive a soft reset, so it tells us whether this boot followed a
// power cut -- in which case the elapsed days are unknown and we ask the user.
RTC_NOINIT_ATTR uint32_t gPowerMagic;
RTC_NOINIT_ATTR uint32_t gPowerMagicInv;
constexpr uint32_t kPowerMagic = 0x7A5C1234u;
bool gPoweredOff = false;

namespace tgc {
namespace app {

uint16_t kGrey, kHighlight, kGreen, kRed, kYellow;

Screen gAfterDate = Screen::Quiz;  // where the date screen returns to
// Days from the current day's date to the proposed "today" (negative =
// earlier), within gSrs.manualStepsMin()..manualStepsMax().
int32_t gDateSteps = 0;
uint16_t gBootDay = 0;  // persisted day index at boot: "last used" on the date screen
// A real wall-clock time was applied since boot (applyWallClock), so the
// power-cut date question is not needed.
bool gWallClockSynced = false;
tgc::TzRule gTz;                // the zone in use (valid while gTzKnown)
bool gTzKnown = false;
bool gTzManual = false;         // gTz came from Settings > Device > Date > Zone
char gTzName[48] = "";          // its IANA name, "" when unknown
char gTzRuleText[tgc::kTzRuleMaxLen + 1] = "";
tgc::TzRule gPhoneTz;           // the zone the page last sent (the "From phone" row)
bool gPhoneTzKnown = false;
char gPhoneTzName[48] = "";
DateSync gDateSync = DateSync::None;

// On-device Wi-Fi portal (roadmap phase 4). Started on the Wi-Fi screen and
// stopped on exit. `gWifiBootUpload` marks the boot-time "no packs yet" state:
// the Wi-Fi screen is then the only screen (there is no quiz to return to), so
// the side button is inert and the app moves to the quiz as soon as a pack has
// been uploaded. It is set only at boot -- entering the portal later from the
// settings home and then deleting every pack must still let the side button
// leave for the settings home.
tgc::WifiPortal gPortal;
bool gWifiBootUpload = false;
PortalMode gPortalMode = PortalMode::Setup;
WifiStep gWifiStep = WifiStep::Serving;
int gQrPage = 0;                      // setup mode: 0 = join the AP, 1 = open the page
char gWifiNote[64] = "";              // setup mode after the boot-time home join failed: why
// Bumped whenever the Wi-Fi screen changes under loop()'s feet (a mode switch,
// a blocking download from the page, leaving for Get decks), so the input read
// before the portal's poll() is not applied to what came after it.
uint32_t gWifiEpoch = 0;

// Settings > Wi-Fi (the submenu replacing the single settings row).
tgc::ListView gWifiMenuList;
String gWifiMenuSsid;          // the saved home network, read when the menu opens
char gWifiMenuNote[48] = "";   // footer note (cleared when the selection moves)
uint16_t gWifiMenuNoteColor = 0;
NetTestStep gTestStep = NetTestStep::NoNetwork;
String gTestIp;
int gTestRssi = 0;
int gTestTime = -1;            // network time: -1 not tried (no time zone), 0 failed, 1 ok
char gTestCatalog[64] = "";
uint16_t gTestCatalogColor = 0;
char gTestFail[48] = "";

enum class UpdateStep : uint8_t { Joining, Catalog, Done, Failed };
UpdateStep gUpdateStep = UpdateStep::Joining;
uint32_t gUpdateStartedAt = 0;  // millis() when the join began
uint32_t gUpdateDoneAt = 0;     // millis() when the result appeared
uint8_t gUpdateFound = 0;       // updates found by this check
char gUpdateErr[48] = "";       // the failure's title (join or catalog)
uint8_t gUpdateCount = 0;       // installed decks with an update (NVS `updcount`)
uint32_t gUpdateDay = 0;        // epoch day of the last check (NVS `updday`)
bool gUpdateKnown = false;      // a catalog read succeeded (NVS `updok`)
constexpr uint32_t kUpdateJoinMs = 30000;  // a tap skips sooner
constexpr uint32_t kUpdateResultMs = 2000; // the result before the quiz
tgc::KeyRepeat gKeyRepeat;      // the held arrow of a keyboard (keyTurn())

void updateCheckRead();
// "18 Oct": the current day for the settings header and the Date row.
void shortDate(uint16_t day, char* out, size_t n) {
    int y = 0;
    unsigned m = 1, dd = 1;
    tgc::civilFromDays(gSrs.epochDayOf(day), &y, &m, &dd);
    st::formatDayMonth(dd, ui().months[(m + 11) % 12], out, n);
}

// First-boot language picker (setup(), docs/decisions.md): the choice-screen
// look with a neutral header ("LANGUAGE / IDIOMA"), every row in its own
// language and the hint bar in the language of the focused row, so it reads
// the same whatever table is active. The side button does nothing here:
// there is nothing to go back to yet, so the bar shows no side action.
tgc::ListView gFirstLangList;

void drawFirstLang() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    char title[40];
    snprintf(title, sizeof title, "%s / %s", kUiEn.langTitle, kUiEs.langTitle);
    menuHeader(d, title);
    gFirstLangList.setCount(tgc::kFirstBootLangRows);
    layoutList(gFirstLangList, kHeaderH, H - kHintH, layout().row1H);
    wg::drawList(d, gFirstLangList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().langRows[tgc::firstBootLangPref(i)];
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    const UiStrings& hl =
        tgc::firstBootLangPref(gFirstLangList.focus()) == tgc::kUiLangSpanish ? kUiEs : kUiEn;
    wg::Hints hints;
    hints.tap = hl.firstLangTap;
    hints.hold = hl.firstLangHold;
    drawHints(d, hints);
    present();  // the hints follow the focus: the whole frame
}

// The calendar date of SRS day `day`: the store's wall-clock anchor once a real
// time was synced, else the fixed 2026-10-17 base (docs/srs.md section 4.1).
// Only differences matter for the schedule; the date is for the user.
void formatDate(uint16_t day, char* out, size_t n) {
    int y = 0;
    unsigned m = 0, d = 0;
    tgc::civilFromDays(gSrs.epochDayOf(day), &y, &m, &d);
    snprintf(out, n, "%04d-%02d-%02d", y, m, d);
}

// Date screen (docs/srs.md section 5, power-cut rule): the last-used date and a
// proposed "today" that starts at the current day. A tap adds a day, a side
// tap takes one away, a hold confirms, a side hold cancels. Below the current
// day the index is kept and the anchor moves back (SrsStore::setDateManual(),
// only on a store with an anchor). Asked on a power-on boot (where the network
// time comes first when it can, see dateSyncBegin()) and opened from Settings >
// Device > Date for manual corrections.

// The proposed "today" as a calendar day (days since 1970-01-01).
int32_t dateTarget() { return gSrs.epochDayOf(gSrs.today()) + gDateSteps; }

const char* weekdayName(uint16_t day) {
    return ui().weekdays[tgc::weekdayFromDays(gSrs.epochDayOf(day))];
}

void openDate(Screen after) {
    gAfterDate = after;
    gDateSteps = 0;
    gScreen = Screen::Date;
    drawScreen();
}

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

// Settings > Device > Date: the date (the stepper) and the time zone (the
// list), in the Wi-Fi submenu's look: the row's value is the date or the
// zone's city. The side button goes back to the Device page.
constexpr int kDateMenuRows = 2;
tgc::ListView gDateMenuList;
char gDateMenuNote[48] = "";
uint16_t gDateMenuNoteColor = 0;

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
int tzListRows() { return 1 + static_cast<int>(tgc::kTzZoneCount); }

// The row of the zone in use: the list entry chosen by hand, else "From phone".
int tzCurrentRow() {
    if (gTzManual)
        for (size_t i = 0; i < tgc::kTzZoneCount; ++i)
            if (std::strcmp(tgc::kTzZones[i].name, gTzName) == 0) return static_cast<int>(i) + 1;
    return 0;
}

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

// A QR code of `text`, as large as fits a `box` px square, with a white
// 2-module quiet zone (the spec asks for 4; phone cameras read 2 fine, and on a
// 135 px screen every pixel goes to the modules). Top-left at (x, y). Returns
// the side drawn, 0 when the text is too long for a legible code.
int qrSide(const char* text, int box) {
    const int version = tgc::qrVersionForBytes(std::strlen(text));
    if (version == 0) return 0;
    const int cells = tgc::qrModules(version) + 4;
    const int px = box / cells;
    return px > 0 ? cells * px : 0;
}

int drawQr(lgfx::LovyanGFX& d, const char* text, int x, int y, int box) {
    const int side = qrSide(text, box);
    if (side == 0) return 0;
    const int version = tgc::qrVersionForBytes(std::strlen(text));
    const int modules = tgc::qrModules(version);
    const int px = side / (modules + 4);
    d.fillRect(x, y, side, side, kWhite);
    // Exactly modules * px wide, so qrcode() uses px-sized modules with no
    // margin of its own; starting at our version it never needs a larger one.
    d.qrcode(text, x + 2 * px, y + 2 * px, modules * px, static_cast<uint8_t>(version));
    return side;
}

// The page address in text set `set`: whole when it fits `w`, else without
// "http://" and the trailing '/' (a browser takes the bare IP too).
void shownUrl(const char* url, int w, uint8_t set, char* out, size_t n) {
    snprintf(out, n, "%s", url);
    if (gRender.textWidth(out, set) <= w) return;
    const char* host = std::strncmp(url, "http://", 7) == 0 ? url + 7 : url;
    snprintf(out, n, "%s", host);
    const size_t len = std::strlen(out);
    if (len > 0 && out[len - 1] == '/') out[len - 1] = 0;
}

// The free space for decks: "free 4.9 / 5.9 MB" over a bar of it (13 px).
void drawFreeSpace(lgfx::LovyanGFX& d, int x, int y, int w, tgc::Align align) {
    const size_t total = LittleFS.totalBytes();
    const size_t freeB = tgc::fsFreeBytes();
    char a[16], b[16], line[48];
    formatMb(freeB, false, a, sizeof a);
    formatMb(total, true, b, sizeof b);
    snprintf(line, sizeof line, ui().wifiFreeFmt, a, b);
    gRender.drawLabel(d, line, x, y, w, 8, tgc::theme::kTextDim, kBlack,
                      tgc::Renderer::kTextBuiltinSmall, align);
    wg::drawProgress(d, x, y + 10, w, 3, static_cast<int>(freeB / 1024u),
                     static_cast<int>(total / 1024u));
}

// Wi-Fi portal screen (WifiPortal.h; docs/ux-refresh.md, Wi-Fi mockup).
// Serving: a QR code -- in setup mode the one that joins the device's access
// point, or (a tap) the one that opens the page; in home mode the page's
// address -- with what to scan for over it, then the network, the address in
// the label face (what to type if the QR does not scan), the portal's status
// (or a download's progress) and the free space as a bar. Portrait stacks them
// under the QR, landscape puts them right of it. Joining and failed: the
// network, why, and setup mode in the hint bar. The UI fonts need no pack, so
// it works before one is open.
void drawWifi() {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().wifiTitle, nullptr, gPortal.active() || gHome.active());
    // The side button (closeWifi()): stops the portal, starts studying on the
    // boot-time screen once a deck is there, and does nothing there before.
    wg::Hints hints;
    if (!(gWifiBootUpload && !gPack->isOpen())) {
        hints.side = &tgc::kIconBack;
        hints.sideWord = gWifiBootUpload ? ui().wifiStudyHint : ui().hintStop;
    }

    if (gWifiStep != WifiStep::Serving) {
        const bool failed = gWifiStep == WifiStep::Failed;
        const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
        gRender.drawCentred(d, failed ? gNetErrTitle : ui().wifiConnectingTo, 4, mid - 34, W - 8,
                            40, 2, failed ? kWarn : kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, failed ? gNetErrDetail : gHome.ssid(), 4, mid + 10, W - 8, 3,
                         failed ? kText : kAccentHi);
        if (failed) hints.tap = ui().wifiTapRetry;
        hints.hold = ui().wifiHoldSetup;
        drawHints(d, hints, tgc::kKeyRight);
        present();
        return;
    }

    const bool home = gPortalMode == PortalMode::Home;
    const bool land = W > H;
    const bool joinQr = !home && gQrPage == 0;
    const std::string qrText =
        joinQr ? tgc::wifiQrOpenNetwork(gPortal.ssid()) : std::string(gPortal.url());
    const char* qrLabel = joinQr ? ui().wifiScanJoin : ui().wifiScanOpen;

    int y = kHeaderH + 3;
    int tx = kGutter, tw = W - 2 * kGutter;
    tgc::Align align = tgc::Align::Centre;
    if (land) {
        const int side = drawQr(d, qrText.c_str(), kGutter, y, H - kHintH - 2 - y);
        tx = kGutter + side + 6;
        tw = W - kGutter - tx;
        align = tgc::Align::Left;
        y += drawCaptionLines(d, qrLabel, tx, y, tw, 2, kText, align) * 10 + 2;
    } else {
        drawCaptionLines(d, qrLabel, tx, y, tw, 1, kText);
        y += 11;
        // As large as fits: on the StickS3, 4 px modules for a version 2 code
        // (the page's address) and 3 px for the version 3 join code (the
        // "Tamagotcho-XXXX" payload is 33 bytes, one more than version 2 holds).
        const int box = std::min(W - 2 * kGutter, (H - y) * 56 / 100);
        const int side = qrSide(qrText.c_str(), box);
        drawQr(d, qrText.c_str(), (W - side) / 2, y, box);
        y += side + 4;
    }
    // Caption lines under (beside) the QR code.
    auto caption = [&](const char* s, uint16_t color, int lines = 1) {
        if (s && *s) y += drawCaptionLines(d, s, tx, y, tw, lines, color, align) * 10 + 1;
    };
    char buf[72];
    if (home) {
        snprintf(buf, sizeof buf, ui().wifiViaFmt, gPortal.ssid());
        caption(buf, kTextDim);
    } else {
        caption(gPortal.ssid(), kText);
    }
    shownUrl(gPortal.url(), tw, tgc::Renderer::kTextUi, buf, sizeof buf);
    gRender.drawLabel(d, buf, tx, y, tw, 20, kAccentHi, kBg, tgc::Renderer::kTextUi, align);
    y += 21;
    if (gPortal.mdnsUrl()[0]) {
        shownUrl(gPortal.mdnsUrl(), tw, kSmall, buf, sizeof buf);
        caption(buf, kTextDim);
    }

    // A download the page asked for: its progress; the side button cancels.
    if (gNetBusy) {
        const int pct = gNetPct < 0 ? 0 : gNetPct;
        snprintf(buf, sizeof buf, ui().wifiDownloadFmt, gNetDeck, (unsigned)pct);
        caption(buf, kWarn);
        wg::drawProgress(d, tx, y + 1, tw, 3, pct, 100);
        wg::Hints busy;
        busy.side = &tgc::kIconBack;
        busy.sideWord = ui().hintCancel;
        drawHints(d, busy);
        present();
        return;
    }
    // The boot-time fallback's reason first; the status line then gets one
    // line so the free space still fits the portrait screen.
    if (gWifiNote[0]) caption(gWifiNote, kWarn, 2);
    if (home && !gPortal.linkUp())
        caption(ui().wifiLinkLost, kWarn);
    else
        caption(gPortal.lastMessage(), kOkHi, gWifiNote[0] ? 1 : 2);
    drawFreeSpace(d, tx, y + 2, tw, align);

    if (!home) hints.tap = ui().wifiTapOtherQr;
    if (gWifiBootUpload && !gPack->isOpen() && tgc::HomeWifi::hasCredentials())
        hints.hold = ui().wifiGetDecksHint;
    drawHints(d, hints, tgc::kKeyRight);
    present();
}

// Settings > Wi-Fi: the web portal (home mode once a network is saved), the
// home network with its test (the network's name is the row's value), setup
// mode (the access point, e.g. to change networks) and "forget". The menu
// rows; a note above the hint bar says what the last action did.
constexpr int kWifiMenuRows = 4;

void drawWifiMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().wifiTitle);
    gWifiMenuList.setCount(kWifiMenuRows);
    layoutList(gWifiMenuList, kHeaderH, H - kHintH - kNoteH, layout().row1H);
    const char* home = gWifiMenuSsid.length() ? gWifiMenuSsid.c_str() : ui().wifiHomeNone;
    wg::drawList(d, gWifiMenuList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().wifiMenuRows[i];
        if (i == 1) {
            row.value = home;
            row.keepLabel = true;
        }
        row.chevron = true;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gWifiMenuNote, gWifiMenuNoteColor);
    menuHints(d, ui().hintOpen);
    present();  // a move clears the note: the whole frame
}

// Settings > Wi-Fi > Home network: joining, checking (NTP and the catalog),
// then the result -- network, IP, signal, catalog, network time -- with the
// radio off again; or why it failed, or that no network is saved. A hold
// tests again.
void drawNetTest() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().testTitle, nullptr, gHome.active());
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;

    if (gTestStep == NetTestStep::NoNetwork) {
        gRender.drawCentred(d, ui().wifiNoNetwork, 4, mid - 40, W - 8, 40, 2, kWarn, kBg,
                            tgc::Renderer::kTextUi);
        drawCaptionLines(d, ui().testNoNetworkHint, 4, mid + 4, W - 8, 4, kText);
        backHints(d);
        present();
        return;
    }
    if (gTestStep == NetTestStep::Joining || gTestStep == NetTestStep::Failed) {
        const bool failed = gTestStep == NetTestStep::Failed;
        gRender.drawCentred(d, failed ? gTestFail : ui().netConnecting, 4, mid - 34, W - 8, 40, 2,
                            failed ? kWarn : kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        backHints(d, nullptr, failed ? ui().testHoldAgain : nullptr);
        present();
        return;
    }
    // Checking / done: the network in the label face, then caption lines.
    int y = kHeaderH + 4;
    gRender.drawLabel(d, gHome.ssid(), 4, y, W - 8, 20, kText, kBg, tgc::Renderer::kTextUi,
                      tgc::Align::Centre);
    y += 24;
    const int step = H > W ? 5 : 2;
    auto line = [&](const char* s, uint16_t color, int lines = 2) {
        if (s && *s) y += drawCaptionLines(d, s, 4, y, W - 8, lines, color) * 10 + step;
    };
    char buf[72];
    snprintf(buf, sizeof buf, ui().testIpFmt, gTestIp.c_str());
    line(buf, kTextDim);
    snprintf(buf, sizeof buf, ui().testSignalFmt, gTestRssi,
             ui().testQuality[tgc::rssiQuality(gTestRssi)]);
    line(buf, kTextDim);
    const bool done = gTestStep == NetTestStep::Done;
    if (!done) {
        line(ui().testChecking, kWarn);
    } else {
        line(gTestCatalog, gTestCatalogColor);
        if (gTestTime >= 0)
            line(gTestTime ? ui().testTimeOk : ui().testTimeFail, gTestTime ? kOkHi : kWarn);
    }
    backHints(d, nullptr, done ? ui().testHoldAgain : nullptr);
    present();
}

// The boot-time question's network attempt ends without an answer from it:
// a join still in progress stops, radio off, and the status line goes. Called
// when the question is answered (by hand or by the network time) and when a
// press on the "Getting the date" screen skips to the stepper.
void dateSyncCancel() {
    if (gDateSync == DateSync::Joining) {
        netRadioOff();
        Serial.printf("clock: network time skipped, set by hand\n");
    }
    gDateSync = DateSync::None;
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

const char* clockSyncName(tgc::ClockSync k) {
    switch (k) {
        case tgc::ClockSync::Rejected: return "rejected";
        case tgc::ClockSync::Anchored: return "anchored";
        case tgc::ClockSync::Same: return "same day";
        case tgc::ClockSync::Advanced: return "advanced";
        case tgc::ClockSync::Reanchored: return "re-anchored";
    }
    return "?";
}

// Reads the time zone from NVS (see kPrefTz): a zone chosen by hand, else the
// one the portal page last sent (its rule, else its bare offset), else none.
// Hands it to the SRS store, whose day lengths follow it, and logs it.
void loadTimeZone() {
    gTzKnown = gTzManual = gPhoneTzKnown = false;
    gTzName[0] = gTzRuleText[0] = gPhoneTzName[0] = 0;
    char phoneRule[tgc::kTzRuleMaxLen + 1] = "";
    if (gPrefs.isKey(kPrefTzRule) &&
        gPrefs.getString(kPrefTzRule, phoneRule, sizeof phoneRule) > 0 &&
        tgc::parseTzRule(phoneRule, &gPhoneTz)) {
        gPhoneTzKnown = true;
    } else if (gPrefs.isKey(kPrefTz)) {
        const int off = gPrefs.getShort(kPrefTz, 0);
        gPhoneTz = tgc::fixedTzRule(off);
        tgc::formatFixedTzRule(off, phoneRule, sizeof phoneRule);
        gPhoneTzKnown = tgc::tzRuleOffsetsPlausible(gPhoneTz);
    }
    if (gPhoneTzKnown && gPrefs.isKey(kPrefTzName))
        gPrefs.getString(kPrefTzName, gPhoneTzName, sizeof gPhoneTzName);

    char manual[48] = "";
    if (gPrefs.isKey(kPrefTzManual)) gPrefs.getString(kPrefTzManual, manual, sizeof manual);
    const tgc::TzZone* z = tgc::findTzZone(manual);
    if (z && tgc::parseTzRule(z->rule, &gTz)) {
        gTzKnown = gTzManual = true;
        snprintf(gTzName, sizeof gTzName, "%s", z->name);
        snprintf(gTzRuleText, sizeof gTzRuleText, "%s", z->rule);
    } else if (gPhoneTzKnown) {
        gTz = gPhoneTz;
        gTzKnown = true;
        snprintf(gTzName, sizeof gTzName, "%s", gPhoneTzName);
        snprintf(gTzRuleText, sizeof gTzRuleText, "%s", phoneRule);
    }
    gSrs.setTimeZone(gTzKnown ? gTz : tgc::fixedTzRule(0));
    if (gTzKnown)
        Serial.printf("clock: zone %s %s (%s)\n", gTzName[0] ? gTzName : "-", gTzRuleText,
                      gTzManual ? "chosen by hand" : "from the phone");
    else
        Serial.printf("clock: no time zone yet\n");
}

// Applies a real wall-clock time to the SRS day clock (docs/srs.md section 5.1):
// the phone's browser through the portal's POST /time, or NTP over the home
// Wi-Fi (Get decks, the boot-time date question, the network test). Anchors or
// advances the day, aligns the 04:00 rollover for `zone` and brings the screen
// up to date: a pending power-cut date question is answered, and a new day
// rebuilds the queue. False when the time was rejected as implausible; nothing
// changes then.
bool applyWallClock(int64_t utcMs, const tgc::TzRule& zone, bool leaveDateNow) {
    const uint16_t before = gSrs.today();
    const int offsetMin = tgc::tzOffsetMinAt(zone, utcMs);
    const tgc::ClockSyncResult r = gSrs.syncWallClock(utcMs, zone);
    if (r.kind == tgc::ClockSync::Rejected) {
        Serial.printf("clock: rejected (%ld s, utc%+d min)\n", static_cast<long>(utcMs / 1000),
                      offsetMin);
        return false;
    }
    gWallClockSynced = true;
    // Persist a new anchor or day. A same-day sync (every page reload) only
    // moved the carry, which the idle save persists, so it does not write the
    // flash. As in goIdle(), a store that failed to load and has no answers is
    // not written over a damaged file; the next answer saves it.
    if (r.kind != tgc::ClockSync::Same && (gDirty || gSrsLoaded)) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    char date[16];
    formatDate(gSrs.today(), date, sizeof date);
    Serial.printf("clock: %s, day %u -> %u (%s), utc%+d min\n", clockSyncName(r.kind),
                  (unsigned)before, (unsigned)gSrs.today(), date, offsetMin);

    // The boot-time "how many days?" question: the real time answers it.
    // dateSyncPoll() may ask to stay on the screen for a moment: it takes the
    // radio session over for the update check before leaving for the quiz.
    if (gScreen == Screen::Date && gAfterDate == Screen::Quiz && leaveDateNow) {
        leaveDate();
        return true;
    }
    // A manual correction in progress restarts from the new (real) today.
    if (gScreen == Screen::Date) gDateSteps = 0;
    if (gSrs.today() != before && gPack->isOpen()) {
        // A new day: cards may have come due. An unanswered question (or the
        // done screen) gives way to the new queue; after an answer, the next
        // question comes from it. On the portal, Get decks or network test
        // screen the same goes for the quiz screen that the settings home's
        // "Continue" shows.
        gQueue.rebuild(*gPack, gSrs, gRng);
        auto refresh = [](Screen& s) {
            if ((s == Screen::Quiz && !gRevealed) || s == Screen::Done)
                s = newQuestion() ? Screen::Quiz : Screen::Done;
        };
        if (gScreen == Screen::Quiz || gScreen == Screen::Done)
            refresh(gScreen);
        else if (gScreen == Screen::Wifi || gScreen == Screen::GetDecks || gScreen == Screen::NetTest)
            refresh(gBeforeMenu);
    }
    if (!gScreenOff) drawScreen();
    return true;
}

// NTP over the home Wi-Fi, connected: the UTC time through the zone in use.
// Without a zone the study day is unknown, and guessing UTC could put it off by
// one, so the sync is skipped (logged). False when there was no usable time.
bool netTimeSync() {
    if (!gTzKnown) {
        Serial.printf("clock: no NTP sync, no time zone yet (open the portal page once, or "
                      "Settings > Device > Date > Zone)\n");
        return false;
    }
    int64_t utcMs = 0;
    return gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs) && applyWallClock(utcMs, gTz);
}

// Boot-time date question (setup()): with a home Wi-Fi and a time zone known,
// start joining; the date screen shows "Getting the date via <network>" (a tap
// skips it) and dateSyncPoll() takes it from there. Otherwise the stepper.
void dateSyncBegin() {
    if (!tgc::HomeWifi::hasCredentials() || !gTzKnown) {
        Serial.printf("clock: no network time at boot (no %s)\n",
                      tgc::HomeWifi::hasCredentials() ? "time zone" : "home Wi-Fi saved");
        return;
    }
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        gDateSync = DateSync::NoNetwork;
        Serial.printf("clock: no network time at boot (radio did not start)\n");
        return;
    }
    gDateSync = DateSync::Joining;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    Serial.printf("clock: getting the date via \"%s\"\n", gHome.ssid());
}

// The date sync has the home Wi-Fi up. If the day's update check is due, take
// the session over: read the catalog on the same connection and show the usual
// result screen, then finishUpdateCheck() goes to the quiz. The radio is turned
// off by updateCheckRead().
void startUpdateCheckFromDate() {
    gScreen = Screen::UpdateCheck;
    gUpdateStep = UpdateStep::Catalog;
    gUpdateStartedAt = millis();
    gUpdateFound = 0;
    gUpdateErr[0] = 0;
    gUpdateDoneAt = 0;
    gNetActivityAt = millis();
    setCpuFrequencyMhz(kCpuWifiMhz);
    drawScreen();       // "Reading catalog..."
    updateCheckRead();  // the join is already up: fetch, count, radio off
}

// loop() on the date screen while Joining. Once the join succeeds: NTP
// (blocking, at most 2 x kNtpTimeoutMs), then applyWallClock(). If the update
// check is due, the same radio session reads the catalog before leaving for the
// quiz; otherwise the radio is off and the quiz follows. On any failure the
// radio is off too and the stepper comes up with a status line that says to set
// the date by hand.
bool dateSyncPoll() {
    if (gDateSync != DateSync::Joining) return false;
    const tgc::HomeWifi::State st = gHome.poll();
    if (st == tgc::HomeWifi::State::Connecting) return false;
    const bool joined = st == tgc::HomeWifi::State::Connected;
    int64_t utcMs = 0;
    const bool got = joined && gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs);
    if (joined) gSwallowInput = true;  // presses made while blocked must not act now
    gDateSync = DateSync::None;
    if (got && applyWallClock(utcMs, gTz, false)) {
        if (updateCheckDue()) {
            startUpdateCheckFromDate();
            return true;
        }
        netRadioOff();
        leaveDate();
        return true;
    }
    netRadioOff();
    gDateSync = joined ? DateSync::NoTime : DateSync::NoNetwork;
    // The status is news: the join's time does not count against the reader.
    gNetActivityAt = millis();
    Serial.printf("clock: no network time at boot (%s), asking by hand\n",
                  joined ? "no NTP answer" : "could not join");
    if (!gScreenOff) drawScreen();
    return false;
}

// --- Settings > Device > Date: loop ------------------------------------------

void dateMenuNote(const char* text, uint16_t color) {
    snprintf(gDateMenuNote, sizeof gDateMenuNote, "%s", text ? text : "");
    gDateMenuNoteColor = color;
}

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

void openTzList() {
    gTzListView.setCount(tzListRows());
    gTzListView.reset(tzCurrentRow());
    gScreen = Screen::TzList;
    drawScreen();
}

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

void tzListLoop(int a, int b) {
    if (a == 1) {
        moveListFocus(gTzListView);
        drawScreen();
    } else if (a == 2) {
        applyTzChoice();
    }
    if (b && gScreen == Screen::TzList) openDateMenu(false);  // back, nothing changed
}

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

// --- Wi-Fi portal integration (roadmap phase 4) ----------------------------

void portalFillStats(tgc::PortalStats& out) {
    const tgc::Stats st = gSrs.stats();
    out.day = gSrs.today();
    formatDate(gSrs.today(), out.date, sizeof out.date);
    out.dateSynced = gSrs.hasWallClock();
    out.seen = st.seen;
    out.learned = st.learned;
    out.answers = st.answers;
    out.again = st.again;
    out.reps = st.reps;
    out.lapses = st.lapses;
    out.streakCurrent = st.streakCurrent;
    out.streakBest = st.streakBest;
    out.reviewMs = st.reviewMs;
    out.packCards = gPack->isOpen() ? gPack->cardCount() : 0;
    if (gPack->isOpen()) {
        const int slash = gCurrentPack.lastIndexOf('/');
        snprintf(out.loadedPack, sizeof out.loadedPack, "%s",
                 gCurrentPack.substring(slash >= 0 ? slash + 1 : 0).c_str());
    }
    // The per-kanji accuracy view: the worst cards, for the portal's table.
    tgc::Trouble rows[tgc::PortalStats::kTroubleMax];
    const size_t n = gSrs.trouble(rows, tgc::PortalStats::kTroubleMax);
    out.troubleCount = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        tgc::PortalStats::TroubleKanji& row = out.trouble[i];
        row.codepoint = rows[i].codepoint;
        row.reps = rows[i].reps;
        row.lapses = rows[i].lapses;
        tgc::utf8Encode(rows[i].codepoint, row.kanji);
    }
}

// --- Deleting progress (docs/srs.md section 2.1) ----------------------------

// Writes the state file through the safe path (temp + rename). False when it
// failed; the answers not yet saved then stay counted for the next flush.
bool saveProgress() {
    if (!gSrs.save(gSrsIo)) {
        Serial.printf("srs: save failed\n");
        return false;
    }
    gDirty = 0;
    return true;
}

// The code points of the pack at `path`, sorted and unique; none when it does
// not read (its kanji cannot be known, so nothing of it is forgotten).
std::vector<uint32_t> packCodepoints(const String& path) {
    std::vector<uint32_t> cps;
    tgc::FsPackSource src(LittleFS.open(path, "r"));
    if (src.ok()) tgc::readPackCodepoints(src, &cps);
    tgc::sortUnique(cps);
    return cps;
}

// The code points of every pack in /packs but `except`, sorted and unique.
std::vector<uint32_t> otherDecksCodepoints(const String& except) {
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == except) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    return all;
}

// The portal's POST /delete (WifiPortal::Hooks::onDeleteDeck): deletes pack
// `path` and the progress of its kanji that no other deck in /packs has, so a
// later download of it starts from scratch; N5's kanji keep theirs while the
// Jouyou deck is installed. The lifetime statistics stay (docs/srs.md 2.1).
// Order: the records are forgotten and the state file saved (temp + rename)
// *before* the pack is removed. A power cut in between leaves the deck
// installed without that progress (deleting it again finishes the job), never
// records of a deck that is gone, which a new download of it would pick up
// again. A failed save (a full filesystem: deleting a deck is how space is
// made) is tried again once the pack is gone. Then the folder is rescanned as
// after any other change: a deleted loaded deck gives way to the first one
// left, and the queue is rebuilt (portalPacksChanged()).
bool portalDeleteDeck(const char* path, uint32_t* forgotten) {
    *forgotten = 0;
    const String p(path);
    if (!tgc::fsExists(p)) return false;
    const std::vector<uint32_t> deck = packCodepoints(p);
    const std::vector<uint32_t> gone = tgc::forgetOnDelete(deck, otherDecksCodepoints(p));
    const size_t n = gSrs.forget(gone.data(), gone.size());
    const bool saved = n == 0 || saveProgress();
    const bool removed = LittleFS.remove(p);
    if (!saved) saveProgress();
    Serial.printf("delete %s: %s; progress of %u of its %u kanji forgotten, %u kanji left\n",
                  path, removed ? "ok" : "failed", (unsigned)n, (unsigned)deck.size(),
                  (unsigned)gSrs.count());
    portalPacksChanged();
    *forgotten = static_cast<uint32_t>(n);
    return removed;
}

// Writes an NVS string only when it changed: the page posts on every load.
void putPrefString(const char* key, const char* value) {
    char old[64] = "";
    if (gPrefs.isKey(key)) gPrefs.getString(key, old, sizeof old);
    if (!gPrefs.isKey(key) || std::strcmp(old, value) != 0) gPrefs.putString(key, value);
}

// Called by the portal with the browser's clock (POST /time on every page load):
// the time, the offset of the moment, and the zone as a POSIX rule and an IANA
// name (docs/srs.md section 5.1). The rule is used when it parses and gives
// that offset now; it becomes the phone's zone (NVS `tzr`, `tzn`, and the
// offset in `tz`), which replaces a zone chosen by hand. Without a usable rule
// (a page from before it): a zone in use that gives the same offset now is
// kept, choice by hand included; else the bare offset becomes the zone, as
// before the rule existed (a DST change then shifts the rollover until the
// next visit).
bool portalTime(int64_t utcMs, int16_t tzOffsetMin, const char* rule, const char* zone) {
    tgc::TzRule r;
    char ruleText[tgc::kTzRuleMaxLen + 1] = "";
    const bool haveRule = rule && *rule && tgc::parseTzRule(rule, &r) &&
                          tgc::tzOffsetMinAt(r, utcMs) == tzOffsetMin;
    if (rule && *rule && !haveRule)
        Serial.printf("clock: page rule \"%s\" ignored (does not parse or gives another offset "
                      "than %+d min)\n",
                      rule, (int)tzOffsetMin);
    if (haveRule) {
        snprintf(ruleText, sizeof ruleText, "%s", rule);
    } else {
        if (gTzKnown && tgc::tzOffsetMinAt(gTz, utcMs) == tzOffsetMin)
            return applyWallClock(utcMs, gTz);
        r = tgc::fixedTzRule(tzOffsetMin);
        tgc::formatFixedTzRule(tzOffsetMin, ruleText, sizeof ruleText);
    }
    // A broken clock or offset changes nothing, the zone included.
    if (!tgc::wallClockPlausible(utcMs, r)) return applyWallClock(utcMs, r);
    putPrefString(kPrefTzRule, ruleText);
    putPrefString(kPrefTzName, zone ? zone : "");
    if (!gPrefs.isKey(kPrefTz) || gPrefs.getShort(kPrefTz, 0) != tzOffsetMin)
        gPrefs.putShort(kPrefTz, tzOffsetMin);
    if (gPrefs.isKey(kPrefTzManual)) gPrefs.remove(kPrefTzManual);
    const bool changed = !gTzKnown || gTzManual || std::strcmp(gTzRuleText, ruleText) != 0 ||
                         std::strcmp(gTzName, zone ? zone : "") != 0;
    if (changed) loadTimeZone();  // logs the zone
    return applyWallClock(utcMs, gTz);
}

// The page's language: the screens' own ("en"/"es"), or none under Auto while
// no deck is loaded (the page then follows the browser, as Get decks follows
// the phone's language in that case).
const char* portalUiLang() {
    return (gUiLangPref == tgc::kUiLangAuto && !gPack->isOpen()) ? "" : uiLangCode();
}

// The page sends the user to the device screen: undo an idle dim (the time
// spent typing on the phone is not button input).
void undoDim() {
    if (gDimmed) {
        tgc::board::setBrightness(gBrightness);
        gDimmed = false;
    }
}

// Stops the portal and opens Get decks in its place: the boot-time upload
// screen's hold, and the portal page's "Get decks on the device screen" (the
// portal's onGetDecks hook, called from poll() once its response is out) from
// the boot-time portal or the settings one. Get decks then leaves as it always
// does: back to the boot-time screen (or into the quiz once a deck is loaded),
// else the deck list. In home mode the station stays up and Get decks uses it.
void wifiToGetDecks(const char* phoneLang) {
    // Only an Auto UI with no deck loaded has no language of its own (it shows
    // English); then the phone's language orders the deck list instead.
    gGetDecksLangHint = phoneLang ? phoneLang : "";
    gPortal.end();
    ++gWifiEpoch;
    // openGetDecks() raises it again while the radio is on.
    if (!gHome.active()) setCpuFrequencyMhz(kCpuMhz);
    QLOG("W wifi: stopped for Get decks\n");
    undoDim();
    openGetDecks(gWifiBootUpload);
}

// "Switch to my Wi-Fi" / "Reconnect" on the page (the portal's onSwitchToHome
// hook, from poll() once its response is out): the portal again, in home mode.
void portalSwitchToHome() {
    QLOG("W wifi: page asked for home mode\n");
    undoDim();
    openPortal(PortalMode::Home);
}

// Home mode: a deck download the page asked for (the portal's onDeckDownload
// hook, blocking, from poll()). The Wi-Fi screen shows the progress.
tgc::DeckDownloadResult portalDeckDownload(const tgc::CatalogEntry& e, bool update) {
    ++gWifiEpoch;
    QLOG("W wifi: page download %s (%s)\n", e.name.c_str(), update ? "update" : "new");
    return downloadCatalogDeck(e);
}

// The portal's POST /progress (WifiPortal::Hooks::onRestoreProgress): replace
// the SRS state with the portal's validated temp file. Order matters: the
// current state is saved and then kept as /state/srs.before-restore.bin, so a
// failed rename or reload can put it back -- a restore never loses progress
// (docs/srs.md section 4). The page posts its clock right after the redirect,
// which aligns the restored day index with this device's date (section 5.1).
bool portalRestoreProgress(const char* tmpPath) {
    if (!tmpPath || std::strcmp(tmpPath, tgc::WifiPortal::kProgressTmp) != 0) return false;
    const String tmp = tgc::WifiPortal::kProgressTmp;
    const String state = tgc::FsSrsFile::kPath;
    const String safety = tgc::FsSrsFile::kBeforeRestore;

    // Persist the answers not yet flushed before they can be displaced.
    if (!saveProgress()) return false;

    // Keep the live file. An old safety copy is replaced: it is this file's.
    if (tgc::fsExists(safety) && !LittleFS.remove(safety)) {
        Serial.printf("srs: restore: cannot remove the old safety copy\n");
        return false;
    }
    if (!LittleFS.rename(state, safety)) {
        Serial.printf("srs: restore: cannot keep the current state\n");
        return false;
    }
    if (!LittleFS.rename(tmp, state)) {
        Serial.printf("srs: restore: rename failed, putting the previous state back\n");
        LittleFS.rename(safety, state);
        return false;
    }

    if (!gSrs.begin(gSrsIo)) {
        // The portal validated the bytes, so this should not happen; if it
        // does, put the previous file back rather than leave an empty store.
        Serial.printf("srs: restore: reload failed, putting the previous state back\n");
        tgc::fsRemoveIfExists(state);
        LittleFS.rename(safety, state);
        gSrsLoaded = gSrs.begin(gSrsIo);
        gSrs.beginSession();
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        return false;
    }
    gSrs.beginSession();
    gSrsLoaded = true;
    gBootDay = gSrs.today();
    gExtraNew = 0;
    gDirty = 0;
    // The old question belongs to the old progress: build a fresh one for the
    // screen the settings home's "Continue" returns to, as after a pack change.
    if (!gPack->isOpen()) {
        gQueue.rebuild(*gPack, gSrs, gRng);
        gBeforeMenu = Screen::Decks;
    } else {
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        gBeforeMenu = newQuestion() ? Screen::Quiz : Screen::Done;
    }
    Serial.printf("srs: restored from the portal: day %u, records %u\n", (unsigned)gSrs.today(),
                  (unsigned)gSrs.count());
    return true;
}

// The portal's home page has just read the catalog and compared every entry
// with /packs: the same read the Updates row does, so refresh the badge from it
// (docs/deck-updates.md, "any Wi-Fi session").
void portalCatalog(const std::vector<tgc::CatalogEntry>& entries,
                   const std::vector<tgc::DeckState>& states) {
    setUpdatesFromCatalog(entries, &states);
}

const tgc::WifiPortal::Hooks kPortalHooks = {
    portalFillStats,      portalPacksChanged, portalTime,          wifiToGetDecks,
    portalUiLang,         portalSwitchToHome, portalDeckDownload,  portalDeleteDeck,
    portalRestoreProgress, portalCatalog,
};

// The mode the portal opens in: home once a network is saved, else setup.
PortalMode defaultPortalMode() {
    return tgc::HomeWifi::hasCredentials() ? PortalMode::Home : PortalMode::Setup;
}

// The Wi-Fi screen's failed state: a title (why) and a detail (the network).
void wifiFailed(const char* title, const char* detail) {
    gWifiStep = WifiStep::Failed;
    snprintf(gNetErrTitle, sizeof gNetErrTitle, "%s", title);
    snprintf(gNetErrDetail, sizeof gNetErrDetail, "%s", detail ? detail : "");
    QLOG("W wifi: failed: %s | %s\n", gNetErrTitle, gNetErrDetail);
}

// Stops the portal and the radio, whichever mode ran; CPU back to normal.
void stopPortal() {
    if (gPortal.active()) {
        gPortal.end();
        QLOG("W wifi: stopped\n");
    }
    netRadioOff();
    setCpuFrequencyMhz(kCpuMhz);
}

// Opens the Wi-Fi screen in `mode` (from the Wi-Fi submenu, the page's switch
// to home mode, or at boot when there is no pack yet). Home mode joins the
// saved network first (wifiLoop() polls it) and then serves the page on it;
// setup mode starts the access point at once. Whatever ran before stops first.
// The boot-time upload mode is decided by the caller (setup() sets
// gWifiBootUpload); opening the portal from the settings never makes the side
// button inert.
void openPortal(PortalMode mode) {
    if (gPortal.active()) gPortal.end();
    netRadioOff();
    ++gWifiEpoch;
    gPortalMode = mode;
    gScreen = Screen::Wifi;
    gQrPage = 0;
    gWifiNote[0] = 0;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (mode == PortalMode::Home) {
        if (!gHome.begin()) {
            netRadioOff();
            setCpuFrequencyMhz(kCpuMhz);
            const bool none = gHome.failure() == tgc::HomeWifi::Failure::NoCredentials;
            wifiFailed(none ? ui().netNoWifi : ui().netNoConnect, none ? "" : gHome.ssid());
        } else {
            gWifiStep = WifiStep::Joining;
            QLOG("W wifi: joining %s for the portal\n", gHome.ssid());
        }
        drawScreen();
        return;
    }
    if (!gPortal.begin(kPortalHooks)) {
        setCpuFrequencyMhz(kCpuMhz);
        wifiFailed(gPortal.lastMessage(), "");
    } else {
        gWifiStep = WifiStep::Serving;
        QLOG("W wifi: setup mode ssid=%s url=%s\n", gPortal.ssid(), gPortal.url());
    }
    drawScreen();
}

// Builds the quiz from the pack the portal loaded and switches to it.
void startQuizFromPortal() {
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// Leaves the Wi-Fi screen (radio off): for the Wi-Fi submenu, or -- on the
// boot-time screen once the home-mode page brought a deck -- into the quiz. On
// the boot-time screen without a deck there is nothing to return to, so the
// side button does nothing there.
void closeWifi() {
    if (gWifiBootUpload && !gPack->isOpen()) return;
    stopPortal();
    if (gWifiBootUpload) {
        gWifiBootUpload = false;
        startQuizFromPortal();
        return;
    }
    openWifiMenu(false);
}

// On the boot-time upload screen in setup mode, move on to the quiz once a pack
// is loaded (an upload or a URL download). Home mode stays, so the page can
// fetch more decks; its side button then starts studying (closeWifi()). True
// when it took over the screen.
bool wifiUploadReadyToQuiz() {
    if (!gWifiBootUpload || !gPack->isOpen() || gPortalMode != PortalMode::Setup) return false;
    stopPortal();
    gWifiBootUpload = false;
    startQuizFromPortal();
    return true;
}

// loop() while on the Wi-Fi screen.
void wifiLoop(int a, int b) {
    if (gWifiStep == WifiStep::Joining) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            // The network is up anyway: take the real date from NTP first, as
            // Get decks does (blocking, at most 2 x kNtpTimeoutMs).
            netSyncClock();
            gSwallowInput = true;  // presses made while blocked must not act now
            gNetActivityAt = millis();
            if (gPortal.beginHome(kPortalHooks, gHome.ssid())) {
                gWifiStep = WifiStep::Serving;
                QLOG("W wifi: home mode on %s at %s\n", gHome.ssid(), gPortal.url());
            } else {
                netRadioOff();
                wifiFailed(ui().netNoConnect, gHome.ssid());
            }
            drawScreen();
            return;
        }
        if (st == tgc::HomeWifi::State::Failed) {
            setCpuFrequencyMhz(kCpuMhz);  // the radio is off already
            if (gWifiBootUpload) {
                // Nothing to study yet: the device's own network instead, and
                // why on its screen.
                char note[sizeof gWifiNote];
                snprintf(note, sizeof note, ui().wifiFallbackFmt, gHome.ssid(), joinFailureText());
                openPortal(PortalMode::Setup);
                if (gWifiStep == WifiStep::Serving) {
                    snprintf(gWifiNote, sizeof gWifiNote, "%s", note);
                    drawScreen();
                }
                return;
            }
            wifiFailed(joinFailureText(), gHome.ssid());
            drawScreen();
            return;
        }
        if (a == 2) openPortal(PortalMode::Setup);
        else if (b) closeWifi();  // cancel the join
        return;
    }
    if (gWifiStep == WifiStep::Failed) {
        if (a == 1) openPortal(gPortalMode);  // try again
        else if (a == 2) openPortal(PortalMode::Setup);
        else if (b) closeWifi();
        return;
    }
    // Serving. The page may leave this screen (Get decks), switch the mode or
    // run a download inside poll(); the input read before it is then stale.
    const uint32_t epoch = gWifiEpoch;
    gPortal.poll();
    if (gScreen != Screen::Wifi || epoch != gWifiEpoch) return;
    if (gPortal.consumeDirty()) drawScreen();
    if (wifiUploadReadyToQuiz()) return;
    if (b) {
        closeWifi();
    } else if (a == 1 && gPortalMode == PortalMode::Setup) {
        gQrPage ^= 1;  // the other QR code: join the AP / open the page
        drawScreen();
    } else if (a == 2 && gWifiBootUpload && !gPack->isOpen() && tgc::HomeWifi::hasCredentials()) {
        // Boot-time screen with a saved home Wi-Fi: Get decks on the device.
        wifiToGetDecks(nullptr);
    }
}

// --- Settings > Study > Delete progress (docs/srs.md section 2.1) ----------
// A two-row list -- "This deck" (the loaded deck's kanji, shared ones too) and
// "Everything" (every record and the lifetime statistics) -- whose hold opens
// a confirmation: what goes, in numbers, over one row that only a hold
// confirms (the hold-progress fill shows on it); the side button cancels. Then
// back to the Study page with a note; the queue, the summary card and every
// count follow the store.
tgc::ListView gResetList;
tgc::ListView gResetConfirmList;  // the confirmation's one row
char gResetNote[32] = "";         // the list's note ("Nothing to delete")
bool gResetAll = false;           // the confirmation is for Everything
uint32_t gResetConfirmAt = 0;     // millis() when it opened (keyAction()'s Enter guard)
std::vector<uint32_t> gResetCps;  // This deck: its kanji, sorted and unique
tgc::DeckReset gResetPreview;     // This deck: its kanji with progress, shared ones
int gResetSharedDecks = 0;        // other installed decks that share some of them
String gResetSharedName;          // the one such deck's name, when only one
constexpr int kResetRows = 2;

void openReset(bool fresh) {
    if (fresh) {
        gResetList.setCount(kResetRows);
        gResetList.reset(0);
    }
    gResetNote[0] = 0;
    gScreen = Screen::Reset;
    drawScreen();
}

void drawReset() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    const bool wide = wideRows();
    gResetList.setCount(kResetRows);
    layoutList(gResetList, kHeaderH, H - kHintH - (gResetNote[0] ? kNoteH : 0),
               wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gResetList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().resetRows[i];
        row.focused = f;
        const char* second = i == 1 ? ui().resetAllCaption
                                    : (gPack->isOpen() ? gPack->deckName() : ui().resetNoDeck);
        if (wide) {
            row.value = second;
            row.keepLabel = true;
        } else {
            row.caption = second;
        }
        if (i == 0 && !gPack->isOpen()) row.muted = true;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gResetNote, tgc::theme::kWarn);
    menuHints(d, ui().hintChoose);
    presentList(gResetList);
}

// The other installed decks' share of the loaded deck's kanji with progress:
// how many of them (over all those decks) and, when a single deck shares any,
// its name.
void resetSharedPreview() {
    gResetSharedDecks = 0;
    gResetSharedName = "";
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == gCurrentPack) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        if (tgc::deckResetPreview(gSrs, gResetCps, cps).shared == 0) continue;
        if (++gResetSharedDecks == 1) {
            tgc::PackInfo info;
            tgc::FsPackSource src(LittleFS.open(p, "r"));
            const bool named = src.ok() && tgc::peekPackInfo(src, &info) == tgc::PackError::Ok &&
                               info.name[0];
            gResetSharedName = named ? String(info.name) : packFileStem(p);
        }
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    gResetPreview = tgc::deckResetPreview(gSrs, gResetCps, all);
}

// A hold on a row: the confirmation, or a note when there is nothing to delete.
void openResetConfirm() {
    gResetAll = gResetList.focus() == 1;
    gResetNote[0] = 0;
    if (gResetAll) {
        if (gSrs.count() == 0 && gSrs.answeredTotal() == 0 && gSrs.reviewMs() == 0 &&
            !gSrs.hasStudied())
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    } else if (!gPack->isOpen()) {
        snprintf(gResetNote, sizeof gResetNote, "%s", ui().summaryNoDeck);
    } else {
        gResetCps = packCodepoints(gCurrentPack);
        resetSharedPreview();
        if (gResetPreview.records == 0)
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    }
    if (gResetNote[0]) {
        drawScreen();
        return;
    }
    gResetConfirmList.setCount(1);
    gResetConfirmList.reset(0);
    gScreen = Screen::ResetConfirm;
    gResetConfirmAt = millis();
    drawScreen();
}

void drawResetConfirm() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    // What goes: the deck (or "Everything") in the label face, then in numbers.
    const char* what = gResetAll ? ui().resetRows[1] : gPack->deckName();
    gRender.drawLabel(d, what, kGutter, kHeaderH + 4, W - 2 * kGutter, 20, kWarn, kBlack,
                      tgc::Renderer::kTextUi, tgc::Align::Centre);
    const int rowH = layout().row1H;
    const int rowsTop = H - kHintH - rowH - 2 * kMarkerBand;
    const int textTop = kHeaderH + 28;
    char text[200];
    if (gResetAll)
        snprintf(text, sizeof text, ui().resetAllFmt, (unsigned)gSrs.count());
    else
        snprintf(text, sizeof text, ui().resetDeckFmt, (unsigned)gResetPreview.records);
    const int used = drawCaptionLines(d, text, kGutter, textTop, W - 2 * kGutter,
                                      std::max(0, (rowsTop - textTop) / 10), kWhite);
    // Kanji other decks share go too: said in warn colour, under a small gap.
    const int sharedTop = textTop + 10 * used + 6;
    if (!gResetAll && gResetPreview.shared > 0 && rowsTop - sharedTop >= 10) {
        if (gResetSharedDecks == 1)
            snprintf(text, sizeof text, ui().resetSharedOneFmt, gResetSharedName.c_str(),
                     (unsigned)gResetPreview.shared);
        else
            snprintf(text, sizeof text, ui().resetSharedManyFmt, (unsigned)gResetPreview.shared);
        drawCaptionLines(d, text, kGutter, sharedTop, W - 2 * kGutter, (rowsTop - sharedTop) / 10,
                         kWarn);
    }
    gResetConfirmList.setCount(1);
    layoutList(gResetConfirmList, rowsTop, H - kHintH, rowH);
    wg::drawList(d, gResetConfirmList, listX(), listW(),
                 [&](int, int x, int y, int w, int h, bool f) {
                     wg::Row row;
                     row.label = ui().resetConfirmRow;
                     row.focused = f;
                     wg::drawRow(d, gRender, x, y, w, h, row);
                 });
    wg::Hints hints;
    hints.hold = ui().hintDelete;
    hints.side = &tgc::kIconBack;
    hints.sideWord = ui().hintCancel;
    drawHints(d, hints);
    present();
}

// The confirmation's hold: forget, save (temp + rename), and let the queue and
// the screen the settings return to follow, as after a deck change: the
// question kept for them may be a card whose progress just went.
void applyReset() {
    size_t n = 0;
    if (gResetAll) {
        n = gSrs.resetProgress();
        snprintf(gMenuNote, sizeof gMenuNote, "%s", ui().resetAllDone);
    } else {
        n = gSrs.forget(gResetCps.data(), gResetCps.size());
        snprintf(gMenuNote, sizeof gMenuNote, ui().resetDoneFmt, (unsigned)n);
    }
    saveProgress();
    Serial.printf("progress: %s: %u kanji forgotten, %u left, %u answers\n",
                  gResetAll ? "everything" : gCurrentPack.c_str(), (unsigned)n,
                  (unsigned)gSrs.count(), (unsigned)gSrs.answeredTotal());
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gPack->isOpen()) gBeforeMenu = newQuestion() ? Screen::Quiz : Screen::Done;
    gResetCps.clear();
    gResetCps.shrink_to_fit();
    gMenuPage = st::Page::Study;
    gScreen = Screen::Menu;
    drawScreen();
}

void resetLoop(int a, int b) {
    if (a == 1) {
        const bool note = gResetNote[0] != 0;
        gResetNote[0] = 0;
        moveListFocus(gResetList);
        if (note) dropPartialPush();  // the note goes: the whole frame
        drawScreen();
    } else if (a == 2) {
        openResetConfirm();
    }
    if (b && gScreen == Screen::Reset) backToMenu();  // the Study page
}

// Only a hold deletes; a tap does nothing, the side button cancels.
void resetConfirmLoop(int a, int b) {
    if (a == 2) {
        applyReset();
    } else if (b) {
        gScreen = Screen::Reset;
        drawScreen();
    }
}

// --- Deck update checks (docs/deck-updates.md) ------------------------------

// Epoch day of the SRS day clock, or 0 while no real clock has been applied.
uint32_t currentEpochDay() {
    return gSrs.hasWallClock() ? static_cast<uint32_t>(gSrs.epochDayOf(gSrs.today())) : 0;
}

void saveUpdateState() {
    gPrefs.putUChar(kPrefUpdateCount, gUpdateCount);
    gPrefs.putUChar(kPrefUpdateKnown, gUpdateKnown ? 1 : 0);
    if (gUpdateDay) gPrefs.putUInt(kPrefUpdateDay, gUpdateDay);
}

// Counts catalog records whose installed file is a `DeckState::Update`: another
// size, or now a same-size rebuild with another SHA-256 (deckStateOnDisk). New
// catalog decks are not updates, and packs the catalog does not list are
// ignored. `stats` accumulates the hashing cost for the caller's one log line.
uint8_t countUpdates(const std::vector<tgc::CatalogEntry>& entries, HashStats* stats) {
    uint8_t n = 0;
    for (const tgc::CatalogEntry& e : entries)
        if (deckStateOnDisk(e, stats) == tgc::DeckState::Update) ++n;
    return n;
}

// Stores the count (and the check day, when the clock is real) after a catalog
// read, writing NVS only when something changed. `states` optionally carries the
// states netRefreshStates() just computed for the same entries, so Get decks
// does not hash every installed pack a second time.
void setUpdatesFromCatalog(const std::vector<tgc::CatalogEntry>& entries,
                           const std::vector<tgc::DeckState>* states) {
    uint8_t n = 0;
    if (states && states->size() == entries.size()) {
        for (tgc::DeckState s : *states)
            if (s == tgc::DeckState::Update) ++n;
        Serial.printf("[update] %u update(s) from a catalog read\n", (unsigned)n);
    } else {
        HashStats hs;
        n = countUpdates(entries, &hs);
        if (hs.files)
            Serial.printf("[update] hashed %u pack(s) in %u ms\n", (unsigned)hs.files,
                          (unsigned)hs.ms);
    }
    const uint32_t day = currentEpochDay();
    const bool known = gUpdateKnown;
    gUpdateKnown = true;
    if (n == gUpdateCount && known && (day == 0 || day == gUpdateDay)) return;
    gUpdateCount = n;
    if (day) gUpdateDay = day;
    saveUpdateState();
}

// The automatic check's policy (docs/deck-updates.md: once a day on external
// power; on battery, once a week and only from 50 %). The boot-time date sync
// piggybacks on its join when due; a normal boot with no other Wi-Fi session
// uses the standalone check. Any catalog read (Get decks, portal, network test)
// refreshes the day, so this only spends a dedicated join when nothing else
// has.
bool updateCheckDue() {
    if (gPacks.empty() || !tgc::HomeWifi::hasCredentials() || !gSrs.hasWallClock()) return false;
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    return tgc::deckUpdateCheckDue(gUpdateDay, currentEpochDay(), external,
                                   tgc::board::batteryPercent());
}

// Leaves the check (result, failure or skip): the boot flow continues with the
// first question, or with the done screen when nothing is due.
void finishUpdateCheck() {
    netRadioOff();
    gSwallowInput = true;
    if (!newQuestion())
        showDone();
    else {
        gScreen = Screen::Quiz;
        drawScreen();
    }
}

// A press during the join gives up without an error; the day is recorded so
// the device does not ask again until tomorrow (or next week on battery).
void updateCheckCancel() {
    netRadioOff();
    const uint32_t day = currentEpochDay();
    if (day) {
        gUpdateDay = day;
        saveUpdateState();
    }
    finishUpdateCheck();
}

// Connected: read the catalog (blocking), count the updates, radio off. The
// result stays on screen for kUpdateResultMs, or until a press.
void updateCheckRead() {
    const String url = tgc::HomeWifi::catalogUrl();
    tgc::CatalogFetch c = tgc::fetchCatalog(url);
    netRadioOff();
    gSwallowInput = true;
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        snprintf(gUpdateErr, sizeof gUpdateErr, "%s", title);
        gUpdateStep = UpdateStep::Failed;
        gUpdateDoneAt = millis();
        const uint32_t day = currentEpochDay();
        if (day) {
            gUpdateDay = day;
            saveUpdateState();
        }
        drawScreen();
        return;
    }
    setUpdatesFromCatalog(c.parse.entries);
    gUpdateFound = gUpdateCount;
    gUpdateStep = UpdateStep::Done;
    gUpdateDoneAt = millis();
    drawScreen();
}

void openUpdateCheck() {
    gScreen = Screen::UpdateCheck;
    gUpdateStep = UpdateStep::Joining;
    gUpdateStartedAt = millis();
    gUpdateFound = 0;
    gUpdateErr[0] = 0;
    gUpdateDoneAt = 0;
    gNetActivityAt = millis();
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        snprintf(gUpdateErr, sizeof gUpdateErr, "%s", ui().netNoConnect);
        netRadioOff();
        gUpdateStep = UpdateStep::Failed;
        gUpdateDoneAt = millis();
        const uint32_t day = currentEpochDay();
        if (day) {
            gUpdateDay = day;
            saveUpdateState();
        }
    }
    drawScreen();
}

// Called from loop(): the join state machine and the result's timer.
void updateCheckTick(uint32_t now) {
    if (gScreen != Screen::UpdateCheck) return;
    if (gUpdateStep == UpdateStep::Joining) {
        gNetActivityAt = now;
        if (now - gUpdateStartedAt >= kUpdateJoinMs) {
            snprintf(gUpdateErr, sizeof gUpdateErr, "%s", joinFailureText());
            netRadioOff();
            gUpdateStep = UpdateStep::Failed;
            gUpdateDoneAt = now;
            const uint32_t day = currentEpochDay();
            if (day) {
                gUpdateDay = day;
                saveUpdateState();
            }
            drawScreen();
            return;
        }
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            gUpdateStep = UpdateStep::Catalog;
            drawScreen();
            updateCheckRead();
        } else if (st == tgc::HomeWifi::State::Failed) {
            snprintf(gUpdateErr, sizeof gUpdateErr, "%s", joinFailureText());
            netRadioOff();
            gUpdateStep = UpdateStep::Failed;
            gUpdateDoneAt = now;
            const uint32_t day = currentEpochDay();
            if (day) {
                gUpdateDay = day;
                saveUpdateState();
            }
            drawScreen();
        }
        return;
    }
    if (gUpdateStep == UpdateStep::Done || gUpdateStep == UpdateStep::Failed) {
        // millis(), not `now`: the date-sync piggyback calls updateCheckRead()
        // from the input path, and the hash blocks for seconds. `now` was read
        // before that, so it would be older than gUpdateDoneAt and the result
        // would vanish at once.
        if (millis() - gUpdateDoneAt >= kUpdateResultMs) finishUpdateCheck();
    }
}

void updateCheckInput(int a, int b) {
    if (!a && !b) return;
    if (gUpdateStep == UpdateStep::Joining || gUpdateStep == UpdateStep::Catalog)
        updateCheckCancel();
    else
        finishUpdateCheck();
}

// The boot check's screen: the join in progress, then the result (or why it
// failed). A tap skips the join or continues from the result.
void drawUpdateCheck() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().updateTitle, nullptr, gHome.active());
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
    if (gUpdateStep == UpdateStep::Joining || gUpdateStep == UpdateStep::Catalog) {
        const char* line =
            gUpdateStep == UpdateStep::Joining ? ui().updateChecking : ui().netCatalog;
        gRender.drawCentred(d, line, 4, mid - 34, W - 8, 40, 2, kText, kBg,
                            tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        wg::Hints h;
        h.tap = ui().updateSkip;
        drawHints(d, h);
    } else {
        char line[64];
        if (gUpdateStep == UpdateStep::Done) {
            if (gUpdateFound == 1)
                snprintf(line, sizeof line, "%s", ui().updateFoundOne);
            else if (gUpdateFound > 1)
                snprintf(line, sizeof line, ui().updateFoundFmt, (unsigned)gUpdateFound);
            else
                snprintf(line, sizeof line, "%s", ui().updateAllCurrent);
            gRender.drawCentred(d, line, 4, mid - 20, W - 8, 40, 2,
                                gUpdateFound ? kAccentHi : kOkHi, kBg,
                                tgc::Renderer::kTextUi);
            if (gUpdateFound)
                drawCaptionLines(d, ui().updateWhere, 4, mid + 22, W - 8, 2, kTextDim);
        } else {
            gRender.drawCentred(d, gUpdateErr, 4, mid - 20, W - 8, 40, 2, kWarn, kBg,
                                tgc::Renderer::kTextUi);
        }
        wg::Hints h;
        h.tap = ui().updateContinue;
        drawHints(d, h);
    }
    present();
}

// --- Settings > Wi-Fi --------------------------------------------------------

void wifiMenuNote(const char* text, uint16_t color) {
    snprintf(gWifiMenuNote, sizeof gWifiMenuNote, "%s", text ? text : "");
    gWifiMenuNoteColor = color;
}

// Opens the Wi-Fi submenu: `fresh` from the settings home (first row, no note),
// else back from one of its screens (selection and note kept).
void openWifiMenu(bool fresh) {
    if (fresh) {
        gWifiMenuList.setCount(kWifiMenuRows);
        gWifiMenuList.reset(0);
        gWifiMenuNote[0] = 0;
    }
    gWifiMenuSsid = tgc::HomeWifi::savedSsid();
    gScreen = Screen::WifiMenu;
    drawScreen();
}

void activateWifiMenu() {
    switch (gWifiMenuList.focus()) {
        case 0:  // Web portal: home mode once a network is saved, else setup mode
            openPortal(defaultPortalMode());
            break;
        case 1:  // Home network: the connection test
            openNetTest();
            break;
        case 2:  // Setup mode: the access point, e.g. to save another network
            openPortal(PortalMode::Setup);
            break;
        case 3:  // Forget network: confirmed on a picker (Cancel first)
            if (gWifiMenuSsid.isEmpty()) {
                wifiMenuNote(ui().wifiNoNetwork, tgc::theme::kWarn);
                drawScreen();
            } else {
                openChoice(Choice::ForgetWifi);
            }
            break;
    }
}

void wifiMenuLoop(int a, int b) {
    if (a == 1) {
        gWifiMenuList.next();
        gWifiMenuNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        activateWifiMenu();
    }
    if (b && gScreen == Screen::WifiMenu) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

// Settings > Wi-Fi > Home network: joins the saved network (radio on, CPU at
// kCpuWifiMhz); netTestLoop() takes it from there.
void openNetTest() {
    gScreen = Screen::NetTest;
    gTestIp = "";
    gTestRssi = 0;
    gTestTime = -1;
    gTestCatalog[0] = 0;
    gTestFail[0] = 0;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    if (!tgc::HomeWifi::hasCredentials()) {
        gTestStep = NetTestStep::NoNetwork;
        drawScreen();
        return;
    }
    netRadioOff();
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        setCpuFrequencyMhz(kCpuMhz);
        gTestStep = NetTestStep::Failed;
        snprintf(gTestFail, sizeof gTestFail, "%s", ui().netNoConnect);
        drawScreen();
        return;
    }
    gTestStep = NetTestStep::Joining;
    QLOG("N test: joining %s\n", gHome.ssid());
    drawScreen();
}

// Connected: the network time (as every join does, when the offset is saved)
// and the catalog, the end-to-end check that the catalog host is reachable and
// answers with a catalog. Blocking; the radio is off again afterwards.
void netTestCheck() {
    gTestIp = gHome.localIp();
    gTestRssi = gHome.rssi();
    gTestStep = NetTestStep::Checking;
    drawScreen();
    if (gTzKnown) {
        int64_t utcMs = 0;
        gTestTime = gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs) ? 1 : 0;
        if (gTestTime) applyWallClock(utcMs, gTz);
    }
    const String url = tgc::HomeWifi::catalogUrl();
    const tgc::CatalogFetch c = tgc::fetchCatalog(url);
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        // A 404 says more as "no catalog published at this URL yet".
        const bool notPublished =
            c.fetch.status == tgc::FetchStatus::Http && c.fetch.httpCode == 404;
        snprintf(gTestCatalog, sizeof gTestCatalog, "%s", notPublished ? ui().netNotPublished : title);
        gTestCatalogColor = tgc::theme::kWarn;
    } else {
        snprintf(gTestCatalog, sizeof gTestCatalog, ui().testCatalogFmt,
                 (unsigned)c.parse.entries.size());
        gTestCatalogColor = tgc::theme::kOkHi;
        // A user-initiated read: refresh the update count too.
        setUpdatesFromCatalog(c.parse.entries);
    }
    Serial.printf("[net] test: \"%s\" ip %s rssi %d dBm, catalog %s, time %d\n", gHome.ssid(),
                  gTestIp.c_str(), gTestRssi, gTestCatalog, gTestTime);
    netRadioOff();
    gSwallowInput = true;  // presses made while blocked must not act now
    gNetActivityAt = millis();
    gTestStep = NetTestStep::Done;
}

void netTestLoop(int a, int b) {
    if (b) {  // back to the Wi-Fi submenu (cancels a join in progress)
        netRadioOff();
        openWifiMenu(false);
        return;
    }
    if (gTestStep == NetTestStep::Joining) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            netTestCheck();
            drawScreen();
        } else if (st == tgc::HomeWifi::State::Failed) {
            setCpuFrequencyMhz(kCpuMhz);  // the radio is off already
            snprintf(gTestFail, sizeof gTestFail, "%s", joinFailureText());
            gTestStep = NetTestStep::Failed;
            gNetActivityAt = millis();
            drawScreen();
        }
        return;
    }
    if (a == 2) openNetTest();  // test again
}

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

#if defined(TGC_REMOTE)
// Serial remote for tools/screenshots (envs sticks3-shots, tdisplay-s3-shots;
// never a release).
// One byte a command over USB serial, read by loop():
//   'a' / 'b'  tap the front / side button (down for 80 ms)
//   'A' / 'B'  hold it (down until the hold fires, then up)
//   's'        print the back buffer, which is what the panel shows: "#shot W H
//              <on|dim|dark>" (the backlight; dark: the last frame drawn), one
//              "#r Y <hex>" line a row (RGB565 as stored: big-endian, four hex
//              digits a pixel), "#end"
//   'w'        wake: counts as activity and lights a dim or dark panel, no press
// Test hooks:
//   'd'        the boot-time date question as after a power cut: "Getting the
//              date via <ssid>..." first, then the stepper (with the reason when
//              the network failed). It writes nothing itself: a hold on the
//              stepper confirms (saved), a side hold cancels, and a network time
//              that answers is applied as by every sync (a same-day one writes
//              nothing)
//   'f'        the next home Wi-Fi join is pinned to an access point that does
//              not exist: the pinned step fails, any access point after 25 s
//   'x'        the next join is pinned there for good: it fails after 40 s ("No
//              network: set it by hand" on the date question)
// Keys (a board with a keyboard, cardputer-adv-shots; two bytes, 'k' and the
// key): 'u' 'd' 'l' 'r' the arrows, 'e' Enter, 'x' Esc, 'm' the menu key (Tab),
// 'b' Backspace (del); 'U' 'D' 'L' 'R' hold an arrow for kRemoteKeyHoldMs (its
// auto-repeat). A key goes down, and up 80 ms later, through the same path as
// the keyboard's (keyTurn()). An unknown key is ignored.
// A press is a virtual button that goes through Key like a real one: it counts
// as activity, a dim panel brightens and acts, a dark one only wakes. While a
// virtual button or key is down the next bytes wait, so a burst runs in order.
struct RemoteKey {
    uint32_t upAt = 0;     // millis() when a tap comes up; 0: up
    bool holding = false;  // a hold that has not fired yet
};
RemoteKey gRemote[2];

// The remote's key: down (`sent` once delivered), up at `upAt`.
constexpr uint32_t kRemoteKeyTapMs = 80;
constexpr uint32_t kRemoteKeyHoldMs = 1000;
struct RemoteKeyPress {
    tgc::Key key = tgc::Key::None;
    bool sent = false;
    uint32_t upAt = 0;
};
RemoteKeyPress gRemoteKey;
bool gRemoteKeyPrefix = false;  // a 'k' came: the next byte names the key

bool remoteBusy() {
    return gRemote[0].upAt || gRemote[0].holding || gRemote[1].upAt || gRemote[1].holding ||
           gRemoteKey.key != tgc::Key::None;
}

// The key a 'k' command names; `held`: an upper-case arrow.
tgc::Key remoteKeyFor(int c, bool* held) {
    using tgc::Key;
    *held = c == 'U' || c == 'D' || c == 'L' || c == 'R';
    switch (c) {
        case 'u': case 'U': return Key::Up;
        case 'd': case 'D': return Key::Down;
        case 'l': case 'L': return Key::Left;
        case 'r': case 'R': return Key::Right;
        case 'e': return Key::Enter;
        case 'x': return Key::Esc;
        case 'm': return Key::Tab;
        case 'b': return Key::Backspace;
        default: return Key::None;
    }
}

// keyTurn()'s source after the board's: the remote key's press, then (once
// its time is up) its release.
bool remoteNextKey(tgc::KeyEvent* ev, uint32_t now) {
    RemoteKeyPress& k = gRemoteKey;
    if (k.key == tgc::Key::None) return false;
    if (!k.sent) {
        k.sent = true;
        *ev = tgc::KeyEvent{k.key, true};
        return true;
    }
    if (static_cast<int32_t>(now - k.upAt) < 0) return false;
    *ev = tgc::KeyEvent{k.key, false};
    k = RemoteKeyPress{};
    return true;
}

bool remoteDown(int k, uint32_t now) {
    RemoteKey& r = gRemote[k];
    if (r.holding) return true;
    if (!r.upAt) return false;
    if (static_cast<int32_t>(now - r.upAt) < 0) return true;
    r.upAt = 0;  // seen up once by Key: the press ends here
    return false;
}

// After Key::poll(): a remote hold lets go shortly after its hold fired, so a
// slow loop() turn can never make it a tap.
void remoteKeyEvent(int k, int event, uint32_t now) {
    if (gRemote[k].holding && event == 2) {
        gRemote[k].holding = false;
        gRemote[k].upAt = now + 50;
    }
}

void remoteShot() {
    static const char kHex[] = "0123456789abcdef";
    static char line[16 + 4 * 320 + 2];  // up to 320 pixels a row (T-Display-S3)
    const int w = gCanvas->width(), h = gCanvas->height();
    const uint8_t* px = static_cast<const uint8_t*>(gCanvas->getBuffer());
    Serial.printf("\n#shot %d %d %s\n", w, h, gScreenOff ? "dark" : gDimmed ? "dim" : "on");
    for (int y = 0; y < h && w <= 320 && px; ++y) {
        int n = snprintf(line, sizeof line, "#r %d ", y);
        const uint8_t* row = px + static_cast<size_t>(y) * w * 2;
        for (int i = 0; i < w * 2; ++i) {
            line[n++] = kHex[row[i] >> 4];
            line[n++] = kHex[row[i] & 15];
        }
        line[n++] = '\n';
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
    Serial.printf("#end\n");
}

// 'd': see above. Not while a radio is in use (the portal, Get decks, the
// network test) or the question is already open.
void remoteDateQuestion() {
    if (gScreen == Screen::Date || gHome.active() || gPortal.active() || !gPack->isOpen()) {
        Serial.printf("remote: no date question now (screen %d, radio %d)\n",
                      static_cast<int>(gScreen), gHome.active() || gPortal.active() ? 1 : 0);
        return;
    }
    Serial.printf("remote: the date question, as after a power cut\n");
    gAdvance.cancel();
    dateSyncBegin();
    openDate(Screen::Quiz);
}

void remotePoll(uint32_t now) {
    while (!remoteBusy() && Serial.available() > 0) {
        const int c = Serial.read();
        if (gRemoteKeyPrefix) {
            gRemoteKeyPrefix = false;
            bool held = false;
            const tgc::Key key = remoteKeyFor(c, &held);
            if (key == tgc::Key::None) {
                Serial.printf("remote: no key '%c'\n", c);
                continue;
            }
            gRemoteKey.key = key;
            gRemoteKey.sent = false;
            gRemoteKey.upAt = now + (held ? kRemoteKeyHoldMs : kRemoteKeyTapMs);
        } else if (c == 'k') {
            gRemoteKeyPrefix = true;
        } else if (c == 'a' || c == 'b') {
            gRemote[c == 'b'].upAt = now + 80;
        } else if (c == 'A' || c == 'B') {
            gRemote[c == 'B'].holding = true;
        } else if (c == 's') {
            remoteShot();
        } else if (c == 'w') {
            gLastInputAt = now;
            if (gScreenOff || gDimmed) wakeScreen();
        } else if (c == 'd') {
            remoteDateQuestion();
        } else if (c == 'u') {
            // The next update check is due: clear the stored day. Test only.
            gUpdateDay = 0;
            gPrefs.putUInt(kPrefUpdateDay, 0);
            Serial.println("remote: the next update check is due");
        } else if (c == 'f' || c == 'x') {
            tgc::HomeWifi::setTestFault(c == 'f' ? tgc::HomeWifi::TestFault::BadPin
                                                 : tgc::HomeWifi::TestFault::NoJoin);
            Serial.printf("remote: the next join %s\n",
                          c == 'f' ? "fails its pinned step" : "fails");
        }
    }
}
#endif

// True when the LittleFS partition holds a LittleFS (the "littlefs" magic of the
// superblock, at byte 8 of either block of the root metadata pair). An erased
// partition (a browser flasher's "erase device", a fresh merged image) or one
// left by another firmware does not. Unknown layout: true, so it is never
// formatted blindly.
bool partitionHasLittleFs() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!p) return true;
    char magic[8];
    for (uint32_t block = 0; block < 2; ++block)
        if (esp_partition_read(p, block * 4096u + 8u, magic, sizeof magic) == ESP_OK &&
            std::memcmp(magic, "littlefs", sizeof magic) == 0)
            return true;
    return false;
}

// Mounts LittleFS. A partition with no LittleFS on it is a fresh device: it is
// formatted, and the empty /packs then leads into the Wi-Fi upload screen. A
// LittleFS that will not mount is damage instead: false, and the caller asks.
bool mountFilesystem() {
    if (LittleFS.begin(false)) return true;
    if (partitionHasLittleFs()) return false;
    Serial.println("fs: no LittleFS on the data partition, formatting (first start)");
    showNotice(ui().fsPreparing, ui().fsPreparingHint, kWhite);
    return LittleFS.format() && LittleFS.begin(false);
}

// A damaged filesystem: nothing on it can be read, but it is only erased on a
// long press of the front button (decks and progress are lost), then the device
// restarts into the fresh-device path. The press that woke the chip must be
// released first, so it cannot become that long press.
[[noreturn]] void askFormat() {
    wg::Hints hints;
    hints.hold = ui().hintErase;
    showError(ui().errNoFilesystem, keysOr(ui().errNoFilesystemHint, ui().errNoFilesystemHintKeys),
              &hints);
    bool released = !tgc::board::isHeld(tgc::board::Button::Select);
    const uint32_t start = millis();
    for (;;) {
        tgc::board::update();
        if (!tgc::board::isHeld(tgc::board::Button::Select)) released = true;
        if (released && tgc::board::wasHeld(tgc::board::Button::Select)) {
            Serial.println("fs: erased on request");
            showNotice(ui().fsPreparing, ui().fsPreparingHint, kWhite);
            LittleFS.format();
            ESP.restart();
        }
        if (millis() - start >= kSleepAfterMs && !tgc::board::externalPower())
            tgc::board::deepSleep();
        if (tgc::board::powerButtonClicked()) tgc::board::deepSleep();
        delay(20);
    }
}

// The boot screen of a device with no deck: the Wi-Fi portal, so a deck can
// come from a phone. With a home Wi-Fi saved, the page over it (setup mode if
// the join fails, see wifiLoop()); without one, the device's own access point.
// An access point that will not start leaves nothing to do: the old error
// screen. From setup(), or once the first-boot language is chosen.
void openBootUploadScreen() {
    gWifiBootUpload = true;
    openPortal(defaultPortalMode());
    if (gPortalMode == PortalMode::Setup && gWifiStep != WifiStep::Serving) {
        showError(ui().errNoPack, ui().errNoPackHint);
        halt();
    }
    Serial.printf("wifi: no packs, upload-ready (%s %s)\n",
                  gPortalMode == PortalMode::Home ? "joining" : "ssid",
                  gPortalMode == PortalMode::Home ? gHome.ssid() : gPortal.ssid());
}

// First-boot language picker (ui/UiLang.h decides when): English preselected.
// Driven by loop() like every screen, so presses count for the dim and sleep
// timers, and a sleep while it is open leaves the preference unset: the wake
// asks again.
void openFirstLang() {
    gFirstLangList.setCount(tgc::kFirstBootLangRows);
    gFirstLangList.reset(tgc::kFirstBootLangDefaultRow);
    gScreen = Screen::FirstLang;
    drawScreen();
}

// Stores the choice as an explicit preference (never Auto: a later deck in
// another language must not switch the screens), applies it and continues the
// boot the picker interrupted, now in that language.
void applyFirstLang() {
    gUiLangPref = tgc::firstBootLangPref(gFirstLangList.focus());
    gPrefs.putInt("ui", gUiLangPref);
    setUiForLang(nullptr);
    Serial.printf("ui: first boot, language %s\n", uiLangCode());
    openBootUploadScreen();
}

// loop() on the picker: a tap moves the focus, a hold chooses. The side
// button does nothing, as on the boot-time upload screen that follows.
void firstLangLoop(int a) {
    if (a == 1) {
        gFirstLangList.next();
        drawScreen();
    } else if (a == 2) {
        applyFirstLang();
    }
}

}  // namespace app
}  // namespace tgc

using namespace tgc::app;

void setup() {
    Serial.begin(115200);
    Serial.printf("%s %s\n", tgc::credits::kAppNameLatin, tgc::firmwareVersion());
    tgc::board::begin();
    setCpuFrequencyMhz(kCpuMhz);
    // The press that woke the chip must not also act on the first screen.
    gSwallowInput = tgc::board::wokeFromSleep();

    gPoweredOff = !(gPowerMagic == kPowerMagic && gPowerMagicInv == ~kPowerMagic);
    gPowerMagic = kPowerMagic;
    gPowerMagicInv = ~kPowerMagic;
    auto& disp = tgc::board::display();
    kGrey = disp.color565(60, 60, 70);
    kHighlight = disp.color565(30, 60, 150);
    kGreen = disp.color565(0, 130, 40);
    kRed = disp.color565(170, 20, 20);
    kYellow = disp.color565(255, 210, 60);

    // Apply the saved orientation right after board::begin() (which set portrait)
    // and before the canvas exists, so the buffer matches the panel from boot.
    // The namespace keeps the project's former name on purpose, so settings
    // survive the rename to Tamagotcho (docs/decisions.md, 2026-10-21).
    gPrefs.begin("tangocho", false);
    gUpdateCount = gPrefs.getUChar(kPrefUpdateCount, 0);
    gUpdateDay = gPrefs.getUInt(kPrefUpdateDay, 0);
    gUpdateKnown = gPrefs.getUChar(kPrefUpdateKnown, 0) != 0;
    // Absent (never chosen) is not Auto: it may ask for the language below.
    const bool uiPrefStored = gPrefs.isKey("ui");
    gUiLangPref = gPrefs.getInt("ui", tgc::kUiLangAuto);
    gBrightness = gPrefs.getUChar("bri", gBrightness);
#if defined(TGC_BATT_TEST)
    gBrightness = 255;  // full backlight: drain the cell in a reasonable time
#endif
    gAdvancePref = gPrefs.getUChar(tgc::kPrefAdvance, tgc::kAdvanceDefault);
    gHoldPref = gPrefs.getUChar(tgc::kPrefHold, tgc::kHoldDefault);
    gOnKana = gPrefs.getUChar("onkana", 0) == 1 ? tgc::OnKana::Hiragana : tgc::OnKana::Katakana;
    tgc::board::setBrightness(gBrightness);
    setUiForLang(nullptr);  // localise the pre-pack error screens to the choice too
    gOrientPref = loadOrientPref();
    tgc::board::setRotation(bootRotation());
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    gRng = tgc::XorShiftRng(tgc::board::randomSeed());

    // Woken after a critical-battery sleep: unless the cell recovered (with
    // hysteresis) or USB is in, say so and sleep again before anything runs.
    // The RTC timestamp of the first sleep is left alone, so the next real
    // wake adds the whole span to the day clock.
    if (tgc::board::wokeFromSleep() && gSleptCritical) {
        int pct = -1;  // the best of a few readings: the gauge is noisy
        for (int i = 0; i < 3; ++i) {
            pct = std::max(pct, tgc::board::batteryPercent());
            delay(20);
        }
        const bool external = tgc::board::externalPower() || tgc::board::isCharging();
        Serial.printf("power: critical wake, battery %d%%%s\n", pct, external ? " (usb)" : "");
#if defined(TGC_BATT_TEST)
        // The filesystem is not mounted yet: keep it for the log after mount.
        gBattWakePct = pct;
        gBattWakeUsb = external;
#endif
        if (!tgc::BatteryGuard::canResume(pct, external)) {
            gUi = gSleepUiEs ? &kUiEs : &kUiEn;
            showNotice(ui().batteryTitle, keysOr(ui().batteryHint, ui().batteryHintKeys),
                       tgc::theme::kWarn);
            delay(kBatteryNoticeMs);
            tgc::board::deepSleep();
        }
    }
    gSleptCritical = false;

    if (!mountFilesystem()) askFormat();
    // A power cut in the middle of a download or an upload leaves its temp
    // behind (up to 5 MB), which would count against every later free-space
    // check. Nothing can be in flight at boot.
    for (const char* tmp : {tgc::kFetchTmp, tgc::WifiPortal::kUploadTmp,
                            tgc::WifiPortal::kProgressTmp})
        if (tgc::fsRemoveIfExists(tmp)) Serial.printf("fs: removed stale %s\n", tmp);
#if defined(TGC_BATT_TEST)
    if (gBattWakePct != -2)
        battLog(gBattWakeUsb ? "boot after critical (usb)" : "boot after critical",
                gBattWakePct);
    else
        battLog("boot");
    battLogPrint();
#endif
    listPacks(gPacks);
    // The time zone first: the slept time (applySleepTime() below) is counted
    // in its study days, 23 or 25 h long across a daylight-saving change.
    loadTimeZone();

    // No packs yet: do not halt. Start the Wi-Fi portal so a pack can be
    // uploaded from a phone (or downloaded from a URL); loop() moves on to the
    // quiz as soon as one arrives. This is the practical answer to a fresh (or
    // launcher-formatted) filesystem, including one mountFilesystem() has just
    // formatted. An AP that will not start leaves nothing to do, so that keeps
    // the old error screen. A brand-new device (no language ever chosen) first
    // asks for the language, so that screen and the page are in it; with no
    // deck there is no date question to come before it.
    if (gPacks.empty()) {
        gSrsLoaded = gSrs.begin(gSrsIo);
        gBootDay = gSrs.today();
        gSrs.beginSession();
        applySleepTime();
        if (tgc::firstBootLangPicker(uiPrefStored, !gPacks.empty())) {
            Serial.println("ui: first boot, asking for the language");
            openFirstLang();
            return;
        }
        openBootUploadScreen();
        return;
    }

    String want = gPrefs.getString("pack", "");
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) want = gPacks[0];
    // The saved deck, else the first other one that opens. None opening at all
    // most likely means decks of a newer pack format than this firmware reads.
    bool opened = loadPack(want);
    for (size_t i = 0; !opened && i < gPacks.size(); ++i)
        if (gPacks[i] != want) opened = loadPack(gPacks[i]);
    if (!opened) {
        Serial.printf("pack: none of %u opens\n", (unsigned)gPacks.size());
        showError(ui().errOpenPack, ui().errOpenPackHint);
        halt();
    }
    gPrefs.putString("pack", gCurrentPack);

    const bool srsLoaded = gSrs.begin(gSrsIo);
    gSrsLoaded = srsLoaded;
    gBootDay = gSrs.today();  // the persisted day, before any slept time is added
    gSrs.beginSession();
    applySleepTime();
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);  // loadPack rebuilt with an empty store
    const tgc::DeckSummary counts = deckCounts();

    char today[16];
    formatDate(gSrs.today(), today, sizeof today);
    Serial.printf("srs: day %u (%s), records %u, due %u, new %u%s, powered_off=%d\n",
                  (unsigned)gSrs.today(), today, (unsigned)gSrs.count(), (unsigned)counts.due,
                  (unsigned)counts.fresh, srsLoaded ? "" : " (new)", gPoweredOff ? 1 : 0);

    // A wake from a sleep taken while the question was open asks it again.
    const bool datePending = tgc::board::wokeFromSleep() && gDatePending;
    gDatePending = false;
    if ((gPoweredOff || datePending) && gSrs.count() > 0 && !gWallClockSynced) {
        // Power cut: the elapsed days are unknown. Ask *before* deciding "done",
        // because advancing the day can make reviews due. A real time applied
        // earlier in this boot (applyWallClock) already answered it. Meanwhile
        // the home Wi-Fi, if saved, may answer it with the network time.
        dateSyncBegin();
        openDate(Screen::Quiz);
        return;
    }
    // Once a day (or a week on battery): check the catalog for newer builds of
    // the installed decks before the first question (docs/deck-updates.md).
    if (updateCheckDue()) {
        openUpdateCheck();
        return;
    }
    if (!newQuestion()) {
        showDone();
    } else {
        drawScreen();
    }
}

namespace tgc {
namespace app {

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

// --- Keyboard (board::hasKeyboard(): the Cardputer ADV) -------------------------
// Each key press is one action (ui/KeyInput.h: next, previous, confirm, back,
// menu) on the screen it lands on. keyAction() is the one place that turns an
// action into the screen's own: mostly the button event it stands for, through
// handleInput(), so every screen keeps a single behaviour; the rest is the
// lists' focus up (listFocusUp()) and the menu key's way to the settings home.
// Held arrows repeat (gKeyRepeat) on the lists and the date stepper.

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
    if (gScreen == Screen::Quiz && !gScreenOff && t - gBatteryCheckAt >= kBatteryCheckMs) {
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
