#include "amiibo_core.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- helpers ------------------------------------------------------------------------------------------------------------------- */
static const char HEX[] = "0123456789abcdef";
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void to_hex(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) out[2 * i] = HEX[p[i] >> 4], out[2 * i + 1] = HEX[p[i] & 15];
    out[2 * n] = 0;
}
int ami_hex_to_bytes(const char *s, size_t n, uint8_t *out) { /* n hex digits */
    for (size_t i = 0; i < n; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i / 2] = (uint8_t)(a << 4 | b);
    }
    return 0;
}
int amiibo_id_from_hex(const char *s, uint8_t id[8]) {
    if (!s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (strlen(s) != 16) return -1;
    return ami_hex_to_bytes(s, 16, id);
}
void amiibo_id_to_hex(const uint8_t id[8], char out[17]) { to_hex(id, 8, out); }
void amiibo_uid_to_hex(const uint8_t uid[7], char out[15]) { to_hex(uid, 7, out); }

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
void amiibo_new(Amiibo *a, const uint8_t id[8], uint64_t seed) {
    memset(a, 0, sizeof *a);
    memcpy(a->id, id, 8);
    uint8_t r[8];
    if (seed) {
        uint64_t s = seed, v = splitmix(&s);
        for (int i = 0; i < 8; i++) r[i] = (uint8_t)(v >> (8 * i));
    } else {
        static uint64_t nonce;
        arc4random_buf(r, sizeof r);
        uint64_t n = __atomic_add_fetch(&nonce, 1, __ATOMIC_RELAXED),
                 m = splitmix(&n); /* two amiibo made by one process never share a UID, whatever the RNG does */
        for (int i = 0; i < 8; i++) r[i] ^= (uint8_t)(m >> (8 * i));
    }
    a->uid[0] = 0x04; /* NXP */
    memcpy(a->uid + 1, r, 6);
    if (a->uid[1] == 0x88) a->uid[1] = 0x87; /* 0x88 is the cascade tag byte: never a real UID byte 0..2 */
}
void amiibo_format(Amiibo *a) {
    a->registered = 0;
    a->nickname[0] = 0;
    memset(a->owner_mii, 0, sizeof a->owner_mii);
    memset(a->owner_raw, 0, sizeof a->owner_raw);
    a->has_owner_raw = 0;
    a->register_date = a->write_date = (AmiiboDate){0};
    amiibo_area_delete(a);
}
/* keep at most 10 characters of UTF-8 */
static void copy_nick(char *dst, const char *src) {
    size_t i = 0;
    int chars = 0;
    while (src && src[i] && chars < 10) {
        size_t l = (src[i] & 0x80) == 0 ? 1 : (src[i] & 0xe0) == 0xc0 ? 2 : (src[i] & 0xf0) == 0xe0 ? 3 : (src[i] & 0xf8) == 0xf0 ? 4 : 1;
        for (size_t k = 1; k < l; k++)
            if (!src[i + k]) l = k; /* truncated sequence: drop what is there */
        if (i + l >= AMIIBO_NICK_BYTES) break;
        memcpy(dst + i, src + i, l);
        i += l;
        chars++;
    }
    dst[i] = 0;
}
void amiibo_set_register(Amiibo *a, const char *nick, const uint8_t mii[AMIIBO_MII_CHARINFO], AmiiboDate when) {
    copy_nick(a->nickname, nick ? nick : "");
    if (mii) memcpy(a->owner_mii, mii, AMIIBO_MII_CHARINFO);
    a->registered = 1;
    a->register_date = a->write_date = when;
}
int amiibo_area_create(Amiibo *a, uint32_t access_id, uint64_t app_id, const void *data, size_t n, uint8_t fill) {
    if (a->has_app_area) return -1;
    if (n > AMIIBO_APP_AREA_SIZE) return -2;
    memset(a->app_data, fill, sizeof a->app_data);
    if (n) memcpy(a->app_data, data, n);
    a->has_app_area = 1;
    a->app_access_id = access_id;
    a->app_id = app_id;
    a->app_write_counter = 0;
    return 0;
}
void amiibo_area_delete(Amiibo *a) {
    a->has_app_area = 0;
    a->app_access_id = 0;
    a->app_id = 0;
    a->app_write_counter = 0;
    memset(a->app_data, 0, sizeof a->app_data);
}

/* ---- base64 ---------------------------------------------------------------------------------------------------------------------- */
size_t ami_b64_encode(const void *in, size_t n, char *out, size_t cap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = in;
    size_t need = (n + 2) / 3 * 4, o = 0;
    if (cap < need + 1) return 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
long ami_b64_decode(const char *in, size_t n, void *out, size_t cap) {
    uint8_t *o = out;
    size_t w = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w >= cap) return -1;
            o[w++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)w;
}

/* ---- JSON ----------------------------------------------------------------------------------------------------------------------- */
static size_t put(char *out, size_t cap, size_t o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static size_t put(char *out, size_t cap, size_t o, const char *fmt, ...) {
    if (o == (size_t)-1) return o;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + o, cap > o ? cap - o : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || o + (size_t)n >= cap) return (size_t)-1;
    return o + (size_t)n;
}
static size_t put_str(char *out, size_t cap, size_t o, const char *s) { /* JSON string literal */
    o = put(out, cap, o, "\"");
    for (; *s && o != (size_t)-1; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') o = put(out, cap, o, "\\%c", c);
        else if (c < 0x20) o = put(out, cap, o, "\\u%04x", c);
        else o = put(out, cap, o, "%c", c);
    }
    return put(out, cap, o, "\"");
}
static void date_str(AmiiboDate d, char *out) {
    snprintf(out, 12, "%04u-%02u-%02u", (unsigned)d.year % 10000u, (unsigned)d.month % 100u, (unsigned)d.day % 100u);
}
size_t amiibo_to_json(const Amiibo *a, char *out, size_t cap) {
    char idh[17], uidh[15], ds[12], b64[256];
    amiibo_id_to_hex(a->id, idh);
    amiibo_uid_to_hex(a->uid, uidh);
    size_t o = 0;
    o = put(out, cap, o, "{\n  \"format\": 1,\n  \"id\": \"%s\",\n  \"uid\": \"%s\",\n  \"nickname\": ", idh, uidh);
    o = put_str(out, cap, o, a->nickname);
    ami_b64_encode(a->owner_mii, sizeof a->owner_mii, b64, sizeof b64);
    o = put(out, cap, o, ",\n  \"owner_mii\": \"%s\",\n  \"registered\": %s,\n", b64, a->registered ? "true" : "false");
    if (a->has_owner_raw) {
        ami_b64_encode(a->owner_raw, sizeof a->owner_raw, b64, sizeof b64);
        o = put(out, cap, o, "  \"owner_raw\": \"%s\",\n", b64);
    }
    date_str(a->register_date, ds);
    o = put(out, cap, o, "  \"register_date\": \"%s\",\n", ds);
    date_str(a->write_date, ds);
    o = put(out, cap, o, "  \"write_date\": \"%s\",\n  \"write_counter\": %u,\n  \"version\": %u,\n  \"font_region\": %u", ds, a->write_counter, a->version,
            a->font_region);
    if (a->has_app_area) {
        char ab[512];
        ami_b64_encode(a->app_data, sizeof a->app_data, ab, sizeof ab);
        o = put(out, cap, o,
                ",\n  \"app_area\": {\n    \"access_id\": \"%08x\",\n    \"app_id\": \"%016llx\",\n    \"write_counter\": %u,\n    \"data\": \"%s\"\n  }",
                a->app_access_id, (unsigned long long)a->app_id, a->app_write_counter, ab);
    }
    o = put(out, cap, o, "\n}\n");
    return o == (size_t)-1 ? 0 : o;
}

/* minimal object reader: finds a top-level member of the object text and returns its raw value span (AmiJVal in amiibo_core.h) */
const char *ami_jws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
    return p;
}
const char *ami_jstring_end(const char *p, const char *e) { /* p at the opening quote; returns past the closing quote */
    for (p++; p < e && *p != '"'; p++)
        if (*p == '\\') p++;
    return p < e ? p + 1 : NULL;
}
const char *ami_jskip(const char *p, const char *e, AmiJVal *v) {
    p = ami_jws(p, e);
    if (p >= e) return NULL;
    const char *s = p;
    if (*p == '"') {
        const char *q = ami_jstring_end(p, e);
        if (!q) return NULL;
        if (v) *v = (AmiJVal){s + 1, (size_t)(q - s - 2), 's'};
        return q;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') {
                p = ami_jstring_end(p, e);
                if (!p) return NULL;
                p--;
            } else if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) {
                    p++;
                    if (v) *v = (AmiJVal){s, (size_t)(p - s), *s == '{' ? 'o' : 'a'};
                    return p;
                }
            }
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    if (v) *v = (AmiJVal){s, (size_t)(p - s), 'n'};
    return p;
}
int ami_jget(const char *text, size_t n, const char *key, AmiJVal *out) {
    const char *p = ami_jws(text, text + n), *e = text + n;
    if (p >= e || *p != '{') return -1;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = ami_jws(p, e);
        if (p >= e) return -1;
        if (*p == '}') return 1; /* not found */
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') return -1;
        const char *ks = p + 1;
        p = ami_jstring_end(p, e);
        if (!p) return -1;
        size_t klen = (size_t)(p - ks - 1);
        p = ami_jws(p, e);
        if (p >= e || *p != ':') return -1;
        AmiJVal v;
        p = ami_jskip(p + 1, e, &v);
        if (!p) return -1;
        if (klen == kl && !memcmp(ks, key, kl)) {
            *out = v;
            return 0;
        }
    }
}
/* decode a JSON string span into UTF-8 (cap includes the NUL); returns the length or -1 */
long ami_jstr(const AmiJVal *v, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < v->n; i++) {
        unsigned c = (unsigned char)v->p[i];
        if (c == '\\' && i + 1 < v->n) {
            c = (unsigned char)v->p[++i];
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u' && i + 4 < v->n) {
                uint8_t b[2];
                if (ami_hex_to_bytes(v->p + i + 1, 4, b)) return -1;
                unsigned cp = (unsigned)b[0] << 8 | b[1];
                i += 4;
                char u[4];
                size_t l = cp < 0x80    ? (u[0] = (char)cp, 1)
                           : cp < 0x800 ? (u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 63)), 2)
                                        : (u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | ((cp >> 6) & 63)), u[2] = (char)(0x80 | (cp & 63)), 3);
                if (o + l >= cap) return -1;
                memcpy(out + o, u, l);
                o += l;
                continue;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return (long)o;
}
static int jnum(const AmiJVal *v, unsigned long *out) {
    char b[24];
    if (v->n == 0 || v->n >= sizeof b) return -1;
    memcpy(b, v->p, v->n);
    b[v->n] = 0;
    char *end;
    *out = strtoul(b, &end, 10);
    return *end ? -1 : 0;
}
static int jdate(const AmiJVal *v, AmiiboDate *d) {
    unsigned y, m, dd;
    char b[16];
    if (v->kind != 's' || v->n >= sizeof b) return -1;
    memcpy(b, v->p, v->n);
    b[v->n] = 0;
    if (sscanf(b, "%u-%u-%u", &y, &m, &dd) != 3 || m > 12 || dd > 31 || y > 9999) return -1;
    *d = (AmiiboDate){(uint16_t)y, (uint8_t)m, (uint8_t)dd};
    return 0;
}
int amiibo_from_json(const char *text, size_t n, Amiibo *a) {
    AmiJVal v;
    unsigned long u;
    char tmp[600];
    if (ami_jget(text, n, "id", &v) != 0) return ami_jget(text, n, "format", &v) < 0 ? -1 : -2;
    if (v.kind != 's' || v.n != 16 || ami_hex_to_bytes(v.p, 16, a->id)) return -2;
    uint8_t id[8];
    memcpy(id, a->id, 8);
    amiibo_new(a, id, 0);
    if (ami_jget(text, n, "uid", &v) == 0) {
        if (v.kind != 's' || v.n != 14 || ami_hex_to_bytes(v.p, 14, a->uid)) return -1;
    }
    if (ami_jget(text, n, "nickname", &v) == 0) {
        if (v.kind != 's' || ami_jstr(&v, tmp, sizeof tmp) < 0) return -1;
        copy_nick(a->nickname, tmp);
    }
    if (ami_jget(text, n, "owner_mii", &v) == 0 && v.kind == 's' && ami_b64_decode(v.p, v.n, a->owner_mii, sizeof a->owner_mii) < 0) return -1;
    if (ami_jget(text, n, "owner_raw", &v) == 0 && v.kind == 's') {
        if (ami_b64_decode(v.p, v.n, a->owner_raw, sizeof a->owner_raw) < 0) return -1;
        a->has_owner_raw = 1;
    }
    if (ami_jget(text, n, "registered", &v) == 0) a->registered = v.n == 4 && !memcmp(v.p, "true", 4);
    if (ami_jget(text, n, "register_date", &v) == 0 && jdate(&v, &a->register_date)) return -1;
    if (ami_jget(text, n, "write_date", &v) == 0 && jdate(&v, &a->write_date)) return -1;
    if (ami_jget(text, n, "write_counter", &v) == 0 && jnum(&v, &u) == 0) a->write_counter = (uint16_t)u;
    if (ami_jget(text, n, "version", &v) == 0 && jnum(&v, &u) == 0) a->version = (uint8_t)u;
    if (ami_jget(text, n, "font_region", &v) == 0 && jnum(&v, &u) == 0) a->font_region = (uint8_t)u;
    AmiJVal ar;
    if (ami_jget(text, n, "app_area", &ar) == 0 && ar.kind == 'o') {
        uint8_t b4[4], b8[8];
        if (ami_jget(ar.p, ar.n, "access_id", &v) != 0 || v.kind != 's' || v.n != 8 || ami_hex_to_bytes(v.p, 8, b4)) return -1;
        a->app_access_id = (uint32_t)b4[0] << 24 | (uint32_t)b4[1] << 16 | (uint32_t)b4[2] << 8 | b4[3];
        if (ami_jget(ar.p, ar.n, "app_id", &v) == 0 && v.kind == 's' && v.n == 16 && !ami_hex_to_bytes(v.p, 16, b8)) {
            a->app_id = 0;
            for (int i = 0; i < 8; i++) a->app_id = a->app_id << 8 | b8[i];
        }
        if (ami_jget(ar.p, ar.n, "write_counter", &v) == 0 && jnum(&v, &u) == 0) a->app_write_counter = (uint16_t)u;
        if (ami_jget(ar.p, ar.n, "data", &v) != 0 || v.kind != 's' || ami_b64_decode(v.p, v.n, a->app_data, sizeof a->app_data) < 0) return -1;
        a->has_app_area = 1;
    }
    return 0;
}

