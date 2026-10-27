/* Whole-program test of the AArch32 lifter (tests/a32jit/progtest.py drives it).
 *
 *   progtest <flat binary> <thumb 0|1> <r0> <r1> <r2> <r3>
 *
 * The binary is the .text of a program compiled for armv7a (ARM or Thumb-2, NEON and VFPv4 enabled); its entry point is offset 0. The program is
 * run twice from the same state, in the interpreter (the oracle) and as lifted code (a32lift output assembled into a dylib), and r0..r12, the flags,
 * the VFP/NEON registers and the stack/scratch arena are compared. Prints "r0=<hex>" (the interpreter's) for the caller, which compares it with the
 * same C source compiled for the host. Exit status 0 = identical. Build: see progtest.py (needs -DNAPPLE_A32_INTERP). */
#define _DARWIN_C_SOURCE
#include "../../src/a32jit/a32lift.h"
#include "../../src/a32/a32_priv.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE_BASE 0x1000000u
#define STACK_TOP 0x2100000u
#define ARENA_LO 0x2000000u
#define ARENA_SZ 0x100000u
#define HLE_ADDR 0xf0000000u

static int h_svc(A32Cpu *c, uint32_t imm) {
    (void)imm;
    return 1;
}
static int h_hle(A32Cpu *c, uint32_t pc) {
    (void)c;
    (void)pc;
    return 1;
}
static uint64_t h_cnt(A32Cpu *c, int w) {
    (void)c;
    return w ? 19200000 : 123456789012ull;
}
static const A32Hooks hooks = {h_svc, h_hle, h_cnt};
static uint32_t jit_op(A32Cpu *c, const A32Lit *l) { return (uint32_t)a32_op_exec(c, &l->insn, c->r[15], l->raw); }
static uint32_t jit_hle(A32Cpu *c, uint32_t pc) {
    (void)c;
    (void)pc;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: progtest <bin> <thumb> r0 r1 r2 r3\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bin = malloc((size_t)len + 16);
    if (fread(bin, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);
    int thumb = atoi(argv[2]);
    uint32_t args[4];
    for (int i = 0; i < 4; i++) args[i] = (uint32_t)strtoul(argv[3 + i], 0, 0);
    uint8_t *w[2];
    for (int k = 0; k < 2; k++) {
        w[k] = mmap((void *)((uint64_t)(8 + 16 * k) << 32), (1ull << 32) + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (w[k] == MAP_FAILED) {
            perror("mmap");
            return 2;
        }
        mprotect(w[k] + CODE_BASE, ((size_t)len + 0x4000) & ~(size_t)0x3fff, PROT_READ | PROT_WRITE);
        memcpy(w[k] + CODE_BASE, bin, (size_t)len);
        mprotect(w[k] + ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE);
    }
    /* lift */
    char dir[] = "/tmp/a32jit_pt_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char pre[256], cmd[4096];
    snprintf(pre, sizeof pre, "%s/t", dir);
    uint32_t seed = CODE_BASE | (uint32_t)thumb;
    LiftModule lm = {"prog", CODE_BASE, (uint32_t)len, 0, 0};
    LiftInput in = {.mem = w[1], .mods = &lm, .nmods = 1, .seeds = &seed, .nseeds = 1};
    LiftStats st;
    char **files;
    int nfiles = a32lift_run(&in, pre, &files, &st);
    if (nfiles < 0) return 2;
    for (int i = 0; i < nfiles; i++) {
        snprintf(cmd, sizeof cmd, "clang -c -target arm64-apple-macos13 -march=armv8.4-a %s -o %s.o", files[i], files[i]);
        if (system(cmd)) return 2;
    }
    snprintf(cmd, sizeof cmd, "clang -dynamiclib -target arm64-apple-macos13 -o %s/t.dylib %s/t.*.S.o", dir, dir);
    if (system(cmd)) return 2;
    snprintf(cmd, sizeof cmd, "%s/t.dylib", dir);
    void *h = dlopen(cmd, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    int (*enter)(A32Cpu *, int64_t) = dlsym(h, "a32jit_enter");
    void **hk = dlsym(h, "a32jit_hooks");
    hk[0] = (void *)jit_op;
    hk[1] = (void *)jit_hle;
    A32Cpu c[2];
    for (int k = 0; k < 2; k++) {
        memset(&c[k], 0, sizeof c[k]);
        c[k].mem = w[k];
        c[k].hooks = &hooks;
        c[k].fpexc_en = 1 << 30;
        c[k].tpidruro = 0x2000100;
        memcpy(c[k].r, args, sizeof args);
        c[k].r[13] = STACK_TOP - 16;
        c[k].r[14] = HLE_ADDR | (uint32_t)thumb;
        c[k].r[15] = CODE_BASE;
        c[k].thumb = (uint32_t)thumb;
        c[k].hle_lo = HLE_ADDR;
        c[k].hle_hi = HLE_ADDR + 0x1000;
        c[k].fpscr = 0;
    }
    A32Exit e0 = A32_EXIT_BUDGET;
    for (int i = 0; i < 2000 && e0 == A32_EXIT_BUDGET; i++) e0 = a32_run(&c[0], 10000000);
    int e1 = 0;
    for (int i = 0; i < 2000; i++) {
        e1 = enter(&c[1], 10000000);
        if (e1 != A32_EXIT_BUDGET) break;
    }
    int bad = 0;
    if (e0 != A32_EXIT_STOP || e1 != A32_EXIT_STOP) {
        fprintf(stderr, "exit: interpreter %d lifted %d (1 = returned)\n", e0, e1);
        bad = 1;
    }
    for (int r = 0; r <= 12; r++)
        if (c[0].r[r] != c[1].r[r]) {
            fprintf(stderr, "r%d interpreter %08x lifted %08x\n", r, c[0].r[r], c[1].r[r]);
            bad = 1;
        }
    if (c[0].r[13] != c[1].r[13]) bad = 1;
    for (int d = 0; d < 32; d++)
        if (c[0].v_.d[d] != c[1].v_.d[d]) {
            fprintf(stderr, "d%d interpreter %016llx lifted %016llx\n", d, (unsigned long long)c[0].v_.d[d], (unsigned long long)c[1].v_.d[d]);
            bad = 1;
        }
    if (memcmp(w[0] + ARENA_LO, w[1] + ARENA_LO, ARENA_SZ)) {
        fprintf(stderr, "arena differs\n");
        bad = 1;
    }
    fprintf(stderr, "lifted: %llu instructions inline, %llu out-of-line\n", (unsigned long long)st.lifted, (unsigned long long)st.fallback);
    printf("r0=%x\n", c[0].r[0]);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) return 2;
    return bad;
}
