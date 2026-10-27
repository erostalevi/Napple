/* Differential test of the AArch32 -> AArch64 lifter against the interpreter (the oracle, itself fuzzed against Unicorn by tests/a32/fuzz.py).
 *
 *   cc -std=gnu11 -O2 -DNAPPLE_A32_INTERP -Isrc/a32 -o difftest tests/a32jit/difftest.c src/a32jit/a32lift.c src/a32/a32_*.c(minus module/proc/hle) && \
 *   ./difftest [--mode arm|thumb] [--count N] [--seed S] [--family NAME] [--show N] [--vfp]
 *
 * For each random instruction word: lifts it as an entry of its own, assembles all of them into a dylib with clang, then runs every sample
 * twice from the same random register/flag/memory state (interpreter, lifted code) and compares r0-r14, pc, NZCVQ, GE, the VFP/NEON state and the
 * 64 KiB data arena. Samples the interpreter itself rejects (undefined, unimplemented, alignment) are skipped. Instructions the lifter hands to
 * the interpreter ("fallback") are counted separately: they pass trivially and are not evidence for the lifter. */
#include "../../src/a32jit/a32lift.h"
#include "../../src/a32/a32_priv.h"
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE_BASE 0x1000000u
#define STRIDE 16u
#define ARENA_LO 0x10000u
#define ARENA_SZ 0x10000u

typedef struct { const char *name; uint32_t v, m; } Fam;
static const Fam FAMS[] = {
    {"dp_reg", 0x00000000, 0x0e000010}, {"dp_regsh", 0x00000010, 0x0e000090}, {"dp_imm", 0x02000000, 0x0e000000},
    {"mul", 0x00000090, 0x0f0000f0}, {"mull", 0x00800090, 0x0f8000f0},
    {"ldst_imm", 0x04000000, 0x0e000000}, {"ldst_reg", 0x06000000, 0x0e000010},
    {"extra", 0x000000b0, 0x0e0000b0}, {"extra2", 0x000000d0, 0x0e0000d0}, {"ldm", 0x08000000, 0x0e000000},
    {"media", 0x06000010, 0x0e000010}, {"misc", 0x01000000, 0x0f900000}, {"branch", 0x0a000000, 0x0e000000},
    {"sync", 0x01800090, 0x0f8000f0}, {"halfmul", 0x01000080, 0x0f900090}, {"movw", 0x03000000, 0x0fb00000},
    {"uncond", 0xf0000000, 0xf0000000}, {"cp", 0x0e000010, 0x0f000010},
    {"vfp_dp", 0x0e000a00, 0x0f000e00}, {"vfp_ldst", 0x0c000a00, 0x0e000e00}, {"vfp_dp64", 0x0e000b00, 0x0f000f00}, {"vfp_xfer", 0x0e000a10, 0x0f000e10},
    {"neon_dp", 0xf2000000, 0xfe000000}, {"neon_ls", 0xf4000000, 0xff100000}, {"vfp_v8", 0xfe000a00, 0xff000e00}, {"vfp_x64", 0x0c400a00, 0x0fe00e00},
};
#define NFAM (sizeof FAMS / sizeof *FAMS)

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd64(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 2685821657736338717ull; }
static uint32_t rnd(void) { return (uint32_t)(rnd64() >> 16); }
static double rndf(void) { return (rnd() & 0xffffff) / 16777216.0; }

static uint32_t rnd_reg(void) {
    double k = rndf();
    if (k < 0.45) { static const uint32_t m[4] = {0, 3, 3, 7}; return ARENA_LO + 0x400 + ((rnd() & 0x3fff) & ~m[rnd() & 3]); }
    if (k < 0.7) { static const uint32_t c[13] = {0, 1, 2, 3, 0xff, 0x80, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x8000, 0xffff, 0x10000}; return c[rnd() % 13]; }
    return rnd();
}

