#include <unity.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pack/MemorySource.h"
#include "pack/PackReader.h"
#include "pack/StdioSource.h"
#include "pack/Utf8.h"
#include "quiz/Quiz.h"
#include "render/Blend565.h"
#include "power/BatteryGuard.h"
#include "render/GlcdCp437.h"
#include "ui/TextLayout.h"
#include "ui/UiLang.h"

using namespace tgc;

// How the pack tests open their packs: main() runs them once with the tables
// in memory and once streamed (PackMemory::Streamed), which must not change a
// single answer.
PackMemory gPackMem = PackMemory::InMemory;

// ---------------------------------------------------------------- generator
namespace {

struct B {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) { u8(x & 255); u8(x >> 8); }
    void u32(uint32_t x) { u16(x & 0xFFFF); u16(x >> 16); }
    void pad4() { while (v.size() % 4) u8(0); }
};

struct CardSpec {
    uint32_t cp;
    const char *mean, *on, *kun;
    std::vector<uint16_t> dis;      // meaning distractors
    std::vector<uint16_t> disRead;  // reading distractors (empty -> offset 0)
};
struct GlyphSpec {
    uint8_t set;
    uint32_t cp;
    uint8_t w, h;
    uint8_t enc = 0;              // glyph table encoding
    std::vector<uint8_t> data{};  // stored bytes; empty -> raw test pattern
};
struct StrokeSpec {  // one mask: position in the em square + size
    uint8_t x, y, w, h;
    uint8_t enc = 0;              // v2 mask encoding
    std::vector<uint8_t> data{};  // stored bytes; empty -> raw test pattern
};
struct WordSpec {  // one v3 example word
    const char *word, *reading, *gloss;
    uint8_t kind, start, len;
};
using CardWords = std::vector<std::vector<WordSpec>>;  // per card

// Raw test pattern of a glyph: byte i = (cp + i) & 0xFF; of a mask: 0xA0 + i.
std::vector<uint8_t> glyphBytes(const GlyphSpec& g) {
    if (!g.data.empty()) return g.data;
    std::vector<uint8_t> v(((g.w + 1) / 2) * g.h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (g.cp + i) & 0xFF;
    return v;
}
std::vector<uint8_t> maskBytes(const StrokeSpec& s) {
    if (!s.data.empty()) return s.data;
    std::vector<uint8_t> v(((s.w + 7) / 8) * s.h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (0xA0 + i) & 0xFF;
    return v;
}

// Builds a valid pack in memory, format `version` 1 or 2 (docs/pack-format.md).
// Glyph and mask data are the specs' stored bytes, or the raw test patterns
// above. `strokes` (optional, aligned with `cards`) appends a stroke section:
// v1 layout (one absolute offset per 4-byte aligned raw mask) or v2 (encoding
// and size per entry, masks back to back from the block's data_off). `words`
// (aligned with `cards`) makes it a v3 pack: the 80-byte header and an
// example-words section at the end, the words' strings in the pool.
std::vector<uint8_t> makePack(const std::vector<CardSpec>& cards, std::vector<GlyphSpec> glyphs,
                              const std::vector<std::vector<StrokeSpec>>& strokes = {},
                              uint16_t version = 1, const CardWords* words = nullptr) {
    if (words) version = 3;
    const uint32_t headerSize = words ? 80 : 64;
    std::sort(glyphs.begin(), glyphs.end(), [](const GlyphSpec& a, const GlyphSpec& b) {
        return a.set != b.set ? a.set < b.set : a.cp < b.cp;
    });
    std::string pool;
    auto addStr = [&](const char* s) {
        uint32_t off = static_cast<uint32_t>(pool.size());
        pool += s;
        pool.push_back('\0');
        return off;
    };
    uint32_t deckRel = addStr("Test Deck");
    struct CS { uint32_t m, o, k; };
    std::vector<CS> cs;  // relative to the pool; made absolute below
    for (auto& c : cards) cs.push_back({addStr(c.mean), addStr(c.on), addStr(c.kun)});
    std::vector<std::vector<CS>> ws;  // the words' (word, reading, gloss), pool-relative
    if (words)
        for (auto& cw : *words) {
            ws.emplace_back();
            for (auto& w : cw) ws.back().push_back({addStr(w.word), addStr(w.reading), addStr(w.gloss)});
        }
    while (pool.size() % 4) pool.push_back('\0');

    const uint32_t cardsOff = headerSize;
    const uint32_t glyphsOff = cardsOff + cards.size() * 32;
    const uint32_t distOff = glyphsOff + glyphs.size() * 16;
    std::vector<uint32_t> dOffs, drOffs;
    uint32_t dsize = 0;
    for (auto& c : cards) {
        dOffs.push_back(distOff + dsize);
        dsize += 2 + 2 * c.dis.size();
    }
    const uint32_t drBase = distOff + dsize;
    uint32_t drsize = 0;
    for (auto& c : cards) {
        if (c.disRead.empty()) {
            drOffs.push_back(0);
            continue;
        }
        drOffs.push_back(drBase + drsize);
        drsize += 2 + 2 * c.disRead.size();
    }
    const uint32_t strOff = (drBase + drsize + 3) & ~3u;
    const uint32_t gdOff = strOff + pool.size();
    const uint32_t deckOff = strOff + deckRel;  // string offsets are absolute
    for (auto& x : cs) { x.m += strOff; x.o += strOff; x.k += strOff; }
    for (auto& cw : ws)
        for (auto& x : cw) { x.m += strOff; x.o += strOff; x.k += strOff; }

    B b;
    b.u8('T'); b.u8('N'); b.u8('G'); b.u8('C');
    b.u16(version); b.u16(headerSize);
    b.u32(cards.size()); b.u32(glyphs.size());
    b.u32(cardsOff); b.u32(glyphsOff); b.u32(distOff); b.u32(strOff);
    b.u32(pool.size()); b.u32(gdOff); b.u32(deckOff);
    b.u8('e'); b.u8('n'); b.u8(0); b.u8(0);
    b.u8(32); b.u8(16); b.u8(12); b.u8(18); b.u8(14);
    b.u8(0); b.u8(0); b.u8(0);
    b.u32(0); b.u32(0);
    if (words) { b.u32(0); b.u32(0); b.u32(0); b.u32(0); }  // words_off (set below), reserved
    for (size_t i = 0; i < cards.size(); ++i) {
        b.u32(cards[i].cp); b.u32(cs[i].m); b.u32(cs[i].o); b.u32(cs[i].k); b.u32(dOffs[i]);
        b.u8(5); b.u8(1); b.u8(4); b.u8(0);
        b.u16(10 + i); b.u16(i); b.u32(drOffs[i]);
    }
    uint32_t dataOff = gdOff;
    for (auto& g : glyphs) {
        b.u32(g.cp); b.u8(g.set); b.u8(g.enc); b.u8(g.w); b.u8(g.h);
        b.u8(static_cast<uint8_t>(-3)); b.u8(static_cast<uint8_t>(-g.h)); b.u8(g.w + 1); b.u8(0);
        b.u32(dataOff);
        dataOff += glyphBytes(g).size();
    }
    for (auto& c : cards) {
        b.u16(c.dis.size());
        for (auto d : c.dis) b.u16(d);
    }
    for (auto& c : cards) {
        if (c.disRead.empty()) continue;
        b.u16(c.disRead.size());
        for (auto d : c.disRead) b.u16(d);
    }
    b.pad4();
    for (char ch : pool) b.u8(ch);
    for (auto& g : glyphs)
        for (uint8_t x : glyphBytes(g)) b.u8(x);

    if (!strokes.empty()) {
        b.pad4();
        const uint32_t strokeOff = static_cast<uint32_t>(b.v.size());
        const uint32_t n = static_cast<uint32_t>(cards.size());
        std::vector<uint32_t> blockOff(n, 0);
        uint32_t cursor = (strokeOff + 4 * n + 3) & ~3u;
        for (uint32_t i = 0; i < n; ++i) {
            if (strokes[i].empty()) continue;
            blockOff[i] = cursor;
            cursor += 8 + 8 * static_cast<uint32_t>(strokes[i].size());
        }
        std::vector<std::vector<uint32_t>> maskOff(n);
        for (uint32_t i = 0; i < n; ++i)
            for (auto& s : strokes[i]) {
                if (version < 2) cursor = (cursor + 3) & ~3u;  // v1 aligned each mask
                maskOff[i].push_back(cursor);
                cursor += maskBytes(s).size();
            }
        for (uint32_t i = 0; i < n; ++i) b.u32(blockOff[i]);
        while (b.v.size() < ((strokeOff + 4 * n + 3) & ~3u)) b.u8(0);
        for (uint32_t i = 0; i < n; ++i) {
            if (strokes[i].empty()) continue;
            b.u16(static_cast<uint16_t>(strokes[i].size()));
            b.u16(32);  // px == header large_px
            b.u32(version < 2 ? 0 : maskOff[i][0]);
            for (size_t k = 0; k < strokes[i].size(); ++k) {
                const StrokeSpec& s = strokes[i][k];
                b.u8(s.x); b.u8(s.y); b.u8(s.w); b.u8(s.h);
                if (version < 2) {
                    b.u32(maskOff[i][k]);
                } else {
                    b.u8(s.enc); b.u8(0); b.u16(static_cast<uint16_t>(maskBytes(s).size()));
                }
            }
        }
        for (uint32_t i = 0; i < n; ++i)
            for (size_t k = 0; k < strokes[i].size(); ++k) {
                while (b.v.size() < maskOff[i][k]) b.u8(0);
                for (uint8_t x : maskBytes(strokes[i][k])) b.u8(x);
            }
        b.v[60] = strokeOff & 0xFF;
        b.v[61] = (strokeOff >> 8) & 0xFF;
        b.v[62] = (strokeOff >> 16) & 0xFF;
        b.v[63] = (strokeOff >> 24) & 0xFF;
    }

    if (words) {
        b.pad4();
        const uint32_t wordsOff = static_cast<uint32_t>(b.v.size());
        uint32_t count = 0;
        for (auto& cw : *words) count += static_cast<uint32_t>(cw.size());
        b.u16(16); b.u16(0); b.u32(count);
        uint32_t first = 0;
        for (auto& cw : *words) {
            b.u32(first);
            first += static_cast<uint32_t>(cw.size());
        }
        b.u32(first);
        for (size_t i = 0; i < words->size(); ++i)
            for (size_t k = 0; k < (*words)[i].size(); ++k) {
                const WordSpec& w = (*words)[i][k];
                b.u32(ws[i][k].m); b.u32(ws[i][k].o); b.u32(ws[i][k].k);
                b.u8(w.kind); b.u8(0); b.u8(w.start); b.u8(w.len);
            }
        for (int k = 0; k < 4; ++k) b.v[64 + k] = (wordsOff >> (8 * k)) & 0xFF;
    }
    return b.v;
}

std::vector<std::vector<StrokeSpec>> someStrokes();

uint32_t rd32le(const std::vector<uint8_t>& v, size_t off) {
    return static_cast<uint32_t>(v[off]) | (static_cast<uint32_t>(v[off + 1]) << 8) |
           (static_cast<uint32_t>(v[off + 2]) << 16) | (static_cast<uint32_t>(v[off + 3]) << 24);
}

std::vector<CardSpec> sixCards() {
    return {
        {0x65E5, "day; sun; Japan", "ニチ、ジツ", "ひ、び", {1, 2}},
        {0x6708, "month; moon", "ゲツ、ガツ", "つき", {0, 2, 99}},
        {0x706B, "fire", "カ", "ひ、び、ほ", {}},
        {0x6C34, "water", "スイ", "みず", {0}},
        {0x6728, "tree; wood", "ボク、モク", "き", {0, 1}},
        {0x4E2D, "middle", "", "なか、うち", {}},  // kun only
    };
}
std::vector<GlyphSpec> someGlyphs() {
    std::vector<GlyphSpec> g;
    for (auto& c : sixCards()) {
        g.push_back({0, c.cp, 19, 26});
        g.push_back({1, c.cp, 11, 13});
    }
    g.push_back({2, 'A', 9, 9});
    g.push_back({2, ' ', 0, 0});
    g.push_back({2, 0x30CB, 9, 6});
    g.push_back({2, 'a', 7, 9});
    return g;
}

std::vector<std::vector<StrokeSpec>> someStrokes() {
    std::vector<std::vector<StrokeSpec>> s(sixCards().size());
    s[0] = {{5, 4, 8, 3}, {6, 8, 3, 9}};
    s[2] = {{1, 1, 4, 4}};
    return s;
}

// True if the UTF-8 string contains a code point in [lo, hi].
bool hasInRange(const std::string& s, uint32_t lo, uint32_t hi) {
    const char* p = s.c_str();
    while (uint32_t cp = utf8Next(&p))
        if (cp >= lo && cp <= hi) return true;
    return false;
}

// True if `s` is one of the "; "-separated meanings in `list`.
bool isMeaningOf(const std::string& s, const char* list) {
    int n = meaningCount(list);
    for (int i = 0; i < n; ++i)
        if (meaningAt(list, i) == s) return true;
    return false;
}

// True if `s` is one of the "、"-separated readings in `list` (dots removed).
bool isReadingOf(const std::string& s, const char* list) {
    std::string item;
    for (const char* p = list;; ++p) {
        if (*p == 0 || std::strncmp(p, "、", 3) == 0) {
            if (item == s) return true;
            if (*p == 0) return false;
            item.clear();
            p += 2;
        } else if (*p != '.') {
            item += *p;
        }
    }
}

// A deck as `--common-readings` builds it: a kind none of whose readings is
// worth asking is dropped, so many cards have no kun'yomi and a few no
// on'yomi; 々 has neither. The reading lists mix kinds, as the builder's do (a
// candidate is listed when either kind is confusable), so they name cards
// without the asked kind too.
std::vector<CardSpec> sparseKunCards() {
    return {
        {0x6C17, "spirit", "キ、ケ", "", {1, 2}, {12, 13, 4}},  // 0  no kun
        {0x767E, "hundred", "ヒャク", "", {0}, {13, 0}},        // 1  no kun
        {0x8089, "meat", "ニク", "", {}, {}},                   // 2  no kun
        {0x90E8, "section", "ブ", "", {}, {}},                  // 3  no kun
        {0x5186, "circle", "エン", "まる.い", {}, {9, 3, 13, 8}},  // 4
        {0x5C71, "mountain", "サン", "やま", {}, {0, 1, 2, 6, 7}},  // 5
        {0x5DDD, "river", "セン", "かわ", {}, {5, 10, 11}},       // 6
        {0x624B, "hand", "シュ", "て", {}, {}},                  // 7
        {0x76EE, "eye", "モク", "め", {}, {}},                   // 8
        {0x6BCE, "every", "マイ", "", {}, {}},                   // 9  no kun
        {0x8336, "tea", "チャ、サ", "", {}, {}},                 // 10 no kun
        {0x7551, "field", "", "はたけ、はた", {}, {0, 1, 5}},    // 11 no on
        {0x8FBC, "crowded", "", "こ.む、こ.める", {}, {}},       // 12 no on
        {0x3005, "repeat mark", "", "", {}, {}},                 // 13 neither
        {0x672C, "book", "ホン", "もと", {}, {}},                // 14
        {0x5B66, "study", "ガク", "まな.ぶ", {}, {}},            // 15
    };
}

// Glyph sets 0 and 1 for every card (all a quiz test needs).
std::vector<GlyphSpec> cardGlyphs(const std::vector<CardSpec>& cs) {
    std::vector<GlyphSpec> gl;
    for (auto& c : cs) {
        gl.push_back({0, c.cp, 4, 4});
        gl.push_back({1, c.cp, 4, 4});
    }
    return gl;
}

}  // namespace

// ------------------------------------------------------------------- tests
void test_open_and_header() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    TEST_ASSERT_EQUAL_UINT32(6, r.cardCount());
    TEST_ASSERT_EQUAL_UINT32(16, r.glyphCount());
    TEST_ASSERT_EQUAL_STRING("Test Deck", r.deckName());
    TEST_ASSERT_EQUAL_STRING("en", r.header().lang);
    TEST_ASSERT_EQUAL_UINT8(32, r.header().largePx);
    TEST_ASSERT_EQUAL_UINT8(14, r.header().textAscent);
    TEST_ASSERT_EQUAL_UINT8(0, r.header().optionPx);  // makePack ships no set 3
    TEST_ASSERT_FALSE(r.hasStrokeOrder());
}

