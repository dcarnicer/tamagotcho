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

    // Text with set-2 glyphs. ASCII missing from the pack uses the built-in
    // font; anything else missing is drawn as a '?' box.
    int lineHeight() const { return lineH_; }
    int charAdvance(uint32_t cp);
    int textWidth(const char* s);
    // y is the top of the line. Returns the width drawn.
    int drawText(lgfx::LovyanGFX& dst, const char* s, int x, int y, uint16_t fg, uint16_t bg);
    // Word-wraps (at spaces, or at any character for CJK) to maxW, at most
    // maxLines lines; the last line is ellipsised on overflow. Returns lines drawn.
    int drawWrapped(lgfx::LovyanGFX& dst, const char* s, int x, int y, int maxW, int maxLines,
                    uint16_t fg, uint16_t bg);

private:
    const uint8_t* bitmapFor(const Glyph& g);
    int drawRange(lgfx::LovyanGFX& dst, const char* b, const char* e, int x, int y, uint16_t fg,
                  uint16_t bg);
    int drawChar(lgfx::LovyanGFX& dst, uint32_t cp, int x, int y, uint16_t fg, uint16_t bg);

    const PackReader* pack_ = nullptr;
    uint8_t** cache_ = nullptr;  // lazily filled, sets 1 and 2 only
    uint8_t* scratch_ = nullptr;
    size_t scratchCap_ = 0;
    int lineH_ = 14;
    int ascent_ = 11;
    int textPx_ = 12;
};

}  // namespace tgc
