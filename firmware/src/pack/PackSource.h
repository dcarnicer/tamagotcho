#pragma once
// Abstract random-access byte source for packs. Pure C++ (no Arduino).
#include <cstddef>
#include <cstdint>

namespace tgc {

class PackSource {
public:
    virtual ~PackSource() = default;
    // Total size in bytes.
    virtual uint32_t size() const = 0;
    // Read exactly `len` bytes at absolute offset `off`. False on any failure.
    virtual bool read(uint32_t off, void* buf, size_t len) = 0;
};

}  // namespace tgc
