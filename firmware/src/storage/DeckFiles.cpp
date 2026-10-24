// Deck paths and the two places' decks; see DeckFiles.h.
#include "storage/DeckFiles.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tgc {

std::string decksFolder(DeckPlace p) {
    return p == DeckPlace::Card ? std::string(kCardMount) + kCardDecksDir
                                : std::string(kDeviceDecksDir);
}

std::string deckPath(DeckPlace p, const std::string& file) { return decksFolder(p) + "/" + file; }

DeckPlace placeOf(const std::string& path) {
    const size_t n = std::strlen(kCardMount);
    return path.size() > n && path.compare(0, n, kCardMount) == 0 && path[n] == '/'
               ? DeckPlace::Card
               : DeckPlace::Device;
}

std::string fileNameOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isDeckFileName(const std::string& name) {
    static const std::string kExt = ".tgc";
    return name.size() > kExt.size() && name[0] != '.' &&
           name.compare(name.size() - kExt.size(), kExt.size(), kExt) == 0;
}

std::vector<DeckFile> mergeDecks(const std::vector<DeckFile>& device,
                                 const std::vector<DeckFile>& card) {
    std::vector<DeckFile> out = device;
    for (const DeckFile& c : card) {
        const bool shadowed = std::any_of(device.begin(), device.end(),
                                          [&](const DeckFile& d) { return d.name == c.name; });
        if (!shadowed) out.push_back(c);
    }
    std::sort(out.begin(), out.end(),
              [](const DeckFile& a, const DeckFile& b) { return a.name < b.name; });
    return out;
}

MoveFit moveFit(uint64_t bytes, uint64_t cardFree, uint64_t oldBytes) {
    const uint64_t need = bytes + kCardReserveBytes;
    if (need <= cardFree) return MoveFit::Fits;
    if (oldBytes && need <= cardFree + oldBytes) return MoveFit::FitsInPlaceOfOld;
    return MoveFit::NoRoom;
}

const char* formatGb(uint64_t bytes, char point, char* out, size_t n) {
    const uint64_t tenths = bytes * 10u / (1024ull * 1024ull * 1024ull);  // a card is < 2^60 B
    snprintf(out, n, "%u%c%u", static_cast<unsigned>(tenths / 10u), point,
             static_cast<unsigned>(tenths % 10u));
    return out;
}

}  // namespace tgc