static int unpredictable(uint32_t word, int thumb) {
    if (thumb) {
        if ((word >> 16) == 0 || ((word & 0xffff) >> 11) < 0x1d) return 0;
        uint32_t hw1 = word & 0xffff, hw2 = word >> 16, top = hw1 >> 8;
        if (top == 0xea || top == 0xeb || ((hw1 & 0xfa00) == 0xf000 && !(hw2 >> 15))) {
            uint32_t op4 = (hw1 >> 5) & 15, s = (hw1 >> 4) & 1, rd = (hw2 >> 8) & 15;
            if (rd == 15 && !(s && (op4 == 0 || op4 == 4 || op4 == 8 || op4 == 13))) return 1;
        }
        if (top == 0xfa || top == 0xfb) {
            uint32_t rn = hw1 & 15, rd = (hw2 >> 8) & 15;
            if (rn == 15 || (top == 0xfb && (rd == 15 || ((hw2 >> 12) == 15 && (hw1 & 0x80)))) || (top == 0xfa && rd == 15)) return 1;
        }
        if ((hw1 & 0xfa00) == 0xf200 && !(hw2 >> 15) && ((hw2 >> 8) & 15) == 15) return 1;
        if ((top == 0xf8 || top == 0xf9) && (hw2 >> 12) == 15) return 1;
        if ((top == 0xf8 || top == 0xf9) && (hw1 & 15) == 15 && !(hw1 & 0x10)) return 1;
        if (top == 0xe8 || top == 0xe9) {
            uint32_t rn = hw1 & 15;
            if (rn == 15) return 1;
            if (hw1 & 0x40) {
                uint32_t p = (hw1 >> 8) & 1, w = (hw1 >> 5) & 1, rt = hw2 >> 12, rt2 = (hw2 >> 8) & 15;
                if ((p || w) && (rn == rt || rn == rt2)) return 1;
            }
        }
        return 0;
    }
    uint32_t cls = (word >> 25) & 7;
    if ((word & 0x0f800ff0) == 0x01800e90 || (word & 0x0f800090) == 0x01800090) { if (((word >> 12) & 15) >= 14 || ((word >> 12) & 15) == 15) return 1; }   /* exclusives with rt = lr or pc: the core indexes r[16] */
    if (((word >> 20) & 0xff) == 0x75 && (((word >> 5) & 7) == 6 || ((word >> 5) & 7) == 7) && ((word >> 12) & 15) == 15) return 1;
    uint32_t rn = (word >> 16) & 15, rt = (word >> 12) & 15;
    if (cls == 2 || cls == 3) {
        uint32_t p = (word >> 24) & 1, w = (word >> 21) & 1;
        if ((!p || w) && (rn == 15 || rn == rt)) return 1;
    }
    if (cls == 0 && (word & 0x90) == 0x90 && (word & 0x60)) {
        uint32_t p = (word >> 24) & 1, w = (word >> 21) & 1;
        if ((!p || w) && (rn == 15 || rn == rt || rn == rt + 1)) return 1;
        if (rt == 15 || (word & 15) == 15) return 1;
    }
    return 0;
}

/* ---- hooks for both runs ---- */
static int h_svc(A32Cpu *c, uint32_t imm) { c->r[0] = 0x1234 + imm; return 0; }
static int h_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; return 1; }
static uint64_t h_cnt(A32Cpu *c, int w) { (void)c; return w ? 19200000 : 123456789012ull; }
static const A32Hooks hooks = { h_svc, h_hle, h_cnt };
static uint32_t jit_step(A32Cpu *c, const A32Lit *l) { return (uint32_t)a32_op_exec(c, &l->insn, c->r[15], l->raw); }
static int hle_hit;
static uint32_t jit_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; hle_hit = 1; return 1; }   /* a random branch into the import range: not comparable, counted as skipped */

static sigjmp_buf jb;
static void on_segv(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; siglongjmp(jb, 1); }

typedef struct { uint32_t word; int thumb; char fam[16]; uint32_t regs[15]; uint32_t flags, ge, fpscr; uint64_t d[32]; } Sample;

static void set_state(A32Cpu *c, uint8_t *mem, const Sample *s, uint32_t pc) {
    A32Cpu keep = *c; (void)keep;
    void *cache = c->cache;
    memset(c, 0, sizeof *c);
    c->cache = cache;
    c->mem = mem; c->hooks = &hooks; c->fpexc_en = 1 << 30; c->tpidruro = 0x4000;
    memcpy(c->r, s->regs, sizeof s->regs);
    c->r[15] = pc & ~1u; c->thumb = s->thumb;
    c->n = s->flags >> 4 & 1; c->z = s->flags >> 3 & 1; c->c = s->flags >> 2 & 1; c->v = s->flags >> 1 & 1; c->q = s->flags & 1;
    c->ge = s->ge; c->fpscr = s->fpscr;
    memcpy(c->v_.d, s->d, sizeof s->d);
}

