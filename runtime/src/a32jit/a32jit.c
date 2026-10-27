#include "a32jit.h"
#include "../a32/a32_priv.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#define EXIT_NOBLOCK 100

static int (*jit_enter)(A32Cpu *c, int64_t budget);
static const uint64_t *jit_hash; /* 16-byte slots: guest key (address | thumb), code address; 0 = empty */
static unsigned jit_bits;
static atomic_ullong st_noblock, st_interp, st_hle, st_op;
/* NAPPLE_A32_JITLOG=<file>: every guest pc where lifted code was left because no block starts there (an indirect branch the lifter could not resolve),
 * appended once per distinct pc. Feed the file back with A32LIFT_SEEDS=<file> when rebuilding the image (profile-guided entry points). */
static FILE *logf;
static uint32_t *logged;
static size_t nlogged;
static void log_target(uint32_t key) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    size_t cap = 1u << 20;
    unsigned h = (key * 0x9E3779B1u) >> 12;
    if (!logged) logged = calloc(cap, 4);
    while (logged[h] && logged[h] != key + 1) h = (h + 1) & (cap - 1);
    if (!logged[h] && nlogged < cap / 2) {
        logged[h] = key + 1;
        nlogged++;
        fprintf(logf, "%x\n", key);
        fflush(logf);
    }
    pthread_mutex_unlock(&mu);
}

/* NAPPLE_A32_JITSTATS=2: which instructions run through the interpreter (the lifter's fallbacks and the dispatcher's steps), by guest pc */
#define PROF_SIZE (1u << 18)
static struct {
    _Atomic uint32_t key;
    _Atomic uint32_t n;
} *prof;
static int profiling;
static atomic_ullong prof_total;
static void prof_dump(void);
static void prof_hit(uint32_t key) {
    if (atomic_fetch_add_explicit(&prof_total, 1, memory_order_relaxed) % 3000000 == 2999999) prof_dump(); /* a crashing guest never reaches atexit */
    unsigned h = (key * 0x9E3779B1u) >> 14;
    for (unsigned k = 0; k < 64; k++, h = (h + 1) & (PROF_SIZE - 1)) {
        uint32_t cur = atomic_load_explicit(&prof[h].key, memory_order_relaxed);
        if (cur == 0) {
            uint32_t z = 0;
            if (!atomic_compare_exchange_strong(&prof[h].key, &z, key)) cur = z;
            else cur = key;
        }
        if (cur == key) {
            atomic_fetch_add_explicit(&prof[h].n, 1, memory_order_relaxed);
            return;
        }
    }
}
static int has_block(uint32_t key);
static int prof_cmp(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[1], y = ((const uint32_t *)b)[1];
    return (x < y) - (x > y);
}
static void prof_dump(void) {
    uint32_t (*v)[2] = malloc(PROF_SIZE * sizeof *v);
    size_t n = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < PROF_SIZE; i++)
        if (prof[i].key) {
            v[n][0] = prof[i].key;
            v[n][1] = prof[i].n;
            total += prof[i].n;
            n++;
        }
    qsort(v, n, sizeof *v, prof_cmp);
    fprintf(stderr, "[napple] a32jit profile: %llu interpreted instructions at %zu distinct pcs; hottest:\n", (unsigned long long)total, n);
    for (size_t i = 0; i < n && i < 40; i++)
        fprintf(stderr, "  %9u  pc %#x%s%s\n", v[i][1], v[i][0] & ~1u, (v[i][0] & 1) ? " (thumb)" : "", has_block(v[i][0]) ? " [has a lifted block]" : "");
}

/* An out-of-line operation of the lifted image: `lit` is the instruction the lifter decoded at build time (a32_priv.h A32Lit). */
static uint32_t jit_op(A32Cpu *c, const A32Lit *lit) {
    if (profiling) prof_hit(c->r[15] | (c->thumb & 1));
    atomic_fetch_add_explicit(&st_op, 1, memory_order_relaxed);
    return (uint32_t)a32_op_exec(c, &lit->insn, c->r[15], lit->raw);
}

/* A jump into a host-implemented import (guest 0xF00xxxxx): what a32_run does for such a pc, then the lifted code continues at lr. */
static uint32_t jit_hle(A32Cpu *c, uint32_t pc) {
    int stop = c->hooks->hle(c, pc & ~1u);
    uint32_t lr = c->r[14];
    if (lr & 1) {
        c->thumb = 1;
        c->r[15] = lr & ~1u;
    } else {
        c->thumb = 0;
        c->r[15] = lr & ~3u;
    }
    atomic_fetch_add_explicit(&st_hle, 1, memory_order_relaxed);
    return stop ? 1 : 0;
}

static int has_block(uint32_t key) {
    unsigned h = (uint32_t)(key * 0x9E3779B1u) >> (32 - jit_bits), mask = (1u << jit_bits) - 1;
    for (;;) {
        const uint64_t *s = jit_hash + 2 * (size_t)h;
        if (!s[1]) return 0;
        if ((uint32_t)s[0] == key) return 1;
        h = (h + 1) & mask;
    }
}

static void print_stats(void);
static void *stats_thread(void *arg) { /* the runtime ends with _exit(), which skips atexit: report every 10 s as well */
    (void)arg;
    for (;;) {
        sleep(10);
        print_stats();
    }
    return NULL;
}

