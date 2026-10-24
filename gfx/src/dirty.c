/* dirty.c - write-protection based dirty tracking of guest memory that backs textures.
 *
 * Textures are stored by the guest in Tegra block-linear layout and must be converted for Metal. After a texture was uploaded its storage
 * pages are made read-only; a guest write faults into napple_gfx_handle_fault(), which records a new generation for the page and
 * unprotects it. Before a texture is used, dirty_since() tells whether any covered page was written after the upload.
 *
 * State is kept per HOST page (16 KiB), in one global page table, and not per memory pool. Guests create many small pools (4 KiB aligned,
 * 4 KiB .. a few MiB) that share host pages with each other and with unrelated heap data; per-pool state made two pools disagree about
 * one page (pool A "protected", pool B "writable"), so a legitimate guest write could not be resolved and the process died with a
 * permission fault (LAS, leaving Marin's house). Now:
 *   - a page has a reference count = number of registered pool ranges that cover it (dirty_add_range / dirty_remove_range);
 *   - the page is protected only while refs > 0, and unprotected when the last range goes away;
 *   - the write-protection state machine is NONE <-> BUSY <-> PROT (+ UNTRACKED when mprotect failed), claimed with a CAS, so mprotect
 *     calls on one page never interleave;
 *   - a fault on a page that is already writable (another thread unprotected it between the fault and this handler) is simply retried.
 *
 * Ordering: the uploader write-protects the pages, then takes a stamp, and only then reads the memory. A write that races with the read
 * bumps the page generation above the stamp, so the next use uploads again; no write can be lost. Re-protecting a writable page also bumps
 * its generation (after the mprotect): a write let through by a fault can land any time until then, after another uploader's read.
 *
 * Signal-safety: the fault path only uses atomics and mprotect (no allocation, no locks). Table chunks are allocated by dirty_add_range
 * (never from the fault path) and are never freed.
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gfx_mtl.h"
#include "timeline.h"

enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2, PG_UNTRACKED = 3 };

typedef struct {
    _Atomic uint64_t gen;                 /* generation of the last guest write seen */
    _Atomic uint32_t state;
    _Atomic uint32_t refs;                /* registered ranges covering the page */
} page_t;

#define CH_PAGES_LOG2 10                  /* 1024 host pages (16 MiB) per chunk */
#define DIR_LOG2 16
#define DIR_SLOTS (1u << DIR_LOG2)

typedef struct { _Atomic uintptr_t key; _Atomic(page_t *) pages; } dir_t;   /* key = chunk index + 1 (0 = empty) */

static dir_t g_dir[DIR_SLOTS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_stale, g_preps;
static int g_diag = -1;

static void init_page(void) {
    if (g_pg) return;
    g_pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    g_shift = (uintptr_t)__builtin_ctzl(g_pg);
    const char *e = getenv("NAPPLE_GFX_DIRTY_DIAG");
    g_diag = e && *e && *e != '0';
}

static void diag(const char *what, uintptr_t a, uint64_t v) {
    if (g_diag <= 0) return;
    char b[160]; int n = snprintf(b, sizeof b, "[napple-gfx] dirty: %s page=%#lx v=%llu\n", what, (unsigned long)(a & ~(g_pg - 1)), (unsigned long long)v);
    if (n > 0 && write(2, b, (size_t)n) < 0) {}
}

static inline uintptr_t chunk_of(uintptr_t a) { return a >> (g_shift + CH_PAGES_LOG2); }
static inline unsigned slot_of(uintptr_t ci) { return (unsigned)(((uint64_t)ci * 0x9E3779B97F4A7C15ull) >> (64 - DIR_LOG2)); }

/* Async-signal-safe lookup of the entry of the page holding `a`; NULL when the page was never registered. */
static page_t *page_find(uintptr_t a) {
    uintptr_t ci = chunk_of(a), key = ci + 1;
    unsigned s = slot_of(ci);
    for (unsigned n = 0; n < DIR_SLOTS; n++, s = (s + 1) & (DIR_SLOTS - 1)) {
        uintptr_t k = atomic_load_explicit(&g_dir[s].key, memory_order_acquire);
        if (k == key) return atomic_load_explicit(&g_dir[s].pages, memory_order_acquire) + ((a >> g_shift) & ((1u << CH_PAGES_LOG2) - 1));
        if (!k) return NULL;
    }
    return NULL;
}

/* Entry for the page holding `a`, creating its chunk (caller holds g_mu). NULL when the table is full or out of memory. */
static page_t *page_get(uintptr_t a) {
    page_t *p = page_find(a);
    if (p) return p;
    uintptr_t ci = chunk_of(a);
    unsigned s = slot_of(ci);
    for (unsigned n = 0; n < DIR_SLOTS; n++, s = (s + 1) & (DIR_SLOTS - 1)) {
        if (atomic_load(&g_dir[s].key)) continue;
        page_t *ch = calloc((size_t)1 << CH_PAGES_LOG2, sizeof(page_t));
        if (!ch) return NULL;
        atomic_store_explicit(&g_dir[s].pages, ch, memory_order_release);
        atomic_store_explicit(&g_dir[s].key, ci + 1, memory_order_release);
        return ch + ((a >> g_shift) & ((1u << CH_PAGES_LOG2) - 1));
    }
    return NULL;
}

static void wait_busy(page_t *p) { while (atomic_load(&p->state) == PG_BUSY) sched_yield(); }

/* Makes a protected page writable. Returns 1 if this call did it, 0 if the page was not protected (already writable / untracked). */
static int unprotect_page(page_t *p, uintptr_t addr) {
    for (;;) {
        uint32_t s = atomic_load(&p->state);
        if (s == PG_BUSY) { wait_busy(p); continue; }
        if (s != PG_PROT) return 0;
        uint32_t exp = PG_PROT;
        if (!atomic_compare_exchange_strong(&p->state, &exp, PG_BUSY)) continue;
        mprotect((void *)(addr & ~(g_pg - 1)), g_pg, PROT_READ | PROT_WRITE);
        atomic_store(&p->state, PG_NONE);
        return 1;
    }
}

void dirty_add_range(const void *mem, size_t size) {
    init_page();
    if (!size) return;
    uintptr_t b = (uintptr_t)mem & ~(g_pg - 1), e = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    pthread_mutex_lock(&g_mu);
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_get(a);
        if (!p) { LOG_ONCE(NAPPLE_LOG_ERROR, "dirty tracking: page table full or out of memory; some memory is uploaded on every use"); continue; }
        atomic_fetch_add(&p->refs, 1);
    }
    pthread_mutex_unlock(&g_mu);
    diag("add range pages", b, (e - b) >> g_shift);
}

