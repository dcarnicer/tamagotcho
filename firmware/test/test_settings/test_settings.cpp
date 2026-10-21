// Native tests for the settings tree and the secondary screens of the UX
// refresh (docs/ux-refresh.md §3, §6): the page tables and row values
// (ui/SettingsTree.h), the screen geometry for every panel
// (ui/ScreenLayout.h), the loaded deck's counts (ui/DeckSummary.h) and the
// pack header peek the deck list reads names with (peekPackInfo).
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "pack/MemorySource.h"
#include "pack/PackReader.h"
#include "srs/DeckProgress.h"
#include "srs/SrsStore.h"
#include "ui/DeckSummary.h"
#include "ui/ScreenLayout.h"
#include "ui/SettingsTree.h"
#include "ui/Theme.h"

using namespace tgc;
using settings::Item;
using settings::Page;

void setUp() {}
void tearDown() {}

namespace {

// ------------------------------------------------- in-memory pack builder
// A minimal valid v1 pack (docs/pack-format.md): `cps` cards with one meaning
// each, no distractors, no glyphs, the given deck name and language.
struct B {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) {
        u8(x & 255);
        u8(x >> 8);
    }
    void u32(uint32_t x) {
        u16(x & 0xFFFF);
        u16(x >> 16);
    }
};

std::vector<uint8_t> makeDeck(const std::vector<uint32_t>& cps, const char* deckName,
                              const char* lang = "en") {
    std::string pool;
    auto addStr = [&](const char* s) {
        const uint32_t off = static_cast<uint32_t>(pool.size());
        pool += s;
        pool.push_back('\0');
        return off;
    };
    const uint32_t deckRel = addStr(deckName);
    const uint32_t meanRel = addStr("meaning");
    const uint32_t emptyRel = addStr("");
    while (pool.size() % 4) pool.push_back('\0');
    const uint32_t n = static_cast<uint32_t>(cps.size());
    const uint32_t cardsOff = 64;
    const uint32_t glyphsOff = cardsOff + n * 32;
    const uint32_t distOff = glyphsOff;  // no glyphs
    const uint32_t strOff = (distOff + 2 * n + 3) & ~3u;
    const uint32_t gdOff = strOff + static_cast<uint32_t>(pool.size());

    B b;
    b.u8('T');
    b.u8('N');
    b.u8('G');
    b.u8('C');
    b.u16(1);
    b.u16(64);
    b.u32(n);
    b.u32(0);
    b.u32(cardsOff);
    b.u32(glyphsOff);
    b.u32(distOff);
    b.u32(strOff);
    b.u32(static_cast<uint32_t>(pool.size()));
    b.u32(gdOff);
    b.u32(strOff + deckRel);
    for (int i = 0; i < 4; ++i) b.u8(i < static_cast<int>(std::strlen(lang)) ? lang[i] : 0);
    b.u8(32);
    b.u8(16);
    b.u8(12);
    b.u8(18);
    b.u8(14);
    b.u8(0);
    b.u8(0);
    b.u8(0);
    b.u32(0);
    b.u32(0);
    for (uint32_t i = 0; i < n; ++i) {
        b.u32(cps[i]);
        b.u32(strOff + meanRel);
        b.u32(strOff + emptyRel);
        b.u32(strOff + emptyRel);
        b.u32(distOff + 2 * i);
        b.u8(5);
        b.u8(1);
        b.u8(4);
        b.u8(0);
        b.u16(10);
        b.u16(static_cast<uint16_t>(i));
        b.u32(0);
    }
    for (uint32_t i = 0; i < n; ++i) b.u16(0);  // empty distractor lists
    while (b.v.size() % 4) b.u8(0);
    for (char ch : pool) b.u8(static_cast<uint8_t>(ch));
    return b.v;
}

bool inside(const layout::Rect& r, int w, int top, int bottom) {
    return r.x >= 0 && r.y >= top && r.w > 0 && r.h > 0 && r.right() <= w && r.bottom() <= bottom;
}

bool overlap(const layout::Rect& a, const layout::Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

struct Panel {
    int w, h;
};
// StickS3 portrait / landscape, T-Display-S3 portrait / landscape.
const Panel kPanels[] = {{135, 240}, {240, 135}, {170, 320}, {320, 170}};

}  // namespace

// ---------------------------------------------------------------- tree

