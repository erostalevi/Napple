#include "settings_core.h"
#include <string.h>
#include <strings.h>

static int lang_is(uint64_t lang, const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s));
    return v == lang;
}

uint32_t settings_region(uint64_t lang, const char *env) {
    static const struct {
        const char *name;
        uint32_t id;
    } named[] = {{"jpn", SET_REGION_JPN}, {"usa", SET_REGION_USA}, {"eur", SET_REGION_EUR},
                 {"aus", SET_REGION_AUS}, {"htk", SET_REGION_HTK}, {"chn", SET_REGION_CHN}};
    if (env && *env) {
        for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
            if (!strcasecmp(env, named[i].name)) return named[i].id;
        if (env[0] >= '0' && env[0] <= '5' && !env[1]) return (uint32_t)(env[0] - '0');
    }
    if (lang_is(lang, "ja")) return SET_REGION_JPN;
    if (lang_is(lang, "en-US") || lang_is(lang, "fr-CA") || lang_is(lang, "es-419") || lang_is(lang, "pt-BR")) return SET_REGION_USA;
    if (lang_is(lang, "ko") || lang_is(lang, "zh-TW") || lang_is(lang, "zh-Hant")) return SET_REGION_HTK;
    if (lang_is(lang, "zh-CN") || lang_is(lang, "zh-Hans")) return SET_REGION_CHN;
    return SET_REGION_EUR; /* en-GB fr de it es nl pt ru, and anything unknown */
}

void settings_timezone_name(const char *env, char out[SET_TZ_NAME_SIZE]) {
    memset(out, 0, SET_TZ_NAME_SIZE);
    size_t n = env ? strlen(env) : 0;
    int ok = n > 0 && n < SET_TZ_NAME_SIZE;
    for (size_t i = 0; ok && i < n; i++) {
        char ch = env[i];
        ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '_' || ch == '-' || ch == '+';
    }
    if (ok && env[0] != '/' && env[n - 1] != '/' && !strstr(env, "..")) memcpy(out, env, n);
    else memcpy(out, "UTC", 3);
}

uint32_t settings_color_set(const char *env) {
    if (env && (!strcasecmp(env, "white") || !strcmp(env, "0"))) return SET_COLOR_WHITE;
    return SET_COLOR_BLACK;
}

/* HID usage ids 0x04-0x1d letters, 0x1e-0x27 digits, then Enter Esc Backspace Tab Space and the punctuation keys (US layout). */
static const struct {
    uint8_t usage;
    char base, shift;
} us_punct[] = {{0x28, '\n', '\n'}, {0x29, 0x1b, 0x1b}, {0x2a, 0x08, 0x08}, {0x2b, '\t', '\t'}, {0x2c, ' ', ' '}, {0x2d, '-', '_'},
                {0x2e, '=', '+'},   {0x2f, '[', '{'},   {0x30, ']', '}'},   {0x31, '\\', '|'},  {0x32, '#', '~'}, {0x33, ';', ':'},
                {0x34, '\'', '"'},  {0x35, '`', '~'},   {0x36, ',', '<'},   {0x37, '.', '>'},   {0x38, '/', '?'}, {0x64, '\\', '|'}};
size_t settings_keycode_map(uint8_t *out, size_t cap) {
    uint8_t map[SET_KEYMAP_SIZE];
    memset(map, 0, sizeof map);
    static const char shifted_digits[] = ")!@#$%^&*(";
    for (unsigned u = 0; u < 256; u++) {
        uint16_t base = 0, shift = 0;
        if (u >= 0x04 && u <= 0x1d) base = (uint16_t)('a' + u - 0x04), shift = (uint16_t)('A' + u - 0x04);
        else if (u >= 0x1e && u <= 0x27) {
            unsigned d = (u - 0x1e + 1) % 10; /* usage 0x1e is "1", 0x27 is "0" */
            base = (uint16_t)('0' + d), shift = (uint16_t)shifted_digits[d];
        } else
            for (unsigned i = 0; i < sizeof us_punct / sizeof us_punct[0]; i++)
                if (us_punct[i].usage == u) base = (uint16_t)(unsigned char)us_punct[i].base, shift = (uint16_t)(unsigned char)us_punct[i].shift;
        if (!base) continue;
        uint8_t *e = map + (size_t)u * SET_KEYMAP_ENTRY;
        memcpy(e, &base, 2);
        memcpy(e + 2, &shift, 2);
        e[8] = 1;
    }
    size_t n = cap < sizeof map ? cap : sizeof map;
    memcpy(out, map, n);
    return n;
}
