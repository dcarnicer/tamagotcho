#pragma once
// The user's home Wi-Fi (station mode), joined on demand for "Get decks" and
// for the network time after a power cut. Never left on: the app calls begin()
// when a network feature starts and end() as soon as it is done, so the battery
// policy holds (the CPU runs at 240 MHz only while the radio is up).
// Arduino/ESP32 only; not part of the native build.
//
// Settings live in NVS namespace "net": `ssid`, `pass` and the optional catalog
// URL override `cat`. They are written from the Wi-Fi portal page and read
// here. The password is never logged, never sent back to a page or JSON, and
// never returned by this API (savedSsid() is the only thing a page may show).
// NVS is not encrypted on this device: anyone holding it with a USB cable can
// read the flash, which is the usual trade-off for a stored Wi-Fi password.
#include <Arduino.h>

#include <cstdint>

namespace tgc {

class HomeWifi {
public:
    // The decks release for the pack format this firmware reads: a fixed tag
    // `decks-v<format version>`, never marked "latest" and refreshed in place by
    // .github/workflows/decks.yml. A format bump publishes `decks-v<N+1>` and
    // changes this URL, so older firmware keeps the decks it can open. Answers
    // 404 until that release exists.
    static constexpr const char* kDefaultCatalogUrl =
        "https://github.com/dcarnicer/tangocho/releases/download/decks-v2/catalog.txt";
    static constexpr uint32_t kConnectTimeoutMs = 15000;
    static constexpr size_t kMaxSsidLen = 32;   // 802.11 limit, in bytes
    static constexpr size_t kMaxUrlLen = 256;

    // --- saved settings (NVS `net`) -------------------------------------------
    static bool hasCredentials();
    static String savedSsid();  // "" when none is saved
    // Saves the network. False (nothing stored) for an empty or over-long SSID,
    // or a password that is neither empty (open network), 8-63 characters
    // (WPA passphrase) nor 64 hex digits (raw PSK).
    static bool saveCredentials(const String& ssid, const String& pass);
    static void forgetCredentials();
    // The catalog URL to use: the saved override, else kDefaultCatalogUrl.
    static String catalogUrl();
    static String catalogUrlOverride();  // "" when the default is in use
    // "" restores the default; otherwise an http(s):// URL of at most
    // kMaxUrlLen characters. False (nothing stored) for anything else.
    static bool saveCatalogUrl(const String& url);

    // --- station mode, on demand ----------------------------------------------
    enum class State : uint8_t { Off, Connecting, Connected, Failed };
    enum class Failure : uint8_t { None, NoCredentials, NotFound, AuthFailed, Timeout, Radio };

    HomeWifi() = default;
    ~HomeWifi() { end(); }
    HomeWifi(const HomeWifi&) = delete;
    HomeWifi& operator=(const HomeWifi&) = delete;

    // Starts joining the saved network (non-blocking). False, with state()
    // Failed, when there are no credentials or the radio would not start.
    bool begin();
    // Call every loop() iteration while Connecting: notices the connection, or
    // gives up after kConnectTimeoutMs (the radio is then already off).
    State poll();
    // Disconnects and turns the radio off. Safe to call in any state.
    void end();

    State state() const { return state_; }
    // True while the radio is (or may be) on: Connecting or Connected.
    bool active() const { return state_ == State::Connecting || state_ == State::Connected; }
    Failure failure() const { return failure_; }
    const char* ssid() const { return ssid_; }  // the network being joined

    // NTP (roadmap phase 2, "real date from NTP"). Blocking: needs Connected,
    // runs SNTP for at most `timeoutMs` and stops it again. On success stores the
    // UTC time in milliseconds since 1970 and returns true. The app turns it into
    // a study day with the saved UTC offset (applyWallClock in main.cpp).
    bool fetchUtcMs(int64_t* outUtcMs, uint32_t timeoutMs);

private:
    void radioOff();

    State state_ = State::Off;
    Failure failure_ = Failure::None;
    uint32_t startedAt_ = 0;
    char ssid_[kMaxSsidLen + 1] = {0};
    int eventId_ = 0;  // Wi-Fi disconnect-event handler while joining
};

}  // namespace tgc
