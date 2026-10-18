// Native tests for time zones (src/srs/TimeZone, the rule-based study day in
// src/srs/WallClock and the SRS store's day lengths): POSIX TZ rule parsing,
// offsets against tzdata for the curated list, the 04:00 study day across
// daylight-saving changes, and the uptime/sleep rollover following them.
#include <unity.h>

#include <cstring>
#include <string>

#include "srs/SrsStore.h"
#include "srs/TimeZone.h"
#include "srs/WallClock.h"

using namespace tgc;

namespace {

constexpr int64_t kMinMs = 60000;
constexpr int64_t kHourMs = 3600000;
constexpr int64_t kDayMs = 86400000;

// UTC milliseconds of a UTC date and time.
int64_t utc(int y, unsigned m, unsigned d, int hh = 0, int mm = 0) {
    return static_cast<int64_t>(daysFromCivil(y, m, d)) * kDayMs + hh * kHourMs + mm * kMinMs;
}

TzRule rule(const char* s) {
    TzRule r;
    TEST_ASSERT_TRUE_MESSAGE(parseTzRule(s, &r), s);
    return r;
}

// UTC offset changes of the curated zones in 2026-2030, from the system tzdata
// (Python zoneinfo, tzdata 2026e): the offset at 2026-01-01T00:00Z, then each
// change as (UTC second, offset from then on, minutes east).
struct Change {
    int64_t t;
    int off;
};
struct ZoneTruth {
    const char* name;
    int first;
    Change changes[10];
};
const ZoneTruth kTruth[] = {
    {"America/Los_Angeles", -480,
     {{1772964000LL, -420}, {1793523600LL, -480}, {1805018400LL, -420}, {1825578000LL, -480},
      {1836468000LL, -420}, {1857027600LL, -480}, {1867917600LL, -420}, {1888477200LL, -480},
      {1899367200LL, -420}, {1919926800LL, -480}}},
    {"America/Denver", -420,
     {{1772960400LL, -360}, {1793520000LL, -420}, {1805014800LL, -360}, {1825574400LL, -420},
      {1836464400LL, -360}, {1857024000LL, -420}, {1867914000LL, -360}, {1888473600LL, -420},
      {1899363600LL, -360}, {1919923200LL, -420}}},
    {"America/Chicago", -360,
     {{1772956800LL, -300}, {1793516400LL, -360}, {1805011200LL, -300}, {1825570800LL, -360},
      {1836460800LL, -300}, {1857020400LL, -360}, {1867910400LL, -300}, {1888470000LL, -360},
      {1899360000LL, -300}, {1919919600LL, -360}}},
    {"America/Mexico_City", -360, {}},
    {"America/New_York", -300,
     {{1772953200LL, -240}, {1793512800LL, -300}, {1805007600LL, -240}, {1825567200LL, -300},
      {1836457200LL, -240}, {1857016800LL, -300}, {1867906800LL, -240}, {1888466400LL, -300},
      {1899356400LL, -240}, {1919916000LL, -300}}},
    {"America/Bogota", -300, {}},
    {"America/Lima", -300, {}},
    {"America/Caracas", -240, {}},
    {"America/Santiago", -180,
     {{1775358000LL, -240}, {1788667200LL, -180}, {1806807600LL, -240}, {1820116800LL, -180},
      {1838257200LL, -240}, {1851566400LL, -180}, {1870311600LL, -240}, {1883016000LL, -180},
      {1901761200LL, -240}, {1915070400LL, -180}}},
    {"America/Argentina/Buenos_Aires", -180, {}},
    {"America/Sao_Paulo", -180, {}},
    {"UTC", 0, {}},
    {"Europe/London", 0,
     {{1774746000LL, 60}, {1792890000LL, 0}, {1806195600LL, 60}, {1824944400LL, 0},
      {1837645200LL, 60}, {1856394000LL, 0}, {1869094800LL, 60}, {1887843600LL, 0},
      {1901149200LL, 60}, {1919293200LL, 0}}},
    {"Europe/Lisbon", 0,
     {{1774746000LL, 60}, {1792890000LL, 0}, {1806195600LL, 60}, {1824944400LL, 0},
      {1837645200LL, 60}, {1856394000LL, 0}, {1869094800LL, 60}, {1887843600LL, 0},
      {1901149200LL, 60}, {1919293200LL, 0}}},
    {"Europe/Madrid", 60,
     {{1774746000LL, 120}, {1792890000LL, 60}, {1806195600LL, 120}, {1824944400LL, 60},
      {1837645200LL, 120}, {1856394000LL, 60}, {1869094800LL, 120}, {1887843600LL, 60},
      {1901149200LL, 120}, {1919293200LL, 60}}},
    {"Europe/Athens", 120,
     {{1774746000LL, 180}, {1792890000LL, 120}, {1806195600LL, 180}, {1824944400LL, 120},
      {1837645200LL, 180}, {1856394000LL, 120}, {1869094800LL, 180}, {1887843600LL, 120},
      {1901149200LL, 180}, {1919293200LL, 120}}},
    {"Asia/Kolkata", 330, {}},
    {"Asia/Shanghai", 480, {}},
    {"Asia/Seoul", 540, {}},
    {"Asia/Tokyo", 540, {}},
    {"Australia/Sydney", 660,
     {{1775318400LL, 600}, {1791043200LL, 660}, {1806768000LL, 600}, {1822492800LL, 660},
      {1838217600LL, 600}, {1853942400LL, 660}, {1869667200LL, 600}, {1885996800LL, 660},
      {1901721600LL, 600}, {1917446400LL, 660}}},
    {"Pacific/Auckland", 780,
     {{1775311200LL, 720}, {1790431200LL, 780}, {1806760800LL, 720}, {1821880800LL, 780},
      {1838210400LL, 720}, {1853330400LL, 780}, {1869660000LL, 720}, {1885384800LL, 780},
      {1901714400LL, 720}, {1916834400LL, 780}}},
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_parse_rules() {
    TzRule r = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    TEST_ASSERT_EQUAL_INT32(3600, r.stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(7200, r.dstOffsetSec);
    TEST_ASSERT_TRUE(r.hasDst);
    TEST_ASSERT_EQUAL_UINT8(3, r.start.month);
    TEST_ASSERT_EQUAL_UINT8(5, r.start.week);
    TEST_ASSERT_EQUAL_UINT8(0, r.start.wday);
    TEST_ASSERT_EQUAL_INT32(7200, r.start.timeSec);  // the default 02:00
    TEST_ASSERT_EQUAL_UINT8(10, r.end.month);
    TEST_ASSERT_EQUAL_INT32(3 * 3600, r.end.timeSec);

    r = rule("EST5EDT,M3.2.0,M11.1.0");  // POSIX counts west: EST5 is UTC-5
    TEST_ASSERT_EQUAL_INT32(-5 * 3600, r.stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(-4 * 3600, r.dstOffsetSec);

    r = rule("<+0530>-5:30");  // quoted numeric name, minutes
    TEST_ASSERT_EQUAL_INT32(19800, r.stdOffsetSec);
    TEST_ASSERT_FALSE(r.hasDst);
    TEST_ASSERT_EQUAL_INT32(-3 * 3600, rule("<-03>3").stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(0, rule("UTC0").stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(14 * 3600, rule("<+14>-14").stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(-12 * 3600, rule("<-12>12").stdOffsetSec);

    // An explicit daylight offset (half an hour, Lord Howe), and times past 24 h
    // or negative (RFC 8536).
    r = rule("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    TEST_ASSERT_EQUAL_INT32(37800, r.stdOffsetSec);
    TEST_ASSERT_EQUAL_INT32(39600, r.dstOffsetSec);
    r = rule("<-04>4<-03>,M9.1.6/24,M4.1.6/24");
    TEST_ASSERT_EQUAL_INT32(24 * 3600, r.start.timeSec);
    TEST_ASSERT_EQUAL_UINT8(6, r.start.wday);
    r = rule("<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
    TEST_ASSERT_EQUAL_INT32(-3600, r.start.timeSec);
    TEST_ASSERT_EQUAL_INT32(0, r.end.timeSec);
    TEST_ASSERT_EQUAL_INT32(-22 * 3600, rule("EET-2EEST,M3.5.0/-22,M10.5.0/-22").start.timeSec);

    // Julian forms.
    r = rule("EST5EDT,J60/2:30,300");
    TEST_ASSERT_EQUAL_INT(TzDate::Julian1, r.start.kind);
    TEST_ASSERT_EQUAL_UINT16(60, r.start.day);
    TEST_ASSERT_EQUAL_INT32(2 * 3600 + 30 * 60, r.start.timeSec);
    TEST_ASSERT_EQUAL_INT(TzDate::Julian0, r.end.kind);
    TEST_ASSERT_EQUAL_UINT16(300, r.end.day);

    // The same offset twice is no daylight time at all.
    TEST_ASSERT_FALSE(rule("AAA-1BBB-1,M3.5.0,M10.5.0").hasDst);
}

void test_parse_rejects() {
    const char* bad[] = {
        "",
        "CET",                          // no offset
        "CE-1",                         // name too short
        "<+1>-1",                       // quoted name too short
        "<+01-1",                       // unterminated name
        "<+0 1>-1",                     // space in a quoted name
        "CET-1CEST",                    // daylight time without its dates
        "CET-1CEST,M3.5.0",             // one date
        "CET-1CEST,M3.5.0,M10.5.0,",    // trailing comma
        "CET-1CEST,M13.5.0,M10.5.0",    // month 13
        "CET-1CEST,M3.6.0,M10.5.0",     // week 6
        "CET-1CEST,M3.5.7,M10.5.0",     // weekday 7
        "CET-1CEST,M3.5,M10.5.0",       // no weekday
        "CET-1CEST,J0,J300",            // Julian day 0
        "CET-1CEST,J366,J300",
        "CET-1CEST,366,300",
        "CET-1CEST,M3.5.0/168,M10.5.0", // time past 167 h
        "CET-1:60",                     // minutes 60
        "CET-25",                       // offset past 24 h
        "<+15>-15",                     // past +14 h
        "<-13>13",                      // past -12 h
        "<+01>-1<+15>-15,M3.5.0,M10.5.0",
        "UTC0x",                        // trailing garbage
        "UTC 0",
        "0UTC",
    };
    TzRule r = fixedTzRule(77);
    for (const char* s : bad) TEST_ASSERT_FALSE_MESSAGE(parseTzRule(s, &r), s);
    TEST_ASSERT_FALSE(parseTzRule(nullptr, &r));
    // kTzRuleMaxLen (63) characters parse, 64 do not.
    const std::string fits = "AAA-1" + std::string(40, 'B') + ",M3.5.0,M10.5.0/30";
    TEST_ASSERT_EQUAL_UINT32(kTzRuleMaxLen, fits.size());
    TEST_ASSERT_FALSE(parseTzRule(("AAA-1B" + fits.substr(5)).c_str(), &r));
    TEST_ASSERT_EQUAL_INT32(77 * 60, r.stdOffsetSec);  // untouched by every failure
    TEST_ASSERT_TRUE(parseTzRule(fits.c_str(), &r));
}

// Every curated zone parses and gives tzdata's offsets around each change of
// 2026-2030; every truth row has its zone in the list and back.
void test_curated_zones_match_tzdata() {
    const size_t truthCount = sizeof kTruth / sizeof kTruth[0];
    TEST_ASSERT_EQUAL_UINT32(kTzZoneCount, truthCount);
    for (size_t i = 0; i < kTzZoneCount; ++i) TEST_ASSERT_NOT_NULL(findTzZone(kTzZones[i].name));
    TEST_ASSERT_NULL(findTzZone("Mars/Olympus_Mons"));
    TEST_ASSERT_NULL(findTzZone(""));
    TEST_ASSERT_NULL(findTzZone(nullptr));
    for (const ZoneTruth& z : kTruth) {
        const TzZone* zone = findTzZone(z.name);
        TEST_ASSERT_NOT_NULL_MESSAGE(zone, z.name);
        const TzRule r = rule(zone->rule);
        int off = z.first;
        TEST_ASSERT_EQUAL_INT_MESSAGE(off, tzOffsetMinAt(r, utc(2026, 1, 1)), z.name);
        int64_t prev = utc(2026, 1, 1) / 1000;
        for (const Change& c : z.changes) {
            if (c.t == 0) break;
            const int64_t ms = c.t * 1000;
            TEST_ASSERT_EQUAL_INT_MESSAGE(off, tzOffsetMinAt(r, (prev + c.t) / 2 * 1000), z.name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(off, tzOffsetMinAt(r, ms - 1), z.name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(c.off, tzOffsetMinAt(r, ms), z.name);
            off = c.off;
            prev = c.t;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(off, tzOffsetMinAt(r, utc(2030, 12, 31, 23, 59)), z.name);
    }
}

void test_fixed_rules_and_labels() {
    char buf[32];
    const struct {
        int min;
        const char* text;
    } fixed[] = {{120, "<+02>-2"}, {-210, "<-0330>3:30"}, {0, "<+00>0"},
                 {330, "<+0530>-5:30"}, {-300, "<-05>5"}, {840, "<+14>-14"}, {-720, "<-12>12"}};
    for (const auto& f : fixed) {
        formatFixedTzRule(f.min, buf, sizeof buf);
        TEST_ASSERT_EQUAL_STRING(f.text, buf);
        const TzRule r = rule(buf);  // and it parses back to the same offset
        TEST_ASSERT_EQUAL_INT32(f.min * 60, r.stdOffsetSec);
        TEST_ASSERT_EQUAL_INT(f.min, tzOffsetMinAt(r, utc(2026, 7, 1)));
        TEST_ASSERT_EQUAL_INT(f.min, tzOffsetMinAt(fixedTzRule(f.min), utc(2026, 7, 1)));
    }
    formatUtcOffset(0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC", buf);
    formatUtcOffset(60, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC+1", buf);
    formatUtcOffset(-180, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC-3", buf);
    formatUtcOffset(330, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC+5:30", buf);
    formatUtcOffset(-570, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC-9:30", buf);
    tzZoneCity("America/Argentina/Buenos_Aires", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Buenos Aires", buf);
    tzZoneCity("Europe/Madrid", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Madrid", buf);
    tzZoneCity("UTC", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("UTC", buf);
    tzZoneCity("", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("", buf);
    char small[5];
    tzZoneCity("America/Los_Angeles", small, sizeof small);
    TEST_ASSERT_EQUAL_STRING("Los ", small);  // cut, still terminated
}

namespace {

void expectDay(const TzRule& r, int64_t t, int y, unsigned m, unsigned d, int64_t intoMs) {
    int32_t day = 0;
    uint32_t into = 0;
    TEST_ASSERT_TRUE(studyDayOf(t, r, &day, &into));
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(y, m, d), day);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(intoMs), into);
}

}  // namespace

// Madrid: 2026-03-29 01:00Z (02:00 CET -> 03:00 CEST) and 2026-10-25 01:00Z
// (03:00 CEST -> 02:00 CET). The study day still starts at 04:00 local; the day
// that contains a change lasts 23 or 25 h.
void test_study_day_dst_madrid() {
    const TzRule r = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    TEST_ASSERT_EQUAL_INT64(utc(2026, 3, 28, 3), studyDayStartUtcMs(r, daysFromCivil(2026, 3, 28)));
    TEST_ASSERT_EQUAL_INT64(utc(2026, 3, 29, 2), studyDayStartUtcMs(r, daysFromCivil(2026, 3, 29)));
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 3, 28)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 3, 29)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 3, 27)));
    TEST_ASSERT_EQUAL_UINT32(25 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 10, 24)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 10, 25)));

    // Spring: 01:59 CET, the jump, 03:59 CEST and 04:00 CEST.
    expectDay(r, utc(2026, 3, 29, 0, 59), 2026, 3, 28, 21 * kHourMs + 59 * kMinMs);
    expectDay(r, utc(2026, 3, 29, 1, 0), 2026, 3, 28, 22 * kHourMs);
    expectDay(r, utc(2026, 3, 29, 1, 59), 2026, 3, 28, 22 * kHourMs + 59 * kMinMs);
    expectDay(r, utc(2026, 3, 29, 2, 0), 2026, 3, 29, 0);
    // Autumn: 02:59 CEST, 02:00 CET (the hour again), 03:59 CET, 04:00 CET.
    expectDay(r, utc(2026, 10, 25, 0, 59), 2026, 10, 24, 22 * kHourMs + 59 * kMinMs);
    expectDay(r, utc(2026, 10, 25, 1, 0), 2026, 10, 24, 23 * kHourMs);
    expectDay(r, utc(2026, 10, 25, 2, 59), 2026, 10, 24, 24 * kHourMs + 59 * kMinMs);
    expectDay(r, utc(2026, 10, 25, 3, 0), 2026, 10, 25, 0);
    // Summer and winter days are plain local days.
    expectDay(r, utc(2026, 7, 1, 1, 59), 2026, 6, 30, 23 * kHourMs + 59 * kMinMs);  // 03:59 CEST
    expectDay(r, utc(2026, 7, 1, 2, 0), 2026, 7, 1, 0);                             // 04:00 CEST
    expectDay(r, utc(2026, 12, 1, 3, 0), 2026, 12, 1, 0);                           // 04:00 CET
}

// New York (changes at 02:00 local) and Sydney (southern hemisphere: daylight
// time over the new year).
void test_study_day_dst_new_york_sydney() {
    const TzRule ny = rule("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, studyDayLengthMs(ny, daysFromCivil(2026, 3, 7)));
    TEST_ASSERT_EQUAL_UINT32(25 * kHourMs, studyDayLengthMs(ny, daysFromCivil(2026, 10, 31)));
    expectDay(ny, utc(2026, 3, 8, 7, 59), 2026, 3, 7, 22 * kHourMs + 59 * kMinMs);  // 03:59 EDT
    expectDay(ny, utc(2026, 3, 8, 8, 0), 2026, 3, 8, 0);                            // 04:00 EDT
    expectDay(ny, utc(2026, 11, 1, 8, 59), 2026, 10, 31, 24 * kHourMs + 59 * kMinMs);  // 03:59 EST
    expectDay(ny, utc(2026, 11, 1, 9, 0), 2026, 11, 1, 0);                              // 04:00 EST

    const TzRule syd = rule("AEST-10AEDT,M10.1.0,M4.1.0/3");
    // 2026-04-05 03:00 AEDT -> 02:00 AEST (2026-04-04 16:00Z); 2026-10-04 02:00
    // AEST -> 03:00 AEDT (2026-10-03 16:00Z).
    TEST_ASSERT_EQUAL_UINT32(25 * kHourMs, studyDayLengthMs(syd, daysFromCivil(2026, 4, 4)));
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, studyDayLengthMs(syd, daysFromCivil(2026, 10, 3)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(syd, daysFromCivil(2026, 12, 31)));
    expectDay(syd, utc(2026, 1, 1, 16, 59), 2026, 1, 1, 23 * kHourMs + 59 * kMinMs);  // 03:59 AEDT
    expectDay(syd, utc(2026, 1, 1, 17, 0), 2026, 1, 2, 0);                            // 04:00 AEDT
    expectDay(syd, utc(2026, 4, 4, 18, 0), 2026, 4, 5, 0);                            // 04:00 AEST
    expectDay(syd, utc(2026, 10, 3, 17, 0), 2026, 10, 4, 0);                          // 04:00 AEDT
}

// Santiago changes at local midnight ("M9.1.6/24": the first Saturday at
// 24:00), so the day before the change is the long or short one.
void test_study_day_santiago() {
    const TzRule r = rule(findTzZone("America/Santiago")->rule);
    // 2026-04-05 00:00 -03 -> 2026-04-04 23:00 -04 (03:00Z).
    TEST_ASSERT_EQUAL_INT(-180, tzOffsetMinAt(r, utc(2026, 4, 5, 2, 59)));
    TEST_ASSERT_EQUAL_INT(-240, tzOffsetMinAt(r, utc(2026, 4, 5, 3, 0)));
    TEST_ASSERT_EQUAL_UINT32(25 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 4, 4)));
    expectDay(r, utc(2026, 4, 5, 3, 30), 2026, 4, 4, 20 * kHourMs + 30 * kMinMs);  // 23:30 -04 again
    expectDay(r, utc(2026, 4, 5, 8, 0), 2026, 4, 5, 0);                            // 04:00 -04
    // 2026-09-06 00:00 -04 -> 01:00 -03 (04:00Z).
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 9, 5)));
    expectDay(r, utc(2026, 9, 6, 7, 0), 2026, 9, 6, 0);  // 04:00 -03
    // 2030: Sunday 1 September; the change is the Sunday after the first Saturday.
    TEST_ASSERT_EQUAL_INT(-240, tzOffsetMinAt(r, utc(2030, 9, 1, 12)));
    TEST_ASSERT_EQUAL_INT(-180, tzOffsetMinAt(r, utc(2030, 9, 8, 12)));
}

// A rule whose changes fall on 04:00 itself: the spring jump skips it (the day
// starts at the jump), the autumn hour repeats it (the first one counts).
void test_study_day_change_at_rollover() {
    const TzRule r = rule("<+01>-1<+02>,M3.5.0/4,M10.5.0/5");
    // 2026-03-29 04:00 +01 (03:00Z) -> 05:00 +02.
    // The day before still has its 24 h; the day of the jump is the short one.
    TEST_ASSERT_EQUAL_INT64(utc(2026, 3, 29, 3), studyDayStartUtcMs(r, daysFromCivil(2026, 3, 29)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 3, 28)));
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 3, 29)));
    expectDay(r, utc(2026, 3, 29, 2, 59), 2026, 3, 28, 23 * kHourMs + 59 * kMinMs);  // 03:59 +01
    expectDay(r, utc(2026, 3, 29, 3, 0), 2026, 3, 29, 0);                            // 05:00 +02
    // 2026-10-25 05:00 +02 (03:00Z) -> 04:00 +01: 04:00 +02 (02:00Z) comes first.
    TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 25, 2), studyDayStartUtcMs(r, daysFromCivil(2026, 10, 25)));
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 10, 24)));
    TEST_ASSERT_EQUAL_UINT32(25 * kHourMs, studyDayLengthMs(r, daysFromCivil(2026, 10, 25)));
    expectDay(r, utc(2026, 10, 25, 3, 30), 2026, 10, 25, 90 * kMinMs);  // 04:30 +01, the second time
}

