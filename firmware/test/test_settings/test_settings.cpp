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
#include "pack/Utf8.h"
#include "srs/DeckProgress.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"
#include "ui/DeckSummary.h"
#include "ui/ScreenLayout.h"
#include "ui/SettingsTree.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/UiFont.h"

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
    TEST_ASSERT_EQUAL(6, study.count);
    TEST_ASSERT_TRUE(study.items[0] == Item::NewPerDay);
    TEST_ASSERT_TRUE(study.items[1] == Item::Introduce);
    TEST_ASSERT_TRUE(study.items[2] == Item::HardAfter);
    TEST_ASSERT_TRUE(study.items[3] == Item::AutoAdvance);
    TEST_ASSERT_TRUE(study.items[4] == Item::OnKana);
    TEST_ASSERT_TRUE(study.items[5] == Item::DeleteProgress);
    const settings::PageItems device = settings::pageItems(Page::Device);
    TEST_ASSERT_EQUAL(8, device.count);
    TEST_ASSERT_TRUE(device.items[0] == Item::Orientation);
    TEST_ASSERT_TRUE(device.items[3] == Item::HoldTime);
    TEST_ASSERT_TRUE(device.items[4] == Item::Date);
    TEST_ASSERT_TRUE(device.items[6] == Item::Firmware);
    TEST_ASSERT_TRUE(device.items[7] == Item::About);
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

// Study > Introduce (docs/learning-mode.md §3.8): a picker row of the Study
// page, under New per day. Its strings (app/UiStrings.cpp; copies here): the
// label fits a portrait page row's 107 px in the Label face in both
// languages, the values fit the 19 Font0 cells of the caption under it, the
// picker's header fits the 135 px header and its description three caption
// lines of 21 characters.
void test_introduce_row() {
    TEST_ASSERT_TRUE(settings::pageOf(Item::Introduce) == Page::Study);
    TEST_ASSERT_EQUAL(1, settings::rowOf(Page::Study, Item::Introduce));
    TEST_ASSERT_TRUE(settings::hasChevron(Item::Introduce));
    TEST_ASSERT_NULL(settings::itemIcon(Item::Introduce));

    const UiFace& f = uifont::kLabel;
    constexpr int kPageLabelPx = 107;
    for (const char* label : {"Introduce", "Presentar"})
        TEST_ASSERT_TRUE_MESSAGE(uiTextWidth(f, label, 0) <= kPageLabelPx, label);
    // The Label face has every character, accents included.
    for (const char* s : {"Introduce", "Presentar", "On", "Off", "S\xC3\xAD", "No"})
        TEST_ASSERT_TRUE_MESSAGE(uiCanDraw(f, s), s);
    auto cells = [](const char* s) {
        int n = 0;
        for (const char* p = s; utf8Next(&p);) ++n;
        return n;
    };
    for (const char* value : {"On", "Off", "S\xC3\xAD", "No"}) TEST_ASSERT_TRUE(cells(value) <= 19);
    // Header: the title and the battery in 135 px (Font0, ASCII).
    for (const char* title : {"INTRODUCE", "PRESENTAR"})
        TEST_ASSERT_TRUE(cells(title) * 6 <= 135 - 2 * theme::kGutter - 18);
    for (const char* desc : {"Show each new kanji before its first question.",
                             "Muestra cada kanji nuevo antes de preguntarlo."}) {
        const std::vector<TextSpan> lines = wrapText(desc, 21);
        TEST_ASSERT_TRUE_MESSAGE(lines.size() <= 3, desc);
    }
}

namespace {

// Lines `text` takes in the Label face wrapped at spaces to `w` px (greedy, as
// Renderer::wrap() does), or -1 when one word alone is wider than `w`.
int labelLines(const char* text, int w) {
    const UiFace& f = uifont::kLabel;
    const int space = uiTextWidth(f, " ", 0);
    int lineW = 0, lines = 1;
    const char* p = text;
    while (*p) {
        const char* word = p;
        while (*p && *p != ' ') ++p;
        const std::string wd(word, static_cast<size_t>(p - word));
        const int ww = uiTextWidth(f, wd.c_str(), 0);
        if (ww > w) return -1;
        if (lineW && lineW + space + ww > w) {
            ++lines;
            lineW = ww;
        } else {
            lineW = lineW ? lineW + space + ww : ww;
        }
        if (*p == ' ') ++p;
    }
    return lines;
}

int captionCells(const char* s) {
    int n = 0;
    for (const char* p = s; utf8Next(&p);) ++n;
    return n;
}

}  // namespace

