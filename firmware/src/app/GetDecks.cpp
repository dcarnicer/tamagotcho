// Get decks; see GetDecks.h.
#include "app/GetDecks.h"

#include <Arduino.h>

#include <utility>

#include "app/App.h"
#include "app/CardMove.h"
#include "app/Clock.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Firmware.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/Storage.h"
#include "render/Widgets.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/UiLang.h"

namespace tgc {
namespace app {

// --- Get decks (roadmap phase 4) -------------------------------------------

NetStep gNetStep = NetStep::Connecting;
bool gNetFromBoot = false;   // opened from the boot-time upload screen
bool gNetUpdatesOnly = false;  // the updates list: only installed decks with a new build
std::string gGetDecksLangHint;  // the phone's language from the portal page, or ""
std::vector<tgc::CatalogEntry> gCatalog;
int gNetSel = 0;
char gNetNote[48] = "";      // footer status line (cleared when the selection moves)

namespace {

std::vector<tgc::DeckState> gCatalogState;  // per gCatalog entry
uint16_t gNetNoteColor = 0;

// "Get decks" screen (docs/ux-refresh.md, get-decks mockup). Connecting /
// reading: what it is doing over which network. List: the network and the
// catalog's size, then one row per catalog deck -- its title, "EN · 79 kanji"
// and the size; a deck already here is muted with a check, one here in
// another build has a download mark and "update" -- and a note line over the
// hint bar (what the last download did; with a card mounted, until then, the
// card's free space: downloads go there). While a deck downloads, its row
// shows the percentage and a bar, and the side button cancels. Error: a title
// and a wrapped detail. The UI fonts need no pack, so it works before one is
// open.
tgc::ListView gNetList;  // the catalog's window; gNetSel is its focus

}  // namespace

void drawGetDecks() {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, gNetUpdatesOnly ? ui().updateTitle : ui().getDecksTitle, nullptr,
               gHome.active());
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;

