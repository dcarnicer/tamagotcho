// Calendar and wall-clock helpers. Design: docs/srs.md section 5.1.
#include "srs/WallClock.h"

#include "srs/SrsStore.h"

namespace tgc {

void civilFromDays(int32_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yr = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yr + (*m <= 2 ? 1 : 0);
}

int weekdayFromDays(int32_t z) {
    // 1970-01-01 was a Thursday (4).
    const int w = static_cast<int>((z + 4) % 7);
    return w < 0 ? w + 7 : w;
}

bool wallClockPlausible(int64_t utcMs, int tzOffsetMin) {
    return utcMs >= kWallClockMinUtcMs && utcMs < kWallClockMaxUtcMs &&
           tzOffsetMin >= kTzOffsetMinLowest && tzOffsetMin <= kTzOffsetMinHighest;
}

bool studyDayOf(int64_t utcMs, int tzOffsetMin, int32_t* epochDay, uint32_t* msIntoDay) {
    if (!wallClockPlausible(utcMs, tzOffsetMin)) return false;
    constexpr int64_t kDay = kSrsMsPerDay;
    constexpr int64_t kRollover = static_cast<int64_t>(kSrsRolloverHour) * 3600000;
    // Shift so the study day starts at 00:00 of the shifted clock. The bounds
    // above keep it positive, so plain division is a floor.
    const int64_t shifted = utcMs + static_cast<int64_t>(tzOffsetMin) * 60000 - kRollover;
    if (epochDay) *epochDay = static_cast<int32_t>(shifted / kDay);
    if (msIntoDay) *msIntoDay = static_cast<uint32_t>(shifted % kDay);
    return true;
}

}  // namespace tgc
