/* Micro-benchmark: a small ARM integer loop and a Thumb loop with a conditional block, run by the interpreter and by lifted code.
 *   cc -std=gnu11 -O2 -DNAPPLE_A32_INTERP -Isrc/a32 -o bench tests/a32jit/bench.c src/a32jit/a32lift.c src/a32/a32_{exec,decode_arm,decode_thumb,vfp,neon}.c && ./bench
 * Prints guest instructions per second for each. Not a game measurement: it shows the ceiling of the translated code on straight integer code. */
#include "../../src/a32jit/a32lift.h"
#include "../../src/a32/a32_priv.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CODE 0x1000000u
static int h_svc(A32Cpu *c, uint32_t imm) { (void)c; return imm == 0x7f; }
static int h_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; return 1; }
static uint64_t h_cnt(A32Cpu *c, int w) { (void)c; (void)w; return 0; }
static const A32Hooks hooks = { h_svc, h_hle, h_cnt };
static uint32_t jit_step(A32Cpu *c, const A32Lit *l) { return (uint32_t)a32_op_exec(c, &l->insn, c->r[15], l->raw); }
static uint32_t jit_hle(A32Cpu *c, uint32_t pc) { (void)c; (void)pc; return 1; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    uint8_t *win = mmap((void *)(8ull << 32), (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (win == MAP_FAILED) { perror("mmap"); return 1; }
    /* ARM: checksum loop, 7 instructions per iteration */
    static const uint32_t arm[] = {
        0xe4902004,   /* ldr r2, [r0], #4        */
        0xe0811002,   /* add r1, r1, r2          */
        0xe0211261,   /* eor r1, r1, r1, ror #4  */
        0xe2033003 ^ 0x0, /* and r3, r3, #3 (kept: a dependent op) */
        0xe2544001,   /* subs r4, r4, #1        */
        0xe3a05000,   /* mov r5, #0              */
        0x1afffff8,   /* bne loop                */
        0xef00007f,   /* svc #0x7f               */
    };
    memcpy(win + CODE, arm, sizeof arm);
    /* Thumb: the same shape, with an IT block in the loop body */
    static const uint16_t thumb[] = {
        0x6802,       /* ldr r2, [r0]            */
        0x1889,       /* adds r1, r1, r2         */
        0x4051,       /* eors r1, r2             */
        0x2a00,       /* cmp r2, #0              */
        0xbf08,       /* it eq                   */
        0x3101,       /* addeq r1, #1            */
        0x3c01,       /* subs r4, #1             */
        0xd1f7,       /* bne loop                */
        0xdf7f,       /* svc #0x7f               */
    };
    memcpy(win + CODE + 0x100, thumb, sizeof thumb);
    char dir[] = "/tmp/a32jit_bench_XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char pre[256]; snprintf(pre, sizeof pre, "%s/b", dir);
    uint32_t seeds[2] = { CODE, (CODE + 0x100) | 1 };
    LiftModule lm = { "bench", CODE, 0x1000, 0, 0 };
    LiftInput in = { .mem = win, .mods = &lm, .nmods = 1, .seeds = seeds, .nseeds = 2 };
    LiftStats st; char **files;
    int n = a32lift_run(&in, pre, &files, &st);
    char cmd[2048];
    for (int f = 0; f < n; f++) { snprintf(cmd, sizeof cmd, "clang -c -target arm64-apple-macos13 -march=armv8.4-a %s -o %s.o", files[f], files[f]); if (system(cmd)) return 1; }
    snprintf(cmd, sizeof cmd, "clang -dynamiclib -target arm64-apple-macos13 -o %s/b.dylib %s/b.*.S.o", dir, dir);
    if (system(cmd)) return 1;
    snprintf(cmd, sizeof cmd, "%s/b.dylib", dir);
    void *h = dlopen(cmd, RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    int (*enter)(A32Cpu *, int64_t) = dlsym(h, "a32jit_enter");
    void **hk = dlsym(h, "a32jit_hooks"); hk[0] = (void *)jit_step; hk[1] = (void *)jit_hle;
    const long iters = 20000000;
    for (int mode = 0; mode < 2; mode++) {
        uint32_t entry = mode ? (CODE + 0x100) : CODE; unsigned per = mode ? 8 : 7;
        for (int use_jit = 0; use_jit < 2; use_jit++) {
            A32Cpu c; memset(&c, 0, sizeof c);
            c.mem = win; c.hooks = &hooks; c.fpexc_en = 1 << 30;
            c.r[0] = 0x20000; c.r[4] = (uint32_t)iters; c.r[15] = entry & ~1u; c.thumb = (unsigned)mode;
            double t0 = now(); A32Exit e;
            if (use_jit) { int r; do r = enter(&c, 1ll << 40); while (r == 0 && 0); e = (A32Exit)(r == 1 ? 1 : 0); }
            else e = a32_run(&c, (uint64_t)iters * (per + 1) + 100);
            double dt = now() - t0;
            double mips = (double)iters * per / dt / 1e6;
            printf("%-6s %-11s %8.1f M guest instructions/s (%.2f s, exit %d, r1=%08x r4=%u)\n", mode ? "Thumb" : "ARM", use_jit ? "lifted" : "interpreter", mips, dt, (int)e, c.r[1], c.r[4]);
        }
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    return 0;
}
