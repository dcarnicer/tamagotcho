#pragma once
// On-device Wi-Fi portal (roadmap phase 4): one web page to get decks onto the
// device, upload, delete and download packs under /packs, save the home Wi-Fi
// and read the SRS statistics from a phone or laptop -- no computer running
// `uploadfs`. Opening the page also hands the browser's clock to the app, which
// is how the device learns the real date. Arduino/ESP32 only; not part of the
// native build.
//
// Two modes, the same page:
//   - Setup mode: the device's own open access point "Tangocho-XXXX" at
//     http://192.168.4.1/, for the first setup (no home Wi-Fi saved yet) or on
//     request. Its page saves the home Wi-Fi and then offers to switch to home
//     mode (Hooks::onSwitchToHome) or to run Get decks on the device screen.
//   - Home mode: the app has joined the saved home network (HomeWifi) and the
//     page is served on the station IP, so the phone stays on its own network.
//     The page then lists the deck catalog itself and downloads the chosen deck
//     through the app (Hooks::onDeckDownload), with a progress view.
// The page is in the device's UI language, or the browser's while the device
// has none (portal/PortalLogic.h); its strings are in portal/PortalText.
//
// Security: neither mode has a password. In setup mode anyone in radio range,
// in home mode anyone on the home network, can list, upload or delete packs,
// read the stats and replace the saved home Wi-Fi or catalog URL (but never read
// the password) while the Wi-Fi screen is open. The portal is meant to be
// started for a moment and stopped again; the idle timeout stops it too.
//
// The class is deliberately decoupled from main.cpp: the app hands over a small
// Hooks struct and the portal never reaches into the app's globals.
#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>
#include <vector>

#include "catalog/Catalog.h"
#include "platform/NetFetch.h"

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
    uint32_t packCards = 0;   // cards in the loaded pack, 0 when none is open
    char loadedPack[49] = ""; // file name of the loaded pack ("n5-en.tgc"), "" when none
};

// What became of a deck download the page asked for (Hooks::onDeckDownload).
struct DeckDownloadResult {
    FetchResult fetch;
    bool batteryLow = false;  // refused before it started: the battery is too low
};

class WifiPortal {
public:
    enum class Mode : uint8_t { Setup, Home };

    // Hooks into the app. Only the portal calls these, and only while active.
    struct Hooks {
        // Fills the counters shown on `/` and `/api/stats`. Must be cheap.
        void (*fillStats)(PortalStats& out) = nullptr;
        // Called after /packs changed (upload, delete or a finished URL
        // download). The app rescans the folder and loads a pack if needed.
        // Returns true when a usable pack is loaded afterwards.
        bool (*onPacksChanged)() = nullptr;
        // Called with the browser's clock, which the page posts on every load
        // (POST /time): `utcMs` is Date.now(), `tzOffsetMin` the offset in
        // minutes east of UTC (-getTimezoneOffset()), `tzRule` the browser's
        // zone as a POSIX TZ rule and `zone` its IANA name (each "" when the
        // page sent none or it failed the character check). The app checks that
        // the time and the rule are plausible and applies them; the portal
        // never touches the SRS. Returns true when the time was accepted.
        bool (*onTime)(int64_t utcMs, int16_t tzOffsetMin, const char* tzRule,
                       const char* zone) = nullptr;
        // The page's "Get decks on the device screen" (POST /getdecks, offered
        // in setup mode once a home Wi-Fi is saved): the device should stop the
        // portal and open Get decks over the home Wi-Fi. Called from poll(),
        // once the response page has had time to reach the phone; the app
        // normally calls end() from it, so poll() touches nothing afterwards.
        // `phoneLang` is the phone's language tag (navigator.language, e.g.
        // "es-ES"; letters, digits and '-' only) or "" when the page sent none.
        void (*onGetDecks)(const char* phoneLang) = nullptr;
        // The device's UI language for the page: "en" or "es", or "" when it has
        // none of its own (the Language setting is Auto and no deck is loaded);
        // the page then follows the browser. Null counts as "".
        const char* (*uiLang)() = nullptr;
        // "Switch to my Wi-Fi" (setup mode) and "Reconnect" (home mode), POST
        // /home, offered once a home Wi-Fi is saved: the app should end this
        // portal and open it again in home mode on the saved network. Called
        // from poll() like onGetDecks, after the response page went out.
        void (*onSwitchToHome)() = nullptr;
        // Home mode: download catalog deck `entry` into /packs (blocking; called
        // from poll() a moment after the page asked, POST /deck). `update` is
        // true when a different build of it is on the device. The app runs the
        // checked NetFetch download, shows the progress on its screen, calls
        // serviceDownload() from its progress callback so the page can follow,
        // and rescans /packs. Null hides the page's download buttons.
        DeckDownloadResult (*onDeckDownload)(const CatalogEntry& entry, bool update) = nullptr;
    };

