// Waveshare ESP32-S3-Touch-AMOLED-2.06.
//
// ESP32-S3R8 (8 MB PSRAM, 32 MB flash), 2.06" 410x502 AMOLED over QSPI
// (CO5300), FT3168 touch, AXP2101 PMIC, QMI8658C IMU, PCF85063 RTC, a 1-bit
// SDMMC TF card slot, and two side buttons: BOOT (GPIO0, active low) and PWR
// (the AXP2101's power key; a 6 s hold powers off, and a short press only
// powers the board on from off).
//
// The display pins are Waveshare's BSP:
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06
// M5GFX's Panel_CO5300 carries the panel's init list; its column offset
// (22 px) and size are set below for this panel instead of the T-Watch
// Ultra's 502x410 memory layout. The QSPI bus is the panel's own (SPI2).
#include "board/Board.h"

#if defined(TGC_BOARD_AMOLED206)
#include <Arduino.h>
#include <HWCDC.h>
#include <Wire.h>
#include <cstdlib>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <lgfx/v1/panel/Panel_CO5300.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <soc/esp32s3/rtc.h>

namespace tgc {
namespace board {
namespace {

// --- Display -----------------------------------------------------------------
// Waveshare's QSPI pins; the panel has no backlight pin (brightness is a CO5300
// command, which Panel_AMOLED::setBrightness sends).
constexpr int kPinSclk = 11;
constexpr int kPinD0 = 4;
constexpr int kPinD1 = 5;
constexpr int kPinD2 = 6;
constexpr int kPinD3 = 7;
constexpr int kPinCs = 12;
constexpr int kPinRst = 8;
constexpr int kPanelW = 410;
constexpr int kPanelH = 502;
constexpr int kColumnOffset = 22;  // the panel's visible columns start at 0x16

// M5GFX's Panel_CO5300 list is the LilyGO T-Watch Ultra's. Use Waveshare's BSP
// list instead: the same controller, but the 0x44 tear line and the sleep-out
// delay the CO5300 needs before display-on. Without the delay the panel
// sometimes comes up all yellow after a reset.
class WaveshareCO5300 : public lgfx::Panel_CO5300 {
public:
    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0xFE, 1, 0x00,                    // page 0
            0x11, 0x80, 120,                  // sleep out, 120 ms
            0xC4, 1, 0x80,
            0x44, 2, 0x01, 0xD1,              // tear effect line 465
            0x35, 1, 0x00,                    // TE off
            0x53, 1, 0x20,
            0x63, 1, 0xFF,
            0x51, 1, 0x00,                    // brightness 0 until the app sets it
            0x3A, 1, 0x55,                    // RGB565
            0x2A, 4, 0x00, 0x16, 0x01, 0xAF,  // columns 22..431
            0x2B, 4, 0x00, 0x00, 0x01, 0xF5,  // rows 0..501
            0x29, 0x80, 10,                   // display on, 10 ms
            0xFF, 0xFF,
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class Amoled206Panel : public lgfx::LGFX_Device {
public:
    Amoled206Panel() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000;  // Waveshare's BSP uses 40 MHz; 20 is safer here
            cfg.freq_read = 20000000;
            cfg.spi_mode = 0;
            cfg.pin_sclk = kPinSclk;
            cfg.pin_io0 = kPinD0;
            cfg.pin_io1 = kPinD1;
            cfg.pin_io2 = kPinD2;
            cfg.pin_io3 = kPinD3;
            cfg.spi_host = SPI2_HOST;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = kPinCs;
            cfg.pin_rst = kPinRst;
            cfg.pin_busy = -1;
            cfg.panel_width = kPanelW;
            cfg.panel_height = kPanelH;
            cfg.memory_width = kPanelW;
            cfg.memory_height = kPanelH;
            cfg.offset_x = kColumnOffset;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 1;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }

private:
    WaveshareCO5300 _panel;
    lgfx::Bus_SPI _bus;
};

Amoled206Panel gDisplay;
uint8_t gBrightness = 160;

// --- Buttons -----------------------------------------------------------------
// BOOT is a plain active-low GPIO. PWR's GPIO10 line is meant to follow the
// AXP2101's power key, but on this unit a short press does not move it; the
// PMIC still sees the 6 s hold that powers off. Keep the pin wired for boards
// where it works; the touch layer will be the real second input.
struct Pin {
    uint8_t gpio;
    bool activeHigh;
    bool down = false;
    bool pressedEdge = false;
    bool holdFired = false;
    uint32_t downSince = 0;
    uint32_t lastChange = 0;
    bool holdEdge = false;
};

Pin gSelect{0, false};   // BOOT, active low
Pin gNext{10, true};     // PWR, high while pressed

Pin& pin(Button b) { return b == Button::Next ? gNext : gSelect; }

void poll(Pin& p, uint32_t now) {
    p.pressedEdge = false;
    p.holdEdge = false;
    const bool level = digitalRead(p.gpio) == HIGH;
    const bool raw = p.activeHigh ? level : !level;
    // 20 ms debounce on state changes.
    if (raw != p.down && now - p.lastChange >= 20) {
        p.down = raw;
        p.lastChange = now;
#if defined(TGC_BTN_LOG)
        Serial.printf("btn %u %s level=%d\n", p.gpio, raw ? "down" : "up", level);
#endif
        if (raw) {
            p.pressedEdge = true;
            p.downSince = now;
            p.holdFired = false;
        }
    }
    if (p.down && !p.holdFired && now - p.downSince >= kHoldMs) {
        p.holdFired = true;
        p.holdEdge = true;
    }
}

// --- AXP2101 PMIC ------------------------------------------------------------
// Only what the app needs: battery level, charging and VBUS presence. The
// PMIC's rails are already on at boot, as on Waveshare's examples. The I2C bus
// (SCL 14, SDA 15) also carries the touch, IMU and RTC.
constexpr uint8_t kPmicAddr = 0x34;
constexpr uint8_t kPmicChipIdReg = 0x03;
constexpr uint8_t kPmicChipId = 0x4A;
bool gPmic = false;

bool pmicRead(uint8_t reg, uint8_t* out) {
    Wire.beginTransmission(kPmicAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<uint8_t>(kPmicAddr), static_cast<uint8_t>(1)) != 1)
        return false;
    *out = Wire.read();
    return true;
}

bool pmicRead14(uint8_t reg, uint16_t* out) {
    Wire.beginTransmission(kPmicAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<uint8_t>(kPmicAddr), static_cast<uint8_t>(2)) != 2)
        return false;
    const uint8_t hi = Wire.read();
    const uint8_t lo = Wire.read();
    *out = static_cast<uint16_t>(((hi << 8) | lo) & 0x3FFF);
    return true;
}

void initPmic() {
    // 100 kHz: the FT3168 touch controller on the same bus does not like the
    // 400 kHz the PMIC and IMU tolerate, and the traffic here is tiny.
    Wire.begin(15, 14, 100000);  // SDA, SCL; Waveshare's internal bus
    uint8_t id = 0;
    if (pmicRead(kPmicChipIdReg, &id) && id == kPmicChipId) {
        gPmic = true;
        Serial.println("pmic: AXP2101 ready");
    } else {
        Serial.println("pmic: AXP2101 not found");
    }
}

// --- Touch (FT3168) ----------------------------------------------------------
// The FT3168 sits at 0x38 on the same bus. Gestures become the app's two
// buttons: tap/hold anywhere is Select (front), a left swipe is Next (the side
// button's back) and a down swipe is a Next hold (the settings home). The app
// consumes the queued events from loop(); the touch never shows the hold fill,
// which stays a physical button's.
constexpr uint8_t kTouchAddr = 0x38;
constexpr int kTouchRst = 9;
constexpr int kTouchInt = 38;
constexpr int kSwipePx = 40;  // physical pixels before a move is a swipe

bool gTouch = false;
bool gTouchDown = false;
uint32_t gTouchDownAt = 0;
int gTouchStartX = 0, gTouchStartY = 0, gTouchLastX = 0, gTouchLastY = 0;
bool gTouchMoved = false;
bool gTouchHoldFired = false;
struct TouchEvent {
    int event = 0;  // 0 none, 1 tap, 2 hold
    uint32_t at = 0;
};
TouchEvent gTouchEvents[2];  // Select, Next

bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t v) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(v);
    return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* data, uint8_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, n) != n) return false;
    for (uint8_t i = 0; i < n; ++i) data[i] = Wire.read();
    return true;
}

