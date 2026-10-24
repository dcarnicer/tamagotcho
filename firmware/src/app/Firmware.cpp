// Firmware updates over the air; see Firmware.h and docs/ota.md.
#include "app/Firmware.h"

#include <Arduino.h>

#include <cstring>
#include <string>

#include "Version.h"
#include "app/App.h"
#include "app/Display.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "board/Board.h"
#include "ota/Manifest.h"
#include "ota/Offer.h"
#include "platform/HomeWifi.h"
#include "platform/Ota.h"
#include "power/BatteryGuard.h"
#include "render/Widgets.h"
#include "ui/ScreenLayout.h"
#include "ui/Theme.h"

using tgc::board::Button;

namespace tgc {
namespace app {

char gFwBadge[24] = "";  // NVS `fwver`
tgc::ListView gFwList;

namespace {

// NVS `tangocho`. The badge and its size; what an install wrote and where,
// set right before the restart into it (firmwareBoot() reads it on the next
// boot); a version the device rolled back from.
constexpr const char* kPrefFwVersion = "fwver";
constexpr const char* kPrefFwBytes = "fwsize";
constexpr const char* kPrefOtaNew = "otanew";
constexpr const char* kPrefOtaSlot = "otaslot";
constexpr const char* kPrefOtaBad = "otabad";

// docs/ota.md §3.5: a new image is good once setup() returned and the loop
// ran this long.
constexpr uint32_t kValidAfterMs = 10000;
// The manifest's body (a few hundred bytes); the connection has its own
// timeouts (NetFetch).
constexpr uint32_t kManifestTimeoutMs = 10000;
constexpr uint32_t kNoticeMs = 4000;   // the rollback notice at boot
constexpr uint32_t kRestartMs = 1500;  // "Restarting into ..." before the restart

bool gLayout = false;      // ota::layoutIsOurs(), read at boot
bool gPending = false;     // this image boots pending verification
// The manifest URL is overridden (NVS `net`/`fwurl`), read again before every
// manifest read and when the screen opens: the portal page may change it.
bool gOverridden = false;
uint32_t gLoopAt = 0;      // millis() of the first loop() turn; 0 while setup() runs
uint32_t gBadgeBytes = 0;  // NVS `fwsize`
char gBad[24] = "";        // NVS `otabad`
char gRolledBack[24] = ""; // this boot came back from that version (the notice, once)

// The last manifest read of this boot.
struct Check {
    bool done = false;  // a read finished, maybe failed
    bool ok = false;    // it arrived and parsed: `verdict` holds
    tgc::OfferVerdict verdict = tgc::OfferVerdict::BadManifest;
    std::string version;   // the manifest's
    tgc::ManifestApp app;  // this build's row (Offer, TooLarge)
    char title[48] = "";   // !ok: why, as a title and a detail
    char detail[96] = "";
};
Check gCheck;

// The screen's session: joining, the blocking manifest read, the install's
// download (blocking, with its progress), Update's image check, the restart.
enum class Step : uint8_t { Idle, Joining, Checking, Downloading, Verifying, Restarting };
Step gStep = Step::Idle;
bool gInstalling = false;  // this session installs (else it only checks)
// The outcome of the last action here (a failed install, a precondition),
// shown instead of the check's status until the next one.
char gNoteTitle[48] = "";
char gNoteDetail[96] = "";
uint16_t gNoteColor = 0;

void prefString(const char* key, char* out, size_t n) {
    out[0] = 0;
    if (gPrefs.isKey(key)) gPrefs.getString(key, out, n);
}

void removePref(const char* key) {
    if (gPrefs.isKey(key)) gPrefs.remove(key);
}

void refreshOverride() { gOverridden = tgc::HomeWifi::firmwareUrlOverride().length() > 0; }

// This device reads a manifest: our layout, and a release build or a URL of
// its own (docs/ota.md §3.2).
bool readAllowed() {
    return gLayout && tgc::manifestReadAllowed(tgc::firmwareVersion(), gOverridden);
}

// `v` is newer than the running version: by release order, or for a dev build
// any other version.
bool newerThanRunning(const char* v) {
    const char* running = tgc::firmwareVersion();
    tgc::FirmwareVersion a, b;
    if (tgc::parseFirmwareVersion(v, std::strlen(v), &a) &&
        tgc::parseFirmwareVersion(running, std::strlen(running), &b))
        return tgc::compareFirmwareVersions(a, b) > 0;
    return std::strcmp(v, running) != 0;
}

// Writes NVS only when the badge changed ("" clears it).
void saveBadge(const char* version, uint32_t bytes) {
    if (std::strcmp(gFwBadge, version) == 0 && gBadgeBytes == bytes) return;
    snprintf(gFwBadge, sizeof gFwBadge, "%s", version);
    gBadgeBytes = bytes;
    if (*version) {
        gPrefs.putString(kPrefFwVersion, version);
        gPrefs.putUInt(kPrefFwBytes, bytes);
    } else {
        removePref(kPrefFwVersion);
        removePref(kPrefFwBytes);
    }
}

void clearInstallRecord() {
    removePref(kPrefOtaNew);
    removePref(kPrefOtaSlot);
}

// The running image has shown it works: no rollback any more (§3.5).
void markGood(const char* why) {
    gPending = false;
    if (!tgc::ota::markValid()) return;
    Serial.printf("[ota] %s in %s marked valid (%s)\n", tgc::firmwareVersion(),
                  tgc::ota::runningSlot(), why);
    clearInstallRecord();
}

// Why a manifest read failed, as a title and a detail (the URL, as the
// catalog's errors show it).
void checkFailed(const tgc::FetchResult& r, const String& url) {
    snprintf(gCheck.detail, sizeof gCheck.detail, "%s", url.c_str());
    switch (r.status) {
        case tgc::FetchStatus::Connect:
            snprintf(gCheck.title, sizeof gCheck.title, "%s", ui().netNoServer);
            break;
        case tgc::FetchStatus::Tls:
            snprintf(gCheck.title, sizeof gCheck.title, "%s", ui().fwTls);
            snprintf(gCheck.detail, sizeof gCheck.detail, "mbedTLS -0x%04X  %s",
                     (unsigned)(-r.tlsError), url.c_str());
            break;
        case tgc::FetchStatus::Http:
            snprintf(gCheck.title, sizeof gCheck.title, ui().netHttpFmt, r.httpCode);
            if (r.httpCode == 404)
                snprintf(gCheck.detail, sizeof gCheck.detail, "%s", ui().fwNotPublished);
            break;
        case tgc::FetchStatus::TooLarge:  // more than 8 KiB is not a manifest
            snprintf(gCheck.title, sizeof gCheck.title, "%s", ui().fwBadManifest);
            break;
        default:
            snprintf(gCheck.title, sizeof gCheck.title, "%s", ui().netFailed);
            break;
    }
}

// Connected: reads and parses the manifest (blocking), decides what it offers
// this build and refreshes the badge: an offer sets it, any other parsed
// answer clears it, a failed read leaves it. True when the manifest parsed.
bool readManifest() {
    refreshOverride();
    const String url = tgc::HomeWifi::firmwareUrl();
    std::string text;
    const tgc::FetchResult r =
        tgc::fetchText(url, tgc::kManifestMaxBytes, kManifestTimeoutMs, &text);
    Serial.printf("[ota] manifest %s: %s (http %d, tls %d, %u bytes)\n", url.c_str(),
                  tgc::fetchStatusString(r.status), r.httpCode, r.tlsError, (unsigned)r.bytes);
    gCheck = Check();
    gCheck.done = true;
    if (r.status != tgc::FetchStatus::Ok) {
        checkFailed(r, url);
        return false;
    }
    const tgc::ManifestParse m = tgc::parseManifest(text.data(), text.size(), url.c_str());
    if (m.error != tgc::ManifestError::Ok) {
        Serial.printf("[ota] manifest: %s (line %u)\n", tgc::manifestErrorString(m.error),
                      (unsigned)m.line);
        snprintf(gCheck.title, sizeof gCheck.title, "%s", ui().fwBadManifest);
        snprintf(gCheck.detail, sizeof gCheck.detail, "%s", url.c_str());
        return false;
    }
    tgc::OfferInput in;
    in.running = tgc::firmwareVersion();
    in.env = tgc::ota::buildEnv();
    in.urlOverridden = gOverridden;
    in.rolledBack = gBad;
    in.slotBytes = tgc::ota::slotBytes();
    const tgc::Offer o = tgc::offerFor(m, in);
    gCheck.ok = true;
    gCheck.verdict = o.verdict;
    gCheck.version = m.version;
    if (o.app) gCheck.app = *o.app;
    Serial.printf("[ota] manifest %s, %u image(s): %s (%s, %s)\n", m.version.c_str(),
                  (unsigned)m.apps.size(), tgc::offerVerdictString(o.verdict), in.env, in.running);
    if (o.verdict == tgc::OfferVerdict::Offer)
        saveBadge(m.version.c_str(), o.app->bytes);
    else
        saveBadge("", 0);
    return true;
}

// The version an Install would get and its size: this boot's read, else the
// badge an earlier one left.
bool offerKnown() {
    if (!readAllowed()) return false;
    if (gCheck.ok) return gCheck.verdict == tgc::OfferVerdict::Offer;
    return gFwBadge[0] != 0;
}
const char* offerVersion() { return gCheck.ok ? gCheck.version.c_str() : gFwBadge; }
uint32_t offerBytes() { return gCheck.ok ? gCheck.app.bytes : gBadgeBytes; }

// The screen's rows: Install while an offer is known, Check now wherever a
// manifest may be read.
enum class FwRow : uint8_t { Install, Check };
int fwRows(FwRow* out) {
    int n = 0;
    if (offerKnown()) out[n++] = FwRow::Install;
    if (readAllowed()) out[n++] = FwRow::Check;
    return n;
}

void note(const char* title, const char* detail, uint16_t color) {
    snprintf(gNoteTitle, sizeof gNoteTitle, "%s", title);
    snprintf(gNoteDetail, sizeof gNoteDetail, "%s", detail ? detail : "");
    gNoteColor = color;
}

// Back to the rows with the radio off, the focus kept within them.
void sessionDone() {
    netRadioOff();
    gStep = Step::Idle;
    gNetActivityAt = millis();
    FwRow rows[2];
    gFwList.setCount(fwRows(rows));
    drawScreen();
}

// Why an install failed, in a few words.
const char* installFailure(const tgc::FetchResult& r, char* buf, size_t n) {
    switch (r.status) {
        case tgc::FetchStatus::Connect: return ui().netNoServer;
        case tgc::FetchStatus::Tls: return ui().fwTls;
        case tgc::FetchStatus::Http:
            snprintf(buf, n, ui().netHttpFmt, r.httpCode);
            return buf;
        case tgc::FetchStatus::SizeMismatch:
        case tgc::FetchStatus::HashMismatch: return ui().netCorrupt;
        case tgc::FetchStatus::Cancelled: return ui().netCancelled;
        case tgc::FetchStatus::BadImage: return ui().fwBadImage;
        case tgc::FetchStatus::WriteFailed: return ui().fwWriteFailed;
        case tgc::FetchStatus::NoSlot: return ui().fwUnavailable;
        case tgc::FetchStatus::TooLarge: return ui().netNoSpace;
        default: return ui().netFailed;
    }
}

// The download's progress; once the whole image is in, the screen says it is
// being checked (Update.end() reads it back before it switches the slot).
bool installProgress(size_t done, size_t total, void* ctx) {
    if (total && done >= total && gStep == Step::Downloading) {
        gStep = Step::Verifying;
        gNetPct = -1;  // redraw at 100 %
    }
    return netProgress(done, total, ctx);
}

// Connected, the manifest offers an image: install it (blocking) and restart
// into it, or say why not. The side button and Esc cancel the download, the
// power button cancels and sleeps (netProgress()).
void installOffer() {
    const tgc::ManifestApp app = gCheck.app;
    const std::string version = gCheck.version;
    char slot[8];
    snprintf(slot, sizeof slot, "%s", tgc::ota::nextSlot());
    gStep = Step::Downloading;
    netTransferBegin();
    tgc::ota::InstallRequest req;
    req.app = &app;
    req.idleTimeoutMs = kNetIdleMs;
    req.totalTimeoutMs = kNetDownloadMs;
    req.progress = installProgress;
    const tgc::FetchResult r = tgc::ota::install(req);
    netTransferEnd();
    if (r.status != tgc::FetchStatus::Ok) {
        netRadioOff();
        gStep = Step::Idle;
        netSleepIfAsked();  // the slot was aborted: nothing is left to undo
        char reason[48], stays[96];
        snprintf(stays, sizeof stays, ui().fwStaysFmt, tgc::firmwareVersion());
        note(installFailure(r, reason, sizeof reason), stays, tgc::theme::kWarn);
        sessionDone();
        return;
    }
    // The boot slot is the new image now. What was installed where goes to
    // NVS first, so the next boot can tell a rollback (firmwareBoot()); then
    // the progress is saved and the device restarts into it.
    gPrefs.putString(kPrefOtaNew, version.c_str());
    gPrefs.putString(kPrefOtaSlot, slot);
    if (gDirty || gSrsLoaded) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    netRadioOff();
    gStep = Step::Restarting;
    drawScreen();
    Serial.printf("[ota] restarting into %s in %s\n", version.c_str(), slot);
    Serial.flush();
    delay(kRestartMs);
    ESP.restart();
}

// Joined: the manifest, then (Install) the image.
void sessionConnected() {
    gStep = Step::Checking;
    drawScreen();
    const bool ok = readManifest();
    gSwallowInput = true;  // presses made while blocked must not act now
    gNetActivityAt = millis();
    if (gInstalling && ok && gCheck.verdict == tgc::OfferVerdict::Offer) {
        installOffer();
        return;
    }
    sessionDone();
}

// Install or Check now: the preconditions, then the join (polled by
// firmwareLoop()).
void startSession(bool install) {
    gNoteTitle[0] = 0;
    if (!tgc::HomeWifi::hasCredentials()) {
        note(ui().netNoWifi, ui().netNoWifiHint, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    // As a deck download: on battery below the level a critical sleep resumes
    // at, the guard would likely cut it.
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    if (install && !tgc::BatteryGuard::canResume(tgc::board::batteryPercent(), external)) {
        note(ui().batteryTitle, ui().fwBatteryHint, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    gInstalling = install;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        setCpuFrequencyMhz(kCpuMhz);
        note(ui().netNoConnect, gHome.ssid(), tgc::theme::kWarn);
        drawScreen();
        return;
    }
    gStep = Step::Joining;
    Serial.printf("[ota] %s: joining %s\n", install ? "install" : "check", gHome.ssid());
    drawScreen();
}

// The status (label face) and the detail (captions) of the idle screen.
void idleStatus(const char** title, uint16_t* color, char* detail, size_t dn) {
    using namespace tgc::theme;
    detail[0] = 0;
    char size[16];
    if (!gLayout) {
        *title = ui().fwUnavailable;
        *color = kWarn;
        snprintf(detail, dn, "%s", ui().fwUnavailableHint);
    } else if (gNoteTitle[0]) {
        *title = gNoteTitle;
        *color = gNoteColor;
        snprintf(detail, dn, "%s", gNoteDetail);
    } else if (!readAllowed()) {
        *title = ui().fwDevBuild;
        *color = kText;
        snprintf(detail, dn, "%s", ui().fwDevBuildHint);
    } else if (gCheck.done && !gCheck.ok) {
        *title = gCheck.title;
        *color = kWarn;
        snprintf(detail, dn, "%s", gCheck.detail);
    } else if (offerKnown()) {
        static char line[48];
        snprintf(line, sizeof line, ui().fwAvailableFmt, offerVersion());
        *title = line;
        *color = kAccentHi;
        if (offerBytes()) {
            formatMb(offerBytes(), true, size, sizeof size);
            snprintf(detail, dn, ui().fwSizeFmt, size);
        }
    } else if (gCheck.ok) {
        const char* v = gCheck.version.c_str();
        *color = kText;
        *title = ui().fwNoUpdate;
        switch (gCheck.verdict) {
            case tgc::OfferVerdict::UpToDate:
                *title = ui().fwUpToDate;
                *color = kOkHi;
                snprintf(detail, dn, ui().fwLatestFmt, v);
                break;
            case tgc::OfferVerdict::NoImage:
                snprintf(detail, dn, ui().fwNoImageFmt, v, tgc::ota::buildEnv());
                break;
            case tgc::OfferVerdict::TooLarge:
                snprintf(detail, dn, ui().fwTooLargeFmt, v);
                break;
            case tgc::OfferVerdict::RolledBack:
                snprintf(detail, dn, ui().fwRolledBackFmt, v);
                break;
            default:
                break;
        }
    } else {
        *title = ui().fwNotChecked;
        *color = kTextDim;
    }
}

// The running version in the label face, the build environment right of it
// in a caption when both fit.
void drawVersionLine(lgfx::LovyanGFX& d, const tgc::layout::Rect& r) {
    using namespace tgc::theme;
    const char* v = tgc::firmwareVersion();
    const char* env = tgc::ota::buildEnv();
    const int vw = gRender.textWidth(v, tgc::Renderer::kTextUi);
    const int ew = wg::captionWidth(env);
    gRender.drawLabel(d, v, r.x, r.y, r.w, r.h, kText, kBg);
    if (vw + 8 + ew <= r.w)
        wg::drawCaption(d, gRender, env, r.right() - ew, r.y + (r.h - 8) / 2, kTextDim, kBg);
}

}  // namespace

// --- boot and loop -----------------------------------------------------------

void firmwareBoot() {
    gLayout = tgc::ota::layoutIsOurs();
    gPending = gLayout && tgc::ota::pendingVerify();
    refreshOverride();
    prefString(kPrefFwVersion, gFwBadge, sizeof gFwBadge);
    gBadgeBytes = gPrefs.isKey(kPrefFwBytes) ? gPrefs.getUInt(kPrefFwBytes, 0) : 0;
    prefString(kPrefOtaBad, gBad, sizeof gBad);
    const char* running = tgc::ota::runningSlot();
    if (!gLayout) {
        Serial.printf("[ota] no update slot on this layout (running %s): USB updates only\n",
                      running);
        return;
    }
    Serial.printf("[ota] %s (%s) in %s; update slot %s, %u bytes\n", tgc::firmwareVersion(),
                  tgc::ota::buildEnv(), running, tgc::ota::nextSlot(),
                  (unsigned)tgc::ota::slotBytes());
    char installing[24], slot[8];
    prefString(kPrefOtaNew, installing, sizeof installing);
    prefString(kPrefOtaSlot, slot, sizeof slot);
    if (gPending) {
        Serial.printf("[ota] first boot of this image: valid after %u s of the loop\n",
                      (unsigned)(kValidAfterMs / 1000));
    } else if (installing[0]) {
        if (std::strcmp(slot, running) == 0) {
            Serial.printf("[ota] %s installed over the air\n", installing);
        } else {
            // The install switched the boot slot to `slot` and the device runs
            // the other one: the new image did not get as far as markGood()
            // (a crash, a watchdog, a reset or a power cut before), and the
            // bootloader went back. Never offered again.
            Serial.printf("[ota] %s did not start in %s (otadata: %s invalid): back on %s\n",
                          installing, slot, tgc::ota::lastInvalidSlot(), tgc::firmwareVersion());
            snprintf(gBad, sizeof gBad, "%s", installing);
            gPrefs.putString(kPrefOtaBad, gBad);
            snprintf(gRolledBack, sizeof gRolledBack, "%s", installing);
        }
        clearInstallRecord();
    }
    // The badge goes once the device runs that version (or a newer one), and
    // after a rollback from it.
    if (gFwBadge[0] && (!newerThanRunning(gFwBadge) || std::strcmp(gFwBadge, gBad) == 0))
        saveBadge("", 0);
}

void firmwareRollbackNotice() {
    if (!gRolledBack[0]) return;
    char text[96];
    snprintf(text, sizeof text, ui().fwFailedFmt, gRolledBack, tgc::firmwareVersion());
    gRolledBack[0] = 0;
    showNotice(ui().fwFailedTitle, text, tgc::theme::kWarn);
    // A few seconds, or until a new press (a button down from before must be
    // released first); that press does not act on the next screen.
    bool released = !tgc::board::isHeld(Button::Select) && !tgc::board::isHeld(Button::Next);
    const uint32_t start = millis();
    while (millis() - start < kNoticeMs) {
        tgc::board::update();
        const bool down = tgc::board::isHeld(Button::Select) || tgc::board::isHeld(Button::Next);
        if (!down) released = true;
        tgc::KeyEvent ev;
        bool key = false;
        while (tgc::board::nextKeyEvent(&ev)) key = key || ev.down;
        if ((released && down) || key) break;
        delay(20);
    }
    gSwallowInput = true;
}

void firmwareTick(uint32_t now) {
    if (!gLoopAt) gLoopAt = now ? now : 1;
    if (gPending && now - gLoopAt >= kValidAfterMs) markGood("10 s of the loop");
}

// The bootloader rolls back on any reset of a pending image, the wake from a
// deep sleep included, so the image cannot stay pending through one: one that
// reached the loop (a deck open or the no-deck screens, the user's own sleep
// or the idle timer) counts as good. One that sleeps from inside the boot
// (halt(), the critical-battery re-sleep) does not, and rolls back.
void firmwareBeforeSleep() {
    if (gPending && gLoopAt) markGood("before a deep sleep");
}

void firmwareAfterCatalog(const tgc::FetchResult& catalog) {
    if (!gLayout) return;  // firmwareBoot() logged it
    refreshOverride();
    if (!readAllowed()) {
        Serial.printf("[ota] development build: no manifest read without a manifest URL\n");
        return;
    }
    // No server answered the catalog: the manifest would wait for the same
    // timeouts and fail the same way.
    if (catalog.status == tgc::FetchStatus::Connect || catalog.status == tgc::FetchStatus::Tls) {
        Serial.printf("[ota] no manifest read: the catalog's server was not reachable\n");
        return;
    }
    readManifest();
}

// --- values --------------------------------------------------------------------

bool firmwareUpdateKnown() { return offerKnown(); }

void firmwareRowValue(char* out, size_t n) {
    if (offerKnown())
        snprintf(out, n, ui().fwAvailableFmt, offerVersion());
    else
        snprintf(out, n, "%s", tgc::firmwareVersion());
}

const char* firmwareAvailable() { return offerKnown() ? offerVersion() : ""; }

bool firmwareOtaLayout() { return gLayout; }

bool firmwareBusy() { return gStep != Step::Idle; }

// --- Settings > Device > Firmware ------------------------------------------------

void openFirmware() {
    gScreen = Screen::Firmware;
    gStep = Step::Idle;
    gNoteTitle[0] = 0;
    refreshOverride();
    FwRow rows[2];
    gFwList.setCount(fwRows(rows));
    gFwList.reset(0);
    drawScreen();
}

void drawFirmware() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    FwRow rows[2];
    const int n = gStep == Step::Idle ? fwRows(rows) : 0;
    const tgc::layout::Firmware l = tgc::layout::firmwareFor(W, H, n);
    const bool transfer = gStep == Step::Downloading || gStep == Step::Verifying ||
                          gStep == Step::Restarting;
    menuHeader(d, ui().fwTitle, transfer ? gCheck.version.c_str() : nullptr, gHome.active());

    if (gStep == Step::Joining) {
        const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
        gRender.drawCentred(d, ui().netConnecting, 4, mid - 34, W - 8, 40, 2, kText, kBg,
                            tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        wg::Hints h;
        h.side = &tgc::kIconBack;
        h.sideWord = ui().hintCancel;
        drawHints(d, h);
        present();
        return;
    }

    drawVersionLine(d, l.version);
    auto status = [&](const char* text, uint16_t color) {
        gRender.drawWrapped(d, text, l.status.x, l.status.y, l.status.w, l.statusLines, color, kBg,
                            tgc::Renderer::kTextUi);
    };
    if (transfer) {
        char line[48];
        if (gStep == Step::Restarting)
            snprintf(line, sizeof line, ui().fwRestartingFmt, gCheck.version.c_str());
        else
            snprintf(line, sizeof line, "%s",
                     gStep == Step::Verifying ? ui().fwVerifying : ui().fwDownloading);
        status(line, gStep == Step::Restarting ? kOkHi : kText);
        if (gStep == Step::Restarting) {
            drawCaptionLines(d, ui().fwKeepsHint, l.detail.x, l.bar.y, l.detail.w, 3, kTextDim,
                             tgc::Align::Left);
        } else {
            const int pct = gNetPct < 0 ? 0 : gNetPct;
            wg::drawProgress(d, l.bar.x, l.bar.y, l.bar.w, l.bar.h, pct, 100);
            char done[12], total[12], numbers[48];
            const uint32_t bytes = gCheck.app.bytes;
            formatMb(static_cast<size_t>(static_cast<uint64_t>(bytes) * pct / 100u), false, done,
                     sizeof done);
            formatMb(bytes, false, total, sizeof total);
            snprintf(numbers, sizeof numbers, ui().fwProgressFmt, done, total, (unsigned)pct);
            gRender.drawLabel(d, numbers, l.bar.x, l.numbersY, l.bar.w, 8, kTextDim, kBg,
                              tgc::Renderer::kTextBuiltinSmall);
        }
        if (gStep == Step::Downloading) {
            wg::Hints h;
            h.side = &tgc::kIconBack;
            h.sideWord = ui().hintCancel;
            drawHints(d, h);
        }
        present();
        return;
    }
    if (gStep == Step::Checking) {
        status(ui().fwChecking, kText);
        present();
        return;
    }

    const char* title = "";
    uint16_t color = kText;
    char detail[128];
    idleStatus(&title, &color, detail, sizeof detail);
    status(title, color);
    drawCaptionLines(d, detail, l.detail.x, l.detail.y, l.detail.w, l.detailLines, kTextDim,
                     tgc::Align::Left);
    if (n == 0) {
        backHints(d);
        present();
        return;
    }
    gFwList.setCount(n);
    layoutList(gFwList, l.rowsTop, H - kHintH, layout().row1H);
    wg::drawList(d, gFwList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().fwRows[rows[i] == FwRow::Install ? 0 : 1];
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    wg::Hints h;
    h.tap = n > 1 ? ui().hintMove : nullptr;
    h.hold = rows[gFwList.focus()] == FwRow::Install ? ui().hintInstall : ui().hintCheck;
    h.side = &tgc::kIconBack;
    h.sideWord = ui().hintBack;
    drawHints(d, h);
    present();
}

// loop() on the Firmware screen: the join's polling, then tap moves, a hold
// installs or checks, the side button goes back (and cancels the join).
void firmwareLoop(int a, int b) {
    if (gStep == Step::Joining) {
        if (b) {
            sessionDone();
            return;
        }
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            sessionConnected();
        } else if (st != tgc::HomeWifi::State::Connecting) {
            setCpuFrequencyMhz(kCpuMhz);  // the radio is off already
            note(joinFailureText(), gHome.ssid(), tgc::theme::kWarn);
            sessionDone();
        }
        return;
    }
    if (gStep != Step::Idle) return;
    if (b) {
        backToMenu();
        return;
    }
    FwRow rows[2];
    const int n = fwRows(rows);
    gFwList.setCount(n);
    if (n == 0) return;
    if (a == 1 && n > 1) {
        gFwList.next();
        drawScreen();
    } else if (a == 2) {
        startSession(rows[gFwList.focus()] == FwRow::Install);
    }
}

}  // namespace app
}  // namespace tgc
