// Firmware updates over the air, the platform side; see Ota.h. Arduino/ESP32 only.
#include "platform/Ota.h"

#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cstring>

// The build environment, set per esp32 env in platformio.ini. This is the one
// place with a fallback, so a build without the flag still compiles; its
// "unknown" matches no manifest row.
#ifndef TGC_ENV
#define TGC_ENV "unknown"
#endif

// initArduino() (the framework's cores/esp32/esp32-hal-misc.c) marks a new
// image valid before setup() unless this returns true. The app marks it valid
// itself once it has shown it works (app/Firmware.cpp, docs/ota.md §3.5). On a
// layout without OTA nothing is ever pending, so this changes nothing there.
extern "C" bool verifyRollbackLater() { return true; }

namespace tgc {
namespace ota {

namespace {

// One of our two app slots: app0 as ota_0 or app1 as ota_1.
bool isOurSlot(const esp_partition_t* p) {
    if (!p || p->type != ESP_PARTITION_TYPE_APP) return false;
    return (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && std::strcmp(p->label, "app0") == 0) ||
           (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && std::strcmp(p->label, "app1") == 0);
}

// The image's body into the update slot. begin() starts Update with the
// manifest's exact size (Update checks it against the slot); write() refuses
// a byte past that size, so a server that sends more fails as a size mismatch
// rather than inside Update. Update aborts itself on a failed write.
class SlotSink : public FetchSink {
public:
    explicit SlotSink(uint32_t bytes) : bytes_(bytes) {}
    FetchStatus begin(int) override {
        if (!Update.begin(bytes_)) {
            Serial.printf("[ota] Update.begin(%u): %s\n", (unsigned)bytes_, Update.errorString());
            const uint8_t e = Update.getError();
            return e == UPDATE_ERROR_SIZE           ? FetchStatus::TooLarge
                   : e == UPDATE_ERROR_NO_PARTITION ? FetchStatus::NoSlot
                                                    : FetchStatus::WriteFailed;  // out of memory
        }
        begun_ = true;
        return FetchStatus::Ok;
    }
    FetchStatus write(const uint8_t* p, size_t n) override {
        if (written_ + n > bytes_) return FetchStatus::SizeMismatch;
        // Update copies the chunk into its sector buffer; it never writes to it.
        if (Update.write(const_cast<uint8_t*>(p), n) != n) {
            Serial.printf("[ota] Update.write at %u: %s\n", (unsigned)written_,
                          Update.errorString());
            return Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? FetchStatus::BadImage
                                                                : FetchStatus::WriteFailed;
        }
        written_ += n;
        return FetchStatus::Ok;
    }
    bool begun() const { return begun_; }

private:
    uint32_t bytes_;
    size_t written_ = 0;
    bool begun_ = false;
};

}  // namespace

const char* buildEnv() { return TGC_ENV; }

bool layoutIsOurs() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    // With a single OTA slot the IDF names the running one as the next: never
    // write over it.
    return otadata && isOurSlot(running) && isOurSlot(next) && running != next &&
           running->subtype != next->subtype && running->size == next->size;
}

uint32_t slotBytes() {
    return layoutIsOurs() ? esp_ota_get_next_update_partition(nullptr)->size : 0;
}

const char* runningSlot() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    return p ? p->label : "";
}

const char* nextSlot() {
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->label : "";
}

bool pendingVerify() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool markValid() {
    if (!pendingVerify()) return false;
    const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) Serial.printf("[ota] mark valid failed: %s\n", esp_err_to_name(e));
    return e == ESP_OK;
}

const char* lastInvalidSlot() {
    const esp_partition_t* p = esp_ota_get_last_invalid_partition();
    return p ? p->label : "";
}

FetchResult install(const InstallRequest& req) {
    FetchResult res;
    if (!req.app || !layoutIsOurs()) {
        res.status = FetchStatus::NoSlot;
        return res;
    }
    const ManifestApp& app = *req.app;
    if (app.bytes > slotBytes()) {
        res.status = FetchStatus::TooLarge;
        return res;
    }
    const String url = app.url.c_str();
    if (!url.startsWith("https://") && !url.startsWith("http://")) {
        res.status = FetchStatus::BadUrl;
        return res;
    }
    Serial.printf("[ota] installing %s (%u bytes) into %s\n", url.c_str(), (unsigned)app.bytes,
                  nextSlot());
    FetchBodyRequest body;
    body.url = url;
    body.expectBytes = app.bytes;
    body.expectSha256 = app.sha256;
    body.idleTimeoutMs = req.idleTimeoutMs;
    body.totalTimeoutMs = req.totalTimeoutMs;
    body.http10 = true;  // a raw stream: no chunked body
    body.progress = req.progress;
    body.progressCtx = req.progressCtx;
    SlotSink sink(app.bytes);
    const uint32_t started = millis();
    res = fetchToSink(body, sink);
    if (res.status == FetchStatus::Ok) {
        // Size and SHA-256 matched the manifest: only now may Update check the
        // image and switch the boot slot. end() without evenIfRemaining also
        // refuses an image shorter than the size it began with.
        if (!Update.end()) {
            Serial.printf("[ota] Update.end: %s\n", Update.errorString());
            res.status = FetchStatus::BadImage;
        }
    } else if (sink.begun()) {
        Update.abort();  // the boot slot stays the running one
    }
    Serial.printf("[ota] install: %s (%u bytes in %u ms, http %d, tls %d)\n",
                  fetchStatusString(res.status), (unsigned)res.bytes,
                  (unsigned)(millis() - started), res.httpCode, res.tlsError);
    return res;
}

}  // namespace ota
}  // namespace tgc
