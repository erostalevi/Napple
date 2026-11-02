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
    { char nick[0x80] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, nick, sizeof nick); svc_call_c(set, 11, &c); CHECK(!strcmp(nick, "Napple"), "set GetDeviceNickName (11)");
      setenv("NAPPLE_DEVICE_NICKNAME", "Desk", 1); memset(&c, 0, sizeof c); svc_set_out_buf(&c, nick, sizeof nick); svc_call_c(set, 11, &c); CHECK(!strcmp(nick, "Desk"), "nickname from the environment");
      unsetenv("NAPPLE_DEVICE_NICKNAME");
      static uint8_t km[SET_KEYMAP_SIZE]; uint32_t port = 0; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&port; c.in_size = 4; svc_set_out_buf(&c, km, sizeof km); svc_call_c(set, 12, &c);
      CHECK(c.result == 0, "GetKeyCodeMapByPort"); }
    svc_call(set, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == code("ja"), "GetLanguageCode");
    svc_call(set, 6, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 18, "18 language codes");
    svc_call(sys, 23, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_COLOR_BLACK, "colour set id");
    svc_call(sys, 53, NULL, 0, &c); CHECK(c.out_size == SET_TZ_NAME_SIZE && !strcmp((const char *)c.out, "UTC"), "time zone name (53)");
    svc_call(sys, 62, NULL, 0, &c); CHECK(c.out_size == 1 && c.out[0] == 0, "GetDebugModeFlag: retail (62)");
    svc_call(sys, 79, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "GetProductModel: Nx");
    { char nick[0x80] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, nick, sizeof nick); svc_call_c(sys, 77, &c); CHECK(!strcmp(nick, "Napple"), "set:sys GetDeviceNickName"); }
    uint8_t fw[0x100]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, fw, sizeof fw); svc_call_c(sys, 3, &c);
    CHECK(c.result == 0 && fw[0] == 18 && !strcmp((char *)fw + 0x68, "18.1.0"), "firmware version 18.1.0");
    uint8_t small[0x10]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, small, sizeof small); svc_call_c(sys, 3, &c); CHECK(c.result != 0, "buffer too small");
    { /* key code maps: the US layout, 16 bytes per HID usage */
        uint8_t km[SET_KEYMAP_SIZE]; unsigned ids[3] = {7, 9, 12}; /* 11 is GetDeviceNickName (10.1.0+), 12 GetKeyCodeMapByPort (18.0.0+) */
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
    { /* set:sys settings store (ipc-depth-cloud r9): every Set* is read back by its Get*, defaults first */
        SysSettings def; sysset_init(&def, "bad serial"); CHECK(!strcmp(def.serial, "XAW00000000000") && def.platform_region == 1 && def.audio_volume[0][4] == 8, "store defaults");
        sysset_init(&def, "XKW12345678901"); CHECK(!strcmp(def.serial, "XKW12345678901"), "NAPPLE_SERIAL kept");
        uint8_t one = 1, zero8[0x40] = {0};
        unsigned flag_pairs[][2] = {{7, 8}, {25, 26}, {27, 28}, {47, 48}};
        for (int i = 0; i < 4; i++) {
            svc_call(sys, flag_pairs[i][0], NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "flag %u off by default", flag_pairs[i][0]);
            svc_call(sys, flag_pairs[i][1], &one, 1, &c); CHECK(c.result == 0 && c.out_size == 0, "set flag %u", flag_pairs[i][1]);
            svc_call(sys, flag_pairs[i][0], NULL, 0, &c); CHECK(c.out_size == 1 && c.out[0] == 1, "flag %u reads back", flag_pairs[i][0]);
            svc_call(sys, flag_pairs[i][1], zero8, 1, &c);
        }
        svc_call(set, 8, &one, 0, &c); CHECK(c.out[0] == 0, "set GetQuestFlag shares the store (cleared again)");
        svc_call(sys, 48, &one, 1, &c); svc_call(set, 8, NULL, 0, &c); CHECK(c.out[0] == 1, "set:sys SetQuestFlag seen by set GetQuestFlag"); svc_call(sys, 48, zero8, 1, &c);
        struct { unsigned get, set, size; } blobs[] = {{9, 10, 0x28}, {13, 14, 0x10}, {15, 16, 0x20}, {17, 18, 4}, {29, 30, 0x18}, {150, 151, 0x18}};
        for (int i = 0; i < 6; i++) {
            uint8_t v[0x40]; for (unsigned k = 0; k < sizeof v; k++) v[k] = (uint8_t)(0x40 + i + k);
            svc_call(sys, blobs[i].get, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == blobs[i].size, "Get %u returns 0x%x bytes", blobs[i].get, blobs[i].size);
            svc_call(sys, blobs[i].set, v, blobs[i].size, &c); CHECK(c.result == 0, "Set %u", blobs[i].set);
            svc_call(sys, blobs[i].get, NULL, 0, &c); CHECK(c.out_size == blobs[i].size && !memcmp(c.out, v, blobs[i].size), "Get %u reads back what Set %u stored", blobs[i].get, blobs[i].set);
        }
        svc_call(sys, 9, NULL, 0, &c); /* backlight default replaced above; check the default on a fresh store instead */
        float br; memcpy(&br, def.backlight + 4, 4); CHECK(br == 0.5f && def.backlight[0] == 0, "default backlight: automatic off, brightness 0.5");

        uint64_t de = code("de"); svc_call(sys, 0, &de, 8, &c); CHECK(c.result == 0, "SetLanguageCode");
        svc_call(set, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == de, "GetLanguageCode follows SetLanguageCode");
        svc_call(set, 4, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_REGION_EUR, "region derived from the stored language");
        int32_t r = SET_REGION_AUS; svc_call(sys, 57, &r, 4, &c); svc_call(set, 4, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_REGION_AUS, "SetRegionCode");
        int32_t white = SET_COLOR_WHITE; svc_call(sys, 24, &white, 4, &c); svc_call(sys, 23, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SET_COLOR_WHITE, "SetColorSetId");
        svc_call(sys, 183, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 1, "GetPlatformRegion: Global");
        int32_t china = 2; svc_call(sys, 184, &china, 4, &c); svc_call(sys, 183, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 2, "SetPlatformRegion");
        char tzn[0x24] = "Asia/Tokyo"; svc_call(sys, 54, tzn, sizeof tzn, &c); svc_call(sys, 53, NULL, 0, &c); CHECK(!strcmp((const char *)c.out, "Asia/Tokyo"), "SetDeviceTimeZoneLocationName");
        svc_call(sys, 68, NULL, 0, &c); CHECK(c.out_size == 0x18 && !strncmp((const char *)c.out, "XAW", 3) && c.out[0x17] == 0, "GetSerialNumber");

        char newnick[0x80] = "Living room", nick[0x80];
        memset(&c, 0, sizeof c); svc_set_in_buf(&c, newnick, sizeof newnick); svc_call_c(sys, 78, &c); CHECK(c.result == 0, "SetDeviceNickName");
        memset(&c, 0, sizeof c); svc_set_out_buf(&c, nick, sizeof nick); svc_call_c(set, 11, &c); CHECK(!strcmp(nick, "Living room"), "set GetDeviceNickName follows SetDeviceNickName");
        memset(&c, 0, sizeof c); svc_set_out_buf(&c, nick, sizeof nick); svc_call_c(sys, 77, &c); CHECK(!strcmp(nick, "Living room"), "set:sys GetDeviceNickName too");

        uint8_t vol[8]; uint32_t spk = 0; svc_call(sys, 19, &spk, 4, &c); CHECK(c.result == 0 && c.out_size == 8 && c.out[4] == 8, "GetAudioVolume default 8");
        uint8_t in[12] = {0}; in[4] = 13; uint32_t hp = 1; memcpy(in + 8, &hp, 4); svc_call(sys, 20, in, 12, &c); CHECK(c.result == 0, "SetAudioVolume headphone");
        svc_call(sys, 19, &hp, 4, &c); memcpy(vol, c.out, 8); CHECK(vol[4] == 13, "headphone volume reads back");
        svc_call(sys, 19, &spk, 4, &c); CHECK(c.out[4] == 8, "speaker unchanged");
        uint32_t bad = 99; svc_call(sys, 19, &bad, 4, &c); CHECK(c.result != 0, "unknown audio target refused");

        unsigned lists[][3] = {{1, 2, 0x400}, {11, 12, 0x200}, {22, 21, 0x30}};
        for (int i = 0; i < 3; i++) {
            static uint8_t src[3 * 0x400], dst[4 * 0x400];
            uint32_t e = lists[i][2]; for (unsigned k = 0; k < 3 * e; k++) src[k] = (uint8_t)(k * 7 + i);
            memset(&c, 0, sizeof c); svc_set_out_buf(&c, dst, 4 * e); svc_call_c(sys, lists[i][1], &c); CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "list %u empty by default", lists[i][1]);
            memset(&c, 0, sizeof c); svc_set_in_buf(&c, src, 3 * e + 5); svc_call_c(sys, lists[i][0], &c); CHECK(c.result == 0, "Set list %u (trailing partial entry dropped)", lists[i][0]);
            memset(dst, 0, sizeof dst); memset(&c, 0, sizeof c); svc_set_out_buf(&c, dst, 4 * e); svc_call_c(sys, lists[i][1], &c);
            CHECK(SVC_OUT(&c, int32_t, 0) == 3 && !memcmp(dst, src, 3 * e), "Get list %u returns the 3 entries", lists[i][1]);
            memset(&c, 0, sizeof c); svc_set_out_buf(&c, dst, 2 * e); svc_call_c(sys, lists[i][1], &c); CHECK(SVC_OUT(&c, int32_t, 0) == 2, "a 2-entry buffer gets 2");
        }
        svc_call(set, 17, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 1, "GetKeyboardLayout: EnglishUs, matching the key code map");
        static uint8_t many[12 * 0x200]; CHECK(sysset_list_set(&def, 1, many, sizeof many) == SYSSET_BLUETOOTH_MAX, "bluetooth list capped at 10");
    }
    printf("{\"test\":\"svc_settings\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
