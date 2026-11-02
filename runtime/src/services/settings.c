/* set and set:sys: language, region, firmware version and the system settings the SDK reads at startup. */
#include "../ipc.h"
#include "host_info.h"
#include "settings_core.h"
#include "sysset_core.h"
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

/* The language a set:sys SetLanguageCode (0) chose, else the host's. */
static uint64_t current_language(void) {
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    uint64_t v = st->language;
    pthread_mutex_unlock(&st->mu);
    return v ? v : host_desired_language();
}
static void set_get_language_code(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = current_language();
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
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    int32_t r = st->region;
    pthread_mutex_unlock(&st->mu);
    uint32_t v = r >= 0 ? (uint32_t)r : settings_region(current_language(), getenv("NAPPLE_REGION"));
    IPC_OUT(c, v);
}
/* GetQuestFlag (set 8, set:sys 47): a retail console is never a quest (kiosk) unit unless set:sys SetQuestFlag (48) said so. */
static void set_get_quest_flag(IpcObject *s, IpcCall *c) {
    (void)s;
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    uint8_t v = st->quest;
    pthread_mutex_unlock(&st->mu);
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
/* GetDeviceNickName (set 11 since 10.1.0, set:sys 77): the console's name, a 0x80-byte NUL-terminated string. The host is a Napple runtime, so that
 * is its name unless NAPPLE_DEVICE_NICKNAME gives another one. */
static void set_get_device_nickname(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[0x80] = {0};
    const char *e = getenv("NAPPLE_DEVICE_NICKNAME");
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    if (st->has_nickname) memcpy(n, st->nickname, sizeof n);
    pthread_mutex_unlock(&st->mu);
    if (!n[0]) snprintf(n, sizeof n, "%s", e && *e ? e : "Napple");
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, n, b->size < sizeof n ? b->size : sizeof n);
}

/* 17 GetKeyboardLayout -> s32 KeyboardLayout: 1 EnglishUs, the layout GetKeyCodeMap serves (libnx SetKeyboardLayout numbering). */
static void set_get_keyboard_layout(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 1;
    IPC_OUT(c, v);
}
static const IpcCmd set_cmds[] = {
    {17, "GetKeyboardLayout", set_get_keyboard_layout, IPC_IMPL},
    {0, "GetLanguageCode", set_get_language_code, IPC_IMPL},
    {1, "GetAvailableLanguageCodes", set_get_available, IPC_IMPL},
    {2, "MakeLanguageCode", set_make_language_code, IPC_IMPL},
    {3, "GetAvailableLanguageCodeCount", set_count15, IPC_IMPL},
    {4, "GetRegionCode", set_get_region_code, IPC_IMPL},
    {5, "GetAvailableLanguageCodes2", set_get_available2, IPC_IMPL},
    {6, "GetAvailableLanguageCodeCount2", set_count18, IPC_IMPL},
    {7, "GetKeyCodeMap", set_key_code_map, IPC_IMPL},
    {9, "GetKeyCodeMap2", set_key_code_map, IPC_IMPL},
    {11, "GetDeviceNickName", set_get_device_nickname, IPC_IMPL}, /* was answered as "GetKeyCodeMap3" before 2026-11-01: 11 is the nickname (10.1.0+) */
    {12, "GetKeyCodeMapByPort", set_key_code_map, IPC_IMPL},     /* 18.0.0+: {u32 port} in; one layout for every port */
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
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    int32_t id = st->color_set;
    pthread_mutex_unlock(&st->mu);
    uint32_t v = id >= 0 ? (uint32_t)id : settings_color_set(getenv("NAPPLE_COLOR_SET"));
    IPC_OUT(c, v);
}
static void sys_get_device_timezone(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[SET_TZ_NAME_SIZE];
    SysSettings *st = sysset_global();
    pthread_mutex_lock(&st->mu);
    int have = st->has_tz;
    if (have) memcpy(n, st->tz, sizeof n);
    pthread_mutex_unlock(&st->mu);
    if (!have) settings_timezone_name(getenv("NAPPLE_TIMEZONE"), n);
    ipc_out(c, n, sizeof n);
}
static void sys_get_debug_mode(IpcObject *s, IpcCall *c) { /* 62 GetDebugModeFlag: a retail console */
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void sys_get_product_model(IpcObject *s, IpcCall *c) { /* 79 GetProductModel: 1 = Nx (the original console model) */
    (void)s;
    uint32_t v = 1;
    IPC_OUT(c, v);
}
/* GetMiiAuthorId (90): the 16-byte id the Mii library mixes into StoreData checksums (mii_core.c). */
__attribute__((weak)) void mii_device_id(uint8_t out[16]) { memset(out, 0, 16); }
static void sys_get_mii_author_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t id[16];
    mii_device_id(id);
    ipc_out(c, id, 16);
}
/* ---- set:sys settings store (sysset_core.c): values the console keeps in its system settings, written by Set* and read back by Get*. Layouts per
 * switchbrew ISystemSettingsServer / libnx setsys (sizes and argument order only). Nothing is persisted across runs. ---- */