/* ---- store ---------------------------------------------------------------------------------------------------------------------- */
static int mkdirs(const char *dir) {
    char b[1024];
    snprintf(b, sizeof b, "%s", dir);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0755);
            *p = '/';
        }
    return mkdir(b, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
static void path_of(const char *dir, const uint8_t uid[7], char *out, size_t cap) {
    char h[15];
    amiibo_uid_to_hex(uid, h);
    snprintf(out, cap, "%s/%s.json", dir, h);
}
int amiibo_store_save(const char *dir, const Amiibo *a) {
    char json[2048], path[1100], tmp[1110];
    size_t n = amiibo_to_json(a, json, sizeof json);
    if (!n || mkdirs(dir)) return -1;
    path_of(dir, a->uid, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(json, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
static int load_file(const char *path, Amiibo *a) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    int big = !feof(f);
    fclose(f);
    if (big) return -2;
    return amiibo_from_json(buf, n, a) ? -2 : 0;
}
int amiibo_store_load(const char *dir, const uint8_t uid[7], Amiibo *a) {
    char path[1100];
    path_of(dir, uid, path, sizeof path);
    return load_file(path, a);
}
int amiibo_store_delete(const char *dir, const uint8_t uid[7]) {
    char path[1100];
    path_of(dir, uid, path, sizeof path);
    return unlink(path) == 0 ? 0 : -1;
}
size_t amiibo_store_list(const char *dir, uint8_t (*uids)[7], size_t max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        uint8_t u[7];
        if (l == 19 && !strcmp(e->d_name + 14, ".json") && !ami_hex_to_bytes(e->d_name, 14, u)) memcpy(uids[n++], u, 7);
    }
    closedir(d);
    /* directory order is arbitrary: sort so callers see a stable list */
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && memcmp(uids[j - 1], uids[j], 7) > 0; j--) {
            uint8_t t[7];
            memcpy(t, uids[j], 7);
            memcpy(uids[j], uids[j - 1], 7);
            memcpy(uids[j - 1], t, 7);
        }
    return n;
}
int amiibo_store_find_by_id(const char *dir, const uint8_t id[8], Amiibo *a) {
    static uint8_t uids[256][7];
    size_t n = amiibo_store_list(dir, uids, 256);
    for (size_t i = 0; i < n; i++) {
        Amiibo t;
        if (amiibo_store_load(dir, uids[i], &t) == 0 && !memcmp(t.id, id, 8)) {
            *a = t;
            return 0;
        }
    }
    return -1;
}

/* ---- dump import / export ------------------------------------------------------------------------------------------------------ */
int amiibo_keys_load(const char *path, AmiiboKeys *k) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t b[161];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n != 160) return -2;
    memcpy(k->data, b, 80);
    memcpy(k->tag, b + 80, 80);
    return 0;
}
static uint16_t date_pack(AmiiboDate d) { return d.year >= 2000 ? (uint16_t)(((d.year - 2000) & 0x7f) << 9 | (d.month & 15) << 5 | (d.day & 31)) : 0; }
static AmiiboDate date_unpack(uint16_t v) { return v ? (AmiiboDate){(uint16_t)(2000 + (v >> 9)), (uint8_t)(v >> 5 & 15), (uint8_t)(v & 31)} : (AmiiboDate){0}; }
static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16), be16(p + 2, v & 0xffff); }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)), be32(p + 4, (uint32_t)v); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

