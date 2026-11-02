#include "romfs_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct RomfsImg {
    RomfsReadFn rd;
    void *ctx;
    uint64_t size, data_off;
    uint8_t *dirs, *files;
    uint64_t dirs_size, files_size;
};

#define NONE 0xffffffffu
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static int read_exact(RomfsReadFn rd, void *ctx, void *dst, size_t len, uint64_t off) {
    for (size_t done = 0; done < len;) {
        ssize_t n = rd(ctx, (uint8_t *)dst + done, len - done, off + done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
RomfsImg *romfs_img_open(RomfsReadFn rd, void *ctx, uint64_t size) {
    uint8_t h[0x50];
    if (size < sizeof h || read_exact(rd, ctx, h, sizeof h, 0)) return NULL;
    if (rd64(h) != 0x50) return NULL;
    uint64_t dm = rd64(h + 0x18), dms = rd64(h + 0x20), fm = rd64(h + 0x38), fms = rd64(h + 0x40), data = rd64(h + 0x48);
    const uint64_t cap = 256ull << 20; /* table size sanity bound */
    if (!dms || dms > cap || fms > cap || dm > size || dms > size - dm || fm > size || fms > size - fm || data > size) return NULL;
    RomfsImg *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->rd = rd;
    r->ctx = ctx;
    r->size = size;
    r->data_off = data;
    r->dirs_size = dms;
    r->files_size = fms;
    r->dirs = malloc((size_t)dms);
    r->files = malloc(fms ? (size_t)fms : 1);
    if (!r->dirs || !r->files || read_exact(rd, ctx, r->dirs, (size_t)dms, dm) || (fms && read_exact(rd, ctx, r->files, (size_t)fms, fm)) || dms < 24) {
        romfs_img_close(r);
        return NULL;
    }
    return r;
}
void romfs_img_close(RomfsImg *img) {
    if (!img) return;
    free(img->dirs);
    free(img->files);
    free(img);
}
/* Bounds-checked entry accessors: a malformed table yields a name that never matches rather than a read past the copy. */
static int dir_ok(const RomfsImg *r, uint32_t o) { return o != NONE && (uint64_t)o + 24 <= r->dirs_size && (uint64_t)o + 24 + rd32(r->dirs + o + 20) <= r->dirs_size; }
static int file_ok(const RomfsImg *r, uint32_t o) { return o != NONE && (uint64_t)o + 32 <= r->files_size && (uint64_t)o + 32 + rd32(r->files + o + 28) <= r->files_size; }
static int name_is(const uint8_t *name, uint32_t len, const char *s, size_t n) { return len == n && !memcmp(name, s, n); }

/* Resolves the directory entry for path[0..n). Returns its table offset or NONE. */
static uint32_t find_dir(const RomfsImg *r, const char *path) {
    uint32_t cur = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        uint32_t c = dir_ok(r, cur) ? rd32(r->dirs + cur + 8) : NONE, hit = NONE;
        for (unsigned guard = 0; dir_ok(r, c) && guard < 1u << 20; guard++) {
            if (name_is(r->dirs + c + 24, rd32(r->dirs + c + 20), p, len)) {
                hit = c;
                break;
            }
            c = rd32(r->dirs + c + 4);
        }
        if (hit == NONE) return NONE;
        cur = hit;
        if (!e) break;
        p = e;
        while (*p == '/') p++;
    }
    return dir_ok(r, cur) ? cur : NONE;
}
int romfs_img_lookup(const RomfsImg *r, const char *path, uint64_t *off, uint64_t *size) {
    if (off) *off = 0;
    if (size) *size = 0;
    if (!r || !path) return ROMFS_NONE;
    size_t n = strlen(path);
    while (n && path[n - 1] == '/') n--;
    char buf[0x302];
    if (n >= sizeof buf) return ROMFS_NONE;
    memcpy(buf, path, n);
    buf[n] = 0;
    uint32_t d = find_dir(r, buf);
    if (d != NONE) return ROMFS_DIR;
    char *slash = strrchr(buf, '/');
    const char *leaf = slash ? slash + 1 : buf;
    if (!*leaf) return ROMFS_NONE;
    uint32_t parent = 0;
    if (slash) {
        *slash = 0;
        parent = find_dir(r, buf);
        if (parent == NONE) return ROMFS_NONE;
    }
    uint32_t f = dir_ok(r, parent) ? rd32(r->dirs + parent + 12) : NONE;
    for (unsigned guard = 0; file_ok(r, f) && guard < 1u << 22; guard++) {
        if (name_is(r->files + f + 32, rd32(r->files + f + 28), leaf, strlen(leaf))) {
            if (off) *off = rd64(r->files + f + 8);
            if (size) *size = rd64(r->files + f + 16);
            return ROMFS_FILE;
        }
        f = rd32(r->files + f + 4);
    }
    return ROMFS_NONE;
}
int romfs_img_child(const RomfsImg *r, const char *dir, unsigned n, char *name, size_t cap, int *is_dir, uint64_t *size) {
    if (!r || !dir || !cap) return 0;
    size_t dl = strlen(dir);
    while (dl && dir[dl - 1] == '/') dl--;
    char buf[0x302];
    if (dl >= sizeof buf) return 0;
    memcpy(buf, dir, dl);
    buf[dl] = 0;
    uint32_t d = find_dir(r, buf);
    if (d == NONE) return 0;
    unsigned i = 0;
    uint32_t c = rd32(r->dirs + d + 8);
    for (unsigned guard = 0; dir_ok(r, c) && guard < 1u << 20; guard++, c = rd32(r->dirs + c + 4)) {
        if (i++ != n) continue;
        uint32_t len = rd32(r->dirs + c + 20);
        snprintf(name, cap, "%.*s", (int)len, (const char *)r->dirs + c + 24);
        if (is_dir) *is_dir = 1;
        if (size) *size = 0;
        return 1;
    }
    uint32_t f = rd32(r->dirs + d + 12);
    for (unsigned guard = 0; file_ok(r, f) && guard < 1u << 22; guard++, f = rd32(r->files + f + 4)) {
        if (i++ != n) continue;
        uint32_t len = rd32(r->files + f + 28);
        snprintf(name, cap, "%.*s", (int)len, (const char *)r->files + f + 32);
        if (is_dir) *is_dir = 0;
        if (size) *size = rd64(r->files + f + 16);
        return 1;
    }
    return 0;
}
ssize_t romfs_img_read(const RomfsImg *r, uint64_t off, uint64_t fsize, uint64_t pos, void *dst, size_t len) {
    if (!r) return -1;
    if (pos >= fsize) return 0;
    if (len > fsize - pos) len = (size_t)(fsize - pos);
    uint64_t at = r->data_off + off + pos;
    if (at > r->size || len > r->size - at) return -1;
    return read_exact(r->rd, r->ctx, dst, len, at) ? -1 : (ssize_t)len;
}
