// HTTP(S) downloads; see NetFetch.h. Arduino/ESP32 only.
#include "platform/NetFetch.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <memory>
#include <new>

#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/RootCerts.h"

// The TLS handshake (certificate parsing, RSA/ECDSA verification) runs on the
// Arduino loop task, whose default 8 KB stack leaves little margin under the
// app's own call chain. 16 KB costs 8 KB of internal RAM for the whole run.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace tgc {

namespace {

constexpr const char* kPacksDir = "/packs";
constexpr uint16_t kHttpTimeoutMs = 8000;            // per read (HTTPClient)
constexpr int32_t kTlsConnectTimeoutMs = 10000;      // TCP connect + TLS socket timeout
constexpr unsigned long kTlsHandshakeTimeoutS = 15;  // whole handshake
constexpr uint32_t kTextIdleMs = 10000;

// SHA-256 over the streamed body (mbedTLS, hardware-accelerated on the ESP32-S3).
class Sha256 {
public:
    Sha256() {
        mbedtls_sha256_init(&ctx_);
        mbedtls_sha256_starts_ret(&ctx_, 0);
    }
    ~Sha256() { mbedtls_sha256_free(&ctx_); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const uint8_t* p, size_t n) { mbedtls_sha256_update_ret(&ctx_, p, n); }
    void finish(uint8_t out[32]) { mbedtls_sha256_finish_ret(&ctx_, out); }

private:
    mbedtls_sha256_context ctx_;
};

// One request's transport: a plain client, or a TLS client that verifies the
// server against kRootCaPem. Declare it before the HTTPClient that uses it, so
// the HTTPClient is destroyed (and stops the client) first.
class Transport {
public:
    WiFiClient* open(bool https) {
        if (https) {
            tls_.reset(new (std::nothrow) WiFiClientSecure);
            if (!tls_) return nullptr;
            tls_->setCACert(kRootCaPem);
            tls_->setHandshakeTimeout(kTlsHandshakeTimeoutS);
            return tls_.get();
        }
        plain_.reset(new (std::nothrow) WiFiClient);
        return plain_.get();
    }
    // mbedTLS error of the last TLS attempt, 0 if none. WiFiClientSecure also
    // records -1 for a socket failure/timeout and the socket number on success;
    // neither is a TLS error.
    int tlsError() {
        if (!tls_) return 0;
        char buf[2];
        const int e = tls_->lastError(buf, sizeof buf);
        return e < -1 ? e : 0;
    }

private:
    std::unique_ptr<WiFiClientSecure> tls_;
    std::unique_ptr<WiFiClient> plain_;
};

void logHeap(const char* what) {
    Serial.printf("[net] %s: internal heap %u free, %u largest, %u min since boot; psram %u free\n",
                  what, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getFreePsram());
}

// Prepares `http` for a GET of `url` (following redirects) and sends it.
// Returns false when the URL could not even be parsed (`code` untouched);
// otherwise `code` is the HTTP status or a negative HTTPClient error, and a TLS
// failure is recorded in res->tlsError.
bool sendGet(HTTPClient& http, Transport& transport, const String& url, bool http10, int* code,
             FetchResult* res) {
    const bool https = url.startsWith("https://");
    WiFiClient* client = transport.open(https);
    if (!client) {
        *code = HTTPC_ERROR_TOO_LESS_RAM;
        return true;
    }
    http.setTimeout(kHttpTimeoutMs);
    if (https) http.setConnectTimeout(kTlsConnectTimeoutMs);
    // GitHub release assets answer 302 to another host; both are https, which
    // HTTPClient follows with the same (verifying) client.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (http10) http.useHTTP10(true);
    if (!http.begin(*client, url)) return false;
    if (https) logHeap("before TLS");
    *code = http.GET();
    if (*code < 0) res->tlsError = transport.tlsError();
    if (https) logHeap("after TLS connect");
    return true;
}

FetchStatus statusForCode(int code, int tlsError) {
    if (code == HTTP_CODE_OK) return FetchStatus::Ok;
    if (code > 0) return FetchStatus::Http;
    return tlsError ? FetchStatus::Tls : FetchStatus::Connect;
}

// Streams the response body to `sink(const uint8_t*, size_t) -> FetchStatus`
// until the announced length or, without one, until the server closes. A body
// cut short of its announced length is a failure, not a short file.
template <typename Sink>
FetchStatus readBody(HTTPClient& http, int remaining, uint32_t idleMs, uint32_t totalMs,
                     Sink&& sink) {
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    const bool sized = remaining > 0;
    const uint32_t start = millis();
    uint32_t lastData = start;
    while (http.connected() && (remaining > 0 || remaining == -1)) {
        if (millis() - start > totalMs) return FetchStatus::Stalled;
        // WiFiClientSecure reports a closed (or failed) TLS session as a negative
        // count; the length check after the loop tells a clean end from a cut.
        const int avail = stream->available();
        if (avail < 0) break;
        if (avail == 0) {
            if (millis() - lastData > idleMs) return FetchStatus::Stalled;
            delay(1);
            continue;
        }
        size_t want = static_cast<size_t>(avail) > sizeof buf ? sizeof buf : static_cast<size_t>(avail);
        if (sized && want > static_cast<size_t>(remaining)) want = static_cast<size_t>(remaining);
        const int r = stream->read(buf, want);
        if (r <= 0) return FetchStatus::Stalled;
        lastData = millis();
        const FetchStatus s = sink(buf, static_cast<size_t>(r));
        if (s != FetchStatus::Ok) return s;
        if (sized) {
            remaining -= r;
            if (remaining <= 0) break;
        }
    }
    return (sized && remaining > 0) ? FetchStatus::Stalled : FetchStatus::Ok;
}

}  // namespace