void test_cards() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    Card c;
    TEST_ASSERT_TRUE(r.card(1, &c));
    TEST_ASSERT_EQUAL_UINT32(0x6708, c.codepoint);
    TEST_ASSERT_EQUAL_STRING("month; moon", c.meanings);
    TEST_ASSERT_EQUAL_STRING("ゲツ、ガツ", c.onyomi);
    TEST_ASSERT_EQUAL_STRING("つき", c.kunyomi);
    TEST_ASSERT_EQUAL_UINT8(5, c.jlpt);
    TEST_ASSERT_EQUAL_UINT8(1, c.grade);
    TEST_ASSERT_EQUAL_UINT8(4, c.strokes);
    TEST_ASSERT_EQUAL_UINT16(11, c.freq);
    TEST_ASSERT_EQUAL_UINT16(1, c.level);
    TEST_ASSERT_FALSE(r.card(6, &c));
    uint16_t d[8];
    uint16_t total = 0;
    TEST_ASSERT_TRUE(r.card(1, &c));
    TEST_ASSERT_EQUAL_UINT16(3, r.distractors(c, d, 8, &total));
    TEST_ASSERT_EQUAL_UINT16(3, total);
    TEST_ASSERT_EQUAL_UINT16(0, d[0]);
    TEST_ASSERT_EQUAL_UINT16(99, d[2]);
    TEST_ASSERT_EQUAL_UINT16(2, r.distractors(c, d, 2, &total));  // truncated by cap
    TEST_ASSERT_EQUAL_UINT16(3, total);
    r.card(2, &c);
    TEST_ASSERT_EQUAL_UINT16(0, r.distractors(c, d, 8));
}

void test_reading_distractor_list() {
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "ひ", {1, 2}, {2}},
                                {0x6708, "month", "ゲツ", "つき", {0}, {0}},
                                {0x706B, "fire", "カ", "ひ", {0, 1}, {}}};  // no reading list
    auto buf = makePack(cs, someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    Card c;
    uint16_t d[4], total;
    r.card(0, &c);
    TEST_ASSERT_EQUAL_UINT16(2, r.distractors(c, d, 4, &total));  // meaning list
    TEST_ASSERT_EQUAL_UINT16(2, total);
    TEST_ASSERT_EQUAL_UINT16(1, r.distractors(c, d, 4, &total, true));  // reading list
    TEST_ASSERT_EQUAL_UINT16(1, total);
    TEST_ASSERT_EQUAL_UINT16(2, d[0]);
    r.card(2, &c);
    TEST_ASSERT_EQUAL_UINT16(2, r.distractors(c, d, 4, &total, true));  // falls back
    TEST_ASSERT_EQUAL_UINT16(0, d[0]);
    TEST_ASSERT_EQUAL_UINT16(1, d[1]);
}

void test_glyph_lookup_and_bitmap() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    const Glyph* g = r.findGlyph(0, 0x65E5);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(19, g->width);
    TEST_ASSERT_EQUAL_UINT8(26, g->height);
    TEST_ASSERT_EQUAL_INT8(-26, g->yOff);
    TEST_ASSERT_NULL(r.findGlyph(1, 'A'));     // wrong set
    TEST_ASSERT_NULL(r.findGlyph(0, 0x1234));  // absent
    TEST_ASSERT_NOT_NULL(r.findGlyph(1, 0x65E5));
    TEST_ASSERT_NOT_NULL(r.findGlyph(2, 'a'));
    TEST_ASSERT_NOT_NULL(r.findGlyph(2, ' '));
    for (uint32_t i = 0; i < r.glyphCount(); ++i) {  // every glyph findable
        const Glyph* gi = r.glyphAt(i);
        TEST_ASSERT_EQUAL_PTR(gi, r.findGlyph(gi->set, gi->codepoint));
    }
    const Glyph* a = r.findGlyph(2, 'A');
    size_t n = PackReader::bitmapSize(*a);
    TEST_ASSERT_EQUAL_UINT32(5 * 9, n);  // ceil(9/2) * 9
    std::vector<uint8_t> bm(n);
    TEST_ASSERT_TRUE(r.readGlyphBitmap(*a, bm.data(), bm.size()));
    for (size_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_UINT8((('A') + i) & 0xFF, bm[i]);
    TEST_ASSERT_FALSE(r.readGlyphBitmap(*a, bm.data(), n - 1));  // too small
    const Glyph* sp = r.findGlyph(2, ' ');
    TEST_ASSERT_EQUAL_UINT32(0, PackReader::bitmapSize(*sp));
    TEST_ASSERT_TRUE(r.readGlyphBitmap(*sp, bm.data(), 0));
}

void test_rejects_bad_packs() {
    auto good = makePack(sixCards(), someGlyphs());
    PackReader r;
    {
        auto b = good; b[0] = 'X';
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadMagic, r.open(s, defaultAllocator(), gPackMem));
    }
    {
        auto b = good; b[4] = 4;  // newer than this reader
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadVersion, r.open(s, defaultAllocator(), gPackMem));
    }
    {
        auto b = good; b[4] = 0;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadVersion, r.open(s, defaultAllocator(), gPackMem));
    }
    {
        auto b = good; b[16] = 0xFF; b[17] = 0xFF;  // cards_off way out
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadHeader, r.open(s, defaultAllocator(), gPackMem));
    }
    {
        std::vector<uint8_t> b(good.begin(), good.begin() + 40);
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadHeader, r.open(s, defaultAllocator(), gPackMem));
    }
    {
        std::vector<uint8_t> b(good.begin(), good.end() - 10);  // glyph data truncated
        MemorySource s(b);
        PackError e = r.open(s, defaultAllocator(), gPackMem);
        TEST_ASSERT_TRUE(e == PackError::BadTable || e == PackError::BadHeader);
    }
    TEST_ASSERT_FALSE(r.isOpen());
}

void test_rejects_unsupported_glyph_encoding() {
    // v1 defines only encoding 0 (raw 4bpp); v2 adds 1 (RLE). Anything else
    // must be refused at open time, not later at draw time.
    const uint32_t glyphsOff = 64 + 6 * 32;  // header + card table
    PackReader r;
    {
        auto buf = makePack(sixCards(), someGlyphs());
        buf[glyphsOff + 5] = 1;  // first glyph entry's encoding byte: RLE in a v1 pack
        MemorySource src(buf);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(src, defaultAllocator(), gPackMem));
    }
    {
        auto buf = makePack(sixCards(), someGlyphs(), {}, 2);
        MemorySource ok(buf);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(ok, defaultAllocator(), gPackMem));  // v2 with raw glyphs only
        r.close();
        buf[glyphsOff + 5] = 2;  // unknown in v2 too
        MemorySource src(buf);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(src, defaultAllocator(), gPackMem));
    }
    TEST_ASSERT_FALSE(r.isOpen());
}

// A v2 pack mixing raw and RLE glyphs and raw/runs/row-XOR masks (the streams
// are the docs/pack-format.md examples): everything reads back decoded.
void test_v2_pack_decodes_glyphs_and_masks() {
    std::vector<GlyphSpec> g = someGlyphs();
    // 5x3 spec example: 00 7F F0 00 7F F0 FF FF 30 as RLE 02 7F 20 05 F4 30.
    g.push_back({3, 0x30CB, 5, 3, 1, {0x02, 0x7F, 0x20, 0x05, 0xF4, 0x30}});
    std::vector<std::vector<StrokeSpec>> st(sixCards().size());
    st[0] = {{5, 4, 8, 3},                    // raw test pattern
             {1, 1, 16, 2, 1, {0x3A, 0x7A}},  // runs
             {2, 3, 4, 6, 2, {0x12}}};        // runs of the row XOR
    auto buf = makePack(sixCards(), g, st, 2);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    TEST_ASSERT_EQUAL_UINT16(2, r.header().version);

    const Glyph* rle = r.findGlyph(3, 0x30CB);
    TEST_ASSERT_NOT_NULL(rle);
    TEST_ASSERT_EQUAL_UINT8(kGlyphEncRle, rle->encoding);
    uint8_t bm[64];
    TEST_ASSERT_TRUE(r.readGlyphBitmap(*rle, bm, PackReader::bitmapSize(*rle)));
    const uint8_t want[] = {0x00, 0x7F, 0xF0, 0x00, 0x7F, 0xF0, 0xFF, 0xFF, 0x30};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, bm, sizeof want);
    TEST_ASSERT_FALSE(r.readGlyphBitmap(*rle, bm, 8));  // cap below the decoded size
    const Glyph* raw = r.findGlyph(2, 'A');  // raw glyphs read as before
    TEST_ASSERT_TRUE(r.readGlyphBitmap(*raw, bm, sizeof bm));
    for (size_t i = 0; i < PackReader::bitmapSize(*raw); ++i)
        TEST_ASSERT_EQUAL_UINT8(('A' + i) & 0xFF, bm[i]);

    Stroke s;
    TEST_ASSERT_EQUAL_UINT16(3, r.strokeCount(0));
    TEST_ASSERT_TRUE(r.strokeAt(0, 0, &s));
    TEST_ASSERT_EQUAL_UINT8(kMaskEncRaw, s.encoding);
    TEST_ASSERT_EQUAL_UINT16(3, s.size);
    TEST_ASSERT_TRUE(r.readStrokeBitmap(s, bm, sizeof bm));
    for (size_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT8((0xA0 + i) & 0xFF, bm[i]);
    const uint32_t first = s.dataOff;
    TEST_ASSERT_TRUE(r.strokeAt(0, 1, &s));
    TEST_ASSERT_EQUAL_UINT8(kMaskEncRuns, s.encoding);
    TEST_ASSERT_EQUAL_UINT32(first + 3, s.dataOff);  // back to back
    TEST_ASSERT_EQUAL_UINT32(4, PackReader::strokeBitmapSize(s));
    TEST_ASSERT_TRUE(r.readStrokeBitmap(s, bm, sizeof bm));
    const uint8_t m1[] = {0x1F, 0xF8, 0x0F, 0xFC};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(m1, bm, 4);
    TEST_ASSERT_FALSE(r.readStrokeBitmap(s, bm, 3));  // cap below the decoded size
    TEST_ASSERT_TRUE(r.strokeAt(0, 2, &s));
    TEST_ASSERT_EQUAL_UINT8(kMaskEncXorRuns, s.encoding);
    TEST_ASSERT_EQUAL_UINT32(first + 5, s.dataOff);
    TEST_ASSERT_TRUE(r.readStrokeBitmap(s, bm, sizeof bm));
    for (int y = 0; y < 6; ++y) TEST_ASSERT_EQUAL_HEX8(0x60, bm[y]);
    TEST_ASSERT_FALSE(r.strokeAt(0, 3, &s));
    TEST_ASSERT_EQUAL_UINT16(0, r.strokeCount(1));
}

void test_v2_rejects_bad_masks_and_glyphs() {
    std::vector<std::vector<StrokeSpec>> st(sixCards().size());
    st[0] = {{5, 4, 8, 3}, {1, 1, 16, 2, 1, {0x3A, 0x7A}}};
    const auto good = makePack(sixCards(), someGlyphs(), st, 2);
    const uint32_t blk = rd32le(good, rd32le(good, 60));
    PackReader r;
    {
        MemorySource s(good);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(s, defaultAllocator(), gPackMem));
        r.close();
    }
    {  // unknown mask encoding
        auto b = good;
        b[blk + 8 + 8 + 4] = 3;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // raw mask whose size is not ceil(w/8)*h
        auto b = good;
        b[blk + 8 + 6] = 2;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // encoded mask larger than its raw size
        auto b = good;
        b[blk + 8 + 8 + 6] = 5;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // first mask offset past the end of the file
        auto b = good;
        b[blk + 4] = 0xF0; b[blk + 5] = 0xFF; b[blk + 6] = 0xFF; b[blk + 7] = 0x7F;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // a corrupt RLE stream opens (streams are checked when read) but fails to decode
        std::vector<GlyphSpec> g = someGlyphs();
        g.push_back({3, 0x30CB, 5, 3, 1, {0xF0, 0x00, 0x00}});  // reserved "F 0"
        auto b = makePack(sixCards(), g, {}, 2);
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(s, defaultAllocator(), gPackMem));
        uint8_t bm[16];
        TEST_ASSERT_FALSE(r.readGlyphBitmap(*r.findGlyph(3, 0x30CB), bm, sizeof bm));
        r.close();
    }
    {  // an RLE glyph whose stream starts before the glyph data is refused
        std::vector<GlyphSpec> g = someGlyphs();
        g.push_back({3, 0x30CB, 5, 3, 1, {0x02, 0x7F, 0x20, 0x05, 0xF4, 0x30}});
        auto b = makePack(sixCards(), g, {}, 2);
        const uint32_t e = 64 + 6 * 32 + (static_cast<uint32_t>(g.size()) - 1) * 16;  // set 3: last
        b[e + 12] = 0; b[e + 13] = 0; b[e + 14] = 0; b[e + 15] = 0;  // data_off 0
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    TEST_ASSERT_FALSE(r.isOpen());
}

