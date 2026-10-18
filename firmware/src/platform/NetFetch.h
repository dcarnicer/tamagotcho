#pragma once
// HTTP(S) downloads shared by the Wi-Fi features: the portal's "download from a
// URL" (plain http:// over the device's own access point) and "Get decks"
// (https:// over the home Wi-Fi, see HomeWifi.h). Arduino/ESP32 only; not part
// of the native build.
//
// Packs are written the same atomic way everywhere: the body streams into
// /packs/.fetch.tmp, is checked (size and SHA-256 when a catalog gave them, the
// "TNGC" magic and a PackReader probe), and only then renamed over the
// destination. Any failure removes the temp and nothing else, so a bad or
// interrupted download never destroys the pack it was replacing.
//
// HTTPS verifies the server against the small root-CA set in RootCerts.h
// (never setInsecure()); redirects are followed (GitHub release assets redirect
// to another host).
#include <Arduino.h>

#include <cstdint>
#include <string>

namespace tgc {

// Free space every pack writer leaves on the filesystem, so a pack can never
// fill it completely (the SRS state file and the temps need room too).
constexpr size_t kPackFsReserveBytes = 96u * 1024u;

// Where every pack download streams before it is checked and renamed. Hidden
// name, so the pack listers skip it. One left by a power cut mid-download is
// removed at boot (setup()) and before the next download.
constexpr const char* kFetchTmp = "/packs/.fetch.tmp";

enum class FetchStatus : uint8_t {
    Ok,
    BadUrl,        // not http(s)://, https:// where it is not allowed, or no file name
    NoSpace,       // the body (plus the reserve) does not fit the free space
    TooLarge,      // larger than the caller's cap
    Connect,       // DNS/TCP failure or timeout before any HTTP answer
    Tls,           // TLS handshake or certificate verification failed (see tlsError)
    Http,          // the server answered with a status other than 200
    WriteFailed,   // cannot create or write the temp file
    Stalled,       // no data for the idle timeout, the total budget ran out, or the
                   // connection closed before the announced length
    Cancelled,     // the progress callback asked to stop
    SizeMismatch,  // the body length differs from the expected size
    HashMismatch,  // the SHA-256 differs from the expected digest
    InvalidPack,   // not a pack (magic or PackReader probe failed)
    RenameFailed,  // the validated temp could not replace the destination
};

struct FetchResult {
    FetchStatus status = FetchStatus::Ok;
    int httpCode = 0;       // HTTP status, a negative HTTPClient error, or 0 before the request
    int tlsError = 0;       // mbedTLS error code when status == Tls
    size_t bytes = 0;       // body bytes received
    bool bodyStarted = false;   // the temp file was opened (the body was being written)
    bool packsChanged = false;  // /packs changed: success, or the old pack was
                                // removed before a failed rename
};

// Called after every received chunk (and once with done = 0 before the body).
// `total` is the expected size, or 0 when unknown. Return false to cancel.
using FetchProgressFn = bool (*)(size_t done, size_t total, void* ctx);

struct PackFetchRequest {
    String url;
    String fileName;            // final basename in /packs ("<name>.tgc"), already sanitised
    bool allowHttps = false;
    size_t maxBytes = 5u * 1024u * 1024u;
    uint32_t expectBytes = 0;               // exact size from a catalog; 0 = unknown
    const uint8_t* expectSha256 = nullptr;  // 32-byte digest from a catalog; nullptr = unchecked
    uint32_t idleTimeoutMs = 5000;          // no data for this long -> Stalled
    uint32_t totalTimeoutMs = 60000;        // whole body budget -> Stalled
    // Send HTTP/1.0 so the server cannot answer with a chunked body (the stream
    // is read raw). Off for the portal (unchanged requests), on for Get decks.
    bool http10 = false;
    FetchProgressFn progress = nullptr;
    void* progressCtx = nullptr;
};

// Blocking download of `req.url` into /packs/<req.fileName>. Needs a network
// (station or access point) to be up already.
FetchResult fetchPackFile(const PackFetchRequest& req);

// Blocking GET of a small text resource (the deck catalog) into `out`, at most
// `maxBytes`; http:// or https://, HTTP/1.0, redirects followed.
FetchResult fetchText(const String& url, size_t maxBytes, uint32_t timeoutMs, std::string* out);

// First 4 bytes are "TNGC" and a cheap PackReader probe (PSRAM-backed) opens it.
bool validatePackFile(const String& path);

const char* fetchStatusString(FetchStatus s);

// Free bytes on LittleFS (total - used, never negative).
size_t fsFreeBytes();

}  // namespace tgc
