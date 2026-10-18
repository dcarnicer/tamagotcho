// Wi-Fi portal implementation. Arduino/ESP32 only; see WifiPortal.h.
#include "platform/WifiPortal.h"

#include <LittleFS.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <utility>

#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "portal/PortalLogic.h"
#include "portal/PortalText.h"
#include "Version.h"

// mDNS ("http://tangocho.local/") in home mode. Off with -DTGC_PORTAL_MDNS=0.
#ifndef TGC_PORTAL_MDNS
#define TGC_PORTAL_MDNS 1
#endif
#if TGC_PORTAL_MDNS
#include <ESPmDNS.h>
#endif

namespace tgc {

namespace {

// Keep this much of the filesystem free so a pack (and the SRS state file and a
// download temp) can never fill it completely and strand the device. Shared
// with every other pack writer (NetFetch.h).
constexpr size_t kFsReserveBytes = kPackFsReserveBytes;

constexpr uint32_t kFetchIdleMs = 5000;      // no data for this long -> abort
constexpr uint32_t kFetchTotalMs = 60000;    // whole download budget

// Leaving this network ("Get decks on the device screen", "Switch to my
// Wi-Fi"): it stays up this long after the request, so the response page
// (handed to lwIP by send(), whose connection the server closes right away)
// reaches the phone before the network goes down.
constexpr uint32_t kLeaveGraceMs = 1000;

// During a deck download from the page, answer the page this often at most
// (the status view refreshes every kBusyRefreshS seconds).
constexpr uint32_t kServiceEveryMs = 100;
constexpr int kBusyRefreshS = 2;
// While the catalog is being read, the page refreshes this often.
constexpr int kCatalogRefreshS = 2;
// Home mode: how often the station link and its IP are checked.
constexpr uint32_t kLinkCheckMs = 1000;

// Nearby networks offered as SSID suggestions on the page.
constexpr size_t kMaxNearby = 12;

// A multipart upload body carries a boundary/header envelope around the file,
// so the request's declared Content-Length is slightly larger than the `.tgc`
// itself. This slack keeps the early "too large" check from rejecting a pack
// that only just fits; the write-time cap stays authoritative for the file.
constexpr size_t kMultipartSlack = 4096;

// Basename only, must end in ".tgc", restricted to a safe character set and a
// short length. Anything else is rejected so the portal never writes outside
// /packs and never stores a surprising filename.
bool sanitizeName(const char* in, String& out) {
    if (!in) return false;
    String s(in);
    const int slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.substring(slash + 1);
    const int backslash = s.lastIndexOf('\\');
    if (backslash >= 0) s = s.substring(backslash + 1);
    if (s.isEmpty() || s.length() > 48) return false;
    if (s.indexOf("..") >= 0) return false;
    if (!s.endsWith(".tgc")) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    if (s.length() <= 4) return false;  // ".tgc" alone is not a name
    out = s;
    return true;
}

// Splits a plain-http URL into host, port, path (no query) and a sanitised
// /packs basename. https:// is deliberately rejected (see docs/decisions.md).
bool parseHttpUrl(const String& in, String& host, uint16_t& port, String& path, String& name) {
    String s = in;
    s.trim();
    if (!s.startsWith("http://")) return false;
    s = s.substring(7);
    const int slash = s.indexOf('/');
    String hostPort = slash < 0 ? s : s.substring(0, slash);
    path = slash < 0 ? String("/") : s.substring(slash);
    const int colon = hostPort.indexOf(':');
    port = 80;
    if (colon >= 0) {
        port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
        host = hostPort.substring(0, colon);
    } else {
        host = hostPort;
    }
    if (host.isEmpty() || port == 0) return false;
    int cut = path.indexOf('?');
    if (cut >= 0) path = path.substring(0, cut);
    cut = path.indexOf('#');
    if (cut >= 0) path = path.substring(0, cut);
    const int lastSlash = path.lastIndexOf('/');
    String base = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
    base.trim();
    if (base.isEmpty()) base = "download.tgc";
    if (!base.endsWith(".tgc")) base += ".tgc";
    return sanitizeName(base.c_str(), name);
}

String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        switch (in[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += in[i]; break;
        }
    }
    return out;
}

// printf into a String. Callers pass names already HTML-escaped, so templates
// with markup (PortalText) can be filled directly.
String fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
String fmt(const char* f, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return String(buf);
}

// Strict decimal: an optional '-' and 1..15 digits (Date.now() has 13). Anything
// else, including an empty field, is refused rather than read as 0.
bool parseDecimal(const String& in, int64_t* out) {
    const size_t n = in.length();
    const bool neg = n > 0 && in[0] == '-';
    size_t i = neg ? 1 : 0;
    if (i >= n || n - i > 15) return false;
    int64_t v = 0;
    for (; i < n; ++i) {
        const char c = in[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = neg ? -v : v;
    return true;
}

// An IANA zone name for the status message only ("Europe/Madrid"): a short
// run of letters, digits and / _ + -; anything else is dropped.
String sanitizeZone(const String& in) {
    if (in.length() > 40) return String();
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '+' || c == '-';
        if (!ok) return String();
    }
    return in;
}

// A BCP 47 tag such as "es-ES": letters, digits and '-' only, at most 15.
String sanitizeLangTag(const String& in) {
    String out;
    for (size_t i = 0; i < in.length() && out.length() < 15; ++i) {
        const char c = in[i];
        if (isalnum(static_cast<unsigned char>(c)) || c == '-') out += c;
    }
    return out;
}

// Posts the browser's clock to /time once per page load (docs/srs.md section
// 5.1) and shows the device's answer in #clk and the stats date (#dt). The
// localised messages come from #clk's data attributes.
const char kTimeScript[] = R"JS(<script>
(function(){var d=new Date(),z='',e=document.getElementById('clk');
try{z=Intl.DateTimeFormat().resolvedOptions().timeZone||''}catch(_){}
var x=new XMLHttpRequest();x.open('POST','/time');
x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
x.onload=function(){if(x.status!=200){e.textContent=e.dataset.bad;return}
e.textContent=e.dataset.ok+x.responseText+(z?' ('+z+')':'');
var t=document.getElementById('dt'),n=document.getElementById('dtn');
if(t)t.textContent=x.responseText;if(n)n.textContent=''};
x.onerror=function(){e.textContent=e.dataset.err};
x.send('ms='+d.getTime()+'&tz='+(-d.getTimezoneOffset())+'&zone='+encodeURIComponent(z))})();
</script>)JS";

// Escape a string for a JSON value (quotes, backslashes and control chars).
String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            continue;  // drop control characters
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// "2h 5m", "5m 30s" or "30s".
String formatDuration(uint32_t ms) {
    const uint32_t totalS = ms / 1000u;
    const uint32_t mins = totalS / 60u;
    const uint32_t hours = mins / 60u;
    char buf[24];
    if (hours > 0)
        snprintf(buf, sizeof buf, "%uh %um", (unsigned)hours, (unsigned)(mins % 60u));
    else if (mins > 0)
        snprintf(buf, sizeof buf, "%um %us", (unsigned)mins, (unsigned)(totalS % 60u));
    else
        snprintf(buf, sizeof buf, "%us", (unsigned)totalS);
    return String(buf);
}

// "704 KB" / "4.1 MB".
String formatSize(size_t bytes) {
    char buf[16];
    if (bytes >= 1024u * 1024u) {
        const unsigned tenths = static_cast<unsigned>(bytes / (1024u * 1024u / 10u));
        snprintf(buf, sizeof buf, "%u.%u MB", tenths / 10u, tenths % 10u);
    } else {
        snprintf(buf, sizeof buf, "%u KB", (unsigned)((bytes + 1023u) / 1024u));
    }
    return String(buf);
}

// The *.tgc files in /packs (name, size), sorted like the device's deck list.
std::vector<std::pair<String, size_t>> listPackFiles(const char* dirPath) {
    std::vector<std::pair<String, size_t>> out;
    File dir = LittleFS.open(dirPath);
    if (!dir || !dir.isDirectory()) return out;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String n = f.name();
        const int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        if (f.isDirectory() || !n.endsWith(".tgc") || n.startsWith(".")) continue;
        out.emplace_back(n, f.size());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The state of catalog deck `e` against /packs (catalog/catalogDeckState).
DeckState deckStateOnDisk(const char* dirPath, const CatalogEntry& e) {
    const String path = String(dirPath) + "/" + e.name.c_str() + ".tgc";
    if (!LittleFS.exists(path)) return DeckState::New;
    File f = LittleFS.open(path, "r");
    if (!f) return DeckState::New;
    const size_t size = f.size();
    f.close();
    return catalogDeckState(true, static_cast<uint32_t>(size), e.bytes);
}

String pageHeader(const PortalText& t, int refreshS = 0) {
    String h;
    h += "<!doctype html><html lang=\"";
    h += t.htmlLang;
    h += "\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    if (refreshS > 0) {
        h += "<meta http-equiv=\"refresh\" content=\"";
        h += String(refreshS);
        h += "\">";
    }
    h += "<title>Tangoch\xC5\x8D</title><style>";
    h += "body{font-family:system-ui,sans-serif;margin:1rem;max-width:44rem;line-height:1.45}";
    h += "table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:.3rem .3rem;";
    h += "border-bottom:1px solid #ccc;vertical-align:middle}td.r{text-align:right;white-space:nowrap}";
    h += "form{margin:.5rem 0}td form{margin:0}button{padding:.35rem .7rem}";
    h += "button.main{font-size:1.05rem;padding:.55rem 1rem}";
    h += "input[type=url]{width:100%;max-width:26rem;padding:.3rem}";
    h += "input[type=text],input[type=password]{width:100%;max-width:16rem;padding:.3rem;margin:.15rem 0}";
    h += "code{background:#eee;padding:0 .2rem}h1{font-size:1.25rem;margin-bottom:.2rem}";
    h += "h2{font-size:1.05rem;margin:1.2rem 0 .3rem}h3{font-size:.95rem;margin:.9rem 0 .2rem}";
    h += "details{margin:1.2rem 0}summary{font-weight:600;cursor:pointer}progress{width:100%}";
    h += ".muted{color:#666}.err{color:#b00}.ok{color:#070}.status{margin-top:0}";
    h += "</style></head><body><h1>Tangoch\xC5\x8D</h1>";
    return h;
}

// A standalone message page (a leave page, an error that has no form).
String messagePage(const PortalText& t, const String& body, bool backLink) {
    String h = pageHeader(t);
    h += body;
    if (backLink) {
        h += "<p><a href=\"/\">";
        h += t.back;
        h += "</a></p>";
    }
    h += "</body></html>";
    return h;
}

const char* uploadErrorText(const PortalText& t, uint8_t e) {
    switch (e) {
        case 1: return t.upBadName;
        case 2: return t.upTooLarge;
        case 3: return t.upNoSpace;
        case 4: return t.upCannotCreate;
        case 5: return t.upWriteFailed;
        case 6: return t.upInvalid;
        case 7: return t.upCannotStore;
        case 8: return t.upAborted;
        case 9: return t.busy;
        default: return t.upAborted;
    }
}

}  // namespace

