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

namespace tgc {

namespace {

// Keep this much of the filesystem free so a pack (and the SRS state file and a
// download temp) can never fill it completely and strand the device. Shared
// with every other pack writer (NetFetch.h).
constexpr size_t kFsReserveBytes = kPackFsReserveBytes;

constexpr uint32_t kFetchIdleMs = 5000;      // no data for this long -> abort
constexpr uint32_t kFetchTotalMs = 60000;    // whole download budget

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

// Posts the browser's clock to /time once per page load (docs/srs.md section
// 5.1) and shows the device's answer in #clk and the stats date (#dt).
const char kTimeScript[] = R"JS(<script>
(function(){var d=new Date(),z='',e=document.getElementById('clk');
try{z=Intl.DateTimeFormat().resolvedOptions().timeZone||''}catch(_){}
var x=new XMLHttpRequest();x.open('POST','/time');
x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
x.onload=function(){if(x.status!=200){e.textContent='The device did not accept this browser\'s clock.';return}
e.textContent='Device date set from this browser: '+x.responseText+(z?' ('+z+')':'');
var t=document.getElementById('dt'),n=document.getElementById('dtn');
if(t)t.textContent=x.responseText;if(n)n.textContent=''};
x.onerror=function(){e.textContent='Could not send the clock to the device.'};
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

String pageHeader(const char* title) {
    String h;
    h += "<!doctype html><html><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    h += "<title>";
    h += title;
    h += " - Tangocho</title><style>";
    h += "body{font-family:system-ui,sans-serif;margin:1rem;max-width:44rem;line-height:1.45}";
    h += "table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:.25rem .3rem;";
    h += "border-bottom:1px solid #ccc}form{margin:.5rem 0}button{padding:.35rem .7rem}";
    h += "input[type=url]{width:100%;max-width:26rem;padding:.3rem}";
    h += "input[type=text],input[type=password]{width:100%;max-width:16rem;padding:.3rem;margin:.15rem 0}";
    h += "code{background:#eee;padding:0 .2rem}h1{font-size:1.25rem}h2{font-size:1rem;margin:1rem 0 .3rem}";
    h += ".muted{color:#666}.err{color:#b00}";
    h += "</style></head><body><h1>Tangocho</h1>";
    return h;
}

}  // namespace

bool WifiPortal::begin(const Hooks& hooks) {
    if (active_) return true;
    hooks_ = hooks;
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
    // Open network: no passphrase (see the security note in the header).
    if (!WiFi.softAP(ssid_)) {
        setMsg("AP start failed");
        WiFi.mode(WIFI_OFF);
        return false;
    }

    server_ = new WebServer(80);
    if (!server_) {
        setMsg("no memory");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    registerRoutes();
    server_->begin();

    active_ = true;
    uploaded_ = 0;
    touch();
    setMsg("ready");
    Serial.printf("[wifi] AP \"%s\" open at %s\n", ssid_, url());
    return true;
}

void WifiPortal::registerRoutes() {
    // Every request counts as activity, so the app's idle timeout does not stop
    // the portal under a phone that is still using it.
    server_->on("/", HTTP_GET, [this]() { touch(); handleRoot(); });
    server_->on("/api/stats", HTTP_GET, [this]() { touch(); handleApiStats(); });
    server_->on("/delete", HTTP_POST, [this]() { touch(); handleDelete(); });
    server_->on("/fetch", HTTP_POST, [this]() { touch(); handleFetchRequest(); });
    server_->on("/wifi", HTTP_POST, [this]() { touch(); handleHomeWifi(); });
    server_->on("/wifi/forget", HTTP_POST, [this]() { touch(); handleForgetWifi(); });
    server_->on("/catalog", HTTP_POST, [this]() { touch(); handleCatalogUrl(); });
    server_->on("/upload", HTTP_POST, [this]() { touch(); handleUploadDone(); },
                [this]() { touch(); handleUploadChunk(); });
    server_->on("/time", HTTP_POST, [this]() { touch(); handleTime(); });
    server_->onNotFound([this]() { touch(); handleNotFound(); });
}

void WifiPortal::poll() {
    if (!active_ || !server_) return;
    server_->handleClient();
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
    if (active_) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    active_ = false;
    uploadOk_ = false;
    uploadName_ = "";
    fetchPending_ = "";
    fetchUrl_ = "";
    ssid_[0] = 0;
    Serial.printf("[wifi] portal stopped\n");
}

void WifiPortal::setMsg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_, sizeof msg_, fmt, ap);
    va_end(ap);
    dirty_ = true;
}

// --- pages -----------------------------------------------------------------

void WifiPortal::handleRoot() {
    PortalStats st;
    if (hooks_.fillStats) hooks_.fillStats(st);

    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    const size_t freeB = total > used ? total - used : 0;
    const uint32_t acc =
        st.reps ? (st.reps > st.lapses ? st.reps - st.lapses : 0) * 100u / st.reps : 0;

    String h = pageHeader("Packs");
    h += "<p>No computer needed: upload a <code>.tgc</code> pack straight from this page.</p>";
    h += "<p id=\"clk\" class=\"muted\">Opening this page sets the device date from this "
         "browser's clock.</p>";

    h += "<h2>Packs in <code>/packs</code></h2><table><tr><th>name</th><th>size</th><th></th></tr>";
    uint32_t count = 0;
    File dir = LittleFS.open(kPacksDir);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String n = f.name();
            const int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (f.isDirectory() || !n.endsWith(".tgc") || n.startsWith(".")) continue;
            ++count;
            const String esc = htmlEscape(n);
            h += "<tr><td>";
            h += esc;
            h += "</td><td>";
            h += String((unsigned)((f.size() + 1023) / 1024));
            h += " KB</td><td><form method=\"post\" action=\"/delete\" style=\"display:inline\">";
            h += "<input type=\"hidden\" name=\"name\" value=\"";
            h += esc;
            h += "\"><button type=\"submit\">delete</button></form></td></tr>";
        }
    }
    if (count == 0) h += "<tr><td colspan=\"3\" class=\"muted\">no packs yet</td></tr>";
    h += "</table>";

