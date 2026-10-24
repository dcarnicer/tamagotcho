#pragma once
// PackSource over an Arduino fs::File (LittleFS or the microSD card), plus the
// PSRAM allocator. Arduino/ESP32 only; not part of the native build.
#include <Arduino.h>
#include <FS.h>

#include "pack/PackReader.h"
#include "pack/PackSource.h"
#include "platform/Psram.h"

namespace tgc {

// `stopOnError`: after a read failed, every later one fails at once. The
// card's decks take it: with the card pulled out, each read of the SD driver
// waits for its timeouts (about a second), and a screen of glyphs is dozens of
// reads (docs/microsd.md §3.5). failed() tells the app a read failed.
class FsPackSource : public PackSource {
public:
    explicit FsPackSource(fs::File f, bool stopOnError = false)
        : f_(f), size_(f ? static_cast<uint32_t>(f.size()) : 0), stopOnError_(stopOnError) {}
    bool ok() const { return static_cast<bool>(f_); }
    bool failed() const { return failed_; }
    uint32_t size() const override { return size_; }
    bool read(uint32_t off, void* buf, size_t len) override {
        if (!f_ || static_cast<uint64_t>(off) + len > size_) return false;
        if (len == 0) return true;
        if (failed_ && stopOnError_) return false;
        if (f_.seek(off) && f_.read(static_cast<uint8_t*>(buf), len) == len) return true;
        failed_ = true;
        return false;
    }

private:
    fs::File f_;
    uint32_t size_;
    bool stopOnError_;
    bool failed_ = false;
};

// Prefer PSRAM, fall back to internal heap (all of it on a board without
// PSRAM, where ps_malloc() returns null).
inline Allocator psramAllocator() {
    return Allocator{
        [](size_t n) -> void* {
            void* p = ps_malloc(n);
            return p ? p : malloc(n);
        },
        [](void* p) { free(p); }};
}

// How the app opens packs (docs/architecture.md, "Memory"): with PSRAM the
// tables in memory, as always, read on demand only if they ever do not fit;
// without PSRAM (the Cardputer ADV) always on demand, so the internal heap
// keeps room for the canvas, Wi-Fi and TLS whatever the deck.
inline PackMemory packMemory() {
    return hasPsram() ? PackMemory::InMemoryOrStreamed : PackMemory::Streamed;
}

// The renderer's glyph cache: unbounded in PSRAM, else a budget of internal
// heap (Renderer::begin()).
constexpr size_t kGlyphCacheBudgetNoPsram = 16u * 1024u;
inline size_t glyphCacheBudget() { return hasPsram() ? 0 : kGlyphCacheBudgetNoPsram; }

}  // namespace tgc
