// Native tests for the Wi-Fi portal's pure helpers (src/portal): the Wi-Fi QR
// payload, the QR version, the page language and the deck-list language.
#include <unity.h>

#include <cstring>
#include <string>

#include "portal/PortalLogic.h"
#include "portal/PortalText.h"

using namespace tgc;

void setUp() {}
void tearDown() {}

void test_wifi_qr_escape() {
    TEST_ASSERT_EQUAL_STRING("Tangocho-A1B2", wifiQrEscape("Tangocho-A1B2").c_str());
    TEST_ASSERT_EQUAL_STRING("", wifiQrEscape("").c_str());
    // Every reserved character gets one backslash, in place.
    TEST_ASSERT_EQUAL_STRING("a\\;b\\,c\\:d\\\"e\\\\f", wifiQrEscape("a;b,c:d\"e\\f").c_str());
    TEST_ASSERT_EQUAL_STRING("\\;\\;", wifiQrEscape(";;").c_str());
    // Spaces, UTF-8 and other punctuation pass through unchanged.
    TEST_ASSERT_EQUAL_STRING("Café Wi-Fi 5G!", wifiQrEscape("Café Wi-Fi 5G!").c_str());
}

void test_wifi_qr_payload() {
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:Tangocho-A1B2;;",
                             wifiQrOpenNetwork("Tangocho-A1B2").c_str());
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:my\\;net\\:5;;", wifiQrOpenNetwork("my;net:5").c_str());
}

void test_qr_version() {
    TEST_ASSERT_EQUAL(1, qrVersionForBytes(0));
    TEST_ASSERT_EQUAL(1, qrVersionForBytes(17));
    TEST_ASSERT_EQUAL(2, qrVersionForBytes(18));
    // The device's texts: the AP join string and the URLs.
    TEST_ASSERT_EQUAL(2, qrVersionForBytes(std::string("WIFI:T:nopass;S:Tangocho-A1B2;;").size()));
    TEST_ASSERT_EQUAL(2, qrVersionForBytes(std::string("http://192.168.4.1/").size()));
    TEST_ASSERT_EQUAL(2, qrVersionForBytes(std::string("http://192.168.100.200/").size()));
    TEST_ASSERT_EQUAL(3, qrVersionForBytes(33));
    TEST_ASSERT_EQUAL(10, qrVersionForBytes(271));
    TEST_ASSERT_EQUAL(0, qrVersionForBytes(272));
    TEST_ASSERT_EQUAL(25, qrModules(2));
    TEST_ASSERT_EQUAL(21, qrModules(1));
}

void test_preferred_language() {
    TEST_ASSERT_EQUAL_STRING("es-ES", preferredLanguage("es-ES,es;q=0.9,en;q=0.8").c_str());
    TEST_ASSERT_EQUAL_STRING("en", preferredLanguage("fr;q=0.5, en;q=0.9").c_str());
    // q defaults to 1; the first entry wins a tie.
    TEST_ASSERT_EQUAL_STRING("de", preferredLanguage("fr;q=0.5, en;q=0.9 ,de").c_str());
    TEST_ASSERT_EQUAL_STRING("fr", preferredLanguage("fr, en").c_str());
    // "*", q=0 and malformed entries are skipped.
    TEST_ASSERT_EQUAL_STRING("en", preferredLanguage("*, es;q=0, en;q=0.1").c_str());
    TEST_ASSERT_EQUAL_STRING("en", preferredLanguage("es;q=2, en;q=0.2").c_str());
    TEST_ASSERT_EQUAL_STRING("en", preferredLanguage("e<s, en;q=0.3").c_str());
    TEST_ASSERT_EQUAL_STRING("", preferredLanguage("").c_str());
    TEST_ASSERT_EQUAL_STRING("", preferredLanguage(nullptr).c_str());
    TEST_ASSERT_EQUAL_STRING("", preferredLanguage("*").c_str());
    TEST_ASSERT_EQUAL_STRING("", preferredLanguage(",,;q=1").c_str());
    // Over-long tags are refused rather than cut.
    TEST_ASSERT_EQUAL_STRING("en", preferredLanguage("x-abcdefghijklmnop, en;q=0.5").c_str());
    // q with three decimals, upper-case Q and extra parameters.
    TEST_ASSERT_EQUAL_STRING("pt", preferredLanguage("en;q=0.001, pt;Q=0.002;x=y").c_str());
}

void test_page_lang() {
    // The device's own language wins.
    TEST_ASSERT_EQUAL_STRING("es", portalPageLang("es", "en", "en-US"));
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang("en", "es", "es-ES"));
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang("xx", nullptr, "es"));
    // No device language: the hint, then the browser.
    TEST_ASSERT_EQUAL_STRING("es", portalPageLang("", "es-MX", "en"));
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang("", "EN", "es"));
    TEST_ASSERT_EQUAL_STRING("es", portalPageLang("", "fr", "fr-FR,es;q=0.7,en;q=0.5"));
    TEST_ASSERT_EQUAL_STRING("es", portalPageLang(nullptr, nullptr, "es-419"));
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang("", "", "de-DE,en-GB;q=0.8,es;q=0.6"));
    // "est" (Estonian) only starts like "es"; nothing usable -> English.
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang("", "", "est, de"));
    TEST_ASSERT_EQUAL_STRING("en", portalPageLang(nullptr, nullptr, nullptr));
}