    if (gNetStep == NetStep::Connecting || gNetStep == NetStep::Catalog) {
        const char* line = gNetStep == NetStep::Connecting ? ui().netConnecting : ui().netCatalog;
        gRender.drawCentred(d, line, 4, mid - 34, W - 8, 40, 2, kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        wg::Hints h;
        h.side = &tgc::kIconBack;
        h.sideWord = ui().hintCancel;
        drawHints(d, h);
        present();
        return;
    }
    if (gNetStep == NetStep::Error) {
        // The title's box is as tall as its lines (drawCentred centres in it),
        // so the detail starts below the ink of a one-line title too.
        const int top = kHeaderH + 6;
        const int lh = gRender.lineHeight(tgc::Renderer::kTextUi);
        const int titleLines =
            gRender.textWidth(gNetErrTitle, tgc::Renderer::kTextUi) <= W - 8 ? 1 : 2;
        gRender.drawCentred(d, gNetErrTitle, 4, top, W - 8, titleLines * lh, 2, kWarn, kBg,
                            tgc::Renderer::kTextUi);
        const int detailY = top + titleLines * lh + 6;
        drawCaptionLines(d, gNetErrDetail, 4, detailY, W - 8, (H - kHintH - 2 - detailY) / 10,
                         kText);
        backHints(d, nullptr, ui().netHoldRetry);
        present();
        return;
    }

    const int n = static_cast<int>(gCatalog.size());
    char line[72];
    if (gNetUpdatesOnly)
        snprintf(line, sizeof line, ui().updateListFmt, gHome.ssid(), (unsigned)n);
    else
        snprintf(line, sizeof line, ui().netListFmt, gHome.ssid(), (unsigned)n);
    gRender.drawLabel(d, line, kGutter, kHeaderH + 3, W - 2 * kGutter, 8, kTextDim, kBg, kSmall);
    const int top = kHeaderH + 12;
    if (n == 0) {
        drawCaptionLines(d, gNetUpdatesOnly ? ui().updateAllCurrent : ui().netEmpty, 4, mid - 10,
                         W - 8, 3, kText);
    } else {
        const bool wide = wideRows();
        gNetList.setCount(n);
        layoutList(gNetList, top, H - kHintH - kNoteH, wide ? layout().row1H : layout().row2H);
        gNetList.setFocus(gNetSel);
        wg::drawList(d, gNetList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
            const tgc::CatalogEntry& e = gCatalog[i];
            const tgc::DeckState state = gCatalogState[i];
            char lang[8] = "?";
            if (!e.lang.empty()) {
                snprintf(lang, sizeof lang, "%s", e.lang.c_str());
                for (char* p = lang; *p; ++p)
                    if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
            }
            char caption[48], size[16], value[64];
            if (state == tgc::DeckState::OnDevice)
                snprintf(caption, sizeof caption, "%s · %s", lang, ui().netOnDevice);
            else if (state == tgc::DeckState::Update)
                snprintf(caption, sizeof caption, "%s · %s", lang, ui().netHoldUpdate);
            else
                snprintf(caption, sizeof caption, ui().netCardsFmt, lang, (unsigned)e.cards);
            formatMb(e.bytes, true, size, sizeof size);
            const std::string title = tgc::catalogDisplayName(e);
            wg::Row row;
            row.label = title.c_str();
            row.focused = f;
            row.keepLabel = true;
            row.muted = state == tgc::DeckState::OnDevice;
            if (state == tgc::DeckState::OnDevice) {
                row.mark = &tgc::kIconCheck;
                row.markColor = kOkHi;
            } else if (state == tgc::DeckState::Update) {
                row.mark = &tgc::kIconDown;
                row.markColor = kWarn;
            }
            if (gNetBusy && f) {
                const int pct = gNetPct < 0 ? 0 : gNetPct;
                snprintf(value, sizeof value, "%d%%", pct);
                row.progress = pct;
            } else if (wide) {
                // One line: the caption and the size right of the title.
                if (state == tgc::DeckState::OnDevice)
                    snprintf(value, sizeof value, "%s", caption);
                else
                    snprintf(value, sizeof value, "%s · %s", caption, size);
            } else {
                // Two lines: the size of a new deck right of the title (a deck
                // with a mark has no room for it).
                snprintf(value, sizeof value, "%s", state == tgc::DeckState::New ? size : "");
            }
            row.value = value;
            if (!wide) row.caption = caption;
            wg::drawRow(d, gRender, x, y, w, h, row);
        });
    }
    if (gNetNote[0]) {
        menuNote(d, gNetNote, gNetNoteColor);
    } else if (tgc::storage::cardMounted()) {
        char gb[16], note[40];
        snprintf(note, sizeof note, ui().netCardFreeFmt,
                 tgc::formatGb(tgc::storage::cardFreeBytes(), ui().decimalPoint[0], gb,
                               sizeof gb));
        menuNote(d, note, kTextDim);
    }

    wg::Hints h;
    h.side = &tgc::kIconBack;
    if (gNetBusy) {
        h.sideWord = ui().hintCancel;
    } else {
        h.sideWord = ui().hintBack;
        if (n > 0) {
            const tgc::DeckState state = gCatalogState[gNetSel];
            h.tap = ui().hintMove;
            h.hold = state == tgc::DeckState::OnDevice ? ui().netHoldStudy
                     : state == tgc::DeckState::Update ? ui().netHoldUpdate
                                                       : ui().netHoldDownload;
        }
    }
    drawHints(d, h);
    present();
}

namespace {

void netError(const char* title, const char* detail) {
    netRadioOff();
    gNetStep = NetStep::Error;
    snprintf(gNetErrTitle, sizeof gNetErrTitle, "%s", title);
    snprintf(gNetErrDetail, sizeof gNetErrDetail, "%s", detail ? detail : "");
    QLOG("N error: %s | %s\n", title, gNetErrDetail);
}

void netNote(const char* text, uint16_t color) {
    snprintf(gNetNote, sizeof gNetNote, "%s", text);
    gNetNoteColor = color;
}

// Compares the catalog with the installed decks (existence, size and, when the
// size matches, SHA-256: catalog/catalogDeckState()).
void netRefreshStates() {
    listPacks(gPacks);
    gCatalogState.assign(gCatalog.size(), tgc::DeckState::New);
    HashStats hs;
    for (size_t i = 0; i < gCatalog.size(); ++i)
        gCatalogState[i] = deckStateOnDisk(gCatalog[i], &hs);
    if (hs.files)
        Serial.printf("[net] decks: hashed %u pack(s) in %u ms\n", (unsigned)hs.files,
                      (unsigned)hs.ms);
}

}  // namespace

// Opens the Get decks screen and starts joining the home Wi-Fi. `fromBoot`: from
// the boot-time upload screen (whose portal the caller already stopped). A card
// put in since is mounted first, and the device's decks move to it.
void openGetDecks(bool fromBoot, bool updatesOnly) {
    mountCardOnOpen();
    gNetFromBoot = fromBoot;
    gNetUpdatesOnly = updatesOnly;
    gScreen = Screen::GetDecks;
    gCatalog.clear();
    gCatalogState.clear();
    gNetSel = 0;
    gNetBusy = false;
    gNetNote[0] = 0;
    gNetActivityAt = millis();
    if (!tgc::HomeWifi::hasCredentials()) {
        netError(ui().netNoWifi, ui().netNoWifiHint);
        drawScreen();
        return;
    }
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netError(ui().netNoConnect, gHome.ssid());
        drawScreen();
        return;
    }
    gNetStep = NetStep::Connecting;
    QLOG("N get decks: joining %s\n", gHome.ssid());
    drawScreen();
}

