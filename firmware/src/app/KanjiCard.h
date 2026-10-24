#pragma once
// The parts the detail card and the intro card share (docs/learning-mode.md
// §3.10): the kanji box with the stroke-order animation, the header's stroke
// counter and the reading rows. They are keyed to a card index, not to the
// question on screen: the intro has no question.
#include <cstddef>
#include <cstdint>

#include "board/Board.h"
#include "quiz/Quiz.h"

namespace tgc {
namespace app {

// The stroke-order animation of one card: the strokes written so far in
// `text`, the current one in `warn`, the rest as a `ghost`, so the final shape
// shows throughout; kStrokeMs a stroke, kStrokePauseMs on the last, then
// again. Only while the pack carries stroke data for the card
// (docs/pack-format.md); otherwise the kanji is the static set-0 glyph.
struct StrokeAnim {
    bool on = false;
    uint32_t cardIndex = 0;
    uint16_t strokes = 0;
    int frame = 0;      // the stroke being written, 0-based
    uint32_t next = 0;  // millis() of the next step
};
extern StrokeAnim gAnim;

// Starts the animation of `cardIndex` at its first stroke; off (the static
// glyph) when the pack has no strokes for it. True when it animates.
bool startStrokeAnim(uint32_t cardIndex, uint32_t now);
// The animation's clock: true when the next stroke was due at `t` and the
// frame moved on; the caller then redraws its header and the kanji
// (drawKanjiBox(), presentKanjiFrame()). It stands still while the panel is
// dim or dark.
bool strokeAnimTick(uint32_t t);
// The kanji box (x, y, w, h): the static set-0 glyph of `cp`, or the
// animation's frame of gAnim.cardIndex.
void drawKanjiBox(lgfx::LovyanGFX& d, uint32_t cp, int x, int y, int w, int h);
// Pushes one animation frame: the header (its counter) and the em square of
// the kanji box.
void presentKanjiFrame(int x, int y, int w, int h);
// The header's counter: "stroke 4/6" while the strokes animate (the bare
// "4/13" when the worded counter of the last stroke does not fit `room`
// caption px; decided per card, ui/QuizUi.h), "strokes 6" from the card's
// stroke count otherwise, "" when it has none.
void strokeCounter(char* out, size_t n, int room, unsigned cardStrokes);

// The wider of the ON and KUN pills: every reading row's text starts after it.
int readingTagW();
// One reading row whose top is y, `lh` px tall: the ON / KUN pill at x, then
// from x + tagW + 5 `first` in `text`, then `rest` in `text-dim` right after
// it, or the "+N" caption when `more` > 0 (ui/QuizUi.h introReadings()). The
// detail card passes every reading as `first`.
void drawReadingRow(lgfx::LovyanGFX& d, int x, int y, int w, int lh, int tagW,
                    tgc::ReadingKind kind, const char* first, const char* rest = nullptr,
                    int more = 0);

}  // namespace app
}  // namespace tgc
