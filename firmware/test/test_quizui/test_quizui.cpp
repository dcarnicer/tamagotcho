// Native tests for the quiz screens' pure logic (ui/QuizUi.h): session
// progress, option navigation, the hint bar policy, hold progress, auto-advance
// and the pixel budgets of the header pills and hint bars in English and
// Spanish (the strings are copies of app/UiStrings.cpp; keep them in step),
// the hint bar's layout (ui/HintBar.h), the date stepper's included, the
// detail card's reading rows, and the learning mode's intro card: its
// header, hint bar, meanings, reading rows, example words and layout on
// every panel, with the widest strings of the N5 decks.
#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/QuizUi.h"
#include "ui/UiFont.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

// Panel widths: StickS3 portrait / landscape, T-Display-S3 portrait.
constexpr int kWidths[] = {135, 240, 170};

// ---------------------------------------------------------------- progress

void test_progress_counts_the_open_question() {
    // First question of a 12-card session: 11 queued after it, none answered.
    SessionProgress p = sessionProgress(0, 11, true);
    TEST_ASSERT_EQUAL_UINT32(12, p.left);
    TEST_ASSERT_EQUAL_UINT32(0, p.done);
    TEST_ASSERT_EQUAL_UINT32(12, p.total);
    // Answered correctly: one done, 11 left.
    p = sessionProgress(1, 11, false);
    TEST_ASSERT_EQUAL_UINT32(11, p.left);
    TEST_ASSERT_EQUAL_UINT32(1, p.done);
    TEST_ASSERT_EQUAL_UINT32(12, p.total);
    // Answered wrong: the card is queued again, the total grows.
    p = sessionProgress(1, 12, false);
    TEST_ASSERT_EQUAL_UINT32(12, p.left);
    TEST_ASSERT_EQUAL_UINT32(13, p.total);
    // Last card answered: the bar is full.
    p = sessionProgress(13, 0, false);
    TEST_ASSERT_EQUAL_UINT32(0, p.left);
    TEST_ASSERT_EQUAL_UINT32(p.total, p.done);
}

// ---------------------------------------------------------------- navigation

void test_option_navigation_wraps_both_ways() {
    TEST_ASSERT_EQUAL(1, nextOption(0, 4));
    TEST_ASSERT_EQUAL(0, nextOption(3, 4));
    TEST_ASSERT_EQUAL(3, prevOption(0, 4));
    TEST_ASSERT_EQUAL(2, prevOption(3, 4));
    TEST_ASSERT_EQUAL(1, nextOption(0, 2));
    TEST_ASSERT_EQUAL(1, prevOption(0, 2));
    // Out of range or no options: never out of [0, count).
    TEST_ASSERT_EQUAL(1, nextOption(4, 4));
    TEST_ASSERT_EQUAL(2, prevOption(-1, 4));
    TEST_ASSERT_EQUAL(0, nextOption(2, 0));
    TEST_ASSERT_EQUAL(0, prevOption(2, 0));
    // From any option, every other is at most 2 presses away (§6.7).
    for (int from = 0; from < 4; ++from)
        for (int to = 0; to < 4; ++to) {
            int down = 0, up = 0;
            for (int s = from; s != to; s = nextOption(s, 4)) ++down;
            for (int s = from; s != to; s = prevOption(s, 4)) ++up;
            TEST_ASSERT_TRUE((down < up ? down : up) <= 2);
        }
}

void test_grid_option_moves_by_row_and_column() {
    // 2x2, row-major: 0 1 / 2 3.
    TEST_ASSERT_EQUAL(2, gridOption(0, 4, 2, 0, +1));
    TEST_ASSERT_EQUAL(2, gridOption(0, 4, 2, 0, -1));  // two rows: up wraps down
    TEST_ASSERT_EQUAL(1, gridOption(0, 4, 2, +1, 0));
    TEST_ASSERT_EQUAL(1, gridOption(0, 4, 2, -1, 0));
    TEST_ASSERT_EQUAL(1, gridOption(3, 4, 2, 0, -1));
    TEST_ASSERT_EQUAL(2, gridOption(3, 4, 2, -1, 0));
    // Three options: 0 1 / 2. Down from 1 has no cell below: the last option.
    TEST_ASSERT_EQUAL(2, gridOption(1, 3, 2, 0, +1));
    TEST_ASSERT_EQUAL(2, gridOption(2, 3, 2, +1, 0));  // alone in its row
    TEST_ASSERT_EQUAL(0, gridOption(2, 3, 2, 0, -1));
    // One column (the list layout): up and down are previous and next.
    TEST_ASSERT_EQUAL(nextOption(1, 4), gridOption(1, 4, 1, 0, +1));
    TEST_ASSERT_EQUAL(prevOption(0, 4), gridOption(0, 4, 1, 0, -1));
    // Out of range or no options: never out of [0, count).
    TEST_ASSERT_EQUAL(2, gridOption(4, 4, 2, 0, +1));
    TEST_ASSERT_EQUAL(0, gridOption(1, 0, 2, 0, +1));
}

// ---------------------------------------------------------------- hint bar

void test_hints_always_while_learning() {
    QuizHints h;
    h.question(1000);
    TEST_ASSERT_TRUE(h.shown(0));
    TEST_ASSERT_TRUE(h.shown(49));  // the question, its answer and the detail card
    TEST_ASSERT_FALSE(h.tick(60000, 49, true));  // already shown: no change
}

void test_hints_after_idle_on_open_question() {
    QuizHints h;
    h.question(1000);
    TEST_ASSERT_FALSE(h.shown(50));
    TEST_ASSERT_FALSE(h.tick(1000 + kHintIdleMs - 1, 50, true));
    h.input(5000);  // a tap restarts the idle clock
    TEST_ASSERT_FALSE(h.tick(1000 + kHintIdleMs, 50, true));
    TEST_ASSERT_FALSE(h.shown(50));
    TEST_ASSERT_TRUE(h.tick(5000 + kHintIdleMs, 50, true));  // appears: redraw
    TEST_ASSERT_TRUE(h.shown(50));
    // It stays while the user moves through the options...
    h.input(20000);
    TEST_ASSERT_FALSE(h.tick(20001, 50, true));
    TEST_ASSERT_TRUE(h.shown(50));
    // ... and for the answer (no tile jump at the reveal) and its detail card.
    TEST_ASSERT_FALSE(h.tick(20002, 51, false));
    TEST_ASSERT_TRUE(h.shown(51));
    // The next question starts hidden again.
    h.question(30000);
    TEST_ASSERT_FALSE(h.shown(51));
}

void test_hints_never_idle_on_a_revealed_answer() {
    QuizHints h;
    h.question(0);
    TEST_ASSERT_FALSE(h.tick(60000, 80, false));
    TEST_ASSERT_FALSE(h.shown(80));
}

// ---------------------------------------------------------------- hold progress