// Settings > Device > Firmware (docs/ota.md §4): a row before About on both
// Device pages, and its screen's strings (app/UiStrings.cpp; copies here, with
// the widest version, v0.10.12, filled in). The labels fit a portrait page
// row's 107 px, the row's value the 19 caption cells under it; every status
// two label lines of 127 px in portrait (no word wider) and one of 232 px in
// landscape; the details that share a 240 x 135 screen with two rows one
// caption line of 38 cells. The home's Device row has no room for a version
// next to "Dispositivo" (92 of 93 px), which is why it shows a mark instead.
void test_firmware_row() {
    TEST_ASSERT_TRUE(settings::pageOf(Item::Firmware) == Page::Device);
    TEST_ASSERT_EQUAL(settings::rowOf(Page::Device, Item::About) - 1,
                      settings::rowOf(Page::Device, Item::Firmware));
    const settings::PageItems fixed = settings::pageItems(Page::Device, true);
    TEST_ASSERT_TRUE(fixed.items[fixed.count - 2] == Item::Firmware);
    TEST_ASSERT_TRUE(fixed.items[fixed.count - 1] == Item::About);
    TEST_ASSERT_TRUE(settings::hasChevron(Item::Firmware));
    TEST_ASSERT_NULL(settings::itemIcon(Item::Firmware));

    const UiFace& f = uifont::kLabel;
    TEST_ASSERT_TRUE(uiTextWidth(f, "Firmware", 0) <= 107);
    // The screen's rows: a list row's label in the 135 px portrait.
    constexpr int kRowLabelPx = 135 - 2 * theme::kGutter - 2 * theme::kRowPadX;
    for (const char* row : {"Install", "Check now", "Instalar", "Comprobar"})
        TEST_ASSERT_TRUE_MESSAGE(uiTextWidth(f, row, 0) <= kRowLabelPx, row);
    for (const char* value : {"v0.10.12 available", "v0.10.12 disponible"})
        TEST_ASSERT_TRUE_MESSAGE(captionCells(value) <= 19, value);
    // The home's Device row: "Device" or "Dispositivo", 5 px and "v0.7.0".
    constexpr int kHomeLabelPx = 135 - 2 * theme::kGutter - 2 * theme::kRowPadX -
                                 (kIconCards.w + 5) - (kIconChevron.w + 4);
    TEST_ASSERT_TRUE(uiTextWidth(f, "Device", 0) + 5 + 6 * 6 > kHomeLabelPx);
    TEST_ASSERT_TRUE(uiTextWidth(f, "Dispositivo", 0) + 5 > kHomeLabelPx - 6);
    // Header titles (Font0, ASCII) with the version right of them.
    TEST_ASSERT_TRUE((captionCells("FIRMWARE") + 1 + captionCells("v0.10.12")) * 6 <=
                     135 - 2 * theme::kGutter - 18);

    // Every status line, and the reused network ones it shows.
    for (const char* status :
         {"v0.10.12 available", "Up to date", "Not checked yet", "No update", "Development build",
          "Not available", "Bad manifest", "Checking...", "Downloading...", "Checking the image...",
          "Restarting into v0.10.12...", "Image rejected", "Flash write failed",
          "Server unreachable", "Secure link failed", "Server error 404", "Download corrupt",
          "Download failed", "Not enough space", "Cancelled", "Battery low", "No home Wi-Fi saved",
          "Could not connect", "Network not found", "Wrong password?",
          "v0.10.12 disponible", "Al día", "Sin comprobar", "Sin actualización",
          "Versión de desarrollo", "No disponible", "Manifiesto no válido",
          "Buscando...", "Descargando...", "Comprobando imagen...", "Reiniciando en v0.10.12...",
          "Imagen rechazada", "Fallo al escribir", "Servidor inaccesible",
          "Sin conexión segura", "Error del servidor 404", "Descarga dañada",
          "Descarga fallida", "No hay espacio", "Cancelado", "Batería baja",
          "Sin Wi-Fi de casa", "No se pudo conectar", "Red no encontrada",
          "¿Contraseña incorrecta?"}) {
        const int portrait = labelLines(status, 127);
        TEST_ASSERT_TRUE_MESSAGE(portrait >= 1 && portrait <= 2, status);
        TEST_ASSERT_TRUE_MESSAGE(uiTextWidth(f, status, 0) <= 232, status);
    }
    // One Spanish word is too wide for the portrait line ("Buscando..." took its
    // place), and the TLS failure of the other screens takes three lines in
    // both languages (the screen says "Secure link failed").
    TEST_ASSERT_EQUAL(-1, labelLines("Comprobando...", 127));
    TEST_ASSERT_EQUAL(3, labelLines("Secure connection failed", 127));
    TEST_ASSERT_EQUAL(3, labelLines("Fallo de conexión segura", 127));
    // Details next to two rows in landscape.
    for (const char* detail : {"1.7 MB · settings and decks stay",
                               "1.7 MB · ajustes y mazos se quedan",
                               "Not installed: v0.10.12 stays.",
                               "Sin instalar: se queda la v0.10.12.",
                               "Charge it or plug in USB first.", "Cárgalo o conecta el USB antes."})
        TEST_ASSERT_TRUE_MESSAGE(captionCells(detail) <= 38, detail);
    // The install's numbers under the bar (Font0, 127 px).
    TEST_ASSERT_TRUE(captionCells("1.7 / 1.7 MB · 100%") * 6 <= 127);
    // The notice after a rollback: two label lines, then four caption lines.
    for (const char* title : {"Update failed", "Actualización fallida"}) {
        const int lines = labelLines(title, 127);
        TEST_ASSERT_TRUE_MESSAGE(lines >= 1 && lines <= 2, title);
    }
    for (const char* text : {"v0.10.12 did not start: back on v0.10.11.",
                             "v0.10.12 no arrancó: de vuelta a v0.10.11."})
        TEST_ASSERT_TRUE_MESSAGE(wrapText(text, 21).size() <= 4, text);
}

