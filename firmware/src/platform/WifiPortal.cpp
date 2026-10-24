// Wi-Fi portal implementation. Arduino/ESP32 only; see WifiPortal.h.
#include "platform/WifiPortal.h"

#include <LittleFS.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "platform/FsQuiet.h"
#include "platform/FsSrsFile.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/Ota.h"
#include "platform/Storage.h"
#include "portal/PortalLogic.h"
#include "portal/PortalText.h"
#include "srs/TimeZone.h"
#include "Credits.h"
#include "Version.h"

// mDNS ("http://tamagotcho.local/") in home mode. Off with -DTGC_PORTAL_MDNS=0.
#ifndef TGC_PORTAL_MDNS
#define TGC_PORTAL_MDNS 1
#endif
#if TGC_PORTAL_MDNS
#include <ESPmDNS.h>
#endif

namespace tgc {

namespace {

// Keep this much of LittleFS free so a pack (and the SRS state file and a
// download temp) can never fill it completely and strand the device. Shared
// with every other pack writer (NetFetch.h); the card has its own
// (reserveFor()).
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
// the decks folders and never stores a surprising filename.
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
// deck file name. https:// is deliberately rejected (see docs/decisions.md).
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

// Accuracy in percent over every answer ever given, from the lifetime counters
// (docs/srs.md section 2.1: forgotten records leave it as it was); 0 before the
// first answer.
uint32_t accuracyOf(const PortalStats& st) {
    if (st.answers == 0) return 0;
    const uint32_t right = st.answers > st.again ? st.answers - st.again : 0;
    return static_cast<uint32_t>(static_cast<uint64_t>(right) * 100u / st.answers);
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

// A POSIX TZ rule from the page ("<+01>-1<+02>,M3.5.0,M10.5.0/3"): at most
// kTzRuleMaxLen characters out of letters, digits and < > + - , . / :, else
// "". The app parses it (srs/TimeZone).
String sanitizeTzRule(const String& in) {
    if (in.length() > tgc::kTzRuleMaxLen) return String();
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        const bool ok = isalnum(static_cast<unsigned char>(c)) || std::strchr("<>+-,./:", c);
        if (!ok || c == '\0') return String();
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
// localised messages come from #clk's data attributes. Besides the offset of
// the moment (`tz`) it sends the zone as a POSIX TZ rule (`tzr`, kTzRuleJs,
// which closes the script) so the device follows daylight-saving changes; an
// error there sends none and the offset alone still works.
const char kTimeScript[] = R"JS(<script>
(function(){var d=new Date(),z='',r='',e=document.getElementById('clk');
try{z=Intl.DateTimeFormat().resolvedOptions().timeZone||''}catch(_){}
try{r=tzr(d.getTime())}catch(_){}
var x=new XMLHttpRequest();x.open('POST','/time');
x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
x.onload=function(){if(x.status!=200){e.textContent=e.dataset.bad;return}
e.textContent=e.dataset.ok+x.responseText+(z?' ('+z+')':'');
var t=document.getElementById('dt'),n=document.getElementById('dtn');
if(t)t.textContent=x.responseText;if(n)n.textContent=''};
x.onerror=function(){e.textContent=e.dataset.err};
x.send('ms='+d.getTime()+'&tz='+(-d.getTimezoneOffset())+'&zone='+encodeURIComponent(z)+
'&tzr='+encodeURIComponent(r))})();
)JS";

// tzr(now): the browser's time zone as a POSIX TZ rule, from the browser's own
// tzdata through getTimezoneOffset(). It finds this year's offset changes (a
// probe a day, then a binary search to the minute); none, or anything but one
// change each way, gives the fixed rule of the current offset ("<+09>-9").
// Otherwise the lower offset is standard time and each change is written as
// Mm.w.d/time in the local time before it, choosing among the readings of that
// instant (the day itself, the day before at 24:00+, the day after at a
// negative time; the nth weekday, or "the last" when it falls in the month's
// last seven days) the one that also gives most of the next five years'
// changes. Names are numeric ("<+01>", "<-0330>"); one hour of daylight time
// leaves its offset implicit. scripts/check_tz_rules.py runs this function
// under Node for many zones and checks the rules against tzdata.
const char kTzRuleJs[] = R"JS(function tzr(now){var D=864e5,M=6e4,y=new Date(now).getUTCFullYear(),a,c,F=[],i,s,d,u;
function off(t){return -new Date(t).getTimezoneOffset()}
function p2(n){return(n<10?'0':'')+n}
function nm(o){var a=Math.abs(o);return'<'+(o<0?'-':'+')+p2(a/60|0)+(a%60?p2(a%60):'')+'>'}
function po(o){var a=Math.abs(o);return(o>0?'-':'')+(a/60|0)+(a%60?':'+p2(a%60):'')}
function tr(y){var r=[],t=Date.UTC(y,0,1),b=Date.UTC(y+1,0,1),o=off(t),q,l,h,m;
for(;t<b;t+=D){q=off(t+D);if(q!=o){l=t;h=t+D;while(h-l>M){m=l+Math.floor((h-l)/2/M)*M;if(off(m)==o)l=m;else h=m}r.push([h,o,q]);o=q}}return r}
function at(r,y,o){var f=new Date(Date.UTC(y,r[0],1)).getUTCDay(),n=new Date(Date.UTC(y,r[0]+1,0)).getUTCDate(),d=1+(r[2]-f+7)%7+(r[1]-1)*7;
while(d>n)d-=7;return Date.UTC(y,r[0],d)+r[3]*1e3-o*M}
function tm(s){return s==7200?'':'/'+(s<0?'-':'')+(Math.abs(s)/3600|0)+(s%3600?':'+p2(Math.abs(s)/60%60|0):'')}
a=tr(y);c=off(now);
if(a.length!=2||a[0][1]!=a[1][2]||a[0][2]!=a[1][1])return nm(c)+po(c);
for(i=1;i<6;i++)F.push(tr(y+i));
s=Math.min(a[0][1],a[0][2]);d=s==a[0][1]?a[0][2]:a[0][1];u=nm(s)+po(s)+nm(d)+(d-s==60?'':po(d));
[s,d].forEach(function(o){var x=a[0][1]==o?a[0]:a[1],L=x[0]+o*M,b,top=-1;
[0,-1,1].forEach(function(k){var l=new Date(L+k*D),m=l.getUTCMonth(),e=l.getUTCDate(),
n=new Date(Date.UTC(l.getUTCFullYear(),m+1,0)).getUTCDate(),w=[Math.floor((e-1)/7)+1];
if(e+7>n)w.unshift(5);
w.forEach(function(v){var r=[m,v,l.getUTCDay(),L%D/1e3-k*86400],h=0;
F.forEach(function(f,j){f.forEach(function(z){if(z[1]==o&&z[0]==at(r,y+j+1,o))h++})});
if(h>top){top=h;b=r}})});
u+=',M'+(b[0]+1)+'.'+b[1]+'.'+b[2]+tm(b[3])});
return u}
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

// The portal's POST /progress temp: the v1 SRS size range and the structural
// checks of SrsStore::validateBytes() (magic, version, count, CRC and sorted
// codepoints). The record table is deliberately not built here: a validated
// file can be up to 131 KB, and on the no-PSRAM Cardputer a temporary SrsStore
// would need a second buffer of the same size. The app loads the file only
// after this check passes, and a failed allocation is a clean rejection.
bool validateProgressFile(const String& path) {
    if (!fsExists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const size_t n = f.size();
    if (n < kSrsMinFileSize || n > kSrsMaxFileSize) {
        f.close();
        return false;
    }
    auto* buf = static_cast<uint8_t*>(std::malloc(n));
    if (!buf) {
        f.close();
        return false;
    }
    const size_t got = f.read(buf, n);
    f.close();
    const bool ok = got == n && SrsStore::validateBytes(buf, n);
    std::free(buf);
    return ok;
}

// Every deck file of both places (the device's, then the card's, each sorted
// by name): the page lists them all, duplicates included, each with its place,
// so either copy can be deleted.
std::vector<DeckFile> allDeckFiles() {
    std::vector<DeckFile> out, card;
    storage::listFiles(DeckPlace::Device, out);
    storage::listFiles(DeckPlace::Card, card);
    out.insert(out.end(), card.begin(), card.end());
    return out;
}

// How many decks the device lists (one per file name, docs/microsd.md §2).
size_t deckCount() {
    std::vector<String> decks;
    storage::listDecks(decks);
    return decks.size();
}

// The state of catalog deck `e` against the installed copy the deck list shows
// (catalog/catalogDeckState): existence and exact size, then -- only when the
// size matches -- the file's SHA-256, so a same-size rebuild shows as an
// update on the portal too. A hash failure falls back to the size-only answer
// rather than claiming an update.
DeckState deckStateOnDisk(const CatalogEntry& e) {
    const String path = storage::installedPath(String(e.name.c_str()) + ".tgc");
    const long size = path.length() ? storage::fileSize(path) : -1;
    if (size < 0) return DeckState::New;
    const uint32_t localBytes = static_cast<uint32_t>(size);
    if (localBytes != e.bytes) return catalogDeckState(true, localBytes, e.bytes);
    uint8_t digest[32];
    return catalogDeckState(true, localBytes, e.bytes,
                            storage::sha256(path, digest) ? digest : nullptr, e.sha256);
}

// "28.1 GB" (the card's sizes).
String formatGbSize(uint64_t bytes) {
    char buf[16];
    return String(formatGb(bytes, '.', buf, sizeof buf)) + " GB";
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
    h += "<title>";
    h += credits::kAppName;
    h += " (";
    h += credits::kAppNameLatin;
    h += ")</title><style>";
    h += "body{font-family:system-ui,sans-serif;margin:1rem;max-width:44rem;line-height:1.45}";
    h += "table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:.3rem .3rem;";
    h += "border-bottom:1px solid #ccc;vertical-align:middle}td.r{text-align:right;white-space:nowrap}";
    h += "form{margin:.5rem 0}td form{margin:0}button{padding:.35rem .7rem}";
    h += "button.main{font-size:1.05rem;padding:.55rem 1rem}";
    h += "input[type=url]{width:100%;max-width:26rem;padding:.3rem}";
    h += "input[type=text],input[type=password]{width:100%;max-width:16rem;padding:.3rem;margin:.15rem 0}";
    h += "code{background:#eee;padding:0 .2rem}h1{font-size:1.25rem;margin-bottom:.2rem}";
    h += "h1 small{font-size:.75em;font-weight:400;color:#666}";
    h += "h2{font-size:1.05rem;margin:1.2rem 0 .3rem}h3{font-size:.95rem;margin:.9rem 0 .2rem}";
    h += "details{margin:1.2rem 0}summary{font-weight:600;cursor:pointer}progress{width:100%}";
    h += ".muted{color:#666}.err{color:#b00}.ok{color:#070}.status{margin-top:0}";
    h += "td.kanji{font-size:1.15rem;line-height:1.1}";
    h += "footer{margin-top:1.5rem;padding-top:.4rem;border-top:1px solid #ccc;font-size:.8rem;";
    h += "color:#666}";
    // The name in kana (the phone has Japanese fonts) and romanised.
    h += "</style></head><body><h1>";
    h += credits::kAppName;
    h += " <small>";
    h += credits::kAppNameLatin;
    h += "</small></h1>";
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

// The page footer: the firmware version and licence, the decks' licence and
// data credits (what Settings > About shows on the device, Credits.h) and the
// source repository.
String pageFooter(const PortalText& t) {
    auto credit = [](const char* c) {
        String s = c;
        s.replace("(c)", "\xC2\xA9");  // the built-in font's "(c)" as a copyright sign
        return s;
    };
    String h = "<footer>";
    h += fmt(t.footVersionFmt, htmlEscape(firmwareVersion()).c_str());
    h += " &middot; ";
    h += fmt(t.footDecksFmt, credit(credits::kKanjidic).c_str(), credit(credits::kKanjiVg).c_str(),
             credit(credits::kJlpt).c_str(), credit(credits::kGlyphs).c_str(),
             credit(credits::kReadings).c_str(), credit(credits::kMeanings).c_str());
    h += " &middot; <a href=\"https://";
    h += credits::kRepo;
    h += "\">";
    h += credits::kRepo;
    h += "</a></footer>";
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
    storage::makeDecksDir(storage::writePlace());

    scanNearby();
    WiFi.mode(WIFI_AP);
    // Stable SSID suffix from the AP MAC's last two bytes ("Tamagotcho-A1B2").
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (mac[4] == 0 && mac[5] == 0) {  // never expected; keep it unique-ish anyway
        mac[4] = 0xAB;
        mac[5] = 0xCD;
    }
    snprintf(ssid_, sizeof ssid_, "%s-%02X%02X", credits::kAppNameLatin, mac[4], mac[5]);
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
    storage::makeDecksDir(storage::writePlace());
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
    return (active_ && mode_ == Mode::Home && mdns_) ? kMdnsUrl : "";
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
    server_->on("/fwurl", HTTP_POST, [this]() { touch(); handleFirmwareUrl(); });
    server_->on("/getdecks", HTTP_POST, [this]() { touch(); handleGetDecks(); });
    server_->on("/home", HTTP_POST, [this]() { touch(); handleSwitchHome(); });
    server_->on("/deck", HTTP_POST, [this]() { touch(); handleDeck(); });
    server_->on("/upload", HTTP_POST, [this]() { touch(); handleUploadDone(); },
                [this]() { touch(); handleUploadChunk(); });
    // Read-only download and destructive restore of /state/srs.bin (Advanced).
    server_->on("/progress", HTTP_GET, [this]() { touch(); handleProgressDownload(); });
    server_->on("/progress", HTTP_POST, [this]() { touch(); handleProgressDone(); },
                [this]() { touch(); handleProgressChunk(); });
    server_->on("/time", HTTP_POST, [this]() { touch(); handleTime(); });
    // Every browser asks for it; unrouted, the core logs "[E] request handler
    // not found" for each page load before onNotFound answers.
    server_->on("/favicon.ico", HTTP_GET, [this]() { server_->send(204); });
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
    if (progressFile_) progressFile_.close();
    // A temp left by an interrupted upload is of no use to anyone.
    storage::removeIfExists(storage::deckPathFor(uploadPlace_, kUploadTmpName));
    fsRemoveIfExists(kProgressTmp);
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
    progressOk_ = false;
    progressSawFile_ = false;
    progressError_ = ProgressError::None;
    progressWritten_ = 0;
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

void WifiPortal::redirect(const char* where, Notice n, const String& arg, uint32_t count) {
    notice_ = n;
    noticeArg_ = arg;
    noticeCount_ = count;
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
        case Notice::ProgressRestored:
        case Notice::ProgressBad:
        case Notice::ProgressFailed:
        case Notice::ProgressMissing: return true;
        case Notice::FirmwareSaved:
        case Notice::FirmwareBad: return true;
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
        case Notice::Deleted:
            text = noticeCount_ ? fmt(t.deletedProgressFmt, arg.c_str(), (unsigned)noticeCount_)
                                : fmt(t.deletedFmt, arg.c_str());
            break;
        case Notice::DeleteFailed: text = t.deleteFailed; ok = false; break;
        case Notice::BadName: text = t.badName; ok = false; break;
        case Notice::UrlStarted: text = fmt(t.urlStartedFmt, arg.c_str()); break;
        case Notice::UrlBad: text = t.urlBad; ok = false; break;
        case Notice::Busy: text = t.busy; ok = false; break;
        case Notice::UnknownDeck: text = t.unknownDeck; ok = false; break;
        case Notice::ProgressRestored: text = t.progressRestored; break;
        case Notice::ProgressBad: text = t.progressBad; ok = false; break;
        case Notice::ProgressFailed: text = t.progressFailed; ok = false; break;
        case Notice::ProgressMissing: text = t.progressMissing; ok = false; break;
        case Notice::FirmwareSaved: text = t.fwUrlSaved; break;
        case Notice::FirmwareBad: text = t.fwUrlBad; ok = false; break;
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
    const size_t packs = deckCount();
    const String homeSsid = HomeWifi::savedSsid();
    const bool firstSetup = mode_ == Mode::Setup && homeSsid.isEmpty();
    const bool catalogBusy =
        catState_ == CatalogState::Pending || catState_ == CatalogState::Loading;
    const bool advOpen = noticeInAdvanced(notice_, firstSetup);  // before appendNotice() clears it

    String h;
    h.reserve(12 * 1024);
    h += pageHeader(t, catalogBusy ? kCatalogRefreshS : 0);

    // Status line: decks, the free space where new ones go (the card while one
    // is mounted), which network this page is on.
    h += "<p class=\"status muted\">";
    if (packs == 0)
        h += t.decksNone;
    else if (packs == 1)
        h += t.decksOne;
    else
        h += fmt(t.decksFmt, (unsigned)packs);
    h += " &middot; ";
    if (storage::cardMounted())
        h += fmt(t.cardFreeFmt, formatGbSize(storage::cardFreeBytes()).c_str());
    else
        h += fmt(t.freeFmt, formatSize(storage::freeBytes(DeckPlace::Device)).c_str());
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
    // A card that did not mount or went away (docs/microsd.md §3.1, §3.5).
    if (storage::cardState() == storage::CardState::Unreadable ||
        storage::cardState() == storage::CardState::Removed) {
        h += "<p class=\"err\">";
        h += storage::cardState() == storage::CardState::Removed ? t.cardRemoved
                                                                 : t.cardUnreadable;
        h += "</p>";
    }
    appendNotice(h);
    // A newer firmware the device found: said here, installed on the device.
    if (st.fwAvailable[0]) {
        h += "<p class=\"ok\">";
        h += fmt(t.fwAvailableFmt, htmlEscape(st.fwAvailable).c_str());
        h += "</p>";
    }

    if (mode_ == Mode::Setup)
        appendSetupDecks(h, homeSsid, firstSetup);
    else
        appendHomeDecks(h, st);
    appendYourDecks(h, st);
    if (!firstSetup) appendStats(h, st);
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
    h += pageFooter(t);
    h += kTimeScript;
    h += kTzRuleJs;
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
    std::vector<DeckState> states;
    states.reserve(rows.size());
    h += "<table>";
    const String loadedFile = fileNameOf(st.loadedPack).c_str();
    for (const CatalogEntry& e : rows) {
        const String name = htmlEscape(e.name.c_str());
        const DeckState ds = deckStateOnDisk(e);
        states.push_back(ds);
        // The deck's title, then the file it is saved as (what "Your decks"
        // lists) before its language, cards and size.
        h += "<tr><td><b>";
        h += htmlEscape(catalogDisplayName(e).c_str());
        h += "</b><br><span class=\"muted\">";
        if (!e.title.empty()) {
            h += name;
            h += ".tgc &middot; ";
        }
        h += fmt(t.deckInfoFmt, htmlEscape(e.lang.empty() ? "?" : e.lang.c_str()).c_str(),
                 (unsigned)e.cards, formatSize(e.bytes).c_str());
        h += "</span></td><td class=\"r\">";
        if (ds == DeckState::OnDevice) {
            h += "<span class=\"ok\">";
            h += t.onDevice;
            if (loadedFile == String(e.name.c_str()) + ".tgc") {
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
    if (hooks_.onCatalog) hooks_.onCatalog(rows, states);
}

// Your decks: every deck file, with its place on a board with a microSD slot
// ("card" or "device", docs/microsd.md §3.7), and a delete for each.
void WifiPortal::appendYourDecks(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    h += "<h2 id=\"yours\">";
    h += t.yoursTitle;
    h += "</h2><table>";
    const std::vector<DeckFile> packs = allDeckFiles();
    const bool places = storage::hasSlot();
    for (const DeckFile& p : packs) {
        const String esc = htmlEscape(p.name.c_str());
        const bool card = p.place == DeckPlace::Card;
        h += "<tr><td>";
        h += esc;
        if (places) {
            h += " <span class=\"muted\">&middot; ";
            h += card ? t.placeCard : t.placeDevice;
            h += "</span>";
        }
        if (p.path == st.loadedPack) {
            h += " <span class=\"ok\">&middot; ";
            h += t.studying;
            h += "</span>";
        }
        h += "</td><td class=\"r\">";
        h += formatSize(p.bytes);
        h += "</td><td class=\"r\"><form method=\"post\" action=\"/delete\" data-q=\"";
        h += htmlEscape(t.deleteConfirm);
        h += "\" onsubmit=\"return confirm(this.dataset.q)\"><input type=\"hidden\" name=\"name\" value=\"";
        h += esc;
        h += "\"><input type=\"hidden\" name=\"where\" value=\"";
        h += card ? "card" : "device";
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

// The study statistics, in the open after the decks: they were under Advanced
// until the 2026-10-19 hardware test found them hidden there.
void WifiPortal::appendStats(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    const uint32_t acc = accuracyOf(st);
    h += "<h2 id=\"stats\">";
    h += t.statsTitle;
    h += "</h2><ul><li>";
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
    h += "</li></ul>";
    appendTrouble(h, st);
}

// The per-kanji accuracy view (roadmap "Statistics"): the cards the learner
// gets wrong most often, worst first, from SrsStore::trouble(). The page only
// carries the worst few, so it stays small on the device; the card's own
// counters are the ones the detail card shows.
void WifiPortal::appendTrouble(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    h += "<h3>";
    h += t.statTrouble;
    h += "</h3><p class=\"muted\">";
    h += t.statTroubleNote;
    h += "</p>";
    if (st.troubleCount == 0) {
        h += "<p class=\"muted\">";
        h += t.statTroubleEmpty;
        h += "</p>";
        return;
    }
    h += "<table><tr><th></th><th class=\"r\">";
    h += t.statRight;
    h += "</th><th class=\"r\">";
    h += t.statReviews;
    h += "</th><th class=\"r\">";
    h += t.statAccuracy;
    h += "</th></tr>";
    for (uint8_t i = 0; i < st.troubleCount && i < PortalStats::kTroubleMax; ++i) {
        const PortalStats::TroubleKanji& k = st.trouble[i];
        const unsigned right = k.reps > k.lapses ? k.reps - k.lapses : 0;
        const unsigned pct = k.reps ? right * 100u / k.reps : 0;
        h += "<tr><td class=\"kanji\">";
        h += htmlEscape(k.kanji);
        h += "</td><td class=\"r\">";
        h += String(right);
        h += "</td><td class=\"r\">";
        h += String((unsigned)k.reps);
        h += "</td><td class=\"r\">";
        h += String(pct);
        h += "%</td></tr>";
    }
    h += "</table>";
}

// Everything that is not the main task, collapsed: upload, URL download, the
// catalog URL, the firmware (its version and manifest URL), the progress
// backup/restore, the home Wi-Fi, the free space and the JSON endpoints.
// Opened when the last action was one of these, so its result is in view.
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

    appendFirmware(h, st);

    // Progress backup: the download is read-only; a restore replaces every
    // deck's progress, so it asks first and the app keeps the replaced file as
    // a safety copy. The page posts its clock on the next load, which aligns
    // the restored day index (docs/srs.md section 4).
    h += "<h3 id=\"progress\">";
    h += t.progressTitle;
    h += "</h3><form method=\"get\" action=\"/progress\"><button type=\"submit\">";
    h += t.progressDownloadButton;
    h += "</button></form>";
    if (hooks_.onRestoreProgress) {
        h += "<form method=\"post\" action=\"/progress\" enctype=\"multipart/form-data\" data-q=\"";
        h += htmlEscape(t.progressConfirm);
        h += "\" onsubmit=\"return confirm(this.dataset.q)\"><input type=\"file\" name=\"progress\" ";
        h += "accept=\".bin\" required> <button type=\"submit\">";
        h += t.progressRestoreButton;
        h += "</button></form>";
    }
    h += "<p class=\"muted\">";
    h += t.progressNote;
    h += " ";
    h += t.progressSafetyNote;
    if (storage::cardMounted()) {
        h += " ";
        h += t.progressCardNote;
    }
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

    h += "<p>";
    h += fmt(t.freeSpaceFmt, formatSize(storage::freeBytes(DeckPlace::Device)).c_str(),
             formatSize(LittleFS.totalBytes()).c_str());
    if (storage::cardMounted()) {
        h += "<br>";
        h += fmt(t.cardSpaceFmt, formatGbSize(storage::cardFreeBytes()).c_str(),
                 formatGbSize(storage::cardTotalBytes()).c_str());
    }
    h += "</p><p class=\"muted\">JSON: <a href=\"/api/stats\">/api/stats</a> &middot; ";
    h += "<a href=\"/api/status\">/api/status</a></p></details>";
}

// Advanced > Firmware: the version this device runs (and its build), the
// newer one when the device found it or why it does not update itself, and the
// manifest URL (docs/ota.md §3.8: http:// allowed there, for testing).
void WifiPortal::appendFirmware(String& h, const PortalStats& st) {
    const PortalText& t = portalText(pageLang());
    h += "<h3 id=\"fw\">";
    h += t.fwTitle;
    h += "</h3><p>";
    h += fmt(t.fwRunningFmt, htmlEscape(firmwareVersion()).c_str(),
             htmlEscape(ota::buildEnv()).c_str());
    if (st.fwAvailable[0]) {
        h += " ";
        h += fmt(t.fwAvailableFmt, htmlEscape(st.fwAvailable).c_str());
    }
    h += "</p>";
    if (!st.fwOta) {
        h += "<p class=\"muted\">";
        h += t.fwNoOta;
        h += "</p>";
        return;
    }
    h += "<form method=\"post\" action=\"/fwurl\"><input type=\"url\" name=\"url\" ";
    h += "maxlength=\"256\" value=\"";
    h += htmlEscape(HomeWifi::firmwareUrlOverride());
    h += "\" placeholder=\"";
    h += t.fwUrlPlaceholder;
    h += "\"> <button type=\"submit\">";
    h += t.save;
    h += "</button></form><p class=\"muted\">";
    h += fmt(t.fwUrlDefaultFmt,
             (String("<code>") + HomeWifi::kDefaultFirmwareUrl + "</code>").c_str());
    h += "</p>";
}

String WifiPortal::catalogProblem() const {
    const PortalText& t = portalText(pageLang());
    switch (catFetch_.status) {
        case FetchStatus::Ok:
            if (catError_ != CatalogError::UnsupportedVersion) return t.catalogBad;
            return catVersion_ < kCatalogVersion ? t.catalogOlder : t.catalogNewer;
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
    const uint32_t acc = accuracyOf(st);

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
    j += ",\"again\":";
    j += String((unsigned)st.again);
    j += ",\"reps\":";
    j += String((unsigned)st.reps);
    j += ",\"lapses\":";
    j += String((unsigned)st.lapses);
    j += ",\"accuracy\":";
    j += String((unsigned)acc);
    j += ",\"trouble\":[";
    for (uint8_t i = 0; i < st.troubleCount && i < PortalStats::kTroubleMax; ++i) {
        const PortalStats::TroubleKanji& k = st.trouble[i];
        const unsigned right = k.reps > k.lapses ? k.reps - k.lapses : 0;
        const unsigned pct = k.reps ? right * 100u / k.reps : 0;
        if (i > 0) j += ",";
        j += "{\"kanji\":\"";
        j += jsonEscape(k.kanji);
        j += "\",\"codepoint\":";
        j += String((unsigned)k.codepoint);
        j += ",\"reps\":";
        j += String((unsigned)k.reps);
        j += ",\"lapses\":";
        j += String((unsigned)k.lapses);
        j += ",\"accuracy\":";
        j += String(pct);
        j += "}";
    }
    j += "]";
    j += ",\"freeBytes\":";
    j += String((unsigned)freeB);
    j += ",\"totalBytes\":";
    j += String((unsigned)total);
    // The microSD card: "none" on a board without a slot (docs/microsd.md).
    const char* card = "none";
    switch (storage::cardState()) {
        case storage::CardState::NoSlot: card = "none"; break;
        case storage::CardState::None: card = "empty"; break;
        case storage::CardState::Unreadable: card = "unreadable"; break;
        case storage::CardState::Mounted: card = "mounted"; break;
        case storage::CardState::Removed: card = "removed"; break;
    }
    j += ",\"card\":{\"state\":\"";
    j += card;
    j += "\",\"freeBytes\":";
    j += String(static_cast<double>(storage::cardFreeBytes()), 0);
    j += ",\"totalBytes\":";
    j += String(static_cast<double>(storage::cardTotalBytes()), 0);
    j += "},\"packs\":[";
    bool first = true;
    for (const DeckFile& p : allDeckFiles()) {
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"";
        j += jsonEscape(p.name.c_str());
        j += "\",\"size\":";
        j += String((unsigned)p.bytes);
        j += ",\"place\":\"";
        j += p.place == DeckPlace::Card ? "card" : "device";
        j += "\"}";
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
// UTC, -getTimezoneOffset()), an optional `zone` (IANA name, for messages and
// the device's "From phone" row) and an optional `tzr` (the zone as a POSIX TZ
// rule, see kTzRuleJs; pages from before it send none). The page posts it on
// every load; posting the same clock again is harmless. The portal only checks
// the syntax: whether the time and the rule are plausible, and what they mean
// for the SRS day, is the app's call (hooks_.onTime, docs/srs.md 5.1).
void WifiPortal::handleTime() {
    int64_t ms = 0, tz = 0;
    if (!parseDecimal(server_->arg("ms"), &ms) || !parseDecimal(server_->arg("tz"), &tz) ||
        tz < -24 * 60 || tz > 24 * 60) {
        server_->send(400, "text/plain", "bad time");
        return;
    }
    const String zone = sanitizeZone(server_->arg("zone"));
    const String rule = sanitizeTzRule(server_->arg("tzr"));
    if (!hooks_.onTime ||
        !hooks_.onTime(ms, static_cast<int16_t>(tz), rule.c_str(), zone.c_str())) {
        setMsg("browser clock rejected");
        server_->send(400, "text/plain", "time rejected");
        return;
    }
    PortalStats st;
    if (hooks_.fillStats) hooks_.fillStats(st);
    if (!deckInFlight()) setMsg("date set %s%s%s", st.date, zone.length() ? " " : "", zone.c_str());
    Serial.printf("[wifi] date from browser: %s (tz %+d min, zone %s, rule %s)\n", st.date,
                  (int)tz, zone.length() ? zone.c_str() : "-", rule.length() ? rule.c_str() : "-");
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
    // The app deletes the file and the progress that goes with it, and rescans
    // the decks (docs/srs.md section 2.1); without that hook, only the file.
    // `where` says which copy: the card's or the device's (the default).
    const DeckPlace place = server_->arg("where") == "card" ? DeckPlace::Card : DeckPlace::Device;
    const String path = storage::deckPathFor(place, name);
    uint32_t forgotten = 0;
    bool ok;
    if (hooks_.onDeleteDeck) {
        ok = hooks_.onDeleteDeck(path.c_str(), &forgotten);
    } else {
        ok = storage::removeFile(path);
        if (hooks_.onPacksChanged) hooks_.onPacksChanged();
    }
    Serial.printf("[wifi] delete %s: %s, progress of %u kanji forgotten\n", name.c_str(),
                  ok ? "ok" : "failed", (unsigned)forgotten);
    setMsg(ok ? "deleted %s" : "delete failed", name.c_str());
    redirect("/#yours", ok ? Notice::Deleted : Notice::DeleteFailed, name, forgotten);
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

// POST /fwurl: the firmware manifest URL ("" = the default). Home mode reads
// the catalog again, and with it the manifest from the new URL, so the next
// page shows what it offers (unless a download runs).
void WifiPortal::handleFirmwareUrl() {
    if (!HomeWifi::saveFirmwareUrl(server_->arg("url"))) {
        redirect("/#fw", Notice::FirmwareBad);
        return;
    }
    setMsg("firmware URL saved");
    if (mode_ == Mode::Home && hooks_.onDeckDownload && !deckInFlight())
        catState_ = CatalogState::Pending;
    redirect("/#fw", Notice::FirmwareSaved);
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
    const DeckState ds = deckStateOnDisk(*e);
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
        b += fmt(t.switchMdnsFmt, kMdnsUrl, kMdnsUrl);
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
    storage::removeIfExists(storage::deckPathFor(uploadPlace_, kUploadTmpName));
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
            // One writer at a time: a download is storing a deck. (The server
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
            // To the card while one is mounted, else LittleFS (docs/microsd.md
            // D1). Discard a temp left by an earlier run before measuring the
            // space.
            uploadPlace_ = storage::writePlace();
            const String tmp = storage::deckPathFor(uploadPlace_, kUploadTmpName);
            storage::removeIfExists(tmp);
            const size_t freeB = storage::freeBytes(uploadPlace_);
            const size_t reserve = static_cast<size_t>(reserveFor(uploadPlace_));
            // `up.totalSize` is always 0 here (this WebServer core only fills it
            // on a buffer flush), so use the request's declared Content-Length.
            // 0 means "unknown/chunked" -- then only the write-time cap applies.
            const int declared = server_->clientContentLength();
            const size_t bodyLen = declared > 0 ? static_cast<size_t>(declared) : 0;
            if (bodyLen > kMaxPackBytes + kMultipartSlack) {
                uploadError_ = UploadError::TooLarge;
                return;
            }
            if (bodyLen && bodyLen + reserve > freeB) {
                uploadError_ = UploadError::NoSpace;
                return;
            }
            storage::makeDecksDir(uploadPlace_);
            uploadName_ = name;
            uploadFree_ = freeB > reserve ? freeB - reserve : 0;
            uploadFile_ = storage::openFile(tmp, "w");
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
            if (uploadWritten_ + n > uploadFree_ || uploadWritten_ + n > kMaxPackBytes) {
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
            const String tmp = storage::deckPathFor(uploadPlace_, kUploadTmpName);
            if (uploadWritten_ < 4 || !validatePackFile(tmp)) {
                storage::removeIfExists(tmp);
                uploadError_ = UploadError::Invalid;
                Serial.printf("[wifi] upload rejected %s (%u bytes)\n", uploadName_.c_str(),
                              (unsigned)uploadWritten_);
                break;
            }
            // Replacing the loaded deck (in either place): the app closes its
            // file first, or neither the remove nor the rename could touch it.
            // A FAT rename does not overwrite: the old file goes first.
            const String dst = storage::deckPathFor(uploadPlace_, uploadName_);
            const bool closed = hooks_.onBeforeReplace && hooks_.onBeforeReplace(dst.c_str());
            const bool hadDst = storage::removeIfExists(dst);
            if (!storage::renameFile(tmp, dst)) {
                storage::removeIfExists(tmp);
                uploadError_ = UploadError::CannotStore;
                Serial.printf("[wifi] upload rename failed %s\n", uploadName_.c_str());
                // The old pack is gone, or the app closed it: let it rescan
                // rather than keep a reader on a missing file or no deck.
                if ((hadDst || closed) && hooks_.onPacksChanged) hooks_.onPacksChanged();
                break;
            }
            // The new copy is the one to list: the other place's goes.
            storage::removeOtherCopy(dst);
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

// --- progress backup / restore ---------------------------------------------

// GET /progress: the state file as a download. Read-only, and available with
// no deck open (progress is global); no file yet is a notice, not an error.
// If a power cut interrupted a save, the app loaded the `.bak` fallback and
// `srs.bin` may be gone: serve that one. The file is not loaded into RAM here:
// a download must also work for a maximum-size state on the no-PSRAM Cardputer.
void WifiPortal::handleProgressDownload() {
    const char* path = FsSrsFile::kPath;
    if (!fsExists(path)) path = FsSrsFile::kBak;
    if (!fsExists(path)) {
        redirect("/#adv", Notice::ProgressMissing);
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        redirect("/#adv", Notice::ProgressMissing);
        return;
    }
    setMsg("sending progress...");
    server_->sendHeader("Content-Disposition", "attachment; filename=\"tangocho-progress.bin\"",
                        true);
    server_->streamFile(f, "application/octet-stream", 200);
    f.close();
}

// A restore upload failed: only the temp goes, never the live state file.
void WifiPortal::abortProgress(ProgressError why) {
    if (progressFile_) progressFile_.close();
    fsRemoveIfExists(kProgressTmp);
    progressOk_ = false;
    progressSawFile_ = false;  // the connection is gone: the next POST starts clean
    if (progressError_ == ProgressError::None) progressError_ = why;
    Serial.printf("[wifi] progress restore failed: error %u\n", (unsigned)why);
}

// POST /progress, multipart field "progress": stream the body to kProgressTmp,
// validate it with SrsStore::validateBytes() and only then hand it to the app
// (Hooks::onRestoreProgress), which keeps the state it replaces. A dropped,
// oversized or invalid upload leaves /state/srs.bin untouched.
void WifiPortal::handleProgressChunk() {
    HTTPUpload& up = server_->upload();
    switch (up.status) {
        case UPLOAD_FILE_START: {
            if (progressSawFile_) break;  // one file per form
            progressSawFile_ = true;
            progressWritten_ = 0;
            progressOk_ = false;
            progressError_ = ProgressError::None;
            // A deck download owns the radio and the decks; a restore must wait.
            if (anyDownload() || leave_ != Leave::None) {
                progressError_ = ProgressError::Busy;
                break;
            }
            if (!hooks_.onRestoreProgress) {
                progressError_ = ProgressError::CannotStore;
                break;
            }
            if (up.name != "progress") {
                progressError_ = ProgressError::BadField;
                break;
            }
            // Discard a temp left by an earlier run before measuring the space.
            fsRemoveIfExists(kProgressTmp);
            const int declared = server_->clientContentLength();
            const size_t bodyLen = declared > 0 ? static_cast<size_t>(declared) : 0;
            if (bodyLen > kProgressMaxBytes + kMultipartSlack) {
                progressError_ = ProgressError::TooLarge;
                break;
            }
            const size_t total = LittleFS.totalBytes();
            const size_t used = LittleFS.usedBytes();
            const size_t freeB = total > used ? total - used : 0;
            if (bodyLen && bodyLen + kFsReserveBytes > freeB) {
                progressError_ = ProgressError::NoSpace;
                break;
            }
            LittleFS.mkdir(FsSrsFile::kDir);
            progressFree_ = freeB;
            progressFile_ = LittleFS.open(kProgressTmp, "w");
            if (!progressFile_) {
                progressError_ = ProgressError::CannotCreate;
                break;
            }
            progressOk_ = true;
            break;
        }
        case UPLOAD_FILE_WRITE: {
            if (!progressOk_) break;
            const size_t n = up.currentSize;
            if (progressWritten_ + n > kProgressMaxBytes) {
                abortProgress(ProgressError::TooLarge);
                break;
            }
            if (progressWritten_ + n + kFsReserveBytes > progressFree_) {
                abortProgress(ProgressError::NoSpace);
                break;
            }
            const size_t w = progressFile_.write(up.buf, n);
            progressWritten_ += w;
            if (w != n) abortProgress(ProgressError::WriteFailed);
            break;
        }
        case UPLOAD_FILE_END: {
            if (!progressOk_) break;
            progressFile_.close();
            progressOk_ = false;
            if (progressWritten_ < kSrsMinFileSize || progressWritten_ > kProgressMaxBytes ||
                !validateProgressFile(kProgressTmp)) {
                fsRemoveIfExists(kProgressTmp);
                progressError_ = ProgressError::Invalid;
                Serial.printf("[wifi] progress rejected (%u bytes)\n",
                              (unsigned)progressWritten_);
                break;
            }
            if (!hooks_.onRestoreProgress(kProgressTmp)) {
                fsRemoveIfExists(kProgressTmp);
                progressError_ = ProgressError::CannotStore;
                Serial.printf("[wifi] progress restore refused by the app\n");
                break;
            }
            Serial.printf("[wifi] progress restored from %u bytes\n", (unsigned)progressWritten_);
            break;
        }
        case UPLOAD_FILE_ABORTED:
            abortProgress(ProgressError::Aborted);
            break;
        default:
            break;
    }
}

// The result goes to Advanced, where the forms are; the next page load also
// posts the browser's clock, which re-aligns the restored day index.
void WifiPortal::handleProgressDone() {
    ProgressError e = progressError_;
    if (e == ProgressError::None && !progressSawFile_) e = ProgressError::BadField;
    if (progressFile_) progressFile_.close();
    progressOk_ = false;
    progressError_ = ProgressError::None;
    progressSawFile_ = false;
    progressWritten_ = 0;
    switch (e) {
        case ProgressError::None:
            setMsg("progress restored");
            redirect("/#adv", Notice::ProgressRestored);
            return;
        case ProgressError::Busy:
            setMsg("busy: progress not restored");
            redirect("/#adv", Notice::Busy);
            return;
        case ProgressError::BadField:
        case ProgressError::TooLarge:
        case ProgressError::Invalid:
        case ProgressError::Aborted:
            setMsg("progress not restored");
            redirect("/#adv", Notice::ProgressBad);
            return;
        default:
            setMsg("progress not restored");
            redirect("/#adv", Notice::ProgressFailed);
            return;
    }
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
    redirect("/#yours", Notice::UrlStarted, storage::deckPathFor(storage::writePlace(), name));
}

bool WifiPortal::doFetch(const String& url) {
    String host, path, name;
    uint16_t port = 0;
    if (!parseHttpUrl(url, host, port, path, name)) {
        setMsg("bad URL");
        return false;
    }
    // The shared atomic download (NetFetch.h): `.fetch.tmp` next to the
    // destination (the card while one is mounted, else /packs), validated,
    // then renamed over it.
    PackFetchRequest req;
    req.url = url;
    req.fileName = name;
    req.maxBytes = kMaxPackBytes;
    req.idleTimeoutMs = kFetchIdleMs;
    req.totalTimeoutMs = kFetchTotalMs;
    // Replacing the loaded deck: the app closes its file first, as Get decks
    // does. The download has no progress callback, so nothing on the device
    // draws from the closed deck before the rescan below reopens it.
    const DeckPlace place = storage::writePlace();
    const String dst = storage::deckPathFor(place, name);
    const bool closed = hooks_.onBeforeReplace && hooks_.onBeforeReplace(dst.c_str());
    const FetchResult r = fetchPackFile(req, place);
    // The decks changed (on success, or a failed rename after the old pack was
    // removed), or the app closed the loaded deck: let it rescan, which reopens
    // the deck rather than keep a reader on a file that is gone, or none.
    if ((r.packsChanged || closed) && hooks_.onPacksChanged) hooks_.onPacksChanged();
    switch (r.status) {
        case FetchStatus::Ok:
            setMsg("downloaded %s (%u KB)", name.c_str(), (unsigned)(r.bytes / 1024));
            Serial.printf("[wifi] fetch ok %s %u bytes\n", name.c_str(), (unsigned)r.bytes);
            return true;
        case FetchStatus::RenameFailed:
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
    catVersion_ = c.parse.version;
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
    // The firmware manifest on the same connection (the app's, docs/ota.md §3.3).
    if (hooks_.onCatalogFetched) hooks_.onCatalogFetched(c.fetch);
    touch();  // the reads may have taken a while
}

bool WifiPortal::validatePackFile(const String& path) const { return tgc::validatePackFile(path); }

}  // namespace tgc
