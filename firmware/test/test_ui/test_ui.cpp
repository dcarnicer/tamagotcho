// Native tests for the UX-refresh foundations in ui/: the scrolling list state
// (ListView), the design tokens and layout grid (Theme), the 1bpp icons and
// the generated UI font tables (UiFace / UiFont).
#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "Credits.h"
#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/ListView.h"
#include "ui/Theme.h"
#include "ui/UiFace.h"
#include "ui/UiFont.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- ListView

// The StickS3 portrait settings list: 12 rows of 26 px with 2 px gaps in the
// band y 20..224 (204 px) -> 7 rows fit.
static ListView settingsList() {
    ListView l;
    l.setCount(12);
    l.reset(0);
    l.setGeometry(20, 204, 26, 2);
    return l;
}

void test_list_fit_and_rows() {
    ListView l = settingsList();
    TEST_ASSERT_EQUAL(7, l.fit());
    TEST_ASSERT_EQUAL(7, l.visible());
    TEST_ASSERT_TRUE(l.overflows());
    TEST_ASSERT_FALSE(l.moreAbove());
    TEST_ASSERT_TRUE(l.moreBelow());
    TEST_ASSERT_EQUAL(20, l.rowY(0));
    TEST_ASSERT_EQUAL(20 + 28, l.rowY(1));
    TEST_ASSERT_EQUAL(20 + 7 * 28 - 2, l.rowsBottom());
    TEST_ASSERT_TRUE(l.isVisible(6));
    TEST_ASSERT_FALSE(l.isVisible(7));
}

void test_list_scrolls_minimally() {
    ListView l = settingsList();
    for (int i = 0; i < 6; ++i) l.next();
    TEST_ASSERT_EQUAL(6, l.focus());
    TEST_ASSERT_EQUAL(0, l.first());  // still on screen: no scroll
    l.next();                         // item 7: one row down
    TEST_ASSERT_EQUAL(1, l.first());
    TEST_ASSERT_TRUE(l.moreAbove());
    TEST_ASSERT_EQUAL(l.rowY(7), 20 + 6 * 28);
    l.prev();
    l.prev();  // back to 5: visible, the window stays
    TEST_ASSERT_EQUAL(5, l.focus());
    TEST_ASSERT_EQUAL(1, l.first());
    for (int i = 0; i < 5; ++i) l.prev();  // 0: the window follows up
    TEST_ASSERT_EQUAL(0, l.focus());
    TEST_ASSERT_EQUAL(0, l.first());
    l.prev();  // wraps to the last item, window at the end
    TEST_ASSERT_EQUAL(11, l.focus());
    TEST_ASSERT_EQUAL(5, l.first());
    TEST_ASSERT_FALSE(l.moreBelow());
    l.next();  // wraps to the top
    TEST_ASSERT_EQUAL(0, l.focus());
    TEST_ASSERT_EQUAL(0, l.first());
}

void test_list_geometry_change_keeps_focus_visible() {
    ListView l = settingsList();
    l.setFocus(6);
    // Landscape: 4 rows of 22 px in y 20..119 (99 px).
    l.setGeometry(20, 99, 22, 2);
    TEST_ASSERT_EQUAL(4, l.fit());
    TEST_ASSERT_TRUE(l.isVisible(6));
    TEST_ASSERT_EQUAL(3, l.first());
    // Back to portrait: the window does not move while the focus is visible.
    l.setGeometry(20, 204, 26, 2);
    TEST_ASSERT_EQUAL(3, l.first());
    TEST_ASSERT_EQUAL(7, l.visible());
}

void test_list_reset_before_first_draw() {
    // Opened with the saved choice focused, before any geometry: the window
    // is fitted from the top at the first draw.
    ListView l;
    l.setCount(23);
    l.reset(5);
    l.setGeometry(20, 204, 26, 2);
    TEST_ASSERT_EQUAL(5, l.focus());
    TEST_ASSERT_EQUAL(0, l.first());
    l.reset(20);
    l.setGeometry(20, 204, 26, 2);
    TEST_ASSERT_EQUAL(14, l.first());
    TEST_ASSERT_TRUE(l.isVisible(20));
}

