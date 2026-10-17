// Tangocho Phase 2: open the saved pack in /packs and run a spaced-repetition
// multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board/Board.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/FsSrsFile.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"
#include "ui/UiLang.h"

using tgc::board::Button;

// Set -DTGC_QUIZ_LOG to trace every question and answer over serial.
#if defined(TGC_QUIZ_LOG)
#define QLOG(...) Serial.printf(__VA_ARGS__)
#else
#define QLOG(...) do {} while (0)
#endif

// The StickS3 has no RTC and the ESP32's RTC domain does not survive a power-off
// (verified: RTC memory is cleared when the device is switched off). RTC_NOINIT
// memory does survive a soft reset, so it tells us whether this boot followed a
// power cut -- in which case the elapsed days are unknown and we ask the user.
RTC_NOINIT_ATTR uint32_t gPowerMagic;
RTC_NOINIT_ATTR uint32_t gPowerMagicInv;
constexpr uint32_t kPowerMagic = 0x7A5C1234u;
bool gPoweredOff = false;

namespace {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
uint16_t kGrey, kHighlight, kGreen, kRed, kYellow;

// Every user-facing on-device string. The table is chosen from the loaded
// pack's language (gPack.header().lang) when the pack changes; English is the
// default and the fallback for any unknown code, so the error screen (which
// runs before a pack is open) still has text.
struct UiStrings {
    const char* appName;  // error screen title bar
    const char* settingsTitle;
    const char* decksTitle;
    const char* backHint;
    const char* menuRows[8];  // continue, decks, date, new/day, speed, stats, orientation, language
    const char* newPerDayTitle;
    const char* speedTitle;
    const char* speedOff;
    const char* speedSecondsFmt;
    const char* quizMeaning;
    const char* quizReading;
    const char* quizKanji;
    const char* dateTitle;
    const char* dateRows[4];
    const char* doneBadge;
    // Font0 is 6 px/char and each done-screen line is centred on the 135 px
    // portrait width, so every done line must stay short (about 21 chars).
    const char* doneAllReviewed;
    const char* doneDayFmt;
    const char* doneDueFmt;
    const char* doneStudyMore;
    const char* doneMenuHint;
    const char* deckCountsFmt;
    const char* statsTitle;
    const char* statsSeenFmt;
    const char* statsDueNewFmt;
    const char* statsAnswersFmt;
    const char* statsAccuracyFmt;
    const char* statsLearnedFmt;
    const char* errNoFilesystem;
    const char* errNoFilesystemHint;
    const char* errNoPack;
    const char* errNoPackHint;
    const char* errOpenPack;
    const char* orientTitle;
    const char* orientRows[2];  // portrait, landscape
    // Detail card (shown after a wrong answer). `detailOn`/`detailKun` label the
    // on'yomi/kun'yomi lines; the meanings label reuses quizMeaning.
    const char* detailTitle;
    const char* detailOn;
    const char* detailKun;
    const char* detailStrokesFmt;
    const char* detailAccuracyFmt;
    // Statistics: day streak and total time on questions.
    const char* statsStreakFmt;  // current, best
    const char* statsTimeFmt;    // whole line, %s = the formatted duration
    const char* statsTimeHM;     // "%uh %um"
    const char* statsTimeMS;     // "%um %us"
    const char* statsTimeS;      // "%us"
    // UI-language picker: the title and the three rows (endonyms, same in every
    // table so a user can find their language whatever the current one is).
    const char* langTitle;
    const char* langRows[3];
};

const UiStrings kUiEn = {
    "Tangocho",
    "SETTINGS", "DECKS", "side: back",
    {"Continue", "Decks", "Date", "New/day", "Speed", "Statistics", "Orientation", "Language"},
    "NEW/DAY", "SPEED", "no", "%.1f s",
    "MEANING", "READING", "KANJI",
    "DATE", {"today (0)", "1 day", "2-7 days", "more (14)"},
    "DONE", "All reviewed for now", "day %u  %u cards", "due %u  new %u", "tap/hold: +10 new",
    "side hold: menu",
    "  %u/%u",
    "STATISTICS", "seen %u / %u", "due %u  new %u", "answers %u", "accuracy %u%%", "learned %u",
    "Filesystem not found", "Run: pio run -t uploadfs",
    "No pack found", "Put a .tgc in data/packs/ then run uploadfs",
    "Cannot open pack",
    "ORIENTATION", {"Portrait", "Landscape"},
    "DETAIL", "ON", "KUN", "strokes %u", "correct %u/%u",
    "streak %u/%u", "time %s", "%uh %um", "%um %us", "%us",
    "LANGUAGE", {"Auto", "English", "Español"},
};

const UiStrings kUiEs = {
    "Tangocho",
    "AJUSTES", "MAZOS", "lateral: atrás",
    {"Seguir", "Mazos", "Fecha", "Nuevas/día", "Velocidad", "Estadisticas", "Orientacion", "Idioma"},
    "NUEVAS/DIA", "VELOCIDAD", "no", "%.1f s",
    "SIGNIFICADO", "LECTURA", "KANJI",
    "FECHA", {"hoy (0)", "1 día", "2-7 días", "más (14)"},
    "FIN", "Todo repasado", "dia %u  %u tarjetas", "vencen %u  nuevas %u",
    "toque/larga: +10", "lateral: menu",
    "  %u/%u",
    "ESTADISTICAS", "vistos %u / %u", "pendientes %u  nuevas %u", "respuestas %u",
    "precisión %u%%", "aprendidas %u",
    "No se encuentra el sistema de archivos", "Ejecuta: pio run -t uploadfs",
    "No hay ningún mazo", "Pon un .tgc en data/packs/ y ejecuta uploadfs",
    "No se puede abrir el mazo",
    "ORIENTACION", {"Vertical", "Horizontal"},
    "DETALLE", "ON", "KUN", "trazos %u", "aciertos %u/%u",
    "racha %u/%u", "tiempo %s", "%uh %um", "%um %us", "%us",
    "IDIOMA", {"Auto", "English", "Español"},
};

const UiStrings* gUi = &kUiEn;  // English until a pack says otherwise
const UiStrings& ui() { return *gUi; }

// NVS `tangocho`/`ui`: the UI-language preference (Auto/English/Spanish). Auto,
// the default, follows the pack language exactly as before.
int gUiLangPref = tgc::kUiLangAuto;

// "es" selects the Spanish table; anything else (including no pack yet) is
// English. The user preference (see UiLang.h) decides when Auto is not set.
void setUiForLang(const char* lang) {
    gUi = (std::strcmp(tgc::uiLangCodeFor(gUiLangPref, lang), "es") == 0) ? &kUiEs : &kUiEn;
}

lgfx::LGFX_Sprite* gCanvas = nullptr;  // full-screen back buffer in PSRAM
tgc::PackReader gPack;
std::unique_ptr<tgc::FsPackSource> gSource;
tgc::Renderer gRender;
tgc::XorShiftRng gRng;

tgc::Question gQ;
int gSel = 0;
bool gRevealed = false;
bool gLastCorrect = false;  // outcome of the last answer (drives the detail card)
bool gNextReadingIsKun = false;

// Stroke-order animation on the mistake detail card. Only active while the
// shown card's pack carries stroke data (docs/pack-format.md); otherwise the
// card is drawn exactly as before.
bool gAnimOn = false;
uint16_t gAnimStrokes = 0;
int gAnimFrame = 0;
uint32_t gAnimNext = 0;
constexpr uint32_t kStrokeMs = 550;       // one stroke step
constexpr uint32_t kStrokePauseMs = 1200; // hold on the last stroke before looping

tgc::SrsStore gSrs;
tgc::FsSrsFile gSrsIo;
tgc::Queue gQueue;
uint32_t gShownAt = 0;     // millis() when the question was drawn
uint32_t gLastNavAt = 0;   // last navigation tap, 0 = never moved
uint32_t gLastAnswerAt = 0;
int gDirty = 0;            // answers not yet persisted
uint16_t gExtraNew = 0;    // "study more" override for the daily new-card cap

enum class Screen { Quiz, Menu, Decks, Choice, Done, Date, Stats, Detail };
// Screen::Choice is shared by the row pickers (see openChoice()).
enum class Choice { NewPerDay, SlowMs, Orientation, UiLang };
Screen gScreen = Screen::Quiz;
Screen gBeforeMenu = Screen::Quiz;
Screen gAfterDate = Screen::Quiz;  // where the date screen returns to
int gDateSel = 0;
std::vector<String> gPacks;  // paths of the *.tgc files in /packs
int gMenuSel = 0;            // highlighted row on the settings home
int gDeckSel = 0;            // highlighted row on the deck list
Choice gChoice = Choice::NewPerDay;
int gChoiceSel = 0;
int gChoiceCount = 0;
uint16_t gChoiceValues[4] = {0, 0, 0, 0};
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

// (Re)create the full-screen back buffer for the current W x H, PSRAM first with
// an internal-RAM fallback. Used at boot and on an orientation change.
void createCanvas() {
    auto& disp = tgc::board::display();
    if (gCanvas) {
        delete gCanvas;
        gCanvas = nullptr;
    }
    gCanvas = new lgfx::LGFX_Sprite(&disp);
    gCanvas->setColorDepth(16);
    gCanvas->setPsram(true);
    if (!gCanvas->createSprite(W, H)) {
        gCanvas->setPsram(false);  // fall back to internal RAM
        gCanvas->createSprite(W, H);
    }
}

// Right-aligned header hint. Drawn with the built-in GFX font (not the pack
// font) so accented Spanish ("atrás") renders; the 1x size matches smallText.
void drawBackHint(lgfx::LovyanGFX& d) {
    const char* s = ui().backHint;
    const int tw = gRender.textWidth(s, tgc::Renderer::kTextBuiltinSmall);
    gRender.drawCentred(d, s, W - 3 - tw, 3, tw, 8, 1, kWhite, kGrey,
                        tgc::Renderer::kTextBuiltinSmall);
}

void showError(const char* line1, const char* line2 = "") {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kRed);
    smallText(d, ui().appName, 3, 3, kWhite, kRed);
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

void drawScreen();  // dispatches to the active screen
void applyOrientation(bool landscape);
void formatDate(uint16_t days, char* out, size_t n);

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
    // Landscape: prompt in the left column, options bounded to the right one.
    const bool landscape = W > H;
    const int leftW = std::max(largePx, W * 2 / 5);
    const int colX = landscape ? leftW : 0;
    const int colW = landscape ? W - leftW : W;

