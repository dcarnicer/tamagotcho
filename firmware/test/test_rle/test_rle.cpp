// Native tests for the pack v2 decoders (src/pack/Rle.*) and the v2 mini.tgc.
// The vectors come from the Python encoder: tools/pack-builder/fixtures/
// rle_vectors.bin, written by `tangocho-pack fixture` (format in the builder's
// fixture.py): synthetic edge cases, every glyph and stroke mask of mini.tgc
// with its decoded bitmap, and malformed streams that must be rejected.
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "pack/MemorySource.h"
#include "pack/PackReader.h"
#include "pack/Rle.h"
#include "pack/StdioSource.h"

using namespace tgc;

namespace {

const char* kVectors = "../tools/pack-builder/fixtures/rle_vectors.bin";
const char* kMini = "../tools/pack-builder/fixtures/mini.tgc";

enum Source : uint8_t { kSynthetic = 0, kMiniGlyph = 1, kMiniMask = 2, kMalformed = 3 };
enum Kind : uint8_t { kGlyph = 0, kMask = 1 };

struct Vector {
    uint8_t source, kind, encoding, w, h;
    std::vector<uint8_t> stored, raw;
};

std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> v;
    if (std::FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Parses rle_vectors.bin; empty when the file is missing or malformed.
std::vector<Vector> loadVectors() {
    std::vector<Vector> out;
    const std::vector<uint8_t> f = readFile(kVectors);
    if (f.size() < 8 || std::memcmp(f.data(), "TRLV", 4) != 0 || rd16(&f[4]) != 1) return out;
    const uint16_t count = rd16(&f[6]);
    size_t pos = 8;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 10 > f.size()) return {};
        Vector v;
        v.source = f[pos];
        v.kind = f[pos + 1];
        v.encoding = f[pos + 2];
        v.w = f[pos + 3];
        v.h = f[pos + 4];
        const uint16_t ns = rd16(&f[pos + 6]), nr = rd16(&f[pos + 8]);
        pos += 10;
        if (pos + ns + nr > f.size()) return {};
        v.stored.assign(f.begin() + pos, f.begin() + pos + ns);
        v.raw.assign(f.begin() + pos + ns, f.begin() + pos + ns + nr);
        pos += ns + nr;
        out.push_back(std::move(v));
    }
    if (pos != f.size()) return {};
    return out;
}

size_t rawSize(const Vector& v) {
    return v.kind == kGlyph ? static_cast<size_t>((v.w + 1) / 2) * v.h
                            : static_cast<size_t>((v.w + 7) / 8) * v.h;
}

// Decodes a vector's stored bytes with the pure decoders (memory input).
bool decode(const Vector& v, std::vector<uint8_t>* out) {
    out->assign(rawSize(v) + 8, 0xEE);  // slack to catch writes past the bitmap
    ByteIn in(v.stored.data(), v.stored.size());
    if (v.kind == kGlyph) {
        if (v.encoding == kGlyphEncRaw) {
            std::memcpy(out->data(), v.stored.data(), v.stored.size());
            return v.stored.size() == rawSize(v);
        }
        return v.encoding == kGlyphEncRle && decodeGlyphRle(in, v.w, v.h, out->data(), rawSize(v));
    }
    if (v.encoding == kMaskEncRaw) {
        std::memcpy(out->data(), v.stored.data(), v.stored.size());
        return v.stored.size() == rawSize(v);
    }
    return decodeMaskRle(in, v.w, v.h, v.encoding == kMaskEncXorRuns, out->data(), rawSize(v));
}

