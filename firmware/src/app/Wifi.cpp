// The Wi-Fi screen and the portal's glue; see Wifi.h.
#include "app/Wifi.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "app/App.h"
#include "app/Clock.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/GetDecks.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Progress.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "app/WifiMenu.h"
#include "pack/Utf8.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "portal/PortalLogic.h"
#include "render/Widgets.h"
#include "ui/Theme.h"
#include "ui/UiLang.h"

namespace tgc {
namespace app {

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

namespace {

int gQrPage = 0;                      // setup mode: 0 = join the AP, 1 = open the page
char gWifiNote[64] = "";              // setup mode after the boot-time home join failed: why
// Bumped whenever the Wi-Fi screen changes under loop()'s feet (a mode switch,
// a blocking download from the page, leaving for Get decks), so the input read
// before the portal's poll() is not applied to what came after it.
uint32_t gWifiEpoch = 0;

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

}  // namespace

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

// --- Wi-Fi portal integration (roadmap phase 4) ----------------------------

namespace {

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

}  // namespace

// The mode the portal opens in: home once a network is saved, else setup.
PortalMode defaultPortalMode() {
    return tgc::HomeWifi::hasCredentials() ? PortalMode::Home : PortalMode::Setup;
}

namespace {

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

}  // namespace

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

namespace {

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

}  // namespace

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

}  // namespace app
}  // namespace tgc
