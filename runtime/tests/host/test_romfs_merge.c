/* Merged RomFS (mod overlay): builds a synthetic RomFS image, merges overlay.json, then reads the result the way a guest does (hash-table
 * path lookup, child/sibling directory listing, file reads). Portable: runs on any POSIX host. */
#include "../check.h"
#include "romfs_map.h"
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- independent minimal RomFS builder (flat sorted arrays, no sharing with the code under test) ---- */
typedef struct {
    const char *path, *data;
} SF;
static uint8_t *g_img;
static uint64_t g_len;
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t hsh(uint32_t par, const char *n, size_t l, uint32_t b) {
    uint32_t h = par ^ 123456789u;
    for (size_t i = 0; i < l; i++) h = ((h >> 5) | (h << 27)) ^ (uint8_t)n[i];
    return h % b;
}
/* Fixed shape: dirs /, /a, /a/b, /c ; the caller lists files with their dir index. */
static const char *DN[] = {"", "a", "b", "c"};
static const int DPAR[] = {0, 0, 1, 0};
typedef struct { int dir; const char *name, *data; } BF;
static void build(const BF *f, int nf) {
    uint32_t doff[4], dsz = 0;
    for (int i = 0; i < 4; i++) { doff[i] = dsz; dsz += 24 + (((uint32_t)strlen(DN[i]) + 3) & ~3u); }
    uint32_t foff[16], fsz = 0;
    for (int i = 0; i < nf; i++) { foff[i] = fsz; fsz += 32 + (((uint32_t)strlen(f[i].name) + 3) & ~3u); }
    uint32_t nb = 3; /* few entries: bucket count 3 for dirs; files use 7 */
    uint32_t nbf = 7;
    uint64_t dh = 0x200, dm = dh + 4 * nb, fh = dm + dsz, fm = fh + 4 * nbf, base = (fm + fsz + 0x1ff) & ~0x1ffull;
    uint64_t at = 0, doffs[16];
    for (int i = 0; i < nf; i++) { doffs[i] = at; at = (at + strlen(f[i].data) + 15) & ~15ull; }
    g_len = base + at;
    g_img = calloc(1, g_len);
    w64(g_img, 0x50); w64(g_img + 8, dh); w64(g_img + 16, 4 * nb); w64(g_img + 24, dm); w64(g_img + 32, dsz);
    w64(g_img + 40, fh); w64(g_img + 48, 4 * nbf); w64(g_img + 56, fm); w64(g_img + 64, fsz); w64(g_img + 72, base);
    for (uint32_t i = 0; i < nb; i++) w32(g_img + dh + 4 * i, 0xFFFFFFFF);
    for (uint32_t i = 0; i < nbf; i++) w32(g_img + fh + 4 * i, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) {
        uint8_t *e = g_img + dm + doff[i];
        uint32_t sib = 0xFFFFFFFF, ch = 0xFFFFFFFF, fl = 0xFFFFFFFF;
        for (int j = i + 1; j < 4; j++) if (DPAR[j] == DPAR[i] && i) { sib = doff[j]; break; }
        for (int j = 1; j < 4; j++) if (DPAR[j] == i && j != i) { ch = doff[j]; break; }
        for (int j = 0; j < nf; j++) if (f[j].dir == i) { fl = foff[j]; break; }
        uint32_t nl = (uint32_t)strlen(DN[i]), b = hsh(doff[DPAR[i]], DN[i], nl, nb);
        w32(e, doff[DPAR[i]]); w32(e + 4, sib); w32(e + 8, ch); w32(e + 12, fl); w32(e + 16, r32(g_img + dh + 4 * b)); w32(e + 20, nl);
        memcpy(e + 24, DN[i], nl);
        w32(g_img + dh + 4 * b, doff[i]);
    }
    for (int i = 0; i < nf; i++) {
        uint8_t *e = g_img + fm + foff[i];
        uint32_t sib = 0xFFFFFFFF, nl = (uint32_t)strlen(f[i].name), b = hsh(doff[f[i].dir], f[i].name, nl, nbf);
        for (int j = i + 1; j < nf; j++) if (f[j].dir == f[i].dir) { sib = foff[j]; break; }
        w32(e, doff[f[i].dir]); w32(e + 4, sib); w64(e + 8, doffs[i]); w64(e + 16, strlen(f[i].data));
        w32(e + 24, r32(g_img + fh + 4 * b)); w32(e + 28, nl); memcpy(e + 32, f[i].name, nl);
        w32(g_img + fh + 4 * b, foff[i]);
        memcpy(g_img + base + doffs[i], f[i].data, strlen(f[i].data));
    }
}
static ssize_t rd_img(void *ctx, void *dst, size_t len, uint64_t off) {
    (void)ctx;
    if (off >= g_len) return -1;
    size_t n = len < g_len - off ? len : (size_t)(g_len - off);
    memcpy(dst, g_img + off, n);
    return (ssize_t)n;
}

