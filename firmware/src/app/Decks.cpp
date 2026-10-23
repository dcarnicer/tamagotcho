// The decks on the device and the deck list; see Decks.h.
#include "app/Decks.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "app/App.h"
#include "app/DeckUpdates.h"
#include "app/Display.h"
#include "app/Done.h"
#include "app/GetDecks.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "render/Widgets.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

std::vector<String> gPacks;  // paths of the *.tgc files in /packs
String gCurrentPack;

// Fill `out` with the *.tgc files in /packs (sorted for a stable menu order).
void listPacks(std::vector<String>& out) {
    out.clear();
    File dir = LittleFS.open("/packs");
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!f.isDirectory() && name.endsWith(".tgc") && !name.startsWith("."))
            out.push_back("/packs/" + name);
    }
    std::sort(out.begin(), out.end());
}

// The loaded deck's counts (ui/DeckSummary.h): one pass over the card table,
// for the boot log, the settings summary, the deck list, the statistics and
// the done screen.
tgc::DeckSummary deckCounts() { return tgc::deckSummary(*gPack, gSrs); }

// Deck list (opened from "Decks"; docs/ux-refresh.md, decks mockup): one row
// per pack with the deck name from its header, a language pill and "79 kanji"
// ("79 kanji · 12 due" for the loaded one, whose radio is on); the caption
// goes under the name in portrait, right of it in landscape. Hold loads the
// focused pack; the side button returns to the settings home. The last row,
// "Get decks", opens the download screen (drawGetDecks).
tgc::ListView gDeckList;

namespace {

// The header of each pack in gPacks (peekPackInfo(): two small reads a file),
// read when the deck list opens (an update keeps the file name but may change
// the header) and again whenever the list of files changed under it.
std::vector<tgc::PackInfo> gPackInfo;
std::vector<String> gPackInfoFor;  // the gPacks the infos were read for

void refreshPackInfo() {
    if (gPackInfoFor == gPacks) return;
    gPackInfo.assign(gPacks.size(), tgc::PackInfo{});
    for (size_t i = 0; i < gPacks.size(); ++i) {
        tgc::FsPackSource src(LittleFS.open(gPacks[i], "r"));
        if (src.ok()) tgc::peekPackInfo(src, &gPackInfo[i]);
    }
    gPackInfoFor = gPacks;
}

}  // namespace

// A pack's file name without the folder and ".tgc", for a deck whose header
// gives no name.
String packFileStem(const String& path) {
    String s = path;
    const int slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.substring(slash + 1);
    if (s.endsWith(".tgc")) s = s.substring(0, s.length() - 4);
    return s;
}

void drawDecks() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().decksTitle);
    refreshPackInfo();
    const uint32_t due = deckCounts().due;  // only shown on the loaded pack's row
    const int packs = static_cast<int>(gPacks.size());
    const int updatesRow = packs;
    const int getDecksRow = packs + 1;
    gDeckList.setCount(packs + 2);  // + "Updates" and "Get decks"
    const bool wide = wideRows();
    layoutList(gDeckList, kHeaderH, H - kHintH, wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gDeckList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.focused = f;
        String stem;
        char caption[48], lang[8];
        if (i == updatesRow) {
            row.label = ui().updatesRow;
            row.icon = &tgc::kIconDown;
            row.chevron = true;
            char value[24];
            if (gUpdateCount > 0)
                snprintf(value, sizeof value, "%u", (unsigned)gUpdateCount);
            else
                snprintf(value, sizeof value, "%s",
                         gUpdateKnown ? ui().updatesUpToDate : ui().updatesCheck);
            (wide ? row.value : row.caption) = value;
            row.labelColor = gUpdateCount ? tgc::theme::kAccentHi : tgc::theme::kText;
        } else if (i == getDecksRow) {
            row.label = ui().getDecksRow;
            row.icon = &tgc::kIconDown;
            row.chevron = true;
            (wide ? row.value : row.caption) = ui().getDecksCaption;
            row.labelColor = tgc::theme::kAccentHi;
        } else {
            const tgc::PackInfo& info = gPackInfo[i];
            const bool loaded = gPacks[i] == gCurrentPack;
            if (info.name[0]) {
                row.label = info.name;
            } else {
                stem = packFileStem(gPacks[i]);
                row.label = stem.c_str();
            }
            snprintf(lang, sizeof lang, "%s", info.lang);
            for (char* p = lang; *p; ++p)
                if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
            if (lang[0]) row.tag = lang;
            if (loaded)
                snprintf(caption, sizeof caption, ui().deckCountsFmt, (unsigned)info.cards,
                         (unsigned)due);
            else if (info.cards)
                snprintf(caption, sizeof caption, ui().deckCardsFmt, (unsigned)info.cards);
            else
                caption[0] = 0;
            row.radio = loaded ? wg::Radio::On : wg::Radio::Off;
            if (wide) {
                row.value = caption;
                row.keepLabel = true;
            } else {
                row.caption = caption;
            }
        }
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintOpen);
    presentList(gDeckList);
}

