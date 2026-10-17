// Persisted review state, the session day clock and the v1 file format.
// Design: docs/srs.md sections 2, 4 and 5.
#include "srs/SrsStore.h"

#include <cmath>
#include <cstring>

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
// 32..47 reserved, must be zero in v1.

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
    p[15] = r.reserved;
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
    r.reserved = p[15];
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
    slowMs_ = kSrsDefaultSlowMs;
    newPerDay_ = kSrsDefaultNewPerDay;
    newIntroducedDay_ = 0;
    newIntroducedCount_ = 0;
    maxIntervalDays_ = kSrsDefaultMaxIntervalDays;
    flags_ = 0;
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
    if (answeredTotal_ > 0 && nowDay_ < 0xFFFF) ++nowDay_;
    resetNewIfNewDay();
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

    uint64_t carry = static_cast<uint64_t>(msCarry_) + delta;
    while (carry >= kSrsMsPerDay) {
        carry -= kSrsMsPerDay;
        if (nowDay_ < 0xFFFF) ++nowDay_;
    }
    msCarry_ = static_cast<uint32_t>(carry);
    resetNewIfNewDay();
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
    r->lastDay = day;

    if (r->reps < 255) ++r->reps;
    if (g == Grade::Again && r->lapses < 255) ++r->lapses;
    if (mode < 8) r->modeSeen = static_cast<uint8_t>(r->modeSeen | (1u << mode));
    // Record encoding (section 2): 0 Again, 1 Hard, 2 Good.
    r->lastGrade = static_cast<uint8_t>(static_cast<uint8_t>(g) - 1u);

    if (answeredTotal_ < 0xFFFFFFFFu) ++answeredTotal_;
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
    // reserved[4] stays zero.

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
