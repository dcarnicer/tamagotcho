#include "imu/OrientationFilter.h"

#include <cmath>

namespace tgc {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;
}  // namespace

OrientationFilter::OrientationFilter(const OrientationConfig& cfg)
    : cfg_(cfg), minPlaneRatio_(std::sin(cfg.minTiltDeg / kRadToDeg)) {}

void OrientationFilter::reset(int current) {
    current_ = (current >= 0 && current <= 3) ? current : kNone;
    pending_ = kNone;
    haveSample_ = false;
}

bool OrientationFilter::upAngle(float x, float y, float z, float* deg) const {
    const float norm = std::sqrt(x * x + y * y + z * z);
    // Written so that NaN samples fail too.
    if (!(norm >= 1.0f - cfg_.motionToleranceG && norm <= 1.0f + cfg_.motionToleranceG))
        return false;
    const float plane = std::sqrt(x * x + y * y);
    if (plane < norm * minPlaneRatio_) return false;  // face up or face down
    *deg = std::atan2(x, y) * kRadToDeg;
    return true;
}

int OrientationFilter::sector(float deg, float halfWidth) const {
    const long k = std::lround(deg / 90.0f);  // -2..2
    const float off = deg - static_cast<float>(k) * 90.0f;
    if (std::fabs(off) > halfWidth) return kNone;
    const int r = static_cast<int>((k + 4) % 4);
    return (cfg_.allowed & (1u << r)) ? r : kNone;
}

int OrientationFilter::classify(float x, float y, float z) const {
    float deg;
    if (!upAngle(x, y, z, &deg)) return kNone;
    return sector(deg, 45.0f);
}

int OrientationFilter::update(float x, float y, float z, uint32_t tMs) {
    const bool gap = haveSample_ && tMs - lastSampleAt_ > cfg_.maxGapMs;
    haveSample_ = true;
    lastSampleAt_ = tMs;

    float deg;
    if (!upAngle(x, y, z, &deg)) {
        pending_ = kNone;  // a flat or moving moment restarts the settle time
        return current_;
    }
    int cand;
    if (current_ == kNone) {
        cand = sector(deg, 45.0f);  // nothing to protect: plain sectors
    } else {
        cand = sector(deg, 45.0f - cfg_.hysteresisDeg);
        if (cand == kNone) cand = current_;  // dead band or a sector not allowed
    }
    if (cand == kNone || cand == current_) {
        pending_ = kNone;
        return current_;
    }
    if (cand != pending_ || gap) {
        pending_ = cand;
        pendingSince_ = tMs;
    }
    if (tMs - pendingSince_ >= cfg_.settleMs) {
        current_ = cand;
        pending_ = kNone;
    }
    return current_;
}

}  // namespace tgc
