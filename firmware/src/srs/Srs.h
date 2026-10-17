#pragma once
// FSRS-5 memory model: stability, difficulty and the interval. Pure math, no
// Arduino. The design and the formulas are in docs/srs.md (section 3).
#include <cstdint>

namespace tgc {

// Grade from the quiz outcome: Again on a miss, Hard on a slow correct, Good on
// a fast correct (docs/srs.md section 3). The numeric values are the FSRS
// grade indices used by the formulas.
enum class Grade : uint8_t { Again = 1, Hard = 2, Good = 3 };

// Per-card memory state: stability S in days (how long 90% retention lasts)
// and difficulty D in [1, 10] (how hard the card is).
struct Memory {
    float s = 0;  // stability, days
    float d = 0;  // difficulty
};

// FSRS-5 default weights (w[0..18], docs/srs.md section 3). w[3] (Easy initial
// stability) and w[16] (Easy bonus) belong to the unused Easy grade but stay in
// the table so the parameter set is recognisable.
constexpr float kWeights[19] = {
    0.40255f, 1.18385f, 3.173f,  15.69105f, 7.1949f,  0.5345f, 1.4604f,
    0.0046f,  1.54575f, 0.1192f, 1.01925f,  1.9395f,  0.11f,   0.29605f,
    2.2698f,  0.2315f,  2.9898f, 0.51655f,  0.6621f};

// F = 19/81: the decay constant at the 0.9 desired retention. With this value
// r^(-2) - 1 = F, so the interval factors cancel to I = roundf(S).
constexpr float kFactor = 19.0f / 81.0f;

// Stability clamps. kSMax is a deliberate shrink of FSRS's 36500 days: the
// interval is capped at 365 anyway and 500 fits the u16 centiday record.
constexpr float S_MIN = 0.01f;
constexpr float S_MAX = 500.0f;

// Interval cap and fuzz band (uniform u in [kFuzzMin, kFuzzMax]).
constexpr int kMaxIntervalDays = 365;
constexpr float kFuzzMin = 0.95f;
constexpr float kFuzzMax = 1.05f;

// First review (reps == 0): S = w[G-1],
// D = clamp(w[4] - expf(w[5] * (G - 1)) + 1, 1, 10).
Memory initMemory(Grade g);

// Probability of recalling a card of stability `stabilityDays` after
// `elapsedDays`: (1 + F * t / S)^-0.5. t == 0 gives 1.
float retrievability(float stabilityDays, float elapsedDays);

// FSRS-5 state transition. `elapsedDays == 0` is the in-session same-day step
// (w[17]/w[18]); a lapse (Again) is capped at the old S; Hard applies the w[15]
// penalty; D mean-reverts toward the Easy initial difficulty D0(4).
Memory nextMemory(Memory m, Grade g, float elapsedDays);

// Interval in days at 0.9 retention: roundf(S) clamped to [1, 365], then fuzzed
// for I >= 2 by roundf(I * u), u = kFuzzMin + (kFuzzMax-kFuzzMin) * uniform01.
// `uniform01` is a caller-drawn value in [0, 1) so srs/ need not depend on quiz/.
int intervalDays(float stabilityDays, float uniform01);

}  // namespace tgc
