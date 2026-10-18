#include "pack/Rle.h"

#include <cstring>

namespace tgc {

bool ByteIn::refill() {
    if (!src_ || left_ == 0) return false;
    const uint32_t n = left_ < kWindow ? left_ : static_cast<uint32_t>(kWindow);
    if (!src_->read(off_, win_, n)) {
        ioError_ = true;
        left_ = 0;
        return false;
    }
    off_ += n;
    left_ -= n;
    cur_ = win_;
    end_ = win_ + n;
    return true;
}

namespace {

// 4-bit codes, high nibble first.
class Nibbles {
public:
    explicit Nibbles(ByteIn& in) : in_(in) {}
    bool next(uint8_t* n) {
        if (low_) {
            *n = byte_ & 0x0F;
            low_ = false;
            return true;
        }
        if (!in_.next(&byte_)) return false;
        *n = byte_ >> 4;
        low_ = true;
        return true;
    }

private:
    ByteIn& in_;
    uint8_t byte_ = 0;
    bool low_ = false;
};

// Glyph length code: 1..10 | 11..14 n -> 11..74 | 15 n n -> 75..330. Never 0.
bool glyphLen(Nibbles& nb, uint8_t c, uint32_t* len) {
    if (c == 0) return false;
    if (c <= 10) {
        *len = c;
        return true;
    }
    uint8_t n;
    if (!nb.next(&n)) return false;
    if (c <= 14) {
        *len = 11u + (c - 11u) * 16u + n;
        return true;
    }
    uint8_t n2;
    if (!nb.next(&n2)) return false;
    *len = 75u + n * 16u + n2;
    return true;
}

// Mask length code: 0..13 | 14 n -> 14..29 | 15 n n -> 30..285.
bool maskLen(Nibbles& nb, uint8_t c, uint32_t* len) {
    if (c < 14) {
        *len = c;
        return true;
    }
    uint8_t n;
    if (!nb.next(&n)) return false;
    if (c == 14) {
        *len = 14u + n;
        return true;
    }
    uint8_t n2;
    if (!nb.next(&n2)) return false;
    *len = 30u + n * 16u + n2;
    return true;
}

// Write cursor over a zeroed raw 4bpp bitmap, pixel by pixel in row-major order.
struct Pixels4 {
    uint8_t* row;
    size_t stride;
    uint32_t w;
    uint32_t x;

    void put(uint8_t v) {
        row[x >> 1] |= (x & 1) ? v : static_cast<uint8_t>(v << 4);
        if (++x == w) {
            x = 0;
            row += stride;
        }
    }
    uint8_t above() const {
        const uint8_t b = (row - stride)[x >> 1];
        return (x & 1) ? (b & 0x0F) : (b >> 4);
    }
    void skip(uint32_t n) {  // pixels stay 0
        const uint32_t t = x + n;
        row += (t / w) * stride;
        x = t % w;
    }
};

}  // namespace

bool decodeGlyphRle(ByteIn& in, uint8_t w, uint8_t h, uint8_t* out, size_t cap) {
    const size_t stride = (static_cast<size_t>(w) + 1) / 2;
    const size_t bytes = stride * h;
    if (cap < bytes) return false;
    if (bytes == 0) return true;
    std::memset(out, 0, bytes);
    const uint32_t total = static_cast<uint32_t>(w) * h;
    Pixels4 px{out, stride, w, 0};
    Nibbles nb(in);
    uint32_t i = 0;
    while (i < total) {
        uint8_t t;
        if (!nb.next(&t)) return false;
        if (t != 0 && t != 15) {  // one literal pixel
            px.put(t);
            ++i;
            continue;
        }
        uint8_t c;
        if (!nb.next(&c)) return false;
        const bool copy = (c == 0);
        if (copy) {
            if (t == 15) return false;  // "F 0" is reserved
            if (!nb.next(&c)) return false;
        }
        uint32_t len;
        if (!glyphLen(nb, c, &len) || len > total - i) return false;
        if (copy) {
            if (i < w) return false;  // nothing above the first row
            for (uint32_t k = 0; k < len; ++k) px.put(px.above());
        } else if (t == 15) {
            for (uint32_t k = 0; k < len; ++k) px.put(15);
        } else {
            px.skip(len);
        }
        i += len;
    }
    return true;
}

bool decodeMaskRle(ByteIn& in, uint8_t w, uint8_t h, bool rowXor, uint8_t* out, size_t cap) {
    const size_t stride = (static_cast<size_t>(w) + 7) / 8;
    const size_t bytes = stride * h;
    if (cap < bytes) return false;
    if (bytes) std::memset(out, 0, bytes);
    const uint32_t total = static_cast<uint32_t>(w) * h;
    Nibbles nb(in);
    uint32_t i = 0;
    bool ink = false;  // runs alternate, starting with empty
    uint8_t c;
    while (nb.next(&c)) {
        uint32_t len;
        if (!maskLen(nb, c, &len) || len > total - i) return false;
        if (ink && len) {
            uint32_t y = i / w, x = i % w;
            uint8_t* row = out + y * stride;
            for (uint32_t k = 0; k < len; ++k) {
                row[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
                if (++x == w) {
                    x = 0;
                    row += stride;
                }
            }
        }
        i += len;
        ink = !ink;
    }
    if (in.ioError()) return false;
    if (rowXor)
        for (size_t y = 1; y < h; ++y) {
            uint8_t* row = out + y * stride;
            const uint8_t* up = row - stride;
            for (size_t b = 0; b < stride; ++b) row[b] ^= up[b];
        }
    return true;
}

}  // namespace tgc
