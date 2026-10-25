/* Shared library for tests/a32/fuzz.py: runs instructions of the AArch32 core on a given state, so the Python harness can compare it
 * against Unicorn (QEMU). Guest addresses 0x8000..0x20000 are backed; any other access is reported as a host fault (100). */
#include "a32/a32.h"
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define WIN_LO 0x8000u
#define WIN_HI 0x20000u

static uint8_t window[WIN_HI - WIN_LO];
static sigjmp_buf jb;
static void on_segv(int s) { (void)s; siglongjmp(jb, 1); }

typedef struct {
    uint32_t r[16]; uint32_t cpsr; uint64_t d[32]; uint32_t fpscr;
    uint32_t fault_word; uint32_t tpidruro; uint32_t itstate;
} FState;

static int svc_hook(A32Cpu *c, uint32_t imm) { (void)c; (void)imm; return 1; }
static const A32Hooks hooks = {.svc = svc_hook};

/* mem_image: WIN_HI - WIN_LO bytes starting at guest 0x8000; the code is already in it. Returns the A32Exit, or 100 on a host fault. */
int ft_run(FState *s, uint8_t *mem_image, int thumb, unsigned steps) {
    static A32Cpu cpu;
    static int init;
    if (!init) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_segv;
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); init = 1;
    }
    memcpy(window, mem_image, sizeof window);
    a32_free(&cpu);
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = window - WIN_LO; cpu.hooks = &hooks;
    memcpy(cpu.r, s->r, sizeof cpu.r);
    cpu.n = s->cpsr >> 31 & 1; cpu.z = s->cpsr >> 30 & 1; cpu.c = s->cpsr >> 29 & 1; cpu.v = s->cpsr >> 28 & 1; cpu.q = s->cpsr >> 27 & 1;
    cpu.ge = s->cpsr >> 16 & 15; cpu.thumb = thumb; cpu.itstate = s->itstate;
    memcpy(cpu.v_.d, s->d, sizeof cpu.v_.d); cpu.fpscr = s->fpscr; cpu.tpidruro = s->tpidruro;
    int rc;
    if (sigsetjmp(jb, 1)) rc = 100;
    else rc = (int)a32_run(&cpu, steps);
    memcpy(s->r, cpu.r, sizeof s->r);
    s->cpsr = cpu.n << 31 | cpu.z << 30 | cpu.c << 29 | cpu.v << 28 | cpu.q << 27 | cpu.ge << 16 | cpu.thumb << 5;
    memcpy(s->d, cpu.v_.d, sizeof s->d); s->fpscr = cpu.fpscr; s->fault_word = cpu.fault_word; s->itstate = cpu.itstate;
    memcpy(mem_image, window, sizeof window);
    return rc;
}