    h += "<h2>Upload a pack</h2><form method=\"post\" action=\"/upload\" ";
    h += "enctype=\"multipart/form-data\"><input type=\"file\" name=\"pack\" accept=\".tgc\" required> ";
    h += "<button type=\"submit\">Upload</button></form>";

    h += "<h2>Download from a URL</h2><form method=\"post\" action=\"/fetch\">";
    h += "<input type=\"url\" name=\"url\" placeholder=\"http://example.com/deck.tgc\" required> ";
    h += "<button type=\"submit\">Download</button></form>";
    h += "<p class=\"muted\">Plain <code>http://</code> only (on its own access point the device ";
    h += "has no internet). For decks from the internet, use the home Wi-Fi below.</p>";

    // Home Wi-Fi for "Get decks". The password is write-only: only the saved
    // network name is ever shown.
    const String homeSsid = HomeWifi::savedSsid();
    h += "<h2>Home Wi-Fi (for Get decks)</h2>";
    if (homeSsid.length()) {
        h += "<div>Saved network: <b>";
        h += htmlEscape(homeSsid);
        h += "</b> <form method=\"post\" action=\"/wifi/forget\" style=\"display:inline\">";
        h += "<button type=\"submit\">forget</button></form></div>";
    } else {
        h += "<p class=\"muted\">No home network saved yet.</p>";
    }
    h += "<form method=\"post\" action=\"/wifi\">";
    h += "<input type=\"text\" name=\"ssid\" list=\"ssids\" maxlength=\"32\" ";
    h += "placeholder=\"network name\" autocomplete=\"off\" required><br>";
    h += "<input type=\"password\" name=\"pass\" maxlength=\"64\" placeholder=\"password\" ";
    h += "autocomplete=\"new-password\"> <button type=\"submit\">Save</button>";
    if (!nearby_.empty()) {
        h += "<datalist id=\"ssids\">";
        for (const String& n : nearby_) {
            h += "<option value=\"";
            h += htmlEscape(n);
            h += "\">";
        }
        h += "</datalist>";
    }
    h += "</form><p class=\"muted\">Leave the password empty for an open network. On the device, ";
    h += "open <b>Decks</b> and pick <b>+ Get decks</b>: it joins this network only for the ";
    h += "download. The password stays on the device (stored unencrypted) and is never shown ";
    h += "again.</p>";

    h += "<h2>Deck catalog</h2><form method=\"post\" action=\"/catalog\">";
    h += "<input type=\"url\" name=\"url\" maxlength=\"256\" value=\"";
    h += htmlEscape(HomeWifi::catalogUrlOverride());
    h += "\" placeholder=\"default catalog\"> <button type=\"submit\">Save</button></form>";
    h += "<p class=\"muted\">Empty = the latest Tangocho release: <code>";
    h += HomeWifi::kDefaultCatalogUrl;
    h += "</code></p>";

