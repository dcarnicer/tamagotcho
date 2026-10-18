// M5Stack StickS3 implementation via M5Unified / M5GFX.
//
// Buttons (verified in M5Unified.cpp): BtnA = GPIO11 = front (big) button,
// BtnB = GPIO12 = side button. Mapping: A = Select, B = Next.
#include "board/Board.h"

#if defined(TGC_BOARD_STICKS3)
#include <M5Unified.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <soc/esp32s3/rtc.h>

namespace tgc {
namespace board {

namespace {
m5::Button_Class& btn(Button b) { return b == Button::Select ? M5.BtnA : M5.BtnB; }

constexpr gpio_num_t kSelectPin = GPIO_NUM_11;  // BtnA, front button, active low
// M5GFX drives the StickS3 backlight (GPIO38) from LEDC channel 7 at 256 Hz with
// 9-bit duty. 256 Hz is audible: the current pulses make the charger's inductor
// buzz while on USB. Moved above the audible range.
constexpr uint8_t kBacklightChannel = 7;
constexpr uint8_t kBacklightBits = 9;
constexpr uint32_t kBacklightHz = 25000;
uint8_t gBrightness = 128;
}  // namespace

void begin() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(0);
    // StickS3 panel is natively portrait (135x240); make sure of it.
    if (M5.Display.width() > M5.Display.height()) M5.Display.setRotation(1);
    ledcChangeFrequency(kBacklightChannel, kBacklightHz, kBacklightBits);
    M5.Display.setBrightness(gBrightness);
    M5.BtnA.setHoldThresh(kHoldMs);
    M5.BtnB.setHoldThresh(kHoldMs);

    // M5Unified probes for an I2C RTC at boot (BM8563/PCF8563 at 0x51). The
    // StickS3 has none (specs list only the BMI270 and M5PM1 on I2C), so this
    // stays silent unless a future board/unit actually has one -- in which case
    // a battery-backed clock would give the SRS real elapsed time.
    if (M5.Rtc.isEnabled()) {
        m5::rtc_datetime_t dt;
        if (M5.Rtc.getDateTime(&dt))
            Serial.printf("rtc: %04d-%02d-%02d %02d:%02d:%02d\n", dt.date.year, dt.date.month,
                          dt.date.date, dt.time.hours, dt.time.minutes, dt.time.seconds);
    }
}

void update() { M5.update(); }

lgfx::LGFX_Device& display() { return M5.Display; }
int width() { return M5.Display.width(); }
int height() { return M5.Display.height(); }

// Rotation 0 = native portrait, 1 = landscape. width()/height() follow it.
void setOrientation(bool landscape) { M5.Display.setRotation(landscape ? 1 : 0); }
bool landscape() { return M5.Display.width() > M5.Display.height(); }

bool wasPressed(Button b) { return btn(b).wasPressed(); }
bool isHeld(Button b) { return btn(b).isPressed(); }
bool wasHeld(Button b) { return btn(b).wasHold(); }

int batteryPercent() {
    int lvl = M5.Power.getBatteryLevel();
    return (lvl < 0 || lvl > 100) ? -1 : lvl;
}

bool isCharging() { return M5.Power.isCharging() == m5::Power_Class::is_charging; }

void setBrightness(uint8_t level) {
    gBrightness = level;
    M5.Display.setBrightness(level);
}

bool wokeFromSleep() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0; }

// VBUS is read from the M5PM1 (mV); a few hundred mV of noise reads as absent.
bool externalPower() { return M5.Power.getVBUSVoltage() > 4000; }

void setDisplayOn(bool on) {
    if (on) {
        M5.Display.wakeup();
        M5.Display.setBrightness(gBrightness);
    } else {
        M5.Display.setBrightness(0);
        M5.Display.sleep();
    }
}

void deepSleep() {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    M5.Display.waitDisplay();
    // Wait for the button to be released, or the wake level would be present
    // immediately and the chip would bounce straight back up.
    while (digitalRead(kSelectPin) == LOW) delay(10);
    // The button has an external pull-up; the RTC one is a belt-and-braces
    // default while the digital pad is powered down.
    rtc_gpio_pullup_en(kSelectPin);
    rtc_gpio_pulldown_dis(kSelectPin);
    esp_sleep_enable_ext0_wakeup(kSelectPin, 0);
    Serial.flush();
    esp_deep_sleep_start();
}

uint64_t rtcMicros() { return esp_rtc_get_time_us(); }
uint32_t randomSeed() { return esp_random(); }

}  // namespace board
}  // namespace tgc
#endif