void test_hold_fill_timing() {
    const uint32_t hold = holdMsFor(kHoldNormal);  // 600
    TEST_ASSERT_TRUE(holdFill(0, hold) < 0.0f);
    TEST_ASSERT_TRUE(holdFill(kHoldShowMs - 1, hold) < 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, holdFill(kHoldShowMs, hold));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, holdFill(360, hold));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, holdFill(hold, hold));  // full when the hold fires
    TEST_ASSERT_EQUAL_FLOAT(1.0f, holdFill(hold + 100, hold));
    // Short and Long holds: still 0 at the start and full at the threshold.
    for (uint8_t pref : {kHoldShort, kHoldLong}) {
        const uint32_t ms = holdMsFor(pref);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, holdFill(kHoldShowMs, ms));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, holdFill(ms, ms));
        TEST_ASSERT_TRUE(holdFill(ms - 1, ms) < 1.0f);
    }
    // About a dozen frames over a normal hold (§2.6).
    TEST_ASSERT_TRUE((hold - kHoldShowMs) / kHoldFrameMs >= 10);
}

// ---------------------------------------------------------------- auto-advance

void test_auto_advance_fires_once_after_the_wait() {
    AutoAdvance a;
    a.start(1000, advanceMsFor(kAdvanceNormal));
    TEST_ASSERT_TRUE(a.active());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, a.left(1000));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, a.left(1500));
    TEST_ASSERT_FALSE(a.due(1999, false));
    TEST_ASSERT_TRUE(a.due(2000, false));
    TEST_ASSERT_FALSE(a.active());
    TEST_ASSERT_FALSE(a.due(2100, false));  // exactly one advance
}

void test_auto_advance_waits_for_buttons_and_off() {
    AutoAdvance a;
    a.start(0, advanceMsFor(kAdvanceShort));
    TEST_ASSERT_FALSE(a.due(700, true));  // a press is under way: it advances by itself
    TEST_ASSERT_TRUE(a.active());
    a.cancel();  // ... and so the press cancels the wait
    TEST_ASSERT_FALSE(a.due(800, false));
    a.start(0, advanceMsFor(kAdvanceOff));  // off: wait for a press
    TEST_ASSERT_FALSE(a.active());
    TEST_ASSERT_FALSE(a.due(5000, false));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.left(10));
}

// ---------------------------------------------------------------- pills

// The quiz header's right side: up to 3 digits of cards left and the battery.
static int quizPillRoom(int w, int leftDigits) { return headerPillRoom(w, leftDigits, true); }

void test_header_pill_room_matches_the_widget() {
    // 135 px: gutter 4, battery 13 + 5, "12" + 6 px, gutter 4.
    TEST_ASSERT_EQUAL(135 - 4 - 18 - 12 - 6 - 4, quizPillRoom(135, 2));
    TEST_ASSERT_EQUAL(135 - 4 - 18 - 4, headerPillRoom(135, 0, true));
    TEST_ASSERT_EQUAL(91, quizPillRoom(135, 2));
    TEST_ASSERT_EQUAL(85, quizPillRoom(135, 3));
}

void test_mode_pills_fit_everywhere() {
    const char* modes[] = {"MEANING", "SIGNIFICADO", "ON", "KUN", "KANJI", "READING", "LECTURA"};
    for (int w : kWidths)
        for (const char* m : modes) TEST_ASSERT_TRUE(pillWidthFor(m, 0) <= quizPillRoom(w, 3));
}

void test_result_pill_full_form_when_it_fits() {
    char buf[32];
    const int check = kIconCheck.w;
    int pw = resultPillText(buf, sizeof buf, "GOOD", 4, "d", check, quizPillRoom(135, 2));
    TEST_ASSERT_EQUAL_STRING("GOOD \xC2\xB7 4d", buf);
    TEST_ASSERT_EQUAL(9 * 6 + 6 + 9 + 3, pw);
    resultPillText(buf, sizeof buf, "AGAIN", -1, "d", kIconCross.w, quizPillRoom(135, 3));
    TEST_ASSERT_EQUAL_STRING("AGAIN", buf);
}

void test_result_pill_shortens_before_overflowing() {
    char buf[32];
    const int check = kIconCheck.w;
    // Spanish Hard with a 2-digit interval: no room for the spaced dot.
    int pw = resultPillText(buf, sizeof buf, "DIFICIL", 12, "d", check, quizPillRoom(135, 2));
    TEST_ASSERT_EQUAL_STRING("DIFICIL\xC2\xB7" "12d", buf);
    TEST_ASSERT_TRUE(pw <= quizPillRoom(135, 2));
    // The longest interval with 3 digits of cards left: the word alone.
    pw = resultPillText(buf, sizeof buf, "DIFICIL", 365, "d", check, quizPillRoom(135, 3));
    TEST_ASSERT_EQUAL_STRING("DIFICIL", buf);
    TEST_ASSERT_TRUE(pw <= quizPillRoom(135, 3));
    // Landscape has room for everything.
    resultPillText(buf, sizeof buf, "DIFICIL", 365, "d", check, quizPillRoom(240, 3));
    TEST_ASSERT_EQUAL_STRING("DIFICIL \xC2\xB7 365d", buf);
}

void test_result_pills_never_overflow() {
    // Every grade word of both tables, every interval length, every panel and
    // 1..3 digits of cards left: the pill fits its room.
    const char* words[] = {"GOOD", "HARD", "AGAIN", "BIEN", "DIFICIL", "OTRA VEZ"};
    const int days[] = {-1, 0, 1, 9, 12, 99, 100, 365};
    char buf[32];
    for (int w : kWidths)
        for (int digits = 1; digits <= 3; ++digits)
            for (const char* word : words)
                for (int d : days) {
                    const int room = quizPillRoom(w, digits);
                    const int pw = resultPillText(buf, sizeof buf, word, d, "d", kIconCheck.w, room);
                    TEST_ASSERT_TRUE_MESSAGE(pw <= room, buf);
                }
    // English always shows the interval with the spaced dot.
    for (int w : kWidths)
        for (const char* word : {"GOOD", "HARD"}) {
            resultPillText(buf, sizeof buf, word, 365, "d", kIconCheck.w, quizPillRoom(w, 3));
            TEST_ASSERT_NOT_NULL(std::strstr(buf, " \xC2\xB7 "));
        }
}

// ---------------------------------------------------------------- detail card

void test_stroke_counter_word_once_per_card() {
    // 135 px detail header: "DETAIL" (36 px) / "DETALLE" (42 px) on the left.
    const int roomEn = headerPillRoom(135, 0, true) - 36 - 6;
    const int roomEs = headerPillRoom(135, 0, true) - 42 - 6;
    TEST_ASSERT_TRUE(strokeCounterWorded("stroke %u/%u", 6, roomEn));
    TEST_ASSERT_TRUE(strokeCounterWorded("trazo %u/%u", 6, roomEs));
    TEST_ASSERT_FALSE(strokeCounterWorded("stroke %u/%u", 13, roomEn));
    TEST_ASSERT_FALSE(strokeCounterWorded("trazo %u/%u", 13, roomEs));
    // The bare counter always fits.
    TEST_ASSERT_TRUE(strokeCounterWorded("%u/%u", 30, roomEs));
    // Wider panels keep the word.
    TEST_ASSERT_TRUE(strokeCounterWorded("stroke %u/%u", 30, headerPillRoom(170, 0, true) - 36 - 6));
}