// A PackSource that fails every read from `failAt` on (I/O error mid-stream).
class FailingSource : public PackSource {
public:
    FailingSource(const std::vector<uint8_t>& d, uint32_t failAt) : d_(d), failAt_(failAt) {}
    uint32_t size() const override { return static_cast<uint32_t>(d_.size()); }
    bool read(uint32_t off, void* buf, size_t len) override {
        ++reads;
        if (off + len > failAt_ || off + len > d_.size()) return false;
        std::memcpy(buf, d_.data() + off, len);
        return true;
    }
    int reads = 0;

private:
    const std::vector<uint8_t>& d_;
    uint32_t failAt_;
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_vectors_present() {
    const auto vs = loadVectors();
    if (vs.empty()) TEST_FAIL_MESSAGE("rle_vectors.bin missing or malformed: run `tangocho-pack fixture`");
    int per[4] = {0, 0, 0, 0};
    for (const auto& v : vs) ++per[v.source & 3];
    TEST_ASSERT_TRUE(per[kSynthetic] >= 20);
    TEST_ASSERT_TRUE(per[kMiniGlyph] > 100);
    TEST_ASSERT_TRUE(per[kMiniMask] > 0);
    TEST_ASSERT_TRUE(per[kMalformed] > 0);
}

// Every well-formed vector decodes to the Python decoder's bitmap and never
// writes past it.
void test_vectors_decode() {
    const auto vs = loadVectors();
    if (vs.empty()) TEST_IGNORE_MESSAGE("no vectors");
    int checked = 0;
    for (size_t i = 0; i < vs.size(); ++i) {
        const Vector& v = vs[i];
        if (v.source == kMalformed) continue;
        char msg[64];
        std::snprintf(msg, sizeof msg, "vector %u (source %u kind %u enc %u %ux%u)", (unsigned)i,
                      v.source, v.kind, v.encoding, v.w, v.h);
        std::vector<uint8_t> out;
        TEST_ASSERT_TRUE_MESSAGE(decode(v, &out), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rawSize(v), v.raw.size(), msg);
        if (!v.raw.empty()) TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(v.raw.data(), out.data(), v.raw.size(), msg);
        for (size_t k = rawSize(v); k < out.size(); ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xEE, out[k], msg);
        ++checked;
    }
    TEST_ASSERT_TRUE(checked > 0);
}

void test_vectors_malformed_rejected() {
    const auto vs = loadVectors();
    if (vs.empty()) TEST_IGNORE_MESSAGE("no vectors");
    int checked = 0;
    for (size_t i = 0; i < vs.size(); ++i) {
        if (vs[i].source != kMalformed) continue;
        std::vector<uint8_t> out;
        char msg[32];
        std::snprintf(msg, sizeof msg, "malformed vector %u", (unsigned)i);
        TEST_ASSERT_FALSE_MESSAGE(decode(vs[i], &out), msg);
        ++checked;
    }
    TEST_ASSERT_TRUE(checked >= 5);
}

// The glyph stream length is implied: decoding stops after w*h pixels and
// leaves the rest of the input alone.
void test_glyph_stops_after_last_pixel() {
    const uint8_t s[] = {0x02, 0x7F, 0x20, 0x05, 0xF4, 0x30, 0xAB, 0xCD};  // spec example + junk
    ByteIn in(s, sizeof s);
    uint8_t out[9];
    TEST_ASSERT_TRUE(decodeGlyphRle(in, 5, 3, out, sizeof out));
    const uint8_t want[] = {0x00, 0x7F, 0xF0, 0x00, 0x7F, 0xF0, 0xFF, 0xFF, 0x30};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof want);
    TEST_ASSERT_EQUAL_UINT32(6, in.consumed());
    ByteIn in2(s, sizeof s);
    TEST_ASSERT_FALSE(decodeGlyphRle(in2, 5, 3, out, 8));  // cap too small
    ByteIn empty(s, 0);
    TEST_ASSERT_TRUE(decodeGlyphRle(empty, 0, 7, out, 0));  // no pixels, nothing to read
}

// Through a PackSource the input comes in 256-byte windows: a stream longer
// than one window decodes the same, and only the bytes it needs are read.
void test_window_refills_from_source() {
    // 60x20 glyph of literal pixels only: the stream is the raw bitmap itself.
    std::vector<uint8_t> file(32, 0);
    std::vector<uint8_t> want;
    for (int i = 0; i < 30 * 20; ++i) {
        const uint8_t b = static_cast<uint8_t>((1 + i % 14) << 4 | (1 + (i * 5) % 14));
        want.push_back(b);
        file.push_back(b);
    }
    file.resize(file.size() + 100, 0x11);  // more data after the stream
    MemorySource src(file);
    ByteIn in(src, 32, static_cast<uint32_t>(want.size()));
    std::vector<uint8_t> out(want.size());
    TEST_ASSERT_TRUE(decodeGlyphRle(in, 60, 20, out.data(), out.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want.data(), out.data(), want.size());
    TEST_ASSERT_EQUAL_UINT32(600, in.consumed());

    // The same stream with a read error in the third window fails cleanly.
    FailingSource bad(file, 32 + 2 * ByteIn::kWindow + 10);
    ByteIn in2(bad, 32, static_cast<uint32_t>(want.size()));
    TEST_ASSERT_FALSE(decodeGlyphRle(in2, 60, 20, out.data(), out.size()));
    TEST_ASSERT_TRUE(in2.ioError());
    TEST_ASSERT_EQUAL(3, bad.reads);

    // A mask whose data is cut by a read error is an error, not "the rest is empty".
    std::vector<uint8_t> mfile(600, 0x11);  // runs of 1: 1 empty, 1 ink, ...
    FailingSource mbad(mfile, 300);
    ByteIn in3(mbad, 0, 600);
    std::vector<uint8_t> mout(15 * 20);  // ceil(120/8) * 20
    TEST_ASSERT_FALSE(decodeMaskRle(in3, 120, 20, false, mout.data(), mout.size()));
}

void test_mask_examples() {
    uint8_t out[8];
    const uint8_t runs[] = {0x3A, 0x7A};
    ByteIn a(runs, sizeof runs);
    TEST_ASSERT_TRUE(decodeMaskRle(a, 16, 2, false, out, 4));
    const uint8_t want[] = {0x1F, 0xF8, 0x0F, 0xFC};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, 4);
    const uint8_t xr[] = {0x12};
    ByteIn b(xr, 1);
    TEST_ASSERT_TRUE(decodeMaskRle(b, 4, 6, true, out, 6));
    for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_HEX8(0x60, out[i]);
    ByteIn c(xr, 0);  // empty stream: an empty mask
    TEST_ASSERT_TRUE(decodeMaskRle(c, 4, 6, false, out, 6));
    for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_HEX8(0x00, out[i]);
}

