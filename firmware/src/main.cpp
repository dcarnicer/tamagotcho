// Tangocho Phase 2: open the saved pack in /packs and run a spaced-repetition
// multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>
#include <cstring>
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
    const char* menuRows[5];  // continue, decks, date, new/day, speed
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
    const char* doneAllReviewed;
    const char* doneDayFmt;
    const char* doneDueFmt;
    const char* doneStudyMore;
    const char* doneMenuHint;
    const char* deckCountsFmt;
    const char* errNoFilesystem;
    const char* errNoFilesystemHint;
    const char* errNoPack;
    const char* errNoPackHint;
    const char* errOpenPack;
};

const UiStrings kUiEn = {
    "Tangocho",
    "SETTINGS", "DECKS", "side: back",
    {"Continue", "Decks", "Date", "New/day", "Speed"},
    "NEW/DAY", "SPEED", "no", "%.1f s",
    "MEANING", "READING", "KANJI",
    "DATE", {"today (0)", "1 day", "2-7 days", "more (14)"},
    "DONE", "All reviewed for now", "day %u  %u cards", "due %u  new %u", "tap/hold: +10 new",
    "side hold: menu",
    "  %u/%u",
    "Filesystem not found", "Run: pio run -t uploadfs",
    "No pack found", "Put a .tgc in data/packs/ then run uploadfs",
    "Cannot open pack",
};

const UiStrings kUiEs = {
    "Tangocho",
    "AJUSTES", "MAZOS", "lateral: atrás",
    {"Seguir", "Mazos", "Fecha", "Nuevas/día", "Velocidad"},
    "NUEVAS/DIA", "VELOCIDAD", "no", "%.1f s",
    "SIGNIFICADO", "LECTURA", "KANJI",
    "FECHA", {"hoy (0)", "1 día", "2-7 días", "más (14)"},
    "FIN", "Todo repasado por ahora", "dia %u  %u tarjetas", "pendientes %u  nuevas %u",
    "tap/hold: +10 nuevas", "lateral: menu",
    "  %u/%u",
    "No se encuentra el sistema de archivos", "Ejecuta: pio run -t uploadfs",
    "No hay ningún mazo", "Pon un .tgc en data/packs/ y ejecuta uploadfs",
    "No se puede abrir el mazo",
};

const UiStrings* gUi = &kUiEn;  // English until a pack says otherwise
const UiStrings& ui() { return *gUi; }

// "es" selects the Spanish table; anything else (including no pack yet) is English.
void setUiForLang(const char* lang) { gUi = (lang && strcmp(lang, "es") == 0) ? &kUiEs : &kUiEn; }

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

enum class Screen { Quiz, Menu, Decks, Choice, Done, Date };
// Screen::Choice is shared by the two four-row pickers (see openChoice()).
enum class Choice { NewPerDay, SlowMs };
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
    smallText(d, k2m ? ui().quizKanji : (reading ? ui().quizReading : ui().quizMeaning), W / 2,
              headerH + 2 + kanjiH, kYellow, kBlack, lgfx::top_center);

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

// One full-width highlighted row for the settings home and the choice screens,
// labelled with the built-in 2x font so accented Spanish renders.
void drawRow(lgfx::LovyanGFX& d, const char* label, int y, bool sel) {
    const uint16_t bg = sel ? kHighlight : kBlack;
    d.fillRoundRect(2, y, W - 4, 32, 5, bg);
    if (sel) d.drawRoundRect(2, y, W - 4, 32, 5, kWhite);
    gRender.drawCentred(d, label, 4, y + 1, W - 8, 30, 1, kWhite, bg, tgc::Renderer::kTextBuiltin);
}

// Settings home (hold the side button from the quiz): tap moves the highlight,
// hold activates the row. Each row is a full-width box with a label.
constexpr int kSettingRows = 5;

void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().settingsTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    const int top = 20, step = 36;
    for (int i = 0; i < kSettingRows; ++i)
        drawRow(d, ui().menuRows[i], top + i * step, i == gMenuSel);
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
    const int top = 18, rowH = 20;
    const int rows = static_cast<int>(gPacks.size());
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

// Generic four-row choice screen: a title and the candidate values. Hold applies
// the highlighted value; the side button cancels back to the settings home.
void drawChoice() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, gChoice == Choice::NewPerDay ? ui().newPerDayTitle : ui().speedTitle, 3, 3, kWhite,
              kGrey);
    drawBackHint(d);

    const int top = 40, step = 40;
    for (int i = 0; i < gChoiceCount; ++i) {
        char label[16];
        if (gChoice == Choice::SlowMs) {
            if (gChoiceValues[i] == 0xFFFF) snprintf(label, sizeof label, "%s", ui().speedOff);
            else snprintf(label, sizeof label, ui().speedSecondsFmt, gChoiceValues[i] / 1000.0f);
        } else {
            snprintf(label, sizeof label, "%u", (unsigned)gChoiceValues[i]);
        }
        drawRow(d, label, top + i * step, i == gChoiceSel);
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

    const int top = 44, rowH = 32;
    for (int i = 0; i < kDateRows; ++i) {
        const int y = top + i * rowH;
        const bool sel = i == gDateSel;
        const uint16_t bg = sel ? kHighlight : kBlack;
        d.fillRoundRect(2, y, W - 4, rowH - 4, 5, bg);
        if (sel) d.drawRoundRect(2, y, W - 4, rowH - 4, 5, kWhite);
        gRender.drawCentred(d, ui().dateRows[i], 4, y + 2, W - 8, rowH - 8, 1, kWhite, bg,
                            tgc::Renderer::kTextBuiltin);
    }
    present();
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

// Opens one of the two four-row pickers (Screen::Choice) with the saved value
// highlighted when it is one of the candidates.
void openChoice(Choice which) {
    gChoice = which;
    gChoiceSel = 0;
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
        showError(ui().errNoFilesystem, ui().errNoFilesystemHint);
        halt();
    }
    listPacks(gPacks);
    if (gPacks.empty()) {
        showError(ui().errNoPack, ui().errNoPackHint);
        halt();
    }
    gPrefs.begin("tangocho", false);
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
