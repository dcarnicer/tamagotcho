// Tamagotcho Phase 2: open the saved pack in /packs and run a spaced-repetition
// multiple-choice quiz (kanji -> meaning / reading / meaning -> kanji).
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board/Board.h"
#include "catalog/Catalog.h"
#include "imu/OrientationFilter.h"
#include "pack/PackReader.h"
#include "platform/FsPackSource.h"
#include "platform/FsQuiet.h"
#include "platform/FsSrsFile.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/WifiPortal.h"
#include "portal/PortalLogic.h"
#include "power/BatteryGuard.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "render/Widgets.h"
#include "srs/DeckProgress.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"
#include "srs/WallClock.h"
#include "ui/DeckSummary.h"
#include "ui/Icons.h"
#include "ui/InputPrefs.h"
#include "ui/ListView.h"
#include "ui/QuizUi.h"
#include "ui/ScreenLayout.h"
#include "ui/SettingsTree.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/UiLang.h"
#include "Credits.h"
#include "Version.h"

using tgc::board::Button;

// Set -DTGC_QUIZ_LOG to trace every question and answer over serial.
#if defined(TGC_QUIZ_LOG)
#define QLOG(...) Serial.printf(__VA_ARGS__)
#else
#define QLOG(...) do {} while (0)
#endif

// The StickS3 has no RTC and the ESP32's RTC domain does not survive a power-off
// (verified: RTC memory is cleared when the device is switched off). RTC_NOINIT
// memory does survive a soft reset, so it tells us whether this boot followed a
// power cut -- in which case the elapsed days are unknown and we ask the user.
RTC_NOINIT_ATTR uint32_t gPowerMagic;
RTC_NOINIT_ATTR uint32_t gPowerMagicInv;
constexpr uint32_t kPowerMagic = 0x7A5C1234u;
bool gPoweredOff = false;

// RTC slow memory survives deep sleep but not a power cut: when the chip went to
// sleep, by the RTC timer, so the next boot can add the slept time to the SRS
// day clock (see applySleepTime()).
RTC_DATA_ATTR uint64_t gSleptAtUs = 0;
RTC_DATA_ATTR uint32_t gSleepMagic = 0;
constexpr uint32_t kSleepMagic = 0x51EE9A7Eu;
// Set when the last sleep was forced by a critical battery, so the wake can
// check the cell before anything else; plus the UI language of the moment, for
// the "battery low" notice shown before a pack (and its language) is loaded.
RTC_DATA_ATTR bool gSleptCritical = false;
RTC_DATA_ATTR bool gSleepUiEs = false;
// The boot-time date question (after a power cut) was still open when the chip
// went to sleep: the wake asks it again instead of silently dropping it.
RTC_DATA_ATTR bool gDatePending = false;

namespace {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
uint16_t kGrey, kHighlight, kGreen, kRed, kYellow;

// Every user-facing on-device string. The table is chosen from the loaded
// pack's language (gPack->header().lang) when the pack changes; English is the
// default and the fallback for any unknown code, so the error screen (which
// runs before a pack is open) still has text.
struct UiStrings {
    const char* appName;  // header of the error screen (and the done screen with no deck)
    const char* settingsTitle;
    const char* decksTitle;
    const char* newPerDayTitle;
    const char* hardAfterTitle;  // Study > Hard after (it was "Speed")
    const char* hardAfterOff;    // its value that disables Hard
    const char* quizMeaning;
    const char* quizReading;
    const char* quizKanji;
    const char* dateTitle;
    // Date screen: the last-used day and a proposed "today" the user steps.
    // The hint bar (docs/ux-refresh.md §2.4, two side actions): "• +1 day
    // ▬ OK" and "◂ -1 day ▬ cancel" (hintCancel), one row from 189 px (English)
    // and 201 px (Spanish), else the side items on a second row; checked in
    // test_quizui, which copies these.
    const char* dateLastUsed;  // caption before the last-used date: "last used Sat 17 Oct"
    const char* dateToday;     // the pill while the proposed day is the current one
    const char* dateOneDay;    // "+1 day": the pill, and the hint bar's tap word
    const char* dateDaysFmt;   // "+%u days" (2 and up)
    const char* dateBackOneDay;  // "-1 day": the pill, and the hint bar's side tap word
    const char* dateBackDaysFmt; // "-%u days" (2 and up)
    const char* weekdays[7];   // short names, Sunday first
    // The boot-time question tries the network time first (docs/srs.md 5.1):
    // a label-face title and the network in a caption; the hint bar's tap
    // (hintByHand) gives up and sets the date by hand.
    const char* dateNetTitle;
    const char* dateNetViaFmt;  // %s = home Wi-Fi name
    // Stepper status line when that failed (caption, wrapped).
    const char* dateNoNetwork;   // could not join
    const char* dateNoTime;      // joined, but no (plausible) time came back
    // Settings > Device > Date: the submenu (row labels; the date and the
    // zone's city are their values) and the time zone list (a city and its
    // offsets, see drawTzList()).
    const char* dateMenuRows[2];  // the date (the stepper), the zone (the list)
    const char* tzNone;           // the zone row's value when no zone is known
    const char* tzTitle;          // header (ASCII)
    const char* tzFromPhone;      // first row: the zone the portal page last sent
    const char* tzSummerFmt;      // a row's offsets: "UTC+1, summer UTC+2"
    const char* tzPhoneNone;      // "From phone" when the page never sent a zone
    const char* tzSaved;          // Date submenu note after a choice
    // The loaded deck's caption in the deck list (Font0 under the row, from
    // its left padding: at most 19 characters in the 135 px portrait).
    const char* deckCountsFmt;  // cards, due
    const char* statsTitle;
    // Error screens (showError()): what happened as a label-face title (two
    // lines at most), then one action in a wrapped caption. End-user words.
    const char* errNoFilesystem;      // LittleFS present but will not mount
    const char* errNoFilesystemHint;  // hold the front button to erase it
    // First boot on an erased (or foreign) data partition: formatting it takes
    // a few seconds. Label-face title + caption hint (showNotice()).
    const char* fsPreparing;
    const char* fsPreparingHint;
    const char* errNoPack;      // no deck, and the access point would not start
    const char* errNoPackHint;
    const char* errOpenPack;    // no deck on the device opens
    const char* orientTitle;
    const char* orientRows[3];  // indexed by OrientPref: portrait, landscape, auto
    // Detail card (shown after a wrong answer). `detailOn`/`detailKun` label the
    // on'yomi/kun'yomi lines; the meanings label reuses quizMeaning.
    const char* detailTitle;
    const char* detailOn;
    const char* detailKun;
    const char* detailStrokesFmt;
    const char* detailAccuracyFmt;
    // Statistics: total time on questions (its tile shows the first unit
    // alone when both do not fit).
    const char* statsTimeHM;     // "%uh %um"
    const char* statsTimeMS;     // "%um %us"
    const char* statsTimeS;      // "%us"
    // UI-language picker: the title and the three rows (endonyms, same in every
    // table so a user can find their language whatever the current one is).
    const char* langTitle;
    const char* langRows[3];
    // First-boot language picker (drawFirstLang()): the hint bar's words, which
    // the picker takes from the table of the highlighted row's language.
    const char* firstLangTap;
    const char* firstLangHold;
    // Wi-Fi portal screen (the hints and status lines are 1x, see drawWifi()).
    const char* wifiTitle;
    const char* brightTitle;
    // Full-screen notices before a deep sleep: a short title (2x font, wrapped
    // at spaces, so no word longer than 10 characters) and a hint (1x font).
    const char* sleepTitle;
    const char* sleepHint;
    const char* batteryTitle;
    const char* batteryHint;
    // "Get decks" (last row of the deck list, a UI-font label): download decks
    // from the release catalog over the home Wi-Fi. Status lines are label-face
    // titles; notes and details are captions (21 characters a line in the
    // 135 px portrait; error details wrap).
    const char* getDecksRow;
    const char* getDecksTitle;
    const char* netConnecting;
    const char* netCatalog;
    // The hint bar's hold word for the focused deck: download, update, study;
    // and retry on the error screen. "update" is also the deck's caption.
    const char* netHoldDownload;
    const char* netHoldUpdate;
    const char* netHoldStudy;
    const char* netHoldRetry;
    const char* netSavedFmt;  // %s = deck name
    const char* netCardsFmt;  // a deck's caption: %s = language (upper case), %u = cards
    const char* netEmpty;
    const char* netNoWifi;
    const char* netNoWifiHint;
    const char* netNotFound;
    const char* netAuthFailed;
    const char* netNoConnect;
    const char* netNoServer;
    const char* netTls;
    const char* netHttpFmt;  // %d = HTTP status
    const char* netNotPublished;
    const char* netBadCatalog;
    const char* netNewerCatalog;
    const char* netOldCatalog;  // a catalog format older than kCatalogVersion (v1 catalog.txt)
    const char* netCorrupt;
    const char* netNoSpace;
    const char* netNoSpaceUpdate;  // detail: an update needs room for the old and the new copy
    const char* netInvalid;
    const char* netFailed;
    const char* netCancelled;
    const char* wifiGetDecksHint;  // hint bar: the boot-time screen's hold (with a home Wi-Fi)
    // Settings > Wi-Fi: the submenu (UI-font row labels, so accents are fine;
    // the titles are header text and stay ASCII).
    const char* wifiMenuRows[4];  // web portal, home network (its name is the value), setup mode, forget
    const char* wifiHomeNone;     // the home-network row's value when none is saved
    const char* wifiNoNetwork;    // note: no home network saved
    const char* wifiForgotten;    // note: the network was forgotten
    const char* forgetTitle;      // the confirmation picker's header
    const char* forgetRows[2];    // cancel, forget
    // The portal screen. Captions, 21 characters in the 135 px portrait (the
    // QR captions too); the hint bar's words are marked.
    const char* wifiScanJoin;      // over the join QR (setup mode)
    const char* wifiScanOpen;      // over the page QR
    const char* wifiTapOtherQr;    // hint bar, setup mode: a tap swaps the two QR codes
    const char* wifiViaFmt;        // home mode, %s = the home network
    const char* wifiConnectingTo;  // joining the home network for the portal (label face)
    const char* wifiHoldSetup;     // hint bar, joining / failed: a hold opens setup mode
    const char* wifiTapRetry;      // hint bar, failed: a tap tries again
    const char* wifiLinkLost;      // home mode: the link dropped, the driver reconnects
    const char* wifiStudyHint;     // hint bar, the side button on the boot-time screen with a deck
    const char* wifiDownloadFmt;   // %s = deck, %u = percent (a download from the page)
    const char* wifiFreeFmt;       // free space: %s free, %s total
    const char* wifiFallbackFmt;   // boot: home join failed, setup mode instead; network, reason
    // Settings > Wi-Fi > Home network: the connection test.
    const char* testTitle;
    const char* testChecking;
    const char* testIpFmt;       // %s = station IP
    const char* testSignalFmt;   // %d = dBm, %s = testQuality word
    const char* testQuality[4];  // weak, fair, good, excellent (rssiQuality())
    const char* testCatalogFmt;  // %u = decks in the catalog
    const char* testTimeOk;
    const char* testTimeFail;
    const char* testHoldAgain;  // hint bar: the hold
    const char* testNoNetworkHint;
    // Settings > Device > About (drawAbout()): the header title (ASCII), each
    // item's heading (label face, the colon dropped; at most 127 px) and the
    // deck item's value when none is loaded. The names and licences themselves
    // are in Credits.h.
    const char* aboutTitle;
    const char* aboutFirmware;  // + MIT
    const char* aboutDecks;     // + CC BY-SA, the licence of the published decks
    const char* aboutDeck;      // + the loaded deck's name
    const char* aboutNoDeck;
    const char* aboutKanji;     // + KANJIDIC2 ...
    const char* aboutStrokes;   // + KanjiVG ...
    const char* aboutJlpt;      // + Jonathan Waller ...
    const char* aboutGlyphs;    // + Noto Sans JP ...
    // The menus' hint bar (widgets::drawHintBar(), Font0, 6 px a character):
    // the front tap, the front hold (open a row / choose a value) and the side
    // button. The side word needs 6 px clear of the front items, else only its
    // icon shows: "• move ▬ open ◂ back" keeps it in the 135 px portrait,
    // "• mover ▬ abrir ◂" does not (2 px short).
    const char* hintMove;
    const char* hintOpen;
    const char* hintChoose;
    const char* hintBack;
    const char* getDecksCaption;  // the Get decks row's second line
    const char* aboutUiFont;      // Settings > About: + the firmware's UI font
    // The quiz and the detail card (docs/ux-refresh.md §6.4). Header pills are
    // Font0 captions in uppercase ASCII (Font0 has no capital I with an
    // accent). The result pill is "<word> · <days><unit>" or the longest
    // shorter form that fits next to the cards left (ui/QuizUi.h
    // resultPillText()); the mode pills reuse quizMeaning, detailOn/detailKun
    // and quizKanji. Budgets are tested in test_quizui, which copies these.
    const char* pillGood;
    const char* pillHard;
    const char* pillAgain;
    const char* pillDaysUnit;    // after the interval: "4d"
    const char* quizK2mCaption;  // over a meaning -> kanji prompt (Font0, two lines in landscape)
    // The quiz's hint bar (the front tap of a question is hintMove): the
    // hold that answers, then the tap after a correct answer (and on the
    // detail card) and after a wrong one. "• mover ▬ responder" leaves 8 px
    // to the settings icon in the 135 px portrait; "siguiente" would not.
    const char* hintAnswer;
    const char* hintNext;
    const char* hintDetails;
    // The detail card's header counter while the strokes animate (stroke,
    // count); the bare "4/13" when the worded one does not fit.
    const char* detailStrokeFmt;
    // --- Settings tree (docs/ux-refresh.md §3, §6.6; ui/SettingsTree.h) ------
    // Row labels are the UI font's Label face (18 px). Budgets in the 135 px
    // portrait: a home row has 93 px for its label, a 5 px gap and its value
    // (the icon and the chevron take the rest) and shows the value only when
    // both fit (Spanish "Estudio" and "Progreso" go without; "Estadísticas",
    // 99 px, did not fit at all); a Study / Device row puts its value on a
    // caption line under the label, so its label gets 107 px. Landscape rows
    // are wider: 110 px on the home, 212 on a page.
    const char* groupRows[5];   // decks, study, stats, Wi-Fi, device
    const char* studyRows[4];   // new per day, hard after, auto-advance, delete (progress)
    const char* deviceRows[7];  // orientation, brightness, language, hold time, date, sleep, about
    const char* studyTitle;     // headers (Font0, ASCII)
    const char* deviceTitle;
    const char* advanceTitle;
    const char* holdTitle;
    const char* studyValueFmt;  // the Study row's value, %u = new per day
    const char* wifiOff;        // the Wi-Fi row's value while the radio is off
    const char* decimalPoint;   // "1.5 s" / "1,5 s"
    const char* advanceRows[3];  // ui/InputPrefs.h order: off, short, normal
    const char* holdRows[3];     // short, normal, long
    const char* months[12];      // short names: the header's "18 Oct", the date tile's "OCT 2026"
    const char* choiceDefault;   // a picker row's value on the default choice
    // Pickers: a description under the header (Font0, wrapped to the width: at
    // most three lines of 21 characters in the 135 px portrait; in landscape
    // only when every row still fits, i.e. on the three-row pickers).
    const char* descNewPerDay;
    const char* descHardAfter;
    const char* descAdvance;
    const char* descHold;
    const char* descOrient;  // only with an accelerometer (it explains Auto)
    const char* descBright;
    const char* descLang;
    // The summary card: due (accent) and new on one caption line under the deck
    // name; landscape adds "seen 45/79" over the coverage bar (12 characters
    // in its 84 px column, else only the numbers).
    const char* summaryDueFmt;
    const char* summaryNewFmt;
    const char* summarySeenFmt;
    const char* summaryNoDeck;      // label face, no deck loaded
    const char* summaryNoDeckHint;  // caption under it
    const char* hintCancel;         // hint bar: the side button cancels
    const char* hintMore;           // hint bar: About's next page
    // --- Secondary screens (docs/ux-refresh.md §2.4, §4 phase 3) -------------
    // The date stepper's calendar tile: the weekday in the label face under
    // the day (at most 96 px: "Wednesday"; the tile is 100 px wide).
    const char* weekdaysLong[7];  // Sunday first
    const char* dateWasOff;       // the notice over "last used" at boot (caption, warn)
    // Under the pill (caption, wrapped to 21 characters, under the network
    // status when there is one): what to step to. The hint bar has the buttons.
    const char* dateExplain;
    const char* hintByHand;  // hint bar on "Getting the date": a tap sets it by hand
    // Statistics: the ring's caption, the six tiles' captions (Font0 in a
    // 62 px tile with 5 px padding: at most 9 characters) and the seen line.
    const char* statsAccuracy;
    const char* statsTiles[6];  // streak, best, due, new, learned, time
    const char* statsSeen;
    // Done screen: the title (label face), tomorrow's due count and the streak
    // (captions: 21 characters in the 135 px portrait), the button (label face)
    // and the hint bar's hold word.
    const char* doneTitle;
    const char* doneTomorrowFmt;  // %u = cards due by tomorrow
    const char* doneStreakFmt;    // %u days, 2 and up
    const char* doneStreakOne;
    const char* doneMore;
    const char* hintStudyMore;
    // Errors: the action after "Cannot open the deck" at boot, and on the quiz
    // screen when its deck went away (the side hold opens the settings).
    const char* errOpenPackHint;
    const char* errOpenPackMenu;
    const char* hintErase;     // hint bar: the damaged-storage screen's hold
    const char* hintSettings;  // hint bar: the side hold opens the settings
    // Get decks: the line under the header (caption: the network and the
    // catalog's size) and an installed deck's caption ("EN · on device", 19
    // characters with the language in the 135 px portrait).
    const char* netListFmt;  // %s = home network, %u = decks
    const char* netOnDevice;
    const char* hintStop;    // hint bar: the side button stops the portal
    const char* deckCardsFmt;  // the deck list's caption of a deck not loaded: %u cards
    const char* aboutReadings;  // About: + JmdictFurigana, wordfreq ... (the reading order)
    const char* aboutMeanings;  // About: + FreeDict/WikDict (the Spanish meaning order)
    // Settings > Study > Delete progress (docs/srs.md section 2.1). The Study
    // row is studyRows[3] ("Delete") over deleteRowValue ("progress"):
    // "Delete progress" (130 px) and "Borrar progreso" (133 px) do not fit a
    // portrait page row's 107 px. Its screen: the header (Font0, ASCII), two
    // rows (label face) with a caption (19 characters in the 135 px
    // portrait), the confirmation's text (captions, wrapped), the one row its
    // hold confirms on and the notes (captions, 21 characters).
    const char* deleteRowValue;
    const char* resetTitle;
    const char* resetRows[2];        // this deck, everything
    const char* resetAllCaption;     // under "Everything"
    const char* resetNoDeck;         // under "This deck" while no deck is loaded
    const char* resetNothing;        // note: there is no progress to delete
    const char* resetDeckFmt;        // %u = the deck's kanji with progress
    const char* resetSharedOneFmt;   // %s = the other deck, %u = kanji it shares
    const char* resetSharedManyFmt;  // %u = kanji other decks share
    const char* resetAllFmt;         // %u = every kanji with progress
    const char* resetConfirmRow;
    const char* hintDelete;          // hint bar: the confirmation's hold
    const char* resetDoneFmt;        // Study page note after a deck: %u kanji
    const char* resetAllDone;        // Study page note after Everything
};

const UiStrings kUiEn = {
    tgc::credits::kAppName,  // たまごっちょ, the header's Kana face
    "SETTINGS", "DECKS",
    "NEW/DAY", "HARD AFTER", "off",
    "MEANING", "READING", "KANJI",
    "DATE", "last used", "today", "+1 day", "+%u days", "-1 day", "-%u days",
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
    "Getting the date", "via %s...",
    "No network: set it by hand", "No network time: set it by hand",
    {"Date", "Zone"}, "none", "TIME ZONE", "From phone", "%s, summer %s",
    "open the web portal", "Time zone saved",
    "%u kanji · %u due",
    "STATS",
    "Storage damaged", "Hold the front button to erase it. Decks and progress are lost.",
    "Preparing storage...", "First start: this takes a few seconds.",
    "No decks yet", "The Wi-Fi to get one did not start. Turn the device off and on again.",
    "Cannot open the deck",
    "ORIENTATION", {"Portrait", "Landscape", "Auto"},
    "DETAIL", "ON", "KUN", "strokes %u", "correct %u/%u",
    "%uh %um", "%um %us", "%us",
    "LANGUAGE", {"Auto", "English", "Español"},
    "other", "OK",
    "WI-FI",
    "BRIGHTNESS",
    "Sleeping", "Progress saved. The front button wakes me.",
    "Battery low", "Charge me, then press the front button.",
    "Get decks", "GET DECKS", "Connecting...", "Reading catalog...",
    "get", "update", "study", "retry", "Saved %s", "%s · %u kanji",
    "No decks in the catalog",
    "No home Wi-Fi saved", "Settings > Wi-Fi > Setup mode, then save your network on the page",
    "Network not found", "Wrong password?", "Could not connect", "Server unreachable",
    "Secure connection failed", "Server error %d", "No catalog published at this URL yet",
    "Bad catalog", "Newer catalog format: update the firmware",
    "Old catalog format: set a catalog-v2.txt URL on the portal page", "Download corrupt",
    "Not enough space",
    "An update needs room for both copies. Delete the old one first: Settings > Wi-Fi > Web portal",
    "Not a valid deck", "Download failed", "Cancelled",
    "get decks",
    {"Web portal", "Home", "Setup mode", "Forget network"}, "none", "No home network saved",
    "Network forgotten", "FORGET WI-FI?", {"Cancel", "Forget"},
    "Scan to join", "Scan to open the page", "other QR", "via %s", "Connecting to",
    "setup", "retry", "Connection lost...", "study", "%s %u%%",
    "free %s / %s", "%s: %s",
    "HOME NETWORK", "Checking catalog...", "IP %s", "signal %d dBm (%s)",
    {"weak", "fair", "good", "excellent"}, "catalog: %u decks", "network time: ok",
    "network time: no answer", "test again", "Wi-Fi > Setup mode: save it on the page",
    "ABOUT", "Firmware:", "Decks:", "Deck:", "none", "Kanji data:", "Stroke order:",
    "JLPT levels:", "Glyphs:",
    "move", "open", "choose", "back", "home Wi-Fi",
    "UI font:",
    "GOOD", "HARD", "AGAIN", "d", "which kanji means",
    "answer", "next", "details",
    "stroke %u/%u",
    {"Decks", "Study", "Stats", "Wi-Fi", "Device"},
    {"New per day", "Hard after", "Auto next", "Delete"},
    {"Orientation", "Brightness", "Language", "Hold time", "Date", "Sleep", "About"},
    "STUDY", "DEVICE", "AUTO NEXT", "HOLD TIME",
    "%u/day", "off", ".",
    {"Off", "Short", "Normal"},
    {"Short", "Normal", "Long"},
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
    "default",
    "New kanji added to each day's reviews.",
    "Slower correct answers count as Hard.",
    "After a correct answer, the next card comes by itself.",
    "How long to hold a button to choose.",
    "Auto turns the screen as you hold the device.",
    "Lower saves battery.",
    "Auto follows the loaded deck.",
    "%u due", "%u new", "seen %u/%u", "No deck", "Get one in Decks",
    "cancel", "more",
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
    "Device was off.",
    "Hold when it shows today.",
    "by hand",
    "accuracy",
    {"streak", "best", "due", "new", "learned", "time"},
    "seen",
    "All caught up", "tomorrow: %u due", "%u-day streak", "1-day streak", "+10 new",
    "study more",
    "Update the firmware from the release page.",
    "Choose another deck in Settings > Decks.",
    "erase", "settings",
    "%s · %u decks", "on device", "stop",
    "%u kanji",
    "Readings:",
    "Meanings:",
    "progress",
    "DELETE PROGRESS",
    {"This deck", "Everything"},
    "and the statistics",
    "no deck loaded",
    "Nothing to delete",
    "Forgets %u kanji: they start again as new.",
    "%s shares %u of them: they reset there too.",
    "Other decks share %u of them: they reset there too.",
    "Forgets %u kanji and the statistics: answers, accuracy, time, streaks. Decks and settings "
    "stay.",
    "Delete",
    "delete",
    "Deleted: %u kanji",
    "Progress deleted",
};

const UiStrings kUiEs = {
    tgc::credits::kAppName,
    "AJUSTES", "MAZOS",
    "NUEVAS/DIA", "DIFICIL TRAS", "no",
    "SIGNIFICADO", "LECTURA", "KANJI",
    "FECHA", "último uso", "hoy", "+1 día", "+%u días", "-1 día", "-%u días",
    {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"},
    "Obteniendo la fecha", "de %s...",
    "Sin red: ajústala a mano", "Sin hora de red: ajústala a mano",
    {"Fecha", "Zona"}, "ninguna", "ZONA HORARIA", "Del móvil", "%s, verano %s",
    "abre el portal web", "Zona horaria guardada",
    "%u kanji · %u hoy",
    "PROGRESO",
    "Memoria dañada", "Mantén el botón frontal para borrarla. Se pierden mazos y progreso.",
    "Preparando memoria...", "Primer arranque: tarda unos segundos.",
    "Aún no hay mazos", "No arrancó el Wi-Fi para bajar uno. Apaga y enciende el aparato.",
    "No se puede abrir el mazo",
    "ORIENTACION", {"Vertical", "Horizontal", "Auto"},
    "DETALLE", "ON", "KUN", "trazos %u", "aciertos %u/%u",
    "%uh %um", "%um %us", "%us",
    "IDIOMA", {"Auto", "English", "Español"},
    "otro", "OK",
    "WI-FI",
    "BRILLO",
    "Suspendido", "Progreso guardado. El botón frontal me despierta.",
    "Batería baja", "Cárgame y pulsa el botón frontal.",
    "Descargar", "DESCARGAR", "Conectando...", "Leyendo catálogo...",
    "bajar", "actualizar", "estudiar", "repetir",
    "Guardado %s", "%s · %u kanji",
    "El catálogo está vacío",
    "Sin Wi-Fi de casa", "Ajustes > Wi-Fi > Modo configuración, y guarda tu red en la página",
    "Red no encontrada", "¿Contraseña incorrecta?", "No se pudo conectar", "Servidor inaccesible",
    "Fallo de conexión segura", "Error del servidor %d", "Aún no hay catálogo en esta URL",
    "Catálogo no válido", "Catálogo más nuevo: actualiza el firmware",
    "Catálogo antiguo: pon una URL de catalog-v2.txt en la página del portal", "Descarga dañada",
    "No hay espacio",
    "Hace falta sitio para las dos copias. Borra antes la vieja: Ajustes > Wi-Fi > Portal web",
    "Mazo no válido", "Descarga fallida", "Cancelado",
    "descargar",
    {"Portal web", "Casa", "Modo configuración", "Olvidar red"}, "ninguna",
    "No hay red de casa guardada", "Red olvidada", "OLVIDAR WI-FI?", {"Cancelar", "Olvidar"},
    "Escanea y únete", "Escanea y abre la web", "otro QR", "vía %s",
    "Conectando a", "config.", "repetir", "Conexión perdida...",
    "estudiar", "%s %u%%", "libre %s / %s", "%s: %s",
    "RED DE CASA", "Comprobando catálogo...", "IP %s", "señal %d dBm (%s)",
    {"débil", "regular", "buena", "excelente"}, "catálogo: %u mazos", "hora de red: ok",
    "hora de red: sin respuesta", "repetir", "Wi-Fi > Modo configuración: guárdala en la página",
    "ACERCA DE", "Firmware:", "Mazos:", "Mazo:", "ninguno", "Datos de kanji:", "Orden de trazos:",
    "Niveles JLPT:", "Glifos:",
    "mover", "abrir", "elegir", "atrás", "Wi-Fi de casa",
    "Fuente:",
    "BIEN", "DIFICIL", "OTRA VEZ", "d", "qué kanji significa",
    "responder", "siguiente", "detalles",
    "trazo %u/%u",
    {"Mazos", "Estudio", "Progreso", "Wi-Fi", "Dispositivo"},
    {"Nuevas/día", "Difícil tras", "Pasar solo", "Borrar"},
    {"Orientación", "Brillo", "Idioma", "Pulsación", "Fecha", "Suspender", "Acerca de"},
    "ESTUDIO", "DISPOSITIVO", "PASAR SOLO", "PULSACION",
    "%u/día", "apagado", ",",
    {"No", "Rápido", "Normal"},
    {"Corta", "Normal", "Larga"},
    {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"},
    "por defecto",
    "Kanji nuevos que entran cada día.",
    "Un acierto más lento cuenta como Difícil.",
    "Tras un acierto, la siguiente tarjeta llega sola.",
    "Cuánto mantener pulsado un botón para elegir.",
    "Auto gira la pantalla según sostengas el aparato.",
    "Más bajo ahorra batería.",
    "Auto sigue el mazo cargado.",
    "%u hoy", "%u nuevas", "vistos %u/%u", "Sin mazo", "Bájalo en Mazos",
    "cancelar", "más",
    {"Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado"},
    "Estuvo apagado.",
    "Mantén cuando sea hoy.",
    "a mano",
    "precisión",
    {"racha", "mejor", "hoy", "nuevas", "dominadas", "tiempo"},
    "vistos",
    "Todo al día", "%u para mañana", "racha de %u días", "racha de 1 día", "+10 nuevas",
    "estudiar más",
    "Actualiza el firmware desde la página de versiones.",
    "Elige otro mazo en Ajustes > Mazos.",
    "borrar", "ajustes",
    "%s · %u mazos", "en el equipo", "parar",
    "%u kanji",
    "Lecturas:",
    "Significados:",
    "progreso",
    "BORRAR PROGRESO",
    {"Este mazo", "Todo"},
    "y las estadísticas",
    "ningún mazo cargado",
    "Nada que borrar",
    "Olvida %u kanji: vuelven a ser nuevos.",
    "%s comparte %u: allí también se reinician.",
    "Otros mazos comparten %u: allí también se reinician.",
    "Olvida %u kanji y las estadísticas: respuestas, precisión, tiempo, rachas. Mazos y ajustes "
    "se quedan.",
    "Borrar",
    "borrar",
    "Borrados: %u kanji",
    "Progreso borrado",
};

const UiStrings* gUi = &kUiEn;  // English until a pack says otherwise
const UiStrings& ui() { return *gUi; }

// NVS `tangocho`/`ui`: the UI-language preference (Auto/English/Spanish). Auto,
// the default, follows the pack language exactly as before.
int gUiLangPref = tgc::kUiLangAuto;

// "es" selects the Spanish table; anything else (including no pack yet) is
// English. The user preference (see UiLang.h) decides when Auto is not set.
void setUiForLang(const char* lang) {
    gUi = (std::strcmp(tgc::uiLangCodeFor(gUiLangPref, lang), "es") == 0) ? &kUiEs : &kUiEn;
}

// The language the screens are in now ("en", "es"): the Language setting, or
// the pack's language under Auto, as setUiForLang() last resolved it.
const char* uiLangCode() { return gUi == &kUiEs ? "es" : "en"; }

lgfx::LGFX_Sprite* gCanvas = nullptr;  // full-screen back buffer in PSRAM
std::unique_ptr<tgc::PackReader> gPack = std::make_unique<tgc::PackReader>();
std::unique_ptr<tgc::FsPackSource> gSource;
tgc::Renderer gRender;
tgc::XorShiftRng gRng;

tgc::Question gQ;
int gSel = 0;
bool gRevealed = false;
bool gLastCorrect = false;  // outcome of the last answer (drives the detail card)
tgc::ReadingTurn gReadingTurn;  // on'yomi and kun'yomi questions take turns
// The last answer's grade and the days to the card's next review (record
// dueDay - today), for the result pill.
tgc::Grade gLastGrade = tgc::Grade::Good;
int gLastInterval = 0;
// The quiz's hint bar policy and the auto-advance after a correct answer
// (ui/QuizUi.h; docs/ux-refresh.md §6.3, §6.5).
tgc::QuizHints gQuizHints;
tgc::AutoAdvance gAdvance;
uint32_t gAdvanceFrameAt = 0;  // millis() of the last countdown frame

// Stroke-order animation on the mistake detail card. Only active while the
// shown card's pack carries stroke data (docs/pack-format.md); otherwise the
// card is drawn exactly as before.
bool gAnimOn = false;
uint16_t gAnimStrokes = 0;
int gAnimFrame = 0;
uint32_t gAnimNext = 0;
constexpr uint32_t kStrokeMs = 550;       // one stroke step
constexpr uint32_t kStrokePauseMs = 1200; // hold on the last stroke before looping

tgc::SrsStore gSrs;
tgc::FsSrsFile gSrsIo;
tgc::Queue gQueue;
uint32_t gShownAt = 0;     // millis() when the question was drawn
int gNavTaps = 0;          // navigation taps on the current question
// Response time credited back per navigation tap: with one button, reaching
// the 4th option costs three taps that are UI cost, not recall time.
constexpr uint32_t kNavAllowanceMs = 400;
uint32_t gLastAnswerAt = 0;
int gDirty = 0;            // answers not yet persisted
// "Study more" override for the daily new-card cap (the done screen's tap), and
// the SRS day it was granted on: it only applies on that day (extraNewToday()).
uint16_t gExtraNew = 0;
uint16_t gExtraNewDay = 0;

enum class Screen {
    Quiz, Menu, Decks, Choice, Done, Date, Stats, Detail, Wifi, GetDecks, WifiMenu, NetTest, About,
    FirstLang, DateMenu, TzList, Reset, ResetConfirm
};
// Screen::Choice is shared by the row pickers (see openChoice()).
enum class Choice {
    NewPerDay, SlowMs, Orientation, UiLang, Brightness, ForgetWifi, AutoAdvance, HoldTime
};
Screen gScreen = Screen::Quiz;
Screen gBeforeMenu = Screen::Quiz;
Screen gAfterDate = Screen::Quiz;  // where the date screen returns to
// Days from the current day's date to the proposed "today" (negative =
// earlier), within gSrs.manualStepsMin()..manualStepsMax().
int32_t gDateSteps = 0;
uint16_t gBootDay = 0;  // persisted day index at boot: "last used" on the date screen
// A real wall-clock time was applied since boot (applyWallClock), so the
// power-cut date question is not needed.
bool gWallClockSynced = false;
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
tgc::TzRule gTz;                // the zone in use (valid while gTzKnown)
bool gTzKnown = false;
bool gTzManual = false;         // gTz came from Settings > Device > Date > Zone
char gTzName[48] = "";          // its IANA name, "" when unknown
char gTzRuleText[tgc::kTzRuleMaxLen + 1] = "";
tgc::TzRule gPhoneTz;           // the zone the page last sent (the "From phone" row)
bool gPhoneTzKnown = false;
char gPhoneTzName[48] = "";
// NTP over the home Wi-Fi gets this long once the network is up.
constexpr uint32_t kNtpTimeoutMs = 3000;
// Network time for the boot-time date question (docs/srs.md section 5.1): with a
// home Wi-Fi and a time zone known, the date screen first joins and asks NTP,
// on a screen of its own ("Getting the date via ...", a tap gives up); the
// stepper follows only when that fails or is skipped. Tried once per boot. The
// radio (gHome, shared with Get decks, which cannot be opened from this screen)
// is on only while Joining; the other states are the stepper's status line.
enum class DateSync : uint8_t { None, Joining, NoNetwork, NoTime };
DateSync gDateSync = DateSync::None;
std::vector<String> gPacks;  // paths of the *.tgc files in /packs
// The list screens' focus and scroll position are tgc::ListViews next to
// their draw functions (gMenuLists, gDeckList, gChoiceList, ...).
Choice gChoice = Choice::NewPerDay;
int gChoiceCount = 0;
uint16_t gChoiceValues[4] = {0, 0, 0, 0};
String gCurrentPack;
Preferences gPrefs;

// Orientation setting, NVS `tangocho`/`orient` (it replaced the bool `land`,
// see loadOrientPref()). Auto is offered only on boards with an accelerometer:
// the IMU then picks rotation 0, 1 or 3 (imu/OrientationFilter) and the picked
// rotation is never persisted. Values double as orientRows indexes.
enum OrientPref : uint8_t { kOrientPortrait = 0, kOrientLandscape = 1, kOrientAuto = 2 };
uint8_t gOrientPref = kOrientAuto;
tgc::OrientationFilter gOrient;
constexpr uint32_t kImuPollMs = 100;  // accelerometer poll while Auto is on
uint32_t gImuPollAt = 0;
float gAccel[3] = {0, 0, 0};          // last sample, for the rotation log

// On-device Wi-Fi portal (roadmap phase 4). Started on the Wi-Fi screen and
// stopped on exit. `gWifiBootUpload` marks the boot-time "no packs yet" state:
// the Wi-Fi screen is then the only screen (there is no quiz to return to), so
// the side button is inert and the app moves to the quiz as soon as a pack has
// been uploaded. It is set only at boot -- entering the portal later from the
// settings home and then deleting every pack must still let the side button
// leave for the settings home.
tgc::WifiPortal gPortal;
bool gWifiBootUpload = false;
// The portal's two modes (WifiPortal.h): home = the page over the saved home
// network, which the screen joins first (gHome); setup = the device's own open
// access point. Home is the default once a network is saved.
enum class PortalMode : uint8_t { Setup, Home };
// The Wi-Fi screen: joining the home network, serving the page, or a failed
// join (or access point) with its reason and the way to setup mode.
enum class WifiStep : uint8_t { Joining, Serving, Failed };
PortalMode gPortalMode = PortalMode::Setup;
WifiStep gWifiStep = WifiStep::Serving;
int gQrPage = 0;                      // setup mode: 0 = join the AP, 1 = open the page
char gWifiNote[64] = "";              // setup mode after the boot-time home join failed: why
// Bumped whenever the Wi-Fi screen changes under loop()'s feet (a mode switch,
// a blocking download from the page, leaving for Get decks), so the input read
// before the portal's poll() is not applied to what came after it.
uint32_t gWifiEpoch = 0;

// Settings > Wi-Fi (the submenu replacing the single settings row).
tgc::ListView gWifiMenuList;
String gWifiMenuSsid;          // the saved home network, read when the menu opens
char gWifiMenuNote[48] = "";   // footer note (cleared when the selection moves)
uint16_t gWifiMenuNoteColor = 0;
// Settings > Wi-Fi > Home network: join, show the IP and the signal, read the
// catalog (and the network time), radio off again.
enum class NetTestStep : uint8_t { NoNetwork, Joining, Checking, Done, Failed };
NetTestStep gTestStep = NetTestStep::NoNetwork;
String gTestIp;
int gTestRssi = 0;
int gTestTime = -1;            // network time: -1 not tried (no time zone), 0 failed, 1 ok
char gTestCatalog[64] = "";
uint16_t gTestCatalogColor = 0;
char gTestFail[48] = "";

// "Get decks" (roadmap phase 4): the last row of the deck list. Joins the saved
// home Wi-Fi on demand (HomeWifi), reads the release catalog over HTTPS and
// downloads the chosen deck into /packs (NetFetch). The radio is on only while
// this screen is, at kCpuWifiMhz. Connecting is polled from loop() so the side
// button can cancel; the catalog read and the download block, the latter with a
// progress callback that redraws, keeps the idle timers away and lets the side
// button cancel.
enum class NetStep { Connecting, Catalog, List, Error };
tgc::HomeWifi gHome;
NetStep gNetStep = NetStep::Connecting;
bool gNetFromBoot = false;   // opened from the boot-time upload screen
std::string gGetDecksLangHint;  // the phone's language from the portal page, or ""
std::vector<tgc::CatalogEntry> gCatalog;
std::vector<tgc::DeckState> gCatalogState;  // per gCatalog entry
int gNetSel = 0;
bool gNetBusy = false;       // a download is running (footer shows the progress)
int gNetPct = -1;            // last drawn download percentage
char gNetDeck[72] = "";      // the title of the deck being downloaded (the portal screen shows it)
uint32_t gNetActivityAt = 0; // millis() of the last network progress (idle timers)
uint32_t gNetInputAt = 0;    // last button poll during a blocking download
char gNetErrTitle[48] = "";
char gNetErrDetail[96] = "";
char gNetNote[48] = "";      // footer status line (cleared when the selection moves)
uint16_t gNetNoteColor = 0;
// A sleep asked for during a blocking download (power button, critical
// battery): netProgress() cancels the download, and netDownload() sleeps once
// fetchPackFile() has cleaned up after it.
enum class NetSleep : uint8_t { None, User, Critical };
NetSleep gNetSleep = NetSleep::None;
// TLS stalls are longer than on the local portal; a big pack may take minutes.
constexpr uint32_t kNetIdleMs = 15000;
constexpr uint32_t kNetDownloadMs = 10u * 60u * 1000u;

// Power management. After kDimAfterMs without input the backlight dims; after
// kSleepAfterMs (kWifiIdleMs while the portal is up and no HTTP request came
// in) the state is saved and, on battery, the chip deep-sleeps until the front
// button wakes it. On USB power only the panel goes dark, so the serial port
// stays usable for flashing and logs; unplugging then lets it sleep.
constexpr uint32_t kDimAfterMs = 30000;
constexpr uint32_t kSleepAfterMs = 120000;
constexpr uint32_t kWifiIdleMs = 300000;
constexpr uint32_t kPowerCheckMs = 5000;  // VBUS poll while the panel is off
// 80 MHz is plenty for the quiz; Wi-Fi gets full speed while the portal runs.
constexpr uint32_t kCpuMhz = 80;
constexpr uint32_t kCpuWifiMhz = 240;
constexpr uint8_t kBrightnessDefault = 128;
uint8_t gBrightness = kBrightnessDefault;  // NVS `tangocho`/`bri`
uint8_t gAdvancePref = tgc::kAdvanceDefault;  // NVS `tangocho`/`adv` (ui/InputPrefs.h)
uint8_t gHoldPref = tgc::kHoldDefault;        // NVS `tangocho`/`hold`
uint32_t gLastInputAt = 0;      // millis() of the last button activity
uint32_t gLastPowerCheck = 0;
bool gDimmed = false;
bool gScreenOff = false;        // idle on USB power: panel off, still running
bool gSwallowInput = false;     // drop button events until all buttons are up
bool gSrsLoaded = false;        // the state file loaded at boot
// The quiz header's battery figure, re-read every kBatteryCheckMs.
constexpr uint32_t kBatteryCheckMs = 30000;
int gShownBattery = -2;
uint32_t gBatteryCheckAt = 0;
// Critical battery (power/BatteryGuard.h): one reading every kBatteryGuardMs on
// every screen, so three low readings put the device to sleep within ~30 s.
constexpr uint32_t kBatteryGuardMs = 10000;
tgc::BatteryGuard gBatteryGuard;
uint32_t gBatteryGuardAt = 0;
// How long the notice before an explicit sleep stays up.
constexpr uint32_t kSleepNoticeMs = 1500;
constexpr uint32_t kBatteryNoticeMs = 2500;

int W, H;

void smallText(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t fg, uint16_t bg,
               lgfx::textdatum_t datum = lgfx::top_left) {
    d.setFont(&lgfx::fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(fg, bg);
    d.setTextDatum(datum);
    d.drawString(s, x, y);
}

void dropPartialPush();

// Microseconds and rows of the last push, for the TGC_QUIZ_LOG frame trace
// in drawScreen().
uint32_t gPushUs = 0;
int gPushRows = 0;

// Every push notes what the hold progress (holdTick()) needs about the frame
// now on the panel: the focused list row drawList() drew for it (if any), a
// frame count and whether the push was partial.
uint32_t gFrames = 0;
bool gLastPushPartial = false;
tgc::widgets::Rect gFrameFocus{};
bool gFrameFocusValid = false;
// One-shot: the next present() pushes only gPushRect (a list's hold-progress
// frame redraws its screen clipped to the focused row).
bool gPushOnly = false;
tgc::widgets::Rect gPushRect{};

void noteFrame(bool partial) {
    gFrameFocusValid = tgc::widgets::takeFocusRow(&gFrameFocus);
    ++gFrames;
    gLastPushPartial = partial;
}
void presentRegion(int x, int y, int w, int h);

void present() {
    dropPartialPush();
    if (gPushOnly) {
        gPushOnly = false;
        presentRegion(gPushRect.x, gPushRect.y, gPushRect.w, gPushRect.h);
        return;
    }
    noteFrame(false);
    const uint32_t t0 = micros();
    gCanvas->pushSprite(&tgc::board::display(), 0, 0);
    gPushUs = micros() - t0;
    gPushRows = H;
}

// Pushes only (x, y, w, h) of the back buffer: a partial update.
void presentRegion(int x, int y, int w, int h) {
    noteFrame(true);
    const uint32_t t0 = micros();
    tgc::widgets::presentRegion(*gCanvas, tgc::board::display(), x, y, w, h);
    gPushUs = micros() - t0;
    gPushRows = h;
}

// (Re)create the full-screen back buffer for the current W x H, PSRAM first with
// an internal-RAM fallback. Used at boot and on an orientation change.
void createCanvas() {
    auto& disp = tgc::board::display();
    if (gCanvas) {
        delete gCanvas;
        gCanvas = nullptr;
    }
    gCanvas = new lgfx::LGFX_Sprite(&disp);
    gCanvas->setColorDepth(16);
    gCanvas->setPsram(true);
    if (!gCanvas->createSprite(W, H)) {
        gCanvas->setPsram(false);  // fall back to internal RAM
        gCanvas->createSprite(W, H);
    }
}

// Error screen (defined with the menu helpers): what happened, the one thing
// to do, and the hint bar when a button does something here.
void showError(const char* title, const char* action, const tgc::widgets::Hints* hints = nullptr);

// Fill `out` with the *.tgc files in /packs (sorted for a stable menu order).
void listPacks(std::vector<String>& out) {
    out.clear();
    File dir = LittleFS.open("/packs");
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!f.isDirectory() && name.endsWith(".tgc") && !name.startsWith("."))
            out.push_back("/packs/" + name);
    }
    std::sort(out.begin(), out.end());
}

const char* gradeName(tgc::Grade g) {
    switch (g) {
        case tgc::Grade::Again: return "again";
        case tgc::Grade::Hard: return "hard";
        case tgc::Grade::Good: return "good";
    }
    return "?";
}

// The loaded deck's counts (ui/DeckSummary.h): one pass over the card table,
// for the boot log, the settings summary, the deck list, the statistics and
// the done screen.
tgc::DeckSummary deckCounts() { return tgc::deckSummary(*gPack, gSrs); }

// The "study more" override in force. It belongs to the day it was granted, so
// any change of the day index (uptime rollover, sleep time, a manual date, a
// real-time sync) drops it without each of those paths having to.
uint16_t extraNewToday() {
    if (gExtraNewDay != gSrs.today()) gExtraNew = 0;
    return gExtraNew;
}

// Pops the next queue item and builds a question for it, trying the item's
// mode first and then the other two (a card may lack readings/meanings).
// False when the queue is exhausted.
bool newQuestion() {
    static const tgc::QuizMode kModes[] = {tgc::QuizMode::Meaning, tgc::QuizMode::Reading,
                                           tgc::QuizMode::MeaningToKanji};
    constexpr int kN = 3;
    for (;;) {
        tgc::QueueItem item;
        if (!gQueue.next(&item)) return false;
        // Ask for on'yomi and kun'yomi in turn, so both kinds show up.
        const tgc::ReadingKind want = gReadingTurn.want();
        for (int k = 0; k < kN; ++k) {
            const tgc::QuizMode mode = kModes[(static_cast<int>(item.mode) + k) % kN];
            if (!tgc::buildQuestion(*gPack, item.cardIndex, mode, gRng, &gQ, want)) continue;
            if (gQ.mode == tgc::QuizMode::Reading) gReadingTurn.asked(gQ.readingKind);
            gSel = 0;
            gRevealed = false;
            gShownAt = millis();
            gNavTaps = 0;
            gQuizHints.question(gShownAt);
            gAdvance.cancel();
            tgc::Card c;
            gPack->card(gQ.cardIndex, &c);
            const char* mk = gQ.mode == tgc::QuizMode::Reading
                                 ? (gQ.readingKind == tgc::ReadingKind::Kun ? "read-kun" : "read-on")
                                 : (gQ.mode == tgc::QuizMode::MeaningToKanji ? "k2m" : "mean");
            QLOG("Q %s mode=%d U+%04X", mk, static_cast<int>(gQ.mode), (unsigned)c.codepoint);
            for (int i = 0; i < gQ.count; ++i)
                QLOG(" |%s %s", i == gQ.correct ? "*" : " ", gQ.options[i].c_str());
            QLOG("\n");
            return true;
        }
        // Card cannot be asked in any mode: skip it and try the next item.
    }
}

void drawScreen();  // dispatches to the active screen
void applyRotation(uint8_t r);
void drawGetDecks();
void drawWifiMenu();
void drawNetTest();
void drawReset();
void drawResetConfirm();
void openNetTest();
void formatDate(uint16_t days, char* out, size_t n);
[[noreturn]] void sleepNow(bool critical);
void netRadioOff();
void netSyncClock();
void openDecks();
void openGetDecks(bool fromBoot);
void openPortal(PortalMode mode);
void openDateMenu(bool fresh);
tgc::DeckDownloadResult downloadCatalogDeck(const tgc::CatalogEntry& e);

// The card of the question on screen. False when there is none to draw or
// grade: no pack open, no question built yet, or an index the open pack does
// not have. (A question left over from a pack the portal or Get decks replaced
// is replaced itself, in portalPacksChanged().)
bool questionCard(tgc::Card* out) {
    return gPack->isOpen() && gQ.count > 0 && gPack->card(gQ.cardIndex, out);
}

// Stand-in for the quiz or detail screen when questionCard() fails (a deck that
// could not be loaded): says so and how to get out, since a side hold opens
// the settings home on both screens.
void drawNoQuestion() {
    tgc::widgets::Hints h;
    h.side = &tgc::kIconMenu;
    h.sideWord = ui().hintSettings;
    showError(ui().errOpenPack, ui().errOpenPackMenu, &h);
}

// The quiz prompt's set-0 kanji. With TGC_QUIZ_LOG it also traces the draw
// cost: the whole call and the bitmap fetch inside it (file read plus the v2
// decode; 0 when the renderer still held this glyph from the last redraw).
void drawPromptKanji(lgfx::LovyanGFX& d, uint32_t cp, int x, int y, int w, int h) {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, x, y, w, h, kWhite, kBlack);
#if defined(TGC_QUIZ_LOG)
    const uint32_t us = micros() - t0;
    const tgc::Glyph* g = gPack->findGlyph(tgc::kGlyphSetLarge, cp);
    QLOG("R U+%04X kanji %ux%u enc=%u draw %u us (fetch %u us)\n", (unsigned)cp,
         g ? (unsigned)g->width : 0u, g ? (unsigned)g->height : 0u, g ? (unsigned)g->encoding : 0u,
         (unsigned)us, (unsigned)gRender.lastFetchUs());
#endif
}

// --- Quiz screen (docs/ux-refresh.md §2.4, §6) ------------------------------
// Header: the mode pill (the result pill once answered), the cards left, the
// battery and the 2 px session bar. Prompt: the kanji, or for meaning -> kanji
// a caption and the meaning. Options: flat tiles (render/Widgets). The hint bar
// follows ui/QuizUi.h's policy (QuizHints); without it the tiles take its space.

namespace wg = tgc::widgets;
using tgc::theme::kGutter;
using tgc::theme::kHeaderH;
using tgc::theme::kHintH;

// Where the quiz puts its parts (drawQuiz(), the focus moves and hold frames,
// the detail card's kanji box). Meanings are rows in the UI font. Readings are
// a 2x2 grid when every option fits a cell -- in the option kana (set 3), else
// the small kana (set 2) -- otherwise rows, in set 3 when every option fits a
// row. Kanji options are a 2x2 grid of set-1 glyphs.
struct QuizGeom {
    int px = 0, py = 0, pw = 0, ph = 0;  // prompt box
    int tx[tgc::kMaxOptions] = {}, ty[tgc::kMaxOptions] = {};
    int tw = 0, th = 0;  // every tile's size
    uint8_t set = tgc::Renderer::kTextUi;
    bool grid = false;
    bool hints = false;  // the hint bar is shown
};
QuizGeom gQuizG;  // as the quiz or the detail card was last drawn

QuizGeom quizGeom(bool hints) {
    using namespace tgc::theme;
    const Layout l = layoutFor(W, H);
    QuizGeom g;
    g.hints = hints;
    g.px = l.promptX;
    g.py = l.promptY;
    g.pw = l.promptW;
    g.ph = l.promptH;
    int ax = l.answersX, ay = l.answersY, aw = l.answersW, ah = l.answersH;
    const int extra = hints ? 0 : kHintH;
    if (!l.landscape) {
        // A larger em (--large-px) gets a taller kanji box, up to 45 % of H.
        const int largePx = gPack->header().largePx ? gPack->header().largePx : 64;
        const int want = std::min(largePx + 6, H * 45 / 100);
        if (want > g.ph) {
            ay += want - g.ph;
            ah -= want - g.ph;
            g.ph = want;
        }
        ah += extra;
    } else {
        g.ph += extra;
        ah += extra ? extra - 3 : 0;  // 4 px from the bottom edge, as on the right
    }
    const int n = gQ.count > 0 ? std::min(gQ.count, tgc::kMaxOptions) : 1;
    // The grid keeps 2 px to the panel edges (rows keep the 4 px gutter): a
    // portrait cell then holds three option kana.
    const int gx = ax - 2, gw = aw + 4;
    const int rows = (n + 1) / 2;
    const int cw = (gw - kGap) / 2;
    const int ch = (ah - (rows - 1) * kGap) / rows;
    auto allFit = [&](uint8_t set, int w) {
        for (int i = 0; i < n; ++i)
            if (gRender.textWidth(gQ.options[i].c_str(), set) > w) return false;
        return true;
    };
    if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
        g.grid = true;
        g.set = tgc::kGlyphSetSmall;
    } else if (gQ.mode == tgc::QuizMode::Reading) {
        g.set = tgc::kGlyphSetOption;
        g.grid = allFit(g.set, cw - 4);
        if (!g.grid && allFit(tgc::kGlyphSetText, cw - 4)) {
            g.set = tgc::kGlyphSetText;
            g.grid = true;
        }
        if (!g.grid && !allFit(g.set, aw - kTilePadX - 6)) g.set = tgc::kGlyphSetText;
    }
    if (g.grid) {
        g.tw = cw;
        g.th = ch;
        for (int i = 0; i < n; ++i) {
            g.tx[i] = gx + (i % 2) * (cw + kGap);
            g.ty[i] = ay + (i / 2) * (ch + kGap);
        }
    } else {
        g.tw = aw;
        g.th = (ah - (n - 1) * kGap) / n;
        for (int i = 0; i < n; ++i) {
            g.tx[i] = ax;
            g.ty[i] = ay + i * (g.th + kGap);
        }
    }
    return g;
}

