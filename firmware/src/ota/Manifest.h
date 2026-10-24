#pragma once
// Parser for the firmware release manifest, `tamagotcho-firmware.txt`
// (docs/ota.md §3.1, "Manifest format, version 1": the contract between
// .github/workflows/release.yml and the firmware), plus the release-version
// parsing and order the update rule needs (ota/Offer.h). Pure C++, no Arduino:
// unit tested natively in test/test_ota.
//
//     #tamagotcho-firmware<TAB>1
//     version<TAB>v0.7.0
//     app<TAB>sticks3<TAB>tamagotcho-sticks3.bin<TAB>1503216<TAB><sha256 hex>
//
// Line 1 is the header; after it blank lines and `#` lines are ignored, and so
// is any record type other than `version` and `app` (later formats may add
// some without a version bump). Unlike the deck catalog, one malformed
// `version` or `app` line refuses the whole manifest: an image is installed
// from it, so there are no partial reads.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tgc {

// The only manifest format version this firmware reads.
constexpr int kManifestVersion = 1;
// A longer body is not a manifest.
constexpr size_t kManifestMaxBytes = 8u * 1024u;
// An `app` line's environment name: [a-z0-9-]{1,31}, the PlatformIO env.
constexpr size_t kManifestMaxEnvLen = 31;

// A release version, `v<major>.<minor>.<patch>`.
struct FirmwareVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
};

// Exactly "v" and three non-negative decimal integers separated by dots,
// nothing before or after ("v0.7.0"; not "0.7.0", "v0.7", "v0.7.0-3-gabc1234",
// "dev"). Each number fits in 32 bits.
bool parseFirmwareVersion(const char* s, size_t len, FirmwareVersion* out);
bool parseFirmwareVersion(const std::string& s, FirmwareVersion* out);
// Numeric order: negative when a < b, 0 when equal, positive when a > b.
int compareFirmwareVersions(const FirmwareVersion& a, const FirmwareVersion& b);

// One `app` line: the image of one build environment.
struct ManifestApp {
    std::string env;          // "m5stack"
    std::string url;          // absolute http(s) URL (a bare file name resolved)
    uint32_t bytes = 0;       // exact size of the app-only image, > 0
    uint8_t sha256[32] = {};  // digest of the whole image
};

enum class ManifestError : uint8_t {
    Ok,
    TooLarge,            // more than kManifestMaxBytes
    NotAManifest,        // line 1 is not "#tamagotcho-firmware<TAB><digits>"
    UnsupportedVersion,  // a format version other than kManifestVersion
    BadVersion,          // `version` missing, repeated, after an `app` line or not vX.Y.Z
    BadApp,              // an `app` line with a bad field count, env, URL, size or digest
    DuplicateApp,        // a second `app` line for the same environment
};

struct ManifestParse {
    ManifestError error = ManifestError::Ok;
    int format = 0;           // the header's format version, when it has one
    size_t line = 0;          // 1-based line of the error (0 when none or not a line's)
    std::string version;      // the `version` field ("v0.7.0")
    FirmwareVersion release;  // `version` parsed
    std::vector<ManifestApp> apps;
};

// Parses `len` bytes of manifest text. `manifestUrl` is the URL the manifest
// was requested from (before any redirect): an `app` file name without a
// scheme is resolved against it the way the catalog's `url` field is
// (resolveCatalogUrl()), so `.../download/v0.7.0/tamagotcho-firmware.txt`
// gives `.../download/v0.7.0/<file>`. On any error `apps` is empty.
ManifestParse parseManifest(const char* text, size_t len, const char* manifestUrl);

// The row for build environment `env`, or nullptr when the manifest has none.
const ManifestApp* manifestAppFor(const ManifestParse& m, const char* env);

// [a-z0-9-]{1,31}.
bool manifestEnvIsValid(const char* env, size_t len);

const char* manifestErrorString(ManifestError e);

}  // namespace tgc
