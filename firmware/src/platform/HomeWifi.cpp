// Home Wi-Fi (station mode on demand); see HomeWifi.h. Arduino/ESP32 only.
#include "platform/HomeWifi.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <lwip/apps/sntp.h>
#include <sys/time.h>

#include <cctype>
#include <cstring>
#include <string>

#include "catalog/Catalog.h"

namespace tgc {

namespace {

constexpr const char* kNs = "net";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPass = "pass";
constexpr const char* kKeyCatalog = "cat";

// Any NTP answer earlier than this (2026-01-01T00:00:00Z) is not believed.
constexpr int64_t kMinPlausibleUtcMs = 1767225600000LL;

// Last disconnect reason reported by the Wi-Fi driver while joining. Written
// from the Wi-Fi event task, read from the loop task.
volatile uint8_t sLastReason = 0;

void onStaDisconnected(arduino_event_t* event) {
    const wifi_event_sta_disconnected_t& d = event->event_info.wifi_sta_disconnected;
    sLastReason = d.reason;
    // Which access point refused us, and why: with several under one name the
    // driver may try one, fail and settle on another.
    Serial.printf("[net] dropped by %02X:%02X:%02X:%02X:%02X:%02X, reason %u, rssi %d dBm\n",
                  d.bssid[0], d.bssid[1], d.bssid[2], d.bssid[3], d.bssid[4], d.bssid[5],
                  (unsigned)d.reason, (int)d.rssi);
}

// Wrong-password style reasons. A WPA2 network with a bad passphrase usually
// reports a 4-way handshake timeout rather than AUTH_FAIL.
bool isAuthReason(uint8_t r) {
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_802_1X_AUTH_FAILED ||
           r == WIFI_REASON_MIC_FAILURE;
}

bool passwordIsValid(const String& pass) {
    const size_t n = pass.length();
    if (n == 0) return true;  // open network
    if (n == 64) {            // raw PSK
        for (size_t i = 0; i < n; ++i)
            if (!isxdigit(static_cast<unsigned char>(pass[i]))) return false;
        return true;
    }
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; ++i) {  // WPA passphrases are printable ASCII
        const char c = pass[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// The saved network ("" when none). The namespace is opened read-write, as
// everywhere here: a read-only open of a missing namespace logs an error.
void loadCredentials(String& ssid, String& pass) {
    Preferences p;
    if (!p.begin(kNs, false)) return;
    if (p.isKey(kKeySsid)) ssid = p.getString(kKeySsid, "");
    if (p.isKey(kKeyPass)) pass = p.getString(kKeyPass, "");
    p.end();
}

// Overwrites a password before its String's buffer is freed.
void wipe(String& s) {
    if (s.length()) std::memset(const_cast<char*>(s.c_str()), 0, s.length());
}

}  // namespace

// --- saved settings ---------------------------------------------------------

// The namespace is opened read-write even to read it: a read-only open of a
// namespace that does not exist yet fails and logs an error on every call.

bool HomeWifi::hasCredentials() { return savedSsid().length() > 0; }

String HomeWifi::savedSsid() {
    Preferences p;
    if (!p.begin(kNs, false)) return String();
    const String s = p.isKey(kKeySsid) ? p.getString(kKeySsid, "") : String();
    p.end();
    return s;
}

bool HomeWifi::saveCredentials(const String& ssid, const String& pass) {
    if (ssid.isEmpty() || ssid.length() > kMaxSsidLen || !passwordIsValid(pass)) return false;
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    const bool ok = p.putString(kKeySsid, ssid) == ssid.length() &&
                    p.putString(kKeyPass, pass) == pass.length();
    p.end();
    Serial.printf("[net] home Wi-Fi %s\n", ok ? "saved" : "save failed");
    return ok;
}

void HomeWifi::forgetCredentials() {
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.remove(kKeySsid);
    p.remove(kKeyPass);
    p.end();
    Serial.printf("[net] home Wi-Fi forgotten\n");
}

String HomeWifi::catalogUrlOverride() {
    Preferences p;
    if (!p.begin(kNs, false)) return String();
    const String s = p.isKey(kKeyCatalog) ? p.getString(kKeyCatalog, "") : String();
    p.end();
    return s;
}

String HomeWifi::catalogUrl() {
    const String s = catalogUrlOverride();
    return s.length() ? s : String(kDefaultCatalogUrl);
}

bool HomeWifi::saveCatalogUrl(const String& url) {
    String u = url;
    u.trim();
    if (u.length() && (u.length() > kMaxUrlLen || !isHttpUrl(std::string(u.c_str()))))
        return false;
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    bool ok = true;
    if (u.isEmpty())
        p.remove(kKeyCatalog);
    else
        ok = p.putString(kKeyCatalog, u) == u.length();
    p.end();
    return ok;
}

// --- station mode -----------------------------------------------------------

bool HomeWifi::begin() {
    if (active()) return true;
    failure_ = Failure::None;
    String ssid, pass;
    loadCredentials(ssid, pass);
    wipe(pass);  // each join step reads it again (joinStep())
    if (ssid.isEmpty()) {
        state_ = State::Failed;
        failure_ = Failure::NoCredentials;
        return false;
    }
    snprintf(ssid_, sizeof ssid_, "%s", ssid.c_str());
    sLastReason = 0;

    // Keep the credentials in our own namespace only: with persistence on, the
    // driver would also copy them into its NVS area on every begin().
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_STA)) {
        WiFi.mode(WIFI_OFF);
        state_ = State::Failed;
        failure_ = Failure::Radio;
        return false;
    }
    eventId_ = static_cast<int>(WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED));
    // For the unpinned step: scan every channel and take the strongest, not
    // the first access point heard (the core's default fast scan).
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    state_ = State::Connecting;
    startedAt_ = millis();
    Serial.printf("[net] joining \"%s\"\n", ssid_);
    // A home network often has several access points under one name, and the
    // strongest may refuse a client's first tries (AUTH_EXPIRE for ~15-20 s;
    // band steering, the ESP32 being 2.4 GHz only). Left to itself the driver
    // then settles on a far one, where TLS times out (StickS3, 2026-10-19).
    // So: our own scan, then a join pinned to the strongest access point, whose
    // refusals the core retries on that access point only; any access point
    // after kPinnedJoinMs.
    if (WiFi.scanNetworks(true, false, false, kScanMsPerChannel, 0, ssid_) == WIFI_SCAN_FAILED)
        joinStep(false);
    else
        step_ = Step::Scan;
    stepAt_ = millis();
    return true;
}

