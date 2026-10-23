// Deck update checks; see DeckUpdates.h.
#include "app/DeckUpdates.h"

#include <Arduino.h>

#include "app/App.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "board/Board.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "render/Widgets.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

// --- Deck update checks (docs/deck-updates.md) ------------------------------

uint8_t gUpdateCount = 0;       // installed decks with an update (NVS `updcount`)
uint32_t gUpdateDay = 0;        // epoch day of the last check (NVS `updday`)
bool gUpdateKnown = false;      // a catalog read succeeded (NVS `updok`)

namespace {

enum class UpdateStep : uint8_t { Joining, Catalog, Done, Failed };
UpdateStep gUpdateStep = UpdateStep::Joining;
uint32_t gUpdateStartedAt = 0;  // millis() when the join began
uint32_t gUpdateDoneAt = 0;     // millis() when the result appeared
uint8_t gUpdateFound = 0;       // updates found by this check
char gUpdateErr[48] = "";       // the failure's title (join or catalog)
constexpr uint32_t kUpdateJoinMs = 30000;  // a tap skips sooner
constexpr uint32_t kUpdateResultMs = 2000; // the result before the quiz

// Epoch day of the SRS day clock, or 0 while no real clock has been applied.
uint32_t currentEpochDay() {
    return gSrs.hasWallClock() ? static_cast<uint32_t>(gSrs.epochDayOf(gSrs.today())) : 0;
}

}  // namespace

void saveUpdateState() {
    gPrefs.putUChar(kPrefUpdateCount, gUpdateCount);
    gPrefs.putUChar(kPrefUpdateKnown, gUpdateKnown ? 1 : 0);
    if (gUpdateDay) gPrefs.putUInt(kPrefUpdateDay, gUpdateDay);
}

namespace {

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

}  // namespace

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

namespace {

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

}  // namespace

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

}  // namespace app
}  // namespace tgc