// The microSD card's strings (docs/microsd.md §4; app/UiStrings.cpp, copies
// here with the widest numbers filled in: a 128 GB card reformatted as FAT32,
// "119.2"). Only the Cardputer ADV (240 x 135) shows them today, but they fit
// the 135 px portrait too. The deck list's top line: one caption line of 38
// cells in landscape, two of 21 in portrait. About's heading in the label
// face; the move's header with "12/12" right of it and its status in two label
// lines; Get decks' note in 21 cells and the Wi-Fi screen's free-space line in
// the 20 cells next to the QR code in landscape.
void test_card_strings() {
    for (const char* line : {"Card not readable: format it as FAT32", "Card removed",
                             "Tarjeta ilegible: formatéala en FAT32", "Tarjeta retirada"}) {
        TEST_ASSERT_TRUE_MESSAGE(captionCells(line) <= 38, line);
        TEST_ASSERT_TRUE_MESSAGE(wrapText(line, 21).size() <= 2, line);
    }
    const UiFace& f = uifont::kLabel;
    for (const char* heading : {"Card", "Tarjeta"})
        TEST_ASSERT_TRUE_MESSAGE(uiTextWidth(f, heading, 0) <= 127, heading);
    for (const char* value : {"119.2 GB, 119.2 free", "119,2 GB, 119,2 libres", "No card",
                              "Sin tarjeta"})
        TEST_ASSERT_TRUE_MESSAGE(wrapText(value, 21).size() <= 2, value);
    for (const char* title : {"SD CARD", "TARJETA SD"})
        TEST_ASSERT_TRUE_MESSAGE(
            (captionCells(title) + 1 + captionCells("12/12")) * 6 <= 135 - 2 * theme::kGutter - 18,
            title);
    for (const char* status : {"Moving decks to the card...", "Mazos a la tarjeta..."}) {
        const int portrait = labelLines(status, 127);
        TEST_ASSERT_TRUE_MESSAGE(portrait >= 1 && portrait <= 2, status);
        TEST_ASSERT_TRUE_MESSAGE(labelLines(status, 232) <= 2, status);
    }
    for (const char* note : {"SD: 119.2 GB free", "SD: 119,2 GB libres"})
        TEST_ASSERT_TRUE_MESSAGE(captionCells(note) <= 21, note);
    TEST_ASSERT_TRUE(captionCells("SD 119.2 / 119.2 GB") <= 20);
    // The pill: two Font0 cells, like a language's.
    TEST_ASSERT_EQUAL(2, captionCells("SD"));
}