static Card readingsCard(const char* on, const char* kun) {
    Card c{};
    c.meanings = "meaning";
    c.onyomi = on;
    c.kunyomi = kun;
    return c;
}

void test_detail_reading_rows_skip_a_missing_kind() {
    DetailReading rows[2];
    // Both kinds: ON then KUN, every reading, okurigana dots removed.
    TEST_ASSERT_EQUAL_INT(2, detailReadings(readingsCard("ショク、ジキ", "た.べる、く.う"), rows));
    TEST_ASSERT_EQUAL(ReadingKind::On, rows[0].kind);
    TEST_ASSERT_EQUAL_STRING("ショク、ジキ", rows[0].text.c_str());
    TEST_ASSERT_EQUAL(ReadingKind::Kun, rows[1].kind);
    TEST_ASSERT_EQUAL_STRING("たべる、くう", rows[1].text.c_str());
    // No kun'yomi (気, 百, 肉 in a common-readings deck): the ON row alone, no
    // KUN pill beside a blank line.
    TEST_ASSERT_EQUAL_INT(1, detailReadings(readingsCard("キ、ケ", ""), rows));
    TEST_ASSERT_EQUAL(ReadingKind::On, rows[0].kind);
    TEST_ASSERT_EQUAL_STRING("キ、ケ", rows[0].text.c_str());
    // No on'yomi: the KUN row moves up to the first row.
    TEST_ASSERT_EQUAL_INT(1, detailReadings(readingsCard("", "はたけ、はた"), rows));
    TEST_ASSERT_EQUAL(ReadingKind::Kun, rows[0].kind);
    TEST_ASSERT_EQUAL_STRING("はたけ、はた", rows[0].text.c_str());
    // Neither: no rows at all.
    TEST_ASSERT_EQUAL_INT(0, detailReadings(readingsCard("", ""), rows));
}

// ---------------------------------------------------------------- hint bar fit

// The quiz's front items must keep 6 px clear of the side icon (the settings
// icon, right-aligned in the gutter), as the widget keeps for a side word.
static bool quizHintFits(int w, const char* tap, const char* hold) {
    const int end = hintFrontEnd(tap, hold, kIconTap.w, kIconHold.w);
    return end + 6 <= w - theme::kGutter - kIconMenu.w;
}

void test_quiz_hints_fit_portrait() {
    for (int w : kWidths) {
        TEST_ASSERT_TRUE(quizHintFits(w, "move", "answer"));
        TEST_ASSERT_TRUE(quizHintFits(w, "mover", "responder"));
        TEST_ASSERT_TRUE(quizHintFits(w, "next", nullptr));
        TEST_ASSERT_TRUE(quizHintFits(w, "details", nullptr));
        TEST_ASSERT_TRUE(quizHintFits(w, "siguiente", nullptr));
        TEST_ASSERT_TRUE(quizHintFits(w, "detalles", nullptr));
    }
    // The wording was chosen for this: "siguiente" next to the hold does not fit.
    TEST_ASSERT_FALSE(quizHintFits(135, "siguiente", "responder"));
}

// A bar without a side-hold word is laid out as before: one row, the side word
// only when it keeps 6 px clear of the front items.
void test_hint_plan_one_side_action_unchanged() {
    HintSpec s;
    s.tap = "move";
    s.hold = "open";
    s.tapMarkW = kIconTap.w;
    s.holdMarkW = kIconHold.w;
    s.sideIconW = kIconBack.w;
    s.sideWord = "back";
    HintPlan p = planHints(135, s);
    TEST_ASSERT_FALSE(p.twoRows);
    TEST_ASSERT_EQUAL(theme::kHintH, p.height);
    TEST_ASSERT_TRUE(p.sideWord);
    TEST_ASSERT_EQUAL(135 - theme::kGutter - (kIconBack.w + 3 + 4 * 6), p.sideX);
    // "• mover ▬ abrir ◂ atrás" does not fit 135 px: the icon alone.
    s.tap = "mover";
    s.hold = "abrir";
    s.sideWord = "atrás";
    p = planHints(135, s);
    TEST_ASSERT_FALSE(p.twoRows);
    TEST_ASSERT_FALSE(p.sideWord);
    TEST_ASSERT_EQUAL(135 - theme::kGutter - kIconBack.w, p.sideX);
    TEST_ASSERT_TRUE(planHints(240, s).sideWord);
    // No front items: the word fits from the gutter on.
    HintSpec only;
    only.sideIconW = kIconBack.w;
    only.sideWord = "stop";
    TEST_ASSERT_TRUE(planHints(135, only).sideWord);
}

// The date stepper (main.cpp drawDate()): "• +1 day ▬ OK" and "◂ -1 day
// ▬ cancel" in English and Spanish. One row where both groups fit (landscape),
// else the side group on a second row; the side words are always drawn, and
// nothing overlaps or leaves the panel.
static HintSpec dateHints(bool es) {
    HintSpec s;
    s.tap = es ? "+1 d\xC3\xAD" "a" : "+1 day";
    s.hold = "OK";
    s.tapMarkW = kIconTap.w;
    s.holdMarkW = kIconHold.w;
    s.sideIconW = kIconBack.w;
    s.sideWord = es ? "-1 d\xC3\xAD" "a" : "-1 day";
    s.sideHold = es ? "cancelar" : "cancel";
    return s;
}

void test_hint_plan_date_stepper() {
    for (bool es : {false, true}) {
        const HintSpec s = dateHints(es);
        const int frontEnd = hintFrontEnd(s.tap, s.hold, s.tapMarkW, s.holdMarkW);
        for (int w : {135, 170, 240, 320}) {
            const HintPlan p = planHints(w, s);
            TEST_ASSERT_TRUE(p.sideWord);
            // The side tap item, 6 px, the side hold item, ending at the gutter.
            TEST_ASSERT_TRUE(p.sideX >= theme::kGutter);
            TEST_ASSERT_EQUAL(p.sideX + hintItemW(s.sideIconW, s.sideWord) + kHintItemGap,
                              p.sideHoldX);
            TEST_ASSERT_EQUAL(w - theme::kGutter, p.sideHoldX + hintItemW(s.holdMarkW, s.sideHold));
            // The front row always fits.
            TEST_ASSERT_TRUE(frontEnd <= w - theme::kGutter);
            // On one row, 6 px between the groups.
            if (!p.twoRows) TEST_ASSERT_TRUE(p.sideX >= frontEnd + kHintItemGap);
            TEST_ASSERT_EQUAL(p.twoRows ? 2 * theme::kHintH : theme::kHintH, p.height);
        }
        // Portrait takes two rows, landscape one.
        TEST_ASSERT_TRUE(planHints(135, s).twoRows);
        TEST_ASSERT_TRUE(planHints(170, s).twoRows);
        TEST_ASSERT_FALSE(planHints(240, s).twoRows);
        TEST_ASSERT_FALSE(planHints(320, s).twoRows);
    }
    // The widths docs/ux-refresh.md §2.4 gives: one row from 189 px (English)
    // and 201 px (Spanish).
    TEST_ASSERT_FALSE(planHints(189, dateHints(false)).twoRows);
    TEST_ASSERT_TRUE(planHints(188, dateHints(false)).twoRows);
    TEST_ASSERT_FALSE(planHints(201, dateHints(true)).twoRows);
    TEST_ASSERT_TRUE(planHints(200, dateHints(true)).twoRows);
}

