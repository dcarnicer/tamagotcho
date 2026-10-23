#include "pack/PackReader.h"

#include <cstdlib>
#include <cstring>
#include <new>

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

// --- streamed tables (PackMemory::Streamed) ---------------------------------
// The card table and the glyph table are read a block at a time into a few
// cached blocks (the least recently used goes first); strings into
// kStringSlots slots, each a buffer grown to the longest string it held. Next
// to the caches: every card's code point (deck summaries and queue rebuilds go
// over all of them) and the key of every glyph block's first entry, so a
// lookup is a binary search in memory and at most one block read.
namespace {
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kCardsPerBlock = 16;   // 512 bytes of card table
constexpr uint32_t kGlyphsPerBlock = 32;  // 512 bytes of glyph table
constexpr uint32_t kCardBlockSlots = 2;
constexpr uint32_t kStrChunk = 64;        // a string is read this much at a time

// (set, codepoint) as one ordered key. Code points are 21 bits, but a pack may
// hold any u32, so the set goes above bit 31.
inline uint64_t glyphKey(uint8_t set, uint32_t cp) {
    return (static_cast<uint64_t>(set) << 32) | cp;
}

// Glyph table entry `e` (16 bytes) as a Glyph.
inline void readGlyphEntry(const uint8_t* e, Glyph* g) {
    g->codepoint = rd32(e);
    g->set = e[4];
    g->encoding = e[5];
    g->width = e[6];
    g->height = e[7];
    g->xOff = static_cast<int8_t>(e[8]);
    g->yOff = static_cast<int8_t>(e[9]);
    g->advance = e[10];
    g->dataOff = rd32(e + 12);
}

// Binary search of `n` glyphs sorted by (set, codepoint).
const Glyph* searchGlyphs(const Glyph* glyphs, uint32_t n, uint8_t set, uint32_t codepoint) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const Glyph& g = glyphs[mid];
        if (g.set == set && g.codepoint == codepoint) return &g;
        if (g.set < set || (g.set == set && g.codepoint < codepoint)) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}
}  // namespace

struct PackReader::Stream {
    struct CardBlock {
        uint32_t block = kNone;
        uint32_t used = 0;
        uint8_t bytes[kCardsPerBlock * kCardSize];
    };
    struct GlyphBlock {
        uint32_t block = kNone;
        uint32_t used = 0;
        uint32_t count = 0;
        Glyph g[kGlyphsPerBlock];
    };
    struct StringSlot {
        uint32_t off = kNone;
        uint32_t used = 0;
        uint32_t cap = 0;
        char* buf = nullptr;
    };
    CardBlock cards[kCardBlockSlots];
    GlyphBlock glyphs[kGlyphBlockSlots];
    StringSlot strings[kStringSlots];
    uint32_t* cps = nullptr;        // every card's code point
    uint64_t* blockKeys = nullptr;  // glyphKey() of each glyph block's first entry
    uint32_t glyphBlocks = 0;
    uint32_t tick = 0;              // the use counter of the LRU caches
    size_t stringBytes = 0;         // the slots' buffers
};

void PackReader::releaseTables() {
    if (alloc_.release) {
        if (cards_) alloc_.release(cards_);
        if (glyphs_) alloc_.release(glyphs_);
        if (strings_) alloc_.release(strings_);
    }
    cards_ = nullptr;
    glyphs_ = nullptr;
    strings_ = nullptr;
}

void PackReader::close() {
    releaseTables();
    if (stream_) {
        if (alloc_.release) {
            for (Stream::StringSlot& s : stream_->strings)
                if (s.buf) alloc_.release(s.buf);
            if (stream_->cps) alloc_.release(stream_->cps);
            if (stream_->blockKeys) alloc_.release(stream_->blockKeys);
            stream_->~Stream();
            alloc_.release(stream_);
        }
        stream_ = nullptr;
    }
    src_ = nullptr;
    hdr_ = PackHeader{};
    wordsTable_ = 0;
    wordEntrySize_ = 0;
}

size_t PackReader::memoryBytes() const {
    if (stream_)
        return sizeof(Stream) + sizeof(uint32_t) * hdr_.cardCount +
               sizeof(uint64_t) * (stream_->glyphBlocks ? stream_->glyphBlocks : 1) +
               stream_->stringBytes;
    if (!cards_) return 0;
    return static_cast<size_t>(hdr_.cardCount) * kCardSize +
           sizeof(Glyph) * (hdr_.glyphCount ? hdr_.glyphCount : 1) + hdr_.stringsSize;
}

