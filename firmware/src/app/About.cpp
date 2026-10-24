// Settings > Device > About; see About.h.
#include "app/About.h"

#include <string>
#include <vector>

#include "Credits.h"
#include "Version.h"
#include "app/App.h"
#include "app/Display.h"
#include "app/ScreenKit.h"
#include "app/UiStrings.h"
#include "platform/Storage.h"
#include "render/Widgets.h"
#include "storage/DeckFiles.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace {

// Settings > Device > About: the firmware's name, version and licence, the
// decks' licence, the UI font and where the source lives, and on a board with
// a microSD slot the card ("29.7 GB, 28.1 free", or why there is none); then
// the loaded deck and the credits of its data (Credits.h, from
// docs/data-sources.md). Each item is
// a label-face heading over its value in captions, so it works before any deck
// is loaded. Two groups -- the firmware, the deck -- laid out by
// ui/TextLayout.h: everything on one page when it fits, else a group that does
// not fit the rest of a page starts the next one. A tap turns the page (after
// the last, back to the first; the header shows "1/2"); the side button goes
// back.
int gAboutPage = 0;

// The app's name, たまごっちょ, in the label face (it has these kana).
void drawAppName(lgfx::LovyanGFX& d, int x, int y, int h, uint16_t fg) {
    gRender.drawLabel(d, tgc::credits::kAppName, x, y, W - x, h, fg, kBlack);
}

// The card item's value: its size and free space, or why there is none.
const char* cardValue(char* out, size_t n) {
    using tgc::storage::CardState;
    switch (tgc::storage::cardState()) {
        case CardState::Mounted: {
            char total[16], freeB[16];
            const char point = ui().decimalPoint[0];
            snprintf(out, n, ui().aboutCardFmt,
                     tgc::formatGb(tgc::storage::cardTotalBytes(), point, total, sizeof total),
                     tgc::formatGb(tgc::storage::cardFreeBytes(), point, freeB, sizeof freeB));
            return out;
        }
        case CardState::Unreadable: return ui().cardUnreadable;
        case CardState::Removed: return ui().cardRemoved;
        default: return ui().aboutNoCard;
    }
}

}  // namespace

void drawAbout() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    constexpr int kLabelH = 20;  // a heading in the label face
    constexpr int kLineH = 10;   // a caption line (8 px) and 2 px leading
    constexpr int kGap = 3;      // between the items of one group
    constexpr int kGroupGap = 8;
    const int top = kHeaderH + 3;
    d.fillScreen(kBlack);

    struct Item {
        const char* label;  // a heading ("Firmware:", its colon dropped); "" for none
        const char* value;
        bool title;         // the app's name over the version
        bool newGroup;
    };
    namespace cr = tgc::credits;
    const bool deckOpen = gPack->isOpen();
    char card[48];
    std::vector<Item> items = {
        {"", tgc::firmwareVersion(), true, true},
        {ui().aboutFirmware, cr::kFirmwareLicence, false, false},
        {ui().aboutDecks, cr::kDecksLicence, false, false},
        {ui().aboutUiFont, cr::kUiFont, false, false},
        {"", cr::kRepo, false, false},
    };
    if (tgc::storage::hasSlot())
        items.push_back({ui().aboutCard, cardValue(card, sizeof card), false, false});
    items.push_back(
        {ui().aboutDeck, deckOpen ? gPack->deckName() : ui().aboutNoDeck, false, true});
    items.push_back({ui().aboutKanji, cr::kKanjidic, false, false});
    // KanjiVG data is in a deck only when it was built with stroke order.
    if (!deckOpen || gPack->hasStrokeOrder())
        items.push_back({ui().aboutStrokes, cr::kKanjiVg, false, false});
    items.push_back({ui().aboutJlpt, cr::kJlpt, false, false});
    items.push_back({ui().aboutReadings, cr::kReadings, false, false});
    items.push_back({ui().aboutMeanings, cr::kMeanings, false, false});
    items.push_back({ui().aboutGlyphs, cr::kGlyphs, false, false});

    // Each value wrapped to the width in the caption font (6 px a character).
    const int maxChars = (W - 8) / 6;
    std::vector<std::vector<tgc::TextSpan>> lines;
    std::vector<tgc::PageBlock> blocks;
    for (const Item& it : items) {
        lines.push_back(tgc::wrapText(it.value, maxChars));
        const int head = (it.title || *it.label) ? kLabelH : 0;
        blocks.push_back({head + static_cast<int>(lines.back().size()) * kLineH, it.newGroup});
    }
    const int pageH = H - kHintH - 2 - top;
    const std::vector<tgc::PageSlot> slots = tgc::layoutPages(blocks, pageH, kGap, kGroupGap);
    const int pages = tgc::pageCount(slots);
    if (gAboutPage >= pages) gAboutPage = 0;

    char pageNo[12] = "";
    if (pages > 1) snprintf(pageNo, sizeof pageNo, "%d/%d", gAboutPage + 1, pages);
    menuHeader(d, ui().aboutTitle, pageNo[0] ? pageNo : nullptr);

    const int bottom = top + pageH;  // a block taller than a page is cut here
    for (size_t i = 0; i < items.size(); ++i) {
        if (slots[i].page != gAboutPage) continue;
        int y = top + slots[i].y;
        if (items[i].title) {
            drawAppName(d, kGutter, y, kLabelH, kText);
            y += kLabelH;
        } else if (*items[i].label) {
            std::string label(items[i].label);
            if (!label.empty() && label.back() == ':') label.pop_back();
            gRender.drawLabel(d, label.c_str(), kGutter, y, W - 2 * kGutter, kLabelH, kText, kBg);
            y += kLabelH;
        }
        for (const tgc::TextSpan& l : lines[i]) {
            if (y + 8 > bottom) break;
            const std::string s(items[i].value + l.begin, l.len);
            wg::drawCaption(d, gRender, s.c_str(), kGutter, y, kTextDim, kBg);
            y += kLineH;
        }
    }
    backHints(d, pages > 1 ? ui().hintMore : nullptr);
    present();
}

void openAbout() {
    gAboutPage = 0;
    gScreen = Screen::About;
    drawScreen();
}

// loop() on About (handleInput()).
void aboutLoop(int a, int b) {
    if (a == 1) {  // next page (drawAbout() wraps after the last)
        ++gAboutPage;
        drawScreen();
    }
    if (b) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

}  // namespace app
}  // namespace tgc