bool quizHintsShown() { return gQuizHints.shown(gSrs.answeredTotal()); }

// Hold progress on the quiz (see holdTick()): a fill frame is on screen, and
// its share, so a full redraw during the hold keeps it.
bool gHoldOn = false;
float gHoldShare = 0.0f;
uint32_t gHoldFrameAt = 0;  // millis() of the last fill frame

wg::TileState quizTileState(int i) {
    if (gRevealed) {
        if (i == gQ.correct) return wg::TileState::Correct;
        return i == gSel ? wg::TileState::Wrong : wg::TileState::Dimmed;
    }
    return i == gSel ? wg::TileState::Focus : wg::TileState::Idle;
}

// Option tile `i` where gQuizG puts it; `fill` >= 0 draws the focused tile
// holding (hold progress).
void drawQuizTile(lgfx::LovyanGFX& d, int i, float fill) {
    const QuizGeom& g = gQuizG;
    wg::TileState st = quizTileState(i);
    if (fill >= 0.0f && st == wg::TileState::Focus) st = wg::TileState::Holding;
    if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
        tgc::Card oc;
        const uint32_t cp = gPack->card(gQ.optionCard[i], &oc) ? oc.codepoint : '?';
        wg::drawKanjiTile(d, gRender, g.tx[i], g.ty[i], g.tw, g.th, st, cp, g.set, fill);
        return;
    }
    wg::drawTile(d, gRender, g.tx[i], g.ty[i], g.tw, g.th, st, gQ.options[i].c_str(), fill, g.set,
                 g.grid ? tgc::Align::Centre : tgc::Align::Left);
}

// The header: before the answer the mode pill in its colour -- MEANING, the
// reading asked for (ON / KUN) or KANJI -- and after it the result: a check
// and "GOOD · 4d" / "HARD · 2d", or a cross and "AGAIN", in the longest form
// that fits next to the cards left (ui/QuizUi.h). Right: the cards left and
// the battery; bottom: answered / (answered + left).
void drawQuizHeader(lgfx::LovyanGFX& d) {
    using namespace tgc::theme;
    const tgc::SessionProgress p =
        tgc::sessionProgress(static_cast<uint32_t>(gQueue.answeredCount()),
                             static_cast<uint32_t>(gQueue.remaining()), !gRevealed);
    char left[12];
    snprintf(left, sizeof left, "%u", (unsigned)p.left);
    const int bat = tgc::board::batteryPercent();
    gShownBattery = bat;
    wg::Header h;
    h.right = left;
    h.battery = bat;
    h.progressNum = static_cast<int>(p.done);
    h.progressDen = static_cast<int>(p.total);
    h.pillBg = kSurfaceHi;
    char pill[40];
    if (!gRevealed) {
        if (gQ.mode == tgc::QuizMode::Reading) {
            h.pill = gQ.readingKind == tgc::ReadingKind::Kun  ? ui().detailKun
                     : gQ.readingKind == tgc::ReadingKind::On ? ui().detailOn
                                                              : ui().quizReading;
            h.pillFg = kModeReading;
        } else if (gQ.mode == tgc::QuizMode::MeaningToKanji) {
            h.pill = ui().quizKanji;
            h.pillFg = kModeKanji;
        } else {
            h.pill = ui().quizMeaning;
            h.pillFg = kAccentHi;
        }
    } else {
        const char* word = ui().pillGood;
        const tgc::Icon* icon = &tgc::kIconCheck;
        int days = gLastInterval > 0 ? gLastInterval : 0;
        if (gLastGrade == tgc::Grade::Again) {
            word = ui().pillAgain;
            icon = &tgc::kIconCross;
            days = -1;
            h.pillFg = kErrHi;
            h.pillBg = kErrBg;
        } else if (gLastGrade == tgc::Grade::Hard) {
            word = ui().pillHard;
            h.pillFg = kWarn;
        } else {
            h.pillFg = kOkHi;
            h.pillBg = kOkBg;
        }
        tgc::resultPillText(pill, sizeof pill, word, days, ui().pillDaysUnit, icon->w,
                            tgc::headerPillRoom(W, tgc::captionChars(left), bat >= 0));
        h.pill = pill;
        h.pillIcon = icon;
    }
    wg::drawHeader(d, gRender, W, h);
}

// The prompt: the kanji, or (meaning -> kanji) a caption saying what is asked
// and the meaning under it (UI font, up to three lines), centred as a block.
void drawQuizPrompt(lgfx::LovyanGFX& d, const tgc::Card& card) {
    const QuizGeom& g = gQuizG;
    if (gQ.mode != tgc::QuizMode::MeaningToKanji) {
        drawPromptKanji(d, card.codepoint, g.px, g.py, g.pw, g.ph);
        return;
    }
    constexpr uint8_t kCap = tgc::Renderer::kTextBuiltinSmall;
    constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
    const char* cap = ui().quizK2mCaption;
    const char* prompt = gQ.options[gQ.correct].c_str();
    const int bx = g.px + kGutter;
    const int bw = g.pw - 2 * kGutter;
    const int lh = gRender.lineHeight(kLabel);
    const int capLines = std::max(1, gRender.lineCount(cap, bw, 2, kCap));
    const int capH = capLines * gRender.lineHeight(kCap);
    constexpr int kGapY = 6;
    const int maxLines = std::max(1, std::min(3, (g.ph - capH - kGapY - 4) / lh));
    const int lines = std::max(1, gRender.lineCount(prompt, bw, maxLines, kLabel));
    const int top = g.py + (g.ph - (capH + kGapY + lines * lh)) / 2;
    gRender.drawCentred(d, cap, bx, top, bw, capH, capLines, tgc::theme::kTextDim, kBlack, kCap);
    gRender.drawCentred(d, prompt, bx, top + capH + kGapY, bw, lines * lh, lines, kWhite, kBlack,
                        kLabel);
}

