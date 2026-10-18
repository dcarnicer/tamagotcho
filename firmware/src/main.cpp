// Tangocho Phase 2: open the saved pack in /packs and run a spaced-repetition
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
#include "platform/FsSrsFile.h"
#include "platform/HomeWifi.h"
#include "platform/NetFetch.h"
#include "platform/WifiPortal.h"
#include "portal/PortalLogic.h"
#include "power/BatteryGuard.h"
#include "quiz/Quiz.h"
#include "render/Renderer.h"
#include "srs/Queue.h"
#include "srs/SrsStore.h"
#include "srs/WallClock.h"
#include "ui/UiLang.h"

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
    const char* appName;  // error screen title bar
    const char* settingsTitle;
    const char* decksTitle;
    const char* backHint;
    // continue, decks, date, new/day, speed, stats, wifi (a submenu), orientation,
    // language, brightness, sleep
    const char* menuRows[11];
    const char* newPerDayTitle;
    const char* speedTitle;
    const char* speedOff;
    const char* speedSecondsFmt;
    const char* quizMeaning;
    const char* quizReading;
    const char* quizKanji;
    const char* dateTitle;
    // Date screen: the last-used day and a proposed "today" the user steps.
    const char* dateLastUsed;  // label of the last-used date
    const char* dateToday;     // label of the proposed date
    const char* dateOneDay;    // "+1 day"
    const char* dateDaysFmt;   // "+%u days" (0 and 2 and up)
    const char* dateHintTap;   // what a tap / hold of the front button does
    const char* dateHintSide;  // what a side-button hold does
    const char* weekdays[7];   // short names, Sunday first
    // Date screen status line while (or after) the boot-time question tries the
    // network time over the home Wi-Fi. 1x font, two lines in portrait.
    const char* dateSyncingFmt;  // %s = home Wi-Fi name
    const char* dateNoNetwork;   // could not join
    const char* dateNoTime;      // joined, but no (plausible) time came back
    const char* doneBadge;
    // Font0 is 6 px/char and each done-screen line is centred on the 135 px
    // portrait width, so every done line must stay short (about 21 chars).
    const char* doneAllReviewed;
    const char* doneDayFmt;
    const char* doneDueFmt;
    const char* doneStudyMore;
    const char* doneMenuHint;
    const char* deckCountsFmt;
    const char* statsTitle;
    const char* statsSeenFmt;
    const char* statsDueNewFmt;
    const char* statsAnswersFmt;
    const char* statsAccuracyFmt;
    const char* statsLearnedFmt;
    const char* errNoFilesystem;      // LittleFS present but will not mount
    const char* errNoFilesystemHint;  // hold the front button to erase it
    // First boot on an erased (or foreign) data partition: formatting it takes
    // a few seconds. 2x title (words of at most 10 characters) + 1x hint.
    const char* fsPreparing;
    const char* fsPreparingHint;
    const char* errNoPack;
    const char* errNoPackHint;
    const char* errOpenPack;
    const char* orientTitle;
    const char* orientRows[3];  // indexed by OrientPref: portrait, landscape, auto
    // Detail card (shown after a wrong answer). `detailOn`/`detailKun` label the
    // on'yomi/kun'yomi lines; the meanings label reuses quizMeaning.
    const char* detailTitle;
    const char* detailOn;
    const char* detailKun;
    const char* detailStrokesFmt;
    const char* detailAccuracyFmt;
    // Statistics: day streak and total time on questions.
    const char* statsStreakFmt;  // current, best
    const char* statsTimeFmt;    // whole line, %s = the formatted duration
    const char* statsTimeHM;     // "%uh %um"
    const char* statsTimeMS;     // "%um %us"
    const char* statsTimeS;      // "%us"
    // UI-language picker: the title and the three rows (endonyms, same in every
    // table so a user can find their language whatever the current one is).
    const char* langTitle;
    const char* langRows[3];
    // Wi-Fi portal screen (the hints and status lines are 1x, see drawWifi()).
    const char* wifiTitle;
    const char* brightTitle;
    // Full-screen notices before a deep sleep: a short title (2x font, wrapped
    // at spaces, so no word longer than 10 characters) and a hint (1x font).
    const char* sleepTitle;
    const char* sleepHint;
    const char* batteryTitle;
    const char* batteryHint;
    // "Get decks" (last row of the deck list): download decks from the release
    // catalog over the home Wi-Fi. Drawn with the 1x built-in font in the list
    // footer and on the error screen, so keep each line short.
    const char* getDecksRow;
    const char* getDecksTitle;
    const char* netConnecting;
    const char* netCatalog;
    const char* netDownloadingFmt;  // %u = percent
    const char* netHoldDownload;
    const char* netHoldUpdate;
    const char* netHoldStudy;
    const char* netHoldRetry;
    const char* netSavedFmt;  // %s = deck name
    const char* netCardsFmt;  // %s = language, %u = cards, %s = size
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
    const char* netCorrupt;
    const char* netNoSpace;
    const char* netNoSpaceUpdate;  // detail: an update needs room for the old and the new copy
    const char* netInvalid;
    const char* netFailed;
    const char* netCancelled;
    const char* wifiGetDecksHint;  // boot-time upload screen with a saved home Wi-Fi
    // Settings > Wi-Fi: the submenu (rows drawn by drawRow(), so accents are
    // fine; the titles are header text and stay ASCII).
    const char* wifiMenuRows[4];  // web portal, home network ("%s" = name), setup mode, forget
    const char* wifiHomeNone;     // the home-network row's value when none is saved
    const char* wifiNoNetwork;    // note: no home network saved
    const char* wifiForgotten;    // note: the network was forgotten
    const char* forgetTitle;      // the confirmation picker's header
    const char* forgetRows[2];    // cancel, forget
    // The portal screen.
    const char* wifiScanJoin;      // over the join QR (setup mode)
    const char* wifiScanOpen;      // over the page QR
    const char* wifiTapOtherQr;    // setup mode: a tap swaps the two QR codes
    const char* wifiViaFmt;        // home mode, %s = the home network
    const char* wifiConnectingTo;  // joining the home network for the portal
    const char* wifiHoldSetup;     // joining / failed: a hold opens setup mode
    const char* wifiTapRetry;      // failed: a tap tries again
    const char* wifiLinkLost;      // home mode: the link dropped, the driver reconnects
    const char* wifiStudyHint;     // boot-time screen in home mode once a deck is loaded
    const char* wifiDownloadFmt;   // %s = deck, %u = percent (a download from the page)
    const char* wifiCancelHint;    // during that download
    const char* wifiFreeFmt;       // %s = free space
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
    const char* testHoldAgain;
    const char* testNoNetworkHint;
};

const UiStrings kUiEn = {
    "Tangocho",
    "SETTINGS", "DECKS", "side: back",
    {"Continue", "Decks", "Date", "New/day", "Speed", "Statistics", "Wi-Fi »", "Orientation",
     "Language", "Brightness", "Sleep"},
    "NEW/DAY", "SPEED", "no", "%.1f s",
    "MEANING", "READING", "KANJI",
    "DATE", "last used", "today", "+1 day", "+%u days", "tap +1  hold: OK", "side hold: -1",
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
    "Syncing via %s...", "No network: set it by hand", "No network time: set it by hand",
    "DONE", "All reviewed for now", "day %u  %u cards", "due %u  new %u", "tap/hold: +10 new",
    "side hold: menu",
    "  %u/%u",
    "STATISTICS", "seen %u / %u", "due %u  new %u", "answers %u", "accuracy %u%%", "learned %u",
    "Storage damaged", "Hold the front button to erase it. Decks and progress are lost.",
    "Preparing storage...", "First start: this takes a few seconds.",
    "No pack found", "Put a .tgc in data/packs/ then run uploadfs",
    "Cannot open pack",
    "ORIENTATION", {"Portrait", "Landscape", "Auto"},
    "DETAIL", "ON", "KUN", "strokes %u", "correct %u/%u",
    "streak %u/%u", "time %s", "%uh %um", "%um %us", "%us",
    "LANGUAGE", {"Auto", "English", "Español"},
    "WI-FI",
    "BRIGHTNESS",
    "Sleeping", "Progress saved. The front button wakes me.",
    "Battery low", "Charge me, then press the front button.",
    "+ Get decks", "GET DECKS", "Connecting...", "Reading catalog...", "Downloading %u%%",
    "hold: download", "hold: update", "hold: study", "hold: retry", "Saved %s", "%s, %u cards, %s",
    "No decks in the catalog",
    "No home Wi-Fi saved", "Settings > Wi-Fi > Setup mode, then save your network on the page",
    "Network not found", "Wrong password?", "Could not connect", "Server unreachable",
    "Secure connection failed", "Server error %d", "No catalog published at this URL yet",
    "Bad catalog", "Newer catalog format: update the firmware", "Download corrupt",
    "Not enough space",
    "An update needs room for both copies. Delete the old one first: Settings > Wi-Fi > Web portal",
    "Not a valid deck", "Download failed", "Cancelled",
    "hold: get decks (home Wi-Fi)",
    {"Web portal", "Home: %s", "Setup mode", "Forget network"}, "none", "No home network saved",
    "Network forgotten", "FORGET WI-FI?", {"Cancel", "Forget"},
    "Scan to join", "Scan to open the page", "tap: other QR", "via %s", "Connecting to",
    "hold: setup mode", "tap: try again", "Connection lost...", "side: start studying", "%s %u%%",
    "side: cancel", "%s free", "%s: %s",
    "HOME NETWORK", "Checking catalog...", "IP %s", "signal %d dBm (%s)",
    {"weak", "fair", "good", "excellent"}, "catalog: %u decks", "network time: ok",
    "network time: no answer", "hold: test again", "Wi-Fi > Setup mode: save it on the page",
};

