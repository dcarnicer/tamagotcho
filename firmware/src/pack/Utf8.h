#pragma once
// Minimal UTF-8 decoding and encoding. Invalid bytes decode to U+FFFD and
// advance by one.
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

// Encodes `cp` as UTF-8 into `out` (at least 5 bytes) and NUL-terminates it.
// Returns the bytes written before the NUL. U+0000 writes just the NUL; a
// surrogate or a value above U+10FFFF becomes U+FFFD, like utf8Next() does on
// bad input.
inline int utf8Encode(uint32_t cp, char* out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp == 0) { out[0] = '\0'; return 0; }
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    out[4] = '\0';
    return 4;
}

}  // namespace tgc
