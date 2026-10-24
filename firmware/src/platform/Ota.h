#pragma once
// Firmware updates over the air, the platform side (docs/ota.md §3.4-§3.7):
// which build environment this image is, whether the flash has our two-slot
// layout and how large the update slot is, the install of a manifest row into
// that slot, and the first boot of a new image. Arduino/ESP32 only; not part
// of the native build (the manifest and the update rule are in ota/).
//
// The install streams the image through NetFetch's body loop (fetchToSink(),
// the same redirects, TLS, timeouts, progress and cancel as a deck download)
// into Arduino's Update library: no file, so no free space is needed. Its size
// and SHA-256 are checked against the manifest row while it streams; only when
// both matched may activate() have Update.end() check the image and switch the
// boot slot. Any failure before that aborts Update, so the running slot stays
// the boot slot and the half-written one is overwritten next time. Update
// writes the image's first 16 bytes last: a cut install never boots.
//
// The bootloader of the framework (Arduino-ESP32 2.0.17) rolls back: a new
// image boots pending verification, and a reset before it is marked valid --
// a crash, a watchdog, a power cut -- boots the previous image instead. (A
// deep-sleep wake probably does not count: that bootloader is built with
// CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP and wakes into the same slot
// without reading otadata.) This file defines verifyRollbackLater() so that,
// on our layout, the app gives that verdict itself (markValid(),
// app/Firmware.cpp).
#include <Arduino.h>

#include <cstdint>

#include "ota/Manifest.h"
#include "platform/NetFetch.h"

namespace tgc {
namespace ota {

// The PlatformIO environment this image was built from (`-DTGC_ENV="m5stack"`),
// the manifest row it takes (docs/ota.md §3.2); "unknown" without the flag.
const char* buildEnv();

// docs/ota.md §3.7, "only our layout": the running app is `app0` or `app1`
// (ota_0 / ota_1), the next update partition is the other one, both are the
// same size, and an `otadata` partition exists. False with the factory-only
// tables of v0.6.0 and older, under M5Launcher and with any other table: then
// the device does not update itself (the Firmware screen says so).
bool layoutIsOurs();
// The update slot's size in bytes, 0 when !layoutIsOurs().
uint32_t slotBytes();
// The running and the next update partition's labels ("app0", "app1"), "" when
// there is none.
const char* runningSlot();
const char* nextSlot();

// The running image is a new one on its first boot, pending verification.
bool pendingVerify();
// Marks the running image valid (cancels the rollback). True when it was
// pending and now is valid.
bool markValid();
// The label of the partition the otadata calls invalid or aborted, "" for none.
// Only for the log: on a device that never updated (erased otadata) the IDF
// answers ota_0, the running slot.
const char* lastInvalidSlot();
// A pending image that failed inside its boot: marks it invalid and restarts
// into the previous one at once (a deep sleep would wake into it again).
void rollBackNow();

struct InstallRequest {
    const ManifestApp* app = nullptr;  // the manifest row: URL, exact size, SHA-256
    uint32_t idleTimeoutMs = 15000;
    uint32_t totalTimeoutMs = 10u * 60u * 1000u;
    FetchProgressFn progress = nullptr;
    void* progressCtx = nullptr;
};

// Blocking install of `req.app` into the update slot (needs a network up).
// Ok: the whole image is written and its size and SHA-256 matched; the boot
// slot is not switched yet: activate() or abandon() next. Any other status:
// Update was aborted and the boot slot is still the running one. NoSlot when
// the layout is not ours, TooLarge for an image larger than the slot,
// SizeMismatch / HashMismatch against the row, BadImage when Update refused
// the image's first byte, WriteFailed for a flash error; the network statuses
// as for packs.
FetchResult install(const InstallRequest& req);
// After an Ok install: Update checks the image and switches the boot slot to
// it (it runs after a restart). False when it refused (the boot slot stays).
bool activate();
// After an Ok install: drop it; the boot slot stays the running one.
void abandon();

}  // namespace ota
}  // namespace tgc
