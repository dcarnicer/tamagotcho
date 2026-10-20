// Native tests for the pure FSRS-5 math in srs/Srs.{h,cpp}. See docs/srs.md
// section 8. Host and ESP32 floats may differ in the last bits, so every
// comparison uses an absolute tolerance.
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "pack/MemorySource.h"
#include "pack/PackReader.h"
#include "quiz/Quiz.h"
#include "srs/DeckProgress.h"
#include "srs/Queue.h"
#include "srs/Srs.h"
#include "srs/SrsStore.h"
#include "srs/WallClock.h"

using namespace tgc;

namespace {

// A representative mid-life memory used by the transition tests.
Memory mid() {
    Memory m;
    m.s = 10.0f;
    m.d = 5.0f;
    return m;
}

// ------------------------------------------------- in-memory pack builder
// Minimal copy of the builder in test_native/test_main.cpp so the SRS and the
// queue tests stay self-contained. Pack card `level` is the card's index.
struct B {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) { u8(x & 255); u8(x >> 8); }
    void u32(uint32_t x) { u16(x & 0xFFFF); u16(x >> 16); }
    void pad4() { while (v.size() % 4) u8(0); }
};

struct CardSpec {
    uint32_t cp;
    const char *mean, *on, *kun;
    std::vector<uint16_t> dis;      // meaning distractors
    std::vector<uint16_t> disRead;  // reading distractors (empty -> offset 0)
};
struct GlyphSpec {
    uint8_t set;
    uint32_t cp;
    uint8_t w, h;
};

std::vector<uint8_t> makePack(const std::vector<CardSpec>& cards, std::vector<GlyphSpec> glyphs) {
    std::sort(glyphs.begin(), glyphs.end(), [](const GlyphSpec& a, const GlyphSpec& b) {
        return a.set != b.set ? a.set < b.set : a.cp < b.cp;
    });
    std::string pool;
    auto addStr = [&](const char* s) {
        uint32_t off = static_cast<uint32_t>(pool.size());
        pool += s;
        pool.push_back('\0');
        return off;
    };
    uint32_t deckRel = addStr("Test Deck");
    struct CS { uint32_t m, o, k; };
    std::vector<CS> cs;
    for (auto& c : cards) cs.push_back({addStr(c.mean), addStr(c.on), addStr(c.kun)});
    while (pool.size() % 4) pool.push_back('\0');

    const uint32_t cardsOff = 64;
    const uint32_t glyphsOff = cardsOff + cards.size() * 32;
    const uint32_t distOff = glyphsOff + glyphs.size() * 16;
    std::vector<uint32_t> dOffs, drOffs;
    uint32_t dsize = 0;
    for (auto& c : cards) {
        dOffs.push_back(distOff + dsize);
        dsize += 2 + 2 * c.dis.size();
    }
    const uint32_t drBase = distOff + dsize;
    uint32_t drsize = 0;
    for (auto& c : cards) {
        if (c.disRead.empty()) {
            drOffs.push_back(0);
            continue;
        }
        drOffs.push_back(drBase + drsize);
        drsize += 2 + 2 * c.disRead.size();
    }
    const uint32_t strOff = (drBase + drsize + 3) & ~3u;
    const uint32_t gdOff = strOff + pool.size();
    const uint32_t deckOff = strOff + deckRel;
    for (auto& x : cs) { x.m += strOff; x.o += strOff; x.k += strOff; }

    B b;
    b.u8('T'); b.u8('N'); b.u8('G'); b.u8('C');
    b.u16(1); b.u16(64);
    b.u32(cards.size()); b.u32(glyphs.size());
    b.u32(cardsOff); b.u32(glyphsOff); b.u32(distOff); b.u32(strOff);
    b.u32(pool.size()); b.u32(gdOff); b.u32(deckOff);
    b.u8('e'); b.u8('n'); b.u8(0); b.u8(0);
    b.u8(32); b.u8(16); b.u8(12); b.u8(18); b.u8(14);
    b.u8(0); b.u8(0); b.u8(0);
    b.u32(0); b.u32(0);
    for (size_t i = 0; i < cards.size(); ++i) {
        b.u32(cards[i].cp); b.u32(cs[i].m); b.u32(cs[i].o); b.u32(cs[i].k); b.u32(dOffs[i]);
        b.u8(5); b.u8(1); b.u8(4); b.u8(0);
        b.u16(10 + i); b.u16(i); b.u32(drOffs[i]);
    }
    uint32_t dataOff = gdOff;
    for (auto& g : glyphs) {
        b.u32(g.cp); b.u8(g.set); b.u8(0); b.u8(g.w); b.u8(g.h);
        b.u8(static_cast<uint8_t>(-3)); b.u8(static_cast<uint8_t>(-g.h)); b.u8(g.w + 1); b.u8(0);
        b.u32(dataOff);
        dataOff += ((g.w + 1) / 2) * g.h;
    }
    for (auto& c : cards) {
        b.u16(c.dis.size());
        for (auto d : c.dis) b.u16(d);
    }
    for (auto& c : cards) {
        if (c.disRead.empty()) continue;
        b.u16(c.disRead.size());
        for (auto d : c.disRead) b.u16(d);
    }
    b.pad4();
    for (char ch : pool) b.u8(ch);
    for (auto& g : glyphs)
        for (int i = 0; i < ((g.w + 1) / 2) * g.h; ++i) b.u8((g.cp + i) & 0xFF);
    return b.v;
}

std::vector<GlyphSpec> glyphsFor(const std::vector<CardSpec>& cs) {
    std::vector<GlyphSpec> g;
    for (auto& c : cs) {
        g.push_back({0, c.cp, 4, 4});
        g.push_back({1, c.cp, 4, 4});
    }
    return g;
}

// A pack of `n` cards that all carry a meaning and a reading, so each offers
// all three quiz modes.
std::vector<CardSpec> modeCards(int n) {
    std::vector<CardSpec> cs;
    for (int i = 0; i < n; ++i)
        cs.push_back({0x4E00u + static_cast<uint32_t>(i), "meaning", "オン", "くん", {}});
    return cs;
}

}  // namespace

void test_init_memory() {
    Memory again = initMemory(Grade::Again);
    Memory hard = initMemory(Grade::Hard);
    Memory good = initMemory(Grade::Good);

    // S = w[G-1].
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, kWeights[0], again.s);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, kWeights[1], hard.s);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, kWeights[2], good.s);

    // D is clamped to [1, 10] for every grade and falls as the grade rises.
    const Memory all[3] = {again, hard, good};
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(all[i].d >= 1.0f);
        TEST_ASSERT_TRUE(all[i].d <= 10.0f);
    }
    TEST_ASSERT_TRUE(again.d > hard.d);
    TEST_ASSERT_TRUE(hard.d > good.d);

    // D0(Good) = w[4] - expf(w[5] * 2) + 1 ~= 5.28 (docs/srs.md section 3).
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.28f, good.d);
}

void test_retrievability() {
    // R(_, 0) == 1.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, retrievability(3.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, retrievability(0.4f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, retrievability(500.0f, 0.0f));

    // R(S, S) = 1/sqrt(1 + F) = 0.9 (the desired retention).
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.9f, retrievability(10.0f, 10.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.9f, retrievability(3.17f, 3.17f));

    // Strictly decreasing in elapsed days.
    float prev = retrievability(5.0f, 0.0f);
    for (int t = 1; t <= 400; ++t) {
        const float r = retrievability(5.0f, static_cast<float>(t));
        TEST_ASSERT_TRUE(r < prev);
        prev = r;
    }
}

void test_interval_at_90() {
    // I = roundf(S) when the fuzz is neutral (u01 = 0.5 -> u = 1.0).
    TEST_ASSERT_EQUAL_INT(3, intervalDays(3.17f, 0.5f));   // first Good
    TEST_ASSERT_EQUAL_INT(1, intervalDays(0.4f, 0.5f));    // round(0.4) -> clamp
    TEST_ASSERT_EQUAL_INT(1, intervalDays(0.0f, 0.5f));    // never below 1
    TEST_ASSERT_EQUAL_INT(10, intervalDays(10.4f, 0.5f));
    TEST_ASSERT_EQUAL_INT(11, intervalDays(10.6f, 0.5f));

    // Cap 365.
    TEST_ASSERT_EQUAL_INT(365, intervalDays(500.0f, 0.5f));
    TEST_ASSERT_EQUAL_INT(365, intervalDays(100000.0f, 0.5f));
    TEST_ASSERT_EQUAL_INT(kMaxIntervalDays, intervalDays(1e9f, 0.5f));

    // Fuzz stays in +/-5% plus rounding slack and is never below 1.
    const float svals[] = {0.4f, 1.0f, 2.0f, 3.17f, 10.0f, 60.0f, 365.0f};
    for (float s : svals) {
        int base = static_cast<int>(roundf(s));
        if (base < 1) base = 1;
        if (base > kMaxIntervalDays) base = kMaxIntervalDays;
        const int lo = base >= 2 ? static_cast<int>(floorf(base * kFuzzMin)) - 1 : 1;
        const int hi = base >= 2 ? static_cast<int>(ceilf(base * kFuzzMax)) + 1 : 1;
        for (int k = 0; k <= 20; ++k) {
            const int i = intervalDays(s, k / 20.0f);
            TEST_ASSERT_TRUE(i >= 1);
            TEST_ASSERT_TRUE(i >= lo);
            TEST_ASSERT_TRUE(i <= hi);
        }
    }

    // Determinism: the same drawn value gives the same interval.
    for (int k = 0; k <= 10; ++k) {
        const float u = k / 10.0f;
        TEST_ASSERT_EQUAL_INT(intervalDays(40.0f, u), intervalDays(40.0f, u));
    }
    // And the fuzz actually varies across the unit interval.
    bool varied = false;
    for (int k = 0; k <= 10; ++k)
        if (intervalDays(40.0f, k / 10.0f) != intervalDays(40.0f, 0.5f)) varied = true;
    TEST_ASSERT_TRUE(varied);
}

void test_success_growth() {
    const Memory m = mid();
    const Memory hard = nextMemory(m, Grade::Hard, 10.0f);
    const Memory good = nextMemory(m, Grade::Good, 10.0f);

    // Both grow S; Good grows it more than Hard (the w[15] penalty).
    TEST_ASSERT_TRUE(hard.s > m.s);
    TEST_ASSERT_TRUE(good.s > m.s);
    TEST_ASSERT_TRUE(good.s > hard.s);

    // D moves down for Good and up for Hard.
    TEST_ASSERT_TRUE(good.d < m.d);
    TEST_ASSERT_TRUE(hard.d > m.d);
    TEST_ASSERT_TRUE(good.d >= 1.0f && good.d <= 10.0f);
    TEST_ASSERT_TRUE(hard.d >= 1.0f && hard.d <= 10.0f);
}