/* ---- guest-style reader over the merged image ---- */
static uint8_t *M;
static uint64_t MS;
static int load_all(RomfsMerged *m, uint64_t size) {
    M = malloc(size);
    MS = size;
    for (uint64_t off = 0; off < size;) {
        size_t chunk = (size - off) > 37 ? 37 : (size_t)(size - off); /* odd chunk sizes exercise extent boundaries */
        ssize_t n = romfs_merged_pread(m, M + off, chunk, off);
        if (n <= 0) return 0;
        off += (uint64_t)n;
    }
    return 1;
}
typedef struct { uint64_t dh, dhs, dm, fh, fhs, fm, base; } H;
static H hd(void) {
    H h = {r64(M + 8), r64(M + 16) / 4, r64(M + 24), r64(M + 40), r64(M + 48) / 4, r64(M + 56), r64(M + 72)};
    return h;
}
static int find_dir(uint32_t par, const char *n, size_t l, uint32_t *out) {
    H h = hd();
    uint32_t c = r32(M + h.dh + 4 * (hsh(par, n, l, (uint32_t)h.dhs)));
    while (c != 0xFFFFFFFF) {
        const uint8_t *e = M + h.dm + c;
        if (r32(e) == par && r32(e + 20) == l && !memcmp(e + 24, n, l)) { *out = c; return 1; }
        c = r32(e + 16);
    }
    return 0;
}
static const uint8_t *find_file(const char *path) { /* path like /a/b/x.bin */
    H h = hd();
    uint32_t dir = 0;
    const char *p = path + 1;
    for (;;) {
        const char *s = strchr(p, '/');
        if (!s) break;
        if (!find_dir(dir, p, (size_t)(s - p), &dir)) return NULL;
        p = s + 1;
    }
    size_t l = strlen(p);
    uint32_t c = r32(M + h.fh + 4 * (hsh(dir, p, l, (uint32_t)h.fhs)));
    while (c != 0xFFFFFFFF) {
        const uint8_t *e = M + h.fm + c;
        if (r32(e) == dir && r32(e + 28) == l && !memcmp(e + 32, p, l)) return e;
        c = r32(e + 24);
    }
    return NULL;
}
static int file_is(const char *path, const char *want) {
    const uint8_t *e = find_file(path);
    if (!e) return 0;
    H h = hd();
    return r64(e + 16) == strlen(want) && !memcmp(M + h.base + r64(e + 8), want, strlen(want));
}
/* directory listing "d:name f:name ..." in chain order */
static void list(const char *dirpath, char *out, size_t cap) {
    H h = hd();
    uint32_t dir = 0;
    const char *p = dirpath;
    while (*p == '/') p++;
    while (*p) {
        const char *s = strchr(p, '/');
        size_t l = s ? (size_t)(s - p) : strlen(p);
        if (!find_dir(dir, p, l, &dir)) { snprintf(out, cap, "<none>"); return; }
        p += l;
        while (*p == '/') p++;
    }
    out[0] = 0;
    for (uint32_t c = r32(M + h.dm + dir + 8); c != 0xFFFFFFFF; c = r32(M + h.dm + c + 4))
        snprintf(out + strlen(out), cap - strlen(out), "d:%.*s ", (int)r32(M + h.dm + c + 20), M + h.dm + c + 24);
    for (uint32_t c = r32(M + h.dm + dir + 12); c != 0xFFFFFFFF; c = r32(M + h.fm + c + 4))
        snprintf(out + strlen(out), cap - strlen(out), "f:%.*s ", (int)r32(M + h.fm + c + 28), M + h.fm + c + 32);
}
static void put(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    fputs(body, f);
    fclose(f);
}

