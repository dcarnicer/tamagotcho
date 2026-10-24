#pragma once
// The app's shared context (docs/architecture.md, "Firmware app layer"): the
// open deck and its renderer, the SRS store and its queue, the settings
// namespace in NVS, the screen on top and the screen dispatch in main.cpp.
// Every app/ module includes it. Arduino only.
#include <Preferences.h>

#include <cstdint>
#include <memory>

#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/FsSrsFile.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"

// Set -DTGC_QUIZ_LOG to trace every question and answer over serial.
#if defined(TGC_QUIZ_LOG)
#define QLOG(...) Serial.printf(__VA_ARGS__)
#else
#define QLOG(...) do {} while (0)
#endif

namespace tgc {
namespace app {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;

// The open deck (closed when there is none), its file and its renderer.
extern std::unique_ptr<tgc::PackReader> gPack;
extern std::unique_ptr<tgc::FsPackSource> gSource;
extern tgc::Renderer gRender;
extern tgc::XorShiftRng gRng;

// The SRS store (/state/srs.bin) and the session's queue over the open deck.
extern tgc::SrsStore gSrs;
extern tgc::FsSrsFile gSrsIo;
extern tgc::Queue gQueue;
extern int gDirty;        // answers not yet persisted
extern bool gSrsLoaded;   // the state file loaded at boot

// Every screen. A new one gets a value here, a draw function and a loop
// function in its own app/ file, and a line in drawScreen() and handleInput()
// (main.cpp).
enum class Screen {
    Quiz, Menu, Decks, Choice, Done, Date, Stats, Detail, Wifi, GetDecks, UpdateCheck, WifiMenu,
    NetTest, About, FirstLang, DateMenu, TzList, Reset, ResetConfirm, Intro, Firmware
};
extern Screen gScreen;
extern Screen gBeforeMenu;  // where the settings home goes back to (closeMenu())
// The list screens' focus and scroll position are tgc::ListViews next to
// their draw functions (gMenuLists, gDeckList, gChoiceList, ...).

extern Preferences gPrefs;  // NVS namespace `tangocho`, opened in boot()

// main.cpp: draws the active screen; runs its input with this turn's button
// events (loop(), and keyAction() for a key).
void drawScreen();
void handleInput(int a, int b, uint32_t pressedAt, uint32_t t, bool down);

}  // namespace app
}  // namespace tgc