// ---------------------------------------------------------------- strokes
void test_stroke_section_read() {
    auto buf = makePack(sixCards(), someGlyphs(), someStrokes());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    TEST_ASSERT_TRUE(r.hasStrokeOrder());

    uint16_t count = 0, px = 0;
    TEST_ASSERT_TRUE(r.strokeInfo(0, &count, &px));
    TEST_ASSERT_EQUAL_UINT16(2, count);
    TEST_ASSERT_EQUAL_UINT16(32, px);
    TEST_ASSERT_EQUAL_UINT16(2, r.strokeCount(0));
    TEST_ASSERT_EQUAL_UINT16(0, r.strokeCount(1));  // card 1 has no data
    TEST_ASSERT_FALSE(r.strokeInfo(1, &count, &px));
    TEST_ASSERT_EQUAL_UINT16(1, r.strokeCount(2));
    TEST_ASSERT_EQUAL_UINT16(0, r.strokeCount(6));  // out of range

    Stroke s;
    TEST_ASSERT_TRUE(r.strokeAt(0, 0, &s));
    TEST_ASSERT_EQUAL_INT8(5, s.xOff);
    TEST_ASSERT_EQUAL_INT8(4, s.yOff);
    TEST_ASSERT_EQUAL_UINT8(8, s.w);
    TEST_ASSERT_EQUAL_UINT8(3, s.h);
    TEST_ASSERT_EQUAL_UINT32(3, PackReader::strokeBitmapSize(s));  // ceil(8/8)*3
    uint8_t bm[64];
    TEST_ASSERT_TRUE(r.readStrokeBitmap(s, bm, sizeof bm));
    for (size_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT8((0xA0 + i) & 0xFF, bm[i]);
    TEST_ASSERT_FALSE(r.readStrokeBitmap(s, bm, 2));  // cap too small

    TEST_ASSERT_TRUE(r.strokeAt(0, 1, &s));
    TEST_ASSERT_EQUAL_UINT8(3, s.w);
    TEST_ASSERT_EQUAL_UINT8(9, s.h);
    TEST_ASSERT_EQUAL_UINT32(9, PackReader::strokeBitmapSize(s));  // ceil(3/8)*9
    TEST_ASSERT_FALSE(r.strokeAt(0, 2, &s));  // out of range
    TEST_ASSERT_FALSE(r.strokeAt(1, 0, &s));  // card without stroke data
}

void test_rejects_bad_stroke_section() {
    auto good = makePack(sixCards(), someGlyphs(), someStrokes());
    const uint32_t base = rd32le(good, 60);
    PackReader r;
    {  // stroke_off far out of bounds
        auto b = good;
        b[60] = 0xFF; b[61] = 0xFF; b[62] = 0xFF; b[63] = 0x7F;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // an index entry pointing out of bounds
        auto b = good;
        b[base] = static_cast<uint8_t>(b.size() - 2);
        b[base + 1] = b[base + 2] = b[base + 3] = 0;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // a mask data_off past the end of the file
        auto b = good;
        uint32_t blk = rd32le(good, base);  // first card's block
        b[blk + 12] = 0xFF; b[blk + 13] = 0xFF; b[blk + 14] = 0xFF; b[blk + 15] = 0x7F;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    {  // a block px that disagrees with the header
        auto b = good;
        uint32_t blk = rd32le(good, base);
        b[blk + 2] = 99;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s, defaultAllocator(), gPackMem));
    }
    TEST_ASSERT_FALSE(r.isOpen());
}

// ---------------------------------------------------------------- quiz
void test_quiz_text_helpers() {
    TEST_ASSERT_EQUAL(3, meaningCount("day; sun; Japan"));
    TEST_ASSERT_EQUAL(1, meaningCount("fire"));
    TEST_ASSERT_EQUAL(0, meaningCount(""));
    TEST_ASSERT_EQUAL_STRING("day", meaningAt("day; sun; Japan", 0).c_str());
    TEST_ASSERT_EQUAL_STRING("Japan", meaningAt("day; sun; Japan", 2).c_str());
    TEST_ASSERT_EQUAL_STRING("", meaningAt("fire", 1).c_str());
    TEST_ASSERT_EQUAL_STRING("たべる", firstReadingDisplay("た.べる、く.う").c_str());
    TEST_ASSERT_EQUAL_STRING("ニチ", firstReadingDisplay("ニチ、ジツ").c_str());
    TEST_ASSERT_EQUAL_STRING("", firstReadingDisplay("").c_str());
    TEST_ASSERT_EQUAL_STRING("たべる、くう", readingsDisplay("た.べる、く.う").c_str());
    TEST_ASSERT_EQUAL_STRING("ニチ、ジツ", readingsDisplay("ニチ、ジツ").c_str());
    TEST_ASSERT_EQUAL_STRING("", readingsDisplay("").c_str());
    // Katakana to hiragana: the whole block (small kana, ヴ, ヵヶ, the iteration
    // marks); ー, the separator, hiragana and ASCII stay.
    TEST_ASSERT_EQUAL_STRING("にち、じつ", toHiragana("ニチ、ジツ").c_str());
    TEST_ASSERT_EQUAL_STRING("きょう", toHiragana("キョウ").c_str());
    TEST_ASSERT_EQUAL_STRING("ぁゔゕゖゝゞ", toHiragana("ァヴヵヶヽヾ").c_str());
    TEST_ASSERT_EQUAL_STRING("らーめん", toHiragana("ラーメン").c_str());
    TEST_ASSERT_EQUAL_STRING("たべる A1", toHiragana("たべる A1").c_str());
    TEST_ASSERT_EQUAL_STRING("ヷ", toHiragana("ヷ").c_str());  // no hiragana form
    TEST_ASSERT_EQUAL_STRING("", toHiragana("").c_str());
}

void test_quiz_meaning() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(12345);
    const char* meanings[] = {"day; sun; Japan", "month; moon",       "fire",
                              "water",            "tree; wood",        "middle"};
    for (int iter = 0; iter < 300; ++iter) {
        uint32_t idx = iter % 6;
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, idx, QuizMode::Meaning, rng, &q));
        TEST_ASSERT_EQUAL(4, q.count);
        // Only the first meaning of each card is ever shown.
        TEST_ASSERT_EQUAL_STRING(meaningAt(meanings[idx], 0).c_str(), q.options[q.correct].c_str());
        TEST_ASSERT_EQUAL_UINT16(idx, q.optionCard[q.correct]);
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) {
                TEST_ASSERT_TRUE(q.options[i] != q.options[j]);
                TEST_ASSERT_TRUE(q.optionCard[i] != q.optionCard[j]);
            }
    }
}

void test_quiz_meaning_to_kanji() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(11);
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::MeaningToKanji, rng, &q));
        TEST_ASSERT_EQUAL(QuizMode::MeaningToKanji, q.mode);
        TEST_ASSERT_EQUAL(4, q.count);
        TEST_ASSERT_EQUAL_STRING("day", q.options[q.correct].c_str());  // prompt = first gloss
        TEST_ASSERT_EQUAL_UINT16(0, q.optionCard[q.correct]);
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) {
                TEST_ASSERT_TRUE(q.options[i] != q.options[j]);       // no ambiguous kanji
                TEST_ASSERT_TRUE(q.optionCard[i] != q.optionCard[j]);
            }
    }
}

void test_quiz_reading_excludes_target_readings() {
    // A distractor whose reading is another reading of the target must never be
    // offered: two options would both be correct.
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ、ジツ", "ひ、び", {1, 2, 3, 4}},
                                {0x706B, "fire", "カ", "ひ", {}},   // kun ひ is a 日 reading
                                {0x6728, "tree", "ジツ", "", {}},    // on ジツ is a 日 reading
                                {0x6708, "month", "ゲツ", "つき", {}},
                                {0x6C34, "water", "スイ", "みず", {}}};
    auto buf = makePack(cs, someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(21);
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Reading, rng, &q, ReadingKind::Kun));
        TEST_ASSERT_EQUAL_STRING("ひ", q.options[q.correct].c_str());
        for (int i = 0; i < q.count; ++i) TEST_ASSERT_TRUE(q.optionCard[i] != 1);
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Reading, rng, &q, ReadingKind::On));
        TEST_ASSERT_EQUAL_STRING("ニチ", q.options[q.correct].c_str());
        for (int i = 0; i < q.count; ++i) TEST_ASSERT_TRUE(q.optionCard[i] != 2);
    }
}

// A card whose strings outlive later reads: a streamed reader's string cache
// keeps only the last PackReader::kStringSlots strings, and these tests hold
// the target across many questions.
struct HeldCard {
    Card c{};
    std::string meanings, onyomi, kunyomi;
};
bool holdCard(const PackReader& r, uint32_t idx, HeldCard* h) {
    if (!r.card(idx, &h->c)) return false;
    h->meanings = h->c.meanings;
    h->onyomi = h->c.onyomi;
    h->kunyomi = h->c.kunyomi;
    h->c.meanings = h->meanings.c_str();
    h->c.onyomi = h->onyomi.c_str();
    h->c.kunyomi = h->kunyomi.c_str();
    return true;
}

void test_quiz_relaxed_pass_excludes_target_readings_of_both_kinds() {
    // Too few cards of the asked kind, so the relaxed pass offers a card's other
    // kind. Card 2's on'yomi ジツ (katakana in a kun'yomi question) and card 3's
    // kun'yomi か (hiragana in an on'yomi question) are readings of 日 as well:
    // either would be a second correct option. So is card 4's カ, in the other
    // script (日's kun'yomi か): in hiragana it reads the same. Cards 5 and 6 are
    // fine fillers.
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ、ジツ", "ひ、か", {}},
                                {0x6708, "month", "ゲツ", "つき", {}},
                                {0x5B9F, "truth", "ジツ、シツ", "", {}},  // on only
                                {0x868A, "mosquito", "", "か", {}},       // kun only
                                {0x706B, "fire", "カ", "", {}},           // on only
                                {0x4E2D, "middle", "", "なか", {}},       // kun only
                                {0x6C34, "water", "スイ", "", {}}};       // on only
    auto buf = makePack(cs, cardGlyphs(cs));
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(31);
    HeldCard held;
    TEST_ASSERT_TRUE(holdCard(r, 0, &held));
    const Card& t = held.c;
    for (int iter = 0; iter < 200; ++iter) {
        const ReadingKind want = (iter & 1) ? ReadingKind::On : ReadingKind::Kun;
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Reading, rng, &q, want));
        TEST_ASSERT_EQUAL(want, q.readingKind);
        TEST_ASSERT_EQUAL_STRING(want == ReadingKind::Kun ? "ひ" : "ニチ", q.options[q.correct].c_str());
        TEST_ASSERT_EQUAL(4, q.count);  // the relaxed pass still fills: スイ or なか
        for (int i = 0; i < q.count; ++i) {
            if (i == q.correct) continue;
            TEST_ASSERT_FALSE(isReadingOf(q.options[i], t.onyomi));
            TEST_ASSERT_FALSE(isReadingOf(q.options[i], t.kunyomi));
            TEST_ASSERT_TRUE(q.optionCard[i] != (want == ReadingKind::Kun ? 2 : 3));
            TEST_ASSERT_TRUE(q.optionCard[i] != 4);
        }
    }
}

namespace {
ReadingKind otherKind(ReadingKind k) { return k == ReadingKind::On ? ReadingKind::Kun : ReadingKind::On; }
}  // namespace

void test_quiz_sparse_kun_fallback_and_options() {
    // A kind the card lacks falls back to its other one; the options are all of
    // the asked kind (the deck has enough of each for the strict pass), so a
    // kun'yomi question never offers a card without kun'yomi: no empty option,
    // no duplicate, no katakana. 々 (neither kind) is never a reading question.
    auto cs = sparseKunCards();
    auto buf = makePack(cs, cardGlyphs(cs));
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(77);
    const ReadingKind wants[] = {ReadingKind::On, ReadingKind::Kun};
    for (uint32_t idx = 0; idx < cs.size(); ++idx) {
        HeldCard held;
        TEST_ASSERT_TRUE(holdCard(r, idx, &held));
        const Card& t = held.c;
        for (ReadingKind want : wants) {
            for (int iter = 0; iter < 40; ++iter) {
                Question q;
                const bool ok = buildQuestion(r, idx, QuizMode::Reading, rng, &q, want);
                if (readingKindOf(t) == ReadingKind::None) {
                    TEST_ASSERT_FALSE(ok);
                    TEST_ASSERT_TRUE(buildQuestion(r, idx, QuizMode::Meaning, rng, &q));
                    continue;
                }
                TEST_ASSERT_TRUE(ok);
                const ReadingKind asked = hasReading(t, want) ? want : otherKind(want);
                TEST_ASSERT_EQUAL(asked, q.readingKind);
                TEST_ASSERT_EQUAL(4, q.count);
                TEST_ASSERT_EQUAL_UINT16(idx, q.optionCard[q.correct]);
                const char* own = asked == ReadingKind::Kun ? t.kunyomi : t.onyomi;
                TEST_ASSERT_EQUAL_STRING(firstReadingDisplay(own).c_str(), q.options[q.correct].c_str());
                for (int i = 0; i < q.count; ++i) {
                    TEST_ASSERT_FALSE(q.options[i].empty());
                    Card c;
                    TEST_ASSERT_TRUE(r.card(q.optionCard[i], &c));
                    TEST_ASSERT_TRUE(hasReading(c, asked));
                    const bool kata = hasInRange(q.options[i], 0x30A0, 0x30FF);
                    const bool hira = hasInRange(q.options[i], 0x3041, 0x309F);
                    TEST_ASSERT_TRUE(asked == ReadingKind::On ? kata && !hira : hira && !kata);
                    for (int j = i + 1; j < q.count; ++j) {
                        TEST_ASSERT_TRUE(q.options[i] != q.options[j]);
                        TEST_ASSERT_TRUE(q.optionCard[i] != q.optionCard[j]);
                    }
                    if (i == q.correct) continue;
                    TEST_ASSERT_FALSE(isReadingOf(q.options[i], t.onyomi));
                    TEST_ASSERT_FALSE(isReadingOf(q.options[i], t.kunyomi));
                }
            }
        }
    }
}

