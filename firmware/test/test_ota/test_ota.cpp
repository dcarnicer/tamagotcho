// Native tests for the firmware manifest (src/ota/Manifest: docs/ota.md §3.1,
// "Manifest format, version 1"), the release-version order and the update rule
// (src/ota/Offer: §3.2, §3.3, §3.5). The first fixture is the spec's example.
#include <unity.h>

#include <cstring>
#include <string>

#include "ota/Manifest.h"
#include "ota/Offer.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

namespace {

const char* kLatestUrl =
    "https://github.com/dcarnicer/tamagotcho/releases/latest/download/tamagotcho-firmware.txt";
const char* kTagUrl =
    "https://github.com/dcarnicer/tamagotcho/releases/download/v0.7.0/tamagotcho-firmware.txt";

const char* kSha = "dc4b83c07f8cdca6cccbe77306936d340a4404dd6dff57ad38febfadf83eee75";
const char* kShaUpper = "DC4B83C07F8CDCA6CCCBE77306936D340A4404DD6DFF57AD38FEBFADF83EEE75";

const char* kHeader = "#tamagotcho-firmware\t1\n";

std::string appLine(const std::string& env, const std::string& file = "", uint32_t bytes = 1503216,
                    const std::string& sha = kSha) {
    const std::string f = file.empty() ? "tamagotcho-" + env + ".bin" : file;
    return "app\t" + env + "\t" + f + "\t" + std::to_string(bytes) + "\t" + sha + "\n";
}

// The spec's example: version, then one row per environment.
std::string specExample() {
    return std::string(kHeader) + "version\tv0.7.0\n" + appLine("sticks3", "", 1503216) +
           appLine("tdisplay-s3", "", 1344128) + appLine("cardputer-adv", "", 1451184) +
           appLine("m5stack", "", 1508720);
}

ManifestParse parse(const std::string& text, const char* url = kTagUrl) {
    return parseManifest(text.data(), text.size(), url);
}

ManifestError errorOf(const std::string& text) { return parse(text).error; }

}  // namespace

// ---------------------------------------------------------------- versions

void test_version_parse() {
    FirmwareVersion v;
    TEST_ASSERT_TRUE(parseFirmwareVersion(std::string("v0.7.0"), &v));
    TEST_ASSERT_EQUAL_UINT32(0, v.major);
    TEST_ASSERT_EQUAL_UINT32(7, v.minor);
    TEST_ASSERT_EQUAL_UINT32(0, v.patch);
    TEST_ASSERT_TRUE(parseFirmwareVersion(std::string("v12.345.6789"), &v));
    TEST_ASSERT_EQUAL_UINT32(12, v.major);
    TEST_ASSERT_EQUAL_UINT32(345, v.minor);
    TEST_ASSERT_EQUAL_UINT32(6789, v.patch);
    TEST_ASSERT_TRUE(parseFirmwareVersion(std::string("v4294967295.0.0"), &v));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, v.major);
    // Anything but v<n>.<n>.<n> is not a release: git describe output, "dev",
    // a missing or extra part, signs, spaces, an overflow.
    for (const char* bad : {"", "v", "0.7.0", "V0.7.0", "v0.7", "v0.7.0.1", "v0.7.0-3-gabc1234",
                            "v0.7.0-dirty", "dev", "abc1234", "v0..0", "v.7.0", "v0.7.", "v-1.0.0",
                            "v+1.0.0", " v0.7.0", "v0.7.0 ", "v0. 7.0", "v4294967296.0.0",
                            "v0.7.0\n", "vx.y.z"})
        TEST_ASSERT_FALSE_MESSAGE(parseFirmwareVersion(std::string(bad), &v), bad);
    TEST_ASSERT_FALSE(parseFirmwareVersion(nullptr, 6, &v));
}

