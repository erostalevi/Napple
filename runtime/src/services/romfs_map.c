/* romfs.map (napple build --update-romfs in-place, development data directories): the RomFS image described as
 * extents of the game files (NSP/NCA on the host) plus the AES-128-CTR parameters to decrypt them, so no RomFS copy is
 * needed on disk. Format: crates/napple-build/src/romfs_map.rs. fs.c serves it through its IStorage when romfs.bin is
 * absent. Thread-safe: pread + a per-call cryptor. */
#include "romfs_map.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    for (uint32_t i = 0; i < m->nfiles; i++)
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
    if (fread(h, 1, 24, f) != 24 || memcmp(h, "RMP1", 4)) goto out;
    memcpy(&m->nfiles, h + 4, 4);
    memcpy(&m->n, h + 8, 8);
    memcpy(&m->size, h + 16, 8);
    if (!m->nfiles || m->nfiles > 64 || !m->n || m->n > (1u << 26)) goto out;
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