void test_lapse() {
    const Memory m = mid();
    const Memory after = nextMemory(m, Grade::Again, 10.0f);

    // A miss never increases stability, and S stays at least S_MIN.
    TEST_ASSERT_TRUE(after.s <= m.s);
    TEST_ASSERT_TRUE(after.s >= S_MIN);
    // A miss makes the card harder.
    TEST_ASSERT_TRUE(after.d > m.d);
    TEST_ASSERT_TRUE(after.d <= 10.0f);

    // Same guarantee from a very small stability.
    Memory tiny;
    tiny.s = 0.05f;
    tiny.d = 8.0f;
    const Memory tinyAfter = nextMemory(tiny, Grade::Again, 20.0f);
    TEST_ASSERT_TRUE(tinyAfter.s <= tiny.s);
    TEST_ASSERT_TRUE(tinyAfter.s >= S_MIN);
}

void test_same_day() {
    const Memory m = mid();
    const float again = m.s * expf(kWeights[17] * (1.0f - 3.0f + kWeights[18]));
    const float hard = m.s * expf(kWeights[17] * (2.0f - 3.0f + kWeights[18]));
    const float good = m.s * expf(kWeights[17] * (3.0f - 3.0f + kWeights[18]));

    // t == 0 uses S' = S * expf(w[17] * (G - 3 + w[18])).
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, again, nextMemory(m, Grade::Again, 0.0f).s);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, hard, nextMemory(m, Grade::Hard, 0.0f).s);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, good, nextMemory(m, Grade::Good, 0.0f).s);

    // Good raises S, Again lowers it; both stay in range.
    TEST_ASSERT_TRUE(good > m.s);
    TEST_ASSERT_TRUE(again < m.s);
    TEST_ASSERT_TRUE(again >= S_MIN);
    TEST_ASSERT_TRUE(good <= S_MAX);
}

void test_monotonic_difficulty() {
    // A fixed, deterministic grade/elapsed pattern over many reviews. D and S
    // must stay inside their clamps no matter how the sequence is driven.
    Memory m = initMemory(Grade::Again);
    for (int i = 0; i < 3000; ++i) {
        const int gi = 1 + ((i * 7 + i / 3 + i / 11) % 3);
        const Grade g = static_cast<Grade>(gi);
        const float t = static_cast<float>((i * 13) % 31);  // 0..30, includes t==0
        m = nextMemory(m, g, t);
        TEST_ASSERT_TRUE(m.d >= 1.0f);
        TEST_ASSERT_TRUE(m.d <= 10.0f);
        TEST_ASSERT_TRUE(m.s >= S_MIN);
        TEST_ASSERT_TRUE(m.s <= S_MAX);
    }
}

// ---------------------------------------------------------------- phase 2
// SrsStore + Queue. See docs/srs.md section 8.

namespace {

// Puts a card into the store as a reviewed card due on `due` with the given
// stability (centidays), bypassing the scheduler so ordering is exact.
void setDue(SrsStore& srs, uint32_t cp, uint16_t due, uint16_t stabCds) {
    Record& r = srs.getOrCreate(cp);
    r.dueDay = due;
    r.stabilityCds = stabCds;
    r.reps = 1;
    r.lastDay = 1;
    r.difficulty10 = 50;
}

}  // namespace

void test_serialize_roundtrip() {
    SrsStore a;
    MemoryIo empty;
    TEST_ASSERT_FALSE(a.begin(empty));  // no file -> empty state, no error
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(a.count()));

    a.setToday(42);
    a.setSlowMs(2500);
    a.setNewPerDay(7);
    a.grade(0x65E5, Grade::Good, 42, 0, 0.5f);
    a.grade(0x4E2D, Grade::Again, 42, 1, 0.25f);
    a.grade(0x65E5, Grade::Hard, 42, 2, 0.75f);  // same-day second review
    // Both new cards graded above were introduced today; pin the header knob
    // afterwards so the round trip checks a known value.
    TEST_ASSERT_EQUAL_UINT16(42, a.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(2, a.newIntroducedCount());
    a.setNewIntroducedCount(3);

    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    TEST_ASSERT_EQUAL_UINT32(48 + 2 * 16 + 4, static_cast<uint32_t>(io.bytes.size()));
    TEST_ASSERT_EQUAL_UINT8('T', io.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8('N', io.bytes[1]);
    TEST_ASSERT_EQUAL_UINT8('G', io.bytes[2]);
    TEST_ASSERT_EQUAL_UINT8('S', io.bytes[3]);
    TEST_ASSERT_EQUAL_UINT8(1, io.bytes[4]);    // version, LE
    TEST_ASSERT_EQUAL_UINT8(0, io.bytes[5]);
    TEST_ASSERT_EQUAL_UINT8(48, io.bytes[6]);   // header_size, LE

    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(42, b.today());
    TEST_ASSERT_EQUAL_UINT16(2500, b.slowMs());
    TEST_ASSERT_EQUAL_UINT16(7, b.newPerDay());
    TEST_ASSERT_EQUAL_UINT16(42, b.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(3, b.newIntroducedCount());
    TEST_ASSERT_EQUAL_UINT32(3, b.answeredTotal());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(b.count()));

    // Sorted ascending and every field survives the round trip.
    TEST_ASSERT_TRUE(b.records()[0].codepoint < b.records()[1].codepoint);
    for (size_t i = 0; i < a.count(); ++i) {
        const Record& x = a.records()[i];
        const Record* y = b.find(x.codepoint);
        TEST_ASSERT_NOT_NULL(y);
        TEST_ASSERT_EQUAL_UINT32(x.codepoint, y->codepoint);
        TEST_ASSERT_EQUAL_UINT16(x.stabilityCds, y->stabilityCds);
        TEST_ASSERT_EQUAL_UINT16(x.dueDay, y->dueDay);
        TEST_ASSERT_EQUAL_UINT16(x.lastDay, y->lastDay);
        TEST_ASSERT_EQUAL_UINT8(x.difficulty10, y->difficulty10);
        TEST_ASSERT_EQUAL_UINT8(x.reps, y->reps);
        TEST_ASSERT_EQUAL_UINT8(x.lapses, y->lapses);
        TEST_ASSERT_EQUAL_UINT8(x.modeSeen, y->modeSeen);
        TEST_ASSERT_EQUAL_UINT8(x.lastGrade, y->lastGrade);
        TEST_ASSERT_EQUAL_UINT8(x.flags, y->flags);
    }
    // modeSeen accumulated bits 0 and 2 on 日.
    TEST_ASSERT_EQUAL_UINT8(0x5, b.find(0x65E5)->modeSeen);
    TEST_ASSERT_EQUAL_UINT8(0x2, b.find(0x4E2D)->modeSeen);
}

void test_serialize_rejects() {
    SrsStore a;
    MemoryIo empty;
    a.begin(empty);
    a.setToday(3);
    a.grade(0x65E5, Grade::Good, 3, 0, 0.5f);
    a.grade(0x6C34, Grade::Good, 3, 1, 0.5f);
    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    const std::vector<uint8_t> good = io.bytes;

    // A store that must stay untouched by every failed load.
    SrsStore b;
    b.setToday(9);
    b.getOrCreate(0xABCD);

    auto reject = [&](const std::vector<uint8_t>& bytes) {
        TEST_ASSERT_FALSE(b.deserialize(bytes.data(), bytes.size()));
        TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(b.count()));
        TEST_ASSERT_EQUAL_UINT16(9, b.today());
        TEST_ASSERT_NOT_NULL(b.find(0xABCD));
    };

    { std::vector<uint8_t> x = good; x[0] = 'X'; reject(x); }              // bad magic
    { std::vector<uint8_t> x = good; x[4] = 2; reject(x); }                // version 2
    { std::vector<uint8_t> x = good; x[6] = 40; reject(x); }               // bad header_size
    { std::vector<uint8_t> x(good.begin(), good.end() - 1); reject(x); }   // truncated
    { std::vector<uint8_t> x = good; x.push_back(0); reject(x); }          // extra byte
    { std::vector<uint8_t> x = good; x[10] = 3; reject(x); }               // count too big
    { std::vector<uint8_t> x = good; x.back() ^= 0xFF; reject(x); }        // bad CRC
    { std::vector<uint8_t> x = good; x[20] ^= 0x03; reject(x); }           // header flip -> CRC
    reject(std::vector<uint8_t>(10, 0));                                   // far too short

    // The untouched bytes still load.
    TEST_ASSERT_TRUE(b.deserialize(good.data(), good.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(b.count()));
    TEST_ASSERT_EQUAL_UINT16(3, b.today());
}

void test_codepoint_identity() {
    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(10);

    // Keyed by codepoint, not by deck or card index.
    Record& r = srs.getOrCreate(0x65E5);
    r.dueDay = 4;
    r.stabilityCds = 250;
    r.reps = 2;
    r.lastDay = 2;
    r.difficulty10 = 60;
    Record& again = srs.getOrCreate(0x65E5);
    TEST_ASSERT_EQUAL_PTR(&r, &again);
    TEST_ASSERT_EQUAL_UINT8(2, again.reps);

    // Pack A: 日 at index 0. Pack B: 月 then 日, so the index changed.
    std::vector<CardSpec> ca = {{0x65E5, "day", "ニチ", "ひ", {0}}, {0x6708, "month", "ゲツ", "つき", {}}};
    std::vector<CardSpec> cb = {{0x6708, "month", "ゲツ", "つき", {0}}, {0x65E5, "day", "ニチ", "ひ", {}}};
    auto pa = makePack(ca, glyphsFor(ca));
    auto pb = makePack(cb, glyphsFor(cb));
    MemorySource sa(pa), sb(pb);
    PackReader ra, rb;
    TEST_ASSERT_EQUAL(PackError::Ok, ra.open(sa));
    TEST_ASSERT_EQUAL(PackError::Ok, rb.open(sb));

    XorShiftRng rng(1);
    Queue qa, qb;
    qa.rebuild(ra, srs, rng);
    qb.rebuild(rb, srs, rng);
    QueueItem a0, b0;
    TEST_ASSERT_TRUE(qa.next(&a0));
    TEST_ASSERT_TRUE(qb.next(&b0));
    TEST_ASSERT_EQUAL_UINT32(0, a0.cardIndex);  // 日 is index 0 in pack A
    TEST_ASSERT_EQUAL_UINT32(1, b0.cardIndex);  // ...and index 1 in pack B
    // Both are the same due review, so both options carry the same codepoint.
    Card ca0, cb1;
    TEST_ASSERT_TRUE(ra.card(a0.cardIndex, &ca0));
    TEST_ASSERT_TRUE(rb.card(b0.cardIndex, &cb1));
    TEST_ASSERT_EQUAL_UINT32(ca0.codepoint, cb1.codepoint);
}

void test_store_cap() {
    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    for (uint32_t cp = 1; cp <= SrsStore::kMaxRecords; ++cp) srs.getOrCreate(cp).reps = 1;
    TEST_ASSERT_EQUAL_UINT32(8192, static_cast<uint32_t>(srs.count()));
    TEST_ASSERT_NOT_NULL(srs.find(8192));

    // Past the cap nothing is inserted.
    srs.getOrCreate(999999);
    TEST_ASSERT_EQUAL_UINT32(8192, static_cast<uint32_t>(srs.count()));
    TEST_ASSERT_NULL(srs.find(999999));
    // grade() refuses to create a record past the cap...
    srs.grade(999999, Grade::Good, 1, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT32(8192, static_cast<uint32_t>(srs.count()));
    TEST_ASSERT_EQUAL_UINT32(0, srs.answeredTotal());
    // ...but an existing record is still graded.
    srs.grade(4096, Grade::Good, 1, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT32(1, srs.answeredTotal());
}

// Sums the whole table: seen/learned/answers/reps/lapses. The threshold is a
// stability of 21 days (centidays >= 2100). See docs/roadmap.md "Statistics".
void test_stats() {
    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(0);

    // Empty store: every counter is zero.
    const Stats empty = srs.stats();
    TEST_ASSERT_EQUAL_UINT32(0, empty.seen);
    TEST_ASSERT_EQUAL_UINT32(0, empty.learned);
    TEST_ASSERT_EQUAL_UINT32(0, empty.answers);
    TEST_ASSERT_EQUAL_UINT32(0, empty.reps);
    TEST_ASSERT_EQUAL_UINT32(0, empty.lapses);
    TEST_ASSERT_EQUAL_UINT32(0, empty.streakCurrent);
    TEST_ASSERT_EQUAL_UINT32(0, empty.streakBest);
    TEST_ASSERT_EQUAL_UINT32(0, empty.reviewMs);

    // Three distinct cards, different grades: 日 Good twice, 月 a lapse, 火 Good.
    srs.grade(0x65E5, Grade::Good, 0, 0, 0.5f);
    srs.grade(0x6708, Grade::Again, 0, 1, 0.5f);
    srs.grade(0x706B, Grade::Good, 0, 2, 0.5f);
    srs.grade(0x65E5, Grade::Good, 0, 0, 0.5f);

    const Stats st = srs.stats();
    TEST_ASSERT_EQUAL_UINT32(3, st.seen);
    TEST_ASSERT_EQUAL_UINT32(4, st.answers);
    TEST_ASSERT_EQUAL_UINT32(4, st.reps);    // one per grade call
    TEST_ASSERT_EQUAL_UINT32(1, st.lapses);  // only the Again on 月
    TEST_ASSERT_EQUAL_UINT32(0, st.learned); // young cards stay below the threshold
    // Four answers, all on SRS day 0. A "has studied" flags bit (not
    // `last_study_day == 0`) decides first-vs-consecutive, so the very first
    // answer on day 0 starts the streak at 1 and the rest are same-day.
    TEST_ASSERT_EQUAL_UINT32(1, st.streakCurrent);
    TEST_ASSERT_EQUAL_UINT32(1, st.streakBest);
    TEST_ASSERT_EQUAL_UINT32(0, st.reviewMs);

    // The 21-day threshold is inclusive and measured in whole days.
    srs.getOrCreate(0x6C34).stabilityCds = kLearnedStabilityDays * 100u;  // 水
    TEST_ASSERT_EQUAL_UINT32(1, srs.stats().learned);
    srs.getOrCreate(0x6C34).stabilityCds = kLearnedStabilityDays * 100u - 1;
    TEST_ASSERT_EQUAL_UINT32(0, srs.stats().learned);

    // A card crosses the threshold on its own after enough Good answers.
    SrsStore learn;
    MemoryIo io2;
    learn.begin(io2);
    learn.grade(0x65E5, Grade::Good, 0, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT32(0, learn.stats().learned);
    int days = 0;
    while (learn.stats().learned == 0) {
        ++days;
        TEST_ASSERT_TRUE(days <= 30);  // the sequence below reaches 21 days by ~8
        learn.grade(0x65E5, Grade::Good, static_cast<uint16_t>(days), 0, 0.5f);
    }
    TEST_ASSERT_EQUAL_UINT32(1, learn.stats().learned);
    TEST_ASSERT_EQUAL_UINT32(1, learn.stats().seen);
    TEST_ASSERT_EQUAL_UINT32(days + 1, learn.stats().answers);
}

void test_clock_boot_and_uptime() {
    // beginSession() no longer moves the day by itself (the device has no clock,
    // so the app asks the user); advanceDays() does it explicitly.
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(7);
    s.beginSession();
    TEST_ASSERT_EQUAL_UINT16(7, s.today());
    s.advanceDays(3);
    TEST_ASSERT_EQUAL_UINT16(10, s.today());
    s.advanceDays(0);
    TEST_ASSERT_EQUAL_UINT16(10, s.today());

    // Uptime: 24 h of ticks advance one day, the carry keeps the remainder.
    SrsStore up;
    MemoryIo io3;
    up.begin(io3);
    up.setToday(10);
    up.tick(1000);
    up.tick(1000 + kSrsMsPerDay - 1);
    TEST_ASSERT_EQUAL_UINT16(10, up.today());
    up.tick(1000 + kSrsMsPerDay);
    TEST_ASSERT_EQUAL_UINT16(11, up.today());
    TEST_ASSERT_EQUAL_UINT32(0, up.msCarry());
    up.tick(1000 + kSrsMsPerDay + 5000);
    TEST_ASSERT_EQUAL_UINT16(11, up.today());
    TEST_ASSERT_EQUAL_UINT32(5000, up.msCarry());
    up.tick(1000 + 3 * kSrsMsPerDay);  // two more days at once
    TEST_ASSERT_EQUAL_UINT16(13, up.today());

    // The day change resets the new-card counter.
    up.setNewIntroducedDay(10);
    up.setNewIntroducedCount(4);
    up.setToday(13);
    TEST_ASSERT_EQUAL_UINT16(13, up.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(0, up.newIntroducedCount());
}

void test_elapsed_sleep_time() {
    // Deep-sleep time feeds the same carry as uptime ticks.
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(5);
    s.tick(0);
    s.tick(kSrsMsPerDay / 2);  // 12 h awake
    TEST_ASSERT_EQUAL_UINT16(5, s.today());
    s.addElapsedMs(kSrsMsPerDay / 2);  // 12 h asleep completes a day
    TEST_ASSERT_EQUAL_UINT16(6, s.today());
    TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());
    s.addElapsedMs(3ull * kSrsMsPerDay + 7000);  // a long sleep
    TEST_ASSERT_EQUAL_UINT16(9, s.today());
    TEST_ASSERT_EQUAL_UINT32(7000, s.msCarry());
    s.tick(kSrsMsPerDay / 2 + 1000);  // the tick base is untouched
    TEST_ASSERT_EQUAL_UINT32(8000, s.msCarry());

    // The day index saturates instead of wrapping.
    s.setToday(0xFFF0);
    s.addElapsedMs(1000ull * kSrsMsPerDay);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, s.today());

    // A new day resets the new-card counter.
    s.setToday(20);
    s.setNewIntroducedDay(20);
    s.setNewIntroducedCount(3);
    s.addElapsedMs(kSrsMsPerDay);
    TEST_ASSERT_EQUAL_UINT16(21, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
}

void test_queue_due_before_new() {
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "ひ", {}},
                                {0x6708, "month", "ゲツ", "つき", {}},
                                {0x706B, "fire", "カ", "ひ", {}},
                                {0x6C34, "water", "スイ", "みず", {}}};
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(10);
    setDue(srs, 0x65E5, 5, 300);  // index 0, due 5, stronger
    setDue(srs, 0x706B, 2, 500);  // index 2, most overdue
    setDue(srs, 0x6C34, 5, 100);  // index 3, due 5, weakest
    // index 1 (月) stays new.

    XorShiftRng rng(3);
    Queue q;
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(q.remaining()));

    QueueItem it;
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(2, it.cardIndex);  // due 2
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(3, it.cardIndex);  // due 5, weakest
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(0, it.cardIndex);  // due 5, stronger
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(1, it.cardIndex);  // reviews first, then new
    TEST_ASSERT_FALSE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.remaining()));
}