// The quiz's hint bar: "• move ▬ answer" on a question, "• next" after a
// correct answer, "• details" after a wrong one; the side button's icon is
// the settings one (side hold). `holding` lights the hold item.
void drawQuizHints(lgfx::LovyanGFX& d, bool holding) {
    wg::Hints h;
    if (!gRevealed) {
        h.tap = ui().hintMove;
        h.hold = ui().hintAnswer;
        h.holding = holding;
    } else {
        h.tap = gLastCorrect ? ui().hintNext : ui().hintDetails;
    }
    h.side = &tgc::kIconMenu;
    wg::drawHintBar(d, gRender, W, H, h);
}

// The auto-advance countdown: a 2 px bar along the bottom edge that drains
// (below the tiles, or in the hint bar's empty bottom rows).
constexpr int kAdvanceBarH = 2;
void drawAdvanceBar(lgfx::LovyanGFX& d, uint32_t now) {
    const int y = H - kAdvanceBarH;
    d.fillRect(0, y, W, kAdvanceBarH, kBlack);
    const int w = static_cast<int>(std::lround(W * gAdvance.left(now)));
    if (w > 0) d.fillRect(0, y, w, kAdvanceBarH, tgc::theme::kOkHi);
}

void drawQuiz() {
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    gQuizG = quizGeom(quizHintsShown());
    drawQuizHeader(d);
    drawQuizPrompt(d, card);
    const bool holding = gHoldOn && !gRevealed;
    for (int i = 0; i < gQ.count; ++i)
        drawQuizTile(d, i, holding && i == gSel ? gHoldShare : -1.0f);
    if (gQuizG.hints) drawQuizHints(d, holding);
    if (gRevealed && gAdvance.active()) drawAdvanceBar(d, millis());
    present();
}

// A focus move (front tap: down, side tap: up, wrapping): only the two tiles
// change, so only they are redrawn and pushed. The hint bar is redrawn into
// the buffer too (unlit): loop() pushes it when a hold fill ended with this tap.
void moveQuizFocus(bool down) {
    tgc::Card card;
    if (!questionCard(&card)) return;  // also keeps gQ.count from being 0 below
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    const int old = gSel;
    gSel = down ? tgc::nextOption(gSel, gQ.count) : tgc::prevOption(gSel, gQ.count);
    ++gNavTaps;
    auto& d = *gCanvas;
    drawQuizTile(d, old, -1.0f);
    drawQuizTile(d, gSel, -1.0f);
    if (gQuizG.hints) drawQuizHints(d, false);
    const QuizGeom& g = gQuizG;
    const int x0 = std::min(g.tx[old], g.tx[gSel]);
    const int y0 = std::min(g.ty[old], g.ty[gSel]);
    const int x1 = std::max(g.tx[old], g.tx[gSel]) + g.tw;
    const int y1 = std::max(g.ty[old], g.ty[gSel]) + g.th;
    presentRegion(x0, y0, x1 - x0, y1 - y0);
    QLOG("F quiz focus %d -> %d: %u us, push %u us (%d rows)\n", old, gSel,
         (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

// --- Menu screens (docs/ux-refresh.md §2.4) ---------------------------------
// The list screens are built from render/Widgets: a header (title, battery,
// hairline), rows of the layout's height that scroll instead of shrinking
// (ui/ListView, a 2 px scroll bar and small markers when rows are hidden), and
// the hint bar. Labels use the UI font, so they need no pack.

namespace wg = tgc::widgets;
using tgc::theme::kGutter;
using tgc::theme::kHeaderH;
using tgc::theme::kHintH;

tgc::theme::Layout layout() { return tgc::theme::layoutFor(W, H); }

// The menus' header: the title, an optional value (the date, a page number)
// and the battery; the radio icon while the Wi-Fi is on.
void menuHeader(lgfx::LovyanGFX& d, const char* title, const char* right = nullptr,
                bool wifi = false) {
    wg::Header h;
    h.title = title;
    h.right = right;
    h.wifi = wifi;
    h.battery = tgc::board::batteryPercent();
    wg::drawHeader(d, gRender, W, h);
}

// Tap moves the focus, a hold does `holdWord`, the side button goes back.
void menuHints(lgfx::LovyanGFX& d, const char* holdWord) {
    wg::Hints h;
    h.tap = ui().hintMove;
    h.hold = holdWord;
    h.side = &tgc::kIconBack;
    h.sideWord = ui().hintBack;
    wg::drawHintBar(d, gRender, W, H, h);
}

// The hint bar of a screen whose side button only goes back.
void backHints(lgfx::LovyanGFX& d, const char* tap = nullptr, const char* hold = nullptr) {
    wg::Hints h;
    h.tap = tap;
    h.hold = hold;
    h.side = &tgc::kIconBack;
    h.sideWord = ui().hintBack;
    wg::drawHintBar(d, gRender, W, H, h);
}

// Lines of `text` wrapped in the caption font to `w` px, drawn from y one
// every 10 px, centred or from x; at most `maxLines`. Returns the lines drawn.
int drawCaptionLines(lgfx::LovyanGFX& d, const char* text, int x, int y, int w, int maxLines,
                     uint16_t color, tgc::Align align = tgc::Align::Centre) {
    if (!text || !*text) return 0;
    const std::vector<tgc::TextSpan> lines = tgc::wrapText(text, w / 6);
    int n = 0;
    for (const tgc::TextSpan& l : lines) {
        if (n >= maxLines) break;
        const std::string s(text + l.begin, l.len);
        gRender.drawLabel(d, s.c_str(), x, y + 10 * n, w, 8, color, kBlack,
                          tgc::Renderer::kTextBuiltinSmall, align);
        ++n;
    }
    return n;
}

void showError(const char* title, const char* action, const wg::Hints* hints) {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().appName);
    // The title (two label lines at most) and the action under it, centred
    // in the space between the header and the hint bar.
    const int lines = static_cast<int>(tgc::wrapText(action ? action : "", (W - 8) / 6).size());
    const int blockH = 44 + 10 * std::min(lines, 6);
    const int top = kHeaderH + std::max(4, (H - kHeaderH - kHintH - blockH) / 2);
    gRender.drawCentred(d, title, 4, top, W - 8, 40, 2, tgc::theme::kWarn, kBlack,
                        tgc::Renderer::kTextUi);
    drawCaptionLines(d, action, 4, top + 44, W - 8, 6, kWhite);
    if (hints) wg::drawHintBar(d, gRender, W, H, *hints);
    present();
}

// Fits `list` (rows of `rowH`) between y `top` and `bottom`, keeping a band
// above and below for the scroll markers. Called on every draw: the window
// follows the orientation.
void layoutList(tgc::ListView& list, int top, int bottom, int rowH) {
    using tgc::theme::kMarkerBand;
    list.setGeometry(top + kMarkerBand, bottom - top - 2 * kMarkerBand, rowH, tgc::theme::kGap);
}
int listX() { return kGutter; }
int listW() { return W - 2 * kGutter; }  // the scroll bar goes in the right gutter

// Lists whose rows carry a second text (deck counts, zone offsets) put it on
// a caption line under the label on narrow panels, and right of the label as
// the row's value where a row is wide enough for both (landscape).
bool wideRows() { return W >= 200; }

// The menus' one-line note above the hint bar (what the last action did).
constexpr int kNoteH = 12;
void menuNote(lgfx::LovyanGFX& d, const char* text, uint16_t color) {
    if (text && *text)
        gRender.drawLabel(d, text, kGutter, H - kHintH - kNoteH + 1, W - 2 * kGutter, 7, color,
                          kBlack, tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
}

// A focus move on a list whose other content does not depend on the focus
// (settings home, decks, pickers, time zones): when the window did not scroll,
// the next presentList() pushes only the rows from the old to the new focus
// (widgets::presentRegion()). One-shot; any present() drops it.
struct PartialPush {
    bool armed = false;
    const tgc::ListView* list = nullptr;
    int oldFocus = 0;
    int first = 0;
};
PartialPush gPartial;
void dropPartialPush() { gPartial.armed = false; }

void moveListFocus(tgc::ListView& list) {
    const int oldFocus = list.focus();
    const int oldFirst = list.first();
    list.next();
    gPartial.armed = list.first() == oldFirst;
    gPartial.list = &list;
    gPartial.oldFocus = oldFocus;
    gPartial.first = oldFirst;
}

void presentList(const tgc::ListView& list) {
    const PartialPush p = gPartial;
    gPartial.armed = false;
    if (p.armed && p.list == &list && list.first() == p.first && list.isVisible(p.oldFocus) &&
        list.isVisible(list.focus())) {
        const int a = std::min(p.oldFocus, list.focus());
        const int b = std::max(p.oldFocus, list.focus());
        const int y0 = list.rowY(a);
        presentRegion(0, y0, W, list.rowY(b) + list.rowH() - y0);
        return;
    }
    present();
}

// --- Settings tree (docs/ux-refresh.md §3, §6.6) ----------------------------
// Hold the side button from the quiz. The home has a summary card (the loaded
// deck, due and new, its coverage) and five group rows with their values;
// Study and Device are pages of rows (a one-level group stack,
// ui/SettingsTree.h), the other groups open their own screens. Tap moves the
// focus, a hold opens or changes the row, the side button goes back: from a
// page to the home, from the home to where the settings were opened. Every
// screen opened from a page returns to it (gMenuPage stays put meanwhile).
namespace st = tgc::settings;
st::Page gMenuPage = st::Page::Home;
tgc::ListView gMenuLists[st::kPageCount];  // each page keeps its focus
char gMenuNote[32] = "";  // a page's note (after Delete progress), until the next press
tgc::ListView& menuList() { return gMenuLists[static_cast<int>(gMenuPage)]; }

// One row of a settings page: its label, its value (none when `value` is null
// or writes "") and what a hold does. menuItem() has one per st::Item.
struct MenuItem {
    st::Item id;
    const char* (*label)();
    void (*value)(char* out, size_t n);
    void (*action)();
};
const MenuItem& menuItem(st::Item id);  // the table, after the actions it calls

// "18 Oct": the current day for the settings header and the Date row.
void shortDate(uint16_t day, char* out, size_t n) {
    int y = 0;
    unsigned m = 1, dd = 1;
    tgc::civilFromDays(gSrs.epochDayOf(day), &y, &m, &dd);
    st::formatDayMonth(dd, ui().months[(m + 11) % 12], out, n);
}

// The summary card (§2.4): the loaded deck's name, "12 due · 5 new" and its
// coverage (seen / total) as a bar. Portrait: a band over the group rows with
// "45/79" right of the name. Landscape: a column, one count per line.
void drawSummaryCard(lgfx::LovyanGFX& d, const tgc::layout::Home& l) {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    const tgc::layout::Rect& c = l.card;
    d.fillRect(c.x, c.y, c.w, c.h, kSurface);
    const int px = c.x + 6;
    const int pw = c.w - 12;
    if (!gPack->isOpen()) {
        gRender.drawLabel(d, ui().summaryNoDeck, px, c.y + 3, pw, 20, kText, kSurface);
        gRender.drawWrapped(d, ui().summaryNoDeckHint, px, c.y + 25, pw, l.cardColumn ? 4 : 1,
                            kTextDim, kSurface, kSmall);
        return;
    }
    const tgc::DeckSummary s = deckCounts();
    char due[24], fresh[24], seen[32];
    snprintf(due, sizeof due, ui().summaryDueFmt, (unsigned)s.due);
    snprintf(fresh, sizeof fresh, ui().summaryNewFmt, (unsigned)s.fresh);
    if (!l.cardColumn) {
        snprintf(seen, sizeof seen, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
        const int sw = wg::captionWidth(seen);
        gRender.drawLabel(d, gPack->deckName(), px, c.y + 3, pw - sw - 6, 20, kText, kSurface);
        wg::drawCaption(d, gRender, seen, c.right() - 6 - sw, c.y + 10, kTextDim, kSurface);
        int x = px;
        x += wg::drawCaption(d, gRender, due, x, c.y + 25, kAccentHi, kSurface);
        x += wg::drawCaption(d, gRender, " · ", x, c.y + 25, kTextFaint, kSurface);
        wg::drawCaption(d, gRender, fresh, x, c.y + 25, kTextDim, kSurface);
        wg::drawProgress(d, px, c.y + 37, pw, 3, static_cast<int>(s.seen()),
                         static_cast<int>(s.total));
        return;
    }
    snprintf(seen, sizeof seen, ui().summarySeenFmt, (unsigned)s.seen(), (unsigned)s.total);
    if (wg::captionWidth(seen) > pw)
        snprintf(seen, sizeof seen, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
    gRender.drawLabel(d, gPack->deckName(), px, c.y + 4, pw, 20, kText, kSurface);
    gRender.drawLabel(d, due, px, c.y + 30, pw, 8, kAccentHi, kSurface, kSmall);
    gRender.drawLabel(d, fresh, px, c.y + 42, pw, 8, kTextDim, kSurface, kSmall);
    gRender.drawLabel(d, seen, px, c.y + 54, pw, 8, kTextDim, kSurface, kSmall);
    wg::drawProgress(d, px, c.y + 67, pw, 3, static_cast<int>(s.seen()), static_cast<int>(s.total));
}

// One settings row. On the home: the group's icon, its value when it fits and
// a chevron. On a page: the value right of the label where rows are wide
// (landscape), else on a caption line under it; a row without a value keeps
// its label centred.
void drawMenuRow(lgfx::LovyanGFX& d, st::Item id, int x, int y, int w, int h, bool focused) {
    const MenuItem& m = menuItem(id);
    char value[48] = "";
    if (m.value) m.value(value, sizeof value);
    wg::Row row;
    row.label = m.label();
    row.icon = st::itemIcon(id);
    row.chevron = st::hasChevron(id);
    row.focused = focused;
    if (value[0]) {
        if (gMenuPage == st::Page::Home) {
            row.value = value;
            // A network name is cut; the other values show whole or not at all.
            row.keepLabel = id == st::Item::Wifi;
            row.valueIfFits = !row.keepLabel;
        } else if (wideRows()) {
            row.value = value;
            row.keepLabel = true;
        } else {
            row.caption = value;
        }
    }
    wg::drawRow(d, gRender, x, y, w, h, row);
}

void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const st::PageItems items = st::pageItems(gMenuPage);
    tgc::ListView& list = menuList();
    list.setCount(items.count);
    auto row = [&](int i, int x, int y, int w, int h, bool f) {
        drawMenuRow(d, items.items[i], x, y, w, h, f);
    };
    if (gMenuPage == st::Page::Home) {
        char date[16];
        shortDate(gSrs.today(), date, sizeof date);
        menuHeader(d, ui().settingsTitle, date);
        const tgc::layout::Home l = tgc::layout::homeFor(W, H);
        drawSummaryCard(d, l);
        layoutList(list, l.rowsTop, l.rowsBottom, layout().row1H);
        wg::drawList(d, list, l.rowsX, l.rowsW, row);
    } else {
        menuHeader(d, gMenuPage == st::Page::Study ? ui().studyTitle : ui().deviceTitle);
        layoutList(list, kHeaderH, H - kHintH - (gMenuNote[0] ? kNoteH : 0),
                   wideRows() ? layout().row1H : layout().row2H);
        wg::drawList(d, list, listX(), listW(), row);
        menuNote(d, gMenuNote, tgc::theme::kOkHi);
    }
    menuHints(d, ui().hintOpen);
    presentList(list);
}

// A group row of the home opened its page: from the top.
void openMenuPage(st::Page p) {
    gMenuPage = p;
    menuList().setCount(st::pageItems(p).count);
    menuList().reset(0);
    gScreen = Screen::Menu;
    drawScreen();
}

// Back to the settings tree from one of its screens (the page it came from).
void backToMenu() {
    gScreen = Screen::Menu;
    drawScreen();
}

// Deck list (opened from "Decks"; docs/ux-refresh.md, decks mockup): one row
// per pack with the deck name from its header, a language pill and "79 kanji"
// ("79 kanji · 12 due" for the loaded one, whose radio is on); the caption
// goes under the name in portrait, right of it in landscape. Hold loads the
// focused pack; the side button returns to the settings home. The last row,
// "Get decks", opens the download screen (drawGetDecks).
tgc::ListView gDeckList;

// The header of each pack in gPacks (peekPackInfo(): two small reads a file),
// read when the deck list opens (an update keeps the file name but may change
// the header) and again whenever the list of files changed under it.
std::vector<tgc::PackInfo> gPackInfo;
std::vector<String> gPackInfoFor;  // the gPacks the infos were read for

void refreshPackInfo() {
    if (gPackInfoFor == gPacks) return;
    gPackInfo.assign(gPacks.size(), tgc::PackInfo{});
    for (size_t i = 0; i < gPacks.size(); ++i) {
        tgc::FsPackSource src(LittleFS.open(gPacks[i], "r"));
        if (src.ok()) tgc::peekPackInfo(src, &gPackInfo[i]);
    }
    gPackInfoFor = gPacks;
}

// A pack's file name without the folder and ".tgc", for a deck whose header
// gives no name.
String packFileStem(const String& path) {
    String s = path;
    const int slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.substring(slash + 1);
    if (s.endsWith(".tgc")) s = s.substring(0, s.length() - 4);
    return s;
}

void drawDecks() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().decksTitle);
    refreshPackInfo();
    const uint32_t due = deckCounts().due;  // only shown on the loaded pack's row
    const int packs = static_cast<int>(gPacks.size());
    gDeckList.setCount(packs + 1);  // + "Get decks"
    const bool wide = wideRows();
    layoutList(gDeckList, kHeaderH, H - kHintH, wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gDeckList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.focused = f;
        String stem;
        char caption[48], lang[8];
        if (i == packs) {
            row.label = ui().getDecksRow;
            row.icon = &tgc::kIconDown;
            row.chevron = true;
            (wide ? row.value : row.caption) = ui().getDecksCaption;
            row.labelColor = tgc::theme::kAccentHi;
        } else {
            const tgc::PackInfo& info = gPackInfo[i];
            const bool loaded = gPacks[i] == gCurrentPack;
            if (info.name[0]) {
                row.label = info.name;
            } else {
                stem = packFileStem(gPacks[i]);
                row.label = stem.c_str();
            }
            snprintf(lang, sizeof lang, "%s", info.lang);
            for (char* p = lang; *p; ++p)
                if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
            if (lang[0]) row.tag = lang;
            if (loaded)
                snprintf(caption, sizeof caption, ui().deckCountsFmt, (unsigned)info.cards,
                         (unsigned)due);
            else if (info.cards)
                snprintf(caption, sizeof caption, ui().deckCardsFmt, (unsigned)info.cards);
            else
                caption[0] = 0;
            row.radio = loaded ? wg::Radio::On : wg::Radio::Off;
            if (wide) {
                row.value = caption;
                row.keepLabel = true;
            } else {
                row.caption = caption;
            }
        }
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintOpen);
    presentList(gDeckList);
}

// Generic choice screen: a title and the candidate values. A radio marks the
// saved value and the accent fill the focus. Hold applies the focused value;
// the side button cancels back to where the picker came from.
tgc::ListView gChoiceList;
int gChoiceSaved = -1;  // row of the saved value, -1 when none (Forget Wi-Fi)

// The Hard threshold as the Study row and its picker show it: "3 s", "off".
void hardAfterText(uint16_t ms, char* out, size_t n) {
    if (ms == 0xFFFF)
        snprintf(out, n, "%s", ui().hardAfterOff);
    else
        st::formatSeconds(ms, ui().decimalPoint, out, n);
}

void choiceLabel(int i, char* out, size_t n) {
    const uint16_t v = gChoiceValues[i];
    if (gChoice == Choice::Orientation) {
        snprintf(out, n, "%s", ui().orientRows[v % 3]);
    } else if (gChoice == Choice::ForgetWifi) {
        snprintf(out, n, "%s", ui().forgetRows[i % 2]);
    } else if (gChoice == Choice::UiLang) {
        snprintf(out, n, "%s", ui().langRows[i]);
    } else if (gChoice == Choice::Brightness) {
        snprintf(out, n, "%u%%", (unsigned)(v * 100u / 255u));
    } else if (gChoice == Choice::SlowMs) {
        hardAfterText(v, out, n);
    } else if (gChoice == Choice::AutoAdvance) {
        snprintf(out, n, "%s", ui().advanceRows[v % tgc::kAdvanceChoices]);
    } else if (gChoice == Choice::HoldTime) {
        snprintf(out, n, "%s", ui().holdRows[v % tgc::kHoldChoices]);
    } else {
        snprintf(out, n, "%u", (unsigned)v);
    }
}

// A picker row's value: the duration a timing choice stands for, else
// "default" on the default choice; "" for none.
void choiceValue(int i, char* out, size_t n) {
    const uint16_t v = gChoiceValues[i];
    out[0] = 0;
    if (gChoice == Choice::AutoAdvance) {
        if (v != tgc::kAdvanceOff)
            st::formatSeconds(tgc::advanceMsFor(v), ui().decimalPoint, out, n);
    } else if (gChoice == Choice::HoldTime) {
        st::formatSeconds(tgc::holdMsFor(v), ui().decimalPoint, out, n);
    } else if ((gChoice == Choice::NewPerDay && v == tgc::kSrsDefaultNewPerDay) ||
               (gChoice == Choice::SlowMs && v == tgc::kSrsDefaultSlowMs) ||
               (gChoice == Choice::Brightness && v == kBrightnessDefault)) {
        snprintf(out, n, "%s", ui().choiceDefault);
    }
}

const char* choiceTitle() {
    switch (gChoice) {
        case Choice::NewPerDay: return ui().newPerDayTitle;
        case Choice::SlowMs: return ui().hardAfterTitle;
        case Choice::Orientation: return ui().orientTitle;
        case Choice::Brightness: return ui().brightTitle;
        case Choice::ForgetWifi: return ui().forgetTitle;
        case Choice::AutoAdvance: return ui().advanceTitle;
        case Choice::HoldTime: return ui().holdTitle;
        case Choice::UiLang: break;
    }
    return ui().langTitle;
}

// The one-line explanation under a picker's header; null for none.
const char* choiceDescription() {
    switch (gChoice) {
        case Choice::NewPerDay: return ui().descNewPerDay;
        case Choice::SlowMs: return ui().descHardAfter;
        case Choice::AutoAdvance: return ui().descAdvance;
        case Choice::HoldTime: return ui().descHold;
        case Choice::Orientation: return tgc::board::hasAccel() ? ui().descOrient : nullptr;
        case Choice::Brightness: return ui().descBright;
        case Choice::UiLang: return ui().descLang;
        case Choice::ForgetWifi: break;
    }
    return nullptr;
}

void drawChoice() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, choiceTitle());
    int top = kHeaderH;
    const int rowH = layout().row1H;
    // "Forget": which network, between the header and the rows.
    if (gChoice == Choice::ForgetWifi) {
        gRender.drawLabel(d, gWifiMenuSsid.c_str(), kGutter, kHeaderH + 2, W - 2 * kGutter, 20,
                          tgc::theme::kWarn, kBlack, tgc::Renderer::kTextUi, tgc::Align::Centre);
        top += 22;
    }
    // The description, wrapped in the caption font, when every row still fits
    // under it (the four-row pickers in landscape go without).
    if (const char* desc = choiceDescription()) {
        const std::vector<tgc::TextSpan> lines = tgc::wrapText(desc, (W - 2 * kGutter) / 6);
        const int descH = 3 + static_cast<int>(lines.size()) * 10;
        const int rowsH = gChoiceCount * (rowH + tgc::theme::kGap) - tgc::theme::kGap +
                          2 * tgc::theme::kMarkerBand;
        if (top + descH + rowsH <= H - kHintH) {
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string line(desc + lines[i].begin, lines[i].len);
                const int y = top + 3 + 10 * static_cast<int>(i);
                wg::drawCaption(d, gRender, line.c_str(), kGutter, y, tgc::theme::kTextDim, kBlack);
            }
            top += descH;
        }
    }
    gChoiceList.setCount(gChoiceCount);
    layoutList(gChoiceList, top, H - kHintH, rowH);
    wg::drawList(d, gChoiceList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        char label[24], value[24];
        choiceLabel(i, label, sizeof label);
        choiceValue(i, value, sizeof value);
        wg::Row row;
        row.label = label;
        row.value = value;
        row.keepLabel = true;
        row.focused = f;
        if (gChoiceSaved >= 0) row.radio = i == gChoiceSaved ? wg::Radio::On : wg::Radio::Off;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintChoose);
    presentList(gChoiceList);
}

// First-boot language picker (setup(), docs/decisions.md): the choice-screen
// look with a neutral header ("LANGUAGE / IDIOMA"), every row in its own
// language and the hint bar in the language of the focused row, so it reads
// the same whatever table is active. The side button does nothing here:
// there is nothing to go back to yet, so the bar shows no side action.
tgc::ListView gFirstLangList;

void drawFirstLang() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    char title[40];
    snprintf(title, sizeof title, "%s / %s", kUiEn.langTitle, kUiEs.langTitle);
    menuHeader(d, title);
    gFirstLangList.setCount(tgc::kFirstBootLangRows);
    layoutList(gFirstLangList, kHeaderH, H - kHintH, layout().row1H);
    wg::drawList(d, gFirstLangList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().langRows[tgc::firstBootLangPref(i)];
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    const UiStrings& hl =
        tgc::firstBootLangPref(gFirstLangList.focus()) == tgc::kUiLangSpanish ? kUiEs : kUiEn;
    wg::Hints hints;
    hints.tap = hl.firstLangTap;
    hints.hold = hl.firstLangHold;
    wg::drawHintBar(d, gRender, W, H, hints);
    present();  // the hints follow the focus: the whole frame
}

// An icon drawn `s` times larger (each pixel an s x s block).
void drawIconScaled(lgfx::LovyanGFX& d, const tgc::Icon& ic, int x, int y, int s, uint16_t ink) {
    for (int yy = 0; yy < ic.h; ++yy)
        for (int xx = 0; xx < ic.w; ++xx)
            if (tgc::icons::pixel(ic, xx, yy)) d.fillRect(x + xx * s, y + yy * s, s, s, ink);
}

// The day streak as it stands today (ui/SettingsTree.h).
uint32_t currentStreak() {
    return st::liveStreak(gSrs.streakCurrent(), gSrs.hasStudied(), gSrs.lastStudyDay(),
                          gSrs.today());
}

// "All done" screen (docs/ux-refresh.md, done mockup): no card is due and no
// new card is left today. A check in a circle, "All caught up", tomorrow's due
// count, the streak and a "+10 new" button. A tap, hold or side tap studies 10
// more (rebuilds the queue and tries again); the side hold opens the settings.
// A deck whose cards have all been studied has nothing to add: no button, no
// hold hint, and those presses do nothing.
bool doneCanStudyMore() { return deckCounts().fresh > 0; }

void drawDone() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, gPack->isOpen() ? gPack->deckName() : ui().appName);
    const tgc::layout::Done l = tgc::layout::doneFor(W, H);
    d.fillCircle(l.iconCx, l.iconCy, l.iconR, kOkBg);
    d.drawCircle(l.iconCx, l.iconCy, l.iconR, kOk);
    d.drawCircle(l.iconCx, l.iconCy, l.iconR - 1, kOk);
    constexpr int kScale = 3;
    drawIconScaled(d, tgc::kIconCheck, l.iconCx - tgc::kIconCheck.w * kScale / 2,
                   l.iconCy - tgc::kIconCheck.h * kScale / 2, kScale, kOkHi);
    const tgc::Align align = W > H ? tgc::Align::Left : tgc::Align::Centre;
    gRender.drawLabel(d, ui().doneTitle, l.textX, l.title.y, l.textW, l.title.h, kText, kBg,
                      tgc::Renderer::kTextUi, align);
    char line[48];
    snprintf(line, sizeof line, ui().doneTomorrowFmt, (unsigned)deckCounts().dueTomorrow);
    gRender.drawLabel(d, line, l.textX, l.line1Y, l.textW, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, align);
    const uint32_t streak = currentStreak();
    if (streak > 0) {
        if (streak == 1)
            snprintf(line, sizeof line, "%s", ui().doneStreakOne);
        else
            snprintf(line, sizeof line, ui().doneStreakFmt, (unsigned)streak);
        const int w = tgc::kIconFlame.w + 4 + wg::captionWidth(line);
        const int x = align == tgc::Align::Left ? l.textX : l.textX + (l.textW - w) / 2;
        wg::drawIcon(d, tgc::kIconFlame, x, l.line2Y, kWarn);
        wg::drawCaption(d, gRender, line, x + tgc::kIconFlame.w + 4, l.line2Y, kWarn, kBg);
    }
    wg::Hints h;
    if (doneCanStudyMore()) {
        const tgc::layout::Rect& b = l.button;
        d.fillRect(b.x, b.y, b.w, b.h, kAccent);
        gRender.drawLabel(d, ui().doneMore, b.x, b.y, b.w, b.h, kText, kAccent,
                          tgc::Renderer::kTextUi, tgc::Align::Centre);
        h.hold = ui().hintStudyMore;
    }
    h.side = &tgc::kIconMenu;
    wg::drawHintBar(d, gRender, W, H, h);
    present();
}