void test_version_order() {
    auto cmp = [](const char* a, const char* b) {
        FirmwareVersion va, vb;
        TEST_ASSERT_TRUE(parseFirmwareVersion(std::string(a), &va));
        TEST_ASSERT_TRUE(parseFirmwareVersion(std::string(b), &vb));
        return compareFirmwareVersions(va, vb);
    };
    TEST_ASSERT_EQUAL(0, cmp("v0.7.0", "v0.7.0"));
    TEST_ASSERT_TRUE(cmp("v0.7.0", "v0.6.9") > 0);
    TEST_ASSERT_TRUE(cmp("v0.6.9", "v0.7.0") < 0);
    // Numeric, not textual.
    TEST_ASSERT_TRUE(cmp("v0.10.0", "v0.9.0") > 0);
    TEST_ASSERT_TRUE(cmp("v0.7.10", "v0.7.9") > 0);
    TEST_ASSERT_TRUE(cmp("v1.0.0", "v0.99.99") > 0);
    TEST_ASSERT_EQUAL(0, cmp("v0.07.0", "v0.7.0"));
}

// ---------------------------------------------------------------- the parser

void test_spec_example() {
    const ManifestParse m = parse(specExample());
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(1, m.format);
    TEST_ASSERT_EQUAL_STRING("v0.7.0", m.version.c_str());
    TEST_ASSERT_EQUAL_UINT32(7, m.release.minor);
    TEST_ASSERT_EQUAL(4, static_cast<int>(m.apps.size()));
    const ManifestApp* a = manifestAppFor(m, "m5stack");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("m5stack", a->env.c_str());
    TEST_ASSERT_EQUAL_UINT32(1508720, a->bytes);
    TEST_ASSERT_EQUAL_HEX8(0xdc, a->sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x75, a->sha256[31]);
    // A bare file name replaces the manifest's own last path segment.
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dcarnicer/tamagotcho/releases/download/v0.7.0/tamagotcho-m5stack.bin",
        a->url.c_str());
    TEST_ASSERT_NOT_NULL(manifestAppFor(m, "tdisplay-s3"));
    TEST_ASSERT_EQUAL_UINT32(1344128, manifestAppFor(m, "tdisplay-s3")->bytes);
    TEST_ASSERT_NULL(manifestAppFor(m, "sticks3-shots"));
    TEST_ASSERT_NULL(manifestAppFor(m, ""));
    TEST_ASSERT_NULL(manifestAppFor(m, nullptr));
}

void test_relative_and_absolute_urls() {
    // Read through releases/latest/download/: a bare name stays under it.
    ManifestParse m = parse(specExample(), kLatestUrl);
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dcarnicer/tamagotcho/releases/latest/download/tamagotcho-sticks3.bin",
        manifestAppFor(m, "sticks3")->url.c_str());
    // A query or fragment of the manifest URL is dropped.
    m = parse(specExample(), "http://192.168.1.20:8000/fw/tamagotcho-firmware.txt?x=1#y");
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8000/fw/tamagotcho-sticks3.bin",
                             manifestAppFor(m, "sticks3")->url.c_str());
    // An http:// or https:// URL is used as it is.
    const std::string text = std::string(kHeader) + "version\tv0.7.0\n" +
                             appLine("sticks3", "http://192.168.1.20:8000/a.bin") +
                             appLine("m5stack", "https://example.com/b/c.bin");
    m = parse(text);
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8000/a.bin", manifestAppFor(m, "sticks3")->url.c_str());
    TEST_ASSERT_EQUAL_STRING("https://example.com/b/c.bin", manifestAppFor(m, "m5stack")->url.c_str());
    // A relative name with no usable manifest URL cannot be resolved.
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadApp),
                      static_cast<int>(parse(specExample(), "").error));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadApp),
                      static_cast<int>(parse(specExample(), nullptr).error));
}

void test_ignored_lines() {
    // CRLF, blank lines, comments and unknown record types (with any fields)
    // around the records; the SHA-256 in upper case; no LF after the last line.
    const std::string text = "#tamagotcho-firmware\t1\r\n"
                             "\r\n"
                             "# written by release.yml\r\n"
                             "notes\thttps://example.com/notes\textra\r\n"
                             "version\tv0.7.0\r\n"
                             "\n"
                             "#app\tbroken\n"
                             "channel\tstable\n"
                             "app\tsticks3\ttamagotcho-sticks3.bin\t1503216\t" +
                             std::string(kShaUpper) + "\r\n"
                             "minfw\t\t\t\n"
                             "app\tm5stack\ttamagotcho-m5stack.bin\t1508720\t" + kSha;
    const ManifestParse m = parse(text);
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(2, static_cast<int>(m.apps.size()));
    TEST_ASSERT_EQUAL_HEX8(0xdc, manifestAppFor(m, "sticks3")->sha256[0]);
    TEST_ASSERT_EQUAL_UINT32(1508720, manifestAppFor(m, "m5stack")->bytes);
    // A version with no app lines is a valid manifest that lists nothing.
    const ManifestParse empty = parse(std::string(kHeader) + "version\tv0.7.0\n");
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(empty.error));
    TEST_ASSERT_TRUE(empty.apps.empty());
}

