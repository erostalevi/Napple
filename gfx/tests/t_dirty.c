/* t_dirty.c - write-protection dirty tracking (src/dirty.c). Regression for the LAS crash on leaving Marin's house: a legitimate guest write to a
 * page that several small 4 KiB-aligned memory pools share could not be resolved (per-pool state disagreed), the fault handler declined and the
 * process died with a permission fault. Covers: deterministic sub-16K sharing / overlap / removal order cases, then a multi-threaded stress of
 * writers vs protect vs pool add/remove with the real SIGSEGV/SIGBUS handler: every write must resolve and no upload may miss a write. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void dirty_add_range(const void *mem, size_t size);
void dirty_remove_range(const void *mem, size_t size);
uint64_t dirty_now(void);
int dirty_since(const void *addr, size_t len, uint64_t stamp);
void dirty_protect(const void *addr, size_t len);
int dirty_tracked(const void *addr, size_t len);
int napple_gfx_handle_fault(void *addr, int is_write, void *ucontext);
void napple_gfx_guest_write_prepare(void *addr, size_t len);

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static _Atomic long g_handled;
static void handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    if (napple_gfx_handle_fault(info->si_addr, 1, ctx)) { atomic_fetch_add(&g_handled, 1); return; }
    static const char m[] = "FAIL: fault the dirty tracker declined (this is the crash)\n";
    if (write(2, m, sizeof m - 1) < 0) {}
    _exit(3);
}

#define PG 16384u
#define K4 4096u

static void deterministic(uint8_t *arena) {
    uint8_t *page = arena + 4 * PG;                     /* one host page = four guest 4 KiB pages */
    /* three small pools sharing the page: [1K..2K) [2K..3K) [3K..4K) in 4 KiB units; plus an unrelated tail */
    dirty_add_range(page + 1 * K4, K4);
    dirty_add_range(page + 2 * K4, K4);
    dirty_add_range(page + 3 * K4, K4);
    CHECK(dirty_tracked(page, PG), "whole page tracked");
    dirty_protect(page + 2 * K4, K4);                    /* a texture in pool 2 is uploaded: protects the host page */
    uint64_t stamp = dirty_now();
    CHECK(!dirty_since(page + 2 * K4, K4, stamp), "clean after protect");
    /* a write into pool 1's 4K range hits the protected page; used to be declined because pool 1's own state was 'writable' */
    page[1 * K4 + 8] = 7;
    CHECK(dirty_since(page + 2 * K4, K4, stamp), "write to a sibling 4K page marks the shared host page dirty");
    CHECK(page[1 * K4 + 8] == 7, "write landed");
    /* removal order: drop pools 1 and 3, page must stay tracked for pool 2 and stay protected */
    dirty_protect(page + 2 * K4, K4);
    uint64_t s2 = dirty_now();
    dirty_remove_range(page + 1 * K4, K4);
    dirty_remove_range(page + 3 * K4, K4);
    CHECK(dirty_tracked(page + 2 * K4, K4), "still tracked by the remaining pool");
    CHECK(!dirty_since(page + 2 * K4, K4, s2), "removing siblings must not unprotect a page another pool still needs");
    page[2 * K4] = 9;
    CHECK(dirty_since(page + 2 * K4, K4, s2), "write after sibling removal still detected");
    /* last pool goes while protected: the page becomes writable and untracked */
    dirty_protect(page + 2 * K4, K4);
    dirty_remove_range(page + 2 * K4, K4);
    CHECK(!dirty_tracked(page + 2 * K4, K4), "untracked after the last removal");
    page[0] = 1; page[PG - 1] = 2;                      /* no fault, no crash: the page is writable */
    /* overlapping pools (the same memory registered twice, then one finalized) */
    dirty_add_range(page, 2 * PG); dirty_add_range(page + PG, PG);
    dirty_protect(page, 2 * PG); uint64_t s3 = dirty_now();
    dirty_remove_range(page, 2 * PG);
    CHECK(!dirty_since(page + PG, PG, s3), "second page still protected through the overlapping pool");
    page[PG + 100] = 5;
    CHECK(dirty_since(page + PG, PG, s3), "write to the overlapped page detected");
    page[5] = 5;                                         /* first page lost its last ref: plain writable */
    dirty_remove_range(page + PG, PG);
    /* host-side write preparation */
    dirty_add_range(page, PG); dirty_protect(page, PG); uint64_t s4 = dirty_now();
    napple_gfx_guest_write_prepare(page, 64); memset(page, 3, 64);
    CHECK(dirty_since(page, PG, s4), "host write marks the page dirty");
    dirty_remove_range(page, PG);
}

