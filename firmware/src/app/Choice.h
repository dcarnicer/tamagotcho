#pragma once
// The pickers (Screen::Choice): one screen for every setting that is a choice
// among a few values, and the Forget Wi-Fi confirmation.
#include <cstddef>
#include <cstdint>

#include "ui/ListView.h"

namespace tgc {
namespace app {

// Screen::Choice is shared by the row pickers (see openChoice()).
enum class Choice {
    NewPerDay, SlowMs, Orientation, UiLang, Brightness, ForgetWifi, AutoAdvance, HoldTime, OnKana,
    Introduce
};
extern tgc::ListView gChoiceList;

void hardAfterText(uint16_t ms, char* out, size_t n);
void drawChoice();
void openChoice(Choice which);
void choiceLoop(int a, int b);

}  // namespace app
}  // namespace tgc
