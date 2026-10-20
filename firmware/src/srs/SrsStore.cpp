// Persisted review state, the session day clock and the v1 file format.
// Design: docs/srs.md sections 2, 4 and 5.
#include "srs/SrsStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "srs/WallClock.h"

namespace tgc {
namespace {

constexpr char kMagic[4] = {'T', 'N', 'G', 'S'};
constexpr uint16_t kVersion = 1;

// Header field offsets (docs/srs.md section 4).
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffHeaderSize = 6;
constexpr size_t kOffNowDay = 8;
constexpr size_t kOffCount = 10;
constexpr size_t kOffMsCarry = 12;
constexpr size_t kOffAnsweredTotal = 16;
constexpr size_t kOffSlowMs = 20;
constexpr size_t kOffNewPerDay = 22;
constexpr size_t kOffNewIntroducedDay = 24;
constexpr size_t kOffNewIntroducedCount = 26;
constexpr size_t kOffMaxInterval = 28;
constexpr size_t kOffFlags = 30;
// 32..47: v1-compatible extension (docs/srs.md section 4). 0 = absent, so an
// old file (reserved bytes were zero) loads as defaults and no version bump is
// needed; an old reader ignores them (the CRC still covers them).
constexpr size_t kOffStreakCurrent = 32;
constexpr size_t kOffStreakBest = 34;
constexpr size_t kOffLastStudyDay = 36;
constexpr size_t kOffEpochAnchor = 38;  // 0 = never synced (section 5.1)
constexpr size_t kOffReviewMs = 40;
constexpr size_t kOffAgainTotal = 44;  // 0 in an older file: recovered from the records

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// CRC-32 (poly 0xEDB88320, init/final 0xFFFFFFFF), bit-at-a-time to avoid a
// 1 KB table in flash.
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

inline void writeRecord(uint8_t* p, const Record& r) {
    wr32(p + 0, r.codepoint);
    wr16(p + 4, r.stabilityCds);
    wr16(p + 6, r.dueDay);
    wr16(p + 8, r.lastDay);
    p[10] = r.difficulty10;
    p[11] = r.reps;
    p[12] = r.lapses;
    p[13] = r.modeSeen;
    p[14] = r.lastGrade;
    p[15] = r.flags;
}

inline Record readRecord(const uint8_t* p) {
    Record r{};
    r.codepoint = rd32(p + 0);
    r.stabilityCds = rd16(p + 4);
    r.dueDay = rd16(p + 6);
    r.lastDay = rd16(p + 8);
    r.difficulty10 = p[10];
    r.reps = p[11];
    r.lapses = p[12];
    r.modeSeen = p[13];
    r.lastGrade = p[14];
    r.flags = p[15];
    return r;
}

inline uint16_t clampU16(uint32_t v, uint16_t lo, uint16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return static_cast<uint16_t>(v);
}

}  // namespace

void SrsStore::reset() {
    records_.clear();
    scratch_ = Record{};
    nowDay_ = 0;
    msCarry_ = 0;
    answeredTotal_ = 0;
    againTotal_ = 0;
    slowMs_ = kSrsDefaultSlowMs;
    newPerDay_ = kSrsDefaultNewPerDay;
    newIntroducedDay_ = 0;
    newIntroducedCount_ = 0;
    maxIntervalDays_ = kSrsDefaultMaxIntervalDays;
    flags_ = 0;
    streakCurrent_ = 0;
    streakBest_ = 0;
    lastStudyDay_ = 0;
    reviewMs_ = 0;
    epochAnchor_ = 0;
    tickStarted_ = false;
    lastTickMs_ = 0;
}

bool SrsStore::begin(StateIo& io) {
    reset();
    std::vector<uint8_t> buf;
    const bool ok = io.load(buf) && deserialize(buf.data(), buf.size());
    if (!ok) reset();
    tickStarted_ = false;
    lastTickMs_ = 0;
    return ok;
}

void SrsStore::resetNewIfNewDay() {
    if (newIntroducedDay_ != nowDay_) {
        newIntroducedDay_ = nowDay_;
        newIntroducedCount_ = 0;
    }
}

void SrsStore::beginSession() {
    tickStarted_ = false;
    lastTickMs_ = 0;
    resetNewIfNewDay();
}

void SrsStore::advanceDays(uint16_t n) {
    const uint32_t day = static_cast<uint32_t>(nowDay_) + n;
    nowDay_ = day > 0xFFFFu ? 0xFFFF : static_cast<uint16_t>(day);
    resetNewIfNewDay();
}

void SrsStore::advanceDaysManual(uint16_t n) {
    if (n == 0) return;
    advanceDays(n);
    msCarry_ = 0;
}

int32_t SrsStore::manualStepsMin() const {
    if (epochAnchor_ == 0) return 0;
    // The earliest date a sync accepts, and an anchor of at least 1 (0 means
    // "none"): the date shown for today() may not go below either.
    int32_t floor = kWallClockMinEpochDay;
    if (floor < static_cast<int32_t>(nowDay_) + 1) floor = static_cast<int32_t>(nowDay_) + 1;
    const int32_t steps = floor - epochDayOf(nowDay_);
    return steps < 0 ? steps : 0;
}

int32_t SrsStore::manualStepsMax() const { return 0xFFFF - static_cast<int32_t>(nowDay_); }

ManualDate SrsStore::setDateManual(int32_t steps) {
    if (steps < manualStepsMin() || steps > manualStepsMax()) return ManualDate::Rejected;
    if (steps == 0) return ManualDate::Same;
    if (steps > 0) {
        advanceDaysManual(static_cast<uint16_t>(steps));
        return ManualDate::Advanced;
    }
    // The index stays; the current day shows the confirmed date. The range
    // keeps the anchor within 1..its old value.
    epochAnchor_ = static_cast<uint16_t>(static_cast<int32_t>(epochAnchor_) + steps);
    msCarry_ = 0;  // late, never early, as for a forward step
    return ManualDate::Reanchored;
}

ClockSyncResult SrsStore::syncWallClock(int64_t utcMs, int tzOffsetMin) {
    return syncWallClock(utcMs, fixedTzRule(tzOffsetMin));
}

ClockSyncResult SrsStore::syncWallClock(int64_t utcMs, const TzRule& zone) {
    ClockSyncResult res;
    int32_t real = 0;
    uint32_t intoDay = 0;
    if (!studyDayOf(utcMs, zone, &real, &intoDay)) return res;

    // The index after the sync: forward to the real study day when it is ahead
    // (saturating like advanceDays), never back.
    uint16_t day = nowDay_;
    if (epochAnchor_ != 0) {
        const int32_t target = real - static_cast<int32_t>(epochAnchor_);
        if (target > static_cast<int32_t>(nowDay_))
            day = target > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(target);
    }
    // The anchor that shows that index as the real date. Unchanged for Advanced
    // and Same; it can only fall out of range for an index tens of thousands of
    // days old, which no real use reaches -- refuse rather than wrap.
    const int32_t anchor = real - static_cast<int32_t>(day);
    if (anchor < 1 || anchor > 0xFFFF) return res;

    if (epochAnchor_ == 0)
        res.kind = ClockSync::Anchored;
    else if (day > nowDay_)
        res.kind = ClockSync::Advanced;
    else if (anchor == static_cast<int32_t>(epochAnchor_))
        res.kind = ClockSync::Same;
    else
        res.kind = ClockSync::Reanchored;

    if (day > nowDay_) {
        res.daysAdvanced = static_cast<uint16_t>(day - nowDay_);
        advanceDays(res.daysAdvanced);  // also starts the new day's new-card budget
    }
    epochAnchor_ = static_cast<uint16_t>(anchor);
    // The next uptime/sleep rollover falls at the next local 04:00.
    msCarry_ = intoDay;
    return res;
}

int32_t SrsStore::epochDayOf(uint16_t day) const {
    const int32_t base =
        epochAnchor_ != 0 ? static_cast<int32_t>(epochAnchor_) : kSrsLegacyEpochDay;
    return base + static_cast<int32_t>(day);
}

void SrsStore::tick(uint32_t nowMs) {
    if (!tickStarted_) {
        lastTickMs_ = nowMs;
        tickStarted_ = true;
        return;
    }
    // Unsigned subtraction is wrap-safe for a 32-bit millisecond clock.
    const uint32_t delta = nowMs - lastTickMs_;
    lastTickMs_ = nowMs;
    addElapsedMs(delta);
}

void SrsStore::addElapsedMs(uint64_t ms) {
    uint64_t carry = static_cast<uint64_t>(msCarry_) + ms;
    if (!tz_.hasDst || epochAnchor_ == 0) {
        const uint64_t days = carry / kSrsMsPerDay;
        const uint32_t day = static_cast<uint32_t>(nowDay_) + (days > 0xFFFFu ? 0xFFFFu : days);
        nowDay_ = day > 0xFFFFu ? 0xFFFF : static_cast<uint16_t>(day);
        msCarry_ = static_cast<uint32_t>(carry % kSrsMsPerDay);
    } else {
        // Day by day, each as long as the zone makes it (23/24/25 h). A long
        // deep sleep is a few hundred iterations, each one cached lookup.
        for (uint32_t len = dayLengthMs(); carry >= len && nowDay_ < 0xFFFF; len = dayLengthMs()) {
            carry -= len;
            ++nowDay_;
        }
        msCarry_ = static_cast<uint32_t>(nowDay_ == 0xFFFF ? carry % kSrsMsPerDay : carry);
    }
    resetNewIfNewDay();
}

void SrsStore::setTimeZone(const TzRule& zone) {
    tz_ = zone;
    lenCacheDay_ = -1;
}

uint32_t SrsStore::dayLengthMs() const {
    if (!tz_.hasDst || epochAnchor_ == 0) return kSrsMsPerDay;
    const int32_t day = static_cast<int32_t>(epochAnchor_) + nowDay_;
    if (day != lenCacheDay_) {
        lenCacheMs_ = studyDayLengthMs(tz_, day);
        lenCacheDay_ = day;
    }
    return lenCacheMs_;
}

void SrsStore::setToday(uint16_t day) {
    nowDay_ = day;
    tickStarted_ = false;
    resetNewIfNewDay();
}

const Record* SrsStore::find(uint32_t codepoint) const {
    size_t lo = 0, hi = records_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (records_[mid].codepoint == codepoint) return &records_[mid];
        if (records_[mid].codepoint < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

Record* SrsStore::find(uint32_t codepoint) {
    return const_cast<Record*>(static_cast<const SrsStore*>(this)->find(codepoint));
}

Stats SrsStore::stats() const {
    Stats s{};
    s.seen = static_cast<uint32_t>(records_.size());
    s.answers = answeredTotal_;
    s.again = againTotal_;
    s.streakCurrent = streakCurrent_;
    s.streakBest = streakBest_;
    s.reviewMs = reviewMs_;
    for (const Record& r : records_) {
        if (r.stabilityCds / 100u >= kLearnedStabilityDays) ++s.learned;
        s.reps += r.reps;
        s.lapses += r.lapses;
    }
    return s;
}

size_t SrsStore::forget(const uint32_t* codepoints, size_t n) {
    if (!codepoints || n == 0 || records_.empty()) return 0;
    // A sorted view of the list, so each record is one binary search.
    std::vector<uint32_t> sorted;
    const uint32_t* first = codepoints;
    const uint32_t* last = codepoints + n;
    if (!std::is_sorted(first, last)) {
        sorted.assign(first, last);
        std::sort(sorted.begin(), sorted.end());
        first = sorted.data();
        last = first + sorted.size();
    }
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [&](const Record& r) {
                                      return std::binary_search(first, last, r.codepoint);
                                  }),
                   records_.end());
    return before - records_.size();
}

size_t SrsStore::resetProgress() {
    const size_t n = records_.size();
    records_.clear();
    records_.shrink_to_fit();
    answeredTotal_ = 0;
    againTotal_ = 0;
    reviewMs_ = 0;
    streakCurrent_ = 0;
    streakBest_ = 0;
    lastStudyDay_ = 0;
    flags_ = static_cast<uint16_t>(flags_ & ~kSrsFlagHasStudied);
    newIntroducedDay_ = nowDay_;
    newIntroducedCount_ = 0;
    return n;
}

Record& SrsStore::getOrCreate(uint32_t codepoint) {
    size_t lo = 0, hi = records_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (records_[mid].codepoint == codepoint) return records_[mid];
        if (records_[mid].codepoint < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (records_.size() >= kMaxRecords) {
        // Past the cap (section 2): hand out a scratch record that is not in
        // the table, so callers cannot grow it by accident.
        scratch_ = Record{};
        scratch_.codepoint = codepoint;
        scratch_.lastGrade = 0xFF;
        return scratch_;
    }
    Record r{};
    r.codepoint = codepoint;
    r.lastGrade = 0xFF;
    return *records_.insert(records_.begin() + static_cast<std::ptrdiff_t>(lo), r);
}

void SrsStore::noteNewIntroduced(uint16_t day) {
    if (newIntroducedDay_ != day) {
        newIntroducedDay_ = day;
        newIntroducedCount_ = 0;
    }
    if (newIntroducedCount_ < 0xFFFF) ++newIntroducedCount_;
}

// Day streak over the SRS day index (docs/srs.md section 4, "Streak"): the first
// answer of a new day either extends the streak (exactly the day after the last
// study day) or resets it to 1. Same-day answers never change it. 32-bit maths
// avoids a wrap when the last study day is 0xFFFF. Whether anything was ever
// studied is the persisted flags bit, not `lastStudyDay_ == 0`, so the first
// answer on day index 0 starts a streak of 1 instead of looking like "same day".
void SrsStore::noteStudyDay(uint16_t day) {
    const bool studied = (flags_ & kSrsFlagHasStudied) != 0;
    if (studied && day == lastStudyDay_) return;
    const bool consecutive =
        studied && static_cast<uint32_t>(day) == static_cast<uint32_t>(lastStudyDay_) + 1u;
    if (consecutive) {
        if (streakCurrent_ < 0xFFFF) ++streakCurrent_;
    } else {
        streakCurrent_ = 1;
    }
    lastStudyDay_ = day;
    flags_ |= kSrsFlagHasStudied;
    if (streakCurrent_ > streakBest_) streakBest_ = streakCurrent_;
}

void SrsStore::addReviewMs(uint32_t ms) {
    const uint64_t t = static_cast<uint64_t>(reviewMs_) + ms;
    reviewMs_ = t > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(t);
}

void SrsStore::grade(uint32_t codepoint, Grade g, uint16_t day, uint8_t mode, float uniform01) {
    Record* r = find(codepoint);
    if (!r) {
        if (records_.size() >= kMaxRecords) return;  // capped: ignore the card
        r = &getOrCreate(codepoint);
    }

    const bool first = r->reps == 0;
    Memory mem;
    if (first) {
        mem = initMemory(g);
    } else {
        mem.s = static_cast<float>(r->stabilityCds) / 100.0f;
        mem.d = static_cast<float>(r->difficulty10) / 10.0f;
        if (mem.s < S_MIN) mem.s = S_MIN;
        if (mem.d < 1.0f) mem.d = 1.0f;
        if (mem.d > 10.0f) mem.d = 10.0f;
        const float elapsed = day > r->lastDay ? static_cast<float>(day - r->lastDay) : 0.0f;
        mem = nextMemory(mem, g, elapsed);
    }

    const uint32_t cds = static_cast<uint32_t>(std::lround(mem.s * 100.0f));
    r->stabilityCds = clampU16(cds, 1, 65535);
    const uint32_t d10 = static_cast<uint32_t>(std::lround(mem.d * 10.0f));
    r->difficulty10 = static_cast<uint8_t>(clampU16(d10, 10, 100));

    const uint32_t due = static_cast<uint32_t>(day) + static_cast<uint32_t>(intervalDays(mem.s, uniform01));
    r->dueDay = due > 0xFFFFu ? 0xFFFF : static_cast<uint16_t>(due);
    if (first)
        r->flags = static_cast<uint8_t>(r->flags | kRecordFlagIntroducedOnLastDay);
    else if (r->lastDay != day)
        r->flags = static_cast<uint8_t>(r->flags & ~kRecordFlagIntroducedOnLastDay);
    r->lastDay = day;

    if (r->reps < 255) ++r->reps;
    if (g == Grade::Again && r->lapses < 255) ++r->lapses;
    if (mode < 8) r->modeSeen = static_cast<uint8_t>(r->modeSeen | (1u << mode));
    // Record encoding (section 2): 0 Again, 1 Hard, 2 Good.
    r->lastGrade = static_cast<uint8_t>(static_cast<uint8_t>(g) - 1u);

    noteStudyDay(day);
    // Both saturate; Again only while the answers can still count, so it
    // never exceeds them.
    if (answeredTotal_ < 0xFFFFFFFFu) {
        ++answeredTotal_;
        if (g == Grade::Again) ++againTotal_;
    }
    if (first) noteNewIntroduced(day);
}

bool SrsStore::serialize(std::vector<uint8_t>& out) const {
    if (records_.size() > kMaxRecords) return false;
    const size_t count = records_.size();
    out.assign(kSrsHeaderSize + count * kSrsRecordSize + kSrsCrcSize, 0);

    std::memcpy(out.data() + kOffMagic, kMagic, sizeof kMagic);
    wr16(out.data() + kOffVersion, kVersion);
    wr16(out.data() + kOffHeaderSize, static_cast<uint16_t>(kSrsHeaderSize));
    wr16(out.data() + kOffNowDay, nowDay_);
    wr16(out.data() + kOffCount, static_cast<uint16_t>(count));
    wr32(out.data() + kOffMsCarry, msCarry_);
    wr32(out.data() + kOffAnsweredTotal, answeredTotal_);
    wr16(out.data() + kOffSlowMs, slowMs_);
    wr16(out.data() + kOffNewPerDay, newPerDay_);
    wr16(out.data() + kOffNewIntroducedDay, newIntroducedDay_);
    wr16(out.data() + kOffNewIntroducedCount, newIntroducedCount_);
    wr16(out.data() + kOffMaxInterval, maxIntervalDays_);
    wr16(out.data() + kOffFlags, flags_);
    // v1-compatible extension (32..47): zero means absent.
    wr16(out.data() + kOffStreakCurrent, streakCurrent_);
    wr16(out.data() + kOffStreakBest, streakBest_);
    wr16(out.data() + kOffLastStudyDay, lastStudyDay_);
    wr16(out.data() + kOffEpochAnchor, epochAnchor_);
    wr32(out.data() + kOffReviewMs, reviewMs_);
    wr32(out.data() + kOffAgainTotal, againTotal_);

    uint8_t* p = out.data() + kSrsHeaderSize;
    for (const Record& r : records_) {
        writeRecord(p, r);
        p += kSrsRecordSize;
    }
    wr32(out.data() + out.size() - kSrsCrcSize, crc32(out.data(), out.size() - kSrsCrcSize));
    return true;
}

bool SrsStore::deserialize(const uint8_t* data, size_t len) {
    if (!data || len < kSrsHeaderSize + kSrsCrcSize) return false;
    if (std::memcmp(data + kOffMagic, kMagic, sizeof kMagic) != 0) return false;
    if (rd16(data + kOffVersion) != kVersion) return false;
    if (rd16(data + kOffHeaderSize) != kSrsHeaderSize) return false;

    const uint16_t count = rd16(data + kOffCount);
    if (count > kMaxRecords) return false;
    const size_t expect = kSrsHeaderSize + static_cast<size_t>(count) * kSrsRecordSize + kSrsCrcSize;
    if (len != expect) return false;
    if (rd32(data + len - kSrsCrcSize) != crc32(data, len - kSrsCrcSize)) return false;

    std::vector<Record> records;
    records.reserve(count);
    uint32_t prev = 0;
    const uint8_t* p = data + kSrsHeaderSize;
    for (uint16_t i = 0; i < count; ++i, p += kSrsRecordSize) {
        Record r = readRecord(p);
        if (i > 0 && r.codepoint <= prev) return false;  // must be strictly sorted
        prev = r.codepoint;
        records.push_back(r);
    }

    records_ = std::move(records);
    nowDay_ = rd16(data + kOffNowDay);
    msCarry_ = rd32(data + kOffMsCarry);
    answeredTotal_ = rd32(data + kOffAnsweredTotal);
    slowMs_ = rd16(data + kOffSlowMs);
    newPerDay_ = rd16(data + kOffNewPerDay);
    newIntroducedDay_ = rd16(data + kOffNewIntroducedDay);
    newIntroducedCount_ = rd16(data + kOffNewIntroducedCount);
    maxIntervalDays_ = rd16(data + kOffMaxInterval);
    flags_ = rd16(data + kOffFlags);
    // An old file left 32..47 zero: they load as the defaults (0).
    streakCurrent_ = rd16(data + kOffStreakCurrent);
    streakBest_ = rd16(data + kOffStreakBest);
    lastStudyDay_ = rd16(data + kOffLastStudyDay);
    epochAnchor_ = rd16(data + kOffEpochAnchor);  // 0 (old file): never synced
    reviewMs_ = rd32(data + kOffReviewMs);
    againTotal_ = rd32(data + kOffAgainTotal);
    // A file written before the Again counter left bytes 44..47 zero: the
    // records' lapses are the same count (short of a saturated record), so
    // start from them. A real zero gives zero lapses too, and nothing has been
    // forgotten before this counter existed.
    if (againTotal_ == 0) {
        uint64_t lapses = 0;
        for (const Record& r : records_) lapses += r.lapses;
        againTotal_ = static_cast<uint32_t>(lapses < answeredTotal_ ? lapses : answeredTotal_);
    }
    if (againTotal_ > answeredTotal_) againTotal_ = answeredTotal_;
    // A file written before the "has studied" flag existed still records a
    // non-zero last study day whenever the streak logic ran, so recover the flag
    // from it; otherwise a continuing streak would be reset to 1.
    if (lastStudyDay_ != 0) flags_ |= kSrsFlagHasStudied;
    tickStarted_ = false;
    lastTickMs_ = 0;
    return true;
}

bool SrsStore::save(StateIo& io) const {
    std::vector<uint8_t> buf;
    if (!serialize(buf)) return false;
    return io.save(buf);
}

}  // namespace tgc