// Without daylight time the rule form is the old offset form exactly.
void test_study_day_fixed_matches_offset() {
    const int zones[] = {0, 120, -300, 330, 14 * 60, -12 * 60};
    for (int tz : zones) {
        for (int64_t t = utc(2026, 10, 20); t < utc(2026, 10, 23); t += 37 * kMinMs) {
            int32_t d1 = 0, d2 = 0;
            uint32_t i1 = 0, i2 = 0;
            TEST_ASSERT_TRUE(studyDayOf(t, tz, &d1, &i1));
            TEST_ASSERT_TRUE(studyDayOf(t, fixedTzRule(tz), &d2, &i2));
            TEST_ASSERT_EQUAL_INT32(d1, d2);
            TEST_ASSERT_EQUAL_UINT32(i1, i2);
            // and the old arithmetic: (local - 4 h) split into days
            const int64_t shifted = t + tz * kMinMs - 4 * kHourMs;
            TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(shifted / kDayMs), d1);
            TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(shifted % kDayMs), i1);
        }
    }
    // A rule with an implausible offset is refused like the offset.
    TzRule wild = fixedTzRule(15 * 60);
    TEST_ASSERT_FALSE(studyDayOf(utc(2026, 10, 20), wild, nullptr, nullptr));
    TEST_ASSERT_FALSE(wallClockPlausible(utc(2026, 10, 20), wild));
    TEST_ASSERT_FALSE(studyDayOf(utc(2025, 12, 31), rule("UTC0"), nullptr, nullptr));
}