void test_queue_new_cap_resets_on_new_day() {
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "ひ", {}},
                                {0x6708, "month", "ゲツ", "つき", {}},
                                {0x706B, "fire", "カ", "ひ", {}},
                                {0x6C34, "water", "スイ", "みず", {}}};
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(5);
    srs.setNewPerDay(2);

    XorShiftRng rng(1);
    Queue q;
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(q.remaining()));

    // Grading a new card marks it introduced today and counts it against the cap.
    srs.grade(0x65E5, Grade::Good, 5, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);
    TEST_ASSERT_TRUE(introducedOn(*srs.find(0x65E5), 5));
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(q.remaining()));  // 2 - 1
    QueueItem it;
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(1, it.cardIndex);  // the next by level

    srs.grade(0x6708, Grade::Good, 5, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.remaining()));
    // "+10 new" (extraNew) opens the cap again.
    q.rebuild(pack, srs, rng, 10);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(q.remaining()));

    // A new day: yesterday's cards no longer count, so the cap is available again.
    srs.setToday(6);
    TEST_ASSERT_FALSE(introducedOn(*srs.find(0x65E5), 6));
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(q.remaining()));
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(2, it.cardIndex);

    // A review on a later day clears the mark; a same-day repeat keeps it.
    srs.grade(0x706B, Grade::Again, 6, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);
    srs.grade(0x706B, Grade::Good, 6, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);
    TEST_ASSERT_TRUE(introducedOn(*srs.find(0x706B), 6));
    srs.grade(0x65E5, Grade::Good, 6, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);
    TEST_ASSERT_EQUAL_UINT8(0, srs.find(0x65E5)->flags & kRecordFlagIntroducedOnLastDay);
}

// The cap is per deck: new cards of another deck do not use it up, but a card
// the two decks share counts in both (progress is keyed by code point).
void test_queue_new_cap_per_deck() {
    std::vector<CardSpec> ca = {{0x65E5, "day", "ニチ", "ひ", {}},
                                {0x6708, "month", "ゲツ", "つき", {}}};
    std::vector<CardSpec> cb = {{0x706B, "fire", "カ", "ひ", {}},
                                {0x6C34, "water", "スイ", "みず", {}}};
    std::vector<CardSpec> cc = {{0x65E5, "day", "ニチ", "ひ", {}},
                                {0x6728, "tree", "モク", "き", {}}};
    auto ba = makePack(ca, glyphsFor(ca));
    auto bb = makePack(cb, glyphsFor(cb));
    auto bc = makePack(cc, glyphsFor(cc));
    MemorySource sa(ba), sb(bb), sc(bc);
    PackReader a, b, c;
    TEST_ASSERT_EQUAL(PackError::Ok, a.open(sa));
    TEST_ASSERT_EQUAL(PackError::Ok, b.open(sb));
    TEST_ASSERT_EQUAL(PackError::Ok, c.open(sc));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(5);
    srs.setNewPerDay(1);
    srs.grade(0x65E5, Grade::Good, 5, static_cast<uint8_t>(QuizMode::Meaning), 0.5f);

    XorShiftRng rng(1);
    Queue q;
    q.rebuild(a, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.remaining()));
    q.rebuild(b, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(q.remaining()));
    q.rebuild(c, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.remaining()));
}