const UiStrings kUiEs = {
    "Tangocho",
    "AJUSTES", "MAZOS", "lateral: atrás",
    {"Seguir", "Mazos", "Fecha", "Nuevas/día", "Velocidad", "Estadisticas", "Wi-Fi »", "Orientacion",
     "Idioma", "Brillo", "Suspender"},
    "NUEVAS/DIA", "VELOCIDAD", "no", "%.1f s",
    "SIGNIFICADO", "LECTURA", "KANJI",
    "FECHA", "último uso", "hoy", "+1 día", "+%u días", "toque +1  larga: OK", "lateral larga: -1",
    {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"},
    "Sincronizando vía %s...", "Sin red: ajústala a mano", "Sin hora de red: ajústala a mano",
    "FIN", "Todo repasado", "dia %u  %u tarjetas", "vencen %u  nuevas %u",
    "toque/larga: +10", "lateral: menu",
    "  %u/%u",
    "ESTADISTICAS", "vistos %u / %u", "pendientes %u  nuevas %u", "respuestas %u",
    "precisión %u%%", "aprendidas %u",
    "Memoria dañada", "Mantén el botón frontal para borrarla. Se pierden mazos y progreso.",
    "Preparando memoria...", "Primer arranque: tarda unos segundos.",
    "No hay ningún mazo", "Pon un .tgc en data/packs/ y ejecuta uploadfs",
    "No se puede abrir el mazo",
    "ORIENTACION", {"Vertical", "Horizontal", "Auto"},
    "DETALLE", "ON", "KUN", "trazos %u", "aciertos %u/%u",
    "racha %u/%u", "tiempo %s", "%uh %um", "%um %us", "%us",
    "IDIOMA", {"Auto", "English", "Español"},
    "WI-FI",
    "BRILLO",
    "Suspendido", "Progreso guardado. El botón frontal me despierta.",
    "Batería baja", "Cárgame y pulsa el botón frontal.",
    "+ Descargar", "DESCARGAR", "Conectando...", "Leyendo catálogo...", "Descargando %u%%",
    "larga: descargar", "larga: actualizar", "larga: estudiar", "larga: reintentar",
    "Guardado %s", "%s %u tarjetas %s",
    "El catálogo está vacío",
    "Sin Wi-Fi de casa", "Ajustes > Wi-Fi > Modo configuración, y guarda tu red en la página",
    "Red no encontrada", "¿Contraseña incorrecta?", "No se pudo conectar", "Servidor inaccesible",
    "Fallo de conexión segura", "Error del servidor %d", "Aún no hay catálogo en esta URL",
    "Catálogo no válido", "Catálogo más nuevo: actualiza el firmware", "Descarga dañada",
    "No hay espacio",
    "Hace falta sitio para las dos copias. Borra antes la vieja: Ajustes > Wi-Fi > Portal web",
    "Mazo no válido", "Descarga fallida", "Cancelado",
    "larga: bajar mazos (Wi-Fi de casa)",
    {"Portal web", "Casa: %s", "Modo configuración", "Olvidar red"}, "ninguna",
    "No hay red de casa guardada", "Red olvidada", "OLVIDAR WI-FI?", {"Cancelar", "Olvidar"},
    "Escanea para unirte", "Escanea para abrir la página", "toque: otro QR", "vía %s",
    "Conectando a", "larga: modo config.", "toque: reintentar", "Conexión perdida...",
    "lateral: a estudiar", "%s %u%%", "lateral: cancelar", "%s libres", "%s: %s",
    "RED DE CASA", "Comprobando catálogo...", "IP %s", "señal %d dBm (%s)",
    {"débil", "regular", "buena", "excelente"}, "catálogo: %u mazos", "hora de red: ok",
    "hora de red: sin respuesta", "larga: repetir", "Wi-Fi > Modo configuración: guárdala en la página",
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
bool gNextReadingIsKun = false;

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

enum class Screen { Quiz, Menu, Decks, Choice, Done, Date, Stats, Detail, Wifi, GetDecks, WifiMenu, NetTest };
// Screen::Choice is shared by the row pickers (see openChoice()).
enum class Choice { NewPerDay, SlowMs, Orientation, UiLang, Brightness, ForgetWifi };
Screen gScreen = Screen::Quiz;
Screen gBeforeMenu = Screen::Quiz;
Screen gAfterDate = Screen::Quiz;  // where the date screen returns to
uint16_t gDateSteps = 0;  // days the proposed "today" is past the current day
uint16_t gBootDay = 0;    // persisted day index at boot: "last used" on the date screen
// A real wall-clock time was applied since boot (applyWallClock), so the
// power-cut date question is not needed.
bool gWallClockSynced = false;
// NVS `tangocho`/`tz`: the UTC offset of the last wall-clock sync, in minutes
// east of UTC (int16). Absent until the first sync; reused by the NTP sync,
// which only yields UTC (docs/srs.md section 5.1).
constexpr const char* kPrefTz = "tz";
// NTP over the home Wi-Fi gets this long once the network is up.
constexpr uint32_t kNtpTimeoutMs = 3000;
// Network time for the boot-time date question (docs/srs.md section 5.1): with a
// home Wi-Fi and an offset saved, the date screen joins in the background and
// asks NTP; the manual answer works meanwhile and is the fallback. Tried once
// per boot. The radio (gHome, shared with Get decks, which cannot be opened from
// this screen) is on only while Joining; the other states are the status line.
enum class DateSync : uint8_t { None, Joining, NoNetwork, NoTime };
DateSync gDateSync = DateSync::None;
int16_t gDateSyncTz = 0;  // the saved offset the sync will apply
std::vector<String> gPacks;  // paths of the *.tgc files in /packs
int gMenuSel = 0;            // highlighted row on the settings home
int gDeckSel = 0;            // highlighted row on the deck list
Choice gChoice = Choice::NewPerDay;
int gChoiceSel = 0;
int gChoiceCount = 0;
uint16_t gChoiceValues[4] = {0, 0, 0, 0};
String gCurrentPack;
Preferences gPrefs;

// Orientation setting, NVS `tangocho`/`orient` (it replaced the bool `land`,
// see loadOrientPref()). Auto is offered only on boards with an accelerometer:
// the IMU then picks rotation 0, 1 or 3 (imu/OrientationFilter) and the picked
// rotation is never persisted. Values double as orientRows indexes.
enum OrientPref : uint8_t { kOrientPortrait = 0, kOrientLandscape = 1, kOrientAuto = 2 };
uint8_t gOrientPref = kOrientPortrait;
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
int gWifiMenuSel = 0;
String gWifiMenuSsid;          // the saved home network, read when the menu opens
char gWifiMenuNote[48] = "";   // footer note (cleared when the selection moves)
uint16_t gWifiMenuNoteColor = 0;
// Settings > Wi-Fi > Home network: join, show the IP and the signal, read the
// catalog (and the network time), radio off again.
enum class NetTestStep : uint8_t { NoNetwork, Joining, Checking, Done, Failed };
NetTestStep gTestStep = NetTestStep::NoNetwork;
String gTestIp;
int gTestRssi = 0;
int gTestTime = -1;            // network time: -1 not tried (no offset saved), 0 failed, 1 ok
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
char gNetDeck[48] = "";      // the deck being downloaded (the portal screen shows it)
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
uint8_t gBrightness = 128;      // NVS `tangocho`/`bri`
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

void present() { gCanvas->pushSprite(&tgc::board::display(), 0, 0); }

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

// Right-aligned header hint. Drawn with the built-in GFX font (not the pack
// font) so accented Spanish ("atrás") renders; the 1x size matches smallText.
void drawBackHint(lgfx::LovyanGFX& d) {
    const char* s = ui().backHint;
    const int tw = gRender.textWidth(s, tgc::Renderer::kTextBuiltinSmall);
    gRender.drawCentred(d, s, W - 3 - tw, 3, tw, 8, 1, kWhite, kGrey,
                        tgc::Renderer::kTextBuiltinSmall);
}

void showError(const char* line1, const char* line2 = "") {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kRed);
    smallText(d, ui().appName, 3, 3, kWhite, kRed);
    d.setTextWrap(true);
    d.setCursor(4, 24);
    d.setFont(&lgfx::fonts::Font0);
    d.setTextColor(kYellow, kBlack);
    d.println(line1);
    d.setTextColor(kWhite, kBlack);
    d.println();
    d.println(line2);
    present();
}

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

// Count the current pack's cards that are due or unseen, for the boot log and
// the done screen. Cheap: one pass over the card table.
void packDueNew(uint32_t* due, uint32_t* fresh) {
    uint32_t d = 0, n = 0;
    const uint16_t today = gSrs.today();
    for (uint32_t i = 0; i < gPack->cardCount(); ++i) {
        tgc::Card c;
        if (!gPack->card(i, &c)) continue;
        const tgc::Record* r = gSrs.find(c.codepoint);
        if (!r || r->dueDay == 0)
            ++n;
        else if (r->dueDay <= today)
            ++d;
    }
    if (due) *due = d;
    if (fresh) *fresh = n;
}

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
        const tgc::ReadingKind want =
            gNextReadingIsKun ? tgc::ReadingKind::Kun : tgc::ReadingKind::On;
        for (int k = 0; k < kN; ++k) {
            const tgc::QuizMode mode = kModes[(static_cast<int>(item.mode) + k) % kN];
            if (!tgc::buildQuestion(*gPack, item.cardIndex, mode, gRng, &gQ, want)) continue;
            if (gQ.mode == tgc::QuizMode::Reading) gNextReadingIsKun = !gNextReadingIsKun;
            gSel = 0;
            gRevealed = false;
            gShownAt = millis();
            gNavTaps = 0;
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
void openNetTest();
void formatDate(uint16_t days, char* out, size_t n);
[[noreturn]] void sleepNow(bool critical);
void netRadioOff();
void netSyncClock();
void openDecks();
void openGetDecks(bool fromBoot);
void openPortal(PortalMode mode);
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
void drawNoQuestion() { showError(ui().errOpenPack, ui().doneMenuHint); }

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

void drawQuiz() {
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    auto& d = *gCanvas;
    d.fillScreen(kBlack);

    // Header: deck name left, battery right (both built-in font, size 1).
    const int headerH = 14;
    d.fillRect(0, 0, W, headerH, kGrey);
    smallText(d, gPack->deckName(), 3, 3, kWhite, kGrey);
    int bat = tgc::board::batteryPercent();
    gShownBattery = bat;
    if (bat >= 0) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", bat);
        smallText(d, b, W - 3, 3, bat < 15 ? kYellow : kWhite, kGrey, lgfx::top_right);
    }

    // Prompt: the kanji, or (meaning -> kanji) the meaning text.
    const bool reading = gQ.mode == tgc::QuizMode::Reading;
    const bool k2m = gQ.mode == tgc::QuizMode::MeaningToKanji;
    const int largePx = gPack->header().largePx ? gPack->header().largePx : 64;
    // Landscape: prompt in the left column, options bounded to the right one.
    const bool landscape = W > H;
    const int leftW = std::max(largePx, W * 2 / 5);
    const int colX = landscape ? leftW : 0;
    const int colW = landscape ? W - leftW : W;

    int kanjiH = largePx + 6;
    if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
    if (k2m) {
        const char* prompt = gQ.options[gQ.correct].c_str();
        // Landscape puts the meaning prompt in the left column (the kanji
        // options are in the right one); portrait keeps the full width.
        const int pX = 0;
        const int pW = landscape ? leftW : colW;
        uint8_t set = tgc::Renderer::kTextBuiltin;
        if (!tgc::Renderer::builtinCanDraw(prompt)) set = tgc::kGlyphSetText;
        else if (gRender.textWidth(prompt, set) > pW - 8) set = tgc::Renderer::kTextBuiltinSmall;
        // The box must be at least one line tall; the prompt is centred in it.
        const int boxY = landscape ? headerH : headerH + 2;
        const int boxH = landscape ? H - headerH : kanjiH;
        int ml = (boxH - 4) / gRender.lineHeight(set);
        if (ml < 1) ml = 1;
        if (ml > 4) ml = 4;
        gRender.drawCentred(d, prompt, pX + 3, boxY, pW - 6, boxH - (landscape ? 2 : 0), ml,
                            kWhite, kBlack, set);
    } else if (landscape) {
        drawPromptKanji(d, card.codepoint, 0, headerH, leftW, H - headerH);
    } else {
        drawPromptKanji(d, card.codepoint, 0, headerH + 2, W, kanjiH);
    }
    if (landscape)
        smallText(d, k2m ? ui().quizKanji : (reading ? ui().quizReading : ui().quizMeaning),
                  leftW / 2, H - 11, kYellow, kBlack, lgfx::top_center);
    else
        smallText(d, k2m ? ui().quizKanji : (reading ? ui().quizReading : ui().quizMeaning), W / 2,
                  headerH + 2 + kanjiH, kYellow, kBlack, lgfx::top_center);

    // Readings: 2x2 grid with large kana (set 3). Meanings: one per row, full
    // width, in the built-in font at 2x (falling back to the pack glyphs).
    const int top = landscape ? headerH + 2 : headerH + kanjiH + 14;
    const int area = H - top - 2;
    const int margin = 1, gap = 3;

    auto optionBg = [&](int i) -> uint16_t {
        if (gRevealed) return i == gQ.correct ? kGreen : (i == gSel ? kRed : kBlack);
        return i == gSel ? kHighlight : kBlack;
    };
    auto optionBox = [&](int i, int x, int y, int w, int h, uint8_t set, int maxLines) {
        uint16_t bg = optionBg(i);
        d.fillRoundRect(x, y, w, h, 4, bg);
        if (!gRevealed && i == gSel) d.drawRoundRect(x, y, w, h, 4, kWhite);
        gRender.drawCentred(d, gQ.options[i].c_str(), x + 2, y + 1, w - 4, h - 2, maxLines, kWhite, bg,
                            set);
    };

    if (k2m) {
        // Meaning prompt: the options are kanji (set 1) in the same 2x2 grid.
        const int cols = 2;
        const int rows = (gQ.count + cols - 1) / cols;
        const int cellW = (colW - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        for (int i = 0; i < gQ.count; ++i) {
            const int cx = colX + margin + (i % cols) * (cellW + gap);
            const int cy = top + (i / cols) * (cellH + gap);
            const uint16_t bg = optionBg(i);
            d.fillRoundRect(cx, cy, cellW, cellH, 4, bg);
            if (!gRevealed && i == gSel) d.drawRoundRect(cx, cy, cellW, cellH, 4, kWhite);
            tgc::Card oc;
            if (gPack->card(gQ.optionCard[i], &oc))
                gRender.drawKanji(d, tgc::kGlyphSetSmall, oc.codepoint, cx, cy, cellW, cellH, kWhite, bg);
        }
    } else if (reading) {
        const int cols = 2;
        const int rows = (gQ.count + cols - 1) / cols;
        const int cellW = (colW - 2 * margin - (cols - 1) * gap) / cols;
        const int cellH = (area - (rows - 1) * gap) / rows;
        auto allFit = [&](uint8_t set, int w) {
            for (int i = 0; i < gQ.count; ++i)
                if (gRender.textWidth(gQ.options[i].c_str(), set) > w - 4) return false;
            return true;
        };
        // The 2x2 grid when every option fits a cell (large kana, else the small
        // text). A cell holds 3 kana, so longer kun readings (ちいさい, うけたまわる)
        // get one full-width row each instead, which holds 6 large kana.
        uint8_t set = tgc::kGlyphSetOption;
        bool grid = allFit(set, cellW);
        if (!grid && allFit(tgc::kGlyphSetText, cellW)) {
            set = tgc::kGlyphSetText;
            grid = true;
        }
        if (grid) {
            for (int i = 0; i < gQ.count; ++i)
                optionBox(i, colX + margin + (i % cols) * (cellW + gap),
                          top + (i / cols) * (cellH + gap), cellW, cellH, set, 1);
        } else {
            const int w = colW - 2 * margin;
            if (!allFit(set, w)) set = tgc::kGlyphSetText;
            const int rowH = area / gQ.count;
            for (int i = 0; i < gQ.count; ++i)
                optionBox(i, colX + margin, top + i * rowH, w, rowH - gap, set, 1);
        }
    } else {
        // The built-in font has no glyph for some characters; then use the pack.
        uint8_t set = tgc::Renderer::kTextBuiltin;
        for (int i = 0; i < gQ.count; ++i)
            if (!tgc::Renderer::builtinCanDraw(gQ.options[i].c_str())) {
                set = tgc::kGlyphSetText;
                break;
            }
        const int w = colW - 2 * margin;
        // Shrink to 1x if any option would otherwise be truncated.
        if (set == tgc::Renderer::kTextBuiltin) {
            for (int i = 0; i < gQ.count; ++i)
                if (gRender.textWidth(gQ.options[i].c_str(), set) > w - 4) {
                    set = tgc::Renderer::kTextBuiltinSmall;
                    break;
                }
        }
        const int rowH = area / gQ.count;
        const int boxH = rowH - gap;
        int maxLines = (boxH - 2) / gRender.lineHeight(set);
        if (maxLines < 1) maxLines = 1;
        if (maxLines > 2) maxLines = 2;
        for (int i = 0; i < gQ.count; ++i)
            optionBox(i, colX + margin, top + i * rowH, w, boxH, set, maxLines);
    }
    present();
}

// Pick the built-in font size for a menu/list label: the 2x font when the box
// is at least `minH` tall and the label fits `w`, else the 1x font so long or
// narrow labels are never clipped.
uint8_t labelFont(const char* label, int h, int minH, int w) {
    if (h < minH) return tgc::Renderer::kTextBuiltinSmall;
    if (gRender.textWidth(label, tgc::Renderer::kTextBuiltin) > w)
        return tgc::Renderer::kTextBuiltinSmall;
    return tgc::Renderer::kTextBuiltin;
}

// One full-width highlighted row for the settings home and the choice screens,
// labelled with the built-in GFX font so accented Spanish renders. `h` is the
// box height; the 2x font is used when it fits, the 1x one otherwise (needed on
// the short landscape settings/choice screens).
void drawRow(lgfx::LovyanGFX& d, const char* label, int y, int h, bool sel) {
    const uint16_t bg = sel ? kHighlight : kBlack;
    d.fillRoundRect(2, y, W - 4, h, 5, bg);
    if (sel) d.drawRoundRect(2, y, W - 4, h, 5, kWhite);
    const uint8_t set = labelFont(label, h, 22, W - 10);
    gRender.drawCentred(d, label, 4, y + 1, W - 8, h - 2, 1, kWhite, bg, set);
}

// Settings home (hold the side button from the quiz): tap moves the highlight,
// hold activates the row. Each row is a full-width box with a label.
constexpr int kSettingRows = 11;

void drawMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().settingsTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    // Shrink the row step on the short landscape screen so all rows fit; in
    // portrait (or when there is room) this keeps the original 36 px step.
    const int top = 20;
    int step = 36;
    if (top + step * kSettingRows > H) {
        step = (H - top - 2) / kSettingRows;
        if (step < 10) step = 10;
    }
    // Tight landscape steps keep only a 1 px gap so the 1x font still fits; at
    // the 10 px floor (eleven rows on the 135 px landscape height) the gap goes,
    // so the highlight's border still clears the 8 px font above and below.
    const int pad = step >= 20 ? 4 : step > 10 ? 1 : 0;
    const int rowH = step - pad < 32 ? step - pad : 32;
    for (int i = 0; i < kSettingRows; ++i)
        drawRow(d, ui().menuRows[i], top + i * step, rowH, i == gMenuSel);
    present();
}

// Deck list (opened from "Mazos"): one row per pack; the loaded pack is marked
// '*' and shows its due/new counts. Hold loads the highlighted pack; the side
// button returns to the settings home. The date row lives in the settings home.
// The last row, "+ Get decks", opens the download screen (drawGetDecks).
void drawDecks() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().decksTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);  // only shown on the loaded pack's row
    const int top = 18;
    const int rows = static_cast<int>(gPacks.size()) + 1;  // + "Get decks"
    int rowH = 20;
    if (rows > 0) {
        const int fit = (H - top - 2) / rows;  // shrink so the list fits landscape
        if (fit < rowH) rowH = fit;
    }
    if (rowH < 10) rowH = 10;
    for (int i = 0; i < rows; ++i) {
        const int y = top + i * rowH;
        if (y + rowH - 2 > H) break;  // no more room
        const bool sel = i == gDeckSel;
        const uint16_t bg = sel ? kHighlight : kBlack;
        d.fillRoundRect(1, y, W - 2, rowH - 2, 4, bg);
        if (sel) d.drawRoundRect(1, y, W - 2, rowH - 2, 4, kWhite);
        if (i == rows - 1) {
            smallText(d, ui().getDecksRow, 4, y + (rowH - 2 - 8) / 2, kYellow, bg);
            break;
        }
        String label = gPacks[i];
        const int slash = label.lastIndexOf('/');
        if (slash >= 0) label = label.substring(slash + 1);
        if (label.endsWith(".tgc")) label = label.substring(0, label.length() - 4);
        if (gPacks[i] == gCurrentPack) {
            char counts[24];
            snprintf(counts, sizeof counts, ui().deckCountsFmt, (unsigned)due, (unsigned)fresh);
            label = "* " + label + counts;
        }
        smallText(d, label.c_str(), 4, y + (rowH - 2 - 8) / 2, kWhite, bg);
    }
    present();
}

