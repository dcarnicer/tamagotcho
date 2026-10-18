#pragma once
// Line wrapping and paging for read-only text screens drawn in the built-in
// font. Font0 is monospaced (6 px per character at 1x), so a line width is a
// number of code points. Pure C++, no Arduino (native-tested in test_native).
// Used by the About screen (drawAbout() in main.cpp).
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pack/Utf8.h"

namespace tgc {

// A wrapped line: `len` bytes of the source text from byte `begin`.
struct TextSpan {
    size_t begin;
    size_t len;
};

// Wraps UTF-8 `s` into lines of at most `maxChars` code points. Lines break at
// spaces (U+0020, dropped at the break); U+00A0 (no-break space) never breaks,
// and Font0, which has no glyph for it, draws it as a blank cell. A word longer
// than a line breaks after its last '/' or '-' that fits, else at the limit.
// An empty or blank text gives no lines.
inline std::vector<TextSpan> wrapText(const char* s, int maxChars) {
    std::vector<TextSpan> lines;
    if (!s || maxChars < 1) return lines;
    std::vector<size_t> at;  // byte offset of each code point, then of the end
    std::vector<uint32_t> cps;
    for (const char* p = s;;) {
        at.push_back(static_cast<size_t>(p - s));
        const uint32_t cp = utf8Next(&p);
        if (!cp) break;
        cps.push_back(cp);
    }
    const int n = static_cast<int>(cps.size());
    auto emit = [&](int b, int e) {  // code points [b, e), trailing spaces trimmed
        while (e > b && cps[e - 1] == ' ') --e;
        if (e > b) lines.push_back({at[b], at[e] - at[b]});
    };
    int i = 0;
    while (i < n) {
        while (i < n && cps[i] == ' ') ++i;
        if (i >= n) break;
        if (n - i <= maxChars) {
            emit(i, n);
            break;
        }
        const int limit = i + maxChars;  // cps[limit] exists: n - i > maxChars
        int brk = -1;
        for (int k = limit; k > i; --k)
            if (cps[k] == ' ') {
                brk = k;
                break;
            }
        if (brk >= 0) {
            emit(i, brk);
            i = brk + 1;
            continue;
        }
        int cut = limit;  // one word longer than the line
        for (int k = limit - 1; k > i; --k)
            if (cps[k] == '/' || cps[k] == '-') {
                cut = k + 1;
                break;
            }
        emit(i, cut);
        i = cut;
    }
    return lines;
}

// One block of a paged screen, `height` px tall. `newGroup` starts a group:
// the blocks up to the next one that starts a group stay on one page when
// they can.
struct PageBlock {
    int height;
    bool newGroup;
};

// Where a block landed: its page (0-based) and its top within that page.
struct PageSlot {
    int page;
    int y;
};

// Lays the blocks out top to bottom on pages `pageH` px tall, with `gap` px
// between the blocks of a group, `groupGap` between groups and nothing above
// the first block of a page. A group that does not fit the rest of a page
// starts the next one; a group taller than a page fills pages block by block;
// a block taller than a page gets a page of its own (the caller clips it).
inline std::vector<PageSlot> layoutPages(const std::vector<PageBlock>& blocks, int pageH, int gap,
                                         int groupGap) {
    std::vector<PageSlot> out(blocks.size(), PageSlot{0, 0});
    int page = 0;
    int used = 0;  // px used on the current page; 0 = empty
    size_t i = 0;
    while (i < blocks.size()) {
        size_t j = i + 1;
        while (j < blocks.size() && !blocks[j].newGroup) ++j;  // the group is [i, j)
        int groupH = 0;
        for (size_t k = i; k < j; ++k) groupH += blocks[k].height + (k > i ? gap : 0);
        if (used > 0 && used + groupGap + groupH > pageH && groupH <= pageH) {
            ++page;
            used = 0;
        }
        for (size_t k = i; k < j; ++k) {
            int sep = used == 0 ? 0 : (k == i ? groupGap : gap);
            if (used > 0 && used + sep + blocks[k].height > pageH) {
                ++page;
                used = 0;
                sep = 0;
            }
            out[k] = {page, used + sep};
            used += sep + blocks[k].height;
        }
        i = j;
    }
    return out;
}

// Pages a layoutPages() result spans (1 for no blocks: an empty screen).
inline int pageCount(const std::vector<PageSlot>& slots) {
    return slots.empty() ? 1 : slots.back().page + 1;
}

}  // namespace tgc