    int kanjiH = largePx + 6;
    if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
    if (k2m) {
        const char* prompt = gQ.options[gQ.correct].c_str();
        // Landscape puts the meaning prompt in the left column (the kanji
        // options are in the right one); portrait keeps the full width.
        const int pX = 0;
        const int pW = landscape ? leftW : colW;
        uint8_t set = tgc::Renderer::kTextBuiltin;
        if (!tgc::Renderer::builtinCanDraw(prompt)) set = tgc::kGlyphSetText;
        else if (gRender.textWidth(prompt, set) > pW - 8) set = tgc::Renderer::kTextBuiltinSmall;
        // The box must be at least one line tall; the prompt is centred in it.
        const int boxY = landscape ? headerH : headerH + 2;
        const int boxH = landscape ? H - headerH : kanjiH;
        int ml = (boxH - 4) / gRender.lineHeight(set);
        if (ml < 1) ml = 1;
        if (ml > 4) ml = 4;
        gRender.drawCentred(d, prompt, pX + 3, boxY, pW - 6, boxH - (landscape ? 2 : 0), ml,
                            kWhite, kBlack, set);
    } else if (landscape) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, card.codepoint, 0, headerH, leftW, H - headerH,
                          kWhite, kBlack);
    } else {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, card.codepoint, 0, headerH + 2, W, kanjiH, kWhite,
                          kBlack);
    }
    if (landscape)
        smallText(d, k2m ? ui().quizKanji : (reading ? ui().quizReading : ui().quizMeaning),
                  leftW / 2, H - 11, kYellow, kBlack, lgfx::top_center);
    else
        smallText(d, k2m ? ui().quizKanji : (reading ? ui().quizReading : ui().quizMeaning), W / 2,
                  headerH + 2 + kanjiH, kYellow, kBlack, lgfx::top_center);

    // Readings: 2x2 grid with large kana (set 3). Meanings: one per row, full
    // width, in the built-in font at 2x (falling back to the pack glyphs).
    const int top = landscape ? headerH + 2 : headerH + kanjiH + 14;
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
        const int cellW = (colW - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        for (int i = 0; i < gQ.count; ++i) {
            const int cx = colX + margin + (i % cols) * (cellW + gap);
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
        const int cellW = (colW - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        // Large kana if the whole question fits the cell, else the small text.
        uint8_t set = tgc::kGlyphSetOption;
        for (int i = 0; i < gQ.count; ++i)
            if (gRender.textWidth(gQ.options[i].c_str(), set) > cellW - 4) {
                set = tgc::kGlyphSetText;
                break;
            }
        for (int i = 0; i < gQ.count; ++i)
            optionBox(i, colX + margin + (i % cols) * (cellW + gap), top + (i / cols) * (cellH + gap),
                      cellW, cellH, set, 1);
    } else {
        // The built-in font has no glyph for some characters; then use the pack.
        uint8_t set = tgc::Renderer::kTextBuiltin;
        for (int i = 0; i < gQ.count; ++i)
            if (!tgc::Renderer::builtinCanDraw(gQ.options[i].c_str())) {
                set = tgc::kGlyphSetText;
                break;
            }
        const int w = colW - 2 * margin;
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
            optionBox(i, colX + margin, top + i * rowH, w, boxH, set, maxLines);
    }
    present();
}

// Pick the built-in font size for a menu/list label: the 2x font when the box
// is at least `minH` tall and the label fits `w`, else the 1x font so long or
// narrow labels are never clipped.
uint8_t labelFont(const char* label, int h, int minH, int w) {
    if (h < minH) return tgc::Renderer::kTextBuiltinSmall;
    if (gRender.textWidth(label, tgc::Renderer::kTextBuiltin) > w)
        return tgc::Renderer::kTextBuiltinSmall;
    return tgc::Renderer::kTextBuiltin;
}

// One full-width highlighted row for the settings home and the choice screens,
// labelled with the built-in GFX font so accented Spanish renders. `h` is the
// box height; the 2x font is used when it fits, the 1x one otherwise (needed on
// the short landscape settings/choice screens).
void drawRow(lgfx::LovyanGFX& d, const char* label, int y, int h, bool sel) {
    const uint16_t bg = sel ? kHighlight : kBlack;
    d.fillRoundRect(2, y, W - 4, h, 5, bg);
    if (sel) d.drawRoundRect(2, y, W - 4, h, 5, kWhite);
    const uint8_t set = labelFont(label, h, 22, W - 10);
    gRender.drawCentred(d, label, 4, y + 1, W - 8, h - 2, 1, kWhite, bg, set);
}

// Settings home (hold the side button from the quiz): tap moves the highlight,
// hold activates the row. Each row is a full-width box with a label.
constexpr int kSettingRows = 8;

void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().settingsTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    // Shrink the row step on the short landscape screen so all rows fit; in
    // portrait (or when there is room) this keeps the original 36 px step.
    const int top = 20;
    int step = 36;
    if (top + step * kSettingRows > H) {
        step = (H - top - 2) / kSettingRows;
        if (step < 10) step = 10;
    }
    const int rowH = step - 4 < 32 ? step - 4 : 32;
    for (int i = 0; i < kSettingRows; ++i)
        drawRow(d, ui().menuRows[i], top + i * step, rowH, i == gMenuSel);
    present();
}