void test_tree_pages() {
    // §6.6: the home's five groups; Study and Device are pages of rows.
    const settings::PageItems home = settings::pageItems(Page::Home);
    TEST_ASSERT_EQUAL(5, home.count);
    TEST_ASSERT_TRUE(home.items[0] == Item::Decks);
    TEST_ASSERT_TRUE(home.items[1] == Item::Study);
    TEST_ASSERT_TRUE(home.items[2] == Item::Stats);
    TEST_ASSERT_TRUE(home.items[3] == Item::Wifi);
    TEST_ASSERT_TRUE(home.items[4] == Item::Device);
    const settings::PageItems study = settings::pageItems(Page::Study);
    TEST_ASSERT_EQUAL(5, study.count);
    TEST_ASSERT_TRUE(study.items[0] == Item::NewPerDay);
    TEST_ASSERT_TRUE(study.items[1] == Item::HardAfter);
    TEST_ASSERT_TRUE(study.items[2] == Item::AutoAdvance);
    TEST_ASSERT_TRUE(study.items[3] == Item::OnKana);
    TEST_ASSERT_TRUE(study.items[4] == Item::DeleteProgress);
    const settings::PageItems device = settings::pageItems(Page::Device);
    TEST_ASSERT_EQUAL(7, device.count);
    TEST_ASSERT_TRUE(device.items[0] == Item::Orientation);
    TEST_ASSERT_TRUE(device.items[3] == Item::HoldTime);
    TEST_ASSERT_TRUE(device.items[4] == Item::Date);
    TEST_ASSERT_TRUE(device.items[6] == Item::About);
}

void test_tree_fixed_orientation_has_no_orientation_row() {
    // A landscape-only board (the Cardputer ADV): the Device page without
    // Orientation, everything else as it was, in the same order.
    const settings::PageItems all = settings::pageItems(Page::Device);
    const settings::PageItems fixed = settings::pageItems(Page::Device, true);
    TEST_ASSERT_EQUAL(all.count - 1, fixed.count);
    for (int r = 0; r < fixed.count; ++r) {
        TEST_ASSERT_TRUE(fixed.items[r] == all.items[r + 1]);
        TEST_ASSERT_TRUE(settings::itemAt(Page::Device, r, true) == all.items[r + 1]);
    }
    TEST_ASSERT_TRUE(settings::itemAt(Page::Device, 0, true) == Item::Brightness);
    TEST_ASSERT_TRUE(settings::itemAt(Page::Device, 99, true) == Item::About);
    // The other pages do not change.
    TEST_ASSERT_EQUAL(settings::pageItems(Page::Home).count,
                      settings::pageItems(Page::Home, true).count);
    TEST_ASSERT_EQUAL(settings::pageItems(Page::Study).count,
                      settings::pageItems(Page::Study, true).count);
}

void test_tree_every_item_once() {
    // Each item is on exactly one page, and rowOf / itemAt / pageOf agree.
    int seen[settings::kItemCount] = {};
    for (int p = 0; p < settings::kPageCount; ++p) {
        const Page page = static_cast<Page>(p);
        const settings::PageItems pi = settings::pageItems(page);
        for (int r = 0; r < pi.count; ++r) {
            const Item it = pi.items[r];
            ++seen[static_cast<int>(it)];
            TEST_ASSERT_EQUAL(r, settings::rowOf(page, it));
            TEST_ASSERT_TRUE(settings::itemAt(page, r) == it);
            TEST_ASSERT_TRUE(settings::pageOf(it) == page);
        }
    }
    for (int i = 0; i < settings::kItemCount; ++i) TEST_ASSERT_EQUAL(1, seen[i]);
    TEST_ASSERT_EQUAL(-1, settings::rowOf(Page::Home, Item::Sleep));
    // Out of range rows clamp.
    TEST_ASSERT_TRUE(settings::itemAt(Page::Study, 9) == Item::DeleteProgress);
    TEST_ASSERT_TRUE(settings::itemAt(Page::Study, -1) == Item::NewPerDay);
}

