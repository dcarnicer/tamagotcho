#pragma once
// Maps the stored UI-language preference and the loaded pack's language to the
// two-letter code of the UiStrings table to use, and decides the first-boot
// language picker. Pure C++, no Arduino (the native tests include it through
// -Isrc). Design: docs/decisions.md.
#include <cstring>

namespace tgc {

// NVS `tangocho`/`ui` values. Auto (the default) follows the pack language, so
// the on-device behaviour is unchanged unless the user forces a language.
enum UiLangPref : int {
    kUiLangAuto = 0,
    kUiLangEnglish = 1,
    kUiLangSpanish = 2,
    kUiLangFrench = 3,
    kUiLangPortuguese = 4,
};

// Returns "en", "es", "fr" or "pt". In Auto mode the pack language decides
// ("es"/"fr"/"pt" select their table; anything else English). An explicit
// choice survives a pack in another language. Any other preference value
// behaves like Auto.
inline const char* uiLangCodeFor(int pref, const char* packLang) {
    switch (pref) {
        case kUiLangEnglish: return "en";
        case kUiLangSpanish: return "es";
        case kUiLangFrench: return "fr";
        case kUiLangPortuguese: return "pt";
        default: break;
    }
    if (packLang && std::strcmp(packLang, "es") == 0) return "es";
    if (packLang && std::strcmp(packLang, "fr") == 0) return "fr";
    if (packLang && std::strcmp(packLang, "pt") == 0) return "pt";
    return "en";
}

// The one-time first-boot language picker ("First-boot language picker" in
// docs/decisions.md): shown only when the `ui` preference has never been stored
// (absent, which is not the same as Auto) and the device holds no deck -- a
// brand-new or fully erased device, whose first screens would otherwise all be
// English. A device that still has decks (a merged-image reflash erases only
// the NVS) keeps Auto and follows its deck, as before; a stored preference,
// Auto included, never asks again.
inline bool firstBootLangPicker(bool prefStored, bool hasDecks) {
    return !prefStored && !hasDecks;
}

// The picker's rows, top to bottom, and the one highlighted at first: English,
// the default that needs no browser to guess it. A row maps to an explicit
// preference, never Auto, so a speaker of another language who later downloads
// an English deck keeps their screens. A row outside the list gives English.
constexpr int kFirstBootLangRows = 4;
constexpr int kFirstBootLangDefaultRow = 0;
inline int firstBootLangPref(int row) {
    switch (row) {
        case 1: return kUiLangSpanish;
        case 2: return kUiLangFrench;
        case 3: return kUiLangPortuguese;
        default: return kUiLangEnglish;
    }
}

}  // namespace tgc
