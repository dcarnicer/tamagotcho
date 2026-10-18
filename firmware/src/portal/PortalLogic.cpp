// Pure helpers for the Wi-Fi portal; see PortalLogic.h.
#include "portal/PortalLogic.h"

#include <cstring>

namespace tgc {

namespace {

// Byte-mode capacity at ECC level L, versions 1..10 (ISO/IEC 18004 table 7).
constexpr size_t kQrBytesL[] = {17, 32, 53, 78, 106, 134, 154, 192, 230, 271};
constexpr int kQrMaxVersion = static_cast<int>(sizeof kQrBytesL / sizeof kQrBytesL[0]);
constexpr size_t kMaxTagLen = 15;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isTagChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// "en" or "es" when `tag`'s primary subtag (before '-' or '_') is one of them,
// ignoring case; nullptr otherwise.
const char* pageLangOf(const char* tag, size_t n) {
    size_t p = 0;
    while (p < n && tag[p] != '-' && tag[p] != '_') ++p;
    if (p != 2) return nullptr;
    const char a = lower(tag[0]), b = lower(tag[1]);
    if (a == 'e' && b == 'n') return "en";
    if (a == 'e' && b == 's') return "es";
    return nullptr;
}

// One Accept-Language entry: the tag and its weight in thousandths (q=1 ->
// 1000). False for "*", an empty or over-long tag, a character outside
// [A-Za-z0-9-], or a malformed q.
bool parseEntry(const char* s, size_t n, std::string* tag, int* weight) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    const size_t start = i;
    while (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t') ++i;
    const size_t len = i - start;
    if (len == 0 || len > kMaxTagLen) return false;
    for (size_t k = start; k < i; ++k)
        if (!isTagChar(s[k])) return false;  // also refuses "*"
    int w = 1000;
    // Parameters: only q matters ("q=0.8"); anything else is ignored.
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ';')) ++i;
        if (i + 1 < n && (s[i] == 'q' || s[i] == 'Q') && s[i + 1] == '=') {
            i += 2;
            if (i >= n || (s[i] != '0' && s[i] != '1')) return false;
            int v = (s[i] - '0') * 1000;
            ++i;
            if (i < n && s[i] == '.') {
                ++i;
                int scale = 100;
                while (i < n && s[i] >= '0' && s[i] <= '9') {
                    if (scale > 0) v += (s[i] - '0') * scale;
                    scale /= 10;
                    ++i;
                }
            }
            if (v > 1000) return false;
            w = v;
        } else {
            while (i < n && s[i] != ';') ++i;
        }
    }
    tag->assign(s + start, len);
    *weight = w;
    return true;
}

// Calls `fn(tag, weight)` for every usable entry of an Accept-Language value.
template <typename Fn>
void forEachEntry(const char* header, Fn&& fn) {
    if (!header) return;
    const char* p = header;
    while (*p) {
        const char* comma = std::strchr(p, ',');
        const size_t n = comma ? static_cast<size_t>(comma - p) : std::strlen(p);
        std::string tag;
        int weight = 0;
        if (parseEntry(p, n, &tag, &weight) && weight > 0) fn(tag, weight);
        if (!comma) break;
        p = comma + 1;
    }
}

}  // namespace

std::string wifiQrEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

std::string wifiQrOpenNetwork(const std::string& ssid) {
    return "WIFI:T:nopass;S:" + wifiQrEscape(ssid) + ";;";
}

int qrVersionForBytes(size_t len) {
    for (int v = 0; v < kQrMaxVersion; ++v)
        if (len <= kQrBytesL[v]) return v + 1;
    return 0;
}

std::string preferredLanguage(const char* acceptLanguage) {
    std::string best;
    int bestWeight = 0;
    forEachEntry(acceptLanguage, [&](const std::string& tag, int weight) {
        if (weight > bestWeight) {  // strictly greater: the first one wins a tie
            best = tag;
            bestWeight = weight;
        }
    });
    return best;
}

const char* portalPageLang(const char* deviceLang, const char* hint, const char* acceptLanguage) {
    if (deviceLang && *deviceLang) {
        const char* l = pageLangOf(deviceLang, std::strlen(deviceLang));
        return l ? l : "en";
    }
    if (hint && *hint) {
        const char* l = pageLangOf(hint, std::strlen(hint));
        if (l) return l;
    }
    const char* best = nullptr;
    int bestWeight = 0;
    forEachEntry(acceptLanguage, [&](const std::string& tag, int weight) {
        const char* l = pageLangOf(tag.data(), tag.size());
        if (l && weight > bestWeight) {
            best = l;
            bestWeight = weight;
        }
    });
    return best ? best : "en";
}

std::string catalogOrderLang(const char* deviceLang, const char* hint, const char* acceptLanguage) {
    if (deviceLang && *deviceLang) return deviceLang;
    if (hint && *hint) return hint;
    return preferredLanguage(acceptLanguage);
}

int rssiQuality(int dBm) {
    if (dBm >= -55) return 3;
    if (dBm >= -67) return 2;
    if (dBm >= -75) return 1;
    return 0;
}

}  // namespace tgc
