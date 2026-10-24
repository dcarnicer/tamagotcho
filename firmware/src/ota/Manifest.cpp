// Firmware manifest parser. Pure C++; see Manifest.h for the format and the rules.
#include "ota/Manifest.h"

#include <cstring>

#include "catalog/Catalog.h"

namespace tgc {

namespace {

constexpr char kTag[] = "#tamagotcho-firmware";
constexpr size_t kMaxFields = 6;  // one more than an `app` line, to see an extra one

struct Field {
    const char* p;
    size_t n;
    bool is(const char* s) const { return std::strlen(s) == n && std::memcmp(p, s, n) == 0; }
};

// Splits a line at every TAB. Returns the number of fields, at most kMaxFields
// (a line with more reports kMaxFields + 1, which no record accepts).
size_t splitFields(const char* line, size_t n, Field* f) {
    size_t nf = 0, start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && line[i] != '\t') continue;
        if (nf == kMaxFields) return kMaxFields + 1;
        f[nf++] = {line + start, i - start};
        start = i + 1;
    }
    return nf;
}

// Plain decimal digits (no sign, no spaces), fitting in 32 bits.
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

// One `app` line's fields after the record type -> row. False when any is invalid.
bool parseApp(const Field* f, const std::string& base, ManifestApp* app) {
    if (!manifestEnvIsValid(f[1].p, f[1].n)) return false;
    std::string url;
    if (!resolveCatalogUrl(base, std::string(f[2].p, f[2].n), &url)) return false;
    uint32_t bytes = 0;
    if (!parseU32(f[3].p, f[3].n, &bytes) || bytes == 0) return false;
    uint8_t digest[32];
    if (!parseSha256Hex(f[4].p, f[4].n, digest)) return false;
    app->env.assign(f[1].p, f[1].n);
    app->url = std::move(url);
    app->bytes = bytes;
    std::memcpy(app->sha256, digest, sizeof digest);
    return true;
}

ManifestParse fail(ManifestParse r, ManifestError e, size_t line) {
    r.error = e;
    r.line = line;
    r.apps.clear();
    return r;
}

}  // namespace

bool parseFirmwareVersion(const char* s, size_t len, FirmwareVersion* out) {
    if (!s || len < 6 || s[0] != 'v') return false;
    uint32_t part[3];
    size_t pos = 1;
    for (int k = 0; k < 3; ++k) {
        size_t end = pos;
        while (end < len && s[end] != '.') ++end;
        if (!parseU32(s + pos, end - pos, &part[k])) return false;
        // Two dots exactly: after the patch number the text must end.
        if ((k < 2) != (end < len)) return false;
        pos = end + 1;
    }
    out->major = part[0];
    out->minor = part[1];
    out->patch = part[2];
    return true;
}

bool parseFirmwareVersion(const std::string& s, FirmwareVersion* out) {
    return parseFirmwareVersion(s.data(), s.size(), out);
}

int compareFirmwareVersions(const FirmwareVersion& a, const FirmwareVersion& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool manifestEnvIsValid(const char* env, size_t len) {
    if (!env || len == 0 || len > kManifestMaxEnvLen) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = env[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

ManifestParse parseManifest(const char* text, size_t len, const char* manifestUrl) {
    ManifestParse r;
    if (!text) len = 0;
    if (len > kManifestMaxBytes) return fail(r, ManifestError::TooLarge, 0);
    const std::string base = manifestUrl ? manifestUrl : "";
    bool sawVersion = false;
    size_t pos = 0, lineNo = 0;
    while (pos < len || lineNo == 0) {
        const char* line = text ? text + pos : "";
        const void* nl = pos < len ? std::memchr(line, '\n', len - pos) : nullptr;
        size_t n = nl ? static_cast<size_t>(static_cast<const char*>(nl) - line) : len - pos;
        pos += n + 1;
        ++lineNo;
        if (n > 0 && line[n - 1] == '\r') --n;  // a CR before the LF is ignored
        Field f[kMaxFields];
        const size_t nf = splitFields(line, n, f);

        if (lineNo == 1) {
            // The header, on line 1 and nowhere else: "#tamagotcho-firmware<TAB>N".
            uint32_t format = 0;
            if (nf != 2 || !f[0].is(kTag) || !parseU32(f[1].p, f[1].n, &format) ||
                format > 9999u)
                return fail(r, ManifestError::NotAManifest, 1);
            r.format = static_cast<int>(format);
            if (r.format != kManifestVersion)
                return fail(r, ManifestError::UnsupportedVersion, 1);
            continue;
        }
        if (n == 0 || line[0] == '#') continue;  // blank or comment

        if (f[0].is("version")) {
            // Exactly once, before any `app` line.
            if (nf != 2 || sawVersion || !r.apps.empty() ||
                !parseFirmwareVersion(f[1].p, f[1].n, &r.release))
                return fail(r, ManifestError::BadVersion, lineNo);
            r.version.assign(f[1].p, f[1].n);
            sawVersion = true;
        } else if (f[0].is("app")) {
            if (!sawVersion) return fail(r, ManifestError::BadVersion, lineNo);
            ManifestApp app;
            if (nf != 5 || !parseApp(f, base, &app)) return fail(r, ManifestError::BadApp, lineNo);
            for (const ManifestApp& prev : r.apps)
                if (prev.env == app.env) return fail(r, ManifestError::DuplicateApp, lineNo);
            r.apps.push_back(std::move(app));
        }
        // Any other record type is ignored (later formats may add some).
    }
    if (!sawVersion) return fail(r, ManifestError::BadVersion, 0);
    return r;
}

const ManifestApp* manifestAppFor(const ManifestParse& m, const char* env) {
    if (m.error != ManifestError::Ok || !env) return nullptr;
    for (const ManifestApp& a : m.apps)
        if (a.env == env) return &a;
    return nullptr;
}

const char* manifestErrorString(ManifestError e) {
    switch (e) {
        case ManifestError::Ok: return "ok";
        case ManifestError::TooLarge: return "not a firmware manifest (too large)";
        case ManifestError::NotAManifest: return "not a firmware manifest";
        case ManifestError::UnsupportedVersion: return "unsupported manifest version";
        case ManifestError::BadVersion: return "bad version line";
        case ManifestError::BadApp: return "bad app line";
        case ManifestError::DuplicateApp: return "duplicate app line";
    }
    return "?";
}

}  // namespace tgc
