/* romfs.map (napple build --update-romfs in-place, development data directories): the RomFS image described as
 * extents of the game files (NSP/NCA on the host) plus the AES-128-CTR parameters to decrypt them, so no RomFS copy is
 * needed on disk. Format: crates/napple-build/src/romfs_map.rs. fs.c serves it through its IStorage when romfs.bin is
 * absent. Thread-safe: pread + a per-call cryptor. */
#include "romfs_map.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#endif

typedef struct {
    uint64_t off, len, file_off, ctr;
    uint32_t file, flags;
    uint8_t hi[8], key[16];
} MapExt;
struct RomfsMap {
    uint64_t size;
    uint64_t n;
    MapExt *e;
    uint32_t nfiles;
    int *fds;
};

void romfs_map_close(RomfsMap *m) {
    if (!m) return;
    for (uint32_t i = 0; m->fds && i < m->nfiles; i++) /* fds is still NULL when the header was refused */
        if (m->fds[i] >= 0) close(m->fds[i]);
    free(m->fds);
    free(m->e);
    free(m);
}

RomfsMap *romfs_map_open(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[24];
    RomfsMap *m = calloc(1, sizeof *m);
    int ok = 0;
    if (!m) {
        fclose(f);
        return NULL;
    }
    if (fread(h, 1, 24, f) != 24 || memcmp(h, "RMP1", 4)) goto out;
    memcpy(&m->nfiles, h + 4, 4);
    memcpy(&m->n, h + 8, 8);
    memcpy(&m->size, h + 16, 8);
    if (!m->nfiles || m->nfiles > 64 || !m->n || m->n > (1u << 26)) goto out;
    struct stat st; /* every extent record is 64 bytes of the file: a count the file cannot hold would only size a huge allocation */
    if (fstat(fileno(f), &st) || (uint64_t)st.st_size < 24 || m->n > ((uint64_t)st.st_size - 24) / 64) goto out;
    m->fds = malloc(m->nfiles * sizeof *m->fds);
    for (uint32_t i = 0; i < m->nfiles; i++) m->fds[i] = -1;
    for (uint32_t i = 0; i < m->nfiles; i++) {
        uint32_t len;
        char p[4096];
        if (fread(&len, 4, 1, f) != 1 || len >= sizeof p || fread(p, 1, len, f) != len) goto out;
        p[len] = 0;
        if ((m->fds[i] = open(p, O_RDONLY)) < 0) {
            fprintf(stderr, "[fs] romfs.map: %s is missing\n", p);
            goto out;
        }
    }
    m->e = calloc(m->n, sizeof *m->e);
    uint64_t at = 0;
    for (uint64_t i = 0; i < m->n; i++) {
        uint8_t r[64];
        MapExt *e = &m->e[i];
        if (fread(r, 1, 64, f) != 64) goto out;
        memcpy(&e->off, r, 8);
        memcpy(&e->len, r + 8, 8);
        memcpy(&e->file_off, r + 16, 8);
        memcpy(&e->ctr, r + 24, 8);
        memcpy(&e->file, r + 32, 4);
        memcpy(&e->flags, r + 36, 4);
        memcpy(e->hi, r + 40, 8);
        memcpy(e->key, r + 48, 16);
        if (e->off != at || e->file >= m->nfiles || !e->len) goto out;
        at += e->len;
    }
    ok = at == m->size;
#ifndef __APPLE__
    for (uint64_t i = 0; ok && i < m->n; i++)
        if (m->e[i].flags & 1) ok = 0; /* AES-CTR needs CommonCrypto */
#endif
out:
    fclose(f);
    if (!ok) {
        romfs_map_close(m);
        return NULL;
    }
    *size = m->size;
    return m;
}

/* One extent-bounded read; returns bytes read or -1. */
ssize_t romfs_map_pread(RomfsMap *m, void *dst, size_t len, uint64_t off) {
    if (off >= m->size) return -1;
    uint64_t lo = 0, hi = m->n;
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (m->e[mid].off <= off) lo = mid;
        else hi = mid;
    }
    const MapExt *e = &m->e[lo];
    uint64_t skip = off - e->off;
    size_t n = len < e->len - skip ? len : (size_t)(e->len - skip);
    int fd = m->fds[e->file];
    if (!(e->flags & 1)) return pread(fd, dst, n, (off_t)(e->file_off + skip));