namespace {

// Leaves the screen with the radio off: back to the deck list, or -- when it was
// opened from the boot-time upload screen -- into the quiz if a deck is now
// loaded, else back to that screen.
void closeGetDecks() {
    netRadioOff();
    gGetDecksLangHint.clear();
    gCatalog.clear();
    gCatalogState.clear();
    gNetUpdatesOnly = false;
    QLOG("N get decks: closed\n");
    if (gNetFromBoot) {
        gNetFromBoot = false;
        if (gPack->isOpen()) {
            gWifiBootUpload = false;
            startQuizFromPortal();
        } else {
            openPortal(defaultPortalMode());
        }
        return;
    }
    listPacks(gPacks);
    openDecks(false);  // the card was tried when Get decks opened
}

}  // namespace

// Connected: the network is up anyway, so take the real date from NTP first
// (blocking, at most kNtpTimeoutMs; netLoadCatalog() right after resets the
// idle timers and drops the presses made meanwhile). Only with a time zone
// (see netTimeSync()); a failure is logged and Get decks carries on.
void netSyncClock() { netTimeSync(); }

namespace {

// Connected: read and parse the catalog (blocking), and the firmware manifest
// (docs/ota.md §3.3), then show the list.
void netLoadCatalog() {
    const String url = tgc::HomeWifi::catalogUrl();
    tgc::CatalogFetch c = tgc::fetchCatalog(url);
    firmwareAfterCatalog(c.fetch);
    gNetActivityAt = millis();
    gSwallowInput = true;  // presses made while blocked must not act now
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        netError(title, detail);
        return;
    }
    gCatalog = std::move(c.parse.entries);
    // The decks in the screen's language first, the catalog order otherwise
    // (every other language is still listed after them).
    const char* firstLang =
        (gUiLangPref == tgc::kUiLangAuto && !gPack->isOpen() && !gGetDecksLangHint.empty())
            ? gGetDecksLangHint.c_str()
            : uiLangCode();
    tgc::orderCatalogForLang(gCatalog, firstLang);
    Serial.printf("[net] catalog: \"%s\" decks first\n", firstLang);
    netRefreshStates();
    setUpdatesFromCatalog(gCatalog, &gCatalogState);
    if (gNetUpdatesOnly) {
        // The updates list: only installed decks whose catalog build has
        // another size or SHA-256 (docs/deck-updates.md). The count is already
        // stored.
        std::vector<tgc::CatalogEntry> keep;
        std::vector<tgc::DeckState> keepState;
        for (size_t i = 0; i < gCatalog.size(); ++i) {
            if (gCatalogState[i] != tgc::DeckState::Update) continue;
            keep.push_back(std::move(gCatalog[i]));
            keepState.push_back(gCatalogState[i]);
        }
        gCatalog = std::move(keep);
        gCatalogState = std::move(keepState);
        gNetSel = 0;
        gNetStep = NetStep::List;
        return;
    }
    // Start on the first deck that is not here yet.
    gNetSel = 0;
    for (size_t i = 0; i < gCatalogState.size(); ++i)
        if (gCatalogState[i] != tgc::DeckState::OnDevice) {
            gNetSel = static_cast<int>(i);
            break;
        }
    gNetStep = NetStep::List;
}

