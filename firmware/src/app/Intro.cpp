// The intro card; see Intro.h.
#include "app/Intro.h"

#include <Arduino.h>

#include <string>

#include "app/App.h"
#include "app/Display.h"
#include "app/KanjiCard.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/ScreenKit.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "pack/Utf8.h"
#include "render/Widgets.h"
#include "ui/HintBar.h"
#include "ui/Icons.h"
#include "ui/QuizUi.h"
#include "ui/Theme.h"

namespace tgc {
namespace app {

namespace {

constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
// A press that goes down this soon after the intro (or its second page) came
// on screen is ignored, so a double tap on the screen before cannot skip it
// (docs/learning-mode.md §3.3; the Delete progress confirmation guards its
// Enter the same way).
constexpr uint32_t kIntroGuardMs = 400;

struct IntroWord {
    std::string word, reading, gloss;
    uint8_t start = 0, len = 0;  // the bytes of `reading` the card's kanji reads
};

// The card on screen, copied in prepareIntro(): a streamed pack keeps a
// card's strings only until its next reads.
struct IntroCard {
    uint32_t cardIndex = 0;
    uint32_t cp = 0;
    uint8_t strokes = 0;
    std::string meanings, onyomi, kunyomi;
    IntroWord words[2];
    int wordCount = 0;
};
IntroCard gIntro;
int gIntroPage = 0;             // 0 the card, 1 its example words (page 2)
bool gIntroDrawn = false;       // the intro has been on screen since prepareIntro()
uint32_t gIntroShownAt = 0;     // first drawn: its time on screen (addIntroMs())
uint32_t gIntroGuardAt = 0;     // presses that go down within kIntroGuardMs are ignored
bool gIntroGuardPress = false;  // a button went down inside the guard: its press goes

// The UI font's advance (kana and kanji from the pack's text set): the
// widths ui/QuizUi.h lays the intro out with.
int adv(uint32_t cp) { return gRender.charAdvance(cp, kLabel); }

tgc::IntroLayout introGeom() {
    const tgc::PackHeader& ph = gPack->header();
    return tgc::introLayout(W, H, ph.largePx ? ph.largePx : 64, ph.smallPx ? ph.smallPx : 40,
                            gRender.lineHeight(kLabel));
}

// The intro's card as ui/QuizUi.h lays it out.
tgc::Card introCard() {
    tgc::Card c{};
    c.codepoint = gIntro.cp;
    c.meanings = gIntro.meanings.c_str();
    c.onyomi = gIntro.onyomi.c_str();
    c.kunyomi = gIntro.kunyomi.c_str();
    c.strokes = gIntro.strokes;
    return c;
}

int introWordTexts(tgc::IntroWordText out[2]) {
    for (int i = 0; i < gIntro.wordCount && i < 2; ++i)
        out[i] = tgc::IntroWordText{gIntro.words[i].word.c_str(), gIntro.words[i].reading.c_str(),
                                    gIntro.words[i].gloss.c_str()};
    return gIntro.wordCount;
}

// Page 1 as planIntroCard() lays it out on this panel.
tgc::IntroCardPlan planCard(const tgc::IntroLayout& l) {
    const tgc::Card c = introCard();
    tgc::IntroWordText words[2];
    const int n = introWordTexts(words);
    return tgc::planIntroCard(l, c, words, n, gOnKana == tgc::OnKana::Hiragana, readingTagW(),
                              gRender.lineHeight(kLabel), adv);
}

// The header: the NEW pill, the stroke counter (decided per card, as on the
// detail card) or "2/2" on the words page, the battery, and the quiz's
// session bar, where the intro still counts as an item left.
void drawIntroHeader(lgfx::LovyanGFX& d) {
    using namespace tgc::theme;
    wg::Header h;
    h.pill = ui().introPill;
    h.pillFg = kAccentHi;
    h.pillBg = kSurfaceHi;
    h.battery = tgc::board::batteryPercent();
    gShownBattery = h.battery;
    const tgc::SessionProgress p =
        tgc::sessionProgress(static_cast<uint32_t>(gQueue.answeredCount()),
                             static_cast<uint32_t>(gQueue.remaining()), true);
    h.progressNum = static_cast<int>(p.done);
    h.progressDen = static_cast<int>(p.total);
    char counter[32];
    if (gIntroPage == 1)
        snprintf(counter, sizeof counter, "%d/%d", 2, 2);
    else
        strokeCounter(counter, sizeof counter, tgc::introCounterRoom(W, h.pill, h.battery >= 0),
                      gIntro.strokes);
    if (counter[0]) h.right = counter;
    wg::drawHeader(d, gRender, W, h);
}

// Always shown (the side tap means something new here): "• next" and
// "✓ known"; on the keys "↵ next" and "← known".
void drawIntroHints(lgfx::LovyanGFX& d) {
    wg::Hints h;
    h.tap = ui().hintNext;
    h.side = &tgc::kIconCheck;
    h.sideWord = ui().hintKnown;
    drawHints(d, h, tgc::kKeyEnter);
}

// A word's reading: the part the card's kanji reads in `text`, the rest in
// `text-dim` (all of it dim when that part is unknown), in hiragana whatever
// the On'yomi setting says: it is how the word is written.
void drawWordReading(lgfx::LovyanGFX& d, const IntroWord& w, int x, int y, int maxW, int lh) {
    using namespace tgc::theme;
    if (!tgc::readingSpanValid(w.reading.c_str(), w.start, w.len)) {
        gRender.drawLabel(d, w.reading.c_str(), x, y, maxW, lh, kTextDim, kBg, kLabel);
        return;
    }
    const std::string parts[3] = {w.reading.substr(0, w.start), w.reading.substr(w.start, w.len),
                                  w.reading.substr(w.start + w.len)};
    const uint16_t inks[3] = {kTextDim, kText, kTextDim};
    int cx = x;
    for (int i = 0; i < 3; ++i) {
        const int room = x + maxW - cx;
        if (parts[i].empty()) continue;
        if (room <= 0) break;
        cx += gRender.drawLabel(d, parts[i].c_str(), cx, y, room, lh, inks[i], kBg, kLabel);
    }
}

// The words from (x, y) in a column `w` px wide, as planIntroWords() laid
// them out: the form in `text`, the reading beside or under it, the gloss
// in `text-dim` under them.
void drawWords(lgfx::LovyanGFX& d, int x, int y, int w, const IntroWord* words, int n,
               const tgc::IntroWordLines* lines, int lh) {
    using namespace tgc::theme;
    for (int i = 0; i < n; ++i) {
        const IntroWord& iw = words[i];
        const int ww = gRender.drawLabel(d, iw.word.c_str(), x, y, w, lh, kText, kBg, kLabel);
        if (lines[i].oneLine) {
            drawWordReading(d, iw, x + ww + tgc::kIntroWordGap, y, w - ww - tgc::kIntroWordGap, lh);
        } else {
            y += lh;
            drawWordReading(d, iw, x, y, w, lh);
        }
        y += lh;
        if (lines[i].glossLines > 0) {
            gRender.drawWrapped(d, iw.gloss.c_str(), x, y, w, lines[i].glossLines, kTextDim, kBg,
                                kLabel);
            y += lines[i].glossLines * lh;
        }
        y += tgc::kIntroWordsGap;
    }
}

// planIntroWords() (ui/QuizUi.h) over `n` of the card's words.
int planWords(const IntroWord* words, int n, int w, int h, int lh, bool sideBySide,
              tgc::IntroWordLines out[2]) {
    tgc::IntroWordText t[2];
    for (int i = 0; i < n && i < 2; ++i)
        t[i] = tgc::IntroWordText{words[i].word.c_str(), words[i].reading.c_str(),
                                  words[i].gloss.c_str()};
    return tgc::planIntroWords(t, n, w, h, lh, sideBySide, adv, out);
}

// Page 1, the detail card's frame: the kanji with its stroke animation, then
// (under it, or right of it in landscape) the meanings -- the asked one in
// `text`, which takes two lines when it is the only one, the others dim on
// one line -- the reading rows, and the words when they all fit under them.
void drawIntroCard(lgfx::LovyanGFX& d, const tgc::IntroLayout& l, const tgc::IntroCardPlan& p) {
    using namespace tgc::theme;
    drawKanjiBox(d, gIntro.cp, l.px, l.py, l.pw, l.ph);
    const int lh = gRender.lineHeight(kLabel);
    const int x = l.colX;
    const int w = l.colW;
    int y = l.colTop;
    const tgc::IntroMeanings m = tgc::introMeanings(gIntro.meanings.c_str());
    if (p.firstLines > 0) {
        gRender.drawWrapped(d, m.first.c_str(), x, y, w, p.firstLines, kText, kBg, kLabel);
        y += p.firstLines * lh + 2;
    }
    if (p.restLine) {
        gRender.drawWrapped(d, m.rest.c_str(), x, y, w, 1, kTextDim, kBg, kLabel);
        y += lh + 2;
    }
    const int tagW = readingTagW();
    for (int i = 0; i < p.rows; ++i) {
        const tgc::IntroReading& r = p.readings[i];
        drawReadingRow(d, x, y, w, lh, tagW, r.kind, r.first.c_str(), r.rest.c_str(), r.more);
        y += lh + 2;
    }
    if (p.words > 0) drawWords(d, x, p.wordsY, w, gIntro.words, p.words, p.wordLines, lh);
}

// Page 2, the example words: a head (portrait: the small kanji beside its
// first meaning; landscape: both on one text line), a rule, then the words
// one under the other (in landscape each word's form and reading side by
// side).
void drawIntroWordsPage(lgfx::LovyanGFX& d, const tgc::IntroLayout& l) {
    using namespace tgc::theme;
    const int lh = gRender.lineHeight(kLabel);
    const tgc::IntroMeanings m = tgc::introMeanings(gIntro.meanings.c_str());
    if (l.headKanji) {
        const int box = l.headH;
        gRender.drawKanji(d, tgc::kGlyphSetSmall, gIntro.cp, kGutter, l.headY, box, box, kWhite,
                          kBlack);
        const int mx = kGutter + box + 4;
        gRender.drawLabel(d, m.first.c_str(), mx, l.headY, W - mx - kGutter, box, kText, kBg,
                          kLabel);
    } else {
        // The kanji in the text set, which holds it in a deck with words.
        char k[5];
        tgc::utf8Encode(gIntro.cp, k);
        const int kw = gRender.drawLabel(d, k, kGutter, l.headY, W - 2 * kGutter, lh, kText, kBg,
                                         kLabel);
        const int mx = kGutter + kw + 8;
        gRender.drawLabel(d, m.first.c_str(), mx, l.headY, W - mx - kGutter, lh, kText, kBg,
                          kLabel);
    }
    d.drawFastHLine(kGutter, l.ruleY, W - 2 * kGutter, kLine);

    tgc::IntroWordLines lines[2];
    const int k = planWords(gIntro.words, gIntro.wordCount, l.wordsW, l.pageBottom - l.wordsY, lh,
                            l.wordsSideBySide, lines);
    drawWords(d, kGutter, l.wordsY, l.wordsW, gIntro.words, k, lines, lh);
}

// One stroke-animation step: only the header (its counter) and the kanji's
// em square are redrawn and pushed.
void drawIntroFrame() {
    auto& d = *gCanvas;
    const tgc::IntroLayout l = introGeom();
    drawIntroHeader(d);
    drawKanjiBox(d, gIntro.cp, l.px, l.py, l.pw, l.ph);
    presentKanjiFrame(l.px, l.py, l.pw, l.ph);
}

// Front press: the learner read it. The exposure is recorded (docs/srs.md §7:
// a learning record, not an answer) and step 1 goes 3 items on.
void introGoOn(uint32_t t) {
    const uint32_t ms = t - gIntroShownAt;
    if (gQueue.introduced(gIntro.cardIndex)) {
        const bool recorded = gSrs.introduce(gIntro.cp, gSrs.today());
        gSrs.addIntroMs(ms);
        ++gDirty;
        gLastAnswerAt = millis();  // the idle flush counts from here
        QLOG("I U+%04X introduced%s, %u ms\n", (unsigned)gIntro.cp,
             recorded ? "" : " (no record: not new)", (unsigned)ms);
    } else {
        QLOG("I U+%04X: no longer the queue's item\n", (unsigned)gIntro.cp);
    }
    showNextItem();
}

// Side tap: "I know this one". The card is asked at once as an ordinary first
// question (no steps); the intro's time still counts.
void introKnown(uint32_t t) {
    const uint32_t ms = t - gIntroShownAt;
    if (gQueue.known(gIntro.cardIndex)) {
        gSrs.addIntroMs(ms);
        QLOG("I U+%04X known, %u ms\n", (unsigned)gIntro.cp, (unsigned)ms);
    }
    showNextItem();
}

}  // namespace

bool prepareIntro(uint32_t cardIndex) {
    tgc::Card c;
    if (!gPack->card(cardIndex, &c)) return false;
    gIntro = IntroCard{};
    gIntro.cardIndex = cardIndex;
    gIntro.cp = c.codepoint;
    gIntro.strokes = c.strokes;
    gIntro.meanings = c.meanings;
    gIntro.onyomi = c.onyomi;
    gIntro.kunyomi = c.kunyomi;
    // After the card's strings are copied: the words' reads may reuse their
    // slots in a streamed pack.
    tgc::ExampleWord w[2];
    const uint32_t n = gPack->exampleWords(cardIndex, w, 2);
    for (uint32_t i = 0; i < n; ++i) {
        IntroWord& iw = gIntro.words[i];
        iw.word = w[i].word;
        iw.reading = w[i].reading;
        iw.gloss = w[i].gloss;
        iw.start = w[i].readingStart;
        iw.len = w[i].readingLen;
    }
    gIntro.wordCount = static_cast<int>(n);
    startStrokeAnim(cardIndex, millis());  // started again when first drawn
    gIntroPage = 0;
    gIntroDrawn = false;
    gIntroGuardPress = false;
    QLOG("I U+%04X intro: strokes %u, words %d\n", (unsigned)gIntro.cp, (unsigned)gAnim.strokes,
         gIntro.wordCount);
    return true;
}

void drawIntro() {
    if (!gPack->isOpen()) {
        drawNoQuestion();
        return;
    }
    // The time on screen, the guard and the animation start with the first
    // frame: an intro made ready behind the settings (a deck change) is seen
    // only later.
    if (!gIntroDrawn) {
        gIntroDrawn = true;
        gIntroShownAt = gIntroGuardAt = millis();
        startStrokeAnim(gIntro.cardIndex, gIntroShownAt);
    }
    auto& d = *gCanvas;
    d.fillScreen(tgc::theme::kBg);
    const tgc::IntroLayout l = introGeom();
    const tgc::IntroCardPlan p = planCard(l);
    if (!p.wordsPage) gIntroPage = 0;  // turned to a panel with the words inline
    drawIntroHeader(d);
    if (gIntroPage == 1)
        drawIntroWordsPage(d, l);
    else
        drawIntroCard(d, l, p);
    drawIntroHints(d);
    present();
}

void introLoop(int a, int b, uint32_t t, bool down) {
    // The stroke animation, as on the detail card (page 1 only).
    if (gIntroPage == 0 && strokeAnimTick(t)) drawIntroFrame();
    // The guard: a press that went down inside it is dropped, up to its
    // release (keys have no release here: those inside it are dropped).
    if (down && t - gIntroGuardAt < kIntroGuardMs) gIntroGuardPress = true;
    const bool guarded = gIntroGuardPress || t - gIntroGuardAt < kIntroGuardMs;
    if (!down) gIntroGuardPress = false;
    if (guarded || (!a && !b)) return;
    if (a == 1 || a == 2) {
        // Front tap or hold (Enter, right, down, G0): page 1 turns to the
        // words when they have a page, else the intro is done.
        if (gIntroPage == 0 && planCard(introGeom()).wordsPage) {
            gIntroPage = 1;
            gIntroGuardAt = t;
            drawIntro();
            return;
        }
        introGoOn(t);
    } else if (b == 1) {
        introKnown(t);  // side tap (left, up): known, on either page
    } else if (b == 2) {
        openMenu();  // side hold (Tab, Esc): the settings, which come back here
    }
}

void introReshown() {
    gIntroGuardAt = millis();
    gIntroGuardPress = false;
}

}  // namespace app
}  // namespace tgc