// Generic choice screen: a title and the candidate values. Hold applies the
// highlighted value; the side button cancels back to the settings home.
void drawChoice() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    const char* title = gChoice == Choice::NewPerDay  ? ui().newPerDayTitle
                        : gChoice == Choice::SlowMs   ? ui().speedTitle
                        : gChoice == Choice::Orientation ? ui().orientTitle
                        : gChoice == Choice::Brightness  ? ui().brightTitle
                        : gChoice == Choice::ForgetWifi  ? ui().forgetTitle
                                                         : ui().langTitle;
    smallText(d, title, 3, 3, kWhite, kGrey);
    drawBackHint(d);
    // "Forget": which network, between the header and the rows.
    if (gChoice == Choice::ForgetWifi)
        gRender.drawCentred(d, gWifiMenuSsid.c_str(), 2, 20, W - 4, 16, 1, kYellow, kBlack,
                            tgc::Renderer::kTextBuiltinSmall);

    // Shrink the row step on the short landscape screen so all rows fit; portrait
    // keeps the original 40 px step and 32 px box.
    const int top = 40;
    int step = 40;
    if (top + step * gChoiceCount > H) {
        step = (H - top - 2) / gChoiceCount;
        if (step < 10) step = 10;
    }
    const int rowH = step - 4 < 32 ? step - 4 : 32;
    for (int i = 0; i < gChoiceCount; ++i) {
        char label[16];
        if (gChoice == Choice::Orientation) {
            snprintf(label, sizeof label, "%s", ui().orientRows[gChoiceValues[i] % 3]);
        } else if (gChoice == Choice::ForgetWifi) {
            snprintf(label, sizeof label, "%s", ui().forgetRows[i % 2]);
        } else if (gChoice == Choice::UiLang) {
            snprintf(label, sizeof label, "%s", ui().langRows[i]);
        } else if (gChoice == Choice::Brightness) {
            snprintf(label, sizeof label, "%u%%", (unsigned)(gChoiceValues[i] * 100u / 255u));
        } else if (gChoice == Choice::SlowMs) {
            if (gChoiceValues[i] == 0xFFFF) snprintf(label, sizeof label, "%s", ui().speedOff);
            else snprintf(label, sizeof label, ui().speedSecondsFmt, gChoiceValues[i] / 1000.0f);
        } else {
            snprintf(label, sizeof label, "%u", (unsigned)gChoiceValues[i]);
        }
        drawRow(d, label, top + i * step, rowH, i == gChoiceSel);
    }
    present();
}