void test_queue_no_repeat_within_session() {
    std::vector<CardSpec> cs = modeCards(5);
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(0);
    srs.setNewPerDay(20);

    XorShiftRng rng(2);
    Queue q;
    q.rebuild(pack, srs, rng);
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(q.remaining()));

    bool seen[5] = {false, false, false, false, false};
    QueueItem it;
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_TRUE(q.next(&it));
        TEST_ASSERT_TRUE(it.cardIndex < 5);
        TEST_ASSERT_FALSE(seen[it.cardIndex]);  // no card twice in one session
        seen[it.cardIndex] = true;
        q.answered(it.cardIndex, Grade::Good);
    }
    TEST_ASSERT_FALSE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.remaining()));
}

void test_queue_relearn_requeues_and_caps() {
    std::vector<CardSpec> cs = modeCards(8);
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(1);
    srs.setNewPerDay(20);

    XorShiftRng rng(4);
    Queue q;
    q.rebuild(pack, srs, rng);

    QueueItem it;
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(0, it.cardIndex);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.answeredCount()));
    q.answered(0, Grade::Good);  // a correct answer does not requeue
    TEST_ASSERT_EQUAL_UINT32(7, static_cast<uint32_t>(q.remaining()));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(q.answeredCount()));

    q.answered(0, Grade::Again);  // requeue 5 items ahead
    TEST_ASSERT_EQUAL_UINT32(8, static_cast<uint32_t>(q.remaining()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(q.answeredCount()));
    for (uint32_t want = 1; want <= 5; ++want) {
        TEST_ASSERT_TRUE(q.next(&it));
        TEST_ASSERT_EQUAL_UINT32(want, it.cardIndex);  // the gap is respected
    }
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(0, it.cardIndex);  // the relearned card comes back
    q.rebuild(pack, srs, rng);  // a new session counts from zero
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(q.answeredCount()));

    // At most three relearn pushes per card per session: 1 serve + 3 pushes.
    std::vector<CardSpec> one = {{0x706B, "fire", "カ", "ひ", {}}};
    auto ob = makePack(one, glyphsFor(one));
    MemorySource os(ob);
    PackReader op;
    TEST_ASSERT_EQUAL(PackError::Ok, op.open(os));
    SrsStore s2;
    MemoryIo io2;
    s2.begin(io2);
    s2.setToday(1);
    Queue q2;
    q2.rebuild(op, s2, rng);
    int serves = 0;
    for (int i = 0; i < 10; ++i) {
        QueueItem o;
        if (!q2.next(&o)) break;
        TEST_ASSERT_EQUAL_UINT32(0, o.cardIndex);
        ++serves;
        q2.answered(0, Grade::Again);
    }
    TEST_ASSERT_EQUAL_INT(4, serves);
}

void test_queue_interleaves_modes() {
    std::vector<CardSpec> cs = modeCards(6);
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(0);
    srs.setNewPerDay(20);

    XorShiftRng rng(9);
    Queue q;
    q.rebuild(pack, srs, rng);

    bool seen[3] = {false, false, false};
    int prev = -1;
    QueueItem it;
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_TRUE(q.next(&it));
        const int m = static_cast<int>(it.mode);
        TEST_ASSERT_TRUE(m >= 0 && m <= 2);
        // Every card here offers all three modes, so a tie is always broken
        // toward a mode different from the previous item's.
        if (prev >= 0) TEST_ASSERT_TRUE(m != prev);
        seen[m] = true;
        prev = m;

        // Simulate the app: grade the answer (which records the mode) and
        // report it to the queue.
        Card c;
        TEST_ASSERT_TRUE(pack.card(it.cardIndex, &c));
        srs.grade(c.codepoint, Grade::Good, 0, static_cast<uint8_t>(m), 0.5f);
        q.answered(it.cardIndex, Grade::Good);
    }
    TEST_ASSERT_TRUE(seen[0] && seen[1] && seen[2]);
    TEST_ASSERT_FALSE(q.next(&it));
}

// A reading question needs one kind of reading, not both: cards without
// kun'yomi (common once the builder drops readings not worth asking) or without
// on'yomi still get their reading turn; a card with neither never does.
void test_queue_reading_needs_one_kind() {
    std::vector<CardSpec> cs = {{0x6C34, "water", "スイ", "みず", {}},
                                {0x6C17, "spirit", "キ、ケ", "", {}},       // no kun'yomi
                                {0x7551, "field", "", "はたけ、はた", {}},  // no on'yomi
                                {0x3005, "repeat mark", "", "", {}}};      // neither
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(PackError::Ok, pack.open(src));

    SrsStore srs;
    MemoryIo io;
    srs.begin(io);
    srs.setToday(10);
    // All due, in card order; meaning and meaning -> kanji already asked, so
    // the reading is the least used mode of every card.
    for (size_t i = 0; i < cs.size(); ++i) {
        setDue(srs, cs[i].cp, static_cast<uint16_t>(1 + i), 100);
        srs.getOrCreate(cs[i].cp).modeSeen = 0x5;
    }

    XorShiftRng rng(4);
    Queue q;
    q.rebuild(pack, srs, rng);
    QueueItem it;
    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(q.next(&it));
        TEST_ASSERT_EQUAL_UINT32(i, it.cardIndex);
        TEST_ASSERT_EQUAL(QuizMode::Reading, it.mode);
    }
    TEST_ASSERT_TRUE(q.next(&it));
    TEST_ASSERT_EQUAL_UINT32(3, it.cardIndex);
    TEST_ASSERT_TRUE(it.mode != QuizMode::Reading);
    TEST_ASSERT_FALSE(q.next(&it));
}

