#pragma once
// The settings tree of the UX refresh (docs/ux-refresh.md §3 as amended by
// §6.6): a summary card and five groups on the home -- Decks, Study, Stats,
// Wi-Fi, Device -- of which Study and Device are pages of rows themselves (a
// one-level group stack). This header is the structure: which rows a page
// has, in which order, which open a page or another screen, the home's icons,
// and how the row values read. main.cpp maps each item to its label, value and
// action (its MenuItem table). New settings go into a group, never onto the
// home. Pure C++, no Arduino (native-tested in test_settings).
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ui/Icons.h"

namespace tgc {
namespace settings {

// The pages of the tree: the home and the two groups whose rows are settings.
enum class Page : uint8_t { Home, Study, Device };
constexpr int kPageCount = 3;

// Every row of every page. The values index main.cpp's MenuItem table.
enum class Item : uint8_t {
    // home
    Decks,
    Study,
    Stats,
    Wifi,
    Device,
    // Study ›
    NewPerDay,
    HardAfter,
    AutoAdvance,
    // Device ›
    Orientation,
    Brightness,
    Language,
    HoldTime,
    Date,
    Sleep,
    About,
};
constexpr int kItemCount = 15;

inline constexpr Item kHomeItems[] = {Item::Decks, Item::Study, Item::Stats, Item::Wifi,
                                      Item::Device};
inline constexpr Item kStudyItems[] = {Item::NewPerDay, Item::HardAfter, Item::AutoAdvance};
inline constexpr Item kDeviceItems[] = {Item::Orientation, Item::Brightness, Item::Language,
                                        Item::HoldTime,    Item::Date,       Item::Sleep,
                                        Item::About};

struct PageItems {
    const Item* items;
    int count;
};

template <size_t N>
constexpr PageItems pageItemsOf(const Item (&items)[N]) {
    return PageItems{items, static_cast<int>(N)};
}

constexpr PageItems pageItems(Page p) {
    return p == Page::Study    ? pageItemsOf(kStudyItems)
           : p == Page::Device ? pageItemsOf(kDeviceItems)
                               : pageItemsOf(kHomeItems);
}

// Row `row` of page `p` (clamped into the page).
constexpr Item itemAt(Page p, int row) {
    const PageItems pi = pageItems(p);
    return pi.items[row < 0 ? 0 : (row >= pi.count ? pi.count - 1 : row)];
}

// Index of item `i` on page `p`, -1 when the page does not have it.
constexpr int rowOf(Page p, Item i) {
    const PageItems pi = pageItems(p);
    for (int r = 0; r < pi.count; ++r)
        if (pi.items[r] == i) return r;
    return -1;
}

// The page a group row opens: Study and Device are pages of the tree; every
// other group opens a screen of its own (the deck list, statistics, the Wi-Fi
// submenu).
constexpr bool opensPage(Item i, Page* out) {
    if (i == Item::Study) {
        *out = Page::Study;
        return true;
    }
    if (i == Item::Device) {
        *out = Page::Device;
        return true;
    }
    return false;
}

// The page a row lives on (each item is on exactly one).
constexpr Page pageOf(Item i) {
    return rowOf(Page::Home, i) >= 0    ? Page::Home
           : rowOf(Page::Study, i) >= 0 ? Page::Study
                                        : Page::Device;
}

// A chevron marks the rows that open a page, a screen or a picker; "Sleep"
// acts at once.
constexpr bool hasChevron(Item i) { return i != Item::Sleep; }

// The home's group icons (9x9); the rows inside a group have none.
constexpr const Icon* itemIcon(Item i) {
    return i == Item::Decks    ? &kIconCards
           : i == Item::Study  ? &kIconBook
           : i == Item::Stats  ? &kIconChart
           : i == Item::Wifi   ? &kIconWifi9
           : i == Item::Device ? &kIconGear
                               : nullptr;
}

// ---------------------------------------------------------------- values

// Accuracy in percent over every grade ever recorded, (reps - lapses) / reps
// rounded down (lapses never exceed reps; a saturated count reads as 0 %).
// -1 when nothing was answered yet.
constexpr int accuracyPercent(uint32_t reps, uint32_t lapses) {
    return reps == 0 ? -1
                     : static_cast<int>(
                           static_cast<uint64_t>(reps > lapses ? reps - lapses : 0) * 100u / reps);
}

// Milliseconds as seconds with at most one decimal: "1.5 s", "3 s", "0.4 s";
// `point` is the language's decimal separator ("." or ",").
inline void formatSeconds(uint32_t ms, const char* point, char* out, size_t n) {
    const unsigned tenths = static_cast<unsigned>((ms + 50u) / 100u);
    if (tenths % 10u == 0)
        snprintf(out, n, "%u s", tenths / 10u);
    else
        snprintf(out, n, "%u%s%u s", tenths / 10u, point ? point : ".", tenths % 10u);
}

// "18 Oct": the day of the month and the month's short name.
inline void formatDayMonth(unsigned day, const char* month, char* out, size_t n) {
    snprintf(out, n, "%u %s", day, month ? month : "");
}

// The day streak as it stands today (docs/srs.md section 4, "Streak"): the
// store keeps the last streak until the next answer, which resets it to 1
// when a study day was skipped, so a streak whose last study day is before
// yesterday already reads 0. Nothing studied yet is 0 as well.
constexpr uint32_t liveStreak(uint32_t stored, bool hasStudied, uint16_t lastStudyDay,
                              uint16_t today) {
    return !hasStudied || today > lastStudyDay + 1u ? 0u : stored;
}

}  // namespace settings
}  // namespace tgc
