#pragma once
// Board HAL: one implementation per target, selected by build flag
// (TGC_BOARD_STICKS3 / TGC_BOARD_TDISPLAY_S3). UI code is written against the
// LovyanGFX API only, so it is identical on both boards.
#include <cstdint>

#if defined(TGC_BOARD_STICKS3)
#include <M5GFX.h>
#elif defined(TGC_BOARD_TDISPLAY_S3)
#include <LovyanGFX.hpp>
#else
#error "Define TGC_BOARD_STICKS3 or TGC_BOARD_TDISPLAY_S3"
#endif

namespace tgc {
namespace board {

enum class Button : uint8_t {
    Next,    // StickS3: side button (B). T-Display-S3: GPIO14.
    Select,  // StickS3: front button (A). T-Display-S3: GPIO0 (BOOT).
};

constexpr uint32_t kHoldMs = 600;  // threshold for wasHeld()

// Initialise display (portrait), buttons, power. Call once from setup().
void begin();
// Poll inputs; call once per loop() iteration before querying buttons.
void update();

lgfx::LGFX_Device& display();
int width();   // portrait
int height();

// True once on the press edge (this update() cycle only).
bool wasPressed(Button b);
// True while the button is down.
bool isHeld(Button b);
// True once when the button has been down for kHoldMs (long press).
bool wasHeld(Button b);

// Battery charge 0..100, or -1 if the board cannot report it.
int batteryPercent();
// Whether external power / charging is detected; false if unknown.
bool isCharging();

// PROTOTYPE: the M5PM1 always-on seconds counter (0x6E regs 0x38..0x3B), or
// 0xFFFFFFFF if the board has none. Used to test whether real elapsed time is
// available across power-off (see docs/srs.md section 5).
uint32_t powerTimerSeconds();
// PROTOTYPE: arm that counter to `seconds` (FLAG action, one-shot) so it runs.
// False if the board has no such timer or the I2C write fails.
bool powerTimerArm(uint32_t seconds);

void setBrightness(uint8_t level);  // 0..255

// Hardware random seed.
uint32_t randomSeed();

}  // namespace board
}  // namespace tgc