// Day streak over the SRS day index: consecutive days extend it, a gap resets
// it and the best is retained (docs/srs.md section 4).
void test_streak() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    TEST_ASSERT_EQUAL_UINT16(0, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(0, s.streakBest());
    TEST_ASSERT_EQUAL_UINT16(0, s.lastStudyDay());
    TEST_ASSERT_FALSE(s.hasStudied());

    // First ever answer starts the streak at 1.
    s.grade(0x65E5, Grade::Good, 10, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(1, s.streakBest());
    TEST_ASSERT_EQUAL_UINT16(10, s.lastStudyDay());
    TEST_ASSERT_TRUE(s.hasStudied());

    // Same-day answers do not change the streak.
    s.grade(0x6708, Grade::Good, 10, 1, 0.5f);
    s.grade(0x65E5, Grade::Hard, 10, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(1, s.streakBest());

    // Consecutive days increment it.
    s.grade(0x65E5, Grade::Good, 11, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(2, s.streakCurrent());
    s.grade(0x65E5, Grade::Good, 12, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(3, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(3, s.streakBest());

    // A gap resets the current streak; the best is retained.
    s.grade(0x65E5, Grade::Good, 20, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(3, s.streakBest());
    TEST_ASSERT_EQUAL_UINT16(20, s.lastStudyDay());

    // A single skipped day is a gap too.
    s.grade(0x65E5, Grade::Good, 22, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(3, s.streakBest());
}

// The first answer a fresh store ever sees, on SRS day index 0: day 0 doubles as
// the "never" value of last_study_day, so this is the regression the dedicated
// "has studied" flag exists for. It must start the streak at 1.
void test_streak_first_answer_on_day_zero() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(0);
    TEST_ASSERT_FALSE(s.hasStudied());

    s.grade(0x65E5, Grade::Good, 0, 0, 0.5f);
    TEST_ASSERT_TRUE(s.hasStudied());
    TEST_ASSERT_EQUAL_UINT16(0, s.lastStudyDay());
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(1, s.streakBest());

    // A same-day answer still changes nothing...
    s.grade(0x6708, Grade::Good, 0, 1, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    // ...and the next day continues it.
    s.grade(0x65E5, Grade::Good, 1, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(2, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(2, s.streakBest());
}

void test_review_ms_saturation() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    TEST_ASSERT_EQUAL_UINT32(0, s.reviewMs());
    s.addReviewMs(1500);
    TEST_ASSERT_EQUAL_UINT32(1500, s.reviewMs());
    s.addReviewMs(2500);
    TEST_ASSERT_EQUAL_UINT32(4000, s.reviewMs());
    // Saturates at 0xFFFFFFFF and never wraps.
    s.addReviewMs(0xFFFFFF00u);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, s.reviewMs());
    s.addReviewMs(1000);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, s.reviewMs());
    s.addReviewMs(0);  // a no-op, stays saturated
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, s.reviewMs());
}

// The 32..47 extension fields survive a save/load, at their documented offsets.
void test_serialize_new_header_roundtrip() {
    SrsStore a;
    MemoryIo empty;
    a.begin(empty);
    // Four consecutive SRS days -> streak 4, last study day 4.
    for (uint16_t day = 1; day <= 4; ++day) a.grade(0x65E5, Grade::Good, day, 0, 0.5f);
    a.addReviewMs(1234);
    a.addReviewMs(2000);

    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    TEST_ASSERT_EQUAL_UINT32(48 + 1 * 16 + 4, static_cast<uint32_t>(io.bytes.size()));
    // Raw offsets: streak_current @32, best @34, last_study_day @36, review_ms @40.
    TEST_ASSERT_EQUAL_UINT8(4, io.bytes[32]);
    TEST_ASSERT_EQUAL_UINT8(0, io.bytes[33]);
    TEST_ASSERT_EQUAL_UINT8(4, io.bytes[34]);
    TEST_ASSERT_EQUAL_UINT8(4, io.bytes[36]);
    TEST_ASSERT_EQUAL_UINT8(0xA2, io.bytes[40]);  // 3234 = 0x00000CA2, LE
    TEST_ASSERT_EQUAL_UINT8(0x0C, io.bytes[41]);
    // flags @30: the "has studied" bit survives too.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kSrsFlagHasStudied), io.bytes[30]);

    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(4, b.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(4, b.streakBest());
    TEST_ASSERT_EQUAL_UINT16(4, b.lastStudyDay());
    TEST_ASSERT_EQUAL_UINT32(3234, b.reviewMs());
    TEST_ASSERT_TRUE(b.hasStudied());

    // A file with the flag bit cleared but a non-zero last study day (an older
    // writer, or that bit reused later) recovers "has studied" from the day so a
    // running streak is not silently reset to 1.
    a.setFlags(0);
    MemoryIo io2;
    TEST_ASSERT_TRUE(a.save(io2));
    TEST_ASSERT_EQUAL_UINT8(0, io2.bytes[30]);
    SrsStore c;
    TEST_ASSERT_TRUE(c.begin(io2));
    TEST_ASSERT_TRUE(c.hasStudied());
    TEST_ASSERT_EQUAL_UINT16(4, c.streakCurrent());
}

// A file whose bytes 32..47 are zero (an old v1 writer, or a store that never
// recorded a streak/time) loads with zeros/defaults.
void test_serialize_old_file_has_defaults() {
    SrsStore a;
    MemoryIo empty;
    a.begin(empty);
    a.getOrCreate(0x65E5).reps = 1;  // a record, but no grade: no streak, no time
    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    for (size_t i = 32; i < 48; ++i) TEST_ASSERT_EQUAL_UINT8(0, io.bytes[i]);

    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(0, b.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(0, b.streakBest());
    TEST_ASSERT_EQUAL_UINT16(0, b.lastStudyDay());
    TEST_ASSERT_FALSE(b.hasStudied());
    TEST_ASSERT_EQUAL_UINT32(0, b.reviewMs());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(b.count()));
}

// ------------------------------------- forgetting progress (section 2.1)

namespace {

// 日 月 火 水 木 金 土: the kanji of the forgetting tests.
constexpr uint32_t kHi = 0x65E5, kTsuki = 0x6708, kKa = 0x706B, kMizu = 0x6C34, kKi = 0x6728,
                   kKin = 0x91D1, kDo = 0x571F;

// A store with records for 日 月 火 水 木 over three study days: 月 and 木
// missed once, a little time on questions.
void studied(SrsStore& s) {
    MemoryIo none;
    s.begin(none);
    const uint32_t cps[] = {kHi, kTsuki, kKa, kMizu, kKi};
    for (uint16_t day = 1; day <= 3; ++day)
        for (uint32_t cp : cps) s.grade(cp, Grade::Good, day, 0, 0.5f);
    s.grade(kTsuki, Grade::Again, 3, 1, 0.5f);
    s.grade(kKi, Grade::Again, 3, 1, 0.5f);
    s.addReviewMs(45000);
    s.setToday(3);
}

}  // namespace

// The lifetime Again counter: one per Again answer, never above the answers,
// at header offset 44 and back from a save.
void test_again_total() {
    SrsStore s;
    studied(s);
    const Stats st = s.stats();
    TEST_ASSERT_EQUAL_UINT32(17, st.answers);
    TEST_ASSERT_EQUAL_UINT32(2, st.again);
    TEST_ASSERT_EQUAL_UINT32(2, s.againTotal());
    TEST_ASSERT_EQUAL_UINT32(st.reps, st.answers);  // nothing forgotten yet
    TEST_ASSERT_EQUAL_UINT32(st.lapses, st.again);
    MemoryIo io;
    TEST_ASSERT_TRUE(s.save(io));
    TEST_ASSERT_EQUAL_UINT8(2, io.bytes[44]);
    TEST_ASSERT_EQUAL_UINT8(0, io.bytes[45]);
    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT32(2, b.againTotal());

    // A file from before the counter (bytes 44..47 zero: a store whose records
    // carry lapses its header never counted) starts it from the records'
    // lapses, at most the answers.
    SrsStore legacy;
    MemoryIo none;
    legacy.begin(none);
    legacy.getOrCreate(kTsuki).lapses = 3;
    legacy.getOrCreate(kTsuki).reps = 5;
    legacy.getOrCreate(kKi).lapses = 1;
    legacy.getOrCreate(kKi).reps = 4;
    legacy.setAnsweredTotal(9);
    MemoryIo oldIo;
    TEST_ASSERT_TRUE(legacy.save(oldIo));
    for (size_t i = 44; i < 48; ++i) TEST_ASSERT_EQUAL_UINT8(0, oldIo.bytes[i]);
    SrsStore fix;
    TEST_ASSERT_TRUE(fix.begin(oldIo));
    TEST_ASSERT_EQUAL_UINT32(4, fix.againTotal());
    TEST_ASSERT_EQUAL_UINT32(4, fix.stats().again);
    legacy.setAnsweredTotal(2);
    MemoryIo oldIo2;
    TEST_ASSERT_TRUE(legacy.save(oldIo2));
    SrsStore fix2;
    TEST_ASSERT_TRUE(fix2.begin(oldIo2));
    TEST_ASSERT_EQUAL_UINT32(2, fix2.againTotal());
}

// forget(): any order, duplicates and kanji without a record are fine; only
// those records go, the table stays sorted, and the lifetime counters stay.
void test_forget_set() {
    SrsStore s;
    studied(s);
    const Stats before = s.stats();
    const Record mizu = *s.find(kMizu);

    const uint32_t gone[] = {kKi, kTsuki, kDo, kTsuki};  // unsorted, a duplicate, 土 unseen
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.forget(gone, 4)));
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(s.count()));
    TEST_ASSERT_NULL(s.find(kTsuki));
    TEST_ASSERT_NULL(s.find(kKi));
    TEST_ASSERT_NOT_NULL(s.find(kHi));
    TEST_ASSERT_NOT_NULL(s.find(kKa));
    TEST_ASSERT_EQUAL_MEMORY(&mizu, s.find(kMizu), sizeof mizu);  // untouched
    for (size_t i = 1; i < s.records().size(); ++i)
        TEST_ASSERT_TRUE(s.records()[i - 1].codepoint < s.records()[i].codepoint);

    // The lifetime statistics are as they were; the per-record ones describe
    // the kanji still tracked.
    const Stats after = s.stats();
    TEST_ASSERT_EQUAL_UINT32(before.answers, after.answers);
    TEST_ASSERT_EQUAL_UINT32(before.again, after.again);
    TEST_ASSERT_EQUAL_UINT32(before.streakCurrent, after.streakCurrent);
    TEST_ASSERT_EQUAL_UINT32(before.streakBest, after.streakBest);
    TEST_ASSERT_EQUAL_UINT32(before.reviewMs, after.reviewMs);
    TEST_ASSERT_EQUAL_UINT32(3, after.seen);
    TEST_ASSERT_EQUAL_UINT32(9, after.reps);  // 日 火 水, three each
    TEST_ASSERT_EQUAL_UINT32(0, after.lapses);
    TEST_ASSERT_TRUE(after.again <= after.answers);
    TEST_ASSERT_TRUE(s.hasStudied());
    TEST_ASSERT_EQUAL_UINT16(3, s.today());

    // A forgotten kanji is new again: its next answer is a first review.
    s.grade(kTsuki, Grade::Good, 3, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT8(1, s.find(kTsuki)->reps);
    TEST_ASSERT_EQUAL_UINT8(0, s.find(kTsuki)->lapses);
    TEST_ASSERT_TRUE(introducedOn(*s.find(kTsuki), 3));

    // Nothing to forget: no change.
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(s.forget(gone, 0)));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(s.forget(nullptr, 3)));
    const uint32_t unseen[] = {kDo, kKin};
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(s.forget(unseen, 2)));
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(s.count()));
}

// Deleting a deck file forgets only the kanji no installed deck still has; the
// code points come from the packs' card tables.
void test_forget_on_delete_keeps_shared() {
    // "N5": 日 月 火 水 (and 日 twice, as a broken pack might); "Jouyou": 月 火 木.
    std::vector<CardSpec> n5 = {{kHi, "day", "ニチ", "ひ", {}, {}},
                                {kTsuki, "month", "ゲツ", "つき", {}, {}},
                                {kKa, "fire", "カ", "ひ", {}, {}},
                                {kMizu, "water", "スイ", "みず", {}, {}},
                                {kHi, "day", "ニチ", "ひ", {}, {}}};
    std::vector<CardSpec> joyo = {{kKi, "tree", "モク", "き", {}, {}},
                                  {kTsuki, "month", "ゲツ", "つき", {}, {}},
                                  {kKa, "fire", "カ", "ひ", {}, {}}};
    const auto bn5 = makePack(n5, glyphsFor(n5));
    const auto bjoyo = makePack(joyo, glyphsFor(joyo));
    MemorySource sn5(bn5), sjoyo(bjoyo);
    std::vector<uint32_t> deck, keep;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok),
                      static_cast<int>(readPackCodepoints(sn5, &deck)));
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(deck.size()));
    TEST_ASSERT_EQUAL_HEX32(kHi, deck[0]);  // table order
    TEST_ASSERT_EQUAL_HEX32(kMizu, deck[3]);
    sortUnique(deck);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(deck.size()));
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok),
                      static_cast<int>(readPackCodepoints(sjoyo, &keep)));
    sortUnique(keep);

    SrsStore s;
    studied(s);  // 日 月 火 水 木
    s.getOrCreate(kDo).reps = 1;  // 土: from a deck deleted long ago
    const Stats before = s.stats();

    // Delete N5 while Jouyou stays: 日 and 水 go, 月 and 火 are Jouyou's too.
    const std::vector<uint32_t> gone = forgetOnDelete(deck, keep);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(gone.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.forget(gone.data(), gone.size())));
    TEST_ASSERT_NULL(s.find(kHi));
    TEST_ASSERT_NULL(s.find(kMizu));
    TEST_ASSERT_NOT_NULL(s.find(kTsuki));
    TEST_ASSERT_NOT_NULL(s.find(kKa));
    TEST_ASSERT_NOT_NULL(s.find(kKi));
    TEST_ASSERT_NOT_NULL(s.find(kDo));  // not this deck's: never touched

    // Then delete Jouyou with no deck left: all of its kanji go.
    const std::vector<uint32_t> rest = forgetOnDelete(keep, {});
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(s.forget(rest.data(), rest.size())));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(s.count()));  // 土

    // Statistics: lifetime counters unchanged, nothing negative.
    const Stats after = s.stats();
    TEST_ASSERT_EQUAL_UINT32(before.answers, after.answers);
    TEST_ASSERT_EQUAL_UINT32(before.again, after.again);
    TEST_ASSERT_EQUAL_UINT32(before.reviewMs, after.reviewMs);
    TEST_ASSERT_EQUAL_UINT32(before.streakBest, after.streakBest);
    TEST_ASSERT_EQUAL_UINT32(1, after.seen);
    TEST_ASSERT_EQUAL_UINT32(0, after.learned);

    // A pack that does not read gives nothing to forget.
    std::vector<uint8_t> bad = bn5;
    bad[0] = 'X';
    MemorySource sbad(bad);
    std::vector<uint32_t> none = {1, 2, 3};
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::BadMagic),
                      static_cast<int>(readPackCodepoints(sbad, &none)));
    TEST_ASSERT_TRUE(none.empty());
    std::vector<uint8_t> cut(bn5.begin(), bn5.begin() + 64 + 32 * 2);  // card table cut short
    MemorySource scut(cut);
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(PackError::Ok),
                          static_cast<int>(readPackCodepoints(scut, &none)));
    TEST_ASSERT_TRUE(none.empty());
}

// The "This deck" preview: the deck's kanji with a record, and how many of
// them another installed deck has.
void test_deck_reset_preview() {
    SrsStore s;
    studied(s);  // 日 月 火 水 木
    std::vector<uint32_t> deck = {kHi, kTsuki, kKa, kDo};  // 土 never studied
    std::vector<uint32_t> others = {kTsuki, kKin, kKa};
    sortUnique(deck);
    sortUnique(others);
    DeckReset r = deckResetPreview(s, deck, others);
    TEST_ASSERT_EQUAL_UINT32(3, r.records);
    TEST_ASSERT_EQUAL_UINT32(2, r.shared);
    r = deckResetPreview(s, deck, {});
    TEST_ASSERT_EQUAL_UINT32(3, r.records);
    TEST_ASSERT_EQUAL_UINT32(0, r.shared);
    // Resetting the deck forgets all of them, the shared ones too.
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(s.forget(deck.data(), deck.size())));
    r = deckResetPreview(s, deck, others);
    TEST_ASSERT_EQUAL_UINT32(0, r.records);
    TEST_ASSERT_EQUAL_UINT32(0, r.shared);
    TEST_ASSERT_NOT_NULL(s.find(kMizu));
    TEST_ASSERT_NOT_NULL(s.find(kKi));
}

