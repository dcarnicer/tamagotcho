// Home Wi-Fi (station mode on demand); see HomeWifi.h. Arduino/ESP32 only.
#include "platform/HomeWifi.h"

#include <Preferences.h>
#include <WiFi.h>
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
    sLastReason = event->event_info.wifi_sta_disconnected.reason;
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
    {
        Preferences p;
        if (p.begin(kNs, false)) {
            if (p.isKey(kKeySsid)) ssid = p.getString(kKeySsid, "");
            if (p.isKey(kKeyPass)) pass = p.getString(kKeyPass, "");
            p.end();
        }
    }
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
    WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
    // Do not leave the password in this String's (soon freed) buffer.
    if (pass.length()) std::memset(const_cast<char*>(pass.c_str()), 0, pass.length());
    state_ = State::Connecting;
    startedAt_ = millis();
    Serial.printf("[net] joining \"%s\"\n", ssid_);
    return true;
}

HomeWifi::State HomeWifi::poll() {
    if (state_ != State::Connecting) return state_;
    if (WiFi.status() == WL_CONNECTED) {
        state_ = State::Connected;
        Serial.printf("[net] connected to \"%s\", ip %s, rssi %d dBm, %u ms\n", ssid_,
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
                      (unsigned)(millis() - startedAt_));
        return state_;
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