void test_header() {
    // Not a manifest: no header, an HTML page, the header not on line 1, a
    // bad tag, a missing or non-numeric version, an extra field, a BOM, empty.
    for (const std::string& bad :
         {std::string("version\tv0.7.0\n"), std::string("<!DOCTYPE html><html>Not Found</html>"),
          std::string("\n#tamagotcho-firmware\t1\nversion\tv0.7.0\n"),
          std::string("# comment\n#tamagotcho-firmware\t1\nversion\tv0.7.0\n"),
          std::string("#tangocho-catalog\t2\n"), std::string("#tamagotcho-firmware\n"),
          std::string("#tamagotcho-firmware\t\n"), std::string("#tamagotcho-firmware\tone\n"),
          std::string("#tamagotcho-firmware 1\n"), std::string("#tamagotcho-firmware\t1\textra\n"),
          std::string("#tamagotcho-firmware\t-1\n"), std::string("#tamagotcho-firmware\t 1\n"),
          std::string("\xEF\xBB\xBF#tamagotcho-firmware\t1\nversion\tv0.7.0\n"), std::string()})
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ManifestError::NotAManifest),
                                  static_cast<int>(errorOf(bad)), bad.c_str());
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::NotAManifest),
                      static_cast<int>(parseManifest(nullptr, 10, kTagUrl).error));
    // Any other format version is refused as a whole, before its records.
    ManifestParse m = parse("#tamagotcho-firmware\t2\nversion\tv0.7.0\n" + appLine("sticks3"));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::UnsupportedVersion), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(2, m.format);
    TEST_ASSERT_TRUE(m.apps.empty());
    m = parse("#tamagotcho-firmware\t0\n");
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::UnsupportedVersion), static_cast<int>(m.error));
    // CRLF on the header is fine.
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok),
                      static_cast<int>(errorOf("#tamagotcho-firmware\t1\r\nversion\tv0.7.0\r\n")));
}

void test_version_line() {
    const std::string h = kHeader;
    // Missing, repeated, after an app line, not vX.Y.Z, extra or missing field.
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion), static_cast<int>(errorOf(h)));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion),
                      static_cast<int>(errorOf(h + "# version\tv0.7.0\n")));
    ManifestParse m = parse(h + "version\tv0.7.0\nversion\tv0.7.0\n");
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(3, static_cast<int>(m.line));
    m = parse(h + appLine("sticks3") + "version\tv0.7.0\n");
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(2, static_cast<int>(m.line));
    m = parse(h + "version\tv0.7.0\n" + appLine("sticks3") + "version\tv0.7.1\n");
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion), static_cast<int>(m.error));
    TEST_ASSERT_TRUE(m.apps.empty());  // no partial reads
    for (const char* v : {"0.7.0", "v0.7", "v0.7.0-rc1", "dev", "", "v0.7.0 "}) {
        const std::string text = h + "version\t" + v + "\n" + appLine("sticks3");
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ManifestError::BadVersion),
                                  static_cast<int>(errorOf(text)), v);
    }
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion),
                      static_cast<int>(errorOf(h + "version\n")));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::BadVersion),
                      static_cast<int>(errorOf(h + "version\tv0.7.0\tlatest\n")));
}

