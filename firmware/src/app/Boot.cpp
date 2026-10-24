// The boot and the first-boot language picker; see Boot.h.
#include "app/Boot.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>

#include "Credits.h"
#include "Version.h"
#include "app/App.h"
#include "app/CardMove.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/Firmware.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "app/Wifi.h"
#include "board/Board.h"
#include "platform/FsQuiet.h"
#include "platform/NetFetch.h"
#include "platform/Storage.h"
#include "platform/WifiPortal.h"
#include "power/BatteryGuard.h"
#include "render/Widgets.h"
#include "ui/DeckSummary.h"
#include "ui/InputPrefs.h"
#include "ui/UiLang.h"

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

namespace {

uint16_t kGrey, kHighlight, kGreen, kRed, kYellow;

}  // namespace

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

namespace {

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
    // A new image that cannot mount it goes back to the previous one first.
    firmwareFailedBoot();
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

}  // namespace

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

// setup(): from the board's start to the first screen (see Boot.h).
void boot() {
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
    // The flash layout, a new image's first boot, how the last install went.
    firmwareBoot();
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
    // Learning mode, before the first rebuild (loadPack() below).
    gIntroOn = gPrefs.getUChar(kPrefIntro, 1) != 0;
    gQueue.setLearning(learnOptions());
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
    // check. Nothing can be in flight at boot. (The card's go whenever it
    // mounts.)
    tgc::storage::removeTemps(tgc::DeckPlace::Device);
    if (tgc::fsRemoveIfExists(tgc::WifiPortal::kProgressTmp))
        Serial.printf("fs: removed stale %s\n", tgc::WifiPortal::kProgressTmp);
    // The microSD card, on a board with a slot (docs/microsd.md §3.1), before
    // any deck opens: the decks on LittleFS move to it first (§3.6), and NVS
    // `pack` follows the one it names.
    tgc::storage::begin();
    moveDecksToCard();
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
        firmwareRollbackNotice();
        if (tgc::firstBootLangPicker(uiPrefStored, !gPacks.empty())) {
            Serial.println("ui: first boot, asking for the language");
            openFirstLang();
            return;
        }
        openBootUploadScreen();
        return;
    }

    // The saved deck; if it is gone, the same deck in the other place (a card
    // that is out, a deck the move could not take), else the first one.
    const String saved = gPrefs.getString("pack", "");
    String want = saved;
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) {
        const String same = sameDeckIn(gPacks, want);
        want = same.length() ? same : gPacks[0];
    }
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
    // A saved deck on a card that is not in stays saved: with the card back,
    // the next boot opens it again.
    if (!(tgc::storage::onCard(saved) && !tgc::storage::cardMounted()))
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

    // After a rollback, say so once (in the deck's language, now known).
    firmwareRollbackNotice();

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
    showNextItem();
}

}  // namespace app
}  // namespace tgc