#define SYSSET_LOCK(st) pthread_mutex_lock(&(st)->mu)
#define SYSSET_UNLOCK(st) pthread_mutex_unlock(&(st)->mu)
/* Raw getters / setters of the blob pairs (lock screen 7/8, backlight 9/10, external steady clock source 13/14, user system clock context 15/16,
 * account settings 17/18, console information upload 25/26, automatic application download 27/28, notification settings 29/30, quest 47/48,
 * time zone location updated time 150/151): Get copies the field to the raw output, Set copies the raw input (a short input changes only its prefix). */
static void sys_blob_get(IpcObject *s, IpcCall *c) {
    (void)s;
    SysSettings *st = sysset_global();
    uint8_t tmp[0x40];
    uint32_t n = 0;
    SYSSET_LOCK(st);
    const uint8_t *p = sysset_blob(st, c->cmd, &n);
    if (p) memcpy(tmp, p, n);
    SYSSET_UNLOCK(st);
    if (p) ipc_out(c, tmp, n);
}
static void sys_blob_set(IpcObject *s, IpcCall *c) {
    (void)s;
    SysSettings *st = sysset_global();
    uint32_t n = 0;
    SYSSET_LOCK(st);
    uint8_t *p = sysset_blob(st, c->cmd, &n);
    if (p) memcpy(p, c->in, c->in_size < n ? c->in_size : n);
    SYSSET_UNLOCK(st);
}
/* 0 SetLanguageCode(u64 LanguageCode): GetLanguageCode (set 0) and the region derived from it follow. */
static void sys_set_language(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = IPC_IN(c, uint64_t, 0);
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    st->language = v;
    SYSSET_UNLOCK(st);
}
/* 24 SetColorSetId(s32), 57 SetRegionCode(s32), 184 SetPlatformRegion(s32): the matching getters (23, set 4, 183) return the value from then on. */
static void sys_set_s32(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = IPC_IN(c, int32_t, 0);
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    if (c->cmd == 24) st->color_set = v < 0 ? -1 : v;
    else if (c->cmd == 57) st->region = v < 0 ? -1 : v;
    else if (c->cmd == 184) st->platform_region = v;
    SYSSET_UNLOCK(st);
}
/* 183 GetPlatformRegion -> s32: 1 Global unless SetPlatformRegion changed it. */
static void sys_get_platform_region(IpcObject *s, IpcCall *c) {
    (void)s;
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    int32_t v = st->platform_region;
    SYSSET_UNLOCK(st);
    IPC_OUT(c, v);
}
/* List settings: 1 SetNetworkSettings / 11 SetBluetoothDevicesSettings / 22 SetEulaVersions take the whole list in a type-5 buffer; 2 / 12 / 21 return
 * {s32 count} and fill a type-6 buffer with whole entries. */
