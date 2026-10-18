#pragma once
// Calendar and wall-clock helpers for the SRS day clock: civil dates, the
// plausibility bounds for a real time coming from a phone (or NTP) and
// the "study day" that rolls over at a fixed local hour. Pure C++, no Arduino.
// Design: docs/srs.md sections 4.1 and 5.1.
#include <cstdint>

namespace tgc {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithm); constexpr so the fixed base below is checked at compile time.
constexpr int32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// The inverse of daysFromCivil().
void civilFromDays(int32_t z, int* y, unsigned* m, unsigned* d);

// Day of the week for days since 1970-01-01: 0 = Sunday ... 6 = Saturday.
int weekdayFromDays(int32_t z);

// The date shown for SRS day 0 while the store has no wall-clock anchor: the
// fixed base of the first firmware, 2026-10-17 (docs/srs.md section 4.1).
constexpr int32_t kSrsLegacyEpochDay = daysFromCivil(2026, 10, 17);
static_assert(kSrsLegacyEpochDay == 20743, "2026-10-17 is epoch day 20743");

// The study day starts at this local hour, as in Anki, so a late-night session
// still counts for the evening's day (docs/srs.md section 5.1).
constexpr int kSrsRolloverHour = 4;

// A wall-clock time is plausible only inside [2026-01-01, 2100-01-01) UTC and
// with an offset every real time zone can have (-12 h .. +14 h). Anything else
// is a broken browser clock or a forged request.
constexpr int64_t kWallClockMinUtcMs = 1767225600000LL;  // 2026-01-01T00:00:00Z
constexpr int64_t kWallClockMaxUtcMs = 4102444800000LL;  // 2100-01-01T00:00:00Z
constexpr int kTzOffsetMinLowest = -12 * 60;
constexpr int kTzOffsetMinHighest = 14 * 60;

// `tzOffsetMin` is always local time minus UTC, in minutes ("minutes east of
// UTC": +120 for CEST, -300 for EST), i.e. JavaScript's -getTimezoneOffset().
bool wallClockPlausible(int64_t utcMs, int tzOffsetMin);

// Splits a wall-clock instant into its study day (days since 1970-01-01 of the
// local date, where the day starts at kSrsRolloverHour) and the milliseconds
// since that day's rollover. False, outputs untouched, when the time is not
// plausible.
bool studyDayOf(int64_t utcMs, int tzOffsetMin, int32_t* epochDay, uint32_t* msIntoDay);

}  // namespace tgc
