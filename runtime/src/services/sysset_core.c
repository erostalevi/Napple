/* set:sys settings store (portable): see sysset_core.h. */
#include "sysset_core.h"
#include <stdlib.h>
#include <string.h>

static int serial_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= SYSSET_SERIAL_SIZE) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7e) return 0;
    return 1;
}

void sysset_init(SysSettings *s, const char *serial) {
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->region = -1;
    s->color_set = -1;
    s->platform_region = SYSSET_PLATFORM_GLOBAL;
    strcpy(s->serial, serial_ok(serial) ? serial : "XAW00000000000");
    float brightness = 0.5f; /* backlight: u32 flags (0: automatic brightness off), float brightness, then coefficients (zero) */
    memcpy(s->backlight + 4, &brightness, 4);
    for (unsigned d = 0; d < SYSSET_AUDIO_DEVICES; d++) s->audio_volume[d][4] = 8; /* {u32 flags (0), u8 volume, pad} */
}

static SysSettings g_store;
static pthread_once_t g_store_once = PTHREAD_ONCE_INIT;
static void init_global(void) { sysset_init(&g_store, getenv("NAPPLE_SERIAL")); }
SysSettings *sysset_global(void) {
    pthread_once(&g_store_once, init_global);
    return &g_store;
}

uint8_t *sysset_blob(SysSettings *s, unsigned cmd, uint32_t *size) {
    uint8_t *p = NULL;
    uint32_t n = 0;
    switch (cmd) {
    case 7: case 8: p = &s->lock_screen; n = 1; break;
    case 9: case 10: p = s->backlight; n = SYSSET_BACKLIGHT_SIZE; break;
    case 13: case 14: p = s->ext_clock_source; n = SYSSET_UUID_SIZE; break;
    case 15: case 16: p = s->user_clock_ctx; n = SYSSET_CLOCK_CONTEXT_SIZE; break;
    case 17: case 18: p = (uint8_t *)&s->account_settings; n = 4; break;
    case 25: case 26: p = &s->console_info_upload; n = 1; break;
    case 27: case 28: p = &s->auto_app_download; n = 1; break;
    case 29: case 30: p = s->notification; n = SYSSET_NOTIFICATION_SIZE; break;
    case 47: case 48: p = &s->quest; n = 1; break;
    case 150: case 151: p = s->tz_updated; n = SYSSET_TIME_POINT_SIZE; break;
    default: break;
    }
    if (size) *size = n;
    return p;
}

static uint8_t *list_base(SysSettings *s, int which, uint32_t **count, uint32_t *max) {
    switch (which) {
    case 0: *count = &s->nnetwork; *max = SYSSET_NETWORK_MAX; return &s->network[0][0];
    case 1: *count = &s->nbluetooth; *max = SYSSET_BLUETOOTH_MAX; return &s->bluetooth[0][0];
    case 2: *count = &s->neula; *max = SYSSET_EULA_MAX; return &s->eula[0][0];
    default: return NULL;
    }
}
uint32_t sysset_list_entry_size(int which) {
    return which == 0 ? SYSSET_NETWORK_ENTRY : which == 1 ? SYSSET_BLUETOOTH_ENTRY : which == 2 ? SYSSET_EULA_ENTRY : 0;
}
uint32_t sysset_list_set(SysSettings *s, int which, const void *data, uint64_t size) {
    uint32_t *count, max, entry = sysset_list_entry_size(which);
    uint8_t *base = list_base(s, which, &count, &max);
    if (!base) return 0;
    uint64_t n = data ? size / entry : 0;
    if (n > max) n = max;
    if (n) memcpy(base, data, (size_t)n * entry);
    *count = (uint32_t)n;
    return *count;
}
uint32_t sysset_list_get(SysSettings *s, int which, void *out, uint64_t cap) {
    uint32_t *count, max, entry = sysset_list_entry_size(which);
    uint8_t *base = list_base(s, which, &count, &max);
    if (!base) return 0;
    uint64_t n = out ? cap / entry : 0;
    if (n > *count) n = *count;
    if (n) memcpy(out, base, (size_t)n * entry);
    return (uint32_t)n;
}

int sysset_audio_volume_get(SysSettings *s, uint32_t device, uint8_t out[SYSSET_AUDIO_VOLUME_SIZE]) {
    if (device >= SYSSET_AUDIO_DEVICES) return -1;
    memcpy(out, s->audio_volume[device], SYSSET_AUDIO_VOLUME_SIZE);
    return 0;
}
int sysset_audio_volume_set(SysSettings *s, uint32_t device, const uint8_t in[SYSSET_AUDIO_VOLUME_SIZE]) {
    if (device >= SYSSET_AUDIO_DEVICES) return -1;
    memcpy(s->audio_volume[device], in, SYSSET_AUDIO_VOLUME_SIZE);
    return 0;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t n) {
    memset(dst, 0, cap);
    if (!src) return;
    size_t len = strnlen(src, n);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
}
void sysset_set_tz(SysSettings *s, const char *name, size_t n) {
    copy_str(s->tz, sizeof s->tz, name, n);
    s->has_tz = 1;
}
void sysset_set_nickname(SysSettings *s, const char *name, size_t n) {
    copy_str(s->nickname, sizeof s->nickname, name, n);
    s->has_nickname = 1;
}
