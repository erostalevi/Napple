#include "bcat_core.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int valid(const char *n, int allow_dot) {
    size_t len = strnlen(n, BCAT_NAME_SIZE);
    if (len == 0 || len >= BCAT_NAME_SIZE) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (allow_dot && c == '.'))) return 0;
    }
    for (size_t i = len; i < BCAT_NAME_SIZE; i++)
        if (n[i]) return 0; /* padding must be NUL */
    return 1;
}
int bcat_valid_directory_name(const char n[BCAT_NAME_SIZE]) { return valid(n, 0); }
int bcat_valid_file_name(const char n[BCAT_NAME_SIZE]) { return valid(n, 1); }

void bcat_make_progress(uint32_t status, uint32_t result, uint8_t out[BCAT_PROGRESS_SIZE]) {
    memset(out, 0, BCAT_PROGRESS_SIZE);
    memcpy(out, &status, 4);
    memcpy(out + 4, &result, 4);
}

void bcat_make_progress_full(const BcatProgress *p, uint8_t out[BCAT_PROGRESS_SIZE]) {
    bcat_make_progress(p->status, p->result, out);
    if (p->directory) strncpy((char *)out + BCAT_PROGRESS_DIR, p->directory, BCAT_NAME_SIZE - 1);
    if (p->file) strncpy((char *)out + BCAT_PROGRESS_FILE, p->file, BCAT_NAME_SIZE - 1);
    memcpy(out + BCAT_PROGRESS_CUR_DONE, &p->current_downloaded, 8);
    memcpy(out + 0x50, &p->current_total, 8);
    memcpy(out + 0x58, &p->total_downloaded, 8);
    memcpy(out + 0x60, &p->total_total, 8);
}
void bcat_pack_entry(const BcatFile *f, uint8_t out[BCAT_ENTRY_SIZE]) {
    memset(out, 0, BCAT_ENTRY_SIZE);
    memcpy(out, f->name, BCAT_NAME_SIZE);
    memcpy(out + 0x20, &f->size, 8);
    memcpy(out + 0x28, f->digest, 16);
}

/* ---- MD5 (RFC 1321): the digest of a cached file ---- */
static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static void md5_block(uint32_t st[4], const uint8_t b[64]) {
    static const uint32_t K[64] = {0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
                                   0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
                                   0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
                                   0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                                   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
                                   0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
                                   0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16], a = st[0], bb = st[1], cc = st[2], d = st[3];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)b[i * 4] | (uint32_t)b[i * 4 + 1] << 8 | (uint32_t)b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (bb & cc) | (~bb & d), g = i;
        else if (i < 32) f = (d & bb) | (~d & cc), g = (5 * i + 1) & 15;
        else if (i < 48) f = bb ^ cc ^ d, g = (3 * i + 5) & 15;
        else f = cc ^ (bb | ~d), g = (7 * i) & 15;
        uint32_t t = d;
        d = cc;
        cc = bb;
        bb = bb + rol(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    st[0] += a, st[1] += bb, st[2] += cc, st[3] += d;
}
void bcat_md5(const void *data, size_t n, uint8_t out[16]) {
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t *p = data;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) md5_block(st, p + i);
    uint8_t tail[128] = {0};
    size_t rest = n - i;
    memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    size_t len = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) tail[len - 8 + k] = (uint8_t)(bits >> (8 * k));
    md5_block(st, tail);
    if (len == 128) md5_block(st, tail + 64);
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = (uint8_t)(st[k] >> (8 * j));
}

/* ---- provider ---- */
static BcatProvider g_provider;
static int g_have, g_env_checked;
void bcat_set_provider(const BcatProvider *p) {
    g_env_checked = 1; /* an explicit choice (even NULL) wins over the environment */
    g_have = p != NULL;
    if (p) g_provider = *p;
}
const BcatProvider *bcat_provider(void) {
    if (!g_env_checked) {
        g_env_checked = 1;
        const char *root = getenv("NAPPLE_BCAT_DIR");
        if (root && *root) {
            g_provider = bcat_auto_provider(root);
            g_have = 1;
        }
    }
    return g_have ? &g_provider : NULL;
}

