// The update rule. Pure C++; see Offer.h.
#include "ota/Offer.h"

#include <cstring>

namespace tgc {

bool isReleaseBuild(const char* runningVersion) {
    FirmwareVersion v;
    return runningVersion && parseFirmwareVersion(runningVersion, std::strlen(runningVersion), &v);
}

bool manifestReadAllowed(const char* runningVersion, bool urlOverridden) {
    return urlOverridden || isReleaseBuild(runningVersion);
}

Offer offerFor(const ManifestParse& m, const OfferInput& in) {
    Offer o;
    if (m.error != ManifestError::Ok) return o;  // BadManifest
    const char* running = in.running ? in.running : "";
    if (!manifestReadAllowed(running, in.urlOverridden)) {
        o.verdict = OfferVerdict::NotAllowed;
        return o;
    }
    FirmwareVersion current;
    if (parseFirmwareVersion(running, std::strlen(running), &current)) {
        if (compareFirmwareVersions(m.release, current) <= 0) {
            o.verdict = OfferVerdict::UpToDate;
            return o;
        }
    } else if (m.version == running) {
        o.verdict = OfferVerdict::UpToDate;
        return o;
    }
    if (in.rolledBack && *in.rolledBack && m.version == in.rolledBack) {
        o.verdict = OfferVerdict::RolledBack;
        return o;
    }
    o.app = manifestAppFor(m, in.env);
    if (!o.app) {
        o.verdict = OfferVerdict::NoImage;
        return o;
    }
    o.verdict = o.app->bytes > in.slotBytes ? OfferVerdict::TooLarge : OfferVerdict::Offer;
    return o;
}

const char* offerVerdictString(OfferVerdict v) {
    switch (v) {
        case OfferVerdict::Offer: return "offer";
        case OfferVerdict::UpToDate: return "up to date";
        case OfferVerdict::NotAllowed: return "development build, default manifest";
        case OfferVerdict::RolledBack: return "rolled back before";
        case OfferVerdict::NoImage: return "no image for this build";
        case OfferVerdict::TooLarge: return "image larger than the update slot";
        case OfferVerdict::BadManifest: return "bad manifest";
    }
    return "?";
}

}  // namespace tgc
