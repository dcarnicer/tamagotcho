#pragma once
// SrsStore::StateIo over LittleFS (`/state/srs.bin`) with a `.bak` fallback and
// temp + rename atomic writes. Arduino/ESP32 only; not part of the native build.
// Design: docs/srs.md section 4.
#include <Arduino.h>
#include <LittleFS.h>

#include <vector>

#include "platform/FsQuiet.h"
#include "srs/SrsStore.h"

namespace tgc {

class FsSrsFile : public StateIo {
public:
    static constexpr const char* kDir = "/state";
    static constexpr const char* kPath = "/state/srs.bin";
    static constexpr const char* kBak = "/state/srs.bak";
    static constexpr const char* kTmp = "/state/srs.tmp";

    // The current file, else the backup from an interrupted write.
    bool load(std::vector<uint8_t>& out) override {
        if (readFile(kPath, out)) return true;
        return readFile(kBak, out);
    }

    // write tmp, flush, close; rename current -> bak (ignore failure), then
    // rename tmp -> current so a power cut leaves at least one loadable file.
    bool save(const std::vector<uint8_t>& data) override {
        LittleFS.mkdir(kDir);
        File f = LittleFS.open(kTmp, "w");
        if (!f) return false;
        size_t n = 0;
        if (!data.empty()) n = f.write(data.data(), data.size());
        f.flush();
        f.close();
        if (n != data.size()) return false;

        if (fsExists(kPath)) LittleFS.rename(kPath, kBak);  // else nothing to keep
        return LittleFS.rename(kTmp, kPath);
    }

private:
    static bool readFile(const char* path, std::vector<uint8_t>& out) {
        if (!fsExists(path)) return false;  // open() would log a missing file
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        const size_t n = f.size();
        out.resize(n);
        const size_t got = n ? f.read(out.data(), n) : 0;
        f.close();
        if (got != n) {
            out.clear();
            return false;
        }
        return true;
    }
};

}  // namespace tgc
