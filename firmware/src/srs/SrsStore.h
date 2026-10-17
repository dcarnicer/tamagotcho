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

namespace tgc {

// v1 file layout (docs/srs.md section 4): header, records, CRC-32.
constexpr size_t kSrsHeaderSize = 48;
constexpr size_t kSrsRecordSize = 16;
constexpr size_t kSrsCrcSize = 4;
constexpr size_t kSrsMaxRecords = 8192;

// Session clock: one day index per 24 h of uptime (section 5).
constexpr uint32_t kSrsMsPerDay = 86400000u;

// Header defaults (docs/srs.md section 3).
constexpr uint16_t kSrsDefaultSlowMs = 3000;
constexpr uint16_t kSrsDefaultNewPerDay = 10;
constexpr uint16_t kSrsDefaultMaxIntervalDays = 365;

// Header `flags` bits (bit0 reserved for a future clean-shutdown marker).
constexpr uint16_t kSrsFlagNone = 0;

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

    // Uptime rule: accumulates elapsed milliseconds into the day carry.
    // `nowMs` is the app's millisecond clock (millis()); wraps are handled.
    void tick(uint32_t nowMs);

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
    // advanced by grade(); `lastStudyDay == 0` means no day has been studied.
    uint16_t streakCurrent() const { return streakCurrent_; }
    uint16_t streakBest() const { return streakBest_; }
    uint16_t lastStudyDay() const { return lastStudyDay_; }
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
    uint16_t lastStudyDay_ = 0;   // SRS day of the last answer (0 = never)
    uint32_t reviewMs_ = 0;       // lifetime time on questions, ms

    bool tickStarted_ = false;
    uint32_t lastTickMs_ = 0;
};

}  // namespace tgc
