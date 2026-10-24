// M5Stack Cardputer ADV implementation via M5Unified / M5GFX, plus a small
// driver for its keyboard's TCA8418.
//
// Stamp-S3A (ESP32-S3FN8: 8 MB flash, no PSRAM), 1.14" ST7789V2 135x240 used
// in landscape over the keyboard, 56 keys (4 x 14) behind a TCA8418 keypad
// controller on the internal I2C bus (SDA G8, SCL G9, shared with the BMI270
// and the ES8311; M5Unified's In_I2C), its INT on G11 (active low), the G0
// button, the battery on G10 through a 1:2 divider, and a power switch.
// M5GFX and M5Unified detect the board (board_M5CardputerADV): panel, backlight
// (G38), the In_I2C pins, BtnA = G0 and the battery ADC all come from them.
//
// NEEDS HARDWARE CONFIRMATION (no Cardputer ADV was at hand when this was
// written): the rotation (1, as M5Stack's Cardputer examples and M5GFX's
// autodetect use: the keyboard below the picture), the TCA8418 setup and key
// numbering (ui/KeyInput.h, after M5Stack's M5Cardputer library), the battery
// reading, and the deep sleep's wake on a key (ext1 on G11).
#include "board/Board.h"

// In the M5 image (TGC_BOARD_M5) this file is board/BoardM5.cpp's
// tgc::board::cardputer, picked at boot.
#if defined(TGC_BOARD_CARDPUTER_ADV) || defined(TGC_BOARD_M5)
#include <HWCDC.h>
#include <M5Unified.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <soc/esp32s3/rtc.h>

