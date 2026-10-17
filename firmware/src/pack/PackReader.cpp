#include "pack/PackReader.h"

#include <cstdlib>
#include <cstring>

namespace tgc {

namespace {
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline bool inBounds(uint64_t off, uint64_t len, uint64_t total) { return off + len <= total; }
void* stdAlloc(size_t n) { return std::malloc(n); }
void stdFree(void* p) { std::free(p); }
}  // namespace

Allocator defaultAllocator() { return Allocator{stdAlloc, stdFree}; }

const char* packErrorString(PackError e) {
    switch (e) {
        case PackError::Ok: return "ok";
        case PackError::Io: return "read error";
        case PackError::BadMagic: return "not a pack file";
        case PackError::BadVersion: return "unsupported pack version";
        case PackError::BadHeader: return "corrupt pack header";
        case PackError::BadTable: return "corrupt pack tables";
        case PackError::NoMemory: return "out of memory";
    }
    return "?";
}

void PackReader::close() {
    if (alloc_.release) {
        if (cards_) alloc_.release(cards_);
        if (glyphs_) alloc_.release(glyphs_);
        if (strings_) alloc_.release(strings_);
    }
    cards_ = nullptr;
    glyphs_ = nullptr;
    strings_ = nullptr;
    src_ = nullptr;
    hdr_ = PackHeader{};
}

PackError PackReader::open(PackSource& src, Allocator alloc) {
    close();
    alloc_ = alloc;
    const uint32_t total = src.size();

    uint8_t h[kPackHeaderSize];
    if (total < kPackHeaderSize) return PackError::BadHeader;
    if (!src.read(0, h, sizeof h)) return PackError::Io;
    if (std::memcmp(h, "TNGC", 4) != 0) return PackError::BadMagic;

    PackHeader hd{};
    hd.version = rd16(h + 4);
    if (hd.version != 1) return PackError::BadVersion;
    hd.headerSize = rd16(h + 6);
    if (hd.headerSize < kPackHeaderSize || hd.headerSize > total) return PackError::BadHeader;
    hd.cardCount = rd32(h + 8);
    hd.glyphCount = rd32(h + 12);
    hd.cardsOff = rd32(h + 16);
    hd.glyphsOff = rd32(h + 20);
    hd.distractOff = rd32(h + 24);
    hd.stringsOff = rd32(h + 28);
    hd.stringsSize = rd32(h + 32);
    hd.glyphDataOff = rd32(h + 36);
    hd.deckNameOff = rd32(h + 40);
    std::memcpy(hd.lang, h + 44, 4);
    hd.lang[4] = 0;
    hd.largePx = h[48];
    hd.smallPx = h[49];
    hd.textPx = h[50];
    hd.textLineHeight = h[51];
    hd.textAscent = h[52];
    hd.optionPx = h[53];
    hd.optionLineHeight = h[54];
    hd.optionAscent = h[55];
    hd.buildTime = rd32(h + 56);

    if (hd.cardCount > 65535 || hd.cardCount == 0) return PackError::BadHeader;
    const uint64_t cardBytes = static_cast<uint64_t>(hd.cardCount) * kCardSize;
    const uint64_t glyphBytes = static_cast<uint64_t>(hd.glyphCount) * kGlyphEntrySize;
    if (!inBounds(hd.cardsOff, cardBytes, total) || hd.cardsOff < hd.headerSize) return PackError::BadHeader;
    if (!inBounds(hd.glyphsOff, glyphBytes, total)) return PackError::BadHeader;
    if (hd.distractOff > total || hd.glyphDataOff > total) return PackError::BadHeader;
    if (!inBounds(hd.stringsOff, hd.stringsSize, total) || hd.stringsSize == 0) return PackError::BadHeader;
    if (hd.deckNameOff < hd.stringsOff || hd.deckNameOff - hd.stringsOff >= hd.stringsSize) return PackError::BadHeader;

    // --- load tables -------------------------------------------------------
    cards_ = static_cast<uint8_t*>(alloc.alloc(static_cast<size_t>(cardBytes)));
    glyphs_ = static_cast<Glyph*>(alloc.alloc(sizeof(Glyph) * (hd.glyphCount ? hd.glyphCount : 1)));
    strings_ = static_cast<char*>(alloc.alloc(hd.stringsSize));
    src_ = &src;  // so close() below releases correctly
    hdr_ = hd;
    auto fail = [&](PackError e) {
        close();
        return e;
    };
    if (!cards_ || !glyphs_ || !strings_) return fail(PackError::NoMemory);

    if (!src.read(hd.cardsOff, cards_, static_cast<size_t>(cardBytes))) return fail(PackError::Io);
    if (!src.read(hd.stringsOff, strings_, hd.stringsSize)) return fail(PackError::Io);
    // Guarantee termination so string access can never run off the pool.
    strings_[hd.stringsSize - 1] = 0;

    // Glyph table: stream in chunks to keep the stack small.
    uint8_t chunk[kGlyphEntrySize * 32];
    for (uint32_t i = 0; i < hd.glyphCount;) {
        uint32_t n = hd.glyphCount - i;
        if (n > 32) n = 32;
        if (!src.read(hd.glyphsOff + i * kGlyphEntrySize, chunk, n * kGlyphEntrySize)) return fail(PackError::Io);
        for (uint32_t k = 0; k < n; ++k, ++i) {
            const uint8_t* e = chunk + k * kGlyphEntrySize;
            Glyph& g = glyphs_[i];
            g.codepoint = rd32(e);
            g.set = e[4];
            g.encoding = e[5];
            g.width = e[6];
            g.height = e[7];
            g.xOff = static_cast<int8_t>(e[8]);
            g.yOff = static_cast<int8_t>(e[9]);
            g.advance = e[10];
            g.dataOff = rd32(e + 12);
            if (g.set > kGlyphSetOption) return fail(PackError::BadTable);
            if (g.encoding != 0) return fail(PackError::BadTable);  // only raw 4bpp is defined
            if (i > 0) {
                const Glyph& p = glyphs_[i - 1];
                if (g.set < p.set || (g.set == p.set && g.codepoint <= p.codepoint)) return fail(PackError::BadTable);
            }
            if (g.encoding == 0 && !inBounds(g.dataOff, bitmapSize(g), total)) return fail(PackError::BadTable);
        }
    }

    // Card table: validate string offsets and distractor offsets.
    for (uint32_t i = 0; i < hd.cardCount; ++i) {
        const uint8_t* c = cards_ + i * kCardSize;
        for (int f = 4; f <= 12; f += 4) {
            uint32_t so = rd32(c + f);
            if (so < hd.stringsOff || so - hd.stringsOff >= hd.stringsSize) return fail(PackError::BadTable);
        }
        uint32_t d = rd32(c + 16);
        if (d < hd.distractOff || !inBounds(d, 2, total)) return fail(PackError::BadTable);
        uint32_t dr = rd32(c + 28);
        if (dr != 0 && (dr < hd.distractOff || !inBounds(dr, 2, total)))
            return fail(PackError::BadTable);
    }
    return PackError::Ok;
}

const char* PackReader::str(uint32_t off) const {
    // String offsets are absolute file offsets like every other offset.
    if (!strings_ || off < hdr_.stringsOff || off - hdr_.stringsOff >= hdr_.stringsSize) return "";
    return strings_ + (off - hdr_.stringsOff);
}

bool PackReader::card(uint32_t idx, Card* out) const {
    if (!cards_ || idx >= hdr_.cardCount) return false;
    const uint8_t* c = cards_ + idx * kCardSize;
    out->codepoint = rd32(c);
    out->meanings = str(rd32(c + 4));
    out->onyomi = str(rd32(c + 8));
    out->kunyomi = str(rd32(c + 12));
    out->distractorOff = rd32(c + 16);
    out->distractorReadOff = rd32(c + 28);
    out->jlpt = c[20];
    out->grade = c[21];
    out->strokes = c[22];
    out->flags = c[23];
    out->freq = rd16(c + 24);
    out->level = rd16(c + 26);
    return true;
}

uint16_t PackReader::distractors(const Card& c, uint16_t* out, uint16_t cap, uint16_t* total,
                                 bool reading) const {
    if (total) *total = 0;
    if (!src_) return 0;
    const uint32_t off = (reading && c.distractorReadOff) ? c.distractorReadOff : c.distractorOff;
    uint8_t b[2];
    if (!src_->read(off, b, 2)) return 0;
    uint16_t n = rd16(b);
    if (total) *total = n;
    if (n > cap) n = cap;
    // Read in chunks to avoid needing an endian-aware bulk buffer.
    uint16_t done = 0;
    uint8_t buf[64];
    while (done < n) {
        uint16_t k = n - done;
        if (k > sizeof(buf) / 2) k = sizeof(buf) / 2;
        if (!src_->read(off + 2 + done * 2u, buf, k * 2u)) return done;
        for (uint16_t i = 0; i < k; ++i) out[done + i] = rd16(buf + i * 2);
        done += k;
    }
    return done;
}

const Glyph* PackReader::findGlyph(uint8_t set, uint32_t codepoint) const {
    if (!glyphs_) return nullptr;
    uint32_t lo = 0, hi = hdr_.glyphCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const Glyph& g = glyphs_[mid];
        if (g.set == set && g.codepoint == codepoint) return &g;
        if (g.set < set || (g.set == set && g.codepoint < codepoint)) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

bool PackReader::readGlyphBitmap(const Glyph& g, uint8_t* buf, size_t cap) const {
    if (!src_ || g.encoding != 0) return false;
    size_t n = bitmapSize(g);
    if (n > cap) return false;
    if (n == 0) return true;
    return src_->read(g.dataOff, buf, n);
}

}  // namespace tgc
