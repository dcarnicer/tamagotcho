#pragma once
// Input timing preferences (docs/ux-refresh.md §6, items 3 and 7): the stored
// choice, its default and the duration it stands for. Settings › Study ›
// Auto-advance and Settings › Device › Hold time store the index in NVS
// (`tangocho`/kPrefAdvance, kPrefHold). Pure C++, no Arduino (native-tested in
// test_ui).
#include <cstdint>

namespace tgc {

// Auto-advance after a correct answer. A wrong answer never auto-advances.
constexpr uint8_t kAdvanceOff = 0;
constexpr uint8_t kAdvanceShort = 1;
constexpr uint8_t kAdvanceNormal = 2;
constexpr uint8_t kAdvanceChoices = 3;
constexpr uint8_t kAdvanceDefault = kAdvanceNormal;

// Milliseconds from the reveal to the next card; 0 = wait for a press. An
// unknown stored value reads as the default.
inline uint32_t advanceMsFor(uint8_t pref) {
    switch (pref) {
        case kAdvanceOff: return 0;
        case kAdvanceShort: return 600;
        case kAdvanceNormal: return 1000;
        default: return advanceMsFor(kAdvanceDefault);
    }
}

// Hold threshold of both buttons (a hold answers in the quiz and activates in
// the menus). Grading uses the press time, so it does not depend on this.
constexpr uint8_t kHoldShort = 0;
constexpr uint8_t kHoldNormal = 1;
constexpr uint8_t kHoldLong = 2;
constexpr uint8_t kHoldChoices = 3;
constexpr uint8_t kHoldDefault = kHoldNormal;

// Milliseconds a press must last to count as a hold. An unknown stored value
// reads as the default.
inline uint32_t holdMsFor(uint8_t pref) {
    switch (pref) {
        case kHoldShort: return 400;
        case kHoldNormal: return 600;
        case kHoldLong: return 900;
        default: return holdMsFor(kHoldDefault);
    }
}

constexpr char kPrefAdvance[] = "adv";
constexpr char kPrefHold[] = "hold";

}  // namespace tgc
