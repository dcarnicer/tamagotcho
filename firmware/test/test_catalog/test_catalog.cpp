// Native tests for the deck catalog parser (src/catalog). The first two
// fixtures are verbatim `tangocho-pack catalog` output for two copies of the
// builder's committed mini.tgc, without and with --base-url.
#include <unity.h>

#include <cstring>
#include <string>

#include "catalog/Catalog.h"

using namespace tgc;

namespace {

const char* kDefaultUrl =
    "https://github.com/dcarnicer/tangocho/releases/latest/download/catalog.txt";

const char* kBuilderBare =
    "#tangocho-catalog\t1\n"
    "# name\tlang\tcards\tbytes\tsha256\turl\n"
    "n5-en\ten\t3\t8288\t89b3ae7646d896e6b80c264fe99263ad3bc6b3a673c4e5d37cfa17c30c7344a8\tn5-en.tgc\n"
    "n5-es-strokes\ten\t3\t8288\t89b3ae7646d896e6b80c264fe99263ad3bc6b3a673c4e5d37cfa17c30c7344a8"
    "\tn5-es-strokes.tgc\n";

const char* kBuilderAbsolute =
    "#tangocho-catalog\t1\n"
    "# name\tlang\tcards\tbytes\tsha256\turl\n"
    "n5-en\ten\t3\t8288\t89b3ae7646d896e6b80c264fe99263ad3bc6b3a673c4e5d37cfa17c30c7344a8"
    "\thttps://github.com/dcarnicer/tangocho/releases/download/v0.3.0/n5-en.tgc\n"
    "n5-es-strokes\ten\t3\t8288\t89b3ae7646d896e6b80c264fe99263ad3bc6b3a673c4e5d37cfa17c30c7344a8"
    "\thttps://github.com/dcarnicer/tangocho/releases/download/v0.3.0/n5-es-strokes.tgc\n";

const char* kSha = "89b3ae7646d896e6b80c264fe99263ad3bc6b3a673c4e5d37cfa17c30c7344a8";

CatalogParse parse(const std::string& text, const char* url = kDefaultUrl) {
    return parseCatalog(text.data(), text.size(), url);
}

// A header plus one record built from the given fields.
std::string withRecord(const std::string& record) {
    return std::string("#tangocho-catalog\t1\n") + record + "\n";
}

std::string record(const std::string& name, const std::string& lang = "en",
                   const std::string& cards = "79", const std::string& bytes = "703123",
                   const std::string& sha = kSha, const std::string& url = "x.tgc") {
    return name + "\t" + lang + "\t" + cards + "\t" + bytes + "\t" + sha + "\t" + url;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_builder_output_bare_urls() {
    const CatalogParse r = parse(kBuilderBare);
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(1, r.version);
    TEST_ASSERT_EQUAL(0u, r.skipped);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_EQUAL(2u, r.entries.size());
    const CatalogEntry& a = r.entries[0];
    TEST_ASSERT_EQUAL_STRING("n5-en", a.name.c_str());
    TEST_ASSERT_EQUAL_STRING("en", a.lang.c_str());
    TEST_ASSERT_EQUAL_UINT32(3, a.cards);
    TEST_ASSERT_EQUAL_UINT32(8288, a.bytes);
    TEST_ASSERT_EQUAL_HEX8(0x89, a.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xb3, a.sha256[1]);
    TEST_ASSERT_EQUAL_HEX8(0xa8, a.sha256[31]);
    // A bare file name is resolved next to the catalog itself.
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dcarnicer/tangocho/releases/latest/download/n5-en.tgc", a.url.c_str());
    TEST_ASSERT_EQUAL_STRING("n5-es-strokes", r.entries[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dcarnicer/tangocho/releases/latest/download/n5-es-strokes.tgc",
        r.entries[1].url.c_str());
}

void test_builder_output_absolute_urls() {
    const CatalogParse r = parse(kBuilderAbsolute);
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(2u, r.entries.size());
    TEST_ASSERT_EQUAL_STRING("https://github.com/dcarnicer/tangocho/releases/download/v0.3.0/n5-en.tgc",
                             r.entries[0].url.c_str());
}

void test_comments_blank_lines_crlf_and_bom() {
    const std::string text = std::string("\xEF\xBB\xBF") + "#tangocho-catalog\t1\r\n" + "\r\n" +
                             "# a comment\r\n" + "   \r\n" + record("a") + "\r\n" + "#another\r\n" +
                             record("b") + "   ";  // no final newline, trailing spaces
    const CatalogParse r = parse(text);
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(2u, r.entries.size());
    TEST_ASSERT_EQUAL_STRING("a", r.entries[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("b", r.entries[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dcarnicer/tangocho/releases/latest/download/x.tgc",
        r.entries[1].url.c_str());
}

void test_leading_blank_lines_before_tag() {
    const CatalogParse r = parse("\n\n#tangocho-catalog\t1\n" + record("a") + "\n");
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(1u, r.entries.size());
}

void test_rejects_non_catalogs() {
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("\n\n  \n").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("<!DOCTYPE html><html>Not Found</html>").error));
    // The tag must be the first non-blank line, TAB-separated, with a number.
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("# hello\n#tangocho-catalog\t1\n").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("#tangocho-catalog 1\n").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("#tangocho-catalog\tv1\n").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parse("#tangocho-catalog\t\n").error));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::NotACatalog),
                      static_cast<int>(parseCatalog(nullptr, 10, kDefaultUrl).error));
}