void test_quiz_reading_turn() {
    ReadingTurn t;
    TEST_ASSERT_EQUAL(ReadingKind::On, t.want());  // on'yomi first
    t.asked(ReadingKind::On);
    TEST_ASSERT_EQUAL(ReadingKind::Kun, t.want());
    t.asked(ReadingKind::On);  // kun wanted, but the card had none
    TEST_ASSERT_EQUAL(ReadingKind::Kun, t.want());  // still kun'yomi's turn
    t.asked(ReadingKind::Kun);
    TEST_ASSERT_EQUAL(ReadingKind::On, t.want());
    t.asked(ReadingKind::Kun);  // on wanted, a kun-only card
    TEST_ASSERT_EQUAL(ReadingKind::On, t.want());
    t.asked(ReadingKind::None);  // nothing asked: no change
    TEST_ASSERT_EQUAL(ReadingKind::On, t.want());
}

void test_quiz_sparse_kun_alternation() {
    // The app's loop (main.cpp, newQuestion()): want() into buildQuestion(),
    // then asked(). Over a session of the sparse deck the kinds alternate: the
    // same kind twice in a row only when the second card lacks the other one,
    // so a card without kun'yomi does not use up kun'yomi's turn.
    auto cs = sparseKunCards();
    auto buf = makePack(cs, cardGlyphs(cs));
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(5);
    ReadingTurn turn;
    ReadingKind prev = ReadingKind::None;
    int on = 0, kun = 0, none = 0;
    for (uint32_t i = 0; i < 400; ++i) {
        const uint32_t idx = (i * 7) % cs.size();  // every card, in a mixed order
        HeldCard held;
        TEST_ASSERT_TRUE(holdCard(r, idx, &held));
        const Card& c = held.c;
        const ReadingKind want = turn.want();
        Question q;
        if (!buildQuestion(r, idx, QuizMode::Reading, rng, &q, want)) {
            TEST_ASSERT_EQUAL(ReadingKind::None, readingKindOf(c));  // only 々
            ++none;
            continue;
        }
        turn.asked(q.readingKind);
        TEST_ASSERT_EQUAL(hasReading(c, want) ? want : otherKind(want), q.readingKind);
        if (q.readingKind == prev) TEST_ASSERT_FALSE(hasReading(c, otherKind(prev)));
        prev = q.readingKind;
        ++(q.readingKind == ReadingKind::Kun ? kun : on);
    }
    TEST_ASSERT_EQUAL(25, none);
    TEST_ASSERT_EQUAL(375, on + kun);
    // 9 of the 15 askable cards have kun'yomi, 13 on'yomi: kun'yomi come up
    // less, but more than when a card without them used up their turn (138).
    TEST_ASSERT_EQUAL(150, kun);
}

void test_quiz_distractor_priority_and_shuffle() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(7);
    int pos[4] = {0, 0, 0, 0};
    for (int iter = 0; iter < 400; ++iter) {
        Question q;
        // card 4 lists {0,1} first: both must always appear.
        TEST_ASSERT_TRUE(buildQuestion(r, 4, QuizMode::Meaning, rng, &q));
        bool has0 = false, has1 = false;
        for (int i = 0; i < 4; ++i) {
            has0 |= q.optionCard[i] == 0;
            has1 |= q.optionCard[i] == 1;
        }
        TEST_ASSERT_TRUE(has0 && has1);
        ++pos[q.correct];
    }
    for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(pos[i] > 50);  // roughly uniform
}

void test_quiz_reading_kinds() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(99);
    // Card 0 has on'yomi: with no kind requested it asks on'yomi; every option
    // is an on'yomi (katakana), so the kun-only card 5 is excluded.
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Reading, rng, &q));
        TEST_ASSERT_EQUAL(ReadingKind::On, q.readingKind);
        TEST_ASSERT_EQUAL(4, q.count);
        TEST_ASSERT_EQUAL_STRING("ニチ", q.options[q.correct].c_str());
        for (int i = 0; i < 4; ++i) {
            TEST_ASSERT_TRUE(q.optionCard[i] != 5);  // kun-only card excluded
            TEST_ASSERT_TRUE(hasInRange(q.options[i], 0x30A0, 0x30FF));  // katakana
        }
    }
    // Requesting kun'yomi on a card that also has on'yomi asks the kun reading,
    // and every option is a kun reading (hiragana).
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Reading, rng, &q, ReadingKind::Kun));
        TEST_ASSERT_EQUAL(ReadingKind::Kun, q.readingKind);
        TEST_ASSERT_EQUAL_STRING("ひ", q.options[q.correct].c_str());
        TEST_ASSERT_EQUAL_UINT16(0, q.optionCard[q.correct]);
        for (int i = 0; i < 4; ++i)
            TEST_ASSERT_FALSE(hasInRange(q.options[i], 0x30A0, 0x30FF));  // no katakana
    }
    // Card 5 is kun-only: asking for on'yomi falls back to its primary kind.
    Question q;
    TEST_ASSERT_TRUE(buildQuestion(r, 5, QuizMode::Reading, rng, &q, ReadingKind::On));
    TEST_ASSERT_EQUAL(ReadingKind::Kun, q.readingKind);
    TEST_ASSERT_EQUAL_STRING("なか", q.options[q.correct].c_str());
    TEST_ASSERT_EQUAL(4, q.count);
}

void test_quiz_on_kana() {
    // The same seed builds the same question in either script; in hiragana no
    // option has katakana, and restyleReadings() turns one into the other.
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    const ReadingKind wants[] = {ReadingKind::On, ReadingKind::Kun};
    for (uint32_t idx = 0; idx < 6; ++idx) {
        for (ReadingKind want : wants) {
            XorShiftRng ra(500 + idx), rb(500 + idx);
            Question k, h;
            TEST_ASSERT_TRUE(buildQuestion(r, idx, QuizMode::Reading, ra, &k, want));
            TEST_ASSERT_TRUE(
                buildQuestion(r, idx, QuizMode::Reading, rb, &h, want, OnKana::Hiragana));
            TEST_ASSERT_EQUAL(k.count, h.count);
            TEST_ASSERT_EQUAL(k.correct, h.correct);
            TEST_ASSERT_EQUAL(k.readingKind, h.readingKind);
            for (int i = 0; i < k.count; ++i) {
                TEST_ASSERT_EQUAL_UINT16(k.optionCard[i], h.optionCard[i]);
                TEST_ASSERT_EQUAL_STRING(toHiragana(k.options[i]).c_str(), h.options[i].c_str());
                TEST_ASSERT_FALSE(hasInRange(h.options[i], 0x30A0, 0x30FF));
            }
            Question t = k;
            restyleReadings(r, OnKana::Hiragana, &t);
            for (int i = 0; i < k.count; ++i)
                TEST_ASSERT_EQUAL_STRING(h.options[i].c_str(), t.options[i].c_str());
            restyleReadings(r, OnKana::Katakana, &t);
            for (int i = 0; i < k.count; ++i)
                TEST_ASSERT_EQUAL_STRING(k.options[i].c_str(), t.options[i].c_str());
        }
    }
    // Other modes are left alone.
    XorShiftRng rng(3);
    Question m;
    TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Meaning, rng, &m));
    const std::string before = m.options[0];
    restyleReadings(r, OnKana::Hiragana, &m);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), m.options[0].c_str());
}

void test_quiz_options_differ_in_either_script() {
    // A kun'yomi question on 日 with too few kun cards: the relaxed pass offers
    // on'yomi, and 火's カ would read か in hiragana, as 蚊's kun option does. It
    // is never offered, in either script; 実's ジツ fills the last option.
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "ひ", {}},
                                {0x868A, "mosquito", "", "か", {}},   // kun only
                                {0x706B, "fire", "カ", "", {}},       // on only
                                {0x4E2D, "middle", "", "なか", {}},   // kun only
                                {0x5B9F, "truth", "ジツ", "", {}}};   // on only
    auto buf = makePack(cs, cardGlyphs(cs));
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(41);
    const OnKana scripts[] = {OnKana::Katakana, OnKana::Hiragana};
    for (int iter = 0; iter < 100; ++iter) {
        for (OnKana script : scripts) {
            Question q;
            TEST_ASSERT_TRUE(
                buildQuestion(r, 0, QuizMode::Reading, rng, &q, ReadingKind::Kun, script));
            TEST_ASSERT_EQUAL(4, q.count);
            for (int i = 0; i < q.count; ++i) {
                TEST_ASSERT_TRUE(q.optionCard[i] != 2);
                for (int j = 0; j < i; ++j)
                    TEST_ASSERT_TRUE(toHiragana(q.options[i]) != toHiragana(q.options[j]));
            }
        }
    }
}

void test_quiz_unavailable_and_tiny() {
    std::vector<CardSpec> cs = {{0x65E5, "day", "", "", {}}, {0x6708, "month", "ゲツ", "", {}}};
    std::vector<GlyphSpec> gl = {{0, 0x65E5, 4, 4}, {1, 0x65E5, 4, 4}, {0, 0x6708, 4, 4}, {1, 0x6708, 4, 4}};
    auto buf = makePack(cs, gl);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(1);
    Question q;
    TEST_ASSERT_FALSE(buildQuestion(r, 0, QuizMode::Reading, rng, &q));  // no reading
    TEST_ASSERT_FALSE(buildQuestion(r, 1, QuizMode::Reading, rng, &q));  // no second option
    TEST_ASSERT_TRUE(buildQuestion(r, 1, QuizMode::Meaning, rng, &q));
    TEST_ASSERT_EQUAL(2, q.count);
    TEST_ASSERT_FALSE(buildQuestion(r, 7, QuizMode::Meaning, rng, &q));
}

void test_quiz_meaning_gloss_collisions() {
    // Cards whose first meaning is "day" must never be offered for 日 ("day"):
    // two options would both be correct.
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "", {1, 2}},
                                {0x6708, "day; other", "ゲツ", "", {}},
                                {0x706B, "fire", "カ", "", {}},
                                {0x6C34, "water", "スイ", "", {}},
                                {0x5929, "day", "テン", "", {}}};
    std::vector<GlyphSpec> gl;
    for (auto& c : cs) { gl.push_back({0, c.cp, 4, 4}); gl.push_back({1, c.cp, 4, 4}); }
    auto buf = makePack(cs, gl);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(5);
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Meaning, rng, &q));
        TEST_ASSERT_EQUAL_STRING("day", q.options[q.correct].c_str());
        TEST_ASSERT_EQUAL(3, q.count);  // cards 1 and 4 share "day" and are skipped
        for (int i = 0; i < q.count; ++i) {
            TEST_ASSERT_TRUE(q.optionCard[i] != 1);
            TEST_ASSERT_TRUE(q.optionCard[i] != 4);
        }
    }
}

void test_quiz_meaning_to_kanji_excludes_cards_with_the_prompt() {
    // Meaning -> kanji: card 1's first gloss is "night", but it also means "day",
    // so as a kanji option it would be a second correct answer for "day". It is
    // not in the precomputed list, so this exercises the random-walk fallback.
    std::vector<CardSpec> cs = {{0x65E5, "day", "ニチ", "", {}},
                                {0x591C, "night; day", "ヤ", "", {}},
                                {0x706B, "fire", "カ", "", {}},
                                {0x6C34, "water", "スイ", "", {}},
                                {0x6728, "tree", "モク", "", {}}};
    std::vector<GlyphSpec> gl;
    for (auto& c : cs) { gl.push_back({0, c.cp, 4, 4}); gl.push_back({1, c.cp, 4, 4}); }
    auto buf = makePack(cs, gl);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(9);
    for (int iter = 0; iter < 100; ++iter) {
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::MeaningToKanji, rng, &q));
        TEST_ASSERT_EQUAL(4, q.count);
        for (int i = 0; i < q.count; ++i) TEST_ASSERT_TRUE(q.optionCard[i] != 1);
    }
}

void test_glcd_cp437_map() {
    // ASCII maps to itself.
    TEST_ASSERT_EQUAL(0x41, glcdIndex('A'));
    TEST_ASSERT_EQUAL(0x20, glcdIndex(' '));
    // Spanish accents live in CP437 slots, not at their Unicode positions.
    TEST_ASSERT_EQUAL(0x82, glcdIndex(0x00E9));  // é
    TEST_ASSERT_EQUAL(0xA0, glcdIndex(0x00E1));  // á
    TEST_ASSERT_EQUAL(0xA1, glcdIndex(0x00ED));  // í
    TEST_ASSERT_EQUAL(0xA2, glcdIndex(0x00F3));  // ó
    TEST_ASSERT_EQUAL(0xA3, glcdIndex(0x00FA));  // ú
    TEST_ASSERT_EQUAL(0x81, glcdIndex(0x00FC));  // ü
    TEST_ASSERT_EQUAL(0xA4, glcdIndex(0x00F1));  // ñ
    TEST_ASSERT_EQUAL(0xA5, glcdIndex(0x00D1));  // Ñ
    TEST_ASSERT_EQUAL(0xA8, glcdIndex(0x00BF));  // ¿
    TEST_ASSERT_EQUAL(0xAD, glcdIndex(0x00A1));  // ¡
    // Separators for captions: the 2x2 middle dot, the bullet, the degree sign.
    TEST_ASSERT_EQUAL(0xF9, glcdIndex(0x00B7));  // ·
    TEST_ASSERT_EQUAL(0x07, glcdIndex(0x2022));  // •
    TEST_ASSERT_EQUAL(0xF8, glcdIndex(0x00B0));  // °
    TEST_ASSERT_EQUAL(-1, glcdIndex(0x2219));    // ∙ (CP437 0xF9's own code point is not mapped)
    // Font0 has no glyph for these (meaning text falls back to the pack font).
    TEST_ASSERT_EQUAL(-1, glcdIndex(0x00C1));  // Á
    TEST_ASSERT_EQUAL(-1, glcdIndex(0x6F22));  // 漢
}

void test_utf8_encode() {
    char b[5];
    TEST_ASSERT_EQUAL(0, utf8Encode(0, b));
    TEST_ASSERT_EQUAL_STRING("", b);
    TEST_ASSERT_EQUAL(1, utf8Encode('A', b));
    TEST_ASSERT_EQUAL_STRING("A", b);
    TEST_ASSERT_EQUAL(2, utf8Encode(0x00E9, b));  // é
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9", b);
    TEST_ASSERT_EQUAL(3, utf8Encode(0x7B49, b));  // 等
    TEST_ASSERT_EQUAL_STRING("等", b);
    TEST_ASSERT_EQUAL(4, utf8Encode(0x1F600, b));  // 😀
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", b);
    // Invalid input becomes U+FFFD, as in utf8Next().
    TEST_ASSERT_EQUAL(3, utf8Encode(0xD800, b));
    TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD", b);
    TEST_ASSERT_EQUAL(3, utf8Encode(0x110000, b));
    TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD", b);
    // Round trip through the decoder.
    TEST_ASSERT_EQUAL(3, utf8Encode(0x7B49, b));
    const char* p = b;
    TEST_ASSERT_EQUAL_HEX32(0x7B49, utf8Next(&p));
    TEST_ASSERT_EQUAL('\0', *p);
}

