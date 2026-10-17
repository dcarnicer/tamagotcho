// Tangocho Phase 0: open the first pack in /packs and run a random
// multiple-choice quiz (kanji -> meaning / reading).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "board/Board.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"

using tgc::board::Button;

// Set -DTGC_QUIZ_LOG to trace every question and answer over serial.
#if defined(TGC_QUIZ_LOG)
#define QLOG(...) Serial.printf(__VA_ARGS__)
#else
#define QLOG(...) do {} while (0)
#endif

namespace {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
uint16_t kGrey, kHighlight, kGreen, kRed, kYellow;

lgfx::LGFX_Sprite* gCanvas = nullptr;  // full-screen back buffer in PSRAM
tgc::PackReader gPack;
std::unique_ptr<tgc::FsPackSource> gSource;
tgc::Renderer gRender;
tgc::XorShiftRng gRng;

tgc::Question gQ;
int gSel = 0;
bool gRevealed = false;
bool gNextModeIsReading = false;
bool gNextReadingIsKun = false;

enum class Screen { Quiz, Menu };
Screen gScreen = Screen::Quiz;
std::vector<String> gPacks;  // paths of the *.tgc files in /packs
int gMenuSel = 0;
String gCurrentPack;
Preferences gPrefs;

int W, H;

void smallText(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t fg, uint16_t bg,
               lgfx::textdatum_t datum = lgfx::top_left) {
    d.setFont(&lgfx::fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(fg, bg);
    d.setTextDatum(datum);
    d.drawString(s, x, y);
}

void present() { gCanvas->pushSprite(&tgc::board::display(), 0, 0); }

void showError(const char* line1, const char* line2 = "") {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kRed);
    smallText(d, "Tangocho", 3, 3, kWhite, kRed);
    d.setTextWrap(true);
    d.setCursor(4, 24);
    d.setFont(&lgfx::fonts::Font0);
    d.setTextColor(kYellow, kBlack);
    d.println(line1);
    d.setTextColor(kWhite, kBlack);
    d.println();
    d.println(line2);
    present();
}

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

bool newQuestion() {
    for (int attempt = 0; attempt < 40; ++attempt) {
        uint32_t idx = gRng.below(gPack.cardCount());
        // Ask for on'yomi and kun'yomi in turn, so both kinds show up.
        tgc::ReadingKind want = gNextReadingIsKun ? tgc::ReadingKind::Kun : tgc::ReadingKind::On;
        tgc::QuizMode first = gNextModeIsReading ? tgc::QuizMode::Reading : tgc::QuizMode::Meaning;
        tgc::QuizMode second = gNextModeIsReading ? tgc::QuizMode::Meaning : tgc::QuizMode::Reading;
        if (tgc::buildQuestion(gPack, idx, first, gRng, &gQ, want) ||
            tgc::buildQuestion(gPack, idx, second, gRng, &gQ, want)) {
            gNextModeIsReading = !gNextModeIsReading;
            if (gQ.mode == tgc::QuizMode::Reading) gNextReadingIsKun = !gNextReadingIsKun;
            gSel = 0;
            gRevealed = false;
            tgc::Card c;
            gPack.card(gQ.cardIndex, &c);
            const char* mk = gQ.mode == tgc::QuizMode::Reading
                                 ? (gQ.readingKind == tgc::ReadingKind::Kun ? "read-kun" : "read-on")
                                 : "mean";
            QLOG("Q %s U+%04X", mk, (unsigned)c.codepoint);
            for (int i = 0; i < gQ.count; ++i)
                QLOG(" |%s %s", i == gQ.correct ? "*" : " ", gQ.options[i].c_str());
            QLOG("\n");
            return true;
        }
    }
    return false;
}

void drawScreen();  // dispatches to the quiz or the deck menu

void drawQuiz() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);

    // Header: deck name left, battery right (both built-in font, size 1).
    const int headerH = 14;
    d.fillRect(0, 0, W, headerH, kGrey);
    smallText(d, gPack.deckName(), 3, 3, kWhite, kGrey);
    int bat = tgc::board::batteryPercent();
    if (bat >= 0) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", bat);
        smallText(d, b, W - 3, 3, bat < 15 ? kYellow : kWhite, kGrey, lgfx::top_right);
    }

    // Kanji prompt.
    tgc::Card card;
    gPack.card(gQ.cardIndex, &card);
    const int largePx = gPack.header().largePx ? gPack.header().largePx : 64;
    int kanjiH = largePx + 6;
    if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
    gRender.drawKanji(d, tgc::kGlyphSetLarge, card.codepoint, 0, headerH + 2, W, kanjiH, kWhite, kBlack);
    const bool reading = gQ.mode == tgc::QuizMode::Reading;
    smallText(d, reading ? "READING" : "MEANING", W / 2, headerH + 2 + kanjiH, kYellow, kBlack,
              lgfx::top_center);

    // Readings: 2x2 grid with large kana (set 3). Meanings: one per row, full
    // width, in the built-in font at 2x (falling back to the pack glyphs).
    const int top = headerH + kanjiH + 14;
    const int area = H - top - 2;
    const int margin = 1, gap = 3;

    auto optionBg = [&](int i) -> uint16_t {
        if (gRevealed) return i == gQ.correct ? kGreen : (i == gSel ? kRed : kBlack);
        return i == gSel ? kHighlight : kBlack;
    };
    auto optionBox = [&](int i, int x, int y, int w, int h, uint8_t set, int maxLines) {
        uint16_t bg = optionBg(i);
        d.fillRoundRect(x, y, w, h, 4, bg);
        if (!gRevealed && i == gSel) d.drawRoundRect(x, y, w, h, 4, kWhite);
        gRender.drawCentred(d, gQ.options[i].c_str(), x + 2, y + 1, w - 4, h - 2, maxLines, kWhite, bg,
                            set);
    };

    if (reading) {
        const int cols = 2;
        const int rows = (gQ.count + cols - 1) / cols;
        const int cellW = (W - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        // Large kana if the whole question fits the cell, else the small text.
        uint8_t set = tgc::kGlyphSetOption;
        for (int i = 0; i < gQ.count; ++i)
            if (gRender.textWidth(gQ.options[i].c_str(), set) > cellW - 4) {
                set = tgc::kGlyphSetText;
                break;
            }
        for (int i = 0; i < gQ.count; ++i)
            optionBox(i, margin + (i % cols) * (cellW + gap), top + (i / cols) * (cellH + gap),
                      cellW, cellH, set, 1);
    } else {
        // The built-in font has no glyph for some characters; then use the pack.
        uint8_t set = tgc::Renderer::kTextBuiltin;
        for (int i = 0; i < gQ.count; ++i)
            if (!tgc::Renderer::builtinCanDraw(gQ.options[i].c_str())) {
                set = tgc::kGlyphSetText;
                break;
            }
        const int w = W - 2 * margin;
        // Shrink to 1x if any option would otherwise be truncated.
        if (set == tgc::Renderer::kTextBuiltin) {
            for (int i = 0; i < gQ.count; ++i)
                if (gRender.textWidth(gQ.options[i].c_str(), set) > w - 4) {
                    set = tgc::Renderer::kTextBuiltinSmall;
                    break;
                }
        }
        const int rowH = area / gQ.count;
        const int boxH = rowH - gap;
        int maxLines = (boxH - 2) / gRender.lineHeight(set);
        if (maxLines < 1) maxLines = 1;
        if (maxLines > 2) maxLines = 2;
        for (int i = 0; i < gQ.count; ++i)
            optionBox(i, margin, top + i * rowH, w, boxH, set, maxLines);
    }
    present();
}

