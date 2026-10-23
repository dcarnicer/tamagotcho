#pragma once
// The on-device UI strings: the table every screen reads through ui(), the
// English and Spanish tables (UiStrings.cpp) and the UI-language preference.
#include "board/Board.h"

namespace tgc {
namespace app {

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
    // new per day, introduce, hard after, auto-advance, on'yomi, delete (progress)
    const char* studyRows[6];
    const char* deviceRows[7];  // orientation, brightness, language, hold time, date, sleep, about
    const char* studyTitle;     // headers (Font0, ASCII)
    const char* deviceTitle;
    const char* advanceTitle;
    const char* holdTitle;
    const char* onKanaTitle;
    const char* studyValueFmt;  // the Study row's value, %u = new per day
    const char* wifiOff;        // the Wi-Fi row's value while the radio is off
    const char* decimalPoint;   // "1.5 s" / "1,5 s"
    const char* advanceRows[3];  // ui/InputPrefs.h order: off, short, normal
    const char* holdRows[3];     // short, normal, long
    const char* onKanaRows[2];   // tgc::OnKana order: katakana, hiragana
    const char* months[12];      // short names: the header's "18 Oct", the date tile's "OCT 2026"
    const char* choiceDefault;   // a picker row's value on the default choice
    // Pickers: a description under the header (Font0, wrapped to the width: at
    // most three lines of 21 characters in the 135 px portrait; in landscape
    // only when every row still fits, i.e. on the three-row pickers).
    const char* descNewPerDay;
    const char* descHardAfter;
    const char* descAdvance;
    const char* descHold;
    const char* descOnKana;
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
    // row is studyRows[4] ("Delete") over deleteRowValue ("progress"):
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
    // A board with a keyboard (the Cardputer ADV; keysOr()): the texts that
    // name the front button elsewhere name keys, or its G0 button, there.
    const char* sleepHintKeys;
    const char* batteryHintKeys;
    const char* dateExplainKeys;           // caption, wrapped (21 characters a line)
    const char* errNoFilesystemHintKeys;
    // --- Deck update checks (docs/deck-updates.md) ---------------------------
    const char* updatesRow;       // the deck list's row
    const char* updatesCheck;     // its value: never checked
    const char* updatesUpToDate;  // its value: checked, nothing to update
    const char* updatesCountOne;  // the home Decks row's value: one update
    const char* updatesCountFmt;  // the home Decks row's value: %u = updates
    const char* updateTitle;      // boot check and updates list header (ASCII)
    const char* updateChecking;   // label face while joining
    const char* updateFoundOne;   // one installed deck with an update
    const char* updateFoundFmt;   // %u = installed decks with an update
    const char* updateAllCurrent; // checked, nothing to update
    const char* updateWhere;      // result: where the updates list is (caption)
    const char* updateListFmt;    // updates list: %s = network, %u = updates
    const char* updateSkip;       // hint bar while checking
    const char* updateContinue;   // hint bar on the result
    // --- Learning mode (docs/learning-mode.md §3.2, §3.8) --------------------
    // The intro card's header pill (Font0, uppercase ASCII: "NUEVA" leaves
    // the worded stroke counter 67 px in the 135 px portrait) and its hint
    // bar's side word ("✓ known"; "← known" on the keys). Checked in
    // test_quizui, which copies these.
    const char* introPill;
    const char* hintKnown;
    // Settings > Study > Introduce: the picker's header (Font0, ASCII), its
    // two rows (on, off; also the Study row's value) and its description.
    const char* introTitle;
    const char* introRows[2];
    const char* descIntro;
};

extern const UiStrings kUiEn;
extern const UiStrings kUiEs;

extern const UiStrings* gUi;  // English until a pack says otherwise
inline const UiStrings& ui() { return *gUi; }
// `keys` on a board with a keyboard, else `buttons` (the UiStrings *Keys texts).
inline const char* keysOr(const char* buttons, const char* keys) {
    return tgc::board::hasKeyboard() ? keys : buttons;
}

// NVS `tangocho`/`ui`: the UI-language preference (ui/UiLang.h).
extern int gUiLangPref;

void setUiForLang(const char* lang);
const char* uiLangCode();

}  // namespace app
}  // namespace tgc