static int sys_list_kind(unsigned cmd) { return (cmd == 1 || cmd == 2) ? 0 : (cmd == 11 || cmd == 12) ? 1 : 2; }
static void sys_list_set(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    sysset_list_set(st, sys_list_kind(c->cmd), p, p ? b->size : 0);
    SYSSET_UNLOCK(st);
}
static void sys_list_get(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    int kind = sys_list_kind(c->cmd);
    uint32_t entry = sysset_list_entry_size(kind);
    uint64_t cap = b ? b->size : 0;
    if (cap > (uint64_t)SYSSET_NETWORK_MAX * SYSSET_NETWORK_ENTRY) cap = (uint64_t)SYSSET_NETWORK_MAX * SYSSET_NETWORK_ENTRY;
    uint8_t *tmp = cap ? malloc((size_t)cap) : NULL;
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    int32_t n = (int32_t)sysset_list_get(st, kind, tmp, tmp ? cap : 0);
    SYSSET_UNLOCK(st);
    if (b && n) ipc_write_buf(c, b, tmp, (uint64_t)n * entry);
    free(tmp);
    IPC_OUT(c, n);
}
/* 19 GetAudioVolume(u32 AudioOutputModeTarget) -> SetSysAudioVolume; 20 SetAudioVolume(SetSysAudioVolume, u32 target). An unknown target is an
 * invalid-argument error (set module 105, napple's reading). */
#define R_SET_INVALID_TARGET 0x1ac69u /* module 105 (settings), description 1123 [unverified] */
static void sys_get_audio_volume(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v[SYSSET_AUDIO_VOLUME_SIZE];
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    int r = sysset_audio_volume_get(st, IPC_IN(c, uint32_t, 0), v);
    SYSSET_UNLOCK(st);
    if (r) {
        c->result = R_SET_INVALID_TARGET;
        return;
    }
    ipc_out(c, v, sizeof v);
}
static void sys_set_audio_volume(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v[SYSSET_AUDIO_VOLUME_SIZE];
    memcpy(v, ipc_in(c, 0, SYSSET_AUDIO_VOLUME_SIZE), sizeof v);
    uint32_t target = IPC_IN(c, uint32_t, SYSSET_AUDIO_VOLUME_SIZE);
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    int r = sysset_audio_volume_set(st, target, v);
    SYSSET_UNLOCK(st);
    if (r) c->result = R_SET_INVALID_TARGET;
}
/* 54 SetDeviceTimeZoneLocationName(LocationName 0x24): GetDeviceTimeZoneLocationName (53) returns it from then on. */
static void sys_set_device_timezone(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[SYSSET_TZ_SIZE];
    memcpy(n, ipc_in(c, 0, SYSSET_TZ_SIZE), sizeof n);
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    sysset_set_tz(st, n, sizeof n);
    SYSSET_UNLOCK(st);
}
/* 78 SetDeviceNickName(type-0x15 buffer, 0x80 bytes): both GetDeviceNickName rows (set 11, set:sys 77) return it from then on. */
static void sys_set_device_nickname(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const char *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    sysset_set_nickname(st, p, p ? (size_t)b->size : 0);
    SYSSET_UNLOCK(st);
}
/* 68 GetSerialNumber -> SerialNumber (0x18 bytes, NUL-terminated): NAPPLE_SERIAL, else "XAW00000000000". */
static void sys_get_serial(IpcObject *s, IpcCall *c) {
    (void)s;
    char n[SYSSET_SERIAL_SIZE];
    SysSettings *st = sysset_global();
    SYSSET_LOCK(st);
    memcpy(n, st->serial, sizeof n);
    SYSSET_UNLOCK(st);
    ipc_out(c, n, sizeof n);
}