void test_blend() {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, blend565(0xFFFF, 0x0000, 15));
    TEST_ASSERT_EQUAL_HEX16(0x0000, blend565(0xFFFF, 0x0000, 0));
    uint16_t mid = blend565(0xFFFF, 0x0000, 8);
    TEST_ASSERT_EQUAL_UINT16(17, (mid >> 11) & 0x1F);  // 31*8/15 = 16.5 -> 17
    TEST_ASSERT_EQUAL_HEX16(0xF800, blend565(0xF800, 0xF800, 7));
}

// ------------------------------------------ v3 example words

// Words for sixCards(): two for 日, one for 火 (kun only), one for 中 (kun),
// none for the others; 日's on'yomi word has a gloss with an accent.
CardWords sixWords() {
    CardWords w(sixCards().size());
    w[0] = {{"毎日", "まいにち", "todos los días", kWordKindOn, 6, 6},
            {"日", "ひ", "día", kWordKindKun, 0, 3}};
    w[2] = {{"火", "ひ", "", kWordKindKun, 0, 3}};
    w[5] = {{"中", "なか", "inside", kWordKindKun, 0, 6}};
    return w;
}

void test_v3_example_words() {
    const CardWords words = sixWords();
    auto buf = makePack(sixCards(), someGlyphs(), someStrokes(), 2, &words);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    TEST_ASSERT_EQUAL_UINT16(3, r.header().version);
    TEST_ASSERT_EQUAL_UINT16(80, r.header().headerSize);
    TEST_ASSERT_EQUAL_UINT32(80, r.header().cardsOff);
    TEST_ASSERT_TRUE(r.hasExampleWords());
    TEST_ASSERT_TRUE(r.hasStrokeOrder());  // the stroke section still reads
    TEST_ASSERT_EQUAL_UINT16(2, r.strokeCount(0));
    const uint32_t counts[6] = {2, 0, 1, 0, 0, 1};
    for (uint32_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_UINT32(counts[i], r.exampleWordCount(i));
    TEST_ASSERT_EQUAL_UINT32(0, r.exampleWordCount(6));  // out of range

    ExampleWord w[4];
    TEST_ASSERT_EQUAL_UINT32(2, r.exampleWords(0, w, 4));
    TEST_ASSERT_EQUAL_STRING("毎日", w[0].word);
    TEST_ASSERT_EQUAL_STRING("まいにち", w[0].reading);
    TEST_ASSERT_EQUAL_STRING("todos los días", w[0].gloss);
    TEST_ASSERT_EQUAL_UINT8(kWordKindOn, w[0].kind);
    TEST_ASSERT_EQUAL_UINT8(6, w[0].readingStart);
    TEST_ASSERT_EQUAL_UINT8(6, w[0].readingLen);
    TEST_ASSERT_EQUAL_STRING("日", w[1].word);
    TEST_ASSERT_EQUAL_STRING("ひ", w[1].reading);
    TEST_ASSERT_EQUAL_STRING("día", w[1].gloss);
    TEST_ASSERT_EQUAL_UINT8(kWordKindKun, w[1].kind);
    // The card's own strings read as before, next to the words'.
    Card c;
    TEST_ASSERT_TRUE(r.card(0, &c));
    TEST_ASSERT_EQUAL_STRING("day; sun; Japan", c.meanings);
    TEST_ASSERT_EQUAL_STRING("毎日", w[0].word);  // still valid after three more strings

    TEST_ASSERT_EQUAL_UINT32(1, r.exampleWords(0, w, 1));  // cap
    TEST_ASSERT_EQUAL_STRING("毎日", w[0].word);
    TEST_ASSERT_EQUAL_UINT32(1, r.exampleWords(2, w, 4));
    TEST_ASSERT_EQUAL_STRING("火", w[0].word);
    TEST_ASSERT_EQUAL_STRING("", w[0].gloss);  // no gloss: an empty string
    TEST_ASSERT_EQUAL_UINT8(kWordKindKun, w[0].kind);
    TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(1, w, 4));
    TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(9, w, 4));
    TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(0, nullptr, 4));
    // A span that cuts a character or passes the end is reported unknown.
    CardWords bad(6);
    bad[0] = {{"毎日", "まいにち", "", kWordKindOn, 7, 5}, {"日", "ひ", "", kWordKindKun, 0, 4}};
    bad[1] = {{"月", "つき", "", kWordKindKun, 0, 0}};
    auto bb = makePack(sixCards(), someGlyphs(), {}, 2, &bad);
    MemorySource bs(bb);
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(bs, defaultAllocator(), gPackMem));
    TEST_ASSERT_EQUAL_UINT32(2, r.exampleWords(0, w, 4));
    TEST_ASSERT_EQUAL_UINT8(0, w[0].readingStart);
    TEST_ASSERT_EQUAL_UINT8(0, w[0].readingLen);
    TEST_ASSERT_EQUAL_UINT8(0, w[1].readingLen);
    TEST_ASSERT_EQUAL_UINT32(1, r.exampleWords(1, w, 4));
    TEST_ASSERT_EQUAL_UINT8(0, w[0].readingLen);
}

// v1 and v2 packs have no example words; a v3 pack may have none either.
void test_no_example_words_before_v3() {
    ExampleWord w[2];
    PackReader r;
    for (uint16_t v : {1, 2}) {
        auto buf = makePack(sixCards(), someGlyphs(), someStrokes(), v);
        MemorySource src(buf);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
        TEST_ASSERT_FALSE(r.hasExampleWords());
        TEST_ASSERT_EQUAL_UINT32(0, r.header().wordsOff);
        TEST_ASSERT_EQUAL_UINT32(0, r.exampleWordCount(0));
        TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(0, w, 2));
    }
    // A header_size past cards_off (64) would overlap the card table.
    {
        auto buf = makePack(sixCards(), someGlyphs(), {}, 2);
        buf[6] = 80;
        MemorySource src(buf);
        TEST_ASSERT_EQUAL(PackError::BadHeader, r.open(src, defaultAllocator(), gPackMem));
    }
    {
        const CardWords none(sixCards().size());
        auto buf = makePack(sixCards(), someGlyphs(), {}, 2, &none);
        MemorySource src(buf);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
        TEST_ASSERT_TRUE(r.hasExampleWords());
        TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(0, w, 2));
        // words_off 0 in a v3 header: no section at all.
        auto b0 = buf;
        b0[64] = b0[65] = b0[66] = b0[67] = 0;
        MemorySource s0(b0);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(s0, defaultAllocator(), gPackMem));
        TEST_ASSERT_FALSE(r.hasExampleWords());
        TEST_ASSERT_EQUAL_UINT32(0, r.exampleWordCount(0));
    }
    StdioSource mini("../tools/pack-builder/fixtures/mini.tgc");
    if (mini.ok()) {
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(mini, defaultAllocator(), gPackMem));
        TEST_ASSERT_FALSE(r.hasExampleWords());
        TEST_ASSERT_EQUAL_UINT32(0, r.exampleWords(0, w, 2));
    }
}

void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int k = 0; k < 4; ++k) b[off + k] = (v >> (8 * k)) & 0xFF;
}

void test_v3_rejects_bad_words() {
    const CardWords words = sixWords();
    const auto good = makePack(sixCards(), someGlyphs(), {}, 2, &words);
    const uint32_t wo = rd32le(good, 64);
    const uint32_t table = wo + 8 + 4 * 7;  // six cards + 1
    PackReader r;
    auto expect = [&](std::vector<uint8_t> b, PackError want) {
        MemorySource s(b);
        TEST_ASSERT_EQUAL(want, r.open(s, defaultAllocator(), gPackMem));
        TEST_ASSERT_FALSE(r.isOpen());
    };
    {
        MemorySource s(good);
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(s, defaultAllocator(), gPackMem));
        r.close();
    }
    { auto b = good; b[6] = 64; expect(b, PackError::BadHeader); }        // v3 needs 80
    { auto b = good; put32(b, 64, wo + 2); expect(b, PackError::BadTable); }  // misaligned
    { auto b = good; put32(b, 64, static_cast<uint32_t>(b.size())); expect(b, PackError::BadTable); }
    { auto b = good; b[wo] = 12; expect(b, PackError::BadTable); }        // entry_size < 16
    { auto b = good; put32(b, wo + 4, 5); expect(b, PackError::BadTable); }   // count != last
    { auto b = good; put32(b, wo + 8, 1); expect(b, PackError::BadTable); }   // first[0] != 0
    { auto b = good; put32(b, wo + 8 + 4, 3); expect(b, PackError::BadTable); }  // 0,3,2: decreasing
    { auto b = good; put32(b, wo + 4, 400); put32(b, wo + 8 + 24, 400); expect(b, PackError::BadTable); }
    { auto b = good; put32(b, table, 0); expect(b, PackError::BadTable); }    // string outside pool
    { auto b = good; put32(b, table + 16 + 8, 0x7FFFFFFF); expect(b, PackError::BadTable); }
    { auto b = good; b[table + 12] = 2; expect(b, PackError::BadTable); }     // unknown kind
}

// The builder's v3 fixture: the three mini kanji in Spanish, with strokes and
// words picked from hand-written word data (tools/pack-builder/tests/data;
// expected values in fixtures/mini_words.json).
void test_mini_words_fixture() {
    StdioSource src("../tools/pack-builder/fixtures/mini_words.tgc");
    if (!src.ok()) TEST_IGNORE_MESSAGE("mini_words.tgc not found");
    TEST_ASSERT_EQUAL_UINT32(8512, src.size());
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    const PackHeader& h = r.header();
    TEST_ASSERT_EQUAL_UINT16(3, h.version);
    TEST_ASSERT_EQUAL_UINT16(80, h.headerSize);
    TEST_ASSERT_EQUAL_UINT32(80, h.cardsOff);
    TEST_ASSERT_EQUAL_UINT32(153, h.glyphCount);
    TEST_ASSERT_EQUAL_UINT32(2656, h.stringsOff);
    TEST_ASSERT_EQUAL_UINT32(222, h.stringsSize);
    TEST_ASSERT_EQUAL_UINT32(8176, h.strokeOff);
    TEST_ASSERT_EQUAL_UINT32(8392, h.wordsOff);
    TEST_ASSERT_EQUAL_STRING("es", h.lang);
    TEST_ASSERT_TRUE(r.hasExampleWords());
    TEST_ASSERT_EQUAL_UINT16(4, r.strokeCount(0));

    struct E {
        const char *word, *reading, *gloss;
        uint8_t kind, start, len;
    };
    const E exp[3][2] = {
        {{"毎日", "まいにち", "todos los días", 0, 6, 6}, {"日", "ひ", "día", 1, 0, 3}},
        {{"今月", "こんげつ", "", 0, 6, 6}, {"月", "つき", "", 1, 0, 6}},
        {{"火山", "かざん", "volcán", 0, 0, 3}, {"火", "ひ", "fuego", 1, 0, 3}},
    };
    const uint32_t cps[3] = {0x65E5, 0x6708, 0x706B};
    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT32(cps[i], r.codepointAt(i));
        ExampleWord w[2];
        TEST_ASSERT_EQUAL_UINT32(2, r.exampleWords(i, w, 2));
        for (int k = 0; k < 2; ++k) {
            TEST_ASSERT_EQUAL_STRING(exp[i][k].word, w[k].word);
            TEST_ASSERT_EQUAL_STRING(exp[i][k].reading, w[k].reading);
            TEST_ASSERT_EQUAL_STRING(exp[i][k].gloss, w[k].gloss);
            TEST_ASSERT_EQUAL_UINT8(exp[i][k].kind, w[k].kind);
            TEST_ASSERT_EQUAL_UINT8(exp[i][k].start, w[k].readingStart);
            TEST_ASSERT_EQUAL_UINT8(exp[i][k].len, w[k].readingLen);
            // Every character of the word, its reading and gloss has a set-2
            // glyph (ASCII may fall back to the built-in font).
            for (const char* s : {w[k].word, w[k].reading, w[k].gloss}) {
                const char* p = s;
                while (uint32_t cp = utf8Next(&p)) {
                    if (cp >= 0x20 && cp < 0x7F) continue;
                    const Glyph* g = r.findGlyph(kGlyphSetText, cp);
                    TEST_ASSERT_NOT_NULL_MESSAGE(g, s);
                    std::vector<uint8_t> bm(PackReader::bitmapSize(*g));
                    TEST_ASSERT_TRUE(r.readGlyphBitmap(*g, bm.data(), bm.size()));
                }
            }
        }
    }
    // The words' other kanji are in set 2 too, rendered as text (pen and
    // baseline relative: a negative y_off, an advance), never in sets 0 and 1.
    for (uint32_t cp : {0x6BCEu, 0x4ECAu, 0x5C71u}) {  // 毎 今 山
        const Glyph* g = r.findGlyph(kGlyphSetText, cp);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_TRUE(g->yOff < 0 && g->advance > 0 && g->width > 0);
        TEST_ASSERT_NULL(r.findGlyph(kGlyphSetLarge, cp));
        TEST_ASSERT_NULL(r.findGlyph(kGlyphSetSmall, cp));
    }
    // And the pack still quizzes as a v2 one does.
    XorShiftRng rng(5);
    Question q;
    TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Meaning, rng, &q));
    TEST_ASSERT_EQUAL(3, q.count);
}

// ------------------------------------------ real v1 fixture (mini_v1.tgc)
// The last mini.tgc written by the v1 builder, kept frozen so this reader keeps
// opening the v1 packs already on devices. The v2 mini.tgc is checked against
// the builder's decoded bitmaps in test/test_rle.
static const char* kMiniV1 = "../tools/pack-builder/fixtures/mini_v1.tgc";