// Deck list (opened from "Mazos"): one row per pack; the loaded pack is marked
// '*' and shows its due/new counts. Hold loads the highlighted pack; the side
// button returns to the settings home. The date row lives in the settings home.
void drawDecks() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().decksTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);  // only shown on the loaded pack's row
    const int top = 18;
    const int rows = static_cast<int>(gPacks.size());
    int rowH = 20;
    if (rows > 0) {
        const int fit = (H - top - 2) / rows;  // shrink so the list fits landscape
        if (fit < rowH) rowH = fit;
    }
    if (rowH < 10) rowH = 10;
    for (int i = 0; i < rows; ++i) {
        const int y = top + i * rowH;
        if (y + rowH - 2 > H) break;  // no more room
        const bool sel = i == gDeckSel;
        const uint16_t bg = sel ? kHighlight : kBlack;
        d.fillRoundRect(1, y, W - 2, rowH - 2, 4, bg);
        if (sel) d.drawRoundRect(1, y, W - 2, rowH - 2, 4, kWhite);
        String label = gPacks[i];
        const int slash = label.lastIndexOf('/');
        if (slash >= 0) label = label.substring(slash + 1);
        if (label.endsWith(".tgc")) label = label.substring(0, label.length() - 4);
        if (gPacks[i] == gCurrentPack) {
            char counts[24];
            snprintf(counts, sizeof counts, ui().deckCountsFmt, (unsigned)due, (unsigned)fresh);
            label = "* " + label + counts;
        }
        smallText(d, label.c_str(), 4, y + (rowH - 2 - 8) / 2, kWhite, bg);
    }
    present();
}

