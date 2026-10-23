// Progress operations; see Progress.h.
#include "app/Progress.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "app/App.h"
#include "app/Clock.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/FsQuiet.h"
#include "platform/FsSrsFile.h"
#include "platform/WifiPortal.h"
#include "render/Widgets.h"
#include "srs/DeckProgress.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

// --- Deleting progress (docs/srs.md section 2.1) ----------------------------

namespace {

// Writes the state file through the safe path (temp + rename). False when it
// failed; the answers not yet saved then stay counted for the next flush.
bool saveProgress() {
    if (!gSrs.save(gSrsIo)) {
        Serial.printf("srs: save failed\n");
        return false;
    }
    gDirty = 0;
    return true;
}

// The code points of the pack at `path`, sorted and unique; none when it does
// not read (its kanji cannot be known, so nothing of it is forgotten).
std::vector<uint32_t> packCodepoints(const String& path) {
    std::vector<uint32_t> cps;
    tgc::FsPackSource src(LittleFS.open(path, "r"));
    if (src.ok()) tgc::readPackCodepoints(src, &cps);
    tgc::sortUnique(cps);
    return cps;
}

// The code points of every pack in /packs but `except`, sorted and unique.
std::vector<uint32_t> otherDecksCodepoints(const String& except) {
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == except) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    return all;
}

}  // namespace

// The portal's POST /delete (WifiPortal::Hooks::onDeleteDeck): deletes pack
// `path` and the progress of its kanji that no other deck in /packs has, so a
// later download of it starts from scratch; N5's kanji keep theirs while the
// Jouyou deck is installed. The lifetime statistics stay (docs/srs.md 2.1).
// Order: the records are forgotten and the state file saved (temp + rename)
// *before* the pack is removed. A power cut in between leaves the deck
// installed without that progress (deleting it again finishes the job), never
// records of a deck that is gone, which a new download of it would pick up
// again. A failed save (a full filesystem: deleting a deck is how space is
// made) is tried again once the pack is gone. Then the folder is rescanned as
// after any other change: a deleted loaded deck gives way to the first one
// left, and the queue is rebuilt (portalPacksChanged()).
bool portalDeleteDeck(const char* path, uint32_t* forgotten) {
    *forgotten = 0;
    const String p(path);
    if (!tgc::fsExists(p)) return false;
    const std::vector<uint32_t> deck = packCodepoints(p);
    const std::vector<uint32_t> gone = tgc::forgetOnDelete(deck, otherDecksCodepoints(p));
    const size_t n = gSrs.forget(gone.data(), gone.size());
    const bool saved = n == 0 || saveProgress();
    const bool removed = LittleFS.remove(p);
    if (!saved) saveProgress();
    Serial.printf("delete %s: %s; progress of %u of its %u kanji forgotten, %u kanji left\n",
                  path, removed ? "ok" : "failed", (unsigned)n, (unsigned)deck.size(),
                  (unsigned)gSrs.count());
    portalPacksChanged();
    *forgotten = static_cast<uint32_t>(n);
    return removed;
}

// The portal's POST /progress (WifiPortal::Hooks::onRestoreProgress): replace
// the SRS state with the portal's validated temp file. Order matters: the
// current state is saved and then kept as /state/srs.before-restore.bin, so a
// failed rename or reload can put it back -- a restore never loses progress
// (docs/srs.md section 4). The page posts its clock right after the redirect,
// which aligns the restored day index with this device's date (section 5.1).
bool portalRestoreProgress(const char* tmpPath) {
    if (!tmpPath || std::strcmp(tmpPath, tgc::WifiPortal::kProgressTmp) != 0) return false;
    const String tmp = tgc::WifiPortal::kProgressTmp;
    const String state = tgc::FsSrsFile::kPath;
    const String safety = tgc::FsSrsFile::kBeforeRestore;

    // Persist the answers not yet flushed before they can be displaced.
    if (!saveProgress()) return false;

    // Keep the live file. An old safety copy is replaced: it is this file's.
    if (tgc::fsExists(safety) && !LittleFS.remove(safety)) {
        Serial.printf("srs: restore: cannot remove the old safety copy\n");
        return false;
    }
    if (!LittleFS.rename(state, safety)) {
        Serial.printf("srs: restore: cannot keep the current state\n");
        return false;
    }
    if (!LittleFS.rename(tmp, state)) {
        Serial.printf("srs: restore: rename failed, putting the previous state back\n");
        LittleFS.rename(safety, state);
        return false;
    }

    if (!gSrs.begin(gSrsIo)) {
        // The portal validated the bytes, so this should not happen; if it
        // does, put the previous file back rather than leave an empty store.
        Serial.printf("srs: restore: reload failed, putting the previous state back\n");
        tgc::fsRemoveIfExists(state);
        LittleFS.rename(safety, state);
        gSrsLoaded = gSrs.begin(gSrsIo);
        gSrs.beginSession();
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        if (gPack->isOpen()) refreshAfterRebuild(gBeforeMenu, true);
        return false;
    }
    gSrs.beginSession();
    gSrsLoaded = true;
    gBootDay = gSrs.today();
    gExtraNew = 0;
    gDirty = 0;
    // The old question belongs to the old progress: build a fresh one for the
    // screen the settings home's "Continue" returns to, as after a pack change.
    if (!gPack->isOpen()) {
        gQueue.rebuild(*gPack, gSrs, gRng);
        gBeforeMenu = Screen::Decks;
    } else {
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        gBeforeMenu = nextItem();
    }
    Serial.printf("srs: restored from the portal: day %u, records %u\n", (unsigned)gSrs.today(),
                  (unsigned)gSrs.count());
    return true;
}