// --- lifecycle ---------------------------------------------------------------

bool WifiPortal::begin(const Hooks& hooks) {
    if (active_) return true;
    mode_ = Mode::Setup;
    LittleFS.mkdir(kPacksDir);

    scanNearby();
    WiFi.mode(WIFI_AP);
    // Stable SSID suffix from the AP MAC's last two bytes ("Tangocho-A1B2").
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (mac[4] == 0 && mac[5] == 0) {  // never expected; keep it unique-ish anyway
        mac[4] = 0xAB;
        mac[5] = 0xCD;
    }
    snprintf(ssid_, sizeof ssid_, "Tangocho-%02X%02X", mac[4], mac[5]);
    snprintf(url_, sizeof url_, "%s", kApUrl);
    // Open network: no passphrase (see the security note in the header).
    if (!WiFi.softAP(ssid_)) {
        setMsg("AP start failed");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    if (!startServer(hooks)) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    Serial.printf("[wifi] AP \"%s\" open at %s\n", ssid_, url());
    return true;
}

bool WifiPortal::beginHome(const Hooks& hooks, const char* homeSsid) {
    if (active_) return true;
    mode_ = Mode::Home;
    LittleFS.mkdir(kPacksDir);
    nearby_.clear();  // no scan while joined: the form takes a typed name
    snprintf(ssid_, sizeof ssid_, "%s", homeSsid ? homeSsid : "");
    snprintf(url_, sizeof url_, "http://%s/", WiFi.localIP().toString().c_str());
    linkUp_ = WiFi.status() == WL_CONNECTED;
    linkCheckAt_ = millis();
    if (!startServer(hooks)) return false;
#if TGC_PORTAL_MDNS
    mdns_ = MDNS.begin(kMdnsHost);
    if (mdns_) MDNS.addService("http", "tcp", 80);
#endif
    catalog_.clear();
    catState_ = hooks_.onDeckDownload ? CatalogState::Pending : CatalogState::None;
    Serial.printf("[wifi] portal on \"%s\" at %s%s%s\n", ssid_, url(), mdns_ ? ", " : "",
                  mdnsUrl());
    logNetHeap("home portal up");
    return true;
}

bool WifiPortal::startServer(const Hooks& hooks) {
    hooks_ = hooks;
    server_ = new WebServer(80);
    if (!server_) {
        setMsg("no memory");
        return false;
    }
    registerRoutes();
    server_->begin();
    active_ = true;
    uploaded_ = 0;
    notice_ = Notice::None;
    lastDlValid_ = false;
    langHint_ = "";
    touch();
    setMsg("ready");
    return true;
}

const char* WifiPortal::mdnsUrl() const {
    return (active_ && mode_ == Mode::Home && mdns_) ? "http://tangocho.local/" : "";
}

void WifiPortal::registerRoutes() {
    // The page language follows the browser while the device has none.
    static const char* kHeaders[] = {"Accept-Language"};
    server_->collectHeaders(kHeaders, 1);
    // Every request counts as activity, so the app's idle timeout does not stop
    // the portal under a phone that is still using it.
    server_->on("/", HTTP_GET, [this]() { touch(); handleRoot(); });
    server_->on("/api/stats", HTTP_GET, [this]() { touch(); handleApiStats(); });
    server_->on("/api/status", HTTP_GET, [this]() { touch(); handleStatus(); });
    server_->on("/delete", HTTP_POST, [this]() { touch(); handleDelete(); });
    server_->on("/fetch", HTTP_POST, [this]() { touch(); handleFetchRequest(); });
    server_->on("/wifi", HTTP_POST, [this]() { touch(); handleHomeWifi(); });
    server_->on("/wifi/forget", HTTP_POST, [this]() { touch(); handleForgetWifi(); });
    server_->on("/catalog", HTTP_POST, [this]() { touch(); handleCatalogUrl(); });
    server_->on("/catalog/reload", HTTP_POST, [this]() { touch(); handleCatalogReload(); });
    server_->on("/getdecks", HTTP_POST, [this]() { touch(); handleGetDecks(); });
    server_->on("/home", HTTP_POST, [this]() { touch(); handleSwitchHome(); });
    server_->on("/deck", HTTP_POST, [this]() { touch(); handleDeck(); });
    server_->on("/upload", HTTP_POST, [this]() { touch(); handleUploadDone(); },
                [this]() { touch(); handleUploadChunk(); });
    server_->on("/time", HTTP_POST, [this]() { touch(); handleTime(); });
    server_->onNotFound([this]() { touch(); handleNotFound(); });
}

void WifiPortal::poll() {
    if (!active_ || !server_) return;
    server_->handleClient();
    if (mode_ == Mode::Home) watchLink();
    // Stage a fetch for one loop iteration so the on-device screen can paint
    // "downloading..." before the (blocking) download starts.
    if (fetchPending_.length()) {
        fetchUrl_ = fetchPending_;
        fetchPending_ = "";
        setMsg("downloading...");
        return;
    }
    if (fetchUrl_.length()) {
        const String url = fetchUrl_;
        fetchUrl_ = "";
        doFetch(url);
        touch();  // the download may have taken a while
        return;
    }
    // Home mode: the catalog, then a deck download from the page; each gets
    // one iteration for the device screen to paint its status first.
    if (catState_ == CatalogState::Pending) {
        catState_ = CatalogState::Loading;
        setMsg("reading catalog...");
        return;
    }
    if (catState_ == CatalogState::Loading) {
        loadCatalog();
        return;
    }
    if (deckStage_ == 1) {
        deckStage_ = 2;
        setMsg("downloading %s...", deckName_.c_str());
        return;
    }
    if (deckStage_ == 2) {
        runDeckDownload();
        return;
    }
    // Leaving this network once the response page had its moment to go out.
    // The hook normally ends this portal, so nothing may run after it.
    if (leave_ != Leave::None && millis() - leaveAt_ >= kLeaveGraceMs) {
        const Leave what = leave_;
        leave_ = Leave::None;
        if (what == Leave::GetDecks) {
            Serial.printf("[wifi] page asked for Get decks: leaving the portal\n");
            if (hooks_.onGetDecks) {
                const String lang = getDecksLang_;  // the hook usually ends this portal
                hooks_.onGetDecks(lang.c_str());
            }
        } else {
            Serial.printf("[wifi] page asked for home mode: leaving the portal\n");
            if (hooks_.onSwitchToHome) hooks_.onSwitchToHome();
        }
        return;
    }
}

void WifiPortal::end() {
    if (uploadFile_) uploadFile_.close();
    // A temp left by an interrupted upload is of no use to anyone.
    LittleFS.remove(kUploadTmp);
    if (server_) {
        server_->stop();
        delete server_;
        server_ = nullptr;
    }
#if TGC_PORTAL_MDNS
    if (mdns_) MDNS.end();
#endif
    mdns_ = false;
    // Home mode leaves the station to its owner (HomeWifi::end()).
    if (active_ && mode_ == Mode::Setup) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    active_ = false;
    uploadOk_ = false;
    uploadName_ = "";
    fetchPending_ = "";
    fetchUrl_ = "";
    leave_ = Leave::None;
    getDecksLang_ = "";
    catState_ = CatalogState::None;
    catalog_.clear();
    deckStage_ = 0;
    lastDlValid_ = false;
    ssid_[0] = 0;
    url_[0] = 0;
    Serial.printf("[wifi] portal stopped\n");
}

void WifiPortal::setMsg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_, sizeof msg_, fmt, ap);
    va_end(ap);
    dirty_ = true;
}

