#pragma once
// The boot, which setup() runs: the board and the settings, the wake after a
// critical-battery sleep, the filesystem (formatted on a fresh device, erased
// on request when damaged), the deck, the SRS state and the time slept, then
// the first screen: the first-boot language picker, the Wi-Fi upload screen of
// a device with no deck, the power-cut date question, the deck update check or
// the quiz.
#include "ui/ListView.h"

namespace tgc {
namespace app {

void boot();

// The first-boot language picker.
extern tgc::ListView gFirstLangList;
void drawFirstLang();
void firstLangLoop(int a);

}  // namespace app
}  // namespace tgc
