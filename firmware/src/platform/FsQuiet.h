#pragma once
// LittleFS questions that stay quiet about a missing file. The core's
// LittleFS.exists() is an open() in disguise, and open() and remove() log
// "[E] ... does not exist" for every miss: a Get decks session printed one
// line per catalog deck not on the device, burying the useful ones
// (2026-10-19). stat() on the VFS path answers the same question silently.
// Arduino/ESP32 only; not part of the native build.
#include <Arduino.h>
#include <LittleFS.h>
#include <sys/stat.h>

namespace tgc {

// Where LittleFS.begin() mounts by default (main.cpp calls begin(false)).
constexpr const char* kLittleFsMount = "/littlefs";

// Size of the regular file at LittleFS path `path` ("/packs/n5-es.tgc"), or
// -1 when there is none.
inline long fsFileSize(const String& path) {
    struct stat st;
    const String full = String(kLittleFsMount) + path;
    if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return static_cast<long>(st.st_size);
}

inline bool fsExists(const String& path) { return fsFileSize(path) >= 0; }

// Removes `path` when it is there; true when a file was removed.
inline bool fsRemoveIfExists(const String& path) { return fsExists(path) && LittleFS.remove(path); }

}  // namespace tgc