// "Everything": every record and the lifetime statistics go; the clock and the
// knobs stay, and the next answer starts a streak of 1.
void test_reset_progress() {
    SrsStore s;
    studied(s);
    // 2026-10-20 10:00 UTC, 12:00 at UTC+2.
    const int64_t noon = static_cast<int64_t>(daysFromCivil(2026, 10, 20)) * kSrsMsPerDay +
                         10 * 3600000LL;
    TEST_ASSERT_TRUE(s.syncWallClock(noon, 120).kind == ClockSync::Anchored);
    s.setNewPerDay(5);
    s.setSlowMs(5000);
    s.setToday(3);
    const uint16_t anchor = s.epochAnchor();
    const uint32_t carry = s.msCarry();

    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(s.resetProgress()));
    const Stats st = s.stats();
    TEST_ASSERT_EQUAL_UINT32(0, st.seen);
    TEST_ASSERT_EQUAL_UINT32(0, st.learned);
    TEST_ASSERT_EQUAL_UINT32(0, st.answers);
    TEST_ASSERT_EQUAL_UINT32(0, st.again);
    TEST_ASSERT_EQUAL_UINT32(0, st.reps);
    TEST_ASSERT_EQUAL_UINT32(0, st.lapses);
    TEST_ASSERT_EQUAL_UINT32(0, st.streakCurrent);
    TEST_ASSERT_EQUAL_UINT32(0, st.streakBest);
    TEST_ASSERT_EQUAL_UINT32(0, st.reviewMs);
    TEST_ASSERT_FALSE(s.hasStudied());
    TEST_ASSERT_EQUAL_UINT16(0, s.lastStudyDay());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
    // Kept: the day, the carry, the anchor and the knobs.
    TEST_ASSERT_EQUAL_UINT16(3, s.today());
    TEST_ASSERT_EQUAL_UINT16(anchor, s.epochAnchor());
    TEST_ASSERT_EQUAL_UINT32(carry, s.msCarry());
    TEST_ASSERT_EQUAL_UINT16(5, s.newPerDay());
    TEST_ASSERT_EQUAL_UINT16(5000, s.slowMs());

    // Survives a save and a load.
    MemoryIo io;
    TEST_ASSERT_TRUE(s.save(io));
    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(b.count()));
    TEST_ASSERT_EQUAL_UINT32(0, b.answeredTotal());
    TEST_ASSERT_EQUAL_UINT32(0, b.againTotal());
    TEST_ASSERT_FALSE(b.hasStudied());
    TEST_ASSERT_EQUAL_UINT16(anchor, b.epochAnchor());
    TEST_ASSERT_EQUAL_UINT16(3, b.today());

    // A fresh start: the next answer, the day after the old last study day,
    // starts a streak of 1 rather than extending the forgotten one.
    b.grade(kHi, Grade::Good, 4, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, b.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(1, b.streakBest());
    TEST_ASSERT_EQUAL_UINT32(1, b.answeredTotal());
}

// Forgetting survives a save and a load: the file holds exactly the records
// that are left, and the lifetime counters.
void test_forget_roundtrip() {
    SrsStore s;
    studied(s);
    const uint32_t gone[] = {kHi, kKi};
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.forget(gone, 2)));
    MemoryIo io;
    TEST_ASSERT_TRUE(s.save(io));
    TEST_ASSERT_EQUAL_UINT32(48 + 3 * 16 + 4, static_cast<uint32_t>(io.bytes.size()));
    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(b.count()));
    TEST_ASSERT_EQUAL_MEMORY(s.records().data(), b.records().data(), 3 * sizeof(Record));
    TEST_ASSERT_EQUAL_UINT32(s.answeredTotal(), b.answeredTotal());
    TEST_ASSERT_EQUAL_UINT32(s.againTotal(), b.againTotal());
    TEST_ASSERT_EQUAL_UINT32(2, b.againTotal());  // 木's miss is still counted
    TEST_ASSERT_EQUAL_UINT32(s.reviewMs(), b.reviewMs());
    TEST_ASSERT_EQUAL_UINT16(s.streakCurrent(), b.streakCurrent());
    TEST_ASSERT_NULL(b.find(kHi));
    TEST_ASSERT_NULL(b.find(kKi));
}

// ------------------------------------------------ wall clock (section 5.1)

namespace {

constexpr int64_t kMinMs = 60000;
constexpr int64_t kHourMs = 60 * kMinMs;

// UTC milliseconds of local time y-m-d hh:mm in a zone `tz` minutes east of UTC.
int64_t utcOf(int y, unsigned m, unsigned d, int hh, int mm, int tz) {
    const int64_t local = static_cast<int64_t>(daysFromCivil(y, m, d)) * kSrsMsPerDay +
                          hh * kHourMs + mm * kMinMs;
    return local - tz * kMinMs;
}

}  // namespace

void test_civil_dates() {
    TEST_ASSERT_EQUAL_INT32(0, daysFromCivil(1970, 1, 1));
    TEST_ASSERT_EQUAL_INT32(-1, daysFromCivil(1969, 12, 31));
    TEST_ASSERT_EQUAL_INT32(11017, daysFromCivil(2000, 3, 1));
    TEST_ASSERT_EQUAL_INT32(20743, daysFromCivil(2026, 10, 17));
    TEST_ASSERT_EQUAL_INT32(20743, kSrsLegacyEpochDay);
    TEST_ASSERT_EQUAL_INT32(21243, daysFromCivil(2028, 2, 29));  // leap day

    // civilFromDays inverts daysFromCivil over a few years, leap days included.
    for (int32_t z = daysFromCivil(2026, 1, 1); z < daysFromCivil(2030, 1, 1); ++z) {
        int y = 0;
        unsigned m = 0, d = 0;
        civilFromDays(z, &y, &m, &d);
        TEST_ASSERT_EQUAL_INT32(z, daysFromCivil(y, m, d));
        TEST_ASSERT_TRUE(m >= 1 && m <= 12 && d >= 1 && d <= 31);
    }
    int y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(21243, &y, &m, &d);
    TEST_ASSERT_EQUAL_INT(2028, y);
    TEST_ASSERT_EQUAL_UINT(2, m);
    TEST_ASSERT_EQUAL_UINT(29, d);

    // Weekdays: 0 = Sunday. 1970-01-01 was a Thursday, 2026-10-17 a Saturday.
    TEST_ASSERT_EQUAL_INT(4, weekdayFromDays(0));
    TEST_ASSERT_EQUAL_INT(3, weekdayFromDays(-1));
    TEST_ASSERT_EQUAL_INT(6, weekdayFromDays(20743));
    TEST_ASSERT_EQUAL_INT(0, weekdayFromDays(20744));
    TEST_ASSERT_EQUAL_INT(2, weekdayFromDays(21243));
}

void test_wall_clock_plausibility() {
    TEST_ASSERT_TRUE(wallClockPlausible(kWallClockMinUtcMs, 0));
    TEST_ASSERT_FALSE(wallClockPlausible(kWallClockMinUtcMs - 1, 0));  // before 2026
    TEST_ASSERT_FALSE(wallClockPlausible(0, 0));                        // the epoch
    TEST_ASSERT_FALSE(wallClockPlausible(-5, 0));
    TEST_ASSERT_TRUE(wallClockPlausible(kWallClockMaxUtcMs - 1, 0));
    TEST_ASSERT_FALSE(wallClockPlausible(kWallClockMaxUtcMs, 0));       // 2100
    TEST_ASSERT_TRUE(wallClockPlausible(utcOf(2026, 10, 18, 12, 0, 0), -12 * 60));
    TEST_ASSERT_TRUE(wallClockPlausible(utcOf(2026, 10, 18, 12, 0, 0), 14 * 60));
    TEST_ASSERT_FALSE(wallClockPlausible(utcOf(2026, 10, 18, 12, 0, 0), -12 * 60 - 1));
    TEST_ASSERT_FALSE(wallClockPlausible(utcOf(2026, 10, 18, 12, 0, 0), 14 * 60 + 1));

    // A rejected sync leaves the store exactly as it was.
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(9);
    s.addElapsedMs(1234);
    const ClockSyncResult r = s.syncWallClock(kWallClockMinUtcMs - 1, 60);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Rejected);
    TEST_ASSERT_EQUAL_UINT16(9, s.today());
    TEST_ASSERT_EQUAL_UINT32(1234, s.msCarry());
    TEST_ASSERT_EQUAL_UINT16(0, s.epochAnchor());
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 18, 12, 0, 0), 15 * 60).kind ==
                     ClockSync::Rejected);
    TEST_ASSERT_FALSE(s.hasWallClock());
}

// The study day starts at 04:00 local, whatever the zone.
void test_study_day_rollover_hour() {
    const int zones[] = {0, 120, -300, 330, 14 * 60, -12 * 60};
    for (int tz : zones) {
        int32_t day = 0;
        uint32_t into = 0;
        // 03:59 local on the 20th still belongs to the 19th, 23 h 59 min in.
        TEST_ASSERT_TRUE(studyDayOf(utcOf(2026, 10, 20, 3, 59, tz), tz, &day, &into));
        TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 19), day);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(23 * kHourMs + 59 * kMinMs), into);
        // 04:00 sharp starts the 20th; 04:01 is one minute in.
        TEST_ASSERT_TRUE(studyDayOf(utcOf(2026, 10, 20, 4, 0, tz), tz, &day, &into));
        TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 20), day);
        TEST_ASSERT_EQUAL_UINT32(0, into);
        TEST_ASSERT_TRUE(studyDayOf(utcOf(2026, 10, 20, 4, 1, tz), tz, &day, &into));
        TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 20), day);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMinMs), into);
        // Local midnight is still the previous study day.
        TEST_ASSERT_TRUE(studyDayOf(utcOf(2026, 10, 20, 0, 0, tz), tz, &day, &into));
        TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 19), day);
    }
    // The same UTC instant is a different study day on either side of the world.
    const int64_t t = utcOf(2026, 10, 20, 2, 0, 0);  // 02:00 UTC
    int32_t east = 0, west = 0;
    TEST_ASSERT_TRUE(studyDayOf(t, 9 * 60, &east, nullptr));   // 11:00 in Tokyo
    TEST_ASSERT_TRUE(studyDayOf(t, -5 * 60, &west, nullptr));  // 21:00 the day before
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 20), east);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 19), west);
    // Implausible input leaves the outputs untouched.
    int32_t day = 77;
    uint32_t into = 88;
    TEST_ASSERT_FALSE(studyDayOf(1000, 0, &day, &into));
    TEST_ASSERT_EQUAL_INT32(77, day);
    TEST_ASSERT_EQUAL_UINT32(88, into);
}

