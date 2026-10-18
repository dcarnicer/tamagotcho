#pragma once
// Persisted per-kanji review state, the session day clock and the v1 file
// format. Pure C++, no Arduino. Design: docs/srs.md sections 2, 4 and 5.
//
// The store owns one 16-byte Record per distinct kanji codepoint, kept sorted
// so lookups are a binary search and serialisation is a straight dump. The app
// calls begin(io) once at boot, beginSession() once per power-up and tick(now)
// from loop(); the module never reads millis() itself.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "srs/Srs.h"
#include "srs/TimeZone.h"

namespace tgc {

// v1 file layout (docs/srs.md section 4): header, records, CRC-32.
constexpr size_t kSrsHeaderSize = 48;
constexpr size_t kSrsRecordSize = 16;
constexpr size_t kSrsCrcSize = 4;
constexpr size_t kSrsMaxRecords = 8192;

// Session clock: one day index per 24 h of uptime (section 5); after a
// wall-clock sync the day ends at 04:00 local (section 5.1), 23 or 25 h later
// on a daylight-saving change when the time zone has a rule.
constexpr uint32_t kSrsMsPerDay = 86400000u;

// Header defaults (docs/srs.md section 3).
constexpr uint16_t kSrsDefaultSlowMs = 3000;
constexpr uint16_t kSrsDefaultNewPerDay = 10;
constexpr uint16_t kSrsDefaultMaxIntervalDays = 365;

// Header `flags` bits (bit0 reserved for a future clean-shutdown marker).
// bit1 records that at least one answer has ever been given: it is what tells
// "never studied" apart from "studied on day index 0", which lastStudyDay_ alone
// cannot (see noteStudyDay and docs/srs.md section 4.1).
constexpr uint16_t kSrsFlagNone = 0;
constexpr uint16_t kSrsFlagHasStudied = 0x0002;

// A card counts as "learned" once its stability reaches a ~3-week interval
// (docs/roadmap.md "Statistics"). Stability is in centidays, so 21 days.
constexpr uint16_t kLearnedStabilityDays = 21;

// Per-card state, 16 bytes in memory and on disk (docs/srs.md section 2).
// `dueDay == 0` means "new, never reviewed"; `lastGrade == 0xFF` means never.
struct Record {
    uint32_t codepoint;
    uint16_t stabilityCds;  // S in centidays, 1..65535
    uint16_t dueDay;        // 0 = new
    uint16_t lastDay;       // 0 = never
    uint8_t difficulty10;   // D * 10, 10..100
    uint8_t reps;           // saturating
    uint8_t lapses;         // saturating
    uint8_t modeSeen;       // bit0 meaning, bit1 reading, bit2 meaning->kanji
    uint8_t lastGrade;      // 0 Again, 1 Hard, 2 Good, 0xFF never
    uint8_t reserved;       // 0
};
static_assert(sizeof(Record) == kSrsRecordSize, "Record must stay 16 bytes");

// Aggregate counters over the whole state table, for the statistics screen
// (docs/roadmap.md "Statistics"). Pure and deterministic: no clock, no I/O.
struct Stats {
    uint32_t seen;           // records in the table (distinct kanji reviewed)
    uint32_t learned;        // records whose stability >= kLearnedStabilityDays
    uint32_t answers;        // lifetime answers (header counter)
    uint32_t reps;           // sum of every card's review count
    uint32_t lapses;         // sum of every card's Again count
    uint32_t streakCurrent;  // current day streak (header field)
    uint32_t streakBest;     // best day streak (header field)
    uint32_t reviewMs;       // total time spent on questions, ms (header field)
};

// Outcome of SrsStore::syncWallClock() (docs/srs.md section 5.1).
enum class ClockSync : uint8_t {
    Rejected,    // implausible time or offset: nothing changed
    Anchored,    // first sync: the current day index became "today"
    Same,        // already on the real day: only the rollover was aligned
    Advanced,    // the real day was ahead: the index moved forward
    Reanchored,  // the index was ahead of the real day: display re-based, index kept
};

struct ClockSyncResult {
    ClockSync kind = ClockSync::Rejected;
    uint16_t daysAdvanced = 0;  // non-zero only for Advanced
};

// Byte sink/source for the state file. Device code implements this over
// LittleFS (temp + rename); MemoryIo below is the test double.
class StateIo {
public:
    virtual ~StateIo() = default;
    // Reads the whole file into `out`. False if there is no readable file.
    virtual bool load(std::vector<uint8_t>& out) = 0;
    // Replaces the whole file. False on any write failure.
    virtual bool save(const std::vector<uint8_t>& data) = 0;
};

class MemoryIo : public StateIo {
public:
    std::vector<uint8_t> bytes;
    bool failLoad = false;
    bool failSave = false;

