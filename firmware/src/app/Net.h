#pragma once
// What Get decks, the portal's home mode, the network test, the boot-time date,
// the deck update check and the firmware install share over the home Wi-Fi:
// the station (gHome) and the radio off, why a join failed, a catalog deck's
// state against the installed decks, why a catalog read failed, the progress of
// a blocking transfer, and the checked deck download.
#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "catalog/Catalog.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/WifiPortal.h"

namespace tgc {
namespace app {

extern tgc::HomeWifi gHome;
// A blocking transfer in progress: a deck download (Get decks and the Wi-Fi
// screen show it) or a firmware install.
extern bool gNetBusy;
extern int gNetPct;
extern char gNetDeck[72];
extern uint32_t gNetActivityAt;  // millis() of the last network progress (idle timers)
// A failure's title and detail: the Get decks error, the Wi-Fi screen's failed state.
extern char gNetErrTitle[48];
extern char gNetErrDetail[96];

void netRadioOff();
const char* joinFailureText();
void formatMb(size_t bytes, bool unit, char* out, size_t n);
String catalogFile(const tgc::CatalogEntry& e);  // "<name>.tgc"
String catalogPath(const tgc::CatalogEntry& e);  // its installed copy, "" when none
// Hashing cost of one comparison pass, logged once per pass rather than per deck.
struct HashStats {
    uint32_t ms = 0;
    uint16_t files = 0;
};

tgc::DeckState deckStateOnDisk(const tgc::CatalogEntry& e, HashStats* stats = nullptr);
bool catalogFailure(const tgc::CatalogFetch& c, const String& url, char* title, size_t tn,
                    char* detail, size_t dn);

// A blocking transfer over the home Wi-Fi (a deck, a firmware image): TLS stalls
// are longer than on the local portal, and a big one may take minutes.
constexpr uint32_t kNetIdleMs = 15000;
constexpr uint32_t kNetDownloadMs = 10u * 60u * 1000u;
// Its progress: netTransferBegin() before (gNetBusy, a first frame), netProgress()
// as the FetchProgressFn, netTransferEnd() after it returned, then
// netSleepIfAsked(), which sleeps when the power button or a critical battery
// cancelled it (the transfer cleaned up first).
void netTransferBegin();
bool netProgress(size_t done, size_t total, void*);
void netTransferEnd();
void netSleepIfAsked();
tgc::DeckDownloadResult downloadCatalogDeck(const tgc::CatalogEntry& e);

}  // namespace app
}  // namespace tgc