static void print_stats(void) {
    fprintf(
        stderr,
        "[napple] a32jit: %llu out-of-line operations executed, %llu exits to the dispatcher (unlifted pc), %llu instructions interpreted (debug flag), %llu calls "
        "into host imports\n",
        (unsigned long long)atomic_load(&st_op), (unsigned long long)atomic_load(&st_noblock), (unsigned long long)atomic_load(&st_interp),
        (unsigned long long)atomic_load(&st_hle));
}

int a32jit_init(const char *dir, const A32Window *w) {
    const char *off = getenv("NAPPLE_A32_JIT");
    if (off && !strcmp(off, "0")) return 0;
    char path[4200];
    snprintf(path, sizeof path, "%s/a32jit.dylib", dir);
    if (access(path, R_OK)) return 0;
    /* the manifest: name base size text_end per module; the lifted code embeds absolute guest addresses */
    char mpath[4200];
    snprintf(mpath, sizeof mpath, "%s/a32jit.manifest", dir);
    FILE *f = fopen(mpath, "r");
    if (!f) {
        fprintf(stderr, "[napple] a32jit: %s has no manifest, not using it\n", path);
        return 0;
    }
    char name[32];
    unsigned base, size, text;
    unsigned n = 0;
    int ok = 1;
    while (fscanf(f, "%31s %x %x %x", name, &base, &size, &text) == 4) {
        unsigned m;
        for (m = 0; m < w->nmods; m++)
            if (!strcmp(w->mods[m].name, name)) break;
        if (m == w->nmods || w->mods[m].base != base || w->mods[m].size != size || w->mods[m].text_end != text) {
            ok = 0;
            fprintf(stderr, "[napple] a32jit: module %s differs from the lifted image\n", name);
        }
        n++;
    }
    fclose(f);
    if (!ok || n != w->nmods) {
        fprintf(stderr, "[napple] a32jit: image does not match the mapped modules (%u vs %u), not using it\n", n, w->nmods);
        return 0;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "[napple] a32jit: %s\n", dlerror());
        return 0;
    }
    jit_enter = (int (*)(A32Cpu *, int64_t))dlsym(h, "a32jit_enter");
    jit_hash = dlsym(h, "a32jit_hash");
    const uint64_t *bits = dlsym(h, "a32jit_hash_bits");
    void **hooks = dlsym(h, "a32jit_hooks");
    if (!jit_enter || !jit_hash || !bits || !hooks) {
        fprintf(stderr, "[napple] a32jit: %s lacks its entry points\n", path);
        jit_enter = NULL;
        return 0;
    }
    jit_bits = (unsigned)*bits;
    hooks[0] = (void *)jit_op;
    hooks[1] = (void *)jit_hle;
    if (getenv("NAPPLE_A32_JITLOG")) logf = fopen(getenv("NAPPLE_A32_JITLOG"), "a");
    const char *stats = getenv("NAPPLE_A32_JITSTATS");
    if (stats) {
        atexit(print_stats);
        pthread_t t;
        if (!pthread_create(&t, NULL, stats_thread, NULL)) pthread_detach(t);
    }
    if (stats && !strcmp(stats, "2")) {
        prof = calloc(PROF_SIZE, sizeof *prof);
        profiling = prof != NULL;
        if (profiling) atexit(prof_dump);
    }
    fprintf(stderr, "[napple] a32jit: lifted code active (%s)\n", path);
    return 1;
}

int a32jit_active(void) { return jit_enter != NULL; }

A32Exit a32jit_run(A32Cpu *c, uint64_t budget) {
    int64_t left = (int64_t)budget;
    for (;;) {
        if (!c->itstate && has_block(c->r[15] | (c->thumb & 1))) {
            int r = jit_enter(c, left);
            if (r == A32_EXIT_BUDGET) return A32_EXIT_BUDGET;
            if (r != EXIT_NOBLOCK) return (A32Exit)r;
            atomic_fetch_add_explicit(&st_noblock, 1, memory_order_relaxed);
            if (logf && c->r[15] < A32_HLE_BASE) log_target(c->r[15] | (c->thumb & 1));
        }
#ifdef NAPPLE_A32_INTERP
        if (profiling) prof_hit(c->r[15] | (c->thumb & 1));
        uint32_t before = c->r[15];
        A32Exit e = a32_run(c, 1);
        if (logf && c->r[15] - before != 2 && c->r[15] - before != 4 && c->r[15] < A32_HLE_BASE && before < A32_HLE_BASE &&
            !has_block(c->r[15] | (c->thumb & 1))) {
            fprintf(stderr, "[a32jit] unlifted target %x reached from %x (lr %x)\n", c->r[15], before, c->r[14]);
            log_target(c->r[15] | (c->thumb & 1));
        } /* a branch target in unlifted code */
        atomic_fetch_add_explicit(&st_interp, 1, memory_order_relaxed);
        if (e != A32_EXIT_BUDGET) return e;
        if (--left <= 0) return A32_EXIT_BUDGET;
#else
        /* the shipped runtime has no interpreter: a pc without lifted code is an incomplete image (add the pc to a32jit.seeds and rebuild) */
        c->fault_pc = c->r[15];
        c->fault_word = 0;
        return A32_EXIT_NOBLOCK;
#endif
    }
}
