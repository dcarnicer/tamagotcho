#include "render/Renderer.h"

#include <cstring>

#include "pack/Utf8.h"
#include "render/Blend565.h"

namespace tgc {

namespace {
const char kEllipsis[] = "...";
}  // namespace

void drawBitmap4bpp(lgfx::LovyanGFX& dst, int x, int y, const uint8_t* bits, int w, int h,
                    uint16_t fg, uint16_t bg) {
    const int stride = (w + 1) / 2;
    dst.startWrite();
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = bits + row * stride;
        for (int col = 0; col < w; ++col) {
            uint8_t b = p[col >> 1];
            uint8_t a = (col & 1) ? (b & 0x0F) : (b >> 4);
            if (a) dst.drawPixel(x + col, y + row, blend565(fg, bg, a));
        }
    }
    dst.endWrite();
}

bool Renderer::begin(const PackReader& pack) {
    end();
    pack_ = &pack;
    const PackHeader& h = pack.header();
    lineH_ = h.textLineHeight ? h.textLineHeight : 14;
    ascent_ = h.textAscent ? h.textAscent : lineH_ - 3;
    textPx_ = h.textPx ? h.textPx : 12;
    size_t n = sizeof(uint8_t*) * (h.glyphCount ? h.glyphCount : 1);
    cache_ = static_cast<uint8_t**>(pack.allocator().alloc(n));
    if (!cache_) return false;
    std::memset(cache_, 0, n);
    return true;
}

void Renderer::end() {
    if (pack_) {
        const Allocator& a = pack_->allocator();
        if (cache_) {
            for (uint32_t i = 0; i < pack_->glyphCount(); ++i)
                if (cache_[i]) a.release(cache_[i]);
            a.release(cache_);
        }
        if (scratch_) a.release(scratch_);
    }
    cache_ = nullptr;
    scratch_ = nullptr;
    scratchCap_ = 0;
    pack_ = nullptr;
}

// Returns bitmap bytes (cached for small glyph sets, scratch for set 0).
const uint8_t* Renderer::bitmapFor(const Glyph& g) {
    size_t n = PackReader::bitmapSize(g);
    if (n == 0) return nullptr;
    const Allocator& a = pack_->allocator();
    if (g.set != kGlyphSetLarge) {
        uint32_t idx = pack_->glyphIndex(&g);
        if (cache_[idx]) return cache_[idx];
        uint8_t* buf = static_cast<uint8_t*>(a.alloc(n));
        if (buf && pack_->readGlyphBitmap(g, buf, n)) {
            cache_[idx] = buf;
            return buf;
        }
        if (buf) a.release(buf);
        return nullptr;
    }
    if (n > scratchCap_) {
        if (scratch_) a.release(scratch_);
        scratch_ = static_cast<uint8_t*>(a.alloc(n));
        scratchCap_ = scratch_ ? n : 0;
        if (!scratch_) return nullptr;
    }
    return pack_->readGlyphBitmap(g, scratch_, n) ? scratch_ : nullptr;
}

void Renderer::drawKanji(lgfx::LovyanGFX& dst, uint8_t set, uint32_t cp, int boxX, int boxY,
                         int boxW, int boxH, uint16_t fg, uint16_t bg) {
    const Glyph* g = pack_->findGlyph(set, cp);
    const int px = set == kGlyphSetLarge ? pack_->header().largePx : pack_->header().smallPx;
    const int penX = boxX + (boxW - px) / 2;
    const int baseY = boxY + (boxH - px) / 2 + px;  // baseline = bottom edge of the em square
    if (!g) {
        dst.drawRect(penX, baseY - px, px, px, fg);
        dst.setTextColor(fg, bg);
        dst.drawString("?", penX + px / 2 - 3, baseY - px / 2 - 4);
        return;
    }
    const uint8_t* bits = bitmapFor(*g);
    if (bits) drawBitmap4bpp(dst, penX + g->xOff, baseY + g->yOff, bits, g->width, g->height, fg, bg);
}