// Home mode: the driver reconnects by itself after a drop, maybe to a new IP.
// Follow both, so the device screen never shows a dead address.
void WifiPortal::watchLink() {
    const uint32_t now = millis();
    if (now - linkCheckAt_ < kLinkCheckMs) return;
    linkCheckAt_ = now;
    const bool up = WiFi.status() == WL_CONNECTED;
    if (up != linkUp_) {
        linkUp_ = up;
        dirty_ = true;
        Serial.printf("[wifi] home link %s\n", up ? "back" : "lost");
    }
    if (!up) return;
    char url[sizeof url_];
    snprintf(url, sizeof url, "http://%s/", WiFi.localIP().toString().c_str());
    if (std::strcmp(url, url_) != 0) {
        snprintf(url_, sizeof url_, "%s", url);
        dirty_ = true;
        Serial.printf("[wifi] portal now at %s\n", url_);
    }
}

// --- language ----------------------------------------------------------------

const char* WifiPortal::pageLang() const {
    const char* dev = hooks_.uiLang ? hooks_.uiLang() : "";
    const String accept = server_ ? server_->header("Accept-Language") : String();
    return portalPageLang(dev, langHint_.c_str(), accept.c_str());
}

String WifiPortal::catalogLang() const {
    const char* dev = hooks_.uiLang ? hooks_.uiLang() : "";
    const String accept = server_ ? server_->header("Accept-Language") : String();
    return String(catalogOrderLang(dev, langHint_.c_str(), accept.c_str()).c_str());
}

// GET /?lang=xx (the page's language links) sticks for this portal session.
void WifiPortal::rememberLangHint() {
    if (!server_->hasArg("lang")) return;
    langHint_ = sanitizeLangTag(server_->arg("lang"));
}

// --- pages -----------------------------------------------------------------

void WifiPortal::redirect(const char* where, Notice n, const String& arg) {
    notice_ = n;
    noticeArg_ = arg;
    server_->sendHeader("Location", where, true);
    server_->send(303, "text/plain", "");
}

// Whether the action behind notice `n` lives in the Advanced section, which
// the next page then shows open (its result in view).
bool WifiPortal::noticeInAdvanced(Notice n, bool firstSetup) const {
    switch (n) {
        case Notice::WifiBad: return !firstSetup;
        case Notice::WifiSaved: return mode_ == Mode::Home;
        case Notice::Forgotten: return mode_ == Mode::Home;
        case Notice::CatalogSaved: return mode_ == Mode::Setup;
        case Notice::CatalogBad:
        case Notice::UrlBad: return true;
        default: return false;
    }
}

void WifiPortal::appendNotice(String& h) {
    const PortalText& t = portalText(pageLang());
    const Notice n = notice_;
    notice_ = Notice::None;
    const String arg = htmlEscape(noticeArg_);
    String text;
    bool ok = true;
    switch (n) {
        case Notice::None: return;
        case Notice::WifiSaved: text = t.wifiSavedOk; break;
        case Notice::WifiBad: text = t.wifiBad; ok = false; break;
        case Notice::Forgotten: text = mode_ == Mode::Home ? t.forgottenHome : t.forgotten; break;
        case Notice::CatalogSaved: text = t.catalogUrlSaved; break;
        case Notice::CatalogBad: text = t.catalogUrlBad; ok = false; break;
        case Notice::Deleted: text = fmt(t.deletedFmt, arg.c_str()); break;
        case Notice::DeleteFailed: text = t.deleteFailed; ok = false; break;
        case Notice::BadName: text = t.badName; ok = false; break;
        case Notice::UrlStarted: text = fmt(t.urlStartedFmt, arg.c_str()); break;
        case Notice::UrlBad: text = t.urlBad; ok = false; break;
        case Notice::Busy: text = t.busy; ok = false; break;
        case Notice::UnknownDeck: text = t.unknownDeck; ok = false; break;
    }
    h += ok ? "<p class=\"ok\">" : "<p class=\"err\">";
    h += text;
    h += "</p>";
}

// The page while a deck download from it is staged or running: its progress,
// refreshing itself, and nothing that could start anything else.
void WifiPortal::sendBusyPage() {
    const PortalText& t = portalText(pageLang());
    const String name = htmlEscape(deckName_);
    String h = pageHeader(t, kBusyRefreshS);
    h += "<h2>";
    if (deckStage_ == 2 && dlTotal_ > 0) {
        const unsigned pct = static_cast<unsigned>(static_cast<uint64_t>(dlDone_) * 100u / dlTotal_);
        h += fmt(t.dlProgressFmt, name.c_str(), pct);
        h += "</h2><progress max=\"100\" value=\"";
        h += String(pct);
        h += "\"></progress>";
    } else {
        h += fmt(t.dlStartingFmt, name.c_str());
        h += "</h2><progress></progress>";
    }
    h += "<p class=\"muted\">";
    h += t.dlBusyNote;
    h += "</p>";
    appendNotice(h);  // e.g. an action refused meanwhile ("busy")
    h += "</body></html>";
    server_->send(200, "text/html", h);
}

