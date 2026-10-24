// The home Wi-Fi's shared parts; see Net.h.
#include "app/Net.h"

#include "app/App.h"
#include "app/Decks.h"
#include "app/Input.h"
#include "app/Power.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "board/Board.h"
#include "platform/FsQuiet.h"
#include "platform/PackHash.h"
#include "power/BatteryGuard.h"
#include "ui/KeyInput.h"

using tgc::board::Button;

namespace tgc {
namespace app {

tgc::HomeWifi gHome;
bool gNetBusy = false;       // a transfer is running (footer shows the progress)
int gNetPct = -1;            // last drawn download percentage
char gNetDeck[72] = "";      // the title of the deck being downloaded (the portal screen shows it)
uint32_t gNetActivityAt = 0; // millis() of the last network progress (idle timers)
char gNetErrTitle[48] = "";
char gNetErrDetail[96] = "";

namespace {

uint32_t gNetInputAt = 0;    // last button poll during a blocking transfer
// A sleep asked for during a blocking transfer (power button, critical
// battery): netProgress() cancels the transfer, and netSleepIfAsked() sleeps
// once it has cleaned up after itself.
enum class NetSleep : uint8_t { None, User, Critical };
NetSleep gNetSleep = NetSleep::None;

}  // namespace

// Radio off and the CPU back to its normal clock. Safe in any state.
void netRadioOff() {
    const bool wasOn = gHome.state() != tgc::HomeWifi::State::Off;
    gHome.end();
    if (wasOn) setCpuFrequencyMhz(kCpuMhz);
}

// Why joining the home Wi-Fi failed (the radio is already off).
const char* joinFailureText() {
    using F = tgc::HomeWifi::Failure;
    const F f = gHome.failure();
    return f == F::NotFound ? ui().netNotFound
                            : (f == F::AuthFailed ? ui().netAuthFailed : ui().netNoConnect);
}

// "4.9" (MB, one decimal), with " MB" when `unit`.
void formatMb(size_t bytes, bool unit, char* out, size_t n) {
    const unsigned tenths = static_cast<unsigned>(bytes / (1024u * 1024u / 10u));
    snprintf(out, n, unit ? "%u.%u MB" : "%u.%u", tenths / 10u, tenths % 10u);
}

String catalogPath(const tgc::CatalogEntry& e) { return String("/packs/") + e.name.c_str() + ".tgc"; }

// The state of catalog deck `e` against /packs: existence and exact size first,
// then -- only when the size matches -- the installed file's SHA-256 against
// the catalog's (docs/deck-updates.md). A file whose size differs is never
// hashed. A hash failure falls back to the size-only answer, so a read error
// never invents an update.
tgc::DeckState deckStateOnDisk(const tgc::CatalogEntry& e, HashStats* stats) {
    const String path = catalogPath(e);
    const long size = tgc::fsFileSize(path);
    if (size < 0) return tgc::DeckState::New;
    const uint32_t localBytes = static_cast<uint32_t>(size);
    if (localBytes != e.bytes) return tgc::catalogDeckState(true, localBytes, e.bytes);
    uint8_t digest[32];
    const uint32_t t0 = millis();
    const bool ok = tgc::fsFileSha256(path, digest);
    if (stats) {
        stats->ms += millis() - t0;
        ++stats->files;
    }
    return tgc::catalogDeckState(true, localBytes, e.bytes, ok ? digest : nullptr,
                                 e.sha256);
}

// Why a catalog read failed, as a title and a detail line (the error screen's
// two parts; the network test shows the title). False when it did not fail.
bool catalogFailure(const tgc::CatalogFetch& c, const String& url, char* title, size_t tn,
                    char* detail, size_t dn) {
    snprintf(detail, dn, "%s", url.c_str());
    switch (c.fetch.status) {
        case tgc::FetchStatus::Ok:
            if (c.parse.error == tgc::CatalogError::Ok) return false;
            snprintf(title, tn, "%s", ui().netBadCatalog);
            if (c.parse.error == tgc::CatalogError::UnsupportedVersion)
                snprintf(detail, dn, "%s",
                         c.parse.version < tgc::kCatalogVersion ? ui().netOldCatalog
                                                                : ui().netNewerCatalog);
            return true;
        case tgc::FetchStatus::Connect:
            snprintf(title, tn, "%s", ui().netNoServer);
            return true;
        case tgc::FetchStatus::Tls:
            snprintf(title, tn, "%s", ui().netTls);
            snprintf(detail, dn, "mbedTLS -0x%04X  %s", (unsigned)(-c.fetch.tlsError), url.c_str());
            return true;
        case tgc::FetchStatus::Http:
            // 404 on the default URL: no release with a catalog exists yet.
            snprintf(title, tn, ui().netHttpFmt, c.fetch.httpCode);
            if (c.fetch.httpCode == 404) snprintf(detail, dn, "%s", ui().netNotPublished);
            return true;
        case tgc::FetchStatus::TooLarge:
            snprintf(title, tn, "%s", ui().netBadCatalog);
            return true;
        default:
            snprintf(title, tn, "%s", ui().netFailed);
            return true;
    }
}

// A blocking transfer starts: the progress state and its first frame.
void netTransferBegin() {
    gNetBusy = true;
    gNetPct = -1;
    gNetInputAt = millis();
    gNetSleep = NetSleep::None;
    drawScreen();
}

// Transfer progress: redraw on a new percentage, keep the idle timers away and
// let the side button cancel (its release is swallowed afterwards). The power
// button and a critical battery cancel too and ask netSleepIfAsked() to
// sleep: a transfer may run for minutes at 240 MHz, and neither can wait that
// long. A download the portal page asked for also lets the portal answer the
// page's progress view meanwhile.
bool netProgress(size_t done, size_t total, void*) {
    const uint32_t now = millis();
    gNetActivityAt = now;
    if (gScreen == Screen::Wifi) gPortal.serviceDownload(done, total);
    if (now - gNetInputAt >= 50) {
        gNetInputAt = now;
        tgc::board::update();
        if (tgc::board::isHeld(Button::Next)) return false;
        // A keyboard's esc (or del) cancels as well. The other keys pressed
        // meanwhile are dropped, as presses during a blocking step are, and a
        // held arrow stops repeating (its release may be among them).
        tgc::KeyEvent ev;
        bool cancel = false, keys = false;
        while (tgc::board::nextKeyEvent(&ev)) {
            keys = true;
            if (ev.down && tgc::keyAction(ev.key) == tgc::KeyAction::Back) cancel = true;
        }
        if (keys) gKeyRepeat.cancel();
        if (cancel) return false;
        if (tgc::board::powerButtonClicked()) {
            gNetSleep = NetSleep::User;
            return false;
        }
    }
    if (batteryCritical(now)) {
        gNetSleep = NetSleep::Critical;
        return false;
    }
    const int pct = total ? static_cast<int>(static_cast<uint64_t>(done) * 100u / total) : 0;
    if (pct != gNetPct) {
        gNetPct = pct;
        drawScreen();
    }
    return true;
}

// The transfer returned: presses made meanwhile must not act now.
void netTransferEnd() {
    gNetBusy = false;
    gNetActivityAt = millis();
    gSwallowInput = true;
}

// Cancelled for a sleep: the transfer has cleaned up (a deck's temp is gone and
// the old deck untouched; a firmware slot aborted), so sleep now (sleepNow()
// saves).
void netSleepIfAsked() {
    if (gNetSleep != NetSleep::None) sleepNow(gNetSleep == NetSleep::Critical);
}

// Downloads catalog deck `e` into /packs (atomic; size and SHA-256 checked),
// then rescans /packs through the portal's path. Shared by the Get decks screen
// and the portal page in home mode; the progress shows on whichever of the two
// is open (netProgress() redraws it), and the side button cancels.
tgc::DeckDownloadResult downloadCatalogDeck(const tgc::CatalogEntry& e) {
    tgc::DeckDownloadResult out;
    const String path = catalogPath(e);
    // On battery below the level a critical sleep resumes at, the download
    // would likely be cut by the guard: refuse it up front.
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    if (!tgc::BatteryGuard::canResume(tgc::board::batteryPercent(), external)) {
        out.batteryLow = true;
        return out;
    }
    // Replacing the loaded deck: close it first so its file is not open while
    // the new one is renamed over it; portalPacksChanged() reloads it below.
    closePackIfLoaded(path);
    snprintf(gNetDeck, sizeof gNetDeck, "%s", tgc::catalogDisplayName(e).c_str());
    netTransferBegin();

    tgc::PackFetchRequest req;
    req.url = e.url.c_str();
    req.fileName = String(e.name.c_str()) + ".tgc";
    req.allowHttps = true;
    req.maxBytes = tgc::WifiPortal::kMaxPackBytes;
    req.expectBytes = e.bytes;
    req.expectSha256 = e.sha256;
    req.idleTimeoutMs = kNetIdleMs;
    req.totalTimeoutMs = kNetDownloadMs;
    req.http10 = true;
    req.progress = netProgress;
    const uint32_t started = millis();
    const tgc::FetchResult r = tgc::fetchPackFile(req);
    netTransferEnd();
    Serial.printf("[net] get %s: %s (%u bytes in %u ms, http %d, tls %d)\n", req.fileName.c_str(),
                  tgc::fetchStatusString(r.status), (unsigned)r.bytes,
                  (unsigned)(millis() - started), r.httpCode, r.tlsError);
    // Cancelled for a sleep: the wake reopens the deck closed above.
    netSleepIfAsked();

    // Rescan /packs the portal's way; that also reopens the deck closed above, or
    // loads the first one when none was open (the boot-time path), and gives
    // the settings home's "Continue" a question from the deck now loaded.
    if (r.packsChanged || !gPack->isOpen()) portalPacksChanged();
    out.fetch = r;
    return out;
}
}  // namespace app
}  // namespace tgc