namespace tgc {
namespace board {
#if defined(TGC_BOARD_M5)
namespace cardputer {
#endif

namespace {

constexpr gpio_num_t kSelectPin = GPIO_NUM_0;   // G0 button, active low
constexpr gpio_num_t kKbdIntPin = GPIO_NUM_11;  // TCA8418 INT, active low (open drain)
uint8_t gBrightness = 128;

// --- TCA8418 (TI datasheet SCPS215; register use as in M5Stack's M5Cardputer
// library, which drives this chip on this board, MIT) ------------------------
constexpr uint8_t kTcaAddr = 0x34;
constexpr uint32_t kTcaFreq = 400000;
constexpr uint8_t kRegCfg = 0x01;        // bit1 GPI_IEN, bit0 KE_IEN
constexpr uint8_t kRegIntStat = 0x02;    // bit3 OVR_FLOW_INT, bit1 GPI_INT, bit0 K_INT; write 1 to clear
constexpr uint8_t kRegKeyEventA = 0x04;  // the FIFO: bit7 pressed, bits 0-6 key number; 0 = empty
constexpr uint8_t kRegGpioIntStat1 = 0x11;
constexpr uint8_t kRegGpioIntEn1 = 0x1A;
constexpr uint8_t kRegKpGpio1 = 0x1D;    // rows in the keypad matrix
constexpr uint8_t kRegKpGpio2 = 0x1E;    // columns 0-7 in the matrix
constexpr uint8_t kRegKpGpio3 = 0x1F;    // columns 8-9
constexpr uint8_t kRegGpiEm1 = 0x20;
constexpr uint8_t kRegGpioDir1 = 0x23;
constexpr uint8_t kRegGpioIntLvl1 = 0x26;
constexpr uint8_t kIntOverflow = 0x08;
constexpr uint32_t kKbdPollMs = 100;  // a read even while INT stays high

bool gKbd = false;            // the TCA8418 answered and is set up
uint32_t gKbdPolledAt = 0;
// Keys down, by key number (1-80), so deepSleep() can wait for every key to
// be up: a release would assert INT and wake the chip at once.
uint64_t gKeysDown[2] = {0, 0};

// The key events update() read, for nextKeyEvent().
constexpr int kQueueLen = 16;
KeyEvent gQueue[kQueueLen];
int gQueueHead = 0, gQueueCount = 0;

void push(const KeyEvent& e) {
    if (gQueueCount == kQueueLen) {
        // Full: the oldest event goes, and the main loop hears that events
        // were lost (every key up) rather than seeing a press without release.
        gQueueHead = (gQueueHead + 1) % kQueueLen;
        --gQueueCount;
        gQueue[(gQueueHead + gQueueCount) % kQueueLen] = KeyEvent{Key::None, false};
        return;
    }
    gQueue[(gQueueHead + gQueueCount) % kQueueLen] = e;
    ++gQueueCount;
}

bool tcaWrite(uint8_t reg, uint8_t v) { return M5.In_I2C.writeRegister8(kTcaAddr, reg, v, kTcaFreq); }
bool tcaRead(uint8_t reg, uint8_t* v) { return M5.In_I2C.readRegister(kTcaAddr, reg, v, 1, kTcaFreq); }

void noteKey(uint8_t raw) {
    const int code = raw & 0x7F;
    if (code < 1 || code > 80) return;
    const int bit = code - 1;
    uint64_t& word = gKeysDown[bit / 64];
    const uint64_t mask = 1ull << (bit % 64);
    if (raw & 0x80)
        word |= mask;
    else
        word &= ~mask;
}

// Reads every event in the FIFO into the queue, then clears the interrupt
// (it stays set while the FIFO holds events). An overflow lost events: every
// key counts as up.
void readKeyboard(bool deliver) {
    // The FIFO holds at most 10 events and reads 0 once it is empty.
    for (int i = 0; i < 16; ++i) {
        uint8_t raw = 0;
        if (!tcaRead(kRegKeyEventA, &raw) || raw == 0) break;
        noteKey(raw);
        KeyEvent e;
        if (deliver && cardputerAdvEvent(raw, &e)) push(e);
    }
    uint8_t st = 0;
    if (!tcaRead(kRegIntStat, &st)) return;
    if (st & kIntOverflow) {
        gKeysDown[0] = gKeysDown[1] = 0;
        if (deliver) push(KeyEvent{Key::None, false});
    }
    if (st & 0x02)  // a GPI event: read its status to clear it (no pins are GPIs here)
        for (uint8_t r = 0; r < 3; ++r) {
            uint8_t dummy = 0;
            tcaRead(kRegGpioIntStat1 + r, &dummy);
        }
    if (st) tcaWrite(kRegIntStat, st);
}

// The keypad setup of M5Stack's TCA8418 reader for this board: every pin an
// input with events, a 7 x 8 matrix, the FIFO flushed (a key that woke the
// chip from deep sleep is dropped here), key and GPI interrupts on INT.
bool setupKeyboard() {
    if (!M5.In_I2C.isEnabled()) return false;
    bool ok = true;
    for (uint8_t r = 0; r < 3; ++r) {
        ok = ok && tcaWrite(kRegGpioDir1 + r, 0x00);
        ok = ok && tcaWrite(kRegGpiEm1 + r, 0xFF);
        ok = ok && tcaWrite(kRegGpioIntLvl1 + r, 0x00);
        ok = ok && tcaWrite(kRegGpioIntEn1 + r, 0xFF);
    }
    ok = ok && tcaWrite(kRegKpGpio1, 0x7F) && tcaWrite(kRegKpGpio2, 0xFF) &&
         tcaWrite(kRegKpGpio3, 0x00);
    if (!ok) return false;
    gKbd = true;
    readKeyboard(false);  // flush
    gKeysDown[0] = gKeysDown[1] = 0;
    uint8_t cfg = 0;
    if (!tcaRead(kRegCfg, &cfg) || !tcaWrite(kRegCfg, cfg | 0x03)) {
        gKbd = false;
        return false;
    }
    return true;
}

bool anyKeyDown() { return gKeysDown[0] || gKeysDown[1]; }

}  // namespace

void begin() {
    // A wake from deepSleep() leaves G0 and G11 in the RTC domain; give them
    // back to the digital pads first.
    rtc_gpio_deinit(kSelectPin);
    rtc_gpio_deinit(kKbdIntPin);
    auto cfg = M5.config();
    // Nothing here uses the IMU, the speaker or the microphone: the BMI270
    // stays in its power-on suspend mode and the ES8311 off.
    cfg.internal_imu = false;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    M5.begin(cfg);
    if (M5.getBoard() != m5::board_t::board_M5CardputerADV)
        Serial.printf("board: expected a Cardputer ADV, M5Unified found board %d\n",
                      static_cast<int>(M5.getBoard()));
    // Landscape, the keyboard below the picture (M5GFX's own default here).
    M5.Display.setRotation(1);
    M5.Display.setBrightness(gBrightness);
    M5.BtnA.setHoldThresh(kHoldMs);
    pinMode(kKbdIntPin, INPUT_PULLUP);
    if (setupKeyboard())
        Serial.println("kbd: TCA8418 ready");
    else
        Serial.println("kbd: TCA8418 not found, keys off");
}

void update() {
    M5.update();
    if (!gKbd) return;
    const uint32_t now = millis();
    // INT low: events are waiting. Every kKbdPollMs a read anyway, in case
    // INT is not wired the way this assumes.
    if (digitalRead(kKbdIntPin) == LOW || now - gKbdPolledAt >= kKbdPollMs) {
        gKbdPolledAt = now;
        readKeyboard(true);
    }
}

bool fixedLandscape() { return true; }
bool hasKeyboard() { return true; }

// The microSD slot: SCK G40, MISO G39, MOSI G14, CS G12, no card-detect pin.
// M5GFX puts the panel on SPI2_HOST (MOSI G35, SCLK G36: its ESP32-S3 default,
// which its autodetect for board_M5CardputerADV keeps), so the card takes SPI3
// (platform/Storage's own SPIClass on HSPI), not Arduino's global `SPI` (SPI2).
// The EXT header carries the same SCK, MOSI and MISO, and the Cap LoRa-1262
// (Meshtastic's radio on this board) sits there with its SX1262 select on G5:
// left floating, the radio answered along with the card and the card's init
// read noise ("crc error" on GO_IDLE_STATE; seen 2026-10-24 with the cap on).
bool sdPins(SdPins* out) {
    out->sck = 40;
    out->miso = 39;
    out->mosi = 14;
    out->cs = 12;
    out->otherCs = 5;  // Cap LoRa-1262 NSS (EXT pin 14)
    return true;
}

bool nextKeyEvent(KeyEvent* out) {
    if (gQueueCount == 0) return false;
    *out = gQueue[gQueueHead];
    gQueueHead = (gQueueHead + 1) % kQueueLen;
    --gQueueCount;
    return true;
}

lgfx::LGFX_Device& display() { return M5.Display; }
int width() { return M5.Display.width(); }
int height() { return M5.Display.height(); }

void setOrientation(bool landscape) { setRotation(landscape ? 1 : 0); }
bool landscape() { return M5.Display.width() > M5.Display.height(); }
void setRotation(uint8_t r) { M5.Display.setRotation(r & 3); }
uint8_t rotation() { return M5.Display.getRotation() & 3; }

// The board has a BMI270, but with a keyboard under the screen there is
// nothing to turn: no Auto orientation (docs/decisions.md, 2026-10-21).
bool hasAccel() { return false; }
bool readAccel(float*, float*, float*) { return false; }

// Select is the G0 button (M5Unified's BtnA); there is no Next button.
bool wasPressed(Button b) { return b == Button::Select && M5.BtnA.wasPressed(); }
bool isHeld(Button b) { return b == Button::Select && M5.BtnA.isPressed(); }
bool wasHeld(Button b) { return b == Button::Select && M5.BtnA.wasHold(); }

// M5Unified reads the cell on G10 (1:2 divider) and maps 3.3-4.15 V to 0-100.
int batteryPercent() {
    const int lvl = M5.Power.getBatteryLevel();
    return (lvl < 0 || lvl > 100) ? -1 : lvl;
}

// No charger status reaches the ESP32 here (M5Unified reports it unknown).
bool isCharging() { return M5.Power.isCharging() == m5::Power_Class::is_charging; }

void setBrightness(uint8_t level) {
    gBrightness = level;
    M5.Display.setBrightness(level);
}

bool wokeFromSleep() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1; }

// No VBUS sense reaches the ESP32 either: a USB host is the next best thing.
// The USB Serial/JTAG sees its start-of-frame packets every millisecond while
// it is plugged into a computer (Arduino's HWCDC::isPlugged(), which needs no
// open terminal); a charger alone sends none and reads as battery.
bool externalPower() { return HWCDC::isPlugged(); }

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
    // G0 and every key up first, or the wake level would be there at once (a
    // key's release asserts INT too). The FIFO is drained and the interrupt
    // cleared meanwhile, so INT is high when the chip sleeps. A key still down
    // after 3 s (or an INT that stays low) leaves G0 as the only wake source.
    const uint32_t t0 = millis();
    bool kbdWake = gKbd;
    for (;;) {
        if (gKbd) readKeyboard(false);
        const bool g0Up = digitalRead(kSelectPin) == HIGH;
        const bool kbdIdle = !gKbd || (!anyKeyDown() && digitalRead(kKbdIntPin) == HIGH);
        if (g0Up && kbdIdle) break;
        if (millis() - t0 >= 3000) {
            kbdWake = false;
            if (g0Up) break;
        }
        delay(10);
    }
    // Both lines have external pull-ups; the RTC ones are a belt-and-braces
    // default while the digital pads are powered down.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    uint64_t mask = 1ull << kSelectPin;
    rtc_gpio_pullup_en(kSelectPin);
    rtc_gpio_pulldown_dis(kSelectPin);
    if (kbdWake) {
        mask |= 1ull << kKbdIntPin;
        rtc_gpio_pullup_en(kKbdIntPin);
        rtc_gpio_pulldown_dis(kKbdIntPin);
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.flush();
    esp_deep_sleep_start();
}

uint64_t rtcMicros() { return esp_rtc_get_time_us(); }
bool powerButtonClicked() { return false; }  // a power switch, no button
uint32_t randomSeed() { return esp_random(); }
const char* name() { return "Cardputer ADV"; }

#if defined(TGC_BOARD_M5)
}  // namespace cardputer
#endif
}  // namespace board
}  // namespace tgc
#endif
