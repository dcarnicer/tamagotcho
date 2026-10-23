#pragma once
// Input beyond the buttons' tap and hold: the hold progress (the fill under a
// held press, docs/ux-refresh.md §2.5) and the keyboard, whose key actions
// keyAction() turns into each screen's own (docs/decisions.md, 2026-10-21).
#include <cstdint>

#include "ui/KeyInput.h"

namespace tgc {
namespace app {

extern tgc::KeyRepeat gKeyRepeat;  // the held arrow of a keyboard (keyTurn())

bool holdTick(uint32_t now, bool down, bool longFired, uint32_t downAt, int event,
              uint32_t holdMs);
void keyTurn(uint32_t t, bool swallow);

}  // namespace app
}  // namespace tgc
