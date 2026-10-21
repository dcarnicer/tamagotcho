#pragma once
// Whether the board has PSRAM in the heap. Not psramFound(): in this Arduino
// core (2.0.x, PSRAM initialised at boot) that reads true even when the init
// failed, as it does for the M5 image's octal-PSRAM build on the Cardputer ADV
// (board/BoardM5.cpp).
#include <esp_heap_caps.h>

namespace tgc {

inline bool hasPsram() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

}  // namespace tgc