// The store's uptime/sleep rollover follows the zone: after a sync, adding
// elapsed time in any steps gives the same day and carry as a fresh sync at the
// new instant, across daylight-saving changes, for a few zones and two years.
void test_store_rollover_follows_zone() {
    const char* names[] = {"Europe/Madrid", "America/New_York", "Australia/Sydney",
                           "America/Santiago", "Pacific/Auckland", "Europe/London",
                           "Asia/Tokyo"};
    for (const char* name : names) {
        const TzRule r = rule(findTzZone(name)->rule);
        SrsStore s;
        MemoryIo io;
        s.begin(io);
        s.setTimeZone(r);
        s.setToday(40);
        int64_t t = utc(2026, 1, 3, 15, 17);
        TEST_ASSERT_TRUE(s.syncWallClock(t, r).kind == ClockSync::Anchored);
        // Odd steps so they land at every hour of the day over time.
        const int64_t steps[] = {97 * kMinMs, 5 * kHourMs + 3 * kMinMs, 31 * kMinMs,
                                 19 * kHourMs + 59 * kMinMs};
        for (int i = 0; t < utc(2028, 1, 1); ++i) {
            const int64_t step = steps[i % 4];
            if (i % 2) {
                s.addElapsedMs(static_cast<uint64_t>(step));
            } else {  // in two parts, as tick() and a wake would
                s.addElapsedMs(static_cast<uint64_t>(step / 2));
                s.addElapsedMs(static_cast<uint64_t>(step - step / 2));
            }
            t += step;
            int32_t day = 0;
            uint32_t into = 0;
            TEST_ASSERT_TRUE(studyDayOf(t, r, &day, &into));
            TEST_ASSERT_EQUAL_INT32_MESSAGE(day, s.epochDayOf(s.today()), name);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(into, s.msCarry(), name);
        }
        // A deep sleep of a year and a half in one go lands on the same day too.
        s.addElapsedMs(static_cast<uint64_t>(547 * kDayMs + 7 * kHourMs));
        t += 547 * kDayMs + 7 * kHourMs;
        int32_t day = 0;
        uint32_t into = 0;
        TEST_ASSERT_TRUE(studyDayOf(t, r, &day, &into));
        TEST_ASSERT_EQUAL_INT32_MESSAGE(day, s.epochDayOf(s.today()), name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(into, s.msCarry(), name);
    }
}

// Madrid spring change seen by the store: the rollover after a Saturday-noon
// sync comes 15 h later (04:00 CEST), not 16 h; in autumn 17 h, not 16 h.
void test_store_madrid_rollover_hours() {
    const TzRule r = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setTimeZone(r);
    s.setToday(10);
    TEST_ASSERT_TRUE(s.syncWallClock(utc(2026, 3, 28, 11), r).kind == ClockSync::Anchored);
    TEST_ASSERT_EQUAL_UINT32(8 * kHourMs, s.msCarry());
    TEST_ASSERT_EQUAL_UINT32(23 * kHourMs, s.dayLengthMs());
    s.addElapsedMs(15 * kHourMs - 1);
    TEST_ASSERT_EQUAL_UINT16(10, s.today());
    s.addElapsedMs(1);
    TEST_ASSERT_EQUAL_UINT16(11, s.today());
    TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, s.dayLengthMs());

    TEST_ASSERT_TRUE(s.syncWallClock(utc(2026, 10, 24, 10), r).kind == ClockSync::Advanced);
    const uint16_t day = s.today();
    TEST_ASSERT_EQUAL_UINT32(8 * kHourMs, s.msCarry());
    s.addElapsedMs(17 * kHourMs - 1);
    TEST_ASSERT_EQUAL_UINT16(day, s.today());
    s.tick(1000);  // the tick rule goes through the same carry
    s.tick(1001);
    TEST_ASSERT_EQUAL_UINT16(day + 1, s.today());

    // Without an anchor the real date is unknown: days are 24 h as before.
    SrsStore fresh;
    MemoryIo empty;
    fresh.begin(empty);
    fresh.setTimeZone(r);
    TEST_ASSERT_EQUAL_UINT32(kSrsMsPerDay, fresh.dayLengthMs());
    fresh.addElapsedMs(kSrsMsPerDay - 1);
    TEST_ASSERT_EQUAL_UINT16(0, fresh.today());
    fresh.addElapsedMs(1);
    TEST_ASSERT_EQUAL_UINT16(1, fresh.today());
}

