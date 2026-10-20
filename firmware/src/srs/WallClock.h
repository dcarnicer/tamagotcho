#pragma once
// Calendar and wall-clock helpers for the SRS day clock: civil dates, the
// plausibility bounds for a real time coming from a phone (or NTP) and
// the "study day" that rolls over at a fixed local hour. Pure C++, no Arduino.
// Design: docs/srs.md sections 4.1 and 5.1.
#include <cstdint>

#include "srs/TimeZone.h"

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
// The same lower bound as a calendar day: the earliest date the date screen
// can set by hand (docs/srs.md section 5, power-cut rule).
constexpr int32_t kWallClockMinEpochDay = daysFromCivil(2026, 1, 1);
static_assert(static_cast<int64_t>(kWallClockMinEpochDay) * 86400000LL == kWallClockMinUtcMs,
              "the earliest date is the day of the earliest time");
constexpr int kTzOffsetMinLowest = -12 * 60;
constexpr int kTzOffsetMinHighest = 14 * 60;

// `tzOffsetMin` is always local time minus UTC, in minutes ("minutes east of
// UTC": +120 for CEST, -300 for EST), i.e. JavaScript's -getTimezoneOffset().
bool wallClockPlausible(int64_t utcMs, int tzOffsetMin);

// The same for a time zone rule: the time is plausible and both offsets of the
// rule are (see tzRuleOffsetsPlausible()).
bool wallClockPlausible(int64_t utcMs, const TzRule& zone);

// Splits a wall-clock instant into its study day (days since 1970-01-01 of the
// local date, where the day starts at kSrsRolloverHour local) and the
// milliseconds since that day's rollover. With a daylight-saving rule the local
// date uses the offset in effect at `utcMs`, and the milliseconds are real
// elapsed time since the rollover instant. False, outputs untouched, when the
// time is not plausible. The offset form is a fixed rule (fixedTzRule()).
bool studyDayOf(int64_t utcMs, const TzRule& zone, int32_t* epochDay, uint32_t* msIntoDay);
bool studyDayOf(int64_t utcMs, int tzOffsetMin, int32_t* epochDay, uint32_t* msIntoDay);

// The UTC instant (ms) at which study day `epochDay` begins: local 04:00 that
// date. When the local clock passes 04:00 twice (daylight time ending at that
// hour) the first one counts; when it skips 04:00 (a jump over it), the moment
// of the jump.
int64_t studyDayStartUtcMs(const TzRule& zone, int32_t epochDay);

// How long study day `epochDay` lasts in real time: 24 h, or 23 h / 25 h (or
// 23.5 h / 24.5 h) when a daylight-saving change falls inside it. This is what
// the SRS uptime and sleep rules count down, so the rollover stays at 04:00
// local across a change.
uint32_t studyDayLengthMs(const TzRule& zone, int32_t epochDay);

}  // namespace tgc
