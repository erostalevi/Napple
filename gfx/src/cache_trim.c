#include "cache_trim.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *path; uint64_t size; time_t mtime; } Entry;
typedef struct { Entry *v; size_t n, cap; uint64_t total; } List;

static int is_protected(const char *name) { return !strcmp(name, "pipelines.bin") || !strcmp(name, "pipelines-seed.bin"); }

static void collect(const char *dir, List *l, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(dir) + strlen(e->d_name) + 2;
        char *p = malloc(len);
        if (!p) break;
        snprintf(p, len, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) { if (depth < 4) collect(p, l, depth + 1); free(p); continue; }
        if (!S_ISREG(st.st_mode)) { free(p); continue; }
        l->total += (uint64_t)st.st_size;
        if (is_protected(e->d_name)) { free(p); continue; }
        if (l->n == l->cap) {
            size_t nc = l->cap ? l->cap * 2 : 256;
            Entry *nv = realloc(l->v, nc * sizeof *nv);
            if (!nv) { free(p); break; }
            l->v = nv; l->cap = nc;
        }
        l->v[l->n++] = (Entry){ p, (uint64_t)st.st_size, st.st_mtime };
    }
    closedir(d);
}

static int by_age(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

uint64_t gfx_cache_trim(const char *dir, uint64_t max_bytes, int dry_run, uint64_t *total_before) {
    List l = {0};
    if (dir) collect(dir, &l, 0);
    if (total_before) *total_before = l.total;
    uint64_t removed = 0;
    if (max_bytes && l.total > max_bytes) {
        qsort(l.v, l.n, sizeof *l.v, by_age);
        for (size_t i = 0; i < l.n && l.total - removed > max_bytes; i++) {
            if (dry_run || unlink(l.v[i].path) == 0) removed += l.v[i].size;
        }
    }
    for (size_t i = 0; i < l.n; i++) free(l.v[i].path);
    free(l.v);
    return removed;
}

uint64_t gfx_cache_max_bytes(void) {
    const char *e = getenv("NAPPLE_GFX_CACHE_MAX_MB");
    unsigned long long mb = e && *e ? strtoull(e, NULL, 10) : GFX_CACHE_DEFAULT_MAX_MB;
    return (uint64_t)mb << 20;
}