size_t fsFreeBytes() {
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    return total > used ? total - used : 0;
}

FetchResult fetchPackFile(const PackFetchRequest& req) {
    FetchResult res;
    const bool https = req.url.startsWith("https://");
    if ((!https && !req.url.startsWith("http://")) || (https && !req.allowHttps) ||
        req.fileName.isEmpty()) {
        res.status = FetchStatus::BadUrl;
        return res;
    }
    const String dst = String(kPacksDir) + "/" + req.fileName;
    LittleFS.mkdir(kPacksDir);
    // Discard a temp left by an interrupted run before measuring the space, or
    // its size would count against this download.
    LittleFS.remove(kFetchTmp);

    // Space first, before any network traffic: the reserve must stay free, and
    // a catalog size that cannot fit is refused without downloading it.
    const size_t freeB = fsFreeBytes();
    if (freeB < kPackFsReserveBytes) {
        res.status = FetchStatus::NoSpace;
        return res;
    }
    if (req.expectBytes > req.maxBytes) {
        res.status = FetchStatus::TooLarge;
        return res;
    }
    if (req.expectBytes && req.expectBytes + kPackFsReserveBytes > freeB) {
        res.status = FetchStatus::NoSpace;
        return res;
    }

    Transport transport;
    HTTPClient http;
    int code = 0;
    if (!sendGet(http, transport, req.url, req.http10, &code, &res)) {
        res.status = FetchStatus::BadUrl;
        return res;
    }
    res.httpCode = code;
    if (code != HTTP_CODE_OK) {
        res.status = statusForCode(code, res.tlsError);
        http.end();
        return res;
    }
    const int remaining = http.getSize();
    FetchStatus early = FetchStatus::Ok;
    if (remaining > 0 && req.expectBytes && static_cast<uint32_t>(remaining) != req.expectBytes)
        early = FetchStatus::SizeMismatch;
    else if (remaining > 0 && static_cast<size_t>(remaining) > req.maxBytes)
        early = FetchStatus::TooLarge;
    else if (remaining > 0 && static_cast<size_t>(remaining) + kPackFsReserveBytes > freeB)
        early = FetchStatus::NoSpace;
    if (early != FetchStatus::Ok) {
        res.status = early;
        http.end();
        return res;
    }
    const size_t total = req.expectBytes ? req.expectBytes : (remaining > 0 ? remaining : 0);

    File f = LittleFS.open(kFetchTmp, "w");
    if (!f) {
        res.status = FetchStatus::WriteFailed;
        http.end();
        return res;
    }
    res.bodyStarted = true;

    std::unique_ptr<Sha256> sha;
    if (req.expectSha256) sha.reset(new (std::nothrow) Sha256);
    size_t written = 0;
    FetchStatus st = FetchStatus::Ok;
    if (req.expectSha256 && !sha) st = FetchStatus::WriteFailed;  // out of memory
    if (st == FetchStatus::Ok && req.progress && !req.progress(0, total, req.progressCtx))
        st = FetchStatus::Cancelled;
    if (st == FetchStatus::Ok) {
        st = readBody(http, remaining, req.idleTimeoutMs, req.totalTimeoutMs,
                      [&](const uint8_t* p, size_t n) {
                          if (written + n + kPackFsReserveBytes > freeB) return FetchStatus::NoSpace;
                          if (written + n > req.maxBytes) return FetchStatus::TooLarge;
                          if (f.write(p, n) != n) return FetchStatus::WriteFailed;
                          if (sha) sha->update(p, n);
                          written += n;
                          if (req.progress && !req.progress(written, total, req.progressCtx))
                              return FetchStatus::Cancelled;
                          return FetchStatus::Ok;
                      });
    }
    f.close();
    http.end();
    res.bytes = written;

    // Integrity before structure: a catalog download must match its size and
    // digest exactly; then any download must look like a pack.
    if (st == FetchStatus::Ok && req.expectBytes && written != req.expectBytes)
        st = FetchStatus::SizeMismatch;
    if (st == FetchStatus::Ok && sha) {
        uint8_t digest[32];
        sha->finish(digest);
        if (std::memcmp(digest, req.expectSha256, sizeof digest) != 0) st = FetchStatus::HashMismatch;
    }
    if (st == FetchStatus::Ok && (written < 4 || !validatePackFile(kFetchTmp)))
        st = FetchStatus::InvalidPack;
    if (st != FetchStatus::Ok) {
        LittleFS.remove(kFetchTmp);
        res.status = st;
        return res;
    }

    const bool hadDst = LittleFS.exists(dst);
    if (hadDst) LittleFS.remove(dst);
    if (!LittleFS.rename(kFetchTmp, dst)) {
        LittleFS.remove(kFetchTmp);
        // The old pack was already removed, so /packs did change: the app must
        // rescan rather than keep a handle to a file that is gone.
        res.status = FetchStatus::RenameFailed;
        res.packsChanged = hadDst;
        return res;
    }
    res.packsChanged = true;
    return res;
}

