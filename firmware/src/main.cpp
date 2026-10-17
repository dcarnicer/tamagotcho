// Tangocho Phase 0: open the first pack in /packs and run a random
// multiple-choice quiz (kanji -> meaning / reading).
#include <Arduino.h>
#include <LittleFS.h>

#include <memory>

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

// Find the first *.tgc in /packs. Returns true and fills `path`.
bool findPack(String* path) {
    File dir = LittleFS.open("/packs");
    if (!dir || !dir.isDirectory()) return false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!f.isDirectory() && name.endsWith(".tgc") && !name.startsWith(".")) {
            *path = "/packs/" + name;
            return true;
        }
    }
    return false;
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

void drawScreen() {
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
    String path;
    if (!findPack(&path)) {
        showError("No pack found", "Put a .tgc in data/packs/ then run uploadfs");
        halt();
    }
    gSource.reset(new tgc::FsPackSource(LittleFS.open(path, "r")));
    if (!gSource->ok()) {
        showError("Cannot open", path.c_str());
        halt();
    }
    tgc::PackError err = gPack.open(*gSource, tgc::psramAllocator());
    if (err != tgc::PackError::Ok) {
        showError("Invalid pack", tgc::packErrorString(err));
        halt();
    }
    if (!gRender.begin(gPack)) {
        showError("Out of memory");
        halt();
    }
    Serial.printf("pack %s: %u cards, %u glyphs, lang %s\n", gPack.deckName(),
                  (unsigned)gPack.cardCount(), (unsigned)gPack.glyphCount(), gPack.header().lang);
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
    if (tgc::board::wasPressed(Button::Next)) tapAction();  // side button shortcut

    static bool down = false;
    static bool longFired = false;
    static uint32_t downAt = 0;
    const bool now = tgc::board::isHeld(Button::Select);
    const uint32_t t = millis();
    if (now && !down) {
        down = true;
        longFired = false;
        downAt = t;
    }
    if (now && !longFired && t - downAt >= tgc::board::kHoldMs) {
        longFired = true;
        holdAction();
    }
    if (!now && down) {
        down = false;
        if (!longFired) tapAction();
    }
    delay(10);
}
