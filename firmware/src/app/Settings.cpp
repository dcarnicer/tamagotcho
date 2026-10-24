// The settings tree; see Settings.h.
#include "app/Settings.h"

#include <Arduino.h>

#include "app/About.h"
#include "app/App.h"
#include "app/Choice.h"
#include "app/Clock.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Firmware.h"
#include "app/Intro.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Progress.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Stats.h"
#include "app/UiStrings.h"
#include "app/WifiMenu.h"
#include "board/Board.h"
#include "render/Widgets.h"
#include "ui/DeckSummary.h"
#include "ui/ScreenLayout.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

uint8_t gBrightness = kBrightnessDefault;  // NVS `tangocho`/`bri`
uint8_t gAdvancePref = tgc::kAdvanceDefault;  // NVS `tangocho`/`adv` (ui/InputPrefs.h)
uint8_t gHoldPref = tgc::kHoldDefault;        // NVS `tangocho`/`hold`
tgc::OnKana gOnKana = tgc::OnKana::Katakana;  // NVS `tangocho`/`onkana` (0 katakana, 1 hiragana)
bool gIntroOn = true;                         // NVS `tangocho`/`intro`

// Two steps, 3 items apart (docs/learning-mode.md §6, decision 5): the
// queue's defaults.
tgc::LearnOptions learnOptions() {
    tgc::LearnOptions o;
    o.on = gIntroOn;
    return o;
}

// --- Settings tree (docs/ux-refresh.md §3, §6.6) ----------------------------
// Hold the side button from the quiz. The home has a summary card (the loaded
// deck, due and new, its coverage) and five group rows with their values;
// Study and Device are pages of rows (a one-level group stack,
// ui/SettingsTree.h), the other groups open their own screens. Tap moves the
// focus, a hold opens or changes the row, the side button goes back: from a
// page to the home, from the home to where the settings were opened. Every
// screen opened from a page returns to it (gMenuPage stays put meanwhile).
st::Page gMenuPage = st::Page::Home;
char gMenuNote[32] = "";  // a page's note (after Delete progress), until the next press

namespace {

tgc::ListView gMenuLists[st::kPageCount];  // each page keeps its focus

}  // namespace

tgc::ListView& menuList() { return gMenuLists[static_cast<int>(gMenuPage)]; }