void WifiPortal::handleRoot() {
    rememberLangHint();
    if (deckInFlight()) {
        sendBusyPage();
        return;
    }
    const PortalText& t = portalText(pageLang());
    PortalStats st;
    if (hooks_.fillStats) hooks_.fillStats(st);
    const auto packs = listPackFiles(kPacksDir);
    const String homeSsid = HomeWifi::savedSsid();
    const bool firstSetup = mode_ == Mode::Setup && homeSsid.isEmpty();
    const bool catalogBusy =
        catState_ == CatalogState::Pending || catState_ == CatalogState::Loading;
    const bool advOpen = noticeInAdvanced(notice_, firstSetup);  // before appendNotice() clears it

    String h;
    h.reserve(12 * 1024);
    h += pageHeader(t, catalogBusy ? kCatalogRefreshS : 0);

    // Status line: decks, free space, which network this page is on.
    h += "<p class=\"status muted\">";
    if (packs.empty())
        h += t.decksNone;
    else if (packs.size() == 1)
        h += t.decksOne;
    else
        h += fmt(t.decksFmt, (unsigned)packs.size());
    h += " &middot; ";
    h += fmt(t.freeFmt, formatSize(fsFreeBytes()).c_str());
    h += " &middot; ";
    h += fmt(mode_ == Mode::Home ? t.viaFmt : t.ownNetFmt, htmlEscape(ssid_).c_str());
    h += "</p><p id=\"clk\" class=\"muted\" data-ok=\"";
    h += htmlEscape(t.clockOk);
    h += "\" data-bad=\"";
    h += htmlEscape(t.clockBad);
    h += "\" data-err=\"";
    h += htmlEscape(t.clockErr);
    h += "\">";
    h += t.clockHint;
    h += "</p>";
    appendNotice(h);

    if (mode_ == Mode::Setup)
        appendSetupDecks(h, homeSsid, firstSetup);
    else
        appendHomeDecks(h, st);
    appendYourDecks(h, st);
    appendAdvanced(h, st, homeSsid, firstSetup, advOpen);

    h += "<p class=\"muted\">";
    if (mode_ == Mode::Home)
        h += fmt(t.securityHomeFmt, htmlEscape(ssid_).c_str());
    else
        h += t.securitySetup;
    h += "</p>";
    // A device with no language of its own follows the browser; offer both.
    const char* dev = hooks_.uiLang ? hooks_.uiLang() : "";
    if (!dev || !*dev) {
        h += "<p class=\"muted\">";
        h += t.language;
        h += " <a href=\"/?lang=en\">English</a> &middot; <a href=\"/?lang=es\">Espa\xC3\xB1ol</a></p>";
    }
    h += kTimeScript;
    h += "</body></html>";
    server_->send(200, "text/html", h);
}

// The home Wi-Fi form (SSID with the nearby suggestions, password). The
// datalist goes with the first form only.
void WifiPortal::appendWifiForm(String& h, const char* formId) {
    const PortalText& t = portalText(pageLang());
    h += "<form method=\"post\" action=\"/wifi\" id=\"";
    h += formId;
    h += "\"><input type=\"text\" name=\"ssid\" list=\"ssids\" maxlength=\"32\" placeholder=\"";
    h += t.ssidPlaceholder;
    h += "\" autocomplete=\"off\" required><br><input type=\"password\" name=\"pass\" maxlength=\"64\" ";
    h += "placeholder=\"";
    h += t.passPlaceholder;
    h += "\" autocomplete=\"new-password\"> <button type=\"submit\">";
    h += t.save;
    h += "</button>";
    if (!nearby_.empty()) {
        h += "<datalist id=\"ssids\">";
        for (const String& n : nearby_) {
            h += "<option value=\"";
            h += htmlEscape(n);
            h += "\">";
        }
        h += "</datalist>";
    }
    h += "</form><p class=\"muted\">";
    h += t.passNote;
    h += "</p>";
}

// Setup mode. First setup: step 1 is the home Wi-Fi, the one thing to do. With
// a network saved: switch to it (the page's own Get decks), or Get decks on
// the device screen.
void WifiPortal::appendSetupDecks(String& h, const String& homeSsid, bool firstSetup) {
    const PortalText& t = portalText(pageLang());
    if (firstSetup) {
        h += "<h2 id=\"wifi\">";
        h += t.step1Title;
        h += "</h2><p>";
        h += t.step1Intro;
        h += "</p>";
        appendWifiForm(h, "wifi1");
        h += "<h2>";
        h += t.step2Title;
        h += "</h2><p class=\"muted\">";
        h += t.step2Text;
        h += "</p><p class=\"muted\">";
        h += t.orUpload;
        h += "</p>";
        return;
    }
    const String home = htmlEscape(homeSsid);
    h += "<h2 id=\"decks\">";
    h += t.getDecksTitle;
    h += "</h2><p>";
    h += fmt(t.yourWifiFmt, home.c_str());
    h += "</p>";
    if (hooks_.onSwitchToHome) {
        h += "<form method=\"post\" action=\"/home\"><button type=\"submit\" class=\"main\">";
        h += t.switchButton;
        h += "</button></form><p class=\"muted\">";
        h += fmt(t.switchNoteFmt, home.c_str());
        h += "</p>";
    }
    if (hooks_.onGetDecks) {
        // The phone's language orders the device's deck list on a device that
        // has no deck to take it from yet (see wifiToGetDecks() in main.cpp).
        h += "<form method=\"post\" action=\"/getdecks\"><input type=\"hidden\" name=\"lang\" ";
        h += "id=\"gdlang\"><button type=\"submit\">";
        h += t.deviceGetDecksButton;
        h += "</button></form><p class=\"muted\">";
        h += t.deviceGetDecksNote;
        h += "</p><script>document.getElementById('gdlang').value=navigator.language||'';</script>";
    }
}

// Home mode: the catalog, own language first, one row per deck with what a
// press does (download, update) or that it is already here.
void WifiPortal::appendHomeDecks(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    h += "<h2 id=\"decks\">";
    h += t.getDecksTitle;
    h += "</h2>";
    if (lastDlValid_) {
        lastDlValid_ = false;
        const bool ok = lastDl_.fetch.status == FetchStatus::Ok && !lastDl_.batteryLow;
        h += ok ? "<p class=\"ok\">" : "<p class=\"err\">";
        h += downloadOutcome();
        h += "</p>";
    }
    if (catState_ == CatalogState::Pending || catState_ == CatalogState::Loading) {
        h += "<p>";
        h += t.catalogLoading;
        h += "</p><progress></progress>";
        return;
    }
    if (catState_ == CatalogState::Failed) {
        h += "<p class=\"err\">";
        h += catalogProblem();
        h += "</p><form method=\"post\" action=\"/catalog/reload\"><button type=\"submit\">";
        h += t.retry;
        h += "</button></form>";
        return;
    }
    if (catState_ != CatalogState::Ready) return;
    if (catalog_.empty()) {
        h += "<p class=\"muted\">";
        h += t.catalogEmpty;
        h += "</p>";
        return;
    }
    std::vector<CatalogEntry> rows = catalog_;
    const String lang = catalogLang();
    orderCatalogForLang(rows, lang.c_str());
    h += "<table>";
    for (const CatalogEntry& e : rows) {
        const String name = htmlEscape(e.name.c_str());
        const DeckState ds = deckStateOnDisk(kPacksDir, e);
        h += "<tr><td><b>";
        h += name;
        h += "</b><br><span class=\"muted\">";
        h += fmt(t.deckInfoFmt, htmlEscape(e.lang.empty() ? "?" : e.lang.c_str()).c_str(),
                 (unsigned)e.cards, formatSize(e.bytes).c_str());
        h += "</span></td><td class=\"r\">";
        if (ds == DeckState::OnDevice) {
            h += "<span class=\"ok\">";
            h += t.onDevice;
            if (std::strcmp(st.loadedPack, (String(e.name.c_str()) + ".tgc").c_str()) == 0) {
                h += " &middot; ";
                h += t.studying;
            }
            h += "</span>";
        } else {
            h += "<form method=\"post\" action=\"/deck\"><input type=\"hidden\" name=\"name\" value=\"";
            h += name;
            h += "\"><button type=\"submit\">";
            h += ds == DeckState::Update ? t.update : t.download;
            h += "</button></form>";
        }
        h += "</td></tr>";
    }
    h += "</table><p class=\"muted\">";
    h += t.deckNote;
    h += "</p>";
}