// Generic choice screen: a title and the candidate values. Hold applies the
// highlighted value; the side button cancels back to the settings home.
void drawChoice() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    const char* title = gChoice == Choice::NewPerDay  ? ui().newPerDayTitle
                        : gChoice == Choice::SlowMs   ? ui().speedTitle
                        : gChoice == Choice::Orientation ? ui().orientTitle
                                                         : ui().langTitle;
    smallText(d, title, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    // Shrink the row step on the short landscape screen so all rows fit; portrait
    // keeps the original 40 px step and 32 px box.
    const int top = 40;
    int step = 40;
    if (top + step * gChoiceCount > H) {
        step = (H - top - 2) / gChoiceCount;
        if (step < 10) step = 10;
    }
    const int rowH = step - 4 < 32 ? step - 4 : 32;
    for (int i = 0; i < gChoiceCount; ++i) {
        char label[16];
        if (gChoice == Choice::Orientation) {
            snprintf(label, sizeof label, "%s", ui().orientRows[i]);
        } else if (gChoice == Choice::UiLang) {
            snprintf(label, sizeof label, "%s", ui().langRows[i]);
        } else if (gChoice == Choice::SlowMs) {
            if (gChoiceValues[i] == 0xFFFF) snprintf(label, sizeof label, "%s", ui().speedOff);
            else snprintf(label, sizeof label, ui().speedSecondsFmt, gChoiceValues[i] / 1000.0f);
        } else {
            snprintf(label, sizeof label, "%u", (unsigned)gChoiceValues[i]);
        }
        drawRow(d, label, top + i * step, rowH, i == gChoiceSel);
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
    smallText(d, ui().doneBadge, W - 3, 3, kWhite, kGrey, lgfx::top_right);

    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);
    char line[48];
    smallText(d, ui().doneAllReviewed, W / 2, H / 2 - 24, kWhite, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, ui().doneDayFmt, (unsigned)gSrs.today(), (unsigned)gSrs.count());
    smallText(d, line, W / 2, H / 2 - 6, kYellow, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, ui().doneDueFmt, (unsigned)due, (unsigned)fresh);
    smallText(d, line, W / 2, H / 2 + 8, kWhite, kBlack, lgfx::top_center);
    smallText(d, ui().doneStudyMore, W / 2, H / 2 + 30, kWhite, kBlack, lgfx::top_center);
    smallText(d, ui().doneMenuHint, W / 2, H / 2 + 44, kWhite, kBlack, lgfx::top_center);
    present();
}

void showDone() {
    gScreen = Screen::Done;
    drawDone();
}

// Civil date helpers (Howard Hinnant's algorithms) for the internal day index.
// Day 0 is 2026-10-17; only differences matter for the SRS, the date is a
// convenience for the user.
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

void civilFromDays(long z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yr = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yr + (*m <= 2);
}

void formatDate(uint16_t days, char* out, size_t n) {
    static const long kBase = daysFromCivil(2026, 10, 17);
    int y = 2026;
    unsigned m = 10, d = 17;
    civilFromDays(kBase + days, &y, &m, &d);
    snprintf(out, n, "%04d-%02d-%02d", y, m, d);
}

// Date screen: shows today and offers to add days (the user's gap since the
// last session, asked on a power-on boot, or a manual correction from the menu).
constexpr int kDateRows = 4;
const uint16_t kDateAdd[kDateRows] = {0, 1, 4, 14};

void drawDate() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().dateTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);
    char date[16];
    formatDate(gSrs.today(), date, sizeof date);
    gRender.drawCentred(d, date, 2, 18, W - 4, 18, 1, kYellow, kBlack, tgc::Renderer::kTextBuiltin);

    // Shrink the rows on the short landscape screen; portrait keeps 44/32.
    int top = 44, rowH = 32;
    if (top + rowH * kDateRows > H - 2) {
        top = 38;
        rowH = (H - top - 2) / kDateRows;
        if (rowH < 12) rowH = 12;
    }
    for (int i = 0; i < kDateRows; ++i) {
        const int y = top + i * rowH;
        const bool sel = i == gDateSel;
        const uint16_t bg = sel ? kHighlight : kBlack;
        d.fillRoundRect(2, y, W - 4, rowH - 4, 5, bg);
        if (sel) d.drawRoundRect(2, y, W - 4, rowH - 4, 5, kWhite);
        const uint8_t set = labelFont(ui().dateRows[i], rowH - 4, 22, W - 10);
        gRender.drawCentred(d, ui().dateRows[i], 4, y + 2, W - 8, rowH - 8, 1, kWhite, bg, set);
    }
    present();
}

