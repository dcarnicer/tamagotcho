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
}

void update() { M5.update(); }

lgfx::LGFX_Device& display() { return M5.Display; }
int width() { return M5.Display.width(); }
int height() { return M5.Display.height(); }

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

// PROTOTYPE: M5PM1 seconds counter. Returns 0xFFFFFFFF on any I2C failure.
uint32_t powerTimerSeconds() {
    uint8_t b[4] = {0, 0, 0, 0};
    if (!M5.In_I2C.readRegister(0x6E, 0x38, b, 4, 100000)) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3] & 0x7F) << 24);
}

bool powerTimerArm(uint32_t seconds) {
    uint8_t b[4] = {static_cast<uint8_t>(seconds), static_cast<uint8_t>(seconds >> 8),
                    static_cast<uint8_t>(seconds >> 16),
                    static_cast<uint8_t>((seconds >> 24) & 0x7F)};
    if (!M5.In_I2C.writeRegister(0x6E, 0x38, b, 4, 100000)) return false;
    if (!M5.In_I2C.writeRegister8(0x6E, 0x3C, 0x01, 100000)) return false;  // FLAG, one-shot
    uint8_t key = 0xA5;                                                      // reload key
    return M5.In_I2C.writeRegister(0x6E, 0x3D, &key, 1, 100000);
}

}  // namespace board
}  // namespace tgc
#endif
