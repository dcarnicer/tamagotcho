// M5Stack StickS3 implementation via M5Unified / M5GFX.
//
// Buttons (verified in M5Unified.cpp): BtnA = GPIO11 = front (big) button,
// BtnB = GPIO12 = side button. Mapping: A = Select, B = Next.
#include "board/Board.h"

#if defined(TGC_BOARD_STICKS3)
#include <M5Unified.h>
#include <esp_random.h>

namespace tgc {
namespace board {

namespace {
m5::Button_Class& btn(Button b) { return b == Button::Select ? M5.BtnA : M5.BtnB; }
}  // namespace

void begin() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(0);
    // StickS3 panel is natively portrait (135x240); make sure of it.
    if (M5.Display.width() > M5.Display.height()) M5.Display.setRotation(1);
    M5.Display.setBrightness(128);
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

void setBrightness(uint8_t level) { M5.Display.setBrightness(level); }
uint32_t randomSeed() { return esp_random(); }

}  // namespace board
}  // namespace tgc
#endif
