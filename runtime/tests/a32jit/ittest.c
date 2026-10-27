/* Differential test of lifted Thumb code with IT blocks and short instruction sequences (the lifter tracks the IT state statically; the interpreter
 * tracks it at run time). Each sample is `it<x><cond>` followed by random Thumb instructions, lifted as one run starting at the IT; both the
 * interpreter and the lifted code then run until the pc leaves the sample, and registers, flags and the data arena are compared.
 *   cc -std=gnu11 -O2 -DNAPPLE_A32_INTERP -Isrc/a32 -o ittest tests/a32jit/ittest.c src/a32jit/a32lift.c src/a32/a32_{exec,decode_arm,decode_thumb,vfp,neon}.c && ./ittest [--count N] [--seed S] [--show N] */
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
#define STRIDE 64u
#define ARENA_LO 0x10000u
#define ARENA_SZ 0x10000u

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd64(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 2685821657736338717ull; }
static uint32_t rnd(void) { return (uint32_t)(rnd64() >> 16); }
static uint32_t rnd_reg(void) {
    uint32_t k = rnd() % 100;
    if (k < 45) return ARENA_LO + 0x400 + ((rnd() & 0x3fff) & ~(rnd() & 3 ? 3u : 0u));
    if (k < 70) { static const uint32_t c[13] = {0, 1, 2, 3, 0xff, 0x80, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x8000, 0xffff, 0x10000}; return c[rnd() % 13]; }
    return rnd();
}
static int h_svc(A32Cpu *c, uint32_t imm) { if (imm == 0x7f) return 1; c->r[0] = 0x1234 + imm; return 0; }   /* svc #0x7f ends a sample */
static int h_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; return 1; }
static uint64_t h_cnt(A32Cpu *c, int w) { (void)c; return w ? 19200000 : 123456789012ull; }
static const A32Hooks hooks = { h_svc, h_hle, h_cnt };
static int tracing;
static void dump(const char *tag, const A32Cpu *c) {
    printf("   %s pc %08x it %02x t%u nzcv %u%u%u%u r:", tag, c->r[15], c->itstate, c->thumb, c->n, c->z, c->c, c->v);
    for (int i = 0; i < 15; i++) printf(" %x", c->r[i]);
    printf("\n");
}
static uint32_t jit_step(A32Cpu *c, const A32Lit *l) { if (tracing) dump("jit-fallback", c); return (uint32_t)a32_op_exec(c, &l->insn, c->r[15], l->raw); }
static int hle_hit;
static uint32_t jit_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; hle_hit = 1; return 1; }   /* a random branch into the import range: not comparable, counted as skipped */
static sigjmp_buf jb;
static void *fault_addr;
static void on_segv(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)ctx; fault_addr = si->si_addr; siglongjmp(jb, 1); }

typedef struct { uint8_t code[STRIDE]; unsigned len; uint32_t regs[15], flags, ge; } Sample;

static void setup(A32Cpu *c, uint8_t *mem, const Sample *s, uint32_t pc, void *cache) {
    memset(c, 0, sizeof *c); c->cache = cache;
    c->mem = mem; c->hooks = &hooks; c->fpexc_en = 1 << 30; c->tpidruro = 0x4000;
    memcpy(c->r, s->regs, sizeof s->regs);
    c->r[15] = pc; c->thumb = 1;
    c->n = s->flags >> 4 & 1; c->z = s->flags >> 3 & 1; c->c = s->flags >> 2 & 1; c->v = s->flags >> 1 & 1; c->q = s->flags & 1; c->ge = s->ge;
}

