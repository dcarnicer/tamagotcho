#pragma once
// Filesystem questions that stay quiet about a missing file. The core's
// FS::exists() is an open() in disguise, and open() and remove() log
// "[E] ... does not exist" for every miss: a Get decks session printed one
// line per catalog deck not on the device, burying the useful ones
// (2026-10-19). stat() on the VFS path answers the same question silently.
// For LittleFS and the microSD card (platform/Storage.h); the overloads
// without a filesystem are LittleFS's. Arduino/ESP32 only; not part of the
// native build.
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <sys/stat.h>

#include "storage/DeckFiles.h"

namespace tgc {

// Where LittleFS.begin() mounts by default (app/Boot calls begin(false)).
constexpr const char* kLittleFsMount = "/littlefs";

// The VFS mount of `fs`: LittleFS's, or the card's (the only other one).
inline const char* fsMount(fs::FS& fs) { return &fs == &LittleFS ? kLittleFsMount : kCardMount; }

// Size of the regular file at `path` on `fs` ("/packs/n5-es.tgc" on LittleFS),
// or -1 when there is none.
inline long fsFileSize(fs::FS& fs, const String& path) {
    struct stat st;
    const String full = String(fsMount(fs)) + path;
    if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return static_cast<long>(st.st_size);
}

inline bool fsExists(fs::FS& fs, const String& path) { return fsFileSize(fs, path) >= 0; }

// Removes `path` when it is there; true when a file was removed.
inline bool fsRemoveIfExists(fs::FS& fs, const String& path) {
    return fsExists(fs, path) && fs.remove(path);
}

inline long fsFileSize(const String& path) { return fsFileSize(LittleFS, path); }
inline bool fsExists(const String& path) { return fsExists(LittleFS, path); }
inline bool fsRemoveIfExists(const String& path) { return fsRemoveIfExists(LittleFS, path); }

}  // namespace tgc
