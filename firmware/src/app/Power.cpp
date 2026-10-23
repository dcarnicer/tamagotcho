// Power: dim, sleep, wake and the battery guard; see Power.h.
#include "app/Power.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "app/App.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/Display.h"
#include "app/GetDecks.h"
#include "app/Net.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "app/WifiMenu.h"
#include "board/Board.h"
#include "power/BatteryGuard.h"

// RTC slow memory survives deep sleep but not a power cut: when the chip went to
// sleep, by the RTC timer, so the next boot can add the slept time to the SRS
// day clock (see applySleepTime()).
RTC_DATA_ATTR uint64_t gSleptAtUs = 0;
RTC_DATA_ATTR uint32_t gSleepMagic = 0;
constexpr uint32_t kSleepMagic = 0x51EE9A7Eu;
// Set when the last sleep was forced by a critical battery, so the wake can
// check the cell before anything else; plus the UI language of the moment, for
// the "battery low" notice shown before a pack (and its language) is loaded.
RTC_DATA_ATTR bool gSleptCritical = false;
RTC_DATA_ATTR bool gSleepUiEs = false;
// The boot-time date question (after a power cut) was still open when the chip
// went to sleep: the wake asks it again instead of silently dropping it.
RTC_DATA_ATTR bool gDatePending = false;

