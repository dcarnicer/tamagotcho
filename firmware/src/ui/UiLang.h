#pragma once
// Maps the stored UI-language preference and the loaded pack's language to the
// two-letter code of the UiStrings table to use. Pure C++, no Arduino (the
// native tests include it through -Isrc). Design: docs/decisions.md.
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

}  // namespace tgc