int Renderer::charAdvance(uint32_t cp) {
    const Glyph* g = pack_->findGlyph(kGlyphSetText, cp);
    if (g) return g->advance;
    return textPx_ / 2 + 2;  // fallback font / '?' box width
}

int Renderer::textWidth(const char* s) {
    int w = 0;
    while (uint32_t cp = utf8Next(&s)) w += charAdvance(cp);
    return w;
}

int Renderer::drawChar(lgfx::LovyanGFX& dst, uint32_t cp, int x, int y, uint16_t fg, uint16_t bg) {
    const Glyph* g = pack_->findGlyph(kGlyphSetText, cp);
    if (g) {
        const uint8_t* bits = bitmapFor(*g);
        if (bits) drawBitmap4bpp(dst, x + g->xOff, y + ascent_ + g->yOff, bits, g->width, g->height, fg, bg);
        return g->advance;
    }
    const int adv = textPx_ / 2 + 2;
    if (cp >= 0x20 && cp < 0x7F) {
        char buf[2] = {static_cast<char>(cp), 0};
        dst.setFont(&lgfx::fonts::Font0);
        dst.setTextSize(1);
        dst.setTextColor(fg, bg);
        dst.setTextDatum(lgfx::top_left);
        dst.drawString(buf, x, y + ascent_ - 7);
    } else {
        dst.drawRect(x, y + ascent_ - textPx_ + 2, adv - 1, textPx_ - 2, fg);
    }
    return adv;
}

int Renderer::drawText(lgfx::LovyanGFX& dst, const char* s, int x, int y, uint16_t fg, uint16_t bg) {
    int pen = x;
    while (uint32_t cp = utf8Next(&s)) pen += drawChar(dst, cp, pen, y, fg, bg);
    return pen - x;
}

int Renderer::drawRange(lgfx::LovyanGFX& dst, const char* b, const char* e, int x, int y,
                        uint16_t fg, uint16_t bg) {
    int pen = x;
    while (b < e) {
        uint32_t cp = utf8Next(&b);
        if (!cp) break;
        pen += drawChar(dst, cp, pen, y, fg, bg);
    }
    return pen - x;
}

int Renderer::drawWrapped(lgfx::LovyanGFX& dst, const char* s, int x, int y, int maxW, int maxLines,
                          uint16_t fg, uint16_t bg) {
    int lines = 0;
    const char* p = s;
    const int ellW = textWidth(kEllipsis);
    while (*p && lines < maxLines) {
        const bool lastLine = (lines == maxLines - 1);
        const char* q = p;
        const char* lastBreak = nullptr;   // end of text at the last break opportunity
        const char* afterBreak = nullptr;  // where the next line starts
        const char* fitEnd = p;            // end of the longest prefix that fits
        const char* ellEnd = p;            // same, leaving room for the ellipsis
        int w = 0;
        bool overflow = false;
        while (*q) {
            const char* start = q;
            uint32_t cp = utf8Next(&q);
            int adv = charAdvance(cp);
            if (w + adv > maxW && start != p) {
                overflow = true;
                break;
            }
            if (cp == ' ' && w > 0) {
                lastBreak = start;
                afterBreak = q;
            }
            w += adv;
            fitEnd = q;
            if (w + ellW <= maxW) ellEnd = q;
            if (cp >= 0x2E80) {  // CJK may break after any character
                lastBreak = q;
                afterBreak = q;
            }
        }
        const int ly = y + lines * lineH_;
        ++lines;
        if (!overflow) {
            drawRange(dst, p, fitEnd, x, ly, fg, bg);
            break;
        }
        if (lastLine) {
            int dw = drawRange(dst, p, ellEnd, x, ly, fg, bg);
            drawRange(dst, kEllipsis, kEllipsis + 3, x + dw, ly, fg, bg);
            break;
        }
        const bool useBreak = lastBreak && lastBreak > p;
        drawRange(dst, p, useBreak ? lastBreak : fitEnd, x, ly, fg, bg);
        p = useBreak ? afterBreak : fitEnd;
        while (*p == ' ') ++p;
    }
    return lines;
}

}  // namespace tgc
