#pragma once
// Settings > Wi-Fi: the submenu (web portal, home network, setup mode, forget)
// and the home network's connection test.
#include <Arduino.h>

#include <cstdint>

#include "ui/ListView.h"

namespace tgc {
namespace app {

extern tgc::ListView gWifiMenuList;
extern String gWifiMenuSsid;    // the saved home network, read when the menu opens
extern char gWifiMenuNote[48];  // footer note (cleared when the selection moves)
// Settings > Wi-Fi > Home network: join, show the IP and the signal, read the
// catalog (and the network time), radio off again.
enum class NetTestStep : uint8_t { NoNetwork, Joining, Checking, Done, Failed };
extern NetTestStep gTestStep;

void drawWifiMenu();
void wifiMenuNote(const char* text, uint16_t color);
void openWifiMenu(bool fresh);
void wifiMenuLoop(int a, int b);

void drawNetTest();
void openNetTest();
void netTestLoop(int a, int b);

}  // namespace app
}  // namespace tgc