void test_app_line() {
    const std::string h = std::string(kHeader) + "version\tv0.7.0\n";
    auto err = [&](const std::string& line) { return static_cast<int>(errorOf(h + line + "\n")); };
    const int bad = static_cast<int>(ManifestError::BadApp);
    const std::string sha = kSha;
    // Field count.
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1503216"));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1503216\t" + sha + "\textra"));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\t\ta.bin\t1503216\t" + sha));
    // The environment name: [a-z0-9-]{1,31}.
    TEST_ASSERT_EQUAL(bad, err("app\t\ta.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tStickS3\ta.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3_dev\ta.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks 3\ta.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\t" + std::string(32, 'a') + "\ta.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok),
                      err("app\t" + std::string(31, 'a') + "\ta.bin\t1503216\t" + sha));
    // The file name or URL: no path, no other scheme, nothing empty.
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\t\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\tfw/a.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\t..\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\tftp://example.com/a.bin\t1503216\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\thttps://\t1503216\t" + sha));
    // The size: decimal, greater than 0, 32 bits.
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t0\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t-1\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1.5\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t0x100\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t4294967296\t" + sha));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t\t" + sha));
    // The digest: 64 hex digits.
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1503216\t" + sha.substr(1)));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1503216\t" + sha + "0"));
    TEST_ASSERT_EQUAL(bad, err("app\tsticks3\ta.bin\t1503216\tg" + sha.substr(1)));
    // One bad line refuses the whole manifest, with its line number.
    const ManifestParse m = parse(h + appLine("sticks3") + appLine("m5stack", "", 0));
    TEST_ASSERT_EQUAL(bad, static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(4, static_cast<int>(m.line));
    TEST_ASSERT_TRUE(m.apps.empty());
    TEST_ASSERT_NULL(manifestAppFor(m, "sticks3"));
}

void test_duplicate_env() {
    const std::string h = std::string(kHeader) + "version\tv0.7.0\n";
    const ManifestParse m =
        parse(h + appLine("sticks3") + appLine("m5stack") + appLine("sticks3", "other.bin"));
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::DuplicateApp), static_cast<int>(m.error));
    TEST_ASSERT_EQUAL(5, static_cast<int>(m.line));
    TEST_ASSERT_TRUE(m.apps.empty());
}

void test_size_cap() {
    // Exactly 8 KiB is read; one byte more is not a manifest.
    std::string text = specExample() + "#";
    while (text.size() < kManifestMaxBytes - 1) text += "x";
    text += "\n";
    TEST_ASSERT_EQUAL(kManifestMaxBytes, text.size());
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::Ok), static_cast<int>(errorOf(text)));
    text += "\n";
    TEST_ASSERT_EQUAL(static_cast<int>(ManifestError::TooLarge), static_cast<int>(errorOf(text)));
}

void test_error_strings() {
    TEST_ASSERT_EQUAL_STRING("not a firmware manifest",
                             manifestErrorString(ManifestError::NotAManifest));
    TEST_ASSERT_EQUAL_STRING("unsupported manifest version",
                             manifestErrorString(ManifestError::UnsupportedVersion));
    TEST_ASSERT_TRUE(manifestEnvIsValid("cardputer-adv", 13));
    TEST_ASSERT_FALSE(manifestEnvIsValid(nullptr, 0));
}

// ---------------------------------------------------------------- the offer

namespace {

OfferInput input(const char* running, bool overridden = false, const char* rolledBack = "",
                 uint32_t slot = 0x1C0000, const char* env = "m5stack") {
    OfferInput in;
    in.running = running;
    in.env = env;
    in.urlOverridden = overridden;
    in.rolledBack = rolledBack;
    in.slotBytes = slot;
    return in;
}

int verdict(const ManifestParse& m, const OfferInput& in) {
    return static_cast<int>(offerFor(m, in).verdict);
}

constexpr int kOffer = static_cast<int>(OfferVerdict::Offer);
constexpr int kUpToDate = static_cast<int>(OfferVerdict::UpToDate);

}  // namespace

void test_release_builds() {
    TEST_ASSERT_TRUE(isReleaseBuild("v0.6.0"));
    TEST_ASSERT_FALSE(isReleaseBuild("v0.6.0-12-gabc1234"));
    TEST_ASSERT_FALSE(isReleaseBuild("v0.6.0-12-gabc1234-dirty"));
    TEST_ASSERT_FALSE(isReleaseBuild("abc1234"));
    TEST_ASSERT_FALSE(isReleaseBuild("dev"));
    TEST_ASSERT_FALSE(isReleaseBuild(nullptr));
    // A release build reads the default manifest; a dev build only an override.
    TEST_ASSERT_TRUE(manifestReadAllowed("v0.6.0", false));
    TEST_ASSERT_TRUE(manifestReadAllowed("v0.6.0", true));
    TEST_ASSERT_FALSE(manifestReadAllowed("v0.6.0-12-gabc1234", false));
    TEST_ASSERT_FALSE(manifestReadAllowed("dev", false));
    TEST_ASSERT_TRUE(manifestReadAllowed("dev", true));
}