#ifdef __APPLE__
    uint64_t c = e->ctr + skip;
    size_t pre = (size_t)(c & 15);
    uint8_t stackbuf[4096];
    uint8_t *tmp = pre + n <= sizeof stackbuf ? stackbuf : malloc(pre + n);
    ssize_t got = -1;
    if (pread(fd, tmp, pre + n, (off_t)(e->file_off + skip - pre)) == (ssize_t)(pre + n)) {
        uint8_t iv[16];
        memcpy(iv, e->hi, 8);
        uint64_t blk = c >> 4;
        for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)(blk >> (8 * i));
        CCCryptorRef cr = NULL;
        size_t moved = 0;
        if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, iv, e->key, 16, NULL, 0, 0, kCCModeOptionCTR_BE, &cr) == kCCSuccess) {
            if (CCCryptorUpdate(cr, tmp, pre + n, tmp, pre + n, &moved) == kCCSuccess && moved == pre + n) {
                memcpy(dst, tmp + pre, n);
                got = (ssize_t)n;
            }
            CCCryptorRelease(cr);
        }
    }
    if (tmp != stackbuf) free(tmp);
    return got;
#else
    return -1;
#endif
}

/* ---- mod overlay ------------------------------------------------------------------------------------------------- */
typedef struct {
    char *path, *file;
    uint64_t size;
    int del;
} OvEntry;
struct RomfsOverlay {
    char *dir;
    size_t n;
    OvEntry *e;
};

void romfs_overlay_free(RomfsOverlay *o) {
    if (!o) return;
    for (size_t i = 0; i < o->n; i++) {
        free(o->e[i].path);
        free(o->e[i].file);
    }
    free(o->e);
    free(o->dir);
    free(o);
}

/* Reads the string value of "key":"..." in line (no escapes are written by the builder: it refuses quotes and backslashes). */
static char *ov_str(const char *line, const char *key) {
    char pat[16];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q) return NULL;
    char *s = malloc((size_t)(q - p) + 1);
    memcpy(s, p, (size_t)(q - p));
    s[q - p] = 0;
    return s;
}

static int ov_path_ok(const char *p, int is_file) {
    if (p[0] != '/' || (is_file && !p[1])) return 0;
    for (const char *c = p; *c; c++)
        if (c[0] == '/' && (c[1] == 0 ? c != p : 0)) return 0; /* trailing slash */
    return strstr(p, "//") == NULL && strstr(p, "/../") == NULL && strstr(p, "/./") == NULL;
}

RomfsOverlay *romfs_overlay_load(const char *data_dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/mods/overlay.json", data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    RomfsOverlay *o = calloc(1, sizeof *o);
    o->dir = strdup(data_dir);
    char line[8192];
    int ok = 1;
    size_t cap = 0;
    while (ok && fgets(line, sizeof line, f)) {
        char *del = ov_str(line, "delete"), *p = del ? NULL : ov_str(line, "path"), *file = p ? ov_str(line, "file") : NULL;
        if (!del && !p) continue; /* header / footer lines */
        const char *sz = strstr(line, "\"size\":");
        if (o->n == cap) {
            cap = cap ? cap * 2 : 64;
            o->e = realloc(o->e, cap * sizeof *o->e);
        }
        OvEntry *e = &o->e[o->n];
        memset(e, 0, sizeof *e);
        e->del = del != NULL;
        e->path = del ? del : p;
        e->file = file;
        if (sz) e->size = strtoull(sz + 7, NULL, 10);
        /* a file entry needs a relative, non-escaping host file; every path must be a clean absolute romfs path */
        if (!ov_path_ok(e->path, !e->del) || (!e->del && (!file || file[0] == '/' || strstr(file, "..")))) ok = 0;
        o->n++;
    }
    fclose(f);
    if (!ok || !o->n) {
        romfs_overlay_free(o);
        return NULL;
    }
    return o;
}

/* p is a strict path prefix of q at a component boundary. */
static int ov_under(const char *p, const char *q) {
    size_t n = strlen(p);
    return strncmp(p, q, n) == 0 && q[n] == '/';
}