// Deck menu: one pack per row; the loaded one is marked with '*'.
void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, "DECKS", 3, 3, kWhite, kGrey);
    smallText(d, "tap=next  hold=open", W - 3, 3, kWhite, kGrey, lgfx::top_right);

    const int top = 18, rowH = 20;
    for (size_t i = 0; i < gPacks.size(); ++i) {
        const int y = top + static_cast<int>(i) * rowH;
        if (y + rowH - 2 > H) break;  // no more room
        const bool sel = static_cast<int>(i) == gMenuSel;
        const uint16_t bg = sel ? kHighlight : kBlack;
        d.fillRoundRect(1, y, W - 2, rowH - 2, 4, bg);
        if (sel) d.drawRoundRect(1, y, W - 2, rowH - 2, 4, kWhite);
        String name = gPacks[i];
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.endsWith(".tgc")) name = name.substring(0, name.length() - 4);
        if (gPacks[i] == gCurrentPack) name = "* " + name;
        smallText(d, name.c_str(), 4, y + (rowH - 2 - 8) / 2, kWhite, bg);
    }
    present();
}

void drawScreen() {
    if (gScreen == Screen::Menu) drawMenu();
    else drawQuiz();
}

// Opens `path`, replacing the live pack only after the new one validates.
bool loadPack(const String& path) {
    auto src = std::make_unique<tgc::FsPackSource>(LittleFS.open(path, "r"));
    if (!src->ok()) return false;
    {
        tgc::PackReader probe;
        if (probe.open(*src, tgc::psramAllocator()) != tgc::PackError::Ok) return false;
    }
    gRender.end();
    gPack.close();
    gSource = std::move(src);
    if (gPack.open(*gSource, tgc::psramAllocator()) != tgc::PackError::Ok) return false;
    if (!gRender.begin(gPack)) return false;
    gCurrentPack = path;
    Serial.printf("pack %s: %u cards, %u glyphs, lang %s\n", gPack.deckName(),
                  (unsigned)gPack.cardCount(), (unsigned)gPack.glyphCount(), gPack.header().lang);
    return true;
}

