// Settings > Wi-Fi and the network test; see WifiMenu.h.
#include "app/WifiMenu.h"

#include "app/App.h"
#include "app/Choice.h"
#include "app/Clock.h"
#include "app/DeckUpdates.h"
#include "app/Display.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "portal/PortalLogic.h"
#include "render/Widgets.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

// --- Settings > Wi-Fi --------------------------------------------------------

// Settings > Wi-Fi (the submenu replacing the single settings row).
tgc::ListView gWifiMenuList;
String gWifiMenuSsid;          // the saved home network, read when the menu opens
char gWifiMenuNote[48] = "";   // footer note (cleared when the selection moves)
NetTestStep gTestStep = NetTestStep::NoNetwork;

namespace {

uint16_t gWifiMenuNoteColor = 0;
String gTestIp;
int gTestRssi = 0;
int gTestTime = -1;            // network time: -1 not tried (no time zone), 0 failed, 1 ok
char gTestCatalog[64] = "";
uint16_t gTestCatalogColor = 0;
char gTestFail[48] = "";

// Settings > Wi-Fi: the web portal (home mode once a network is saved), the
// home network with its test (the network's name is the row's value), setup
// mode (the access point, e.g. to change networks) and "forget". The menu
// rows; a note above the hint bar says what the last action did.
constexpr int kWifiMenuRows = 4;

}  // namespace

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

namespace {

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

}  // namespace

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

namespace {

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

}  // namespace

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

}  // namespace app
}  // namespace tgc
