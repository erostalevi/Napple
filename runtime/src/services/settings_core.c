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