void test_catalog_order_lang() {
    TEST_ASSERT_EQUAL_STRING("es", catalogOrderLang("es", "en", "fr").c_str());
    TEST_ASSERT_EQUAL_STRING("en", catalogOrderLang("", "en", "fr").c_str());
    TEST_ASSERT_EQUAL_STRING("fr-FR", catalogOrderLang("", "", "fr-FR,es;q=0.5").c_str());
    TEST_ASSERT_EQUAL_STRING("", catalogOrderLang(nullptr, nullptr, nullptr).c_str());
}

void test_rssi_quality() {
    TEST_ASSERT_EQUAL(3, rssiQuality(-40));
    TEST_ASSERT_EQUAL(3, rssiQuality(-55));
    TEST_ASSERT_EQUAL(2, rssiQuality(-56));
    TEST_ASSERT_EQUAL(2, rssiQuality(-67));
    TEST_ASSERT_EQUAL(1, rssiQuality(-68));
    TEST_ASSERT_EQUAL(1, rssiQuality(-75));
    TEST_ASSERT_EQUAL(0, rssiQuality(-76));
    TEST_ASSERT_EQUAL(0, rssiQuality(-100));
}

// Every page string exists in both tables and the Spanish one translates all
// of them (a string left out of makeSpanish() would show English on a Spanish
// page). The table is a plain struct of string pointers, so walk it as one.
void test_page_text_complete() {
    static_assert(sizeof(PortalText) % sizeof(const char*) == 0, "PortalText holds only strings");
    constexpr size_t n = sizeof(PortalText) / sizeof(const char*);
    const PortalText& en = portalText("en");
    const PortalText& es = portalText("es");
    TEST_ASSERT_TRUE(&en != &es);
    TEST_ASSERT_TRUE(&portalText(nullptr) == &en);
    TEST_ASSERT_TRUE(&portalText("fr") == &en);
    const char* const* a = reinterpret_cast<const char* const*>(&en);
    const char* const* b = reinterpret_cast<const char* const*>(&es);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_NOT_NULL(a[i]);
        TEST_ASSERT_NOT_NULL(b[i]);
        if (std::strcmp(a[i], b[i]) == 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "untranslated string %u: \"%s\"", (unsigned)i, a[i]);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_ASSERT_EQUAL_STRING("es", es.htmlLang);
    TEST_ASSERT_EQUAL_STRING("Guardar", es.save);
}

static int countConversions(const char* f) {
    int n = 0;
    for (const char* p = std::strchr(f, '%'); p; p = std::strchr(p + 2, '%')) {
        if (p[1] == '%') continue;
        ++n;
        if (p[1] == '\0') break;
    }
    return n;
}

// The footer's templates take exactly the arguments pageFooter() passes (the
// version; the six credits), in every language: a missing or extra "%s"
// would read garbage.
void test_page_footer_formats() {
    for (const char* lang : {"en", "es"}) {
        const PortalText& t = portalText(lang);
        TEST_ASSERT_EQUAL(1, countConversions(t.footVersionFmt));
        TEST_ASSERT_NOT_NULL(std::strstr(t.footVersionFmt, "%s"));
        TEST_ASSERT_EQUAL(6, countConversions(t.footDecksFmt));
        int s = 0;
        for (const char* p = std::strstr(t.footDecksFmt, "%s"); p; p = std::strstr(p + 2, "%s")) ++s;
        TEST_ASSERT_EQUAL(6, s);
    }
}

// The delete notices take what handleDelete()'s notice passes: the deck name,
// then (once its progress went) the number of kanji forgotten, in that order.
void test_page_delete_formats() {
    for (const char* lang : {"en", "es"}) {
        const PortalText& t = portalText(lang);
        TEST_ASSERT_EQUAL(1, countConversions(t.deletedFmt));
        TEST_ASSERT_NOT_NULL(std::strstr(t.deletedFmt, "%s"));
        TEST_ASSERT_EQUAL(2, countConversions(t.deletedProgressFmt));
        const char* name = std::strstr(t.deletedProgressFmt, "%s");
        const char* count = std::strstr(t.deletedProgressFmt, "%u");
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_NOT_NULL(count);
        TEST_ASSERT_TRUE(name < count);
        TEST_ASSERT_EQUAL(0, countConversions(t.deleteConfirm));  // a plain JS confirm()
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_wifi_qr_escape);
    RUN_TEST(test_wifi_qr_payload);
    RUN_TEST(test_qr_version);
    RUN_TEST(test_preferred_language);
    RUN_TEST(test_page_lang);
    RUN_TEST(test_catalog_order_lang);
    RUN_TEST(test_rssi_quality);
    RUN_TEST(test_page_text_complete);
    RUN_TEST(test_page_footer_formats);
    RUN_TEST(test_page_delete_formats);
    return UNITY_END();
}
