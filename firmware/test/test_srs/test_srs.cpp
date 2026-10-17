// Native tests for the pure FSRS-5 math in srs/Srs.{h,cpp}. See docs/srs.md
// section 8. Host and ESP32 floats may differ in the last bits, so every
// comparison uses an absolute tolerance.
#include <unity.h>

#include <cmath>

#include "srs/Srs.h"

using namespace tgc;

namespace {

// A representative mid-life memory used by the transition tests.
Memory mid() {
    Memory m;
    m.s = 10.0f;
    m.d = 5.0f;
    return m;
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
    return UNITY_END();
}