    bool load(std::vector<uint8_t>& out) override {
        if (failLoad || bytes.empty()) return false;
        out = bytes;
        return true;
    }
    bool save(const std::vector<uint8_t>& data) override {
        if (failSave) return false;
        bytes = data;
        return true;
    }
};

class SrsStore {
public:
    static constexpr size_t kMaxRecords = kSrsMaxRecords;

    // Loads from `io`. On failure (no file, bad magic/version/size/CRC) the
    // store is reset to empty and false is returned; callers keep going.
    // Call beginSession() after a successful or failed begin().
    bool begin(StateIo& io);

    // Boot rule: resets the per-session tick. The day index is advanced by the
    // caller instead of automatically: the device has no clock, so the app asks
    // the user how much time passed (or the uptime rule advances it while on).
    void beginSession();

    // Adds `n` days to the index (the user-reported gap, or a manual fix).
    void advanceDays(uint16_t n);

    // A gap the user entered on the date screen: advanceDays(n) and, for n > 0,
    // a restarted day carry. The time of day is unknown after a power cut, so the
    // next rollover comes a full day after the answer -- late, never early
    // (docs/srs.md section 5, power-cut rule).
    void advanceDaysManual(uint16_t n);

    // Applies a real wall-clock time (docs/srs.md section 5.1); `tzOffsetMin` is
    // local time minus UTC in minutes. The first sync anchors the displayed date
    // to the current index; later ones advance the index when the real study day
    // is ahead and re-base the display (never the index) when it is behind.
    // Every accepted sync aligns the day carry so the uptime and sleep rules roll
    // over at kSrsRolloverHour local. A Rejected result changes nothing.
    // The rule form takes the offset in effect at `utcMs` from `zone`; the
    // offset form is a fixed rule. Neither changes timeZone().
    ClockSyncResult syncWallClock(int64_t utcMs, int tzOffsetMin);
    ClockSyncResult syncWallClock(int64_t utcMs, const TzRule& zone);

    // The time zone the uptime and sleep rules count study days in (docs/srs.md
    // section 5.1): with a daylight-saving rule and a wall-clock anchor, the
    // day that contains a change lasts 23 or 25 h, so the rollover stays at
    // 04:00 local. Not persisted (the app keeps the rule in NVS and sets it at
    // boot, before addElapsedMs()); UTC by default, which gives 24 h days.
    void setTimeZone(const TzRule& zone);
    const TzRule& timeZone() const { return tz_; }
    // Real length of the current index day: 24 h unless the zone has a
    // daylight-saving rule and the store has an anchor (section 5.1).
    uint32_t dayLengthMs() const;

    // Days since 1970-01-01 of SRS day 0 (header byte 38); 0 = never synced.
    uint16_t epochAnchor() const { return epochAnchor_; }
    bool hasWallClock() const { return epochAnchor_ != 0; }
    // Calendar day (days since 1970-01-01) shown for SRS day `day`: anchor + day,
    // or the fixed 2026-10-17 base + day while never synced.
    int32_t epochDayOf(uint16_t day) const;

    // Uptime rule: accumulates elapsed milliseconds into the day carry.
    // `nowMs` is the app's millisecond clock (millis()); wraps are handled.
    void tick(uint32_t nowMs);

    // Adds time that passed while the app was not ticking (deep sleep, measured
    // by the RTC timer) to the same day carry as tick(). Does not touch the tick
    // base, so it can be called before or after the first tick().
    void addElapsedMs(uint64_t ms);

    uint16_t today() const { return nowDay_; }
    // Test-only override of the day index; also re-bases the tick clock.
    void setToday(uint16_t day);

    // Null if the codepoint has no record.
    Record* find(uint32_t codepoint);
    const Record* find(uint32_t codepoint) const;

    // Existing record or a freshly inserted one, keeping the vector sorted.
    // Past kMaxRecords no record is added: a scratch record is returned that
    // is not part of the table (callers should check count()/find() first).
    Record& getOrCreate(uint32_t codepoint);

