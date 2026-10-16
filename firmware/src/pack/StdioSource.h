#pragma once
// PackSource over a stdio FILE (native tests / host tools).
#include <cstdio>

#include "pack/PackSource.h"

namespace tgc {

class StdioSource : public PackSource {
public:
    explicit StdioSource(const char* path) {
        f_ = std::fopen(path, "rb");
        if (f_) {
            std::fseek(f_, 0, SEEK_END);
            long n = std::ftell(f_);
            size_ = n > 0 ? static_cast<uint32_t>(n) : 0;
        }
    }
    ~StdioSource() override {
        if (f_) std::fclose(f_);
    }
    StdioSource(const StdioSource&) = delete;
    StdioSource& operator=(const StdioSource&) = delete;

    bool ok() const { return f_ != nullptr; }
    uint32_t size() const override { return size_; }
    bool read(uint32_t off, void* buf, size_t len) override {
        if (!f_ || static_cast<uint64_t>(off) + len > size_) return false;
        if (std::fseek(f_, static_cast<long>(off), SEEK_SET) != 0) return false;
        return len == 0 || std::fread(buf, 1, len, f_) == len;
    }

private:
    std::FILE* f_ = nullptr;
    uint32_t size_ = 0;
};

}  // namespace tgc
