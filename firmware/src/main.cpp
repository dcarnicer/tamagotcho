// Tangocho Phase 2: open the saved pack in /packs and run a spaced-repetition
// multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "board/Board.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/FsSrsFile.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"

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
bool gNextReadingIsKun = false;

tgc::SrsStore gSrs;
tgc::FsSrsFile gSrsIo;
tgc::Queue gQueue;
uint32_t gShownAt = 0;     // millis() when the question was drawn
uint32_t gLastNavAt = 0;   // last navigation tap, 0 = never moved
uint32_t gLastAnswerAt = 0;
int gDirty = 0;            // answers not yet persisted
uint16_t gExtraNew = 0;    // "study more" override for the daily new-card cap

enum class Screen { Quiz, Menu, Done };
Screen gScreen = Screen::Quiz;
Screen gBeforeMenu = Screen::Quiz;
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

const char* gradeName(tgc::Grade g) {
    switch (g) {
        case tgc::Grade::Again: return "again";
        case tgc::Grade::Hard: return "hard";
        case tgc::Grade::Good: return "good";
    }
    return "?";
}

// Count the current pack's cards that are due or unseen, for the boot log and
// the done screen. Cheap: one pass over the card table.
void packDueNew(uint32_t* due, uint32_t* fresh) {
    uint32_t d = 0, n = 0;
    const uint16_t today = gSrs.today();
    for (uint32_t i = 0; i < gPack.cardCount(); ++i) {
        tgc::Card c;
        if (!gPack.card(i, &c)) continue;
        const tgc::Record* r = gSrs.find(c.codepoint);
        if (!r || r->dueDay == 0)
            ++n;
        else if (r->dueDay <= today)
            ++d;
    }
    if (due) *due = d;
    if (fresh) *fresh = n;
}

