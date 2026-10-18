#pragma once
// Turns accelerometer samples into a stable display rotation for the "Auto"
// orientation setting. Pure C++ (no Arduino), unit tested natively in
// test/test_imu. Thresholds and the choice of rotations: docs/decisions.md
// (2026-10-18, auto-rotation).
//
// Frame: samples are in g in the *panel frame* of the board HAL
// (board::readAccel): +x toward the panel's right edge, +y toward its top edge,
// +z out of the screen, all as seen in rotation 0 (native portrait). At rest the
// accelerometer reads +1 g along the axis that points up, so "up" within the
// screen plane is the direction of (x, y).
//
// Rotations are LovyanGFX's setRotation() values: r = 0, 1, 2, 3 puts the top of
// the picture at the panel's top, right, bottom or left edge, so the picture is
// upright when that edge points up. The direction of (x, y), measured clockwise
// from +y, is therefore r * 90 degrees for an upright picture.
#include <cstdint>

namespace tgc {

struct OrientationConfig {
    // Bit r set: rotation r may be chosen. The default leaves out 2 (portrait
    // upside down): the poses that produce it are usually transient.
    uint8_t allowed = 0b1011;
    // A rotation other than the current one is taken only once the pose is
    // within (45 - hysteresisDeg) degrees of its centre. Each 45-degree boundary
    // thus has a dead band of +-hysteresisDeg where nothing changes.
    float hysteresisDeg = 15.0f;
    // Below this angle between the screen and the horizontal the device counts
    // as lying flat (face up or down): the in-plane direction means nothing.
    float minTiltDeg = 25.0f;
    // |a| outside 1 g +- this is motion (shaking, a tap, free fall), not
    // gravity: the sample is ignored.
    float motionToleranceG = 0.3f;
    // A new rotation must be seen continuously for this long before it applies.
    uint32_t settleMs = 500;
    // Samples further apart than this restart the settle time (polling paused).
    uint32_t maxGapMs = 1000;
};

class OrientationFilter {
public:
    static constexpr int kNone = -1;

    explicit OrientationFilter(const OrientationConfig& cfg = OrientationConfig());

    // Sets the current rotation (kNone: no history) and drops any pending change.
    void reset(int current = kNone);

    // Feeds one sample (g, panel frame) taken at `tMs`. Returns the stable
    // rotation 0-3: the current one until a different allowed pose has been held
    // for settleMs. kNone while there is no history and no settled pose yet.
    // Flat poses and strong motion never vote and restart a pending change.
    int update(float x, float y, float z, uint32_t tMs);

    int current() const { return current_; }

    // One-shot pick without history, hysteresis or settle time: the allowed
    // rotation whose 90-degree sector holds the sample, or kNone when the pose is
    // flat, moving or in a sector that is not allowed. For the first frame at boot.
    int classify(float x, float y, float z) const;

private:
    // Screen-plane direction of "up" in degrees clockwise from +y, (-180, 180];
    // false for a flat pose or a sample that is not gravity.
    bool upAngle(float x, float y, float z, float* deg) const;
    // The allowed rotation whose centre is within `halfWidth` degrees, or kNone.
    int sector(float deg, float halfWidth) const;

    OrientationConfig cfg_;
    float minPlaneRatio_;  // sin(minTiltDeg)
    int current_ = kNone;
    int pending_ = kNone;
    uint32_t pendingSince_ = 0;
    uint32_t lastSampleAt_ = 0;
    bool haveSample_ = false;
};

}  // namespace tgc