bool initTouch() {
    pinMode(kTouchRst, OUTPUT);
    digitalWrite(kTouchRst, LOW);
    delay(10);
    digitalWrite(kTouchRst, HIGH);
    delay(50);
    pinMode(kTouchInt, INPUT_PULLUP);
    // The threshold set of esp_lcd_touch_ft5x06 (Espressif, Apache-2.0).
    static constexpr uint8_t kInit[][2] = {
        {0x80, 70}, {0x81, 60}, {0x82, 16}, {0x83, 60}, {0x84, 10},
        {0x85, 20}, {0x87, 2},  {0x88, 12}, {0x89, 40},
    };
    bool ok = true;
    for (const auto& p : kInit) ok = i2cWrite(kTouchAddr, p[0], p[1]) && ok;
    Serial.printf("touch: FT3168 %s\n", ok ? "ready" : "not found");
    return ok;
}

// A touch point, if one is down. The FT5x06 family reports up to five; the
// first is enough for a gesture.
bool readTouch(int* x, int* y) {
    uint8_t n = 0;
    if (!i2cRead(kTouchAddr, 0x02, &n, 1)) return false;
    n &= 0x0F;
    if (n == 0) return false;
    uint8_t d[6] = {};
    if (!i2cRead(kTouchAddr, 0x03, d, 6)) return false;
    *x = ((d[0] & 0x0F) << 8) | d[1];
    *y = ((d[2] & 0x0F) << 8) | d[3];
    return true;
}