// "All done" screen: no card is due and no new card is left today. A tap, hold
// or side tap rebuilds the queue and tries again; the side hold opens the menu.
void drawDone() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, gPack->deckName(), 3, 3, kWhite, kGrey);
    smallText(d, ui().doneBadge, W - 3, 3, kWhite, kGrey, lgfx::top_right);

    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);
    char line[48];
    smallText(d, ui().doneAllReviewed, W / 2, H / 2 - 24, kWhite, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, ui().doneDayFmt, (unsigned)gSrs.today(), (unsigned)gSrs.count());
    smallText(d, line, W / 2, H / 2 - 6, kYellow, kBlack, lgfx::top_center);
    snprintf(line, sizeof line, ui().doneDueFmt, (unsigned)due, (unsigned)fresh);
    smallText(d, line, W / 2, H / 2 + 8, kWhite, kBlack, lgfx::top_center);
    smallText(d, ui().doneStudyMore, W / 2, H / 2 + 30, kWhite, kBlack, lgfx::top_center);
    smallText(d, ui().doneMenuHint, W / 2, H / 2 + 44, kWhite, kBlack, lgfx::top_center);
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
// hold takes one back (never below the current day: the index does not
// rewind), a hold confirms. Asked on a power-on boot (the side tap skips it) and
// opened from the settings home for manual corrections.
uint16_t dateTarget() {
    const uint32_t t = static_cast<uint32_t>(gSrs.today()) + gDateSteps;
    return t > 0xFFFFu ? 0xFFFF : static_cast<uint16_t>(t);
}

const char* weekdayName(uint16_t day) {
    return ui().weekdays[tgc::weekdayFromDays(gSrs.epochDayOf(day))];
}

void openDate(Screen after) {
    gAfterDate = after;
    gDateSteps = 0;
    gScreen = Screen::Date;
    drawScreen();
}

// Labels share a line with their value when it fits (landscape), else sit above
// it; the proposed date is one 2x line when it fits, else weekday over date.
// All text uses the built-in font so accented Spanish renders.
void drawDate() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().dateTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    constexpr uint8_t kBig = tgc::Renderer::kTextBuiltin;
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;
    const int w = W - 4;
    const int gap = H > W ? 10 : 3;  // portrait has room to breathe
    char date[16], value[32], line[64];

    // Last used: the day the device last ran.
    formatDate(gBootDay, date, sizeof date);
    snprintf(value, sizeof value, "%s %s", weekdayName(gBootDay), date);
    snprintf(line, sizeof line, "%s: %s", ui().dateLastUsed, value);
    int y = 19;
    if (gRender.textWidth(line, kSmall) <= w) {
        gRender.drawCentred(d, line, 2, y, w, 10, 1, kWhite, kBlack, kSmall);
        y += 10;
    } else {
        gRender.drawCentred(d, ui().dateLastUsed, 2, y, w, 10, 1, kWhite, kBlack, kSmall);
        gRender.drawCentred(d, value, 2, y + 10, w, 10, 1, kWhite, kBlack, kSmall);
        y += 20;
    }
    y += gap;

    // The proposed "today", highlighted: this is what the buttons change.
    const uint16_t target = dateTarget();
    formatDate(target, date, sizeof date);
    snprintf(value, sizeof value, "%s %s", weekdayName(target), date);
    gRender.drawCentred(d, ui().dateToday, 2, y, w, 10, 1, kYellow, kBlack, kSmall);
    y += 11;
    const bool oneLine = gRender.textWidth(value, kBig) <= w - 8;
    const int boxH = oneLine ? 24 : 42;
    d.fillRoundRect(2, y, w, boxH, 5, kHighlight);
    d.drawRoundRect(2, y, w, boxH, 5, kWhite);
    if (oneLine) {
        gRender.drawCentred(d, value, 4, y + 2, w - 4, boxH - 4, 1, kWhite, kHighlight, kBig);
    } else {
        gRender.drawCentred(d, weekdayName(target), 4, y + 3, w - 4, 17, 1, kWhite, kHighlight,
                            kBig);
        gRender.drawCentred(d, date, 4, y + 21, w - 4, 17, 1, kWhite, kHighlight, kBig);
    }
    y += boxH + gap;

    // How far that is from the current day.
    if (gDateSteps == 1)
        snprintf(line, sizeof line, "%s", ui().dateOneDay);
    else
        snprintf(line, sizeof line, ui().dateDaysFmt, (unsigned)gDateSteps);
    gRender.drawCentred(d, line, 2, y, w, 18, 1, kYellow, kBlack,
                        gRender.textWidth(line, kBig) <= w ? kBig : kSmall);

    // The network-time sync of the boot-time question, right above the hints
    // (one line in landscape, where the space above is taken).
    if (gDateSync != DateSync::None) {
        const char* status = line;
        uint16_t color = kGreen;
        if (gDateSync == DateSync::Joining) {
            snprintf(line, sizeof line, ui().dateSyncingFmt, gHome.ssid());
        } else {
            status = gDateSync == DateSync::NoNetwork ? ui().dateNoNetwork : ui().dateNoTime;
            color = kYellow;
        }
        const int lines = H > W ? 2 : 1;
        gRender.drawCentred(d, status, 2, H - 24 - lines * 10, w, lines * 10, lines, color, kBlack,
                            kSmall);
    }

    // Button hints along the bottom.
    gRender.drawCentred(d, ui().dateHintTap, 2, H - 22, w, 10, 1, kWhite, kBlack, kSmall);
    gRender.drawCentred(d, ui().dateHintSide, 2, H - 11, w, 10, 1, kWhite, kBlack, kSmall);
    present();
}

// "2h 5m", "5m 30s" or "30s" depending on the magnitude. The unit strings come
// from UiStrings so a language can change them; the result stays short enough
// for the 135 px portrait width (the stats line shrinks to the 1x font if not).
void formatReviewTime(uint32_t ms, char* out, size_t n) {
    const uint32_t totalS = ms / 1000u;
    const uint32_t mins = totalS / 60u;
    const uint32_t hours = mins / 60u;
    if (hours > 0)
        snprintf(out, n, ui().statsTimeHM, (unsigned)hours, (unsigned)(mins % 60u));
    else if (mins > 0)
        snprintf(out, n, ui().statsTimeMS, (unsigned)mins, (unsigned)(totalS % 60u));
    else
        snprintf(out, n, ui().statsTimeS, (unsigned)totalS);
}

// Statistics screen (opened from the settings home): the internal date, the day
// streak and total time on questions, cards seen out of the loaded pack, its
// due/new counts, and the lifetime answer counters from the SRS store. Read-only;
// the side button returns to settings.
void drawStats() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().statsTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    const tgc::Stats st = gSrs.stats();
    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);

    // Accuracy over every grade ever recorded; guard a zero divisor and a
    // saturation edge (lapses can equal reps but never exceed it).
    const uint32_t acc = st.reps ? (st.reps > st.lapses ? st.reps - st.lapses : 0) * 100u / st.reps
                                 : 0;

    char date[16], streak[24], timeLine[32], seen[24], dueNew[32], answers[24], accuracy[24],
        learned[24], duration[16];
    formatDate(gSrs.today(), date, sizeof date);
    snprintf(streak, sizeof streak, ui().statsStreakFmt, (unsigned)st.streakCurrent,
             (unsigned)st.streakBest);
    formatReviewTime(st.reviewMs, duration, sizeof duration);
    snprintf(timeLine, sizeof timeLine, ui().statsTimeFmt, duration);
    snprintf(seen, sizeof seen, ui().statsSeenFmt, (unsigned)st.seen, (unsigned)gPack->cardCount());
    snprintf(dueNew, sizeof dueNew, ui().statsDueNewFmt, (unsigned)due, (unsigned)fresh);
    snprintf(answers, sizeof answers, ui().statsAnswersFmt, (unsigned)st.answers);
    snprintf(accuracy, sizeof accuracy, ui().statsAccuracyFmt, (unsigned)acc);
    snprintf(learned, sizeof learned, ui().statsLearnedFmt, (unsigned)st.learned);

    const char* lines[] = {date, streak, timeLine, seen, dueNew, answers, accuracy, learned};
    constexpr int kStatsLines = 8;
    // Shrink the line step on the short landscape screen; portrait keeps 26/24.
    const int top = 26;
    int step = 24;
    if (top + step * kStatsLines > H) {
        step = (H - top - 2) / kStatsLines;
        if (step < 10) step = 10;
    }
    for (int i = 0; i < kStatsLines; ++i) {
        const uint8_t set = labelFont(lines[i], step - 2, 18, W - 4);
        gRender.drawCentred(d, lines[i], 2, top + i * step, W - 4, step - 4, 1,
                            i == 0 ? kYellow : kWhite, kBlack, set);
    }
    present();
}

