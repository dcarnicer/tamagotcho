# Waveshare ESP32-S3-Touch-AMOLED-2.06

Port notes for the watch-shaped Waveshare board. **Status (2026-11-07): it
boots on hardware, shows the UI, runs the quiz, and touch + BOOT drive it.**
The StickS3 remains the primary target; nothing here changes the other boards.
Still missing: the IMU, RTC and SD card. The release tooling does not carry
this env yet.

## Hardware

| part | detail |
|---|---|
| MCU | ESP32-S3R8: 8 MB octal PSRAM, 32 MB flash |
| panel | 2.06" AMOLED, 410x502, CO5300 over QSPI, brightness with command 0x51 (no backlight pin) |
| touch | FT3168 on I2C (SCL 14, SDA 15), INT 38, reset 9 |
| PMIC | AXP2101 at 0x34 on the same I2C bus |
| IMU | QMI8658C on I2C, INT 21 |
| RTC | PCF85063ATL on I2C, INT 39, battery-backed through the PMIC |
| card | 1-bit SDMMC: CLK 2, CMD 1, D0 3 (no SPI CS) |
| buttons | BOOT = GPIO0 (active low), PWR = GPIO10 (high while pressed; a ~6 s hold powers off) |
| audio | ES8311 speaker / ES7210 microphones (not used by the app) |

Display pins: SCLK 11, SIO0-3 4/5/6/7, CS 12, reset 8, TE 13. The panel's
visible columns start at 0x16 (22 px), which is the CO5300 column offset.

Sources: [Waveshare's BSP](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)
and [the wiki](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06).

## Firmware

`firmware/platformio.ini`:

- `amoled206`: the app, 16 MB of the 32 MB flash (the T-Display-S3 table with
  2 MiB OTA slots and 11.94 MiB LittleFS), `TGC_BOARD_AMOLED206`.
- `amoled206-shots`: the same with the serial remote; never a release.

`firmware/src/board/BoardAmoled206.cpp`:

- M5GFX's `Panel_CO5300` (Waveshare's init list, including the 120 ms
  sleep-out delay) on a `Bus_SPI` in QSPI mode, SPI2, **20 MHz** (40 MHz made
  the panel come up all yellow now and then), portrait 410x502 with the 22 px
  column offset. `begin()` also holds the panel reset low for 20 ms and waits
  120 ms after releasing it before the init list.
- The UI is drawn at **177x223 logical pixels and pushed at 2x**, with a 28 px
  physical black border for the watch's rounded corners
  (`board::uiScale()` / `uiInset()`).
- BOOT = `Select` (front); it works. PWR = the AXP2101's power key: a 6 s hold
  powers off, a short press only powers the board on from off. Its GPIO10
  SYS_OUT line does not follow short presses on this unit and the PMIC does not
  report them either, so PWR is not a navigation button.
- **Touch (FT3168)** drives every screen: a tap is the front tap (next /
  advance), a ~0.6 s hold is the front hold (answer / open), a left swipe is
  the side tap (back / cancel) and a down swipe is the side hold (the settings
  home). The gestures are queued as button events (`board::takeButtonEvent()`,
  consumed by `main.cpp`); touch does not show the hold fill, which stays a
  physical button's. The I2C bus runs at 100 kHz for the FT3168.
- AXP2101 battery voltage and VBUS/charging, so the percentage, the critical
  guard and "on USB power" work.
- No IMU, RTC or SD yet: `hasAccel()` is false, `sdPins()` false.

`render/Widgets.cpp` does the scaled push. The canvas keeps the panel's 16-bit
byte order; a raw `uint16_t*` push must go through the `pixelcopy_t` constructor
(the bare `pushImage(uint16_t*)` treats it as non-swapped and the AMOLED shows
garbage). Partial pushes are rounded to even columns on an even-width canvas:
the CO5300 rejects an odd partial window.

## Next steps

1. ~~Flash the full image and confirm the panel~~ (done 2026-11-07: portrait,
   colors right, 2x UI with the black border).
2. ~~Take a `remote.py shot` at 410x502~~ (done: the remote dumps the physical
   scaled screen).
3. ~~Input: the FT3168 touch layer~~ (done: tap/hold/swipe left/swipe down
   verified on hardware; BOOT works too).
4. QMI8658C for Auto orientation (`hasAccel()`/`readAccel()`; axes still need
   measuring), then the PCF85063 RTC (a real date across a power-off), then the
   1-bit SDMMC card behind `platform/Storage`.
5. Only then: release env in `tools/release/build_dist.py`, the workflow and
   the manifest.