void queueTouch(Button b, int event, uint32_t at) {
    TouchEvent& q = gTouchEvents[b == Button::Next ? 1 : 0];
    if (q.event == 0) {
        q.event = event;
        q.at = at;
    }
}

void touchTick(uint32_t now) {
    if (!gTouch) return;
    int x = 0, y = 0;
    const bool down = readTouch(&x, &y);
    if (down) {
        if (!gTouchDown) {
            gTouchDown = true;
            gTouchDownAt = now;
            gTouchStartX = gTouchLastX = x;
            gTouchStartY = gTouchLastY = y;
            gTouchMoved = false;
            gTouchHoldFired = false;
        } else {
            gTouchLastX = x;
            gTouchLastY = y;
            if (std::abs(x - gTouchStartX) >= kSwipePx ||
                std::abs(y - gTouchStartY) >= kSwipePx)
                gTouchMoved = true;
            if (!gTouchMoved && !gTouchHoldFired && now - gTouchDownAt >= kHoldMs) {
                gTouchHoldFired = true;
                queueTouch(Button::Select, 2, gTouchDownAt);
            }
        }
    } else if (gTouchDown) {
        gTouchDown = false;
        if (gTouchHoldFired) return;
        const int dx = gTouchLastX - gTouchStartX;
        const int dy = gTouchLastY - gTouchStartY;
        if (gTouchMoved) {
            if (std::abs(dx) > std::abs(dy)) {
                if (dx <= -kSwipePx) queueTouch(Button::Next, 1, gTouchDownAt);
            } else if (dy >= kSwipePx) {
                queueTouch(Button::Next, 2, gTouchDownAt);
            }
        } else {
            queueTouch(Button::Select, 1, gTouchDownAt);
        }
    }
}

}  // namespace

void begin() {
    // A wake from deepSleep() leaves BOOT in the RTC domain; give it back.
    rtc_gpio_deinit(static_cast<gpio_num_t>(gSelect.gpio));
    pinMode(gSelect.gpio, INPUT_PULLUP);
    pinMode(gNext.gpio, INPUT);
#if defined(TGC_BTN_LOG)
    Serial.printf("btn: boot=%d pwr=%d (gpio %u/%u)\n", digitalRead(gSelect.gpio),
                  digitalRead(gNext.gpio), gSelect.gpio, gNext.gpio);
#endif
    initPmic();

    // A longer reset than M5GFX's own: the CO5300 can otherwise come up all
    // yellow after a reset, before its sleep-out delay has run.
    pinMode(kPinRst, OUTPUT);
    digitalWrite(kPinRst, LOW);
    delay(20);
    digitalWrite(kPinRst, HIGH);
    delay(120);

    Serial.println("display: init");
    const bool displayOk = gDisplay.init();
    Serial.printf("display: %s, %dx%d\n", displayOk ? "ok" : "FAILED", gDisplay.width(),
                  gDisplay.height());
    gDisplay.setRotation(0);  // native 410x502 portrait
    gDisplay.setBrightness(gBrightness);
    gTouch = initTouch();
    Serial.printf("board: %s, panel %dx%d, ui %dx%d at %dx, psram %u KB\n", name(),
                  gDisplay.width(), gDisplay.height(), width(), height(), uiScale(),
                  static_cast<unsigned>(ESP.getPsramSize() / 1024));
}

