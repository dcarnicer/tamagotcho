#pragma once
// Reader for Tangocho packs (*.tgc), format v1 and v2. See docs/pack-format.md.
// Pure C++, no Arduino. Caches header, card table, glyph table and the string
// pool in memory (obtained through a pluggable allocator, PSRAM on device);
// bitmaps and distractor lists are read on demand through a PackSource, and
// v2's compressed bitmaps are decoded on the way (pack/Rle.h).
#include <cstddef>
#include <cstdint>

#include "pack/PackSource.h"
#include "pack/Rle.h"  // glyph and mask encoding constants, decoders

namespace tgc {

enum class PackError : uint8_t {
    Ok = 0,
    Io,           // source read failed
    BadMagic,
    BadVersion,
    BadHeader,    // header_size, counts or offsets inconsistent / out of bounds
    BadTable,     // card/glyph table content invalid
    NoMemory,
};

const char* packErrorString(PackError e);

struct Allocator {
    void* (*alloc)(size_t);
    void (*release)(void*);
};
// malloc/free.
Allocator defaultAllocator();

constexpr uint16_t kPackVersionMin = 1;  // oldest readable pack version
constexpr uint16_t kPackVersionMax = 2;  // newest readable pack version
constexpr uint32_t kPackHeaderSize = 64;
constexpr uint32_t kCardSize = 32;
constexpr uint32_t kGlyphEntrySize = 16;
constexpr uint8_t kGlyphSetLarge = 0;
constexpr uint8_t kGlyphSetSmall = 1;
constexpr uint8_t kGlyphSetText = 2;
constexpr uint8_t kGlyphSetOption = 3;  // large text (reading answers)
constexpr uint8_t kCardFlagMeaningFallback = 0x01;

struct PackHeader {
    uint16_t version;
    uint16_t headerSize;
    uint32_t cardCount;
    uint32_t glyphCount;
    uint32_t cardsOff, glyphsOff, distractOff, stringsOff, stringsSize, glyphDataOff;
    uint32_t deckNameOff;
    char lang[5];  // NUL terminated
    uint8_t largePx, smallPx, textPx, textLineHeight, textAscent;
    uint8_t optionPx, optionLineHeight, optionAscent;  // set 3; 0 = absent
    uint32_t buildTime;
    uint32_t strokeOff;  // absolute offset of the stroke section; 0 = absent
};

struct Card {
    uint32_t codepoint;
    const char* meanings;  // "; " separated; never null
    const char* onyomi;    // "、" separated katakana; may be ""
    const char* kunyomi;   // "、" separated hiragana, '.' okurigana marker; may be ""
    uint32_t distractorOff;
    uint32_t distractorReadOff;  // 0 = use distractorOff
    uint8_t jlpt, grade, strokes, flags;
    uint16_t freq, level;
};

struct Glyph {
    uint32_t codepoint;
    uint8_t set, encoding, width, height;
    int8_t xOff, yOff;
    uint8_t advance;
    uint32_t dataOff;
};

// One stroke's mask: position/size inside the em square plus where its data
// lives in the file and how it is stored (see docs/pack-format.md).
struct Stroke {
    int8_t xOff, yOff;
    uint8_t w, h;
    uint8_t encoding;  // kMaskEnc*; always raw in a v1 pack
    uint16_t size;     // stored bytes at dataOff
    uint32_t dataOff;
};

class PackReader {
public:
    PackReader() = default;
    ~PackReader() { close(); }
    PackReader(const PackReader&) = delete;
    PackReader& operator=(const PackReader&) = delete;

    // `src` must outlive the reader. On failure the reader stays closed.
    PackError open(PackSource& src, Allocator alloc = defaultAllocator());
    void close();
    bool isOpen() const { return src_ != nullptr; }

    const PackHeader& header() const { return hdr_; }
    uint32_t cardCount() const { return hdr_.cardCount; }
    uint32_t glyphCount() const { return hdr_.glyphCount; }
    const char* deckName() const { return str(hdr_.deckNameOff); }
    const Allocator& allocator() const { return alloc_; }

    // False if idx is out of range.
    bool card(uint32_t idx, Card* out) const;

    // String pool access by absolute file offset; "" if out of range.
    const char* str(uint32_t off) const;

    // Copies up to `cap` distractor card indices, returns the number copied
    // (0 on error / empty). `reading` selects the reading list (falls back to
    // the meaning list when the pack has none). `total` (optional) receives the
    // list's full count.
    uint16_t distractors(const Card& c, uint16_t* out, uint16_t cap, uint16_t* total = nullptr,
                         bool reading = false) const;

    // Binary search by (set, codepoint). Null if absent.
    const Glyph* findGlyph(uint8_t set, uint32_t codepoint) const;
    const Glyph* glyphAt(uint32_t idx) const { return idx < hdr_.glyphCount ? &glyphs_[idx] : nullptr; }
    // Index of a Glyph returned by findGlyph/glyphAt.
    uint32_t glyphIndex(const Glyph* g) const { return static_cast<uint32_t>(g - glyphs_); }

    // Bytes of the decoded (raw 4bpp) bitmap of this glyph: ceil(w/2)*h.
    static size_t bitmapSize(const Glyph& g) { return static_cast<size_t>((g.width + 1) / 2) * g.height; }
    // Reads the bitmap into buf (cap >= bitmapSize) as raw 4bpp, decoding RLE
    // glyphs (encoding 1) on the way. False on a read error or a bad stream.
    bool readGlyphBitmap(const Glyph& g, uint8_t* buf, size_t cap) const;

    // --- stroke order (optional section; all false/0 when absent) ------------
    // True when the pack carries a stroke section (stroke_off != 0).
    bool hasStrokeOrder() const { return hdr_.strokeOff != 0; }
    // Number of strokes of a card (0 when it has none / on any error).
    uint16_t strokeCount(uint32_t cardIdx) const;
    // Fills count/px (px = the em square the masks live in). False when absent.
    bool strokeInfo(uint32_t cardIdx, uint16_t* count, uint16_t* px) const;
    // The i-th stroke of a card. False when out of range / absent.
    bool strokeAt(uint32_t cardIdx, uint16_t i, Stroke* out) const;
    // Bytes of the decoded (raw 1bpp) mask of a stroke: ceil(w/8)*h.
    static size_t strokeBitmapSize(const Stroke& s) { return static_cast<size_t>((s.w + 7) / 8) * s.h; }
    // Reads the mask into buf (cap >= strokeBitmapSize) as raw 1bpp, decoding
    // encodings 1 and 2 on the way, like readGlyphBitmap.
    bool readStrokeBitmap(const Stroke& s, uint8_t* buf, size_t cap) const;

private:
    // Card's stroke block: offset of its first entry, count, px and (v2) the
    // offset of its first mask. False when the card has none.
    bool strokeBlock(uint32_t cardIdx, uint32_t* entriesOff, uint16_t* count, uint16_t* px,
                     uint32_t* firstMask = nullptr) const;
    // Validates one card's stroke block at open() time.
    PackError checkStrokeBlock(uint32_t blk, uint32_t total) const;

    PackSource* src_ = nullptr;
    Allocator alloc_{nullptr, nullptr};
    PackHeader hdr_{};
    uint8_t* cards_ = nullptr;  // raw card table
    Glyph* glyphs_ = nullptr;
    char* strings_ = nullptr;
};

}  // namespace tgc