void WifiPortal::appendYourDecks(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    h += "<h2 id=\"yours\">";
    h += t.yoursTitle;
    h += "</h2><table>";
    const auto packs = listPackFiles(kPacksDir);
    for (const auto& p : packs) {
        const String esc = htmlEscape(p.first);
        h += "<tr><td>";
        h += esc;
        if (p.first == st.loadedPack) {
            h += " <span class=\"ok\">&middot; ";
            h += t.studying;
            h += "</span>";
        }
        h += "</td><td class=\"r\">";
        h += formatSize(p.second);
        h += "</td><td class=\"r\"><form method=\"post\" action=\"/delete\" data-q=\"";
        h += htmlEscape(t.deleteConfirm);
        h += "\" onsubmit=\"return confirm(this.dataset.q)\"><input type=\"hidden\" name=\"name\" value=\"";
        h += esc;
        h += "\"><button type=\"submit\">";
        h += t.deleteButton;
        h += "</button></form></td></tr>";
    }
    if (packs.empty()) {
        h += "<tr><td colspan=\"3\" class=\"muted\">";
        h += t.yoursEmpty;
        h += "</td></tr>";
    }
    h += "</table>";
}

// Everything that is not the main task, collapsed: upload, URL download, the
// catalog URL, the home Wi-Fi, the statistics. Opened when the last action was
// one of these, so its result is in view.
void WifiPortal::appendAdvanced(String& h, const PortalStats& st, const String& homeSsid,
                                bool firstSetup, bool open) {
    const PortalText& t = portalText(pageLang());
    h += open ? "<details id=\"adv\" open><summary>" : "<details id=\"adv\"><summary>";
    h += t.advanced;
    h += "</summary>";

    h += "<h3>";
    h += t.uploadTitle;
    h += "</h3><form method=\"post\" action=\"/upload\" enctype=\"multipart/form-data\">";
    h += "<input type=\"file\" name=\"pack\" accept=\".tgc\" required> <button type=\"submit\">";
    h += t.uploadButton;
    h += "</button></form>";

    h += "<h3>";
    h += t.urlTitle;
    h += "</h3><form method=\"post\" action=\"/fetch\">";
    h += "<input type=\"url\" name=\"url\" placeholder=\"http://example.com/deck.tgc\" required> ";
    h += "<button type=\"submit\">";
    h += t.download;
    h += "</button></form><p class=\"muted\">";
    h += mode_ == Mode::Home ? t.urlNoteHome : t.urlNoteSetup;
    h += "</p>";

    h += "<h3>";
    h += t.catalogTitle;
    h += "</h3><form method=\"post\" action=\"/catalog\"><input type=\"url\" name=\"url\" ";
    h += "maxlength=\"256\" value=\"";
    h += htmlEscape(HomeWifi::catalogUrlOverride());
    h += "\" placeholder=\"";
    h += t.catalogPlaceholder;
    h += "\"> <button type=\"submit\">";
    h += t.save;
    h += "</button></form><p class=\"muted\">";
    h += fmt(t.catalogDefaultFmt, (String("<code>") + HomeWifi::kDefaultCatalogUrl + "</code>").c_str());
    h += "</p>";

    // Home Wi-Fi: change or forget it. The first setup has its form on top. The
    // password is write-only: only the saved network name is ever shown.
    if (!firstSetup) {
        h += "<h3 id=\"wifi\">";
        h += t.wifiTitle;
        h += "</h3>";
        if (homeSsid.length()) {
            h += "<div>";
            h += fmt(t.savedNetFmt, htmlEscape(homeSsid).c_str());
            h += " <form method=\"post\" action=\"/wifi/forget\" style=\"display:inline\" data-q=\"";
            h += htmlEscape(t.forgetConfirm);
            h += "\" onsubmit=\"return confirm(this.dataset.q)\"><button type=\"submit\">";
            h += t.forget;
            h += "</button></form></div>";
        } else {
            h += "<p class=\"muted\">";
            h += t.noHomeWifi;
            h += "</p>";
        }
        appendWifiForm(h, "wifi2");
        if (mode_ == Mode::Home && homeSsid.length() && hooks_.onSwitchToHome) {
            h += "<form method=\"post\" action=\"/home\"><button type=\"submit\">";
            h += t.reconnect;
            h += "</button> <span class=\"muted\">";
            h += t.changeNoteHome;
            h += "</span></form>";
        }
    }

    const uint32_t acc =
        st.reps ? (st.reps > st.lapses ? st.reps - st.lapses : 0) * 100u / st.reps : 0;
    h += "<h3>";
    h += t.statsTitle;
    h += "</h3><ul><li>";
    h += t.statDate;
    h += " <span id=\"dt\">";
    h += st.date;
    h += "</span> <span id=\"dtn\" class=\"muted\">";
    if (!st.dateSynced) h += t.statEstimated;
    h += "</span> &middot; ";
    h += t.statDay;
    h += " ";
    h += String((unsigned)st.day);
    h += "</li><li>";
    h += t.statStreak;
    h += " ";
    h += String((unsigned)st.streakCurrent);
    h += " / ";
    h += String((unsigned)st.streakBest);
    h += "</li><li>";
    h += t.statTime;
    h += " ";
    h += formatDuration(st.reviewMs);
    h += "</li><li>";
    h += t.statSeen;
    h += " ";
    h += String((unsigned)st.seen);
    h += " / ";
    h += String((unsigned)st.packCards);
    h += " ";
    h += t.statCards;
    h += "</li><li>";
    h += t.statAnswers;
    h += " ";
    h += String((unsigned)st.answers);
    h += "</li><li>";
    h += t.statAccuracy;
    h += " ";
    h += String((unsigned)acc);
    h += "%</li><li>";
    h += t.statLearned;
    h += " ";
    h += String((unsigned)st.learned);
    h += "</li></ul><p>";
    h += fmt(t.freeSpaceFmt, formatSize(fsFreeBytes()).c_str(),
             formatSize(LittleFS.totalBytes()).c_str());
    h += "</p><p class=\"muted\">JSON: <a href=\"/api/stats\">/api/stats</a> &middot; ";
    h += "<a href=\"/api/status\">/api/status</a></p></details>";
}

String WifiPortal::catalogProblem() const {
    const PortalText& t = portalText(pageLang());
    switch (catFetch_.status) {
        case FetchStatus::Ok:
            return catError_ == CatalogError::UnsupportedVersion ? t.catalogNewer : t.catalogBad;
        case FetchStatus::Connect: return t.netServer;
        case FetchStatus::Tls:
            return String(t.netTls) + fmt(" (mbedTLS -0x%04X)", (unsigned)(-catFetch_.tlsError));
        case FetchStatus::Http:
            return catFetch_.httpCode == 404 ? String(t.catalogNotPublished)
                                             : fmt(t.netHttpFmt, catFetch_.httpCode);
        case FetchStatus::TooLarge: return t.catalogBad;
        default: return t.catalogFailed;
    }
}

String WifiPortal::downloadOutcome() const {
    const PortalText& t = portalText(pageLang());
    if (lastDl_.batteryLow) return t.dlBattery;
    const FetchResult& r = lastDl_.fetch;
    switch (r.status) {
        case FetchStatus::Ok: return fmt(t.dlSavedFmt, htmlEscape(lastDlName_).c_str());
        case FetchStatus::NoSpace: return lastDlUpdate_ ? t.dlNoSpaceUpdate : t.dlNoSpace;
        case FetchStatus::TooLarge: return t.dlNoSpace;
        case FetchStatus::SizeMismatch:
        case FetchStatus::HashMismatch: return t.dlCorrupt;
        case FetchStatus::InvalidPack: return t.dlInvalid;
        case FetchStatus::Cancelled: return t.dlCancelled;
        case FetchStatus::Connect: return t.netServer;
        case FetchStatus::Tls: return t.netTls;
        case FetchStatus::Http: return fmt(t.netHttpFmt, r.httpCode);
        default: return t.dlFailed;
    }
}

