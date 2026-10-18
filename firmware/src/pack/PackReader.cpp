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

namespace {
// Reads and checks the 64-byte header: magic, version, sizes, and that every
// table and the deck name lie inside the file. Shared by PackReader::open()
// and peekPackInfo().
PackError readHeader(PackSource& src, PackHeader* out) {
    const uint32_t total = src.size();
    uint8_t h[kPackHeaderSize];
    if (total < kPackHeaderSize) return PackError::BadHeader;
    if (!src.read(0, h, sizeof h)) return PackError::Io;
    if (std::memcmp(h, "TNGC", 4) != 0) return PackError::BadMagic;

    PackHeader hd{};
    hd.version = rd16(h + 4);
    if (hd.version < kPackVersionMin || hd.version > kPackVersionMax) return PackError::BadVersion;
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
    hd.strokeOff = rd32(h + 60);

    if (hd.cardCount > 65535 || hd.cardCount == 0) return PackError::BadHeader;
    const uint64_t cardBytes = static_cast<uint64_t>(hd.cardCount) * kCardSize;
    const uint64_t glyphBytes = static_cast<uint64_t>(hd.glyphCount) * kGlyphEntrySize;
    if (!inBounds(hd.cardsOff, cardBytes, total) || hd.cardsOff < hd.headerSize) return PackError::BadHeader;
    if (!inBounds(hd.glyphsOff, glyphBytes, total)) return PackError::BadHeader;
    if (hd.distractOff > total || hd.glyphDataOff > total) return PackError::BadHeader;
    if (!inBounds(hd.stringsOff, hd.stringsSize, total) || hd.stringsSize == 0) return PackError::BadHeader;
    if (hd.deckNameOff < hd.stringsOff || hd.deckNameOff - hd.stringsOff >= hd.stringsSize) return PackError::BadHeader;
    *out = hd;
    return PackError::Ok;
}
}  // namespace

PackError peekPackInfo(PackSource& src, PackInfo* out) {
    *out = PackInfo{};
    PackHeader hd;
    const PackError e = readHeader(src, &hd);
    if (e != PackError::Ok) return e;
    // The name ends at its NUL, the end of the string pool or the buffer.
    const uint32_t poolLeft = hd.stringsOff + hd.stringsSize - hd.deckNameOff;
    const size_t n = poolLeft < sizeof out->name - 1 ? poolLeft : sizeof out->name - 1;
    if (!src.read(hd.deckNameOff, out->name, n)) return PackError::Io;
    out->name[n] = 0;
    // A name cut at the buffer drops a character it split.
    const size_t len = std::strlen(out->name);
    if (len == n) {
        size_t lead = len;
        while (lead > 0 && (static_cast<uint8_t>(out->name[lead - 1]) & 0xC0) == 0x80) --lead;
        if (lead > 0 && static_cast<uint8_t>(out->name[lead - 1]) >= 0xC0) {
            const uint8_t b = static_cast<uint8_t>(out->name[lead - 1]);
            const size_t need = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2);
            if (len - (lead - 1) < need) out->name[lead - 1] = 0;
        }
    }
    std::memcpy(out->lang, hd.lang, sizeof out->lang);
    out->cards = hd.cardCount;
    out->version = hd.version;
    return PackError::Ok;
}