// "704 KB" / "1.2 MB", or "704K" / "1.2M" when `compact`.
void formatBytes(uint32_t bytes, char* out, size_t n, bool compact) {
    if (bytes >= 1024u * 1024u) {
        const unsigned tenths = static_cast<unsigned>(bytes / (1024u * 1024u / 10u));
        snprintf(out, n, compact ? "%u.%uM" : "%u.%u MB", tenths / 10u, tenths % 10u);
    } else {
        snprintf(out, n, compact ? "%uK" : "%u KB", (unsigned)((bytes + 1023u) / 1024u));
    }
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

// The page address for the screen: whole when it fits `w` in the 1x font, else
// without "http://" and the trailing '/' (a browser takes the bare IP too).
void shownUrl(const char* url, int w, char* out, size_t n) {
    snprintf(out, n, "%s", url);
    if (gRender.textWidth(out, tgc::Renderer::kTextBuiltinSmall) <= w) return;
    const char* host = std::strncmp(url, "http://", 7) == 0 ? url + 7 : url;
    snprintf(out, n, "%s", host);
    const size_t len = std::strlen(out);
    if (len > 0 && out[len - 1] == '/') out[len - 1] = 0;
}

// Wi-Fi portal screen (WifiPortal.h). Serving: a QR code -- in setup mode the
// one that joins the device's access point, or (a tap) the one that opens the
// page; in home mode the page's address -- with the network, the address, the
// portal's status line (or a download's progress), the free space and the
// hints. Portrait stacks them under the QR, landscape puts them right of it.
// Joining and failed: the network, why, and the way to setup mode. Only the
// built-in font is used, so it works before any pack is open.
void drawWifi() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int headerH = 14;
    d.fillRect(0, 0, W, headerH, kGrey);
    smallText(d, ui().wifiTitle, 3, 3, kWhite, kGrey);
    // On the boot-time screen without a deck the side button does nothing.
    if (!gWifiBootUpload || gPack->isOpen()) drawBackHint(d);
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;

    if (gWifiStep != WifiStep::Serving) {
        const bool failed = gWifiStep == WifiStep::Failed;
        const char* title = failed ? gNetErrTitle : ui().wifiConnectingTo;
        gRender.drawCentred(d, title, 2, H / 2 - 36, W - 4, 22, 2, failed ? kYellow : kWhite, kBlack,
                            labelFont(title, 22, 16, W - 8));
        gRender.drawCentred(d, failed ? gNetErrDetail : gHome.ssid(), 2, H / 2 - 10, W - 4, 20, 2,
                            failed ? kWhite : kYellow, kBlack, kSmall);
        if (failed) gRender.drawCentred(d, ui().wifiTapRetry, 2, H - 23, W - 4, 10, 1, kWhite, kBlack, kSmall);
        gRender.drawCentred(d, ui().wifiHoldSetup, 2, H - 12, W - 4, 10, 1, kWhite, kBlack, kSmall);
        present();
        return;
    }

    const bool home = gPortalMode == PortalMode::Home;
    const bool landscape = W > H;
    const bool joinQr = !home && gQrPage == 0;
    const std::string qrText =
        joinQr ? tgc::wifiQrOpenNetwork(gPortal.ssid()) : std::string(gPortal.url());
    const char* qrLabel = joinQr ? ui().wifiScanJoin : ui().wifiScanOpen;

    int y = headerH + 2;
    int tx = 2, tw = W - 4;
    if (landscape) {
        const int side = drawQr(d, qrText.c_str(), 2, y + 1, H - y - 3);
        tx = side + 6;
        tw = W - tx - 2;
    } else {
        gRender.drawCentred(d, qrLabel, 2, y, W - 4, 10, 1, kYellow, kBlack, kSmall);
        y += 11;
        const int box = std::min(W - 4, (H - y) * 11 / 20);
        const int side = qrSide(qrText.c_str(), box);
        drawQr(d, qrText.c_str(), (W - side) / 2, y, box);
        y += side + 3;
    }
    // One line (or `n` wrapped lines) of 1x text in the text area.
    auto line = [&](const char* s, uint16_t color, int n = 1) {
        if (!s || !*s || y + 10 * n > H) return;
        gRender.drawCentred(d, s, tx, y, tw, 10 * n, n, color, kBlack, kSmall);
        y += 10 * n + 1;
    };
    char buf[72];
    if (landscape) line(qrLabel, kYellow, 2);
    if (home) {
        snprintf(buf, sizeof buf, ui().wifiViaFmt, gPortal.ssid());
        line(buf, kWhite);
    } else {
        line(gPortal.ssid(), kWhite);
    }
    shownUrl(gPortal.url(), tw, buf, sizeof buf);
    line(buf, kYellow);
    if (gPortal.mdnsUrl()[0]) {
        shownUrl(gPortal.mdnsUrl(), tw, buf, sizeof buf);
        line(buf, kWhite);
    }

    // Status: a download from the page, a dropped link, or the portal's line.
    if (gNetBusy) {
        snprintf(buf, sizeof buf, ui().wifiDownloadFmt, gNetDeck, (unsigned)(gNetPct < 0 ? 0 : gNetPct));
        line(buf, kYellow);
        if (y + 8 <= H) {
            d.drawRect(tx + 2, y, tw - 4, 7, kWhite);
            const int fill = gNetPct > 0 ? (tw - 6) * gNetPct / 100 : 0;
            if (fill > 0) d.fillRect(tx + 3, y + 1, fill, 5, kGreen);
            y += 9;
        }
        line(ui().wifiCancelHint, kWhite);
        present();
        return;
    }
    // The boot-time fallback's reason first; the status line then gets one line
    // so the hints still fit the portrait screen.
    if (gWifiNote[0]) line(gWifiNote, kYellow, 2);
    if (home && !gPortal.linkUp())
        line(ui().wifiLinkLost, kYellow);
    else
        line(gPortal.lastMessage(), kGreen, gWifiNote[0] ? 1 : 2);
    char size[16];
    formatBytes(static_cast<uint32_t>(tgc::fsFreeBytes()), size, sizeof size, false);
    snprintf(buf, sizeof buf, ui().wifiFreeFmt, size);
    line(buf, kWhite);

    // Hints.
    if (!home) line(ui().wifiTapOtherQr, kWhite);
    if (gWifiBootUpload && gPack->isOpen())
        line(ui().wifiStudyHint, kGreen);
    else if (gWifiBootUpload && tgc::HomeWifi::hasCredentials())
        line(ui().wifiGetDecksHint, kGreen);
    present();
}

// Settings > Wi-Fi: the web portal (home mode once a network is saved), the
// home network with its test, setup mode (the access point, e.g. to change
// networks) and "forget". Same rows as the settings home; a footer note says
// what the last action did.
constexpr int kWifiMenuRows = 4;

void drawWifiMenu() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().wifiTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);

    const int top = 20;
    const int noteH = 12;
    int step = 36;
    if (top + step * kWifiMenuRows > H - noteH) step = (H - noteH - top - 2) / kWifiMenuRows;
    const int rowH = step - 4 < 32 ? step - 4 : 32;
    char home[64];
    snprintf(home, sizeof home, ui().wifiMenuRows[1],
             gWifiMenuSsid.length() ? gWifiMenuSsid.c_str() : ui().wifiHomeNone);
    const char* labels[kWifiMenuRows] = {ui().wifiMenuRows[0], home, ui().wifiMenuRows[2],
                                         ui().wifiMenuRows[3]};
    for (int i = 0; i < kWifiMenuRows; ++i) drawRow(d, labels[i], top + i * step, rowH, i == gWifiMenuSel);
    if (gWifiMenuNote[0])
        gRender.drawCentred(d, gWifiMenuNote, 2, H - noteH + 1, W - 4, 10, 1, gWifiMenuNoteColor,
                            kBlack, tgc::Renderer::kTextBuiltinSmall);
    present();
}

// Settings > Wi-Fi > Home network: joining, checking (NTP and the catalog),
// then the result -- network, IP, signal, catalog, network time -- with the
// radio off again; or why it failed, or that no network is saved.
void drawNetTest() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().testTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;

    if (gTestStep == NetTestStep::NoNetwork) {
        gRender.drawCentred(d, ui().wifiNoNetwork, 2, H / 2 - 36, W - 4, 22, 2, kYellow, kBlack, kSmall);
        gRender.drawCentred(d, ui().testNoNetworkHint, 2, H / 2 - 8, W - 4, 40, 4, kWhite, kBlack, kSmall);
        present();
        return;
    }
    if (gTestStep == NetTestStep::Joining || gTestStep == NetTestStep::Failed) {
        const bool failed = gTestStep == NetTestStep::Failed;
        const char* title = failed ? gTestFail : ui().netConnecting;
        gRender.drawCentred(d, title, 2, H / 2 - 36, W - 4, 22, 2, kYellow, kBlack,
                            labelFont(title, 22, 16, W - 8));
        gRender.drawCentred(d, gHome.ssid(), 2, H / 2 - 10, W - 4, 20, 2, kWhite, kBlack, kSmall);
        if (failed)
            gRender.drawCentred(d, ui().testHoldAgain, 2, H - 12, W - 4, 10, 1, kWhite, kBlack, kSmall);
        present();
        return;
    }
    int y = 20;
    auto line = [&](const char* s, uint16_t color, int n = 1) {
        if (!s || !*s || y + 10 * n > H - 12) return;
        gRender.drawCentred(d, s, 2, y, W - 4, 10 * n, n, color, kBlack, kSmall);
        y += 10 * n + (H > W ? 4 : 1);
    };
    char buf[72];
    line(gHome.ssid(), kYellow, 2);
    snprintf(buf, sizeof buf, ui().testIpFmt, gTestIp.c_str());
    line(buf, kWhite);
    snprintf(buf, sizeof buf, ui().testSignalFmt, gTestRssi, ui().testQuality[tgc::rssiQuality(gTestRssi)]);
    line(buf, kWhite, 2);
    if (gTestStep == NetTestStep::Checking) {
        line(ui().testChecking, kYellow);
        present();
        return;
    }
    line(gTestCatalog, gTestCatalogColor, 2);
    if (gTestTime >= 0) line(gTestTime ? ui().testTimeOk : ui().testTimeFail, gTestTime ? kGreen : kYellow);
    gRender.drawCentred(d, ui().testHoldAgain, 2, H - 12, W - 4, 10, 1, kWhite, kBlack, kSmall);
    present();
}