void WifiPortal::handleApiStats() {
    PortalStats st;
    if (hooks_.fillStats) hooks_.fillStats(st);
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    const size_t freeB = total > used ? total - used : 0;
    const uint32_t acc =
        st.reps ? (st.reps > st.lapses ? st.reps - st.lapses : 0) * 100u / st.reps : 0;

    String j;
    j += "{\"version\":\"";
    j += jsonEscape(firmwareVersion());
    j += "\",\"day\":";
    j += String((unsigned)st.day);
    j += ",\"date\":\"";
    j += jsonEscape(st.date);
    j += "\",\"dateSynced\":";
    j += st.dateSynced ? "true" : "false";
    j += ",\"streak\":{\"current\":";
    j += String((unsigned)st.streakCurrent);
    j += ",\"best\":";
    j += String((unsigned)st.streakBest);
    j += "},\"reviewMs\":";
    j += String((unsigned)st.reviewMs);
    j += ",\"seen\":";
    j += String((unsigned)st.seen);
    j += ",\"learned\":";
    j += String((unsigned)st.learned);
    j += ",\"answers\":";
    j += String((unsigned)st.answers);
    j += ",\"reps\":";
    j += String((unsigned)st.reps);
    j += ",\"lapses\":";
    j += String((unsigned)st.lapses);
    j += ",\"accuracy\":";
    j += String((unsigned)acc);
    j += ",\"freeBytes\":";
    j += String((unsigned)freeB);
    j += ",\"totalBytes\":";
    j += String((unsigned)total);
    j += ",\"packs\":[";
    bool first = true;
    for (const auto& p : listPackFiles(kPacksDir)) {
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"";
        j += jsonEscape(p.first);
        j += "\",\"size\":";
        j += String((unsigned)p.second);
        j += "}";
    }
    j += "]}";
    server_->send(200, "application/json", j);
}

// GET /api/status: what the portal is doing, for a script or a quick look.
void WifiPortal::handleStatus() {
    const char* cat = "none";
    switch (catState_) {
        case CatalogState::None: cat = "none"; break;
        case CatalogState::Pending:
        case CatalogState::Loading: cat = "loading"; break;
        case CatalogState::Ready: cat = "ready"; break;
        case CatalogState::Failed: cat = "failed"; break;
    }
    String j;
    j += "{\"version\":\"";
    j += jsonEscape(firmwareVersion());
    j += "\",\"mode\":\"";
    j += mode_ == Mode::Home ? "home" : "setup";
    j += "\",\"network\":\"";
    j += jsonEscape(ssid_);
    j += "\",\"url\":\"";
    j += jsonEscape(url_);
    j += "\",\"catalog\":\"";
    j += cat;
    j += "\",\"catalogDecks\":";
    j += String((unsigned)catalog_.size());
    j += ",\"busy\":";
    j += deckInFlight() ? "true" : "false";
    j += ",\"deck\":\"";
    j += jsonEscape(deckInFlight() ? deckName_ : String());
    j += "\",\"done\":";
    j += String((unsigned)(deckInFlight() ? dlDone_ : 0));
    j += ",\"total\":";
    j += String((unsigned)(deckInFlight() ? dlTotal_ : 0));
    j += ",\"message\":\"";
    j += jsonEscape(msg_);
    j += "\"}";
    server_->send(200, "application/json", j);
}

// POST /time, form fields `ms` (the browser's Date.now()), `tz` (minutes east of
// UTC, -getTimezoneOffset()) and an optional `zone` (IANA name, message only).
// The page posts it on every load; posting the same clock again is harmless.
// The portal only checks the syntax: whether the time is plausible, and what it
// means for the SRS day, is the app's call (hooks_.onTime, docs/srs.md 5.1).
void WifiPortal::handleTime() {
    int64_t ms = 0, tz = 0;
    if (!parseDecimal(server_->arg("ms"), &ms) || !parseDecimal(server_->arg("tz"), &tz) ||
        tz < -24 * 60 || tz > 24 * 60) {
        server_->send(400, "text/plain", "bad time");
        return;
    }
    if (!hooks_.onTime || !hooks_.onTime(ms, static_cast<int16_t>(tz))) {
        setMsg("browser clock rejected");
        server_->send(400, "text/plain", "time rejected");
        return;
    }
    PortalStats st;
    if (hooks_.fillStats) hooks_.fillStats(st);
    const String zone = sanitizeZone(server_->arg("zone"));
    if (!deckInFlight()) setMsg("date set %s%s%s", st.date, zone.length() ? " " : "", zone.c_str());
    Serial.printf("[wifi] date from browser: %s (tz %+d min %s)\n", st.date, (int)tz,
                  zone.c_str());
    server_->send(200, "text/plain", st.date);
}

void WifiPortal::handleNotFound() {
    const PortalText& t = portalText(pageLang());
    server_->send(404, "text/html",
                  messagePage(t, String("<p class=\"err\">") + t.notFound + "</p>", true));
}

void WifiPortal::handleDelete() {
    if (anyDownload()) {
        redirect("/#yours", Notice::Busy);
        return;
    }
    String name;
    if (!sanitizeName(server_->arg("name").c_str(), name)) {
        redirect("/#yours", Notice::BadName);
        return;
    }
    const bool ok = LittleFS.remove(String(kPacksDir) + "/" + name);
    Serial.printf("[wifi] delete %s: %s\n", name.c_str(), ok ? "ok" : "failed");
    setMsg(ok ? "deleted %s" : "delete failed", name.c_str());
    if (hooks_.onPacksChanged) hooks_.onPacksChanged();
    redirect("/#yours", ok ? Notice::Deleted : Notice::DeleteFailed, name);
}

// --- home Wi-Fi and catalog URL ----------------------------------------------

void WifiPortal::handleHomeWifi() {
    String ssid = server_->arg("ssid");
    ssid.trim();  // a phone keyboard's trailing space is likelier than a real one
    const bool firstSetup = mode_ == Mode::Setup && !HomeWifi::hasCredentials();
    if (!HomeWifi::saveCredentials(ssid, server_->arg("pass"))) {
        redirect(firstSetup ? "/#wifi" : "/#adv", Notice::WifiBad);
        return;
    }
    setMsg("home Wi-Fi saved");
    // Setup mode: back to Get decks, which now offers the switch; home mode:
    // to the form, next to Reconnect.
    redirect(mode_ == Mode::Setup ? "/#decks" : "/#adv", Notice::WifiSaved);
}

void WifiPortal::handleForgetWifi() {
    HomeWifi::forgetCredentials();
    setMsg("home Wi-Fi forgotten");
    redirect("/#wifi", Notice::Forgotten);
}

void WifiPortal::handleCatalogUrl() {
    if (!HomeWifi::saveCatalogUrl(server_->arg("url"))) {
        redirect("/#adv", Notice::CatalogBad);
        return;
    }
    setMsg("catalog URL saved");
    // Home mode lists the new catalog right away (unless a download runs).
    if (mode_ == Mode::Home && hooks_.onDeckDownload && !deckInFlight())
        catState_ = CatalogState::Pending;
    redirect(mode_ == Mode::Home ? "/#decks" : "/#adv", Notice::CatalogSaved);
}

// POST /catalog/reload, the page's "Try again" after a failed catalog read.
void WifiPortal::handleCatalogReload() {
    if (mode_ == Mode::Home && hooks_.onDeckDownload && !deckInFlight())
        catState_ = CatalogState::Pending;
    redirect("/#decks", Notice::None);
}

