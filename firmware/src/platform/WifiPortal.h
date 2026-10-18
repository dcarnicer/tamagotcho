#pragma once
// On-device Wi-Fi access point + HTTP portal (roadmap phase 4): upload, delete
// and download packs under /packs and read the SRS statistics from a phone or
// laptop -- no computer running `uploadfs`. Opening the page also hands the
// browser's clock to the app, which is how the device learns the real date.
// The page also stores the home Wi-Fi network and the deck catalog URL that
// "Get decks" uses (HomeWifi.h); the password is write-only from the page's
// point of view. Arduino/ESP32 only; not part of the native build.
//
// Security: the AP is intentionally open (no password) so joining it is easy.
// Anyone in radio range can list, upload or delete packs, read the stats and
// replace the saved home Wi-Fi or catalog URL (but never read the password).
// The portal is meant to be started for a moment and stopped again, not left
// running on an untrusted network.
//
// The class is deliberately decoupled from main.cpp: the app hands over a small
// Hooks struct (statistics, a "packs changed" callback and a sink for the
// browser's clock) and the portal never reaches into the app's globals.
#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>
#include <vector>

namespace tgc {

// Read-only view of the app state the portal displays. The app fills this; the
// portal only reads it while building a page.
struct PortalStats {
    uint32_t day = 0;
    char date[12] = "";       // "YYYY-MM-DD" of `day` (docs/srs.md section 5.1)
    bool dateSynced = false;  // the date comes from a real clock, not the fixed base
    uint32_t seen = 0, learned = 0, answers = 0, reps = 0, lapses = 0;
    uint32_t streakCurrent = 0, streakBest = 0;
    uint32_t reviewMs = 0;
    uint32_t packCards = 0;  // cards in the loaded pack, 0 when none is open
};

class WifiPortal {
public:
    // Hooks into the app. Only the portal calls these, and only while active.
    struct Hooks {
        // Fills the counters shown on `/` and `/api/stats`. Must be cheap.
        void (*fillStats)(PortalStats& out) = nullptr;
        // Called after /packs changed (upload, delete or a finished download).
        // The app rescans the folder and loads a pack if needed. Returns true
        // when a usable pack is loaded afterwards (used to leave the boot-time
        // "no packs yet" screen).
        bool (*onPacksChanged)() = nullptr;
        // Called with the browser's clock, which the page posts on every load
        // (POST /time): `utcMs` is Date.now(), `tzOffsetMin` the offset in
        // minutes east of UTC (-getTimezoneOffset()). The app checks that the
        // time is plausible and applies it; the portal never touches the SRS.
        // Returns true when the time was accepted.
        bool (*onTime)(int64_t utcMs, int16_t tzOffsetMin) = nullptr;
    };

    WifiPortal() = default;
    ~WifiPortal() { end(); }
    WifiPortal(const WifiPortal&) = delete;
    WifiPortal& operator=(const WifiPortal&) = delete;

    // Starts the AP and the HTTP server. False if the AP could not be started;
    // lastMessage() then says why. A no-op while already active.
    bool begin(const Hooks& hooks);
    // Serves pending requests and drives a URL download. Call every loop()
    // iteration while on the Wi-Fi screen; a download may block briefly.
    void poll();
    // Stops the server and the AP and frees the state. Safe to call when idle.
    void end();

    bool active() const { return active_; }
    const char* ssid() const { return ssid_; }
    const char* url() const { return kUrl; }
    size_t uploadedPacks() const { return uploaded_; }  // this session
    const char* lastMessage() const { return msg_; }
    // millis() of the last HTTP request (or of begin()), for the idle timeout.
    uint32_t lastActivityMs() const { return lastActivity_; }

    // True once after anything that changes what the on-device screen shows
    // (upload finished, delete, download progress). Reading clears it.
    bool consumeDirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    static constexpr const char* kUrl = "http://192.168.4.1";
    static constexpr const char* kPacksDir = "/packs";
    // A pack larger than this is refused outright: the StickS3 filesystem is
    // ~5.9 MB and the portal keeps a reserve free for /state and temp files.
    static constexpr size_t kMaxPackBytes = 5u * 1024u * 1024u;
    // Uploads stream here first and are renamed over the destination only after
    // the whole body arrived and validated, so a dropped/oversized/invalid
    // upload can never destroy the pack it was replacing. Hidden name, so the
    // pack listers skip it.
    static constexpr const char* kUploadTmp = "/packs/.upload.tmp";

private:
    void registerRoutes();
    void handleRoot();
    void handleApiStats();
    void handleDelete();
    void handleFetchRequest();
    void handleUploadChunk();
    void handleUploadDone();
    void handleTime();
    void handleNotFound();
    void handleHomeWifi();
    void handleForgetWifi();
    void handleCatalogUrl();
    // Remembers the nearby network names (strongest first) for the page's SSID
    // suggestions. Runs once in begin(), before the AP starts.
    void scanNearby();

    // Blocking download of `url` into /packs; true on success.
    bool doFetch(const String& url);
    // First 4 bytes are "TNGC" and a cheap PackReader probe succeeds.
    bool validatePackFile(const String& path) const;
    void abortUpload(const char* why);
    // printf into msg_ and mark the screen dirty.
    void setMsg(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void touch() { lastActivity_ = millis(); }

    WebServer* server_ = nullptr;
    bool active_ = false;
    Hooks hooks_{};
    char ssid_[20] = {0};
    char msg_[80] = "ready";
    bool dirty_ = false;
    size_t uploaded_ = 0;  // packs received this session
    uint32_t lastActivity_ = 0;

    // Upload state, valid between UPLOAD_FILE_START and UPLOAD_FILE_END. The
    // body goes to kUploadTmp (never the destination); only the validated temp
    // is renamed over the destination.
    File uploadFile_;
    String uploadName_;
    String uploadError_;
    bool uploadOk_ = false;
    size_t uploadWritten_ = 0;
    size_t uploadFree_ = 0;

    // Fetch state. A /fetch request is staged here and runs from poll() so the
    // on-device screen can paint "downloading..." before the loop blocks.
    String fetchPending_;
    String fetchUrl_;

    std::vector<String> nearby_;  // SSIDs seen by scanNearby()
};

}  // namespace tgc