// What the app sets the queue to (app/Settings.cpp learnOptions()): learning
// on or off, with the queue's defaults of two steps three items apart
// (docs/learning-mode.md §6, decision 5); off is the default of the queue
// itself, so a queue the app never configured behaves as before.
void test_learn_options_defaults() {
    const LearnOptions o;
    TEST_ASSERT_FALSE(o.on);
    TEST_ASSERT_EQUAL_UINT8(2, o.steps);
    TEST_ASSERT_EQUAL_UINT8(3, o.gap);
    Queue q;
    TEST_ASSERT_FALSE(q.learning().on);
    LearnOptions on;
    on.on = true;
    q.setLearning(on);
    TEST_ASSERT_TRUE(q.learning().on);
    TEST_ASSERT_EQUAL_UINT8(2, q.learning().steps);
    TEST_ASSERT_EQUAL_UINT8(3, q.learning().gap);
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

// Settings > Device > Firmware on every panel, with 0 to 2 rows: the parts stay
// between the header and the rows (or the hint bar), in order, and leave at
// least one detail line; the install's bar and numbers fit above the hint bar.
void test_layout_firmware() {
    using namespace theme;
    for (const Panel& p : kPanels) {
        for (int rows = 0; rows <= 2; ++rows) {
            const layout::Firmware l = layout::firmwareFor(p.w, p.h, rows);
            const Layout g = layoutFor(p.w, p.h);
            const int hintY = p.h - kHintH;
            TEST_ASSERT_TRUE(inside(l.version, p.w, kHeaderH, l.rowsTop));
            TEST_ASSERT_TRUE(inside(l.status, p.w, kHeaderH, l.rowsTop));
            TEST_ASSERT_TRUE(l.version.bottom() <= l.status.y);
            TEST_ASSERT_TRUE(l.status.bottom() < l.detail.y);
            TEST_ASSERT_TRUE(l.detail.bottom() < l.rowsTop);
            TEST_ASSERT_TRUE(l.detailLines >= 1);
            TEST_ASSERT_TRUE(l.detail.y + 10 * (l.detailLines - 1) + 8 <= l.detail.bottom());
            TEST_ASSERT_EQUAL(p.w > p.h ? 1 : 2, l.statusLines);
            TEST_ASSERT_EQUAL(20 * l.statusLines, l.status.h);
            // The rows fit the band layoutList() is given.
            const int band = hintY - l.rowsTop - 2 * kMarkerBand;
            TEST_ASSERT_TRUE(rows == 0 ? l.rowsTop == hintY
                                       : band >= rows * g.row1H + (rows - 1) * kGap);
            if (rows == 0) {
                TEST_ASSERT_TRUE(inside(l.bar, p.w, l.status.bottom(), hintY));
                TEST_ASSERT_TRUE(l.numbersY + 8 <= hintY);
            }
        }
    }
    // The StickS3's and the Cardputer's landscape: one detail line with two
    // rows, three with one.
    TEST_ASSERT_EQUAL(1, layout::firmwareFor(240, 135, 2).detailLines);
    TEST_ASSERT_EQUAL(3, layout::firmwareFor(240, 135, 1).detailLines);
    TEST_ASSERT_TRUE(layout::firmwareFor(135, 240, 2).detailLines >= 6);
}

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

// A learning record (an intro whose steps a rebuild dropped, see
// docs/learning-mode.md section 3.5) is seen and due, not new, also when it
// was introduced on SRS day 0 (due day 0); the cap of today counts it. Once
// answered it is an ordinary record; forgotten, the kanji is new again.
void test_deck_summary_learning() {
    const std::vector<uint8_t> buf = makeDeck({0x4E00, 0x4E8C, 0x4E09}, "JLPT N5");
    MemorySource src(buf);
    PackReader pack;
    TEST_ASSERT_EQUAL(static_cast<int>(PackError::Ok), static_cast<int>(pack.open(src)));

    SrsStore srs;
    srs.setToday(0);
    TEST_ASSERT_TRUE(srs.introduce(0x4E00, 0));
    DeckSummary s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(2, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(1, s.seen());
    TEST_ASSERT_EQUAL_UINT32(1, s.due);
    TEST_ASSERT_EQUAL_UINT32(1, s.dueTomorrow);
    TEST_ASSERT_EQUAL_UINT32(1, s.introducedToday);

    // Still learning on a later day: due, no longer introduced today.
    srs.setToday(2);
    s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(2, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(1, s.due);
    TEST_ASSERT_EQUAL_UINT32(0, s.introducedToday);

    // Answered (a step, or its review): due in the future like any card.
    srs.grade(0x4E00, Grade::Good, 2, 0, 0.5f);
    s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(1, s.seen());
    TEST_ASSERT_EQUAL_UINT32(0, s.due);

    // Delete progress on an intro: the kanji is new again.
    TEST_ASSERT_TRUE(srs.introduce(0x4E8C, 2));
    const uint32_t gone[] = {0x4E00, 0x4E8C};
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(srs.forget(gone, 2)));
    s = deckSummary(pack, srs);
    TEST_ASSERT_EQUAL_UINT32(3, s.fresh);
    TEST_ASSERT_EQUAL_UINT32(0, s.due);
    TEST_ASSERT_EQUAL_UINT32(0, s.introducedToday);
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
    RUN_TEST(test_introduce_row);
    RUN_TEST(test_firmware_row);
    RUN_TEST(test_card_strings);
    RUN_TEST(test_learn_options_defaults);
    RUN_TEST(test_values);
    RUN_TEST(test_live_streak);
    RUN_TEST(test_layout_home);
    RUN_TEST(test_layout_stats);
    RUN_TEST(test_layout_date);
    RUN_TEST(test_layout_done);
    RUN_TEST(test_layout_firmware);
    RUN_TEST(test_deck_summary);
    RUN_TEST(test_deck_summary_learning);
    RUN_TEST(test_study_more_extra);
    RUN_TEST(test_peek_pack_info);
    RUN_TEST(test_peek_long_utf8_name);
    return UNITY_END();
}
