#pragma once
// Minimal UTF-8 decoding. Invalid bytes decode to U+FFFD and advance by one.
#include <cstdint>

namespace tgc {

// Decodes one code point at *p and advances *p. Returns 0 at NUL.
inline uint32_t utf8Next(const char** p) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(*p);
    uint8_t c = s[0];
    if (c == 0) return 0;
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { *p += 1; return 0xFFFD; }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n;
    return cp;
}

}  // namespace tgc
