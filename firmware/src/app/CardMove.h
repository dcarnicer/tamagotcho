#pragma once
// The device's decks moving to the microSD card (docs/microsd.md §3.6, D4):
// whenever a card mounts -- at boot, before any deck opens, or when the deck
// list or Get decks mounts one later -- every deck on LittleFS moves to the
// card's decks folder, each one checked on the card before its LittleFS copy
// goes, on a screen of its own ("Moving decks to the card..." over a bar,
// "2/5" in the header). It blocks for a few seconds; nothing cancels it (each
// deck is atomic, and the next mount finishes the rest).

namespace tgc {
namespace app {

// Moves what fits, in file-name order; a deck the card has no room for stays
// on LittleFS and stops the move (the card is full). The loaded deck may be
// among them: it is closed before its LittleFS file goes and gCurrentPack
// names its new path, as NVS `pack` does when it named the old one. True when
// a deck moved or was closed: the caller rescans (portalPacksChanged()).
// `showFullNotice`: the blocking "Card full" notice after the move (boot);
// false when the deck list retries the move and its own top line says it.
bool moveDecksToCard(bool showFullNotice = true);
// How many decks the last move left on LittleFS because the card was full;
// 0 when it did not stop for room. Cleared when a move runs again.
int cardMoveFullCount();
// The deck list and Get decks, on opening: a card put in since (none was
// mounted) is mounted, the device's decks move to it and the decks are
// rescanned. A mounted card that no longer answers (pulled out while no deck
// of it was read) is unmounted first, "removed" if no card answers at all.
// True when a card mounted.
bool mountCardOnOpen();

}  // namespace app
}  // namespace tgc
