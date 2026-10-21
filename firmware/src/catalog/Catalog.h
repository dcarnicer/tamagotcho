#pragma once
// Parser for the line-oriented deck catalog written by `tamagotcho-pack catalog`
// (format v2: tools/pack-builder/README.md, "Catalog format"; published as
// `catalog-v2.txt`), plus the small URL and file-name helpers the on-device "Get
// decks" screen needs. Pure C++, no Arduino: unit tested natively in
// test/test_catalog.
//
//     #tangocho-catalog<TAB>2
//     # name<TAB>title<TAB>lang<TAB>cards<TAB>bytes<TAB>sha256<TAB>url
//     n5-en<TAB>JLPT N5<TAB>en<TAB>79<TAB>703123<TAB>ab12...<TAB>https://.../n5-en.tgc
//
// The first non-blank line must carry the format tag and version 2; any other
// version (v1, which had no title column, included) is refused as a whole.
// Every other `#` line and every blank line is skipped. A record that does not
// validate (wrong field count, bad title, number or digest, unsafe name,
// unusable URL, duplicate name) is skipped and counted, so one bad line does not
// hide the rest of the catalog.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tgc {

// The only catalog format version this firmware reads (no backward
// compatibility: docs/decisions.md, 2026-10-18 "Catalog v2: deck titles").
constexpr int kCatalogVersion = 2;
// Valid records beyond this many are ignored (`truncated`): the device lists the
// catalog on a small screen and keeps it in RAM.
constexpr size_t kCatalogMaxEntries = 32;
// `name` + ".tgc" must stay within the Wi-Fi portal's 48-character file-name cap.
constexpr size_t kCatalogMaxNameLen = 43;
// Bytes of UTF-8 in a `title` (the builder's TITLE_MAX_BYTES).
constexpr size_t kCatalogMaxTitleLen = 64;
constexpr size_t kCatalogMaxLangLen = 8;
constexpr size_t kCatalogMaxUrlLen = 1024;
// Same cap as WifiPortal::kMaxPackBytes: the StickS3 filesystem is ~5.9 MB.
constexpr uint32_t kCatalogMaxPackBytes = 5u * 1024u * 1024u;
// A pack is at least its 64-byte header.
constexpr uint32_t kCatalogMinPackBytes = 64u;

struct CatalogEntry {
    std::string name;   // file name without ".tgc"; safe as a /packs file name
    std::string title;  // the pack's deck name ("JLPT N5"), valid UTF-8; may be empty
    std::string lang;   // the pack's meaning language ("en", "es"); may be empty
    uint32_t cards = 0;
    uint32_t bytes = 0;      // exact file size
    uint8_t sha256[32] = {};  // digest of the whole file
    std::string url;         // absolute http(s) URL (bare file names resolved)
};

enum class CatalogError : uint8_t {
    Ok,
    NotACatalog,         // no "#tangocho-catalog<TAB>N" first line (e.g. an HTML error page)
    UnsupportedVersion,  // a format version this firmware does not know
    NoValidEntries,      // records were present but none of them validated
};

struct CatalogParse {
    CatalogError error = CatalogError::Ok;
    int version = 0;          // the version from the tag line, when present
    size_t skipped = 0;       // records ignored as invalid, unsafe or duplicate
    bool truncated = false;   // more than kCatalogMaxEntries valid records
    std::vector<CatalogEntry> entries;
};

// Parses `len` bytes of catalog text. `catalogUrl` is the URL the catalog was
// requested from (before any redirect): a record whose `url` is a bare file name
// is resolved against it, so `.../releases/download/decks-v2/catalog-v2.txt`
// lists `.../releases/download/decks-v2/<file>`. An empty catalog (tag line
// only) is Ok with no entries. On UnsupportedVersion, `version` tells an older
// catalog (below kCatalogVersion: a v1 `catalog.txt`) from a newer one.
CatalogParse parseCatalog(const char* text, size_t len, const char* catalogUrl);

// True for a name the device may save as "/packs/<name>.tgc": 1..43 characters
// from [A-Za-z0-9._-], not starting with '.' (hidden: the pack listers skip it),
// not ending with '.' (the file name would contain ".."), and no "..".
bool catalogNameIsSafe(const char* name, size_t len);

// A usable `title`: at most kCatalogMaxTitleLen bytes of well-formed UTF-8 (no
// overlong forms, surrogates or code points past U+10FFFF) with no control
// character (U+0000..U+001F, U+007F). Empty is fine.
bool catalogTitleIsValid(const char* title, size_t len);

// What a list shows for a deck: its title, or its name when the title is empty.
const std::string& catalogDisplayName(const CatalogEntry& e);

// Exactly 64 hex digits (either case) -> 32 bytes.
bool parseSha256Hex(const char* hex, size_t len, uint8_t out[32]);

// "http://" or "https://" followed by a non-empty host, no whitespace or
// control characters, at most kCatalogMaxUrlLen.
bool isHttpUrl(const std::string& url);

// Resolves a catalog `url` field. An absolute http(s) URL is kept; a bare file
// name (no '/', '\\', ':', '?', '#', whitespace, not "." or "..") replaces the
// last path segment of `base` (query and fragment dropped). Anything else is
// refused. `out` is untouched on failure.
bool resolveCatalogUrl(const std::string& base, const std::string& ref, std::string* out);

const char* catalogErrorString(CatalogError e);

// True when a catalog `lang` is the UI language `uiLang` ("en", "es"): the
// primary subtags ("es" of "es-MX" or "es_419") compared without regard to
// ASCII case. An empty `lang` or `uiLang` never matches.
bool catalogLangMatches(const std::string& lang, const char* uiLang);

// Moves the decks in `uiLang` (catalogLangMatches) to the front and keeps the
// catalog order otherwise (stable): the Get decks list shows the user's own
// language first and still lists every other one.
void orderCatalogForLang(std::vector<CatalogEntry>& entries, const char* uiLang);

// A catalog deck compared with the file of the same name in /packs: not there
// (New), there with the catalog's exact size (OnDevice), or there with another
// size, i.e. an older or newer build (Update). Size only: a same-size file with
// other content is not detected (hashing the local file is a follow-up).
enum class DeckState : uint8_t { New, OnDevice, Update };
DeckState catalogDeckState(bool present, uint32_t localBytes, uint32_t catalogBytes);

}  // namespace tgc