PackError PackReader::open(PackSource& src, Allocator alloc) {
    close();
    alloc_ = alloc;
    const uint32_t total = src.size();
    PackHeader hd;
    const PackError he = readHeader(src, &hd);
    if (he != PackError::Ok) return he;
    const bool v2 = hd.version >= 2;
    const uint64_t cardBytes = static_cast<uint64_t>(hd.cardCount) * kCardSize;

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
            if (i > 0) {
                const Glyph& p = glyphs_[i - 1];
                if (g.set < p.set || (g.set == p.set && g.codepoint <= p.codepoint)) return fail(PackError::BadTable);
            }
            if (g.encoding == kGlyphEncRaw) {
                if (!inBounds(g.dataOff, bitmapSize(g), total)) return fail(PackError::BadTable);
            } else if (g.encoding == kGlyphEncRle && v2) {
                // The stream length is implied by its codes; it starts inside
                // the glyph data and is checked while decoding (readGlyphBitmap).
                if (bitmapSize(g) == 0 || g.dataOff < hd.glyphDataOff || g.dataOff >= total)
                    return fail(PackError::BadTable);
            } else {
                return fail(PackError::BadTable);  // unknown encoding, or RLE in a v1 pack
            }
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

    // Stroke section (optional; stroke_off != 0). Validate the whole index,
    // every block and every mask so a corrupt pack is rejected here, exactly
    // like the other tables, instead of failing at draw time.
    if (hd.strokeOff != 0) {
        if ((hd.strokeOff & 3u) || !inBounds(hd.strokeOff, static_cast<uint64_t>(hd.cardCount) * 4, total))
            return fail(PackError::BadTable);
        uint8_t idx[256];
        for (uint32_t i = 0; i < hd.cardCount;) {
            uint32_t m = hd.cardCount - i;
            if (m > sizeof(idx) / 4) m = sizeof(idx) / 4;
            if (!src.read(hd.strokeOff + i * 4u, idx, m * 4u)) return fail(PackError::Io);
            for (uint32_t q = 0; q < m; ++q, ++i) {
                const uint32_t blk = rd32(idx + q * 4);
                if (blk == 0) continue;  // no stroke data for this card
                const PackError e = checkStrokeBlock(blk, total);
                if (e != PackError::Ok) return fail(e);
            }
        }
    }
    return PackError::Ok;
}