typedef struct {
    char root[512];
} DirCtx;
BcatProvider bcat_auto_provider(const char *root) {
    char meta[1024];
    struct stat st;
    snprintf(meta, sizeof meta, "%s/directories.meta", root);
    return stat(meta, &st) == 0 && S_ISREG(st.st_mode) ? bcat_save_provider(root) : bcat_dir_provider(root);
}
static int path_join(char *out, size_t cap, const char *root, const char *a, const char *b) {
    int n = b ? snprintf(out, cap, "%s/%s/%s", root, a, b) : a ? snprintf(out, cap, "%s/%s", root, a) : snprintf(out, cap, "%s", root);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
static int dir_list_dirs(void *ctx, char names[][BCAT_NAME_SIZE], int max) {
    DirCtx *d = ctx;
    if (!d) return 0;
    DIR *dp = opendir(d->root);
    if (!dp) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(dp))) {
        char name[BCAT_NAME_SIZE] = {0}, path[1024];
        if (strlen(e->d_name) >= BCAT_NAME_SIZE) continue;
        strncpy(name, e->d_name, BCAT_NAME_SIZE - 1);
        struct stat st;
        if (!bcat_valid_directory_name(name) || path_join(path, sizeof path, d->root, name, NULL) || stat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        memcpy(names[n++], name, BCAT_NAME_SIZE);
    }
    closedir(dp);
    return n;
}
static int cmp_file(const void *a, const void *b) { return strncmp(((const BcatFile *)a)->name, ((const BcatFile *)b)->name, BCAT_NAME_SIZE); }
static int read_all(const char *path, uint8_t **buf, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (64l << 20)) {
        fclose(f);
        return -1;
    }
    *buf = malloc((size_t)sz + 1);
    if (!*buf || fread(*buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(*buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *size = (uint64_t)sz;
    return 0;
}
static int dir_list_files(void *ctx, const char *dir, BcatFile *out, int max) {
    DirCtx *d = ctx;
    char path[1024];
    if (!d || path_join(path, sizeof path, d->root, dir, NULL)) return -1;
    DIR *dp = opendir(path);
    if (!dp) return -1;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(dp))) {
        BcatFile f;
        memset(&f, 0, sizeof f);
        if (strlen(e->d_name) >= BCAT_NAME_SIZE) continue;
        strncpy(f.name, e->d_name, BCAT_NAME_SIZE - 1);
        struct stat st;
        uint8_t *data;
        if (!bcat_valid_file_name(f.name) || path_join(path, sizeof path, d->root, dir, f.name) || stat(path, &st) || !S_ISREG(st.st_mode) ||
            read_all(path, &data, &f.size))
            continue;
        bcat_md5(data, (size_t)f.size, f.digest);
        free(data);
        out[n++] = f;
    }
    closedir(dp);
    qsort(out, (size_t)n, sizeof *out, cmp_file);
    return n;
}
static int64_t dir_read(void *ctx, const char *dir, const char *file, uint64_t off, void *buf, size_t n) {
    DirCtx *d = ctx;
    char path[1024];
    if (!d || path_join(path, sizeof path, d->root, dir, file)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int64_t got = 0;
    if (off <= (uint64_t)INT64_MAX && fseek(f, (long)off, SEEK_SET) == 0) got = (int64_t)fread(buf, 1, n, f);
    fclose(f);
    return got;
}
/* <root>/list.msgpack, for both layouts: the delivery list a sync left beside the cache (bcat_core.h read_list). */
static int64_t root_read_list(void *ctx, void *buf, size_t n) {
    DirCtx *d = ctx;
    char path[1024];
    uint8_t *data;
    uint64_t size;
    if (!d || path_join(path, sizeof path, d->root, "list.msgpack", NULL) || read_all(path, &data, &size)) return -1;
    if (buf && n) memcpy(buf, data, (size_t)(size < n ? size : n));
    free(data);
    return (int64_t)size;
}
BcatProvider bcat_dir_provider(const char *root) {
    DirCtx *d = calloc(1, sizeof *d); /* lives as long as the provider (the process, or until the test exits) */
    if (d) snprintf(d->root, sizeof d->root, "%s", root ? root : "");
    return (BcatProvider){d, dir_list_dirs, dir_list_files, dir_read, root_read_list};
}

/* ---- the console's storage layout (bcat_core.h) ---- */
static int meta_load(const char *path, size_t entry, uint8_t **buf, int *count, int max) {
    uint64_t size;
    uint8_t *raw;
    if (read_all(path, &raw, &size)) return -1;
    uint32_t hdr = 0;
    if (size >= 4) memcpy(&hdr, raw, 4);
    if (size < 4 || hdr != BCAT_META_HEADER) {
        free(raw);
        return -1;
    }
    int n = (int)((size - 4) / entry);
    *count = n > max ? max : n;
    *buf = raw;
    return 0;
}
static int save_list_dirs(void *ctx, char names[][BCAT_NAME_SIZE], int max) {
    DirCtx *d = ctx;
    char path[1024];
    uint8_t *raw;
    int count, n = 0;
    if (!d || path_join(path, sizeof path, d->root, "directories.meta", NULL) || meta_load(path, BCAT_DIR_META_ENTRY, &raw, &count, BCAT_MAX_DIRS)) return 0;
    for (int i = 0; i < count && n < max; i++) {
        const char *name = (const char *)raw + 4 + (size_t)i * BCAT_DIR_META_ENTRY;
        if (!bcat_valid_directory_name(name)) continue;
        memcpy(names[n++], name, BCAT_NAME_SIZE);
    }
    free(raw);
    return n;
}
static int save_list_files(void *ctx, const char *dir, BcatFile *out, int max) {
    DirCtx *d = ctx;
    char sub[BCAT_NAME_SIZE + 16], path[1024];
    uint8_t *raw;
    int count, n = 0;
    if (!d || snprintf(sub, sizeof sub, "directories/%s/files.meta", dir) <= 0 || path_join(path, sizeof path, d->root, sub, NULL) ||
        meta_load(path, BCAT_FILE_META_ENTRY, &raw, &count, BCAT_MAX_FILES))
        return -1;
    for (int i = 0; i < count && n < max; i++) {
        const uint8_t *e = raw + 4 + (size_t)i * BCAT_FILE_META_ENTRY;
        BcatFile f;
        int64_t size;
        memset(&f, 0, sizeof f);
        memcpy(f.name, e, BCAT_NAME_SIZE);
        memcpy(&size, e + 0x28, 8); /* name 0x20, s64 id at 0x20, s64 size at 0x28, digest at 0x30 */
        if (!bcat_valid_file_name(f.name) || size < 0) continue;
        f.size = (uint64_t)size;
        memcpy(f.digest, e + 0x30, 16);
        out[n++] = f;
    }
    free(raw);
    return n;
}
static int64_t save_read(void *ctx, const char *dir, const char *file, uint64_t off, void *buf, size_t n) {
    DirCtx *d = ctx;
    char sub[2 * BCAT_NAME_SIZE + 32], path[1024];
    if (!d || snprintf(sub, sizeof sub, "directories/%s/files/%s", dir, file) <= 0 || path_join(path, sizeof path, d->root, sub, NULL)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int64_t got = 0;
    if (off <= (uint64_t)INT64_MAX && fseek(f, (long)off, SEEK_SET) == 0) got = (int64_t)fread(buf, 1, n, f);
    fclose(f);
    return got;
}
BcatProvider bcat_save_provider(const char *root) {
    DirCtx *d = calloc(1, sizeof *d);
    if (d) snprintf(d->root, sizeof d->root, "%s", root ? root : "");
    return (BcatProvider){d, save_list_dirs, save_list_files, save_read, root_read_list};
}

/* ---- update fingerprint (bcat_core.h) ---- */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static int cmp_name(const void *a, const void *b) { return strncmp(a, b, BCAT_NAME_SIZE); }
uint64_t bcat_storage_fingerprint(const BcatProvider *p) {
    if (!p || !p->list_dirs) return 0;
    char names[BCAT_MAX_DIRS][BCAT_NAME_SIZE];
    BcatFile files[BCAT_MAX_FILES];
    memset(names, 0, sizeof names);
    int n = p->list_dirs(p->ctx, names, BCAT_MAX_DIRS);
    if (n < 0) n = 0;
    qsort(names, (size_t)n, BCAT_NAME_SIZE, cmp_name);
    uint64_t h = fnv(0xcbf29ce484222325ull, &n, sizeof n);
    for (int i = 0; i < n; i++) {
        h = fnv(h, names[i], BCAT_NAME_SIZE);
        int m = p->list_files ? p->list_files(p->ctx, names[i], files, BCAT_MAX_FILES) : -1;
        if (m < 0) m = -1;
        h = fnv(h, &m, sizeof m);
        if (m > 0) qsort(files, (size_t)m, sizeof *files, cmp_file);
        for (int k = 0; k < m; k++) {
            h = fnv(h, files[k].name, BCAT_NAME_SIZE);
            h = fnv(h, &files[k].size, sizeof files[k].size);
            h = fnv(h, files[k].digest, sizeof files[k].digest);
        }
    }
    return h ? h : 1; /* 0 stays "no provider" */
}

/* ---- local delivery-task state ---- */
static BcatTimer g_timers[BCAT_MAX_TIMERS];
static int g_ntimers, g_stopped, g_nlog;
static BcatPushLogEntry g_log[BCAT_MAX_PUSH_LOG];
static uint32_t g_gen;
static BcatTimer *timer_find(uint64_t key, int system) {
    for (int i = 0; i < g_ntimers; i++)
        if (g_timers[i].key == key && g_timers[i].system == (system != 0)) return &g_timers[i];
    return NULL;
}
int bcat_timer_set(uint64_t key, int system, uint32_t seconds) {
    BcatTimer *t = timer_find(key, system);
    if (!seconds) {
        if (t) { *t = g_timers[--g_ntimers]; g_gen++; }
        return 0;
    }
    if (!t) {
        if (g_ntimers == BCAT_MAX_TIMERS) return -1;
        t = &g_timers[g_ntimers++];
        t->key = key;
        t->system = system != 0;
    }
    t->seconds = seconds;
    t->subscribed = 1;
    g_stopped = 0;
    g_gen++;
    return 0;
}
int bcat_timer_get(uint64_t key, int system, uint32_t *seconds, int *subscribed) {
    const BcatTimer *t = timer_find(key, system);
    if (!t) return 0;
    if (seconds) *seconds = t->seconds;
    if (subscribed) *subscribed = t->subscribed;
    return 1;
}
int bcat_timer_count(void) { return g_ntimers; }
int bcat_subscription_clear(uint64_t key) {
    int n = 0;
    for (int i = 0; i < g_ntimers; i++)
        if (g_timers[i].key == key && g_timers[i].subscribed) { g_timers[i].subscribed = 0; n++; }
    if (n) g_gen++;
    return n;
}
int bcat_register_system_tasks(const uint64_t *ids, int n) {
    int done = 0;
    for (int i = 0; i < n; i++) {
        BcatTimer *t = timer_find(ids[i], 1);
        if (!t) {
            if (g_ntimers == BCAT_MAX_TIMERS) break;
            t = &g_timers[g_ntimers++];
            t->key = ids[i];
            t->system = 1;
            t->seconds = 3600; /* default interval of a task registered without an explicit timer [unverified] */
        }
        t->subscribed = 1;
        done++;
    }
    if (done) { g_stopped = 0; g_gen++; }
    return done;
}
void bcat_background_stop(void) {
    g_ntimers = 0;
    g_stopped = 1;
    g_gen++;
}
int bcat_background_stopped(void) { return g_stopped; }
void bcat_push_log_add(uint64_t app_id, uint64_t tick, uint32_t kind) {
    if (g_nlog == BCAT_MAX_PUSH_LOG) {
        memmove(g_log, g_log + 1, (BCAT_MAX_PUSH_LOG - 1) * sizeof g_log[0]);
        g_nlog--;
    }
    g_log[g_nlog++] = (BcatPushLogEntry){app_id, tick, kind};
    g_gen++;
}
int bcat_push_log_count(void) { return g_nlog; }
int bcat_push_log_get(int i, BcatPushLogEntry *out) {
    if (i < 0 || i >= g_nlog) return 0;
    *out = g_log[i];
    return 1;
}
void bcat_pack_push_log(const BcatPushLogEntry *e, uint8_t out[BCAT_PUSH_LOG_ENTRY_SIZE]) {
    memset(out, 0, BCAT_PUSH_LOG_ENTRY_SIZE);
    memcpy(out, &e->app_id, 8);
    memcpy(out + 8, &e->tick, 8);
    memcpy(out + 16, &e->kind, 4);
}
void bcat_storage_usage(int64_t *bytes, int32_t *dirs, int32_t *files) {
    *bytes = 0;
    *dirs = *files = 0;
    const BcatProvider *p = bcat_provider();
    if (!p || !p->list_dirs || !p->list_files) return;
    static char names[BCAT_MAX_DIRS][BCAT_NAME_SIZE];
    static BcatFile fl[BCAT_MAX_FILES]; /* serialized by the single service thread */
    int nd = p->list_dirs(p->ctx, names, BCAT_MAX_DIRS);
    if (nd < 0) nd = 0;
    for (int i = 0; i < nd; i++) {
        int nf = p->list_files(p->ctx, names[i], fl, BCAT_MAX_FILES);
        if (nf < 0) continue;
        (*dirs)++;
        for (int j = 0; j < nf; j++) { (*files)++; *bytes += (int64_t)fl[j].size; }
    }
}
void bcat_pack_usage(int64_t bytes, int32_t dirs, int32_t files, uint8_t out[BCAT_USAGE_SIZE]) {
    memcpy(out, &bytes, 8);
    memcpy(out + 8, &dirs, 4);
    memcpy(out + 12, &files, 4);
}
uint32_t bcat_state_generation(void) { return g_gen; }
void bcat_state_reset(void) {
    g_ntimers = g_stopped = g_nlog = 0;
    g_gen = 0;
}
