// Native tests for the quiz screens' pure logic (ui/QuizUi.h): session
// progress, option navigation, the hint bar policy, hold progress, auto-advance
// and the pixel budgets of the header pills and hint bars in English and
// Spanish (the strings are copies of main.cpp's UiStrings; keep them in step).
#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/QuizUi.h"

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

void test_caption_chars_counts_code_points() {
    TEST_ASSERT_EQUAL(0, captionChars(nullptr));
    TEST_ASSERT_EQUAL(9, captionChars("GOOD \xC2\xB7 4d"));
    TEST_ASSERT_EQUAL(21, captionChars("\xC2\xBFqu\xC3\xA9 kanji significa?"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_progress_counts_the_open_question);
    RUN_TEST(test_option_navigation_wraps_both_ways);
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
    RUN_TEST(test_quiz_hints_fit_portrait);
    RUN_TEST(test_caption_chars_counts_code_points);
    return UNITY_END();
}
