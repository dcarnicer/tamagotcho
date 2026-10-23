#pragma once
// The detail card (docs/ux-refresh.md §4 phase 2): after a wrong answer, the
// card's pick, accuracy, meanings and readings, and its stroke order animating.
#include <cstdint>

namespace tgc {
namespace app {

void showDetail();
void drawDetail();
void detailLoop(int a, int b, uint32_t t);

}  // namespace app
}  // namespace tgc
