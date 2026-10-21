// Keyboard input (ui/KeyInput.h): the Cardputer ADV's TCA8418 key numbers on
// the printed grid, the keys the app tells apart, their actions and the
// arrows' auto-repeat.
#include <unity.h>

#include "ui/KeyInput.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

// The key number of grid key (x, y): the inverse of the M5Cardputer remap.
static uint8_t codeAt(int x, int y) {
    const int r = x / 2;
    const int c = y + (x % 2 ? 4 : 0);
    return static_cast<uint8_t>(1 + r * 10 + c);
}

void test_every_matrix_key_lands_once_on_the_grid() {
    bool seen[kCardputerRows][kCardputerCols] = {};
    int keys = 0;
    for (int r = 0; r < 7; ++r)
        for (int c = 0; c < 8; ++c) {
            int x = -1, y = -1;
            TEST_ASSERT_TRUE(cardputerAdvKeyPos(static_cast<uint8_t>(1 + r * 10 + c), &x, &y));
            TEST_ASSERT_TRUE(x >= 0 && x < kCardputerCols && y >= 0 && y < kCardputerRows);
            TEST_ASSERT_FALSE(seen[y][x]);
            seen[y][x] = true;
            ++keys;
        }
    TEST_ASSERT_EQUAL(56, keys);
}

void test_key_numbers_outside_the_matrix_are_no_key() {
    int x = 0, y = 0;
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(0, &x, &y));     // empty FIFO
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(0x80, &x, &y));  // pressed, number 0
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(9, &x, &y));     // row 0, column 8
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(10, &x, &y));    // row 0, column 9
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(71, &x, &y));    // row 7
    TEST_ASSERT_FALSE(cardputerAdvKeyPos(97, &x, &y));    // a GPI event
    KeyEvent e;
    TEST_ASSERT_FALSE(cardputerAdvEvent(0, &e));
}

void test_the_app_keys_by_their_legends() {
    // From M5Cardputer's key value map: ` (Esc with Fn) and del on row 0, tab
    // on row 1, ; (up) and enter on row 2, , . / (left, down, right) on row 3.
    TEST_ASSERT_EQUAL(Key::Esc, cardputerKeyAt(0, 0));
    TEST_ASSERT_EQUAL(Key::Backspace, cardputerKeyAt(13, 0));
    TEST_ASSERT_EQUAL(Key::Tab, cardputerKeyAt(0, 1));
    TEST_ASSERT_EQUAL(Key::Up, cardputerKeyAt(11, 2));
    TEST_ASSERT_EQUAL(Key::Enter, cardputerKeyAt(13, 2));
    TEST_ASSERT_EQUAL(Key::Left, cardputerKeyAt(10, 3));
    TEST_ASSERT_EQUAL(Key::Down, cardputerKeyAt(11, 3));
    TEST_ASSERT_EQUAL(Key::Right, cardputerKeyAt(12, 3));
    // Letters, digits and modifiers are Other.
    TEST_ASSERT_EQUAL(Key::Other, cardputerKeyAt(1, 0));   // 1
    TEST_ASSERT_EQUAL(Key::Other, cardputerKeyAt(0, 2));   // fn
    TEST_ASSERT_EQUAL(Key::Other, cardputerKeyAt(1, 3));   // opt
    TEST_ASSERT_EQUAL(Key::Other, cardputerKeyAt(13, 3));  // space
    TEST_ASSERT_EQUAL(Key::None, cardputerKeyAt(14, 0));
    TEST_ASSERT_EQUAL(Key::None, cardputerKeyAt(0, 4));
    int others = 0;
    for (int y = 0; y < kCardputerRows; ++y)
        for (int x = 0; x < kCardputerCols; ++x) others += cardputerKeyAt(x, y) == Key::Other;
    TEST_ASSERT_EQUAL(56 - 8, others);
}

void test_events_carry_the_key_and_its_edge() {
    KeyEvent e;
    TEST_ASSERT_TRUE(cardputerAdvEvent(static_cast<uint8_t>(0x80 | codeAt(13, 2)), &e));
    TEST_ASSERT_EQUAL(Key::Enter, e.key);
    TEST_ASSERT_TRUE(e.down);
    TEST_ASSERT_TRUE(cardputerAdvEvent(codeAt(13, 2), &e));
    TEST_ASSERT_EQUAL(Key::Enter, e.key);
    TEST_ASSERT_FALSE(e.down);
    // The arrows' chip positions (row, column): ; = (5, 6), . = (5, 7),
    // , = (5, 3), / = (6, 3).
    TEST_ASSERT_TRUE(cardputerAdvEvent(0x80 | (1 + 5 * 10 + 6), &e));
    TEST_ASSERT_EQUAL(Key::Up, e.key);
    TEST_ASSERT_TRUE(cardputerAdvEvent(0x80 | (1 + 5 * 10 + 7), &e));
    TEST_ASSERT_EQUAL(Key::Down, e.key);
    TEST_ASSERT_TRUE(cardputerAdvEvent(0x80 | (1 + 5 * 10 + 3), &e));
    TEST_ASSERT_EQUAL(Key::Left, e.key);
    TEST_ASSERT_TRUE(cardputerAdvEvent(0x80 | (1 + 6 * 10 + 3), &e));
    TEST_ASSERT_EQUAL(Key::Right, e.key);
    // ` is the chip's first key.
    TEST_ASSERT_TRUE(cardputerAdvEvent(0x81, &e));
    TEST_ASSERT_EQUAL(Key::Esc, e.key);
}

