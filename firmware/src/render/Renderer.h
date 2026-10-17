#pragma once
// Draws pack glyphs (4bpp) and UTF-8 text onto any LovyanGFX target
// (display, LGFX_Sprite). Arduino/ESP32 only.
#include <cstdint>

#include "board/Board.h"
#include "pack/PackReader.h"

namespace tgc {

// Draw a raw 4bpp bitmap (high nibble first, rows byte-aligned) with ink `fg`
// blended over flat background `bg`. Zero-alpha pixels are skipped.
void drawBitmap4bpp(lgfx::LovyanGFX& dst, int x, int y, const uint8_t* bits, int w, int h,
                    uint16_t fg, uint16_t bg);

class Renderer {
public:
    ~Renderer() { end(); }
    // `pack` must outlive the renderer.
    bool begin(const PackReader& pack);
    void end();

    // Kanji (set 0 or 1) centred horizontally in the box, vertically centred
    // as an em square. Draws a '?' box if the glyph is missing.
    void drawKanji(lgfx::LovyanGFX& dst, uint8_t set, uint32_t cp, int boxX, int boxY, int boxW,
                   int boxH, uint16_t fg, uint16_t bg);

    // Built-in GFX font as a text style: pass kTextBuiltin as `set`. It carries
    // ASCII plus the Latin-1 accents Font0 keeps in its CP437 slots; indices are
    // translated in glcdIndex(). builtinCanDraw() reports full coverage.
    static constexpr uint8_t kTextBuiltin = 0xFF;
    static bool builtinCanDraw(const char* s);

    // Text metrics per set. `set` is 2 (small text), 3 (large option text) or
    // kTextBuiltin. Set 3 falls back to set 2 when the pack has no set 3.
    int lineHeight(uint8_t set = kGlyphSetText) const;
    int ascent(uint8_t set = kGlyphSetText) const;

    // ASCII missing from the pack uses the built-in font; any other missing
    // character (or a missing set-3 glyph) is drawn as a '?' box.
    int charAdvance(uint32_t cp, uint8_t set = kGlyphSetText) const;
    int textWidth(const char* s, uint8_t set = kGlyphSetText) const;

    // y is the top of the line. Returns the width drawn.
    int drawText(lgfx::LovyanGFX& dst, const char* s, int x, int y, uint16_t fg, uint16_t bg,
                 uint8_t set = kGlyphSetText);
    // Word-wraps (at spaces, or at any character for CJK) to maxW, at most
    // maxLines lines; the last line is ellipsised on overflow. Returns lines drawn.
    int drawWrapped(lgfx::LovyanGFX& dst, const char* s, int x, int y, int maxW, int maxLines,
                    uint16_t fg, uint16_t bg, uint8_t set = kGlyphSetText);
    // As drawWrapped, but centres the block vertically in `h` and each line
    // horizontally in `w`. Returns lines drawn.
    int drawCentred(lgfx::LovyanGFX& dst, const char* s, int x, int y, int w, int h, int maxLines,
                    uint16_t fg, uint16_t bg, uint8_t set = kGlyphSetText);

private:
    struct Line {
        const char* begin;
        const char* end;
        bool ellipsis;
    };
    static constexpr int kMaxWrapLines = 8;

    // Set-3 lookups fall back to set 2; anything else is returned as-is.
    const Glyph* glyphFor(uint8_t set, uint32_t cp) const;
    const uint8_t* bitmapFor(const Glyph& g);
    int textRangeWidth(const char* b, const char* e, uint8_t set) const;
    int wrap(const char* s, int maxW, int maxLines, uint8_t set, Line* out, int cap) const;
    int drawRange(lgfx::LovyanGFX& dst, const char* b, const char* e, int x, int y, uint16_t fg,
                  uint16_t bg, uint8_t set);
    int drawCodepoint(lgfx::LovyanGFX& dst, uint32_t cp, int x, int y, uint16_t fg, uint16_t bg,
                      uint8_t set);

    const PackReader* pack_ = nullptr;
    uint8_t** cache_ = nullptr;  // lazily filled, sets 1..3
    uint8_t* scratch_ = nullptr;
    size_t scratchCap_ = 0;
    int lineH_ = 14;
    int ascent_ = 11;
    int textPx_ = 12;
    int optLineH_ = 14;
    int optAscent_ = 11;
};

}  // namespace tgc