// Pops the next queue item and builds a question for it, trying the item's
// mode first and then the other two (a card may lack readings/meanings).
// False when the queue is exhausted.
bool newQuestion() {
    static const tgc::QuizMode kModes[] = {tgc::QuizMode::Meaning, tgc::QuizMode::Reading,
                                           tgc::QuizMode::MeaningToKanji};
    constexpr int kN = 3;
    for (;;) {
        tgc::QueueItem item;
        if (!gQueue.next(&item)) return false;
        // Ask for on'yomi and kun'yomi in turn, so both kinds show up.
        const tgc::ReadingKind want =
            gNextReadingIsKun ? tgc::ReadingKind::Kun : tgc::ReadingKind::On;
        for (int k = 0; k < kN; ++k) {
            const tgc::QuizMode mode = kModes[(static_cast<int>(item.mode) + k) % kN];
            if (!tgc::buildQuestion(gPack, item.cardIndex, mode, gRng, &gQ, want)) continue;
            if (gQ.mode == tgc::QuizMode::Reading) gNextReadingIsKun = !gNextReadingIsKun;
            gSel = 0;
            gRevealed = false;
            gShownAt = millis();
            gLastNavAt = 0;
            tgc::Card c;
            gPack.card(gQ.cardIndex, &c);
            const char* mk = gQ.mode == tgc::QuizMode::Reading
                                 ? (gQ.readingKind == tgc::ReadingKind::Kun ? "read-kun" : "read-on")
                                 : (gQ.mode == tgc::QuizMode::MeaningToKanji ? "k2m" : "mean");
            QLOG("Q %s mode=%d U+%04X", mk, static_cast<int>(gQ.mode), (unsigned)c.codepoint);
            for (int i = 0; i < gQ.count; ++i)
                QLOG(" |%s %s", i == gQ.correct ? "*" : " ", gQ.options[i].c_str());
            QLOG("\n");
            return true;
        }
        // Card cannot be asked in any mode: skip it and try the next item.
    }
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

    // Prompt: the kanji, or (meaning -> kanji) the meaning text.
    const bool reading = gQ.mode == tgc::QuizMode::Reading;
    const bool k2m = gQ.mode == tgc::QuizMode::MeaningToKanji;
    tgc::Card card;
    gPack.card(gQ.cardIndex, &card);
    const int largePx = gPack.header().largePx ? gPack.header().largePx : 64;
    int kanjiH = largePx + 6;
    if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
    if (k2m) {
        const char* prompt = gQ.options[gQ.correct].c_str();
        uint8_t set = tgc::Renderer::kTextBuiltin;
        if (!tgc::Renderer::builtinCanDraw(prompt)) set = tgc::kGlyphSetText;
        else if (gRender.textWidth(prompt, set) > W - 8) set = tgc::Renderer::kTextBuiltinSmall;
        int ml = (kanjiH - 4) / gRender.lineHeight(set);
        if (ml < 1) ml = 1;
        if (ml > 4) ml = 4;
        gRender.drawCentred(d, prompt, 3, headerH + 2, W - 6, kanjiH, ml, kWhite, kBlack, set);
    } else {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, card.codepoint, 0, headerH + 2, W, kanjiH, kWhite,
                          kBlack);
    }
    smallText(d, k2m ? "KANJI" : (reading ? "READING" : "MEANING"), W / 2, headerH + 2 + kanjiH,
              kYellow, kBlack, lgfx::top_center);

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

    if (k2m) {
        // Meaning prompt: the options are kanji (set 1) in the same 2x2 grid.
        const int cols = 2;
        const int rows = (gQ.count + cols - 1) / cols;
        const int cellW = (W - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        for (int i = 0; i < gQ.count; ++i) {
            const int cx = margin + (i % cols) * (cellW + gap);
            const int cy = top + (i / cols) * (cellH + gap);
            const uint16_t bg = optionBg(i);
            d.fillRoundRect(cx, cy, cellW, cellH, 4, bg);
            if (!gRevealed && i == gSel) d.drawRoundRect(cx, cy, cellW, cellH, 4, kWhite);
            tgc::Card oc;
            if (gPack.card(gQ.optionCard[i], &oc))
                gRender.drawKanji(d, tgc::kGlyphSetSmall, oc.codepoint, cx, cy, cellW, cellH, kWhite, bg);
        }
    } else if (reading) {
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

// "All done" screen: no card is due and no new card is left today. A tap, hold
// or side tap rebuilds the queue and tries again; the side hold opens the menu.
void drawDone() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, gPack.deckName(), 3, 3, kWhite, kGrey);
    smallText(d, "DONE", W - 3, 3, kWhite, kGrey, lgfx::top_right);

    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);
    char line[48];
    smallText(d, "All reviewed for now", W / 2, H / 2 - 24, kWhite, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, "day %u  %u cards", (unsigned)gSrs.today(), (unsigned)gSrs.count());
    smallText(d, line, W / 2, H / 2 - 6, kYellow, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, "due %u  new %u", (unsigned)due, (unsigned)fresh);
    smallText(d, line, W / 2, H / 2 + 8, kWhite, kBlack, lgfx::top_center);
    smallText(d, "tap/hold: +10 new", W / 2, H / 2 + 30, kWhite, kBlack, lgfx::top_center);
    smallText(d, "side hold: decks", W / 2, H / 2 + 44, kWhite, kBlack, lgfx::top_center);
    present();
}

void showDone() {
    gScreen = Screen::Done;
    drawDone();
}

void drawScreen() {
    if (gScreen == Screen::Menu) drawMenu();
    else if (gScreen == Screen::Done) drawDone();
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
    // Progress is global (keyed by codepoint), so persist it before the swap.
    if (gDirty) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gRender.end();
    gPack.close();
    gSource = std::move(src);
    if (gPack.open(*gSource, tgc::psramAllocator()) != tgc::PackError::Ok) return false;
    if (!gRender.begin(gPack)) return false;
    gCurrentPack = path;
    gQueue.rebuild(gPack, gSrs, gRng);
    Serial.printf("pack %s: %u cards, %u glyphs, lang %s\n", gPack.deckName(),
                  (unsigned)gPack.cardCount(), (unsigned)gPack.glyphCount(), gPack.header().lang);
    return true;
}

