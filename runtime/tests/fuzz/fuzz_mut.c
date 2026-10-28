/* Seeded mutation driver for one fuzz target (built with -fsanitize=address,undefined -fno-sanitize-recover=all, plus trace-pc-guard so inputs that
 * reach new code stay in the corpus). Deterministic for a given seed and iteration count.
 *   <name>_mut [-n iterations] [-t seconds] [-s seed] [-m max_len] [-o crash_dir] [-q] [-r] [file ...]
 * Files are extra seeds; with -r they are replayed once each (reproduce a crash) and nothing is mutated.
 * A crash (sanitizer report, signal, abort, or an input running longer than FUZZ_TIMEOUT seconds, default 20) writes the input (and the few before it,
 * since a target may keep state) to the crash directory as crash-<name>-<hash> and exits non-zero. */
#include "fuzz.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void __sanitizer_set_death_callback(void (*cb)(void));
void __sanitizer_print_stack_trace(void);
const char *__asan_default_options(void) { return "detect_leaks=0:max_allocation_size_mb=512:allocator_may_return_null=0:handle_abort=1"; }

#define RING 4
#define MAXCORPUS 65536
typedef struct {
    uint8_t *p;
    size_t n;
} Buf;
static Buf corpus[MAXCORPUS];
static size_t ncorpus, g_maxlen = 1 << 16;
static uint64_t rng_state;
static uint8_t *cur_buf[RING]; /* the executing input and the ones before it */
static size_t cur_len[RING];
static unsigned cur_idx;
static const char *crash_dir = "build/fuzz/crashes";
static volatile sig_atomic_t dumping;
static uint64_t g_iter;
static int g_replay;
static char errbuf[1 << 16];
#ifdef __APPLE__
#define FZ_PURGE(f) fpurge(f)
#else
#include <stdio_ext.h>
#define FZ_PURGE(f) __fpurge(f)
#endif

static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x >> 12, x ^= x << 25, x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static size_t rndn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

/* ---- coverage feedback ------------------------------------------------------------------------------------------------------------------ */
/* One byte per edge in a shared mapping: a target that forks per input (stateful ones, see t_ipc.c) still reports what its child reached. cov[0..7] counts edges. */
#define COV_EDGES (1u << 22)
static volatile uint8_t *cov;
static uint32_t cov_n = 8; /* ids 1..7 would collide with the counter */
static uint64_t edges_seen(void) {
    uint64_t v;
    memcpy(&v, (const void *)cov, 8);
    return v;
}
void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    if (!cov) cov = mmap(NULL, COV_EDGES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (start == stop || *start) return;
    for (uint32_t *g = start; g < stop; g++) *g = ++cov_n < COV_EDGES ? cov_n : COV_EDGES - 1;
}
void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    if (!cov[*guard]) {
        cov[*guard] = 1;
        (*(volatile uint64_t *)cov)++;
    }
}

/* ---- crash reporting -------------------------------------------------------------------------------------------------------------------- */
static void write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return;
        b += w, n -= (size_t)w;
    }
}
static void dump_inputs(void) {
    if (dumping) return;
    dumping = 1;
    uint64_t h = 1469598103934665603ULL;
    unsigned last = (cur_idx + RING - 1) % RING;
    for (size_t i = 0; cur_buf[last] && i < cur_len[last]; i++) h = (h ^ cur_buf[last][i]) * 1099511628211ULL;
    for (unsigned k = 0; k < RING; k++) {
        unsigned slot = (last + RING - k) % RING;
        if (!cur_buf[slot]) continue;
        char path[600];
        if (k) snprintf(path, sizeof path, "%s/crash-%s-%016llx.prev%u", crash_dir, fuzz_target.name, (unsigned long long)h, k);
        else snprintf(path, sizeof path, "%s/crash-%s-%016llx", crash_dir, fuzz_target.name, (unsigned long long)h);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) continue;
        write_all(fd, cur_buf[slot], cur_len[slot]);
        close(fd);
        if (!k) {
            char msg[700];
            int m = snprintf(msg, sizeof msg, "FUZZ crash: %s iteration %llu, input (%zu bytes) saved to %s\n", fuzz_target.name, (unsigned long long)g_iter,
                             cur_len[slot], path);
            write_all(2, msg, (size_t)m);
        }
    }
}
static void on_death(void) { dump_inputs(); }
static void on_signal(int sig) {
    dump_inputs();
    const char *m = sig == SIGALRM ? "FUZZ hang: input ran too long\n" : "FUZZ fatal signal\n";
    write_all(2, m, strlen(m));
    __sanitizer_print_stack_trace(); /* where it was (not async-signal-safe, but the process ends here) */
    _exit(sig == SIGALRM ? 77 : 70);
}

