#pragma once
// The attribution the device shows (Settings > About) and the portal page
// repeats in its footer: the licences and the data sources of the published
// decks, from docs/data-sources.md -- keep them in step. Language-neutral; the
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
constexpr const char kKanjidic[] = "KANJIDIC2 (c)" TGC_NBSP "EDRDG, CC" TGC_NBSP "BY-SA" TGC_NBSP "4.0";
constexpr const char kKanjiVg[] =
    "KanjiVG (c)" TGC_NBSP "Ulrich" TGC_NBSP "Apel, CC" TGC_NBSP "BY-SA" TGC_NBSP "3.0";
constexpr const char kJlpt[] = "Jonathan" TGC_NBSP "Waller (tanos.co.uk), CC" TGC_NBSP "BY";
constexpr const char kGlyphs[] = "Noto" TGC_NBSP "Sans" TGC_NBSP "JP, SIL" TGC_NBSP "OFL" TGC_NBSP "1.1";
constexpr const char kRepo[] = "github.com/dcarnicer/tangocho";

}  // namespace credits
}  // namespace tgc
