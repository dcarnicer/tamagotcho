// The display: back buffer, pushes and rotation; see Display.h.
#include "app/Display.h"

#include <Preferences.h>

#include "app/App.h"
#include "app/Power.h"
#include "platform/Psram.h"

namespace tgc {
namespace app {

lgfx::LGFX_Sprite* gCanvas = nullptr;  // full-screen back buffer (PSRAM when there is some)
int W, H;

// Microseconds and rows of the last push, for the TGC_QUIZ_LOG frame trace
// in drawScreen().
uint32_t gPushUs = 0;
int gPushRows = 0;

// Every push notes what the hold progress (holdTick()) needs about the frame
// now on the panel: the focused list row drawList() drew for it (if any), a
// frame count and whether the push was partial.
uint32_t gFrames = 0;
bool gLastPushPartial = false;
tgc::widgets::Rect gFrameFocus{};
bool gFrameFocusValid = false;
// One-shot: the next present() pushes only gPushRect (a list's hold-progress
// frame redraws its screen clipped to the focused row).
bool gPushOnly = false;
tgc::widgets::Rect gPushRect{};

namespace {

void noteFrame(bool partial) {
    gFrameFocusValid = tgc::widgets::takeFocusRow(&gFrameFocus);
    ++gFrames;
    gLastPushPartial = partial;
}

}  // namespace

void present() {
    dropPartialPush();
    if (gPushOnly) {
        gPushOnly = false;
        presentRegion(gPushRect.x, gPushRect.y, gPushRect.w, gPushRect.h);
        return;
    }
    noteFrame(false);
    const uint32_t t0 = micros();
    gCanvas->pushSprite(&tgc::board::display(), 0, 0);
    gPushUs = micros() - t0;
    gPushRows = H;
}

// Pushes only (x, y, w, h) of the back buffer: a partial update.
void presentRegion(int x, int y, int w, int h) {
    noteFrame(true);
    const uint32_t t0 = micros();
    tgc::widgets::presentRegion(*gCanvas, tgc::board::display(), x, y, w, h);
    gPushUs = micros() - t0;
    gPushRows = h;
}

// (Re)create the full-screen back buffer for the current W x H, PSRAM first with
// an internal-RAM fallback (a board without PSRAM goes straight to internal
// RAM: 64.8 KB at 240 x 135). Used at boot and on an orientation change.
void createCanvas() {
    auto& disp = tgc::board::display();
    if (gCanvas) {
        delete gCanvas;
        gCanvas = nullptr;
    }
    gCanvas = new lgfx::LGFX_Sprite(&disp);
    gCanvas->setColorDepth(16);
    gCanvas->setPsram(tgc::hasPsram());
    if (!gCanvas->createSprite(W, H)) {
        gCanvas->setPsram(false);  // fall back to internal RAM
        gCanvas->createSprite(W, H);
    }
}

namespace {

// A focus move on a list whose other content does not depend on the focus
// (settings home, decks, pickers, time zones): when the window did not scroll,
// the next presentList() pushes only the rows from the old to the new focus
// (widgets::presentRegion()). One-shot; any present() drops it.
struct PartialPush {
    bool armed = false;
    const tgc::ListView* list = nullptr;
    int oldFocus = 0;
    int first = 0;
};
PartialPush gPartial;

}  // namespace

void dropPartialPush() { gPartial.armed = false; }

// `dir` 1 is the next row (the front tap), -1 the previous one (a key's up).
void moveListFocus(tgc::ListView& list, int dir) {
    const int oldFocus = list.focus();
    const int oldFirst = list.first();
    if (dir < 0)
        list.prev();
    else
        list.next();
    gPartial.armed = list.first() == oldFirst;
    gPartial.list = &list;
    gPartial.oldFocus = oldFocus;
    gPartial.first = oldFirst;
}

void presentList(const tgc::ListView& list) {
    const PartialPush p = gPartial;
    gPartial.armed = false;
    if (p.armed && p.list == &list && list.first() == p.first && list.isVisible(p.oldFocus) &&
        list.isVisible(list.focus())) {
        const int a = std::min(p.oldFocus, list.focus());
        const int b = std::max(p.oldFocus, list.focus());
        const int y0 = list.rowY(a);
        presentRegion(0, y0, W, list.rowY(b) + list.rowH() - y0);
        return;
    }
    present();
}

bool gQuietFrame = false;  // drawScreen() without its frame-time line

// Rotate the panel (0-3, see board::setRotation), pick up the new width/height,
// recreate the back buffer for it and redraw. The canvas is recreated here so a
// rotation can never leave the previous orientation's buffer mis-sized.
// Persisting the setting is the caller's job: Auto rotations are never stored.
void applyRotation(uint8_t r) {
    tgc::board::setRotation(r);
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    drawScreen();
}

uint8_t gOrientPref = kOrientAuto;
tgc::OrientationFilter gOrient;

namespace {

constexpr uint32_t kImuPollMs = 100;  // accelerometer poll while Auto is on
uint32_t gImuPollAt = 0;
float gAccel[3] = {0, 0, 0};          // last sample, for the rotation log

}  // namespace

bool orientAuto() { return gOrientPref == kOrientAuto && tgc::board::hasAccel(); }

// The rotation a fixed setting asks for. Auto on a board whose accelerometer is
// missing or failed to start behaves as portrait (the setting is kept). A
// landscape-only board (the Cardputer ADV) is always landscape, whatever the
// setting says (it has no Orientation row).
uint8_t fixedRotation() {
    if (tgc::board::fixedLandscape()) return 1;
    return gOrientPref == kOrientLandscape ? 1 : 0;
}

// The setting as the Device page and its picker show it: that Auto reads as
// Portrait, which is what it draws (the T-Display-S3 has no accelerometer).
uint8_t shownOrientPref() {
    return gOrientPref == kOrientAuto && !orientAuto() ? kOrientPortrait : gOrientPref;
}

// The orientation setting. Up to 2026-10-18 it was the bool `land`; that key
// is read once, rewritten as `orient` and removed, so a landscape user keeps
// landscape. New users get Auto (2026-10-19, once the axes were confirmed on
// the StickS3), which a board without an accelerometer draws as portrait.
uint8_t loadOrientPref() {
    if (gPrefs.isKey("orient")) {
        const uint8_t v = gPrefs.getUChar("orient", kOrientAuto);
        return v <= kOrientAuto ? v : kOrientAuto;
    }
    const uint8_t v = gPrefs.getBool("land", false) ? kOrientLandscape : kOrientAuto;
    gPrefs.putUChar("orient", v);
    gPrefs.remove("land");
    return v;
}

// First rotation at boot. In Auto mode one accelerometer sample decides at once
// (no settle time: nothing is on screen yet); a flat or unclear pose falls back
// to portrait. The filter then starts from what is shown.
uint8_t bootRotation() {
    if (!orientAuto()) return fixedRotation();
    int r = tgc::OrientationFilter::kNone;
    float x, y, z;
    for (int tries = 0; tries < 10; ++tries) {  // the first sample needs ~10 ms
        if (tgc::board::readAccel(&x, &y, &z)) {
            r = gOrient.classify(x, y, z);
            break;
        }
        delay(5);
    }
    if (r == tgc::OrientationFilter::kNone) r = 0;
    gOrient.reset(r);
    return static_cast<uint8_t>(r);
}

// Auto orientation, once per loop(): sample the accelerometer every kImuPollMs
// while the panel is on and apply a settled rotation only while no button is
// down, so the picture never turns in the middle of a press (the change waits
// for the release). A rotation is not user activity: it neither brightens a
// dimmed panel nor delays sleep, so a device turning in a bag still sleeps.
void orientTick(uint32_t now, bool buttonDown) {
    if (!orientAuto() || gScreenOff) return;
    if (now - gImuPollAt >= kImuPollMs) {
        gImuPollAt = now;
        if (tgc::board::readAccel(&gAccel[0], &gAccel[1], &gAccel[2]))
            gOrient.update(gAccel[0], gAccel[1], gAccel[2], now);
    }
    const int want = gOrient.current();
    if (want == tgc::OrientationFilter::kNone || buttonDown) return;
    if (want == tgc::board::rotation()) return;
    QLOG("O rotation %d (a=%.2f %.2f %.2f)\n", want, gAccel[0], gAccel[1], gAccel[2]);
    applyRotation(static_cast<uint8_t>(want));
}

}  // namespace app
}  // namespace tgc