// Only when even a row of its own is too narrow does the side tap lose its
// word; the side hold item and the icon stay. No side tap: the hold alone.
void test_hint_plan_side_hold_fallbacks() {
    const HintSpec s = dateHints(true);
    const HintPlan p = planHints(100, s);
    TEST_ASSERT_TRUE(p.twoRows);
    TEST_ASSERT_FALSE(p.sideWord);
    TEST_ASSERT_EQUAL(100 - theme::kGutter - hintItemW(s.holdMarkW, s.sideHold), p.sideHoldX);
    TEST_ASSERT_EQUAL(p.sideHoldX - kHintItemGap - s.sideIconW, p.sideX);

    HintSpec hold = dateHints(false);
    hold.sideIconW = 0;
    const HintPlan q = planHints(240, hold);
    TEST_ASSERT_FALSE(q.twoRows);
    TEST_ASSERT_EQUAL(240 - theme::kGutter - hintItemW(hold.holdMarkW, hold.sideHold), q.sideHoldX);
}

void test_caption_chars_counts_code_points() {
    TEST_ASSERT_EQUAL(0, captionChars(nullptr));
    TEST_ASSERT_EQUAL(9, captionChars("GOOD \xC2\xB7 4d"));
    TEST_ASSERT_EQUAL(21, captionChars("\xC2\xBFqu\xC3\xA9 kanji significa?"));
}

// ---------------------------------------------------------------- keyboard bars
// The Cardputer ADV's bars (main.cpp boardHints(), 240 px landscape only):
// every screen's words in English and Spanish, copied from UiStrings, with
// their keycaps. Each must fit one row with the side word shown.
struct KeyBar {
    const char* tap;
    const KeyCap* tapKey;
    const char* hold;
    const KeyCap* side;
    const char* sideWord;
    const char* sideHold;
};

static void checkKeyBar(const KeyBar& b) {
    const HintSpec s =
        keyHintSpec(b.tap, b.tapKey ? *b.tapKey : kKeyMove, b.hold, b.side, b.sideWord, b.sideHold);
    const HintPlan p = planHints(240, s);
    TEST_ASSERT_FALSE(p.twoRows);
    TEST_ASSERT_EQUAL(theme::kHintH, p.height);
    if (b.side && b.sideWord) TEST_ASSERT_TRUE(p.sideWord);
    const int frontEnd = hintFrontEnd(s.tap, s.hold, s.tapMarkW, s.holdMarkW);
    if (b.side) TEST_ASSERT_TRUE(p.sideX >= frontEnd + kHintItemGap);
    if (b.sideHold)
        TEST_ASSERT_EQUAL(240 - theme::kGutter,
                          p.sideHoldX + hintItemW(keyCapW(kKeyEsc), s.sideHold));
}

void test_keycap_widths() {
    TEST_ASSERT_EQUAL(15, keyCapW(kKeyMove));
    TEST_ASSERT_EQUAL(11, keyCapW(kKeyEnter));
    TEST_ASSERT_EQUAL(11, keyCapW(kKeyRight));
    TEST_ASSERT_EQUAL(11, keyCapW(kKeyLeft));
    TEST_ASSERT_EQUAL(22, keyCapW(kKeyEsc));
    TEST_ASSERT_EQUAL(22, keyCapW(kKeyMenu));
    // Every icon fits the keycap's height with a pixel to spare.
    for (const KeyCap* k : {&kKeyMove, &kKeyEnter, &kKeyRight, &kKeyLeft})
        TEST_ASSERT_TRUE(k->icon->h <= kKeyCapH - 2);
}

void test_keyboard_bars_fit_one_row() {
    const KeyCap* esc = &kKeyEsc;
    const KeyCap* menu = &kKeyMenu;
    const KeyCap* move = &kKeyMove;
    const KeyCap* right = &kKeyRight;
    const KeyCap* enter = &kKeyEnter;
    const KeyBar bars[] = {
        // quiz: question, after a correct / a wrong answer; detail card; done
        {"move", move, "answer", menu, "settings", nullptr},
        {"mover", move, "responder", menu, "ajustes", nullptr},
        {"next", enter, nullptr, menu, "settings", nullptr},
        {"siguiente", enter, nullptr, menu, "ajustes", nullptr},
        {"details", enter, nullptr, menu, "settings", nullptr},
        {"detalles", enter, nullptr, menu, "ajustes", nullptr},
        {nullptr, nullptr, "study more", menu, "settings", nullptr},
        {nullptr, nullptr, "estudiar más", menu, "ajustes", nullptr},
        // menus and pickers
        {"move", move, "open", esc, "back", nullptr},
        {"mover", move, "abrir", esc, "atrás", nullptr},
        {"move", move, "choose", esc, "back", nullptr},
        {"mover", move, "elegir", esc, "atrás", nullptr},
        // date stepper: right +1 day, Enter OK, left -1 day, esc cancel
        {"+1 day", right, "OK", &kKeyLeft, "-1 day", "cancel"},
        {"+1 d\xC3\xAD" "a", right, "OK", &kKeyLeft, "-1 d\xC3\xAD" "a", "cancelar"},
        {"by hand", enter, nullptr, nullptr, nullptr, nullptr},
        {"a mano", enter, nullptr, nullptr, nullptr, nullptr},
        // About, statistics, network test
        {"more", right, nullptr, esc, "back", nullptr},
        {"m\xC3\xA1s", right, nullptr, esc, "atr\xC3\xA1s", nullptr},
        {nullptr, nullptr, "test again", esc, "back", nullptr},
        {nullptr, nullptr, "repetir", esc, "atr\xC3\xA1s", nullptr},
        // Wi-Fi portal: setup mode, failed join, the boot-time screen, a download
        {"other QR", right, "get decks", esc, "stop", nullptr},
        {"otro QR", right, "descargar", esc, "parar", nullptr},
        {"retry", right, "setup", esc, "stop", nullptr},
        {"repetir", right, "config.", esc, "parar", nullptr},
        {"other QR", right, nullptr, esc, "study", nullptr},
        {"otro QR", right, nullptr, esc, "estudiar", nullptr},
        {nullptr, nullptr, nullptr, esc, "cancel", nullptr},
        {nullptr, nullptr, nullptr, esc, "cancelar", nullptr},
        // Get decks: the list (download, update, study), an error
        {"move", move, "get", esc, "back", nullptr},
        {"mover", move, "actualizar", esc, "atr\xC3\xA1s", nullptr},
        {"mover", move, "estudiar", esc, "atr\xC3\xA1s", nullptr},
        {nullptr, nullptr, "retry", esc, "back", nullptr},
        {nullptr, nullptr, "repetir", esc, "atr\xC3\xA1s", nullptr},
        // Delete progress: the confirmation; the first-boot language picker
        {nullptr, nullptr, "delete", esc, "cancel", nullptr},
        {nullptr, nullptr, "borrar", esc, "cancelar", nullptr},
        {"other", move, "OK", nullptr, nullptr, nullptr},
        {"otro", move, "OK", nullptr, nullptr, nullptr},
        // the intro card: Enter goes on, left says known
        {"next", enter, nullptr, &kKeyLeft, "known", nullptr},
        {"siguiente", enter, nullptr, &kKeyLeft, "la s\xC3\xA9", nullptr},
    };
    for (const KeyBar& b : bars) checkKeyBar(b);
}