int romfs_overlay_lookup(const RomfsOverlay *o, const char *path, char *host, size_t cap, uint64_t *size) {
    if (!o || !path) return ROMFS_OVERLAY_ORIGINAL;
    for (size_t i = 0; i < o->n; i++) {
        const OvEntry *e = &o->e[i];
        if (!e->del && !strcmp(e->path, path)) {
            snprintf(host, cap, "%s/%s", o->dir, e->file);
            if (size) *size = e->size;
            return ROMFS_OVERLAY_REPLACED;
        }
    }
    for (size_t i = 0; i < o->n; i++) {
        const OvEntry *e = &o->e[i];
        if (e->del && (!strcmp(e->path, path) || ov_under(e->path, path))) return ROMFS_OVERLAY_DELETED;
    }
    return ROMFS_OVERLAY_ORIGINAL;
}

int romfs_overlay_child(const RomfsOverlay *o, const char *dir_path, size_t n, char *name, size_t cap, int *is_dir) {
    if (!o) return 0;
    size_t dl = strcmp(dir_path, "/") ? strlen(dir_path) : 0;
    size_t seen = 0;
    for (size_t i = 0; i < o->n; i++) {
        const OvEntry *e = &o->e[i];
        if (e->del || strncmp(e->path, dir_path, dl) || e->path[dl] != '/') continue;
        const char *rest = e->path + dl + 1, *slash = strchr(rest, '/');
        size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
        int dup = 0; /* each child name is reported once, in first-seen order */
        for (size_t j = 0; j < i && !dup; j++) {
            const OvEntry *d = &o->e[j];
            dup = !d->del && !strncmp(d->path, dir_path, dl) && d->path[dl] == '/' && !strncmp(d->path + dl + 1, rest, len) &&
                  (d->path[dl + 1 + len] == '/' || d->path[dl + 1 + len] == 0);
        }
        if (dup) continue;
        if (seen++ != n) continue;
        snprintf(name, cap, "%.*s", (int)len, rest);
        if (is_dir) *is_dir = slash != NULL;
        return 1;
    }
    return 0;
}

/* ---- merged RomFS (virtual image = original tables + overlay) ---------------------------------------------------- */
#include <pthread.h>
#define RFS_NONE 0xFFFFFFFFu
#define RFS_HDR 0x50
#define RFS_MAX_NODES (1u << 22)
#define RFS_MAX_META (256u << 20)

typedef struct MNode {
    char *name;
    struct MNode *parent, **kid;
    size_t nkid, capkid;
    int is_dir, from_overlay;
    uint64_t size, src_off; /* file: size and offset in the ORIGINAL data area, or in the host file */
    char *host;             /* NULL = original image */
    uint64_t data_off;      /* layout: offset in the new data area */
    uint32_t meta;          /* layout: offset in the dir / file meta table */
    struct MNode *next_sib, *first_dir, *first_file; /* layout: same-kind next sibling, first child of each kind (children are sorted) */
} MNode;
typedef struct {
    uint64_t start, len, off; /* image offset, length, source offset */
    int host;                 /* index into hosts[] or -1 for the original image */
} MSeg;
struct RomfsMerged {
    uint8_t *meta; /* image bytes [0, meta_len) */
    uint64_t meta_len, size, data_base;
    MSeg *seg;
    size_t nseg;
    char **hosts;
    size_t nhosts;
    RomfsReadFn rd;
    void *ctx;
    uint64_t orig_data;
    pthread_mutex_t mu; /* fd cache */
    int fd[8];
    int fd_host[8];
    unsigned fd_next;
    unsigned replaced, added, removed, skipped;
};

static void mn_free(MNode *n) {
    if (!n) return;
    for (size_t i = 0; i < n->nkid; i++) mn_free(n->kid[i]);
    free(n->kid);
    free(n->name);
    free(n->host);
    free(n);
}
static MNode *mn_new(MNode *parent, const char *name, size_t nl, int is_dir) {
    MNode *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->name = malloc(nl + 1);
    if (!n->name) {
        free(n);
        return NULL;
    }
    memcpy(n->name, name, nl);
    n->name[nl] = 0;
    n->parent = parent;
    n->is_dir = is_dir;
    if (parent) {
        if (parent->nkid == parent->capkid) {
            size_t c = parent->capkid ? parent->capkid * 2 : 4;
            MNode **k = realloc(parent->kid, c * sizeof *k);
            if (!k) {
                free(n->name);
                free(n);
                return NULL;
            }
            parent->kid = k;
            parent->capkid = c;
        }
        parent->kid[parent->nkid++] = n;
    }
    return n;
}
static MNode *mn_find(const MNode *d, const char *name, size_t nl) {
    for (size_t i = 0; i < d->nkid; i++)
        if (strlen(d->kid[i]->name) == nl && !memcmp(d->kid[i]->name, name, nl)) return d->kid[i];
    return NULL;
}
static void mn_detach(MNode *n) {
    MNode *p = n->parent;
    for (size_t i = 0; i < p->nkid; i++)
        if (p->kid[i] == n) {
            memmove(&p->kid[i], &p->kid[i + 1], (p->nkid - i - 1) * sizeof *p->kid);
            p->nkid--;
            break;
        }
    mn_free(n);
}
static unsigned mn_count(const MNode *n) {
    unsigned c = 1;
    for (size_t i = 0; i < n->nkid; i++) c += mn_count(n->kid[i]);
    return c;
}
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
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static int rd_full(RomfsReadFn rd, void *ctx, void *dst, uint64_t len, uint64_t off) {
    for (uint64_t done = 0; done < len;) {
        ssize_t n = rd(ctx, (char *)dst + done, (size_t)(len - done), off + done);
        if (n <= 0) return 0;
        done += (uint64_t)n;
    }
    return 1;
}