void test_actions() {
    TEST_ASSERT_EQUAL(KeyAction::Next, keyAction(Key::Down));
    TEST_ASSERT_EQUAL(KeyAction::Next, keyAction(Key::Right));
    TEST_ASSERT_EQUAL(KeyAction::Prev, keyAction(Key::Up));
    TEST_ASSERT_EQUAL(KeyAction::Prev, keyAction(Key::Left));
    TEST_ASSERT_EQUAL(KeyAction::Confirm, keyAction(Key::Enter));
    TEST_ASSERT_EQUAL(KeyAction::Back, keyAction(Key::Esc));
    TEST_ASSERT_EQUAL(KeyAction::Back, keyAction(Key::Backspace));
    TEST_ASSERT_EQUAL(KeyAction::Menu, keyAction(Key::Tab));
    TEST_ASSERT_EQUAL(KeyAction::None, keyAction(Key::Other));
    TEST_ASSERT_EQUAL(KeyAction::None, keyAction(Key::None));
    TEST_ASSERT_TRUE(keyRepeats(Key::Up));
    TEST_ASSERT_TRUE(keyRepeats(Key::Right));
    TEST_ASSERT_FALSE(keyRepeats(Key::Enter));
    TEST_ASSERT_FALSE(keyRepeats(Key::Esc));
    TEST_ASSERT_FALSE(keyRepeats(Key::Tab));
}

void test_repeat_after_the_delay_then_at_the_rate() {
    KeyRepeat r;
    r.down(Key::Down, 1000);
    TEST_ASSERT_EQUAL(Key::None, r.poll(1000));
    TEST_ASSERT_EQUAL(Key::None, r.poll(1000 + kKeyRepeatDelayMs - 1));
    TEST_ASSERT_EQUAL(Key::Down, r.poll(1000 + kKeyRepeatDelayMs));
    TEST_ASSERT_EQUAL(Key::None, r.poll(1000 + kKeyRepeatDelayMs + 1));
    const uint32_t t = 1000 + kKeyRepeatDelayMs + kKeyRepeatRateMs;
    TEST_ASSERT_EQUAL(Key::None, r.poll(t - 1));
    TEST_ASSERT_EQUAL(Key::Down, r.poll(t));
    r.up(Key::Down);
    TEST_ASSERT_EQUAL(Key::None, r.poll(t + 10000));
}

void test_a_late_poll_never_bursts() {
    KeyRepeat r;
    r.down(Key::Up, 0);
    // A slow loop turn: one repeat, then a whole period to the next.
    TEST_ASSERT_EQUAL(Key::Up, r.poll(2000));
    TEST_ASSERT_EQUAL(Key::None, r.poll(2000 + kKeyRepeatRateMs - 1));
    TEST_ASSERT_EQUAL(Key::Up, r.poll(2000 + kKeyRepeatRateMs));
}

void test_only_arrows_repeat_and_the_last_key_wins() {
    KeyRepeat r;
    r.down(Key::Enter, 0);
    TEST_ASSERT_EQUAL(Key::None, r.poll(5000));
    r.down(Key::Esc, 0);
    TEST_ASSERT_EQUAL(Key::None, r.poll(5000));
    // An arrow held, then another key: the arrow stops.
    r.down(Key::Left, 0);
    r.down(Key::Other, 100);
    TEST_ASSERT_EQUAL(Key::None, r.poll(5000));
    // A second arrow takes over; the first one's release does not stop it.
    r.down(Key::Up, 0);
    r.down(Key::Down, 200);
    r.up(Key::Up);
    TEST_ASSERT_EQUAL(Key::Down, r.poll(200 + kKeyRepeatDelayMs));
    // Every key up (lost events), and cancel().
    r.up(Key::None);
    TEST_ASSERT_EQUAL(Key::None, r.poll(9000));
    r.down(Key::Right, 0);
    r.cancel();
    TEST_ASSERT_EQUAL(Key::None, r.poll(9000));
    TEST_ASSERT_EQUAL(Key::None, r.held());
}

void test_repeat_across_the_millis_wrap() {
    KeyRepeat r;
    const uint32_t t0 = 0xFFFFFF00u;
    r.down(Key::Down, t0);
    TEST_ASSERT_EQUAL(Key::None, r.poll(t0 + 100));
    TEST_ASSERT_EQUAL(Key::Down, r.poll(t0 + kKeyRepeatDelayMs));  // wrapped past 0
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_matrix_key_lands_once_on_the_grid);
    RUN_TEST(test_key_numbers_outside_the_matrix_are_no_key);
    RUN_TEST(test_the_app_keys_by_their_legends);
    RUN_TEST(test_events_carry_the_key_and_its_edge);
    RUN_TEST(test_actions);
    RUN_TEST(test_repeat_after_the_delay_then_at_the_rate);
    RUN_TEST(test_a_late_poll_never_bursts);
    RUN_TEST(test_only_arrows_repeat_and_the_last_key_wins);
    RUN_TEST(test_repeat_across_the_millis_wrap);
    return UNITY_END();
}
