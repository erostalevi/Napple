#include "host_info.h"
#include "../napple.h"
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The language: NAPPLE_LANG as given (a Switch language code), else NAPPLE_LOCALE (BCP 47), else the Mac's current locale. */
uint64_t host_desired_language(void) {
    const char *explicit_code = getenv("NAPPLE_LANG");
    char code[8] = {0};
    if (explicit_code) { if (strlen(explicit_code) > 7) runtime_fail("NAPPLE_LANG too long"); memcpy(code, explicit_code, strlen(explicit_code)); }
    else {
        char language[16] = {0}, region[4] = {0};
        const char *tag = getenv("NAPPLE_LOCALE");
        if (tag && *tag) {
            size_t n = strcspn(tag, "-_");
            if (n < sizeof language) memcpy(language, tag, n);
            for (const char *p = tag + n; *p;) {
                p += strspn(p, "-_"); size_t k = strcspn(p, "-_");
                if (k == 2 && isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1])) { region[0] = (char)toupper((unsigned char)p[0]); region[1] = (char)toupper((unsigned char)p[1]); }
                p += k;
            }
        } else {
            CFLocaleRef locale = CFLocaleCopyCurrent();
            CFStringRef v = CFLocaleGetValue(locale, kCFLocaleLanguageCode);
            if (v) CFStringGetCString(v, language, sizeof language, kCFStringEncodingUTF8);
            v = CFLocaleGetValue(locale, kCFLocaleCountryCode);
            if (v) CFStringGetCString(v, region, sizeof region, kCFStringEncodingUTF8);
            CFRelease(locale);
        }
        const char *sel = !strcmp(language, "ja") ? "ja" : !strcmp(language, "de") ? "de" : !strcmp(language, "fr") ? "fr" : !strcmp(language, "es") ? "es" :
                          !strcmp(language, "it") ? "it" : !strcmp(language, "nl") ? "nl" : !strcmp(language, "ru") ? "ru" : !strcmp(language, "ko") ? "ko" :
                          !strcmp(language, "zh") ? "zh-Hans" : !strcmp(language, "pt") ? (!strcmp(region, "BR") ? "pt-BR" : "pt") : "en-US";
        memcpy(code, sel, strlen(sel));
    }
    uint64_t v; memcpy(&v, code, 8);
    LOGV("host", "language %s", code);
    return v;
}
void host_display_version(char out[16]) { snprintf(out, 16, "%s", "1.0.0"); }
const char *host_user_name(void) { return "Napple"; }
const char *host_save_dir(void) {
    static char dir[1100];
    if (dir[0]) return dir;
    char a[1024]; mkdir(g_cfg.save_root, 0755);
    snprintf(a, sizeof a, "%s/%s", g_cfg.save_root, g_cfg.title_id); mkdir(a, 0755);
    snprintf(dir, sizeof dir, "%s/save", a); mkdir(dir, 0755);
    return dir;
}