// --- Settings > Study > Delete progress (docs/srs.md section 2.1) ----------
// A two-row list -- "This deck" (the loaded deck's kanji, shared ones too) and
// "Everything" (every record and the lifetime statistics) -- whose hold opens
// a confirmation: what goes, in numbers, over one row that only a hold
// confirms (the hold-progress fill shows on it); the side button cancels. Then
// back to the Study page with a note; the queue, the summary card and every
// count follow the store.
tgc::ListView gResetList;
char gResetNote[32] = "";         // the list's note ("Nothing to delete")
uint32_t gResetConfirmAt = 0;     // millis() when it opened (keyAction()'s Enter guard)

namespace {

tgc::ListView gResetConfirmList;  // the confirmation's one row
bool gResetAll = false;           // the confirmation is for Everything
std::vector<uint32_t> gResetCps;  // This deck: its kanji, sorted and unique
tgc::DeckReset gResetPreview;     // This deck: its kanji with progress, shared ones
int gResetSharedDecks = 0;        // other installed decks that share some of them
String gResetSharedName;          // the one such deck's name, when only one
constexpr int kResetRows = 2;

}  // namespace

void openReset(bool fresh) {
    if (fresh) {
        gResetList.setCount(kResetRows);
        gResetList.reset(0);
    }
    gResetNote[0] = 0;
    gScreen = Screen::Reset;
    drawScreen();
}

void drawReset() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    const bool wide = wideRows();
    gResetList.setCount(kResetRows);
    layoutList(gResetList, kHeaderH, H - kHintH - (gResetNote[0] ? kNoteH : 0),
               wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gResetList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().resetRows[i];
        row.focused = f;
        const char* second = i == 1 ? ui().resetAllCaption
                                    : (gPack->isOpen() ? gPack->deckName() : ui().resetNoDeck);
        if (wide) {
            row.value = second;
            row.keepLabel = true;
        } else {
            row.caption = second;
        }
        if (i == 0 && !gPack->isOpen()) row.muted = true;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gResetNote, tgc::theme::kWarn);
    menuHints(d, ui().hintChoose);
    presentList(gResetList);
}

namespace {

// The other installed decks' share of the loaded deck's kanji with progress:
// how many of them (over all those decks) and, when a single deck shares any,
// its name.
void resetSharedPreview() {
    gResetSharedDecks = 0;
    gResetSharedName = "";
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == gCurrentPack) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        if (tgc::deckResetPreview(gSrs, gResetCps, cps).shared == 0) continue;
        if (++gResetSharedDecks == 1) {
            tgc::PackInfo info;
            tgc::FsPackSource src(LittleFS.open(p, "r"));
            const bool named = src.ok() && tgc::peekPackInfo(src, &info) == tgc::PackError::Ok &&
                               info.name[0];
            gResetSharedName = named ? String(info.name) : packFileStem(p);
        }
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    gResetPreview = tgc::deckResetPreview(gSrs, gResetCps, all);
}

// A hold on a row: the confirmation, or a note when there is nothing to delete.
void openResetConfirm() {
    gResetAll = gResetList.focus() == 1;
    gResetNote[0] = 0;
    if (gResetAll) {
        if (gSrs.count() == 0 && gSrs.answeredTotal() == 0 && gSrs.reviewMs() == 0 &&
            !gSrs.hasStudied())
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    } else if (!gPack->isOpen()) {
        snprintf(gResetNote, sizeof gResetNote, "%s", ui().summaryNoDeck);
    } else {
        gResetCps = packCodepoints(gCurrentPack);
        resetSharedPreview();
        if (gResetPreview.records == 0)
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    }
    if (gResetNote[0]) {
        drawScreen();
        return;
    }
    gResetConfirmList.setCount(1);
    gResetConfirmList.reset(0);
    gScreen = Screen::ResetConfirm;
    gResetConfirmAt = millis();
    drawScreen();
}

}  // namespace