    h += "<h2>Statistics</h2><ul>";
    h += "<li>date <span id=\"dt\">";
    h += st.date;
    h += "</span> <span id=\"dtn\" class=\"muted\">";
    if (!st.dateSynced) h += "(estimated: not set from a clock yet)";
    h += "</span> &middot; day ";
    h += String((unsigned)st.day);
    h += "</li><li>streak ";
    h += String((unsigned)st.streakCurrent);
    h += " / ";
    h += String((unsigned)st.streakBest);
    h += "</li><li>time on questions ";
    h += formatDuration(st.reviewMs);
    h += "</li><li>seen ";
    h += String((unsigned)st.seen);
    h += " / ";
    h += String((unsigned)st.packCards);
    h += " cards</li><li>answers ";
    h += String((unsigned)st.answers);
    h += "</li><li>accuracy ";
    h += String((unsigned)acc);
    h += "%</li><li>learned ";
    h += String((unsigned)st.learned);
    h += "</li></ul>";

    h += "<p>Free space: ";
    h += String((unsigned)(freeB / 1024));
    h += " KB of ";
    h += String((unsigned)(total / 1024));
    h += " KB &middot; packs: ";
    h += String((unsigned)count);
    h += "</p>";

    h += "<p class=\"muted\">JSON: <a href=\"/api/stats\">/api/stats</a></p>";
    h += "<p class=\"muted\">The access point is <b>open</b>: anyone in range can read or ";
    h += "delete your packs and see these statistics. Stop the portal when you are done.</p>";
    h += kTimeScript;
    h += "</body></html>";

    server_->send(200, "text/html", h);
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
    j += "{\"day\":";
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
    File dir = LittleFS.open(kPacksDir);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String n = f.name();
            const int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (f.isDirectory() || !n.endsWith(".tgc") || n.startsWith(".")) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"name\":\"";
            j += jsonEscape(n);
            j += "\",\"size\":";
            j += String((unsigned)f.size());
            j += "}";
        }
    }
    j += "]}";
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
    setMsg("date set %s%s%s", st.date, zone.length() ? " " : "", zone.c_str());
    Serial.printf("[wifi] date from browser: %s (tz %+d min %s)\n", st.date, (int)tz,
                  zone.c_str());
    server_->send(200, "text/plain", st.date);
}

void WifiPortal::handleNotFound() {
    server_->send(404, "text/html",
                  pageHeader("Not found") + "<p class=\"err\">Not found.</p><p><a href=\"/\">"
                                            "Back</a></p></body></html>");
}

void WifiPortal::handleDelete() {
    String name;
    if (!sanitizeName(server_->arg("name").c_str(), name)) {
        server_->send(400, "text/html",
                      pageHeader("Delete") + "<p class=\"err\">Bad pack name.</p>"
                                             "<p><a href=\"/\">Back</a></p></body></html>");
        return;
    }
    const bool ok = LittleFS.remove(String(kPacksDir) + "/" + name);
    Serial.printf("[wifi] delete %s: %s\n", name.c_str(), ok ? "ok" : "failed");
    setMsg(ok ? "deleted %s" : "delete failed", name.c_str());
    if (hooks_.onPacksChanged) hooks_.onPacksChanged();
    server_->sendHeader("Location", "/", true);
    server_->send(303, "text/plain", "");
}

// --- home Wi-Fi and catalog URL ----------------------------------------------

void WifiPortal::handleHomeWifi() {
    String ssid = server_->arg("ssid");
    ssid.trim();  // a phone keyboard's trailing space is likelier than a real one
    if (!HomeWifi::saveCredentials(ssid, server_->arg("pass"))) {
        server_->send(400, "text/html",
                      pageHeader("Home Wi-Fi") +
                          "<p class=\"err\">Not saved: the network name must be 1-32 characters "
                          "and the password empty (open network), 8-63 characters or 64 hex "
                          "digits.</p><p><a href=\"/\">Back</a></p></body></html>");
        return;
    }
    setMsg("home Wi-Fi saved");
    server_->sendHeader("Location", "/", true);
    server_->send(303, "text/plain", "");
}

void WifiPortal::handleForgetWifi() {
    HomeWifi::forgetCredentials();
    setMsg("home Wi-Fi forgotten");
    server_->sendHeader("Location", "/", true);
    server_->send(303, "text/plain", "");
}