// The keyboard's esc mark replaces the hold mark in the side group's width.
void test_side_hold_mark_of_its_own() {
    HintSpec s;
    s.sideIconW = 11;
    s.sideWord = "-1 day";
    s.sideHold = "cancel";
    s.holdMarkW = kIconHold.w;
    const HintPlan button = planHints(240, s);
    s.sideHoldMarkW = keyCapW(kKeyEsc);
    const HintPlan key = planHints(240, s);
    TEST_ASSERT_EQUAL(button.sideHoldX - (keyCapW(kKeyEsc) - kIconHold.w), key.sideHoldX);
}

// ---------------------------------------------------------------- intro card
// The widths the intro is laid out with (app/Intro.cpp adv()): the UI font's
// Label face for Latin, 18 px for kana and kanji, which come from the pack's
// text set at text_px 18 (full width in Noto Sans JP).
static int uiAdv(uint32_t cp) {
    const UiGlyph* g = uiGlyph(uifont::kLabel, cp);
    return g ? g->advance : 18;
}
static int uiW(const char* s) { return textWidthBy(s, uiAdv); }
static int lh() { return uifont::kLabel.pitch; }
constexpr int kLargePx = 96;  // the builder's --large-px and --small-px
constexpr int kSmallPx = 40;
struct IntroPanel {
    int w, h;
};
// StickS3 portrait and landscape (the Cardputer ADV's panel too), T-Display-S3
// portrait and landscape.
constexpr IntroPanel kIntroPanels[] = {{135, 240}, {240, 135}, {170, 320}, {320, 170}};
static IntroLayout introAt(const IntroPanel& p) {
    return introLayout(p.w, p.h, kLargePx, kSmallPx, lh());
}
static int readingTag() { return std::max(pillWidthFor("ON", 0), pillWidthFor("KUN", 0)); }

static Card introCardOf(const char* meanings, const char* on, const char* kun) {
    Card c{};
    c.meanings = meanings;
    c.onyomi = on;
    c.kunyomi = kun;
    return c;
}

// The widest strings of the N5 decks (English and Spanish, 2026-10-23, with
// --example-words): forms, readings and glosses, paired worst with worst.
static const IntroWordText kWideWords[] = {
    {"\xE5\xA4\x96\xE5\x9B\xBD\xE4\xBA\xBA", "\xE3\x81\x8C\xE3\x81\x84\xE3\x81\x93\xE3\x81\x8F"
                                            "\xE3\x81\x98\xE3\x82\x93",
     "yen (currency of Japan)"},  // 外国人 がいこくじん
    {"\xE5\xA4\xA7\xE4\xB8\x88\xE5\xA4\xAB", "\xE3\x81\xA0\xE3\x81\x84\xE3\x81\x98\xE3\x82\x87"
                                            "\xE3\x81\x86\xE3\x81\xB6",
     "Chiba (city, prefecture)"},  // 大丈夫 だいじょうぶ
    {"\xE5\x8C\x97\xE6\xB5\xB7\xE9\x81\x93", "\xE3\x81\xBB\xE3\x81\xA3\xE3\x81\x8B\xE3\x81\x84"
                                            "\xE3\x81\xA9\xE3\x81\x86",
     "long (distance, length)"},  // 北海道 ほっかいどう
    {"\xE7\x94\x9F\xE3\x81\xBE\xE3\x82\x8C\xE3\x82\x8B",
     "\xE3\x81\x86\xE3\x81\xBE\xE3\x82\x8C\xE3\x82\x8B", "presidente de empresa"},  // 生まれる
    {"\xE5\x8F\x8E\xE5\x85\xA5", "\xE3\x81\x97\xE3\x82\x85\xE3\x81\x86\xE3\x81\xAB\xE3\x82\x85"
                                "\xE3\x81\x86",
     "este (punto cardinal)"},  // 収入 しゅうにゅう
};
// 休's words in the mockups: 休日 きゅうじつ "holiday", 休む やすむ "to rest".
static const IntroWordText kRestWords[] = {
    {"\xE4\xBC\x91\xE6\x97\xA5", "\xE3\x81\x8D\xE3\x82\x85\xE3\x81\x86\xE3\x81\x98\xE3\x81\xA4",
     "holiday"},
    {"\xE4\xBC\x91\xE3\x82\x80", "\xE3\x82\x84\xE3\x81\x99\xE3\x82\x80", "to rest"},
};
// 休 (rest): キュウ; やす.む、やす.まる、やす.める.
static Card restCard() {
    return introCardOf("rest; day off; retire", "\xE3\x82\xAD\xE3\x83\xA5\xE3\x82\xA6",
                       "\xE3\x82\x84\xE3\x81\x99.\xE3\x82\x80\xE3\x80\x81\xE3\x82\x84\xE3\x81\x99."
                       "\xE3\x81\xBE\xE3\x82\x8B\xE3\x80\x81\xE3\x82\x84\xE3\x81\x99.\xE3\x82\x81"
                       "\xE3\x82\x8B");
}

void test_intro_meanings_split() {
    IntroMeanings m = introMeanings("rest; day off; retire");
    TEST_ASSERT_EQUAL_STRING("rest", m.first.c_str());
    TEST_ASSERT_EQUAL_STRING("day off; retire", m.rest.c_str());
    m = introMeanings("water");
    TEST_ASSERT_EQUAL_STRING("water", m.first.c_str());
    TEST_ASSERT_EQUAL_STRING("", m.rest.c_str());
    m = introMeanings("");
    TEST_ASSERT_EQUAL_STRING("", m.first.c_str());
    m = introMeanings(nullptr);
    TEST_ASSERT_EQUAL_STRING("", m.first.c_str());
    TEST_ASSERT_EQUAL_STRING("", m.rest.c_str());
}