    WifiPortal() = default;
    ~WifiPortal() { end(); }
    WifiPortal(const WifiPortal&) = delete;
    WifiPortal& operator=(const WifiPortal&) = delete;

    // Setup mode: scans the nearby networks (for the page's suggestions), then
    // starts the open AP and the HTTP server. False if the AP could not be
    // started; lastMessage() then says why. A no-op while already active.
    bool begin(const Hooks& hooks);
    // Home mode: serves the page on the home network the app has already joined
    // (HomeWifi Connected; `homeSsid` is its name, for the page) and reads the
    // deck catalog on the next polls. The radio stays the app's business: end()
    // leaves the station up, and the app turns it off (HomeWifi::end()).
    bool beginHome(const Hooks& hooks, const char* homeSsid);
    // Serves pending requests and runs the staged work (catalog read, URL or
    // deck download, leaving for Get decks or home mode). Call every loop()
    // iteration while on the Wi-Fi screen; a download blocks. May call
    // Hooks::onGetDecks or onSwitchToHome, which usually stop the portal.
    void poll();
    // Stops the server (and in setup mode the AP and the radio) and frees the
    // state. Safe to call when idle.
    void end();
    // From the app's progress callback during Hooks::onDeckDownload: records
    // the progress for the page and answers its requests now and then (the
    // status view, the clock; anything that would change /packs is refused
    // while the download runs).
    void serviceDownload(size_t done, size_t total);

    bool active() const { return active_; }
    Mode mode() const { return mode_; }
    // Setup mode: the access point's name. Home mode: the home network's.
    const char* ssid() const { return ssid_; }
    // The page's address: "http://192.168.4.1/", or "http://<station ip>/".
    const char* url() const { return url_; }
    // Home mode with mDNS up: "http://tangocho.local/"; else "".
    const char* mdnsUrl() const;
    // Home mode: the station link is up (false while the driver reconnects
    // after a drop). Always true in setup mode.
    bool linkUp() const { return mode_ == Mode::Setup || linkUp_; }
    size_t uploadedPacks() const { return uploaded_; }  // this session
    const char* lastMessage() const { return msg_; }
    // millis() of the last HTTP request (or of begin()), for the idle timeout.
    uint32_t lastActivityMs() const { return lastActivity_; }

    // True once after anything that changes what the on-device screen shows
    // (upload finished, delete, download progress, catalog read, link change).
    // Reading clears it.
    bool consumeDirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    static constexpr const char* kApUrl = "http://192.168.4.1/";
    static constexpr const char* kMdnsHost = "tangocho";
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
    // A one-shot line at the top of the next page, set by a POST handler before
    // it redirects to "/".
    enum class Notice : uint8_t {
        None, WifiSaved, WifiBad, Forgotten, CatalogSaved, CatalogBad, Deleted, DeleteFailed,
        BadName, UrlStarted, UrlBad, Busy, UnknownDeck,
    };
    enum class UploadError : uint8_t {
        None, BadName, TooLarge, NoSpace, CannotCreate, WriteFailed, Invalid, CannotStore,
        Aborted, Busy,
    };
    enum class CatalogState : uint8_t { None, Pending, Loading, Ready, Failed };
    // Leaving this network a moment after the response page went out.
    enum class Leave : uint8_t { None, GetDecks, SwitchHome };

    bool startServer(const Hooks& hooks);
    void registerRoutes();
    void handleRoot();
    void handleApiStats();
    void handleStatus();
    void handleDelete();
    void handleFetchRequest();
    void handleUploadChunk();
    void handleUploadDone();
    void handleTime();
    void handleNotFound();
    void handleHomeWifi();
    void handleForgetWifi();
    void handleCatalogUrl();
    void handleCatalogReload();
    void handleGetDecks();
    void handleSwitchHome();
    void handleDeck();
    // Remembers the nearby network names (strongest first) for the page's SSID
    // suggestions. Runs once in begin(), before the AP starts.
    void scanNearby();

