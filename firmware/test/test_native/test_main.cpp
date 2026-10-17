#include <unity.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "pack/MemorySource.h"
#include "pack/PackReader.h"
#include "pack/StdioSource.h"
#include "pack/Utf8.h"
#include "quiz/Quiz.h"
#include "render/Blend565.h"
#include "render/GlcdCp437.h"
#include "ui/UiLang.h"

using namespace tgc;

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
};
struct StrokeSpec {  // one mask: position in the em square + size
    uint8_t x, y, w, h;
};

// Builds a valid v1 pack in memory. Glyph bitmaps are filled with a
// deterministic pattern: byte i = (cp + i) & 0xFF. `strokes` (optional, aligned
// with `cards`) appends a stroke section, mask bytes = (0xA0 + i) & 0xFF.
std::vector<uint8_t> makePack(const std::vector<CardSpec>& cards, std::vector<GlyphSpec> glyphs,
                              const std::vector<std::vector<StrokeSpec>>& strokes = {}) {
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
    while (pool.size() % 4) pool.push_back('\0');

    const uint32_t cardsOff = 64;
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

    B b;
    b.u8('T'); b.u8('N'); b.u8('G'); b.u8('C');
    b.u16(1); b.u16(64);
    b.u32(cards.size()); b.u32(glyphs.size());
    b.u32(cardsOff); b.u32(glyphsOff); b.u32(distOff); b.u32(strOff);
    b.u32(pool.size()); b.u32(gdOff); b.u32(deckOff);
    b.u8('e'); b.u8('n'); b.u8(0); b.u8(0);
    b.u8(32); b.u8(16); b.u8(12); b.u8(18); b.u8(14);
    b.u8(0); b.u8(0); b.u8(0);
    b.u32(0); b.u32(0);
    for (size_t i = 0; i < cards.size(); ++i) {
        b.u32(cards[i].cp); b.u32(cs[i].m); b.u32(cs[i].o); b.u32(cs[i].k); b.u32(dOffs[i]);
        b.u8(5); b.u8(1); b.u8(4); b.u8(0);
        b.u16(10 + i); b.u16(i); b.u32(drOffs[i]);
    }
    uint32_t dataOff = gdOff;
    for (auto& g : glyphs) {
        b.u32(g.cp); b.u8(g.set); b.u8(0); b.u8(g.w); b.u8(g.h);
        b.u8(static_cast<uint8_t>(-3)); b.u8(static_cast<uint8_t>(-g.h)); b.u8(g.w + 1); b.u8(0);
        b.u32(dataOff);
        dataOff += ((g.w + 1) / 2) * g.h;
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
        for (int i = 0; i < ((g.w + 1) / 2) * g.h; ++i) b.u8((g.cp + i) & 0xFF);

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
                cursor = (cursor + 3) & ~3u;
                maskOff[i].push_back(cursor);
                cursor += ((s.w + 7) / 8) * s.h;
            }
        for (uint32_t i = 0; i < n; ++i) b.u32(blockOff[i]);
        while (b.v.size() < ((strokeOff + 4 * n + 3) & ~3u)) b.u8(0);
        for (uint32_t i = 0; i < n; ++i) {
            if (strokes[i].empty()) continue;
            b.u16(static_cast<uint16_t>(strokes[i].size()));
            b.u16(32);  // px == header large_px
            b.u32(0);
            for (size_t k = 0; k < strokes[i].size(); ++k) {
                const StrokeSpec& s = strokes[i][k];
                b.u8(s.x); b.u8(s.y); b.u8(s.w); b.u8(s.h);
                b.u32(maskOff[i][k]);
            }
        }
        for (uint32_t i = 0; i < n; ++i)
            for (size_t k = 0; k < strokes[i].size(); ++k) {
                const StrokeSpec& s = strokes[i][k];
                while (b.v.size() < maskOff[i][k]) b.u8(0);
                for (int j = 0; j < ((s.w + 7) / 8) * s.h; ++j) b.u8((0xA0 + j) & 0xFF);
            }
        b.v[60] = strokeOff & 0xFF;
        b.v[61] = (strokeOff >> 8) & 0xFF;
        b.v[62] = (strokeOff >> 16) & 0xFF;
        b.v[63] = (strokeOff >> 24) & 0xFF;
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

}  // namespace

// ------------------------------------------------------------------- tests
void test_open_and_header() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
        TEST_ASSERT_EQUAL(PackError::BadMagic, r.open(s));
    }
    {
        auto b = good; b[4] = 2;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadVersion, r.open(s));
    }
    {
        auto b = good; b[16] = 0xFF; b[17] = 0xFF;  // cards_off way out
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadHeader, r.open(s));
    }
    {
        std::vector<uint8_t> b(good.begin(), good.begin() + 40);
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadHeader, r.open(s));
    }
    {
        std::vector<uint8_t> b(good.begin(), good.end() - 10);  // glyph data truncated
        MemorySource s(b);
        PackError e = r.open(s);
        TEST_ASSERT_TRUE(e == PackError::BadTable || e == PackError::BadHeader);
    }
    TEST_ASSERT_FALSE(r.isOpen());
}

