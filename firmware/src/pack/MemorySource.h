#pragma once
// PackSource over a caller-owned byte buffer (tests, embedded packs).
#include <cstring>
#include <vector>

#include "pack/PackSource.h"

namespace tgc {

class MemorySource : public PackSource {
public:
    MemorySource(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit MemorySource(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}

    uint32_t size() const override { return static_cast<uint32_t>(size_); }
    bool read(uint32_t off, void* buf, size_t len) override {
        if (static_cast<uint64_t>(off) + len > size_) return false;
        if (len) std::memcpy(buf, data_ + off, len);
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
};

}  // namespace tgc