    // --- page building (WifiPortal.cpp) -----------------------------------------
    // The page language for this request ("en"/"es") and the deck-list one.
    const char* pageLang() const;
    String catalogLang() const;
    void rememberLangHint();
    void sendBusyPage();
    void appendNotice(String& h);
    void appendSetupDecks(String& h, const String& homeSsid, bool firstSetup);
    void appendHomeDecks(String& h, const PortalStats& st);
    void appendYourDecks(String& h, const PortalStats& st);
    void appendAdvanced(String& h, const PortalStats& st, const String& homeSsid, bool firstSetup,
                        bool open);
    bool noticeInAdvanced(Notice n, bool firstSetup) const;
    void appendWifiForm(String& h, const char* formId);
    void redirect(const char* where, Notice n, const String& arg = String());
    String catalogProblem() const;
    String downloadOutcome() const;

    // Blocking download of `url` into /packs; true on success.
    bool doFetch(const String& url);
    void loadCatalog();
    void runDeckDownload();
    void watchLink();
    // True while a deck download from the page is staged or running (the page
    // shows its progress instead of the forms).
    bool deckInFlight() const { return deckStage_ != 0; }
    // True while any download is staged or running: actions that change
    // /packs (or start another download) are refused meanwhile.
    bool anyDownload() const { return deckInFlight() || fetchPending_.length() || fetchUrl_.length(); }
    // First 4 bytes are "TNGC" and a cheap PackReader probe succeeds.
    bool validatePackFile(const String& path) const;
    void abortUpload(UploadError why);
    // printf into msg_ and mark the screen dirty.
    void setMsg(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void touch() { lastActivity_ = millis(); }

    WebServer* server_ = nullptr;
    bool active_ = false;
    Mode mode_ = Mode::Setup;
    Hooks hooks_{};
    char ssid_[33] = {0};
    char url_[32] = {0};
    bool mdns_ = false;
    bool linkUp_ = true;
    uint32_t linkCheckAt_ = 0;
    char msg_[80] = "ready";
    bool dirty_ = false;
    size_t uploaded_ = 0;  // packs received this session
    uint32_t lastActivity_ = 0;
    String langHint_;  // the page's ?lang= choice this session ("" = none)

    Notice notice_ = Notice::None;
    String noticeArg_;

    // Upload state, valid between UPLOAD_FILE_START and UPLOAD_FILE_END. The
    // body goes to kUploadTmp (never the destination); only the validated temp
    // is renamed over the destination.
    File uploadFile_;
    String uploadName_;
    UploadError uploadError_ = UploadError::None;
    bool uploadOk_ = false;
    size_t uploadWritten_ = 0;
    size_t uploadFree_ = 0;

    // Fetch state. A /fetch request is staged here and runs from poll() so the
    // on-device screen can paint "downloading..." before the loop blocks.
    String fetchPending_;
    String fetchUrl_;

    // Home mode: the deck catalog, read from poll() after beginHome() (and on
    // the page's "Try again" or a new catalog URL).
    CatalogState catState_ = CatalogState::None;
    std::vector<CatalogEntry> catalog_;
    FetchResult catFetch_;
    CatalogError catError_ = CatalogError::Ok;

    // Home mode: one deck download from the page at a time. POST /deck stages
    // it (stage 1); the next poll only marks the device screen for a repaint
    // (stage 2) and the one after runs it, blocking, through
    // Hooks::onDeckDownload (still stage 2 meanwhile).
    uint8_t deckStage_ = 0;
    String deckName_;
    bool deckUpdate_ = false;
    size_t dlDone_ = 0, dlTotal_ = 0;
    uint32_t dlServiceAt_ = 0;
    // The last finished download, shown once on the next page.
    bool lastDlValid_ = false;
    String lastDlName_;
    bool lastDlUpdate_ = false;
    DeckDownloadResult lastDl_;

    // Leaving this network: POST /getdecks or /home only records it and poll()
    // calls the hook a moment later, after the response page went out while
    // this network was still up.
    Leave leave_ = Leave::None;
    uint32_t leaveAt_ = 0;     // millis() of the request
    String getDecksLang_;      // the phone's language tag sent with /getdecks, or ""

    std::vector<String> nearby_;  // SSIDs seen by scanNearby()
};

}  // namespace tgc