int main(void) {
    BF files[] = {{0, "root.txt", "ROOT"}, {1, "keep.bin", "KEEP-A"}, {1, "old.bin", "OLD"}, {2, "deep.bin", "DEEP"}, {3, "gone.bin", "GONE"}, {3, "same.bin", "SAME"}};
    build(files, 6);
    char dir[] = "/tmp/napple-merge-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char p[4096];
    snprintf(p, sizeof p, "%s/mods", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files/0", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files/0/a", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files/0/n", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files/0/n/m", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mods/files/0/a/old.bin", dir); put(p, "REPLACED-LONGER-CONTENT-0123456789");
    snprintf(p, sizeof p, "%s/mods/files/0/a/new.bin", dir); put(p, "NEW");
    snprintf(p, sizeof p, "%s/mods/files/0/n/m/x.bin", dir); put(p, "X");
    snprintf(p, sizeof p, "%s/mods/files/0/c.txt", dir); put(p, "UNDER-DELETED");
    snprintf(p, sizeof p, "%s/mods/overlay.json", dir);
    put(p, "{\"format\":1,\"entries\":[\n"
           "{\"path\":\"/a/old.bin\",\"file\":\"mods/files/0/a/old.bin\",\"size\":34},\n"
           "{\"path\":\"/a/new.bin\",\"file\":\"mods/files/0/a/new.bin\",\"size\":3},\n"
           "{\"path\":\"/n/m/x.bin\",\"file\":\"mods/files/0/n/m/x.bin\",\"size\":1},\n"
           "{\"path\":\"/a/b\",\"file\":\"mods/files/0/a/new.bin\",\"size\":3},\n"
           "{\"path\":\"/root.txt/x\",\"file\":\"mods/files/0/c.txt\",\"size\":13},\n"
           "{\"path\":\"/missing.bin\",\"file\":\"mods/files/0/nope.bin\",\"size\":3},\n"
           "{\"delete\":\"/c\"},\n"
           "{\"path\":\"/c/back.txt\",\"file\":\"mods/files/0/c.txt\",\"size\":13},\n"
           "{\"delete\":\"/a/b\"}\n"
           "]}\n");
    RomfsOverlay *o = romfs_overlay_load(dir);
    CHECK(o != NULL, "overlay loads");

    char why[128] = "";
    uint64_t sz = 0;
    RomfsMerged *m = romfs_merge_open(o, rd_img, NULL, g_len, &sz, why, sizeof why);
    CHECK(m != NULL, "merge: %s", why);
    if (!m) return 1;
    CHECK(load_all(m, sz), "whole image reads");
    unsigned rep, add, rem, skip;
    romfs_merged_stats(m, &rep, &add, &rem, &skip);
    CHECK(rep == 1, "1 replaced, got %u", rep);
    CHECK(add == 4, "4 added (a/new.bin, n/m/x.bin, c/back.txt, a/b as a file after its directory was deleted), got %u", add);
    CHECK(rem == 5, "removed /c (3 nodes) and /a/b (2 nodes), got %u", rem);
    CHECK(skip == 2, "skipped: missing host file + path below a file, got %u", skip);
    /* deletes run first, so the file added at /a/b (a directory in the original) replaces the deleted directory */

    CHECK(file_is("/root.txt", "ROOT"), "untouched root file");
    CHECK(file_is("/a/keep.bin", "KEEP-A"), "untouched file next to a replaced one");
    CHECK(file_is("/a/old.bin", "REPLACED-LONGER-CONTENT-0123456789"), "replaced (longer) file");
    CHECK(file_is("/a/new.bin", "NEW"), "added file");
    CHECK(file_is("/n/m/x.bin", "X"), "added file in new directories");
    CHECK(file_is("/c/back.txt", "UNDER-DELETED"), "replacement under a deleted subtree wins");
    CHECK(find_file("/c/gone.bin") == NULL && find_file("/c/same.bin") == NULL, "deleted subtree files are gone");
    CHECK(find_file("/a/b/deep.bin") == NULL && file_is("/a/b", "NEW"), "deleted directory /a/b is gone, a file took its name");
    CHECK(find_file("/missing.bin") == NULL && find_file("/nope.bin") == NULL, "unreadable host file is skipped");
    char ls[256];
    list("/", ls, sizeof ls);
    CHECK(!strcmp(ls, "d:a d:c d:n f:root.txt "), "root listing: '%s'", ls);
    list("/a", ls, sizeof ls);
    CHECK(!strcmp(ls, "f:b f:keep.bin f:new.bin f:old.bin "), "/a listing (sorted): '%s'", ls);
    list("/n/m", ls, sizeof ls);
    CHECK(!strcmp(ls, "f:x.bin "), "/n/m listing: '%s'", ls);
    list("/c", ls, sizeof ls);
    CHECK(!strcmp(ls, "f:back.txt "), "/c listing: '%s'", ls);

    /* ranged reads straddling the metadata/data boundary and past the end */
    uint8_t buf[64];
    ssize_t n = romfs_merged_pread(m, buf, sizeof buf, sz - 1);
    CHECK(n == 1, "read of the last byte returns 1, got %zd", n);
    CHECK(romfs_merged_pread(m, buf, 8, sz) < 0, "read at the end fails");
    romfs_merged_close(m);
    romfs_overlay_free(o);
    free(M);

    /* a delete-only overlay, and an original that is not a RomFS */
    snprintf(p, sizeof p, "%s/mods/overlay.json", dir);
    put(p, "{\"entries\":[\n{\"delete\":\"/root.txt\"}\n]}\n");
    o = romfs_overlay_load(dir);
    m = romfs_merge_open(o, rd_img, NULL, g_len, &sz, why, sizeof why);
    CHECK(m && load_all(m, sz), "delete-only merge");
    CHECK(find_file("/root.txt") == NULL && file_is("/a/keep.bin", "KEEP-A") && file_is("/c/same.bin", "SAME"), "only the deleted file is gone");
    romfs_merged_close(m);
    free(M);
    uint8_t junk[256] = {1, 2, 3};
    uint8_t *saved = g_img; uint64_t savedlen = g_len;
    g_img = junk; g_len = sizeof junk;
    CHECK(romfs_merge_open(o, rd_img, NULL, g_len, &sz, why, sizeof why) == NULL && why[0], "junk image is refused: %s", why);
    w64(junk, 0x50); w64(junk + 0x18, 0xFFFFFFFFFFull); w64(junk + 0x20, 64);
    CHECK(romfs_merge_open(o, rd_img, NULL, g_len, &sz, why, sizeof why) == NULL, "out-of-range tables are refused");
    g_img = saved; g_len = savedlen;
    /* hostile original: random byte flips in the header and tables must never crash or hang, whatever the merge decides */
    g_img = saved; g_len = savedlen;
    romfs_overlay_free(o);
    o = romfs_overlay_load(dir);
    uint64_t seed = 0x1234567ull;
    for (int i = 0; i < 4000; i++) {
        uint8_t *copy = malloc(g_len);
        memcpy(copy, saved, g_len);
        for (int k = 0; k < 1 + i % 6; k++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            copy[(seed >> 33) % 0x400] = (uint8_t)(seed >> 20);
        }
        g_img = copy;
        RomfsMerged *fm = romfs_merge_open(o, rd_img, NULL, g_len, &sz, why, sizeof why);
        if (fm) {
            uint8_t b[97];
            for (uint64_t off = 0; off < sz; off += 97) romfs_merged_pread(fm, b, sizeof b, off);
            romfs_merged_close(fm);
        }
        g_img = saved;
        free(copy);
    }
    romfs_overlay_free(o);
    printf("test_romfs_merge: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
