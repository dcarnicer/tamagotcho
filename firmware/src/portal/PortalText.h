#pragma once
// Every string of the Wi-Fi portal's web page, in English (the member defaults)
// and Spanish (PortalText.cpp overrides them by name, so a string missing there
// falls back to English instead of breaking the page). Pure data, no Arduino.
//
// Conventions: "%s" / "%u" / "%d" are printf-style. A "%s" that the page fills
// with a network or deck name gets the HTML-escaped value, so templates may
// carry markup such as <b>; plain text strings never do.
namespace tgc {

struct PortalText {
    const char* htmlLang = "en";

    // Header status line.
    const char* decksNone = "No decks on the device yet";
    const char* decksOne = "1 deck on the device";
    const char* decksFmt = "%u decks on the device";
    const char* freeFmt = "%s free";
    const char* viaFmt = "via <b>%s</b>";
    const char* ownNetFmt = "on the device's own network <b>%s</b>";
    const char* clockHint = "Opening this page sets the device date and time zone from this browser.";
    const char* clockOk = "Device date set from this browser: ";
    const char* clockBad = "The device did not accept this browser's clock.";
    const char* clockErr = "Could not send the clock to the device.";

    // First setup (setup mode, no home Wi-Fi saved).
    const char* step1Title = "1. Connect the device to your Wi-Fi";
    const char* step1Intro =
        "The device downloads decks over your home network. Pick it and type its password.";
    const char* ssidPlaceholder = "network name";
    const char* passPlaceholder = "password";
    const char* save = "Save";
    const char* passNote =
        "Leave the password empty for an open network. It stays on the device (stored "
        "unencrypted) and is never shown again.";
    const char* step2Title = "2. Get decks";
    const char* step2Text =
        "Once the network is saved, switch the device to it: this page then works from your "
        "usual Wi-Fi and lists the decks to download.";
    const char* orUpload = "No Wi-Fi at hand? Upload a deck file under Advanced.";

    // "Get decks" in setup mode (home Wi-Fi saved).
    const char* getDecksTitle = "Get decks";
    const char* wifiSavedOk = "Saved.";
    const char* yourWifiFmt = "Your Wi-Fi: <b>%s</b>";
    const char* switchButton = "Switch to my Wi-Fi";
    const char* switchNoteFmt =
        "The device leaves this network and joins <b>%s</b>. Put the phone back on that Wi-Fi "
        "and open the address the device shows (or scan its QR code): this page is there, "
        "with the decks to download.";
    const char* deviceGetDecksButton = "Get decks on the device screen";
    const char* deviceGetDecksNote =
        "The device leaves this network and lists the decks on its own screen: a tap moves, a "
        "hold downloads.";

    // "Get decks" in home mode: the catalog.
    const char* catalogLoading = "Reading the deck catalog...";
    const char* catalogEmpty = "No decks in the catalog.";
    const char* retry = "Try again";
    const char* netServer = "Cannot reach the server.";
    const char* netTls = "Secure connection failed.";
    const char* netHttpFmt = "The server answered %d.";
    const char* catalogNotPublished = "No catalog is published at this URL yet.";
    const char* catalogBad = "This is not a deck catalog.";
    const char* catalogNewer = "Newer catalog format: update the firmware.";
    const char* catalogFailed = "Could not read the catalog.";
    const char* deckInfoFmt = "%s &middot; %u cards &middot; %s";
    const char* download = "Download";
    const char* update = "Update";
    const char* onDevice = "On the device";
    const char* studying = "studying";
    const char* deckNote =
        "Downloads and updates keep your progress (it belongs to each kanji, not to the deck). "
        "An update needs room for both copies for a moment.";

    // A deck download from the page.
    const char* dlStartingFmt = "Starting the download of <b>%s</b>...";
    const char* dlProgressFmt = "Downloading <b>%s</b>: %u%%";
    const char* dlBusyNote =
        "This page refreshes by itself. The device shows it too; its side button cancels.";
    const char* dlSavedFmt = "Saved <b>%s</b>.";
    const char* dlNoSpace = "Not enough space.";
    const char* dlNoSpaceUpdate =
        "Not enough space: an update needs room for the old and the new copy. Delete the old "
        "one under Your decks first.";
    const char* dlCorrupt = "The download was corrupt (size or checksum); nothing was changed.";
    const char* dlInvalid = "Not a valid deck; nothing was changed.";
    const char* dlCancelled = "Cancelled on the device.";
    const char* dlBattery = "Battery low: charge the device, then try again.";
    const char* dlFailed = "Download failed; nothing was changed.";
    const char* busy = "The device is busy with a download; try again when it is done.";
    const char* unknownDeck = "That deck is not in the catalog.";

    // Your decks.
    const char* yoursTitle = "Your decks";
    const char* yoursEmpty = "No decks yet.";
    const char* deleteButton = "Delete";
    const char* deleteConfirm = "Delete this deck? Your progress is kept.";
    const char* deletedFmt = "Deleted <b>%s</b>.";
    const char* deleteFailed = "Could not delete it.";
    const char* badName = "Bad deck name.";

