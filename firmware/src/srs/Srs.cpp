// FSRS-5 memory model. Design: docs/srs.md section 3.
#include "srs/Srs.h"

#include <cmath>

namespace tgc {
namespace {

// D0(G) = w[4] - expf(w[5] * (G - 1)) + 1: the initial difficulty for a grade.
// G is 1..4 (1 = Again .. 4 = Easy); only 1..3 are ever used, 4 gives D0(4).
inline float initialDifficulty(float g) {
    return kWeights[4] - expf(kWeights[5] * (g - 1.0f)) + 1.0f;
}

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace

Memory initMemory(Grade g) {
    const int gi = static_cast<int>(g);
    const float gf = static_cast<float>(gi);
    Memory m;
    m.s = kWeights[gi - 1];
    m.d = clampf(initialDifficulty(gf), 1.0f, 10.0f);
    return m;
}

float retrievability(float stabilityDays, float elapsedDays) {
    if (elapsedDays <= 0.0f) return 1.0f;
    if (stabilityDays <= 0.0f) return 0.0f;
    return 1.0f / sqrtf(1.0f + kFactor * elapsedDays / stabilityDays);
}

Memory nextMemory(Memory m, Grade g, float elapsedDays) {
    const int gi = static_cast<int>(g);
    const float gf = static_cast<float>(gi);
    const float r = retrievability(m.s, elapsedDays);

    float sp;
    if (elapsedDays == 0.0f) {
        // Same day (in-session relearn): only w[17]/w[18] matter.
        sp = m.s * expf(kWeights[17] * (gf - 3.0f + kWeights[18]));
    } else if (g == Grade::Again) {
        // Lapse.
        sp = kWeights[11] * powf(m.d, -kWeights[12]) *
             (powf(m.s + 1.0f, kWeights[13]) - 1.0f) * expf((1.0f - r) * kWeights[14]);
        if (sp > m.s) sp = m.s;  // a miss never increases stability
    } else {
        // Hard / Good: the w[15] penalty applies to Hard only.
        const float hard = (g == Grade::Hard) ? kWeights[15] : 1.0f;
        sp = m.s * (1.0f + expf(kWeights[8]) * (11.0f - m.d) * powf(m.s, -kWeights[9]) *
                              (expf((1.0f - r) * kWeights[10]) - 1.0f) * hard);
    }

    const float deltaD = -kWeights[6] * (gf - 3.0f);
    const float dCand = clampf(m.d + deltaD * (10.0f - m.d) / 9.0f, 1.0f, 10.0f);
    const float d0Easy = initialDifficulty(4.0f);

    Memory out;
    out.s = clampf(sp, S_MIN, S_MAX);
    out.d = clampf(kWeights[7] * d0Easy + (1.0f - kWeights[7]) * dCand, 1.0f, 10.0f);
    return out;
}

int intervalDays(float stabilityDays, float uniform01) {
    float i = roundf(stabilityDays);
    if (i < 1.0f) i = 1.0f;
    if (i > static_cast<float>(kMaxIntervalDays)) i = static_cast<float>(kMaxIntervalDays);
    int days = static_cast<int>(i);

    if (days >= 2) {
        float u01 = uniform01;
        if (u01 < 0.0f) u01 = 0.0f;
        if (u01 > 1.0f) u01 = 1.0f;
        const float u = kFuzzMin + (kFuzzMax - kFuzzMin) * u01;
        days = static_cast<int>(roundf(static_cast<float>(days) * u));
        if (days < 1) days = 1;
        if (days > kMaxIntervalDays) days = kMaxIntervalDays;
    }
    return days;
}

}  // namespace tgc