// No anchor: the legacy base is shown and the first sync anchors the current
// index as today without moving it.
void test_sync_anchor_absent_then_present() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(5);
    TEST_ASSERT_FALSE(s.hasWallClock());
    TEST_ASSERT_EQUAL_INT32(kSrsLegacyEpochDay + 5, s.epochDayOf(5));
    TEST_ASSERT_EQUAL_INT32(kSrsLegacyEpochDay, s.epochDayOf(0));

    const int tz = 60;  // CET
    const ClockSyncResult r = s.syncWallClock(utcOf(2026, 11, 2, 10, 0, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Anchored);
    TEST_ASSERT_EQUAL_UINT16(0, r.daysAdvanced);
    TEST_ASSERT_EQUAL_UINT16(5, s.today());  // the index is not moved
    TEST_ASSERT_TRUE(s.hasWallClock());
    TEST_ASSERT_EQUAL_UINT16(daysFromCivil(2026, 11, 2) - 5, s.epochAnchor());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 2), s.epochDayOf(s.today()));
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 31), s.epochDayOf(3));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(6 * kHourMs), s.msCarry());  // since 04:00
}

// Anchored: a real day ahead advances, the same day only aligns the carry,
// a real day behind re-bases the display and keeps the index.
void test_sync_ahead_same_behind() {
    const int tz = -300;  // EST
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(10);
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 11, 1, 20, 0, tz), tz).kind ==
                     ClockSync::Anchored);
    const uint16_t anchor = s.epochAnchor();

    // Same study day, later: only the carry moves. 03:59 the next morning is
    // still the same study day.
    ClockSyncResult r = s.syncWallClock(utcOf(2026, 11, 2, 3, 59, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Same);
    TEST_ASSERT_EQUAL_UINT16(10, s.today());
    TEST_ASSERT_EQUAL_UINT16(anchor, s.epochAnchor());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(23 * kHourMs + 59 * kMinMs), s.msCarry());

    // Two minutes later it is 04:01: the next study day.
    r = s.syncWallClock(utcOf(2026, 11, 2, 4, 1, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(1, r.daysAdvanced);
    TEST_ASSERT_EQUAL_UINT16(11, s.today());
    TEST_ASSERT_EQUAL_UINT16(anchor, s.epochAnchor());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMinMs), s.msCarry());

    // Idempotent: the same post again changes nothing.
    r = s.syncWallClock(utcOf(2026, 11, 2, 4, 1, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Same);
    TEST_ASSERT_EQUAL_UINT16(11, s.today());
    TEST_ASSERT_EQUAL_UINT16(anchor, s.epochAnchor());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMinMs), s.msCarry());

    // A week away.
    r = s.syncWallClock(utcOf(2026, 11, 9, 18, 30, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(7, r.daysAdvanced);
    TEST_ASSERT_EQUAL_UINT16(18, s.today());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 9), s.epochDayOf(s.today()));

    // The user overshot by two days on the date screen: the index is ahead of
    // the real date. The sync keeps the index and re-bases the display.
    s.advanceDaysManual(2);
    TEST_ASSERT_EQUAL_UINT16(20, s.today());
    r = s.syncWallClock(utcOf(2026, 11, 9, 19, 0, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Reanchored);
    TEST_ASSERT_EQUAL_UINT16(0, r.daysAdvanced);
    TEST_ASSERT_EQUAL_UINT16(20, s.today());  // never rewound
    TEST_ASSERT_EQUAL_UINT16(anchor - 2, s.epochAnchor());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 9), s.epochDayOf(s.today()));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(15 * kHourMs), s.msCarry());

    // From there index and real date move together again.
    r = s.syncWallClock(utcOf(2026, 11, 10, 8, 0, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(21, s.today());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 10), s.epochDayOf(s.today()));
}

// After a sync the uptime and sleep rules roll the day over at 04:00 local.
void test_sync_aligns_rollover_to_four() {
    const int zones[] = {120, -420};
    for (int tz : zones) {
        SrsStore s;
        MemoryIo io;
        s.begin(io);
        s.setToday(3);
        s.addElapsedMs(17 * kHourMs);  // an arbitrary carry from before the sync
        TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 12, 1, 22, 0, tz), tz).kind ==
                         ClockSync::Anchored);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(18 * kHourMs), s.msCarry());

        // Awake until 03:59:59.999 local: still the same day.
        s.tick(0);
        s.tick(static_cast<uint32_t>(6 * kHourMs - 1));
        TEST_ASSERT_EQUAL_UINT16(3, s.today());
        // 04:00 local: the next day.
        s.tick(static_cast<uint32_t>(6 * kHourMs));
        TEST_ASSERT_EQUAL_UINT16(4, s.today());
        TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 12, 2), s.epochDayOf(s.today()));

        // Deep sleep from 10:00 to 03:00 the next morning: not yet...
        s.addElapsedMs(6 * kHourMs);   // 10:00
        s.addElapsedMs(17 * kHourMs);  // 03:00
        TEST_ASSERT_EQUAL_UINT16(4, s.today());
        s.addElapsedMs(kHourMs);  // ...04:00
        TEST_ASSERT_EQUAL_UINT16(5, s.today());
        TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());
    }
}

// The day index never decreases, whatever the sync sequence (an earlier but
// plausible phone clock included).
void test_sync_monotonic() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(40);
    const int64_t times[] = {
        utcOf(2026, 12, 10, 12, 0, 0), utcOf(2026, 12, 12, 9, 0, 0), utcOf(2026, 12, 1, 9, 0, 0),
        utcOf(2026, 12, 12, 3, 0, 0),  utcOf(2027, 1, 5, 23, 0, 0),  utcOf(2026, 1, 1, 0, 0, 0),
        utcOf(2027, 1, 6, 5, 0, 0),
    };
    const int zones[] = {0, 540, -600, 60, 0, 0, 330};
    uint16_t prev = s.today();
    for (size_t i = 0; i < sizeof times / sizeof times[0]; ++i) {
        const ClockSyncResult r = s.syncWallClock(times[i], zones[i]);
        TEST_ASSERT_TRUE(r.kind != ClockSync::Rejected);
        TEST_ASSERT_TRUE(s.today() >= prev);
        TEST_ASSERT_EQUAL_UINT16(s.today() - prev, r.daysAdvanced);
        // Whatever happened to the index, the displayed date is the real one.
        int32_t real = 0;
        TEST_ASSERT_TRUE(studyDayOf(times[i], zones[i], &real, nullptr));
        TEST_ASSERT_EQUAL_INT32(real, s.epochDayOf(s.today()));
        prev = s.today();
    }
}

// The new-card budget and the streak behave across a sync-driven day change.
void test_sync_counters() {
    const int tz = 0;
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(0);
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 20, 9, 0, tz), tz).kind ==
                     ClockSync::Anchored);
    s.grade(0x65E5, Grade::Good, s.today(), 0, 0.5f);  // a new card, first study day
    TEST_ASSERT_EQUAL_UINT16(1, s.newIntroducedCount());
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());

    // Same day: nothing resets.
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 20, 21, 0, tz), tz).kind == ClockSync::Same);
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(1, s.newIntroducedCount());

    // Next day: a fresh new-card budget, and the next answer extends the streak.
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 21, 7, 0, tz), tz).kind ==
                     ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(1, s.today());
    TEST_ASSERT_EQUAL_UINT16(1, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
    s.grade(0x6708, Grade::Good, s.today(), 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.newIntroducedCount());
    TEST_ASSERT_EQUAL_UINT16(2, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(2, s.streakBest());

    // Two days later: the gap restarts the streak, the best is kept.
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 23, 7, 0, tz), tz).kind ==
                     ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(3, s.today());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
    s.grade(0x65E5, Grade::Good, s.today(), 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(1, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(2, s.streakBest());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());  // a review, not a new card

    // Re-anchoring (the index ran ahead) touches neither counter.
    s.advanceDaysManual(1);
    s.grade(0x706B, Grade::Good, s.today(), 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT16(2, s.streakCurrent());
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 10, 23, 20, 0, tz), tz).kind ==
                     ClockSync::Reanchored);
    TEST_ASSERT_EQUAL_UINT16(4, s.today());
    TEST_ASSERT_EQUAL_UINT16(2, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(4, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(1, s.newIntroducedCount());
}

// The date screen's gap: n > 0 restarts the carry (late, never early).
void test_advance_days_manual() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(7);
    s.addElapsedMs(20 * kHourMs);
    s.advanceDaysManual(0);  // "same day": nothing changes
    TEST_ASSERT_EQUAL_UINT16(7, s.today());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(20 * kHourMs), s.msCarry());

    s.setNewIntroducedDay(7);
    s.setNewIntroducedCount(5);
    s.advanceDaysManual(2);
    TEST_ASSERT_EQUAL_UINT16(9, s.today());
    TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());  // the next rollover is 24 h away
    TEST_ASSERT_EQUAL_UINT16(9, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
    s.addElapsedMs(kSrsMsPerDay - 1);
    TEST_ASSERT_EQUAL_UINT16(9, s.today());

    s.setToday(0xFFFE);
    s.advanceDaysManual(5);  // saturates like advanceDays
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, s.today());
}

// The date screen, forward and "today": a later date is advanceDaysManual(),
// 0 changes nothing (the carry included), past the last index day is refused.
void test_date_manual_forward_and_same() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(7);
    s.addElapsedMs(20 * kHourMs);
    TEST_ASSERT_EQUAL_INT32(0, s.manualStepsMin());  // never synced: today is the floor
    TEST_ASSERT_EQUAL_INT32(0xFFFF - 7, s.manualStepsMax());

    TEST_ASSERT_TRUE(s.setDateManual(0) == ManualDate::Same);
    TEST_ASSERT_EQUAL_UINT16(7, s.today());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(20 * kHourMs), s.msCarry());

    s.setNewIntroducedDay(7);
    s.setNewIntroducedCount(5);
    TEST_ASSERT_TRUE(s.setDateManual(3) == ManualDate::Advanced);
    TEST_ASSERT_EQUAL_UINT16(10, s.today());
    TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());
    TEST_ASSERT_EQUAL_UINT16(10, s.newIntroducedDay());
    TEST_ASSERT_EQUAL_UINT16(0, s.newIntroducedCount());
    TEST_ASSERT_FALSE(s.hasWallClock());  // a forward step never anchors

    s.setToday(0xFFFE);
    TEST_ASSERT_EQUAL_INT32(1, s.manualStepsMax());
    TEST_ASSERT_TRUE(s.setDateManual(2) == ManualDate::Rejected);
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, s.today());
    TEST_ASSERT_TRUE(s.setDateManual(1) == ManualDate::Advanced);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, s.today());
    TEST_ASSERT_EQUAL_INT32(0, s.manualStepsMax());
}

