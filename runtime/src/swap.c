/* swap.c - the swap region allocator: unlinked sparse temp files mapped MAP_SHARED over reserved address space, and the write-back / eviction policy.
 * Design, accounting measurements and the device procedure: docs/architecture/memory-swap.md. Portable POSIX plus the Darwin msync flags. */
#include "swap.h"
#include "napple.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILES 8
#define SLICE (64ULL << 20)

static SwapFile *files[MAX_FILES];
static unsigned nfiles;
static pthread_mutex_t swap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t st_written, st_deact, st_trims;

int swap_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NAPPLE_IOS_SWAP");
        on = e && *e && strcmp(e, "0") && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
    }
    return on;
}

static void swap_dir(char *out, size_t n) {
    const char *d = getenv("NAPPLE_SWAP_DIR");
    if (!d || !*d) d = getenv("TMPDIR");
    if (!d || !*d) d = "/tmp";
    snprintf(out, n, "%s%s", d, d[strlen(d) - 1] == '/' ? "" : "/");
}

int swap_file_open(SwapFile *f, const char *name, uint64_t size) {
    char path[1100], dir[1024];
    swap_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%snapple-swap-%s.XXXXXX", dir, name);
    int fd = mkstemp(path);
    if (fd < 0) {
        LOG("swap", "%s: cannot create a temp file in %s: %s", name, dir, strerror(errno));
        return -1;
    }
    unlink(path); /* gone from the namespace at once: exit, crash and jetsam all release the space */
    if (ftruncate(fd, (off_t)size)) {
        LOG("swap", "%s: ftruncate to %llu MB failed: %s", name, (unsigned long long)(size >> 20), strerror(errno));
        close(fd);
        return -1;
    }
    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->size = size;
    f->name = name;
    pthread_mutex_lock(&swap_lock);
    if (nfiles < MAX_FILES) files[nfiles++] = f;
    pthread_mutex_unlock(&swap_lock);
    LOG("swap", "%s: %llu MB sparse backing file (dir %s, unlinked)", name, (unsigned long long)(size >> 20), dir);
    return 0;
}

void *swap_commit(SwapFile *f, uint8_t *region, uint64_t off, uint64_t len) {
    if (!len || off + len > f->size) return NULL;
    void *p = mmap(region + off, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, f->fd, (off_t)off);
    if (p == MAP_FAILED) {
        LOG("swap", "%s: mmap %llu MB at offset %#llx failed: %s", f->name, (unsigned long long)(len >> 20), (unsigned long long)off, strerror(errno));
        return NULL;
    }
    pthread_mutex_lock(&swap_lock);
    f->base = region;
    if (off + len > f->high) f->high = off + len;
    f->committed += len; /* an upper bound when a range is committed twice; swap_stats reports residency separately */
    pthread_mutex_unlock(&swap_lock);
    return p;
}

int swap_decommit(SwapFile *f, uint8_t *region, uint64_t off, uint64_t len) {
    if (mmap(region + off, len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) return -1;
#ifdef F_PUNCHHOLE
    struct fpunchhole ph = {.fp_flags = 0, .reserved = 0, .fp_offset = (off_t)off, .fp_length = (off_t)len};
    if (fcntl(f->fd, F_PUNCHHOLE, &ph)) { /* the pages stay on disk until the file goes; zeroed when mapped again by the caller if it cares */
        static int warned;
        if (!warned++) LOG("swap", "%s: F_PUNCHHOLE refused (%s): freed ranges keep their disk blocks", f->name, strerror(errno));
    }
#endif
    pthread_mutex_lock(&swap_lock);
    f->committed = f->committed > len ? f->committed - len : 0;
    pthread_mutex_unlock(&swap_lock);
    return 0;
}

void *swap_map_anywhere(uint64_t len) {
    if (!swap_enabled()) return NULL;
    SwapFile *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    char path[1100], dir[1024];
    swap_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%snapple-swap-anon.XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { free(f); return NULL; }
    unlink(path);
    void *p = MAP_FAILED;
    if (!ftruncate(fd, (off_t)len)) p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* the mapping keeps the file alive */
    free(f);
    return p == MAP_FAILED ? NULL : p;
}

#ifndef MS_DEACTIVATE
#define MS_DEACTIVATE 0
#endif

/* Writes back and, from level 1, deactivates a slice of a file's committed range so the kernel can reclaim the pages first. Errors (holes) are ignored. */
static void trim_range(SwapFile *f, uint64_t from, uint64_t len, int deactivate) {
    if (!f->base || from >= f->high) return;
    if (from + len > f->high) len = f->high - from;
    if (msync(f->base + from, len, deactivate ? (MS_SYNC | MS_DEACTIVATE) : MS_ASYNC) == 0) {
        if (deactivate) st_deact += len;
        else st_written += len;
    }
}

void swap_trim(unsigned level) {
    pthread_mutex_lock(&swap_lock);
    st_trims++;
    for (unsigned i = 0; i < nfiles; i++) {
        SwapFile *f = files[i];
        if (!f->high) continue;
        if (level >= 2) {
            trim_range(f, 0, f->high, 1);
        } else {
            uint64_t span = level ? 4 * SLICE : SLICE; /* a warning cleans 256 MiB per call, the idle pass 64 MiB */
            if (f->cursor >= f->high) f->cursor = 0;
            trim_range(f, f->cursor, span, level);
            f->cursor += span;
        }
    }
    pthread_mutex_unlock(&swap_lock);
}

void swap_stats(SwapStats *s) {
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&swap_lock);
    s->files = nfiles;
    s->written_back = st_written;
    s->deactivated = st_deact;
    s->trims = st_trims;
    static unsigned char vec[SLICE / 4096];
    for (unsigned i = 0; i < nfiles; i++) {
        SwapFile *f = files[i];
        s->committed += f->committed;
        for (uint64_t o = 0; f->base && o < f->high; o += SLICE) {
            uint64_t n = f->high - o < SLICE ? f->high - o : SLICE;
            if (mincore(f->base + o, n, (char *)vec)) continue;
            uint64_t ps = (uint64_t)getpagesize();
            for (uint64_t p = 0; p < (n + ps - 1) / ps; p++) s->resident += (vec[p] & 1) ? ps : 0;
        }
    }
    pthread_mutex_unlock(&swap_lock);
}