void showDone() {
    gScreen = Screen::Done;
    drawDone();
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

// Date screen (docs/srs.md section 5, power-cut rule): the last-used date and a
// proposed "today" that starts at the current day. A tap adds a day, a side
// tap takes one away, a hold confirms, a side hold cancels. Below the current
// day the index is kept and the anchor moves back (SrsStore::setDateManual(),
// only on a store with an anchor). Asked on a power-on boot (where the network
// time comes first when it can, see dateSyncBegin()) and opened from Settings >
// Device > Date for manual corrections.

// The proposed "today" as a calendar day (days since 1970-01-01).
int32_t dateTarget() { return gSrs.epochDayOf(gSrs.today()) + gDateSteps; }

const char* weekdayName(uint16_t day) {
    return ui().weekdays[tgc::weekdayFromDays(gSrs.epochDayOf(day))];
}

void openDate(Screen after) {
    gAfterDate = after;
    gDateSteps = 0;
    gScreen = Screen::Date;
    drawScreen();
}

// "Sat 17 Oct": a day as the date screen's "last used" line writes it.
void weekdayDayMonth(uint16_t day, char* out, size_t n) {
    char dm[16];
    shortDate(day, dm, sizeof dm);
    snprintf(out, n, "%s %s", weekdayName(day), dm);
}

// The calendar tile (§2.4): an accent band with "OCT 2026", the day in the
// numeral face and the weekday under it, for calendar day `epochDay`.
void drawCalendarTile(lgfx::LovyanGFX& d, const tgc::layout::Date& l, int32_t epochDay) {
    using namespace tgc::theme;
    const tgc::layout::Rect& t = l.tile;
    int y = 0;
    unsigned m = 1, dd = 1;
    tgc::civilFromDays(epochDay, &y, &m, &dd);
    d.fillRect(t.x, t.y, t.w, t.h, kSurface);
    d.fillRect(t.x, t.y, t.w, l.bandH, kAccent);
    char band[16], num[4];
    snprintf(band, sizeof band, "%s %d", ui().months[(m + 11) % 12], y);
    for (char* p = band; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
    gRender.drawLabel(d, band, t.x, t.y, t.w, l.bandH, kText, kAccent,
                      tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
    snprintf(num, sizeof num, "%u", dd);
    gRender.drawLabel(d, num, t.x, t.y + l.bandH + 2, t.w, 36, kText, kSurface,
                      tgc::Renderer::kTextUiNumeral, tgc::Align::Centre);
    gRender.drawLabel(d, ui().weekdaysLong[tgc::weekdayFromDays(epochDay)], t.x,
                      t.y + l.bandH + 40, t.w, 20, kTextDim, kSurface, tgc::Renderer::kTextUi,
                      tgc::Align::Centre);
}

// The boot-time question's network attempt (gDateSync == Joining): what it is
// doing, over which network, and that a tap gives up and shows the stepper.
void drawDateNet() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
    gRender.drawCentred(d, ui().dateNetTitle, 4, mid - 36, W - 8, 40, 2, kText, kBg,
                        tgc::Renderer::kTextUi);
    char line[64];
    snprintf(line, sizeof line, ui().dateNetViaFmt, gHome.ssid());
    drawCaptionLines(d, line, 4, mid + 8, W - 8, 2, kTextDim);
    wg::Hints h;
    h.tap = ui().hintByHand;
    wg::drawHintBar(d, gRender, W, H, h);
    present();
}

// The stepper (docs/ux-refresh.md, date mockup): at boot "Device was off." and
// the last-used day, then the calendar tile of the proposed day, the "+N days"
// / "-N days" pill, why it is asked by hand when the network time failed, and
// what to step to. The hint bar has both buttons' two actions: "• +1 day ▬ OK"
// and "◂ -1 day ▬ cancel", the side ones on a row of their own when the panel
// is too narrow for one row (the portraits); "-1 day" is dimmed at the floor.
void drawDate() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().dateTitle, nullptr, gDateSync == DateSync::Joining);
    if (gDateSync == DateSync::Joining) {
        drawDateNet();
        return;
    }
    wg::Hints h;
    h.tap = ui().dateOneDay;
    h.hold = ui().firstLangHold;  // "OK"
    h.side = &tgc::kIconBack;
    h.sideWord = ui().dateBackOneDay;
    h.sideDim = gDateSteps <= gSrs.manualStepsMin();
    h.sideHold = ui().hintCancel;
    const tgc::layout::Date l = tgc::layout::dateFor(W, H, wg::hintBarHeight(W, h));
    const bool land = W > H;
    const tgc::Align align = land ? tgc::Align::Left : tgc::Align::Centre;
    char line[64], day[32];

    // At boot: the device was off; always: the day it last ran.
    const bool boot = gAfterDate == Screen::Quiz;
    weekdayDayMonth(gBootDay, day, sizeof day);
    snprintf(line, sizeof line, "%s %s", ui().dateLastUsed, day);
    const tgc::layout::Rect& n = l.notice;
    if (boot)
        gRender.drawLabel(d, ui().dateWasOff, n.x, n.y, n.w, 8, kWarn, kBg,
                          tgc::Renderer::kTextBuiltinSmall, align);
    gRender.drawLabel(d, line, n.x, boot ? n.y + 10 : n.y + 5, n.w, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, align);

    // The proposed "today": this is what the buttons change.
    drawCalendarTile(d, l, dateTarget());
    if (gDateSteps == 0)
        snprintf(line, sizeof line, "%s", ui().dateToday);
    else if (gDateSteps == 1)
        snprintf(line, sizeof line, "%s", ui().dateOneDay);
    else if (gDateSteps == -1)
        snprintf(line, sizeof line, "%s", ui().dateBackOneDay);
    else if (gDateSteps > 0)
        snprintf(line, sizeof line, ui().dateDaysFmt, static_cast<unsigned>(gDateSteps));
    else
        snprintf(line, sizeof line, ui().dateBackDaysFmt, static_cast<unsigned>(-gDateSteps));
    const int pw = wg::pillWidth(line);
    wg::drawPill(d, gRender, l.pillCx - pw / 2, l.pillY, line, gDateSteps ? kWarn : kTextDim,
                 kSurfaceHi);

    // Why it is asked by hand after all (the network time failed), then what
    // to step to (the hint bar has the buttons).
    const tgc::layout::Rect& e = l.explain;
    const int maxLines = e.h / 10;
    int lines = 0;
    if (gDateSync == DateSync::NoNetwork || gDateSync == DateSync::NoTime) {
        const char* status =
            gDateSync == DateSync::NoNetwork ? ui().dateNoNetwork : ui().dateNoTime;
        lines = drawCaptionLines(d, status, e.x, e.y, e.w, maxLines, kWarn, align);
        ++lines;  // a blank line under it
    }
    drawCaptionLines(d, ui().dateExplain, e.x, e.y + 10 * lines, e.w, maxLines - lines, kTextDim,
                     align);

    wg::drawHintBar(d, gRender, W, H, h);
    present();
}

// --- Settings > Device > Date ------------------------------------------------

// "+1", "-3", "+5:30", "0": one offset for the zone list's right column.
void formatOffsetShort(int offsetMin, char* out, size_t n) {
    if (offsetMin == 0) {
        snprintf(out, n, "0");
        return;
    }
    const int a = offsetMin < 0 ? -offsetMin : offsetMin;
    if (a % 60)
        snprintf(out, n, "%c%d:%02d", offsetMin < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(out, n, "%c%d", offsetMin < 0 ? '-' : '+', a / 60);
}

// A zone's offsets: "+1/+2" (short) or "UTC+1, summer UTC+2" (footer); one
// offset alone without daylight time.
void tzOffsets(const tgc::TzRule& r, bool longForm, char* out, size_t n) {
    char a[16], b[16];
    if (longForm) {
        tgc::formatUtcOffset(r.stdOffsetSec / 60, a, sizeof a);
        tgc::formatUtcOffset(r.dstOffsetSec / 60, b, sizeof b);
    } else {
        formatOffsetShort(r.stdOffsetSec / 60, a, sizeof a);
        formatOffsetShort(r.dstOffsetSec / 60, b, sizeof b);
    }
    if (!r.hasDst)
        snprintf(out, n, "%s", a);
    else if (longForm)
        snprintf(out, n, ui().tzSummerFmt, a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

// The zone in use for the Date submenu: its city, else its offset (an old page
// left only that), else "none".
void currentZoneLabel(char* out, size_t n) {
    if (!gTzKnown)
        snprintf(out, n, "%s", ui().tzNone);
    else if (gTzName[0])
        tgc::tzZoneCity(gTzName, out, n);
    else
        tgc::formatUtcOffset(gTz.stdOffsetSec / 60, out, n);
}

// Settings > Device > Date: the date (the stepper) and the time zone (the
// list), in the Wi-Fi submenu's look: the row's value is the date or the
// zone's city. The side button goes back to the Device page.
constexpr int kDateMenuRows = 2;
tgc::ListView gDateMenuList;
char gDateMenuNote[48] = "";
uint16_t gDateMenuNoteColor = 0;

void drawDateMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().dateTitle);
    char date[16], zone[48];
    formatDate(gSrs.today(), date, sizeof date);
    currentZoneLabel(zone, sizeof zone);
    const char* values[kDateMenuRows] = {date, zone};
    gDateMenuList.setCount(kDateMenuRows);
    layoutList(gDateMenuList, kHeaderH, H - kHintH - kNoteH, layout().row1H);
    wg::drawList(d, gDateMenuList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().dateMenuRows[i];
        row.value = values[i];
        row.keepLabel = true;
        row.chevron = true;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gDateMenuNote, gDateMenuNoteColor);
    menuHints(d, ui().hintOpen);
    present();  // a move clears the note: the whole frame
}

// Settings > Device > Date > Zone: "From phone" (the zone the portal page last
// sent), then tgc::kTzZones. Row 0 is "From phone", row i is kTzZones[i - 1].
tgc::ListView gTzListView;
int tzListRows() { return 1 + static_cast<int>(tgc::kTzZoneCount); }

// The row of the zone in use: the list entry chosen by hand, else "From phone".
int tzCurrentRow() {
    if (gTzManual)
        for (size_t i = 0; i < tgc::kTzZoneCount; ++i)
            if (std::strcmp(tgc::kTzZones[i].name, gTzName) == 0) return static_cast<int>(i) + 1;
    return 0;
}

// Each row: the city and its offsets ("UTC+1, summer UTC+2" under it in
// portrait, or "+1/+2" when that does not fit; "+1/+2" right of it in
// landscape); "From phone" with the page's city and offsets, or how to get
// them. A radio marks the zone in use.
void drawTzList() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().tzTitle);
    gTzListView.setCount(tzListRows());
    const bool wide = wideRows();
    layoutList(gTzListView, kHeaderH, H - kHintH, wide ? layout().row1H : layout().row2H);
    const int current = tzCurrentRow();
    wg::drawList(d, gTzListView, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        char label[40], caption[64];
        if (i == 0) {
            snprintf(label, sizeof label, "%s", ui().tzFromPhone);
            if (gPhoneTzKnown && gPhoneTzName[0]) {
                char city[40], offsets[24];
                tgc::tzZoneCity(gPhoneTzName, city, sizeof city);
                tzOffsets(gPhoneTz, false, offsets, sizeof offsets);
                snprintf(caption, sizeof caption, "%s · %s", city, offsets);
            } else if (gPhoneTzKnown) {
                tzOffsets(gPhoneTz, !wide, caption, sizeof caption);
            } else {
                snprintf(caption, sizeof caption, "%s", ui().tzPhoneNone);
            }
        } else {
            const tgc::TzZone& z = tgc::kTzZones[i - 1];
            tgc::tzZoneCity(z.name, label, sizeof label);
            tgc::TzRule r;
            caption[0] = 0;
            if (tgc::parseTzRule(z.rule, &r)) {
                tzOffsets(r, !wide, caption, sizeof caption);
                if (!wide && wg::captionWidth(caption) > w - 2 * tgc::theme::kRowPadX)
                    tzOffsets(r, false, caption, sizeof caption);
            }
        }
        wg::Row row;
        row.label = label;
        if (wide) {
            row.value = caption;
            row.keepLabel = i == 0;  // "From phone" stays whole, the city is cut
        } else {
            row.caption = caption;
        }
        row.radio = i == current ? wg::Radio::On : wg::Radio::Off;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuHints(d, ui().hintChoose);
    presentList(gTzListView);
}

// "2h 5m", "5m 30s" or "30s" depending on the magnitude; `firstUnit` keeps
// only the largest unit ("2h", "5m"). The unit strings come from UiStrings so
// a language can change them.
void formatReviewTime(uint32_t ms, char* out, size_t n, bool firstUnit = false) {
    const uint32_t totalS = ms / 1000u;
    const uint32_t mins = totalS / 60u;
    const uint32_t hours = mins / 60u;
    if (hours > 0)
        snprintf(out, n, ui().statsTimeHM, (unsigned)hours, (unsigned)(mins % 60u));
    else if (mins > 0)
        snprintf(out, n, ui().statsTimeMS, (unsigned)mins, (unsigned)(totalS % 60u));
    else
        snprintf(out, n, ui().statsTimeS, (unsigned)totalS);
    if (firstUnit) {
        char* space = std::strchr(out, ' ');
        if (space) *space = 0;
    }
}

// One statistics tile: the value in the label face (after an optional icon)
// over its caption.
void drawStatTile(lgfx::LovyanGFX& d, const tgc::layout::Rect& t, const char* value,
                  const char* caption, const tgc::Icon* icon = nullptr) {
    using namespace tgc::theme;
    // 4 px in from the sides: a 62 px tile keeps 54 px, nine caption
    // characters ("dominadas").
    constexpr int kPad = 4;
    d.fillRect(t.x, t.y, t.w, t.h, kSurface);
    int x = t.x + kPad;
    // The value (an 18 px box) and the caption under it, centred as a pair.
    const int valueY = t.y + std::max(3, (t.h - 29) / 2);
    if (icon) {
        wg::drawIcon(d, *icon, x, valueY + (18 - icon->h) / 2, kWarn);
        x += icon->w + 3;
    }
    gRender.drawLabel(d, value, x, valueY, t.right() - kPad - x, 18, kText, kSurface);
    gRender.drawLabel(d, caption, t.x + kPad, valueY + 19, t.w - 2 * kPad, 8, kTextDim, kSurface,
                      tgc::Renderer::kTextBuiltinSmall);
}

// Statistics (docs/ux-refresh.md, stats mockup; opened from the settings
// home): the accuracy over every answer as a ring, six tiles -- the day streak
// (a flame) and the best one, the loaded deck's due and new cards, the learned
// cards and the time on questions -- and how much of the deck has been seen.
// Read-only; the side button goes back.
void drawStats() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().statsTitle);
    const tgc::layout::Stats l = tgc::layout::statsFor(W, H);
    const tgc::Stats stats = gSrs.stats();
    const tgc::DeckSummary s = deckCounts();
    const int acc = st::accuracyPercent(stats.answers, stats.again);

    // The ring: a track, the accuracy clockwise from 12 o'clock, the number in
    // the hole ("87" in the numeral face, "%" in the label face, on one
    // baseline), else the label face alone when that does not fit ("100%").
    d.fillArc(l.cx, l.cy, l.rOuter, l.rInner, 0.0f, 360.0f, kSurfaceHi);
    if (acc > 0)
        d.fillArc(l.cx, l.cy, l.rOuter, l.rInner, tgc::layout::kRingStartDeg,
                  tgc::layout::ringEndDeg(acc), kAccentHi);
    char digits[8];
    snprintf(digits, sizeof digits, acc >= 0 ? "%d" : "--", acc);
    const int dw = gRender.textWidth(digits, tgc::Renderer::kTextUiNumeral);
    const int pw = acc >= 0 ? gRender.textWidth("%", tgc::Renderer::kTextUi) : 0;
    if (tgc::layout::ringFits(dw + pw, l.rInner)) {
        const int x = l.cx - (dw + pw) / 2;
        gRender.drawLabel(d, digits, x, l.cy - 12, dw, 24, kText, kBg,
                          tgc::Renderer::kTextUiNumeral);
        if (pw) gRender.drawLabel(d, "%", x + dw, l.cy - 2, pw, 14, kTextDim, kBg);
    } else {
        char label[8];
        snprintf(label, sizeof label, "%d%%", acc);
        gRender.drawLabel(d, label, l.cx - l.rInner, l.cy - 7, 2 * l.rInner, 14, kText, kBg,
                          tgc::Renderer::kTextUi, tgc::Align::Centre);
    }
    const int colW = l.seenStacked ? 2 * l.cx : W;
    gRender.drawLabel(d, ui().statsAccuracy, 0, l.captionY, colW, 8, kTextDim, kBg,
                      tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);

    // The tiles, in pairs.
    char v[6][16];
    snprintf(v[0], sizeof v[0], "%u", (unsigned)currentStreak());
    snprintf(v[1], sizeof v[1], "%u", (unsigned)stats.streakBest);
    snprintf(v[2], sizeof v[2], "%u", (unsigned)s.due);
    snprintf(v[3], sizeof v[3], "%u", (unsigned)s.fresh);
    snprintf(v[4], sizeof v[4], "%u", (unsigned)stats.learned);
    formatReviewTime(stats.reviewMs, v[5], sizeof v[5]);
    if (gRender.textWidth(v[5], tgc::Renderer::kTextUi) > l.tiles[5].w - 8)
        formatReviewTime(stats.reviewMs, v[5], sizeof v[5], true);
    for (int i = 0; i < 6; ++i)
        drawStatTile(d, l.tiles[i], v[i], ui().statsTiles[i], i == 0 ? &tgc::kIconFlame : nullptr);

    // Seen / total of the loaded deck: one line across (portrait), or the
    // caption over the bar in the left column (landscape).
    char nums[24];
    snprintf(nums, sizeof nums, "%u/%u", (unsigned)s.seen(), (unsigned)s.total);
    const tgc::layout::Rect& sn = l.seen;
    if (!l.seenStacked) {
        const int ww = wg::captionWidth(ui().statsSeen);
        const int nw = wg::captionWidth(nums);
        wg::drawCaption(d, gRender, ui().statsSeen, sn.x, sn.y, kTextDim, kBg);
        wg::drawCaption(d, gRender, nums, sn.right() - nw, sn.y, kTextDim, kBg);
        wg::drawProgress(d, sn.x + ww + 6, sn.y + 2, sn.w - ww - nw - 12, 3,
                         static_cast<int>(s.seen()), static_cast<int>(s.total));
    } else {
        char line[40];
        snprintf(line, sizeof line, ui().summarySeenFmt, (unsigned)s.seen(), (unsigned)s.total);
        if (wg::captionWidth(line) > sn.w) snprintf(line, sizeof line, "%s", nums);
        gRender.drawLabel(d, line, sn.x, sn.y, sn.w, 8, kTextDim, kBg,
                          tgc::Renderer::kTextBuiltinSmall, tgc::Align::Centre);
        wg::drawProgress(d, sn.x, sn.y + 10, sn.w, 3, static_cast<int>(s.seen()),
                         static_cast<int>(s.total));
    }
    backHints(d);
    present();
}

// Settings > Device > About: the firmware's name, version and licence, the
// decks' licence, the UI font and where the source lives; then the loaded deck
// and the credits of its data (Credits.h, from docs/data-sources.md). Each item is
// a label-face heading over its value in captions, so it works before any deck
// is loaded. Two groups -- the firmware, the deck -- laid out by
// ui/TextLayout.h: everything on one page when it fits, else a group that does
// not fit the rest of a page starts the next one. A tap turns the page (after
// the last, back to the first; the header shows "1/2"); the side button goes
// back.
int gAboutPage = 0;

// The app's name, たまごっちょ, in the label face (it has these kana).
void drawAppName(lgfx::LovyanGFX& d, int x, int y, int h, uint16_t fg) {
    gRender.drawLabel(d, tgc::credits::kAppName, x, y, W - x, h, fg, kBlack);
}

void drawAbout() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    constexpr int kLabelH = 20;  // a heading in the label face
    constexpr int kLineH = 10;   // a caption line (8 px) and 2 px leading
    constexpr int kGap = 3;      // between the items of one group
    constexpr int kGroupGap = 8;
    const int top = kHeaderH + 3;
    d.fillScreen(kBlack);

    struct Item {
        const char* label;  // a heading ("Firmware:", its colon dropped); "" for none
        const char* value;
        bool title;         // the app's name over the version
        bool newGroup;
    };
    namespace cr = tgc::credits;
    const bool deckOpen = gPack->isOpen();
    std::vector<Item> items = {
        {"", tgc::firmwareVersion(), true, true},
        {ui().aboutFirmware, cr::kFirmwareLicence, false, false},
        {ui().aboutDecks, cr::kDecksLicence, false, false},
        {ui().aboutUiFont, cr::kUiFont, false, false},
        {"", cr::kRepo, false, false},
        {ui().aboutDeck, deckOpen ? gPack->deckName() : ui().aboutNoDeck, false, true},
        {ui().aboutKanji, cr::kKanjidic, false, false},
    };
    // KanjiVG data is in a deck only when it was built with stroke order.
    if (!deckOpen || gPack->hasStrokeOrder())
        items.push_back({ui().aboutStrokes, cr::kKanjiVg, false, false});
    items.push_back({ui().aboutJlpt, cr::kJlpt, false, false});
    items.push_back({ui().aboutReadings, cr::kReadings, false, false});
    items.push_back({ui().aboutMeanings, cr::kMeanings, false, false});
    items.push_back({ui().aboutGlyphs, cr::kGlyphs, false, false});

    // Each value wrapped to the width in the caption font (6 px a character).
    const int maxChars = (W - 8) / 6;
    std::vector<std::vector<tgc::TextSpan>> lines;
    std::vector<tgc::PageBlock> blocks;
    for (const Item& it : items) {
        lines.push_back(tgc::wrapText(it.value, maxChars));
        const int head = (it.title || *it.label) ? kLabelH : 0;
        blocks.push_back({head + static_cast<int>(lines.back().size()) * kLineH, it.newGroup});
    }
    const int pageH = H - kHintH - 2 - top;
    const std::vector<tgc::PageSlot> slots = tgc::layoutPages(blocks, pageH, kGap, kGroupGap);
    const int pages = tgc::pageCount(slots);
    if (gAboutPage >= pages) gAboutPage = 0;

    char pageNo[12] = "";
    if (pages > 1) snprintf(pageNo, sizeof pageNo, "%d/%d", gAboutPage + 1, pages);
    menuHeader(d, ui().aboutTitle, pageNo[0] ? pageNo : nullptr);

    const int bottom = top + pageH;  // a block taller than a page is cut here
    for (size_t i = 0; i < items.size(); ++i) {
        if (slots[i].page != gAboutPage) continue;
        int y = top + slots[i].y;
        if (items[i].title) {
            drawAppName(d, kGutter, y, kLabelH, kText);
            y += kLabelH;
        } else if (*items[i].label) {
            std::string label(items[i].label);
            if (!label.empty() && label.back() == ':') label.pop_back();
            gRender.drawLabel(d, label.c_str(), kGutter, y, W - 2 * kGutter, kLabelH, kText, kBg);
            y += kLabelH;
        }
        for (const tgc::TextSpan& l : lines[i]) {
            if (y + 8 > bottom) break;
            const std::string s(items[i].value + l.begin, l.len);
            wg::drawCaption(d, gRender, s.c_str(), kGutter, y, kTextDim, kBg);
            y += kLineH;
        }
    }
    backHints(d, pages > 1 ? ui().hintMore : nullptr);
    present();
}

void openAbout() {
    gAboutPage = 0;
    gScreen = Screen::About;
    drawScreen();
}

// A QR code of `text`, as large as fits a `box` px square, with a white
// 2-module quiet zone (the spec asks for 4; phone cameras read 2 fine, and on a
// 135 px screen every pixel goes to the modules). Top-left at (x, y). Returns
// the side drawn, 0 when the text is too long for a legible code.
int qrSide(const char* text, int box) {
    const int version = tgc::qrVersionForBytes(std::strlen(text));
    if (version == 0) return 0;
    const int cells = tgc::qrModules(version) + 4;
    const int px = box / cells;
    return px > 0 ? cells * px : 0;
}

int drawQr(lgfx::LovyanGFX& d, const char* text, int x, int y, int box) {
    const int side = qrSide(text, box);
    if (side == 0) return 0;
    const int version = tgc::qrVersionForBytes(std::strlen(text));
    const int modules = tgc::qrModules(version);
    const int px = side / (modules + 4);
    d.fillRect(x, y, side, side, kWhite);
    // Exactly modules * px wide, so qrcode() uses px-sized modules with no
    // margin of its own; starting at our version it never needs a larger one.
    d.qrcode(text, x + 2 * px, y + 2 * px, modules * px, static_cast<uint8_t>(version));
    return side;
}

// The page address in text set `set`: whole when it fits `w`, else without
// "http://" and the trailing '/' (a browser takes the bare IP too).
void shownUrl(const char* url, int w, uint8_t set, char* out, size_t n) {
    snprintf(out, n, "%s", url);
    if (gRender.textWidth(out, set) <= w) return;
    const char* host = std::strncmp(url, "http://", 7) == 0 ? url + 7 : url;
    snprintf(out, n, "%s", host);
    const size_t len = std::strlen(out);
    if (len > 0 && out[len - 1] == '/') out[len - 1] = 0;
}

// "4.9" (MB, one decimal), with " MB" when `unit`.
void formatMb(size_t bytes, bool unit, char* out, size_t n) {
    const unsigned tenths = static_cast<unsigned>(bytes / (1024u * 1024u / 10u));
    snprintf(out, n, unit ? "%u.%u MB" : "%u.%u", tenths / 10u, tenths % 10u);
}

// The free space for decks: "free 4.9 / 5.9 MB" over a bar of it (13 px).
void drawFreeSpace(lgfx::LovyanGFX& d, int x, int y, int w, tgc::Align align) {
    const size_t total = LittleFS.totalBytes();
    const size_t freeB = tgc::fsFreeBytes();
    char a[16], b[16], line[48];
    formatMb(freeB, false, a, sizeof a);
    formatMb(total, true, b, sizeof b);
    snprintf(line, sizeof line, ui().wifiFreeFmt, a, b);
    gRender.drawLabel(d, line, x, y, w, 8, tgc::theme::kTextDim, kBlack,
                      tgc::Renderer::kTextBuiltinSmall, align);
    wg::drawProgress(d, x, y + 10, w, 3, static_cast<int>(freeB / 1024u),
                     static_cast<int>(total / 1024u));
}

// Wi-Fi portal screen (WifiPortal.h; docs/ux-refresh.md, Wi-Fi mockup).
// Serving: a QR code -- in setup mode the one that joins the device's access
// point, or (a tap) the one that opens the page; in home mode the page's
// address -- with what to scan for over it, then the network, the address in
// the label face (what to type if the QR does not scan), the portal's status
// (or a download's progress) and the free space as a bar. Portrait stacks them
// under the QR, landscape puts them right of it. Joining and failed: the
// network, why, and setup mode in the hint bar. The UI fonts need no pack, so
// it works before one is open.
void drawWifi() {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().wifiTitle, nullptr, gPortal.active() || gHome.active());
    // The side button (closeWifi()): stops the portal, starts studying on the
    // boot-time screen once a deck is there, and does nothing there before.
    wg::Hints hints;
    if (!(gWifiBootUpload && !gPack->isOpen())) {
        hints.side = &tgc::kIconBack;
        hints.sideWord = gWifiBootUpload ? ui().wifiStudyHint : ui().hintStop;
    }

    if (gWifiStep != WifiStep::Serving) {
        const bool failed = gWifiStep == WifiStep::Failed;
        const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;
        gRender.drawCentred(d, failed ? gNetErrTitle : ui().wifiConnectingTo, 4, mid - 34, W - 8,
                            40, 2, failed ? kWarn : kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, failed ? gNetErrDetail : gHome.ssid(), 4, mid + 10, W - 8, 3,
                         failed ? kText : kAccentHi);
        if (failed) hints.tap = ui().wifiTapRetry;
        hints.hold = ui().wifiHoldSetup;
        wg::drawHintBar(d, gRender, W, H, hints);
        present();
        return;
    }

    const bool home = gPortalMode == PortalMode::Home;
    const bool land = W > H;
    const bool joinQr = !home && gQrPage == 0;
    const std::string qrText =
        joinQr ? tgc::wifiQrOpenNetwork(gPortal.ssid()) : std::string(gPortal.url());
    const char* qrLabel = joinQr ? ui().wifiScanJoin : ui().wifiScanOpen;

    int y = kHeaderH + 3;
    int tx = kGutter, tw = W - 2 * kGutter;
    tgc::Align align = tgc::Align::Centre;
    if (land) {
        const int side = drawQr(d, qrText.c_str(), kGutter, y, H - kHintH - 2 - y);
        tx = kGutter + side + 6;
        tw = W - kGutter - tx;
        align = tgc::Align::Left;
        y += drawCaptionLines(d, qrLabel, tx, y, tw, 2, kText, align) * 10 + 2;
    } else {
        drawCaptionLines(d, qrLabel, tx, y, tw, 1, kText);
        y += 11;
        // As large as fits: on the StickS3, 4 px modules for a version 2 code
        // (the page's address) and 3 px for the version 3 join code (the
        // "Tamagotcho-XXXX" payload is 33 bytes, one more than version 2 holds).
        const int box = std::min(W - 2 * kGutter, (H - y) * 56 / 100);
        const int side = qrSide(qrText.c_str(), box);
        drawQr(d, qrText.c_str(), (W - side) / 2, y, box);
        y += side + 4;
    }
    // Caption lines under (beside) the QR code.
    auto caption = [&](const char* s, uint16_t color, int lines = 1) {
        if (s && *s) y += drawCaptionLines(d, s, tx, y, tw, lines, color, align) * 10 + 1;
    };
    char buf[72];
    if (home) {
        snprintf(buf, sizeof buf, ui().wifiViaFmt, gPortal.ssid());
        caption(buf, kTextDim);
    } else {
        caption(gPortal.ssid(), kText);
    }
    shownUrl(gPortal.url(), tw, tgc::Renderer::kTextUi, buf, sizeof buf);
    gRender.drawLabel(d, buf, tx, y, tw, 20, kAccentHi, kBg, tgc::Renderer::kTextUi, align);
    y += 21;
    if (gPortal.mdnsUrl()[0]) {
        shownUrl(gPortal.mdnsUrl(), tw, kSmall, buf, sizeof buf);
        caption(buf, kTextDim);
    }

    // A download the page asked for: its progress; the side button cancels.
    if (gNetBusy) {
        const int pct = gNetPct < 0 ? 0 : gNetPct;
        snprintf(buf, sizeof buf, ui().wifiDownloadFmt, gNetDeck, (unsigned)pct);
        caption(buf, kWarn);
        wg::drawProgress(d, tx, y + 1, tw, 3, pct, 100);
        wg::Hints busy;
        busy.side = &tgc::kIconBack;
        busy.sideWord = ui().hintCancel;
        wg::drawHintBar(d, gRender, W, H, busy);
        present();
        return;
    }
    // The boot-time fallback's reason first; the status line then gets one
    // line so the free space still fits the portrait screen.
    if (gWifiNote[0]) caption(gWifiNote, kWarn, 2);
    if (home && !gPortal.linkUp())
        caption(ui().wifiLinkLost, kWarn);
    else
        caption(gPortal.lastMessage(), kOkHi, gWifiNote[0] ? 1 : 2);
    drawFreeSpace(d, tx, y + 2, tw, align);

    if (!home) hints.tap = ui().wifiTapOtherQr;
    if (gWifiBootUpload && !gPack->isOpen() && tgc::HomeWifi::hasCredentials())
        hints.hold = ui().wifiGetDecksHint;
    wg::drawHintBar(d, gRender, W, H, hints);
    present();
}

