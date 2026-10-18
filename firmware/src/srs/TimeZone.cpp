// POSIX TZ rules and the curated zone list. Design: docs/srs.md section 5.1.
#include "srs/TimeZone.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "srs/WallClock.h"

namespace tgc {

namespace {

constexpr int32_t kDaySec = 86400;

bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// A zone abbreviation: 3+ letters, or <...> holding 3+ letters, digits, '+'
// or '-'.
bool parseName(const char*& p) {
    if (*p == '<') {
        const char* q = p + 1;
        int n = 0;
        for (; *q && *q != '>'; ++q, ++n)
            if (!isAlpha(*q) && !isDigit(*q) && *q != '+' && *q != '-') return false;
        if (*q != '>' || n < 3) return false;
        p = q + 1;
        return true;
    }
    int n = 0;
    while (isAlpha(p[n])) ++n;
    if (n < 3) return false;
    p += n;
    return true;
}

// 1..3 digits in [lo, hi].
bool parseNum(const char*& p, int lo, int hi, int* out) {
    int v = 0, digits = 0;
    while (isDigit(*p) && digits < 3) {
        v = v * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    if (digits == 0 || v < lo || v > hi) return false;
    *out = v;
    return true;
}

// [+-]h[h][:mm[:ss]] in seconds, hours at most `maxH`.
bool parseHms(const char*& p, int maxH, int32_t* out) {
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        ++p;
    }
    int h = 0, m = 0, s = 0;
    if (!parseNum(p, 0, maxH, &h)) return false;
    if (*p == ':') {
        ++p;
        if (!parseNum(p, 0, 59, &m)) return false;
        if (*p == ':') {
            ++p;
            if (!parseNum(p, 0, 59, &s)) return false;
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

// Mm.w.d, Jn or n, then an optional /time (-167..167 h, RFC 8536).
bool parseDate(const char*& p, TzDate* out) {
    TzDate d;
    int v = 0;
    if (*p == 'M') {
        ++p;
        if (!parseNum(p, 1, 12, &v)) return false;
        d.month = static_cast<uint8_t>(v);
        if (*p != '.') return false;
        ++p;
        if (!parseNum(p, 1, 5, &v)) return false;
        d.week = static_cast<uint8_t>(v);
        if (*p != '.') return false;
        ++p;
        if (!parseNum(p, 0, 6, &v)) return false;
        d.wday = static_cast<uint8_t>(v);
        d.kind = TzDate::MonthWeekDay;
    } else if (*p == 'J') {
        ++p;
        if (!parseNum(p, 1, 365, &v)) return false;
        d.day = static_cast<uint16_t>(v);
        d.kind = TzDate::Julian1;
    } else {
        if (!parseNum(p, 0, 365, &v)) return false;
        d.day = static_cast<uint16_t>(v);
        d.kind = TzDate::Julian0;
    }
    if (*p == '/') {
        ++p;
        if (!parseHms(p, 167, &d.timeSec)) return false;
    }
    *out = d;
    return true;
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The UTC second at which `d` happens in `year`, given the offset in effect
// just before it.
int64_t transitionUtcSec(const TzDate& d, int year, int32_t offsetSec) {
    int32_t day = 0;
    if (d.kind == TzDate::MonthWeekDay) {
        const int32_t first = daysFromCivil(year, d.month, 1);
        const int32_t next = d.month == 12 ? daysFromCivil(year + 1, 1, 1)
                                           : daysFromCivil(year, d.month + 1u, 1);
        int32_t dom0 = (d.wday - weekdayFromDays(first) + 7) % 7 + (d.week - 1) * 7;
        while (first + dom0 >= next) dom0 -= 7;  // week 5: the last one
        day = first + dom0;
    } else {
        day = daysFromCivil(year, 1, 1) + d.day;
        if (d.kind == TzDate::Julian1) day += (isLeap(year) && d.day >= 60) ? 0 : -1;
    }
    return static_cast<int64_t>(day) * kDaySec + d.timeSec - offsetSec;
}

}  // namespace

bool parseTzRule(const char* s, TzRule* out) {
    if (!s || !out) return false;
    const size_t len = strnlen(s, kTzRuleMaxLen + 1);
    if (len == 0 || len > kTzRuleMaxLen) return false;
    const char* p = s;
    TzRule r;
    int32_t off = 0;
    if (!parseName(p) || !parseHms(p, 24, &off)) return false;
    r.stdOffsetSec = -off;  // POSIX counts hours west of UTC
    r.dstOffsetSec = r.stdOffsetSec;
    if (*p != '\0') {
        if (!parseName(p)) return false;
        r.dstOffsetSec = r.stdOffsetSec + 3600;
        if (*p != ',') {
            if (!parseHms(p, 24, &off)) return false;
            r.dstOffsetSec = -off;
        }
        // The dates are optional in POSIX (with an implementation default);
        // here they are required: no guessing.
        if (*p != ',') return false;
        ++p;
        if (!parseDate(p, &r.start) || *p != ',') return false;
        ++p;
        if (!parseDate(p, &r.end) || *p != '\0') return false;
        r.hasDst = r.dstOffsetSec != r.stdOffsetSec;
        if (!r.hasDst) r.start = r.end = TzDate{};
    }
    if (!tzRuleOffsetsPlausible(r)) return false;
    *out = r;
    return true;
}

TzRule fixedTzRule(int offsetMin) {
    TzRule r;
    r.stdOffsetSec = r.dstOffsetSec = offsetMin * 60;
    return r;
}

void formatFixedTzRule(int offsetMin, char* out, size_t n) {
    if (!out || n == 0) return;
    const int a = offsetMin < 0 ? -offsetMin : offsetMin;
    const char sign = offsetMin < 0 ? '-' : '+';
    const char* posixSign = offsetMin > 0 ? "-" : "";  // POSIX counts west
    if (a % 60)
        snprintf(out, n, "<%c%02d%02d>%s%d:%02d", sign, a / 60, a % 60, posixSign, a / 60, a % 60);
    else
        snprintf(out, n, "<%c%02d>%s%d", sign, a / 60, posixSign, a / 60);
}

bool tzRuleOffsetsPlausible(const TzRule& r) {
    for (int32_t o : {r.stdOffsetSec, r.dstOffsetSec})
        if (o % 60 != 0 || o / 60 < kTzOffsetMinLowest || o / 60 > kTzOffsetMinHighest)
            return false;
    return true;
}

int32_t tzOffsetSecAt(const TzRule& r, int64_t utcMs) {
    if (!r.hasDst) return r.stdOffsetSec;
    const int64_t t = floorDiv(utcMs, 1000);
    // The year as the local standard time sees it; both transitions of that
    // year decide (start before end: northern; start after end: southern).
    int y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(static_cast<int32_t>(floorDiv(t + r.stdOffsetSec, kDaySec)), &y, &m, &d);
    const int64_t s = transitionUtcSec(r.start, y, r.stdOffsetSec);
    const int64_t e = transitionUtcSec(r.end, y, r.dstOffsetSec);
    const bool dst = s < e ? (t >= s && t < e) : (t < e || t >= s);
    return dst ? r.dstOffsetSec : r.stdOffsetSec;
}

void formatUtcOffset(int offsetMin, char* out, size_t n) {
    if (!out || n == 0) return;
    if (offsetMin == 0) {
        snprintf(out, n, "UTC");
        return;
    }
    const int a = offsetMin < 0 ? -offsetMin : offsetMin;
    if (a % 60)
        snprintf(out, n, "UTC%c%d:%02d", offsetMin < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(out, n, "UTC%c%d", offsetMin < 0 ? '-' : '+', a / 60);
}

void tzZoneCity(const char* name, char* out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!name) return;
    const char* slash = std::strrchr(name, '/');
    const char* city = slash ? slash + 1 : name;
    size_t i = 0;
    for (; city[i] && i + 1 < n; ++i) out[i] = city[i] == '_' ? ' ' : city[i];
    out[i] = '\0';
}

// The rules are the POSIX footers of tzdata 2026e for these zones (test_tz
// checks each against known transitions). West to east by standard offset.
const TzZone kTzZones[] = {
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Bogota", "<-05>5"},
    {"America/Lima", "<-05>5"},
    {"America/Caracas", "<-04>4"},
    {"America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/Sao_Paulo", "<-03>3"},
    {"UTC", "UTC0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Tokyo", "JST-9"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
const size_t kTzZoneCount = sizeof kTzZones / sizeof kTzZones[0];

const TzZone* findTzZone(const char* name) {
    if (!name || !*name) return nullptr;
    for (size_t i = 0; i < kTzZoneCount; ++i)
        if (std::strcmp(kTzZones[i].name, name) == 0) return &kTzZones[i];
    return nullptr;
}

}  // namespace tgc