void drawResetConfirm() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    // What goes: the deck (or "Everything") in the label face, then in numbers.
    const char* what = gResetAll ? ui().resetRows[1] : gPack->deckName();
    gRender.drawLabel(d, what, kGutter, kHeaderH + 4, W - 2 * kGutter, 20, kWarn, kBlack,
                      tgc::Renderer::kTextUi, tgc::Align::Centre);
    const int rowH = layout().row1H;
    const int rowsTop = H - kHintH - rowH - 2 * kMarkerBand;
    const int textTop = kHeaderH + 28;
    char text[200];
    if (gResetAll)
        snprintf(text, sizeof text, ui().resetAllFmt, (unsigned)gSrs.count());
    else
        snprintf(text, sizeof text, ui().resetDeckFmt, (unsigned)gResetPreview.records);
    const int used = drawCaptionLines(d, text, kGutter, textTop, W - 2 * kGutter,
                                      std::max(0, (rowsTop - textTop) / 10), kWhite);
    // Kanji other decks share go too: said in warn colour, under a small gap.
    const int sharedTop = textTop + 10 * used + 6;
    if (!gResetAll && gResetPreview.shared > 0 && rowsTop - sharedTop >= 10) {
        if (gResetSharedDecks == 1)
            snprintf(text, sizeof text, ui().resetSharedOneFmt, gResetSharedName.c_str(),
                     (unsigned)gResetPreview.shared);
        else
            snprintf(text, sizeof text, ui().resetSharedManyFmt, (unsigned)gResetPreview.shared);
        drawCaptionLines(d, text, kGutter, sharedTop, W - 2 * kGutter, (rowsTop - sharedTop) / 10,
                         kWarn);
    }
    gResetConfirmList.setCount(1);
    layoutList(gResetConfirmList, rowsTop, H - kHintH, rowH);
    wg::drawList(d, gResetConfirmList, listX(), listW(),
                 [&](int, int x, int y, int w, int h, bool f) {
                     wg::Row row;
                     row.label = ui().resetConfirmRow;
                     row.focused = f;
                     wg::drawRow(d, gRender, x, y, w, h, row);
                 });
    wg::Hints hints;
    hints.hold = ui().hintDelete;
    hints.side = &tgc::kIconBack;
    hints.sideWord = ui().hintCancel;
    drawHints(d, hints);
    present();
}

namespace {

// The confirmation's hold: forget, save (temp + rename), and let the queue and
// the screen the settings return to follow, as after a deck change: the
// question kept for them may be a card whose progress just went.
void applyReset() {
    size_t n = 0;
    if (gResetAll) {
        n = gSrs.resetProgress();
        snprintf(gMenuNote, sizeof gMenuNote, "%s", ui().resetAllDone);
    } else {
        n = gSrs.forget(gResetCps.data(), gResetCps.size());
        snprintf(gMenuNote, sizeof gMenuNote, ui().resetDoneFmt, (unsigned)n);
    }
    saveProgress();
    Serial.printf("progress: %s: %u kanji forgotten, %u left, %u answers\n",
                  gResetAll ? "everything" : gCurrentPack.c_str(), (unsigned)n,
                  (unsigned)gSrs.count(), (unsigned)gSrs.answeredTotal());
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gPack->isOpen()) gBeforeMenu = nextItem();
    gResetCps.clear();
    gResetCps.shrink_to_fit();
    gMenuPage = st::Page::Study;
    gScreen = Screen::Menu;
    drawScreen();
}

}  // namespace

void resetLoop(int a, int b) {
    if (a == 1) {
        const bool note = gResetNote[0] != 0;
        gResetNote[0] = 0;
        moveListFocus(gResetList);
        if (note) dropPartialPush();  // the note goes: the whole frame
        drawScreen();
    } else if (a == 2) {
        openResetConfirm();
    }
    if (b && gScreen == Screen::Reset) backToMenu();  // the Study page
}

// Only a hold deletes; a tap does nothing, the side button cancels.
void resetConfirmLoop(int a, int b) {
    if (a == 2) {
        applyReset();
    } else if (b) {
        gScreen = Screen::Reset;
        drawScreen();
    }
}

}  // namespace app
}  // namespace tgc
