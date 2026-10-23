#pragma once
// The serial remote for tools/screenshots (TGC_REMOTE: the -shots envs, never a
// release): virtual presses and keys, screenshots and test hooks over USB
// serial. The protocol is described in Remote.cpp.
#include <cstdint>

#include "ui/KeyInput.h"

#if defined(TGC_REMOTE)
namespace tgc {
namespace app {

void remotePoll(uint32_t now);
bool remoteDown(int k, uint32_t now);
void remoteKeyEvent(int k, int event, uint32_t now);
bool remoteNextKey(tgc::KeyEvent* ev, uint32_t now);

}  // namespace app
}  // namespace tgc
#endif