void HomeWifi::pickAccessPoint(int16_t n) {
    static const char* const kAuth[] = {"open",     "wep",  "wpa",       "wpa2", "wpa/wpa2",
                                        "wpa2-ent", "wpa3", "wpa2/wpa3", "wapi"};
    int best = -1;
    for (int16_t i = 0; i < n; ++i) {
        if (WiFi.SSID(i) != ssid_) continue;
        const unsigned a = static_cast<unsigned>(WiFi.encryptionType(i));
        Serial.printf("[net] seen %s ch %d, %d dBm, %s\n", WiFi.BSSIDstr(i).c_str(),
                      (int)WiFi.channel(i), (int)WiFi.RSSI(i),
                      a < sizeof kAuth / sizeof kAuth[0] ? kAuth[a] : "?");
        if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best >= 0) {
        std::memcpy(bssid_, WiFi.BSSID(best), sizeof bssid_);
        channel_ = WiFi.channel(best);
    }
    WiFi.scanDelete();
    joinStep(best >= 0);
}

void HomeWifi::joinStep(bool pinned) {
    String ssid, pass;
    loadCredentials(ssid, pass);
    const char* pw = pass.length() ? pass.c_str() : nullptr;
    if (pinned)
        WiFi.begin(ssid_, pw, channel_, bssid_);
    else
        WiFi.begin(ssid_, pw);
    // Do not leave the password in this String's (soon freed) buffer.
    wipe(pass);
    step_ = pinned ? Step::Pinned : Step::Any;
    stepAt_ = millis();
}

HomeWifi::State HomeWifi::poll() {
    if (state_ != State::Connecting) return state_;
    if (step_ == Step::Scan) {
        const int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (millis() - stepAt_ < kScanTimeoutMs) return state_;
            esp_wifi_scan_stop();
        }
        pickAccessPoint(n);  // n < 0 (failed, too slow): any access point
    } else if (WiFi.status() == WL_CONNECTED) {
        state_ = State::Connected;
        Serial.printf("[net] connected to \"%s\" (%s, ch %d), ip %s, rssi %d dBm, %u ms\n", ssid_,
                      WiFi.BSSIDstr().c_str(), (int)WiFi.channel(),
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
                      (unsigned)(millis() - startedAt_));
        return state_;
    } else if (step_ == Step::Pinned && millis() - stepAt_ >= kPinnedJoinMs) {
        Serial.printf("[net] %02X:%02X:%02X:%02X:%02X:%02X did not take us in %u s, any access point now\n",
                      bssid_[0], bssid_[1], bssid_[2], bssid_[3], bssid_[4], bssid_[5],
                      (unsigned)(kPinnedJoinMs / 1000));
        joinStep(false);
    }
    if (millis() - startedAt_ >= kConnectTimeoutMs) {
        const uint8_t reason = sLastReason;
        failure_ = reason == WIFI_REASON_NO_AP_FOUND ? Failure::NotFound
                   : isAuthReason(reason)            ? Failure::AuthFailed
                                                     : Failure::Timeout;
        Serial.printf("[net] could not join \"%s\" (reason %u)\n", ssid_, (unsigned)reason);
        radioOff();
        state_ = State::Failed;
    }
    return state_;
}

void HomeWifi::end() {
    if (state_ == State::Off) return;
    radioOff();
    state_ = State::Off;
}

bool HomeWifi::linkUp() const { return state_ == State::Connected && WiFi.status() == WL_CONNECTED; }

String HomeWifi::localIp() const { return linkUp() ? WiFi.localIP().toString() : String(); }

int HomeWifi::rssi() const { return linkUp() ? static_cast<int>(WiFi.RSSI()) : 0; }

void HomeWifi::radioOff() {
    if (eventId_) {
        WiFi.removeEvent(static_cast<wifi_event_id_t>(eventId_));
        eventId_ = 0;
    }
    if (state_ == State::Connecting || state_ == State::Connected) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.printf("[net] radio off\n");
    }
}

bool HomeWifi::fetchUtcMs(int64_t* outUtcMs, uint32_t timeoutMs) {
    if (!outUtcMs || state_ != State::Connected) return false;
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    // UTC only: the time zone is the caller's business.
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    const uint32_t start = millis();
    bool synced = false;
    while (millis() - start < timeoutMs) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        delay(50);
    }
    if (sntp_enabled()) sntp_stop();  // one answer is all we need
    if (!synced) {
        Serial.printf("[net] NTP: no answer in %u ms\n", (unsigned)timeoutMs);
        return false;
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    if (ms < kMinPlausibleUtcMs) return false;
    *outUtcMs = ms;
    Serial.printf("[net] NTP: %lld ms since epoch\n", static_cast<long long>(ms));
    return true;
}

}  // namespace tgc
