#pragma once
// The decks on the device: the /packs folder, the loaded deck (loading and
// replacing it) and its counts, and the deck list (Settings > Decks).
#include <Arduino.h>

#include <vector>

#include "ui/DeckSummary.h"
#include "ui/ListView.h"

namespace tgc {
namespace app {

extern std::vector<String> gPacks;  // paths of the *.tgc files in /packs
extern String gCurrentPack;         // the loaded deck's path, "" when none
extern tgc::ListView gDeckList;

void listPacks(std::vector<String>& out);
tgc::DeckSummary deckCounts();
String packFileStem(const String& path);
bool loadPack(const String& path);
bool closePackIfLoaded(const String& path);
bool portalPacksChanged();

void drawDecks();
void openDecks();
void decksLoop(int a, int b);

}  // namespace app
}  // namespace tgc
