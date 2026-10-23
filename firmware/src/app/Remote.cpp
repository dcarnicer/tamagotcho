// The serial remote; see Remote.h.
#include "app/Remote.h"

#if defined(TGC_REMOTE)
#include <Arduino.h>

#include "app/App.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Display.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/Wifi.h"
#include "platform/HomeWifi.h"

namespace tgc {
namespace app {

namespace {

// Serial remote for tools/screenshots (envs sticks3-shots, tdisplay-s3-shots;
// never a release).
// One byte a command over USB serial, read by loop():
//   'a' / 'b'  tap the front / side button (down for 80 ms)
//   'A' / 'B'  hold it (down until the hold fires, then up)
//   's'        print the back buffer, which is what the panel shows: "#shot W H
//              <on|dim|dark>" (the backlight; dark: the last frame drawn), one
//              "#r Y <hex>" line a row (RGB565 as stored: big-endian, four hex
//              digits a pixel), "#end"
//   'w'        wake: counts as activity and lights a dim or dark panel, no press
// Test hooks:
//   'd'        the boot-time date question as after a power cut: "Getting the
//              date via <ssid>..." first, then the stepper (with the reason when
//              the network failed). It writes nothing itself: a hold on the
//              stepper confirms (saved), a side hold cancels, and a network time
//              that answers is applied as by every sync (a same-day one writes
//              nothing)
//   'f'        the next home Wi-Fi join is pinned to an access point that does
//              not exist: the pinned step fails, any access point after 25 s
//   'x'        the next join is pinned there for good: it fails after 40 s ("No
//              network: set it by hand" on the date question)
// Keys (a board with a keyboard, cardputer-adv-shots; two bytes, 'k' and the
// key): 'u' 'd' 'l' 'r' the arrows, 'e' Enter, 'x' Esc, 'm' the menu key (Tab),
// 'b' Backspace (del); 'U' 'D' 'L' 'R' hold an arrow for kRemoteKeyHoldMs (its
// auto-repeat). A key goes down, and up 80 ms later, through the same path as
// the keyboard's (keyTurn()). An unknown key is ignored.
// A press is a virtual button that goes through Key like a real one: it counts
// as activity, a dim panel brightens and acts, a dark one only wakes. While a
// virtual button or key is down the next bytes wait, so a burst runs in order.
struct RemoteKey {
    uint32_t upAt = 0;     // millis() when a tap comes up; 0: up
    bool holding = false;  // a hold that has not fired yet
};
RemoteKey gRemote[2];

// The remote's key: down (`sent` once delivered), up at `upAt`.
constexpr uint32_t kRemoteKeyTapMs = 80;
constexpr uint32_t kRemoteKeyHoldMs = 1000;
struct RemoteKeyPress {
    tgc::Key key = tgc::Key::None;
    bool sent = false;
    uint32_t upAt = 0;
};
RemoteKeyPress gRemoteKey;
bool gRemoteKeyPrefix = false;  // a 'k' came: the next byte names the key

bool remoteBusy() {
    return gRemote[0].upAt || gRemote[0].holding || gRemote[1].upAt || gRemote[1].holding ||
           gRemoteKey.key != tgc::Key::None;
}

// The key a 'k' command names; `held`: an upper-case arrow.
tgc::Key remoteKeyFor(int c, bool* held) {
    using tgc::Key;
    *held = c == 'U' || c == 'D' || c == 'L' || c == 'R';
    switch (c) {
        case 'u': case 'U': return Key::Up;
        case 'd': case 'D': return Key::Down;
        case 'l': case 'L': return Key::Left;
        case 'r': case 'R': return Key::Right;
        case 'e': return Key::Enter;
        case 'x': return Key::Esc;
        case 'm': return Key::Tab;
        case 'b': return Key::Backspace;
        default: return Key::None;
    }
}

}  // namespace

// keyTurn()'s source after the board's: the remote key's press, then (once
// its time is up) its release.
bool remoteNextKey(tgc::KeyEvent* ev, uint32_t now) {
    RemoteKeyPress& k = gRemoteKey;
    if (k.key == tgc::Key::None) return false;
    if (!k.sent) {
        k.sent = true;
        *ev = tgc::KeyEvent{k.key, true};
        return true;
    }
    if (static_cast<int32_t>(now - k.upAt) < 0) return false;
    *ev = tgc::KeyEvent{k.key, false};
    k = RemoteKeyPress{};
    return true;
}

bool remoteDown(int k, uint32_t now) {
    RemoteKey& r = gRemote[k];
    if (r.holding) return true;
    if (!r.upAt) return false;
    if (static_cast<int32_t>(now - r.upAt) < 0) return true;
    r.upAt = 0;  // seen up once by Key: the press ends here
    return false;
}

// After Key::poll(): a remote hold lets go shortly after its hold fired, so a
// slow loop() turn can never make it a tap.
void remoteKeyEvent(int k, int event, uint32_t now) {
    if (gRemote[k].holding && event == 2) {
        gRemote[k].holding = false;
        gRemote[k].upAt = now + 50;
    }
}

namespace {

void remoteShot() {
    static const char kHex[] = "0123456789abcdef";
    static char line[16 + 4 * 320 + 2];  // up to 320 pixels a row (T-Display-S3)
    const int w = gCanvas->width(), h = gCanvas->height();
    const uint8_t* px = static_cast<const uint8_t*>(gCanvas->getBuffer());
    Serial.printf("\n#shot %d %d %s\n", w, h, gScreenOff ? "dark" : gDimmed ? "dim" : "on");
    for (int y = 0; y < h && w <= 320 && px; ++y) {
        int n = snprintf(line, sizeof line, "#r %d ", y);
        const uint8_t* row = px + static_cast<size_t>(y) * w * 2;
        for (int i = 0; i < w * 2; ++i) {
            line[n++] = kHex[row[i] >> 4];
            line[n++] = kHex[row[i] & 15];
        }
        line[n++] = '\n';
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
    Serial.printf("#end\n");
}

// 'd': see above. Not while a radio is in use (the portal, Get decks, the
// network test) or the question is already open.
void remoteDateQuestion() {
    if (gScreen == Screen::Date || gHome.active() || gPortal.active() || !gPack->isOpen()) {
        Serial.printf("remote: no date question now (screen %d, radio %d)\n",
                      static_cast<int>(gScreen), gHome.active() || gPortal.active() ? 1 : 0);
        return;
    }
    Serial.printf("remote: the date question, as after a power cut\n");
    gAdvance.cancel();
    dateSyncBegin();
    openDate(Screen::Quiz);
}

}  // namespace

void remotePoll(uint32_t now) {
    while (!remoteBusy() && Serial.available() > 0) {
        const int c = Serial.read();
        if (gRemoteKeyPrefix) {
            gRemoteKeyPrefix = false;
            bool held = false;
            const tgc::Key key = remoteKeyFor(c, &held);
            if (key == tgc::Key::None) {
                Serial.printf("remote: no key '%c'\n", c);
                continue;
            }
            gRemoteKey.key = key;
            gRemoteKey.sent = false;
            gRemoteKey.upAt = now + (held ? kRemoteKeyHoldMs : kRemoteKeyTapMs);
        } else if (c == 'k') {
            gRemoteKeyPrefix = true;
        } else if (c == 'a' || c == 'b') {
            gRemote[c == 'b'].upAt = now + 80;
        } else if (c == 'A' || c == 'B') {
            gRemote[c == 'B'].holding = true;
        } else if (c == 's') {
            remoteShot();
        } else if (c == 'w') {
            gLastInputAt = now;
            if (gScreenOff || gDimmed) wakeScreen();
        } else if (c == 'd') {
            remoteDateQuestion();
        } else if (c == 'u') {
            // The next update check is due: clear the stored day. Test only.
            gUpdateDay = 0;
            gPrefs.putUInt(kPrefUpdateDay, 0);
            Serial.println("remote: the next update check is due");
        } else if (c == 'f' || c == 'x') {
            tgc::HomeWifi::setTestFault(c == 'f' ? tgc::HomeWifi::TestFault::BadPin
                                                 : tgc::HomeWifi::TestFault::NoJoin);
            Serial.printf("remote: the next join %s\n",
                          c == 'f' ? "fails its pinned step" : "fails");
        }
    }
}

}  // namespace app
}  // namespace tgc
#endif