    // Advanced.
    const char* advanced = "Advanced";
    const char* uploadTitle = "Upload a deck file (.tgc)";
    const char* uploadButton = "Upload";
    const char* uploadOk = "Deck received.";
    const char* uploadRejected = "Rejected:";
    const char* upBadName = "bad file name (use a short name.tgc)";
    const char* upTooLarge = "deck too large";
    const char* upNoSpace = "not enough space";
    const char* upCannotCreate = "cannot create the file";
    const char* upWriteFailed = "write failed";
    const char* upInvalid = "not a valid .tgc deck (discarded)";
    const char* upCannotStore = "cannot store the deck";
    const char* upAborted = "upload aborted";
    const char* urlTitle = "Download from a URL";
    const char* urlNoteSetup =
        "Plain http:// only: on its own network the device has no internet, so this is for a "
        "server on your phone or computer.";
    const char* urlNoteHome =
        "Plain http:// only, e.g. a server on your computer. For the published decks use Get "
        "decks above.";
    const char* urlBad = "Only plain http:// URLs ending in a .tgc name are supported.";
    const char* urlStartedFmt =
        "Download started into <b>%s</b>. Watch the device screen; this page lists the deck "
        "when it is done.";
    const char* catalogTitle = "Deck catalog";
    const char* catalogPlaceholder = "default catalog";
    const char* catalogDefaultFmt = "Empty = the Tangochō decks release: %s";
    const char* catalogUrlBad =
        "Not saved: use an http:// or https:// URL of at most 256 characters, or leave it empty "
        "for the default.";
    const char* catalogUrlSaved = "Catalog URL saved.";
    const char* wifiTitle = "Home Wi-Fi";
    const char* savedNetFmt = "Saved network: <b>%s</b>";
    const char* noHomeWifi = "No home network saved.";
    const char* forget = "Forget";
    const char* forgetConfirm = "Forget this network?";
    const char* changeNoteHome =
        "Another network saved here is used after Reconnect, or the next time the portal opens.";
    const char* reconnect = "Reconnect";
    const char* wifiBad =
        "Not saved: the network name must be 1-32 characters and the password empty (open "
        "network), 8-63 characters or 64 hex digits.";
    const char* forgottenHome =
        "Forgotten. This session carries on; next time the portal opens in setup mode.";
    const char* forgotten = "Forgotten.";
    const char* statsTitle = "Statistics";
    const char* statDate = "date";
    const char* statEstimated = "(estimated: not set from a clock yet)";
    const char* statDay = "day";
    const char* statStreak = "streak";
    const char* statTime = "time on questions";
    const char* statSeen = "seen";
    const char* statCards = "cards";
    const char* statAnswers = "answers";
    const char* statAccuracy = "accuracy";
    const char* statLearned = "learned";
    const char* freeSpaceFmt = "Free space: %s of %s";
    const char* securitySetup =
        "This network is <b>open</b>: anyone in range can list, upload or delete decks and read "
        "these statistics while the Wi-Fi screen is open on the device. Leave it (side button) "
        "when you are done.";
    const char* securityHomeFmt =
        "This page has no password: anyone on <b>%s</b> can list, upload or delete decks and "
        "read these statistics while the Wi-Fi screen is open on the device. Leave it (side "
        "button) when you are done.";
    const char* language = "Language:";

    // Pages shown when the device leaves this network.
    const char* back = "Back";
    const char* notFound = "Not found.";
    const char* noHomeSaved = "No home Wi-Fi saved yet: save your network on the page first.";
    const char* leaveGetDecksTitle = "Getting decks over your home Wi-Fi";
    const char* leaveGetDecksFmt =
        "The device is leaving this Wi-Fi (<b>%s</b>) to download decks over <b>%s</b>, so this "
        "page stops answering now.";
    const char* leavePhoneFmt =
        "You can disconnect the phone from <b>%s</b>: it goes back to its own network (or pick "
        "that one again in its Wi-Fi settings).";
    const char* leaveFollowFmt =
        "<b>Follow the device screen.</b> It joins <b>%s</b> and lists the decks: a tap moves, a "
        "hold downloads, the side button goes back.";
    const char* leaveAgain =
        "To use this page again, open <b>Settings &gt; Wi-Fi &gt; Web portal</b> on the device.";
    const char* switchTitle = "Switching to your Wi-Fi";
    const char* switchLeaveFmt =
        "The device is leaving its own network (<b>%s</b>) to join <b>%s</b>, so this page stops "
        "answering here.";
    const char* switchPhoneFmt =
        "Put the phone back on <b>%s</b> (it often does that by itself), then open the address "
        "the device shows, or scan its QR code.";
    const char* switchMdnsFmt = "It is usually also at <a href=\"%s\">%s</a>.";
    const char* switchFail =
        "If the device cannot join, its screen says why; a hold of its front button brings back "
        "setup mode.";
    const char* reconnectTitle = "Reconnecting";
    const char* reconnectFmt =
        "The device leaves the network and joins <b>%s</b>, so this page stops answering for a "
        "moment. Then open the address the device shows (it may change).";

    // Page footer: the version and the credits Settings > About shows on the
    // device. The names and licences come from Credits.h.
    const char* footVersionFmt = "Tangochō %s &middot; MIT-licensed firmware";  // %s = version
    const char* footDecksFmt =  // kanji data, stroke order, JLPT levels, glyphs
        "Decks: CC BY-SA. Kanji data: %s; stroke order: %s; JLPT levels: %s; glyphs: %s.";
};

// The table for a page language: "es" selects Spanish, anything else English.
const PortalText& portalText(const char* lang);

}  // namespace tgc