// "2h 5m", "5m 30s" or "30s" depending on the magnitude. The unit strings come
// from UiStrings so a language can change them; the result stays short enough
// for the 135 px portrait width (the stats line shrinks to the 1x font if not).
void formatReviewTime(uint32_t ms, char* out, size_t n) {
    const uint32_t totalS = ms / 1000u;
    const uint32_t mins = totalS / 60u;
    const uint32_t hours = mins / 60u;
    if (hours > 0)
        snprintf(out, n, ui().statsTimeHM, (unsigned)hours, (unsigned)(mins % 60u));
    else if (mins > 0)
        snprintf(out, n, ui().statsTimeMS, (unsigned)mins, (unsigned)(totalS % 60u));
    else
        snprintf(out, n, ui().statsTimeS, (unsigned)totalS);
}

// Statistics screen (opened from the settings home): the internal date, the day
// streak and total time on questions, cards seen out of the loaded pack, its
// due/new counts, and the lifetime answer counters from the SRS store. Read-only;
// the side button returns to settings.
void drawStats() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().statsTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    const tgc::Stats st = gSrs.stats();
    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);

    // Accuracy over every grade ever recorded; guard a zero divisor and a
    // saturation edge (lapses can equal reps but never exceed it).
    const uint32_t acc = st.reps ? (st.reps > st.lapses ? st.reps - st.lapses : 0) * 100u / st.reps
                                 : 0;

    char date[16], streak[24], timeLine[32], seen[24], dueNew[32], answers[24], accuracy[24],
        learned[24], duration[16];
    formatDate(gSrs.today(), date, sizeof date);
    snprintf(streak, sizeof streak, ui().statsStreakFmt, (unsigned)st.streakCurrent,
             (unsigned)st.streakBest);
    formatReviewTime(st.reviewMs, duration, sizeof duration);
    snprintf(timeLine, sizeof timeLine, ui().statsTimeFmt, duration);
    snprintf(seen, sizeof seen, ui().statsSeenFmt, (unsigned)st.seen, (unsigned)gPack.cardCount());
    snprintf(dueNew, sizeof dueNew, ui().statsDueNewFmt, (unsigned)due, (unsigned)fresh);
    snprintf(answers, sizeof answers, ui().statsAnswersFmt, (unsigned)st.answers);
    snprintf(accuracy, sizeof accuracy, ui().statsAccuracyFmt, (unsigned)acc);
    snprintf(learned, sizeof learned, ui().statsLearnedFmt, (unsigned)st.learned);

    const char* lines[] = {date, streak, timeLine, seen, dueNew, answers, accuracy, learned};
    constexpr int kStatsLines = 8;
    // Shrink the line step on the short landscape screen; portrait keeps 26/24.
    const int top = 26;
    int step = 24;
    if (top + step * kStatsLines > H) {
        step = (H - top - 2) / kStatsLines;
        if (step < 10) step = 10;
    }
    for (int i = 0; i < kStatsLines; ++i) {
        const uint8_t set = labelFont(lines[i], step - 2, 18, W - 4);
        gRender.drawCentred(d, lines[i], 2, top + i * step, W - 4, step - 4, 1,
                            i == 0 ? kYellow : kWhite, kBlack, set);
    }
    present();
}

// Joins a card's "、"-separated reading list into one display string, dropping
// the okurigana markers ("た.べる、く.う" -> "たべる、くう") for display.
std::string joinReadings(const char* list) {
    std::string out;
    if (!list || !*list) return out;
    const char* kSep = "\xE3\x80\x81";  // U+3001 "、"
    const char* p = list;
    while (*p) {
        const char* e = std::strstr(p, kSep);
        const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
        for (size_t i = 0; i < n; ++i)
            if (p[i] != '.') out += p[i];
        if (!e) break;
        out += kSep;
        p = e + 3;
    }
    return out;
}

// The detail card's kanji: the static set-0 glyph, or -- when the pack carries
// stroke data for the card and the animation is running -- the strokes of the
// current frame: completed ones in white, the current one in yellow, the
// undrawn ones absent. The masks live in the same em square drawKanji uses.
void drawDetailKanji(int boxX, int boxY, int boxW, int boxH, uint32_t cp) {
    auto& d = *gCanvas;
    const int px = gPack.header().largePx ? gPack.header().largePx : 64;
    if (!gAnimOn) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, boxX, boxY, boxW, boxH, kWhite, kBlack);
        return;
    }
    const int emX = boxX + (boxW - px) / 2;
    const int emY = boxY + (boxH - px) / 2;
    for (int i = 0; i <= gAnimFrame; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           i == gAnimFrame ? kYellow : kWhite);
}

