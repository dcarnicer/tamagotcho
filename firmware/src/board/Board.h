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

void setBrightness(uint8_t level);  // 0..255

// Hardware random seed.
uint32_t randomSeed();

}  // namespace board
}  // namespace tgc
