#pragma once
// Firmware updates over the air (docs/ota.md): Settings > Device > Firmware
// (the running version against the one the last manifest read found, Install
// with a hold, Check now), the install and the restart into the new image;
// the manifest read that every home-Wi-Fi session reading the deck catalog
// adds (§3.3) and the badge it leaves (the Device and Firmware rows); the
// first boot of a new image, marked valid once it has shown it works (§3.5),
// and the notice after a rollback.
#include <cstddef>
#include <cstdint>

#include "platform/NetFetch.h"
#include "ui/ListView.h"

namespace tgc {
namespace app {

// The longest version kept (NVS strings, the badge): a manifest's vX.Y.Z is at
// most 33 characters, a build's at most 40 (scripts/version.py).
constexpr size_t kFwVersionMax = 40;
// The badge: a newer firmware the last manifest read offered, "" for none
// (NVS `fwver`; its size in `fwsize`). A failed read leaves it as it was.
extern char gFwBadge[kFwVersionMax + 1];

// boot(), once the settings namespace is open: the layout, a pending first
// boot, the outcome of the last install (a rollback is remembered, NVS
// `otabad`, and not offered again) and the badge.
void firmwareBoot();
// boot(), once the deck (and so the UI language) is known: the notice after a
// rollback, once, for a few seconds or until a press.
void firmwareRollbackNotice();
// loop(), every turn: a pending image is marked valid after 10 s of the loop.
void firmwareTick(uint32_t now);
// Before every deep sleep of the running app: a pending image that reached the
// loop counts as good.
void firmwareBeforeSleep();
// A fatal screen of the boot (halt(), the damaged-storage question): a pending
// image rolls back to the previous one at once instead of waiting there (a
// deep sleep would wake into it again). Returns when nothing is pending.
void firmwareFailedBoot();

// A home-Wi-Fi session has just read the deck catalog (the boot check, the date
// sync, Get decks, the network test, the portal page): read the firmware
// manifest on the same connection and refresh the badge. Silent (serial log
// only); skipped when the catalog read failed to reach any server, when the
// layout has no update slot and for a development build without a manifest
// URL of its own.
void firmwareAfterCatalog(const tgc::FetchResult& catalog);

// The badge: the home's Device row carries the update mark while an update is
// known, and the Device page's Firmware row reads "v0.7.0 available" (else the
// running version).
bool firmwareUpdateKnown();
void firmwareRowValue(char* out, size_t n);
// What the portal page shows: the newer version ("" for none) and whether this
// device updates itself at all.
const char* firmwareAvailable();
bool firmwareOtaLayout();

extern tgc::ListView gFwList;
void openFirmware();
void drawFirmware();
void firmwareLoop(int a, int b);
// A check or an install is under way (the menu key does not leave then).
bool firmwareBusy();

}  // namespace app
}  // namespace tgc