void test_tree_groups_and_marks() {
    Page p = Page::Home;
    TEST_ASSERT_TRUE(settings::opensPage(Item::Study, &p));
    TEST_ASSERT_TRUE(p == Page::Study);
    TEST_ASSERT_TRUE(settings::opensPage(Item::Device, &p));
    TEST_ASSERT_TRUE(p == Page::Device);
    TEST_ASSERT_FALSE(settings::opensPage(Item::Decks, &p));
    TEST_ASSERT_FALSE(settings::opensPage(Item::NewPerDay, &p));
    // Every row but Sleep opens something.
    for (int i = 0; i < settings::kItemCount; ++i) {
        const Item it = static_cast<Item>(i);
        TEST_ASSERT_EQUAL(it != Item::Sleep, settings::hasChevron(it));
        // Icons only on the home's group rows.
        TEST_ASSERT_EQUAL(settings::pageOf(it) == Page::Home, settings::itemIcon(it) != nullptr);
    }
    TEST_ASSERT_EQUAL_PTR(&kIconGear, settings::itemIcon(Item::Device));
    TEST_ASSERT_EQUAL_PTR(&kIconCards, settings::itemIcon(Item::Decks));
}

void test_values() {
    TEST_ASSERT_EQUAL(-1, settings::accuracyPercent(0, 0));
    TEST_ASSERT_EQUAL(87, settings::accuracyPercent(100, 13));
    TEST_ASSERT_EQUAL(66, settings::accuracyPercent(3, 1));  // rounded down
    TEST_ASSERT_EQUAL(100, settings::accuracyPercent(5, 0));
    TEST_ASSERT_EQUAL(0, settings::accuracyPercent(5, 5));
    TEST_ASSERT_EQUAL(0, settings::accuracyPercent(5, 9));  // saturated
    TEST_ASSERT_EQUAL(99, settings::accuracyPercent(0xFFFFFFFFu, 0xFFFFFFFFu / 100u));

    char buf[16];
    settings::formatSeconds(1500, ".", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("1.5 s", buf);
    settings::formatSeconds(3000, ".", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("3 s", buf);
    settings::formatSeconds(400, ",", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("0,4 s", buf);
    settings::formatSeconds(600, ".", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("0.6 s", buf);
    settings::formatSeconds(1000, ".", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("1 s", buf);
    settings::formatSeconds(899, ".", buf, sizeof buf);  // rounded to the tenth
    TEST_ASSERT_EQUAL_STRING("0.9 s", buf);

    settings::formatDayMonth(18, "Oct", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("18 Oct", buf);
    settings::formatDayMonth(1, "ene", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("1 ene", buf);
}

void test_live_streak() {
    // Nothing studied yet: 0, whatever the field says.
    TEST_ASSERT_EQUAL_UINT32(0, settings::liveStreak(3, false, 10, 10));
    // Studied today or yesterday: the stored streak still counts.
    TEST_ASSERT_EQUAL_UINT32(3, settings::liveStreak(3, true, 10, 10));
    TEST_ASSERT_EQUAL_UINT32(3, settings::liveStreak(3, true, 10, 11));
    // A day skipped: the next answer restarts it, so it reads 0 now.
    TEST_ASSERT_EQUAL_UINT32(0, settings::liveStreak(3, true, 10, 12));
    // Day index 0 is a real study day.
    TEST_ASSERT_EQUAL_UINT32(1, settings::liveStreak(1, true, 0, 1));
    TEST_ASSERT_EQUAL_UINT32(0, settings::liveStreak(1, true, 0, 2));
    // No overflow at the end of the day range.
    TEST_ASSERT_EQUAL_UINT32(2, settings::liveStreak(2, true, 0xFFFF, 0xFFFF));
}

// ---------------------------------------------------------------- layouts

void test_layout_home() {
    using namespace theme;
    for (const Panel& p : kPanels) {
        const layout::Home l = layout::homeFor(p.w, p.h);
        const Layout g = layoutFor(p.w, p.h);
        TEST_ASSERT_TRUE(inside(l.card, p.w, kHeaderH, p.h - kHintH));
        TEST_ASSERT_TRUE(l.rowsX >= 0 && l.rowsX + l.rowsW <= p.w - kGutter + 1);
        TEST_ASSERT_TRUE(l.rowsTop >= kHeaderH && l.rowsBottom <= p.h - kHintH);
        // How many group rows fit the band (layoutList keeps 4 px marker
        // bands above and below).
        const int band = l.rowsBottom - l.rowsTop - 2 * kMarkerBand;
        const int fit = (band + kGap) / (g.row1H + kGap);
        if (p.w > p.h) {
            TEST_ASSERT_TRUE(l.cardColumn);
            TEST_ASSERT_TRUE(l.card.right() < l.rowsX);  // the card is left of the rows
            TEST_ASSERT_TRUE(fit >= 4);
            // Room for "Estudio  10/día" next to an icon and a chevron.
            TEST_ASSERT_TRUE(l.rowsW >= 144);
        } else {
            TEST_ASSERT_FALSE(l.cardColumn);
            TEST_ASSERT_TRUE(l.card.bottom() <= l.rowsTop);  // the card is above the rows
            TEST_ASSERT_TRUE(fit >= 5);  // all five groups without scrolling
        }
    }
}

void test_layout_stats() {
    using namespace theme;
    for (const Panel& p : kPanels) {
        const layout::Stats s = layout::statsFor(p.w, p.h);
        const int hintY = p.h - kHintH;
        // The ring inside the screen, under the header.
        TEST_ASSERT_TRUE(s.rOuter >= 30 && s.rOuter <= layout::kRingMaxR);
        TEST_ASSERT_TRUE(s.rInner > 0 && s.rInner < s.rOuter);
        TEST_ASSERT_TRUE(s.cx - s.rOuter >= 0 && s.cx + s.rOuter <= p.w);
        TEST_ASSERT_TRUE(s.cy - s.rOuter >= kHeaderH);
        TEST_ASSERT_TRUE(s.captionY >= s.cy + s.rOuter);
        // "87" in the numeral plus "%" in the label face fits the hole
        // (36 + 17 px).
        TEST_ASSERT_TRUE(layout::ringFits(53, s.rInner));
        // Tiles: inside, apart from each other and from the ring.
        for (int i = 0; i < 6; ++i) {
            TEST_ASSERT_TRUE(inside(s.tiles[i], p.w, kHeaderH, hintY));
            TEST_ASSERT_TRUE(s.tiles[i].h >= 32);
            TEST_ASSERT_TRUE(s.tiles[i].w >= 62);  // "dominadas" (54 px) and 4 px a side
            for (int j = i + 1; j < 6; ++j) TEST_ASSERT_FALSE(overlap(s.tiles[i], s.tiles[j]));
            const layout::Rect ring =
                layout::rect(s.cx - s.rOuter, s.cy - s.rOuter, 2 * s.rOuter, 2 * s.rOuter);
            TEST_ASSERT_FALSE(overlap(s.tiles[i], ring));
            TEST_ASSERT_FALSE(overlap(s.tiles[i], s.seen));
        }
        TEST_ASSERT_TRUE(inside(s.seen, p.w, kHeaderH, hintY));
        TEST_ASSERT_EQUAL(p.w > p.h, s.seenStacked);
        if (p.w <= p.h) TEST_ASSERT_TRUE(s.seen.y >= s.tiles[5].bottom());
    }
    // Ring sweep: from 12 o'clock, clockwise; clamped.
    TEST_ASSERT_EQUAL_FLOAT(270.0f, layout::ringEndDeg(0));
    TEST_ASSERT_EQUAL_FLOAT(270.0f + 180.0f, layout::ringEndDeg(50));
    TEST_ASSERT_EQUAL_FLOAT(270.0f + 360.0f, layout::ringEndDeg(100));
    TEST_ASSERT_EQUAL_FLOAT(270.0f + 360.0f, layout::ringEndDeg(140));
    TEST_ASSERT_EQUAL_FLOAT(270.0f, layout::ringEndDeg(-3));
    TEST_ASSERT_TRUE(layout::ringFits(50, 28));
    TEST_ASSERT_FALSE(layout::ringFits(51, 28));
}

void test_layout_date() {
    using namespace theme;
    // The hint bar is one row, or two where the side button's -1 day and
    // cancel do not fit next to the front items (widgets::hintBarHeight()).
    for (const Panel& p : kPanels) {
        for (int hintH : {kHintH, 2 * kHintH}) {
            const layout::Date d = layout::dateFor(p.w, p.h, hintH);
            const int hintY = p.h - hintH;
            TEST_ASSERT_TRUE(inside(d.tile, p.w, kHeaderH, hintY));
            TEST_ASSERT_TRUE(inside(d.notice, p.w, kHeaderH, hintY));
            TEST_ASSERT_TRUE(inside(d.explain, p.w, kHeaderH, hintY));
            TEST_ASSERT_TRUE(d.tile.w >= 100);  // "Wednesday" in the label face is 96 px
            TEST_ASSERT_EQUAL(layout::kDateTileH, d.tile.h);
            TEST_ASSERT_TRUE(d.bandH < d.tile.h);
            // The pill under the tile and above the hint bar.
            TEST_ASSERT_TRUE(d.pillY >= d.tile.bottom());
            TEST_ASSERT_TRUE(d.pillY + kPillH <= hintY);
            TEST_ASSERT_TRUE(d.pillCx > d.tile.x && d.pillCx < d.tile.right());
            TEST_ASSERT_FALSE(overlap(d.tile, d.notice));
            TEST_ASSERT_FALSE(overlap(d.tile, d.explain));
            TEST_ASSERT_FALSE(overlap(d.notice, d.explain));
            // At least five caption lines for the explanation: a network
            // status (two lines), a blank one and "Hold when it shows today."
            // (two lines in 21 characters).
            TEST_ASSERT_TRUE(d.explain.h >= 50);
            // Two caption lines of the notice: "Último uso sáb 17 oct" is 21
            // characters (126 px).
            TEST_ASSERT_TRUE(d.notice.w >= 126 && d.notice.h >= 20);
            if (p.w <= p.h) TEST_ASSERT_TRUE(d.explain.y >= d.pillY + kPillH);
        }
    }
}

void test_layout_done() {
    using namespace theme;
    for (const Panel& p : kPanels) {
        const layout::Done d = layout::doneFor(p.w, p.h);
        const int hintY = p.h - kHintH;
        TEST_ASSERT_TRUE(inside(d.button, p.w, kHeaderH, hintY));
        TEST_ASSERT_TRUE(inside(d.title, p.w, kHeaderH, hintY));
        TEST_ASSERT_TRUE(d.iconCy - d.iconR >= kHeaderH);
        TEST_ASSERT_TRUE(d.iconCx - d.iconR >= 0 && d.iconCx + d.iconR <= p.w);
        // The two caption lines end above the button.
        TEST_ASSERT_TRUE(d.line2Y + 8 <= d.button.y);
        TEST_ASSERT_TRUE(d.line1Y >= d.title.bottom());
        TEST_ASSERT_TRUE(d.textX >= 0 && d.textX + d.textW <= p.w);
        // "All caught up" (109 px) fits the text column.
        TEST_ASSERT_TRUE(d.textW >= 110);
        if (p.w > p.h) {
            TEST_ASSERT_TRUE(d.textX > d.iconCx + d.iconR);  // circle left of the text
        } else {
            TEST_ASSERT_TRUE(d.title.y >= d.iconCy + d.iconR);  // circle above the text
        }
    }
}

// ---------------------------------------------------------------- deck counts

void test_deck_summary() {
    const std::vector<uint8_t> buf = makeDeck({0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94}, "JLPT N5");
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(pack.open(src)));

    SrsStore srs;
    srs.setToday(10);
    DeckSummary s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(5, s.total);
    TEST_ASSERT_EQUAL_UINT32(5, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(0, s.seen());
    TEST_ASSERT_EQUAL_UINT32(0, s.due);

    srs.getOrCreate(0x4E00).dueDay = 9;    // overdue
    srs.getOrCreate(0x4E8C).dueDay = 10;   // due today
    srs.getOrCreate(0x4E09).dueDay = 11;   // due tomorrow
    srs.getOrCreate(0x56DB).dueDay = 30;   // later
    srs.getOrCreate(0x4E94).dueDay = 0;    // a record never scheduled: still new
    srs.getOrCreate(0x9999).dueDay = 1;    // another deck's card: not counted
    s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(5, s.total);
    TEST_ASSERT_EQUAL_UINT32(1, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(4, s.seen());
    TEST_ASSERT_EQUAL_UINT32(2, s.due);
    TEST_ASSERT_EQUAL_UINT32(3, s.dueTomorrow);

    PackReader closed;
    s = deckSummary(closed, srs);
    TEST_ASSERT_EQUAL_UINT32(0, s.total);
    TEST_ASSERT_EQUAL_UINT32(0, s.seen());

    // Introduced today: a first grade today counts, another deck's card does not.
    TEST_ASSERT_EQUAL_UINT32(0, deckSummary(pack, srs).introducedToday);
    srs.getOrCreate(0x4E94).dueDay = 0;
    srs.grade(0x4E94, Grade::Good, 10, 0, 0.5f);
    srs.grade(0x9998, Grade::Good, 10, 0, 0.5f);
    TEST_ASSERT_EQUAL_UINT32(1, deckSummary(pack, srs).introducedToday);

    // Delete progress > This deck (docs/srs.md section 2.1): the deck's kanji go
    // back to new, its cap of new cards for today is free again, and another
    // deck's record stays.
    std::vector<uint32_t> cps;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok),
                      static_cast<int>(readPackCodepoints(src, &cps)));
    sortUnique(cps);
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(srs.forget(cps.data(), cps.size())));
    s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(5, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(0, s.seen());
    TEST_ASSERT_EQUAL_UINT32(0, s.due);
    TEST_ASSERT_EQUAL_UINT32(0, s.introducedToday);
    TEST_ASSERT_NOT_NULL(srs.find(0x9999));
    // Everything: no records left at all; the accuracy shows "--" again.
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(srs.resetProgress()));
    TEST_ASSERT_EQUAL(-1, settings::accuracyPercent(srs.stats().answers, srs.stats().again));
}

void test_study_more_extra() {
    // Cap 10, 10 introduced, no extra: 10 more.
    TEST_ASSERT_EQUAL_UINT16(10, studyMoreExtra(10, 10, 0));
    // A second "+10" on the same deck: 20.
    TEST_ASSERT_EQUAL_UINT16(20, studyMoreExtra(10, 20, 10));
    // Back on a deck that used "+10" earlier today (the extra reset to 0):
    // still 10 beyond the 20 it introduced.
    TEST_ASSERT_EQUAL_UINT16(20, studyMoreExtra(10, 20, 0));
    // Cap not reached (e.g. the deck ran out of other work): the extra grows by 10.
    TEST_ASSERT_EQUAL_UINT16(10, studyMoreExtra(10, 4, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, studyMoreExtra(10, 70000, 0));
}

// ---------------------------------------------------------------- pack peek

void test_peek_pack_info() {
    const std::vector<uint8_t> buf = makeDeck({0x4E00, 0x4E8C, 0x4E09}, "JLPT N5", "es");
    MemorySource src(buf);
    PackInfo info;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(peekPackInfo(src, &info)));
    TEST_ASSERT_EQUAL_STRING("JLPT N5", info.name);
    TEST_ASSERT_EQUAL_STRING("es", info.lang);
    TEST_ASSERT_EQUAL_UINT32(3, info.cards);
    TEST_ASSERT_EQUAL_UINT16(1, info.version);
    // The same pack opens; the peek agrees with the reader.
    PackReader r;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(r.open(src)));
    TEST_ASSERT_EQUAL_STRING(r.deckName(), info.name);

    // A broken header fails like open() does, and leaves an empty name.
    std::vector<uint8_t> bad = buf;
    bad[0] = 'X';
    MemorySource badSrc(bad);
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::BadMagic),
                      static_cast<int>(peekPackInfo(badSrc, &info)));
    TEST_ASSERT_EQUAL_STRING("", info.name);
    std::vector<uint8_t> shortFile(buf.begin(), buf.begin() + 40);
    MemorySource shortSrc(shortFile);
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::BadHeader),
                      static_cast<int>(peekPackInfo(shortSrc, &info)));
}

