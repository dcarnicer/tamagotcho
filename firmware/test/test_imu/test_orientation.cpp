// Native tests for the auto-rotation filter in imu/OrientationFilter.{h,cpp}.
// Samples are built from an "up" direction in the screen plane (degrees
// clockwise from the panel's top edge, so rotation r is r * 90) and a tilt of
// the screen from the horizontal (90 = upright, 0 = lying face up).
#include <unity.h>

#include <cmath>
#include <cstdint>

#include "imu/OrientationFilter.h"

using tgc::OrientationConfig;
using tgc::OrientationFilter;

namespace {

constexpr int kNone = OrientationFilter::kNone;
constexpr uint32_t kStep = 100;  // the app's polling period

struct G {
    float x, y, z;
};

G pose(float upDeg, float tiltDeg = 90.0f, float g = 1.0f) {
    const float r = upDeg * 3.14159265f / 180.0f;
    const float t = tiltDeg * 3.14159265f / 180.0f;
    return {g * std::sin(r) * std::sin(t), g * std::cos(r) * std::sin(t), g * std::cos(t)};
}

// Feeds `pose` every kStep ms for `ms` (inclusive of both ends) starting at *t,
// leaves *t on the next free slot and returns the last result.
int hold(OrientationFilter& f, uint32_t* t, const G& p, uint32_t ms) {
    int r = kNone;
    const uint32_t end = *t + ms;
    for (; *t <= end; *t += kStep) r = f.update(p.x, p.y, p.z, *t);
    return r;
}

int feed(OrientationFilter& f, uint32_t t, const G& p) { return f.update(p.x, p.y, p.z, t); }

OrientationConfig allFour() {
    OrientationConfig c;
    c.allowed = 0b1111;
    return c;
}

}  // namespace

// Each rotation from clean samples, both one-shot and through the filter.
void test_each_orientation_clean() {
    OrientationFilter f;
    TEST_ASSERT_EQUAL_INT(0, f.classify(0.0f, 1.0f, 0.0f));   // top edge up
    TEST_ASSERT_EQUAL_INT(1, f.classify(1.0f, 0.0f, 0.0f));   // right edge up
    TEST_ASSERT_EQUAL_INT(3, f.classify(-1.0f, 0.0f, 0.0f));  // left edge up
    // Upside down is not offered by default...
    TEST_ASSERT_EQUAL_INT(kNone, f.classify(0.0f, -1.0f, 0.0f));
    // ...but the filter supports it when allowed.
    OrientationFilter all(allFour());
    TEST_ASSERT_EQUAL_INT(2, all.classify(0.0f, -1.0f, 0.0f));
    // Held in the hand, screen tilted back 45 degrees, still clear.
    TEST_ASSERT_EQUAL_INT(1, f.classify(pose(90, 45).x, pose(90, 45).y, pose(90, 45).z));

    uint32_t t = 0;
    f.reset(0);
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(90), 600));
    TEST_ASSERT_EQUAL_INT(3, hold(f, &t, pose(-90), 600));
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(0), 600));
    // Upside down keeps whatever was showing.
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(180), 2000));
    f.reset(1);
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(180), 2000));

    all.reset(0);
    TEST_ASSERT_EQUAL_INT(2, hold(all, &t, pose(180), 600));
}

// Noise around the 45-degree boundary must not flip-flop; the switch needs the
// pose to be 15 degrees past the boundary, and the way back too.
void test_hysteresis_near_boundary() {
    OrientationFilter f;
    f.reset(0);
    uint32_t t = 0;
    for (int i = 0; i < 60; ++i, t += kStep)
        TEST_ASSERT_EQUAL_INT(0, feed(f, t, pose(i % 2 ? 40.0f : 50.0f)));
    // Inside the dead band (30..60) for a long time: no change.
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(58), 3000));
    // Clearly past it: switches after the settle time.
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(65), 600));
    // Back across the boundary, still within the dead band: stays landscape.
    for (int i = 0; i < 60; ++i, t += kStep)
        TEST_ASSERT_EQUAL_INT(1, feed(f, t, pose(i % 2 ? 40.0f : 50.0f)));
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(32), 3000));
    // Clearly portrait again.
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(25), 600));
}