FetchResult fetchText(const String& url, size_t maxBytes, uint32_t timeoutMs, std::string* out) {
    FetchResult res;
    out->clear();
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        res.status = FetchStatus::BadUrl;
        return res;
    }
    Transport transport;
    HTTPClient http;
    int code = 0;
    if (!sendGet(http, transport, url, /*http10=*/true, &code, &res)) {
        res.status = FetchStatus::BadUrl;
        return res;
    }
    res.httpCode = code;
    if (code != HTTP_CODE_OK) {
        res.status = statusForCode(code, res.tlsError);
        http.end();
        return res;
    }
    const int remaining = http.getSize();
    if (remaining > 0 && static_cast<size_t>(remaining) > maxBytes) {
        res.status = FetchStatus::TooLarge;
        http.end();
        return res;
    }
    out->reserve(remaining > 0 ? static_cast<size_t>(remaining) : 1024u);
    res.bodyStarted = true;
    const FetchStatus st =
        readBody(http, remaining, kTextIdleMs, timeoutMs, [&](const uint8_t* p, size_t n) {
            if (out->size() + n > maxBytes) return FetchStatus::TooLarge;
            out->append(reinterpret_cast<const char*>(p), n);
            return FetchStatus::Ok;
        });
    http.end();
    res.bytes = out->size();
    if (st != FetchStatus::Ok) {
        out->clear();
        res.status = st;
    }
    return res;
}

bool validatePackFile(const String& path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t magic[4] = {0, 0, 0, 0};
    const size_t n = f.read(magic, sizeof magic);
    f.close();
    if (n != sizeof magic || std::memcmp(magic, "TNGC", 4) != 0) return false;

    // Cheap structural probe (PSRAM-backed); freed again before we return.
    auto src = std::make_unique<FsPackSource>(LittleFS.open(path, "r"));
    if (!src->ok()) return false;
    PackReader probe;
    const PackError e = probe.open(*src, psramAllocator());
    if (e != PackError::Ok) Serial.printf("[wifi] pack probe failed: %s\n", packErrorString(e));
    return e == PackError::Ok;
}

const char* fetchStatusString(FetchStatus s) {
    switch (s) {
        case FetchStatus::Ok: return "ok";
        case FetchStatus::BadUrl: return "bad URL";
        case FetchStatus::NoSpace: return "no space";
        case FetchStatus::TooLarge: return "too large";
        case FetchStatus::Connect: return "cannot connect";
        case FetchStatus::Tls: return "TLS error";
        case FetchStatus::Http: return "HTTP error";
        case FetchStatus::WriteFailed: return "write failed";
        case FetchStatus::Stalled: return "stalled";
        case FetchStatus::Cancelled: return "cancelled";
        case FetchStatus::SizeMismatch: return "size mismatch";
        case FetchStatus::HashMismatch: return "sha256 mismatch";
        case FetchStatus::InvalidPack: return "not a valid pack";
        case FetchStatus::RenameFailed: return "rename failed";
    }
    return "?";
}

}  // namespace tgc