    // Applies one answer at `day`. First review (reps == 0) uses initMemory,
    // otherwise nextMemory with elapsed = day - lastDay (clamped at 0).
    // `mode` is a QuizMode value (0..2) and sets its modeSeen bit.
    // `uniform01` is the caller-drawn fuzz value for intervalDays().
    void grade(uint32_t codepoint, Grade g, uint16_t day, uint8_t mode, float uniform01);

    size_t count() const { return records_.size(); }
    const std::vector<Record>& records() const { return records_; }

    // Walks the table once and sums the counters above. O(n), no allocation.
    Stats stats() const;

    // --- format --------------------------------------------------------
    // Header + records + CRC, v1. False only on an oversized table.
    bool serialize(std::vector<uint8_t>& out) const;
    // Validates magic, version, header size, count/size consistency, CRC and
    // ascending codepoints. On failure the store is left unchanged.
    bool deserialize(const uint8_t* data, size_t len);
    bool save(StateIo& io) const;

    // --- header knobs --------------------------------------------------
    uint16_t slowMs() const { return slowMs_; }
    void setSlowMs(uint16_t v) { slowMs_ = v; }
    uint16_t newPerDay() const { return newPerDay_; }
    void setNewPerDay(uint16_t v) { newPerDay_ = v; }
    uint32_t answeredTotal() const { return answeredTotal_; }
    void setAnsweredTotal(uint32_t v) { answeredTotal_ = v; }
    uint16_t newIntroducedDay() const { return newIntroducedDay_; }
    void setNewIntroducedDay(uint16_t v) { newIntroducedDay_ = v; }
    uint16_t newIntroducedCount() const { return newIntroducedCount_; }
    void setNewIntroducedCount(uint16_t v) { newIntroducedCount_ = v; }
    uint16_t maxIntervalDays() const { return maxIntervalDays_; }
    void setMaxIntervalDays(uint16_t v) { maxIntervalDays_ = v; }
    uint16_t flags() const { return flags_; }
    void setFlags(uint16_t v) { flags_ = v; }
    uint32_t msCarry() const { return msCarry_; }

    // Day streak and total review time (docs/srs.md section 4). The streak is
    // advanced by grade(); `lastStudyDay()` is the SRS day of the last answer
    // and is only meaningful while `hasStudied()` is true.
    uint16_t streakCurrent() const { return streakCurrent_; }
    uint16_t streakBest() const { return streakBest_; }
    uint16_t lastStudyDay() const { return lastStudyDay_; }
    // True once any answer has ever been given (persisted flags bit). Tells
    // "never studied" apart from a study day with index 0.
    bool hasStudied() const { return (flags_ & kSrsFlagHasStudied) != 0; }
    uint32_t reviewMs() const { return reviewMs_; }
    // Adds `ms` to the total time on questions, saturating at 0xFFFFFFFF.
    void addReviewMs(uint32_t ms);

private:
    void reset();
    void resetNewIfNewDay();
    void noteNewIntroduced(uint16_t day);
    void noteStudyDay(uint16_t day);

    std::vector<Record> records_;  // sorted by codepoint ascending
    Record scratch_{};             // returned by getOrCreate() past the cap

    uint16_t nowDay_ = 0;
    uint32_t msCarry_ = 0;  // milliseconds toward the next day index
    uint32_t answeredTotal_ = 0;
    uint16_t slowMs_ = kSrsDefaultSlowMs;
    uint16_t newPerDay_ = kSrsDefaultNewPerDay;
    uint16_t newIntroducedDay_ = 0;
    uint16_t newIntroducedCount_ = 0;
    uint16_t maxIntervalDays_ = kSrsDefaultMaxIntervalDays;
    uint16_t flags_ = 0;
    uint16_t streakCurrent_ = 0;  // consecutive SRS days with an answer
    uint16_t streakBest_ = 0;     // best streak ever
    uint16_t lastStudyDay_ = 0;   // SRS day of the last answer (valid while studied)
    uint32_t reviewMs_ = 0;       // lifetime time on questions, ms
    uint16_t epochAnchor_ = 0;    // days since 1970-01-01 of SRS day 0; 0 = never synced

    bool tickStarted_ = false;
    uint32_t lastTickMs_ = 0;

    TzRule tz_;                          // not part of the file, kept by reset()
    mutable int32_t lenCacheDay_ = -1;   // epoch day dayLengthMs() last computed
    mutable uint32_t lenCacheMs_ = kSrsMsPerDay;
};

}  // namespace tgc
