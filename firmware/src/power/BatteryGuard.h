#pragma once
// Critical-battery guard: decides, from the noisy battery percentage, when the
// app must save and deep-sleep before a nearly flat cell browns out and takes
// the ESP32's RTC timer (the SRS day clock) with it. Pure C++, no Arduino (the
// native tests include it through -Isrc). Design: docs/decisions.md.
#include <cstdint>

namespace tgc {

class BatteryGuard {
public:
    // A reading at or below kCriticalPct is low. kLowReadings low readings
    // trigger the sleep; only a reading at or above kResumePct (or external
    // power) resets the count, so a gauge wobbling across the threshold still
    // gets there.
    static constexpr int kCriticalPct = 3;
    static constexpr int kResumePct = 6;
    static constexpr uint8_t kLowReadings = 3;

    // Feeds one reading: `pct` is 0..100, or negative when the board cannot
    // tell (ignored); `external` is USB/charging, which is never critical.
    // True when the device should save and sleep now.
    bool update(int pct, bool external) {
        if (external) {
            low_ = 0;
            return false;
        }
        if (pct < 0) return false;
        if (pct <= kCriticalPct) {
            if (low_ < kLowReadings) ++low_;
        } else if (pct >= kResumePct) {
            low_ = 0;
        }
        return low_ >= kLowReadings;
    }

    uint8_t lowReadings() const { return low_; }

    // On a wake after a critical sleep: may the app carry on? Only with
    // external power or at kResumePct, a margin above the trigger so a cell
    // that rests back up by a point does not wake into another critical sleep.
    // An unknown level never locks the user out.
    static bool canResume(int pct, bool external) {
        return external || pct < 0 || pct >= kResumePct;
    }

private:
    uint8_t low_ = 0;
};

}  // namespace tgc