void test_peek_long_utf8_name() {
    // 63 ASCII bytes and a 3-byte character: cut at 64 bytes, the split
    // character goes and the name stays valid UTF-8.
    std::string name(63, 'a');
    name += "\xE6\xBC\xA2";  // 漢
    const std::vector<uint8_t> buf = makeDeck({0x4E00}, name.c_str());
    MemorySource src(buf);
    PackInfo info;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(peekPackInfo(src, &info)));
    TEST_ASSERT_EQUAL(63, static_cast<int>(std::strlen(info.name)));
    // 61 ASCII bytes and the same character: 64 bytes, kept whole.
    std::string fits(61, 'b');
    fits += "\xE6\xBC\xA2";
    const std::vector<uint8_t> buf2 = makeDeck({0x4E00}, fits.c_str());
    MemorySource src2(buf2);
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(peekPackInfo(src2, &info)));
    TEST_ASSERT_EQUAL_STRING(fits.c_str(), info.name);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tree_pages);
    RUN_TEST(test_tree_fixed_orientation_has_no_orientation_row);
    RUN_TEST(test_tree_every_item_once);
    RUN_TEST(test_tree_groups_and_marks);
    RUN_TEST(test_values);
    RUN_TEST(test_live_streak);
    RUN_TEST(test_layout_home);
    RUN_TEST(test_layout_stats);
    RUN_TEST(test_layout_date);
    RUN_TEST(test_layout_done);
    RUN_TEST(test_deck_summary);
    RUN_TEST(test_study_more_extra);
    RUN_TEST(test_peek_pack_info);
    RUN_TEST(test_peek_long_utf8_name);
    return UNITY_END();
}
