#pragma once
// Decoders for the pack v2 compressed bitmaps (docs/pack-format.md: "Bitmap
// encoding 1" for glyphs, "Mask encoding 1/2" for stroke masks). Pure C++, no
// Arduino, no allocation: the caller owns the output buffer, and input comes
// through a ByteIn (a memory span, or a small window refilled from a
// PackSource, so a bitmap is decoded without loading the pack in RAM).
#include <cstddef>
#include <cstdint>

#include "pack/PackSource.h"

namespace tgc {

// Glyph table `encoding` values.
constexpr uint8_t kGlyphEncRaw = 0;  // raw 4bpp
constexpr uint8_t kGlyphEncRle = 1;  // RLE 4bpp (v2)
// Stroke entry `encoding` values (v2).
constexpr uint8_t kMaskEncRaw = 0;      // raw 1bpp
constexpr uint8_t kMaskEncRuns = 1;     // alternating run lengths
constexpr uint8_t kMaskEncXorRuns = 2;  // run lengths of each row XOR the row above

// Sequential byte input for the decoders.
class ByteIn {
public:
    // Reads `len` bytes of caller memory.
    ByteIn(const uint8_t* data, size_t len) : cur_(data), end_(data + len) {}
    // Reads `len` bytes of `src` from `off`, kWindow bytes per source read.
    ByteIn(PackSource& src, uint32_t off, uint32_t len) : src_(&src), off_(off), left_(len) {}
    ByteIn(const ByteIn&) = delete;
    ByteIn& operator=(const ByteIn&) = delete;

    // Next byte; false at the end of the input or on a read error.
    bool next(uint8_t* b) {
        if (cur_ == end_ && !refill()) return false;
        *b = *cur_++;
        ++consumed_;
        return true;
    }
    // Bytes handed out by next() so far.
    size_t consumed() const { return consumed_; }
    // True when the source failed a read (as opposed to a clean end of input).
    bool ioError() const { return ioError_; }

    static constexpr size_t kWindow = 256;

private:
    bool refill();

    const uint8_t* cur_ = nullptr;
    const uint8_t* end_ = nullptr;
    PackSource* src_ = nullptr;
    uint32_t off_ = 0;   // next source offset to read
    uint32_t left_ = 0;  // source bytes not read yet
    size_t consumed_ = 0;
    bool ioError_ = false;
    uint8_t win_[kWindow];
};

// Decodes a glyph RLE stream (encoding 1) into the raw 4bpp layout: row stride
// ceil(w/2), high nibble first, padding nibbles 0. `cap` must be at least
// ceil(w/2)*h. Stops after exactly w*h pixels (the stream length is implied by
// its codes). False on a malformed or truncated stream; the output is then
// unspecified.
bool decodeGlyphRle(ByteIn& in, uint8_t w, uint8_t h, uint8_t* out, size_t cap);

// Decodes a stroke mask in encoding 1 (runs) or, with `rowXor`, 2 (runs of the
// row XOR) into raw 1bpp (bit 7 = leftmost, stride ceil(w/8)). The stream is the
// whole input; pixels after its last run are empty. `cap` >= ceil(w/8)*h.
bool decodeMaskRle(ByteIn& in, uint8_t w, uint8_t h, bool rowXor, uint8_t* out, size_t cap);

}  // namespace tgc