namespace {
// Reads and checks the header (64 bytes; v3: 80, docs/pack-format.md): magic,
// version, sizes, and that every table and the deck name lie inside the file.
// Shared by PackReader::open() and peekPackInfo().
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
    hd.wordsOff = 0;
    if (hd.version >= 3) {
        // The v3 header extension: words_off, then three reserved u32 (ignored;
        // later optional sections). v1 and v2 headers have no extension.
        if (hd.headerSize < kPackHeaderSizeV3) return PackError::BadHeader;
        uint8_t ext[4];
        if (!src.read(kPackHeaderSize, ext, sizeof ext)) return PackError::Io;
        hd.wordsOff = rd32(ext);
    }

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

PackError readPackCodepoints(PackSource& src, std::vector<uint32_t>* out) {
    out->clear();
    PackHeader hd;
    const PackError e = readHeader(src, &hd);
    if (e != PackError::Ok) return e;
    out->reserve(hd.cardCount);
    // The card table a chunk at a time: the code point is a card's first field.
    constexpr uint32_t kChunk = 32;
    uint8_t buf[kChunk * kCardSize];
    for (uint32_t i = 0; i < hd.cardCount; i += kChunk) {
        const uint32_t n = hd.cardCount - i < kChunk ? hd.cardCount - i : kChunk;
        if (!src.read(hd.cardsOff + i * kCardSize, buf, n * kCardSize)) {
            out->clear();
            return PackError::Io;
        }
        for (uint32_t k = 0; k < n; ++k) out->push_back(rd32(buf + k * kCardSize));
    }
    return PackError::Ok;
}

// One glyph entry: sorted after `prev`, a known encoding, its data inside the
// file.
PackError PackReader::checkGlyphEntry(const Glyph& g, const Glyph* prev) const {
    const uint32_t total = src_->size();
    if (g.set > kGlyphSetOption) return PackError::BadTable;
    if (prev && (g.set < prev->set || (g.set == prev->set && g.codepoint <= prev->codepoint)))
        return PackError::BadTable;
    if (g.encoding == kGlyphEncRaw) {
        if (!inBounds(g.dataOff, bitmapSize(g), total)) return PackError::BadTable;
    } else if (g.encoding == kGlyphEncRle && hdr_.version >= 2) {
        // The stream length is implied by its codes; it starts inside the glyph
        // data and is checked while decoding (readGlyphBitmap).
        if (bitmapSize(g) == 0 || g.dataOff < hdr_.glyphDataOff || g.dataOff >= total)
            return PackError::BadTable;
    } else {
        return PackError::BadTable;  // unknown encoding, or RLE in a v1 pack
    }
    return PackError::Ok;
}

// One card: its string offsets inside the pool, its distractor lists inside
// the file.
PackError PackReader::checkCardEntry(const uint8_t* c) const {
    const uint32_t total = src_->size();
    for (int f = 4; f <= 12; f += 4) {
        const uint32_t so = rd32(c + f);
        if (so < hdr_.stringsOff || so - hdr_.stringsOff >= hdr_.stringsSize)
            return PackError::BadTable;
    }
    const uint32_t d = rd32(c + 16);
    if (d < hdr_.distractOff || !inBounds(d, 2, total)) return PackError::BadTable;
    const uint32_t dr = rd32(c + 28);
    if (dr != 0 && (dr < hdr_.distractOff || !inBounds(dr, 2, total))) return PackError::BadTable;
    return PackError::Ok;
}

PackError PackReader::loadTables() {
    const PackHeader& hd = hdr_;
    PackSource& src = *src_;
    const uint64_t cardBytes = static_cast<uint64_t>(hd.cardCount) * kCardSize;
    cards_ = static_cast<uint8_t*>(alloc_.alloc(static_cast<size_t>(cardBytes)));
    glyphs_ = static_cast<Glyph*>(alloc_.alloc(sizeof(Glyph) * (hd.glyphCount ? hd.glyphCount : 1)));
    strings_ = static_cast<char*>(alloc_.alloc(hd.stringsSize));
    if (!cards_ || !glyphs_ || !strings_) {
        releaseTables();
        return PackError::NoMemory;
    }

    if (!src.read(hd.cardsOff, cards_, static_cast<size_t>(cardBytes))) return PackError::Io;
    if (!src.read(hd.stringsOff, strings_, hd.stringsSize)) return PackError::Io;
    // Guarantee termination so string access can never run off the pool.
    strings_[hd.stringsSize - 1] = 0;

    // Glyph table: stream in chunks to keep the stack small.
    uint8_t chunk[kGlyphEntrySize * 32];
    for (uint32_t i = 0; i < hd.glyphCount;) {
        uint32_t n = hd.glyphCount - i;
        if (n > 32) n = 32;
        if (!src.read(hd.glyphsOff + i * kGlyphEntrySize, chunk, n * kGlyphEntrySize)) return PackError::Io;
        for (uint32_t k = 0; k < n; ++k, ++i) {
            readGlyphEntry(chunk + k * kGlyphEntrySize, &glyphs_[i]);
            const PackError e = checkGlyphEntry(glyphs_[i], i > 0 ? &glyphs_[i - 1] : nullptr);
            if (e != PackError::Ok) return e;
        }
    }

    for (uint32_t i = 0; i < hd.cardCount; ++i) {
        const PackError e = checkCardEntry(cards_ + i * kCardSize);
        if (e != PackError::Ok) return e;
    }
    return PackError::Ok;
}

// The streamed state: the caches, every card's code point and the glyph
// index, after the same checks as loadTables() (both tables are read through
// once, a block at a time).
PackError PackReader::loadIndex() {
    const PackHeader& hd = hdr_;
    PackSource& src = *src_;
    void* mem = alloc_.alloc(sizeof(Stream));
    if (!mem) return PackError::NoMemory;
    stream_ = new (mem) Stream();
    Stream& st = *stream_;
    st.glyphBlocks = (hd.glyphCount + kGlyphsPerBlock - 1) / kGlyphsPerBlock;
    st.cps = static_cast<uint32_t*>(alloc_.alloc(sizeof(uint32_t) * hd.cardCount));
    st.blockKeys = static_cast<uint64_t*>(
        alloc_.alloc(sizeof(uint64_t) * (st.glyphBlocks ? st.glyphBlocks : 1)));
    if (!st.cps || !st.blockKeys) return PackError::NoMemory;

    // The caches' own buffers serve as scratch; they start empty (kNone).
    uint8_t chunk[kGlyphsPerBlock * kGlyphEntrySize];
    Glyph* g = st.glyphs[0].g;
    Glyph prev{};
    for (uint32_t b = 0; b < st.glyphBlocks; ++b) {
        const uint32_t first = b * kGlyphsPerBlock;
        uint32_t n = hd.glyphCount - first;
        if (n > kGlyphsPerBlock) n = kGlyphsPerBlock;
        if (!src.read(hd.glyphsOff + first * kGlyphEntrySize, chunk, n * kGlyphEntrySize))
            return PackError::Io;
        for (uint32_t k = 0; k < n; ++k) {
            readGlyphEntry(chunk + k * kGlyphEntrySize, &g[k]);
            const PackError e = checkGlyphEntry(g[k], first + k > 0 ? &prev : nullptr);
            if (e != PackError::Ok) return e;
            prev = g[k];
        }
        st.blockKeys[b] = glyphKey(g[0].set, g[0].codepoint);
    }

    uint8_t* cards = st.cards[0].bytes;
    for (uint32_t first = 0; first < hd.cardCount; first += kCardsPerBlock) {
        uint32_t n = hd.cardCount - first;
        if (n > kCardsPerBlock) n = kCardsPerBlock;
        if (!src.read(hd.cardsOff + first * kCardSize, cards, n * kCardSize)) return PackError::Io;
        for (uint32_t k = 0; k < n; ++k) {
            const uint8_t* c = cards + k * kCardSize;
            const PackError e = checkCardEntry(c);
            if (e != PackError::Ok) return e;
            st.cps[first + k] = rd32(c);
        }
    }
    return PackError::Ok;
}

PackError PackReader::open(PackSource& src, Allocator alloc, PackMemory mem) {
    close();
    alloc_ = alloc;
    PackHeader hd;
    const PackError he = readHeader(src, &hd);
    if (he != PackError::Ok) return he;
    src_ = &src;  // so close() below releases correctly
    hdr_ = hd;
    auto fail = [&](PackError e) {
        close();
        return e;
    };

    bool stream = mem == PackMemory::Streamed;
    if (!stream) {
        const PackError e = loadTables();
        if (e == PackError::NoMemory && mem == PackMemory::InMemoryOrStreamed)
            stream = true;  // loadTables() released what it had
        else if (e != PackError::Ok)
            return fail(e);
    }
    if (stream) {
        const PackError e = loadIndex();
        if (e != PackError::Ok) return fail(e);
    }
    PackError e = checkStrokes();
    if (e != PackError::Ok) return fail(e);
    e = checkWords();
    if (e != PackError::Ok) return fail(e);
    return PackError::Ok;
}

// Example-words section (v3, optional; words_off != 0). Validated whole at
// open(), like the other tables: the header, the card index (0 first, never
// decreasing, word_count last) and every entry (strings inside the pool, a
// known kind). Read in chunks of 256 bytes; nothing is kept but two numbers.
PackError PackReader::checkWords() {
    wordsTable_ = 0;
    wordEntrySize_ = 0;
    const PackHeader& hd = hdr_;
    if (hd.wordsOff == 0) return PackError::Ok;
    const uint32_t total = src_->size();
    const uint64_t indexBytes = (static_cast<uint64_t>(hd.cardCount) + 1) * 4;
    if ((hd.wordsOff & 3u) || !inBounds(hd.wordsOff, 8 + indexBytes, total))
        return PackError::BadTable;
    uint8_t sh[8];
    if (!src_->read(hd.wordsOff, sh, sizeof sh)) return PackError::Io;
    const uint32_t entrySize = rd16(sh);  // then u16 reserved
    const uint32_t count = rd32(sh + 4);
    if (entrySize < kWordEntrySize) return PackError::BadTable;
    const uint64_t table = hd.wordsOff + 8 + indexBytes;
    if (!inBounds(table, static_cast<uint64_t>(entrySize) * count, total)) return PackError::BadTable;

    uint8_t buf[256];
    uint32_t prev = 0;
    const uint32_t n = hd.cardCount + 1;
    for (uint32_t i = 0; i < n;) {
        uint32_t m = n - i;
        if (m > sizeof buf / 4) m = sizeof buf / 4;
        if (!src_->read(hd.wordsOff + 8 + i * 4u, buf, m * 4u)) return PackError::Io;
        for (uint32_t q = 0; q < m; ++q, ++i) {
            const uint32_t first = rd32(buf + q * 4);
            if ((i == 0 && first != 0) || first < prev) return PackError::BadTable;
            prev = first;
        }
    }
    if (prev != count) return PackError::BadTable;

    // Entries: several per read when they are small (always, in v3).
    const uint32_t per = entrySize <= sizeof buf ? static_cast<uint32_t>(sizeof buf) / entrySize : 1;
    for (uint32_t k = 0; k < count;) {
        uint32_t m = count - k;
        if (m > per) m = per;
        const uint32_t bytes = per > 1 ? m * entrySize : kWordEntrySize;
        if (!src_->read(static_cast<uint32_t>(table + static_cast<uint64_t>(k) * entrySize), buf, bytes))
            return PackError::Io;
        for (uint32_t q = 0; q < m; ++q, ++k) {
            const uint8_t* e = buf + q * entrySize;
            for (int f = 0; f <= 8; f += 4) {
                const uint32_t so = rd32(e + f);
                if (so < hd.stringsOff || so - hd.stringsOff >= hd.stringsSize) return PackError::BadTable;
            }
            if (e[12] > kWordKindKun) return PackError::BadTable;
        }
    }
    wordsTable_ = static_cast<uint32_t>(table);
    wordEntrySize_ = entrySize;
    return PackError::Ok;
}

// Stroke section (optional; stroke_off != 0). Validate the whole index, every
// block and every mask so a corrupt pack is rejected at open(), exactly like
// the other tables, instead of failing at draw time.
PackError PackReader::checkStrokes() const {
    const PackHeader& hd = hdr_;
    if (hd.strokeOff == 0) return PackError::Ok;
    const uint32_t total = src_->size();
    if ((hd.strokeOff & 3u) || !inBounds(hd.strokeOff, static_cast<uint64_t>(hd.cardCount) * 4, total))
        return PackError::BadTable;
    uint8_t idx[256];
    for (uint32_t i = 0; i < hd.cardCount;) {
        uint32_t m = hd.cardCount - i;
        if (m > sizeof(idx) / 4) m = sizeof(idx) / 4;
        if (!src_->read(hd.strokeOff + i * 4u, idx, m * 4u)) return PackError::Io;
        for (uint32_t q = 0; q < m; ++q, ++i) {
            const uint32_t blk = rd32(idx + q * 4);
            if (blk == 0) continue;  // no stroke data for this card
            const PackError e = checkStrokeBlock(blk, total);
            if (e != PackError::Ok) return e;
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
    if (off < hdr_.stringsOff || off - hdr_.stringsOff >= hdr_.stringsSize) return "";
    if (strings_) return strings_ + (off - hdr_.stringsOff);
    return stream_ ? streamStr(off) : "";
}

// The string at `off` (inside the pool) from its slot: a hit, or the least
// recently used slot refilled. It ends at its NUL or, as in memory, at the
// pool's last byte, which counts as a NUL whatever it holds.
const char* PackReader::streamStr(uint32_t off) const {
    Stream& st = *stream_;
    Stream::StringSlot* slot = &st.strings[0];
    for (Stream::StringSlot& s : st.strings) {
        if (s.off == off) {
            s.used = ++st.tick;
            return s.buf;
        }
        if (s.used < slot->used) slot = &s;
    }
    slot->off = kNone;
    const uint32_t end = hdr_.stringsOff + hdr_.stringsSize - 1;  // the forced NUL
    uint32_t len = 0;
    for (;;) {
        const uint32_t left = end - (off + len);
        const uint32_t n = left < kStrChunk ? left : kStrChunk;
        if (slot->cap < len + n + 1) {
            const uint32_t cap = (len + n + 1 + 31) & ~31u;
            char* buf = static_cast<char*>(alloc_.alloc(cap));
            if (!buf) return "";
            if (slot->buf) {
                std::memcpy(buf, slot->buf, len);
                alloc_.release(slot->buf);
            }
            st.stringBytes += cap - slot->cap;
            slot->buf = buf;
            slot->cap = cap;
        }
        if (n == 0) break;
        if (!src_->read(off + len, slot->buf + len, n)) return "";
        const void* z = std::memchr(slot->buf + len, 0, n);
        if (z) {
            len = static_cast<uint32_t>(static_cast<const char*>(z) - slot->buf);
            break;
        }
        len += n;
    }
    slot->buf[len] = 0;
    slot->off = off;
    slot->used = ++st.tick;
    return slot->buf;
}

// Card `idx`'s 32 bytes from its cached block (read on a miss); null on a read
// error.
const uint8_t* PackReader::streamCard(uint32_t idx) const {
    Stream& st = *stream_;
    const uint32_t block = idx / kCardsPerBlock;
    Stream::CardBlock* slot = &st.cards[0];
    for (Stream::CardBlock& c : st.cards) {
        if (c.block == block) {
            c.used = ++st.tick;
            return c.bytes + (idx % kCardsPerBlock) * kCardSize;
        }
        if (c.used < slot->used) slot = &c;
    }
    const uint32_t first = block * kCardsPerBlock;
    uint32_t n = hdr_.cardCount - first;
    if (n > kCardsPerBlock) n = kCardsPerBlock;
    slot->block = kNone;
    if (!src_->read(hdr_.cardsOff + first * kCardSize, slot->bytes, n * kCardSize)) return nullptr;
    slot->block = block;
    slot->used = ++st.tick;
    return slot->bytes + (idx % kCardsPerBlock) * kCardSize;
}

uint32_t PackReader::codepointAt(uint32_t idx) const {
    if (idx >= hdr_.cardCount) return 0;
    if (cards_) return rd32(cards_ + idx * kCardSize);
    return stream_ ? stream_->cps[idx] : 0;
}

bool PackReader::card(uint32_t idx, Card* out, bool text) const {
    if (idx >= hdr_.cardCount) return false;
    const uint8_t* c = cards_ ? cards_ + idx * kCardSize : (stream_ ? streamCard(idx) : nullptr);
    if (!c) return false;
    // Streamed, `c` points into a card block; the strings have caches of their
    // own, so reading them leaves it in place.
    out->codepoint = rd32(c);
    out->meanings = text ? str(rd32(c + 4)) : "";
    out->onyomi = text ? str(rd32(c + 8)) : "";
    out->kunyomi = text ? str(rd32(c + 12)) : "";
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

// Glyph block `block` (kGlyphsPerBlock entries, fewer in the last one) from
// the cache, read on a miss; null on a read error.
const Glyph* PackReader::streamGlyphBlock(uint32_t block, uint32_t* count) const {
    Stream& st = *stream_;
    Stream::GlyphBlock* slot = &st.glyphs[0];
    for (Stream::GlyphBlock& g : st.glyphs) {
        if (g.block == block) {
            g.used = ++st.tick;
            *count = g.count;
            return g.g;
        }
        if (g.used < slot->used) slot = &g;
    }
    const uint32_t first = block * kGlyphsPerBlock;
    uint32_t n = hdr_.glyphCount - first;
    if (n > kGlyphsPerBlock) n = kGlyphsPerBlock;
    uint8_t chunk[kGlyphsPerBlock * kGlyphEntrySize];
    slot->block = kNone;
    if (!src_->read(hdr_.glyphsOff + first * kGlyphEntrySize, chunk, n * kGlyphEntrySize))
        return nullptr;
    for (uint32_t k = 0; k < n; ++k) readGlyphEntry(chunk + k * kGlyphEntrySize, &slot->g[k]);
    slot->block = block;
    slot->count = n;
    slot->used = ++st.tick;
    *count = n;
    return slot->g;
}

const Glyph* PackReader::findGlyph(uint8_t set, uint32_t codepoint) const {
    if (glyphs_) return searchGlyphs(glyphs_, hdr_.glyphCount, set, codepoint);
    if (!stream_ || stream_->glyphBlocks == 0) return nullptr;
    // The last block whose first entry is not after the key.
    const uint64_t key = glyphKey(set, codepoint);
    const uint64_t* keys = stream_->blockKeys;
    uint32_t lo = 0, hi = stream_->glyphBlocks;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid] <= key) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;  // before the first glyph
    uint32_t n = 0;
    const Glyph* g = streamGlyphBlock(lo - 1, &n);
    return g ? searchGlyphs(g, n, set, codepoint) : nullptr;
}

const Glyph* PackReader::glyphAt(uint32_t idx) const {
    if (idx >= hdr_.glyphCount) return nullptr;
    if (glyphs_) return &glyphs_[idx];
    if (!stream_) return nullptr;
    uint32_t n = 0;
    const Glyph* g = streamGlyphBlock(idx / kGlyphsPerBlock, &n);
    return g && idx % kGlyphsPerBlock < n ? &g[idx % kGlyphsPerBlock] : nullptr;
}

uint32_t PackReader::glyphIndex(const Glyph* g) const {
    if (glyphs_) {
        if (g >= glyphs_ && g < glyphs_ + hdr_.glyphCount) return static_cast<uint32_t>(g - glyphs_);
        return hdr_.glyphCount;
    }
    if (stream_)
        for (const Stream::GlyphBlock& b : stream_->glyphs)
            if (b.block != kNone && g >= b.g && g < b.g + b.count)
                return b.block * kGlyphsPerBlock + static_cast<uint32_t>(g - b.g);
    return hdr_.glyphCount;
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

bool PackReader::wordRange(uint32_t cardIdx, uint32_t* first, uint32_t* end) const {
    if (!src_ || wordEntrySize_ == 0 || cardIdx >= hdr_.cardCount) return false;
    uint8_t b[8];
    if (!src_->read(hdr_.wordsOff + 8 + cardIdx * 4u, b, sizeof b)) return false;
    *first = rd32(b);
    *end = rd32(b + 4);
    return *end >= *first;  // checked at open()
}

uint32_t PackReader::exampleWordCount(uint32_t cardIdx) const {
    uint32_t first = 0, end = 0;
    return wordRange(cardIdx, &first, &end) ? end - first : 0;
}

uint32_t PackReader::exampleWords(uint32_t cardIdx, ExampleWord* out, uint32_t cap) const {
    uint32_t first = 0, end = 0;
    if (!out || !wordRange(cardIdx, &first, &end)) return 0;
    uint32_t n = 0;
    for (uint32_t k = first; k < end && n < cap; ++k) {
        uint8_t e[kWordEntrySize];
        if (!src_->read(wordsTable_ + k * wordEntrySize_, e, sizeof e)) return n;
        ExampleWord& w = out[n];
        w.word = str(rd32(e));
        w.reading = str(rd32(e + 4));
        w.gloss = str(rd32(e + 8));
        w.kind = e[12];
        w.readingStart = e[14];
        w.readingLen = e[15];
        // A span that does not fit the reading, or cuts a character, is unknown.
        const size_t len = std::strlen(w.reading);
        const auto cont = [&](size_t i) {
            return i < len && (static_cast<uint8_t>(w.reading[i]) & 0xC0) == 0x80;
        };
        if (w.readingLen == 0 || w.readingStart + w.readingLen > len || cont(w.readingStart) ||
            cont(w.readingStart + w.readingLen)) {
            w.readingStart = 0;
            w.readingLen = 0;
        }
        ++n;
    }
    return n;
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
