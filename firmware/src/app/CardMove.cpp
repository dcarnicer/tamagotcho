// The device's decks moving to the microSD card; see CardMove.h.
#include "app/CardMove.h"

#include <Arduino.h>

#include <cstring>
#include <vector>

#include "app/App.h"
#include "app/Decks.h"
#include "app/Display.h"
#include "app/Power.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "platform/NetFetch.h"
#include "platform/Storage.h"
#include "render/Widgets.h"
#include "storage/DeckFiles.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace {

// What the screen shows: the deck being moved (1-based) of how many, the
// bytes of the decks done and of all of them, and that deck's file name.
struct MoveView {
    int index = 0;
    int count = 0;
    uint64_t done = 0;
    uint64_t total = 0;
    int pct = -1;
    const char* name = "";
};
MoveView gMove;

// The move's screen: the header with "2/5", the status in the label face, a
// bar of the bytes moved and the deck's file name under it. No hint bar:
// nothing to press (docs/microsd.md §3.6).
void drawMove() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    char count[12];
    snprintf(count, sizeof count, "%d/%d", gMove.index, gMove.count);
    menuHeader(d, ui().moveTitle, count);
    const int mid = kHeaderH + (H - kHeaderH) / 2;
    gRender.drawCentred(d, ui().moveStatus, 4, mid - 38, W - 8, 40, 2, kText, kBg,
                        tgc::Renderer::kTextUi);
    wg::drawProgress(d, kGutter, mid + 8, W - 2 * kGutter, 4, gMove.pct < 0 ? 0 : gMove.pct, 100);
    drawCaptionLines(d, gMove.name, 4, mid + 18, W - 8, 1, kTextDim);
    present();
}

// copyFile()'s progress: the whole move's share, redrawn when it changes.
void moveProgress(size_t done, void*) {
    if (!gMove.total) return;
    const int pct = static_cast<int>((gMove.done + done) * 100u / gMove.total);
    if (pct == gMove.pct) return;
    gMove.pct = pct;
    drawMove();
}

enum class Moved : uint8_t { Yes, NoRoom, Failed, KeptBoth };

const char* movedString(Moved m) {
    switch (m) {
        case Moved::Yes: return "moved";
        case Moved::NoRoom: return "kept: the card is full";
        case Moved::Failed: return "kept: the copy failed";
        case Moved::KeptBoth: return "copied, but its LittleFS file stays";
    }
    return "?";
}

// One deck, docs/microsd.md §3.6: the room for it, the copy into the card's
// temp (its SHA-256 taken on the way), the check of the copy (size, SHA-256 of
// what the card holds, the pack probe), the card's copy of the same name
// replaced, then the LittleFS file removed -- the loaded deck closed first,
// and the paths that named it pointed at the card. Any failure before the
// rename leaves the LittleFS deck as it was and removes the temp.
Moved moveOne(const tgc::DeckFile& deck) {
    const String src = deck.path.c_str();
    const String dst = tgc::storage::deckPathFor(tgc::DeckPlace::Card, deck.name.c_str());
    const String tmp = tgc::storage::deckPathFor(tgc::DeckPlace::Card, tgc::kMoveTmpName);
    const long old = tgc::storage::fileSize(dst);
    const tgc::MoveFit fit = tgc::moveFit(deck.bytes, tgc::storage::cardFreeBytes(),
                                          old > 0 ? static_cast<uint64_t>(old) : 0);
    if (fit == tgc::MoveFit::NoRoom) return Moved::NoRoom;
    tgc::storage::removeIfExists(tmp);
    // Room only in place of the old copy: it goes first (the device's copy,
    // the one that wins, stays on LittleFS until the new one checked).
    if (fit == tgc::MoveFit::FitsInPlaceOfOld && !tgc::storage::removeFile(dst))
        return Moved::Failed;
    uint8_t want[32], got[32];
    bool ok = tgc::storage::copyFile(src, tmp, want, moveProgress);
    ok = ok && tgc::storage::fileSize(tmp) == static_cast<long>(deck.bytes);
    ok = ok && tgc::storage::sha256(tmp, got) && std::memcmp(want, got, sizeof want) == 0;
    ok = ok && tgc::validatePackFile(tmp);
    if (ok) {
        tgc::storage::removeIfExists(dst);  // FAT renames do not overwrite
        ok = tgc::storage::renameFile(tmp, dst);
    }
    if (!ok) {
        tgc::storage::removeIfExists(tmp);
        return Moved::Failed;
    }
    // LittleFS will not remove an open file: the loaded deck closes, and the
    // caller's rescan reopens it (at its new path, set below).
    closePackIfLoaded(src);
    if (!tgc::storage::removeFile(src)) return Moved::KeptBoth;  // the next mount retries
    if (gCurrentPack == src) gCurrentPack = dst;
    if (gPrefs.getString("pack", "") == src) gPrefs.putString("pack", dst);
    return Moved::Yes;
}

}  // namespace

bool moveDecksToCard() {
    if (!tgc::storage::cardMounted()) return false;
    std::vector<tgc::DeckFile> decks;
    tgc::storage::listFiles(tgc::DeckPlace::Device, decks);
    if (decks.empty()) return false;
    gMove = MoveView{};
    gMove.count = static_cast<int>(decks.size());
    for (const tgc::DeckFile& d : decks) gMove.total += d.bytes;
    const bool wasOpen = gPack->isOpen();
    const uint32_t t0 = millis();
    int moved = 0;
    for (const tgc::DeckFile& d : decks) {
        ++gMove.index;
        gMove.name = d.name.c_str();
        drawMove();
        const uint32_t t1 = millis();
        const Moved m = moveOne(d);
        Serial.printf("[sd] move %s (%u bytes): %s (%u ms)\n", d.name.c_str(), (unsigned)d.bytes,
                      movedString(m), (unsigned)(millis() - t1));
        if (m == Moved::Yes) ++moved;
        // A deck that does not fit stops the move: the card is full, so the
        // later decks stay too.
        if (m == Moved::NoRoom) break;
        gMove.done += d.bytes;
    }
    Serial.printf("[sd] moved %d of %d decks to the card in %u ms\n", moved, gMove.count,
                  (unsigned)(millis() - t0));
    gSwallowInput = true;  // presses made meanwhile must not act now
    return moved > 0 || (wasOpen && !gPack->isOpen());
}

bool mountCardOnOpen() {
    using tgc::storage::CardState;
    if (tgc::storage::cardMounted()) {
        if (tgc::storage::cardAnswers()) return false;
        // It went away since (no deck of it was being read): the loaded deck,
        // if it is on the card, closes before the card is unmounted and tried
        // again. Another card in its place mounts like a new one.
        if (tgc::storage::onCard(gCurrentPack)) closePackIfLoaded(gCurrentPack);
        if (tgc::storage::readFailed() != CardState::Mounted) {
            portalPacksChanged();
            return false;
        }
    } else if (!tgc::storage::remount()) {
        return false;
    }
    const bool hadDeck = gPack->isOpen();
    // The card's own decks join the list too; with no deck loaded yet, the
    // rescan loads one of them.
    if (moveDecksToCard() || !hadDeck)
        portalPacksChanged();
    else
        listPacks(gPacks);
    return true;
}

}  // namespace app
}  // namespace tgc