/* ---- mutations -------------------------------------------------------------------------------------------------------------------------- */
static const uint64_t interesting[] = {0,
                                       1,
                                       2,
                                       3,
                                       4,
                                       7,
                                       8,
                                       0x0f,
                                       0x10,
                                       0x20,
                                       0x3f,
                                       0x40,
                                       0x7f,
                                       0x80,
                                       0xff,
                                       0x100,
                                       0x3ff,
                                       0x400,
                                       0x7fff,
                                       0x8000,
                                       0xffff,
                                       0x10000,
                                       0xfffff,
                                       0x1000000,
                                       0x7fffffff,
                                       0x80000000u,
                                       0xfffffffe,
                                       0xffffffffu,
                                       0x100000000ull,
                                       0x7fffffffffffffffull,
                                       0x8000000000000000ull,
                                       0xffffffffffffffffull};
static void put_le(uint8_t *p, uint64_t v, size_t w) {
    for (size_t i = 0; i < w; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get_le(const uint8_t *p, size_t w) {
    uint64_t v = 0;
    for (size_t i = 0; i < w; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
/* One random mutation of b (cap = allocated size of b->p); other is a second corpus entry to splice from. */
static void mutate_once(Buf *b, size_t cap, const Buf *other) {
    size_t n = b->n;
    switch (rnd() % 12) {
        case 0: /* bit flip */
            if (n) b->p[rndn(n)] ^= (uint8_t)(1u << (rnd() & 7));
            break;
        case 1: /* random byte */
            if (n) b->p[rndn(n)] = (uint8_t)rnd();
            break;
        case 2: { /* huge length field: an interesting 1/2/4/8 byte value at a random offset */
            size_t w = (size_t)1 << (rnd() & 3);
            if (n >= w) put_le(b->p + rndn(n - w + 1), interesting[rndn(sizeof interesting / sizeof *interesting)], w);
            break;
        }
        case 3: { /* small arithmetic on a 1/2/4 byte field */
            size_t w = (size_t)1 << (rnd() % 3);
            if (n >= w) {
                uint8_t *q = b->p + rndn(n - w + 1);
                int64_t d = (int64_t)rndn(35) - 17;
                put_le(q, (uint64_t)((int64_t)get_le(q, w) + d), w);
            }
            break;
        }
        case 4: /* truncate */
            if (n > 1) b->n = 1 + rndn(n - 1);
            break;
        case 5: { /* truncate to a power of two / aligned size */
            if (n > 4) b->n = (size_t)1 << (rnd() % 16) < n ? (size_t)1 << (rnd() % 16) : n - 1;
            if (!b->n) b->n = 1;
            break;
        }
        case 6: { /* append random bytes */
            size_t add = 1 + rndn(64);
            if (n + add > cap) add = cap - n;
            for (size_t i = 0; i < add; i++) b->p[n + i] = (uint8_t)rnd();
            b->n = n + add;
            break;
        }
        case 7: { /* delete a chunk */
            if (n > 2) {
                size_t at = rndn(n - 1), len = 1 + rndn(n - at < 64 ? n - at : 64);
                memmove(b->p + at, b->p + at + len, n - at - len);
                b->n = n - len;
            }
            break;
        }
        case 8: { /* duplicate a chunk in place */
            if (n) {
                size_t at = rndn(n), len = 1 + rndn(n - at < 64 ? n - at : 64);
                if (n + len <= cap) {
                    memmove(b->p + at + len, b->p + at, n - at);
                    b->n = n + len;
                }
            }
            break;
        }
        case 9:
        case 10: { /* splice a chunk of another seed over / into this one */
            if (other && other->n && n) {
                size_t len = 1 + rndn(other->n < 128 ? other->n : 128), from = rndn(other->n - len + 1), at = rndn(n);
                if (rnd() & 1) { /* overwrite */
                    if (at + len > cap) len = cap - at;
                    memcpy(b->p + at, other->p + from, len);
                    if (at + len > b->n) b->n = at + len;
                } else if (n + len <= cap) { /* insert */
                    memmove(b->p + at + len, b->p + at, n - at);
                    memcpy(b->p + at, other->p + from, len);
                    b->n = n + len;
                }
            }
            break;
        }
        default: { /* fill a run with one byte */
            if (n) {
                size_t at = rndn(n), len = 1 + rndn(n - at < 32 ? n - at : 32);
                memset(b->p + at, (int)(rnd() & 1 ? 0xff : 0), len);
            }
        }
    }
}

static void add_corpus(const uint8_t *p, size_t n) {
    if (ncorpus >= MAXCORPUS) return;
    if (n > g_maxlen) n = g_maxlen;
    corpus[ncorpus].p = malloc(n ? n : 1);
    memcpy(corpus[ncorpus].p, p, n);
    corpus[ncorpus++].n = n;
}
static void seed_cb(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    add_corpus(p, n);
}

static void run_one(const uint8_t *p, size_t n) {
    free(cur_buf[cur_idx]);
    cur_buf[cur_idx] = malloc(n ? n : 1);
    memcpy(cur_buf[cur_idx], p, n);
    cur_len[cur_idx] = n;
    cur_idx = (cur_idx + 1) % RING;
    /* the target gets its own exactly-sized copy so ASan sees reads past the end */
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, p, n);
    alarm(getenv("FUZZ_TIMEOUT") ? (unsigned)atoi(getenv("FUZZ_TIMEOUT")) : 20);
    fuzz_target.run(copy, n);
    alarm(0);
    if (!g_replay) FZ_PURGE(stderr); /* diagnostics the target printed with stdio (sanitizer reports and this driver write to fd 2 directly) */
    free(copy);
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? (size_t)sz : 1);
    *n = fread(b, 1, sz > 0 ? (size_t)sz : 0, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    uint64_t iters = 0, seed = 1;
    double secs = 0;
    int replay = 0, quiet = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1];
        if (o == 'r') replay = 1;
        else if (o == 'q') quiet = 1;
        else if (i + 1 < argc && o == 'n') iters = strtoull(argv[++i], NULL, 0);
        else if (i + 1 < argc && o == 't') secs = atof(argv[++i]);
        else if (i + 1 < argc && o == 's') seed = strtoull(argv[++i], NULL, 0);
        else if (i + 1 < argc && o == 'm') g_maxlen = strtoull(argv[++i], NULL, 0);
        else if (i + 1 < argc && o == 'o') crash_dir = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-n iterations] [-t seconds] [-s seed] [-m max_len] [-o crash_dir] [-q] [-r] [file ...]\n", argv[0]);
            return 2;
        }
    }
    if (!iters && secs <= 0) iters = 20000;
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    if (!rng_state) rng_state = 1;
    {
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s", crash_dir);
        for (char *c = tmp + 1; *c; c++)
            if (*c == '/') *c = 0, mkdir(tmp, 0755), *c = '/';
        mkdir(tmp, 0755);
    }
    __sanitizer_set_death_callback(on_death);
    int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGALRM};
    for (unsigned k = 0; k < sizeof sigs / sizeof *sigs; k++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigaction(sigs[k], &sa, NULL);
    }
    if (replay) {
        g_replay = 1;
        for (; i < argc; i++) {
            size_t n;
            uint8_t *b = read_file(argv[i], &n);
            if (!b) {
                fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            run_one(b, n);
            free(b);
            if (!quiet) printf("replayed %s (%zu bytes)\n", argv[i], n);
        }
        return 0;
    }
    setvbuf(stderr, errbuf, _IOFBF, sizeof errbuf); /* the target's own stderr chatter is buffered and dropped after each input (see run_one) */
    fuzz_target.seeds(seed_cb, NULL);
    for (; i < argc; i++) {
        size_t n;
        uint8_t *b = read_file(argv[i], &n);
        if (b) add_corpus(b, n), free(b);
    }
    size_t nseeds = ncorpus;
    if (!ncorpus) add_corpus((const uint8_t *)"", 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t k = 0; k < ncorpus; k++) run_one(corpus[k].p, corpus[k].n); /* the seeds themselves first */
    uint8_t *work = malloc(g_maxlen + 256);
    uint64_t execs = ncorpus;
    for (g_iter = 0; iters ? g_iter < iters : 1; g_iter++) {
        if (secs > 0 && (g_iter & 63) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9 >= secs) break;
        }
        /* base: mostly a corpus entry, biased towards the ones added after the seeds (they reached new code) */
        size_t bi = ncorpus > nseeds && rnd() % 3 == 0 ? nseeds + rndn(ncorpus - nseeds) : rndn(ncorpus);
        const Buf *base = &corpus[bi], *other = &corpus[rndn(ncorpus)];
        Buf b = {work, base->n};
        memcpy(work, base->p, base->n);
        unsigned nm = 1 + (unsigned)(rnd() % 4);
        for (unsigned m = 0; m < nm; m++) mutate_once(&b, g_maxlen, other);
        uint64_t before = edges_seen();
        run_one(b.p, b.n);
        execs++;
        if (edges_seen() != before) add_corpus(b.p, b.n); /* new code reached */
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!quiet)
        printf("FUZZ %s seed=%llu execs=%llu corpus=%zu (seeds %zu) edges=%llu time=%.1fs OK\n", fuzz_target.name, (unsigned long long)seed,
               (unsigned long long)execs, ncorpus, nseeds, (unsigned long long)edges_seen(),
               (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    return 0;
}
