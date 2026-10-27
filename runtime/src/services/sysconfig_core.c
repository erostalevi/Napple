#include "sysconfig_core.h"
#include "amiibo_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int sysconfig_path(const char *support, char *out, size_t cap) {
    if (!support || !*support) return -1;
    return snprintf(out, cap, "%s/system.json", support) < (int)cap ? 0 : -1;
}

static const struct {
    const char *key, *env;
    size_t off;
} FIELDS[] = {
    {"amiibo_database", "NAPPLE_AMIIBO_DB", offsetof(SysConfig, amiibo_database)},
    {"amiibo_key", "NAPPLE_AMIIBO_KEY", offsetof(SysConfig, amiibo_key)},
    {"amiibo_store", "NAPPLE_AMIIBO_STORE", offsetof(SysConfig, amiibo_store)},
    {"online_provider", "NAPPLE_ONLINE_PROVIDER", offsetof(SysConfig, online_provider)},
    {"mii_dir", "NAPPLE_MII_DIR", offsetof(SysConfig, mii_dir)},
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])

int sysconfig_load(const char *support, SysConfig *c) {
    memset(c, 0, sizeof *c);
    char path[1100];
    if (sysconfig_path(support, path, sizeof path)) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    char *buf = malloc(65537);
    size_t n = buf ? fread(buf, 1, 65536, f) : 0;
    fclose(f);
    int rc = 0;
    AmiJVal probe;
    if (!buf || ami_jget(buf, n, "", &probe) < 0) rc = -1; /* not a JSON object */
    for (size_t i = 0; !rc && i < NFIELDS; i++) {
        AmiJVal v;
        if (ami_jget(buf, n, FIELDS[i].key, &v) == 0 && v.kind == 's') {
            if (ami_jstr(&v, (char *)c + FIELDS[i].off, 512) < 0) ((char *)c)[FIELDS[i].off] = 0;
        }
    }
    free(buf);
    if (rc) memset(c, 0, sizeof *c);
    return rc;
}

static void put_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

int sysconfig_save(const char *support, const SysConfig *c) {
    char path[1100], tmp[1120];
    if (sysconfig_path(support, path, sizeof path)) return -1;
    mkdir(support, 0755);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fputs("{\n", f);
    for (size_t i = 0; i < NFIELDS; i++) {
        fprintf(f, "  \"%s\": ", FIELDS[i].key);
        put_json_string(f, (const char *)c + FIELDS[i].off);
        fputs(i + 1 < NFIELDS ? ",\n" : "\n", f);
    }
    fputs("}\n", f);
    int ok = fflush(f) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int sysconfig_apply_env(const SysConfig *c) {
    int n = 0;
    for (size_t i = 0; i < NFIELDS; i++) {
        const char *v = (const char *)c + FIELDS[i].off;
        const char *cur = getenv(FIELDS[i].env);
        if (*v && !(cur && *cur)) {
            setenv(FIELDS[i].env, v, 1);
            n++;
        }
    }
    return n;
}