/* Parses directory dir_off of the original tables into d (recursive, bounded by depth and node budget). */
static int rfs_walk(MNode *d, const uint8_t *dm, uint64_t dml, const uint8_t *fm, uint64_t fml, uint32_t dir_off, int depth, unsigned *budget) {
    if (depth > 128) return 0;
    uint32_t c = rd32(dm + dir_off + 8);
    while (c != RFS_NONE) {
        if (!*budget || (uint64_t)c + 24 > dml) return 0;
        (*budget)--;
        uint32_t nl = rd32(dm + c + 20);
        if (nl > 0x300 || (uint64_t)c + 24 + nl > dml) return 0;
        MNode *k = mn_new(d, (const char *)dm + c + 24, nl, 1);
        if (!k || !rfs_walk(k, dm, dml, fm, fml, c, depth + 1, budget)) return 0;
        c = rd32(dm + c + 4);
    }
    uint32_t f = rd32(dm + dir_off + 12);
    while (f != RFS_NONE) {
        if (!*budget || (uint64_t)f + 32 > fml) return 0;
        (*budget)--;
        uint32_t nl = rd32(fm + f + 28);
        if (nl > 0x300 || (uint64_t)f + 32 + nl > fml) return 0;
        MNode *k = mn_new(d, (const char *)fm + f + 32, nl, 0);
        if (!k) return 0;
        k->src_off = rd64(fm + f + 8);
        k->size = rd64(fm + f + 16);
        f = rd32(fm + f + 4);
    }
    return 1;
}

/* Resolves an absolute clean path to a node; *parent_out/last for creation are handled by the caller. */
static MNode *rfs_resolve(MNode *root, const char *path) {
    MNode *n = root;
    const char *p = path;
    while (*p == '/') p++;
    while (*p && n) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!n->is_dir) return NULL;
        n = mn_find(n, p, l);
        p += l;
        while (*p == '/') p++;
    }
    return n;
}

static int mn_cmp(const void *a, const void *b) {
    const MNode *x = *(MNode *const *)a, *y = *(MNode *const *)b;
    return strcmp(x->name, y->name);
}

/* Hash-table bucket count rule of the RomFS builders. */
static uint32_t rfs_buckets(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    for (;; n++)
        if (n % 2 && n % 3 && n % 5 && n % 7 && n % 11 && n % 13 && n % 17) return n;
}
static uint32_t rfs_hash(uint32_t parent, const char *name, uint32_t nl, uint32_t buckets) {
    uint32_t h = parent ^ 123456789u;
    for (uint32_t i = 0; i < nl; i++) h = ((h >> 5) | (h << 27)) ^ (uint8_t)name[i];
    return h % buckets;
}
static uint32_t al4(uint32_t v) { return (v + 3u) & ~3u; }

