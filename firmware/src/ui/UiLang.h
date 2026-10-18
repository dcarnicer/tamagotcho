#pragma once
// Maps the stored UI-language preference and the loaded pack's language to the
// two-letter code of the UiStrings table to use, and decides the first-boot
// language picker. Pure C++, no Arduino (the native tests include it through
// -Isrc). Design: docs/decisions.md.
#include <cstring>

namespace tgc {

// NVS `tangocho`/`ui` values. Auto (the default) follows the pack language, so
// the on-device behaviour is unchanged unless the user forces a language.
enum UiLangPref : int { kUiLangAuto = 0, kUiLangEnglish = 1, kUiLangSpanish = 2 };

// Returns "en" or "es". In Auto mode the pack language decides (today's
// behaviour: "es" selects Spanish, anything else English); English/Spanish force
// the table, so an explicit choice survives a pack in another language. Any
// other preference value behaves like Auto.
inline const char* uiLangCodeFor(int pref, const char* packLang) {
    if (pref == kUiLangEnglish) return "en";
    if (pref == kUiLangSpanish) return "es";
    return (packLang && std::strcmp(packLang, "es") == 0) ? "es" : "en";
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
// preference, never Auto, so a Spanish speaker who later downloads an English
// deck keeps Spanish screens. A row outside the list gives English.
constexpr int kFirstBootLangRows = 2;
constexpr int kFirstBootLangDefaultRow = 0;
inline int firstBootLangPref(int row) { return row == 1 ? kUiLangSpanish : kUiLangEnglish; }

}  // namespace tgc
