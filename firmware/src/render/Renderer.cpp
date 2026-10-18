#include "render/Renderer.h"

#include <esp_timer.h>

#include <cstring>

#include "pack/Utf8.h"
#include "render/Blend565.h"
#include "render/GlcdCp437.h"
#include "ui/UiFont.h"

namespace tgc {

namespace {
const char kEllipsis[] = "...";

// Font0 glyph metrics (see lovyanGFX GLCDfont): 6 px wide, 8 px tall, baseline
// at 7. Drawn at 1x or 2x (kTextBuiltin) depending on the room available.
constexpr int kBuiltinGlyphW = 6;
constexpr int kBuiltinGlyphH = 8;
constexpr int kBuiltinGlyphAscent = 7;

inline bool isBuiltin(uint8_t set) {
    return set == Renderer::kTextBuiltin || set == Renderer::kTextBuiltinSmall;
}
inline int builtinScale(uint8_t set) { return set == Renderer::kTextBuiltin ? 2 : 1; }

// The UI font face of a text style, or null for the pack sets and Font0.
inline const UiFace* uiFace(uint8_t set) {
    if (set == Renderer::kTextUi) return &uifont::kLabel;
    if (set == Renderer::kTextUiNumeral) return &uifont::kNumeral;
    return nullptr;
}
// Width of the box drawn for a character a UI face (and the pack) lacks.
inline int uiMissingAdvance(const UiFace& f) { return f.px / 2 + 2; }
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

void drawBitmap1bpp(lgfx::LovyanGFX& dst, int x, int y, const uint8_t* bits, int w, int h,
                    uint16_t ink) {
    const int stride = (w + 7) / 8;
    dst.startWrite();
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = bits + row * stride;
        for (int col = 0; col < w; ++col)
            if (p[col >> 3] & (0x80 >> (col & 7))) dst.drawPixel(x + col, y + row, ink);
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
    // No set 3 in the pack -> set 3 uses set 2's metrics and glyphs.
    optLineH_ = h.optionLineHeight ? h.optionLineHeight : lineH_;
    optAscent_ = h.optionAscent ? h.optionAscent : ascent_;
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
    scratchGlyph_ = kNoGlyph;
    pack_ = nullptr;
}

int Renderer::lineHeight(uint8_t set) const {
    if (isBuiltin(set)) return kBuiltinGlyphH * builtinScale(set);
    if (const UiFace* f = uiFace(set)) return f->pitch;
    return set == kGlyphSetOption ? optLineH_ : lineH_;
}
int Renderer::ascent(uint8_t set) const {
    if (isBuiltin(set)) return kBuiltinGlyphAscent * builtinScale(set);
    // The baseline that centres the cap height in the compact pitch.
    if (const UiFace* f = uiFace(set)) return (f->pitch + f->capHeight + 1) / 2;
    return set == kGlyphSetOption ? optAscent_ : ascent_;
}
int Renderer::capHeight(uint8_t set) const {
    if (isBuiltin(set)) return kBuiltinGlyphAscent * builtinScale(set);
    if (const UiFace* f = uiFace(set)) return f->capHeight;
    if (pack_)
        if (const Glyph* g = glyphFor(set, 'H'))
            if (g->height) return -g->yOff;
    return ascent(set) * 2 / 3;
}

bool Renderer::builtinCanDraw(const char* s) {
    const char* p = s;
    while (uint32_t cp = utf8Next(&p))
        if (glcdIndex(cp) < 0) return false;
    return true;
}

// The shared scratch buffer, grown to at least n bytes (null if out of memory).
uint8_t* Renderer::scratchFor(size_t n) {
    if (n > scratchCap_) {
        const Allocator& a = pack_->allocator();
        if (scratch_) a.release(scratch_);
        scratch_ = static_cast<uint8_t*>(a.alloc(n));
        scratchCap_ = scratch_ ? n : 0;
        scratchGlyph_ = kNoGlyph;
    }
    return scratch_;
}

// Returns the decoded bitmap (cached for small glyph sets, scratch for set 0).
const uint8_t* Renderer::bitmapFor(const Glyph& g) {
    lastFetchUs_ = 0;
    size_t n = PackReader::bitmapSize(g);
    if (n == 0) return nullptr;
    const Allocator& a = pack_->allocator();
    const uint32_t idx = pack_->glyphIndex(&g);
    if (g.set != kGlyphSetLarge) {
        if (cache_[idx]) return cache_[idx];
        uint8_t* buf = static_cast<uint8_t*>(a.alloc(n));
        const int64_t t0 = esp_timer_get_time();
        if (buf && pack_->readGlyphBitmap(g, buf, n)) {
            lastFetchUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
            cache_[idx] = buf;
            return buf;
        }
        if (buf) a.release(buf);
        return nullptr;
    }
    if (scratchGlyph_ == idx) return scratch_;
    if (!scratchFor(n)) return nullptr;
    const int64_t t0 = esp_timer_get_time();
    if (!pack_->readGlyphBitmap(g, scratch_, n)) {
        scratchGlyph_ = kNoGlyph;
        return nullptr;
    }
    lastFetchUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
    scratchGlyph_ = idx;
    return scratch_;
}

const Glyph* Renderer::glyphFor(uint8_t set, uint32_t cp) const {
    if (const Glyph* g = pack_->findGlyph(set, cp)) return g;
    if (set != kGlyphSetText) return pack_->findGlyph(kGlyphSetText, cp);
    return nullptr;
}

const Glyph* Renderer::uiFallback(uint32_t cp) const {
    // Only with a pack the renderer could start on (cache_ exists).
    return pack_ && cache_ ? pack_->findGlyph(kGlyphSetText, cp) : nullptr;
}

int Renderer::charAdvance(uint32_t cp, uint8_t set) const {
    if (isBuiltin(set)) return kBuiltinGlyphW * builtinScale(set);
    if (const UiFace* f = uiFace(set)) {
        if (const UiGlyph* g = uiGlyph(*f, cp)) return g->advance;
        if (const Glyph* g = uiFallback(cp)) return g->advance;
        return uiMissingAdvance(*f);
    }
    if (const Glyph* g = glyphFor(set, cp)) return g->advance;
    return textPx_ / 2 + 2;  // fallback font / '?' box width
}

int Renderer::textWidth(const char* s, uint8_t set) const {
    int w = 0;
    while (uint32_t cp = utf8Next(&s)) w += charAdvance(cp, set);
    return w;
}

int Renderer::textRangeWidth(const char* b, const char* e, uint8_t set) const {
    int w = 0;
    while (b < e) {
        uint32_t cp = utf8Next(&b);
        if (!cp) break;
        w += charAdvance(cp, set);
    }
    return w;
}

int Renderer::drawCodepoint(lgfx::LovyanGFX& dst, uint32_t cp, int x, int y, uint16_t fg,
                            uint16_t bg, uint8_t set) {
    if (isBuiltin(set)) {
        dst.setFont(&lgfx::fonts::Font0);
        dst.setTextSize(builtinScale(set));
        dst.setTextColor(fg, bg);
        dst.setTextDatum(lgfx::top_left);
        dst.cp437(true);
        const int ix = glcdIndex(cp);
        if (ix >= 0) dst.drawChar(static_cast<uint16_t>(ix), x, y);
        return kBuiltinGlyphW * builtinScale(set);
    }
    const int asc = ascent(set);
    if (const UiFace* f = uiFace(set)) {
        const int base = y + asc;
        if (const UiGlyph* g = uiGlyph(*f, cp)) {
            if (g->width)
                drawBitmap4bpp(dst, x + g->xOff, base + g->yOff, uiGlyphBits(*f, *g), g->width,
                               g->height, fg, bg);
            return g->advance;
        }
        if (const Glyph* g = uiFallback(cp)) {
            const uint8_t* bits = bitmapFor(*g);
            if (bits) drawBitmap4bpp(dst, x + g->xOff, base + g->yOff, bits, g->width, g->height, fg, bg);
            return g->advance;
        }
        const int adv = uiMissingAdvance(*f);
        dst.drawRect(x + 1, base - f->capHeight, adv - 2, f->capHeight, fg);
        return adv;
    }
    if (const Glyph* g = glyphFor(set, cp)) {
        const uint8_t* bits = bitmapFor(*g);
        if (bits) drawBitmap4bpp(dst, x + g->xOff, y + asc + g->yOff, bits, g->width, g->height, fg, bg);
        return g->advance;
    }
    const int adv = textPx_ / 2 + 2;
    if (cp >= 0x20 && cp < 0x7F) {
        char buf[2] = {static_cast<char>(cp), 0};
        dst.setFont(&lgfx::fonts::Font0);
        dst.setTextSize(1);
        dst.setTextColor(fg, bg);
        dst.setTextDatum(lgfx::top_left);
        dst.drawString(buf, x, y + asc - 7);
    } else {
        dst.drawRect(x, y + asc - textPx_ + 2, adv - 1, textPx_ - 2, fg);
    }
    return adv;
}

int Renderer::drawRange(lgfx::LovyanGFX& dst, const char* b, const char* e, int x, int y,
                        uint16_t fg, uint16_t bg, uint8_t set) {
    int pen = x;
    while (b < e) {
        uint32_t cp = utf8Next(&b);
        if (!cp) break;
        pen += drawCodepoint(dst, cp, pen, y, fg, bg, set);
    }
    return pen - x;
}

int Renderer::drawText(lgfx::LovyanGFX& dst, const char* s, int x, int y, uint16_t fg, uint16_t bg,
                       uint8_t set) {
    int pen = x;
    while (uint32_t cp = utf8Next(&s)) pen += drawCodepoint(dst, cp, pen, y, fg, bg, set);
    return pen - x;
}

// Greedy wrap; fills `out` with up to `cap` line ranges and returns the count.
// The final line carries an ellipsis flag when the text did not fit.
int Renderer::wrap(const char* s, int maxW, int maxLines, uint8_t set, Line* out, int cap) const {
    int lines = 0;
    const char* p = s;
    const int ellW = textWidth(kEllipsis, set);
    while (*p && lines < maxLines && lines < cap) {
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
            int adv = charAdvance(cp, set);
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
        if (!overflow) {
            out[lines++] = Line{p, fitEnd, false};
            break;
        }
        if (lastLine) {
            out[lines++] = Line{p, ellEnd, true};
            break;
        }
        const bool useBreak = lastBreak && lastBreak > p;
        out[lines++] = Line{p, useBreak ? lastBreak : fitEnd, false};
        p = useBreak ? afterBreak : fitEnd;
        while (*p == ' ') ++p;
    }
    return lines;
}

int Renderer::drawWrapped(lgfx::LovyanGFX& dst, const char* s, int x, int y, int maxW, int maxLines,
                          uint16_t fg, uint16_t bg, uint8_t set) {
    Line lines[kMaxWrapLines];
    const int n = wrap(s, maxW, maxLines, set, lines, kMaxWrapLines);
    const int lh = lineHeight(set);
    for (int i = 0; i < n; ++i) {
        const int ly = y + i * lh;
        int dw = drawRange(dst, lines[i].begin, lines[i].end, x, ly, fg, bg, set);
        if (lines[i].ellipsis) drawRange(dst, kEllipsis, kEllipsis + 3, x + dw, ly, fg, bg, set);
    }
    return n;
}

int Renderer::lineCount(const char* s, int maxW, int maxLines, uint8_t set) const {
    if (!s) return 0;
    Line lines[kMaxWrapLines];
    return wrap(s, maxW, maxLines, set, lines, kMaxWrapLines);
}

int Renderer::drawCentred(lgfx::LovyanGFX& dst, const char* s, int x, int y, int w, int h,
                          int maxLines, uint16_t fg, uint16_t bg, uint8_t set) {
    Line lines[kMaxWrapLines];
    const int n = wrap(s, w, maxLines, set, lines, kMaxWrapLines);
    const int lh = lineHeight(set);
    const int top = y + (h - n * lh) / 2;
    for (int i = 0; i < n; ++i) {
        int dw = textRangeWidth(lines[i].begin, lines[i].end, set);
        if (lines[i].ellipsis) dw += textWidth(kEllipsis, set);
        int tx = x + (w - dw) / 2;
        if (tx < x) tx = x;
        const int ly = top + i * lh;
        int drawn = drawRange(dst, lines[i].begin, lines[i].end, tx, ly, fg, bg, set);
        if (lines[i].ellipsis) drawRange(dst, kEllipsis, kEllipsis + 3, tx + drawn, ly, fg, bg, set);
    }
    return n;
}

int Renderer::drawLabel(lgfx::LovyanGFX& dst, const char* s, int x, int y, int w, int h,
                        uint16_t fg, uint16_t bg, uint8_t set, Align align) {
    Line line;
    if (!s || wrap(s, w, 1, set, &line, 1) == 0) return 0;
    // "Los ..." reads worse than "Los...": no space before the ellipsis.
    if (line.ellipsis)
        while (line.end > line.begin && line.end[-1] == ' ') --line.end;
    const int ellW = line.ellipsis ? textWidth(kEllipsis, set) : 0;
    const int tw = textRangeWidth(line.begin, line.end, set) + ellW;
    int tx = x;
    if (align == Align::Right)
        tx = x + w - tw;
    else if (align == Align::Centre)
        tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    const int top = y + (h + capHeight(set)) / 2 - ascent(set);  // baseline - ascent
    const int drawn = drawRange(dst, line.begin, line.end, tx, top, fg, bg, set);
    if (line.ellipsis) drawRange(dst, kEllipsis, kEllipsis + 3, tx + drawn, top, fg, bg, set);
    return tw;
}

void Renderer::drawKanji(lgfx::LovyanGFX& dst, uint8_t set, uint32_t cp, int boxX, int boxY,
                         int boxW, int boxH, uint16_t fg, uint16_t bg) {
    const Glyph* g = pack_->findGlyph(set, cp);
    const int px = set == kGlyphSetLarge ? pack_->header().largePx : pack_->header().smallPx;
    const int penX = boxX + (boxW - px) / 2;
    const int baseY = boxY + (boxH - px) / 2 + px;  // baseline = bottom edge of the em square
    if (!g) {
        dst.drawRect(penX, baseY - px, px, px, fg);
        dst.setFont(&lgfx::fonts::Font0);
        dst.setTextSize(1);
        dst.setTextColor(fg, bg);
        dst.setTextDatum(lgfx::top_left);
        dst.drawString("?", penX + px / 2 - 3, baseY - px / 2 - 4);
        return;
    }
    const uint8_t* bits = bitmapFor(*g);
    if (bits) drawBitmap4bpp(dst, penX + g->xOff, baseY + g->yOff, bits, g->width, g->height, fg, bg);
}

bool Renderer::drawStroke(lgfx::LovyanGFX& dst, uint32_t cardIdx, uint16_t i, int emX, int emY,
                          uint16_t ink) {
    if (!pack_) return false;
    Stroke s;
    if (!pack_->strokeAt(cardIdx, i, &s)) return false;
    const size_t n = PackReader::strokeBitmapSize(s);
    if (n == 0 || !scratchFor(n)) return false;
    scratchGlyph_ = kNoGlyph;  // the scratch now holds a mask
    if (!pack_->readStrokeBitmap(s, scratch_, n)) return false;
    drawBitmap1bpp(dst, emX + s.xOff, emY + s.yOff, scratch_, s.w, s.h, ink);
    return true;
}

}  // namespace tgc
