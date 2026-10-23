#pragma once
// Deck update checks (docs/deck-updates.md): the count of installed decks with
// a newer build in the catalog, the policy of the automatic check and its
// boot-time screen.
#include <cstdint>
#include <vector>

#include "catalog/Catalog.h"

namespace tgc {
namespace app {

// Deck update checks (docs/deck-updates.md): the boot-time catalog read and
// the count the Decks rows show. `gUpdateDay` is the epoch day of the last
// completed check (success or attempt; 0 = never); `gUpdateKnown` says whether
// a catalog read has ever succeeded; the count is only a badge, because the
// updates list reads the catalog again. The automatic check runs once a day on
// external power, or once a week on battery from 50 %.
extern uint8_t gUpdateCount;  // installed decks with an update (NVS `updcount`)
extern uint32_t gUpdateDay;   // epoch day of the last check (NVS `updday`)
extern bool gUpdateKnown;     // a catalog read succeeded (NVS `updok`)
constexpr const char* kPrefUpdateDay = "updday";
constexpr const char* kPrefUpdateCount = "updcount";
constexpr const char* kPrefUpdateKnown = "updok";

void saveUpdateState();
void setUpdatesFromCatalog(const std::vector<tgc::CatalogEntry>& entries,
                           const std::vector<tgc::DeckState>* states = nullptr);
bool updateCheckDue();
void openUpdateCheck();
void startUpdateCheckFromDate();
void updateCheckTick(uint32_t now);
void updateCheckInput(int a, int b);
void drawUpdateCheck();

}  // namespace app
}  // namespace tgc