int main(int argc, char **argv) {
    int count = 5000, show = 20, keep = 0; unsigned long seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep")) keep = 1;
    }
    rs ^= seed * 0x9E3779B97F4A7C15ull; for (int i = 0; i < 8; i++) rnd64();
    uint8_t *win = mmap((void *)(8ull << 32), (1ull << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    uint8_t *winj = mmap((void *)(24ull << 32), (1ull << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (win == MAP_FAILED || winj == MAP_FAILED) { perror("mmap"); return 1; }
    size_t csz = ((size_t)STRIDE * ((size_t)count + 4) + 0x3fff) & ~(size_t)0x3fff;
    uint8_t *w2[2] = { win, winj };
    for (int i = 0; i < 2; i++) { mprotect(w2[i] + ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE); mprotect(w2[i] + CODE_BASE, csz, PROT_READ | PROT_WRITE); }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Sample *smp = calloc((size_t)count, sizeof *smp);
    uint32_t *seeds = calloc((size_t)count, 4);
    static const uint32_t hws[] = {0xe800, 0xe900, 0xea00, 0xeb00, 0xf000, 0xf100, 0xf200, 0xf300, 0xf400, 0xf500, 0xf600, 0xf700, 0xf800, 0xf900, 0xfa00, 0xfb00};
    for (int k = 0; k < count; k++) {
        Sample *s = &smp[k];
        uint32_t cond = rnd() % 14, mask;
        do mask = rnd() & 15; while (!mask);
        unsigned n = 4 - (unsigned)__builtin_ctz(mask);   /* instructions in the block */
        uint16_t it = (uint16_t)(0xbf00 | cond << 4 | mask);
        unsigned len = 0;
        memcpy(s->code + len, &it, 2); len += 2;
        for (unsigned j = 0; j < n; j++) {
            for (;;) {   /* a random instruction that stays in the sample: no branches, no pc writes, defined */
                uint8_t b4[4]; unsigned l;
                if (rnd() % 100 < 55) {
                    uint16_t h;
                    do h = (uint16_t)rnd(); while ((h >> 11) >= 0x1d || (h & 0xff00) == 0xbf00);
                    memcpy(b4, &h, 2); uint16_t z = 0xbf00; memcpy(b4 + 2, &z, 2); l = 2;
                } else {
                    uint16_t h1 = (uint16_t)(hws[rnd() % 16] | (rnd() & 0xff)), h2 = (uint16_t)rnd();
                    memcpy(b4, &h1, 2); memcpy(b4 + 2, &h2, 2); l = 4;
                }
                uint16_t d1, d2; memcpy(&d1, b4, 2); memcpy(&d2, b4 + 2, 2);
                A32Insn in; memset(&in, 0, sizeof in);
                unsigned dl = a32_decode_thumb(d1, d2, &in);
                if (dl != l || in.op == OP_UNDEF || in.op == OP_UNIMPL || in.op == OP_IT) continue;
                int bad = 0;
                switch (in.op) {
                case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG: case OP_CBZ: case OP_TB: case OP_SVC: case OP_BKPT: case OP_LDREX: case OP_STREX: case OP_VFP: case OP_NEON: bad = 1; break;   /* exclusives: the host faults on unaligned addresses */
                case OP_DP: bad = in.rd == 15 && !(in.sub >= 8 && in.sub <= 11); break;
                case OP_LDST: bad = in.rd == 15 || in.ra == 15 || in.rn == 15; break;
                case OP_LDM: bad = (in.imm >> 15 & 1) || in.rn == 15; break;
                default: break;
                }
                if (bad) continue;
                memcpy(s->code + len, b4, l); len += l;
                break;
            }
        }
        { uint16_t end = 0xdf7f; memcpy(s->code + len, &end, 2); len += 2; }   /* svc #0x7f: stops both runs */
        s->len = len;
        for (; len + 2 <= STRIDE; len += 2) { uint16_t nop = 0xbf00; memcpy(s->code + len, &nop, 2); }
        for (int r = 0; r < 15; r++) s->regs[r] = rnd_reg();
        s->flags = rnd() & 31; s->ge = rnd() & 15;
        for (int r = 0; r < 15; r++) if ((s->regs[r] > ARENA_LO + 0x4000 + 0x100) && rnd() % 2) s->regs[r] = ARENA_LO + 0x400 + (rnd() & 0x3ff0);
        seeds[k] = (CODE_BASE + STRIDE * (uint32_t)k) | 1u;
        for (int i = 0; i < 2; i++) memcpy(w2[i] + CODE_BASE + STRIDE * k, s->code, STRIDE);
    }
    char dir[] = "/tmp/a32jit_it_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char pre[256]; snprintf(pre, sizeof pre, "%s/t", dir);
    LiftModule lm = { "test", CODE_BASE, (uint32_t)(STRIDE * (unsigned)count + 64), 0, 0 };
    LiftInput in = { .mem = win, .mods = &lm, .nmods = 1, .seeds = seeds, .nseeds = (unsigned)count, .each_entry = 0 };
    LiftStats st; char **files;
    int nfiles = a32lift_run(&in, pre, &files, &st);
    if (nfiles < 0) return 1;
    char cmd[4096];
    for (int f = 0; f < nfiles; f++) {
        snprintf(cmd, sizeof cmd, "clang -c -target arm64-apple-macos13 -march=armv8.4-a %s -o %s.o", files[f], files[f]);
        if (system(cmd)) return 1;
    }
    snprintf(cmd, sizeof cmd, "clang -dynamiclib -target arm64-apple-macos13 -o %s/t.dylib %s/t.*.S.o", dir, dir);
    if (system(cmd)) return 1;
    snprintf(cmd, sizeof cmd, "%s/t.dylib", dir);
    void *h = dlopen(cmd, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*enter)(A32Cpu *, int64_t) = dlsym(h, "a32jit_enter");
    { void **hk = dlsym(h, "a32jit_hooks"); hk[0] = (void *)jit_step; hk[1] = (void *)jit_hle; }

    uint8_t *tmpl = malloc(ARENA_SZ), *ai = malloc(ARENA_SZ);
    A32Cpu ci, cj; void *cache_i = NULL, *cache_j = NULL;
    unsigned long cmp = 0, mism = 0, skipped = 0, faults = 0;
    for (int k = 0; k < count; k++) {
        const Sample *s = &smp[k];
        uint32_t pc = CODE_BASE + STRIDE * (uint32_t)k, lo = pc, hi = pc + s->len;
        uint8_t blk[64]; for (int b = 0; b < 64; b++) blk[b] = (uint8_t)rnd();
        for (size_t b = 0; b < ARENA_SZ; b += 64) memcpy(tmpl + b, blk, 64);
        for (int i = 0; i < 2; i++) memcpy(w2[i] + pc, s->code, STRIDE);
        memcpy(win + ARENA_LO, tmpl, ARENA_SZ);
        setup(&ci, win, s, pc, cache_i);
        A32Exit e = A32_EXIT_BUDGET; int steps = 0;
        if (sigsetjmp(jb, 1) == 0) { while (ci.r[15] >= lo && ci.r[15] < hi && steps++ < 64) { e = a32_run(&ci, 1); if (e != A32_EXIT_BUDGET) break; } } else { faults++; cache_i = ci.cache; continue; }
        cache_i = ci.cache;
        if ((e != A32_EXIT_BUDGET && e != A32_EXIT_STOP) || steps >= 64) { skipped++; continue; }
        memcpy(ai, win + ARENA_LO, ARENA_SZ);
        memcpy(winj + ARENA_LO, tmpl, ARENA_SZ);
        setup(&cj, winj, s, pc, cache_j);
        int r = 0; steps = 0; hle_hit = 0;
        if (sigsetjmp(jb, 1) == 0) {
            for (;;) {   /* the dispatcher: lifted code, and the interpreter for pcs inside the sample that are not entries */
                r = enter(&cj, 1000);
                if (r != 100 || cj.r[15] < lo || cj.r[15] >= hi || steps++ > 64) break;
                A32Exit e2 = a32_run(&cj, 1); if (e2 != A32_EXIT_BUDGET) { r = e2; break; }
            }
        } else r = -1;
        cache_j = cj.cache;
        if (hle_hit) { skipped++; continue; }
        char why[512] = ""; size_t wl = 0;
#define NOTE(...) do { if (wl < sizeof why - 80) wl += (size_t)snprintf(why + wl, sizeof why - wl, __VA_ARGS__); } while (0)
        if (r == -1) NOTE("lifted code faulted at host %p (window %p) ", fault_addr, (void *)winj);
        else if (r != 0 && r != 100 && r != (int)(e == A32_EXIT_STOP ? A32_EXIT_STOP : 100)) NOTE("exit %d ", r);
        for (int i = 0; i < 15; i++) if (ci.r[i] != cj.r[i]) NOTE("r%d i=%08x j=%08x ", i, ci.r[i], cj.r[i]);
        if (ci.r[15] != cj.r[15]) NOTE("pc i=%08x j=%08x ", ci.r[15], cj.r[15]);
        if (ci.thumb != cj.thumb) NOTE("thumb i=%u j=%u ", ci.thumb, cj.thumb);
        if (ci.n != cj.n || ci.z != cj.z || ci.c != cj.c || ci.v != cj.v) NOTE("nzcv i=%u%u%u%u j=%u%u%u%u ", ci.n, ci.z, ci.c, ci.v, cj.n, cj.z, cj.c, cj.v);
        if (ci.q != cj.q || ci.ge != cj.ge) NOTE("q/ge ");
        if (memcmp(ai, winj + ARENA_LO, ARENA_SZ)) NOTE("memory ");
        cmp++;
        if (wl) {
            mism++;
            if (mism <= (unsigned long)show) {
                printf("MISMATCH sample %d (pc %#x) it-block %02x%02x:", k, pc, s->code[1], s->code[0]);
                for (unsigned o = 2; o < s->len; o += 2) printf(" %02x%02x", s->code[o + 1], s->code[o]);
                printf(": %s\n   in: ", why);
                for (int i = 0; i < 15; i++) printf("%08x ", s->regs[i]);
                printf("nzcvq=%02x\n", s->flags);
                if (mism <= 2) {   /* trace both runs of this sample */
                    tracing = 1;
                    memcpy(win + ARENA_LO, tmpl, ARENA_SZ); setup(&ci, win, s, pc, cache_i);
                    if (sigsetjmp(jb, 1) == 0) for (int st2 = 0; st2 < 12 && ci.r[15] >= lo && ci.r[15] < hi; st2++) { dump("interp", &ci); if (a32_run(&ci, 1) != A32_EXIT_BUDGET) break; }
                    memcpy(winj + ARENA_LO, tmpl, ARENA_SZ); setup(&cj, winj, s, pc, cache_j);
                    if (sigsetjmp(jb, 1) == 0) enter(&cj, 1000); else printf("   (jit faulted)\n");
                    tracing = 0;
                }
            }
        }
    }
    printf("it-block samples compared %lu, skipped %lu (interpreter rejects or loops) + %lu (memory outside the arena), mismatches %lu\n", cmp, skipped, faults, mism);
    if (keep) printf("kept %s (sample k is at %#x)\n", dir, CODE_BASE); else { snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {} }
    return mism ? 1 : 0;
}