// "Get decks" screen. Connecting / reading: a centred status and the network
// name. List: one row per catalog deck ('+' new, '=' already here, '~' here
// with another size), scrolled to keep the selection visible, and a two-line
// footer (language, cards and size of the selection, or a status note, then the
// hold hint; a progress bar while downloading). Error: a title and a wrapped
// detail. Only the built-in font is used, so it works before any pack is open.
void drawGetDecks() {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    d.fillRect(0, 0, W, 14, kGrey);
    smallText(d, ui().getDecksTitle, 3, 3, kWhite, kGrey);
    drawBackHint(d);
    constexpr uint8_t kSmall = tgc::Renderer::kTextBuiltinSmall;

    if (gNetStep == NetStep::Connecting || gNetStep == NetStep::Catalog) {
        const char* line = gNetStep == NetStep::Connecting ? ui().netConnecting : ui().netCatalog;
        gRender.drawCentred(d, line, 2, H / 2 - 22, W - 4, 18, 1, kYellow, kBlack,
                            labelFont(line, 18, 16, W - 8));
        gRender.drawCentred(d, gHome.ssid(), 2, H / 2 + 2, W - 4, 20, 2, kWhite, kBlack, kSmall);
        present();
        return;
    }
    if (gNetStep == NetStep::Error) {
        const int titleH = 36;
        gRender.drawCentred(d, gNetErrTitle, 2, 20, W - 4, titleH, 2, kYellow, kBlack,
                            labelFont(gNetErrTitle, titleH, 16, W - 8));
        gRender.drawCentred(d, gNetErrDetail, 2, 20 + titleH + 4, W - 4, H - 20 - titleH - 20, 6,
                            kWhite, kBlack, kSmall);
        gRender.drawCentred(d, ui().netHoldRetry, 2, H - 12, W - 4, 10, 1, kWhite, kBlack, kSmall);
        present();
        return;
    }

    const int footerH = 22;
    const int footY = H - footerH;
    const int n = static_cast<int>(gCatalog.size());
    if (n == 0) {
        gRender.drawCentred(d, ui().netEmpty, 2, 20, W - 4, footY - 22, 3, kWhite, kBlack, kSmall);
    } else {
        const int top = 18;
        const int rowH = H >= 200 ? 18 : 15;
        int visible = (footY - 2 - top) / rowH;
        if (visible < 1) visible = 1;
        const int first = gNetSel >= visible ? gNetSel - visible + 1 : 0;
        for (int i = first; i < n && i < first + visible; ++i) {
            const int y = top + (i - first) * rowH;
            const bool sel = i == gNetSel;
            const uint16_t bg = sel ? kHighlight : kBlack;
            d.fillRoundRect(1, y, W - 2, rowH - 2, 4, bg);
            if (sel) d.drawRoundRect(1, y, W - 2, rowH - 2, 4, kWhite);
            const tgc::CatalogEntry& e = gCatalog[i];
            const tgc::DeckState st = gCatalogState[i];
            const uint16_t fg = st == tgc::DeckState::OnDevice ? kGreen
                                : (st == tgc::DeckState::Update ? kYellow : kWhite);
            const int ty = y + (rowH - 2 - 8) / 2;
            // Right column "en 704K"; when the name would not fit next to it
            // (the 135 px portrait), only the size (the footer has the language).
            const int nameX = 13;
            String name = e.name.c_str();
            char right[24], size[12];
            formatBytes(e.bytes, size, sizeof size, true);
            snprintf(right, sizeof right, "%s %s", e.lang.c_str(), size);
            int maxW = W - 4 - gRender.textWidth(right, kSmall) - 6 - nameX;
            if (gRender.textWidth(name.c_str(), kSmall) > maxW) {
                snprintf(right, sizeof right, "%s", size);
                maxW = W - 4 - gRender.textWidth(right, kSmall) - 6 - nameX;
            }
            smallText(d, right, W - 4, ty, kWhite, bg, lgfx::top_right);
            smallText(d, st == tgc::DeckState::OnDevice ? "=" : (st == tgc::DeckState::Update ? "~" : "+"),
                      4, ty, fg, bg);
            // The name gets what the size column leaves, cut with ".." if needed.
            if (gRender.textWidth(name.c_str(), kSmall) > maxW) {
                while (name.length() > 1 && gRender.textWidth((name + "..").c_str(), kSmall) > maxW)
                    name.remove(name.length() - 1);
                name += "..";
            }
            smallText(d, name.c_str(), nameX, ty, fg, bg);
        }
    }

    // Footer: what the selection is (or a status note), then what a hold does.
    char line1[64] = "";
    uint16_t c1 = kWhite;
    const char* line2 = "";
    if (gNetBusy) {
        snprintf(line1, sizeof line1, ui().netDownloadingFmt, (unsigned)(gNetPct < 0 ? 0 : gNetPct));
        c1 = kYellow;
    } else if (gNetNote[0]) {
        snprintf(line1, sizeof line1, "%s", gNetNote);
        c1 = gNetNoteColor;
    } else if (n > 0) {
        const tgc::CatalogEntry& e = gCatalog[gNetSel];
        char size[16];
        formatBytes(e.bytes, size, sizeof size, false);
        snprintf(line1, sizeof line1, ui().netCardsFmt, e.lang.empty() ? "?" : e.lang.c_str(),
                 (unsigned)e.cards, size);
    }
    if (!gNetBusy && n > 0) {
        const tgc::DeckState st = gCatalogState[gNetSel];
        line2 = st == tgc::DeckState::OnDevice
                    ? ui().netHoldStudy
                    : (st == tgc::DeckState::Update ? ui().netHoldUpdate : ui().netHoldDownload);
    }
    gRender.drawCentred(d, line1, 2, footY + 1, W - 4, 10, 1, c1, kBlack, kSmall);
    if (gNetBusy) {
        const int bw = W - 8;
        d.drawRect(4, footY + 12, bw, 8, kWhite);
        const int fill = gNetPct > 0 ? (bw - 2) * gNetPct / 100 : 0;
        if (fill > 0) d.fillRect(5, footY + 13, fill, 6, kGreen);
    } else {
        gRender.drawCentred(d, line2, 2, footY + 11, W - 4, 10, 1, kWhite, kBlack, kSmall);
    }
    present();
}

// Joins a card's "、"-separated reading list into one display string, dropping
// the okurigana markers ("た.べる、く.う" -> "たべる、くう") for display.
std::string joinReadings(const char* list) {
    std::string out;
    if (!list || !*list) return out;
    const char* kSep = "\xE3\x80\x81";  // U+3001 "、"
    const char* p = list;
    while (*p) {
        const char* e = std::strstr(p, kSep);
        const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
        for (size_t i = 0; i < n; ++i)
            if (p[i] != '.') out += p[i];
        if (!e) break;
        out += kSep;
        p = e + 3;
    }
    return out;
}

// The detail card's kanji: the static set-0 glyph, or -- when the pack carries
// stroke data for the card and the animation is running -- the strokes of the
// current frame: completed ones in white, the current one in yellow, the
// undrawn ones absent. The masks live in the same em square drawKanji uses.
void drawDetailKanji(int boxX, int boxY, int boxW, int boxH, uint32_t cp) {
    auto& d = *gCanvas;
    const int px = gPack->header().largePx ? gPack->header().largePx : 64;
    if (!gAnimOn) {
        gRender.drawKanji(d, tgc::kGlyphSetLarge, cp, boxX, boxY, boxW, boxH, kWhite, kBlack);
        return;
    }
    const int emX = boxX + (boxW - px) / 2;
    const int emY = boxY + (boxH - px) / 2;
#if defined(TGC_QUIZ_LOG)
    const uint32_t t0 = micros();
#endif
    for (int i = 0; i <= gAnimFrame; ++i)
        gRender.drawStroke(d, gQ.cardIndex, static_cast<uint16_t>(i), emX, emY,
                           i == gAnimFrame ? kYellow : kWhite);
#if defined(TGC_QUIZ_LOG)
    // One line per animation loop: the last frame draws every stroke (worst case).
    if (gAnimFrame == static_cast<int>(gAnimStrokes) - 1)
        QLOG("R U+%04X strokes %d draw %u us\n", (unsigned)cp, gAnimFrame + 1,
             (unsigned)(micros() - t0));
#endif
}