// POST /deck (home mode): download catalog deck `name`. Only recorded here;
// poll() runs it (Hooks::onDeckDownload) once this redirect went out, and the
// page shows the progress meanwhile.
void WifiPortal::handleDeck() {
    if (mode_ != Mode::Home || !hooks_.onDeckDownload || catState_ != CatalogState::Ready) {
        redirect("/#decks", Notice::UnknownDeck);
        return;
    }
    if (anyDownload() || leave_ != Leave::None) {
        redirect("/#decks", Notice::Busy);
        return;
    }
    const String name = server_->arg("name");
    const CatalogEntry* e = nullptr;
    for (const CatalogEntry& c : catalog_)
        if (name == c.name.c_str()) e = &c;
    if (!e) {
        redirect("/#decks", Notice::UnknownDeck);
        return;
    }
    const DeckState ds = deckStateOnDisk(kPacksDir, *e);
    if (ds == DeckState::OnDevice) {  // a second tab, or a double submit
        redirect("/#decks", Notice::None);
        return;
    }
    deckName_ = e->name.c_str();
    deckUpdate_ = ds == DeckState::Update;
    deckStage_ = 1;
    dlDone_ = 0;
    dlTotal_ = e->bytes;
    lastDlValid_ = false;
    Serial.printf("[wifi] page asked for deck %s (%s)\n", deckName_.c_str(),
                  deckUpdate_ ? "update" : "new");
    redirect("/#decks", Notice::None);
}

// Runs the staged deck download through the app (blocking).
void WifiPortal::runDeckDownload() {
    const CatalogEntry* e = nullptr;
    for (const CatalogEntry& c : catalog_)
        if (deckName_ == c.name.c_str()) e = &c;
    if (!e || !hooks_.onDeckDownload) {
        deckStage_ = 0;
        return;
    }
    const CatalogEntry entry = *e;  // the hook may rescan; keep our own copy
    dlServiceAt_ = millis();
    logNetHeap("page download start");
    const DeckDownloadResult r = hooks_.onDeckDownload(entry, deckUpdate_);
    logNetHeap("page download end");
    if (!active_) return;  // the app stopped the portal meanwhile
    deckStage_ = 0;
    lastDl_ = r;
    lastDlName_ = deckName_;
    lastDlUpdate_ = deckUpdate_;
    lastDlValid_ = true;
    if (r.batteryLow)
        setMsg("battery low: not downloading");
    else if (r.fetch.status == FetchStatus::Ok)
        setMsg("saved %s", deckName_.c_str());
    else
        setMsg("%s: %s", deckName_.c_str(), fetchStatusString(r.fetch.status));
    touch();  // the download may have taken a while
}

void WifiPortal::serviceDownload(size_t done, size_t total) {
    dlDone_ = done;
    if (total) dlTotal_ = total;
    if (!active_ || !server_) return;
    const uint32_t now = millis();
    if (now - dlServiceAt_ < kServiceEveryMs) return;
    dlServiceAt_ = now;
    touch();
    server_->handleClient();
}

// POST /getdecks, the setup-mode page's "Get decks on the device screen": the
// device leaves its access point and opens Get decks over the saved home Wi-Fi
// (Hooks::onGetDecks). Only recorded here; poll() leaves a moment later, so
// this page is sent while the network is still up.
void WifiPortal::handleGetDecks() {
    const PortalText& t = portalText(pageLang());
    const String home = HomeWifi::savedSsid();
    if (!hooks_.onGetDecks || home.isEmpty()) {
        server_->send(400, "text/html",
                      messagePage(t, String("<p class=\"err\">") + t.noHomeSaved + "</p>", true));
        return;
    }
    // Home mode lists the decks on the page itself (a stale setup-mode tab).
    if (mode_ == Mode::Home) {
        redirect("/#decks", Notice::None);
        return;
    }
    if (anyDownload()) {
        redirect("/#decks", Notice::Busy);
        return;
    }
    leave_ = Leave::GetDecks;
    leaveAt_ = millis();
    getDecksLang_ = sanitizeLangTag(server_->arg("lang"));
    setMsg("starting Get decks...");
    const String homeEsc = htmlEscape(home);
    const String here = htmlEscape(ssid_);
    String b = "<h2>";
    b += t.leaveGetDecksTitle;
    b += "</h2><p>";
    b += fmt(t.leaveGetDecksFmt, here.c_str(), homeEsc.c_str());
    b += "</p><p>";
    b += fmt(t.leavePhoneFmt, here.c_str());
    b += "</p><p>";
    b += fmt(t.leaveFollowFmt, homeEsc.c_str());
    b += "</p><p class=\"muted\">";
    b += t.leaveAgain;
    b += "</p>";
    server_->send(200, "text/html", messagePage(t, b, false));
}

// POST /home: "Switch to my Wi-Fi" (setup mode) or "Reconnect" (home mode).
// Staged like /getdecks: the app ends this portal and opens it in home mode.
void WifiPortal::handleSwitchHome() {
    const PortalText& t = portalText(pageLang());
    const String home = HomeWifi::savedSsid();
    if (!hooks_.onSwitchToHome || home.isEmpty()) {
        server_->send(400, "text/html",
                      messagePage(t, String("<p class=\"err\">") + t.noHomeSaved + "</p>", true));
        return;
    }
    if (anyDownload()) {
        redirect("/#decks", Notice::Busy);
        return;
    }
    leave_ = Leave::SwitchHome;
    leaveAt_ = millis();
    setMsg("switching to %s...", home.c_str());
    const String homeEsc = htmlEscape(home);
    String b = "<h2>";
    if (mode_ == Mode::Setup) {
        b += t.switchTitle;
        b += "</h2><p>";
        b += fmt(t.switchLeaveFmt, htmlEscape(ssid_).c_str(), homeEsc.c_str());
        b += "</p><p><b>";
        b += fmt(t.switchPhoneFmt, homeEsc.c_str());
        b += "</b></p>";
#if TGC_PORTAL_MDNS
        b += "<p>";
        b += fmt(t.switchMdnsFmt, "http://tangocho.local/", "http://tangocho.local/");
        b += "</p>";
#endif
        b += "<p class=\"muted\">";
        b += t.switchFail;
        b += "</p>";
    } else {
        b += t.reconnectTitle;
        b += "</h2><p>";
        b += fmt(t.reconnectFmt, homeEsc.c_str());
        b += "</p>";
    }
    server_->send(200, "text/html", messagePage(t, b, false));
}

void WifiPortal::scanNearby() {
    nearby_.clear();
    WiFi.mode(WIFI_STA);
    // Active scan at ~100 ms per channel: about 1.5 s, once per portal start.
    const int16_t n = WiFi.scanNetworks(false, false, false, 100);
    std::vector<std::pair<int32_t, String>> seen;
    for (int16_t i = 0; i < n; ++i) {
        const String s = WiFi.SSID(i);
        if (s.isEmpty()) continue;  // hidden network
        seen.emplace_back(WiFi.RSSI(i), s);
    }
    WiFi.scanDelete();
    std::sort(seen.begin(), seen.end(),
              [](const std::pair<int32_t, String>& a, const std::pair<int32_t, String>& b) {
                  return a.first > b.first;
              });
    for (const auto& e : seen) {
        if (nearby_.size() >= kMaxNearby) break;
        if (std::find(nearby_.begin(), nearby_.end(), e.second) == nearby_.end())
            nearby_.push_back(e.second);
    }
    Serial.printf("[wifi] scan: %d networks, %u names\n", (int)n, (unsigned)nearby_.size());
}

// --- upload ----------------------------------------------------------------

// Aborts the upload in flight. Only the temp file is removed: the destination
// pack is never touched before the final, validated rename, so a dropped
// connection, a size cap hit or a 0-byte body always leaves the old pack usable.
void WifiPortal::abortUpload(UploadError why) {
    if (uploadFile_) uploadFile_.close();
    LittleFS.remove(kUploadTmp);
    uploadOk_ = false;
    uploadError_ = why;
    Serial.printf("[wifi] upload failed %s: error %u\n", uploadName_.c_str(), (unsigned)why);
}

