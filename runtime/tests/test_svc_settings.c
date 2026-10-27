/* Unit tests: set / set:sys values derived from the host (region, colour set, time zone) and the language tables. */
#include "../src/services/settings.c"
#include "svc_test.h"

static uint64_t code(const char *s) { uint64_t v = 0; memcpy(&v, s, strlen(s)); return v; }
#ifdef __linux__
/* Linux harness: host_info.c (CoreFoundation) is not linked, so stand in for it. */
static uint64_t g_lang;
uint64_t host_desired_language(void) { return g_lang; }
#define SET_LANG(s) (g_lang = code(s))
#else
/* Mac: the real host_info.o is linked; NAPPLE_LANG is its explicit override. */
#define SET_LANG(s) setenv("NAPPLE_LANG", (s), 1)
#endif

int main(void) {
    CHECK(settings_region(code("ja"), NULL) == SET_REGION_JPN, "ja is the Japan region");
    CHECK(settings_region(code("en-US"), NULL) == SET_REGION_USA && settings_region(code("es-419"), NULL) == SET_REGION_USA, "Americas");
    CHECK(settings_region(code("de"), NULL) == SET_REGION_EUR && settings_region(code("en-GB"), NULL) == SET_REGION_EUR, "Europe");
    CHECK(settings_region(code("ko"), NULL) == SET_REGION_HTK && settings_region(code("zh-TW"), NULL) == SET_REGION_HTK, "Hong Kong, Taiwan, Korea");
    CHECK(settings_region(code("zh-CN"), NULL) == SET_REGION_CHN, "China");
    CHECK(settings_region(code("xx"), NULL) == SET_REGION_EUR, "unknown language falls back to Europe");
    CHECK(settings_region(code("de"), "aus") == SET_REGION_AUS && settings_region(code("de"), "AUS") == SET_REGION_AUS && settings_region(code("de"), "1") == SET_REGION_USA, "override by name or digit");
    CHECK(settings_region(code("ja"), "bogus") == SET_REGION_JPN && settings_region(code("ja"), "9") == SET_REGION_JPN, "invalid override ignored");

    char tz[SET_TZ_NAME_SIZE];
    settings_timezone_name(NULL, tz); CHECK(!strcmp(tz, "UTC"), "default UTC");
    settings_timezone_name("Europe/Berlin", tz); CHECK(!strcmp(tz, "Europe/Berlin") && tz[SET_TZ_NAME_SIZE - 1] == 0, "plausible name kept");
    settings_timezone_name("../etc/passwd", tz); CHECK(!strcmp(tz, "UTC"), "traversal rejected");
    settings_timezone_name("America/Argentina/Buenos_Aires_extra_long_name_xx", tz); CHECK(!strcmp(tz, "UTC"), "name too long for the 0x24 buffer");
    settings_timezone_name("Bad Name", tz); CHECK(!strcmp(tz, "UTC"), "space rejected");
    CHECK(settings_color_set(NULL) == SET_COLOR_BLACK && settings_color_set("white") == SET_COLOR_WHITE && settings_color_set("0") == SET_COLOR_WHITE && settings_color_set("x") == SET_COLOR_BLACK, "colour set");

    IpcObject *set = set_open(), *sys = sys_open(); IpcCall c;
    SET_LANG("fr");
    svc_call(set, 4, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_REGION_EUR, "GetRegionCode follows the language");
    SET_LANG("ja"); svc_call(set, 4, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_REGION_JPN, "GetRegionCode ja");
    svc_call(set, 8, NULL, 0, &c); CHECK(c.out[0] == 0, "not a quest unit");
    svc_call(set, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == code("ja"), "GetLanguageCode");
    svc_call(set, 6, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 18, "18 language codes");
    svc_call(sys, 23, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_COLOR_BLACK, "colour set id");
    svc_call(sys, 62, NULL, 0, &c); CHECK(c.out_size == SET_TZ_NAME_SIZE && !strcmp((const char *)c.out, "UTC"), "time zone name");
    uint8_t fw[0x100]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, fw, sizeof fw); svc_call_c(sys, 3, &c);
    CHECK(c.result == 0 && fw[0] == 18 && !strcmp((char *)fw + 0x68, "18.1.0"), "firmware version 18.1.0");
    uint8_t small[0x10]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, small, sizeof small); svc_call_c(sys, 3, &c); CHECK(c.result != 0, "buffer too small");
    { /* key code maps: the US layout, 16 bytes per HID usage */
        uint8_t km[SET_KEYMAP_SIZE]; unsigned ids[3] = {7, 9, 11};
        for (int i = 0; i < 3; i++) {
            memset(km, 0xcc, sizeof km); memset(&c, 0, sizeof c); svc_set_out_buf(&c, km, sizeof km); svc_call_c(set, ids[i], &c);
            const uint8_t *a = km + 0x04 * SET_KEYMAP_ENTRY, *one = km + 0x1e * SET_KEYMAP_ENTRY, *zero = km + 0x27 * SET_KEYMAP_ENTRY, *none = km + 0x01 * SET_KEYMAP_ENTRY;
            CHECK(c.result == 0 && a[0] == 'a' && a[2] == 'A' && a[8] == 1 && one[0] == '1' && one[2] == '!' && zero[0] == '0' && zero[2] == ')' && none[0] == 0 && none[8] == 0, "GetKeyCodeMap %u: letters, digits and shifted forms", ids[i]);
        }
        const uint8_t *sp = km + 0x2c * SET_KEYMAP_ENTRY, *en = km + 0x28 * SET_KEYMAP_ENTRY, *sl = km + 0x38 * SET_KEYMAP_ENTRY;
        CHECK(sp[0] == ' ' && en[0] == '\n' && sl[0] == '/' && sl[2] == '?', "space, enter and punctuation");
        uint8_t part[0x30]; memset(part, 0xcc, sizeof part); memset(&c, 0, sizeof c); svc_set_out_buf(&c, part, sizeof part); svc_call_c(set, 7, &c);
        CHECK(c.result == 0 && !memcmp(part, km, sizeof part), "a short buffer gets a prefix");
        uint8_t big[SET_KEYMAP_SIZE + 16]; memset(big, 0xcc, sizeof big); memset(&c, 0, sizeof c); svc_set_out_buf(&c, big, sizeof big); svc_call_c(set, 7, &c);
        CHECK(!memcmp(big, km, SET_KEYMAP_SIZE) && big[SET_KEYMAP_SIZE] == 0xcc, "a long buffer is written only up to the map size");
    }
    printf("{\"test\":\"svc_settings\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
