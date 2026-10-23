#pragma once
// Get decks (Settings > Decks): the release catalog over the home Wi-Fi and its
// downloads, and the updates list (only installed decks with a new build).
#include <cstdint>
#include <string>
#include <vector>

#include "catalog/Catalog.h"

namespace tgc {
namespace app {

// "Get decks" (roadmap phase 4): the last row of the deck list. Joins the saved
// home Wi-Fi on demand (HomeWifi), reads the release catalog over HTTPS and
// downloads the chosen deck into /packs (NetFetch). The radio is on only while
// this screen is, at kCpuWifiMhz. Connecting is polled from loop() so the side
// button can cancel; the catalog read and the download block, the latter with a
// progress callback that redraws, keeps the idle timers away and lets the side
// button cancel.
enum class NetStep { Connecting, Catalog, List, Error };
extern NetStep gNetStep;
extern bool gNetFromBoot;              // opened from the boot-time upload screen
extern bool gNetUpdatesOnly;           // the updates list
extern std::string gGetDecksLangHint;  // the phone's language from the portal page, or ""
extern std::vector<tgc::CatalogEntry> gCatalog;
extern int gNetSel;
extern char gNetNote[48];  // footer status line (cleared when the selection moves)

void drawGetDecks();
void openGetDecks(bool fromBoot, bool updatesOnly = false);
void netSyncClock();
void getDecksLoop(int a, int b);

}  // namespace app
}  // namespace tgc
