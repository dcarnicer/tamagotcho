// The clock; see Clock.h.
#include "app/Clock.h"

#include <Arduino.h>

#include <cstring>

#include "app/App.h"
#include "app/DateScreens.h"
#include "app/DeckUpdates.h"
#include "app/Net.h"
#include "app/Power.h"
#include "app/Quiz.h"
#include "app/Settings.h"
#include "app/UiStrings.h"
#include "platform/HomeWifi.h"
#include "srs/WallClock.h"

namespace tgc {
namespace app {

uint16_t gBootDay = 0;  // persisted day index at boot: "last used" on the date screen
// A real wall-clock time was applied since boot (applyWallClock), so the
// power-cut date question is not needed.
bool gWallClockSynced = false;
tgc::TzRule gTz;                // the zone in use (valid while gTzKnown)
bool gTzKnown = false;
bool gTzManual = false;         // gTz came from Settings > Device > Date > Zone
char gTzName[48] = "";          // its IANA name, "" when unknown
tgc::TzRule gPhoneTz;           // the zone the page last sent (the "From phone" row)
bool gPhoneTzKnown = false;
char gPhoneTzName[48] = "";
DateSync gDateSync = DateSync::None;

namespace {

char gTzRuleText[tgc::kTzRuleMaxLen + 1] = "";

}  // namespace

// "18 Oct": the current day for the settings header and the Date row.
void shortDate(uint16_t day, char* out, size_t n) {
    int y = 0;
    unsigned m = 1, dd = 1;
    tgc::civilFromDays(gSrs.epochDayOf(day), &y, &m, &dd);
    st::formatDayMonth(dd, ui().months[(m + 11) % 12], out, n);
}

// The calendar date of SRS day `day`: the store's wall-clock anchor once a real
// time was synced, else the fixed 2026-10-17 base (docs/srs.md section 4.1).
// Only differences matter for the schedule; the date is for the user.
void formatDate(uint16_t day, char* out, size_t n) {
    int y = 0;
    unsigned m = 0, d = 0;
    tgc::civilFromDays(gSrs.epochDayOf(day), &y, &m, &d);
    snprintf(out, n, "%04d-%02d-%02d", y, m, d);
}

// The boot-time question's network attempt ends without an answer from it:
// a join still in progress stops, radio off, and the status line goes. Called
// when the question is answered (by hand or by the network time) and when a
// press on the "Getting the date" screen skips to the stepper.
void dateSyncCancel() {
    if (gDateSync == DateSync::Joining) {
        netRadioOff();
        Serial.printf("clock: network time skipped, set by hand\n");
    }
    gDateSync = DateSync::None;
}

namespace {

const char* clockSyncName(tgc::ClockSync k) {
    switch (k) {
        case tgc::ClockSync::Rejected: return "rejected";
        case tgc::ClockSync::Anchored: return "anchored";
        case tgc::ClockSync::Same: return "same day";
        case tgc::ClockSync::Advanced: return "advanced";
        case tgc::ClockSync::Reanchored: return "re-anchored";
    }
    return "?";
}

}  // namespace

// Reads the time zone from NVS (see kPrefTz): a zone chosen by hand, else the
// one the portal page last sent (its rule, else its bare offset), else none.
// Hands it to the SRS store, whose day lengths follow it, and logs it.
void loadTimeZone() {
    gTzKnown = gTzManual = gPhoneTzKnown = false;
    gTzName[0] = gTzRuleText[0] = gPhoneTzName[0] = 0;
    char phoneRule[tgc::kTzRuleMaxLen + 1] = "";
    if (gPrefs.isKey(kPrefTzRule) &&
        gPrefs.getString(kPrefTzRule, phoneRule, sizeof phoneRule) > 0 &&
        tgc::parseTzRule(phoneRule, &gPhoneTz)) {
        gPhoneTzKnown = true;
    } else if (gPrefs.isKey(kPrefTz)) {
        const int off = gPrefs.getShort(kPrefTz, 0);
        gPhoneTz = tgc::fixedTzRule(off);
        tgc::formatFixedTzRule(off, phoneRule, sizeof phoneRule);
        gPhoneTzKnown = tgc::tzRuleOffsetsPlausible(gPhoneTz);
    }
    if (gPhoneTzKnown && gPrefs.isKey(kPrefTzName))
        gPrefs.getString(kPrefTzName, gPhoneTzName, sizeof gPhoneTzName);

    char manual[48] = "";
    if (gPrefs.isKey(kPrefTzManual)) gPrefs.getString(kPrefTzManual, manual, sizeof manual);
    const tgc::TzZone* z = tgc::findTzZone(manual);
    if (z && tgc::parseTzRule(z->rule, &gTz)) {
        gTzKnown = gTzManual = true;
        snprintf(gTzName, sizeof gTzName, "%s", z->name);
        snprintf(gTzRuleText, sizeof gTzRuleText, "%s", z->rule);
    } else if (gPhoneTzKnown) {
        gTz = gPhoneTz;
        gTzKnown = true;
        snprintf(gTzName, sizeof gTzName, "%s", gPhoneTzName);
        snprintf(gTzRuleText, sizeof gTzRuleText, "%s", phoneRule);
    }
    gSrs.setTimeZone(gTzKnown ? gTz : tgc::fixedTzRule(0));
    if (gTzKnown)
        Serial.printf("clock: zone %s %s (%s)\n", gTzName[0] ? gTzName : "-", gTzRuleText,
                      gTzManual ? "chosen by hand" : "from the phone");
    else
        Serial.printf("clock: no time zone yet\n");
}

// Applies a real wall-clock time to the SRS day clock (docs/srs.md section 5.1):
// the phone's browser through the portal's POST /time, or NTP over the home
// Wi-Fi (Get decks, the boot-time date question, the network test). Anchors or
// advances the day, aligns the 04:00 rollover for `zone` and brings the screen
// up to date: a pending power-cut date question is answered, and a new day
// rebuilds the queue. False when the time was rejected as implausible; nothing
// changes then.
bool applyWallClock(int64_t utcMs, const tgc::TzRule& zone, bool leaveDateNow) {
    const uint16_t before = gSrs.today();
    const int offsetMin = tgc::tzOffsetMinAt(zone, utcMs);
    const tgc::ClockSyncResult r = gSrs.syncWallClock(utcMs, zone);
    if (r.kind == tgc::ClockSync::Rejected) {
        Serial.printf("clock: rejected (%ld s, utc%+d min)\n", static_cast<long>(utcMs / 1000),
                      offsetMin);
        return false;
    }
    gWallClockSynced = true;
    // Persist a new anchor or day. A same-day sync (every page reload) only
    // moved the carry, which the idle save persists, so it does not write the
    // flash. As in goIdle(), a store that failed to load and has no answers is
    // not written over a damaged file; the next answer saves it.
    if (r.kind != tgc::ClockSync::Same && (gDirty || gSrsLoaded)) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    char date[16];
    formatDate(gSrs.today(), date, sizeof date);
    Serial.printf("clock: %s, day %u -> %u (%s), utc%+d min\n", clockSyncName(r.kind),
                  (unsigned)before, (unsigned)gSrs.today(), date, offsetMin);

    // The boot-time "how many days?" question: the real time answers it.
    // dateSyncPoll() may ask to stay on the screen for a moment: it takes the
    // radio session over for the update check before leaving for the quiz.
    if (gScreen == Screen::Date && gAfterDate == Screen::Quiz && leaveDateNow) {
        leaveDate();
        return true;
    }
    // A manual correction in progress restarts from the new (real) today.
    if (gScreen == Screen::Date) gDateSteps = 0;
    if (gSrs.today() != before && gPack->isOpen()) {
        // A new day: cards may have come due. An unanswered question (or the
        // done screen) gives way to the new queue; after an answer, the next
        // question comes from it. On the portal, Get decks or network test
        // screen the same goes for the quiz screen that the settings home's
        // "Continue" shows.
        gQueue.rebuild(*gPack, gSrs, gRng);
        auto refresh = [](Screen& s) {
            if ((s == Screen::Quiz && !gRevealed) || s == Screen::Done)
                s = newQuestion() ? Screen::Quiz : Screen::Done;
        };
        if (gScreen == Screen::Quiz || gScreen == Screen::Done)
            refresh(gScreen);
        else if (gScreen == Screen::Wifi || gScreen == Screen::GetDecks || gScreen == Screen::NetTest)
            refresh(gBeforeMenu);
    }
    if (!gScreenOff) drawScreen();
    return true;
}

// NTP over the home Wi-Fi, connected: the UTC time through the zone in use.
// Without a zone the study day is unknown, and guessing UTC could put it off by
// one, so the sync is skipped (logged). False when there was no usable time.
bool netTimeSync() {
    if (!gTzKnown) {
        Serial.printf("clock: no NTP sync, no time zone yet (open the portal page once, or "
                      "Settings > Device > Date > Zone)\n");
        return false;
    }
    int64_t utcMs = 0;
    return gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs) && applyWallClock(utcMs, gTz);
}

// Boot-time date question (setup()): with a home Wi-Fi and a time zone known,
// start joining; the date screen shows "Getting the date via <network>" (a tap
// skips it) and dateSyncPoll() takes it from there. Otherwise the stepper.
void dateSyncBegin() {
    if (!tgc::HomeWifi::hasCredentials() || !gTzKnown) {
        Serial.printf("clock: no network time at boot (no %s)\n",
                      tgc::HomeWifi::hasCredentials() ? "time zone" : "home Wi-Fi saved");
        return;
    }
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        gDateSync = DateSync::NoNetwork;
        Serial.printf("clock: no network time at boot (radio did not start)\n");
        return;
    }
    gDateSync = DateSync::Joining;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    Serial.printf("clock: getting the date via \"%s\"\n", gHome.ssid());
}

