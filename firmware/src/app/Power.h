#pragma once
// Power: the idle dim and sleep, the explicit sleep and its notice, the wake,
// the critical-battery guard, the time slept (added to the SRS day clock) and
// the CPU clocks. Arduino only.
#include <cstdint>

// RTC slow memory, kept through a deep sleep (Power.cpp): the last sleep was
// forced by a critical battery, the UI language then, and the boot-time date
// question was still open.
extern bool gSleptCritical;
extern bool gSleepUiEs;
extern bool gDatePending;

namespace tgc {
namespace app {

// Power management. After kDimAfterMs without input the backlight dims; after
// kSleepAfterMs (kWifiIdleMs while the portal is up and no HTTP request came
// in) the state is saved and, on battery, the chip deep-sleeps until the front
// button wakes it. On USB power only the panel goes dark, so the serial port
// stays usable for flashing and logs; unplugging then lets it sleep.
#if defined(TGC_BATT_TEST)
// Battery discharge test (env sticks3-batt, never a release): the device must
// stay awake on battery for hours, so the idle dim and sleep are pushed out of
// the way. The critical guard still sleeps at 3 %.
constexpr uint32_t kDimAfterMs = 24u * 60 * 60 * 1000;
constexpr uint32_t kSleepAfterMs = 24u * 60 * 60 * 1000;
#else
constexpr uint32_t kDimAfterMs = 30000;
constexpr uint32_t kSleepAfterMs = 120000;
#endif
constexpr uint32_t kWifiIdleMs = 300000;
constexpr uint32_t kPowerCheckMs = 5000;  // VBUS poll while the panel is off
// 80 MHz is plenty for the quiz; Wi-Fi gets full speed while the portal runs.
constexpr uint32_t kCpuMhz = 80;
constexpr uint32_t kCpuWifiMhz = 240;
// How long the notice before an explicit sleep stays up.
constexpr uint32_t kSleepNoticeMs = 1500;
constexpr uint32_t kBatteryNoticeMs = 2500;

extern uint32_t gLastInputAt;  // millis() of the last button activity
extern bool gDimmed;
extern bool gScreenOff;        // idle on USB power: panel off, still running
extern bool gSwallowInput;     // drop button events until all buttons are up

#if defined(TGC_BATT_TEST)
// Test-only log in LittleFS (env sticks3-batt).
extern uint32_t gBattLogAt;
extern int gBattWakePct;
extern bool gBattWakeUsb;
void battLog(const char* what, int pct = -2);
void battLogPrint();
#endif

bool batteryCritical(uint32_t now);
void applySleepTime();
void wakeScreen();
[[noreturn]] void sleepNow(bool critical);
void powerTick(uint32_t now);
void halt();

}  // namespace app
}  // namespace tgc
