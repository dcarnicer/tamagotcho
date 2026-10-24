#pragma once
// The decks on the device and on the microSD card: their list (both places,
// platform/Storage), the loaded deck (loading and replacing it, and losing it
// to a pulled card) and its counts, and the deck list (Settings > Decks).
#include <Arduino.h>

#include <vector>

#include "ui/DeckSummary.h"
#include "ui/ListView.h"

namespace tgc {
namespace app {

extern std::vector<String> gPacks;  // the deck list's paths, both places (listPacks())
extern String gCurrentPack;         // the loaded deck's path, "" when none
extern tgc::ListView gDeckList;

void listPacks(std::vector<String>& out);
String sameDeckIn(const std::vector<String>& packs, const String& path);
tgc::DeckSummary deckCounts();
String packFileStem(const String& path);
bool loadPack(const String& path);
bool closePackIfLoaded(const String& path);
bool portalPacksChanged();
void deckReadTick(uint32_t now);

void drawDecks();
void openDecks(bool mountCard = true);
void decksLoop(int a, int b);

}  // namespace app
}  // namespace tgc
