#pragma once
// On-device Wi-Fi access point + HTTP portal (roadmap phase 4): upload, delete
// and download packs under /packs and read the SRS statistics from a phone or
// laptop -- no computer running `uploadfs`. Arduino/ESP32 only; not part of the
// native build.
//
// Security: the AP is intentionally open (no password) so joining it is easy.
// Anyone in radio range can list, upload or delete packs and read the stats.
// The portal is meant to be started for a moment and stopped again, not left
// running on an untrusted network.
//
// The class is deliberately decoupled from main.cpp: the app hands over a small
// Hooks struct (statistics + a "packs changed" callback) and the portal never
// reaches into the app's globals.
#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>

namespace tgc {

// Read-only view of the app state the portal displays. The app fills this; the
// portal only reads it while building a page.
struct PortalStats {
    uint32_t day = 0;
    uint32_t seen = 0, learned = 0, answers = 0, reps = 0, lapses = 0;
    uint32_t streakCurrent = 0, streakBest = 0;
    uint32_t reviewMs = 0;
    uint32_t packCards = 0;          // cards in the loaded pack, 0 when none is open
    const char* packName = nullptr;  // loaded deck name, may be null
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

private:
    void registerRoutes();
    void handleRoot();
    void handleApiStats();
    void handleDelete();
    void handleFetchRequest();
    void handleUploadChunk();
    void handleUploadDone();
    void handleNotFound();

    // Blocking download of `url` into /packs; true on success.
    bool doFetch(const String& url);
    // First 4 bytes are "TNGC" and a cheap PackReader probe succeeds.
    bool validatePackFile(const String& path) const;
    void abortUpload(const char* why);
    // printf into msg_ and mark the screen dirty.
    void setMsg(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    WebServer* server_ = nullptr;
    bool active_ = false;
    Hooks hooks_{};
    char ssid_[20] = {0};
    char msg_[80] = "ready";
    bool dirty_ = false;
    size_t uploaded_ = 0;  // packs received this session

    // Upload state, valid between UPLOAD_FILE_START and UPLOAD_FILE_END.
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
};

}  // namespace tgc