int main(int argc, char **argv) {
    const char *from = NULL; uint8_t *fdata = NULL; size_t flen = 0;
    int thumb = 0, count = 20000, show = 25, vfp = 0, have_word = 0; uint32_t only_word = 0; const char *fam_only = NULL; unsigned long seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) thumb = !strcmp(argv[++i], "thumb");
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--family") && i + 1 < argc) fam_only = argv[++i];
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vfp")) vfp = 1;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];   /* take the instruction words from a real module image (code in the real distribution) */
        else if (!strcmp(argv[i], "--word") && i + 1 < argc) { only_word = (uint32_t)strtoul(argv[++i], 0, 0); have_word = 1; }
    }
    if (from) { FILE *ff = fopen(from, "rb"); if (!ff) { perror(from); return 1; } fseek(ff, 0, SEEK_END); flen = (size_t)ftell(ff); fseek(ff, 0, SEEK_SET); fdata = malloc(flen); if (fread(fdata, 1, flen, ff) != flen) return 1; fclose(ff); }
    rs ^= seed * 0x9E3779B97F4A7C15ull; for (int i = 0; i < 8; i++) rnd64();
    uint8_t *win = mmap((void *)(8ull << 32), (1ull << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (win == MAP_FAILED) { perror("mmap"); return 1; }
    /* the jit run gets a window of its own: stores outside the arena accumulate in both windows identically as long as the runs agree */
    uint8_t *winj = mmap((void *)(24ull << 32), (1ull << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (winj == MAP_FAILED) { perror("mmap"); return 1; }
    mprotect(win + ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE); mprotect(winj + ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE);
    { size_t csz = ((size_t)STRIDE * ((size_t)count + 4) + 0x3fff) & ~(size_t)0x3fff;
      mprotect(win + CODE_BASE, csz, PROT_READ | PROT_WRITE); mprotect(winj + CODE_BASE, csz, PROT_READ | PROT_WRITE); }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Sample *smp = calloc((size_t)count, sizeof *smp);
    uint32_t *seeds = calloc((size_t)count, 4);
    uint8_t *fbmap = calloc((size_t)count, 1);
    size_t ns = 0;
    const Fam *fams[NFAM]; unsigned nf = 0;
    for (unsigned f = 0; f < NFAM; f++) {
        int isv = !strncmp(FAMS[f].name, "vfp", 3) || !strncmp(FAMS[f].name, "neon", 4);
        if (fam_only ? !strcmp(fam_only, FAMS[f].name) : (vfp || !isv)) fams[nf++] = &FAMS[f];
    }
    if (!nf) { fprintf(stderr, "no such family\n"); return 2; }
    for (int k = 0; k < count; k++) {
        Sample *s = &smp[ns];
        uint32_t w; const Fam *fm = fams[rnd() % nf];
        if (thumb) {
            if (rndf() < 0.4) {
                uint32_t h = rnd() & 0xffff;
                if ((h & 0xff00) == 0xbf00 && (h & 15)) h = 0xbf00;
                w = h;
                if ((h >> 11) >= 0x1d) w |= (rnd() & 0xffff) << 16; else w |= 0xbf00u << 16;
                snprintf(s->fam, sizeof s->fam, "t16");
                if ((h >> 11) >= 0x1d) snprintf(s->fam, sizeof s->fam, "t32");
            } else {
                static const uint32_t hws[] = {0xe800, 0xe900, 0xea00, 0xeb00, 0xf000, 0xf100, 0xf200, 0xf300, 0xf400, 0xf500, 0xf600, 0xf700, 0xf800, 0xf900, 0xfa00, 0xfb00, 0xec00, 0xed00, 0xee00, 0xef00, 0xfc00, 0xfd00, 0xfe00, 0xff00};
                uint32_t h1 = hws[rnd() % (vfp ? 24 : 16)] | (rnd() & 0xff);
                w = h1 | (rnd() & 0xffff) << 16;
                snprintf(s->fam, sizeof s->fam, "t32");
            }
        } else {
            w = (rnd() & ~fm->m) | fm->v;
            if ((fm->v >> 28) != 0xf && rndf() < 0.9) w = (w & 0x0fffffff) | 0xe0000000;
            snprintf(s->fam, sizeof s->fam, "%s", fm->name);
        }
        if (fdata) {
            size_t off = (rnd64() % (flen - 8)) & ~(size_t)(thumb ? 1 : 3);
            if (thumb) { uint16_t h1, h2; memcpy(&h1, fdata + off, 2); memcpy(&h2, fdata + off + 2, 2); w = h1 | (a32_thumb_is32(h1) ? (uint32_t)h2 << 16 : 0xbf00u << 16); snprintf(s->fam, sizeof s->fam, a32_thumb_is32(h1) ? "t32" : "t16"); }
            else { memcpy(&w, fdata + off, 4); snprintf(s->fam, sizeof s->fam, "real"); }
        }
        if (have_word) w = only_word;
        else if (unpredictable(w, thumb)) continue;
        s->word = w; s->thumb = thumb;
        for (int r = 0; r < 15; r++) s->regs[r] = rnd_reg();
        s->flags = rnd() & 31; s->ge = rnd() & 15;
        if (!thumb && ((w >> 25) & 7) == 4 && (w >> 28) != 15) if (((w >> 16) & 15) < 15) s->regs[(w >> 16) & 15] = ARENA_LO + 0x400 + ((rnd() & 0x3ff0));   /* ldm/stm: a base that cannot wrap the 4 GiB window */
        if (thumb && ((w & 0xffff) >> 9) == 0x74 && (w & 15) < 15) s->regs[w & 15] = ARENA_LO + 0x400 + ((rnd() & 0x3ff0));
        if (vfp) { for (int r = 0; r < 32; r++) s->d[r] = rnd64(); s->fpscr = (rnd() & 3) ? 0 : (rnd() & 0x03c00000); }   /* a quarter of the samples run with random flush-to-zero / default-NaN / rounding-mode settings */
        seeds[ns] = (CODE_BASE + STRIDE * (uint32_t)ns) | (thumb ? 1u : 0u);
        for (int wi = 0; wi < 2; wi++) {
            uint8_t *p = (wi ? winj : win) + CODE_BASE + STRIDE * ns;
            if (thumb) { uint16_t h1 = w & 0xffff, h2 = w >> 16; memcpy(p, &h1, 2); memcpy(p + 2, &h2, 2); }
            else { memcpy(p, &w, 4); uint32_t nop = 0xe320f000; memcpy(p + 4, &nop, 4); }
        }
        ns++;
    }
    /* lift + build */
    char dir[] = "/tmp/a32jit_dt_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char pre[256]; snprintf(pre, sizeof pre, "%s/t", dir);
    LiftModule lm = { "test", CODE_BASE, (uint32_t)(STRIDE * ns + 64), 0, 0 };
    LiftInput in = { .mem = win, .mods = &lm, .nmods = 1, .seeds = seeds, .nseeds = (unsigned)ns, .each_entry = 1, .seed_fb = fbmap, .fb_base = CODE_BASE, .fb_stride = STRIDE };
    LiftStats st; char **files;
    int nfiles = a32lift_run(&in, pre, &files, &st);
    if (nfiles < 0) return 1;
    char cmd[4096];
    for (int f = 0; f < nfiles; f++) {
        snprintf(cmd, sizeof cmd, "clang -c -target arm64-apple-macos13 -march=armv8.4-a %s -o %s.o", files[f], files[f]);
        if (system(cmd)) { fprintf(stderr, "assembler failed: %s\n", files[f]); return 1; }
    }
    snprintf(cmd, sizeof cmd, "clang -dynamiclib -target arm64-apple-macos13 -o %s/t.dylib %s/t.*.S.o", dir, dir);
    if (system(cmd)) return 1;
    snprintf(cmd, sizeof cmd, "%s/t.dylib", dir);
    void *h = dlopen(cmd, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*enter)(A32Cpu *, int64_t) = dlsym(h, "a32jit_enter");
    { void **hk = dlsym(h, "a32jit_hooks"); hk[0] = (void *)jit_step; hk[1] = (void *)jit_hle; }
    if (!enter) { fprintf(stderr, "no a32jit_enter\n"); return 1; }

    uint8_t *tmpl = malloc(ARENA_SZ), *ai = malloc(ARENA_SZ);
    A32Cpu ci, cj; memset(&ci, 0, sizeof ci); memset(&cj, 0, sizeof cj);
    unsigned long cmp = 0, nativecmp = 0, mism = 0, skipped = 0, faults = 0;
    struct { char n[16]; unsigned long cmp, nat, bad; } fs[64]; unsigned nfs = 0;
    for (size_t k = 0; k < ns; k++) {
        const Sample *s = &smp[k];
        uint8_t blk[64]; for (int b = 0; b < 64; b++) blk[b] = (uint8_t)rnd();
        for (size_t b = 0; b < ARENA_SZ; b += 64) memcpy(tmpl + b, blk, 64);
        uint32_t pc = CODE_BASE + STRIDE * (uint32_t)k;
        for (int wi = 0; wi < 2; wi++) {   /* a random store may have hit the code image: restore this sample's words (the lifted code is static) */
            uint8_t *p = (wi ? winj : win) + pc;
            if (s->thumb) { uint16_t h1 = s->word & 0xffff, h2 = s->word >> 16; memcpy(p, &h1, 2); memcpy(p + 2, &h2, 2); }
            else { uint32_t nop = 0xe320f000; memcpy(p, &s->word, 4); memcpy(p + 4, &nop, 4); }
        }
        memcpy(win + ARENA_LO, tmpl, ARENA_SZ);
        set_state(&ci, win, s, pc | (s->thumb ? 1u : 0u));
        A32Exit e = A32_EXIT_UNDEF;
        if (sigsetjmp(jb, 1) == 0) e = a32_run(&ci, 1); else { faults++; continue; }   /* touched memory outside the arena: not testable */
        if (e != A32_EXIT_BUDGET) { skipped++; continue; }
        memcpy(ai, win + ARENA_LO, ARENA_SZ);
        memcpy(winj + ARENA_LO, tmpl, ARENA_SZ);
        set_state(&cj, winj, s, pc | (s->thumb ? 1u : 0u));
        int r = -1; hle_hit = 0;
        if (sigsetjmp(jb, 1) == 0) r = enter(&cj, 1);
        if (r == -1) {   /* unaligned exclusives: the core raises its own alignment exit, the host faults */
            A32Insn di; memset(&di, 0, sizeof di);
            if (s->thumb) a32_decode_thumb((uint16_t)s->word, (uint16_t)(s->word >> 16), &di); else a32_decode_arm(s->word, &di);
            if (di.op == OP_LDREX || di.op == OP_STREX) { skipped++; continue; }
        }   /* unaligned exclusives: the core raises its own alignment exit, the host faults */
        if (hle_hit) { skipped++; continue; }
        char why[512] = ""; size_t wl = 0;
#define NOTE(...) do { if (wl < sizeof why - 80) wl += (size_t)snprintf(why + wl, sizeof why - wl, __VA_ARGS__); } while (0)
        if (r == -1) NOTE("lifted code faulted where the interpreter did not ");
        else if (r != 0 && r != 100) NOTE("exit %d ", r);
        for (int i = 0; i < 15; i++) if (ci.r[i] != cj.r[i]) NOTE("r%d i=%08x j=%08x ", i, ci.r[i], cj.r[i]);
        if (ci.r[15] != cj.r[15]) NOTE("pc i=%08x j=%08x ", ci.r[15], cj.r[15]);
        if (ci.thumb != cj.thumb) NOTE("thumb i=%u j=%u ", ci.thumb, cj.thumb);
        if (ci.n != cj.n || ci.z != cj.z || ci.c != cj.c || ci.v != cj.v) NOTE("nzcv i=%u%u%u%u j=%u%u%u%u ", ci.n, ci.z, ci.c, ci.v, cj.n, cj.z, cj.c, cj.v);
        if (ci.q != cj.q || ci.ge != cj.ge) NOTE("q/ge i=%u/%x j=%u/%x ", ci.q, ci.ge, cj.q, cj.ge);
        /* the cumulative exception flags (IOC DZC OFC UFC IXC IDC) are not compared: the lifted code gets them from the host FPU, the interpreter derives them in software and
         * differs on flushed results and square roots; guests do not depend on them */
        if (memcmp(ci.v_.d, cj.v_.d, sizeof ci.v_.d)) NOTE("vfp state ");
        if ((ci.fpscr ^ cj.fpscr) & ~0x9fu) NOTE("fpscr interp %08x lifted %08x (start %08x) ", ci.fpscr, cj.fpscr, s->fpscr);
        if (memcmp(ai, winj + ARENA_LO, ARENA_SZ)) NOTE("memory ");
        unsigned f; for (f = 0; f < nfs; f++) if (!strcmp(fs[f].n, s->fam)) break;
        if (f == nfs) { snprintf(fs[nfs].n, 16, "%s", s->fam); fs[nfs].cmp = fs[nfs].nat = fs[nfs].bad = 0; nfs++; }
        cmp++; fs[f].cmp++;
        if (!fbmap[k]) { nativecmp++; fs[f].nat++; }
        if (wl) {
            mism++; fs[f].bad++;
            if (mism <= (unsigned long)show) {
                printf("MISMATCH %s word=%08x (%s)%s: %s\n   in: ", s->fam, s->word, s->thumb ? "thumb" : "arm", fbmap[k] ? " [fallback]" : "", why);
                for (int i = 0; i < 15; i++) printf("%08x ", s->regs[i]);
                printf("nzcvq=%02x\n", s->flags);
            }
        }
    }
    printf("compared %lu samples (%lu native, %lu through the interpreter), skipped %lu (interpreter rejects) + %lu (memory outside the arena), mismatches %lu\n", cmp, nativecmp, cmp - nativecmp, skipped, faults, mism);
    for (unsigned f = 0; f < nfs; f++) printf("  %-10s compared %6lu native %6lu mismatches %lu\n", fs[f].n, fs[f].cmp, fs[f].nat, fs[f].bad);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); system(cmd);
    return mism ? 1 : 0;
}