void test_mini_v1_fixture() {
    StdioSource src(kMiniV1);
    if (!src.ok()) TEST_IGNORE_MESSAGE("mini_v1.tgc not found");
    TEST_ASSERT_EQUAL_UINT32(8288, src.size());
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    const PackHeader& h = r.header();
    TEST_ASSERT_EQUAL_UINT16(1, h.version);
    TEST_ASSERT_EQUAL_UINT32(3, h.cardCount);
    TEST_ASSERT_EQUAL_UINT32(129, h.glyphCount);
    TEST_ASSERT_EQUAL_UINT32(64, h.cardsOff);
    TEST_ASSERT_EQUAL_UINT32(160, h.glyphsOff);
    TEST_ASSERT_EQUAL_UINT32(2224, h.distractOff);
    TEST_ASSERT_EQUAL_UINT32(2256, h.stringsOff);
    TEST_ASSERT_EQUAL_UINT32(116, h.stringsSize);
    TEST_ASSERT_EQUAL_UINT32(2372, h.glyphDataOff);
    TEST_ASSERT_EQUAL_STRING("en", h.lang);
    TEST_ASSERT_EQUAL_UINT8(32, h.largePx);
    TEST_ASSERT_EQUAL_UINT8(16, h.smallPx);
    TEST_ASSERT_EQUAL_UINT8(12, h.textPx);
    TEST_ASSERT_EQUAL_UINT8(18, h.textLineHeight);
    TEST_ASSERT_EQUAL_UINT8(14, h.textAscent);
    TEST_ASSERT_EQUAL_UINT8(18, h.optionPx);
    TEST_ASSERT_EQUAL_UINT8(27, h.optionLineHeight);
    TEST_ASSERT_EQUAL_UINT8(21, h.optionAscent);
    TEST_ASSERT_EQUAL_UINT32(0, h.buildTime);
    TEST_ASSERT_EQUAL_STRING("JLPT N5", r.deckName());

    struct E {
        uint32_t cp;
        const char *m, *on, *kun;
        uint16_t dis[2], rdis[2];
        uint8_t rdisN;  // reading lists can be shorter (target readings are excluded)
        uint16_t freq, level;
    };
    const E exp[3] = {
        {26085, "day; sun; Japan", "ニチ、ジツ", "ひ、び、か", {2, 1}, {1, 0}, 1, 1, 0},
        {26376, "month; moon", "ゲツ、ガツ", "つき", {2, 0}, {0, 2}, 2, 23, 1},
        {28779, "fire", "カ", "ひ、び、ほ", {1, 0}, {1, 0}, 1, 574, 2},
    };
    const uint32_t distOffs[3] = {2224, 2230, 2236};
    const uint32_t readOffs[3] = {2242, 2246, 2252};
    for (uint32_t i = 0; i < 3; ++i) {
        Card c;
        TEST_ASSERT_TRUE(r.card(i, &c));
        TEST_ASSERT_EQUAL_UINT32(exp[i].cp, c.codepoint);
        TEST_ASSERT_EQUAL_STRING(exp[i].m, c.meanings);
        TEST_ASSERT_EQUAL_STRING(exp[i].on, c.onyomi);
        TEST_ASSERT_EQUAL_STRING(exp[i].kun, c.kunyomi);
        TEST_ASSERT_EQUAL_UINT32(distOffs[i], c.distractorOff);
        TEST_ASSERT_EQUAL_UINT32(readOffs[i], c.distractorReadOff);
        TEST_ASSERT_EQUAL_UINT8(5, c.jlpt);
        TEST_ASSERT_EQUAL_UINT8(1, c.grade);
        TEST_ASSERT_EQUAL_UINT8(4, c.strokes);
        TEST_ASSERT_EQUAL_UINT8(0, c.flags);
        TEST_ASSERT_EQUAL_UINT16(exp[i].freq, c.freq);
        TEST_ASSERT_EQUAL_UINT16(exp[i].level, c.level);
        uint16_t d[4], total;
        TEST_ASSERT_EQUAL_UINT16(2, r.distractors(c, d, 4, &total));
        TEST_ASSERT_EQUAL_UINT16(exp[i].dis[0], d[0]);
        TEST_ASSERT_EQUAL_UINT16(exp[i].dis[1], d[1]);
        TEST_ASSERT_EQUAL_UINT16(exp[i].rdisN, r.distractors(c, d, 4, &total, true));
        TEST_ASSERT_EQUAL_UINT16(exp[i].rdisN, total);
        for (int k = 0; k < exp[i].rdisN; ++k) TEST_ASSERT_EQUAL_UINT16(exp[i].rdis[k], d[k]);
    }
    const Glyph* g = r.findGlyph(0, 26085);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(19, g->width);
    TEST_ASSERT_EQUAL_UINT8(26, g->height);
    TEST_ASSERT_EQUAL_INT8(6, g->xOff);
    TEST_ASSERT_EQUAL_INT8(-29, g->yOff);
    TEST_ASSERT_EQUAL_UINT8(32, g->advance);
    TEST_ASSERT_EQUAL_UINT32(2372, g->dataOff);
    g = r.findGlyph(0, 28779);
    TEST_ASSERT_EQUAL_UINT8(29, g->width);
    TEST_ASSERT_EQUAL_UINT32(2968, g->dataOff);
    g = r.findGlyph(1, 26085);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(11, g->width);
    TEST_ASSERT_EQUAL_UINT8(13, g->height);
    TEST_ASSERT_EQUAL_INT8(-15, g->yOff);
    TEST_ASSERT_EQUAL_UINT32(3403, g->dataOff);
    g = r.findGlyph(2, 'A');
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(9, g->width);
    TEST_ASSERT_EQUAL_UINT8(8, g->advance);
    uint8_t bm[64];
    TEST_ASSERT_TRUE(r.readGlyphBitmap(*g, bm, sizeof bm));
    const uint8_t want[] = {0x00, 0x03, 0xb0, 0x00, 0x00};  // first row of 'A' per mini.json
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, bm, 5);
    g = r.findGlyph(2, ' ');
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(0, g->width);
    TEST_ASSERT_EQUAL_UINT8(4, g->advance);
    g = r.findGlyph(2, 12491);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(6, g->height);
    TEST_ASSERT_EQUAL_UINT32(6563, g->dataOff);
    // Set 3: the same ニ rendered larger for reading answers.
    g = r.findGlyph(3, 12491);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(14, g->width);
    TEST_ASSERT_EQUAL_UINT8(9, g->height);
    TEST_ASSERT_EQUAL_UINT8(18, g->advance);
    TEST_ASSERT_EQUAL_UINT32(7829, g->dataOff);
    int cnt[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < r.glyphCount(); ++i) ++cnt[r.glyphAt(i)->set];
    TEST_ASSERT_EQUAL(3, cnt[0]);
    TEST_ASSERT_EQUAL(3, cnt[1]);
    TEST_ASSERT_EQUAL(109, cnt[2]);
    TEST_ASSERT_EQUAL(14, cnt[3]);

    // Stroke order section (KanjiVG excerpts for the three mini kanji).
    TEST_ASSERT_EQUAL_UINT32(7892, h.strokeOff);
    TEST_ASSERT_TRUE(r.hasStrokeOrder());
    uint16_t sc = 0, spx = 0;
    TEST_ASSERT_TRUE(r.strokeInfo(0, &sc, &spx));
    TEST_ASSERT_EQUAL_UINT16(4, sc);
    TEST_ASSERT_EQUAL_UINT16(32, spx);
    TEST_ASSERT_EQUAL_UINT16(4, r.strokeCount(2));
    Stroke st;
    TEST_ASSERT_TRUE(r.strokeAt(0, 0, &st));
    TEST_ASSERT_EQUAL_INT8(8, st.xOff);
    TEST_ASSERT_EQUAL_INT8(6, st.yOff);
    TEST_ASSERT_EQUAL_UINT8(3, st.w);
    TEST_ASSERT_EQUAL_UINT8(21, st.h);
    TEST_ASSERT_EQUAL_UINT32(8024, st.dataOff);
    TEST_ASSERT_EQUAL_UINT8(kMaskEncRaw, st.encoding);  // v1 masks are always raw
    TEST_ASSERT_EQUAL_UINT16(21, st.size);
    TEST_ASSERT_EQUAL_UINT32(21, PackReader::strokeBitmapSize(st));  // ceil(3/8)*21
    uint8_t sb[64];
    TEST_ASSERT_TRUE(r.readStrokeBitmap(st, sb, sizeof sb));
    TEST_ASSERT_EQUAL_HEX8(0xC0, sb[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC0, sb[1]);
    TEST_ASSERT_EQUAL_HEX8(0x20, sb[2]);
    TEST_ASSERT_FALSE(r.strokeAt(0, 4, &st));  // 日 has exactly 4 strokes

    // Quiz on a 3-card pack: 3 options max.
    XorShiftRng rng(3);
    Question q;
    TEST_ASSERT_TRUE(buildQuestion(r, 0, QuizMode::Meaning, rng, &q));
    TEST_ASSERT_EQUAL(3, q.count);
    TEST_ASSERT_TRUE(isMeaningOf(q.options[q.correct], "day; sun; Japan"));
    TEST_ASSERT_TRUE(buildQuestion(r, 1, QuizMode::Reading, rng, &q));
    TEST_ASSERT_EQUAL_STRING("ゲツ", q.options[q.correct].c_str());
}

void test_real_pack_if_present() {
    StdioSource src("../tools/pack-builder/out/n5-en.tgc");
    if (!src.ok()) TEST_IGNORE_MESSAGE("n5-en.tgc not built");
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), gPackMem));
    XorShiftRng rng(42);
    int made = 0;
    for (uint32_t i = 0; i < r.cardCount(); ++i) {
        Card c;
        r.card(i, &c);
        TEST_ASSERT_NOT_NULL(r.findGlyph(0, c.codepoint));
        TEST_ASSERT_NOT_NULL(r.findGlyph(1, c.codepoint));
        Question q;
        TEST_ASSERT_TRUE(buildQuestion(r, i, QuizMode::Meaning, rng, &q));
        TEST_ASSERT_EQUAL(4, q.count);
        if (buildQuestion(r, i, QuizMode::Reading, rng, &q)) {
            TEST_ASSERT_EQUAL(4, q.count);
            ++made;
        }
    }
    TEST_ASSERT_TRUE(made > 0);
}

// ------------------------------------------ streamed tables (PackMemory)

// An allocator with a byte budget that counts what is live, so a test can
// see what a reader holds (memoryBytes()) and that close() returns it all.
size_t gBudget = static_cast<size_t>(-1);
size_t gLive = 0;
void* budgetAlloc(size_t n) {
    if (gLive + n > gBudget) return nullptr;
    auto* p = static_cast<size_t*>(std::malloc(n + 16));
    if (!p) return nullptr;
    *p = n;
    gLive += n;
    return reinterpret_cast<char*>(p) + 16;
}
void budgetFree(void* q) {
    if (!q) return;
    auto* p = reinterpret_cast<size_t*>(static_cast<char*>(q) - 16);
    gLive -= *p;
    std::free(p);
}
Allocator budgetAllocator() { return Allocator{budgetAlloc, budgetFree}; }

void assertSameGlyph(const Glyph* a, const Glyph* b) {
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(a->codepoint, b->codepoint);
    TEST_ASSERT_EQUAL_UINT8(a->set, b->set);
    TEST_ASSERT_EQUAL_UINT8(a->encoding, b->encoding);
    TEST_ASSERT_EQUAL_UINT8(a->width, b->width);
    TEST_ASSERT_EQUAL_UINT8(a->height, b->height);
    TEST_ASSERT_EQUAL_INT8(a->xOff, b->xOff);
    TEST_ASSERT_EQUAL_INT8(a->yOff, b->yOff);
    TEST_ASSERT_EQUAL_UINT8(a->advance, b->advance);
    TEST_ASSERT_EQUAL_UINT32(a->dataOff, b->dataOff);
}

