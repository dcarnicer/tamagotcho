#pragma once
// Statistics (docs/ux-refresh.md, stats mockup): the accuracy ring, six tiles
// and the loaded deck's coverage. Read-only.

namespace tgc {
namespace app {

void drawStats();
void openStats();
void statsLoop(int b);

}  // namespace app
}  // namespace tgc