typedef struct {
    MNode **dirs, **files;
    uint32_t nd, nf, dsz, fsz;
} Lay;
static void rfs_collect(MNode *d, Lay *l) {
    qsort(d->kid, d->nkid, sizeof *d->kid, mn_cmp);
    MNode *last_dir = NULL, *last_file = NULL;
    for (size_t i = 0; i < d->nkid; i++) {
        MNode *k = d->kid[i];
        MNode **last = k->is_dir ? &last_dir : &last_file;
        if (*last) (*last)->next_sib = k;
        else if (k->is_dir) d->first_dir = k;
        else d->first_file = k;
        *last = k;
        if (k->is_dir) l->dirs[l->nd++] = k;
        else l->files[l->nf++] = k;
    }
    for (size_t i = 0; i < d->nkid; i++)
        if (d->kid[i]->is_dir) rfs_collect(d->kid[i], l);
}
static int rfs_host_index(RomfsMerged *m, const char *host) {
    for (size_t i = 0; i < m->nhosts; i++)
        if (!strcmp(m->hosts[i], host)) return (int)i;
    char **h = realloc(m->hosts, (m->nhosts + 1) * sizeof *h);
    if (!h) return -1;
    m->hosts = h;
    if (!(h[m->nhosts] = strdup(host))) return -1;
    return (int)m->nhosts++;
}

void romfs_merged_close(RomfsMerged *m) {
    if (!m) return;
    for (int i = 0; i < 8; i++)
        if (m->fd[i] >= 0) close(m->fd[i]);
    for (size_t i = 0; i < m->nhosts; i++) free(m->hosts[i]);
    free(m->hosts);
    free(m->seg);
    free(m->meta);
    pthread_mutex_destroy(&m->mu);
    free(m);
}
void romfs_merged_stats(const RomfsMerged *m, unsigned *replaced, unsigned *added, unsigned *removed, unsigned *skipped) {
    if (replaced) *replaced = m->replaced;
    if (added) *added = m->added;
    if (removed) *removed = m->removed;
    if (skipped) *skipped = m->skipped;
}

#define WHY(...) do { if (why && whycap) snprintf(why, whycap, __VA_ARGS__); } while (0)