// syncWallClock with a rule uses the offset in effect at that instant, and
// the offset form is the fixed rule.
void test_store_sync_with_rule() {
    const TzRule r = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    SrsStore a, b;
    MemoryIo io;
    a.begin(io);
    b.begin(io);
    a.setToday(3);
    b.setToday(3);
    const int64_t t = utc(2026, 7, 1, 1, 30);  // 03:30 CEST: still June 30
    TEST_ASSERT_TRUE(a.syncWallClock(t, r).kind == ClockSync::Anchored);
    TEST_ASSERT_TRUE(b.syncWallClock(t, 120).kind == ClockSync::Anchored);
    TEST_ASSERT_EQUAL_UINT16(a.epochAnchor(), b.epochAnchor());
    TEST_ASSERT_EQUAL_UINT32(a.msCarry(), b.msCarry());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 30), a.epochDayOf(a.today()));
    // With the winter offset the same instant would be an hour earlier locally.
    SrsStore c;
    c.begin(io);
    c.setToday(3);
    TEST_ASSERT_TRUE(c.syncWallClock(t, 60).kind == ClockSync::Anchored);
    TEST_ASSERT_EQUAL_UINT32(a.msCarry() - kHourMs, c.msCarry());
    // An implausible time changes nothing.
    TEST_ASSERT_TRUE(a.syncWallClock(utc(2025, 1, 1), r).kind == ClockSync::Rejected);
    // The zone is not part of the file: UTC by default, kept by begin().
    TEST_ASSERT_FALSE(a.timeZone().hasDst);
    TEST_ASSERT_EQUAL_INT32(0, a.timeZone().stdOffsetSec);
    a.setTimeZone(r);
    a.begin(io);
    TEST_ASSERT_TRUE(a.timeZone().hasDst);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_rules);
    RUN_TEST(test_parse_rejects);
    RUN_TEST(test_curated_zones_match_tzdata);
    RUN_TEST(test_fixed_rules_and_labels);
    RUN_TEST(test_study_day_dst_madrid);
    RUN_TEST(test_study_day_dst_new_york_sydney);
    RUN_TEST(test_study_day_santiago);
    RUN_TEST(test_study_day_change_at_rollover);
    RUN_TEST(test_study_day_fixed_matches_offset);
    RUN_TEST(test_store_rollover_follows_zone);
    RUN_TEST(test_store_madrid_rollover_hours);
    RUN_TEST(test_store_sync_with_rule);
    return UNITY_END();
}
