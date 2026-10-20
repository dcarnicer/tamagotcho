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
    // changes this URL, so older firmware keeps the decks it can open. The file
    // is named after the catalog format this firmware reads (catalog-v2.txt,
    // kCatalogVersion), independently of the pack format in the tag.
    static constexpr const char* kDefaultCatalogUrl =
        "https://github.com/dcarnicer/tangocho/releases/download/decks-v2/catalog-v2.txt";
    // A join: a scan for the network (kScanMsPerChannel a channel, at most
    // kScanTimeoutMs), then kPinnedJoinMs on its strongest access point, which
    // may refuse a client's first tries for ~15-20 s (docs/decisions.md,
    // 2026-10-19), then any access point until kConnectTimeoutMs.
    static constexpr uint32_t kScanMsPerChannel = 120;
    static constexpr uint32_t kScanTimeoutMs = 5000;
    static constexpr uint32_t kPinnedJoinMs = 25000;
    static constexpr uint32_t kConnectTimeoutMs = 40000;
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

    // While Connected: whether the link is up right now (the driver reconnects
    // by itself after a drop), the station IP ("" while down) and the signal.
    bool linkUp() const;
    String localIp() const;
    int rssi() const;

    // NTP (roadmap phase 2, "real date from NTP"). Blocking: needs Connected,
    // runs SNTP for at most `timeoutMs` and stops it again. On success stores the
    // UTC time in milliseconds since 1970 and returns true. The app turns it into
    // a study day with the saved UTC offset (applyWallClock in main.cpp).
    bool fetchUtcMs(int64_t* outUtcMs, uint32_t timeoutMs);

#if defined(TGC_REMOTE)
    // Test hook for the serial remote (main.cpp; never in a release): the next
    // begin() pins its join to an access point that does not exist, for the
    // pinned step only (BadPin: any access point after kPinnedJoinMs) or for
    // the whole join (NoJoin: it fails after kConnectTimeoutMs).
    enum class TestFault : uint8_t { None, BadPin, NoJoin };
    static void setTestFault(TestFault f);
#endif

private:
    // The steps of a join (begin(), poll()).
    enum class Step : uint8_t { Scan, Pinned, Any };

    void pickAccessPoint(int16_t scanCount);  // after the scan: pinned or any
    void joinStep(bool pinned);
    void radioOff();

    State state_ = State::Off;
    Failure failure_ = Failure::None;
    Step step_ = Step::Scan;
    uint32_t startedAt_ = 0;
    uint32_t stepAt_ = 0;
    uint8_t bssid_[6] = {0};  // the strongest access point seen by the scan
    int32_t channel_ = 0;
    char ssid_[kMaxSsidLen + 1] = {0};
    int eventId_ = 0;  // Wi-Fi disconnect-event handler while joining
#if defined(TGC_REMOTE)
    TestFault fault_ = TestFault::None;  // this join's (setTestFault())
#endif
};

}  // namespace tgc
