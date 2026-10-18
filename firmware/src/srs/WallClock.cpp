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

bool wallClockPlausible(int64_t utcMs, const TzRule& zone) {
    return utcMs >= kWallClockMinUtcMs && utcMs < kWallClockMaxUtcMs &&
           tzRuleOffsetsPlausible(zone);
}

namespace {
constexpr int64_t kDayMs = kSrsMsPerDay;
constexpr int64_t kRolloverMs = static_cast<int64_t>(kSrsRolloverHour) * 3600000;
}  // namespace

int64_t studyDayStartUtcMs(const TzRule& zone, int32_t epochDay) {
    const int64_t wall = static_cast<int64_t>(epochDay) * kDayMs + kRolloverMs;  // local
    const int64_t cs = wall - static_cast<int64_t>(zone.stdOffsetSec) * 1000;
    if (!zone.hasDst) return cs;
    const int64_t cd = wall - static_cast<int64_t>(zone.dstOffsetSec) * 1000;
    // A candidate is real when the offset it assumed is the one in effect then.
    const bool vs = tzOffsetSecAt(zone, cs) == zone.stdOffsetSec;
    const bool vd = tzOffsetSecAt(zone, cd) == zone.dstOffsetSec;
    if (vs && vd) return cs < cd ? cs : cd;
    if (vs) return cs;
    if (vd) return cd;
    // 04:00 does not exist that day: find the jump over it, to the second.
    // local(lo) < wall <= local(hi), and both are whole seconds.
    int64_t lo = cs < cd ? cs : cd;
    int64_t hi = cs < cd ? cd : cs;
    while (hi - lo > 1000) {
        const int64_t mid = lo + (hi - lo) / 2000 * 1000;
        if (mid + static_cast<int64_t>(tzOffsetSecAt(zone, mid)) * 1000 >= wall)
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}

uint32_t studyDayLengthMs(const TzRule& zone, int32_t epochDay) {
    if (!zone.hasDst) return kSrsMsPerDay;
    const int64_t len = studyDayStartUtcMs(zone, epochDay + 1) - studyDayStartUtcMs(zone, epochDay);
    // Real rules shift by at most a couple of hours; anything else is a rule
    // this code does not understand, and a plain day is the safe answer.
    if (len < 20 * 3600000LL || len > 28 * 3600000LL) return kSrsMsPerDay;
    return static_cast<uint32_t>(len);
}

bool studyDayOf(int64_t utcMs, const TzRule& zone, int32_t* epochDay, uint32_t* msIntoDay) {
    if (!wallClockPlausible(utcMs, zone)) return false;
    // Shift so the study day starts at 00:00 of the shifted clock. The bounds
    // above keep it positive, so plain division is a floor.
    const int64_t local = utcMs + static_cast<int64_t>(tzOffsetSecAt(zone, utcMs)) * 1000;
    const int32_t day = static_cast<int32_t>((local - kRolloverMs) / kDayMs);
    if (epochDay) *epochDay = day;
    if (msIntoDay) {
        // Real time since the rollover (equal to the local time since 04:00
        // unless a daylight-saving change came in between).
        const int64_t len = studyDayLengthMs(zone, day);
        int64_t into = utcMs - studyDayStartUtcMs(zone, day);
        if (into < 0) into = 0;
        if (into >= len) into = len - 1;
        *msIntoDay = static_cast<uint32_t>(into);
    }
    return true;
}

bool studyDayOf(int64_t utcMs, int tzOffsetMin, int32_t* epochDay, uint32_t* msIntoDay) {
    return studyDayOf(utcMs, fixedTzRule(tzOffsetMin), epochDay, msIntoDay);
}

}  // namespace tgc
