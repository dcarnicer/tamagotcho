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

// --- Accelerometer axes -> panel frame (see Board.h) ------------------------
// NEEDS HARDWARE CONFIRMATION: derived from sources and docs, not measured.
// - M5Unified remaps the StickS3's BMI270 ("StickS3 BMI270 : X=+Y, Y=-X, Z=+Z",
//   IMU_Class.inl) so that, combined with M5Stack's StickS3 axis diagram (chip
//   X toward the USB end, Y toward the screen's right edge, Z out of the
//   screen), its output is X = screen right, Y = top (HAT) end, Z = out of the
//   screen with the front button below the screen -- the same frame M5Stack
//   documents for the StickC Plus2, whose MPU6886 M5Unified does not remap.
// - Rotation 0 here is that same pose (portrait, front button below the screen).
// So the panel frame is M5Unified's frame unchanged. If Auto shows the picture
// sideways or upside down on hardware, fix it here and nowhere else: swap X/Y
// for a picture 90 degrees off, flip a sign for one 180 degrees off.
constexpr bool kAccelSwapXY = false;
constexpr float kAccelSignX = 1.0f;
constexpr float kAccelSignY = 1.0f;
constexpr float kAccelSignZ = 1.0f;
bool gAccelSeen = false;  // an accelerometer sample has been read

// BMI270 registers (datasheet) for suspending it before deep sleep.
constexpr uint8_t kBmiPwrConf = 0x7C;
constexpr uint8_t kBmiPwrCtrl = 0x7D;

// M5Unified leaves the BMI270's accelerometer and gyroscope running in normal
// mode and does not stop them for deep sleep. Put the chip in suspend (sensors
// off, advanced power save: its state after a power-on reset); M5.begin() soft-
// resets and re-initialises it on the next boot.
void suspendImu() {
    if (M5.Imu.getType() != m5::imu_bmi270) return;
    auto* imu = M5.Imu.getImuInstancePtr(0);
    if (!imu) return;
    imu->writeRegister8(kBmiPwrCtrl, 0x00);  // accel, gyro, aux, temperature off
    delayMicroseconds(500);
    imu->writeRegister8(kBmiPwrConf, 0x03);  // adv_power_save (reset value)
}
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
void setOrientation(bool landscape) { setRotation(landscape ? 1 : 0); }
bool landscape() { return M5.Display.width() > M5.Display.height(); }
void setRotation(uint8_t r) { M5.Display.setRotation(r & 3); }
uint8_t rotation() { return M5.Display.getRotation() & 3; }

// M5.begin() starts the BMI270 (100 Hz, +-8 g); M5.update() does not read it.
bool hasAccel() { return M5.Imu.isEnabled(); }

bool readAccel(float* x, float* y, float* z) {
    if (!M5.Imu.isEnabled()) return false;
    // update() is 0 when no new sample is ready; the previous one stays valid.
    if (M5.Imu.update() & m5::IMU_Class::sensor_mask_accel) gAccelSeen = true;
    if (!gAccelSeen) return false;
    const m5::imu_3d_t a = M5.Imu.getImuData().accel;
    const float ax = kAccelSwapXY ? a.y : a.x;
    const float ay = kAccelSwapXY ? a.x : a.y;
    *x = kAccelSignX * ax;
    *y = kAccelSignY * ay;
    *z = kAccelSignZ * a.z;
    return true;
}

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
    suspendImu();
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
