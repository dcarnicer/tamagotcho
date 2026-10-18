// Deck catalog parser. Pure C++; see Catalog.h for the format and the rules.
#include "catalog/Catalog.h"

#include <cstring>

namespace tgc {

namespace {

constexpr char kTag[] = "#tangocho-catalog";
constexpr size_t kTagLen = sizeof(kTag) - 1;
constexpr size_t kFields = 6;  // name, lang, cards, bytes, sha256, url

struct Field {
    const char* p;
    size_t n;
};

bool isBlank(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return false;
    return true;
}

// Plain decimal digits only (no sign, no spaces), fitting in 32 bits.
bool parseU32(const char* s, size_t n, uint32_t* out) {
    if (n == 0 || n > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + static_cast<uint64_t>(s[i] - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#tangocho-catalog<TAB><digits>". False when the line is not the tag line.
bool parseTag(const char* s, size_t n, int* version) {
    if (n < kTagLen + 2 || std::memcmp(s, kTag, kTagLen) != 0 || s[kTagLen] != '\t') return false;
    uint32_t v = 0;
    if (!parseU32(s + kTagLen + 1, n - kTagLen - 1, &v) || v > 9999u) return false;
    *version = static_cast<int>(v);
    return true;
}

bool langIsSafe(const char* s, size_t n) {
    if (n > kCatalogMaxLangLen) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// One record line (trailing CR/spaces already trimmed) -> entry. False if any
// field is missing, extra or invalid.
bool parseRecord(const char* line, size_t n, const std::string& base, CatalogEntry* e) {
    Field f[kFields];
    size_t nf = 0, start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && line[i] != '\t') continue;
        if (nf == kFields) return false;  // a seventh field
        f[nf++] = {line + start, i - start};
        start = i + 1;
    }
    if (nf != kFields) return false;

    if (!catalogNameIsSafe(f[0].p, f[0].n)) return false;
    if (!langIsSafe(f[1].p, f[1].n)) return false;
    uint32_t cards = 0, bytes = 0;
    if (!parseU32(f[2].p, f[2].n, &cards) || cards == 0 || cards > 65535u) return false;
    if (!parseU32(f[3].p, f[3].n, &bytes) || bytes < kCatalogMinPackBytes ||
        bytes > kCatalogMaxPackBytes)
        return false;
    uint8_t digest[32];
    if (!parseSha256Hex(f[4].p, f[4].n, digest)) return false;
    std::string url;
    if (!resolveCatalogUrl(base, std::string(f[5].p, f[5].n), &url)) return false;

    e->name.assign(f[0].p, f[0].n);
    e->lang.assign(f[1].p, f[1].n);
    e->cards = cards;
    e->bytes = bytes;
    std::memcpy(e->sha256, digest, sizeof digest);
    e->url = std::move(url);
    return true;
}

// Length of "http://" / "https://", or 0 for any other scheme.
size_t schemeLen(const std::string& url) {
    if (url.compare(0, 7, "http://") == 0) return 7;
    if (url.compare(0, 8, "https://") == 0) return 8;
    return 0;
}

}  // namespace

bool catalogNameIsSafe(const char* name, size_t len) {
    if (!name || len == 0 || len > kCatalogMaxNameLen) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
        if (c == '.' && i + 1 < len && name[i + 1] == '.') return false;
    }
    return true;
}

bool parseSha256Hex(const char* hex, size_t len, uint8_t out[32]) {
    if (!hex || len != 64) return false;
    uint8_t tmp[32];
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hexNibble(hex[2 * i]);
        const int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        tmp[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    std::memcpy(out, tmp, sizeof tmp);
    return true;
}

bool isHttpUrl(const std::string& url) {
    const size_t s = schemeLen(url);
    if (s == 0 || url.size() > kCatalogMaxUrlLen) return false;
    for (char c : url) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F) return false;
    }
    // Host (with an optional ":port") up to the path, query or fragment.
    size_t end = url.find_first_of("/?#", s);
    if (end == std::string::npos) end = url.size();
    const size_t colon = url.find(':', s);
    const size_t hostEnd = (colon != std::string::npos && colon < end) ? colon : end;
    return hostEnd > s;
}

bool resolveCatalogUrl(const std::string& base, const std::string& ref, std::string* out) {
    if (isHttpUrl(ref)) {
        *out = ref;
        return true;
    }
    if (ref.empty() || ref == "." || ref == "..") return false;
    for (char c : ref) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '/' || c == '\\' || c == ':' || c == '?' || c == '#')
            return false;
    }
    if (!isHttpUrl(base)) return false;
    std::string b = base;
    const size_t frag = b.find('#');
    if (frag != std::string::npos) b.resize(frag);
    const size_t query = b.find('?');
    if (query != std::string::npos) b.resize(query);
    const size_t pathStart = b.find('/', schemeLen(b));
    std::string resolved = pathStart == std::string::npos ? b + "/" : b.substr(0, b.rfind('/') + 1);
    resolved += ref;
    if (resolved.size() > kCatalogMaxUrlLen) return false;
    *out = std::move(resolved);
    return true;
}

CatalogParse parseCatalog(const char* text, size_t len, const char* catalogUrl) {
    CatalogParse r;
    const std::string base = catalogUrl ? catalogUrl : "";
    if (!text) len = 0;
    size_t pos = 0;
    // Tolerate a UTF-8 byte-order mark added by an editor.
    if (len >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
        static_cast<uint8_t>(text[2]) == 0xBF)
        pos = 3;

    bool sawTag = false;
    while (pos < len) {
        const char* line = text + pos;
        const void* nl = std::memchr(line, '\n', len - pos);
        size_t n = nl ? static_cast<size_t>(static_cast<const char*>(nl) - line) : len - pos;
        pos += n + 1;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) --n;
        if (isBlank(line, n)) continue;

        if (!sawTag) {
            // The first non-blank line decides whether this is a catalog at all
            // and which version; an unknown version is refused before any record.
            int version = 0;
            if (!parseTag(line, n, &version)) {
                r.error = CatalogError::NotACatalog;
                return r;
            }
            r.version = version;
            if (version != kCatalogVersion) {
                r.error = CatalogError::UnsupportedVersion;
                return r;
            }
            sawTag = true;
            continue;
        }
        if (line[0] == '#') continue;  // comment (the column line included)

        CatalogEntry e;
        if (!parseRecord(line, n, base, &e)) {
            ++r.skipped;
            continue;
        }
        bool dup = false;
        for (const CatalogEntry& prev : r.entries)
            if (prev.name == e.name) dup = true;
        if (dup) {
            ++r.skipped;
            continue;
        }
        if (r.entries.size() >= kCatalogMaxEntries) {
            r.truncated = true;
            continue;
        }
        r.entries.push_back(std::move(e));
    }
    if (!sawTag) {
        r.error = CatalogError::NotACatalog;
        return r;
    }
    if (r.entries.empty() && r.skipped > 0) r.error = CatalogError::NoValidEntries;
    return r;
}

const char* catalogErrorString(CatalogError e) {
    switch (e) {
        case CatalogError::Ok: return "ok";
        case CatalogError::NotACatalog: return "not a catalog";
        case CatalogError::UnsupportedVersion: return "unsupported catalog version";
        case CatalogError::NoValidEntries: return "no valid entries";
    }
    return "?";
}

}  // namespace tgc