void update() {
    const uint32_t now = millis();
    poll(gSelect, now);
    poll(gNext, now);
    touchTick(now);
}

lgfx::LGFX_Device& display() { return gDisplay; }

// The panel is twice the UI's design size and has rounded corners: draw at
// 193x239 logical pixels (2x) inside a 12 px physical border. The black border
// also covers the panel's corners.
int uiScale() { return 2; }
int uiInset() { return 12; }
int width() { return (gDisplay.width() - 2 * uiInset()) / uiScale(); }
int height() { return (gDisplay.height() - 2 * uiInset()) / uiScale(); }

// Rotation 0 = native portrait, 1 = landscape. width()/height() follow it.
void setOrientation(bool landscape) { setRotation(landscape ? 1 : 0); }
bool landscape() { return gDisplay.width() > gDisplay.height(); }
void setRotation(uint8_t r) { gDisplay.setRotation(r & 3); }
uint8_t rotation() { return gDisplay.getRotation() & 3; }

// The QMI8658C is wired (INT on GPIO21) but its driver is not here yet.
bool hasAccel() { return false; }
bool readAccel(float*, float*, float*) { return false; }

bool wasPressed(Button b) { return pin(b).pressedEdge; }
bool isHeld(Button b) { return pin(b).down; }
bool wasHeld(Button b) { return pin(b).holdEdge; }

bool takeButtonEvent(Button b, int* event, uint32_t* pressedAt) {
    TouchEvent& q = gTouchEvents[b == Button::Next ? 1 : 0];
    if (q.event == 0) return false;
    *event = q.event;
    *pressedAt = q.at;
    q.event = 0;
    return true;
}

// AXP2101 battery voltage, mapped like the other boards (3.3 V = 0 %,
// 4.15 V = 100 %). The gauge register (0xA4) needs its own enable sequence and
// reads 0 until then, so the ADC is the safer first source.
int batteryPercent() {
    if (!gPmic) return -1;
    uint16_t mv = 0;
    if (!pmicRead14(0x34, &mv) || mv < 2500 || mv > 4500) return -1;
    int pct = (static_cast<int>(mv) - 3300) * 100 / (4150 - 3300);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

bool isCharging() {
    if (!gPmic) return false;
    uint8_t st = 0;
    return pmicRead(0x01, &st) && ((st >> 5) & 0x3) == 0x1;
}

void setBrightness(uint8_t level) {
    gBrightness = level;
    gDisplay.setBrightness(level);
}

bool wokeFromSleep() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0; }

// VBUS good from the PMIC; the USB-Serial/JTAG link is the fallback when the
// PMIC did not answer.
bool externalPower() {
    if (gPmic) {
        uint8_t st = 0;
        if (pmicRead(0x00, &st)) return (st & 0x20) != 0;
    }
    return HWCDC::isPlugged();
}

void setDisplayOn(bool on) {
    if (on) {
        gDisplay.wakeup();
        gDisplay.setBrightness(gBrightness);
    } else {
        gDisplay.setBrightness(0);
        gDisplay.sleep();
    }
}

void deepSleep() {
    gDisplay.setBrightness(0);
    gDisplay.sleep();
    gDisplay.waitDisplay();
    const gpio_num_t sel = static_cast<gpio_num_t>(gSelect.gpio);
    // Wait for BOOT to be released, or the wake level would be present at once.
    while (digitalRead(sel) == LOW) delay(10);
    rtc_gpio_pullup_en(sel);
    rtc_gpio_pulldown_dis(sel);
    esp_sleep_enable_ext0_wakeup(sel, 0);
    Serial.flush();
    esp_deep_sleep_start();
}

uint64_t rtcMicros() { return esp_rtc_get_time_us(); }
// The AXP2101 owns the PWR key (6 s to power off); the settings' Sleep row and
// the idle timer are the ways to deep-sleep. BOOT wakes the chip.
bool powerButtonClicked() { return false; }
uint32_t randomSeed() { return esp_random(); }
const char* name() { return "Waveshare AMOLED 2.06"; }

}  // namespace board
}  // namespace tgc
#endif