// Detail card (roadmap phase 3): a tap after a wrong reveal opens this read-only
// card with the kanji, all meanings, all on/kun readings (okurigana dots
// removed), the stroke count and this card's own accuracy from the SRS record.
// Portrait centres the kanji in the upper ~45% with the details below; landscape
// mirrors the quiz (kanji left, details right). Japanese readings use the pack's
// kana (set 2, falling back to set 3 by width as the quiz does); Latin values use
// the built-in font when it can draw them (2x, shrunk to 1x), else pack glyphs.
// When the pack carries stroke data for the card, the kanji animates instead
// (see drawDetailKanji and the Screen::Detail branch of loop()).
void drawDetail() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int headerH = 14;
    d.fillRect(0, 0, W, headerH, kGrey);
    smallText(d, gPack.deckName(), 3, 3, kWhite, kGrey);
    smallText(d, ui().detailTitle, W - 3, 3, kWhite, kGrey, lgfx::top_right);

    tgc::Card card;
    gPack.card(gQ.cardIndex, &card);
    const int largePx = gPack.header().largePx ? gPack.header().largePx : 64;
    const bool landscape = W > H;

    int x, w, top, bottom, kx, ky, kw, kh;
    if (landscape) {
        const int leftW = std::max(largePx, W * 2 / 5);
        kx = 0;
        ky = headerH;
        kw = leftW;
        kh = H - headerH;
        x = leftW + 3;
        w = W - leftW - 6;
        top = headerH + 2;
        bottom = H - 2;
    } else {
        int kanjiH = largePx + 6;
        if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
        kx = 0;
        ky = headerH + 2;
        kw = W;
        kh = kanjiH;
        x = 3;
        w = W - 6;
        top = headerH + 2 + kanjiH;
        bottom = H - 2;
    }
    drawDetailKanji(kx, ky, kw, kh, card.codepoint);

    const int lh = gRender.lineHeight(tgc::kGlyphSetText);
    const std::string on = joinReadings(card.onyomi);
    const std::string kun = joinReadings(card.kunyomi);
    const tgc::Record* rec = gSrs.find(card.codepoint);
    const bool hasAcc = rec && rec->reps > 0;

    // Kana: set 2, falling back to set 3 only when set 2 overflows and set 3 fits
    // (mirrors the quiz's width test; in practice readings are short).
    auto kanaSet = [&](const std::string& s) -> uint8_t {
        if (gRender.textWidth(s.c_str(), tgc::kGlyphSetText) > w &&
            gRender.textWidth(s.c_str(), tgc::kGlyphSetOption) <= w)
            return tgc::kGlyphSetOption;
        return tgc::kGlyphSetText;
    };
    // Latin: the built-in font when it covers the text (2x, shrunk to 1x), else
    // the pack glyphs (same fallback rules as the quiz meaning options).
    auto latinSet = [&](const char* s) -> uint8_t {
        if (!tgc::Renderer::builtinCanDraw(s)) return tgc::kGlyphSetText;
        return gRender.textWidth(s, tgc::Renderer::kTextBuiltin) <= w
                   ? static_cast<uint8_t>(tgc::Renderer::kTextBuiltin)
                   : static_cast<uint8_t>(tgc::Renderer::kTextBuiltinSmall);
    };

    // Reserve a label + one line for each optional reading and one short line
    // each for strokes and accuracy, so the meanings block only takes what is
    // left (it ellipsises rather than clipping, as elsewhere in this file).
    int reserve = 11;                                       // strokes line
    if (!on.empty()) reserve += 8 + lh;                     // ON label + value
    if (!kun.empty()) reserve += 8 + lh;                    // KUN label + value
    if (hasAcc) reserve += 11;                              // accuracy line
    int meaningLines = (bottom - top - reserve - 8) / lh;
    if (meaningLines < 1) meaningLines = 1;
    if (meaningLines > 3) meaningLines = 3;

    int y = top;
    const uint8_t mset = latinSet(card.meanings);
    smallText(d, ui().quizMeaning, x, y, kYellow, kBlack);
    y += 8;
    y += gRender.drawWrapped(d, card.meanings, x, y, w, meaningLines, kWhite, kBlack, mset) *
         gRender.lineHeight(mset);
    y += 3;

    auto drawReading = [&](const char* label, const std::string& s) {
        if (s.empty() || y + 8 + lh > bottom) return;
        const uint8_t set = kanaSet(s);
        smallText(d, label, x, y, kYellow, kBlack);
        y += 8;
        y += gRender.drawWrapped(d, s.c_str(), x, y, w, 1, kWhite, kBlack, set) *
             gRender.lineHeight(set);
        y += 3;
    };
    drawReading(ui().detailOn, on);
    drawReading(ui().detailKun, kun);

    if (y + 11 <= bottom) {
        char s[32];
        snprintf(s, sizeof s, ui().detailStrokesFmt, (unsigned)card.strokes);
        smallText(d, s, x, y, kWhite, kBlack);
        y += 11;
    }
    if (hasAcc && y + 11 <= bottom) {
        char s[32];
        const uint32_t total = rec->reps;
        const uint32_t correct = rec->reps > rec->lapses ? rec->reps - rec->lapses : 0;
        snprintf(s, sizeof s, ui().detailAccuracyFmt, (unsigned)correct, (unsigned)total);
        smallText(d, s, x, y, kGreen, kBlack);
    }
    present();
}

void showDetail() {
    tgc::Card c;
    gPack.card(gQ.cardIndex, &c);
    // Start the stroke animation when the pack has stroke data for this card.
    uint16_t sc = 0;
    gAnimOn = gPack.strokeInfo(gQ.cardIndex, &sc, nullptr) && sc > 0;
    gAnimStrokes = sc;
    gAnimFrame = 0;
    gAnimNext = millis() + kStrokeMs;
    if (gAnimOn)
        QLOG("D U+%04X strokes=%u animating\n", (unsigned)c.codepoint, (unsigned)sc);
    else
        QLOG("D U+%04X\n", (unsigned)c.codepoint);
    gScreen = Screen::Detail;
    drawDetail();
}