// Opens `src` both ways and checks that the streamed reader answers exactly as
// the in-memory one: every card (its strings too), every glyph by index and by
// lookup (and the absent keys next to each), every bitmap and stroke mask, and
// every question built from the same seeds.
void checkModesAgree(PackSource& src) {
    PackReader mem, str;
    TEST_ASSERT_EQUAL(PackError::Ok, mem.open(src, defaultAllocator(), PackMemory::InMemory));
    TEST_ASSERT_EQUAL(PackError::Ok, str.open(src, defaultAllocator(), PackMemory::Streamed));
    TEST_ASSERT_FALSE(mem.streamed());
    TEST_ASSERT_TRUE(str.streamed());
    TEST_ASSERT_EQUAL_UINT32(mem.cardCount(), str.cardCount());
    TEST_ASSERT_EQUAL_UINT32(mem.glyphCount(), str.glyphCount());
    TEST_ASSERT_EQUAL_STRING(mem.deckName(), str.deckName());

    for (uint32_t i = 0; i < mem.cardCount(); ++i) {
        Card a, b;
        TEST_ASSERT_TRUE(mem.card(i, &a));
        TEST_ASSERT_TRUE(str.card(i, &b));
        TEST_ASSERT_EQUAL_UINT32(a.codepoint, b.codepoint);
        TEST_ASSERT_EQUAL_STRING(a.meanings, b.meanings);
        TEST_ASSERT_EQUAL_STRING(a.onyomi, b.onyomi);
        TEST_ASSERT_EQUAL_STRING(a.kunyomi, b.kunyomi);
        TEST_ASSERT_EQUAL_UINT32(a.distractorOff, b.distractorOff);
        TEST_ASSERT_EQUAL_UINT32(a.distractorReadOff, b.distractorReadOff);
        TEST_ASSERT_EQUAL_UINT16(a.level, b.level);
        TEST_ASSERT_EQUAL_UINT16(a.freq, b.freq);
        TEST_ASSERT_EQUAL_UINT8(a.jlpt, b.jlpt);
        TEST_ASSERT_EQUAL_UINT8(a.strokes, b.strokes);
        TEST_ASSERT_EQUAL_UINT32(a.codepoint, str.codepointAt(i));
        TEST_ASSERT_EQUAL_UINT32(a.codepoint, mem.codepointAt(i));
        // Without text: the numbers, no strings, in both modes.
        TEST_ASSERT_TRUE(str.card(i, &b, false));
        TEST_ASSERT_TRUE(mem.card(i, &a, false));
        TEST_ASSERT_EQUAL_UINT32(a.codepoint, b.codepoint);
        TEST_ASSERT_EQUAL_UINT16(a.level, b.level);
        TEST_ASSERT_EQUAL_STRING("", b.meanings);
        TEST_ASSERT_EQUAL_STRING("", a.kunyomi);
        // Example words (v3).
        TEST_ASSERT_EQUAL(mem.hasExampleWords(), str.hasExampleWords());
        TEST_ASSERT_EQUAL_UINT32(mem.exampleWordCount(i), str.exampleWordCount(i));
        {
            ExampleWord wa[4], wb[4];
            const uint32_t na = mem.exampleWords(i, wa, 4);
            TEST_ASSERT_EQUAL_UINT32(na, str.exampleWords(i, wb, 4));
            for (uint32_t k = 0; k < na; ++k) {
                TEST_ASSERT_EQUAL_STRING(wa[k].word, wb[k].word);
                TEST_ASSERT_EQUAL_STRING(wa[k].reading, wb[k].reading);
                TEST_ASSERT_EQUAL_STRING(wa[k].gloss, wb[k].gloss);
                TEST_ASSERT_EQUAL_UINT8(wa[k].kind, wb[k].kind);
                TEST_ASSERT_EQUAL_UINT8(wa[k].readingStart, wb[k].readingStart);
                TEST_ASSERT_EQUAL_UINT8(wa[k].readingLen, wb[k].readingLen);
            }
        }
        // Strokes.
        TEST_ASSERT_EQUAL_UINT16(mem.strokeCount(i), str.strokeCount(i));
        for (uint16_t k = 0; k < mem.strokeCount(i); ++k) {
            Stroke sa, sb;
            TEST_ASSERT_TRUE(mem.strokeAt(i, k, &sa));
            TEST_ASSERT_TRUE(str.strokeAt(i, k, &sb));
            TEST_ASSERT_EQUAL_UINT32(sa.dataOff, sb.dataOff);
            std::vector<uint8_t> ba(PackReader::strokeBitmapSize(sa)), bb(ba.size());
            TEST_ASSERT_TRUE(mem.readStrokeBitmap(sa, ba.data(), ba.size()));
            TEST_ASSERT_TRUE(str.readStrokeBitmap(sb, bb.data(), bb.size()));
            TEST_ASSERT_TRUE(ba == bb);
        }
    }
    TEST_ASSERT_FALSE(str.card(str.cardCount(), nullptr));
    TEST_ASSERT_EQUAL_UINT32(0, str.codepointAt(str.cardCount()));

    for (uint32_t i = 0; i < mem.glyphCount(); ++i) {
        const Glyph* a = mem.glyphAt(i);
        const Glyph* b = str.glyphAt(i);
        assertSameGlyph(a, b);
        TEST_ASSERT_EQUAL_UINT32(i, str.glyphIndex(b));
        const Glyph* f = str.findGlyph(a->set, a->codepoint);
        assertSameGlyph(a, f);
        TEST_ASSERT_EQUAL_UINT32(i, str.glyphIndex(f));
        TEST_ASSERT_EQUAL_UINT32(i, mem.glyphIndex(mem.findGlyph(a->set, a->codepoint)));
        // The keys around it are found (or not) alike.
        for (uint32_t cp : {a->codepoint - 1, a->codepoint + 1}) {
            const Glyph* x = mem.findGlyph(a->set, cp);
            const Glyph* y = str.findGlyph(a->set, cp);
            TEST_ASSERT_EQUAL(x != nullptr, y != nullptr);
            if (x) assertSameGlyph(x, y);
        }
        std::vector<uint8_t> ba(PackReader::bitmapSize(*a)), bb(ba.size());
        TEST_ASSERT_TRUE(mem.readGlyphBitmap(*a, ba.data(), ba.size()));
        TEST_ASSERT_TRUE(str.readGlyphBitmap(*b, bb.data(), bb.size()));
        TEST_ASSERT_TRUE(ba == bb);
    }
    TEST_ASSERT_NULL(str.glyphAt(str.glyphCount()));
    TEST_ASSERT_NULL(str.findGlyph(0, 0));     // before the first entry
    TEST_ASSERT_NULL(str.findGlyph(9, 0x41));  // after the last set
    Glyph stray{};
    TEST_ASSERT_EQUAL_UINT32(str.glyphCount(), str.glyphIndex(&stray));
    TEST_ASSERT_EQUAL_UINT32(mem.glyphCount(), mem.glyphIndex(&stray));

    for (uint32_t i = 0; i < mem.cardCount(); ++i)
        for (QuizMode m : {QuizMode::Meaning, QuizMode::Reading, QuizMode::MeaningToKanji}) {
            XorShiftRng ra(1000 + i), rb(1000 + i);
            Question qa, qb;
            const bool oa = buildQuestion(mem, i, m, ra, &qa);
            TEST_ASSERT_EQUAL(oa, buildQuestion(str, i, m, rb, &qb));
            if (!oa) continue;
            TEST_ASSERT_EQUAL(qa.count, qb.count);
            TEST_ASSERT_EQUAL(qa.correct, qb.correct);
            for (int k = 0; k < qa.count; ++k) {
                TEST_ASSERT_EQUAL_STRING(qa.options[k].c_str(), qb.options[k].c_str());
                TEST_ASSERT_EQUAL_UINT16(qa.optionCard[k], qb.optionCard[k]);
            }
        }
}

void test_streamed_matches_in_memory() {
    {
        auto buf = makePack(sixCards(), someGlyphs(), someStrokes());
        MemorySource src(buf);
        checkModesAgree(src);
    }
    {
        auto g = someGlyphs();
        auto buf = makePack(sixCards(), g, someStrokes(), 2);
        MemorySource src(buf);
        checkModesAgree(src);
    }
    StdioSource v1(kMiniV1);
    if (v1.ok()) checkModesAgree(v1);
    StdioSource v2("../tools/pack-builder/fixtures/mini.tgc");
    if (v2.ok()) checkModesAgree(v2);
    {
        const CardWords words = sixWords();
        auto buf = makePack(sixCards(), someGlyphs(), someStrokes(), 2, &words);
        MemorySource src(buf);
        checkModesAgree(src);
    }
    StdioSource v3("../tools/pack-builder/fixtures/mini_words.tgc");
    if (v3.ok()) checkModesAgree(v3);
}

// The published decks, when a directory with them is given in $TGC_PACK_DIR
// (not committed; e.g. the decks-v2 release downloaded somewhere): every pack
// in it is read both ways.
void test_streamed_matches_real_packs() {
    const char* dir = std::getenv("TGC_PACK_DIR");
    if (!dir || !*dir) TEST_IGNORE_MESSAGE("set TGC_PACK_DIR to a directory of .tgc packs");
    int checked = 0;
    for (const char* name : {"n5-en", "n5-es", "n4-en", "n4-es", "n3-en", "n3-es", "n2-en",
                             "n2-es", "n1-en", "n1-es", "joyo-en", "joyo-es"}) {
        const std::string path = std::string(dir) + "/" + name + ".tgc";
        StdioSource src(path.c_str());
        if (!src.ok()) continue;
        checkModesAgree(src);
        ++checked;
        // What each mode holds (64-bit host sizes; docs/architecture.md has the
        // ESP32's), after a pass over every card's text.
        PackReader mem, str;
        mem.open(src, defaultAllocator(), PackMemory::InMemory);
        str.open(src, defaultAllocator(), PackMemory::Streamed);
        for (uint32_t i = 0; i < str.cardCount(); ++i) {
            Card c;
            str.card(i, &c);
        }
        char msg[96];
        snprintf(msg, sizeof msg, "%s: in memory %u bytes, streamed %u bytes", name,
                 (unsigned)mem.memoryBytes(), (unsigned)str.memoryBytes());
        TEST_MESSAGE(msg);
    }
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, "no packs in TGC_PACK_DIR");
}

// A big deck: the streamed reader holds a few KB plus 4 bytes a card, the
// in-memory one the whole tables; memoryBytes() says exactly what is live and
// close() gives it all back.
std::vector<CardSpec> manyCards(int n, std::vector<std::string>* text) {
    text->clear();
    text->reserve(3 * static_cast<size_t>(n));
    std::vector<CardSpec> cs;
    for (int i = 0; i < n; ++i) {
        // Meanings of 20 to 200 bytes: some need several reads of a string.
        std::string m = "meaning " + std::to_string(i);
        while (m.size() < static_cast<size_t>(20 + (i * 37) % 180)) m += "; more";
        text->push_back(m);
        text->push_back("オン" + std::to_string(i % 7));
        text->push_back(i % 3 ? "くん" : "");
    }
    for (int i = 0; i < n; ++i) {
        const std::string* t = &(*text)[3 * static_cast<size_t>(i)];
        cs.push_back({static_cast<uint32_t>(0x4E00 + i), t[0].c_str(), t[1].c_str(), t[2].c_str(),
                      {static_cast<uint16_t>((i + 1) % n), static_cast<uint16_t>((i + 2) % n)},
                      {}});
    }
    return cs;
}

std::vector<GlyphSpec> glyphsFor(const std::vector<CardSpec>& cs) {
    std::vector<GlyphSpec> g;
    for (const CardSpec& c : cs) {
        g.push_back({0, c.cp, 6, 6});
        g.push_back({1, c.cp, 4, 4});
    }
    for (uint32_t cp = 0x20; cp < 0x7F; ++cp) g.push_back({2, cp, 3, 5});
    return g;
}

void test_streamed_memory_is_small_and_released() {
    std::vector<std::string> text;
    const auto cs = manyCards(2000, &text);
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    gBudget = static_cast<size_t>(-1);
    size_t inMemory = 0;
    {
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, budgetAllocator(), PackMemory::InMemory));
        TEST_ASSERT_EQUAL(gLive, r.memoryBytes());
        inMemory = r.memoryBytes();
        r.close();
        TEST_ASSERT_EQUAL(0, gLive);
    }
    {
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, budgetAllocator(), PackMemory::Streamed));
        TEST_ASSERT_EQUAL(gLive, r.memoryBytes());
        // Read every string: the slots grow to the longest they held, no more.
        for (uint32_t i = 0; i < r.cardCount(); ++i) {
            Card c;
            TEST_ASSERT_TRUE(r.card(i, &c));
            TEST_ASSERT_EQUAL_STRING(text[3 * i].c_str(), c.meanings);
            TEST_ASSERT_EQUAL_STRING(text[3 * i + 1].c_str(), c.onyomi);
            TEST_ASSERT_EQUAL_STRING(text[3 * i + 2].c_str(), c.kunyomi);
        }
        TEST_ASSERT_EQUAL(gLive, r.memoryBytes());
        TEST_ASSERT_TRUE(r.memoryBytes() < 8 * 1024 + 4 * 2000 + 16 * 256);
        TEST_ASSERT_TRUE(r.memoryBytes() * 8 < inMemory);
    }
    TEST_ASSERT_EQUAL(0, gLive);
}

// InMemoryOrStreamed falls back when the tables do not fit; InMemory fails.
void test_in_memory_or_streamed_falls_back() {
    std::vector<std::string> text;
    const auto cs = manyCards(1000, &text);
    auto buf = makePack(cs, glyphsFor(cs));
    MemorySource src(buf);
    gBudget = 32 * 1024;  // under the tables (~180 KB), over the streamed state
    {
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::NoMemory, r.open(src, budgetAllocator(), PackMemory::InMemory));
        TEST_ASSERT_FALSE(r.isOpen());
        TEST_ASSERT_EQUAL(0, gLive);
        TEST_ASSERT_EQUAL(PackError::Ok,
                          r.open(src, budgetAllocator(), PackMemory::InMemoryOrStreamed));
        TEST_ASSERT_TRUE(r.streamed());
        Card c;
        TEST_ASSERT_TRUE(r.card(999, &c));
        TEST_ASSERT_EQUAL_STRING(text[3 * 999].c_str(), c.meanings);
        TEST_ASSERT_NOT_NULL(r.findGlyph(1, c.codepoint));
    }
    TEST_ASSERT_EQUAL(0, gLive);
    // Plenty of memory: the tables.
    gBudget = static_cast<size_t>(-1);
    {
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::Ok,
                          r.open(src, budgetAllocator(), PackMemory::InMemoryOrStreamed));
        TEST_ASSERT_FALSE(r.streamed());
    }
    // Not even the streamed state fits.
    gBudget = 1024;
    {
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::NoMemory,
                          r.open(src, budgetAllocator(), PackMemory::InMemoryOrStreamed));
        TEST_ASSERT_EQUAL(0, gLive);
    }
    gBudget = static_cast<size_t>(-1);
}

// A string pointer stays valid while fewer than kStringSlots other strings
// are read; the pool's last byte ends a string as in memory; corrupt packs
// are refused the same way.
void test_streamed_string_cache_and_pool_end() {
    std::vector<std::string> text;
    const auto cs = manyCards(40, &text);
    auto buf = makePack(cs, glyphsFor(cs));
    {
        MemorySource src(buf);
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(src, defaultAllocator(), PackMemory::Streamed));
        Card first;
        TEST_ASSERT_TRUE(r.card(0, &first));
        const char* kept = first.meanings;
        // Its own two readings and four more cards with text: 14 other
        // strings, fewer than the slots, so the meaning (the least recent
        // string) is still there.
        for (uint32_t i = 1; i <= 4; ++i) {
            Card c;
            TEST_ASSERT_TRUE(r.card(i, &c));
        }
        TEST_ASSERT_EQUAL_STRING(text[0].c_str(), kept);
        TEST_ASSERT_EQUAL_STRING("Test Deck", r.deckName());
        TEST_ASSERT_EQUAL_STRING("", r.str(0));                     // before the pool
        TEST_ASSERT_EQUAL_STRING("", r.str(r.header().stringsOff + r.header().stringsSize));
    }
    // The pool cut in the middle of the last card's meaning: its last byte
    // becomes the string's end in both modes.
    {
        std::vector<uint8_t> cut = buf;
        MemorySource probe(cut);
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::Ok, r.open(probe));
        Card last;
        TEST_ASSERT_TRUE(r.card(39, &last));
        const uint32_t meaningOff = static_cast<uint32_t>(cut[64 + 39 * 32 + 4]) |
                                    static_cast<uint32_t>(cut[64 + 39 * 32 + 5]) << 8 |
                                    static_cast<uint32_t>(cut[64 + 39 * 32 + 6]) << 16 |
                                    static_cast<uint32_t>(cut[64 + 39 * 32 + 7]) << 24;
        const uint32_t size = meaningOff + 11 - r.header().stringsOff;  // 10 bytes + the cut
        r.close();
        // Every later card string must stay inside: drop the last card's
        // readings' offsets onto its meaning.
        for (int f = 8; f <= 12; f += 4)
            for (int k = 0; k < 4; ++k) cut[64 + 39 * 32 + f + k] = cut[64 + 39 * 32 + 4 + k];
        for (int k = 0; k < 4; ++k) cut[32 + k] = static_cast<uint8_t>(size >> (8 * k));
        MemorySource src(cut);
        for (PackMemory m : {PackMemory::InMemory, PackMemory::Streamed}) {
            PackReader q;
            TEST_ASSERT_EQUAL(PackError::Ok, q.open(src, defaultAllocator(), m));
            Card c;
            TEST_ASSERT_TRUE(q.card(39, &c));
            TEST_ASSERT_EQUAL_STRING(text[3 * 39].substr(0, 10).c_str(), c.meanings);
        }
    }
    // A bad card offset is refused streamed as well.
    {
        std::vector<uint8_t> bad = buf;
        bad[64 + 5 * 32 + 4] = 0;  // a meaning offset before the pool
        bad[64 + 5 * 32 + 5] = 0;
        bad[64 + 5 * 32 + 6] = 0;
        bad[64 + 5 * 32 + 7] = 0;
        MemorySource src(bad);
        PackReader r;
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(src, defaultAllocator(), PackMemory::Streamed));
    }
}

