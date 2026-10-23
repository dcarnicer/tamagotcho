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

tgc::KeyRepeat gKeyRepeat;      // the held arrow of a keyboard (keyTurn())

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
        dateLoop(a, b);
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