void test_rejects_unsupported_glyph_encoding() {
    // Only encoding 0 (raw 4bpp) is defined. A pack declaring a future encoding
    // must be refused at open time, not later at draw time.
    auto buf = makePack(sixCards(), someGlyphs());
    const uint32_t glyphsOff = 64 + 6 * 32;  // header + card table
    buf[glyphsOff + 5] = 1;                   // first glyph entry's encoding byte
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::BadTable, r.open(src));
    TEST_ASSERT_FALSE(r.isOpen());
}

// ---------------------------------------------------------------- strokes
void test_stroke_section_read() {
    auto buf = makePack(sixCards(), someGlyphs(), someStrokes());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s));
    }
    {  // an index entry pointing out of bounds
        auto b = good;
        b[base] = static_cast<uint8_t>(b.size() - 2);
        b[base + 1] = b[base + 2] = b[base + 3] = 0;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s));
    }
    {  // a mask data_off past the end of the file
        auto b = good;
        uint32_t blk = rd32le(good, base);  // first card's block
        b[blk + 12] = 0xFF; b[blk + 13] = 0xFF; b[blk + 14] = 0xFF; b[blk + 15] = 0x7F;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s));
    }
    {  // a block px that disagrees with the header
        auto b = good;
        uint32_t blk = rd32le(good, base);
        b[blk + 2] = 99;
        MemorySource s(b);
        TEST_ASSERT_EQUAL(PackError::BadTable, r.open(s));
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
}

void test_quiz_meaning() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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

void test_quiz_distractor_priority_and_shuffle() {
    auto buf = makePack(sixCards(), someGlyphs());
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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

void test_quiz_unavailable_and_tiny() {
    std::vector<CardSpec> cs = {{0x65E5, "day", "", "", {}}, {0x6708, "month", "ゲツ", "", {}}};
    std::vector<GlyphSpec> gl = {{0, 0x65E5, 4, 4}, {1, 0x65E5, 4, 4}, {0, 0x6708, 4, 4}, {1, 0x6708, 4, 4}};
    auto buf = makePack(cs, gl);
    MemorySource src(buf);
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    // Font0 has no glyph for these (meaning text falls back to the pack font).
    TEST_ASSERT_EQUAL(-1, glcdIndex(0x00C1));  // Á
    TEST_ASSERT_EQUAL(-1, glcdIndex(0x6F22));  // 漢
}

void test_blend() {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, blend565(0xFFFF, 0x0000, 15));
    TEST_ASSERT_EQUAL_HEX16(0x0000, blend565(0xFFFF, 0x0000, 0));
    uint16_t mid = blend565(0xFFFF, 0x0000, 8);
    TEST_ASSERT_EQUAL_UINT16(17, (mid >> 11) & 0x1F);  // 31*8/15 = 16.5 -> 17
    TEST_ASSERT_EQUAL_HEX16(0xF800, blend565(0xF800, 0xF800, 7));
}

// ------------------------------------------------- real fixture (mini.tgc)
static const char* kMini = "../tools/pack-builder/fixtures/mini.tgc";

void test_mini_fixture() {
    StdioSource src(kMini);
    if (!src.ok()) TEST_IGNORE_MESSAGE("mini.tgc not found");
    TEST_ASSERT_EQUAL_UINT32(8288, src.size());
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_open_and_header);
    RUN_TEST(test_cards);
    RUN_TEST(test_reading_distractor_list);
    RUN_TEST(test_glyph_lookup_and_bitmap);
    RUN_TEST(test_rejects_bad_packs);
    RUN_TEST(test_rejects_unsupported_glyph_encoding);
    RUN_TEST(test_stroke_section_read);
    RUN_TEST(test_rejects_bad_stroke_section);
    RUN_TEST(test_quiz_text_helpers);
    RUN_TEST(test_quiz_meaning);
    RUN_TEST(test_quiz_meaning_to_kanji);
    RUN_TEST(test_quiz_distractor_priority_and_shuffle);
    RUN_TEST(test_quiz_reading_kinds);
    RUN_TEST(test_quiz_reading_excludes_target_readings);
    RUN_TEST(test_quiz_unavailable_and_tiny);
    RUN_TEST(test_quiz_meaning_gloss_collisions);
    RUN_TEST(test_glcd_cp437_map);
    RUN_TEST(test_blend);
    RUN_TEST(test_mini_fixture);
    RUN_TEST(test_real_pack_if_present);
    RUN_TEST(test_ui_lang_code);
    return UNITY_END();
}
