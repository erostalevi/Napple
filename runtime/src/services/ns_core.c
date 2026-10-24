#include "ns_core.h"
#include <string.h>
#include <sys/statvfs.h>

static const char *const codes[NS_LANG_COUNT] = {"en-US", "en-GB", "ja", "fr", "de", "es-419", "es", "it", "nl", "fr-CA", "pt", "ru", "ko", "zh-Hant", "zh-Hans", "pt-BR"};

uint64_t ns_language_code(int lang) {
    if (lang < 0 || lang >= NS_LANG_COUNT) return 0;
    uint64_t v = 0; memcpy(&v, codes[lang], strlen(codes[lang])); return v;
}
int ns_language_from_code(uint64_t code) {
    for (int i = 0; i < NS_LANG_COUNT; i++) if (ns_language_code(i) == code) return i;
    return -1;
}

/* Preferred substitutes when the title does not carry the desired language, nearest first. */
static const int8_t fallback[NS_LANG_COUNT][2] = {
    [NS_LANG_AMERICAN_ENGLISH] = {NS_LANG_BRITISH_ENGLISH, -1}, [NS_LANG_BRITISH_ENGLISH] = {NS_LANG_AMERICAN_ENGLISH, -1},
    [NS_LANG_FRENCH] = {NS_LANG_CANADIAN_FRENCH, -1}, [NS_LANG_CANADIAN_FRENCH] = {NS_LANG_FRENCH, -1},
    [NS_LANG_SPANISH] = {NS_LANG_LATIN_AMERICAN_SPANISH, -1}, [NS_LANG_LATIN_AMERICAN_SPANISH] = {NS_LANG_SPANISH, -1},
    [NS_LANG_PORTUGUESE] = {NS_LANG_BRAZILIAN_PORTUGUESE, -1}, [NS_LANG_BRAZILIAN_PORTUGUESE] = {NS_LANG_PORTUGUESE, -1},
    [NS_LANG_TRADITIONAL_CHINESE] = {NS_LANG_SIMPLIFIED_CHINESE, -1}, [NS_LANG_SIMPLIFIED_CHINESE] = {NS_LANG_TRADITIONAL_CHINESE, -1},
    [NS_LANG_JAPANESE] = {-1, -1}, [NS_LANG_GERMAN] = {-1, -1}, [NS_LANG_ITALIAN] = {-1, -1}, [NS_LANG_DUTCH] = {-1, -1}, [NS_LANG_RUSSIAN] = {-1, -1}, [NS_LANG_KOREAN] = {-1, -1},
};

int ns_select_language(uint32_t supported, uint64_t desired_code) {
    if (!(supported & ((1u << NS_LANG_COUNT) - 1))) return -1;
    int want = ns_language_from_code(desired_code);
    if (want >= 0) {
        if (supported & (1u << want)) return want;
        for (int k = 0; k < 2; k++) { int f = fallback[want][k]; if (f >= 0 && (supported & (1u << f))) return f; }
    }
    if (supported & 1u) return NS_LANG_AMERICAN_ENGLISH;   /* English is the usual default */
    for (int i = 0; i < NS_LANG_COUNT; i++) if (supported & (1u << i)) return i;
    return -1;
}

uint64_t ns_free_space(const char *path) {
    struct statvfs v; if (!path || statvfs(path, &v)) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}
uint64_t ns_total_space(const char *path) {
    struct statvfs v; if (!path || statvfs(path, &v)) return 0;
    return (uint64_t)v.f_blocks * (uint64_t)v.f_frsize;
}