// Settings > Wi-Fi: the web portal (home mode once a network is saved), the
// home network with its test (the network's name is the row's value), setup
// mode (the access point, e.g. to change networks) and "forget". The menu
// rows; a note above the hint bar says what the last action did.
constexpr int kWifiMenuRows = 4;

void drawWifiMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().wifiTitle);
    gWifiMenuList.setCount(kWifiMenuRows);
    layoutList(gWifiMenuList, kHeaderH, H - kHintH - kNoteH, layout().row1H);
    const char* home = gWifiMenuSsid.length() ? gWifiMenuSsid.c_str() : ui().wifiHomeNone;
    wg::drawList(d, gWifiMenuList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().wifiMenuRows[i];
        if (i == 1) {
            row.value = home;
            row.keepLabel = true;
        }
        row.chevron = true;
        row.focused = f;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gWifiMenuNote, gWifiMenuNoteColor);
    menuHints(d, ui().hintOpen);
    present();  // a move clears the note: the whole frame
}

// Settings > Wi-Fi > Home network: joining, checking (NTP and the catalog),
// then the result -- network, IP, signal, catalog, network time -- with the
// radio off again; or why it failed, or that no network is saved. A hold
// tests again.
void drawNetTest() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().testTitle, nullptr, gHome.active());
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;

    if (gTestStep == NetTestStep::NoNetwork) {
        gRender.drawCentred(d, ui().wifiNoNetwork, 4, mid - 40, W - 8, 40, 2, kWarn, kBg,
                            tgc::Renderer::kTextUi);
        drawCaptionLines(d, ui().testNoNetworkHint, 4, mid + 4, W - 8, 4, kText);
        backHints(d);
        present();
        return;
    }
    if (gTestStep == NetTestStep::Joining || gTestStep == NetTestStep::Failed) {
        const bool failed = gTestStep == NetTestStep::Failed;
        gRender.drawCentred(d, failed ? gTestFail : ui().netConnecting, 4, mid - 34, W - 8, 40, 2,
                            failed ? kWarn : kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        backHints(d, nullptr, failed ? ui().testHoldAgain : nullptr);
        present();
        return;
    }
    // Checking / done: the network in the label face, then caption lines.
    int y = kHeaderH + 4;
    gRender.drawLabel(d, gHome.ssid(), 4, y, W - 8, 20, kText, kBg, tgc::Renderer::kTextUi,
                      tgc::Align::Centre);
    y += 24;
    const int step = H > W ? 5 : 2;
    auto line = [&](const char* s, uint16_t color, int lines = 2) {
        if (s && *s) y += drawCaptionLines(d, s, 4, y, W - 8, lines, color) * 10 + step;
    };
    char buf[72];
    snprintf(buf, sizeof buf, ui().testIpFmt, gTestIp.c_str());
    line(buf, kTextDim);
    snprintf(buf, sizeof buf, ui().testSignalFmt, gTestRssi,
             ui().testQuality[tgc::rssiQuality(gTestRssi)]);
    line(buf, kTextDim);
    const bool done = gTestStep == NetTestStep::Done;
    if (!done) {
        line(ui().testChecking, kWarn);
    } else {
        line(gTestCatalog, gTestCatalogColor);
        if (gTestTime >= 0)
            line(gTestTime ? ui().testTimeOk : ui().testTimeFail, gTestTime ? kOkHi : kWarn);
    }
    backHints(d, nullptr, done ? ui().testHoldAgain : nullptr);
    present();
}

// "Get decks" screen (docs/ux-refresh.md, get-decks mockup). Connecting /
// reading: what it is doing over which network. List: the network and the
// catalog's size, then one row per catalog deck -- its title, "EN · 79 kanji"
// and the size; a deck already here is muted with a check, one here in
// another build has a download mark and "update" -- and a note line over the
// hint bar (what the last download did). While a deck downloads, its row shows
// the percentage and a bar, and the side button cancels. Error: a title and a
// wrapped detail. The UI fonts need no pack, so it works before one is open.
tgc::ListView gNetList;  // the catalog's window; gNetSel is its focus

void drawGetDecks() {
    using namespace tgc::theme;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().getDecksTitle, nullptr, gHome.active());
    const int mid = kHeaderH + (H - kHeaderH - kHintH) / 2;

    if (gNetStep == NetStep::Connecting || gNetStep == NetStep::Catalog) {
        const char* line = gNetStep == NetStep::Connecting ? ui().netConnecting : ui().netCatalog;
        gRender.drawCentred(d, line, 4, mid - 34, W - 8, 40, 2, kText, kBg, tgc::Renderer::kTextUi);
        drawCaptionLines(d, gHome.ssid(), 4, mid + 10, W - 8, 2, kAccentHi);
        wg::Hints h;
        h.side = &tgc::kIconBack;
        h.sideWord = ui().hintCancel;
        wg::drawHintBar(d, gRender, W, H, h);
        present();
        return;
    }
    if (gNetStep == NetStep::Error) {
        const int top = kHeaderH + 6;
        const int titleLines = gRender.drawCentred(d, gNetErrTitle, 4, top, W - 8, 40, 2, kWarn,
                                                   kBg, tgc::Renderer::kTextUi);
        const int detailY = top + 20 * std::max(titleLines, 1) + 6;
        drawCaptionLines(d, gNetErrDetail, 4, detailY, W - 8, (H - kHintH - 2 - detailY) / 10,
                         kText);
        backHints(d, nullptr, ui().netHoldRetry);
        present();
        return;
    }

    const int n = static_cast<int>(gCatalog.size());
    char line[72];
    snprintf(line, sizeof line, ui().netListFmt, gHome.ssid(), (unsigned)n);
    gRender.drawLabel(d, line, kGutter, kHeaderH + 3, W - 2 * kGutter, 8, kTextDim, kBg, kSmall);
    const int top = kHeaderH + 12;
    if (n == 0) {
        drawCaptionLines(d, ui().netEmpty, 4, mid - 10, W - 8, 3, kText);
    } else {
        const bool wide = wideRows();
        gNetList.setCount(n);
        layoutList(gNetList, top, H - kHintH - kNoteH, wide ? layout().row1H : layout().row2H);
        gNetList.setFocus(gNetSel);
        wg::drawList(d, gNetList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
            const tgc::CatalogEntry& e = gCatalog[i];
            const tgc::DeckState state = gCatalogState[i];
            char lang[8] = "?";
            if (!e.lang.empty()) {
                snprintf(lang, sizeof lang, "%s", e.lang.c_str());
                for (char* p = lang; *p; ++p)
                    if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
            }
            char caption[48], size[16], value[64];
            if (state == tgc::DeckState::OnDevice)
                snprintf(caption, sizeof caption, "%s · %s", lang, ui().netOnDevice);
            else if (state == tgc::DeckState::Update)
                snprintf(caption, sizeof caption, "%s · %s", lang, ui().netHoldUpdate);
            else
                snprintf(caption, sizeof caption, ui().netCardsFmt, lang, (unsigned)e.cards);
            formatMb(e.bytes, true, size, sizeof size);
            const std::string title = tgc::catalogDisplayName(e);
            wg::Row row;
            row.label = title.c_str();
            row.focused = f;
            row.keepLabel = true;
            row.muted = state == tgc::DeckState::OnDevice;
            if (state == tgc::DeckState::OnDevice) {
                row.mark = &tgc::kIconCheck;
                row.markColor = kOkHi;
            } else if (state == tgc::DeckState::Update) {
                row.mark = &tgc::kIconDown;
                row.markColor = kWarn;
            }
            if (gNetBusy && f) {
                const int pct = gNetPct < 0 ? 0 : gNetPct;
                snprintf(value, sizeof value, "%d%%", pct);
                row.progress = pct;
            } else if (wide) {
                // One line: the caption and the size right of the title.
                if (state == tgc::DeckState::OnDevice)
                    snprintf(value, sizeof value, "%s", caption);
                else
                    snprintf(value, sizeof value, "%s · %s", caption, size);
            } else {
                // Two lines: the size of a new deck right of the title (a deck
                // with a mark has no room for it).
                snprintf(value, sizeof value, "%s", state == tgc::DeckState::New ? size : "");
            }
            row.value = value;
            if (!wide) row.caption = caption;
            wg::drawRow(d, gRender, x, y, w, h, row);
        });
    }
    menuNote(d, gNetNote, gNetNoteColor);

    wg::Hints h;
    h.side = &tgc::kIconBack;
    if (gNetBusy) {
        h.sideWord = ui().hintCancel;
    } else {
        h.sideWord = ui().hintBack;
        if (n > 0) {
            const tgc::DeckState state = gCatalogState[gNetSel];
            h.tap = ui().hintMove;
            h.hold = state == tgc::DeckState::OnDevice ? ui().netHoldStudy
                     : state == tgc::DeckState::Update ? ui().netHoldUpdate
                                                       : ui().netHoldDownload;
        }
    }
    wg::drawHintBar(d, gRender, W, H, h);
    present();
}

// The detail card's kanji box (gQuizG's prompt box, where the quiz showed the
// kanji): the static set-0 glyph, or -- while the pack's stroke data animates
// -- every stroke: those written so far in `text`, the current one in `warn`,
// the rest as a `ghost`, so the final shape shows throughout. The masks live
// in the em square drawKanji() uses.
void drawDetailKanji(lgfx::LovyanGFX& d, uint32_t cp) {
    const QuizGeom& g = gQuizG;
    d.fillRect(g.px, g.py, g.pw, g.ph, kBlack);
    if (!gAnimOn) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, g.px, g.py, g.pw, g.ph, kWhite, kBlack);
        return;
    }
    const int px = gPack->header().largePx ? gPack->header().largePx : 64;
    const int emX = g.px + (g.pw - px) / 2;
    const int emY = g.py + (g.ph - px) / 2;
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    const int n = static_cast<int>(gAnimStrokes);
    for (int i = gAnimFrame + 1; i < n; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY, tgc::theme::kGhost);
    for (int i = 0; i <= gAnimFrame; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           i == gAnimFrame ? tgc::theme::kWarn : tgc::theme::kText);
#if defined(TGC_QUIZ_LOG)
    // One line per animation loop (every frame draws every stroke now).
    if (gAnimFrame == n - 1)
        QLOG("R U+%04X strokes %d draw %u us\n", (unsigned)cp, n, (unsigned)(micros() - t0));
#endif
}

// The detail card's header: "DETAIL", then the stroke counter while the
// strokes animate ("stroke 4/6", or "4/13" when the worded counter of the last
// stroke would not fit; decided per card, ui/QuizUi.h) or the stroke count.
void drawDetailHeader(lgfx::LovyanGFX& d, const tgc::Card& card) {
    wg::Header h;
    h.title = ui().detailTitle;
    h.battery = tgc::board::batteryPercent();
    char counter[32] = "";
    if (gAnimOn) {
        const int room = tgc::headerPillRoom(W, 0, h.battery >= 0) -
                         wg::captionWidth(h.title) - 6;
        const bool worded = tgc::strokeCounterWorded(ui().detailStrokeFmt, gAnimStrokes, room);
        snprintf(counter, sizeof counter, worded ? ui().detailStrokeFmt : "%u/%u",
                 (unsigned)(gAnimFrame + 1), (unsigned)gAnimStrokes);
    } else if (card.strokes) {
        snprintf(counter, sizeof counter, ui().detailStrokesFmt, (unsigned)card.strokes);
    }
    if (counter[0]) h.right = counter;
    wg::drawHeader(d, gRender, W, h);
}

// Detail card (roadmap phase 3, restyled in docs/ux-refresh.md §4 phase 2): a
// tap after a wrong reveal opens it. Under the kanji: what was picked (a cross
// and the option; for meaning -> kanji the picked kanji's meaning) and the
// card's own accuracy, the meanings (UI font, what room is left, at most three
// lines), and the readings, one line each behind an ON / KUN pill (the UI
// font draws kana from the pack's text set). Landscape mirrors the quiz:
// kanji left, details right. When the pack carries stroke data for the card,
// the kanji animates (drawDetailFrame(), the Screen::Detail branch of loop()).
void drawDetail() {
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBg);
    gQuizG = quizGeom(quizHintsShown());
    const QuizGeom& g = gQuizG;
    drawDetailHeader(d, card);
    drawDetailKanji(d, card.codepoint);

    constexpr uint8_t kLabel = tgc::Renderer::kTextUi;
    const int lh = gRender.lineHeight(kLabel);
    const bool landscape = W > H;
    const int x = landscape ? g.pw + kGutter : kGutter;
    const int w = W - x - kGutter;
    int y = landscape ? kHeaderH + 3 : g.py + g.ph + 2;
    const int bottom = (g.hints ? H - kHintH : H) - 2;

    // What was picked, and "correct 3/5" (the bare "3/5" when the pick
    // needs the room).
    const tgc::Record* rec = gSrs.find(card.codepoint);
    char accFull[32] = "", accBare[16] = "";
    if (rec && rec->reps > 0) {
        const unsigned total = rec->reps;
        const unsigned correct = rec->reps > rec->lapses ? rec->reps - rec->lapses : 0;
        snprintf(accFull, sizeof accFull, ui().detailAccuracyFmt, correct, total);
        snprintf(accBare, sizeof accBare, "%u/%u", correct, total);
    }
    const char* picked = gSel >= 0 && gSel < gQ.count ? gQ.options[gSel].c_str() : "";
    const int markW = tgc::kIconCross.w + 4;
    const int pickedW = gRender.textWidth(picked, kLabel);
    const char* acc = nullptr;
    if (accFull[0])
        acc = markW + pickedW + 6 + wg::captionWidth(accFull) <= w ? accFull : accBare;
    const int accW = acc ? wg::captionWidth(acc) : 0;
    wg::drawIcon(d, tgc::kIconCross, x, y + (lh - tgc::kIconCross.h + 1) / 2, kErrHi);
    gRender.drawLabel(d, picked, x + markW, y, w - markW - (acc ? accW + 6 : 0), lh, kErrHi, kBg,
                      kLabel);
    if (acc) wg::drawCaption(d, gRender, acc, x + w - accW, y + (lh - 7 + 1) / 2, kTextDim, kBg);
    y += lh + 2;

    // Only the kinds the card has get a row (ui/QuizUi.h).
    tgc::DetailReading readings[2];
    const int readingRows = tgc::detailReadings(card, readings);
    const int lines = std::max(1, std::min(3, (bottom - y - readingRows * (lh + 2)) / lh));
    y += gRender.drawWrapped(d, card.meanings, x, y, w, lines, kText, kBg, kLabel) * lh + 2;

    // Both readings start at the same x, after the wider tag.
    const int tagW = std::max(wg::pillWidth(ui().detailOn), wg::pillWidth(ui().detailKun));
    for (int i = 0; i < readingRows && y + lh <= bottom + 2; ++i) {
        const char* tag = readings[i].kind == tgc::ReadingKind::Kun ? ui().detailKun : ui().detailOn;
        wg::drawPill(d, gRender, x, y + (lh - kPillH + 1) / 2, tag, kModeReading, kSurfaceHi);
        gRender.drawLabel(d, readings[i].text.c_str(), x + tagW + 5, y, w - tagW - 5, lh, kText, kBg,
                          kLabel);
        y += lh + 2;
    }

    if (g.hints) {
        wg::Hints hints;
        hints.tap = ui().hintNext;
        hints.side = &tgc::kIconMenu;
        wg::drawHintBar(d, gRender, W, H, hints);
    }
    present();
}

// One stroke-animation step: only the header (its counter) and the kanji's
// em square are redrawn and pushed.
void drawDetailFrame() {
    tgc::Card card;
    if (!questionCard(&card)) return;
    auto& d = *gCanvas;
    drawDetailHeader(d, card);
    drawDetailKanji(d, card.codepoint);
    const QuizGeom& g = gQuizG;
    const int px = gPack->header().largePx ? gPack->header().largePx : 64;
    const int ex = std::max(g.px, g.px + (g.pw - px) / 2 - 1);
    const int ey = std::max(g.py, g.py + (g.ph - px) / 2 - 1);
    presentRegion(0, 0, W, kHeaderH);
    presentRegion(ex, ey, std::min(px + 2, g.pw), std::min(px + 2, g.ph));
}