// The v2 mini.tgc read through PackReader: every glyph and every stroke mask
// matches the builder's decoded bitmap (rle_vectors.bin, sources 1 and 2).
void test_mini_v2_fixture() {
    StdioSource src(kMini);
    if (!src.ok()) TEST_IGNORE_MESSAGE("mini.tgc not found");
    const auto vs = loadVectors();
    if (vs.empty()) TEST_IGNORE_MESSAGE("no vectors");
    PackReader r;
    TEST_ASSERT_EQUAL(PackError::Ok, r.open(src));
    TEST_ASSERT_EQUAL_UINT16(2, r.header().version);
    TEST_ASSERT_EQUAL_UINT32(3, r.cardCount());
    TEST_ASSERT_EQUAL_STRING("JLPT N5", r.deckName());
    TEST_ASSERT_TRUE(r.hasStrokeOrder());

    std::vector<const Vector*> glyphs, masks;
    for (const auto& v : vs) {
        if (v.source == kMiniGlyph) glyphs.push_back(&v);
        if (v.source == kMiniMask) masks.push_back(&v);
    }
    TEST_ASSERT_EQUAL_UINT32(r.glyphCount(), glyphs.size());
    int rle = 0;
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < r.glyphCount(); ++i) {
        const Glyph* g = r.glyphAt(i);
        const Vector& v = *glyphs[i];
        TEST_ASSERT_EQUAL_UINT8(v.encoding, g->encoding);
        TEST_ASSERT_EQUAL_UINT8(v.w, g->width);
        TEST_ASSERT_EQUAL_UINT8(v.h, g->height);
        buf.assign(PackReader::bitmapSize(*g), 0xEE);
        TEST_ASSERT_TRUE(r.readGlyphBitmap(*g, buf.data(), buf.size()));
        if (!buf.empty()) TEST_ASSERT_EQUAL_UINT8_ARRAY(v.raw.data(), buf.data(), buf.size());
        rle += g->encoding == kGlyphEncRle;
    }
    TEST_ASSERT_TRUE(rle > 0);  // the fixture does exercise the decoder

    size_t m = 0;
    int encoded = 0;
    for (uint32_t c = 0; c < r.cardCount(); ++c) {
        const uint16_t n = r.strokeCount(c);
        TEST_ASSERT_EQUAL_UINT16(4, n);  // 日, 月 and 火 have four strokes each
        for (uint16_t k = 0; k < n; ++k, ++m) {
            TEST_ASSERT_TRUE(m < masks.size());
            Stroke s;
            TEST_ASSERT_TRUE(r.strokeAt(c, k, &s));
            const Vector& v = *masks[m];
            TEST_ASSERT_EQUAL_UINT8(v.encoding, s.encoding);
            TEST_ASSERT_EQUAL_UINT8(v.w, s.w);
            TEST_ASSERT_EQUAL_UINT8(v.h, s.h);
            TEST_ASSERT_EQUAL_UINT16(v.stored.size(), s.size);
            buf.assign(PackReader::strokeBitmapSize(s), 0xEE);
            TEST_ASSERT_TRUE(r.readStrokeBitmap(s, buf.data(), buf.size()));
            TEST_ASSERT_EQUAL_UINT8_ARRAY(v.raw.data(), buf.data(), buf.size());
            encoded += s.encoding != kMaskEncRaw;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(masks.size(), m);
    TEST_ASSERT_TRUE(encoded > 0);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_vectors_present);
    RUN_TEST(test_vectors_decode);
    RUN_TEST(test_vectors_malformed_rejected);
    RUN_TEST(test_glyph_stops_after_last_pixel);
    RUN_TEST(test_window_refills_from_source);
    RUN_TEST(test_mask_examples);
    RUN_TEST(test_mini_v2_fixture);
    return UNITY_END();
}