// Detail card (roadmap phase 3): a tap after a wrong reveal opens this read-only
// card with the kanji, all meanings, all on/kun readings (okurigana dots
// removed), the stroke count and this card's own accuracy from the SRS record.
// Portrait centres the kanji in the upper ~45% with the details below; landscape
// mirrors the quiz (kanji left, details right). Japanese readings use the pack's
// kana (set 2, falling back to set 3 by width as the quiz does); Latin values use
// the built-in font when it can draw them (2x, shrunk to 1x), else pack glyphs.
// When the pack carries stroke data for the card, the kanji animates instead
// (see drawDetailKanji and the Screen::Detail branch of loop()).
void drawDetail() {
    tgc::Card card;
    if (!questionCard(&card)) {
        drawNoQuestion();
        return;
    }
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int headerH = 14;
    d.fillRect(0, 0, W, headerH, kGrey);
    smallText(d, gPack->deckName(), 3, 3, kWhite, kGrey);
    smallText(d, ui().detailTitle, W - 3, 3, kWhite, kGrey, lgfx::top_right);

    const int largePx = gPack->header().largePx ? gPack->header().largePx : 64;
    const bool landscape = W > H;

    int x, w, top, bottom, kx, ky, kw, kh;
    if (landscape) {
        const int leftW = std::max(largePx, W * 2 / 5);
        kx = 0;
        ky = headerH;
        kw = leftW;
        kh = H - headerH;
        x = leftW + 3;
        w = W - leftW - 6;
        top = headerH + 2;
        bottom = H - 2;
    } else {
        int kanjiH = largePx + 6;
        if (kanjiH > H * 45 / 100) kanjiH = H * 45 / 100;
        kx = 0;
        ky = headerH + 2;
        kw = W;
        kh = kanjiH;
        x = 3;
        w = W - 6;
        top = headerH + 2 + kanjiH;
        bottom = H - 2;
    }
    drawDetailKanji(kx, ky, kw, kh, card.codepoint);

    const int lh = gRender.lineHeight(tgc::kGlyphSetText);
    const std::string on = joinReadings(card.onyomi);
    const std::string kun = joinReadings(card.kunyomi);
    const tgc::Record* rec = gSrs.find(card.codepoint);
    const bool hasAcc = rec && rec->reps > 0;

    // Kana: set 2, falling back to set 3 only when set 2 overflows and set 3 fits
    // (mirrors the quiz's width test; in practice readings are short).
    auto kanaSet = [&](const std::string& s) -> uint8_t {
        if (gRender.textWidth(s.c_str(), tgc::kGlyphSetText) > w &&
            gRender.textWidth(s.c_str(), tgc::kGlyphSetOption) <= w)
            return tgc::kGlyphSetOption;
        return tgc::kGlyphSetText;
    };
    // Latin: the built-in font when it covers the text (2x, shrunk to 1x), else
    // the pack glyphs (same fallback rules as the quiz meaning options).
    auto latinSet = [&](const char* s) -> uint8_t {
        if (!tgc::Renderer::builtinCanDraw(s)) return tgc::kGlyphSetText;
        return gRender.textWidth(s, tgc::Renderer::kTextBuiltin) <= w
                   ? static_cast<uint8_t>(tgc::Renderer::kTextBuiltin)
                   : static_cast<uint8_t>(tgc::Renderer::kTextBuiltinSmall);
    };

    // Reserve a label + one line for each optional reading and one short line
    // each for strokes and accuracy, so the meanings block only takes what is
    // left (it ellipsises rather than clipping, as elsewhere in this file).
    int reserve = 11;                                       // strokes line
    if (!on.empty()) reserve += 8 + lh;                     // ON label + value
    if (!kun.empty()) reserve += 8 + lh;                    // KUN label + value
    if (hasAcc) reserve += 11;                              // accuracy line
    int meaningLines = (bottom - top - reserve - 8) / lh;
    if (meaningLines < 1) meaningLines = 1;
    if (meaningLines > 3) meaningLines = 3;

    int y = top;
    const uint8_t mset = latinSet(card.meanings);
    smallText(d, ui().quizMeaning, x, y, kYellow, kBlack);
    y += 8;
    y += gRender.drawWrapped(d, card.meanings, x, y, w, meaningLines, kWhite, kBlack, mset) *
         gRender.lineHeight(mset);
    y += 3;

    auto drawReading = [&](const char* label, const std::string& s) {
        if (s.empty() || y + 8 + lh > bottom) return;
        const uint8_t set = kanaSet(s);
        smallText(d, label, x, y, kYellow, kBlack);
        y += 8;
        y += gRender.drawWrapped(d, s.c_str(), x, y, w, 1, kWhite, kBlack, set) *
             gRender.lineHeight(set);
        y += 3;
    };
    drawReading(ui().detailOn, on);
    drawReading(ui().detailKun, kun);

    if (y + 11 <= bottom) {
        char s[32];
        snprintf(s, sizeof s, ui().detailStrokesFmt, (unsigned)card.strokes);
        smallText(d, s, x, y, kWhite, kBlack);
        y += 11;
    }
    if (hasAcc && y + 11 <= bottom) {
        char s[32];
        const uint32_t total = rec->reps;
        const uint32_t correct = rec->reps > rec->lapses ? rec->reps - rec->lapses : 0;
        snprintf(s, sizeof s, ui().detailAccuracyFmt, (unsigned)correct, (unsigned)total);
        smallText(d, s, x, y, kGreen, kBlack);
    }
    present();
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

// The boot-time question has its answer (from the user or from the network
// time): a join still in progress stops, radio off, and the status line goes.
void dateSyncCancel() {
    if (gDateSync == DateSync::Joining) {
        netRadioOff();
        Serial.printf("clock: network time cancelled, answered by hand\n");
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

// Applies the stepped days (advanceDaysManual also restarts the day carry, see
// docs/srs.md section 5), persists them, rebuilds the queue (cards may now be
// due) and goes back to where the date screen was opened from: the settings
// home, or the quiz on a power-off boot.
void applyDate() {
    dateSyncCancel();
    gSrs.advanceDaysManual(gDateSteps);
    gSrs.save(gSrsIo);
    gDirty = 0;
    gQueue.rebuild(*gPack, gSrs, gRng);
    if (gAfterDate == Screen::Menu) {
        gScreen = Screen::Menu;
        drawScreen();
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

// Applies a real wall-clock time to the SRS day clock (docs/srs.md section 5.1):
// the phone's browser through the portal's POST /time, or NTP over the home
// Wi-Fi (Get decks, the boot-time date question). Anchors or advances the day,
// aligns the 04:00 rollover, remembers the UTC offset and brings the screen up
// to date: a pending power-cut date question is answered, and a new day
// rebuilds the queue. `tzOffsetMin` is local time minus UTC in minutes. False
// when the time was rejected as implausible; nothing changes then.
bool applyWallClock(int64_t utcMs, int tzOffsetMin) {
    const uint16_t before = gSrs.today();
    const tgc::ClockSyncResult r = gSrs.syncWallClock(utcMs, tzOffsetMin);
    if (r.kind == tgc::ClockSync::Rejected) {
        Serial.printf("clock: rejected (%ld s, utc%+d min)\n", static_cast<long>(utcMs / 1000),
                      tzOffsetMin);
        return false;
    }
    gWallClockSynced = true;
    if (!gPrefs.isKey(kPrefTz) || gPrefs.getShort(kPrefTz, 0) != tzOffsetMin)
        gPrefs.putShort(kPrefTz, static_cast<int16_t>(tzOffsetMin));
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
                  (unsigned)before, (unsigned)gSrs.today(), date, tzOffsetMin);

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

// The UTC offset of the last real-time sync (NVS `tz`); false before the first
// one. NTP only yields UTC: without the offset the study day is unknown, and
// guessing UTC could put it off by one, so the NTP paths skip the sync instead.
bool savedTzOffset(int16_t* out) {
    if (!gPrefs.isKey(kPrefTz)) return false;
    *out = gPrefs.getShort(kPrefTz, 0);
    return true;
}

// Boot-time date question (setup()): with a home Wi-Fi and an offset saved,
// start joining in the background; dateSyncPoll() takes it from there.
void dateSyncBegin() {
    int16_t tz = 0;
    if (!tgc::HomeWifi::hasCredentials() || !savedTzOffset(&tz)) {
        Serial.printf("clock: no network time at boot (no %s saved)\n",
                      tgc::HomeWifi::hasCredentials() ? "UTC offset" : "home Wi-Fi");
        return;
    }
    setCpuFrequencyMhz(kCpuWifiMhz);
    if (!gHome.begin()) {
        netRadioOff();
        gDateSync = DateSync::NoNetwork;
        Serial.printf("clock: no network time at boot (radio did not start)\n");
        return;
    }
    gDateSyncTz = tz;
    gDateSync = DateSync::Joining;
    gNetActivityAt = millis();  // the join keeps the idle timers away (powerTick)
}

// loop() on the date screen. Once the join succeeds: NTP (blocking, at most
// kNtpTimeoutMs), radio off, then applyWallClock(), which answers the question
// and leaves for the quiz -- true when that happened. On any failure the radio
// is off too and the status line says to set the date by hand.
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
    if (got && applyWallClock(utcMs, gDateSyncTz)) return true;
    gDateSync = joined ? DateSync::NoTime : DateSync::NoNetwork;
    // The status is news: the join's time does not count against the reader.
    gNetActivityAt = millis();
    Serial.printf("clock: no network time at boot (%s), asking by hand\n",
                  joined ? "no NTP answer" : "could not join");
    if (!gScreenOff) drawScreen();
    return false;
}

void drawScreen() {
    if (gScreen == Screen::Menu) drawMenu();
    else if (gScreen == Screen::Decks) drawDecks();
    else if (gScreen == Screen::Choice) drawChoice();
    else if (gScreen == Screen::Done) drawDone();
    else if (gScreen == Screen::Date) drawDate();
    else if (gScreen == Screen::Stats) drawStats();
    else if (gScreen == Screen::Detail) drawDetail();
    else if (gScreen == Screen::Wifi) drawWifi();
    else if (gScreen == Screen::GetDecks) drawGetDecks();
    else if (gScreen == Screen::WifiMenu) drawWifiMenu();
    else if (gScreen == Screen::NetTest) drawNetTest();
    else drawQuiz();
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
// landscape. New users get portrait.
uint8_t loadOrientPref() {
    if (gPrefs.isKey("orient")) {
        const uint8_t v = gPrefs.getUChar("orient", kOrientPortrait);
        return v <= kOrientAuto ? v : kOrientPortrait;
    }
    const uint8_t v = gPrefs.getBool("land", false) ? kOrientLandscape : kOrientPortrait;
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

// Called by the portal with the browser's clock (POST /time on every page load).
bool portalTime(int64_t utcMs, int16_t tzOffsetMin) {
    return applyWallClock(utcMs, tzOffsetMin);
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

const tgc::WifiPortal::Hooks kPortalHooks = {portalFillStats, portalPacksChanged, portalTime,
                                             wifiToGetDecks,  portalUiLang,       portalSwitchToHome,
                                             portalDeckDownload};

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
    if (gDirty) {  // entering the menu is a flush point (docs/srs.md section 4)
        gSrs.save(gSrsIo);
        gDirty = 0;
    }
    gBeforeMenu = gScreen;
    gScreen = Screen::Menu;
    gMenuSel = 0;
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
    }
    drawScreen();
}

// Opens the deck list with the loaded pack highlighted.
void openDecks() {
    gDeckSel = 0;
    for (size_t i = 0; i < gPacks.size(); ++i)
        if (gPacks[i] == gCurrentPack) gDeckSel = static_cast<int>(i);
    gScreen = Screen::Decks;
    drawScreen();
}

// Opens one of the Screen::Choice pickers (2/3/4 rows) with the saved value
// highlighted when it is one of the candidates.
void openChoice(Choice which) {
    gChoice = which;
    gChoiceSel = 0;
    if (which == Choice::Orientation) {
        // Auto first, as in the language picker; only with an accelerometer.
        gChoiceCount = 0;
        if (tgc::board::hasAccel()) gChoiceValues[gChoiceCount++] = kOrientAuto;
        gChoiceValues[gChoiceCount++] = kOrientPortrait;
        gChoiceValues[gChoiceCount++] = kOrientLandscape;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gOrientPref) gChoiceSel = i;
        gScreen = Screen::Choice;
        drawScreen();
        return;
    }
    if (which == Choice::ForgetWifi) {
        // Cancel first and selected: forgetting takes a tap and a hold.
        gChoiceCount = 2;
        gChoiceValues[0] = 0;
        gChoiceValues[1] = 1;
        gScreen = Screen::Choice;
        drawScreen();
        return;
    }
    if (which == Choice::UiLang) {
        gChoiceCount = 3;
        gChoiceValues[0] = tgc::kUiLangAuto;
        gChoiceValues[1] = tgc::kUiLangEnglish;
        gChoiceValues[2] = tgc::kUiLangSpanish;
        gChoiceSel = gUiLangPref;  // rows are in preference order
        if (gChoiceSel < 0 || gChoiceSel >= gChoiceCount) gChoiceSel = 0;
        gScreen = Screen::Choice;
        drawScreen();
        return;
    }
    gChoiceCount = 4;
    if (which == Choice::Brightness) {
        gChoiceValues[0] = 32;
        gChoiceValues[1] = 64;
        gChoiceValues[2] = 128;
        gChoiceValues[3] = 255;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gBrightness) gChoiceSel = i;
    } else if (which == Choice::NewPerDay) {
        gChoiceValues[0] = 3;
        gChoiceValues[1] = 5;
        gChoiceValues[2] = 10;
        gChoiceValues[3] = 20;
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.newPerDay()) gChoiceSel = i;
    } else {
        gChoiceValues[0] = 1500;
        gChoiceValues[1] = 3000;
        gChoiceValues[2] = 5000;
        gChoiceValues[3] = 0xFFFF;  // disables Hard
        for (int i = 0; i < gChoiceCount; ++i)
            if (gChoiceValues[i] == gSrs.slowMs()) gChoiceSel = i;
    }
    gScreen = Screen::Choice;
    drawScreen();
}

// Applies the highlighted choice, persists it and returns to the settings home.
void applyChoice() {
    if (gChoice == Choice::Orientation) {
        gOrientPref = static_cast<uint8_t>(gChoiceValues[gChoiceSel]);
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
        if (gChoiceValues[gChoiceSel] == 1) {
            tgc::HomeWifi::forgetCredentials();
            wifiMenuNote(ui().wifiForgotten, kGreen);
        }
        openWifiMenu(false);
        return;
    }
    if (gChoice == Choice::UiLang) {
        gUiLangPref = static_cast<int>(gChoiceValues[gChoiceSel]);
        gPrefs.putInt("ui", gUiLangPref);
        setUiForLang(gPack->header().lang);  // re-render in the chosen language
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    if (gChoice == Choice::Brightness) {
        gBrightness = static_cast<uint8_t>(gChoiceValues[gChoiceSel]);
        gPrefs.putUChar("bri", gBrightness);
        tgc::board::setBrightness(gBrightness);
        gDimmed = false;
        gScreen = Screen::Menu;
        drawScreen();
        return;
    }
    const uint16_t v = gChoiceValues[gChoiceSel];
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

// Activates the highlighted settings row.
void activateSetting() {
    switch (gMenuSel) {
        case 0:  // Seguir
            closeMenu();
            break;
        case 1:  // Mazos
            openDecks();
            break;
        case 2:  // Fecha
            openDate(Screen::Menu);
            break;
        case 3:  // Nuevas/dia
            openChoice(Choice::NewPerDay);
            break;
        case 4:  // Velocidad
            openChoice(Choice::SlowMs);
            break;
        case 5:  // Estadisticas
            gScreen = Screen::Stats;
            drawScreen();
            break;
        case 6:  // Wi-Fi >
            openWifiMenu(true);
            break;
        case 7:  // Orientacion
            openChoice(Choice::Orientation);
            break;
        case 8:  // Idioma / Language
            openChoice(Choice::UiLang);
            break;
        case 9:  // Brillo / Brightness
            openChoice(Choice::Brightness);
            break;
        case 10:  // Suspender / Sleep
            sleepNow(false);
            break;
    }
}

// Loads the highlighted deck (replacing the live pack only after it validates),
// resets the "study more" override and starts the quiz.
void selectPack() {
    bool loaded = false;
    gExtraNew = 0;
    if (gDeckSel >= 0 && gDeckSel < static_cast<int>(gPacks.size())) {
        const String path = gPacks[gDeckSel];
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
        if (!LittleFS.exists(path)) continue;
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        gCatalogState[i] =
            tgc::catalogDeckState(true, static_cast<uint32_t>(f.size()), gCatalog[i].bytes);
        f.close();
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
// idle timers and drops the presses made meanwhile). Only with the saved UTC
// offset (see savedTzOffset()); a failure is logged and Get decks carries on.
void netSyncClock() {
    int16_t tz = 0;
    if (!savedTzOffset(&tz)) {
        Serial.printf("clock: no NTP sync, no UTC offset saved yet (open the portal page once)\n");
        return;
    }
    int64_t utcMs = 0;
    if (gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs)) applyWallClock(utcMs, tz);
}

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
                snprintf(detail, dn, "%s", ui().netNewerCatalog);
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
    snprintf(gNetDeck, sizeof gNetDeck, "%s", e.name.c_str());
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
        netNote(ui().batteryTitle, kYellow);
        drawScreen();
        return;
    }
    const tgc::FetchResult& r = d.fetch;
    netRefreshStates();
    char buf[48];
    if (r.status == tgc::FetchStatus::Ok) {
        snprintf(buf, sizeof buf, ui().netSavedFmt, e.name.c_str());
        netNote(buf, kGreen);
    } else if (r.status == tgc::FetchStatus::NoSpace && updating) {
        // The old copy stays until the new one validated, so an update needs
        // room for both: say how to make it.
        netError(ui().netNoSpace, ui().netNoSpaceUpdate);
    } else {
        netNote(netDownloadError(r, buf, sizeof buf), kYellow);
    }
    drawScreen();
}

// Loads a deck that is already on the device and starts studying it.
void netStudy(int i) {
    const String path = catalogPath(gCatalog[i]);
    if (!loadPack(path)) {
        netNote(ui().netInvalid, kYellow);
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
        gWifiMenuSel = 0;
        gWifiMenuNote[0] = 0;
    }
    gWifiMenuSsid = tgc::HomeWifi::savedSsid();
    gScreen = Screen::WifiMenu;
    drawScreen();
}

void activateWifiMenu() {
    switch (gWifiMenuSel) {
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
                wifiMenuNote(ui().wifiNoNetwork, kYellow);
                drawScreen();
            } else {
                openChoice(Choice::ForgetWifi);
            }
            break;
    }
}

void wifiMenuLoop(int a, int b) {
    if (a == 1) {
        gWifiMenuSel = (gWifiMenuSel + 1) % kWifiMenuRows;
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
    int16_t tz = 0;
    if (savedTzOffset(&tz)) {
        int64_t utcMs = 0;
        gTestTime = gHome.fetchUtcMs(&utcMs, kNtpTimeoutMs) ? 1 : 0;
        if (gTestTime) applyWallClock(utcMs, tz);
    }
    const String url = tgc::HomeWifi::catalogUrl();
    const tgc::CatalogFetch c = tgc::fetchCatalog(url);
    char title[48], detail[96];
    if (catalogFailure(c, url, title, sizeof title, detail, sizeof detail)) {
        // A 404 says more as "no catalog published at this URL yet".
        const bool notPublished =
            c.fetch.status == tgc::FetchStatus::Http && c.fetch.httpCode == 404;
        snprintf(gTestCatalog, sizeof gTestCatalog, "%s", notPublished ? ui().netNotPublished : title);
        gTestCatalogColor = kYellow;
    } else {
        snprintf(gTestCatalog, sizeof gTestCatalog, ui().testCatalogFmt,
                 (unsigned)c.parse.entries.size());
        gTestCatalogColor = kGreen;
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

// Full-screen notice: `title` in the 2x built-in font, `hint` below in the 1x
// one. Built-in fonts only, so it works before a pack is open.
void showNotice(const char* title, const char* hint, uint16_t titleColor) {
    auto& d = *gCanvas;
    d.fillScreen(kBlack);
    const int mid = H / 2;
    gRender.drawCentred(d, title, 4, mid - 44, W - 8, 40, 2, titleColor, kBlack,
                        tgc::Renderer::kTextBuiltin);
    gRender.drawCentred(d, hint, 4, mid + 2, W - 8, 40, 4, kWhite, kBlack,
                        tgc::Renderer::kTextBuiltinSmall);
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
        showNotice(ui().batteryTitle, ui().batteryHint, kYellow);
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
    if (gHome.active() || gDateSync != DateSync::None) {
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
    showError(ui().errNoFilesystem, ui().errNoFilesystemHint);
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

}  // namespace

void setup() {
    Serial.begin(115200);
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
    gPrefs.begin("tangocho", false);
    gUiLangPref = gPrefs.getInt("ui", tgc::kUiLangAuto);
    gBrightness = gPrefs.getUChar("bri", gBrightness);
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
            showNotice(ui().batteryTitle, ui().batteryHint, kYellow);
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
        if (LittleFS.remove(tmp)) Serial.printf("fs: removed stale %s\n", tmp);
    listPacks(gPacks);

    // No packs yet: do not halt. Start the Wi-Fi portal so a pack can be
    // uploaded from a phone (or downloaded from a URL); loop() moves on to the
    // quiz as soon as one arrives. This is the practical answer to a fresh (or
    // launcher-formatted) filesystem, including one mountFilesystem() has just
    // formatted. An AP that will not start leaves nothing to do, so that keeps
    // the old error screen.
    if (gPacks.empty()) {
        gSrsLoaded = gSrs.begin(gSrsIo);
        gBootDay = gSrs.today();
        gSrs.beginSession();
        applySleepTime();
        // With a home Wi-Fi saved, the page over it (setup mode if the join
        // fails, see wifiLoop()); without one, the device's own access point.
        gWifiBootUpload = true;
        openPortal(defaultPortalMode());
        if (gPortalMode == PortalMode::Setup && gWifiStep != WifiStep::Serving) {
            showError(ui().errNoPack, ui().errNoPackHint);
            halt();
        }
        Serial.printf("wifi: no packs, upload-ready (%s %s)\n",
                      gPortalMode == PortalMode::Home ? "joining" : "ssid",
                      gPortalMode == PortalMode::Home ? gHome.ssid() : gPortal.ssid());
        return;
    }

    String want = gPrefs.getString("pack", "");
    if (std::find(gPacks.begin(), gPacks.end(), want) == gPacks.end()) want = gPacks[0];
    if (!loadPack(want)) {
        showError(ui().errOpenPack, want.c_str());
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
    uint32_t due = 0, fresh = 0;
    packDueNew(&due, &fresh);

    char today[16];
    formatDate(gSrs.today(), today, sizeof today);
    Serial.printf("srs: day %u (%s), records %u, due %u, new %u%s, powered_off=%d\n",
                  (unsigned)gSrs.today(), today, (unsigned)gSrs.count(), (unsigned)due,
                  (unsigned)fresh, srsLoaded ? "" : " (new)", gPoweredOff ? 1 : 0);

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

// One button drives everything: a tap cycles the highlight (or, once revealed,
// moves to the next card); a 600 ms hold confirms the highlighted option. The
// tap action runs on release, and a hold suppresses it, so a long press is never
// also a tap.

// The input after the answer has been revealed: a correct answer advances as
// before; a wrong one opens the detail card first, and the next input advances.
void afterReveal() {
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

void tapAction() {
    if (gRevealed) {
        afterReveal();
        return;
    }
    tgc::Card card;
    if (!questionCard(&card)) return;  // also keeps gQ.count from being 0 below
    gSel = (gSel + 1) % gQ.count;
    ++gNavTaps;
    drawScreen();
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
    QLOG("A sel=%d correct=%d %s grade=%s rt=%ums S=%.2f D=%.1f I=%d\n", gSel, gQ.correct,
         correct ? "OK" : "MISS", gradeName(grade), (unsigned)rt_ms,
         r ? r->stabilityCds / 100.0f : 0.0f, r ? r->difficulty10 / 10.0f : 0.0f, interval);
    drawScreen();
}

void loop() {
    tgc::board::update();
    const uint32_t t = millis();
    gSrs.tick(t);
    static Key keyA, keyB;
    const bool downA = tgc::board::isHeld(Button::Select);
    const bool downB = tgc::board::isHeld(Button::Next);
    int a = keyA.poll(downA, t, tgc::board::kHoldMs);
    int b = keyB.poll(downB, t, tgc::board::kHoldMs);
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
    if (gScreen == Screen::Menu) {
        if (a == 1) {
            gMenuSel = (gMenuSel + 1) % kSettingRows;
            drawScreen();
        } else if (a == 2) {
            activateSetting();
        }
        if (b) closeMenu();  // side button backs out to the previous screen
    } else if (gScreen == Screen::Decks) {
        if (a == 1) {
            gDeckSel = (gDeckSel + 1) % (static_cast<int>(gPacks.size()) + 1);  // + "Get decks"
            drawScreen();
        } else if (a == 2) {
            if (gDeckSel == static_cast<int>(gPacks.size()))
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
            gChoiceSel = (gChoiceSel + 1) % gChoiceCount;
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
        // Tap: one more day. Hold: confirm. Side hold: one day back, never below
        // the current day (undoes an overshoot). Side tap: cancel. Confirming or
        // cancelling also stops the network-time attempt (see leaveDate()).
        if (dateSyncPoll()) {
            // The network time answered the boot-time question: on the quiz now.
        } else if (a == 1) {
            if (dateTarget() < 0xFFFF) ++gDateSteps;
            drawScreen();
        } else if (a == 2) {
            applyDate();
        } else if (b == 2) {
            if (gDateSteps > 0) {
                --gDateSteps;
                drawScreen();
            }
        } else if (b == 1) {  // cancel: back to the menu, or straight into the quiz
            if (gAfterDate == Screen::Menu) {
                gScreen = Screen::Menu;
                drawScreen();
            } else {
                leaveDate();
            }
        }
    } else if (gScreen == Screen::Stats) {
        if (b) {  // side button returns to the settings home
            gScreen = Screen::Menu;
            drawScreen();
        }
    } else if (gScreen == Screen::Detail) {
        // Animate the stroke order while idle: one stroke every ~550 ms, a
        // ~1.2 s hold on the last one, then loop. Redraw only on a new frame.
        if (gAnimOn && static_cast<int32_t>(t - gAnimNext) >= 0) {
            const int count = static_cast<int>(gAnimStrokes);
            gAnimFrame = (gAnimFrame + 1) % count;
            gAnimNext = t + (gAnimFrame == count - 1 ? kStrokePauseMs : kStrokeMs);
            drawDetail();
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
        } else if (a || b == 1) {
            // Nothing due and the daily new cap is spent: study 10 more today.
            gExtraNew = static_cast<uint16_t>(extraNewToday() + 10);
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
    } else {
        if (a == 1) tapAction();
        else if (a == 2) holdAction(keyA.downAt);
        if (b == 1) tapAction();  // side button shortcut
        else if (b == 2) openMenu();
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