static const IpcCmd sys_cmds[] = {
    {0, "SetLanguageCode", sys_set_language, IPC_IMPL},
    {1, "SetNetworkSettings", sys_list_set, IPC_IMPL},
    {2, "GetNetworkSettings", sys_list_get, IPC_IMPL},
    {7, "GetLockScreenFlag", sys_blob_get, IPC_IMPL},
    {8, "SetLockScreenFlag", sys_blob_set, IPC_IMPL},
    {9, "GetBacklightSettings", sys_blob_get, IPC_IMPL},
    {10, "SetBacklightSettings", sys_blob_set, IPC_IMPL},
    {11, "SetBluetoothDevicesSettings", sys_list_set, IPC_IMPL},
    {12, "GetBluetoothDevicesSettings", sys_list_get, IPC_IMPL},
    {13, "GetExternalSteadyClockSourceId", sys_blob_get, IPC_IMPL},
    {14, "SetExternalSteadyClockSourceId", sys_blob_set, IPC_IMPL},
    {15, "GetUserSystemClockContext", sys_blob_get, IPC_IMPL},
    {16, "SetUserSystemClockContext", sys_blob_set, IPC_IMPL},
    {17, "GetAccountSettings", sys_blob_get, IPC_IMPL},
    {18, "SetAccountSettings", sys_blob_set, IPC_IMPL},
    {19, "GetAudioVolume", sys_get_audio_volume, IPC_IMPL},
    {20, "SetAudioVolume", sys_set_audio_volume, IPC_IMPL},
    {21, "GetEulaVersions", sys_list_get, IPC_IMPL},
    {22, "SetEulaVersions", sys_list_set, IPC_IMPL},
    {24, "SetColorSetId", sys_set_s32, IPC_IMPL},
    {25, "GetConsoleInformationUploadFlag", sys_blob_get, IPC_IMPL},
    {26, "SetConsoleInformationUploadFlag", sys_blob_set, IPC_IMPL},
    {27, "GetAutomaticApplicationDownloadFlag", sys_blob_get, IPC_IMPL},
    {28, "SetAutomaticApplicationDownloadFlag", sys_blob_set, IPC_IMPL},
    {29, "GetNotificationSettings", sys_blob_get, IPC_IMPL},
    {30, "SetNotificationSettings", sys_blob_set, IPC_IMPL},
    {47, "GetQuestFlag", set_get_quest_flag, IPC_IMPL},
    {48, "SetQuestFlag", sys_blob_set, IPC_IMPL},
    {54, "SetDeviceTimeZoneLocationName", sys_set_device_timezone, IPC_IMPL},
    {57, "SetRegionCode", sys_set_s32, IPC_IMPL},
    {68, "GetSerialNumber", sys_get_serial, IPC_IMPL},
    {78, "SetDeviceNickName", sys_set_device_nickname, IPC_IMPL},
    {150, "GetDeviceTimeZoneLocationUpdatedTime", sys_blob_get, IPC_IMPL},
    {151, "SetDeviceTimeZoneLocationUpdatedTime", sys_blob_set, IPC_IMPL},
    {183, "GetPlatformRegion", sys_get_platform_region, IPC_IMPL},
    {184, "SetPlatformRegion", sys_set_s32, IPC_IMPL},
    {90, "GetMiiAuthorId", sys_get_mii_author_id, IPC_IMPL},
    {3, "GetFirmwareVersion", sys_get_firmware_version, IPC_IMPL},
    {4, "GetFirmwareVersion2", sys_get_firmware_version, IPC_IMPL},
    {23, "GetColorSetId", sys_get_color_set_id, IPC_IMPL},
    /* 53 was registered as 62 before 2026-11-01; 62 is GetDebugModeFlag (switchbrew ISystemSettingsServer) */
    {53, "GetDeviceTimeZoneLocationName", sys_get_device_timezone, IPC_IMPL},
    {62, "GetDebugModeFlag", sys_get_debug_mode, IPC_IMPL},
    {77, "GetDeviceNickName", set_get_device_nickname, IPC_IMPL},
    {79, "GetProductModel", sys_get_product_model, IPC_IMPL},
};
static IpcObject *sys_open(void) { return IPC_OBJECT("set:sys", sys_cmds, NULL, NULL); }
void service_register_settings(void) {
    ipc_register_service("set", set_open);
    ipc_register_service("set:sys", sys_open);
}