void amiibo_to_plain(const Amiibo *a, uint8_t p[AMIIBO_TAG_SIZE]) {
    memset(p, 0, AMIIBO_TAG_SIZE);
    p[0x28] = 0xa5;
    be16(p + 0x29, a->write_counter);
    p[0x2b] = a->version;
    p[0x2c] = (uint8_t)((a->registered ? 0x10 : 0) | (a->has_app_area ? 0x20 : 0));
    be16(p + 0x30, date_pack(a->register_date));
    be16(p + 0x32, date_pack(a->write_date));
    /* nickname: up to 10 characters as UTF-16BE (BMP only) */
    const unsigned char *s = (const unsigned char *)a->nickname;
    for (int i = 0; i < 10 && *s; i++) {
        unsigned cp = *s < 0x80             ? *s++
                      : (*s & 0xe0) == 0xc0 ? (unsigned)((s[0] & 31) << 6 | (s[1] & 63))
                                            : (unsigned)((s[0] & 15) << 12 | (s[1] & 63) << 6 | (s[2] & 63));
        if (cp >= 0x80) s += (*s & 0xe0) == 0xc0 ? 2 : 3;
        be16(p + 0x38 + 2 * i, cp);
    }
    if (a->has_owner_raw) memcpy(p + 0x4c, a->owner_raw, 0x60);
    be64(p + 0xac, a->app_id);
    be16(p + 0xb4, a->app_write_counter);
    be32(p + 0xb6, a->app_access_id);
    if (a->has_app_area) memcpy(p + 0xdc, a->app_data, AMIIBO_APP_AREA_SIZE);
    /* UID with its check bytes, then the model id and a salt derived from them (a virtual amiibo has no stored salt) */
    p[0x1d4] = a->uid[0], p[0x1d5] = a->uid[1], p[0x1d6] = a->uid[2];
    p[0x1d7] = 0x88 ^ a->uid[0] ^ a->uid[1] ^ a->uid[2];
    memcpy(p + 0x1d8, a->uid + 3, 4);
    p[0x000] = a->uid[3] ^ a->uid[4] ^ a->uid[5] ^ a->uid[6];
    memcpy(p + 0x1dc, a->id, 8);
    uint8_t seed[15];
    memcpy(seed, a->uid, 7);
    memcpy(seed + 7, a->id, 8);
    ami_sha256(seed, sizeof seed, p + 0x1e8);
}
int amiibo_from_plain(const uint8_t p[AMIIBO_TAG_SIZE], Amiibo *a) {
    if (p[0x28] != 0xa5) return -2;
    memset(a, 0, sizeof *a);
    memcpy(a->id, p + 0x1dc, 8);
    a->uid[0] = p[0x1d4], a->uid[1] = p[0x1d5], a->uid[2] = p[0x1d6];
    memcpy(a->uid + 3, p + 0x1d8, 4);
    a->write_counter = rd16(p + 0x29);
    a->version = p[0x2b];
    a->registered = (p[0x2c] & 0x10) != 0;
    a->register_date = date_unpack(rd16(p + 0x30));
    a->write_date = date_unpack(rd16(p + 0x32));
    char *o = a->nickname;
    for (int i = 0; i < 10; i++) {
        unsigned cp = rd16(p + 0x38 + 2 * i);
        if (!cp) break;
        if (cp < 0x80) *o++ = (char)cp;
        else if (cp < 0x800) *o++ = (char)(0xc0 | cp >> 6), *o++ = (char)(0x80 | (cp & 63));
        else *o++ = (char)(0xe0 | cp >> 12), *o++ = (char)(0x80 | ((cp >> 6) & 63)), *o++ = (char)(0x80 | (cp & 63));
    }
    *o = 0;
    memcpy(a->owner_raw, p + 0x4c, 0x60);
    a->has_owner_raw = 1;
    if (p[0x2c] & 0x20) {
        a->has_app_area = 1;
        a->app_id = rd64(p + 0xac);
        a->app_write_counter = rd16(p + 0xb4);
        a->app_access_id = rd32(p + 0xb6);
        memcpy(a->app_data, p + 0xdc, AMIIBO_APP_AREA_SIZE);
    }
    return 0;
}
int amiibo_import_dump(const AmiiboKeys *k, const uint8_t tag[AMIIBO_TAG_SIZE], Amiibo *a) {
    uint8_t plain[AMIIBO_TAG_SIZE];
    if (amiibo_tag_unpack(k, tag, plain)) return -1;
    return amiibo_from_plain(plain, a) ? -2 : 0;
}
void amiibo_export_dump(const AmiiboKeys *k, const Amiibo *a, uint8_t tag[AMIIBO_TAG_SIZE]) {
    uint8_t plain[AMIIBO_TAG_SIZE];
    amiibo_to_plain(a, plain);
    amiibo_tag_pack(k, plain, tag);
}

int ami_jiter(const char **cur, const char *end, AmiJVal *key, AmiJVal *val) {
    const char *p = ami_jws(*cur, end);
    if (p < end && *p == '{') p++; /* first call: step into the object */
    for (;;) {
        p = ami_jws(p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            *cur = p;
            return 0;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') return -1;
        const char *ks = p + 1;
        p = ami_jstring_end(p, end);
        if (!p) return -1;
        *key = (AmiJVal){ks, (size_t)(p - ks - 1), 's'};
        p = ami_jws(p, end);
        if (p >= end || *p != ':') return -1;
        p = ami_jskip(p + 1, end, val);
        if (!p) return -1;
        *cur = p;
        return 1;
    }
}
