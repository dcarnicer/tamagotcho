// The M5 image (TGC_BOARD_M5): the StickS3 and the Cardputer ADV in one build,
// for a single M5Burner entry. Both board files are compiled, each in its own
// namespace (board/BoardSticks3.cpp: sticks3, board/BoardCardputerAdv.cpp:
// cardputer), and every tgc::board function forwards to the one of the board
// found at boot.
//
// The board is known before M5.begin() (which needs it: the Cardputer's
// begin() frees G11, the StickS3's front button, from the RTC domain): the
// StickS3 has 8 MB of PSRAM and the Cardputer ADV none. The build is the
// octal-PSRAM one; on the Cardputer the PSRAM init fails and the boot goes on
// (CONFIG_SPIRAM_IGNORE_NOTFOUND), leaving no SPIRAM heap -- what is checked
// here, since psramFound() reads true either way in this core.
// begin() then checks the choice against M5Unified's own board detection.
#include "board/Board.h"

#if defined(TGC_BOARD_M5)
#include <M5Unified.h>
#include <esp_heap_caps.h>

namespace tgc {
namespace board {

// The API of Board.h, once per board namespace.
#define TGC_BOARD_DECLARE                     \
    bool fixedLandscape();                    \
    bool hasKeyboard();                       \
    bool nextKeyEvent(KeyEvent* out);         \
    bool sdPins(SdPins* out);                 \
    void begin();                             \
    void update();                            \
    lgfx::LGFX_Device& display();             \
    int width();                              \
    int height();                             \
    void setOrientation(bool landscape);      \
    bool landscape();                         \
    void setRotation(uint8_t r);              \
    uint8_t rotation();                       \
    bool hasAccel();                          \
    bool readAccel(float* x, float* y, float* z); \
    bool wasPressed(Button b);                \
    bool isHeld(Button b);                    \
    bool wasHeld(Button b);                   \
    int batteryPercent();                     \
    bool isCharging();                        \
    void setBrightness(uint8_t level);        \
    bool wokeFromSleep();                     \
    bool externalPower();                     \
    void setDisplayOn(bool on);               \
    [[noreturn]] void deepSleep();            \
    uint64_t rtcMicros();                     \
    bool powerButtonClicked();                \
    uint32_t randomSeed();                    \
    const char* name();

namespace sticks3 {
TGC_BOARD_DECLARE
}
namespace cardputer {
TGC_BOARD_DECLARE
}
#undef TGC_BOARD_DECLARE

namespace {
// The Cardputer ADV when there is no SPIRAM heap (see above); decided once.
bool cardputerBoard() {
    static const bool card = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0;
    return card;
}
}  // namespace

// Calls `f` of the board found at boot.
#define TGC_BOARD_CALL(f, ...) (cardputerBoard() ? cardputer::f(__VA_ARGS__) : sticks3::f(__VA_ARGS__))

bool fixedLandscape() { return TGC_BOARD_CALL(fixedLandscape); }
bool hasKeyboard() { return TGC_BOARD_CALL(hasKeyboard); }
bool nextKeyEvent(KeyEvent* out) { return TGC_BOARD_CALL(nextKeyEvent, out); }
bool sdPins(SdPins* out) { return TGC_BOARD_CALL(sdPins, out); }

void begin() {
    TGC_BOARD_CALL(begin);
    const m5::board_t want =
        cardputerBoard() ? m5::board_t::board_M5CardputerADV : m5::board_t::board_M5StickS3;
    Serial.printf("board: %s (M5 image)%s\n", name(),
                  M5.getBoard() == want ? "" : ", but M5Unified found another board");
}

void update() { TGC_BOARD_CALL(update); }
lgfx::LGFX_Device& display() { return TGC_BOARD_CALL(display); }
int width() { return TGC_BOARD_CALL(width); }
int height() { return TGC_BOARD_CALL(height); }
void setOrientation(bool landscape) { TGC_BOARD_CALL(setOrientation, landscape); }
bool landscape() { return TGC_BOARD_CALL(landscape); }
void setRotation(uint8_t r) { TGC_BOARD_CALL(setRotation, r); }
uint8_t rotation() { return TGC_BOARD_CALL(rotation); }
bool hasAccel() { return TGC_BOARD_CALL(hasAccel); }
bool readAccel(float* x, float* y, float* z) { return TGC_BOARD_CALL(readAccel, x, y, z); }
bool wasPressed(Button b) { return TGC_BOARD_CALL(wasPressed, b); }
bool isHeld(Button b) { return TGC_BOARD_CALL(isHeld, b); }
bool wasHeld(Button b) { return TGC_BOARD_CALL(wasHeld, b); }
int batteryPercent() { return TGC_BOARD_CALL(batteryPercent); }
bool isCharging() { return TGC_BOARD_CALL(isCharging); }
void setBrightness(uint8_t level) { TGC_BOARD_CALL(setBrightness, level); }
bool wokeFromSleep() { return TGC_BOARD_CALL(wokeFromSleep); }
bool externalPower() { return TGC_BOARD_CALL(externalPower); }
void setDisplayOn(bool on) { TGC_BOARD_CALL(setDisplayOn, on); }

void deepSleep() {
    if (cardputerBoard()) cardputer::deepSleep();
    sticks3::deepSleep();
}

uint64_t rtcMicros() { return TGC_BOARD_CALL(rtcMicros); }
bool powerButtonClicked() { return TGC_BOARD_CALL(powerButtonClicked); }
uint32_t randomSeed() { return TGC_BOARD_CALL(randomSeed); }
const char* name() { return TGC_BOARD_CALL(name); }

#undef TGC_BOARD_CALL

}  // namespace board
}  // namespace tgc
#endif