/* Must be called with the same (mem, size) as the matching dirty_add_range. */
void dirty_remove_range(const void *mem, size_t size) {
    init_page();
    if (!size) return;
    uintptr_t b = (uintptr_t)mem & ~(g_pg - 1), e = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_find(a);
        if (!p) continue;
        uint32_t r = atomic_load(&p->refs);
        while (r && !atomic_compare_exchange_weak(&p->refs, &r, r - 1)) {}
        if (r != 1) continue;                      /* other ranges still need the page (or it was never counted) */
        /* last reference gone: the page must end up writable. A protect() that claimed it before seeing refs == 0 is waited for. */
        atomic_store(&p->gen, atomic_fetch_add(&g_now, 1) + 1);
        for (;;) {
            uint32_t s = atomic_load(&p->state);
            if (s == PG_BUSY) { wait_busy(p); continue; }
            if (s == PG_PROT) {
                uint32_t exp = PG_PROT;
                if (!atomic_compare_exchange_strong(&p->state, &exp, PG_BUSY)) continue;
                mprotect((void *)a, g_pg, PROT_READ | PROT_WRITE);
                atomic_store(&p->state, PG_NONE);
            } else if (s == PG_UNTRACKED) {
                atomic_store(&p->state, PG_NONE);
            }
            break;
        }
    }
    diag("remove range pages", b, (e - b) >> g_shift);
}

uint64_t dirty_now(void) { return atomic_load(&g_now); }

int dirty_tracked(const void *addr, size_t len) {
    init_page();
    if (!len) len = 1;
    uintptr_t b = (uintptr_t)addr & ~(g_pg - 1), e = ((uintptr_t)addr + len + g_pg - 1) & ~(g_pg - 1);
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_find(a);
        if (!p || !atomic_load(&p->refs)) return 0;
    }
    return 1;
}

/* Claims a run of writable tracked pages, write-protects them with one mprotect call, then publishes the new state. */
static void protect_run(uintptr_t start, size_t n) {
    if (!n) return;
    int rc = mprotect((void *)start, n << g_shift, PROT_READ);
    if (rc != 0) LOG_ONCE(NAPPLE_LOG_WARN, "dirty tracking: mprotect failed; the memory is uploaded on every use");
    for (size_t k = 0; k < n; k++) {
        page_t *p = page_find(start + (k << g_shift));
        /* The page was writable until the mprotect above, and a write let through by an earlier fault may have landed only now (the
         * handler bumps the generation before the faulting store is retried). Protecting closes that window, so it counts as a write:
         * bumped after mprotect, so any stamp older than this protect sees the page dirty. */
        if (rc == 0) atomic_store(&p->gen, atomic_fetch_add(&g_now, 1) + 1);
        atomic_store(&p->state, rc == 0 ? PG_PROT : PG_UNTRACKED);
    }
}