void showDetail() {
    tgc::Card c{};  // stays zero when there is no card (drawDetail() then says so)
    questionCard(&c);
    // Start the stroke animation when the pack has stroke data for this card.
    uint16_t sc = 0;
    gAnimOn = gPack->strokeInfo(gQ.cardIndex, &sc, nullptr) && sc > 0;
    gAnimStrokes = sc;
    gAnimFrame = 0;
    gAnimNext = millis() + kStrokeMs;
    if (gAnimOn)
        QLOG("D U+%04X strokes=%u animating\n", (unsigned)c.codepoint, (unsigned)sc);
    else
        QLOG("D U+%04X\n", (unsigned)c.codepoint);
    gScreen = Screen::Detail;
    drawDetail();
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

// Rebuilds the queue and shows the quiz (or the done screen if nothing is due).
// Used after the date screen so newly due cards appear.
void leaveDate() {
    dateSyncCancel();
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// Applies the stepped days (setDateManual: later dates advance the index,
// earlier ones move the anchor back; either restarts the day carry, see
// docs/srs.md section 5), persists them, rebuilds the queue (cards may now be
// due) and goes back to where the date screen was opened from: Settings >
// Device > Date, or the quiz on a power-off boot.
void applyDate() {
    dateSyncCancel();
    const tgc::ManualDate r = gSrs.setDateManual(gDateSteps);
    char date[16];
    formatDate(gSrs.today(), date, sizeof date);
    Serial.printf("clock: set by hand, %+ld days: %s, day %u (%s)\n",
                  static_cast<long>(gDateSteps),
                  r == tgc::ManualDate::Advanced     ? "advanced"
                  : r == tgc::ManualDate::Reanchored ? "re-anchored"
                  : r == tgc::ManualDate::Same       ? "same day"
                                                     : "rejected",
                  (unsigned)gSrs.today(), date);
    gSrs.save(gSrsIo);
    gDirty = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gAfterDate != Screen::Quiz) {
        openDateMenu(false);
        return;
    }
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

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
bool applyWallClock(int64_t utcMs, const tgc::TzRule& zone) {
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
    if (gScreen == Screen::Date && gAfterDate == Screen::Quiz) {
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
// (blocking, at most kNtpTimeoutMs), radio off, then applyWallClock(), which
// answers the question and leaves for the quiz -- true when that happened. On
// any failure the radio is off too and the stepper comes up with a status line
// that says to set the date by hand.
bool dateSyncPoll() {
    if (gDateSync != DateSync::Joining) return false;
    const tgc::HomeWifi::State st = gHome.poll();
    if (st == tgc::HomeWifi::State::Connecting) return false;
    const bool joined = st == tgc::HomeWifi::State::Connected;
    int64_t utcMs = 0;
    const bool got = joined && gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs);
    netRadioOff();
    if (joined) gSwallowInput = true;  // presses made while blocked must not act now
    gDateSync = DateSync::None;
    if (got && applyWallClock(utcMs, gTz)) return true;
    gDateSync = joined ? DateSync::NoTime : DateSync::NoNetwork;
    // The status is news: the join's time does not count against the reader.
    gNetActivityAt = millis();
    Serial.printf("clock: no network time at boot (%s), asking by hand\n",
                  joined ? "no NTP answer" : "could not join");
    if (!gScreenOff) drawScreen();
    return false;
}

// --- Settings > Device > Date: loop ------------------------------------------

void dateMenuNote(const char* text, uint16_t color) {
    snprintf(gDateMenuNote, sizeof gDateMenuNote, "%s", text ? text : "");
    gDateMenuNoteColor = color;
}

// Opens the Date submenu: `fresh` from the settings home (first row, no note),
// else back from the stepper or the zone list (selection and note kept).
void openDateMenu(bool fresh) {
    if (fresh) {
        gDateMenuList.setCount(kDateMenuRows);
        gDateMenuList.reset(0);
        gDateMenuNote[0] = 0;
    }
    gScreen = Screen::DateMenu;
    drawScreen();
}

void openTzList() {
    gTzListView.setCount(tzListRows());
    gTzListView.reset(tzCurrentRow());
    gScreen = Screen::TzList;
    drawScreen();
}

void dateMenuLoop(int a, int b) {
    if (a == 1) {
        gDateMenuList.next();
        gDateMenuNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        if (gDateMenuList.focus() == 0)
            openDate(Screen::DateMenu);
        else
            openTzList();
    }
    if (b && gScreen == Screen::DateMenu) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

// A hold on the zone list: the zone chosen by hand (NVS `tzm`), or "From
// phone", which drops that choice. It takes effect for the day lengths now and
// for the next real time (NTP or the page); the day and the rollover stay
// where they are until then.
void applyTzChoice() {
    const int sel = gTzListView.focus();
    if (sel == 0) {
        if (gPrefs.isKey(kPrefTzManual)) gPrefs.remove(kPrefTzManual);
    } else {
        gPrefs.putString(kPrefTzManual, tgc::kTzZones[sel - 1].name);
    }
    loadTimeZone();
    if (gTzKnown)
        dateMenuNote(ui().tzSaved, tgc::theme::kOkHi);
    else
        dateMenuNote(ui().tzPhoneNone, tgc::theme::kWarn);
    openDateMenu(false);
}

void tzListLoop(int a, int b) {
    if (a == 1) {
        moveListFocus(gTzListView);
        drawScreen();
    } else if (a == 2) {
        applyTzChoice();
    }
    if (b && gScreen == Screen::TzList) openDateMenu(false);  // back, nothing changed
}

bool gQuietFrame = false;  // drawScreen() without its frame-time line

void drawScreen() {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    if (gScreen == Screen::Menu) drawMenu();
    else if (gScreen == Screen::Decks) drawDecks();
    else if (gScreen == Screen::Choice) drawChoice();
    else if (gScreen == Screen::Done) drawDone();
    else if (gScreen == Screen::Date) drawDate();
    else if (gScreen == Screen::DateMenu) drawDateMenu();
    else if (gScreen == Screen::TzList) drawTzList();
    else if (gScreen == Screen::Stats) drawStats();
    else if (gScreen == Screen::Detail) drawDetail();
    else if (gScreen == Screen::Wifi) drawWifi();
    else if (gScreen == Screen::GetDecks) drawGetDecks();
    else if (gScreen == Screen::WifiMenu) drawWifiMenu();
    else if (gScreen == Screen::NetTest) drawNetTest();
    else if (gScreen == Screen::About) drawAbout();
    else if (gScreen == Screen::FirstLang) drawFirstLang();
    else if (gScreen == Screen::Reset) drawReset();
    else if (gScreen == Screen::ResetConfirm) drawResetConfirm();
    else drawQuiz();
    // Frame cost (docs/ux-refresh.md §4, phase 1): the whole redraw and, in
    // it, the push to the panel (rows: H, or fewer for a partial push). A hold
    // frame logs its own line (drawHoldFrame()).
    if (!gQuietFrame)
        QLOG("F screen %d: %u us, push %u us (%d rows)\n", static_cast<int>(gScreen),
             (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

// Rotate the panel (0-3, see board::setRotation), pick up the new width/height,
// recreate the back buffer for it and redraw. The canvas is recreated here so a
// rotation can never leave the previous orientation's buffer mis-sized.
// Persisting the setting is the caller's job: Auto rotations are never stored.
void applyRotation(uint8_t r) {
    tgc::board::setRotation(r);
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    drawScreen();
}

bool orientAuto() { return gOrientPref == kOrientAuto && tgc::board::hasAccel(); }

// The rotation a fixed setting asks for. Auto on a board whose accelerometer is
// missing or failed to start behaves as portrait (the setting is kept).
uint8_t fixedRotation() { return gOrientPref == kOrientLandscape ? 1 : 0; }

// The orientation setting. Up to 2026-10-18 it was the bool `land`; that key
// is read once, rewritten as `orient` and removed, so a landscape user keeps
// landscape. New users get Auto (2026-10-19, once the axes were confirmed on
// the StickS3), which a board without an accelerometer draws as portrait.
uint8_t loadOrientPref() {
    if (gPrefs.isKey("orient")) {
        const uint8_t v = gPrefs.getUChar("orient", kOrientAuto);
        return v <= kOrientAuto ? v : kOrientAuto;
    }
    const uint8_t v = gPrefs.getBool("land", false) ? kOrientLandscape : kOrientAuto;
    gPrefs.putUChar("orient", v);
    gPrefs.remove("land");
    return v;
}

// First rotation at boot. In Auto mode one accelerometer sample decides at once
// (no settle time: nothing is on screen yet); a flat or unclear pose falls back
// to portrait. The filter then starts from what is shown.
uint8_t bootRotation() {
    if (!orientAuto()) return fixedRotation();
    int r = tgc::OrientationFilter::kNone;
    float x, y, z;
    for (int tries = 0; tries < 10; ++tries) {  // the first sample needs ~10 ms
        if (tgc::board::readAccel(&x, &y, &z)) {
            r = gOrient.classify(x, y, z);
            break;
        }
        delay(5);
    }
    if (r == tgc::OrientationFilter::kNone) r = 0;
    gOrient.reset(r);
    return static_cast<uint8_t>(r);
}

// Auto orientation, once per loop(): sample the accelerometer every kImuPollMs
// while the panel is on and apply a settled rotation only while no button is
// down, so the picture never turns in the middle of a press (the change waits
// for the release). A rotation is not user activity: it neither brightens a
// dimmed panel nor delays sleep, so a device turning in a bag still sleeps.
void orientTick(uint32_t now, bool buttonDown) {
    if (!orientAuto() || gScreenOff) return;
    if (now - gImuPollAt >= kImuPollMs) {
        gImuPollAt = now;
        if (tgc::board::readAccel(&gAccel[0], &gAccel[1], &gAccel[2]))
            gOrient.update(gAccel[0], gAccel[1], gAccel[2], now);
    }
    const int want = gOrient.current();
    if (want == tgc::OrientationFilter::kNone || buttonDown) return;
    if (want == tgc::board::rotation()) return;
    QLOG("O rotation %d (a=%.2f %.2f %.2f)\n", want, gAccel[0], gAccel[1], gAccel[2]);
    applyRotation(static_cast<uint8_t>(want));
}

// Opens `path`, replacing the live pack only after the new one validates.
// The new reader is opened next to the live one and swapped in only once it
// validated, so a failure leaves the live pack (and the renderer and queue that
// point at it) untouched.
bool loadPack(const String& path) {
    auto src = std::make_unique<tgc::FsPackSource>(LittleFS.open(path, "r"));
    if (!src->ok()) return false;
    auto next = std::make_unique<tgc::PackReader>();
    if (next->open(*src, tgc::psramAllocator()) != tgc::PackError::Ok) return false;
    // Progress is global (keyed by codepoint), so persist it before the swap.
    if (gDirty) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gRender.end();
    gPack = std::move(next);  // the old reader closes before its source goes
    gSource = std::move(src);
    if (!gRender.begin(*gPack)) {
        // Out of memory for the glyph cache: leave a consistent "no pack" state
        // rather than a pack the renderer cannot draw.
        gPack->close();
        gCurrentPack = "";
        return false;
    }
    setUiForLang(gPack->header().lang);
    gCurrentPack = path;
    gQueue.rebuild(*gPack, gSrs, gRng);
    Serial.printf("pack %s: %u cards, %u glyphs, lang %s\n", gPack->deckName(),
                  (unsigned)gPack->cardCount(), (unsigned)gPack->glyphCount(), gPack->header().lang);
    return true;
}

// --- Wi-Fi portal integration (roadmap phase 4) ----------------------------

void portalFillStats(tgc::PortalStats& out) {
    const tgc::Stats st = gSrs.stats();
    out.day = gSrs.today();
    formatDate(gSrs.today(), out.date, sizeof out.date);
    out.dateSynced = gSrs.hasWallClock();
    out.seen = st.seen;
    out.learned = st.learned;
    out.answers = st.answers;
    out.again = st.again;
    out.reps = st.reps;
    out.lapses = st.lapses;
    out.streakCurrent = st.streakCurrent;
    out.streakBest = st.streakBest;
    out.reviewMs = st.reviewMs;
    out.packCards = gPack->isOpen() ? gPack->cardCount() : 0;
    if (gPack->isOpen()) {
        const int slash = gCurrentPack.lastIndexOf('/');
        snprintf(out.loadedPack, sizeof out.loadedPack, "%s",
                 gCurrentPack.substring(slash >= 0 ? slash + 1 : 0).c_str());
    }
}

// Called by the portal after /packs changed, and by Get decks after a download.
// Rescans the folder and keeps the live pack usable: reload the loaded pack (its
// contents may have changed), else fall back to the first remaining one, else
// leave no pack open. Both callers run under the settings home, whose
// "Continue" returns to the quiz, detail or done screen; when the live reader
// was replaced or closed, the question kept for it belongs to the old pack and
// must never be shown or graded. So Continue gets a fresh question from the
// rebuilt queue (or the done screen) -- or, with no pack left, the deck list,
// where "+ Get decks" still works. A failed reload that kept the old reader
// keeps its question too.
bool portalPacksChanged() {
    const tgc::PackReader* const before = gPack->isOpen() ? gPack.get() : nullptr;
    const String pathBefore = gCurrentPack;
    listPacks(gPacks);
    if (gPacks.empty()) {
        gRender.end();
        gPack->close();
        gSource.reset();
        gCurrentPack = "";
        gPrefs.remove("pack");
    } else {
        String want = gCurrentPack;
        if (want.isEmpty() || std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end())
            want = gPacks[0];
        if (loadPack(want) || loadPack(gPacks[0])) gPrefs.putString("pack", gCurrentPack);
    }
    if (!gPack->isOpen()) {
        gQueue.rebuild(*gPack, gSrs, gRng);  // empty: nothing refers to the old cards
        gBeforeMenu = Screen::Decks;
    } else if (gPack.get() != before) {
        if (gCurrentPack != pathBefore) gExtraNew = 0;  // another deck, as in selectPack()
        gQueue.rebuild(*gPack, gSrs, gRng, extraNewToday());
        gBeforeMenu = newQuestion() ? Screen::Quiz : Screen::Done;
    }
    return gPack->isOpen();
}

// --- Deleting progress (docs/srs.md section 2.1) ----------------------------

// Writes the state file through the safe path (temp + rename). False when it
// failed; the answers not yet saved then stay counted for the next flush.
bool saveProgress() {
    if (!gSrs.save(gSrsIo)) {
        Serial.printf("srs: save failed\n");
        return false;
    }
    gDirty = 0;
    return true;
}

// The code points of the pack at `path`, sorted and unique; none when it does
// not read (its kanji cannot be known, so nothing of it is forgotten).
std::vector<uint32_t> packCodepoints(const String& path) {
    std::vector<uint32_t> cps;
    tgc::FsPackSource src(LittleFS.open(path, "r"));
    if (src.ok()) tgc::readPackCodepoints(src, &cps);
    tgc::sortUnique(cps);
    return cps;
}

// The code points of every pack in /packs but `except`, sorted and unique.
std::vector<uint32_t> otherDecksCodepoints(const String& except) {
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == except) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    return all;
}

// The portal's POST /delete (WifiPortal::Hooks::onDeleteDeck): deletes pack
// `path` and the progress of its kanji that no other deck in /packs has, so a
// later download of it starts from scratch; N5's kanji keep theirs while the
// Jouyou deck is installed. The lifetime statistics stay (docs/srs.md 2.1).
// Order: the records are forgotten and the state file saved (temp + rename)
// *before* the pack is removed. A power cut in between leaves the deck
// installed without that progress (deleting it again finishes the job), never
// records of a deck that is gone, which a new download of it would pick up
// again. A failed save (a full filesystem: deleting a deck is how space is
// made) is tried again once the pack is gone. Then the folder is rescanned as
// after any other change: a deleted loaded deck gives way to the first one
// left, and the queue is rebuilt (portalPacksChanged()).
bool portalDeleteDeck(const char* path, uint32_t* forgotten) {
    *forgotten = 0;
    const String p(path);
    if (!tgc::fsExists(p)) return false;
    const std::vector<uint32_t> deck = packCodepoints(p);
    const std::vector<uint32_t> gone = tgc::forgetOnDelete(deck, otherDecksCodepoints(p));
    const size_t n = gSrs.forget(gone.data(), gone.size());
    const bool saved = n == 0 || saveProgress();
    const bool removed = LittleFS.remove(p);
    if (!saved) saveProgress();
    Serial.printf("delete %s: %s; progress of %u of its %u kanji forgotten, %u kanji left\n",
                  path, removed ? "ok" : "failed", (unsigned)n, (unsigned)deck.size(),
                  (unsigned)gSrs.count());
    portalPacksChanged();
    *forgotten = static_cast<uint32_t>(n);
    return removed;
}

// Writes an NVS string only when it changed: the page posts on every load.
void putPrefString(const char* key, const char* value) {
    char old[64] = "";
    if (gPrefs.isKey(key)) gPrefs.getString(key, old, sizeof old);
    if (!gPrefs.isKey(key) || std::strcmp(old, value) != 0) gPrefs.putString(key, value);
}

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

// The page's language: the screens' own ("en"/"es"), or none under Auto while
// no deck is loaded (the page then follows the browser, as Get decks follows
// the phone's language in that case).
const char* portalUiLang() {
    return (gUiLangPref == tgc::kUiLangAuto && !gPack->isOpen()) ? "" : uiLangCode();
}

// The page sends the user to the device screen: undo an idle dim (the time
// spent typing on the phone is not button input).
void undoDim() {
    if (gDimmed) {
        tgc::board::setBrightness(gBrightness);
        gDimmed = false;
    }
}

// Stops the portal and opens Get decks in its place: the boot-time upload
// screen's hold, and the portal page's "Get decks on the device screen" (the
// portal's onGetDecks hook, called from poll() once its response is out) from
// the boot-time portal or the settings one. Get decks then leaves as it always
// does: back to the boot-time screen (or into the quiz once a deck is loaded),
// else the deck list. In home mode the station stays up and Get decks uses it.
void wifiToGetDecks(const char* phoneLang) {
    // Only an Auto UI with no deck loaded has no language of its own (it shows
    // English); then the phone's language orders the deck list instead.
    gGetDecksLangHint = phoneLang ? phoneLang : "";
    gPortal.end();
    ++gWifiEpoch;
    // openGetDecks() raises it again while the radio is on.
    if (!gHome.active()) setCpuFrequencyMhz(kCpuMhz);
    QLOG("W wifi: stopped for Get decks\n");
    undoDim();
    openGetDecks(gWifiBootUpload);
}

// "Switch to my Wi-Fi" / "Reconnect" on the page (the portal's onSwitchToHome
// hook, from poll() once its response is out): the portal again, in home mode.
void portalSwitchToHome() {
    QLOG("W wifi: page asked for home mode\n");
    undoDim();
    openPortal(PortalMode::Home);
}

// Home mode: a deck download the page asked for (the portal's onDeckDownload
// hook, blocking, from poll()). The Wi-Fi screen shows the progress.
tgc::DeckDownloadResult portalDeckDownload(const tgc::CatalogEntry& e, bool update) {
    ++gWifiEpoch;
    QLOG("W wifi: page download %s (%s)\n", e.name.c_str(), update ? "update" : "new");
    return downloadCatalogDeck(e);
}

const tgc::WifiPortal::Hooks kPortalHooks = {portalFillStats,    portalPacksChanged, portalTime,
                                             wifiToGetDecks,     portalUiLang,       portalSwitchToHome,
                                             portalDeckDownload, portalDeleteDeck};

// The mode the portal opens in: home once a network is saved, else setup.
PortalMode defaultPortalMode() {
    return tgc::HomeWifi::hasCredentials() ? PortalMode::Home : PortalMode::Setup;
}

// Why joining the home Wi-Fi failed (the radio is already off).
const char* joinFailureText() {
    using F = tgc::HomeWifi::Failure;
    const F f = gHome.failure();
    return f == F::NotFound ? ui().netNotFound
                            : (f == F::AuthFailed ? ui().netAuthFailed : ui().netNoConnect);
}

// The Wi-Fi screen's failed state: a title (why) and a detail (the network).
void wifiFailed(const char* title, const char* detail) {
    gWifiStep = WifiStep::Failed;
    snprintf(gNetErrTitle, sizeof gNetErrTitle, "%s", title);
    snprintf(gNetErrDetail, sizeof gNetErrDetail, "%s", detail ? detail : "");
    QLOG("W wifi: failed: %s | %s\n", gNetErrTitle, gNetErrDetail);
}

// Stops the portal and the radio, whichever mode ran; CPU back to normal.
void stopPortal() {
    if (gPortal.active()) {
        gPortal.end();
        QLOG("W wifi: stopped\n");
    }
    netRadioOff();
    setCpuFrequencyMhz(kCpuMhz);
}

// Opens the Wi-Fi screen in `mode` (from the Wi-Fi submenu, the page's switch
// to home mode, or at boot when there is no pack yet). Home mode joins the
// saved network first (wifiLoop() polls it) and then serves the page on it;
// setup mode starts the access point at once. Whatever ran before stops first.
// The boot-time upload mode is decided by the caller (setup() sets
// gWifiBootUpload); opening the portal from the settings never makes the side
// button inert.
void openPortal(PortalMode mode) {
    if (gPortal.active()) gPortal.end();
    netRadioOff();
    ++gWifiEpoch;
    gPortalMode = mode;
    gScreen = Screen::Wifi;
    gQrPage = 0;
    gWifiNote[0] = 0;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (mode == PortalMode::Home) {
        if (!gHome.begin()) {
            netRadioOff();
            setCpuFrequencyMhz(kCpuMhz);
            const bool none = gHome.failure() == tgc::HomeWifi::Failure::NoCredentials;
            wifiFailed(none ? ui().netNoWifi : ui().netNoConnect, none ? "" : gHome.ssid());
        } else {
            gWifiStep = WifiStep::Joining;
            QLOG("W wifi: joining %s for the portal\n", gHome.ssid());
        }
        drawScreen();
        return;
    }
    if (!gPortal.begin(kPortalHooks)) {
        setCpuFrequencyMhz(kCpuMhz);
        wifiFailed(gPortal.lastMessage(), "");
    } else {
        gWifiStep = WifiStep::Serving;
        QLOG("W wifi: setup mode ssid=%s url=%s\n", gPortal.ssid(), gPortal.url());
    }
    drawScreen();
}

// Builds the quiz from the pack the portal loaded and switches to it.
void startQuizFromPortal() {
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (!newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

void openWifiMenu(bool fresh);
void wifiMenuNote(const char* text, uint16_t color);

// Leaves the Wi-Fi screen (radio off): for the Wi-Fi submenu, or -- on the
// boot-time screen once the home-mode page brought a deck -- into the quiz. On
// the boot-time screen without a deck there is nothing to return to, so the
// side button does nothing there.
void closeWifi() {
    if (gWifiBootUpload && !gPack->isOpen()) return;
    stopPortal();
    if (gWifiBootUpload) {
        gWifiBootUpload = false;
        startQuizFromPortal();
        return;
    }
    openWifiMenu(false);
}

// On the boot-time upload screen in setup mode, move on to the quiz once a pack
// is loaded (an upload or a URL download). Home mode stays, so the page can
// fetch more decks; its side button then starts studying (closeWifi()). True
// when it took over the screen.
bool wifiUploadReadyToQuiz() {
    if (!gWifiBootUpload || !gPack->isOpen() || gPortalMode != PortalMode::Setup) return false;
    stopPortal();
    gWifiBootUpload = false;
    startQuizFromPortal();
    return true;
}

// loop() while on the Wi-Fi screen.
void wifiLoop(int a, int b) {
    if (gWifiStep == WifiStep::Joining) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            // The network is up anyway: take the real date from NTP first, as
            // Get decks does (blocking, at most kNtpTimeoutMs).
            netSyncClock();
            gSwallowInput = true;  // presses made while blocked must not act now
            gNetActivityAt = millis();
            if (gPortal.beginHome(kPortalHooks, gHome.ssid())) {
                gWifiStep = WifiStep::Serving;
                QLOG("W wifi: home mode on %s at %s\n", gHome.ssid(), gPortal.url());
            } else {
                netRadioOff();
                wifiFailed(ui().netNoConnect, gHome.ssid());
            }
            drawScreen();
            return;
        }
        if (st == tgc::HomeWifi::State::Failed) {
            setCpuFrequencyMhz(kCpuMhz);  // the radio is off already
            if (gWifiBootUpload) {
                // Nothing to study yet: the device's own network instead, and
                // why on its screen.
                char note[sizeof gWifiNote];
                snprintf(note, sizeof note, ui().wifiFallbackFmt, gHome.ssid(), joinFailureText());
                openPortal(PortalMode::Setup);
                if (gWifiStep == WifiStep::Serving) {
                    snprintf(gWifiNote, sizeof gWifiNote, "%s", note);
                    drawScreen();
                }
                return;
            }
            wifiFailed(joinFailureText(), gHome.ssid());
            drawScreen();
            return;
        }
        if (a == 2) openPortal(PortalMode::Setup);
        else if (b) closeWifi();  // cancel the join
        return;
    }
    if (gWifiStep == WifiStep::Failed) {
        if (a == 1) openPortal(gPortalMode);  // try again
        else if (a == 2) openPortal(PortalMode::Setup);
        else if (b) closeWifi();
        return;
    }
    // Serving. The page may leave this screen (Get decks), switch the mode or
    // run a download inside poll(); the input read before it is then stale.
    const uint32_t epoch = gWifiEpoch;
    gPortal.poll();
    if (gScreen != Screen::Wifi || epoch != gWifiEpoch) return;
    if (gPortal.consumeDirty()) drawScreen();
    if (wifiUploadReadyToQuiz()) return;
    if (b) {
        closeWifi();
    } else if (a == 1 && gPortalMode == PortalMode::Setup) {
        gQrPage ^= 1;  // the other QR code: join the AP / open the page
        drawScreen();
    } else if (a == 2 && gWifiBootUpload && !gPack->isOpen() && tgc::HomeWifi::hasCredentials()) {
        // Boot-time screen with a saved home Wi-Fi: Get decks on the device.
        wifiToGetDecks(nullptr);
    }
}

void openMenu() {
    gAdvance.cancel();  // back on a revealed answer, a press goes on
    if (gDirty) {  // entering the menu is a flush point (docs/srs.md section 4)
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gBeforeMenu = gScreen;
    gScreen = Screen::Menu;
    gMenuPage = st::Page::Home;
    menuList().setCount(st::pageItems(gMenuPage).count);
    menuList().reset(0);
    drawScreen();
}

void closeMenu() {
    if (gBeforeMenu == Screen::Decks) {  // no pack open (see portalPacksChanged())
        openDecks();
        return;
    }
    gScreen = gBeforeMenu;
    // The question was hidden behind the settings, or even built there (see
    // portalPacksChanged()): restart its answer clock, as wakeScreen() does,
    // so the time away does not grade it Hard.
    if (gScreen == Screen::Quiz && !gRevealed) {
        gShownAt = millis();
        gNavTaps = 0;
        gQuizHints.input(gShownAt);
    }
    drawScreen();
}

// Opens the deck list with the loaded pack focused.
void openDecks() {
    int sel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) sel = static_cast<int>(i);
    gDeckList.setCount(static_cast<int>(gPacks.size()) + 1);  // + "Get decks"
    gDeckList.reset(sel);
    gPackInfoFor.clear();  // read the headers again (drawDecks())
    gScreen = Screen::Decks;
    drawScreen();
}

// Shows the picker set up by openChoice(): the saved value's row (or none,
// -1) is marked and focused.
void showChoice(int saved) {
    gChoiceSaved = saved;
    gChoiceList.setCount(gChoiceCount);
    gChoiceList.reset(saved >= 0 ? saved : 0);
    gScreen = Screen::Choice;
    drawScreen();
}

// Opens one of the Screen::Choice pickers (2/3/4 rows) with the saved value
// marked and focused when it is one of the candidates.
void openChoice(Choice which) {
    gChoice = which;
    int saved = -1;
    if (which == Choice::Orientation) {
        // Auto first, as in the language picker; only with an accelerometer.
        gChoiceCount = 0;
        if (tgc::board::hasAccel()) gChoiceValues[gChoiceCount++] = kOrientAuto;
        gChoiceValues[gChoiceCount++] = kOrientPortrait;
        gChoiceValues[gChoiceCount++] = kOrientLandscape;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gOrientPref) saved = i;
        showChoice(saved);
        return;
    }
    if (which == Choice::ForgetWifi) {
        // Cancel first and focused, no saved value: forgetting takes a tap and
        // a hold.
        gChoiceCount = 2;
        gChoiceValues[0] = 0;
        gChoiceValues[1] = 1;
        showChoice(-1);
        return;
    }
    if (which == Choice::UiLang) {
        gChoiceCount = 3;
        gChoiceValues[0] = tgc::kUiLangAuto;
        gChoiceValues[1] = tgc::kUiLangEnglish;
        gChoiceValues[2] = tgc::kUiLangSpanish;
        saved = gUiLangPref;  // rows are in preference order
        if (saved < 0 || saved >= gChoiceCount) saved = -1;
        showChoice(saved);
        return;
    }
    if (which == Choice::AutoAdvance || which == Choice::HoldTime) {
        // ui/InputPrefs.h: the row is the stored index.
        gChoiceCount = 3;
        for (int i = 0; i < gChoiceCount; ++i) gChoiceValues[i] = static_cast<uint16_t>(i);
        saved = which == Choice::AutoAdvance ? gAdvancePref : gHoldPref;
        if (saved >= gChoiceCount) saved = -1;
        showChoice(saved);
        return;
    }
    gChoiceCount = 4;
    if (which == Choice::Brightness) {
        gChoiceValues[0] = 32;
        gChoiceValues[1] = 64;
        gChoiceValues[2] = 128;
        gChoiceValues[3] = 255;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gBrightness) saved = i;
    } else if (which == Choice::NewPerDay) {
        gChoiceValues[0] = 3;
        gChoiceValues[1] = 5;
        gChoiceValues[2] = 10;
        gChoiceValues[3] = 20;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.newPerDay()) saved = i;
    } else {
        gChoiceValues[0] = 1500;
        gChoiceValues[1] = 3000;
        gChoiceValues[2] = 5000;
        gChoiceValues[3] = 0xFFFF;  // disables Hard
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.slowMs()) saved = i;
    }
    showChoice(saved);
}

// Applies the focused choice, persists it and returns to the settings page it
// came from (the Wi-Fi submenu for "Forget").
void applyChoice() {
    const int sel = gChoiceList.focus();
    if (gChoice == Choice::Orientation) {
        gOrientPref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar("orient", gOrientPref);
        gScreen = Screen::Menu;
        if (orientAuto()) {
            // The hold is still down: keep the picture; orientTick() takes over
            // from it after the release and the settle time.
            gOrient.reset(tgc::board::rotation());
            drawScreen();
        } else {
            applyRotation(fixedRotation());  // redraws the active screen
        }
        return;
    }
    if (gChoice == Choice::ForgetWifi) {
        if (gChoiceValues[sel] == 1) {
            tgc::HomeWifi::forgetCredentials();
            wifiMenuNote(ui().wifiForgotten, tgc::theme::kOkHi);
        }
        openWifiMenu(false);
        return;
    }
    if (gChoice == Choice::UiLang) {
        gUiLangPref = static_cast<int>(gChoiceValues[sel]);
        gPrefs.putInt("ui", gUiLangPref);
        setUiForLang(gPack->header().lang);  // re-render in the chosen language
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (gChoice == Choice::Brightness) {
        gBrightness = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar("bri", gBrightness);
        tgc::board::setBrightness(gBrightness);
        gDimmed = false;
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (gChoice == Choice::AutoAdvance) {
        gAdvancePref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar(tgc::kPrefAdvance, gAdvancePref);
        backToMenu();
        return;
    }
    if (gChoice == Choice::HoldTime) {
        // Read by loop() from the next press on; the hold that chose it has
        // fired already.
        gHoldPref = static_cast<uint8_t>(gChoiceValues[sel]);
        gPrefs.putUChar(tgc::kPrefHold, gHoldPref);
        backToMenu();
        return;
    }
    const uint16_t v = gChoiceValues[sel];
    if (gChoice == Choice::NewPerDay) {
        gSrs.setNewPerDay(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
        gQueue.rebuild(*gPack, gSrs, gRng);  // the daily new-card cap changed
    } else {
        gSrs.setSlowMs(v);
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gScreen = Screen::Menu;
    drawScreen();
}

void openStats() {
    gScreen = Screen::Stats;
    drawScreen();
}

// --- Settings > Study > Delete progress (docs/srs.md section 2.1) ----------
// A two-row list -- "This deck" (the loaded deck's kanji, shared ones too) and
// "Everything" (every record and the lifetime statistics) -- whose hold opens
// a confirmation: what goes, in numbers, over one row that only a hold
// confirms (the hold-progress fill shows on it); the side button cancels. Then
// back to the Study page with a note; the queue, the summary card and every
// count follow the store.
tgc::ListView gResetList;
tgc::ListView gResetConfirmList;  // the confirmation's one row
char gResetNote[32] = "";         // the list's note ("Nothing to delete")
bool gResetAll = false;           // the confirmation is for Everything
std::vector<uint32_t> gResetCps;  // This deck: its kanji, sorted and unique
tgc::DeckReset gResetPreview;     // This deck: its kanji with progress, shared ones
int gResetSharedDecks = 0;        // other installed decks that share some of them
String gResetSharedName;          // the one such deck's name, when only one
constexpr int kResetRows = 2;

void openReset(bool fresh) {
    if (fresh) {
        gResetList.setCount(kResetRows);
        gResetList.reset(0);
    }
    gResetNote[0] = 0;
    gScreen = Screen::Reset;
    drawScreen();
}

void drawReset() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    const bool wide = wideRows();
    gResetList.setCount(kResetRows);
    layoutList(gResetList, kHeaderH, H - kHintH - (gResetNote[0] ? kNoteH : 0),
               wide ? layout().row1H : layout().row2H);
    wg::drawList(d, gResetList, listX(), listW(), [&](int i, int x, int y, int w, int h, bool f) {
        wg::Row row;
        row.label = ui().resetRows[i];
        row.focused = f;
        const char* second = i == 1 ? ui().resetAllCaption
                                    : (gPack->isOpen() ? gPack->deckName() : ui().resetNoDeck);
        if (wide) {
            row.value = second;
            row.keepLabel = true;
        } else {
            row.caption = second;
        }
        if (i == 0 && !gPack->isOpen()) row.muted = true;
        wg::drawRow(d, gRender, x, y, w, h, row);
    });
    menuNote(d, gResetNote, tgc::theme::kWarn);
    menuHints(d, ui().hintChoose);
    presentList(gResetList);
}

// The other installed decks' share of the loaded deck's kanji with progress:
// how many of them (over all those decks) and, when a single deck shares any,
// its name.
void resetSharedPreview() {
    gResetSharedDecks = 0;
    gResetSharedName = "";
    std::vector<String> packs;
    listPacks(packs);
    std::vector<uint32_t> all;
    for (const String& p : packs) {
        if (p == gCurrentPack) continue;
        const std::vector<uint32_t> cps = packCodepoints(p);
        if (tgc::deckResetPreview(gSrs, gResetCps, cps).shared == 0) continue;
        if (++gResetSharedDecks == 1) {
            tgc::PackInfo info;
            tgc::FsPackSource src(LittleFS.open(p, "r"));
            const bool named = src.ok() && tgc::peekPackInfo(src, &info) == tgc::PackError::Ok &&
                               info.name[0];
            gResetSharedName = named ? String(info.name) : packFileStem(p);
        }
        all.insert(all.end(), cps.begin(), cps.end());
    }
    tgc::sortUnique(all);
    gResetPreview = tgc::deckResetPreview(gSrs, gResetCps, all);
}

// A hold on a row: the confirmation, or a note when there is nothing to delete.
void openResetConfirm() {
    gResetAll = gResetList.focus() == 1;
    gResetNote[0] = 0;
    if (gResetAll) {
        if (gSrs.count() == 0 && gSrs.answeredTotal() == 0 && gSrs.reviewMs() == 0 &&
            !gSrs.hasStudied())
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    } else if (!gPack->isOpen()) {
        snprintf(gResetNote, sizeof gResetNote, "%s", ui().summaryNoDeck);
    } else {
        gResetCps = packCodepoints(gCurrentPack);
        resetSharedPreview();
        if (gResetPreview.records == 0)
            snprintf(gResetNote, sizeof gResetNote, "%s", ui().resetNothing);
    }
    if (gResetNote[0]) {
        drawScreen();
        return;
    }
    gResetConfirmList.setCount(1);
    gResetConfirmList.reset(0);
    gScreen = Screen::ResetConfirm;
    drawScreen();
}

void drawResetConfirm() {
    using namespace tgc::theme;
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    menuHeader(d, ui().resetTitle);
    // What goes: the deck (or "Everything") in the label face, then in numbers.
    const char* what = gResetAll ? ui().resetRows[1] : gPack->deckName();
    gRender.drawLabel(d, what, kGutter, kHeaderH + 4, W - 2 * kGutter, 20, kWarn, kBlack,
                      tgc::Renderer::kTextUi, tgc::Align::Centre);
    const int rowH = layout().row1H;
    const int rowsTop = H - kHintH - rowH - 2 * kMarkerBand;
    const int textTop = kHeaderH + 28;
    char text[200];
    if (gResetAll)
        snprintf(text, sizeof text, ui().resetAllFmt, (unsigned)gSrs.count());
    else
        snprintf(text, sizeof text, ui().resetDeckFmt, (unsigned)gResetPreview.records);
    const int used = drawCaptionLines(d, text, kGutter, textTop, W - 2 * kGutter,
                                      std::max(0, (rowsTop - textTop) / 10), kWhite);
    // Kanji other decks share go too: said in warn colour, under a small gap.
    const int sharedTop = textTop + 10 * used + 6;
    if (!gResetAll && gResetPreview.shared > 0 && rowsTop - sharedTop >= 10) {
        if (gResetSharedDecks == 1)
            snprintf(text, sizeof text, ui().resetSharedOneFmt, gResetSharedName.c_str(),
                     (unsigned)gResetPreview.shared);
        else
            snprintf(text, sizeof text, ui().resetSharedManyFmt, (unsigned)gResetPreview.shared);
        drawCaptionLines(d, text, kGutter, sharedTop, W - 2 * kGutter, (rowsTop - sharedTop) / 10,
                         kWarn);
    }
    gResetConfirmList.setCount(1);
    layoutList(gResetConfirmList, rowsTop, H - kHintH, rowH);
    wg::drawList(d, gResetConfirmList, listX(), listW(),
                 [&](int, int x, int y, int w, int h, bool f) {
                     wg::Row row;
                     row.label = ui().resetConfirmRow;
                     row.focused = f;
                     wg::drawRow(d, gRender, x, y, w, h, row);
                 });
    wg::Hints hints;
    hints.hold = ui().hintDelete;
    hints.side = &tgc::kIconBack;
    hints.sideWord = ui().hintCancel;
    wg::drawHintBar(d, gRender, W, H, hints);
    present();
}

// The confirmation's hold: forget, save (temp + rename), and let the queue and
// the screen the settings return to follow, as after a deck change: the
// question kept for them may be a card whose progress just went.
void applyReset() {
    size_t n = 0;
    if (gResetAll) {
        n = gSrs.resetProgress();
        snprintf(gMenuNote, sizeof gMenuNote, "%s", ui().resetAllDone);
    } else {
        n = gSrs.forget(gResetCps.data(), gResetCps.size());
        snprintf(gMenuNote, sizeof gMenuNote, ui().resetDoneFmt, (unsigned)n);
    }
    saveProgress();
    Serial.printf("progress: %s: %u kanji forgotten, %u left, %u answers\n",
                  gResetAll ? "everything" : gCurrentPack.c_str(), (unsigned)n,
                  (unsigned)gSrs.count(), (unsigned)gSrs.answeredTotal());
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gPack->isOpen()) gBeforeMenu = newQuestion() ? Screen::Quiz : Screen::Done;
    gResetCps.clear();
    gResetCps.shrink_to_fit();
    gMenuPage = st::Page::Study;
    gScreen = Screen::Menu;
    drawScreen();
}

void resetLoop(int a, int b) {
    if (a == 1) {
        const bool note = gResetNote[0] != 0;
        gResetNote[0] = 0;
        moveListFocus(gResetList);
        if (note) dropPartialPush();  // the note goes: the whole frame
        drawScreen();
    } else if (a == 2) {
        openResetConfirm();
    }
    if (b && gScreen == Screen::Reset) backToMenu();  // the Study page
}

// Only a hold deletes; a tap does nothing, the side button cancels.
void resetConfirmLoop(int a, int b) {
    if (a == 2) {
        applyReset();
    } else if (b) {
        gScreen = Screen::Reset;
        drawScreen();
    }
}

// Every settings row: its label (UiStrings), its value and what a hold does.
// A page lists its rows in ui/SettingsTree.h; a new setting gets an st::Item,
// a place on a page there and an entry here.
const MenuItem& menuItem(st::Item id) {
    using I = st::Item;
    static const MenuItem kItems[] = {
        // --- home
        {I::Decks, [] { return ui().groupRows[0]; },
         [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)gPacks.size()); }, openDecks},
        {I::Study, [] { return ui().groupRows[1]; },
         [](char* o, size_t n) { snprintf(o, n, ui().studyValueFmt, (unsigned)gSrs.newPerDay()); },
         [] { openMenuPage(st::Page::Study); }},
        {I::Stats, [] { return ui().groupRows[2]; },
         [](char* o, size_t n) {
             const tgc::Stats s = gSrs.stats();
             const int acc = st::accuracyPercent(s.answers, s.again);
             if (acc >= 0) snprintf(o, n, "%d%%", acc);
         },
         openStats},
        {I::Wifi, [] { return ui().groupRows[3]; },
         // The radio is on only on the Wi-Fi screens, so the home sees it off.
         [](char* o, size_t n) {
             snprintf(o, n, "%s", gHome.active() ? gHome.ssid() : ui().wifiOff);
         },
         [] { openWifiMenu(true); }},
        {I::Device, [] { return ui().groupRows[4]; }, nullptr,
         [] { openMenuPage(st::Page::Device); }},
        // --- Study
        {I::NewPerDay, [] { return ui().studyRows[0]; },
         [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)gSrs.newPerDay()); },
         [] { openChoice(Choice::NewPerDay); }},
        {I::HardAfter, [] { return ui().studyRows[1]; },
         [](char* o, size_t n) { hardAfterText(gSrs.slowMs(), o, n); },
         [] { openChoice(Choice::SlowMs); }},
        {I::AutoAdvance, [] { return ui().studyRows[2]; },
         [](char* o, size_t n) {
             snprintf(o, n, "%s", ui().advanceRows[gAdvancePref % tgc::kAdvanceChoices]);
         },
         [] { openChoice(Choice::AutoAdvance); }},
        {I::DeleteProgress, [] { return ui().studyRows[3]; },
         [](char* o, size_t n) { snprintf(o, n, "%s", ui().deleteRowValue); },
         [] { openReset(true); }},
        // --- Device
        {I::Orientation, [] { return ui().deviceRows[0]; },
         [](char* o, size_t n) { snprintf(o, n, "%s", ui().orientRows[gOrientPref % 3]); },
         [] { openChoice(Choice::Orientation); }},
        {I::Brightness, [] { return ui().deviceRows[1]; },
         [](char* o, size_t n) { snprintf(o, n, "%u%%", (unsigned)(gBrightness * 100u / 255u)); },
         [] { openChoice(Choice::Brightness); }},
        {I::Language, [] { return ui().deviceRows[2]; },
         [](char* o, size_t n) {
             const int pref = gUiLangPref >= 0 && gUiLangPref < 3 ? gUiLangPref : 0;
             snprintf(o, n, "%s", ui().langRows[pref]);
         },
         [] { openChoice(Choice::UiLang); }},
        {I::HoldTime, [] { return ui().deviceRows[3]; },
         [](char* o, size_t n) {
             snprintf(o, n, "%s", ui().holdRows[gHoldPref % tgc::kHoldChoices]);
         },
         [] { openChoice(Choice::HoldTime); }},
        {I::Date, [] { return ui().deviceRows[4]; },
         [](char* o, size_t n) { shortDate(gSrs.today(), o, n); }, [] { openDateMenu(true); }},
        {I::Sleep, [] { return ui().deviceRows[5]; }, nullptr, [] { sleepNow(false); }},
        {I::About, [] { return ui().deviceRows[6]; }, nullptr, openAbout},
    };
    static_assert(sizeof kItems / sizeof kItems[0] == st::kItemCount, "one entry per item");
    for (const MenuItem& m : kItems)
        if (m.id == id) return m;
    return kItems[0];
}