void test_intro_reading_rows() {
    const Card c = restCard();
    IntroReading r[2];
    // The 135 px portrait: 98 px after the pill. やすむ、やすまる、やすめる is
    // 234 px, so the others are a "+2" (the mockup's row).
    const int w = 135 - 2 * theme::kGutter - readingTag() - 5;
    TEST_ASSERT_EQUAL(98, w);
    TEST_ASSERT_EQUAL(2, introReadings(c, false, w, uiAdv, r));
    TEST_ASSERT_EQUAL(ReadingKind::On, r[0].kind);
    TEST_ASSERT_EQUAL_STRING("\xE3\x82\xAD\xE3\x83\xA5\xE3\x82\xA6", r[0].first.c_str());  // キュウ
    TEST_ASSERT_EQUAL_STRING("", r[0].rest.c_str());
    TEST_ASSERT_EQUAL(0, r[0].more);
    TEST_ASSERT_EQUAL(ReadingKind::Kun, r[1].kind);
    TEST_ASSERT_EQUAL_STRING("\xE3\x82\x84\xE3\x81\x99\xE3\x82\x80", r[1].first.c_str());  // やすむ
    TEST_ASSERT_EQUAL_STRING("", r[1].rest.c_str());
    TEST_ASSERT_EQUAL(2, r[1].more);
    // Room for all of them: the others, dim, after "、".
    introReadings(c, false, 400, uiAdv, r);
    TEST_ASSERT_EQUAL_STRING("\xE3\x80\x81\xE3\x82\x84\xE3\x81\x99\xE3\x81\xBE\xE3\x82\x8B\xE3\x80"
                             "\x81\xE3\x82\x84\xE3\x81\x99\xE3\x82\x81\xE3\x82\x8B",
                             r[1].rest.c_str());  // 、やすまる、やすめる
    TEST_ASSERT_EQUAL(0, r[1].more);
    // On'yomi in hiragana (Settings > Study > On'yomi); kun'yomi as they are.
    introReadings(c, true, w, uiAdv, r);
    TEST_ASSERT_EQUAL_STRING("\xE3\x81\x8D\xE3\x82\x85\xE3\x81\x86", r[0].first.c_str());  // きゅう
    TEST_ASSERT_EQUAL_STRING("\xE3\x82\x84\xE3\x81\x99\xE3\x82\x80", r[1].first.c_str());
    // When even "+2" does not fit after やすむ (54 + 4 + 12 px), the reading
    // alone.
    introReadings(c, false, 69, uiAdv, r);
    TEST_ASSERT_EQUAL(0, r[1].more);
    introReadings(c, false, 70, uiAdv, r);
    TEST_ASSERT_EQUAL(2, r[1].more);
    // One kind: one row, the others after "、" when they fit; none: no row.
    TEST_ASSERT_EQUAL(1, introReadings(introCardOf("m", "\xE3\x82\xAD\xE3\x80\x81\xE3\x82\xB1", ""),
                                       false, w, uiAdv, r));  // キ、ケ
    TEST_ASSERT_EQUAL(ReadingKind::On, r[0].kind);
    TEST_ASSERT_EQUAL_STRING("\xE3\x82\xAD", r[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("\xE3\x80\x81\xE3\x82\xB1", r[0].rest.c_str());
    TEST_ASSERT_EQUAL(0, introReadings(introCardOf("m", "", ""), false, w, uiAdv, r));
}

// The widest first readings of the N5 decks with their counts (おおきい +2,
// うまれる +16, ジョウ +2), and 休's row, show their "+N" on every panel.
void test_intro_reading_rows_fit_every_panel() {
    const char* kWideKun = "\xE3\x81\x86\xE3\x81\xBE.\xE3\x82\x8C\xE3\x82\x8B";  // うま.れる
    std::string many = kWideKun;
    for (int i = 0; i < 16; ++i) many += "\xE3\x80\x81\xE3\x81\x84.\xE3\x81\x8D\xE3\x82\x8B";
    const Card wide = introCardOf("m", "\xE3\x82\xB8\xE3\x83\xA7\xE3\x82\xA6\xE3\x80\x81\xE3\x82"
                                       "\xB7\xE3\x83\xA7\xE3\x82\xA6\xE3\x80\x81\xE3\x82\xB7\xE3"
                                       "\x83\xA3\xE3\x83\xB3",  // ジョウ、ショウ、シャン
                                  many.c_str());
    for (const IntroPanel& p : kIntroPanels) {
        const IntroLayout l = introAt(p);
        const int w = l.colW - readingTag() - 5;
        IntroReading r[2];
        TEST_ASSERT_EQUAL(2, introReadings(wide, false, w, uiAdv, r));
        TEST_ASSERT_TRUE(r[0].more == 2 || !r[0].rest.empty());
        TEST_ASSERT_EQUAL(16, r[1].more);
        TEST_ASSERT_TRUE(uiW(r[1].first.c_str()) + kIntroMoreGap + 3 * kCaptionCharW <= w);
        introReadings(restCard(), false, w, uiAdv, r);
        TEST_ASSERT_EQUAL(2, r[1].more);
    }
}

// The header: the NEW / NUEVA pill and the worded stroke counter up to 29
// strokes in both languages on every panel (79 and 67 px in the 135 px
// portrait, docs/learning-mode.md §3.2); the counter without stroke data and
// page 2's "2/2" too.
void test_intro_header_fits() {
    TEST_ASSERT_EQUAL(79, introCounterRoom(135, "NEW", true));
    TEST_ASSERT_EQUAL(67, introCounterRoom(135, "NUEVA", true));
    for (int w : {135, 170, 240, 320}) {
        const int en = introCounterRoom(w, "NEW", true);
        const int es = introCounterRoom(w, "NUEVA", true);
        TEST_ASSERT_TRUE(strokeCounterWorded("stroke %u/%u", 29, en));
        TEST_ASSERT_TRUE(strokeCounterWorded("trazo %u/%u", 29, es));
        TEST_ASSERT_TRUE(captionChars("strokes 29") * kCaptionCharW <= en);
        TEST_ASSERT_TRUE(captionChars("trazos 29") * kCaptionCharW <= es);
        TEST_ASSERT_TRUE(captionChars("2/2") * kCaptionCharW <= es);
    }
    // Any two-digit count keeps the word (Jōyō's most is 29); three digits
    // would go bare in the 135 px portrait.
    TEST_ASSERT_TRUE(strokeCounterWorded("trazo %u/%u", 99, introCounterRoom(135, "NUEVA", true)));
    TEST_ASSERT_FALSE(strokeCounterWorded("trazo %u/%u", 100, introCounterRoom(135, "NUEVA", true)));
}

// The hint bar, always shown: "• next ✓ known" and "• siguiente ✓ la sé" on
// one row with the side word on every panel; in the 135 px portrait the
// Spanish front items end at x 66 and the side group starts at 89.
void test_intro_hints_fit() {
    const char* pairs[][2] = {{"next", "known"}, {"siguiente", "la s\xC3\xA9"}};
    for (int w : {135, 170, 240, 320})
        for (const auto& pr : pairs) {
            HintSpec s;
            s.tap = pr[0];
            s.tapMarkW = kIconTap.w;
            s.sideIconW = kIconCheck.w;
            s.sideWord = pr[1];
            const HintPlan p = planHints(w, s);
            TEST_ASSERT_FALSE(p.twoRows);
            TEST_ASSERT_TRUE_MESSAGE(p.sideWord, pr[1]);
        }
    HintSpec es;
    es.tap = "siguiente";
    es.tapMarkW = kIconTap.w;
    es.sideIconW = kIconCheck.w;
    es.sideWord = "la s\xC3\xA9";
    TEST_ASSERT_EQUAL(66, hintFrontEnd(es.tap, nullptr, kIconTap.w, 0));
    TEST_ASSERT_EQUAL(89, planHints(135, es).sideX);
}

// Where the parts go on each panel: the fullest page 1 (two meaning lines,
// two reading rows) above the hint bar, page 2's head, rule and words inside
// the panel with room for two words -- in portrait each of two lines and a
// gloss of two, in landscape each of one line (side by side) and a gloss of
// one -- and the words inline only on the T-Display-S3's portrait.
void test_intro_layout_panels() {
    for (const IntroPanel& p : kIntroPanels) {
        const IntroLayout l = introAt(p);
        TEST_ASSERT_TRUE(l.colTop + 4 * (lh() + 2) <= l.bottom + 2);
        TEST_ASSERT_TRUE(l.colX >= theme::kGutter && l.colX + l.colW <= p.w - theme::kGutter);
        TEST_ASSERT_TRUE(l.bottom < p.h - theme::kHintH && l.pageBottom <= p.h - theme::kHintH);
        TEST_ASSERT_TRUE(l.headY >= theme::kHeaderH);
        TEST_ASSERT_TRUE(l.ruleY >= l.headY + l.headH);
        TEST_ASSERT_TRUE(l.wordsY > l.ruleY);
        TEST_ASSERT_EQUAL(p.w - 2 * theme::kGutter, l.wordsW);
        const int perWord = l.wordsSideBySide ? 2 * lh() : 4 * lh();
        TEST_ASSERT_TRUE(l.pageBottom - l.wordsY >= 2 * perWord + kIntroWordsGap);
        TEST_ASSERT_EQUAL(p.w < p.h, l.headKanji);
        TEST_ASSERT_EQUAL(p.w > p.h, l.wordsSideBySide);
        if (l.headKanji) TEST_ASSERT_TRUE(theme::kGutter + kSmallPx + 4 + 60 <= p.w);
    }
    TEST_ASSERT_FALSE(introAt({135, 240}).wordsInline);
    TEST_ASSERT_FALSE(introAt({240, 135}).wordsInline);
    TEST_ASSERT_TRUE(introAt({170, 320}).wordsInline);
    TEST_ASSERT_FALSE(introAt({320, 170}).wordsInline);
    // The design's figures: the 135 px portrait's column from y 120 to 206.
    const IntroLayout s = introAt({135, 240});
    TEST_ASSERT_EQUAL(120, s.colTop);
    TEST_ASSERT_EQUAL(206, s.colTop + 4 * (lh() + 2) - 2);
}

// The widest words and glosses of the N5 decks fit page 2 on every panel,
// any two of them, none shortened: in portrait the form and the reading each
// whole on its line and the gloss in at most two; in landscape the form and
// the reading side by side and the gloss on one line.
void test_intro_words_fit_page_two() {
    for (const IntroPanel& p : kIntroPanels) {
        const IntroLayout l = introAt(p);
        const int h = l.pageBottom - l.wordsY;
        for (const IntroWordText& a : kWideWords) {
            TEST_ASSERT_TRUE(uiW(a.word) <= l.wordsW);
            TEST_ASSERT_TRUE(uiW(a.reading) <= l.wordsW);
            TEST_ASSERT_TRUE(wrapLines(a.gloss, l.wordsW, 8, uiAdv) <=
                             (l.wordsSideBySide ? 1 : kIntroGlossLines));
            for (const IntroWordText& b : kWideWords) {
                const IntroWordText pair[2] = {a, b};
                IntroWordLines lines[2];
                TEST_ASSERT_EQUAL(2, planIntroWords(pair, 2, l.wordsW, h, lh(), l.wordsSideBySide,
                                                    uiAdv, lines));
                for (int i = 0; i < 2; ++i) {
                    TEST_ASSERT_EQUAL(l.wordsSideBySide, lines[i].oneLine);
                    TEST_ASSERT_EQUAL(wrapLines(pair[i].gloss, l.wordsW, 2, uiAdv),
                                      lines[i].glossLines);
                }
            }
        }
    }
    // The 24-character limit the builder keeps English glosses to: two lines
    // in the 135 px portrait, one in the landscape (240 px).
    TEST_ASSERT_EQUAL(24, static_cast<int>(std::strlen("Chiba (city, prefecture)")));
    TEST_ASSERT_EQUAL(2, wrapLines("Chiba (city, prefecture)", 127, 8, uiAdv));
    TEST_ASSERT_EQUAL(1, wrapLines("Chiba (city, prefecture)", 232, 8, uiAdv));
}

// The planner's fallbacks: a gloss loses its second line, the last word's
// first, then the last word goes; a word without a gloss is form and reading
// only; side by side only when both fit.
void test_intro_words_plan_fallbacks() {
    const IntroWordText two[2] = {kWideWords[0], kWideWords[1]};  // both glosses 2 lines at 127
    IntroWordLines lines[2];
    TEST_ASSERT_EQUAL(2, planIntroWords(two, 2, 127, 162, 20, false, uiAdv, lines));
    TEST_ASSERT_EQUAL(2, lines[0].glossLines);
    TEST_ASSERT_EQUAL(2, lines[1].glossLines);
    TEST_ASSERT_EQUAL(80, lines[0].height);
    TEST_ASSERT_EQUAL(2, planIntroWords(two, 2, 127, 161, 20, false, uiAdv, lines));
    TEST_ASSERT_EQUAL(2, lines[0].glossLines);
    TEST_ASSERT_EQUAL(1, lines[1].glossLines);
    TEST_ASSERT_EQUAL(2, planIntroWords(two, 2, 127, 122, 20, false, uiAdv, lines));
    TEST_ASSERT_EQUAL(1, lines[0].glossLines);
    TEST_ASSERT_EQUAL(1, lines[1].glossLines);
    TEST_ASSERT_EQUAL(1, planIntroWords(two, 2, 127, 121, 20, false, uiAdv, lines));
    TEST_ASSERT_EQUAL(0, planIntroWords(two, 2, 127, 59, 20, false, uiAdv, lines));
    const IntroWordText bare = {kRestWords[0].word, kRestWords[0].reading, ""};
    TEST_ASSERT_EQUAL(1, planIntroWords(&bare, 1, 127, 200, 20, false, uiAdv, lines));
    TEST_ASSERT_EQUAL(0, lines[0].glossLines);
    TEST_ASSERT_EQUAL(40, lines[0].height);
    // 休日 きゅうじつ (132 px) side by side in 162 px, not in 127.
    TEST_ASSERT_EQUAL(1, planIntroWords(kRestWords, 1, 162, 200, 20, true, uiAdv, lines));
    TEST_ASSERT_TRUE(lines[0].oneLine);
    TEST_ASSERT_EQUAL(1, planIntroWords(kRestWords, 1, 127, 200, 20, true, uiAdv, lines));
    TEST_ASSERT_FALSE(lines[0].oneLine);
}

// Page 1 on every panel: 休 (two meanings, both kinds) keeps every row above
// the hint bar; its words go under the readings on the T-Display-S3's
// portrait (as the mockup) and on page 2 elsewhere; the widest words would
// lose one under the readings there, so they get page 2 too; a v2 pack (no
// words) never has a page 2.
void test_intro_card_plan() {
    const Card c = restCard();
    for (const IntroPanel& p : kIntroPanels) {
        const IntroLayout l = introAt(p);
        IntroCardPlan plan = planIntroCard(l, c, kRestWords, 2, false, readingTag(), lh(), uiAdv);
        TEST_ASSERT_EQUAL(1, plan.firstLines);
        TEST_ASSERT_TRUE(plan.restLine);
        TEST_ASSERT_EQUAL(2, plan.rows);
        TEST_ASSERT_TRUE(plan.wordsY <= l.bottom + 4);
        TEST_ASSERT_EQUAL(!l.wordsInline, plan.wordsPage);
        TEST_ASSERT_EQUAL(l.wordsInline ? 2 : 0, plan.words);
        if (plan.words) {
            TEST_ASSERT_TRUE(plan.wordLines[0].oneLine);
            TEST_ASSERT_TRUE(plan.wordsY + plan.wordLines[0].height + kIntroWordsGap +
                                 plan.wordLines[1].height <=
                             l.pageBottom);
        }
        const IntroWordText wide[2] = {kWideWords[0], kWideWords[1]};
        plan = planIntroCard(l, c, wide, 2, false, readingTag(), lh(), uiAdv);
        TEST_ASSERT_TRUE(plan.wordsPage);
        TEST_ASSERT_EQUAL(0, plan.words);
        plan = planIntroCard(l, c, nullptr, 0, false, readingTag(), lh(), uiAdv);
        TEST_ASSERT_FALSE(plan.wordsPage);
        TEST_ASSERT_EQUAL(0, plan.words);
        // One meaning: it may take two lines ("water (esp. cool or cold)").
        plan = planIntroCard(l, introCardOf("water (esp. cool or cold) and more", "", ""), nullptr,
                             0, false, readingTag(), lh(), uiAdv);
        TEST_ASSERT_TRUE(plan.firstLines >= 1 && plan.firstLines <= 2);
        TEST_ASSERT_FALSE(plan.restLine);
        TEST_ASSERT_EQUAL(0, plan.rows);
    }
}

// wrapLines() breaks as Renderer::wrap() does: at a space, after any CJK
// character, else where the line is full; capped at maxLines.
void test_wrap_lines_like_the_renderer() {
    TEST_ASSERT_EQUAL(0, wrapLines("", 100, 2, uiAdv));
    TEST_ASSERT_EQUAL(0, wrapLines(nullptr, 100, 2, uiAdv));
    TEST_ASSERT_EQUAL(1, wrapLines("holiday", 127, 2, uiAdv));
    TEST_ASSERT_EQUAL(2, wrapLines("yen (currency of Japan)", 127, 8, uiAdv));
    TEST_ASSERT_EQUAL(1, wrapLines("yen (currency of Japan)", 127, 1, uiAdv));  // capped
    TEST_ASSERT_EQUAL(1, wrapLines("yen (currency of Japan)", 400, 8, uiAdv));
    // Kana at 18 px in 40 px: two a line (きゅうじつ: 3 lines).
    TEST_ASSERT_EQUAL(3, wrapLines("\xE3\x81\x8D\xE3\x82\x85\xE3\x81\x86\xE3\x81\x98\xE3\x81\xA4",
                                   40, 8, uiAdv));
    // A word longer than the line breaks where it is full.
    TEST_ASSERT_TRUE(uiW("electricidad") > 60);
    TEST_ASSERT_EQUAL(2, wrapLines("electricidad", 60, 8, uiAdv));
    // A line always takes at least one character, even when it is too wide.
    TEST_ASSERT_EQUAL(2, wrapLines("ab", 1, 8, uiAdv));
}

void test_reading_span() {
    const char* r = "\xE3\x81\xBE\xE3\x81\x84\xE3\x81\xAB\xE3\x81\xA1";  // まいにち
    TEST_ASSERT_TRUE(readingSpanValid(r, 6, 6));   // にち (毎日 for 日)
    TEST_ASSERT_TRUE(readingSpanValid(r, 0, 12));  // all of it
    TEST_ASSERT_FALSE(readingSpanValid(r, 0, 0));  // unknown
    TEST_ASSERT_FALSE(readingSpanValid(r, 6, 9));  // past the end
    TEST_ASSERT_FALSE(readingSpanValid(r, 4, 6));  // starts inside a character
    TEST_ASSERT_FALSE(readingSpanValid(r, 6, 4));  // ends inside one
    TEST_ASSERT_FALSE(readingSpanValid(nullptr, 0, 3));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_progress_counts_the_open_question);
    RUN_TEST(test_option_navigation_wraps_both_ways);
    RUN_TEST(test_grid_option_moves_by_row_and_column);
    RUN_TEST(test_hints_always_while_learning);
    RUN_TEST(test_hints_after_idle_on_open_question);
    RUN_TEST(test_hints_never_idle_on_a_revealed_answer);
    RUN_TEST(test_hold_fill_timing);
    RUN_TEST(test_auto_advance_fires_once_after_the_wait);
    RUN_TEST(test_auto_advance_waits_for_buttons_and_off);
    RUN_TEST(test_header_pill_room_matches_the_widget);
    RUN_TEST(test_mode_pills_fit_everywhere);
    RUN_TEST(test_result_pill_full_form_when_it_fits);
    RUN_TEST(test_result_pill_shortens_before_overflowing);
    RUN_TEST(test_result_pills_never_overflow);
    RUN_TEST(test_stroke_counter_word_once_per_card);
    RUN_TEST(test_detail_reading_rows_skip_a_missing_kind);
    RUN_TEST(test_quiz_hints_fit_portrait);
    RUN_TEST(test_hint_plan_one_side_action_unchanged);
    RUN_TEST(test_hint_plan_date_stepper);
    RUN_TEST(test_hint_plan_side_hold_fallbacks);
    RUN_TEST(test_caption_chars_counts_code_points);
    RUN_TEST(test_keycap_widths);
    RUN_TEST(test_keyboard_bars_fit_one_row);
    RUN_TEST(test_side_hold_mark_of_its_own);
    RUN_TEST(test_intro_meanings_split);
    RUN_TEST(test_intro_reading_rows);
    RUN_TEST(test_intro_reading_rows_fit_every_panel);
    RUN_TEST(test_intro_header_fits);
    RUN_TEST(test_intro_hints_fit);
    RUN_TEST(test_intro_layout_panels);
    RUN_TEST(test_intro_words_fit_page_two);
    RUN_TEST(test_intro_words_plan_fallbacks);
    RUN_TEST(test_intro_card_plan);
    RUN_TEST(test_wrap_lines_like_the_renderer);
    RUN_TEST(test_reading_span);
    return UNITY_END();
}
