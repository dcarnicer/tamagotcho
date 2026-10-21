#pragma once
// Keyboard input on boards with a keyboard (the Cardputer ADV; docs/decisions.md,
// 2026-10-21). The app has no text entry: it tells a handful of keys apart and
// turns each into one of five actions, which main.cpp maps to the current
// screen's own (what the front tap, the front hold or the side button does
// there). This header has which key a TCA8418 event is on the Cardputer ADV's
// wiring, the key -> action table and the arrows' auto-repeat. Pure C++, no
// Arduino (native-tested in test_keys); the TCA8418 itself is read in
// board/BoardCardputerAdv.cpp.
#include <cstdint>

namespace tgc {

// The keys the app tells apart. The arrows are the ; . , / keys (their Fn
// layer prints the arrows; with no text entry anywhere the plain keys are
// arrows too, so Fn does not matter), Esc is the ` key (Esc in its Fn layer),
// Backspace is "del", Tab is "tab". Every other key -- letters, digits, Fn,
// Shift, Ctrl, Opt, Alt, Space -- is Other: it wakes the screen and counts as
// activity, and does nothing else.
enum class Key : uint8_t { None, Up, Down, Left, Right, Enter, Esc, Backspace, Tab, Other };

// What a key asks for, the same on every screen.
enum class KeyAction : uint8_t {
    None,
    Next,     // down, right: what the front tap does (the next option, row, day)
    Prev,     // up, left: the previous option or day, a list's focus up
    Confirm,  // Enter: what the front hold does, at once
    Back,     // Esc, Backspace: the screen's way back or cancel
    Menu,     // Tab: the settings home
};

constexpr KeyAction keyAction(Key k) {
    return k == Key::Down || k == Key::Right  ? KeyAction::Next
           : k == Key::Up || k == Key::Left   ? KeyAction::Prev
           : k == Key::Enter                  ? KeyAction::Confirm
           : k == Key::Esc || k == Key::Backspace ? KeyAction::Back
           : k == Key::Tab                    ? KeyAction::Menu
                                              : KeyAction::None;
}

// The keys that repeat while held: the arrows.
constexpr bool keyRepeats(Key k) {
    return k == Key::Up || k == Key::Down || k == Key::Left || k == Key::Right;
}

// A key going down or up. Key::None going up stands for every key (the board
// lost events, e.g. its FIFO overflowed): nothing is held any more.
struct KeyEvent {
    Key key = Key::None;
    bool down = false;
};

// ---------------------------------------------------------------- Cardputer
// The keyboard as printed: 4 rows of 14 keys, row 0 at the top
// (` 1 2 3 4 5 6 7 8 9 0 - = del), row 1 (tab q ... \), row 2
// (fn shift a ... ' enter), row 3 (ctrl opt alt z ... , . / space).
constexpr int kCardputerCols = 14;
constexpr int kCardputerRows = 4;

constexpr Key cardputerKeyAt(int x, int y) {
    if (x < 0 || x >= kCardputerCols || y < 0 || y >= kCardputerRows) return Key::None;
    if (y == 0) return x == 0 ? Key::Esc : x == 13 ? Key::Backspace : Key::Other;
    if (y == 1) return x == 0 ? Key::Tab : Key::Other;
    if (y == 2) return x == 11 ? Key::Up : x == 13 ? Key::Enter : Key::Other;
    return x == 10 ? Key::Left : x == 11 ? Key::Down : x == 12 ? Key::Right : Key::Other;
}

// A TCA8418 KEY_EVENT_A byte read on the Cardputer ADV: bit 7 set = pressed,
// bits 0-6 the key number, 1 + row * 10 + column of the chip's matrix (7 rows
// x 8 columns there; 0 = the FIFO is empty). The matrix is wired to the grid
// above as M5Stack's M5Cardputer library maps it (MIT;
// src/utility/Keyboard/KeyboardReader/TCA8418.cpp, get_key_event_raw() and
// remap(), github.com/m5stack/M5Cardputer at f1392858): chip row r and column
// c are grid column 2r + (c > 3 ? 1 : 0) and grid row c % 4. False for an
// empty FIFO and for a number outside the matrix (a GPI event).
inline bool cardputerAdvKeyPos(uint8_t raw, int* x, int* y) {
    const int code = raw & 0x7F;
    if (code == 0 || code > 80) return false;
    const int r = (code - 1) / 10;
    const int c = (code - 1) % 10;
    if (r >= 7 || c >= 8) return false;
    *x = 2 * r + (c > 3 ? 1 : 0);
    *y = c % 4;
    return true;
}

// The event for `raw` (see above). False when it is no key of the grid.
inline bool cardputerAdvEvent(uint8_t raw, KeyEvent* out) {
    int x = 0, y = 0;
    if (!cardputerAdvKeyPos(raw, &x, &y)) return false;
    out->key = cardputerKeyAt(x, y);
    out->down = (raw & 0x80) != 0;
    return true;
}

// ---------------------------------------------------------------- repeat
// The arrows' auto-repeat: a held arrow acts again kKeyRepeatDelayMs after its
// press, then every kKeyRepeatRateMs, until it comes up or another key goes
// down (only the last key pressed repeats, as on a PC). main.cpp honours the
// repeats on the list screens and the date stepper only; Enter and Esc never
// repeat.
constexpr uint32_t kKeyRepeatDelayMs = 400;
constexpr uint32_t kKeyRepeatRateMs = 120;

class KeyRepeat {
public:
    // A key went down at `now` (the caller acts on the press itself).
    void down(Key k, uint32_t now) {
        held_ = keyRepeats(k) ? k : Key::None;
        next_ = now + kKeyRepeatDelayMs;
    }
    // A key came up; Key::None: every key.
    void up(Key k) {
        if (k == Key::None || k == held_) held_ = Key::None;
    }
    void cancel() { held_ = Key::None; }
    // The held arrow when a repeat is due at `now`, else Key::None. A late call
    // gets one repeat and the next one a full period later, never a burst.
    Key poll(uint32_t now) {
        if (held_ == Key::None || static_cast<int32_t>(now - next_) < 0) return Key::None;
        next_ = now + kKeyRepeatRateMs;
        return held_;
    }
    Key held() const { return held_; }

private:
    Key held_ = Key::None;
    uint32_t next_ = 0;
};

}  // namespace tgc
