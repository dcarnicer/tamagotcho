// LilyGO T-Display-S3 implementation via LovyanGFX.
// Panel pins validated on hardware in the gr-iiix-liveview project.
// Buttons: GPIO0 (BOOT) = Select, GPIO14 = Next; both active-low.
#include "board/Board.h"

#if defined(TGC_BOARD_TDISPLAY_S3)
#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <soc/esp32s3/rtc.h>

namespace tgc {
namespace board {

namespace {

class TDisplayS3Panel : public lgfx::LGFX_Device {
public:
    static constexpr int kPowerOnPin = 15;  // gates LCD power; must be high before init
    TDisplayS3Panel() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000;  // 40 MHz blanked the panel on real hardware
            cfg.pin_wr = 8;
            cfg.pin_rd = 9;
            cfg.pin_rs = 7;
            cfg.pin_d0 = 39;
            cfg.pin_d1 = 40;
            cfg.pin_d2 = 41;
            cfg.pin_d3 = 42;
            cfg.pin_d4 = 45;
            cfg.pin_d5 = 46;
            cfg.pin_d6 = 47;
            cfg.pin_d7 = 48;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 6;
            cfg.pin_rst = 5;
            cfg.pin_busy = -1;
            cfg.panel_width = 170;
            cfg.panel_height = 320;
            cfg.offset_x = 35;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 38;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }

private:
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_Parallel8 _bus;
    lgfx::Light_PWM _light;
};

TDisplayS3Panel gDisplay;

struct Pin {
    uint8_t gpio;
    bool down = false;
    bool prevDown = false;
    bool pressedEdge = false;
    bool holdFired = false;
    uint32_t downSince = 0;
    uint32_t lastChange = 0;
    bool holdEdge = false;
};
Pin gNext{14};
Pin gSelect{0};
uint8_t gBrightness = 160;

Pin& pin(Button b) { return b == Button::Next ? gNext : gSelect; }

void poll(Pin& p, uint32_t now) {
    p.pressedEdge = false;
    p.holdEdge = false;
    bool raw = digitalRead(p.gpio) == LOW;
    // 20 ms debounce on state changes.
    if (raw != p.down && now - p.lastChange >= 20) {
        p.down = raw;
        p.lastChange = now;
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

}  // namespace

void begin() {
    pinMode(TDisplayS3Panel::kPowerOnPin, OUTPUT);
    digitalWrite(TDisplayS3Panel::kPowerOnPin, HIGH);
    pinMode(gNext.gpio, INPUT_PULLUP);
    pinMode(gSelect.gpio, INPUT_PULLUP);
    gDisplay.init();
    gDisplay.setRotation(0);  // native 170x320 portrait
    gDisplay.setBrightness(gBrightness);
}

void update() {
    uint32_t now = millis();
    poll(gNext, now);
    poll(gSelect, now);
}

lgfx::LGFX_Device& display() { return gDisplay; }
int width() { return gDisplay.width(); }
int height() { return gDisplay.height(); }

// Rotation 0 = native portrait, 1 = landscape. width()/height() follow it.
void setOrientation(bool landscape) { setRotation(landscape ? 1 : 0); }
bool landscape() { return gDisplay.width() > gDisplay.height(); }
void setRotation(uint8_t r) { gDisplay.setRotation(r & 3); }
uint8_t rotation() { return gDisplay.getRotation() & 3; }

// No IMU on this board: the app does not offer the Auto orientation.
bool hasAccel() { return false; }
bool readAccel(float*, float*, float*) { return false; }

bool wasPressed(Button b) { return pin(b).pressedEdge; }
bool isHeld(Button b) { return pin(b).down; }
bool wasHeld(Button b) { return pin(b).holdEdge; }

int batteryPercent() { return -1; }  // no fuel gauge; ADC divider not read yet
bool isCharging() { return false; }

void setBrightness(uint8_t level) {
    gBrightness = level;
    gDisplay.setBrightness(level);
}

bool wokeFromSleep() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0; }
bool externalPower() { return false; }  // no VBUS sense wired up yet

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
    const gpio_num_t sel = static_cast<gpio_num_t>(gSelect.gpio);  // GPIO0, active low
    gDisplay.setBrightness(0);
    gDisplay.sleep();
    digitalWrite(TDisplayS3Panel::kPowerOnPin, LOW);  // cut LCD power
    while (digitalRead(sel) == LOW) delay(10);
    rtc_gpio_pullup_en(sel);
    rtc_gpio_pulldown_dis(sel);
    esp_sleep_enable_ext0_wakeup(sel, 0);
    Serial.flush();
    esp_deep_sleep_start();
}

uint64_t rtcMicros() { return esp_rtc_get_time_us(); }
uint32_t randomSeed() { return esp_random(); }

}  // namespace board
}  // namespace tgc
#endif
