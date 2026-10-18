#pragma once
// The firmware's own UI font: types and lookup for the glyph tables that
// tools/ui-font/gen_ui_font.py generates into ui/UiFont.h (Noto Sans JP at
// 18 px "Label" and 32 px "Numeral", docs/ux-refresh.md §2.3). It needs no
// pack, so it also draws the screens shown before one is loaded.
//
// Bitmaps use the pack glyph format (docs/pack-format.md): 4bpp coverage, high
// nibble first, rows byte-aligned (stride (width + 1) / 2), drawn with the
// top-left at (pen + xOff, baseline + yOff). Pure C++, no Arduino
// (native-tested in test_ui).
#include <cstdint>

#include "pack/Utf8.h"

namespace tgc {

struct UiGlyph {
    uint16_t cp;      // code point (the faces stay in the BMP)
    uint16_t offset;  // first byte of the bitmap in UiFace::bits
    uint8_t width, height;
    int8_t xOff, yOff;  // ink box from the pen position and the baseline
    uint8_t advance;
};

struct UiFace {
    const UiGlyph* glyphs;  // sorted by cp, no duplicates
    uint16_t count;
    const uint8_t* bits;
    uint32_t bitsSize;
    uint8_t px;          // em size
    uint8_t ascent;      // the font's ascent (as a pack's text_ascent)
    uint8_t lineHeight;  // the font's line box (ascent + descent)
    uint8_t capHeight;   // ink height of 'H' (Label) or '0' (Numeral)
    uint8_t pitch;       // compact line pitch for one-script UI lines
};

// The glyph for `cp`, or nullptr when the face does not have it.
inline const UiGlyph* uiGlyph(const UiFace& f, uint32_t cp) {
    int lo = 0, hi = static_cast<int>(f.count) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t c = f.glyphs[mid].cp;
        if (c == cp) return &f.glyphs[mid];
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

inline const uint8_t* uiGlyphBits(const UiFace& f, const UiGlyph& g) { return f.bits + g.offset; }

inline uint32_t uiGlyphBytes(const UiGlyph& g) {
    return static_cast<uint32_t>((g.width + 1) / 2) * g.height;
}

// Width of UTF-8 `s` in the face; a code point it lacks counts `missing`.
inline int uiTextWidth(const UiFace& f, const char* s, int missing) {
    int w = 0;
    while (uint32_t cp = utf8Next(&s)) {
        const UiGlyph* g = uiGlyph(f, cp);
        w += g ? g->advance : missing;
    }
    return w;
}

// True when the face has every code point of `s`.
inline bool uiCanDraw(const UiFace& f, const char* s) {
    while (uint32_t cp = utf8Next(&s))
        if (!uiGlyph(f, cp)) return false;
    return true;
}

}  // namespace tgc