void dirty_protect(const void *addr, size_t len) {
    init_page();
    if (!len) len = 1;
    uintptr_t b = (uintptr_t)addr & ~(g_pg - 1), e = ((uintptr_t)addr + len + g_pg - 1) & ~(g_pg - 1);
    uintptr_t run = 0; size_t rn = 0;
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_find(a);
        int claimed = 0;
        if (p && atomic_load(&p->refs)) {
            uint32_t exp = PG_NONE;
            if (atomic_compare_exchange_strong(&p->state, &exp, PG_BUSY)) {
                if (atomic_load(&p->refs)) claimed = 1;                 /* re-check after the claim: dirty_remove_range may have dropped the last ref */
                else atomic_store(&p->state, PG_NONE);
            }
        }
        if (claimed) { if (!rn) run = a; rn++; continue; }
        protect_run(run, rn); rn = 0;                                   /* page already protected, busy elsewhere or not tracked: the run ends */
    }
    protect_run(run, rn);
}

int dirty_since(const void *addr, size_t len, uint64_t stamp) {
    init_page();
    if (!len) len = 1;
    uintptr_t b = (uintptr_t)addr & ~(g_pg - 1), e = ((uintptr_t)addr + len + g_pg - 1) & ~(g_pg - 1);
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_find(a);
        if (!p || !atomic_load(&p->refs)) return 1;                      /* not (fully) tracked */
        uint32_t s = atomic_load(&p->state);
        if (s != PG_PROT || atomic_load(&p->gen) > stamp) return 1;   /* NONE: never protected since the last write; BUSY: being (un)protected */
    }
    return 0;
}

int napple_gfx_handle_fault(void *addr, int is_write, void *ucontext) {
    (void)ucontext; (void)is_write;
    init_page();
    uintptr_t a = (uintptr_t)addr;
    page_t *p = page_find(a);
    if (!p) return 0;                                                    /* never tracked: not ours */
    atomic_store(&p->gen, atomic_fetch_add(&g_now, 1) + 1);              /* bump before unprotecting: an uploader's stamp is always older */
    static _Thread_local uintptr_t last_page;
    static _Thread_local unsigned repeat;
    uintptr_t pg = a & ~(g_pg - 1);
    for (;;) {
        uint32_t s = atomic_load(&p->state);
        if (s == PG_BUSY) { wait_busy(p); continue; }
        if (s == PG_PROT) {
            if (unprotect_page(p, a)) { atomic_fetch_add(&g_faults, 1); last_page = 0; repeat = 0; return 1; }
            continue;
        }
        break;
    }
    /* The page is writable as far as the table knows: another thread unprotected it between the fault and now (retry), or the protection
     * changed behind our back (remap, runtime mprotect). A page that keeps faulting is forced writable when a registered range covers it. */
    atomic_fetch_add(&g_stale, 1);
    if (last_page != pg) { last_page = pg; repeat = 0; }
    if (++repeat <= 4) return 1;
    if (atomic_load(&p->refs)) {
        diag("forcing a stale page writable", a, repeat);
        mprotect((void *)pg, g_pg, PROT_READ | PROT_WRITE);
        atomic_store(&p->state, PG_NONE);
        if (repeat > 64) return 0;
        return 1;
    }
    return 0;
}

void napple_gfx_guest_write_prepare(void *addr, size_t len) {
    init_page();
    if (!len) len = 1;
    uintptr_t b = (uintptr_t)addr & ~(g_pg - 1), e = ((uintptr_t)addr + len + g_pg - 1) & ~(g_pg - 1);
    for (uintptr_t a = b; a < e; a += g_pg) {
        page_t *p = page_find(a);
        if (!p) continue;
        atomic_store(&p->gen, atomic_fetch_add(&g_now, 1) + 1);
        unprotect_page(p, a);
        atomic_fetch_add(&g_preps, 1);
    }
}

/* The mapping under [mem, mem+size) was replaced (mmap / mach_vm_remap): the old protection and contents are gone. */
void dirty_invalidate(const void *mem, size_t size) {
    napple_gfx_guest_write_prepare((void *)mem, size);
}

/* One-line description of the tracking state of the page holding `addr` (crash diagnostics). */
int dirty_describe(const void *addr, char *buf, size_t n) {
    init_page();
    page_t *p = page_find((uintptr_t)addr);
    if (!p) return snprintf(buf, n, "page never tracked");
    return snprintf(buf, n, "state=%u refs=%u gen=%llu now=%llu faults=%llu stale=%llu", atomic_load(&p->state), atomic_load(&p->refs),
                    (unsigned long long)atomic_load(&p->gen), (unsigned long long)atomic_load(&g_now), (unsigned long long)atomic_load(&g_faults),
                    (unsigned long long)atomic_load(&g_stale));
}
