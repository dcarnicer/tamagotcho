// Wi-Fi portal implementation. Arduino/ESP32 only; see WifiPortal.h.
#include "platform/WifiPortal.h"

#include <LittleFS.h>
#include <esp_mac.h>

#include <cstdarg>
#include <cstring>

#include <HTTPClient.h>

#include "pack/PackReader.h"
#include "platform/FsPackSource.h"

namespace tgc {

namespace {

// Keep this much of the filesystem free so a pack (and the SRS state file and a
// download temp) can never fill it completely and strand the device.
constexpr size_t kFsReserveBytes = 96u * 1024u;

constexpr uint16_t kHttpTimeoutMs = 8000;    // per read
constexpr uint32_t kFetchIdleMs = 5000;      // no data for this long -> abort
constexpr uint32_t kFetchTotalMs = 60000;    // whole download budget
constexpr size_t kFetchBufSize = 1024;

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
    setMsg("ready");
    Serial.printf("[wifi] AP \"%s\" open at %s\n", ssid_, url());
    return true;
}

void WifiPortal::registerRoutes() {
    server_->on("/", HTTP_GET, [this]() { handleRoot(); });
    server_->on("/api/stats", HTTP_GET, [this]() { handleApiStats(); });
    server_->on("/delete", HTTP_POST, [this]() { handleDelete(); });
    server_->on("/fetch", HTTP_POST, [this]() { handleFetchRequest(); });
    server_->on("/upload", HTTP_POST, [this]() { handleUploadDone(); },
                [this]() { handleUploadChunk(); });
    server_->onNotFound([this]() { handleNotFound(); });
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
    }
}

void WifiPortal::end() {
    if (uploadFile_) uploadFile_.close();
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
    h += "<p class=\"muted\">Plain <code>http://</code> only; TLS to arbitrary hosts is untested.</p>";

    h += "<h2>Statistics</h2><ul>";
    h += "<li>day ";
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

// --- upload ----------------------------------------------------------------

void WifiPortal::abortUpload(const char* why) {
    if (uploadFile_) uploadFile_.close();
    if (uploadName_.length()) LittleFS.remove(String(kPacksDir) + "/" + uploadName_);
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
            const size_t total = LittleFS.totalBytes();
            const size_t used = LittleFS.usedBytes();
            const size_t freeB = total > used ? total - used : 0;
            if (up.totalSize > kMaxPackBytes) {
                uploadError_ = "Pack too large";
                return;
            }
            if (up.totalSize && up.totalSize + kFsReserveBytes > freeB) {
                uploadError_ = "Not enough space";
                return;
            }
            LittleFS.mkdir(kPacksDir);
            uploadName_ = name;
            uploadFree_ = freeB;
            const String path = String(kPacksDir) + "/" + name;
            LittleFS.remove(path);  // replace an existing pack of the same name
            uploadFile_ = LittleFS.open(path, "w");
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
            const String path = String(kPacksDir) + "/" + uploadName_;
            if (uploadWritten_ < 4 || !validatePackFile(path)) {
                LittleFS.remove(path);  // never leave a broken pack behind
                uploadError_ = "Not a valid .tgc pack (deleted)";
                Serial.printf("[wifi] upload rejected %s (%u bytes)\n", uploadName_.c_str(),
                              (unsigned)uploadWritten_);
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
    const String dst = String(kPacksDir) + "/" + name;
    const String tmp = String(kPacksDir) + "/.fetch.tmp";

    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    const size_t freeB = total > used ? total - used : 0;
    if (freeB < kFsReserveBytes) {
        setMsg("no space for %s", name.c_str());
        return false;
    }

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(url)) {
        setMsg("bad URL");
        return false;
    }
    const int code = http.GET();
    if (code != 200) {
        setMsg("HTTP %d", code);
        http.end();
        return false;
    }
    int remaining = http.getSize();
    if (remaining > 0 && (size_t)remaining > kMaxPackBytes) {
        setMsg("pack too big");
        http.end();
        return false;
    }
    if (remaining > 0 && (size_t)remaining + kFsReserveBytes > freeB) {
        setMsg("no space");
        http.end();
        return false;
    }

    LittleFS.remove(tmp);
    File f = LittleFS.open(tmp, "w");
    if (!f) {
        setMsg("write failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[kFetchBufSize];
    size_t written = 0;
    bool ok = true;
    const uint32_t start = millis();
    uint32_t lastData = millis();
    while (http.connected() && (remaining > 0 || remaining == -1)) {
        if (millis() - start > kFetchTotalMs) {
            ok = false;
            break;
        }
        const size_t avail = stream->available();
        if (avail) {
            const size_t want = avail > sizeof buf ? sizeof buf : avail;
            const int r = stream->read(buf, want);
            if (r <= 0) {
                ok = false;
                break;
            }
            if (written + (size_t)r + kFsReserveBytes > freeB ||
                written + (size_t)r > kMaxPackBytes) {
                ok = false;
                break;
            }
            if (f.write(buf, r) != (size_t)r) {
                ok = false;
                break;
            }
            written += r;
            lastData = millis();
            if (remaining > 0) {
                remaining -= r;
                if (remaining <= 0) break;
            }
        } else {
            if (millis() - lastData > kFetchIdleMs) {
                ok = false;
                break;
            }
            delay(1);
        }
    }
    f.close();
    http.end();

    if (ok && written >= 4 && validatePackFile(tmp)) {
        if (LittleFS.exists(dst)) LittleFS.remove(dst);
        if (LittleFS.rename(tmp, dst)) {
            setMsg("downloaded %s (%u KB)", name.c_str(), (unsigned)(written / 1024));
            Serial.printf("[wifi] fetch ok %s %u bytes\n", name.c_str(), (unsigned)written);
            if (hooks_.onPacksChanged) hooks_.onPacksChanged();
            return true;
        }
        LittleFS.remove(tmp);
        setMsg("rename failed");
        return false;
    }
    LittleFS.remove(tmp);
    setMsg(ok ? "not a valid .tgc" : "download failed");
    Serial.printf("[wifi] fetch failed %s (%u bytes)\n", url.c_str(), (unsigned)written);
    return false;
}

bool WifiPortal::validatePackFile(const String& path) const {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t magic[4] = {0, 0, 0, 0};
    const size_t n = f.read(magic, sizeof magic);
    f.close();
    if (n != sizeof magic || memcmp(magic, "TNGC", 4) != 0) return false;

    // Cheap structural probe (PSRAM-backed); freed again before we return.
    auto src = std::make_unique<FsPackSource>(LittleFS.open(path, "r"));
    if (!src->ok()) return false;
    PackReader probe;
    const PackError e = probe.open(*src, psramAllocator());
    if (e != PackError::Ok) Serial.printf("[wifi] pack probe failed: %s\n", packErrorString(e));
    return e == PackError::Ok;
}

}  // namespace tgc