// Rebuilds the queue and shows the quiz (or the done screen if nothing is due).
// Used after the date screen so newly due cards appear.
void leaveDate() {
    gQueue.rebuild(gPack, gSrs, gRng);
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// Applies the chosen number of days, persists it, rebuilds the queue (cards may
// now be due) and goes back to where the date screen was opened from: the
// settings home, or the quiz on a power-off boot.
void applyDate() {
    gSrs.advanceDays(kDateAdd[gDateSel]);
    gSrs.save(gSrsIo);
    gDirty = 0;
    gQueue.rebuild(gPack, gSrs, gRng);
    if (gAfterDate == Screen::Menu) {
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

void drawScreen() {
    if (gScreen == Screen::Menu) drawMenu();
    else if (gScreen == Screen::Decks) drawDecks();
    else if (gScreen == Screen::Choice) drawChoice();
    else if (gScreen == Screen::Done) drawDone();
    else if (gScreen == Screen::Date) drawDate();
    else if (gScreen == Screen::Stats) drawStats();
    else if (gScreen == Screen::Detail) drawDetail();
    else drawQuiz();
}

// Switch the panel between portrait (default) and landscape: rotate, pick up the
// new width/height, recreate the back buffer for it, persist the choice in NVS
// and redraw. The canvas is recreated here so a landscape change can never leave
// the portrait default (or the previous orientation) mis-sized.
void applyOrientation(bool landscape) {
    tgc::board::setOrientation(landscape);
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    gPrefs.putBool("land", landscape);
    drawScreen();
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
    setUiForLang(gPack.header().lang);
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
    drawScreen();
}

void closeMenu() {
    gScreen = gBeforeMenu;
    drawScreen();
}

// Opens the deck list with the loaded pack highlighted.
void openDecks() {
    gDeckSel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) gDeckSel = static_cast<int>(i);
    gScreen = Screen::Decks;
    drawScreen();
}

// Opens one of the Screen::Choice pickers (2/3/4 rows) with the saved value
// highlighted when it is one of the candidates.
void openChoice(Choice which) {
    gChoice = which;
    gChoiceSel = 0;
    if (which == Choice::Orientation) {
        gChoiceCount = 2;
        gChoiceValues[0] = 0;  // portrait
        gChoiceValues[1] = 1;  // landscape
        gChoiceSel = gPrefs.getBool("land", false) ? 1 : 0;
        gScreen = Screen::Choice;
        drawScreen();
        return;
    }
    if (which == Choice::UiLang) {
        gChoiceCount = 3;
        gChoiceValues[0] = tgc::kUiLangAuto;
        gChoiceValues[1] = tgc::kUiLangEnglish;
        gChoiceValues[2] = tgc::kUiLangSpanish;
        gChoiceSel = gUiLangPref;  // rows are in preference order
        if (gChoiceSel < 0 || gChoiceSel >= gChoiceCount) gChoiceSel = 0;
        gScreen = Screen::Choice;
        drawScreen();
        return;
    }
    gChoiceCount = 4;
    if (which == Choice::NewPerDay) {
        gChoiceValues[0] = 3;
        gChoiceValues[1] = 5;
        gChoiceValues[2] = 10;
        gChoiceValues[3] = 20;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.newPerDay()) gChoiceSel = i;
    } else {
        gChoiceValues[0] = 1500;
        gChoiceValues[1] = 3000;
        gChoiceValues[2] = 5000;
        gChoiceValues[3] = 0xFFFF;  // disables Hard
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.slowMs()) gChoiceSel = i;
    }
    gScreen = Screen::Choice;
    drawScreen();
}