/* ---- stress ---- */
#define ARENA_PAGES 256
static uint8_t *g_arena;
static _Atomic int g_stop;
static _Atomic uint64_t g_writes, g_checks;
typedef struct { uintptr_t off, size; _Atomic int live; } pool_rec;
#define NPOOLS 96
static pool_rec g_pools[NPOOLS];

static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void *writer(void *arg) {
    uint32_t s = (uint32_t)(uintptr_t)arg * 2654435761u + 1;
    while (!atomic_load(&g_stop)) {
        uint32_t unit = rnd(&s) % (ARENA_PAGES * 4);
        uint32_t v = rnd(&s) | 1;
        volatile uint32_t *p = (volatile uint32_t *)(g_arena + (size_t)unit * K4 + (rnd(&s) % (K4 / 4)) * 4);
        *p = v; atomic_fetch_add(&g_writes, 1);
        if ((rnd(&s) & 0xff) == 0) usleep(50);
    }
    return NULL;
}
static void *churn(void *arg) {
    uint32_t s = 77 + (uint32_t)(uintptr_t)arg;
    int half = NPOOLS / 2, base = ((int)(uintptr_t)arg & 1) * half;      /* each churn thread owns half of the records */
    while (!atomic_load(&g_stop)) {
        pool_rec *r = &g_pools[base + (int)(rnd(&s) % (uint32_t)half)];
        if (atomic_load(&r->live)) { dirty_remove_range(g_arena + r->off, r->size); atomic_store(&r->live, 0); }
        else {
            r->off = (rnd(&s) % (ARENA_PAGES * 4 - 80)) * K4;                   /* 4K aligned, not 16K aligned in general */
            r->size = (1 + rnd(&s) % 64) * K4;
            dirty_add_range(g_arena + r->off, r->size);
            atomic_store(&r->live, 1);
        }
    }
    for (int i = base; i < base + half; i++) if (atomic_load(&g_pools[i].live)) { dirty_remove_range(g_arena + g_pools[i].off, g_pools[i].size); atomic_store(&g_pools[i].live, 0); }
    return NULL;
}
static void *uploader(void *arg) {
    uint32_t s = 1234 + (uint32_t)(uintptr_t)arg;
    uint8_t *snap = malloc(80 * K4);
    while (!atomic_load(&g_stop)) {
        size_t off = (rnd(&s) % (ARENA_PAGES * 4 - 80)) * K4, size = (1 + rnd(&s) % 32) * K4;
        dirty_protect(g_arena + off, size);                  /* protect first, then stamp ... */
        uint64_t stamp = dirty_now();
        memcpy(snap, g_arena + off, size);                   /* ... then read */
        usleep(rnd(&s) % 200);
        if (!dirty_since(g_arena + off, size, stamp)) {      /* claims "unchanged": the memory must really be unchanged */
            if (memcmp(snap, g_arena + off, size) && !dirty_since(g_arena + off, size, stamp)) {
                printf("FAIL: a write after the upload snapshot was not reported (off %#zx size %#zx)\n", off, size); g_fail++;
            }
            atomic_fetch_add(&g_checks, 1);
        }
    }
    free(snap);
    return NULL;
}

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    g_arena = mmap(NULL, (size_t)ARENA_PAGES * PG + PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(g_arena != MAP_FAILED, "arena");
    deterministic(g_arena + PG * 0);
    printf("deterministic cases done, faults handled: %ld\n", atomic_load(&g_handled));

    double secs = getenv("T_DIRTY_SECS") ? atof(getenv("T_DIRTY_SECS")) : 3.0;
    pthread_t th[10]; int n = 0;
    for (int i = 0; i < 4; i++) pthread_create(&th[n++], NULL, writer, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < 2; i++) pthread_create(&th[n++], NULL, uploader, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < 2; i++) pthread_create(&th[n++], NULL, churn, (void *)(uintptr_t)(i + 1));
    usleep((useconds_t)(secs * 1e6));
    atomic_store(&g_stop, 1);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    printf("stress: %llu writes, %ld faults resolved, %llu clean-upload checks\n", (unsigned long long)atomic_load(&g_writes), atomic_load(&g_handled), (unsigned long long)atomic_load(&g_checks));
    CHECK(atomic_load(&g_writes) > 1000, "writers made progress");
    if (g_fail) { printf("FAILED (%d)\n", g_fail); return 1; }
    printf("OK\n");
    return 0;
}
