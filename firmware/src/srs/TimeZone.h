#pragma once
// Time zones for the SRS day clock: a parser for POSIX TZ rules
// ("CET-1CEST,M3.5.0,M10.5.0/3"), the UTC offset a rule gives at an instant,
// and the curated list Settings > Date > Time zone offers. Pure C++, no
// Arduino, no libc time zone state (the native tests and the device compute
// the same thing). Design: docs/srs.md section 5.1.
//
// Only the subset real zones use is accepted: a standard name and offset, an
// optional daylight name and offset (default: one hour ahead), and then both
// transition dates (Mm.w.d, Jn or n, each with an optional /time, which may be
// negative or past 24 h as in RFC 8536). Names are alphabetic (3+ letters) or
// quoted (<+01>, <-0330>). Offsets outside -12 h..+14 h are rejected.
#include <cstddef>
#include <cstdint>

namespace tgc {

// The longest rule stored or accepted from the portal page (NVS string).
constexpr size_t kTzRuleMaxLen = 63;

// One end of daylight time: a date rule and the local time of the change
// (standard time for the start, daylight time for the end, as POSIX says).
struct TzDate {
    enum Kind : uint8_t { MonthWeekDay, Julian1, Julian0 };
    Kind kind = MonthWeekDay;
    uint8_t month = 1;  // M: 1..12
    uint8_t week = 1;   // M: 1..5, 5 = the last such weekday of the month
    uint8_t wday = 0;   // M: 0 = Sunday .. 6
    uint16_t day = 0;   // J: 1..365 (Feb 29 never counted), n: 0..365
    int32_t timeSec = 2 * 3600;
};

struct TzRule {
    int32_t stdOffsetSec = 0;  // local standard time minus UTC ("east" positive)
    int32_t dstOffsetSec = 0;  // local daylight time minus UTC (== std without DST)
    bool hasDst = false;
    TzDate start;  // daylight time begins (in local standard time)
    TzDate end;    // daylight time ends (in local daylight time)
};

// Parses a POSIX TZ rule. False, `out` untouched, for anything outside the
// subset above (including a daylight name without its two dates).
bool parseTzRule(const char* s, TzRule* out);

// A rule with one fixed offset and no daylight time: what an offset alone (the
// old portal page, or the NVS `tz` minutes) stands for.
TzRule fixedTzRule(int offsetMin);

// Writes the POSIX form of a fixed offset ("<+02>-2", "<-0330>3:30", "<+00>0").
void formatFixedTzRule(int offsetMin, char* out, size_t n);

// True when both offsets of the rule are ones a real zone can have
// (-12 h .. +14 h, whole minutes).
bool tzRuleOffsetsPlausible(const TzRule& r);

// Local time minus UTC, in seconds / minutes, at the UTC instant `utcMs`.
int32_t tzOffsetSecAt(const TzRule& r, int64_t utcMs);
inline int tzOffsetMinAt(const TzRule& r, int64_t utcMs) {
    return static_cast<int>(tzOffsetSecAt(r, utcMs) / 60);
}

// "UTC", "UTC+1", "UTC-3", "UTC+5:30", "UTC-9:30".
void formatUtcOffset(int offsetMin, char* out, size_t n);

// The city of an IANA name for display: the last path component with '_'
// as a space ("America/Argentina/Buenos_Aires" -> "Buenos Aires"); "UTC" and
// names without a '/' are kept as they are.
void tzZoneCity(const char* name, char* out, size_t n);

// Settings > Date > Time zone: a short list of zones as POSIX rules (the tzdata
// footers, checked in test_tz), west to east.
struct TzZone {
    const char* name;  // IANA name
    const char* rule;  // POSIX TZ rule
};
extern const TzZone kTzZones[];
extern const size_t kTzZoneCount;
// The list entry with that IANA name, or null.
const TzZone* findTzZone(const char* name);

}  // namespace tgc