void openMenu() {
    gScreen = Screen::Menu;
    gMenuSel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) gMenuSel = static_cast<int>(i);
    drawScreen();
}

void closeMenu() {
    gScreen = Screen::Quiz;
    drawScreen();
}

void selectPack() {
    if (gMenuSel >= 0 && gMenuSel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[gMenuSel];
        if (loadPack(path) && newQuestion()) gPrefs.putString("pack", path);
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// Tap/hold detection: 1 = tap (on release), 2 = hold (at the threshold).
struct Key {
    bool down = false;
    bool longFired = false;
    uint32_t downAt = 0;
    int poll(bool isDown, uint32_t now, uint32_t holdMs) {
        if (isDown && !down) {
            down = true;
            longFired = false;
            downAt = now;
        }
        if (isDown && !longFired && now - downAt >= holdMs) {
            longFired = true;
            return 2;
        }
        if (!isDown && down) {
            down = false;
            if (!longFired) return 1;
        }
        return 0;
    }
};

void halt() {
    for (;;) {
        tgc::board::update();
        delay(50);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    tgc::board::begin();
    auto& disp = tgc::board::display();
    W = tgc::board::width();
    H = tgc::board::height();
    kGrey = disp.color565(60, 60, 70);
    kHighlight = disp.color565(30, 60, 150);
    kGreen = disp.color565(0, 130, 40);
    kRed = disp.color565(170, 20, 20);
    kYellow = disp.color565(255, 210, 60);

    gCanvas = new lgfx::LGFX_Sprite(&disp);
    gCanvas->setColorDepth(16);
    gCanvas->setPsram(true);
    if (!gCanvas->createSprite(W, H)) {
        // Fall back to internal RAM.
        gCanvas->setPsram(false);
        gCanvas->createSprite(W, H);
    }
    gRng = tgc::XorShiftRng(tgc::board::randomSeed());

    if (!LittleFS.begin(false)) {
        showError("Filesystem not found", "Run: pio run -t uploadfs");
        halt();
    }
    listPacks(gPacks);
    if (gPacks.empty()) {
        showError("No pack found", "Put a .tgc in data/packs/ then run uploadfs");
        halt();
    }
    gPrefs.begin("tangocho", false);
    String want = gPrefs.getString("pack", "");
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) want = gPacks[0];
    if (!loadPack(want)) {
        showError("Cannot open pack", want.c_str());
        halt();
    }
    gPrefs.putString("pack", gCurrentPack);
    if (!newQuestion()) {
        showError("Pack has no usable cards");
        halt();
    }
    drawScreen();
}

// One button drives everything: a tap cycles the highlight (or, once revealed,
// moves to the next card); a 600 ms hold confirms the highlighted option. The
// tap action runs on release, and a hold suppresses it, so a long press is never
// also a tap.
void tapAction() {
    if (gRevealed) {
        if (newQuestion()) drawScreen();
        return;
    }
    gSel = (gSel + 1) % gQ.count;
    drawScreen();
}

void holdAction() {
    if (gRevealed) {
        if (newQuestion()) drawScreen();
        return;
    }
    gRevealed = true;
    QLOG("A sel=%d correct=%d %s\n", gSel, gQ.correct, gSel == gQ.correct ? "OK" : "MISS");
    drawScreen();
}

void loop() {
    tgc::board::update();
    const uint32_t t = millis();
    static Key keyA, keyB;
    const int a = keyA.poll(tgc::board::isHeld(Button::Select), t, tgc::board::kHoldMs);
    const int b = keyB.poll(tgc::board::isHeld(Button::Next), t, tgc::board::kHoldMs);
    if (gScreen == Screen::Menu) {
        if (a == 1) {
            gMenuSel = (gMenuSel + 1) % static_cast<int>(gPacks.size());
            drawScreen();
        } else if (a == 2) {
            selectPack();
        }
        if (b) closeMenu();  // side button backs out of the menu
    } else {
        if (a == 1) tapAction();
        else if (a == 2) holdAction();
        if (b == 1) tapAction();  // side button shortcut
        else if (b == 2) openMenu();
    }
    delay(10);
}