// Applies the highlighted choice, persists it and returns to the settings home.
void applyChoice() {
    if (gChoice == Choice::Orientation) {
        gScreen = Screen::Menu;  // applyOrientation redraws the active screen
        applyOrientation(gChoiceValues[gChoiceSel] != 0);
        return;
    }
    if (gChoice == Choice::UiLang) {
        gUiLangPref = static_cast<int>(gChoiceValues[gChoiceSel]);
        gPrefs.putInt("ui", gUiLangPref);
        setUiForLang(gPack.header().lang);  // re-render in the chosen language
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    const uint16_t v = gChoiceValues[gChoiceSel];
    if (gChoice == Choice::NewPerDay) {
        gSrs.setNewPerDay(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
        gQueue.rebuild(gPack, gSrs, gRng);  // the daily new-card cap changed
    } else {
        gSrs.setSlowMs(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gScreen = Screen::Menu;
    drawScreen();
}

// Activates the highlighted settings row.
void activateSetting() {
    switch (gMenuSel) {
        case 0:  // Seguir
            closeMenu();
            break;
        case 1:  // Mazos
            openDecks();
            break;
        case 2:  // Fecha
            gAfterDate = Screen::Menu;
            gDateSel = 0;
            gScreen = Screen::Date;
            drawScreen();
            break;
        case 3:  // Nuevas/dia
            openChoice(Choice::NewPerDay);
            break;
        case 4:  // Velocidad
            openChoice(Choice::SlowMs);
            break;
        case 5:  // Estadisticas
            gScreen = Screen::Stats;
            drawScreen();
            break;
        case 6:  // Orientacion
            openChoice(Choice::Orientation);
            break;
        case 7:  // Idioma / Language
            openChoice(Choice::UiLang);
            break;
    }
}

// Loads the highlighted deck (replacing the live pack only after it validates),
// resets the "study more" override and starts the quiz.
void selectPack() {
    bool loaded = false;
    gExtraNew = 0;
    if (gDeckSel >= 0 && gDeckSel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[gDeckSel];
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

    gPoweredOff = !(gPowerMagic == kPowerMagic && gPowerMagicInv == ~kPowerMagic);
    gPowerMagic = kPowerMagic;
    gPowerMagicInv = ~kPowerMagic;
    auto& disp = tgc::board::display();
    kGrey = disp.color565(60, 60, 70);
    kHighlight = disp.color565(30, 60, 150);
    kGreen = disp.color565(0, 130, 40);
    kRed = disp.color565(170, 20, 20);
    kYellow = disp.color565(255, 210, 60);

    // Apply the saved orientation right after board::begin() (which set portrait)
    // and before the canvas exists, so the buffer matches the panel from boot.
    gPrefs.begin("tangocho", false);
    gUiLangPref = gPrefs.getInt("ui", tgc::kUiLangAuto);
    setUiForLang(nullptr);  // localise the pre-pack error screens to the choice too
    tgc::board::setOrientation(gPrefs.getBool("land", false));
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    gRng = tgc::XorShiftRng(tgc::board::randomSeed());

    if (!LittleFS.begin(false)) {
        showError(ui().errNoFilesystem, ui().errNoFilesystemHint);
        halt();
    }
    listPacks(gPacks);
    if (gPacks.empty()) {
        showError(ui().errNoPack, ui().errNoPackHint);
        halt();
    }
    String want = gPrefs.getString("pack", "");
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) want = gPacks[0];
    if (!loadPack(want)) {
        showError(ui().errOpenPack, want.c_str());
        halt();
    }
    gPrefs.putString("pack", gCurrentPack);

    const bool srsLoaded = gSrs.begin(gSrsIo);
    gSrs.beginSession();
    gExtraNew = 0;
    gQueue.rebuild(gPack, gSrs, gRng);  // loadPack rebuilt with an empty store
    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);

    char today[16];
    formatDate(gSrs.today(), today, sizeof today);
    Serial.printf("srs: day %u (%s), records %u, due %u, new %u%s, powered_off=%d\n",
                  (unsigned)gSrs.today(), today, (unsigned)gSrs.count(), (unsigned)due,
                  (unsigned)fresh, srsLoaded ? "" : " (new)", gPoweredOff ? 1 : 0);

    if (gPoweredOff && gSrs.count() > 0) {
        // Power cut: the elapsed days are unknown. Ask *before* deciding "done",
        // because advancing the day can make reviews due.
        gAfterDate = Screen::Quiz;
        gDateSel = 0;
        gScreen = Screen::Date;
        drawScreen();
        return;
    }
    if (!newQuestion()) {
        showDone();
    } else {
        drawScreen();
    }
}

// One button drives everything: a tap cycles the highlight (or, once revealed,
// moves to the next card); a 600 ms hold confirms the highlighted option. The
// tap action runs on release, and a hold suppresses it, so a long press is never
// also a tap.

// The input after the answer has been revealed: a correct answer advances as
// before; a wrong one opens the detail card first, and the next input advances.
void afterReveal() {
    if (!gLastCorrect) {
        showDetail();
        return;
    }
    if (newQuestion()) {
        gScreen = Screen::Quiz;
        drawScreen();
    } else {
        showDone();
    }
}

void tapAction() {
    if (gRevealed) {
        afterReveal();
        return;
    }
    gSel = (gSel + 1) % gQ.count;
    gLastNavAt = millis();  // answer clock restarts on every navigation tap
    drawScreen();
}

void holdAction(uint32_t pressedAt) {
    if (gRevealed) {
        afterReveal();
        return;
    }
    gRevealed = true;

    const bool correct = gSel == gQ.correct;
    gLastCorrect = correct;
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
    gSrs.addReviewMs(rt_ms);
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
            gMenuSel = (gMenuSel + 1) % kSettingRows;
            drawScreen();
        } else if (a == 2) {
            activateSetting();
        }
        if (b) closeMenu();  // side button backs out to the previous screen
    } else if (gScreen == Screen::Decks) {
        if (a == 1) {
            if (!gPacks.empty()) gDeckSel = (gDeckSel + 1) % static_cast<int>(gPacks.size());
            drawScreen();
        } else if (a == 2) {
            selectPack();
        }
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Choice) {
        if (a == 1) {
            gChoiceSel = (gChoiceSel + 1) % gChoiceCount;
            drawScreen();
        } else if (a == 2) {
            applyChoice();
        }
        if (b) {  // cancel: back to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Date) {
        if (a == 1) {
            gDateSel = (gDateSel + 1) % kDateRows;
            drawScreen();
        } else if (a == 2) {
            applyDate();
        }
        if (b) {  // cancel: back to the menu, or straight into the quiz
            if (gAfterDate == Screen::Menu) {
                gScreen = Screen::Menu;
                drawScreen();
            } else {
                leaveDate();
            }
        }
    } else if (gScreen == Screen::Stats) {
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Detail) {
        // Animate the stroke order while idle: one stroke every ~550 ms, a
        // ~1.2 s hold on the last one, then loop. Redraw only on a new frame.
        if (gAnimOn && static_cast<int32_t>(t - gAnimNext) >= 0) {
            const int count = static_cast<int>(gAnimStrokes);
            gAnimFrame = (gAnimFrame + 1) % count;
            gAnimNext = t + (gAnimFrame == count - 1 ? kStrokePauseMs : kStrokeMs);
            drawDetail();
        }
        // Any input (tap, hold or side tap) starts the next question; a side hold
        // opens the menu, as on the quiz.
        if (a == 1 || a == 2 || b == 1) {
            if (newQuestion()) {
                gScreen = Screen::Quiz;
                drawScreen();
            } else {
                showDone();
            }
        } else if (b == 2) {
            openMenu();
        }
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