// An earlier date on an anchored store: the manual twin of a Reanchored sync.
// The index, due days, streak and new-card counter stay; the anchor moves
// back, the carry restarts, and later syncs measure from the new anchor.
void test_date_manual_back_reanchors() {
    const int tz = 60;
    SrsStore s;
    MemoryIo empty;
    s.begin(empty);
    s.setToday(18);
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 11, 9, 10, 0, tz), tz).kind ==
                     ClockSync::Anchored);
    s.grade(0x65E5, Grade::Good, s.today(), 0, 0.5f);
    const Record before = *s.find(0x65E5);
    const uint16_t anchor = s.epochAnchor();

    // The user overshot by two days earlier on: the device shows the 11th.
    s.advanceDaysManual(2);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 11), s.epochDayOf(s.today()));
    s.grade(0x6708, Grade::Good, s.today(), 0, 0.5f);  // a new card on the 11th
    s.addElapsedMs(5 * kHourMs);
    const uint16_t streak = s.streakCurrent();
    const uint16_t introduced = s.newIntroducedCount();
    TEST_ASSERT_EQUAL_UINT16(1, introduced);
    TEST_ASSERT_TRUE(s.manualStepsMin() < -2);

    TEST_ASSERT_TRUE(s.setDateManual(-2) == ManualDate::Reanchored);
    TEST_ASSERT_EQUAL_UINT16(20, s.today());  // never rewound
    TEST_ASSERT_EQUAL_UINT16(anchor - 2, s.epochAnchor());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 9), s.epochDayOf(s.today()));
    TEST_ASSERT_EQUAL_UINT32(0, s.msCarry());  // late, never early
    const Record* after = s.find(0x65E5);
    TEST_ASSERT_EQUAL_UINT16(before.dueDay, after->dueDay);
    TEST_ASSERT_EQUAL_UINT16(before.lastDay, after->lastDay);
    TEST_ASSERT_EQUAL_UINT16(streak, s.streakCurrent());
    TEST_ASSERT_EQUAL_UINT16(20, s.lastStudyDay());
    TEST_ASSERT_EQUAL_UINT16(20, s.newIntroducedDay());  // same index day: same budget
    TEST_ASSERT_EQUAL_UINT16(introduced, s.newIntroducedCount());

    // It persists like any anchor.
    MemoryIo io;
    TEST_ASSERT_TRUE(s.save(io));
    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(anchor - 2, b.epochAnchor());
    TEST_ASSERT_EQUAL_UINT16(20, b.today());

    // From there index and real date move together: the same day is Same,
    // the next one advances by one.
    TEST_ASSERT_TRUE(b.syncWallClock(utcOf(2026, 11, 9, 22, 0, tz), tz).kind == ClockSync::Same);
    TEST_ASSERT_EQUAL_UINT16(20, b.today());
    const ClockSyncResult r = b.syncWallClock(utcOf(2026, 11, 10, 9, 0, tz), tz);
    TEST_ASSERT_TRUE(r.kind == ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(1, r.daysAdvanced);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 10), b.epochDayOf(b.today()));
}

// Without an anchor there is no date to correct: below the current day is
// refused and nothing changes; the screen's own forward steps can still be
// undone because they were never applied.
void test_date_manual_back_needs_anchor() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(12);
    s.addElapsedMs(3 * kHourMs);
    TEST_ASSERT_EQUAL_INT32(0, s.manualStepsMin());
    TEST_ASSERT_TRUE(s.setDateManual(-1) == ManualDate::Rejected);
    TEST_ASSERT_EQUAL_UINT16(12, s.today());
    TEST_ASSERT_EQUAL_UINT16(0, s.epochAnchor());
    TEST_ASSERT_FALSE(s.hasWallClock());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(3 * kHourMs), s.msCarry());
    TEST_ASSERT_EQUAL_INT32(kSrsLegacyEpochDay + 12, s.epochDayOf(s.today()));

    // The first sync still anchors the current index as today.
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 12, 1, 12, 0, 0), 0).kind ==
                     ClockSync::Anchored);
    TEST_ASSERT_EQUAL_UINT16(12, s.today());
    TEST_ASSERT_TRUE(s.manualStepsMin() < 0);
}

// The floor: 2026-01-01, the earliest date a sync accepts, and an anchor that
// stays at 1 or more even for an index larger than that day count.
void test_date_manual_floor() {
    SrsStore s;
    MemoryIo io;
    s.begin(io);
    s.setToday(2);
    TEST_ASSERT_TRUE(s.syncWallClock(utcOf(2026, 1, 3, 12, 0, 0), 0).kind ==
                     ClockSync::Anchored);
    TEST_ASSERT_EQUAL_INT32(-2, s.manualStepsMin());
    TEST_ASSERT_TRUE(s.setDateManual(-3) == ManualDate::Rejected);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 1, 3), s.epochDayOf(s.today()));
    TEST_ASSERT_TRUE(s.setDateManual(-2) == ManualDate::Reanchored);
    TEST_ASSERT_EQUAL_INT32(kWallClockMinEpochDay, s.epochDayOf(s.today()));
    TEST_ASSERT_EQUAL_INT32(0, s.manualStepsMin());
    TEST_ASSERT_TRUE(s.setDateManual(-1) == ManualDate::Rejected);

    // An index past the floor's day count (synced in 2099): the anchor is the
    // limit, never 0 ("none").
    SrsStore t;
    MemoryIo io2;
    t.begin(io2);
    t.setToday(30000);
    TEST_ASSERT_TRUE(t.syncWallClock(utcOf(2099, 12, 31, 12, 0, 0), 0).kind ==
                     ClockSync::Anchored);
    const int32_t shown = t.epochDayOf(t.today());
    TEST_ASSERT_EQUAL_INT32(30001 - shown, t.manualStepsMin());
    TEST_ASSERT_TRUE(t.setDateManual(t.manualStepsMin() - 1) == ManualDate::Rejected);
    TEST_ASSERT_TRUE(t.setDateManual(t.manualStepsMin()) == ManualDate::Reanchored);
    TEST_ASSERT_EQUAL_UINT16(1, t.epochAnchor());
    TEST_ASSERT_TRUE(t.hasWallClock());
    TEST_ASSERT_EQUAL_UINT16(30000, t.today());
}

// The anchor survives a save/load at byte 38; bytes 44..47 stay zero.
void test_serialize_anchor_roundtrip() {
    SrsStore a;
    MemoryIo empty;
    a.begin(empty);
    a.setToday(12);
    a.grade(0x65E5, Grade::Good, 12, 0, 0.5f);
    TEST_ASSERT_TRUE(a.syncWallClock(utcOf(2026, 11, 20, 13, 30, 60), 60).kind ==
                     ClockSync::Anchored);
    const uint16_t anchor = a.epochAnchor();
    TEST_ASSERT_EQUAL_UINT16(daysFromCivil(2026, 11, 20) - 12, anchor);

    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    TEST_ASSERT_EQUAL_UINT8(anchor & 0xFF, io.bytes[38]);  // LE
    TEST_ASSERT_EQUAL_UINT8(anchor >> 8, io.bytes[39]);
    for (size_t i = 44; i < 48; ++i) TEST_ASSERT_EQUAL_UINT8(0, io.bytes[i]);

    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(anchor, b.epochAnchor());
    TEST_ASSERT_TRUE(b.hasWallClock());
    TEST_ASSERT_EQUAL_UINT16(12, b.today());
    TEST_ASSERT_EQUAL_UINT32(a.msCarry(), b.msCarry());
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 11, 20), b.epochDayOf(b.today()));
    TEST_ASSERT_EQUAL_UINT16(1, b.streakCurrent());  // the neighbours are intact

    // A loaded anchor keeps working: the next day advances.
    TEST_ASSERT_TRUE(b.syncWallClock(utcOf(2026, 11, 21, 9, 0, 60), 60).kind ==
                     ClockSync::Advanced);
    TEST_ASSERT_EQUAL_UINT16(13, b.today());
}

// A file written before the anchor existed (byte 38 zero) loads unchanged: no
// anchor, the legacy date, and it serialises back to the very same bytes.
void test_serialize_pre_anchor_file_unchanged() {
    SrsStore a;
    MemoryIo empty;
    a.begin(empty);
    a.setToday(30);
    a.addElapsedMs(5 * kHourMs);
    for (uint16_t day = 28; day <= 30; ++day) a.grade(0x65E5, Grade::Good, day, 0, 0.5f);
    a.addReviewMs(4321);
    MemoryIo io;
    TEST_ASSERT_TRUE(a.save(io));
    TEST_ASSERT_EQUAL_UINT8(0, io.bytes[38]);
    TEST_ASSERT_EQUAL_UINT8(0, io.bytes[39]);
    const std::vector<uint8_t> old = io.bytes;

    SrsStore b;
    TEST_ASSERT_TRUE(b.begin(io));
    TEST_ASSERT_EQUAL_UINT16(0, b.epochAnchor());
    TEST_ASSERT_FALSE(b.hasWallClock());
    TEST_ASSERT_EQUAL_INT32(kSrsLegacyEpochDay + 30, b.epochDayOf(b.today()));
    TEST_ASSERT_EQUAL_UINT16(30, b.today());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(5 * kHourMs), b.msCarry());
    TEST_ASSERT_EQUAL_UINT16(3, b.streakCurrent());
    TEST_ASSERT_EQUAL_UINT32(4321, b.reviewMs());
    std::vector<uint8_t> again;
    TEST_ASSERT_TRUE(b.serialize(again));
    TEST_ASSERT_TRUE(again == old);
}

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_init_memory);
    RUN_TEST(test_retrievability);
    RUN_TEST(test_interval_at_90);
    RUN_TEST(test_success_growth);
    RUN_TEST(test_lapse);
    RUN_TEST(test_same_day);
    RUN_TEST(test_monotonic_difficulty);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_serialize_rejects);
    RUN_TEST(test_codepoint_identity);
    RUN_TEST(test_store_cap);
    RUN_TEST(test_stats);
    RUN_TEST(test_clock_boot_and_uptime);
    RUN_TEST(test_queue_due_before_new);
    RUN_TEST(test_queue_new_cap_resets_on_new_day);
    RUN_TEST(test_queue_new_cap_per_deck);
    RUN_TEST(test_queue_no_repeat_within_session);
    RUN_TEST(test_queue_relearn_requeues_and_caps);
    RUN_TEST(test_queue_interleaves_modes);
    RUN_TEST(test_queue_reading_needs_one_kind);
    RUN_TEST(test_streak);
    RUN_TEST(test_streak_first_answer_on_day_zero);
    RUN_TEST(test_review_ms_saturation);
    RUN_TEST(test_serialize_new_header_roundtrip);
    RUN_TEST(test_serialize_old_file_has_defaults);
    RUN_TEST(test_again_total);
    RUN_TEST(test_forget_set);
    RUN_TEST(test_forget_on_delete_keeps_shared);
    RUN_TEST(test_deck_reset_preview);
    RUN_TEST(test_reset_progress);
    RUN_TEST(test_forget_roundtrip);
    RUN_TEST(test_elapsed_sleep_time);
    RUN_TEST(test_civil_dates);
    RUN_TEST(test_wall_clock_plausibility);
    RUN_TEST(test_study_day_rollover_hour);
    RUN_TEST(test_sync_anchor_absent_then_present);
    RUN_TEST(test_sync_ahead_same_behind);
    RUN_TEST(test_sync_aligns_rollover_to_four);
    RUN_TEST(test_sync_monotonic);
    RUN_TEST(test_sync_counters);
    RUN_TEST(test_advance_days_manual);
    RUN_TEST(test_date_manual_forward_and_same);
    RUN_TEST(test_date_manual_back_reanchors);
    RUN_TEST(test_date_manual_back_needs_anchor);
    RUN_TEST(test_date_manual_floor);
    RUN_TEST(test_serialize_anchor_roundtrip);
    RUN_TEST(test_serialize_pre_anchor_file_unchanged);
    return UNITY_END();
}