RomfsMerged *romfs_merge_open(const RomfsOverlay *o, RomfsReadFn rd, void *ctx, uint64_t orig_size, uint64_t *size, char *why, size_t whycap) {
    uint8_t hdr[RFS_HDR];
    uint8_t *dm = NULL, *fm = NULL;
    MNode *root = NULL;
    RomfsMerged *m = NULL;
    Lay lay = {0};
    if (!o || !rd || orig_size < RFS_HDR || !rd_full(rd, ctx, hdr, RFS_HDR, 0) || rd64(hdr) != RFS_HDR) {
        WHY("not a RomFS image");
        return NULL;
    }
    uint64_t dmo = rd64(hdr + 0x18), dml = rd64(hdr + 0x20), fmo = rd64(hdr + 0x38), fml = rd64(hdr + 0x40), fdo = rd64(hdr + 0x48);
    if (dml < 24 || fml > RFS_MAX_META || dml > RFS_MAX_META || dmo > orig_size || dml > orig_size - dmo || fmo > orig_size || fml > orig_size - fmo || fdo > orig_size) {
        WHY("RomFS header out of range");
        return NULL;
    }
    dm = malloc(dml);
    fm = malloc(fml ? fml : 1);
    if (!dm || !fm || !rd_full(rd, ctx, dm, dml, dmo) || (fml && !rd_full(rd, ctx, fm, fml, fmo))) {
        WHY("RomFS tables unreadable");
        goto fail;
    }
    root = mn_new(NULL, "", 0, 1);
    unsigned budget = RFS_MAX_NODES;
    if (!root || !rfs_walk(root, dm, dml, fm, fml, 0, 0, &budget)) {
        WHY("RomFS tables malformed");
        goto fail;
    }
    free(dm);
    free(fm);
    dm = fm = NULL;
    m = calloc(1, sizeof *m);
    if (!m) goto fail;
    pthread_mutex_init(&m->mu, NULL);
    for (int i = 0; i < 8; i++) m->fd[i] = -1;
    m->rd = rd;
    m->ctx = ctx;
    m->orig_data = fdo;

    /* 1. deletes (a delete of the root is refused) */
    for (size_t i = 0; i < o->n; i++) {
        if (!o->e[i].del) continue;
        MNode *n = o->e[i].path[1] ? rfs_resolve(root, o->e[i].path) : NULL;
        if (n && n != root) {
            m->removed += mn_count(n);
            mn_detach(n);
        }
    }
    /* 2. replacements and additions: the first entry of a path wins, like romfs_overlay_lookup */
    for (size_t i = 0; i < o->n; i++) {
        const OvEntry *e = &o->e[i];
        if (e->del) continue;
        struct stat st;
        char host[4096];
        snprintf(host, sizeof host, "%s/%s", o->dir, e->file);
        if (stat(host, &st) || !S_ISREG(st.st_mode)) {
            m->skipped++;
            continue;
        }
        MNode *d = root;
        const char *p = e->path;
        int bad = 0, created = 0;
        while (*p == '/') p++;
        for (;;) {
            const char *s = strchr(p, '/');
            size_t l = s ? (size_t)(s - p) : strlen(p);
            MNode *k = mn_find(d, p, l);
            if (!s) { /* last component: the file */
                if (k && (k->is_dir || k->from_overlay)) {
                    bad = 1;
                    break;
                }
                if (!k) {
                    if (!(k = mn_new(d, p, l, 0))) goto fail;
                    created = 1;
                }
                free(k->host);
                k->host = strdup(host);
                k->from_overlay = 1;
                k->size = (uint64_t)st.st_size;
                k->src_off = 0;
                if (created) m->added++;
                else m->replaced++;
                break;
            }
            if (k && !k->is_dir) {
                bad = 1;
                break;
            }
            if (!k && !(k = mn_new(d, p, l, 1))) goto fail;
            d = k;
            p = s + 1;
        }
        if (bad) m->skipped++;
    }

    /* 3. layout */
    unsigned total = mn_count(root);
    lay.dirs = malloc(total * sizeof *lay.dirs);
    lay.files = malloc(total * sizeof *lay.files);
    if (!lay.dirs || !lay.files) goto fail;
    lay.dirs[lay.nd++] = root;
    rfs_collect(root, &lay);
    for (uint32_t i = 0; i < lay.nd; i++) {
        lay.dirs[i]->meta = lay.dsz;
        lay.dsz += 24 + al4((uint32_t)strlen(lay.dirs[i]->name));
    }
    for (uint32_t i = 0; i < lay.nf; i++) {
        lay.files[i]->meta = lay.fsz;
        lay.fsz += 32 + al4((uint32_t)strlen(lay.files[i]->name));
    }
    uint32_t nbd = rfs_buckets(lay.nd), nbf = rfs_buckets(lay.nf);
    uint64_t dh_off = 0x200, dm_off = dh_off + 4ull * nbd, fh_off = dm_off + lay.dsz, fm_off = fh_off + 4ull * nbf;
    uint64_t data_base = (fm_off + lay.fsz + 0x1ff) & ~0x1ffull;
    uint64_t at = 0;
    for (uint32_t i = 0; i < lay.nf; i++) {
        lay.files[i]->data_off = at;
        at = (at + lay.files[i]->size + 0xf) & ~0xfull;
    }
    uint64_t last_end = lay.nf ? lay.files[lay.nf - 1]->data_off + lay.files[lay.nf - 1]->size : 0;
    m->data_base = data_base;
    m->meta_len = data_base;
    m->size = data_base + last_end;
    m->meta = calloc(1, (size_t)data_base);
    if (!m->meta) goto fail;
    uint8_t *mb = m->meta;
    wr64(mb, RFS_HDR);
    wr64(mb + 0x08, dh_off);
    wr64(mb + 0x10, 4ull * nbd);
    wr64(mb + 0x18, dm_off);
    wr64(mb + 0x20, lay.dsz);
    wr64(mb + 0x28, fh_off);
    wr64(mb + 0x30, 4ull * nbf);
    wr64(mb + 0x38, fm_off);
    wr64(mb + 0x40, lay.fsz);
    wr64(mb + 0x48, data_base);
    for (uint32_t i = 0; i < nbd; i++) wr32(mb + dh_off + 4ull * i, RFS_NONE);
    for (uint32_t i = 0; i < nbf; i++) wr32(mb + fh_off + 4ull * i, RFS_NONE);
    for (uint32_t i = 0; i < lay.nd; i++) {
        MNode *d = lay.dirs[i];
        uint8_t *e = mb + dm_off + d->meta;
        uint32_t nl = (uint32_t)strlen(d->name), sib = d->next_sib ? d->next_sib->meta : RFS_NONE;
        uint32_t child = d->first_dir ? d->first_dir->meta : RFS_NONE, file = d->first_file ? d->first_file->meta : RFS_NONE;
        uint32_t par = d->parent ? d->parent->meta : 0, b = rfs_hash(par, d->name, nl, nbd);
        wr32(e, par);
        wr32(e + 4, sib);
        wr32(e + 8, child);
        wr32(e + 12, file);
        wr32(e + 16, rd32(mb + dh_off + 4ull * b));
        wr32(e + 20, nl);
        memcpy(e + 24, d->name, nl);
        wr32(mb + dh_off + 4ull * b, d->meta);
    }
    for (uint32_t i = 0; i < lay.nf; i++) {
        MNode *f = lay.files[i];
        uint8_t *e = mb + fm_off + f->meta;
        uint32_t nl = (uint32_t)strlen(f->name), sib = f->next_sib ? f->next_sib->meta : RFS_NONE;
        MNode *p = f->parent;
        uint32_t b = rfs_hash(p->meta, f->name, nl, nbf);
        wr32(e, p->meta);
        wr32(e + 4, sib);
        wr64(e + 8, f->data_off);
        wr64(e + 16, f->size);
        wr32(e + 24, rd32(mb + fh_off + 4ull * b));
        wr32(e + 28, nl);
        memcpy(e + 32, f->name, nl);
        wr32(mb + fh_off + 4ull * b, f->meta);
    }
    /* 4. extents, in image order (the files are sorted by data_off already) */
    m->seg = calloc(lay.nf ? lay.nf : 1, sizeof *m->seg);
    if (!m->seg) goto fail;
    for (uint32_t i = 0; i < lay.nf; i++) {
        MNode *f = lay.files[i];
        if (!f->size) continue;
        MSeg *s = &m->seg[m->nseg++];
        s->start = data_base + f->data_off;
        s->len = f->size;
        s->off = f->src_off;
        s->host = f->host ? rfs_host_index(m, f->host) : -1;
        if (f->host && s->host < 0) goto fail;
        if (!f->host && (f->src_off > orig_size || f->size > orig_size || fdo + f->src_off + f->size > orig_size)) {
            WHY("RomFS file extent past the image");
            goto fail;
        }
    }
    *size = m->size;
    free(lay.dirs);
    free(lay.files);
    mn_free(root);
    return m;
fail:
    if (why && whycap && !why[0]) WHY("out of memory");
    free(dm);
    free(fm);
    free(lay.dirs);
    free(lay.files);
    mn_free(root);
    romfs_merged_close(m);
    return NULL;
}