// A hold on the focused settings row.
void activateSetting() {
    menuItem(st::itemAt(gMenuPage, menuList().focus())).action();
}

// loop() on the settings tree: tap moves, hold activates, the side button goes
// from a page back to the home and from the home out of the settings.
void menuLoop(int a, int b) {
    const bool note = gMenuNote[0] != 0;
    if (a || b) gMenuNote[0] = 0;  // a page's note lasts until the next press
    if (a == 1) {
        moveListFocus(menuList());
        if (note) dropPartialPush();  // the note goes: the whole frame
        drawScreen();
    } else if (a == 2) {
        activateSetting();
    }
    if (b && gScreen == Screen::Menu) {
        if (gMenuPage != st::Page::Home) {
            gMenuPage = st::Page::Home;  // the home's focus is still on the group
            drawScreen();
        } else {
            closeMenu();
        }
    }
}

// Loads the focused deck (replacing the live pack only after it validates),
// resets the "study more" override and starts the quiz.
void selectPack() {
    bool loaded = false;
    gExtraNew = 0;
    const int sel = gDeckList.focus();
    if (sel >= 0 && sel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[sel];
        if (loadPack(path)) {
            gPrefs.putString("pack", path);
            loaded = true;
        }
    }
    if (loaded && !newQuestion()) {
        showDone();
        return;
    }
    gScreen = Screen::Quiz;
    drawScreen();
}

// --- Get decks (roadmap phase 4) -------------------------------------------

// Radio off and the CPU back to its normal clock. Safe in any state.
void netRadioOff() {
    const bool wasOn = gHome.state() != tgc::HomeWifi::State::Off;
    gHome.end();
    if (wasOn) setCpuFrequencyMhz(kCpuMhz);
}

void netError(const char* title, const char* detail) {
    netRadioOff();
    gNetStep = NetStep::Error;
    snprintf(gNetErrTitle, sizeof gNetErrTitle, "%s", title);
    snprintf(gNetErrDetail, sizeof gNetErrDetail, "%s", detail ? detail : "");
    QLOG("N error: %s | %s\n", title, gNetErrDetail);
}

void netNote(const char* text, uint16_t color) {
    snprintf(gNetNote, sizeof gNetNote, "%s", text);
    gNetNoteColor = color;
}

String catalogPath(const tgc::CatalogEntry& e) { return String("/packs/") + e.name.c_str() + ".tgc"; }

// Compares the catalog with /packs (existence and size, catalogDeckState()).
void netRefreshStates() {
    listPacks(gPacks);
    gCatalogState.assign(gCatalog.size(), tgc::DeckState::New);
    for (size_t i = 0; i < gCatalog.size(); ++i) {
        const String path = catalogPath(gCatalog[i]);
        const long size = tgc::fsFileSize(path);
        if (size < 0) continue;
        gCatalogState[i] =
            tgc::catalogDeckState(true, static_cast<uint32_t>(size), gCatalog[i].bytes);
    }
}

// Opens the Get decks screen and starts joining the home Wi-Fi. `fromBoot`: from
// the boot-time upload screen (whose portal the caller already stopped).
void openGetDecks(bool fromBoot) {
    gNetFromBoot = fromBoot;
    gScreen = Screen::GetDecks;
    gCatalog.clear();
    gCatalogState.clear();
    gNetSel = 0;
    gNetBusy = false;
    gNetNote[0] = 0;
    gNetActivityAt = millis();
    if (!tgc::HomeWifi::hasCredentials()) {
        netError(ui().netNoWifi, ui().netNoWifiHint);
        drawScreen();
        return;
    }
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netError(ui().netNoConnect, gHome.ssid());
        drawScreen();
        return;
    }
    gNetStep = NetStep::Connecting;
    QLOG("N get decks: joining %s\n", gHome.ssid());
    drawScreen();
}

// Leaves the screen with the radio off: back to the deck list, or -- when it was
// opened from the boot-time upload screen -- into the quiz if a deck is now
// loaded, else back to that screen.
void closeGetDecks() {
    netRadioOff();
    gGetDecksLangHint.clear();
    gCatalog.clear();
    gCatalogState.clear();
    QLOG("N get decks: closed\n");
    if (gNetFromBoot) {
        gNetFromBoot = false;
        if (gPack->isOpen()) {
            gWifiBootUpload = false;
            startQuizFromPortal();
        } else {
            openPortal(defaultPortalMode());
        }
        return;
    }
    listPacks(gPacks);
    openDecks();
}

// Connected: the network is up anyway, so take the real date from NTP first
// (blocking, at most kNtpTimeoutMs; netLoadCatalog() right after resets the
// idle timers and drops the presses made meanwhile). Only with a time zone
// (see netTimeSync()); a failure is logged and Get decks carries on.
void netSyncClock() { netTimeSync(); }

// Why a catalog read failed, as a title and a detail line (the error screen's
// two parts; the network test shows the title). False when it did not fail.
bool catalogFailure(const tgc::CatalogFetch& c, const String& url, char* title, size_t tn,
                    char* detail, size_t dn) {
    snprintf(detail, dn, "%s", url.c_str());
    switch (c.fetch.status) {
        case tgc::FetchStatus::Ok:
            if (c.parse.error == tgc::CatalogError::Ok) return false;
            snprintf(title, tn, "%s", ui().netBadCatalog);
            if (c.parse.error == tgc::CatalogError::UnsupportedVersion)
                snprintf(detail, dn, "%s",
                         c.parse.version < tgc::kCatalogVersion ? ui().netOldCatalog
                                                                : ui().netNewerCatalog);
            return true;
        case tgc::FetchStatus::Connect:
            snprintf(title, tn, "%s", ui().netNoServer);
            return true;
        case tgc::FetchStatus::Tls:
            snprintf(title, tn, "%s", ui().netTls);
            snprintf(detail, dn, "mbedTLS -0x%04X  %s", (unsigned)(-c.fetch.tlsError), url.c_str());
            return true;
        case tgc::FetchStatus::Http:
            // 404 on the default URL: no release with a catalog exists yet.
            snprintf(title, tn, ui().netHttpFmt, c.fetch.httpCode);
            if (c.fetch.httpCode == 404) snprintf(detail, dn, "%s", ui().netNotPublished);
            return true;
        case tgc::FetchStatus::TooLarge:
            snprintf(title, tn, "%s", ui().netBadCatalog);
            return true;
        default:
            snprintf(title, tn, "%s", ui().netFailed);
            return true;
    }
}

// Connected: read and parse the catalog (blocking), then show the list.
void netLoadCatalog() {
    const String url = tgc::HomeWifi::catalogUrl();
    tgc::CatalogFetch c = tgc::fetchCatalog(url);
    gNetActivityAt = millis();
    gSwallowInput = true;  // presses made while blocked must not act now
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        netError(title, detail);
        return;
    }
    gCatalog = std::move(c.parse.entries);
    // The decks in the screen's language first, the catalog order otherwise
    // (every other language is still listed after them).
    const char* firstLang =
        (gUiLangPref == tgc::kUiLangAuto && !gPack->isOpen() && !gGetDecksLangHint.empty())
            ? gGetDecksLangHint.c_str()
            : uiLangCode();
    tgc::orderCatalogForLang(gCatalog, firstLang);
    Serial.printf("[net] catalog: \"%s\" decks first\n", firstLang);
    netRefreshStates();
    // Start on the first deck that is not here yet.
    gNetSel = 0;
    for (size_t i = 0; i < gCatalogState.size(); ++i)
        if (gCatalogState[i] != tgc::DeckState::OnDevice) {
            gNetSel = static_cast<int>(i);
            break;
        }
    gNetStep = NetStep::List;
}

// Critical-battery guard (power/BatteryGuard.h): one reading every
// kBatteryGuardMs, from loop() and during a download. True when the device must
// save and sleep now.
bool batteryCritical(uint32_t now) {
    if (now - gBatteryGuardAt < kBatteryGuardMs) return false;
    gBatteryGuardAt = now;
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    return gBatteryGuard.update(tgc::board::batteryPercent(), external);
}

// Download progress: redraw on a new percentage, keep the idle timers away and
// let the side button cancel (its release is swallowed afterwards). The power
// button and a critical battery cancel too and ask downloadCatalogDeck() to
// sleep: a download may run for minutes at 240 MHz, and neither can wait that
// long. A download the portal page asked for also lets the portal answer the
// page's progress view meanwhile.
bool netProgress(size_t done, size_t total, void*) {
    const uint32_t now = millis();
    gNetActivityAt = now;
    if (gScreen == Screen::Wifi) gPortal.serviceDownload(done, total);
    if (now - gNetInputAt >= 50) {
        gNetInputAt = now;
        tgc::board::update();
        if (tgc::board::isHeld(Button::Next)) return false;
        if (tgc::board::powerButtonClicked()) {
            gNetSleep = NetSleep::User;
            return false;
        }
    }
    if (batteryCritical(now)) {
        gNetSleep = NetSleep::Critical;
        return false;
    }
    const int pct = total ? static_cast<int>(static_cast<uint64_t>(done) * 100u / total) : 0;
    if (pct != gNetPct) {
        gNetPct = pct;
        drawScreen();
    }
    return true;
}

const char* netDownloadError(const tgc::FetchResult& r, char* buf, size_t n) {
    switch (r.status) {
        case tgc::FetchStatus::Connect: return ui().netNoServer;
        case tgc::FetchStatus::Tls: return ui().netTls;
        case tgc::FetchStatus::Http:
            snprintf(buf, n, ui().netHttpFmt, r.httpCode);
            return buf;
        case tgc::FetchStatus::NoSpace:
        case tgc::FetchStatus::TooLarge: return ui().netNoSpace;
        case tgc::FetchStatus::SizeMismatch:
        case tgc::FetchStatus::HashMismatch: return ui().netCorrupt;
        case tgc::FetchStatus::InvalidPack: return ui().netInvalid;
        case tgc::FetchStatus::Cancelled: return ui().netCancelled;
        default: return ui().netFailed;
    }
}

// Downloads catalog deck `e` into /packs (atomic; size and SHA-256 checked),
// then rescans /packs through the portal's path. Shared by the Get decks screen
// and the portal page in home mode; the progress shows on whichever of the two
// is open (netProgress() redraws it), and the side button cancels.
tgc::DeckDownloadResult downloadCatalogDeck(const tgc::CatalogEntry& e) {
    tgc::DeckDownloadResult out;
    const String path = catalogPath(e);
    // On battery below the level a critical sleep resumes at, the download
    // would likely be cut by the guard: refuse it up front.
    const bool external = tgc::board::externalPower() || tgc::board::isCharging();
    if (!tgc::BatteryGuard::canResume(tgc::board::batteryPercent(), external)) {
        out.batteryLow = true;
        return out;
    }
    // Replacing the loaded deck: close it first so its file is not open while
    // the new one is renamed over it; portalPacksChanged() reloads it below.
    const bool replacingLive = gPack->isOpen() && path == gCurrentPack;
    if (replacingLive) {
        gRender.end();
        gPack->close();
        gSource.reset();
    }
    snprintf(gNetDeck, sizeof gNetDeck, "%s", tgc::catalogDisplayName(e).c_str());
    gNetBusy = true;
    gNetPct = -1;
    gNetInputAt = millis();
    gNetSleep = NetSleep::None;
    drawScreen();

    tgc::PackFetchRequest req;
    req.url = e.url.c_str();
    req.fileName = String(e.name.c_str()) + ".tgc";
    req.allowHttps = true;
    req.maxBytes = tgc::WifiPortal::kMaxPackBytes;
    req.expectBytes = e.bytes;
    req.expectSha256 = e.sha256;
    req.idleTimeoutMs = kNetIdleMs;
    req.totalTimeoutMs = kNetDownloadMs;
    req.http10 = true;
    req.progress = netProgress;
    const uint32_t started = millis();
    const tgc::FetchResult r = tgc::fetchPackFile(req);
    gNetBusy = false;
    gNetActivityAt = millis();
    gSwallowInput = true;
    Serial.printf("[net] get %s: %s (%u bytes in %u ms, http %d, tls %d)\n", req.fileName.c_str(),
                  tgc::fetchStatusString(r.status), (unsigned)r.bytes,
                  (unsigned)(millis() - started), r.httpCode, r.tlsError);
    // Cancelled for a sleep: the temp is gone and the old deck untouched, so
    // sleep now (sleepNow() saves); the wake reopens the deck closed above.
    if (gNetSleep != NetSleep::None) sleepNow(gNetSleep == NetSleep::Critical);

    // Rescan /packs the portal's way; that also reopens the deck closed above, or
    // loads the first one when none was open (the boot-time path), and gives
    // the settings home's "Continue" a question from the deck now loaded.
    if (r.packsChanged || !gPack->isOpen()) portalPacksChanged();
    out.fetch = r;
    return out;
}

// Get decks: downloads catalog deck `i` and marks the row.
void netDownload(int i) {
    const tgc::CatalogEntry e = gCatalog[i];
    const bool updating = gCatalogState[i] == tgc::DeckState::Update;
    gNetNote[0] = 0;
    const tgc::DeckDownloadResult d = downloadCatalogDeck(e);
    if (d.batteryLow) {
        netNote(ui().batteryTitle, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    const tgc::FetchResult& r = d.fetch;
    netRefreshStates();
    char buf[48];
    if (r.status == tgc::FetchStatus::Ok) {
        snprintf(buf, sizeof buf, ui().netSavedFmt, tgc::catalogDisplayName(e).c_str());
        netNote(buf, tgc::theme::kOkHi);
    } else if (r.status == tgc::FetchStatus::NoSpace && updating) {
        // The old copy stays until the new one validated, so an update needs
        // room for both: say how to make it.
        netError(ui().netNoSpace, ui().netNoSpaceUpdate);
    } else {
        netNote(netDownloadError(r, buf, sizeof buf), tgc::theme::kWarn);
    }
    drawScreen();
}

// Loads a deck that is already on the device and starts studying it.
void netStudy(int i) {
    const String path = catalogPath(gCatalog[i]);
    if (!loadPack(path)) {
        netNote(ui().netInvalid, tgc::theme::kWarn);
        drawScreen();
        return;
    }
    gPrefs.putString("pack", path);
    listPacks(gPacks);
    netRadioOff();
    gCatalog.clear();
    gCatalogState.clear();
    gNetFromBoot = false;
    gWifiBootUpload = false;
    startQuizFromPortal();
}

// Shows why joining the home Wi-Fi failed (the radio is already off).
void netJoinFailed() { netError(joinFailureText(), gHome.ssid()); }

// loop() while on the Get decks screen.
void getDecksLoop(int a, int b) {
    if (b) {  // side button: back (or cancel the connection attempt)
        closeGetDecks();
        return;
    }
    if (gNetStep == NetStep::Connecting) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            gNetStep = NetStep::Catalog;
            drawScreen();
            netSyncClock();
            netLoadCatalog();
            drawScreen();
        } else if (st == tgc::HomeWifi::State::Failed) {
            netJoinFailed();
            drawScreen();
        }
        return;
    }
    if (gNetStep == NetStep::Error) {
        if (a == 2) openGetDecks(gNetFromBoot);  // retry from the start
        return;
    }
    if (gNetStep != NetStep::List || gCatalog.empty()) return;
    if (a == 1) {
        gNetSel = (gNetSel + 1) % static_cast<int>(gCatalog.size());
        gNetNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        if (gCatalogState[gNetSel] == tgc::DeckState::OnDevice)
            netStudy(gNetSel);
        else
            netDownload(gNetSel);
    }
}

// --- Settings > Wi-Fi --------------------------------------------------------

void wifiMenuNote(const char* text, uint16_t color) {
    snprintf(gWifiMenuNote, sizeof gWifiMenuNote, "%s", text ? text : "");
    gWifiMenuNoteColor = color;
}

// Opens the Wi-Fi submenu: `fresh` from the settings home (first row, no note),
// else back from one of its screens (selection and note kept).
void openWifiMenu(bool fresh) {
    if (fresh) {
        gWifiMenuList.setCount(kWifiMenuRows);
        gWifiMenuList.reset(0);
        gWifiMenuNote[0] = 0;
    }
    gWifiMenuSsid = tgc::HomeWifi::savedSsid();
    gScreen = Screen::WifiMenu;
    drawScreen();
}

void activateWifiMenu() {
    switch (gWifiMenuList.focus()) {
        case 0:  // Web portal: home mode once a network is saved, else setup mode
            openPortal(defaultPortalMode());
            break;
        case 1:  // Home network: the connection test
            openNetTest();
            break;
        case 2:  // Setup mode: the access point, e.g. to save another network
            openPortal(PortalMode::Setup);
            break;
        case 3:  // Forget network: confirmed on a picker (Cancel first)
            if (gWifiMenuSsid.isEmpty()) {
                wifiMenuNote(ui().wifiNoNetwork, tgc::theme::kWarn);
                drawScreen();
            } else {
                openChoice(Choice::ForgetWifi);
            }
            break;
    }
}

void wifiMenuLoop(int a, int b) {
    if (a == 1) {
        gWifiMenuList.next();
        gWifiMenuNote[0] = 0;
        drawScreen();
    } else if (a == 2) {
        activateWifiMenu();
    }
    if (b && gScreen == Screen::WifiMenu) {  // side button returns to the settings home
        gScreen = Screen::Menu;
        drawScreen();
    }
}

// Settings > Wi-Fi > Home network: joins the saved network (radio on, CPU at
// kCpuWifiMhz); netTestLoop() takes it from there.
void openNetTest() {
    gScreen = Screen::NetTest;
    gTestIp = "";
    gTestRssi = 0;
    gTestTime = -1;
    gTestCatalog[0] = 0;
    gTestFail[0] = 0;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
    if (!tgc::HomeWifi::hasCredentials()) {
        gTestStep = NetTestStep::NoNetwork;
        drawScreen();
        return;
    }
    netRadioOff();
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        setCpuFrequencyMhz(kCpuMhz);
        gTestStep = NetTestStep::Failed;
        snprintf(gTestFail, sizeof gTestFail, "%s", ui().netNoConnect);
        drawScreen();
        return;
    }
    gTestStep = NetTestStep::Joining;
    QLOG("N test: joining %s\n", gHome.ssid());
    drawScreen();
}

// Connected: the network time (as every join does, when the offset is saved)
// and the catalog, the end-to-end check that the catalog host is reachable and
// answers with a catalog. Blocking; the radio is off again afterwards.
void netTestCheck() {
    gTestIp = gHome.localIp();
    gTestRssi = gHome.rssi();
    gTestStep = NetTestStep::Checking;
    drawScreen();
    if (gTzKnown) {
        int64_t utcMs = 0;
        gTestTime = gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs) ? 1 : 0;
        if (gTestTime) applyWallClock(utcMs, gTz);
    }
    const String url = tgc::HomeWifi::catalogUrl();
    const tgc::CatalogFetch c = tgc::fetchCatalog(url);
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        // A 404 says more as "no catalog published at this URL yet".
        const bool notPublished =
            c.fetch.status == tgc::FetchStatus::Http && c.fetch.httpCode == 404;
        snprintf(gTestCatalog, sizeof gTestCatalog, "%s", notPublished ? ui().netNotPublished : title);
        gTestCatalogColor = tgc::theme::kWarn;
    } else {
        snprintf(gTestCatalog, sizeof gTestCatalog, ui().testCatalogFmt,
                 (unsigned)c.parse.entries.size());
        gTestCatalogColor = tgc::theme::kOkHi;
    }
    Serial.printf("[net] test: \"%s\" ip %s rssi %d dBm, catalog %s, time %d\n", gHome.ssid(),
                  gTestIp.c_str(), gTestRssi, gTestCatalog, gTestTime);
    netRadioOff();
    gSwallowInput = true;  // presses made while blocked must not act now
    gNetActivityAt = millis();
    gTestStep = NetTestStep::Done;
}

void netTestLoop(int a, int b) {
    if (b) {  // back to the Wi-Fi submenu (cancels a join in progress)
        netRadioOff();
        openWifiMenu(false);
        return;
    }
    if (gTestStep == NetTestStep::Joining) {
        const tgc::HomeWifi::State st = gHome.poll();
        if (st == tgc::HomeWifi::State::Connected) {
            netTestCheck();
            drawScreen();
        } else if (st == tgc::HomeWifi::State::Failed) {
            setCpuFrequencyMhz(kCpuMhz);  // the radio is off already
            snprintf(gTestFail, sizeof gTestFail, "%s", joinFailureText());
            gTestStep = NetTestStep::Failed;
            gNetActivityAt = millis();
            drawScreen();
        }
        return;
    }
    if (a == 2) openNetTest();  // test again
}

// Tap/hold detection: 1 = tap (on release), 2 = hold (at the threshold).
struct Key {
    bool down = false;
    bool longFired = false;
    uint32_t downAt = 0;
    int poll(bool isDown, uint32_t now, uint32_t holdMs) {
        if (isDown && !down) {
            down = true;
            longFired = false;
            downAt = now;
        }
        if (isDown && !longFired && now - downAt >= holdMs) {
            longFired = true;
            return 2;
        }
        if (!isDown && down) {
            down = false;
            if (!longFired) return 1;
        }
        return 0;
    }
};

#if defined(TGC_REMOTE)
// Serial remote for tools/screenshots (env sticks3-shots, never a release).
// One byte a command over USB serial, read by loop():
//   'a' / 'b'  tap the front / side button (down for 80 ms)
//   'A' / 'B'  hold it (down until the hold fires, then up)
//   's'        print the back buffer, which is what the panel shows: "#shot W H
//              <on|dim|dark>" (the backlight; dark: the last frame drawn), one
//              "#r Y <hex>" line a row (RGB565 as stored: big-endian, four hex
//              digits a pixel), "#end"
//   'w'        wake: counts as activity and lights a dim or dark panel, no press
// Test hooks:
//   'd'        the boot-time date question as after a power cut: "Getting the
//              date via <ssid>..." first, then the stepper (with the reason when
//              the network failed). It writes nothing itself: a hold on the
//              stepper confirms (saved), a side tap cancels, and a network time
//              that answers is applied as by every sync (a same-day one writes
//              nothing)
//   'f'        the next home Wi-Fi join is pinned to an access point that does
//              not exist: the pinned step fails, any access point after 25 s
//   'x'        the next join is pinned there for good: it fails after 40 s ("No
//              network: set it by hand" on the date question)
// A press is a virtual button that goes through Key like a real one: it counts
// as activity, a dim panel brightens and acts, a dark one only wakes. While a
// virtual button is down the next bytes wait, so a burst runs in order.
struct RemoteKey {
    uint32_t upAt = 0;     // millis() when a tap comes up; 0: up
    bool holding = false;  // a hold that has not fired yet
};
RemoteKey gRemote[2];
void wakeScreen();

bool remoteBusy() {
    return gRemote[0].upAt || gRemote[0].holding || gRemote[1].upAt || gRemote[1].holding;
}

bool remoteDown(int k, uint32_t now) {
    RemoteKey& r = gRemote[k];
    if (r.holding) return true;
    if (!r.upAt) return false;
    if (static_cast<int32_t>(now - r.upAt) < 0) return true;
    r.upAt = 0;  // seen up once by Key: the press ends here
    return false;
}

// After Key::poll(): a remote hold lets go shortly after its hold fired, so a
// slow loop() turn can never make it a tap.
void remoteKeyEvent(int k, int event, uint32_t now) {
    if (gRemote[k].holding && event == 2) {
        gRemote[k].holding = false;
        gRemote[k].upAt = now + 50;
    }
}

void remoteShot() {
    static const char kHex[] = "0123456789abcdef";
    static char line[16 + 4 * 320 + 2];  // up to 320 pixels a row (T-Display-S3)
    const int w = gCanvas->width(), h = gCanvas->height();
    const uint8_t* px = static_cast<const uint8_t*>(gCanvas->getBuffer());
    Serial.printf("\n#shot %d %d %s\n", w, h, gScreenOff ? "dark" : gDimmed ? "dim" : "on");
    for (int y = 0; y < h && w <= 320 && px; ++y) {
        int n = snprintf(line, sizeof line, "#r %d ", y);
        const uint8_t* row = px + static_cast<size_t>(y) * w * 2;
        for (int i = 0; i < w * 2; ++i) {
            line[n++] = kHex[row[i] >> 4];
            line[n++] = kHex[row[i] & 15];
        }
        line[n++] = '\n';
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
    Serial.printf("#end\n");
}

// 'd': see above. Not while a radio is in use (the portal, Get decks, the
// network test) or the question is already open.
void remoteDateQuestion() {
    if (gScreen == Screen::Date || gHome.active() || gPortal.active() || !gPack->isOpen()) {
        Serial.printf("remote: no date question now (screen %d, radio %d)\n",
                      static_cast<int>(gScreen), gHome.active() || gPortal.active() ? 1 : 0);
        return;
    }
    Serial.printf("remote: the date question, as after a power cut\n");
    gAdvance.cancel();
    dateSyncBegin();
    openDate(Screen::Quiz);
}

void remotePoll(uint32_t now) {
    while (!remoteBusy() && Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'a' || c == 'b') {
            gRemote[c == 'b'].upAt = now + 80;
        } else if (c == 'A' || c == 'B') {
            gRemote[c == 'B'].holding = true;
        } else if (c == 's') {
            remoteShot();
        } else if (c == 'w') {
            gLastInputAt = now;
            if (gScreenOff || gDimmed) wakeScreen();
        } else if (c == 'd') {
            remoteDateQuestion();
        } else if (c == 'f' || c == 'x') {
            tgc::HomeWifi::setTestFault(c == 'f' ? tgc::HomeWifi::TestFault::BadPin
                                                 : tgc::HomeWifi::TestFault::NoJoin);
            Serial.printf("remote: the next join %s\n",
                          c == 'f' ? "fails its pinned step" : "fails");
        }
    }
}
#endif

// Adds the time spent in deep sleep (RTC timer) to the SRS day clock. Only a
// wake-up from our own deepSleep() qualifies: after a power cut both the RTC
// timer and RTC memory are reset, and a crash reset is not a wake-up.
void applySleepTime() {
    const bool slept = tgc::board::wokeFromSleep() && gSleepMagic == kSleepMagic;
    gSleepMagic = 0;
    if (!slept) return;
    const uint64_t now = tgc::board::rtcMicros();
    if (now <= gSleptAtUs) return;
    const uint64_t ms = (now - gSleptAtUs) / 1000u;
    gSrs.addElapsedMs(ms);
    Serial.printf("power: woke after %lu s, day %u\n", (unsigned long)(ms / 1000u),
                  (unsigned)gSrs.today());
}

// Leaves what the next wake needs in RTC memory: when the chip went to sleep
// (for applySleepTime) and whether the boot-time date question is still open.
// Called by prepareDeepSleep() right after the save, so the saved day carry and
// this timestamp describe the same moment and the wake adds exactly the time
// since (a notice shown before the sleep included).
void armSleep() {
    gSleptAtUs = tgc::board::rtcMicros();
    gSleepMagic = kSleepMagic;
    gDatePending = gScreen == Screen::Date && gAfterDate == Screen::Quiz;
}

// Persists the SRS, which also persists the uptime day carry, and stops the
// portal and the home Wi-Fi. A store that failed to load and has no answers is
// not written, so a damaged file (and its .bak) stays on disk.
void saveAndRadiosOff() {
    if (gDirty || gSrsLoaded) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    if (gPortal.active()) {
        gPortal.end();
        setCpuFrequencyMhz(kCpuMhz);
        QLOG("W wifi: stopped\n");
    }
    if (gHome.state() != tgc::HomeWifi::State::Off) QLOG("N home wifi: stopped\n");
    netRadioOff();  // safe in any state
}

// What every deep sleep of the running app needs first: the state saved, the
// radios off and the RTC timestamp the wake adds to the day clock. Used by the
// idle sleep on battery, the sleep after unplugging a dark panel and the
// explicit sleep. Not by the critical-battery re-sleep at boot, which keeps the
// first sleep's timestamp, nor by halt(), where the SRS may not be loaded.
void prepareDeepSleep() {
    saveAndRadiosOff();
    armSleep();
}

// Idle timeout reached: on battery, deep-sleep. On USB power only the panel
// goes dark (the serial port stays up), after the same save and radio stop;
// powerTick() sleeps once it is unplugged. Returns only in the USB case.
void goIdle() {
    if (!tgc::board::externalPower()) {
        prepareDeepSleep();
        QLOG("P deep sleep\n");
        tgc::board::deepSleep();
    }
    saveAndRadiosOff();
    tgc::board::setDisplayOn(false);
    gScreenOff = true;
    gDimmed = false;
    gLastPowerCheck = millis();
    QLOG("P screen off (usb)\n");
}

// First input after the panel went dark or dim: restore it. A Wi-Fi screen whose
// portal the idle timeout stopped gets its portal back.
void wakeScreen() {
    if (gScreenOff) {
        tgc::board::setDisplayOn(true);
        gScreenOff = false;
        // The question was not visible while dark: restart its answer clock so
        // the time away does not grade it Hard.
        if (gScreen == Screen::Quiz && !gRevealed) {
            gShownAt = millis();
            gNavTaps = 0;
        }
        if (gScreen == Screen::Wifi && gWifiStep != WifiStep::Failed && !gPortal.active()) {
            openPortal(gPortalMode);  // the idle timeout stopped the portal: start it again
        } else if (gScreen == Screen::NetTest &&
                   (gTestStep == NetTestStep::Joining || gTestStep == NetTestStep::Checking)) {
            openNetTest();  // the idle timeout cut the test short: run it again
        } else if (gScreen == Screen::GetDecks && gNetStep != NetStep::Error && !gHome.active()) {
            openGetDecks(gNetFromBoot);  // the idle timeout turned the radio off: join again
        } else {
            drawScreen();
        }
    }
    tgc::board::setBrightness(gBrightness);  // also undoes the dim level
    gDimmed = false;
}