// One card's stroke block: header, entries and (bounds of) every mask. v1 entries
// carry an absolute offset per raw mask; v2 entries carry encoding and size, the
// masks following each other from the block's data_off.
PackError PackReader::checkStrokeBlock(uint32_t blk, uint32_t total) const {
    if ((blk & 3u) || !inBounds(blk, 8, total)) return PackError::BadTable;
    uint8_t sh[8];
    if (!src_->read(blk, sh, 8)) return PackError::Io;
    const uint16_t count = rd16(sh);
    const uint16_t px = rd16(sh + 2);
    if (px == 0 || px != hdr_.largePx) return PackError::BadTable;
    if (!inBounds(blk + 8ull, static_cast<uint64_t>(count) * 8, total)) return PackError::BadTable;
    const bool v2 = hdr_.version >= 2;
    uint64_t cursor = rd32(sh + 4);  // v2: first mask
    uint8_t eb[8 * 32];
    for (uint16_t k = 0; k < count;) {
        uint16_t z = count - k;
        if (z > 32) z = 32;
        if (!src_->read(blk + 8 + k * 8u, eb, z * 8u)) return PackError::Io;
        for (uint16_t j = 0; j < z; ++j) {
            const uint8_t* e = eb + j * 8;
            const uint8_t w = e[2], hgt = e[3];
            if (w == 0 || hgt == 0) return PackError::BadTable;
            const uint64_t raw = static_cast<uint64_t>((w + 7) / 8) * hgt;
            if (v2) {
                const uint8_t enc = e[4];
                const uint16_t size = rd16(e + 6);
                if (enc > kMaskEncXorRuns) return PackError::BadTable;
                if (enc == kMaskEncRaw ? size != raw : size > raw) return PackError::BadTable;
                if (!inBounds(cursor, size, total)) return PackError::BadTable;
                cursor += size;
            } else if (!inBounds(rd32(e + 4), raw, total)) {
                return PackError::BadTable;
            }
        }
        k += z;
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
    if (!src_) return false;
    const size_t n = bitmapSize(g);
    if (n > cap) return false;
    if (n == 0) return true;
    if (g.encoding == kGlyphEncRaw) return src_->read(g.dataOff, buf, n);
    if (g.encoding != kGlyphEncRle) return false;
    // The stream is never longer than the raw bitmap (docs/pack-format.md), so
    // that bounds the input; the window reads only as much as the codes need.
    const uint32_t total = src_->size();
    if (g.dataOff >= total) return false;
    const uint32_t avail = total - g.dataOff;
    ByteIn in(*src_, g.dataOff, n < avail ? static_cast<uint32_t>(n) : avail);
    return decodeGlyphRle(in, g.width, g.height, buf, cap);
}

bool PackReader::strokeBlock(uint32_t cardIdx, uint32_t* entriesOff, uint16_t* count,
                             uint16_t* px, uint32_t* firstMask) const {
    if (!src_ || hdr_.strokeOff == 0 || cardIdx >= hdr_.cardCount) return false;
    uint8_t ib[4];
    if (!src_->read(hdr_.strokeOff + cardIdx * 4u, ib, 4)) return false;
    const uint32_t blk = rd32(ib);
    if (blk == 0) return false;  // no stroke data for this card
    uint8_t sh[8];
    if (!src_->read(blk, sh, 8)) return false;
    if (entriesOff) *entriesOff = blk + 8;
    if (count) *count = rd16(sh);
    if (px) *px = rd16(sh + 2);
    if (firstMask) *firstMask = rd32(sh + 4);  // v2 only (reserved 0 in v1)
    return true;
}

uint16_t PackReader::strokeCount(uint32_t cardIdx) const {
    uint16_t n = 0;
    return strokeBlock(cardIdx, nullptr, &n, nullptr) ? n : 0;
}

bool PackReader::strokeInfo(uint32_t cardIdx, uint16_t* count, uint16_t* px) const {
    uint16_t n = 0, p = 0;
    if (!strokeBlock(cardIdx, nullptr, &n, &p)) return false;
    if (count) *count = n;
    if (px) *px = p;
    return true;
}

bool PackReader::strokeAt(uint32_t cardIdx, uint16_t i, Stroke* out) const {
    uint32_t entries = 0, mask = 0;
    uint16_t count = 0;
    if (!out || !strokeBlock(cardIdx, &entries, &count, nullptr, &mask) || i >= count) return false;
    uint8_t e[8];
    if (hdr_.version < 2) {
        if (!src_->read(entries + i * 8u, e, 8)) return false;
        out->encoding = kMaskEncRaw;
        out->size = static_cast<uint16_t>((e[2] + 7) / 8 * e[3]);
        out->dataOff = rd32(e + 4);
    } else {
        // v2: masks are back to back, so stroke i starts after the sizes of
        // strokes 0..i-1 (read in one go: entries are 8 bytes).
        uint8_t eb[8 * 32];
        for (uint16_t k = 0; k <= i;) {
            uint16_t z = static_cast<uint16_t>(i + 1 - k);
            if (z > 32) z = 32;
            if (!src_->read(entries + k * 8u, eb, z * 8u)) return false;
            for (uint16_t j = 0; j < z; ++j, ++k) {
                if (k == i) {
                    std::memcpy(e, eb + j * 8, 8);
                } else {
                    mask += rd16(eb + j * 8 + 6);
                }
            }
        }
        out->encoding = e[4];
        out->size = rd16(e + 6);
        out->dataOff = mask;
    }
    out->xOff = static_cast<int8_t>(e[0]);
    out->yOff = static_cast<int8_t>(e[1]);
    out->w = e[2];
    out->h = e[3];
    return true;
}

bool PackReader::readStrokeBitmap(const Stroke& s, uint8_t* buf, size_t cap) const {
    if (!src_) return false;
    const size_t n = strokeBitmapSize(s);
    if (n > cap) return false;
    if (n == 0) return true;
    if (s.encoding == kMaskEncRaw) return s.size == n && src_->read(s.dataOff, buf, n);
    if (s.encoding != kMaskEncRuns && s.encoding != kMaskEncXorRuns) return false;
    ByteIn in(*src_, s.dataOff, s.size);
    return decodeMaskRle(in, s.w, s.h, s.encoding == kMaskEncXorRuns, buf, cap);
}

}  // namespace tgc
