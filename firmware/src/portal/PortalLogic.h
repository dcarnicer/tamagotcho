#pragma once
// Small decisions behind the Wi-Fi portal and its device screen that need no
// radio: the Wi-Fi QR payload, the QR size, the page language and the signal
// quality words. Pure C++, no Arduino: unit tested natively in test/test_portal.
// The portal itself is platform/WifiPortal (Arduino only).
#include <cstddef>
#include <string>

namespace tgc {

// Escapes the characters the de-facto "WIFI:" QR format (ZXing's) reserves --
// backslash, ';', ',', ':' and '"' -- with a backslash.
std::string wifiQrEscape(const std::string& s);

// The QR text that joins the open network `ssid`: "WIFI:T:nopass;S:<ssid>;;"
// with the SSID escaped as above. Phone cameras offer to join it.
std::string wifiQrOpenNetwork(const std::string& ssid);

// The smallest QR version that holds `len` bytes in byte mode at error
// correction level L (the level M5GFX/LovyanGFX's qrcode() uses), 1..10; 0 when
// even version 10 (271 bytes) is too small. The device's QR texts are a few
// dozen bytes, so larger versions are never needed (and would not be legible on
// a 135 px screen).
int qrVersionForBytes(size_t len);

// Modules per side of a QR code of `version` (21 for version 1).
inline int qrModules(int version) { return 17 + 4 * version; }

// The language tag an HTTP Accept-Language header prefers most: the entry with
// the highest q (the first one on a tie), e.g. "es-ES" for
// "es-ES,es;q=0.9,en;q=0.8". "*", entries with q=0 and malformed tags are
// skipped; "" when nothing usable is left. Tags keep only letters, digits and
// '-' and are at most 15 characters.
std::string preferredLanguage(const char* acceptLanguage);

// The portal page's language, "en" or "es". The device's own UI language wins
// when it has one (`deviceLang` "en"/"es"; "" when the setting is Auto and no
// deck is loaded, so the device has no language of its own yet). Otherwise a
// `?lang=` hint from the page ("en", "es-MX", ...), then the first English or
// Spanish entry of the browser's Accept-Language by q, then English.
const char* portalPageLang(const char* deviceLang, const char* hint, const char* acceptLanguage);

// The language the page's deck list puts first (orderCatalogForLang): the
// device's UI language when it has one, else the page's hint, else the
// browser's preferred language whatever it is ("fr-FR" lists French decks first
// even though the page itself is in English). "" when none is known.
std::string catalogOrderLang(const char* deviceLang, const char* hint, const char* acceptLanguage);

// Signal quality from an RSSI in dBm: 0 weak (< -75), 1 fair (-75..-68),
// 2 good (-67..-56), 3 excellent (>= -55). Rough bands, for one word on the
// device's network test screen.
int rssiQuality(int dBm);

}  // namespace tgc
