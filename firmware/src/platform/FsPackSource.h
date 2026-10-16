#pragma once
// PackSource over an Arduino fs::File (LittleFS), plus the PSRAM allocator.
// Arduino/ESP32 only; not part of the native build.
#include <Arduino.h>
#include <FS.h>

#include "pack/PackReader.h"
#include "pack/PackSource.h"

namespace tgc {

class FsPackSource : public PackSource {
public:
    explicit FsPackSource(fs::File f) : f_(f), size_(f ? static_cast<uint32_t>(f.size()) : 0) {}
    bool ok() const { return static_cast<bool>(f_); }
    uint32_t size() const override { return size_; }
    bool read(uint32_t off, void* buf, size_t len) override {
        if (!f_ || static_cast<uint64_t>(off) + len > size_) return false;
        if (len == 0) return true;
        if (!f_.seek(off)) return false;
        return f_.read(static_cast<uint8_t*>(buf), len) == len;
    }

private:
    fs::File f_;
    uint32_t size_;
};

// Prefer PSRAM, fall back to internal heap.
inline Allocator psramAllocator() {
    return Allocator{
        [](size_t n) -> void* {
            void* p = ps_malloc(n);
            return p ? p : malloc(n);
        },
        [](void* p) { free(p); }};
}

}  // namespace tgc
