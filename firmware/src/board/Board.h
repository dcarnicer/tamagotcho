#pragma once
// Board HAL: one implementation per target, selected by build flag
// (TGC_BOARD_STICKS3 / TGC_BOARD_TDISPLAY_S3 / TGC_BOARD_CARDPUTER_ADV). UI
// code is written against the LovyanGFX API only, so it is identical on every
// board.
#include <cstdint>

#include "ui/KeyInput.h"

#if defined(TGC_BOARD_STICKS3) || defined(TGC_BOARD_CARDPUTER_ADV)
#include <M5GFX.h>
#elif defined(TGC_BOARD_TDISPLAY_S3)
#include <LovyanGFX.hpp>
#else
#error "Define TGC_BOARD_STICKS3, TGC_BOARD_TDISPLAY_S3 or TGC_BOARD_CARDPUTER_ADV"
#endif

namespace tgc {
namespace board {

enum class Button : uint8_t {
    Next,    // StickS3: side button (B). T-Display-S3: GPIO14. Cardputer ADV: none
             // (never down; its keyboard has the actions, see nextKeyEvent()).
    Select,  // StickS3: front button (A). T-Display-S3: GPIO0 (BOOT). Cardputer
             // ADV: the G0 button.
};

// --- Capabilities and keyboard ------------------------------------------------
// fixedLandscape(): the panel is used one way only (Cardputer ADV: landscape,
// rotation 1, over the keyboard), so the app has no Orientation setting.
// hasKeyboard(): the board has a keyboard (Cardputer ADV); the hint bar names
// keys and nextKeyEvent() delivers them.
// nextKeyEvent(): the next key event update() read, oldest first; false when
// there is none (always on a board without a keyboard). Key::None going up
// means every key is up (events were lost).
#if defined(TGC_BOARD_CARDPUTER_ADV)
bool fixedLandscape();
bool hasKeyboard();
bool nextKeyEvent(KeyEvent* out);
#else
// No keyboard, every orientation: the StickS3 and the T-Display-S3.
inline bool fixedLandscape() { return false; }
inline bool hasKeyboard() { return false; }
inline bool nextKeyEvent(KeyEvent*) { return false; }
#endif

constexpr uint32_t kHoldMs = 600;  // threshold for wasHeld()

// Initialise display (portrait; the Cardputer ADV landscape), buttons, the
// keyboard, power. Call once from setup().
void begin();
// Poll inputs; call once per loop() iteration before querying buttons.
void update();

lgfx::LGFX_Device& display();
int width();   // width of the current rotation
int height();  // height of the current rotation

// Display rotation. Portrait is the default (begin() sets it); landscape swaps
// the panel's width and height (e.g. 135x240 -> 240x135). width()/height()
// follow the current rotation.
void setOrientation(bool landscape);  // setRotation(landscape ? 1 : 0)
bool landscape();
// Rotation 0-3 as LovyanGFX's setRotation(): the top of the picture is at the
// panel's top (0, native portrait), right (1, landscape), bottom (2) or left
// (3, landscape the other way) edge, as seen in rotation 0.
void setRotation(uint8_t r);
uint8_t rotation();

// --- Accelerometer ---------------------------------------------------------
// True when the board has a working accelerometer (StickS3: BMI270).
bool hasAccel();
// Latest acceleration in g in the *panel frame*: +x toward the panel's right
// edge, +y toward its top edge, +z out of the screen, as seen in rotation 0. At
// rest the axis that points up reads +1 g. Non-blocking; false when the board
// has no accelerometer or no sample has arrived yet.
bool readAccel(float* x, float* y, float* z);

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

// --- Power -----------------------------------------------------------------
// True when this boot is a wake-up from deepSleep() rather than a power-on or
// a reset.
bool wokeFromSleep();
// True when USB/external power is present; false if unknown. The app then only
// blanks the panel when idle instead of sleeping (keeps the serial port up).
bool externalPower();
// Panel and backlight on/off without sleeping; off keeps the frame buffer.
void setDisplayOn(bool on);
// Blanks the panel and puts the chip in deep sleep. The Select button wakes it
// (on the Cardputer ADV any key too, which is not delivered as a key event),
// and the chip then boots through setup() again. Never returns.
[[noreturn]] void deepSleep();
// Microseconds since the RTC timer started. It keeps counting through deep
// sleep and resets only on a power cut, so differences measure sleep time.
uint64_t rtcMicros();
// True once when the power button was pressed since the last report, meaning
// "save and sleep now". StickS3: begin() disables the M5PM1's single-click
// reset (which cuts the chip's power and with it the RTC timer) and this polls
// the PM1's press flag; the double-click power-off stays as the hardware
// escape hatch. False when the PM1 is missing, unknown or failed to configure
// (the button then keeps its factory behaviour), and always on the
// T-Display-S3. A press does not wake the chip from deepSleep(); only Select
// does.
bool powerButtonClicked();

// Hardware random seed.
uint32_t randomSeed();

}  // namespace board
}  // namespace tgc
