#pragma once
// The "All caught up" screen (docs/ux-refresh.md, done mockup): nothing is due
// and no new card is left today; "+10 new" studies more.
#include <cstdint>

namespace tgc {
namespace app {

uint32_t currentStreak();
void drawDone();
void showDone();
void doneLoop(int a, int b);

}  // namespace app
}  // namespace tgc