void test_rejects_unknown_versions() {
    CatalogParse r = parse("#tangocho-catalog\t2\n" + record("a") + "\n");
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::UnsupportedVersion), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(2, r.version);
    TEST_ASSERT_EQUAL(0u, r.entries.size());
    r = parse("#tangocho-catalog\t0\n");
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::UnsupportedVersion), static_cast<int>(r.error));
}

void test_empty_catalog_is_ok() {
    const CatalogParse r = parse("#tangocho-catalog\t1\n# name\tlang\tcards\tbytes\tsha256\turl\n");
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(0u, r.entries.size());
    TEST_ASSERT_EQUAL(0u, r.skipped);
}

void test_skips_invalid_records() {
    const std::string bad[] = {
        // field count
        "a\ten\t79\t703123\t" + std::string(kSha),
        record("a") + "\textra",
        record("a") + "\t",
        // unsafe names
        record(""), record("../evil"), record(".hidden"), record("a/b"), record("a\\b"),
        record("a b"), record("a..b"), record("a."), record(std::string(44, 'n')),
        record("caf\xC3\xA9"),
        // lang
        record("a", "e n"), record("a", "toolonglang"),
        // numbers
        record("a", "en", "0"), record("a", "en", "-1"), record("a", "en", "+5"),
        record("a", "en", ""), record("a", "en", "65536"), record("a", "en", "7x"),
        record("a", "en", "79", "63"), record("a", "en", "79", "5242881"),
        record("a", "en", "79", "99999999999"), record("a", "en", "79", " 100"),
        // digest
        record("a", "en", "79", "703123", std::string(kSha).substr(0, 63)),
        record("a", "en", "79", "703123", std::string(kSha) + "0"),
        record("a", "en", "79", "703123", "zz" + std::string(kSha).substr(2)),
        // url
        record("a", "en", "79", "703123", kSha, ""),
        record("a", "en", "79", "703123", kSha, "packs/a.tgc"),
        record("a", "en", "79", "703123", kSha, "../a.tgc"),
        record("a", "en", "79", "703123", kSha, ".."),
        record("a", "en", "79", "703123", kSha, "/a.tgc"),
        record("a", "en", "79", "703123", kSha, "ftp://example.com/a.tgc"),
        record("a", "en", "79", "703123", kSha, "https://"),
        record("a", "en", "79", "703123", kSha, "https://:443/a.tgc"),
        record("a", "en", "79", "703123", kSha, "https://exa mple.com/a.tgc"),
        record("a", "en", "79", "703123", kSha, "a.tgc?x=1"),
        record("a", "en", "79", "703123", kSha, "javascript:alert(1)"),
    };
    for (const std::string& line : bad) {
        const CatalogParse r = parse(withRecord(line));
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(CatalogError::NoValidEntries),
                                  static_cast<int>(r.error), line.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(1u, r.skipped, line.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(0u, r.entries.size(), line.c_str());
    }
}

void test_one_bad_record_does_not_hide_the_rest() {
    const std::string text = "#tangocho-catalog\t1\n" + record("../evil") + "\n" + record("good") +
                             "\n" + record("good") + "\n";  // duplicate name skipped too
    const CatalogParse r = parse(text);
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(1u, r.entries.size());
    TEST_ASSERT_EQUAL_STRING("good", r.entries[0].name.c_str());
    TEST_ASSERT_EQUAL(2u, r.skipped);
}

void test_accepts_edge_values() {
    const std::string upper = "89B3AE7646D896E6B80C264FE99263AD3BC6B3A673C4E5D37CFA17C30C7344A8";
    const CatalogParse r = parse(withRecord(record(std::string(43, 'n'), "", "65535", "5242880", upper,
                                                   "http://192.168.1.10:8000/packs/n.tgc")));
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(1u, r.entries.size());
    TEST_ASSERT_EQUAL_STRING("", r.entries[0].lang.c_str());
    TEST_ASSERT_EQUAL_UINT32(65535, r.entries[0].cards);
    TEST_ASSERT_EQUAL_UINT32(5242880, r.entries[0].bytes);
    TEST_ASSERT_EQUAL_HEX8(0x89, r.entries[0].sha256[0]);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8000/packs/n.tgc", r.entries[0].url.c_str());
}

void test_caps_the_entry_count() {
    std::string text = "#tangocho-catalog\t1\n";
    for (int i = 0; i < 40; ++i) text += record("deck" + std::to_string(i)) + "\n";
    const CatalogParse r = parse(text);
    TEST_ASSERT_EQUAL(static_cast<int>(CatalogError::Ok), static_cast<int>(r.error));
    TEST_ASSERT_EQUAL(kCatalogMaxEntries, r.entries.size());
    TEST_ASSERT_TRUE(r.truncated);
    TEST_ASSERT_EQUAL_STRING("deck0", r.entries[0].name.c_str());
}

void test_resolve_urls() {
    std::string out = "untouched";
    TEST_ASSERT_TRUE(resolveCatalogUrl("https://example.com/a/b/catalog.txt?x=1#f", "n5.tgc", &out));
    TEST_ASSERT_EQUAL_STRING("https://example.com/a/b/n5.tgc", out.c_str());
    TEST_ASSERT_TRUE(resolveCatalogUrl("https://example.com", "n5.tgc", &out));
    TEST_ASSERT_EQUAL_STRING("https://example.com/n5.tgc", out.c_str());
    TEST_ASSERT_TRUE(resolveCatalogUrl("http://example.com:8080?q", "n5.tgc", &out));
    TEST_ASSERT_EQUAL_STRING("http://example.com:8080/n5.tgc", out.c_str());
    TEST_ASSERT_TRUE(resolveCatalogUrl("https://example.com/dir/", "n5.tgc", &out));
    TEST_ASSERT_EQUAL_STRING("https://example.com/dir/n5.tgc", out.c_str());
    // Absolute references are kept whatever the base is.
    TEST_ASSERT_TRUE(resolveCatalogUrl("", "https://cdn.example.com/x.tgc", &out));
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.com/x.tgc", out.c_str());
    // A bare name needs a usable base; paths and other schemes are refused.
    out = "untouched";
    TEST_ASSERT_FALSE(resolveCatalogUrl("", "n5.tgc", &out));
    TEST_ASSERT_FALSE(resolveCatalogUrl("file:///tmp/catalog.txt", "n5.tgc", &out));
    TEST_ASSERT_FALSE(resolveCatalogUrl("https://example.com/c.txt", "sub/n5.tgc", &out));
    TEST_ASSERT_FALSE(resolveCatalogUrl("https://example.com/c.txt", "HTTPS://x/y", &out));
    TEST_ASSERT_EQUAL_STRING("untouched", out.c_str());
    // Over-long results are refused.
    const std::string longBase = "https://example.com/" + std::string(1000, 'p') + "/c.txt";
    TEST_ASSERT_FALSE(resolveCatalogUrl(longBase, std::string(40, 'n') + ".tgc", &out));
}

void test_helpers() {
    TEST_ASSERT_TRUE(catalogNameIsSafe("n5-en_strokes.v2", 16));
    TEST_ASSERT_FALSE(catalogNameIsSafe("n5", 0));
    TEST_ASSERT_FALSE(catalogNameIsSafe(nullptr, 3));
    TEST_ASSERT_FALSE(catalogNameIsSafe(".n5", 3));
    TEST_ASSERT_FALSE(catalogNameIsSafe("n5..x", 5));
    // "x." would be saved as "x..tgc", which the portal cannot delete.
    TEST_ASSERT_FALSE(catalogNameIsSafe("x.", 2));
    TEST_ASSERT_TRUE(catalogNameIsSafe("x.v2", 4));
    uint8_t d[32];
    TEST_ASSERT_TRUE(parseSha256Hex(kSha, 64, d));
    TEST_ASSERT_EQUAL_HEX8(0x7c, d[24]);
    TEST_ASSERT_FALSE(parseSha256Hex(kSha, 63, d));
    TEST_ASSERT_TRUE(isHttpUrl("http://a"));
    TEST_ASSERT_TRUE(isHttpUrl("https://a:1/b?c#d"));
    TEST_ASSERT_FALSE(isHttpUrl("https://"));
    TEST_ASSERT_FALSE(isHttpUrl("https://a\tb"));
    TEST_ASSERT_FALSE(isHttpUrl("mailto:x@y"));
    TEST_ASSERT_EQUAL_STRING("not a catalog", catalogErrorString(CatalogError::NotACatalog));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_builder_output_bare_urls);
    RUN_TEST(test_builder_output_absolute_urls);
    RUN_TEST(test_comments_blank_lines_crlf_and_bom);
    RUN_TEST(test_leading_blank_lines_before_tag);
    RUN_TEST(test_rejects_non_catalogs);
    RUN_TEST(test_rejects_unknown_versions);
    RUN_TEST(test_empty_catalog_is_ok);
    RUN_TEST(test_skips_invalid_records);
    RUN_TEST(test_one_bad_record_does_not_hide_the_rest);
    RUN_TEST(test_accepts_edge_values);
    RUN_TEST(test_caps_the_entry_count);
    RUN_TEST(test_resolve_urls);
    RUN_TEST(test_helpers);
    return UNITY_END();
}