void WifiPortal::handleCatalogUrl() {
    if (!HomeWifi::saveCatalogUrl(server_->arg("url"))) {
        server_->send(400, "text/html",
                      pageHeader("Catalog") +
                          "<p class=\"err\">Not saved: use an <code>http://</code> or "
                          "<code>https://</code> URL of at most 256 characters, or leave it "
                          "empty for the default.</p><p><a href=\"/\">Back</a></p></body></html>");
        return;
    }
    setMsg("catalog URL saved");
    server_->sendHeader("Location", "/", true);
    server_->send(303, "text/plain", "");
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
void WifiPortal::abortUpload(const char* why) {
    if (uploadFile_) uploadFile_.close();
    LittleFS.remove(kUploadTmp);
    uploadOk_ = false;
    uploadError_ = why;
    Serial.printf("[wifi] upload failed %s: %s\n", uploadName_.c_str(), why);
}

void WifiPortal::handleUploadChunk() {
    HTTPUpload& up = server_->upload();
    switch (up.status) {
        case UPLOAD_FILE_START: {
            uploadWritten_ = 0;
            uploadOk_ = false;
            uploadError_ = "";
            uploadName_ = "";
            String name;
            if (!sanitizeName(up.filename.c_str(), name)) {
                uploadError_ = "Bad filename (use a short name.tgc)";
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
                uploadError_ = "Pack too large";
                return;
            }
            if (bodyLen && bodyLen + kFsReserveBytes > freeB) {
                uploadError_ = "Not enough space";
                return;
            }
            LittleFS.mkdir(kPacksDir);
            uploadName_ = name;
            uploadFree_ = freeB;
            uploadFile_ = LittleFS.open(kUploadTmp, "w");
            if (!uploadFile_) {
                uploadError_ = "Cannot create file";
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
                abortUpload("Not enough space");
                break;
            }
            const size_t w = uploadFile_.write(up.buf, n);
            uploadWritten_ += w;
            if (w != n) abortUpload("Write failed");
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
                uploadError_ = "Not a valid .tgc pack (discarded)";
                Serial.printf("[wifi] upload rejected %s (%u bytes)\n", uploadName_.c_str(),
                              (unsigned)uploadWritten_);
                break;
            }
            const String dst = String(kPacksDir) + "/" + uploadName_;
            if (LittleFS.exists(dst)) LittleFS.remove(dst);
            if (!LittleFS.rename(kUploadTmp, dst)) {
                LittleFS.remove(kUploadTmp);
                uploadError_ = "Cannot store pack";
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
            abortUpload("Upload aborted");
            break;
        default:
            break;
    }
}

void WifiPortal::handleUploadDone() {
    if (uploadError_.length()) {
        server_->send(400, "text/html",
                      pageHeader("Upload") + "<p class=\"err\">Rejected: " +
                          htmlEscape(uploadError_) + "</p><p><a href=\"/\">Back</a></p></body></html>");
        return;
    }
    server_->send(200, "text/html",
                  pageHeader("Upload") + "<p>Pack received into <code>/packs</code>.</p>"
                                         "<p><a href=\"/\">Back to the packs</a></p></body></html>");
}

// --- URL download ----------------------------------------------------------

void WifiPortal::handleFetchRequest() {
    String url = server_->arg("url");
    url.trim();
    String host, path, name;
    uint16_t port = 0;
    if (url.length() == 0 || url.length() > 256 || !url.startsWith("http://") ||
        !parseHttpUrl(url, host, port, path, name)) {
        server_->send(400, "text/html",
                      pageHeader("Download") + "<p class=\"err\">Only plain <code>http://</code> "
                                               "URLs ending in a <code>.tgc</code> name are "
                                               "supported.</p><p><a href=\"/\">Back</a></p>"
                                               "</body></html>");
        return;
    }
    fetchPending_ = url;
    setMsg("downloading...");
    server_->send(200, "text/html",
                  pageHeader("Download") + "<p>Download started into <code>/packs/" +
                      htmlEscape(name) + "</code>.</p><p>Watch the device screen; the list above "
                      "refreshes when the download finishes.</p>"
                      "<p><a href=\"/\">Back</a></p></body></html>");
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

bool WifiPortal::validatePackFile(const String& path) const { return tgc::validatePackFile(path); }

}  // namespace tgc