void test_list_short_and_empty() {
    ListView l;
    l.setCount(3);
    l.reset(1);
    l.setGeometry(20, 204, 26, 2);
    TEST_ASSERT_EQUAL(3, l.visible());
    TEST_ASSERT_FALSE(l.overflows());
    TEST_ASSERT_FALSE(l.moreAbove());
    TEST_ASSERT_FALSE(l.moreBelow());
    const ListView::Thumb t = l.thumb();
    TEST_ASSERT_EQUAL(20, t.trackY);
    TEST_ASSERT_EQUAL(3 * 28 - 2, t.trackH);

    ListView e;
    e.setCount(0);
    e.reset(4);
    e.setGeometry(20, 204, 26, 2);
    TEST_ASSERT_EQUAL(0, e.focus());
    TEST_ASSERT_EQUAL(0, e.visible());
    TEST_ASSERT_EQUAL(20, e.rowsBottom());
    e.next();
    TEST_ASSERT_EQUAL(0, e.focus());

    // A band smaller than one row still shows the focused row.
    ListView tiny;
    tiny.setCount(5);
    tiny.reset(3);
    tiny.setGeometry(20, 10, 26, 2);
    TEST_ASSERT_EQUAL(1, tiny.fit());
    TEST_ASSERT_EQUAL(3, tiny.first());
}

void test_list_thumb() {
    ListView l = settingsList();  // 7 of 12 visible, track 194 px
    ListView::Thumb t = l.thumb();
    TEST_ASSERT_EQUAL(20, t.trackY);
    TEST_ASSERT_EQUAL(194, t.trackH);
    TEST_ASSERT_EQUAL(194 * 7 / 12, t.h);
    TEST_ASSERT_EQUAL(20, t.y);  // at the top
    l.setFocus(11);              // window at the end
    t = l.thumb();
    TEST_ASSERT_EQUAL(20 + 194 - t.h, t.y);
    // Many items: the thumb keeps its minimum length.
    ListView m;
    m.setCount(500);
    m.reset(250);
    m.setGeometry(20, 204, 26, 2);
    t = m.thumb(6);
    TEST_ASSERT_EQUAL(6, t.h);
    TEST_ASSERT_TRUE(t.y > t.trackY && t.y + t.h < t.trackY + t.trackH);
}

// ---------------------------------------------------------------- Theme

