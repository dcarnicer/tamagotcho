#pragma once
// The intro card of learning mode (docs/learning-mode.md §3.2, §3.3, §3.11):
// before a never-seen kanji's first question, its stroke order, meanings and
// readings, and with a v3 pack its example words (under the readings on the
// T-Display-S3's portrait, on a second page elsewhere). A front press goes on
// (the card enters its learning steps), a side tap says "known" (the card is
// asked at once, as without learning mode), a side hold opens the settings.
#include <cstdint>

namespace tgc {
namespace app {

// Makes the intro of `cardIndex` the item on screen (nextItem()): copies its
// card and example words and starts its stroke animation. Nothing is drawn.
// False when the card cannot be read.
bool prepareIntro(uint32_t cardIndex);
void drawIntro();
// loop() on the intro (handleInput()): `down`, a button is down now.
void introLoop(int a, int b, uint32_t t, bool down);
// The intro is on screen again after the settings: presses are guarded anew.
void introReshown();

}  // namespace app
}  // namespace tgc
