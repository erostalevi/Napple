/* set and set:sys: language, region, firmware version and the system settings the SDK reads at startup. */
#include "../ipc.h"
#include "host_info.h"
#include "settings_core.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const languages[] = {"ja", "en-US", "fr",    "de",    "it",    "es",     "zh-CN",   "ko",      "nl",
                                        "pt", "ru",    "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NLANG 18
static uint64_t lang_code(const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s));
    return v;
}

static void set_get_language_code(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = host_desired_language();
    IPC_OUT(c, v);
}
static void language_list(IpcCall *c, int max) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    int n = 0;
    if (b) {
        uint64_t out[NLANG];
        for (; n < max && n < NLANG && (uint64_t)(n + 1) * 8 <= b->size; n++) out[n] = lang_code(languages[n]);
        ipc_write_buf(c, b, out, (uint64_t)n * 8);
    }
    int32_t cnt = n;
    IPC_OUT(c, cnt);
}
static void set_get_available(IpcObject *s, IpcCall *c) {
    (void)s;
    language_list(c, 15);
}
static void set_get_available2(IpcObject *s, IpcCall *c) {
    (void)s;
    language_list(c, NLANG);
}
static void set_make_language_code(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t i = IPC_IN(c, uint32_t, 0);
    uint64_t v = i < NLANG ? lang_code(languages[i]) : 0;
    IPC_OUT(c, v);
}
static void set_count15(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 15;
    IPC_OUT(c, v);
}
static void set_count18(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = NLANG;
    IPC_OUT(c, v);
}
static void set_get_region_code(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = settings_region(host_desired_language(), getenv("NAPPLE_REGION"));
    IPC_OUT(c, v);
}
static void set_get_quest_flag(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
/* Key code maps (the keyboard layout a firmware applet or the on-screen keyboard asks for): the US layout (settings_keycode_map); one layout serves all three calls. */
static void set_key_code_map(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b && b->size) {
        uint8_t map[SET_KEYMAP_SIZE];
        size_t n = settings_keycode_map(map, b->size < sizeof map ? (size_t)b->size : sizeof map);
        ipc_write_buf(c, b, map, n);
    }
}
static const IpcCmd set_cmds[] = {
    {0, "GetLanguageCode", set_get_language_code, IPC_IMPL},
    {1, "GetAvailableLanguageCodes", set_get_available, IPC_IMPL},
    {2, "MakeLanguageCode", set_make_language_code, IPC_IMPL},
    {3, "GetAvailableLanguageCodeCount", set_count15, IPC_IMPL},
    {4, "GetRegionCode", set_get_region_code, IPC_IMPL},
    {5, "GetAvailableLanguageCodes2", set_get_available2, IPC_IMPL},
    {6, "GetAvailableLanguageCodeCount2", set_count18, IPC_IMPL},
    {7, "GetKeyCodeMap", set_key_code_map, IPC_IMPL},
    {9, "GetKeyCodeMap2", set_key_code_map, IPC_IMPL},
    {11, "GetKeyCodeMap3", set_key_code_map, IPC_IMPL},
    {8, "GetQuestFlag", set_get_quest_flag, IPC_IMPL}, /* a retail console is never a quest unit */
};
static IpcObject *set_open(void) { return IPC_OBJECT("set", set_cmds, NULL, NULL); }

/* set:sys */
static void sys_get_firmware_version(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || b->size < 0x100) {
        c->result = 0x1d80;
        return;
    }
    uint8_t v[0x100] = {0};
    v[0] = 18;
    v[1] = 1;
    v[2] = 0;
    v[4] = 18;
    v[5] = 1;
    v[6] = 0; /* major.minor.micro, revision major/minor */
    snprintf((char *)v + 0x28, 0x10, "NX");
    snprintf((char *)v + 0x68, 0x18, "18.1.0");
    snprintf((char *)v + 0x80, 0x80, "NintendoSDK Firmware for NX 18.1.0-1.0");
    ipc_write_buf(c, b, v, 0x100);
}
static void sys_get_color_set_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = settings_color_set(getenv("NAPPLE_COLOR_SET"));
    IPC_OUT(c, v);
}
static void sys_get_device_timezone(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[SET_TZ_NAME_SIZE];
    settings_timezone_name(getenv("NAPPLE_TIMEZONE"), n);
    ipc_out(c, n, sizeof n);
}
/* GetMiiAuthorId (90): the 16-byte id the Mii library mixes into StoreData checksums (mii_core.c). */
__attribute__((weak)) void mii_device_id(uint8_t out[16]) { memset(out, 0, 16); }
static void sys_get_mii_author_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t id[16];
    mii_device_id(id);
    ipc_out(c, id, 16);
}
static const IpcCmd sys_cmds[] = {
    {90, "GetMiiAuthorId", sys_get_mii_author_id, IPC_IMPL},
    {3, "GetFirmwareVersion", sys_get_firmware_version, IPC_IMPL},
    {4, "GetFirmwareVersion2", sys_get_firmware_version, IPC_IMPL},
    {23, "GetColorSetId", sys_get_color_set_id, IPC_IMPL},
    {62, "GetDeviceTimeZoneLocationName", sys_get_device_timezone, IPC_IMPL},
};
static IpcObject *sys_open(void) { return IPC_OBJECT("set:sys", sys_cmds, NULL, NULL); }
void service_register_settings(void) {
    ipc_register_service("set", set_open);
    ipc_register_service("set:sys", sys_open);
}