void test_offer_release() {
    const ManifestParse m = parse(specExample());  // v0.7.0
    const Offer o = offerFor(m, input("v0.6.0"));
    TEST_ASSERT_EQUAL(kOffer, static_cast<int>(o.verdict));
    TEST_ASSERT_NOT_NULL(o.app);
    TEST_ASSERT_EQUAL_STRING("m5stack", o.app->env.c_str());
    TEST_ASSERT_EQUAL_UINT32(1508720, o.app->bytes);
    // Only a strictly newer release: the same one or an older one is up to date.
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v0.7.0")));
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v0.7.1")));
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v0.10.0")));
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v1.0.0")));
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.9")));
    // An override does not change a release build's rule (no downgrades).
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v0.7.1", true)));
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.0", true)));
}

void test_offer_dev() {
    const ManifestParse m = parse(specExample());
    // A dev build with the default URL never reads it (§3.2) ...
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::NotAllowed),
                      verdict(m, input("v0.6.0-12-gabc1234")));
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::NotAllowed), verdict(m, input("dev")));
    // ... and with an override takes any version other than its own, older too.
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.0-12-gabc1234", true)));
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.8.0-3-gdef5678-dirty", true)));
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("dev", true)));
}

void test_offer_rolled_back_no_image_too_large() {
    const ManifestParse m = parse(specExample());
    // The version the device came back from is never offered again ...
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::RolledBack),
                      verdict(m, input("v0.6.0", false, "v0.7.0")));
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::RolledBack),
                      verdict(m, input("dev", true, "v0.7.0")));
    // ... while a later one is, and so is this one after another failure.
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.0", false, "v0.6.5")));
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.0", false, nullptr)));
    // Up to date wins over a remembered failure of the running version.
    TEST_ASSERT_EQUAL(kUpToDate, verdict(m, input("v0.7.0", false, "v0.7.0")));
    // No row for this build's environment: nothing.
    Offer o = offerFor(m, input("v0.6.0", false, "", 0x1C0000, "sticks3-shots"));
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::NoImage), static_cast<int>(o.verdict));
    TEST_ASSERT_NULL(o.app);
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::NoImage),
                      verdict(m, input("v0.6.0", false, "", 0x1C0000, "")));
    // An image larger than the update slot: nothing, with the row for the reason.
    o = offerFor(m, input("v0.6.0", false, "", 1508719));
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::TooLarge), static_cast<int>(o.verdict));
    TEST_ASSERT_NOT_NULL(o.app);
    TEST_ASSERT_EQUAL(kOffer, verdict(m, input("v0.6.0", false, "", 1508720)));
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::TooLarge),
                      verdict(m, input("v0.6.0", false, "", 0)));  // no slot at all
    // A manifest that did not parse offers nothing.
    TEST_ASSERT_EQUAL(static_cast<int>(OfferVerdict::BadManifest),
                      verdict(parse("<html></html>"), input("v0.6.0")));
    TEST_ASSERT_EQUAL_STRING("offer", offerVerdictString(OfferVerdict::Offer));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_version_parse);
    RUN_TEST(test_version_order);
    RUN_TEST(test_spec_example);
    RUN_TEST(test_relative_and_absolute_urls);
    RUN_TEST(test_ignored_lines);
    RUN_TEST(test_header);
    RUN_TEST(test_version_line);
    RUN_TEST(test_app_line);
    RUN_TEST(test_duplicate_env);
    RUN_TEST(test_size_cap);
    RUN_TEST(test_error_strings);
    RUN_TEST(test_release_builds);
    RUN_TEST(test_offer_release);
    RUN_TEST(test_offer_dev);
    RUN_TEST(test_offer_rolled_back_no_image_too_large);
    return UNITY_END();
}