namespace {

// A page's rows on this board: a landscape-only one has no Orientation row.
st::PageItems pageRows(st::Page p) { return st::pageItems(p, tgc::board::fixedLandscape()); }

// One row of a settings page: its label, its value (none when `value` is null
// or writes "") and what a hold does. menuItem() has one per st::Item.
struct MenuItem {
    st::Item id;
    const char* (*label)();
    void (*value)(char* out, size_t n);
    void (*action)();
};
const MenuItem& menuItem(st::Item id);  // the table, after the actions it calls

// The summary card (§2.4): the loaded deck's name, "12 due · 5 new" and its
// coverage (seen / total) as a bar. Portrait: a band over the group rows with
// "45/79" right of the name. Landscape: a column, one count per line.
void drawSummaryCard(lgfx::LovyanGFX& d, const tgc::layout::Home& l) {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    const tgc::layout::Rect& c = l.card;
    d.fillRect(c.x, c.y, c.w, c.h, kSurface);
    const int px = c.x + 6;
    const int pw = c.w - 12;
    if (!gPack->isOpen()) {
        gRender.drawLabel(d, ui().summaryNoDeck, px, c.y + 3, pw, 20, kText, kSurface);
        gRender.drawWrapped(d, ui().summaryNoDeckHint, px, c.y + 25, pw, l.cardColumn ? 4 : 1,
                            kTextDim, kSurface, kSmall);
        return;
    }
    const tgc::DeckSummary s = deckCounts();
    char due[24], fresh[24], seen[32];
    snprintf(due, sizeof due, ui().summaryDueFmt, (unsigned)s.due);
    snprintf(fresh, sizeof fresh, ui().summaryNewFmt, (unsigned)s.fresh);
    if (!l.cardColumn) {
        snprintf(seen, sizeof seen, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
        const int sw = wg::captionWidth(seen);
        gRender.drawLabel(d, gPack->deckName(), px, c.y + 3, pw - sw - 6, 20, kText, kSurface);
        wg::drawCaption(d, gRender, seen, c.right() - 6 - sw, c.y + 10, kTextDim, kSurface);
        int x = px;
        x += wg::drawCaption(d, gRender, due, x, c.y + 25, kAccentHi, kSurface);
        x += wg::drawCaption(d, gRender, " · ", x, c.y + 25, kTextFaint, kSurface);
        wg::drawCaption(d, gRender, fresh, x, c.y + 25, kTextDim, kSurface);
        wg::drawProgress(d, px, c.y + 37, pw, 3, static_cast<int>(s.seen()),
                         static_cast<int>(s.total));
        return;
    }
    snprintf(seen, sizeof seen, ui().summarySeenFmt, (unsigned)s.seen(), (unsigned)s.total);
    if (wg::captionWidth(seen) > pw)
        snprintf(seen, sizeof seen, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
    gRender.drawLabel(d, gPack->deckName(), px, c.y + 4, pw, 20, kText, kSurface);
    gRender.drawLabel(d, due, px, c.y + 30, pw, 8, kAccentHi, kSurface, kSmall);
    gRender.drawLabel(d, fresh, px, c.y + 42, pw, 8, kTextDim, kSurface, kSmall);
    gRender.drawLabel(d, seen, px, c.y + 54, pw, 8, kTextDim, kSurface, kSmall);
    wg::drawProgress(d, px, c.y + 67, pw, 3, static_cast<int>(s.seen()), static_cast<int>(s.total));
}

// One settings row. On the home: the group's icon, its value when it fits and
// a chevron. On a page: the value right of the label where rows are wide
// (landscape), else on a caption line under it; a row without a value keeps
// its label centred.
void drawMenuRow(lgfx::LovyanGFX& d, st::Item id, int x, int y, int w, int h, bool focused) {
    const MenuItem& m = menuItem(id);
    char value[48] = "";
    if (m.value) m.value(value, sizeof value);
    wg::Row row;
    row.label = m.label();
    row.icon = st::itemIcon(id);
    row.chevron = st::hasChevron(id);
    row.focused = focused;
    // A firmware update (docs/ota.md §4): the home's Device row has no room
    // for a version next to "Dispositivo" (92 of its 93 px in portrait). Wide
    // rows carry the update mark Get decks uses in place of the chevron; narrow
    // ones, where that mark (3 px wider than the chevron) would cut the label,
    // colour the row's icon instead. The Firmware row in the page names the
    // version.
    if (id == st::Item::Device && firmwareUpdateKnown()) {
        if (wideRows()) {
            row.mark = &tgc::kIconDown;
            row.markColor = tgc::theme::kWarn;
        } else {
            row.iconColor = tgc::theme::kWarn;
        }
    }
    if (value[0]) {
        if (gMenuPage == st::Page::Home) {
            row.value = value;
            // A network name is cut; the other values show whole or not at all.
            row.keepLabel = id == st::Item::Wifi;
            row.valueIfFits = !row.keepLabel;
        } else if (wideRows()) {
            row.value = value;
            row.keepLabel = true;
        } else {
            row.caption = value;
        }
    }
    wg::drawRow(d, gRender, x, y, w, h, row);
}

}  // namespace

void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const st::PageItems items = pageRows(gMenuPage);
    tgc::ListView& list = menuList();
    list.setCount(items.count);
    auto row = [&](int i, int x, int y, int w, int h, bool f) {
        drawMenuRow(d, items.items[i], x, y, w, h, f);
    };
    if (gMenuPage == st::Page::Home) {
        char date[16];
        shortDate(gSrs.today(), date, sizeof date);
        menuHeader(d, ui().settingsTitle, date);
        const tgc::layout::Home l = tgc::layout::homeFor(W, H);
        drawSummaryCard(d, l);
        layoutList(list, l.rowsTop, l.rowsBottom, layout().row1H);
        wg::drawList(d, list, l.rowsX, l.rowsW, row);
    } else {
        menuHeader(d, gMenuPage == st::Page::Study ? ui().studyTitle : ui().deviceTitle);
        layoutList(list, kHeaderH, H - kHintH - (gMenuNote[0] ? kNoteH : 0),
                   wideRows() ? layout().row1H : layout().row2H);
        wg::drawList(d, list, listX(), listW(), row);
        menuNote(d, gMenuNote, tgc::theme::kOkHi);
    }
    menuHints(d, ui().hintOpen);
    presentList(list);
}

namespace {

// A group row of the home opened its page: from the top.
void openMenuPage(st::Page p) {
    gMenuPage = p;
    menuList().setCount(pageRows(p).count);
    menuList().reset(0);
    gScreen = Screen::Menu;
    drawScreen();
}

}  // namespace

// Back to the settings tree from one of its screens (the page it came from).
void backToMenu() {
    gScreen = Screen::Menu;
    drawScreen();
}

void openMenu() {
    gAdvance.cancel();  // back on a revealed answer, a press goes on
    if (gDirty) {  // entering the menu is a flush point (docs/srs.md section 4)
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gBeforeMenu = gScreen;
    gScreen = Screen::Menu;
    gMenuPage = st::Page::Home;
    menuList().setCount(pageRows(gMenuPage).count);
    menuList().reset(0);
    drawScreen();
}

namespace {

void closeMenu() {
    if (gBeforeMenu == Screen::Decks) {  // no pack open (see portalPacksChanged())
        openDecks();
        return;
    }
    gScreen = gBeforeMenu;
    // The question was hidden behind the settings, or even built there (see
    // portalPacksChanged()): restart its answer clock, as wakeScreen() does,
    // so the time away does not grade it Hard.
    if (gScreen == Screen::Quiz && !gRevealed) {
        gShownAt = millis();
        gNavTaps = 0;
        gQuizHints.input(gShownAt);
    }
    if (gScreen == Screen::Intro) introReshown();
    drawScreen();
}

// Every settings row: its label (UiStrings), its value and what a hold does.
// A page lists its rows in ui/SettingsTree.h; a new setting gets an st::Item,
// a place on a page there and an entry here.
const MenuItem& menuItem(st::Item id) {
    using I = st::Item;
    static const MenuItem kItems[] = {
        // --- home
        {I::Decks, [] { return ui().groupRows[0]; },
         [](char* o, size_t n) {
             if (gUpdateCount == 1)
                 snprintf(o, n, "%s", ui().updatesCountOne);
             else if (gUpdateCount > 0)
                 snprintf(o, n, ui().updatesCountFmt, (unsigned)gUpdateCount);
             else
                 snprintf(o, n, "%u", (unsigned)gPacks.size());
         },
         openDecks},
        {I::Study, [] { return ui().groupRows[1]; },
         [](char* o, size_t n) { snprintf(o, n, ui().studyValueFmt, (unsigned)gSrs.newPerDay()); },
         [] { openMenuPage(st::Page::Study); }},
        {I::Stats, [] { return ui().groupRows[2]; },
         [](char* o, size_t n) {
             const tgc::Stats s = gSrs.stats();
             const int acc = st::accuracyPercent(s.answers, s.again);
             if (acc >= 0) snprintf(o, n, "%d%%", acc);
         },
         openStats},
        {I::Wifi, [] { return ui().groupRows[3]; },
         // The radio is on only on the Wi-Fi screens, so the home sees it off.
         [](char* o, size_t n) {
             snprintf(o, n, "%s", gHome.active() ? gHome.ssid() : ui().wifiOff);
         },
         [] { openWifiMenu(true); }},
        {I::Device, [] { return ui().groupRows[4]; }, nullptr,
         [] { openMenuPage(st::Page::Device); }},
        // --- Study
        {I::NewPerDay, [] { return ui().studyRows[0]; },
         [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)gSrs.newPerDay()); },
         [] { openChoice(Choice::NewPerDay); }},
        {I::Introduce, [] { return ui().studyRows[1]; },
         [](char* o, size_t n) { snprintf(o, n, "%s", ui().introRows[gIntroOn ? 0 : 1]); },
         [] { openChoice(Choice::Introduce); }},
        {I::HardAfter, [] { return ui().studyRows[2]; },
         [](char* o, size_t n) { hardAfterText(gSrs.slowMs(), o, n); },
         [] { openChoice(Choice::SlowMs); }},
        {I::AutoAdvance, [] { return ui().studyRows[3]; },
         [](char* o, size_t n) {
             snprintf(o, n, "%s", ui().advanceRows[gAdvancePref % tgc::kAdvanceChoices]);
         },
         [] { openChoice(Choice::AutoAdvance); }},
        {I::OnKana, [] { return ui().studyRows[4]; },
         [](char* o, size_t n) {
             snprintf(o, n, "%s", ui().onKanaRows[static_cast<int>(gOnKana) % 2]);
         },
         [] { openChoice(Choice::OnKana); }},
        {I::DeleteProgress, [] { return ui().studyRows[5]; },
         [](char* o, size_t n) { snprintf(o, n, "%s", ui().deleteRowValue); },
         [] { openReset(true); }},
        // --- Device
        {I::Orientation, [] { return ui().deviceRows[0]; },
         [](char* o, size_t n) { snprintf(o, n, "%s", ui().orientRows[shownOrientPref() % 3]); },
         [] { openChoice(Choice::Orientation); }},
        {I::Brightness, [] { return ui().deviceRows[1]; },
         [](char* o, size_t n) { snprintf(o, n, "%u%%", (unsigned)(gBrightness * 100u / 255u)); },
         [] { openChoice(Choice::Brightness); }},
        {I::Language, [] { return ui().deviceRows[2]; },
         [](char* o, size_t n) {
             const int pref = gUiLangPref >= 0 && gUiLangPref < 3 ? gUiLangPref : 0;
             snprintf(o, n, "%s", ui().langRows[pref]);
         },
         [] { openChoice(Choice::UiLang); }},
        {I::HoldTime, [] { return ui().deviceRows[3]; },
         [](char* o, size_t n) {
             snprintf(o, n, "%s", ui().holdRows[gHoldPref % tgc::kHoldChoices]);
         },
         [] { openChoice(Choice::HoldTime); }},
        {I::Date, [] { return ui().deviceRows[4]; },
         [](char* o, size_t n) { shortDate(gSrs.today(), o, n); }, [] { openDateMenu(true); }},
        {I::Sleep, [] { return ui().deviceRows[5]; }, nullptr, [] { sleepNow(false); }},
        {I::Firmware, [] { return ui().deviceRows[6]; }, firmwareRowValue, openFirmware},
        {I::About, [] { return ui().deviceRows[7]; }, nullptr, openAbout},
    };
    static_assert(sizeof kItems / sizeof kItems[0] == st::kItemCount, "one entry per item");
    for (const MenuItem& m : kItems)
        if (m.id == id) return m;
    return kItems[0];
}

// A hold on the focused settings row.
void activateSetting() {
    menuItem(st::itemAt(gMenuPage, menuList().focus(), tgc::board::fixedLandscape())).action();
}

}  // namespace

// loop() on the settings tree: tap moves, hold activates, the side button goes
// from a page back to the home and from the home out of the settings.
void menuLoop(int a, int b) {
    const bool note = gMenuNote[0] != 0;
    if (a || b) gMenuNote[0] = 0;  // a page's note lasts until the next press
    if (a == 1) {
        moveListFocus(menuList());
        if (note) dropPartialPush();  // the note goes: the whole frame
        drawScreen();
    } else if (a == 2) {
        activateSetting();
    }
    if (b && gScreen == Screen::Menu) {
        if (gMenuPage != st::Page::Home) {
            gMenuPage = st::Page::Home;  // the home's focus is still on the group
            drawScreen();
        } else {
            closeMenu();
        }
    }
}

}  // namespace app
}  // namespace tgc
