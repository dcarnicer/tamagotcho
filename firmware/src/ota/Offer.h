#pragma once
// Whether the device reads a firmware manifest at all, and whether it offers
// the image the manifest lists for its build (docs/ota.md §3.2, §3.3, §3.5).
// Pure C++, no Arduino: unit tested natively in test/test_ota.
//
// A release build (its version is a plain vX.Y.Z) reads the default manifest
// and offers only a strictly newer release. A development build (any other
// version: `git describe` output, "dev") never reads the default manifest,
// whose image would drop its serial remote or test code: only an overridden
// manifest URL, and then any version other than its own, since a dev build has
// no order to compare. Either way a version the device rolled back from is not
// offered again, a manifest with no row for the build's environment offers
// nothing, and neither does an image larger than the device's update slot.
#include <cstdint>

#include "ota/Manifest.h"

namespace tgc {

// A release build: its version parses as vX.Y.Z (parseFirmwareVersion()).
bool isReleaseBuild(const char* runningVersion);

// Whether this build reads a manifest: a release build always, a development
// build only from an overridden URL.
bool manifestReadAllowed(const char* runningVersion, bool urlOverridden);

enum class OfferVerdict : uint8_t {
    Offer,        // install `app`
    UpToDate,     // the manifest's version is not newer (release) or is the running one (dev)
    NotAllowed,   // a development build with the default URL (manifestReadAllowed())
    RolledBack,   // the version the device rolled back from; never offered again
    NoImage,      // the manifest has no row for this build's environment
    TooLarge,     // the image does not fit the update slot
    BadManifest,  // the manifest did not parse
};

struct OfferInput {
    const char* running = "";     // tgc::firmwareVersion()
    const char* env = "";         // the build environment (TGC_ENV)
    bool urlOverridden = false;   // the manifest URL is the user's, not the default
    const char* rolledBack = "";  // the version remembered as rolled back, "" for none
    uint32_t slotBytes = 0;       // the update slot's size (0: no slot)
};

struct Offer {
    OfferVerdict verdict = OfferVerdict::BadManifest;
    const ManifestApp* app = nullptr;  // this build's row (Offer and TooLarge), into the parse
};

// The verdict for manifest `m` on this device. Checked in this order: the
// manifest parsed, the build may read one, the version (newer, or other for a
// dev build), not rolled back, a row for the environment, the size.
Offer offerFor(const ManifestParse& m, const OfferInput& in);

const char* offerVerdictString(OfferVerdict v);

}  // namespace tgc