// loop() on the date screen while Joining. Once the join succeeds: NTP
// (blocking, at most 2 x kNtpTimeoutMs), then applyWallClock(). If the update
// check is due, the same radio session reads the catalog before leaving for the
// quiz; otherwise the radio is off and the quiz follows. On any failure the
// radio is off too and the stepper comes up with a status line that says to set
// the date by hand.
bool dateSyncPoll() {
    if (gDateSync != DateSync::Joining) return false;
    const tgc::HomeWifi::State st = gHome.poll();
    if (st == tgc::HomeWifi::State::Connecting) return false;
    const bool joined = st == tgc::HomeWifi::State::Connected;
    int64_t utcMs = 0;
    const bool got = joined && gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs);
    if (joined) gSwallowInput = true;  // presses made while blocked must not act now
    gDateSync = DateSync::None;
    if (got && applyWallClock(utcMs, gTz, false)) {
        if (updateCheckDue()) {
            startUpdateCheckFromDate();
            return true;
        }
        netRadioOff();
        leaveDate();
        return true;
    }
    netRadioOff();
    gDateSync = joined ? DateSync::NoTime : DateSync::NoNetwork;
    // The status is news: the join's time does not count against the reader.
    gNetActivityAt = millis();
    Serial.printf("clock: no network time at boot (%s), asking by hand\n",
                  joined ? "no NTP answer" : "could not join");
    if (!gScreenOff) drawScreen();
    return false;
}