void setUp() {}
void tearDown() {}

// The UI-language preference: Auto follows the pack, English/Spanish force the
// table, and anything unknown falls back to Auto. Pure (ui/UiLang.h).
void test_ui_lang_code() {
    using tgc::uiLangCodeFor;
    using tgc::kUiLangAuto;
    using tgc::kUiLangEnglish;
    using tgc::kUiLangSpanish;

    // Auto reproduces today's pack-driven behaviour.
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(kUiLangAuto, "es"));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(kUiLangAuto, "en"));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(kUiLangAuto, "fr"));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(kUiLangAuto, ""));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(kUiLangAuto, nullptr));

    // Explicit choices ignore the pack.
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(kUiLangEnglish, "es"));
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(kUiLangSpanish, "en"));
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(kUiLangSpanish, ""));
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(kUiLangSpanish, nullptr));

    // Unknown values behave like Auto.
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(99, "es"));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(-1, "fr"));
}

// The first-boot language picker: only with no stored preference and no deck;
// its rows map to explicit preferences (English first and preselected), never
// Auto, so the choice survives a deck in the other language. Pure (ui/UiLang.h).
void test_first_boot_lang_picker() {
    using tgc::firstBootLangPicker;
    using tgc::firstBootLangPref;
    using tgc::uiLangCodeFor;

    TEST_ASSERT_TRUE(firstBootLangPicker(false, false));   // brand-new or erased device
    TEST_ASSERT_FALSE(firstBootLangPicker(false, true));   // NVS erased, decks kept: Auto
    TEST_ASSERT_FALSE(firstBootLangPicker(true, false));   // chosen before (Auto too), decks gone
    TEST_ASSERT_FALSE(firstBootLangPicker(true, true));    // an ordinary boot

    TEST_ASSERT_EQUAL(2, tgc::kFirstBootLangRows);
    TEST_ASSERT_EQUAL(tgc::kUiLangEnglish, firstBootLangPref(tgc::kFirstBootLangDefaultRow));
    TEST_ASSERT_EQUAL(tgc::kUiLangEnglish, firstBootLangPref(0));
    TEST_ASSERT_EQUAL(tgc::kUiLangSpanish, firstBootLangPref(1));
    TEST_ASSERT_EQUAL(tgc::kUiLangEnglish, firstBootLangPref(-1));
    TEST_ASSERT_EQUAL(tgc::kUiLangEnglish, firstBootLangPref(2));
    for (int row = 0; row < tgc::kFirstBootLangRows; ++row)
        TEST_ASSERT_NOT_EQUAL(tgc::kUiLangAuto, firstBootLangPref(row));

    // Before any deck, and after a deck in the other language.
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(firstBootLangPref(1), nullptr));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(firstBootLangPref(0), nullptr));
    TEST_ASSERT_EQUAL_STRING("es", uiLangCodeFor(firstBootLangPref(1), "en"));
    TEST_ASSERT_EQUAL_STRING("en", uiLangCodeFor(firstBootLangPref(0), "es"));
}

// Critical battery: three low readings trigger the sleep, only a clearly fine
// reading or external power resets the count, and a wake after a critical sleep
// needs a margin above the trigger. Pure (power/BatteryGuard.h).
void test_battery_guard() {
    using tgc::BatteryGuard;

    // Three readings at or below 3 % trigger, not fewer.
    BatteryGuard g;
    TEST_ASSERT_FALSE(g.update(3, false));
    TEST_ASSERT_FALSE(g.update(2, false));
    TEST_ASSERT_TRUE(g.update(3, false));
    TEST_ASSERT_TRUE(g.update(0, false));  // stays triggered while low

    // Noise just above the threshold (4-5 %) does not reset the count...
    BatteryGuard wobble;
    TEST_ASSERT_FALSE(wobble.update(3, false));
    TEST_ASSERT_FALSE(wobble.update(4, false));
    TEST_ASSERT_FALSE(wobble.update(3, false));
    TEST_ASSERT_FALSE(wobble.update(5, false));
    TEST_ASSERT_TRUE(wobble.update(2, false));

    // ...but a reading at 6 % or more does.
    BatteryGuard fine;
    fine.update(3, false);
    fine.update(3, false);
    TEST_ASSERT_FALSE(fine.update(6, false));
    TEST_ASSERT_EQUAL_UINT8(0, fine.lowReadings());
    TEST_ASSERT_FALSE(fine.update(3, false));
    TEST_ASSERT_FALSE(fine.update(3, false));

    // USB/charging is never critical and resets the count; unknown is ignored.
    BatteryGuard usb;
    usb.update(1, false);
    usb.update(1, false);
    TEST_ASSERT_FALSE(usb.update(1, true));
    TEST_ASSERT_EQUAL_UINT8(0, usb.lowReadings());
    BatteryGuard unknown;
    for (int i = 0; i < 10; ++i) TEST_ASSERT_FALSE(unknown.update(-1, false));
    unknown.update(2, false);
    TEST_ASSERT_FALSE(unknown.update(-1, false));  // neither counts nor resets
    TEST_ASSERT_EQUAL_UINT8(1, unknown.lowReadings());

    // A full battery never triggers.
    BatteryGuard full;
    for (int i = 0; i < 10; ++i) TEST_ASSERT_FALSE(full.update(100, false));

    // Waking after a critical sleep: hysteresis up to 6 %, USB always resumes,
    // an unknown level does not lock the user out.
    TEST_ASSERT_FALSE(BatteryGuard::canResume(3, false));
    TEST_ASSERT_FALSE(BatteryGuard::canResume(5, false));
    TEST_ASSERT_TRUE(BatteryGuard::canResume(6, false));
    TEST_ASSERT_TRUE(BatteryGuard::canResume(0, true));
    TEST_ASSERT_TRUE(BatteryGuard::canResume(-1, false));
}

// The lines wrapText() makes of `s`, as strings.
static std::vector<std::string> wrapped(const char* s, int maxChars) {
    std::vector<std::string> out;
    for (const tgc::TextSpan& l : tgc::wrapText(s, maxChars)) out.emplace_back(s + l.begin, l.len);
    return out;
}

static void assertLines(const std::vector<std::string>& want, const std::vector<std::string>& got) {
    TEST_ASSERT_EQUAL_UINT(want.size(), got.size());
    for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}

// Wrapping for the built-in font's text screens (ui/TextLayout.h): spaces
// break, no-break spaces do not, a long word breaks after '/' or '-', and the
// width counts code points, not bytes.
void test_text_wrap() {
    TEST_ASSERT_EQUAL_UINT(0, tgc::wrapText("", 10).size());
    TEST_ASSERT_EQUAL_UINT(0, tgc::wrapText("   ", 10).size());
    TEST_ASSERT_EQUAL_UINT(0, tgc::wrapText(nullptr, 10).size());
    TEST_ASSERT_EQUAL_UINT(0, tgc::wrapText("abc", 0).size());
    assertLines({"abc"}, wrapped("abc", 10));
    assertLines({"abc"}, wrapped("  abc  ", 3));
    // A line of exactly maxChars; the space after it is the break.
    assertLines({"Stroke order: KanjiVG", "(c) Ulrich Apel,"},
                wrapped("Stroke order: KanjiVG (c) Ulrich Apel,", 21));
    assertLines({"a b", "c"}, wrapped("a b c", 4));
    assertLines({"one", "two"}, wrapped("one    two", 5));
    // U+00A0 keeps "CC BY-SA 4.0" together.
    assertLines({"(c) EDRDG,", "CC\xC2\xA0" "BY-SA\xC2\xA0" "4.0"},
                wrapped("(c) EDRDG, CC\xC2\xA0" "BY-SA\xC2\xA0" "4.0", 21));
    // ... unless it is longer than a line: then after its '-'.
    assertLines({"CC\xC2\xA0" "BY-", "SA\xC2\xA0" "4.0"}, wrapped("CC\xC2\xA0" "BY-SA\xC2\xA0" "4.0", 8));
    // A URL breaks after its last '/' that fits.
    assertLines({"github.com/dcarnicer/", "tamagotcho"}, wrapped("github.com/dcarnicer/tamagotcho", 21));
    assertLines({"github.com/", "dcarnicer/", "tamagotcho"},
                wrapped("github.com/dcarnicer/tamagotcho", 12));
    assertLines({"github.com/dcarnicer/tamagotcho"}, wrapped("github.com/dcarnicer/tamagotcho", 39));
    // A version string breaks after a '-'.
    assertLines({"v0.1.0-12-gabcdef0-", "dirty"}, wrapped("v0.1.0-12-gabcdef0-dirty", 21));
    // Nothing to break at: cut at the limit.
    assertLines({"abcd", "efgh", "ij"}, wrapped("abcdefghij", 4));
    // Code points, not bytes: "más" is 3 wide.
    assertLines({"más", "más"}, wrapped("más más", 3));
    assertLines({"toque: más"}, wrapped("toque: más", 10));
}

// Paging (ui/TextLayout.h): groups stay together when they fit a page, gaps
// only between blocks on one page, oversized groups and blocks still land.
void test_text_pages() {
    using tgc::PageBlock;
    using tgc::PageSlot;
    using tgc::layoutPages;
    using tgc::pageCount;

    TEST_ASSERT_EQUAL(1, pageCount(layoutPages({}, 100, 3, 8)));

    // Everything fits: one page, gap 3 inside a group, 8 between groups.
    auto s = layoutPages({{28, true}, {10, false}, {10, true}, {20, false}}, 100, 3, 8);
    TEST_ASSERT_EQUAL(1, pageCount(s));
    TEST_ASSERT_EQUAL(0, s[0].y);
    TEST_ASSERT_EQUAL(31, s[1].y);
    TEST_ASSERT_EQUAL(49, s[2].y);
    TEST_ASSERT_EQUAL(62, s[3].y);

    // The second group (10 + 3 + 50 = 63) does not fit the rest of the page
    // (41 + 8 + 63 > 100) but fits an empty one: it moves whole.
    s = layoutPages({{28, true}, {10, false}, {10, true}, {50, false}}, 100, 3, 8);
    TEST_ASSERT_EQUAL(2, pageCount(s));
    TEST_ASSERT_EQUAL(0, s[1].page);
    TEST_ASSERT_EQUAL(1, s[2].page);
    TEST_ASSERT_EQUAL(0, s[2].y);
    TEST_ASSERT_EQUAL(13, s[3].y);

    // Exactly full is still a fit.
    s = layoutPages({{50, true}, {42, true}}, 100, 3, 8);
    TEST_ASSERT_EQUAL(1, pageCount(s));
    TEST_ASSERT_EQUAL(58, s[1].y);

    // A group taller than a page fills the current page block by block.
    s = layoutPages({{30, true}, {40, true}, {40, false}, {40, false}}, 100, 3, 8);
    TEST_ASSERT_EQUAL(0, s[1].page);
    TEST_ASSERT_EQUAL(38, s[1].y);
    TEST_ASSERT_EQUAL(1, s[2].page);
    TEST_ASSERT_EQUAL(0, s[2].y);
    TEST_ASSERT_EQUAL(1, s[3].page);
    TEST_ASSERT_EQUAL(43, s[3].y);

    // A block taller than a page gets one of its own; the next one follows.
    s = layoutPages({{10, true}, {150, true}, {10, true}}, 100, 3, 8);
    TEST_ASSERT_EQUAL(3, pageCount(s));
    TEST_ASSERT_EQUAL(1, s[1].page);
    TEST_ASSERT_EQUAL(0, s[1].y);
    TEST_ASSERT_EQUAL(2, s[2].page);
}

// The pack and quiz tests: main() runs them once per PackMemory mode.
void runPackTests() {
    RUN_TEST(test_open_and_header);
    RUN_TEST(test_cards);
    RUN_TEST(test_reading_distractor_list);
    RUN_TEST(test_glyph_lookup_and_bitmap);
    RUN_TEST(test_rejects_bad_packs);
    RUN_TEST(test_rejects_unsupported_glyph_encoding);
    RUN_TEST(test_v2_pack_decodes_glyphs_and_masks);
    RUN_TEST(test_v2_rejects_bad_masks_and_glyphs);
    RUN_TEST(test_stroke_section_read);
    RUN_TEST(test_rejects_bad_stroke_section);
    RUN_TEST(test_v3_example_words);
    RUN_TEST(test_no_example_words_before_v3);
    RUN_TEST(test_v3_rejects_bad_words);
    RUN_TEST(test_mini_words_fixture);
    RUN_TEST(test_quiz_text_helpers);
    RUN_TEST(test_quiz_meaning);
    RUN_TEST(test_quiz_meaning_to_kanji);
    RUN_TEST(test_quiz_distractor_priority_and_shuffle);
    RUN_TEST(test_quiz_reading_kinds);
    RUN_TEST(test_quiz_reading_excludes_target_readings);
    RUN_TEST(test_quiz_relaxed_pass_excludes_target_readings_of_both_kinds);
    RUN_TEST(test_quiz_sparse_kun_fallback_and_options);
    RUN_TEST(test_quiz_reading_turn);
    RUN_TEST(test_quiz_sparse_kun_alternation);
    RUN_TEST(test_quiz_on_kana);
    RUN_TEST(test_quiz_options_differ_in_either_script);
    RUN_TEST(test_quiz_unavailable_and_tiny);
    RUN_TEST(test_quiz_meaning_gloss_collisions);
    RUN_TEST(test_quiz_meaning_to_kanji_excludes_cards_with_the_prompt);
    RUN_TEST(test_mini_v1_fixture);
    RUN_TEST(test_real_pack_if_present);
}

int main() {
    UNITY_BEGIN();
    for (PackMemory m : {PackMemory::InMemory, PackMemory::Streamed}) {
        gPackMem = m;
        runPackTests();
    }
    gPackMem = PackMemory::InMemory;
    RUN_TEST(test_streamed_matches_in_memory);
    RUN_TEST(test_streamed_matches_real_packs);
    RUN_TEST(test_streamed_memory_is_small_and_released);
    RUN_TEST(test_in_memory_or_streamed_falls_back);
    RUN_TEST(test_streamed_string_cache_and_pool_end);
    RUN_TEST(test_glcd_cp437_map);
    RUN_TEST(test_utf8_encode);
    RUN_TEST(test_blend);
    RUN_TEST(test_ui_lang_code);
    RUN_TEST(test_first_boot_lang_picker);
    RUN_TEST(test_battery_guard);
    RUN_TEST(test_text_wrap);
    RUN_TEST(test_text_pages);
    return UNITY_END();
}