void WifiPortal::handleUploadChunk() {
    HTTPUpload& up = server_->upload();
    switch (up.status) {
        case UPLOAD_FILE_START: {
            uploadWritten_ = 0;
            uploadOk_ = false;
            uploadError_ = UploadError::None;
            uploadName_ = "";
            // One writer at a time: a download is filling /packs. (The server
            // still reads the body; it is just not stored.)
            if (anyDownload()) {
                uploadError_ = UploadError::Busy;
                return;
            }
            String name;
            if (!sanitizeName(up.filename.c_str(), name)) {
                uploadError_ = UploadError::BadName;
                return;
            }
            // Discard a temp left by an earlier run before measuring the space.
            LittleFS.remove(kUploadTmp);
            const size_t total = LittleFS.totalBytes();
            const size_t used = LittleFS.usedBytes();
            const size_t freeB = total > used ? total - used : 0;
            // `up.totalSize` is always 0 here (this WebServer core only fills it
            // on a buffer flush), so use the request's declared Content-Length.
            // 0 means "unknown/chunked" -- then only the write-time cap applies.
            const int declared = server_->clientContentLength();
            const size_t bodyLen = declared > 0 ? static_cast<size_t>(declared) : 0;
            if (bodyLen > kMaxPackBytes + kMultipartSlack) {
                uploadError_ = UploadError::TooLarge;
                return;
            }
            if (bodyLen && bodyLen + kFsReserveBytes > freeB) {
                uploadError_ = UploadError::NoSpace;
                return;
            }
            LittleFS.mkdir(kPacksDir);
            uploadName_ = name;
            uploadFree_ = freeB;
            uploadFile_ = LittleFS.open(kUploadTmp, "w");
            if (!uploadFile_) {
                uploadError_ = UploadError::CannotCreate;
                return;
            }
            uploadOk_ = true;
            break;
        }
        case UPLOAD_FILE_WRITE: {
            if (!uploadOk_) break;
            const size_t n = up.currentSize;
            if (uploadWritten_ + n + kFsReserveBytes > uploadFree_ ||
                uploadWritten_ + n > kMaxPackBytes) {
                abortUpload(UploadError::NoSpace);
                break;
            }
            const size_t w = uploadFile_.write(up.buf, n);
            uploadWritten_ += w;
            if (w != n) abortUpload(UploadError::WriteFailed);
            break;
        }
        case UPLOAD_FILE_END: {
            if (!uploadOk_) break;
            uploadFile_.close();
            uploadOk_ = false;
            // Probe the temp, then swap it in. The destination is removed only
            // now, after the body is known-good, so a rejected upload leaves the
            // existing pack intact.
            if (uploadWritten_ < 4 || !validatePackFile(kUploadTmp)) {
                LittleFS.remove(kUploadTmp);
                uploadError_ = UploadError::Invalid;
                Serial.printf("[wifi] upload rejected %s (%u bytes)\n", uploadName_.c_str(),
                              (unsigned)uploadWritten_);
                break;
            }
            const String dst = String(kPacksDir) + "/" + uploadName_;
            if (LittleFS.exists(dst)) LittleFS.remove(dst);
            if (!LittleFS.rename(kUploadTmp, dst)) {
                LittleFS.remove(kUploadTmp);
                uploadError_ = UploadError::CannotStore;
                Serial.printf("[wifi] upload rename failed %s\n", uploadName_.c_str());
                break;
            }
            ++uploaded_;
            setMsg("uploaded %s (%u KB)", uploadName_.c_str(), (unsigned)(uploadWritten_ / 1024));
            Serial.printf("[wifi] upload ok %s %u bytes\n", uploadName_.c_str(),
                          (unsigned)uploadWritten_);
            if (hooks_.onPacksChanged) hooks_.onPacksChanged();
            break;
        }
        case UPLOAD_FILE_ABORTED:
            abortUpload(UploadError::Aborted);
            break;
        default:
            break;
    }
}

// The result as a page of its own rather than a redirect: on the boot-time
// screen in setup mode a good upload sends the device into the quiz and stops
// the portal right after this response, so a follow-up GET would find nothing.
void WifiPortal::handleUploadDone() {
    const PortalText& t = portalText(pageLang());
    if (uploadError_ != UploadError::None) {
        String b = "<p class=\"err\">";
        b += t.uploadRejected;
        b += " ";
        b += uploadErrorText(t, static_cast<uint8_t>(uploadError_));
        b += "</p>";
        server_->send(400, "text/html", messagePage(t, b, true));
        return;
    }
    server_->send(200, "text/html",
                  messagePage(t, String("<p class=\"ok\">") + t.uploadOk + "</p>", true));
}

// --- URL download ----------------------------------------------------------

void WifiPortal::handleFetchRequest() {
    if (anyDownload()) {
        redirect("/#adv", Notice::Busy);
        return;
    }
    String url = server_->arg("url");
    url.trim();
    String host, path, name;
    uint16_t port = 0;
    if (url.length() == 0 || url.length() > 256 || !url.startsWith("http://") ||
        !parseHttpUrl(url, host, port, path, name)) {
        redirect("/#adv", Notice::UrlBad);
        return;
    }
    fetchPending_ = url;
    setMsg("downloading...");
    redirect("/#yours", Notice::UrlStarted, String(kPacksDir) + "/" + name);
}

bool WifiPortal::doFetch(const String& url) {
    String host, path, name;
    uint16_t port = 0;
    if (!parseHttpUrl(url, host, port, path, name)) {
        setMsg("bad URL");
        return false;
    }
    // The shared atomic download (NetFetch.h): /packs/.fetch.tmp, validated,
    // then renamed over the destination.
    PackFetchRequest req;
    req.url = url;
    req.fileName = name;
    req.maxBytes = kMaxPackBytes;
    req.idleTimeoutMs = kFetchIdleMs;
    req.totalTimeoutMs = kFetchTotalMs;
    const FetchResult r = fetchPackFile(req);
    switch (r.status) {
        case FetchStatus::Ok:
            setMsg("downloaded %s (%u KB)", name.c_str(), (unsigned)(r.bytes / 1024));
            Serial.printf("[wifi] fetch ok %s %u bytes\n", name.c_str(), (unsigned)r.bytes);
            if (hooks_.onPacksChanged) hooks_.onPacksChanged();
            return true;
        case FetchStatus::RenameFailed:
            // The old pack was already removed, so /packs did change: let the app
            // rescan rather than keep a handle to a file that is gone.
            if (r.packsChanged && hooks_.onPacksChanged) hooks_.onPacksChanged();
            setMsg("rename failed");
            return false;
        case FetchStatus::InvalidPack:
            setMsg("not a valid .tgc");
            break;
        default:
            if (r.bodyStarted) {
                setMsg("download failed");
                break;
            }
            // Refused before the body: nothing was written.
            if (r.status == FetchStatus::BadUrl)
                setMsg("bad URL");
            else if (r.status == FetchStatus::NoSpace && r.httpCode == 0)
                setMsg("no space for %s", name.c_str());
            else if (r.status == FetchStatus::NoSpace)
                setMsg("no space");
            else if (r.status == FetchStatus::TooLarge)
                setMsg("pack too big");
            else if (r.status == FetchStatus::WriteFailed)
                setMsg("write failed");
            else
                setMsg("HTTP %d", r.httpCode);
            return false;
    }
    Serial.printf("[wifi] fetch failed %s (%u bytes)\n", url.c_str(), (unsigned)r.bytes);
    return false;
}

// --- catalog (home mode) -------------------------------------------------------

void WifiPortal::loadCatalog() {
    CatalogFetch c = fetchCatalog(HomeWifi::catalogUrl());
    catFetch_ = c.fetch;
    catError_ = c.parse.error;
    if (c.fetch.status == FetchStatus::Ok && c.parse.error == CatalogError::Ok) {
        catalog_ = std::move(c.parse.entries);
        catState_ = CatalogState::Ready;
        setMsg("catalog: %u decks", (unsigned)catalog_.size());
    } else {
        catalog_.clear();
        catState_ = CatalogState::Failed;
        setMsg("catalog: %s", c.fetch.status == FetchStatus::Ok
                                  ? catalogErrorString(c.parse.error)
                                  : fetchStatusString(c.fetch.status));
    }
    touch();  // the read may have taken a while
}

bool WifiPortal::validatePackFile(const String& path) const { return tgc::validatePackFile(path); }

}  // namespace tgc
