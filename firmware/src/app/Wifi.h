#pragma once
// The Wi-Fi screen and the on-device portal (platform/WifiPortal.h): home mode
// over the saved home network, setup mode on the device's own access point,
// the boot-time upload screen of a device with no deck, and the hooks through
// which the portal page changes the app (decks, progress, the clock).
#include <cstdint>

#include "platform/WifiPortal.h"

namespace tgc {
namespace app {

extern tgc::WifiPortal gPortal;
extern bool gWifiBootUpload;  // the boot-time "no packs yet" screen (see Wifi.cpp)
// The portal's two modes (WifiPortal.h): home = the page over the saved home
// network, which the screen joins first (gHome); setup = the device's own open
// access point. Home is the default once a network is saved.
enum class PortalMode : uint8_t { Setup, Home };
// The Wi-Fi screen: joining the home network, serving the page, or a failed
// join (or access point) with its reason and the way to setup mode.
enum class WifiStep : uint8_t { Joining, Serving, Failed };
extern PortalMode gPortalMode;
extern WifiStep gWifiStep;

void drawWifi();
PortalMode defaultPortalMode();
void openPortal(PortalMode mode);
void startQuizFromPortal();
void wifiLoop(int a, int b);

}  // namespace app
}  // namespace tgc