namespace tgc {
namespace app {

uint32_t gLastInputAt = 0;      // millis() of the last button activity
bool gDimmed = false;
bool gScreenOff = false;        // idle on USB power: panel off, still running
bool gSwallowInput = false;     // drop button events until all buttons are up

namespace {

uint32_t gLastPowerCheck = 0;
// Critical battery (power/BatteryGuard.h): one reading every kBatteryGuardMs on
// every screen, so three low readings put the device to sleep within ~30 s.
constexpr uint32_t kBatteryGuardMs = 10000;
tgc::BatteryGuard gBatteryGuard;
uint32_t gBatteryGuardAt = 0;

}  // namespace

#if defined(TGC_BATT_TEST)
// Test-only log in LittleFS: one line per event, so the unplugged run leaves
// evidence. It is printed over USB at boot and once a minute while awake.
uint32_t gBattLogAt = 0;
int gBattWakePct = -2;  // the best reading of a critical wake, logged after mount
bool gBattWakeUsb = false;
void battLog(const char* what, int pct) {
    if (pct == -2) pct = tgc::board::batteryPercent();
    File f = LittleFS.open("/batt.log", FILE_APPEND);
    if (!f) return;
    f.printf("%lu ms %d%%%s %s\n", (unsigned long)millis(), pct,
             tgc::board::externalPower() ? " usb" : "", what);
    f.close();
}
void battLogPrint() {
    File f = LittleFS.open("/batt.log", FILE_READ);
    if (!f) return;
    Serial.println("--- /batt.log ---");
    while (f.available()) Serial.write(static_cast<uint8_t>(f.read()));
    Serial.println("--- end ---");
    f.close();
}
#endif

// Critical-battery guard (power/BatteryGuard.h): one reading every
// kBatteryGuardMs, from loop() and during a download. True when the device must
// save and sleep now.
bool batteryCritical(uint32_t now) {
    if (now - gBatteryGuardAt < kBatteryGuardMs) return false;
    gBatteryGuardAt = now;
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    return gBatteryGuard.update(tgc::board::batteryPercent(), external);
}

// Adds the time spent in deep sleep (RTC timer) to the SRS day clock. Only a
// wake-up from our own deepSleep() qualifies: after a power cut both the RTC
// timer and RTC memory are reset, and a crash reset is not a wake-up.
void applySleepTime() {
    const bool slept = tgc::board::wokeFromSleep() && gSleepMagic == kSleepMagic;
    gSleepMagic = 0;
    if (!slept) return;
    const uint64_t now = tgc::board::rtcMicros();
    if (now <= gSleptAtUs) return;
    const uint64_t ms = (now - gSleptAtUs) / 1000u;
    gSrs.addElapsedMs(ms);
    Serial.printf("power: woke after %lu s, day %u\n", (unsigned long)(ms / 1000u),
                  (unsigned)gSrs.today());
}

namespace {

// Leaves what the next wake needs in RTC memory: when the chip went to sleep
// (for applySleepTime) and whether the boot-time date question is still open.
// Called by prepareDeepSleep() right after the save, so the saved day carry and
// this timestamp describe the same moment and the wake adds exactly the time
// since (a notice shown before the sleep included).
void armSleep() {
    gSleptAtUs = tgc::board::rtcMicros();
    gSleepMagic = kSleepMagic;
    gDatePending = gScreen == Screen::Date && gAfterDate == Screen::Quiz;
}

// Persists the SRS, which also persists the uptime day carry, and stops the
// portal and the home Wi-Fi. A store that failed to load and has no answers is
// not written, so a damaged file (and its .bak) stays on disk.
void saveAndRadiosOff() {
    if (gDirty || gSrsLoaded) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    if (gPortal.active()) {
        gPortal.end();
        setCpuFrequencyMhz(kCpuMhz);
        QLOG("W wifi: stopped\n");
    }
    if (gHome.state() != tgc::HomeWifi::State::Off) QLOG("N home wifi: stopped\n");
    netRadioOff();  // safe in any state
}

// What every deep sleep of the running app needs first: the state saved, the
// radios off and the RTC timestamp the wake adds to the day clock. Used by the
// idle sleep on battery, the sleep after unplugging a dark panel and the
// explicit sleep. Not by the critical-battery re-sleep at boot, which keeps the
// first sleep's timestamp, nor by halt(), where the SRS may not be loaded.
void prepareDeepSleep() {
    saveAndRadiosOff();
    armSleep();
}

// Idle timeout reached: on battery, deep-sleep. On USB power only the panel
// goes dark (the serial port stays up), after the same save and radio stop;
// powerTick() sleeps once it is unplugged. Returns only in the USB case.
void goIdle() {
    if (!tgc::board::externalPower()) {
        prepareDeepSleep();
        QLOG("P deep sleep\n");
        tgc::board::deepSleep();
    }
    saveAndRadiosOff();
    tgc::board::setDisplayOn(false);
    gScreenOff = true;
    gDimmed = false;
    gLastPowerCheck = millis();
    QLOG("P screen off (usb)\n");
}

}  // namespace

// First input after the panel went dark or dim: restore it. A Wi-Fi screen whose
// portal the idle timeout stopped gets its portal back.
void wakeScreen() {
    if (gScreenOff) {
        tgc::board::setDisplayOn(true);
        gScreenOff = false;
        // The question was not visible while dark: restart its answer clock so
        // the time away does not grade it Hard.
        if (gScreen == Screen::Quiz && !gRevealed) {
            gShownAt = millis();
            gNavTaps = 0;
        }
        if (gScreen == Screen::Wifi && gWifiStep != WifiStep::Failed && !gPortal.active()) {
            openPortal(gPortalMode);  // the idle timeout stopped the portal: start it again
        } else if (gScreen == Screen::NetTest &&
                   (gTestStep == NetTestStep::Joining || gTestStep == NetTestStep::Checking)) {
            openNetTest();  // the idle timeout cut the test short: run it again
        } else if (gScreen == Screen::GetDecks && gNetStep != NetStep::Error && !gHome.active()) {
            openGetDecks(gNetFromBoot, gNetUpdatesOnly);  // the idle timeout turned the radio off: join again
        } else {
            drawScreen();
        }
    }
    tgc::board::setBrightness(gBrightness);  // also undoes the dim level
    gDimmed = false;
}

// Explicit sleep: the power button, the "Sleep" settings row, or a critical
// battery. Saves like goIdle(), but deep-sleeps even on USB power -- the user
// asked for it (or the cell is about to die) -- after a short notice that says
// how to wake up. The RTC timestamp keeps the day clock running through it.
void sleepNow(bool critical) {
    // Saved before the notice: the first click of a double click (power off)
    // already gets the state onto the flash.
    prepareDeepSleep();
    QLOG("P deep sleep (%s)\n", critical ? "battery" : "user");
    if (gScreenOff) tgc::board::setDisplayOn(true);
    tgc::board::setBrightness(gBrightness);
    if (critical)
        showNotice(ui().batteryTitle, keysOr(ui().batteryHint, ui().batteryHintKeys),
                   tgc::theme::kWarn);
    else
        showNotice(ui().sleepTitle, keysOr(ui().sleepHint, ui().sleepHintKeys), kWhite);
    delay(critical ? kBatteryNoticeMs : kSleepNoticeMs);
    gSleptCritical = critical;
    gSleepUiEs = gUi == &kUiEs;
#if defined(TGC_BATT_TEST)
    if (critical) battLog("critical sleep");
#endif
    tgc::board::deepSleep();
}

// Called once per loop() with the current time: dims, then idles.
void powerTick(uint32_t now) {
    if (gScreenOff) {
        // Unplugged while dark: nothing keeps it awake any more. The day carry
        // kept running in RAM since goIdle() saved (a night on the charger is
        // hours of it, maybe the 04:00 rollover), so save again first.
        if (now - gLastPowerCheck >= kPowerCheckMs) {
            gLastPowerCheck = now;
            if (!tgc::board::externalPower()) {
                prepareDeepSleep();
                QLOG("P deep sleep (unplugged)\n");
                tgc::board::deepSleep();
            }
        }
        return;
    }
    uint32_t idle = now - gLastInputAt;
    if (gPortal.active()) {
        const uint32_t http = now - gPortal.lastActivityMs();
        if (http < idle) idle = http;
    }
    // A download, catalog read or join counts as activity, and so does the end
    // of the date screen's network-time attempt (a new status line to read).
    // A join holds them off for as long as it takes: up to 40 s
    // (HomeWifi::kConnectTimeoutMs), longer than the dim.
    if (gHome.active() || gDateSync != DateSync::None) {
        if (gHome.state() == tgc::HomeWifi::State::Connecting) gNetActivityAt = now;
        const uint32_t net = now - gNetActivityAt;
        if (net < idle) idle = net;
    }
    if (!gDimmed && idle >= kDimAfterMs) {
        uint8_t dim = gBrightness / 4;
        if (dim < 8) dim = 8;
        tgc::board::setBrightness(dim);
        gDimmed = true;
    }
    if (idle >= (gPortal.active() ? kWifiIdleMs : kSleepAfterMs)) goIdle();
}

// Fatal error screen. Nothing to save here (the SRS may not even be loaded), so
// on battery it just sleeps after the usual timeout; a wake shows it again.
void halt() {
    const uint32_t start = millis();
    for (;;) {
        tgc::board::update();
        if (millis() - start >= kSleepAfterMs && !tgc::board::externalPower())
            tgc::board::deepSleep();
        if (tgc::board::powerButtonClicked()) tgc::board::deepSleep();  // no click reset now
        delay(50);
    }
}

}  // namespace app
}  // namespace tgc
