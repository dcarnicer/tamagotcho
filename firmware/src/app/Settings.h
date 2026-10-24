#pragma once
// The settings tree (docs/ux-refresh.md §3, §6.6): the home with its summary
// card and five groups, the Study and Device pages, the table that gives each
// ui/SettingsTree.h item its label, value and action; and the settings'
// values.
#include <cstdint>

#include "quiz/Quiz.h"
#include "srs/Queue.h"
#include "ui/InputPrefs.h"
#include "ui/ListView.h"
#include "ui/SettingsTree.h"

namespace tgc {
namespace app {

namespace st = tgc::settings;

// The settings' values (NVS `tangocho`, read in boot()).
constexpr uint8_t kBrightnessDefault = 128;
extern uint8_t gBrightness;   // NVS `tangocho`/`bri`
extern uint8_t gAdvancePref;  // NVS `tangocho`/`adv` (ui/InputPrefs.h)
extern uint8_t gHoldPref;     // NVS `tangocho`/`hold`
extern tgc::OnKana gOnKana;   // NVS `tangocho`/`onkana` (0 katakana, 1 hiragana)
// Study > Introduce (docs/learning-mode.md §3.8): learning mode, an intro
// card before a new kanji's first question. NVS `tangocho`/`intro` (1 on, the
// default; 0 off), global.
extern bool gIntroOn;
constexpr const char* kPrefIntro = "intro";
// The queue's learning options for gIntroOn (Queue::setLearning()).
tgc::LearnOptions learnOptions();

// The page on screen, and its note (after Delete progress) until the next press.
extern st::Page gMenuPage;
extern char gMenuNote[32];
tgc::ListView& menuList();

void drawMenu();
void openMenu();
void backToMenu();
void menuLoop(int a, int b);

}  // namespace app
}  // namespace tgc