void openMenu() {
    if (gDirty) {  // entering the menu is a flush point (docs/srs.md section 4)
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gBeforeMenu = gScreen;
    gScreen = Screen::Menu;
    gMenuSel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) gMenuSel = static_cast<int>(i);
    drawScreen();
}

void closeMenu() {
    gScreen = gBeforeMenu;
    drawScreen();
}

void selectPack() {
    bool loaded = false;
    gExtraNew = 0;
    if (gMenuSel >= 0 && gMenuSel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[gMenuSel];
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

    const bool srsLoaded = gSrs.begin(gSrsIo);
    gSrs.beginSession();
    gExtraNew = 0;
    gQueue.rebuild(gPack, gSrs, gRng);  // loadPack rebuilt with an empty store
    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);
    Serial.printf("srs: day %u, records %u, due %u, new %u%s\n", (unsigned)gSrs.today(),
                  (unsigned)gSrs.count(), (unsigned)due, (unsigned)fresh,
                  srsLoaded ? "" : " (new)");

    if (!newQuestion()) showDone();
    else drawScreen();
}

// One button drives everything: a tap cycles the highlight (or, once revealed,
// moves to the next card); a 600 ms hold confirms the highlighted option. The
// tap action runs on release, and a hold suppresses it, so a long press is never
// also a tap.
void tapAction() {
    if (gRevealed) {
        if (newQuestion()) drawScreen();
        else showDone();
        return;
    }
    gSel = (gSel + 1) % gQ.count;
    gLastNavAt = millis();  // answer clock restarts on every navigation tap
    drawScreen();
}

void holdAction(uint32_t pressedAt) {
    if (gRevealed) {
        if (newQuestion()) drawScreen();
        else showDone();
        return;
    }
    gRevealed = true;

    const bool correct = gSel == gQ.correct;
    const uint32_t start = gLastNavAt ? gLastNavAt : gShownAt;
    const uint32_t rt_ms = pressedAt - start;  // until the hold's press-down, not its fire
    tgc::Grade grade;
    if (!correct)
        grade = tgc::Grade::Again;
    else if (rt_ms >= gSrs.slowMs())
        grade = tgc::Grade::Hard;
    else
        grade = tgc::Grade::Good;

    tgc::Card card;
    gPack.card(gQ.cardIndex, &card);
    const float u01 = gRng.below(1000) / 1000.0f;
    gSrs.grade(card.codepoint, grade, gSrs.today(), static_cast<uint8_t>(gQ.mode), u01);
    gQueue.answered(gQ.cardIndex, grade);
    ++gDirty;
    gLastAnswerAt = millis();

    const tgc::Record* r = gSrs.find(card.codepoint);
    const int interval = r ? static_cast<int>(r->dueDay) - static_cast<int>(gSrs.today()) : 0;
    QLOG("A sel=%d correct=%d %s grade=%s rt=%ums S=%.2f D=%.1f I=%d\n", gSel, gQ.correct,
         correct ? "OK" : "MISS", gradeName(grade), (unsigned)rt_ms,
         r ? r->stabilityCds / 100.0f : 0.0f, r ? r->difficulty10 / 10.0f : 0.0f, interval);
    drawScreen();
}

void loop() {
    tgc::board::update();
    const uint32_t t = millis();
    gSrs.tick(t);
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
    } else if (gScreen == Screen::Done) {
        if (b == 2) {
            openMenu();
        } else if (a || b == 1) {
            // Nothing due and the daily new cap is spent: study kMoreNew more.
            gExtraNew = static_cast<uint16_t>(gExtraNew + 10);
            gQueue.rebuild(gPack, gSrs, gRng, gExtraNew);
            if (newQuestion()) {
                gScreen = Screen::Quiz;
                drawScreen();
            } else {
                drawDone();
            }
        }
    } else {
        if (a == 1) tapAction();
        else if (a == 2) holdAction(keyA.downAt);
        if (b == 1) tapAction();  // side button shortcut
        else if (b == 2) openMenu();
    }
    if (gDirty >= 8 || (gDirty > 0 && t - gLastAnswerAt > 120000)) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    delay(10);
}