namespace {

// Writes an NVS string only when it changed: the page posts on every load.
void putPrefString(const char* key, const char* value) {
    char old[64] = "";
    if (gPrefs.isKey(key)) gPrefs.getString(key, old, sizeof old);
    if (!gPrefs.isKey(key) || std::strcmp(old, value) != 0) gPrefs.putString(key, value);
}

}  // namespace

// Called by the portal with the browser's clock (POST /time on every page load):
// the time, the offset of the moment, and the zone as a POSIX rule and an IANA
// name (docs/srs.md section 5.1). The rule is used when it parses and gives
// that offset now; it becomes the phone's zone (NVS `tzr`, `tzn`, and the
// offset in `tz`), which replaces a zone chosen by hand. Without a usable rule
// (a page from before it): a zone in use that gives the same offset now is
// kept, choice by hand included; else the bare offset becomes the zone, as
// before the rule existed (a DST change then shifts the rollover until the
// next visit).
bool portalTime(int64_t utcMs, int16_t tzOffsetMin, const char* rule, const char* zone) {
    tgc::TzRule r;
    char ruleText[tgc::kTzRuleMaxLen + 1] = "";
    const bool haveRule = rule && *rule && tgc::parseTzRule(rule, &r) &&
                          tgc::tzOffsetMinAt(r, utcMs) == tzOffsetMin;
    if (rule && *rule && !haveRule)
        Serial.printf("clock: page rule \"%s\" ignored (does not parse or gives another offset "
                      "than %+d min)\n",
                      rule, (int)tzOffsetMin);
    if (haveRule) {
        snprintf(ruleText, sizeof ruleText, "%s", rule);
    } else {
        if (gTzKnown && tgc::tzOffsetMinAt(gTz, utcMs) == tzOffsetMin)
            return applyWallClock(utcMs, gTz);
        r = tgc::fixedTzRule(tzOffsetMin);
        tgc::formatFixedTzRule(tzOffsetMin, ruleText, sizeof ruleText);
    }
    // A broken clock or offset changes nothing, the zone included.
    if (!tgc::wallClockPlausible(utcMs, r)) return applyWallClock(utcMs, r);
    putPrefString(kPrefTzRule, ruleText);
    putPrefString(kPrefTzName, zone ? zone : "");
    if (!gPrefs.isKey(kPrefTz) || gPrefs.getShort(kPrefTz, 0) != tzOffsetMin)
        gPrefs.putShort(kPrefTz, tzOffsetMin);
    if (gPrefs.isKey(kPrefTzManual)) gPrefs.remove(kPrefTzManual);
    const bool changed = !gTzKnown || gTzManual || std::strcmp(gTzRuleText, ruleText) != 0 ||
                         std::strcmp(gTzName, zone ? zone : "") != 0;
    if (changed) loadTimeZone();  // logs the zone
    return applyWallClock(utcMs, gTz);
}

}  // namespace app
}  // namespace tgc