static ssize_t rfs_host_pread(RomfsMerged *m, int host, void *dst, size_t n, uint64_t off) {
    int fd = -1;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < 8; i++)
        if (m->fd[i] >= 0 && m->fd_host[i] == host) fd = m->fd[i];
    if (fd < 0) { /* round-robin eviction; a read racing an eviction on another thread is made safe by dup */
        unsigned slot = m->fd_next++ & 7;
        if (m->fd[slot] >= 0) close(m->fd[slot]);
        m->fd[slot] = open(m->hosts[host], O_RDONLY);
        m->fd_host[slot] = host;
        fd = m->fd[slot];
    }
    int use = fd >= 0 ? dup(fd) : -1;
    pthread_mutex_unlock(&m->mu);
    if (use < 0) return -1;
    ssize_t r = pread(use, dst, n, (off_t)off);
    close(use);
    return r;
}

ssize_t romfs_merged_pread(RomfsMerged *m, void *dst, size_t len, uint64_t off) {
    if (off >= m->size || !len) return -1;
    if (off < m->meta_len) {
        size_t n = len < m->meta_len - off ? len : (size_t)(m->meta_len - off);
        memcpy(dst, m->meta + off, n);
        return (ssize_t)n;
    }
    size_t lo = 0, hi = m->nseg;
    while (lo < hi) { /* first extent that ends after off */
        size_t mid = lo + (hi - lo) / 2;
        if (m->seg[mid].start + m->seg[mid].len <= off) lo = mid + 1;
        else hi = mid;
    }
    if (lo == m->nseg) { /* trailing padding after the last extent */
        size_t n = len < m->size - off ? len : (size_t)(m->size - off);
        memset(dst, 0, n);
        return (ssize_t)n;
    }
    const MSeg *s = &m->seg[lo];
    if (off < s->start) { /* alignment padding between files reads as zeros */
        size_t n = len < s->start - off ? len : (size_t)(s->start - off);
        memset(dst, 0, n);
        return (ssize_t)n;
    }
    uint64_t skip = off - s->start;
    size_t n = len < s->len - skip ? len : (size_t)(s->len - skip);
    if (s->host >= 0) return rfs_host_pread(m, s->host, dst, n, s->off + skip);
    return m->rd(m->ctx, dst, n, m->orig_data + s->off + skip);
}
