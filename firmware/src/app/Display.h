#pragma once
// The display: the full-screen back buffer and its pushes to the panel (the
// whole frame, a region, a list's focus move), and the rotation (the
// Orientation setting; Auto follows the accelerometer).
#include <cstdint>

#include "board/Board.h"
#include "imu/OrientationFilter.h"
#include "render/Widgets.h"
#include "ui/ListView.h"

namespace tgc {
namespace app {

extern lgfx::LGFX_Sprite* gCanvas;  // full-screen back buffer (PSRAM when there is some)
extern int W, H;                    // the panel in its current rotation

// The last push's microseconds and rows (the TGC_QUIZ_LOG frame trace), and
// what the hold progress (holdTick()) needs about the frame on the panel.
extern uint32_t gPushUs;
extern int gPushRows;
extern uint32_t gFrames;
extern bool gLastPushPartial;
extern tgc::widgets::Rect gFrameFocus;
extern bool gFrameFocusValid;
// One-shot: the next present() pushes only gPushRect.
extern bool gPushOnly;
extern tgc::widgets::Rect gPushRect;
extern bool gQuietFrame;  // drawScreen() without its frame-time line

void present();
void presentRegion(int x, int y, int w, int h);
void createCanvas();

// A list's focus move and the push that follows it (only the rows that
// changed when the window did not scroll); dropPartialPush() cancels it.
void dropPartialPush();
void moveListFocus(tgc::ListView& list, int dir = 1);
void presentList(const tgc::ListView& list);

// Orientation setting, NVS `tangocho`/`orient` (it replaced the bool `land`,
// see loadOrientPref()). Auto is offered only on boards with an accelerometer:
// the IMU then picks rotation 0, 1 or 3 (imu/OrientationFilter) and the picked
// rotation is never persisted. Values double as orientRows indexes.
enum OrientPref : uint8_t { kOrientPortrait = 0, kOrientLandscape = 1, kOrientAuto = 2 };
extern uint8_t gOrientPref;
extern tgc::OrientationFilter gOrient;

void applyRotation(uint8_t r);
bool orientAuto();
uint8_t fixedRotation();
uint8_t shownOrientPref();
uint8_t loadOrientPref();
uint8_t bootRotation();
void orientTick(uint32_t now, bool buttonDown);

}  // namespace app
}  // namespace tgc