void test_theme_tokens() {
    using namespace theme;
    // The RGB inputs of tools/ux-mockups/render.py (class T) give the table's
    // RGB565 values (docs/ux-refresh.md §2.2).
    TEST_ASSERT_EQUAL_HEX16(kBg, rgb565(0, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(kSurface, rgb565(24, 28, 36));
    TEST_ASSERT_EQUAL_HEX16(kSurfaceHi, rgb565(40, 46, 60));
    TEST_ASSERT_EQUAL_HEX16(kLine, rgb565(56, 64, 80));
    TEST_ASSERT_EQUAL_HEX16(kGhost, rgb565(44, 50, 64));
    TEST_ASSERT_EQUAL_HEX16(kText, rgb565(255, 255, 255));
    TEST_ASSERT_EQUAL_HEX16(kTextDim, rgb565(164, 172, 188));
    TEST_ASSERT_EQUAL_HEX16(kTextFaint, rgb565(100, 108, 124));
    TEST_ASSERT_EQUAL_HEX16(kAccent, rgb565(40, 96, 216));
    TEST_ASSERT_EQUAL_HEX16(kAccentPress, rgb565(96, 156, 255));
    TEST_ASSERT_EQUAL_HEX16(kAccentHi, rgb565(120, 170, 255));
    TEST_ASSERT_EQUAL_HEX16(kOk, rgb565(0, 128, 64));
    TEST_ASSERT_EQUAL_HEX16(kOkHi, rgb565(90, 220, 140));
    TEST_ASSERT_EQUAL_HEX16(kOkBg, rgb565(0, 48, 24));
    TEST_ASSERT_EQUAL_HEX16(kErr, rgb565(200, 40, 40));
    TEST_ASSERT_EQUAL_HEX16(kErrHi, rgb565(255, 110, 110));
    TEST_ASSERT_EQUAL_HEX16(kErrBg, rgb565(56, 16, 16));
    TEST_ASSERT_EQUAL_HEX16(kWarn, rgb565(255, 196, 48));
    TEST_ASSERT_EQUAL_HEX16(kModeReading, rgb565(110, 220, 200));
    TEST_ASSERT_EQUAL_HEX16(kModeKanji, rgb565(200, 160, 255));
    TEST_ASSERT_EQUAL(2, kGap);
    TEST_ASSERT_EQUAL(0, kRadius);
}

void test_theme_layouts() {
    using namespace theme;
    // StickS3 portrait (§2.1): prompt y 16..117, answers y 120..223, hint 228..239.
    constexpr Layout p = layoutFor(135, 240);
    TEST_ASSERT_FALSE(p.landscape);
    TEST_ASSERT_EQUAL(26, p.row1H);
    TEST_ASSERT_EQUAL(36, p.row2H);
    TEST_ASSERT_EQUAL(16, p.promptY);
    TEST_ASSERT_EQUAL(117, p.promptY + p.promptH - 1);
    TEST_ASSERT_EQUAL(120, p.answersY);
    TEST_ASSERT_EQUAL(223, p.answersY + p.answersH - 1);
    TEST_ASSERT_EQUAL(228, p.hintY);
    // StickS3 landscape: prompt x 0..99, answers x 104..235, hint 123..134.
    constexpr Layout l = layoutFor(240, 135);
    TEST_ASSERT_TRUE(l.landscape);
    TEST_ASSERT_EQUAL(22, l.row1H);
    TEST_ASSERT_EQUAL(32, l.row2H);
    TEST_ASSERT_EQUAL(99, l.promptX + l.promptW - 1);
    TEST_ASSERT_EQUAL(104, l.answersX);
    TEST_ASSERT_EQUAL(235, l.answersX + l.answersW - 1);
    TEST_ASSERT_EQUAL(123, l.hintY);
    // T-Display-S3 portrait: answers y 120..303, hint 308..319, rows 30/40.
    constexpr Layout t = layoutFor(170, 320);
    TEST_ASSERT_EQUAL(30, t.row1H);
    TEST_ASSERT_EQUAL(40, t.row2H);
    TEST_ASSERT_EQUAL(303, t.answersY + t.answersH - 1);
    TEST_ASSERT_EQUAL(308, t.hintY);
    // Its landscape (derived): rows 26/36, a 120 px prompt column.
    constexpr Layout tl = layoutFor(320, 170);
    TEST_ASSERT_TRUE(tl.landscape);
    TEST_ASSERT_EQUAL(26, tl.row1H);
    TEST_ASSERT_EQUAL(120, tl.promptW);
    TEST_ASSERT_EQUAL(320 - 4, tl.answersX + tl.answersW);
}

// ---------------------------------------------------------------- Icons

void test_icons_packing() {
    // check, row 0 "........#": only the 9th pixel, in the second byte.
    TEST_ASSERT_EQUAL(9, kIconCheck.w);
    TEST_ASSERT_EQUAL(7, kIconCheck.h);
    TEST_ASSERT_EQUAL_HEX8(0x00, kIconCheck.bits[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, kIconCheck.bits[1]);
    // row 2 "#.....##." -> 1000 0011 / 0
    TEST_ASSERT_EQUAL_HEX8(0x83, kIconCheck.bits[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, kIconCheck.bits[5]);
    // One byte per row up to 8 px: chevron row 3 "..##" -> 0011 0000.
    TEST_ASSERT_EQUAL_HEX8(0x30, kIconChevron.bits[3]);
    // radio on/off differ only in the dot.
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x) {
            const bool dot = x >= 2 && x <= 4 && y >= 2 && y <= 4;
            if (!dot)
                TEST_ASSERT_EQUAL(icons::pixel(kIconRadioOn, x, y), icons::pixel(kIconRadioOff, x, y));
            else
                TEST_ASSERT_TRUE(icons::pixel(kIconRadioOn, x, y) && !icons::pixel(kIconRadioOff, x, y));
        }
    // The hold mark is the tap mark stretched: 11 x 5 with rounded ends.
    TEST_ASSERT_FALSE(icons::pixel(kIconHold, 0, 0));
    TEST_ASSERT_TRUE(icons::pixel(kIconHold, 1, 0));
    TEST_ASSERT_TRUE(icons::pixel(kIconHold, 10, 2));
    // Scroll markers point up and down.
    TEST_ASSERT_TRUE(icons::pixel(kIconMoreUp, 2, 0));
    TEST_ASSERT_FALSE(icons::pixel(kIconMoreUp, 0, 0));
    TEST_ASSERT_TRUE(icons::pixel(kIconMoreDown, 0, 0));
    TEST_ASSERT_TRUE(icons::pixel(kIconMoreDown, 2, 2));
    TEST_ASSERT_FALSE(icons::pixel(kIconMoreDown, 0, 2));
}

// ---------------------------------------------------------------- UI font

static void checkFace(const UiFace& f) {
    TEST_ASSERT_TRUE(f.count > 0);
    uint32_t prev = 0;
    for (uint16_t i = 0; i < f.count; ++i) {
        const UiGlyph& g = f.glyphs[i];
        if (i > 0) TEST_ASSERT_TRUE(g.cp > prev);  // sorted, no duplicates
        prev = g.cp;
        // Every bitmap lies inside the face's data.
        TEST_ASSERT_TRUE(g.offset + uiGlyphBytes(g) <= f.bitsSize);
        // Ink sits above the baseline by at most the font's ascent.
        if (g.height) TEST_ASSERT_TRUE(-g.yOff <= f.ascent);
        TEST_ASSERT_EQUAL_PTR(&g, uiGlyph(f, g.cp));
    }
}

void test_ui_font_label() {
    const UiFace& f = uifont::kLabel;
    checkFace(f);
    TEST_ASSERT_EQUAL(18, f.px);
    TEST_ASSERT_EQUAL(20, f.pitch);
    // ASCII, Latin-1 and the UI punctuation are all there.
    for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) TEST_ASSERT_NOT_NULL(uiGlyph(f, cp));
    for (uint32_t cp = 0xA0; cp <= 0xFF; ++cp) TEST_ASSERT_NOT_NULL(uiGlyph(f, cp));
    for (uint32_t cp : {0x2013u, 0x2014u, 0x2026u, 0x2039u, 0x203Au})
        TEST_ASSERT_NOT_NULL(uiGlyph(f, cp));
    TEST_ASSERT_NULL(uiGlyph(f, 0x1F));
    TEST_ASSERT_NULL(uiGlyph(f, 0x6F22));  // 漢: no CJK in the UI font
    // Cap height = the ink above the baseline of 'H'.
    const UiGlyph* h = uiGlyph(f, 'H');
    TEST_ASSERT_EQUAL(f.capHeight, -h->yOff);
    TEST_ASSERT_TRUE(f.capHeight > 10 && f.capHeight < f.pitch);
    // Spaces have an advance and no bitmap; "Wi-Fi" is the sum of its letters.
    const UiGlyph* sp = uiGlyph(f, ' ');
    TEST_ASSERT_EQUAL(0, sp->width);
    TEST_ASSERT_TRUE(sp->advance > 0);
    TEST_ASSERT_EQUAL(uiGlyph(f, 0xA0)->advance, sp->advance);
    int sum = 0;
    for (const char* p = "Wi-Fi"; *p; ++p) sum += uiGlyph(f, static_cast<uint8_t>(*p))->advance;
    TEST_ASSERT_EQUAL(sum, uiTextWidth(f, "Wi-Fi", 0));
    // Spanish needs nothing outside the face; a kanji counts as `missing`.
    TEST_ASSERT_TRUE(uiCanDraw(f, "Orientación · Estadísticas ¿ñ?"));
    TEST_ASSERT_FALSE(uiCanDraw(f, "漢字"));
    TEST_ASSERT_EQUAL(2 * 7, uiTextWidth(f, "漢字", 7));
}

void test_ui_font_numeral() {
    const UiFace& f = uifont::kNumeral;
    checkFace(f);
    TEST_ASSERT_EQUAL(32, f.px);
    for (const char* p = "0123456789%:/+-."; *p; ++p)
        TEST_ASSERT_NOT_NULL(uiGlyph(f, static_cast<uint8_t>(*p)));
    TEST_ASSERT_NULL(uiGlyph(f, 'A'));
    TEST_ASSERT_EQUAL(f.capHeight, -uiGlyph(f, '0')->yOff);
    TEST_ASSERT_TRUE(f.capHeight > uifont::kLabel.capHeight);
}

// The app's name, たまごっちょ (Credits.h): the Label face draws it as the About
// title and the Kana face in the 16 px header (render/Widgets drawHeader picks
// the Kana face for a title it can draw entirely). Every kana is in both faces,
// the ink stays inside the header above its 2 px progress bar / hairline and
// inside a 20 px label line, and the name fits the narrowest header (the
// StickS3 in portrait, 135 px) beside the Wi-Fi icon and the battery, so it is
// never cut.
void test_ui_font_app_name() {
    const char* name = credits::kAppName;
    TEST_ASSERT_EQUAL_STRING("\xE3\x81\x9F\xE3\x81\xBE\xE3\x81\x94\xE3\x81\xA3\xE3\x81\xA1\xE3\x82\x87",
                             name);  // たまごっちょ
    const UiFace& kana = uifont::kKana;
    const UiFace& label = uifont::kLabel;
    checkFace(kana);
    TEST_ASSERT_EQUAL(12, kana.px);
    TEST_ASSERT_EQUAL(6, kana.count);  // only the app's name
    TEST_ASSERT_TRUE(uiCanDraw(kana, name));
    TEST_ASSERT_TRUE(uiCanDraw(label, name));
    // Other titles stay in Font0 captions.
    for (const char* t : {"SETTINGS", "AJUSTES", "JLPT N5", "TAMAGOTCHO", " "})
        TEST_ASSERT_FALSE_MESSAGE(uiCanDraw(kana, t), t);
    TEST_ASSERT_EQUAL(kana.capHeight, -uiGlyph(kana, 0x305F)->yOff);  // た

    // Renderer::drawLabel's baseline in a box of height h: (h + cap) / 2.
    const int headerBase = (theme::kHeaderH - 1 + kana.capHeight) / 2;
    const int labelBase = (label.pitch + label.capHeight) / 2;
    for (const char* p = name; uint32_t cp = utf8Next(&p);) {
        const UiGlyph* g = uiGlyph(kana, cp);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_TRUE(headerBase + g->yOff >= 0);
        TEST_ASSERT_TRUE(headerBase + g->yOff + g->height <= theme::kHeaderH - 2);
        const UiGlyph* l = uiGlyph(label, cp);
        TEST_ASSERT_NOT_NULL(l);
        TEST_ASSERT_TRUE(labelBase + l->yOff >= 0);
        TEST_ASSERT_TRUE(labelBase + l->yOff + l->height <= label.pitch);
    }

    constexpr int kNarrowestW = 135;
    const int titleEnd = theme::kGutter + uiTextWidth(kana, name, 0);
    const int rightStart = kNarrowestW - theme::kGutter - 13 - 5 - kIconWifi.w - 5;
    TEST_ASSERT_TRUE(titleEnd + 6 <= rightStart);
    TEST_ASSERT_TRUE(uiTextWidth(label, name, 0) <= kNarrowestW - 2 * theme::kGutter);
}

// Settings > Study > Delete progress (main.cpp's UiStrings; these are copies):
// every row label fits a portrait page row's 107 px in both languages, which
// "Delete progress" (130 px) and "Borrar progreso" (133 px) do not -- the
// Study row says "Delete" over a "progress" caption -- and every caption fits
// the 19 Font0 cells (6 px each) under a portrait row's label.
void test_delete_progress_labels() {
    const UiFace& f = uifont::kLabel;
    constexpr int kPageLabelPx = 107;
    for (const char* label : {"Delete", "Borrar", "This deck", "Este mazo", "Everything", "Todo"})
        TEST_ASSERT_TRUE_MESSAGE(uiTextWidth(f, label, 0) <= kPageLabelPx, label);
    TEST_ASSERT_TRUE(uiTextWidth(f, "Delete progress", 0) > kPageLabelPx);
    TEST_ASSERT_TRUE(uiTextWidth(f, "Borrar progreso", 0) > kPageLabelPx);
    constexpr int kCaptionCells = 19;
    for (const char* caption : {"progress", "progreso", "and the statistics", "y las estadísticas",
                                "no deck loaded", "ningún mazo cargado"}) {
        int cells = 0;
        for (const char* p = caption; utf8Next(&p);) ++cells;
        TEST_ASSERT_TRUE_MESSAGE(cells <= kCaptionCells, caption);
    }
}

// The deck-update texts (main.cpp's UiStrings; copies). The boot result's
// Spanish line used to wrap as "6" + "actualizaciones" (130 px in a 127 px box)
// and lose "de mazos"; it now wraps into two lines that fit. The settings
// home's Decks row used to drop its badge in portrait: "Mazos" + "6 act." did
// not fit the 93 px left of the icon and chevron. Both are checked here so a
// new wording cannot silently reintroduce the cut.
void test_update_text_fits() {
    const UiFace& f = uifont::kLabel;
    constexpr int kResultW = 127;  // W - 8 on the narrowest screen
    const int kSpace = uiTextWidth(f, " ", 0);
    for (const char* text : {"6 mazos por actualizar", "1 actualización de mazo",
                             "6 deck updates available", "1 deck update available"}) {
        int lineW = 0, lines = 1;
        const char* p = text;
        while (*p) {
            const char* word = p;
            while (*p && *p != ' ') ++p;
            char buf[64];
            const size_t n = static_cast<size_t>(p - word);
            if (n >= sizeof buf) break;
            std::memcpy(buf, word, n);
            buf[n] = 0;
            const int ww = uiTextWidth(f, buf, 0);
            if (lineW && lineW + kSpace + ww > kResultW) {
                ++lines;
                lineW = ww;
            } else {
                lineW = lineW ? lineW + kSpace + ww : ww;
            }
            TEST_ASSERT_TRUE_MESSAGE(lineW <= kResultW, text);
            if (*p == ' ') ++p;
        }
        TEST_ASSERT_TRUE_MESSAGE(lines <= 2, text);
    }
    // The old Spanish line did not fit: its longest word alone was 130 px.
    TEST_ASSERT_TRUE(uiTextWidth(f, "actualizaciones", 0) > kResultW);

    // The home row: 135 - 2 * gutter - 2 * row pad - icon - chevron.
    constexpr int kHomeLabelPx = 135 - 2 * theme::kGutter - 2 * theme::kRowPadX -
                                 (kIconCards.w + 5) - (kIconChevron.w + 4);
    struct RowText {
        const char* label;
        const char* value;
    };
    for (const RowText& r : {RowText{"Decks", "6 upd"}, RowText{"Mazos", "6 act"},
                             RowText{"Decks", "1 upd"}, RowText{"Mazos", "1 act"}})
        TEST_ASSERT_TRUE_MESSAGE(
            uiTextWidth(f, r.label, 0) + 5 + static_cast<int>(std::strlen(r.value)) * 6 <=
                kHomeLabelPx,
            r.label);
    // The values the fix replaced were too wide (Spanish by one pixel).
    TEST_ASSERT_TRUE(uiTextWidth(f, "Mazos", 0) + 5 + 6 * 6 > kHomeLabelPx);
    TEST_ASSERT_TRUE(uiTextWidth(f, "Decks", 0) + 5 + 9 * 6 > kHomeLabelPx);
}

// ---------------------------------------------------------------- InputPrefs

static void test_input_prefs() {
    TEST_ASSERT_EQUAL_UINT32(0, advanceMsFor(kAdvanceOff));
    TEST_ASSERT_EQUAL_UINT32(600, advanceMsFor(kAdvanceShort));
    TEST_ASSERT_EQUAL_UINT32(1000, advanceMsFor(kAdvanceNormal));
    TEST_ASSERT_EQUAL_UINT32(1000, advanceMsFor(kAdvanceDefault));
    TEST_ASSERT_EQUAL_UINT32(advanceMsFor(kAdvanceDefault), advanceMsFor(kAdvanceChoices));
    TEST_ASSERT_EQUAL_UINT32(400, holdMsFor(kHoldShort));
    TEST_ASSERT_EQUAL_UINT32(600, holdMsFor(kHoldNormal));
    TEST_ASSERT_EQUAL_UINT32(900, holdMsFor(kHoldLong));
    TEST_ASSERT_EQUAL_UINT32(600, holdMsFor(kHoldDefault));
    TEST_ASSERT_EQUAL_UINT32(holdMsFor(kHoldDefault), holdMsFor(0xFF));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_list_fit_and_rows);
    RUN_TEST(test_list_scrolls_minimally);
    RUN_TEST(test_list_geometry_change_keeps_focus_visible);
    RUN_TEST(test_list_reset_before_first_draw);
    RUN_TEST(test_list_short_and_empty);
    RUN_TEST(test_list_thumb);
    RUN_TEST(test_theme_tokens);
    RUN_TEST(test_theme_layouts);
    RUN_TEST(test_icons_packing);
    RUN_TEST(test_ui_font_label);
    RUN_TEST(test_ui_font_numeral);
    RUN_TEST(test_ui_font_app_name);
    RUN_TEST(test_delete_progress_labels);
    RUN_TEST(test_update_text_fits);
    RUN_TEST(test_input_prefs);
    return UNITY_END();
}