// Opens `path`, replacing the live pack only after the new one validates.
// The new reader is opened next to the live one and swapped in only once it
// validated, so a failure leaves the live pack (and the renderer and queue that
// point at it) untouched.
bool loadPack(const String& path) {
    auto src = std::make_unique<tgc::FsPackSource>(LittleFS.open(path, "r"));
    if (!src->ok()) return false;
    auto next = std::make_unique<tgc::PackReader>();
    if (next->open(*src, tgc::psramAllocator(), tgc::packMemory()) != tgc::PackError::Ok)
        return false;
    // Progress is global (keyed by codepoint), so persist it before the swap.
    if (gDirty) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gRender.end();
    gPack = std::move(next);  // the old reader closes before its source goes
    gSource = std::move(src);
    if (!gRender.begin(*gPack, tgc::glyphCacheBudget())) {
        // Out of memory for the glyph cache: leave a consistent "no pack" state
        // rather than a pack the renderer cannot draw.
        gPack->close();
        gCurrentPack = "";
        return false;
    }
    setUiForLang(gPack->header().lang);
    gCurrentPack = path;
    gQueue.rebuild(*gPack, gSrs, gRng);
    Serial.printf("pack %s: %u cards, %u glyphs, lang %s, tables %s (%u bytes); internal heap "
                  "%u free, %u largest\n",
                  gPack->deckName(), (unsigned)gPack->cardCount(), (unsigned)gPack->glyphCount(),
                  gPack->header().lang, gPack->streamed() ? "streamed" : "in memory",
                  (unsigned)gPack->memoryBytes(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

// Called by the portal after /packs changed, and by Get decks after a download.
// Rescans the folder and keeps the live pack usable: reload the loaded pack (its
// contents may have changed), else fall back to the first remaining one, else
// leave no pack open. Both callers run under the settings home, whose
// "Continue" returns to the quiz, detail or done screen; when the live reader
// was replaced or closed, the question kept for it belongs to the old pack and
// must never be shown or graded. So Continue gets a fresh question from the
// rebuilt queue (or the done screen) -- or, with no pack left, the deck list,
// where "+ Get decks" still works. A failed reload that kept the old reader
// keeps its question too.
bool portalPacksChanged() {
    const tgc::PackReader* const before = gPack->isOpen() ? gPack.get() : nullptr;
    const String pathBefore = gCurrentPack;
    listPacks(gPacks);
    if (gPacks.empty()) {
        gRender.end();
        gPack->close();
        gSource.reset();
        gCurrentPack = "";
        gPrefs.remove("pack");
    } else {
        String want = gCurrentPack;
        if (want.isEmpty() || std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end())
            want = gPacks[0];
        if (loadPack(want) || loadPack(gPacks[0])) gPrefs.putString("pack", gCurrentPack);
    }
    if (!gPack->isOpen()) {
        gQueue.rebuild(*gPack, gSrs, gRng);  // empty: nothing refers to the old cards
        gBeforeMenu = Screen::Decks;
    } else if (gPack.get() != before) {
        if (gCurrentPack != pathBefore) gExtraNew = 0;  // another deck, as in selectPack()
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        gBeforeMenu = newQuestion() ? Screen::Quiz : Screen::Done;
    }
    return gPack->isOpen();
}

// Opens the deck list with the loaded pack focused.
void openDecks() {
    int sel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) sel = static_cast<int>(i);
    if (gPacks.empty()) sel = 1;  // no deck to focus: "Get decks", not "Updates"
    gDeckList.setCount(static_cast<int>(gPacks.size()) + 2);  // + "Updates" and "Get decks"
    gDeckList.reset(sel);
    gPackInfoFor.clear();  // read the headers again (drawDecks())
    gScreen = Screen::Decks;
    drawScreen();
}

namespace {

// Loads the focused deck (replacing the live pack only after it validates),
// resets the "study more" override and starts the quiz.
void selectPack() {
    bool loaded = false;
    gExtraNew = 0;
    const int sel = gDeckList.focus();
    if (sel >= 0 && sel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[sel];
        if (loadPack(path)) {
            gPrefs.putString("pack", path);
            loaded = true;
        }
    }
    if (loaded && !newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

}  // namespace

// loop() on the deck list (handleInput()).
void decksLoop(int a, int b) {
    if (a == 1) {
        gDeckList.setCount(static_cast<int>(gPacks.size()) + 2);  // + "Updates" and "Get decks"
        moveListFocus(gDeckList);
        drawScreen();
    } else if (a == 2) {
        const int packs = static_cast<int>(gPacks.size());
        if (gDeckList.focus() == packs)
            openGetDecks(false, true);  // the "Updates" row
        else if (gDeckList.focus() == packs + 1)
            openGetDecks(false);  // the "+ Get decks" row
        else
            selectPack();
    }
    if (b) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

}  // namespace app
}  // namespace tgc
