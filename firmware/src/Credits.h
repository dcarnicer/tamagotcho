#pragma once
// The attribution the device shows (Settings > About) and the portal page
// repeats in its footer: the licences, the firmware's UI font and the data
// sources of the published decks, from docs/data-sources.md -- keep them in
// step. Language-neutral; the
// labels ("Kanji data:") are in each UI's string tables.
//
// "(c)" because the built-in font has no copyright sign (the page swaps in
// one). TGC_NBSP (U+00A0) joins what must not be split over two lines; the
// built-in font draws it as a blank cell (ui/TextLayout.h).
#define TGC_NBSP "\xC2\xA0"

namespace tgc {
namespace credits {

constexpr const char kFirmwareLicence[] = "MIT";
constexpr const char kDecksLicence[] = "CC" TGC_NBSP "BY-SA";
// KANJIDIC2, and JMdict for the decks' reading order (--common-readings) and,
// in v3 decks, their example words, readings and English glosses
// (--example-words): both EDRDG files, whose licence asks for the credit on an
// About screen.
constexpr const char kKanjidic[] =
    "KANJIDIC2, JMdict (c)" TGC_NBSP "EDRDG, CC" TGC_NBSP "BY-SA" TGC_NBSP "4.0";
constexpr const char kKanjiVg[] =
    "KanjiVG (c)" TGC_NBSP "Ulrich" TGC_NBSP "Apel, CC" TGC_NBSP "BY-SA" TGC_NBSP "3.0";
// The kanji's JLPT levels and the vocabulary lists behind the reading order.
constexpr const char kJlpt[] = "Jonathan" TGC_NBSP "Waller (tanos.co.uk), CC" TGC_NBSP "BY";
// The rest of the reading order's sources (docs/common-readings.md), which
// also choose the example words and say which part of a word's reading its
// kanji reads; ASCII so the built-in font can draw it.
constexpr const char kReadings[] =
    "JmdictFurigana, CC" TGC_NBSP "BY-SA; wordfreq (c)" TGC_NBSP "Robyn" TGC_NBSP "Speer, CC"
    TGC_NBSP "BY-SA" TGC_NBSP "4.0; Tatoeba, CC" TGC_NBSP "BY" TGC_NBSP "2.0" TGC_NBSP
    "FR; Joyo kanji table (Agency for Cultural Affairs)";
// The source of the Spanish decks' meaning order (docs/meaning-order.md):
// FreeDict's English-Spanish dictionary, built by WikDict from Wiktionary. It
// also drafted the project's reviewed Spanish example glosses (v3 decks;
// the project's own work is under kDecksLicence), so they need no line of
// their own (docs/data-sources.md, "Example words").
constexpr const char kMeanings[] =
    "FreeDict/WikDict (Wiktionary), CC" TGC_NBSP "BY-SA" TGC_NBSP "3.0";
constexpr const char kGlyphs[] = "Noto" TGC_NBSP "Sans" TGC_NBSP "JP, SIL" TGC_NBSP "OFL" TGC_NBSP "1.1";
// The firmware's own UI font (ui/UiFont.h): glyphs pre-rendered from the
// same font, so the same credit (About shows it with the firmware).
constexpr const char kUiFont[] = "Noto" TGC_NBSP "Sans" TGC_NBSP "JP, SIL" TGC_NBSP "OFL" TGC_NBSP "1.1";
constexpr const char kRepo[] = "github.com/dcarnicer/tamagotcho";

// The app's name (docs/decisions.md, 2026-10-21; formerly Tangochō): in
// hiragana on screen (the UI font has these six kana: the Label face for the
// About title, the Kana face for the 16 px header), romanised where only
// ASCII goes (the serial log, the access point's SSID, the mDNS host).
constexpr const char kAppName[] = "たまごっちょ";
constexpr const char kAppNameLatin[] = "Tamagotcho";

}  // namespace credits
}  // namespace tgc
