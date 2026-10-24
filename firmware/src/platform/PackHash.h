#pragma once
// SHA-256 of a file on LittleFS or the microSD card, streamed so a
// multi-megabyte pack never sits in RAM at once. The deck update check reads the installed file's digest only
// when its size already matches the catalog (docs/deck-updates.md); the hash
// itself uses the same mbedTLS implementation as the download check
// (platform/NetFetch.cpp). Arduino/ESP32 only; not part of the native build.
#include <Arduino.h>
#include <FS.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>

namespace tgc {

// Digest of the file at `path` on `fs` ("/packs/n5-es.tgc" on LittleFS) into
// `out[32]`. False when the file cannot be opened, read or allocated for, so a
// caller can fall back to its size-only answer. Deck paths go through
// platform/Storage's sha256(), which finds the filesystem.
inline bool fsFileSha256(fs::FS& fs, const String& path, uint8_t out[32]) {
    constexpr size_t kChunk = 4 * 1024;
    File f = fs.open(path, "r");
    if (!f || f.isDirectory()) return false;
    const size_t expected = f.size();

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    bool ok = mbedtls_sha256_starts_ret(&ctx, 0) == 0;
    uint8_t* buf = ok ? static_cast<uint8_t*>(malloc(kChunk)) : nullptr;
    if (!buf) ok = false;
    size_t total = 0;
    while (ok) {
        const int n = f.read(buf, kChunk);
        if (n < 0) {
            ok = false;
        } else if (n == 0) {
            break;
        } else {
            total += static_cast<size_t>(n);
            ok = mbedtls_sha256_update_ret(&ctx, buf, static_cast<size_t>(n)) == 0;
        }
    }
    // A file that shrank or grew between stat() and open() is not the one the
    // caller measured; report a failure rather than a digest of other bytes.
    if (ok && total != expected) ok = false;
    if (ok) ok = mbedtls_sha256_finish_ret(&ctx, out) == 0;
    free(buf);
    mbedtls_sha256_free(&ctx);
    f.close();
    return ok;
}

}  // namespace tgc