const char* netDownloadError(const tgc::FetchResult& r, char* buf, size_t n) {
    switch (r.status) {
        case tgc::FetchStatus::Connect: return ui().netNoServer;
        case tgc::FetchStatus::Tls: return ui().netTls;
        case tgc::FetchStatus::Http:
            snprintf(buf, n, ui().netHttpFmt, r.httpCode);
            return buf;
        case tgc::FetchStatus::NoSpace:
        case tgc::FetchStatus::TooLarge: return ui().netNoSpace;
        case tgc::FetchStatus::SizeMismatch:
        case tgc::FetchStatus::HashMismatch: return ui().netCorrupt;
        case tgc::FetchStatus::InvalidPack: return ui().netInvalid;
        case tgc::FetchStatus::Cancelled: return ui().netCancelled;
        default: return ui().netFailed;
    }
}

// Get decks: downloads catalog deck `i` and marks the row.
void netDownload(int i) {
    const tgc::CatalogEntry e = gCatalog[i];
    const bool updating = gCatalogState[i] == tgc::DeckState::Update;
    gNetNote[0] = 0;
    const tgc::DeckDownloadResult d = downloadCatalogDeck(e);
    if (d.batteryLow) {
        netNote(ui().batteryTitle, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    const tgc::FetchResult& r = d.fetch;
    if (r.status == tgc::FetchStatus::Ok && updating) {
        if (gUpdateCount > 0) --gUpdateCount;
        saveUpdateState();
    }
    if (r.status == tgc::FetchStatus::Ok && gNetUpdatesOnly && updating) {
        // The updates list shows only what still needs it: drop the row now.
        gCatalog.erase(gCatalog.begin() + i);
        gCatalogState.erase(gCatalogState.begin() + i);
        if (gNetSel >= static_cast<int>(gCatalog.size()))
            gNetSel = gCatalog.empty() ? 0 : static_cast<int>(gCatalog.size()) - 1;
    } else {
        netRefreshStates();
    }
    char buf[48];
    if (r.status == tgc::FetchStatus::Ok) {
        snprintf(buf, sizeof buf, ui().netSavedFmt, tgc::catalogDisplayName(e).c_str());
        netNote(buf, tgc::theme::kOkHi);
    } else if (r.status == tgc::FetchStatus::NoSpace && updating) {
        // The old copy stays until the new one validated, so an update needs
        // room for both: say how to make it.
        netError(ui().netNoSpace, ui().netNoSpaceUpdate);
    } else {
        netNote(netDownloadError(r, buf, sizeof buf), tgc::theme::kWarn);
    }
    drawScreen();
}

// Loads a deck that is already on the device and starts studying it.
void netStudy(int i) {
    const String path = catalogPath(gCatalog[i]);
    if (!loadPack(path)) {
        netNote(ui().netInvalid, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    gPrefs.putString("pack", path);
    listPacks(gPacks);
    netRadioOff();
    gCatalog.clear();
    gCatalogState.clear();
    gNetFromBoot = false;
    gNetUpdatesOnly = false;
    gWifiBootUpload = false;
    startQuizFromPortal();
}

// Shows why joining the home Wi-Fi failed (the radio is already off).
void netJoinFailed() { netError(joinFailureText(), gHome.ssid()); }

}  // namespace

// loop() while on the Get decks screen.
void getDecksLoop(int a, int b) {
    if (b) {  // side button: back (or cancel the connection attempt)
        closeGetDecks();
        return;
    }
    if (gNetStep == NetStep::Connecting) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            gNetStep = NetStep::Catalog;
            drawScreen();
            netSyncClock();
            netLoadCatalog();
            drawScreen();
        } else if (st == tgc::HomeWifi::State::Failed) {
            netJoinFailed();
            drawScreen();
        }
        return;
    }
    if (gNetStep == NetStep::Error) {
        if (a == 2) openGetDecks(gNetFromBoot, gNetUpdatesOnly);  // retry from the start
        return;
    }
    if (gNetStep != NetStep::List || gCatalog.empty()) return;
    if (a == 1) {
        gNetSel = (gNetSel + 1) % static_cast<int>(gCatalog.size());
        gNetNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        if (gCatalogState[gNetSel] == tgc::DeckState::OnDevice)
            netStudy(gNetSel);
        else
            netDownload(gNetSel);
    }
}

}  // namespace app
}  // namespace tgc
