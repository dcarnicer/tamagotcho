#pragma once
// The clock (docs/srs.md section 5): the SRS day as a date, the time zone, a
// real time from the phone (the portal's POST /time) or from NTP, and the
// boot-time date question's attempt at the network time.
#include <cstddef>
#include <cstdint>

#include "srs/TimeZone.h"

namespace tgc {
namespace app {

extern uint16_t gBootDay;      // persisted day index at boot: "last used" on the date screen
extern bool gWallClockSynced;  // a real time was applied since boot (applyWallClock())
// The time zone (docs/srs.md section 5.1), in NVS `tangocho`:
// - `tzr` (string): the POSIX TZ rule the portal page last sent, `tzn` its IANA
//   name ("" when the page sent none);
// - `tz` (int16): the UTC offset in minutes east the page last sent; the only
//   zone a page from before the rule left, read as a fixed rule then;
// - `tzm` (string): the IANA name of a zone chosen by hand from kTzZones
//   (Settings > Device > Date > Zone). It wins while set; the next page visit
//   that sends a zone removes it.
// NTP yields UTC only, so without a zone the network time is not used.
constexpr const char* kPrefTz = "tz";
constexpr const char* kPrefTzRule = "tzr";
constexpr const char* kPrefTzName = "tzn";
constexpr const char* kPrefTzManual = "tzm";
extern tgc::TzRule gTz;  // the zone in use (valid while gTzKnown)
extern bool gTzKnown;
extern bool gTzManual;   // gTz came from Settings > Device > Date > Zone
extern char gTzName[48];
extern tgc::TzRule gPhoneTz;  // the zone the page last sent (the "From phone" row)
extern bool gPhoneTzKnown;
extern char gPhoneTzName[48];
// NTP over the home Wi-Fi gets this long once the network is up, twice at most
// (HomeWifi::fetchUtcMs asks again when nothing answered).
constexpr uint32_t kNtpTimeoutMs = 3000;
// Network time for the boot-time date question (docs/srs.md section 5.1): with a
// home Wi-Fi and a time zone known, the date screen first joins and asks NTP,
// on a screen of its own ("Getting the date via ...", a tap gives up); the
// stepper follows only when that fails or is skipped. Tried once per boot. The
// radio (gHome, shared with Get decks, which cannot be opened from this screen)
// is on only while Joining; the other states are the stepper's status line.
enum class DateSync : uint8_t { None, Joining, NoNetwork, NoTime };
extern DateSync gDateSync;

void formatDate(uint16_t day, char* out, size_t n);
void shortDate(uint16_t day, char* out, size_t n);
void loadTimeZone();
bool applyWallClock(int64_t utcMs, const tgc::TzRule& zone, bool leaveDateNow = true);
bool netTimeSync();
bool portalTime(int64_t utcMs, int16_t tzOffsetMin, const char* rule, const char* zone);
void dateSyncBegin();
bool dateSyncPoll();
void dateSyncCancel();

}  // namespace app
}  // namespace tgc