// A brief tilt does not switch; a held one switches only after settleMs.
void test_settle_time() {
    OrientationFilter f;
    f.reset(0);
    uint32_t t = 1000;
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90), 400));  // 0..400 ms: too short
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(0), 300));
    // Held: nothing before 500 ms, the switch at 500 ms.
    const uint32_t start = t;
    for (; t < start + 500; t += kStep) TEST_ASSERT_EQUAL_INT(0, feed(f, t, pose(90)));
    TEST_ASSERT_EQUAL_INT(1, feed(f, t, pose(90)));
    // A single interruption restarts the full settle time.
    t += kStep;
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(-90), 400));
    TEST_ASSERT_EQUAL_INT(1, feed(f, t, pose(-45)));  // on a boundary: no vote
    t += kStep;
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(-90), 400));
    TEST_ASSERT_EQUAL_INT(3, hold(f, &t, pose(-90), 100));
}

// Lying flat (face up or down, or nearly) never votes, whatever the small
// in-plane component says, and interrupts a pending change.
void test_flat_pose_ignored() {
    OrientationFilter f;
    f.reset(0);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 0), 3000));     // face up
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 15), 3000));    // nearly flat
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 180), 3000));   // face down
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 170), 3000));   // nearly, face down
    TEST_ASSERT_EQUAL_INT(kNone, f.classify(0.0f, 0.1f, 0.99f));
    // A flat moment in the middle of a settle restarts it.
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90), 400));
    TEST_ASSERT_EQUAL_INT(0, feed(f, t, pose(90, 0)));
    t += kStep;
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90), 400));
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(90), 100));
    // Tilted back to 30 degrees (above the 25-degree floor) still counts.
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(0, 30), 600));
}

// Samples whose magnitude is far from 1 g are motion, not gravity.
void test_shaking_ignored() {
    OrientationFilter f;
    f.reset(0);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 90, 1.6f), 3000));
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 90, 0.4f), 3000));
    TEST_ASSERT_EQUAL_INT(0, hold(f, &t, pose(90, 90, 0.0f), 3000));  // free fall / no data
    TEST_ASSERT_EQUAL_INT(kNone, f.classify(NAN, 1.0f, 0.0f));
    // Shaking interleaved with real samples keeps restarting the settle time.
    for (int i = 0; i < 40; ++i, t += kStep)
        TEST_ASSERT_EQUAL_INT(0, feed(f, t, i % 3 == 2 ? pose(90, 90, 2.0f) : pose(90)));
    // Mild acceleration (walking) is tolerated.
    TEST_ASSERT_EQUAL_INT(1, hold(f, &t, pose(90, 90, 1.2f), 600));
}

// No history: nothing is reported until a clear pose has settled; flat or
// boundary samples at startup keep it unknown.
void test_startup_no_history() {
    OrientationFilter f;
    TEST_ASSERT_EQUAL_INT(kNone, f.current());
    uint32_t t = 0;
    TEST_ASSERT_EQUAL_INT(kNone, hold(f, &t, pose(0, 0), 2000));  // flat on a table
    for (int i = 0; i < 30; ++i, t += kStep)                      // right on 45 degrees
        TEST_ASSERT_EQUAL_INT(kNone, feed(f, t, pose(i % 2 ? 44.0f : 46.0f)));
    TEST_ASSERT_EQUAL_INT(kNone, hold(f, &t, pose(-80), 400));
    TEST_ASSERT_EQUAL_INT(3, hold(f, &t, pose(-80), 100));
    // reset() forgets everything again.
    f.reset();
    TEST_ASSERT_EQUAL_INT(kNone, f.current());
    TEST_ASSERT_EQUAL_INT(kNone, feed(f, t, pose(0)));
    // classify() needs no history at all.
    const G p = pose(-80);
    TEST_ASSERT_EQUAL_INT(3, f.classify(p.x, p.y, p.z));
}

// A long gap between samples (polling paused: panel off, Auto off) does not
// count as having held the pose.
void test_gap_restarts_settle() {
    OrientationFilter f;
    f.reset(0);
    TEST_ASSERT_EQUAL_INT(0, feed(f, 0, pose(90)));
    TEST_ASSERT_EQUAL_INT(0, feed(f, 5000, pose(90)));
    TEST_ASSERT_EQUAL_INT(0, feed(f, 5400, pose(90)));
    TEST_ASSERT_EQUAL_INT(1, feed(f, 5500, pose(90)));
}

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_each_orientation_clean);
    RUN_TEST(test_hysteresis_near_boundary);
    RUN_TEST(test_settle_time);
    RUN_TEST(test_flat_pose_ignored);
    RUN_TEST(test_shaking_ignored);
    RUN_TEST(test_startup_no_history);
    RUN_TEST(test_gap_restarts_settle);
    return UNITY_END();
}
