#pragma once
// Alpha blending for 4bpp glyph pixels. Pure, header-only.
#include <cstdint>

namespace tgc {

// Blend `fg` over `bg` (both RGB565, standard R5G6B5 layout) with alpha a/15.
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    if (a >= 15) return fg;
    if (a == 0) return bg;
    const uint32_t ia = 15 - a;
    const uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * ia + 7) / 15;
    const uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * ia + 7) / 15;
    const uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * ia + 7) / 15;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

}  // namespace tgc
