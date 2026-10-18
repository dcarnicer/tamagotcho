#pragma once
// 1bpp UI icons (docs/ux-refresh.md §2.4), drawn in one colour with
// tgc::drawBitmap1bpp(). The shapes are the `ICONS` table of
// tools/ux-mockups/render.py, written the same way ('#' = ink, '.' = clear)
// and packed at compile time: bit 7 = leftmost pixel, rows byte-aligned, the
// stroke-mask layout. The art string must have exactly W x H characters, or it
// does not compile. Pure C++, no Arduino (native-tested in test_ui).
#include <cstdint>

namespace tgc {

struct Icon {
    uint8_t w, h;
    const uint8_t* bits;
};

namespace icons {

template <int W, int H>
struct Bits {
    uint8_t b[H * ((W + 7) / 8)];
};

template <int W, int H>
constexpr Bits<W, H> pack(const char (&art)[W * H + 1]) {
    Bits<W, H> out{};
    constexpr int stride = (W + 7) / 8;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (art[y * W + x] == '#')
                out.b[y * stride + x / 8] = static_cast<uint8_t>(out.b[y * stride + x / 8] |
                                                                 (0x80 >> (x & 7)));
    return out;
}

// True when pixel (x, y) of `icon` is ink.
constexpr bool pixel(const Icon& icon, int x, int y) {
    return (icon.bits[y * ((icon.w + 7) / 8) + x / 8] & (0x80 >> (x & 7))) != 0;
}

// --- result marks (tiles, result pill)
inline constexpr auto kCheckBits = pack<9, 7>(
    "........#"
    ".......##"
    "#.....##."
    "##...##.."
    ".##.##..."
    "..###...."
    "...#.....");
inline constexpr auto kCrossBits = pack<7, 7>(
    "#.....#"
    "##...##"
    ".##.##."
    "..###.."
    ".##.##."
    "##...##"
    "#.....#");

// --- navigation (rows, hint bar)
inline constexpr auto kChevronBits = pack<4, 7>(  // opens a sub-screen
    "#..."
    "##.."
    ".##."
    "..##"
    ".##."
    "##.."
    "#...");
inline constexpr auto kBackBits = pack<4, 7>(  // side button: back
    "...#"
    "..##"
    ".###"
    "####"
    ".###"
    "..##"
    "...#");
inline constexpr auto kTapBits = pack<5, 5>(  // a short press (Morse dot)
    ".###."
    "#####"
    "#####"
    "#####"
    ".###.");
inline constexpr auto kHoldBits = pack<11, 5>(  // a long press (Morse dash)
    ".#########."
    "###########"
    "###########"
    "###########"
    ".#########.");
inline constexpr auto kMenuBits = pack<7, 5>(  // side button: settings
    "#######"
    "......."
    "#######"
    "......."
    "#######");

// --- status and actions
inline constexpr auto kWifiBits = pack<9, 7>(  // header: the radio is on
    ".#######."
    "#.......#"
    "..#####.."
    ".#.....#."
    "...###..."
    "........."
    "....#....");
inline constexpr auto kPlusBits = pack<5, 5>(
    "..#.."
    "..#.."
    "#####"
    "..#.."
    "..#..");
inline constexpr auto kDownBits = pack<7, 8>(  // download
    "...#..."
    "...#..."
    "...#..."
    "#..#..#"
    ".#.#.#."
    "..###.."
    "...#..."
    "#######");
inline constexpr auto kRadioOnBits = pack<7, 7>(  // the saved / loaded choice
    "..###.."
    ".#...#."
    "#.###.#"
    "#.###.#"
    "#.###.#"
    ".#...#."
    "..###..");
inline constexpr auto kRadioOffBits = pack<7, 7>(
    "..###.."
    ".#...#."
    "#.....#"
    "#.....#"
    "#.....#"
    ".#...#."
    "..###..");

// --- settings groups (9x9)
inline constexpr auto kCardsBits = pack<9, 9>(  // decks
    "..######."
    "..#....#."
    "######.#."
    "#....#.#."
    "#....#.#."
    "#....###."
    "#....#..."
    "######..."
    ".........");
inline constexpr auto kBookBits = pack<9, 9>(  // study ("study" in render.py)
    ".##...##."
    "#..#.#..#"
    "#...#...#"
    "#...#...#"
    "#...#...#"
    "#...#...#"
    "##..#..##"
    "..##.##.."
    ".........");
inline constexpr auto kChartBits = pack<9, 9>(  // statistics
    ".......##"
    ".......##"
    "....##.##"
    "....##.##"
    ".##.##.##"
    ".##.##.##"
    ".##.##.##"
    "#########"
    ".........");
inline constexpr auto kWifi9Bits = pack<9, 9>(  // the Wi-Fi group ("wifi9" in render.py)
    "..#####.."
    ".#.....#."
    "#..###..#"
    "..#...#.."
    ".#..#..#."
    "...#.#..."
    "....#...."
    "....#...."
    ".........");
inline constexpr auto kGearBits = pack<9, 9>(  // device
    "...#.#..."
    ".#######."
    ".##...##."
    "##.....##"
    ".#..#..#."
    "##.....##"
    ".##...##."
    ".#######."
    "...#.#...");
inline constexpr auto kFlameBits = pack<7, 7>(  // streak
    "...#..."
    "..##..."
    "..###.."
    ".####.#"
    ".######"
    "#######"
    ".#####.");

// --- list scroll markers: rows hidden above / below
inline constexpr auto kMoreUpBits = pack<5, 3>(
    "..#.."
    ".###."
    "#####");
inline constexpr auto kMoreDownBits = pack<5, 3>(
    "#####"
    ".###."
    "..#..");

}  // namespace icons

inline constexpr Icon kIconCheck = {9, 7, icons::kCheckBits.b};
inline constexpr Icon kIconCross = {7, 7, icons::kCrossBits.b};
inline constexpr Icon kIconChevron = {4, 7, icons::kChevronBits.b};
inline constexpr Icon kIconBack = {4, 7, icons::kBackBits.b};
inline constexpr Icon kIconTap = {5, 5, icons::kTapBits.b};
inline constexpr Icon kIconHold = {11, 5, icons::kHoldBits.b};
inline constexpr Icon kIconMenu = {7, 5, icons::kMenuBits.b};
inline constexpr Icon kIconWifi = {9, 7, icons::kWifiBits.b};
inline constexpr Icon kIconPlus = {5, 5, icons::kPlusBits.b};
inline constexpr Icon kIconDown = {7, 8, icons::kDownBits.b};
inline constexpr Icon kIconRadioOn = {7, 7, icons::kRadioOnBits.b};
inline constexpr Icon kIconRadioOff = {7, 7, icons::kRadioOffBits.b};
inline constexpr Icon kIconCards = {9, 9, icons::kCardsBits.b};
inline constexpr Icon kIconBook = {9, 9, icons::kBookBits.b};
inline constexpr Icon kIconChart = {9, 9, icons::kChartBits.b};
inline constexpr Icon kIconWifi9 = {9, 9, icons::kWifi9Bits.b};
inline constexpr Icon kIconGear = {9, 9, icons::kGearBits.b};
inline constexpr Icon kIconFlame = {7, 7, icons::kFlameBits.b};
inline constexpr Icon kIconMoreUp = {5, 3, icons::kMoreUpBits.b};
inline constexpr Icon kIconMoreDown = {5, 3, icons::kMoreDownBits.b};

}  // namespace tgc