// Full-screen notice (sleep, battery, first start): `title` in the label face
// (two lines at most), `hint` under it in captions. The UI fonts need no pack.
void showNotice(const char* title, const char* hint, uint16_t titleColor) {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int mid = H / 2;
    gRender.drawCentred(d, title, 4, mid - 44, W - 8, 40, 2, titleColor, kBlack,
                        tgc::Renderer::kTextUi);
    drawCaptionLines(d, hint, 4, mid + 2, W - 8, 4, tgc::theme::kTextDim);
    present();
}

// Explicit sleep: the power button, the "Sleep" settings row, or a critical
// battery. Saves like goIdle(), but deep-sleeps even on USB power -- the user
// asked for it (or the cell is about to die) -- after a short notice that says
// how to wake up. The RTC timestamp keeps the day clock running through it.
void sleepNow(bool critical) {
    // Saved before the notice: the first click of a double click (power off)
    // already gets the state onto the flash.
    prepareDeepSleep();
    QLOG("P deep sleep (%s)\n", critical ? "battery" : "user");
    if (gScreenOff) tgc::board::setDisplayOn(true);
    tgc::board::setBrightness(gBrightness);
    if (critical)
        showNotice(ui().batteryTitle, ui().batteryHint, tgc::theme::kWarn);
    else
        showNotice(ui().sleepTitle, ui().sleepHint, kWhite);
    delay(critical ? kBatteryNoticeMs : kSleepNoticeMs);
    gSleptCritical = critical;
    gSleepUiEs = gUi == &kUiEs;
    tgc::board::deepSleep();
}

// Called once per loop() with the current time: dims, then idles.
void powerTick(uint32_t now) {
    if (gScreenOff) {
        // Unplugged while dark: nothing keeps it awake any more. The day carry
        // kept running in RAM since goIdle() saved (a night on the charger is
        // hours of it, maybe the 04:00 rollover), so save again first.
        if (now - gLastPowerCheck >= kPowerCheckMs) {
            gLastPowerCheck = now;
            if (!tgc::board::externalPower()) {
                prepareDeepSleep();
                QLOG("P deep sleep (unplugged)\n");
                tgc::board::deepSleep();
            }
        }
        return;
    }
    uint32_t idle = now - gLastInputAt;
    if (gPortal.active()) {
        const uint32_t http = now - gPortal.lastActivityMs();
        if (http < idle) idle = http;
    }
    // A download, catalog read or join counts as activity, and so does the end
    // of the date screen's network-time attempt (a new status line to read).
    // A join holds them off for as long as it takes: up to 40 s
    // (HomeWifi::kConnectTimeoutMs), longer than the dim.
    if (gHome.active() || gDateSync != DateSync::None) {
        if (gHome.state() == tgc::HomeWifi::State::Connecting) gNetActivityAt = now;
        const uint32_t net = now - gNetActivityAt;
        if (net < idle) idle = net;
    }
    if (!gDimmed && idle >= kDimAfterMs) {
        uint8_t dim = gBrightness / 4;
        if (dim < 8) dim = 8;
        tgc::board::setBrightness(dim);
        gDimmed = true;
    }
    if (idle >= (gPortal.active() ? kWifiIdleMs : kSleepAfterMs)) goIdle();
}

// Fatal error screen. Nothing to save here (the SRS may not even be loaded), so
// on battery it just sleeps after the usual timeout; a wake shows it again.
void halt() {
    const uint32_t start = millis();
    for (;;) {
        tgc::board::update();
        if (millis() - start >= kSleepAfterMs && !tgc::board::externalPower())
            tgc::board::deepSleep();
        if (tgc::board::powerButtonClicked()) tgc::board::deepSleep();  // no click reset now
        delay(50);
    }
}

// True when the LittleFS partition holds a LittleFS (the "littlefs" magic of the
// superblock, at byte 8 of either block of the root metadata pair). An erased
// partition (a browser flasher's "erase device", a fresh merged image) or one
// left by another firmware does not. Unknown layout: true, so it is never
// formatted blindly.
bool partitionHasLittleFs() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!p) return true;
    char magic[8];
    for (uint32_t block = 0; block < 2; ++block)
        if (esp_partition_read(p, block * 4096u + 8u, magic, sizeof magic) == ESP_OK &&
            std::memcmp(magic, "littlefs", sizeof magic) == 0)
            return true;
    return false;
}

// Mounts LittleFS. A partition with no LittleFS on it is a fresh device: it is
// formatted, and the empty /packs then leads into the Wi-Fi upload screen. A
// LittleFS that will not mount is damage instead: false, and the caller asks.
bool mountFilesystem() {
    if (LittleFS.begin(false)) return true;
    if (partitionHasLittleFs()) return false;
    Serial.println("fs: no LittleFS on the data partition, formatting (first start)");
    showNotice(ui().fsPreparing, ui().fsPreparingHint, kWhite);
    return LittleFS.format() && LittleFS.begin(false);
}

// A damaged filesystem: nothing on it can be read, but it is only erased on a
// long press of the front button (decks and progress are lost), then the device
// restarts into the fresh-device path. The press that woke the chip must be
// released first, so it cannot become that long press.
[[noreturn]] void askFormat() {
    wg::Hints hints;
    hints.hold = ui().hintErase;
    showError(ui().errNoFilesystem, ui().errNoFilesystemHint, &hints);
    bool released = !tgc::board::isHeld(tgc::board::Button::Select);
    const uint32_t start = millis();
    for (;;) {
        tgc::board::update();
        if (!tgc::board::isHeld(tgc::board::Button::Select)) released = true;
        if (released && tgc::board::wasHeld(tgc::board::Button::Select)) {
            Serial.println("fs: erased on request");
            showNotice(ui().fsPreparing, ui().fsPreparingHint, kWhite);
            LittleFS.format();
            ESP.restart();
        }
        if (millis() - start >= kSleepAfterMs && !tgc::board::externalPower())
            tgc::board::deepSleep();
        if (tgc::board::powerButtonClicked()) tgc::board::deepSleep();
        delay(20);
    }
}

// The boot screen of a device with no deck: the Wi-Fi portal, so a deck can
// come from a phone. With a home Wi-Fi saved, the page over it (setup mode if
// the join fails, see wifiLoop()); without one, the device's own access point.
// An access point that will not start leaves nothing to do: the old error
// screen. From setup(), or once the first-boot language is chosen.
void openBootUploadScreen() {
    gWifiBootUpload = true;
    openPortal(defaultPortalMode());
    if (gPortalMode == PortalMode::Setup && gWifiStep != WifiStep::Serving) {
        showError(ui().errNoPack, ui().errNoPackHint);
        halt();
    }
    Serial.printf("wifi: no packs, upload-ready (%s %s)\n",
                  gPortalMode == PortalMode::Home ? "joining" : "ssid",
                  gPortalMode == PortalMode::Home ? gHome.ssid() : gPortal.ssid());
}

// First-boot language picker (ui/UiLang.h decides when): English preselected.
// Driven by loop() like every screen, so presses count for the dim and sleep
// timers, and a sleep while it is open leaves the preference unset: the wake
// asks again.
void openFirstLang() {
    gFirstLangList.setCount(tgc::kFirstBootLangRows);
    gFirstLangList.reset(tgc::kFirstBootLangDefaultRow);
    gScreen = Screen::FirstLang;
    drawScreen();
}

// Stores the choice as an explicit preference (never Auto: a later deck in
// another language must not switch the screens), applies it and continues the
// boot the picker interrupted, now in that language.
void applyFirstLang() {
    gUiLangPref = tgc::firstBootLangPref(gFirstLangList.focus());
    gPrefs.putInt("ui", gUiLangPref);
    setUiForLang(nullptr);
    Serial.printf("ui: first boot, language %s\n", uiLangCode());
    openBootUploadScreen();
}

// loop() on the picker: a tap moves the focus, a hold chooses. The side
// button does nothing, as on the boot-time upload screen that follows.
void firstLangLoop(int a) {
    if (a == 1) {
        gFirstLangList.next();
        drawScreen();
    } else if (a == 2) {
        applyFirstLang();
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.printf("%s %s\n", tgc::credits::kAppNameLatin, tgc::firmwareVersion());
    tgc::board::begin();
    setCpuFrequencyMhz(kCpuMhz);
    // The press that woke the chip must not also act on the first screen.
    gSwallowInput = tgc::board::wokeFromSleep();

    gPoweredOff = !(gPowerMagic == kPowerMagic && gPowerMagicInv == ~kPowerMagic);
    gPowerMagic = kPowerMagic;
    gPowerMagicInv = ~kPowerMagic;
    auto& disp = tgc::board::display();
    kGrey = disp.color565(60, 60, 70);
    kHighlight = disp.color565(30, 60, 150);
    kGreen = disp.color565(0, 130, 40);
    kRed = disp.color565(170, 20, 20);
    kYellow = disp.color565(255, 210, 60);

    // Apply the saved orientation right after board::begin() (which set portrait)
    // and before the canvas exists, so the buffer matches the panel from boot.
    // The namespace keeps the project's former name on purpose, so settings
    // survive the rename to Tamagotcho (docs/decisions.md, 2026-10-21).
    gPrefs.begin("tangocho", false);
    // Absent (never chosen) is not Auto: it may ask for the language below.
    const bool uiPrefStored = gPrefs.isKey("ui");
    gUiLangPref = gPrefs.getInt("ui", tgc::kUiLangAuto);
    gBrightness = gPrefs.getUChar("bri", gBrightness);
    gAdvancePref = gPrefs.getUChar(tgc::kPrefAdvance, tgc::kAdvanceDefault);
    gHoldPref = gPrefs.getUChar(tgc::kPrefHold, tgc::kHoldDefault);
    tgc::board::setBrightness(gBrightness);
    setUiForLang(nullptr);  // localise the pre-pack error screens to the choice too
    gOrientPref = loadOrientPref();
    tgc::board::setRotation(bootRotation());
    W = tgc::board::width();
    H = tgc::board::height();
    createCanvas();
    gRng = tgc::XorShiftRng(tgc::board::randomSeed());

    // Woken after a critical-battery sleep: unless the cell recovered (with
    // hysteresis) or USB is in, say so and sleep again before anything runs.
    // The RTC timestamp of the first sleep is left alone, so the next real
    // wake adds the whole span to the day clock.
    if (tgc::board::wokeFromSleep() && gSleptCritical) {
        int pct = -1;  // the best of a few readings: the gauge is noisy
        for (int i = 0; i < 3; ++i) {
            pct = std::max(pct, tgc::board::batteryPercent());
            delay(20);
        }
        const bool external = tgc::board::externalPower() || tgc::board::isCharging();
        Serial.printf("power: critical wake, battery %d%%%s\n", pct, external ? " (usb)" : "");
        if (!tgc::BatteryGuard::canResume(pct, external)) {
            gUi = gSleepUiEs ? &kUiEs : &kUiEn;
            showNotice(ui().batteryTitle, ui().batteryHint, tgc::theme::kWarn);
            delay(kBatteryNoticeMs);
            tgc::board::deepSleep();
        }
    }
    gSleptCritical = false;

    if (!mountFilesystem()) askFormat();
    // A power cut in the middle of a download or an upload leaves its temp
    // behind (up to 5 MB), which would count against every later free-space
    // check. Nothing can be in flight at boot.
    for (const char* tmp : {tgc::kFetchTmp, tgc::WifiPortal::kUploadTmp})
        if (tgc::fsRemoveIfExists(tmp)) Serial.printf("fs: removed stale %s\n", tmp);
    listPacks(gPacks);
    // The time zone first: the slept time (applySleepTime() below) is counted
    // in its study days, 23 or 25 h long across a daylight-saving change.
    loadTimeZone();

    // No packs yet: do not halt. Start the Wi-Fi portal so a pack can be
    // uploaded from a phone (or downloaded from a URL); loop() moves on to the
    // quiz as soon as one arrives. This is the practical answer to a fresh (or
    // launcher-formatted) filesystem, including one mountFilesystem() has just
    // formatted. An AP that will not start leaves nothing to do, so that keeps
    // the old error screen. A brand-new device (no language ever chosen) first
    // asks for the language, so that screen and the page are in it; with no
    // deck there is no date question to come before it.
    if (gPacks.empty()) {
        gSrsLoaded = gSrs.begin(gSrsIo);
        gBootDay = gSrs.today();
        gSrs.beginSession();
        applySleepTime();
        if (tgc::firstBootLangPicker(uiPrefStored, !gPacks.empty())) {
            Serial.println("ui: first boot, asking for the language");
            openFirstLang();
            return;
        }
        openBootUploadScreen();
        return;
    }

    String want = gPrefs.getString("pack", "");
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) want = gPacks[0];
    // The saved deck, else the first other one that opens. None opening at all
    // most likely means decks of a newer pack format than this firmware reads.
    bool opened = loadPack(want);
    for (size_t i = 0; !opened && i < gPacks.size(); ++i)
        if (gPacks[i] != want) opened = loadPack(gPacks[i]);
    if (!opened) {
        Serial.printf("pack: none of %u opens\n", (unsigned)gPacks.size());
        showError(ui().errOpenPack, ui().errOpenPackHint);
        halt();
    }
    gPrefs.putString("pack", gCurrentPack);

    const bool srsLoaded = gSrs.begin(gSrsIo);
    gSrsLoaded = srsLoaded;
    gBootDay = gSrs.today();  // the persisted day, before any slept time is added
    gSrs.beginSession();
    applySleepTime();
    gExtraNew = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);  // loadPack rebuilt with an empty store
    const tgc::DeckSummary counts = deckCounts();

    char today[16];
    formatDate(gSrs.today(), today, sizeof today);
    Serial.printf("srs: day %u (%s), records %u, due %u, new %u%s, powered_off=%d\n",
                  (unsigned)gSrs.today(), today, (unsigned)gSrs.count(), (unsigned)counts.due,
                  (unsigned)counts.fresh, srsLoaded ? "" : " (new)", gPoweredOff ? 1 : 0);

    // A wake from a sleep taken while the question was open asks it again.
    const bool datePending = tgc::board::wokeFromSleep() && gDatePending;
    gDatePending = false;
    if ((gPoweredOff || datePending) && gSrs.count() > 0 && !gWallClockSynced) {
        // Power cut: the elapsed days are unknown. Ask *before* deciding "done",
        // because advancing the day can make reviews due. A real time applied
        // earlier in this boot (applyWallClock) already answered it. Meanwhile
        // the home Wi-Fi, if saved, may answer it with the network time.
        dateSyncBegin();
        openDate(Screen::Quiz);
        return;
    }
    if (!newQuestion()) {
        showDone();
    } else {
        drawScreen();
    }
}

// The quiz's input (docs/ux-refresh.md §6.7). On a question: front tap = the
// next option (down), side tap = the previous one (up), both wrapping; front
// hold = answer; side hold = settings. A tap acts on release and a hold fires
// at holdMsFor(gHoldPref), so a long press is never also a tap. Revealed: any
// press but the side hold goes on (after a wrong answer to the detail card
// first); after a correct one the auto-advance countdown may do it instead.

// The input after the answer has been revealed: a correct answer advances as
// before; a wrong one opens the detail card first, and the next input advances.
void afterReveal() {
    gAdvance.cancel();
    if (!gLastCorrect) {
        showDetail();
        return;
    }
    if (newQuestion()) {
        gScreen = Screen::Quiz;
        drawScreen();
    } else {
        showDone();
    }
}

void holdAction(uint32_t pressedAt) {
    if (gRevealed) {
        afterReveal();
        return;
    }
    // No card, no grade: a record must never be written for a card that is not
    // the one on screen (see questionCard()).
    tgc::Card card;
    if (!questionCard(&card)) return;
    gRevealed = true;

    const bool correct = gSel == gQ.correct;
    gLastCorrect = correct;
    // From the question being drawn to the hold's press-down (not its fire),
    // minus a fixed allowance per navigation tap. Thinking time before and
    // between taps counts, the mechanical cost of reaching the option does not.
    const uint32_t raw = pressedAt - gShownAt;
    const uint32_t nav = static_cast<uint32_t>(gNavTaps) * kNavAllowanceMs;
    const uint32_t rt_ms = raw > nav ? raw - nav : 0;
    tgc::Grade grade;
    if (!correct)
        grade = tgc::Grade::Again;
    else if (rt_ms >= gSrs.slowMs())
        grade = tgc::Grade::Hard;
    else
        grade = tgc::Grade::Good;

    const float u01 = gRng.below(1000) / 1000.0f;
    gSrs.grade(card.codepoint, grade, gSrs.today(), static_cast<uint8_t>(gQ.mode), u01);
    gSrs.addReviewMs(rt_ms);
    gQueue.answered(gQ.cardIndex, grade);
    ++gDirty;
    gLastAnswerAt = millis();

    const tgc::Record* r = gSrs.find(card.codepoint);
    const int interval = r ? static_cast<int>(r->dueDay) - static_cast<int>(gSrs.today()) : 0;
    gLastGrade = grade;
    gLastInterval = interval;
    QLOG("A sel=%d correct=%d %s grade=%s rt=%ums S=%.2f D=%.1f I=%d\n", gSel, gQ.correct,
         correct ? "OK" : "MISS", gradeName(grade), (unsigned)rt_ms,
         r ? r->stabilityCds / 100.0f : 0.0f, r ? r->difficulty10 / 10.0f : 0.0f, interval);
    // A correct answer moves on by itself after the auto-advance time; a
    // wrong one never does (the detail card follows a press).
    if (correct)
        gAdvance.start(gLastAnswerAt, tgc::advanceMsFor(gAdvancePref));
    else
        gAdvance.cancel();
    gAdvanceFrameAt = gLastAnswerAt;
    drawScreen();
}

// loop() on the quiz screen (see above). `down`: a button is down now.
void quizLoop(int a, int b, uint32_t pressedAt, uint32_t now, bool down) {
    if (!gRevealed) {
        if (down || a || b) gQuizHints.input(now);
        if (a == 1)
            moveQuizFocus(true);
        else if (a == 2)
            holdAction(pressedAt);
        else if (b == 1)
            moveQuizFocus(false);
        else if (b == 2)
            openMenu();
        else if (!gScreenOff && gQuizHints.tick(now, gSrs.answeredTotal(), true))
            drawScreen();  // the idle hint bar appears: the tiles make room
        return;
    }
    if (a || b == 1) {
        afterReveal();
    } else if (b == 2) {
        openMenu();
    } else if (gAdvance.active()) {
        if (gAdvance.due(now, down)) {
            afterReveal();
        } else if (!gScreenOff && now - gAdvanceFrameAt >= tgc::kHoldFrameMs) {
            gAdvanceFrameAt = now;
            drawAdvanceBar(*gCanvas, now);
            presentRegion(0, H - kAdvanceBarH, W, kAdvanceBarH);
        }
    }
}

// --- Hold progress (docs/ux-refresh.md §2.5) --------------------------------
// kHoldShowMs after the front button went down on the quiz's focused tile or
// on the focused row of a list screen, the target fills with accent-press from
// the left; it is full when the hold fires (holdMsFor(gHoldPref)), and
// releasing earlier is a tap and the fill goes. Every kHoldFrameMs only the
// target is redrawn and pushed: the quiz draws its tile directly; a list
// screen is redrawn by its own draw function under a clip rect on the row,
// with the widgets' press fill set, so every drawList() screen gets it with no
// code of its own. The first frame also lights the hint bar's hold item (the
// quiz pushes the bar, a list pushes that one frame whole). Grading still uses
// the press time.
struct HoldTarget {
    bool quiz;
    wg::Rect r;
};

bool holdTarget(HoldTarget* out) {
    if (gScreen == Screen::Quiz) {
        tgc::Card c;
        if (gRevealed || !questionCard(&c) || gSel < 0 || gSel >= gQ.count) return false;
        out->quiz = true;
        out->r = wg::Rect{static_cast<int16_t>(gQuizG.tx[gSel]), static_cast<int16_t>(gQuizG.ty[gSel]),
                          static_cast<int16_t>(gQuizG.tw), static_cast<int16_t>(gQuizG.th)};
        return true;
    }
    if (!gFrameFocusValid) return false;
    out->quiz = false;
    out->r = gFrameFocus;
    return true;
}

void drawHoldFrame(const HoldTarget& tg, float fill, bool first) {
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    auto& d = *gCanvas;
    if (tg.quiz) {
        gHoldShare = fill;
        drawQuizTile(d, gSel, fill);
        presentRegion(tg.r.x, tg.r.y, tg.r.w, tg.r.h);
        if (first && gQuizG.hints) {
            drawQuizHints(d, true);
            presentRegion(0, H - kHintH, W, kHintH);
        }
    } else {
        wg::setPressFill(fill);
        gQuietFrame = true;
        if (first) {
            drawScreen();
        } else {
            gPushOnly = true;
            gPushRect = tg.r;
            d.setClipRect(tg.r.x, tg.r.y, tg.r.w, tg.r.h);
            drawScreen();
            d.clearClipRect();
            gPushOnly = false;  // in case the screen pushed nothing
        }
        gQuietFrame = false;
    }
    QLOG("H hold %s %d%%%s: %u us, push %u us (%d rows)\n", tg.quiz ? "tile" : "row",
         static_cast<int>(fill * 100.0f + 0.5f), first ? " (first)" : "",
         (unsigned)(micros() - t0), (unsigned)gPushUs, gPushRows);
}

// Called by loop() with the front button's key and event before the input is
// handled. True when a fill was on screen and its press just ended (a tap, a
// hold, or a press that was dropped): loop() then makes sure a frame without
// the fill follows.
bool holdTick(uint32_t now, bool down, bool longFired, uint32_t downAt, int event,
              uint32_t holdMs) {
    const bool pressing = down && !longFired && event == 0 && !gSwallowInput && !gScreenOff;
    const float fill = pressing ? tgc::holdFill(now - downAt, holdMs) : -1.0f;
    HoldTarget tg{};
    if (fill < 0.0f || !holdTarget(&tg)) {
        if (!gHoldOn) return false;
        gHoldOn = false;
        wg::setPressFill(-1.0f);
        return true;
    }
    if (gHoldOn && now - gHoldFrameAt < tgc::kHoldFrameMs) return false;
    const bool first = !gHoldOn;
    gHoldOn = true;
    gHoldFrameAt = now;
    drawHoldFrame(tg, fill, first);
    return false;
}

void loop() {
    tgc::board::update();
    const uint32_t t = millis();
    gSrs.tick(t);
    static Key keyA, keyB;
#if defined(TGC_REMOTE)
    remotePoll(t);
    const bool downA = tgc::board::isHeld(Button::Select) || remoteDown(0, t);
    const bool downB = tgc::board::isHeld(Button::Next) || remoteDown(1, t);
#else
    const bool downA = tgc::board::isHeld(Button::Select);
    const bool downB = tgc::board::isHeld(Button::Next);
#endif
    const uint32_t holdMs = tgc::holdMsFor(gHoldPref);
    int a = keyA.poll(downA, t, holdMs);
    int b = keyB.poll(downB, t, holdMs);
#if defined(TGC_REMOTE)
    remoteKeyEvent(0, a, t);
    remoteKeyEvent(1, b, t);
#endif
    if (downA || downB || a || b) {
        gLastInputAt = t;
        // A dark panel only wakes up: that press (through its release) is
        // dropped. A dimmed panel brightens and the press acts as usual.
        if (gScreenOff) gSwallowInput = true;
        if (gScreenOff || gDimmed) wakeScreen();
    }
    if (gSwallowInput) {
        a = b = 0;
        if (!downA && !downB) gSwallowInput = false;
    }
    // The power button (StickS3 M5PM1, click reset disabled) means "sleep now";
    // on a dark panel (idle on USB) it only wakes the panel, like the others.
    if (tgc::board::powerButtonClicked()) {
        gLastInputAt = t;
        if (!gScreenOff) sleepNow(false);
        wakeScreen();
    }
    // Hold progress first, so the fill never outlives its press; then the
    // input. A fill that ended needs a frame without it: the handler's own
    // (plus the hint bar, whose hold item was lit, after a partial push), or
    // a redraw when the press did nothing.
    const bool fillEnded = holdTick(t, keyA.down, keyA.longFired, keyA.downAt, a, holdMs);
    const uint32_t framesBefore = gFrames;
    if (gScreen == Screen::Menu) {
        menuLoop(a, b);
    } else if (gScreen == Screen::Decks) {
        if (a == 1) {
            gDeckList.setCount(static_cast<int>(gPacks.size()) + 1);  // + "Get decks"
            moveListFocus(gDeckList);
            drawScreen();
        } else if (a == 2) {
            if (gDeckList.focus() == static_cast<int>(gPacks.size()))
                openGetDecks(false);  // the "+ Get decks" row
            else
                selectPack();
        }
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Choice) {
        if (a == 1) {
            moveListFocus(gChoiceList);
            drawScreen();
        } else if (a == 2) {
            applyChoice();
        }
        if (b && gScreen == Screen::Choice) {  // cancel: back to where the picker came from
            if (gChoice == Choice::ForgetWifi) {
                openWifiMenu(false);
            } else {
                gScreen = Screen::Menu;
                drawScreen();
            }
        }
    } else if (gScreen == Screen::Date) {
        // Network first (boot-time question): "Getting the date via ..." until
        // the network time answers (on the quiz then) or fails (the stepper with
        // the reason); any press gives up on it, radio off, and shows the
        // stepper. Presses are handled before the poll, which may block for the
        // NTP request.
        // The stepper: tap one more day, side tap one day less (as the quiz's
        // side tap is the previous option; below the current day only with an
        // anchor, see SrsStore::manualStepsMin()), hold confirm, side hold
        // cancel.
        if (gDateSync == DateSync::Joining) {
            if (a || b) {
                dateSyncCancel();
                drawScreen();
            } else {
                dateSyncPoll();
            }
        } else if (a == 1) {
            if (gDateSteps < gSrs.manualStepsMax()) ++gDateSteps;
            drawScreen();
        } else if (a == 2) {
            applyDate();
        } else if (b == 1) {
            if (gDateSteps > gSrs.manualStepsMin()) {
                --gDateSteps;
                drawScreen();
            }
        } else if (b == 2) {  // cancel: back to the Date submenu, or straight into the quiz
            if (gAfterDate != Screen::Quiz)
                openDateMenu(false);
            else
                leaveDate();
        }
    } else if (gScreen == Screen::DateMenu) {
        dateMenuLoop(a, b);
    } else if (gScreen == Screen::TzList) {
        tzListLoop(a, b);
    } else if (gScreen == Screen::Stats) {
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::About) {
        if (a == 1) {  // next page (drawAbout() wraps after the last)
            ++gAboutPage;
            drawScreen();
        }
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Detail) {
        // Animate the stroke order while idle: one stroke every ~550 ms, a
        // ~1.2 s hold on the last one, then loop; it stops while the panel is
        // dim or dark (§2.6). Only the counter and the kanji are redrawn.
        if (gAnimOn && !gDimmed && !gScreenOff && static_cast<int32_t>(t - gAnimNext) >= 0) {
            const int count = static_cast<int>(gAnimStrokes);
            gAnimFrame = (gAnimFrame + 1) % count;
            gAnimNext = t + (gAnimFrame == count - 1 ? kStrokePauseMs : kStrokeMs);
            drawDetailFrame();
        }
        // Any input (tap, hold or side tap) starts the next question; a side hold
        // opens the menu, as on the quiz.
        if (a == 1 || a == 2 || b == 1) {
            if (newQuestion()) {
                gScreen = Screen::Quiz;
                drawScreen();
            } else {
                showDone();
            }
        } else if (b == 2) {
            openMenu();
        }
    } else if (gScreen == Screen::Done) {
        if (b == 2) {
            openMenu();
        } else if ((a || b == 1) && doneCanStudyMore()) {
            // Nothing due and the daily new cap is spent: study 10 more today.
            gExtraNew = tgc::studyMoreExtra(gSrs.newPerDay(), deckCounts().introducedToday,
                                            extraNewToday());
            gExtraNewDay = gSrs.today();
            gQueue.rebuild(*gPack, gSrs, gRng, gExtraNew);
            if (newQuestion()) {
                gScreen = Screen::Quiz;
                drawScreen();
            } else {
                drawDone();
            }
        }
    } else if (gScreen == Screen::Wifi) {
        // The page may leave this screen (Get decks), switch to home mode or
        // download; the boot-time upload screen moves on to the quiz as soon as
        // a pack arrived in setup mode (see wifiLoop()).
        wifiLoop(a, b);
    } else if (gScreen == Screen::GetDecks) {
        getDecksLoop(a, b);
    } else if (gScreen == Screen::WifiMenu) {
        wifiMenuLoop(a, b);
    } else if (gScreen == Screen::NetTest) {
        netTestLoop(a, b);
    } else if (gScreen == Screen::FirstLang) {
        firstLangLoop(a);
    } else if (gScreen == Screen::Reset) {
        resetLoop(a, b);
    } else if (gScreen == Screen::ResetConfirm) {
        resetConfirmLoop(a, b);
    } else {
        quizLoop(a, b, keyA.downAt, t, downA || downB);
    }
    if (fillEnded) {
        if (gFrames == framesBefore)
            drawScreen();
        else if (gLastPushPartial)
            presentRegion(0, H - kHintH, W, kHintH);
    }
    if (gDirty >= 8 || (gDirty > 0 && t - gLastAnswerAt > 120000)) {
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    if (gScreen == Screen::Quiz && !gScreenOff && t - gBatteryCheckAt >= kBatteryCheckMs) {
        gBatteryCheckAt = t;
        if (tgc::board::batteryPercent() != gShownBattery) drawScreen();
    }
    orientTick(t, downA || downB);
    // Save and sleep before a nearly flat cell browns out and takes the RTC
    // timer (the SRS day clock) with it.
    if (batteryCritical(t)) sleepNow(true);
    powerTick(t);
    delay(10);
}
