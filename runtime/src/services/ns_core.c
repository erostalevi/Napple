#include "ns_core.h"
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

static const char *const codes[NS_LANG_COUNT] = {"en-US", "en-GB", "ja", "fr", "de", "es-419",  "es",      "it",
                                                 "nl",    "fr-CA", "pt", "ru", "ko", "zh-Hant", "zh-Hans", "pt-BR"};

uint64_t ns_language_code(int lang) {
    if (lang < 0 || lang >= NS_LANG_COUNT) return 0;
    uint64_t v = 0;
    memcpy(&v, codes[lang], strlen(codes[lang]));
    return v;
}
int ns_language_from_code(uint64_t code) {
    for (int i = 0; i < NS_LANG_COUNT; i++)
        if (ns_language_code(i) == code) return i;
    return -1;
}

/* Preferred substitutes when the title does not carry the desired language, nearest first. */
static const int8_t fallback[NS_LANG_COUNT][2] = {
    [NS_LANG_AMERICAN_ENGLISH] = {NS_LANG_BRITISH_ENGLISH, -1},
    [NS_LANG_BRITISH_ENGLISH] = {NS_LANG_AMERICAN_ENGLISH, -1},
    [NS_LANG_FRENCH] = {NS_LANG_CANADIAN_FRENCH, -1},
    [NS_LANG_CANADIAN_FRENCH] = {NS_LANG_FRENCH, -1},
    [NS_LANG_SPANISH] = {NS_LANG_LATIN_AMERICAN_SPANISH, -1},
    [NS_LANG_LATIN_AMERICAN_SPANISH] = {NS_LANG_SPANISH, -1},
    [NS_LANG_PORTUGUESE] = {NS_LANG_BRAZILIAN_PORTUGUESE, -1},
    [NS_LANG_BRAZILIAN_PORTUGUESE] = {NS_LANG_PORTUGUESE, -1},
    [NS_LANG_TRADITIONAL_CHINESE] = {NS_LANG_SIMPLIFIED_CHINESE, -1},
    [NS_LANG_SIMPLIFIED_CHINESE] = {NS_LANG_TRADITIONAL_CHINESE, -1},
    [NS_LANG_JAPANESE] = {-1, -1},
    [NS_LANG_GERMAN] = {-1, -1},
    [NS_LANG_ITALIAN] = {-1, -1},
    [NS_LANG_DUTCH] = {-1, -1},
    [NS_LANG_RUSSIAN] = {-1, -1},
    [NS_LANG_KOREAN] = {-1, -1},
};

int ns_select_language(uint32_t supported, uint64_t desired_code) {
    if (!(supported & ((1u << NS_LANG_COUNT) - 1))) return -1;
    int want = ns_language_from_code(desired_code);
    if (want >= 0) {
        if (supported & (1u << want)) return want;
        for (int k = 0; k < 2; k++) {
            int f = fallback[want][k];
            if (f >= 0 && (supported & (1u << f))) return f;
        }
    }
    if (supported & 1u) return NS_LANG_AMERICAN_ENGLISH; /* English is the usual default */
    for (int i = 0; i < NS_LANG_COUNT; i++)
        if (supported & (1u << i)) return i;
    return -1;
}

uint64_t ns_free_space(const char *path) {
    struct statvfs v;
    if (!path || statvfs(path, &v)) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}
uint64_t ns_total_space(const char *path) {
    struct statvfs v;
    if (!path || statvfs(path, &v)) return 0;
    return (uint64_t)v.f_blocks * (uint64_t)v.f_frsize;
}

uint64_t ns_parse_title_id(const char *hex) {
    uint64_t v = 0;
    if (!hex || strlen(hex) != 16) return 0;
    for (int i = 0; i < 16; i++) {
        char ch = hex[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    return v;
}
void ns_app_record(uint64_t id, uint8_t out[NS_APP_RECORD_SIZE]) {
    memset(out, 0, NS_APP_RECORD_SIZE);
    memcpy(out, &id, 8);
    out[8] = 3;
}

int ns_control_path(const char *data_dir, char *out, unsigned cap) {
    if (!data_dir || !*data_dir || !out || !cap) return 0;
    if (snprintf(out, cap, "%s/control.nacp", data_dir) >= (int)cap) return 0;
    FILE *f = fopen(out, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0 && n <= (long)NS_NACP_SIZE;
}
unsigned ns_control_load(const char *data_dir, uint8_t buf[NS_NACP_SIZE]) {
    char path[1400];
    if (!ns_control_path(data_dir, path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    memset(buf, 0, NS_NACP_SIZE);
    size_t n = fread(buf, 1, NS_NACP_SIZE, f);
    fclose(f);
    return n ? NS_NACP_SIZE : 0;
}
