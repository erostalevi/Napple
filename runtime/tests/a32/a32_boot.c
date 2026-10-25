/* Boot harness for AArch32 titles: maps the modules of a data directory into the guest window, sets up a stack and the thread registers and runs
 * nn::init::Start on the interpreter with a minimal svc layer that logs every distinct svc. Dev tool for finding what the real runtime integration needs:
 *   cc -std=gnu11 -O2 -Isrc/a32 -o a32_boot tests/a32/a32_boot.c src/a32/*.c , then   a32_boot <data dir> [max million instructions] */
#include "../../src/a32/a32.h"
#include "../../src/a32/a32_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static A32Window W;
static uint32_t g_heap = 0x40000000u, g_heap_size;
static uint32_t stub_exit, stub_cb[2]; static unsigned long long g_budget_left;
static int seen[256];

static int map_rw(uint32_t a, uint32_t n) { return mprotect(W.host + a, n, PROT_READ | PROT_WRITE); }

static const char *svc_name(unsigned n) {
    static const char *names[] = { [1]="SetHeapSize", [2]="SetMemoryPermission", [3]="SetMemoryAttribute", [4]="MapMemory", [5]="UnmapMemory", [6]="QueryMemory", [7]="ExitProcess",
        [8]="CreateThread", [9]="StartThread", [0xA]="ExitThread", [0xB]="SleepThread", [0xC]="GetThreadPriority", [0xD]="SetThreadPriority", [0xE]="GetThreadCoreMask", [0xF]="SetThreadCoreMask",
        [0x10]="GetCurrentProcessorNumber", [0x11]="SignalEvent", [0x12]="ClearEvent", [0x13]="MapSharedMemory", [0x14]="UnmapSharedMemory", [0x15]="CreateTransferMemory", [0x16]="CloseHandle",
        [0x17]="ResetSignal", [0x18]="WaitSynchronization", [0x19]="CancelSynchronization", [0x1A]="ArbitrateLock", [0x1B]="ArbitrateUnlock", [0x1C]="WaitProcessWideKeyAtomic",
        [0x1D]="SignalProcessWideKey", [0x1E]="GetSystemTick", [0x1F]="ConnectToNamedPort", [0x21]="SendSyncRequest", [0x22]="SendSyncRequestWithUserBuffer", [0x24]="GetProcessId",
        [0x25]="GetThreadId", [0x26]="Break", [0x27]="OutputDebugString", [0x29]="GetInfo", [0x34]="WaitForAddress", [0x35]="SignalToAddress", [0x36]="SynchronizePreemptionState" };
    return n < sizeof names / sizeof *names && names[n] ? names[n] : "?";
}

static int svc(A32Cpu *c, uint32_t imm) {
    uint32_t *r = c->r;
    if (imm < 256 && !seen[imm]++) fprintf(stderr, "[svc] first %#x %s  r0..r5 = %#x %#x %#x %#x %#x %#x  pc %#x\n", imm, svc_name(imm), r[0], r[1], r[2], r[3], r[4], r[5], c->r[15]);
    switch (imm) {
    case 0x01:   /* SetHeapSize(size) -> r0 result, r1 address */
        if (map_rw(g_heap, r[1] > g_heap_size ? r[1] : g_heap_size)) { r[0] = 0xD001; return 0; }
        g_heap_size = r[1]; r[0] = 0; r[1] = g_heap; return 0;
    case 0x06: memset(W.host + r[0], 0, 0x20); { uint32_t *mi = (uint32_t *)(W.host + r[0]); mi[0] = r[2] & ~0xFFFu; mi[1] = 0x1000; mi[2] = 3; mi[3] = 3; } r[0] = 0; r[1] = 0; return 0;   /* QueryMemory: not faithful */
    case 0x1E: { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); uint64_t v = (uint64_t)t.tv_sec * 19200000ull + (uint64_t)t.tv_nsec * 19200000ull / 1000000000ull; r[0] = (uint32_t)v; r[1] = (uint32_t)(v >> 32); return 0; }
    case 0x29: r[0] = 0xE401; return 0;     /* GetInfo: invalid enum value for now */
    case 0x26: fprintf(stderr, "[svc] Break r0 %#x r1 %#x r2 %#x lr %#x\n", r[0], r[1], r[2], r[14]); return 1;
    case 0x27: fprintf(stderr, "[svc] OutputDebugString: %.*s\n", (int)r[1], (char *)(W.host + r[0])); r[0] = 0; return 0;
    case 0x07: fprintf(stderr, "[svc] ExitProcess\n"); return 1;
    default: r[0] = 0; return 0;
    }
}
static int hle(A32Cpu *c, uint32_t pc) {
    if (pc == stub_exit) return 1;
    const char *n = a32_hle_name(&W, pc);
    fprintf(stderr, "[hle] %s called (r0 %#x r1 %#x) lr %#x\n", n ? n : "?", c->r[0], c->r[1], c->r[14]);
    return pc == stub_cb[0] || pc == stub_cb[1] ? 0 : 1;
}
static uint64_t counter(A32Cpu *c, int which) { (void)c; if (which) return 19200000; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 19200000ull + (uint64_t)t.tv_nsec * 19200000ull / 1000000000ull; }


static A32Cpu *g_cpu;
/* Calls a guest function (ARM/Thumb by bit 0) on the cpu and runs until it returns to the exit stub. Returns the exit reason (A32_EXIT_STOP on a normal return). */
static A32Exit call_guest(uint32_t fn, const char *what, uint32_t a0, uint32_t a1, uint32_t *ret) {
    A32Cpu *c = g_cpu;
    uint32_t saved_sp = c->r[13];
    c->r[0] = a0; c->r[1] = a1; c->r[14] = stub_exit; c->r[15] = fn & ~1u; c->thumb = fn & 1; c->itstate = 0;
    fprintf(stderr, "[boot] calling %s at %#x\n", what, fn);
    A32Exit e = A32_EXIT_BUDGET;
    while (g_budget_left) {
        unsigned long long n = g_budget_left < 1000000ull ? g_budget_left : 1000000ull;
        e = a32_run(c, n); g_budget_left -= n;
        if (e != A32_EXIT_BUDGET) break;
    }
    if (ret) *ret = c->r[0];
    c->r[13] = saved_sp;
    return e;
}
static int fail_report(A32Exit e) {
    A32Cpu *c = g_cpu;
    uint32_t pc = c->fault_pc ? c->fault_pc : c->r[15];
    fprintf(stderr, "[boot] STOP: %s after %llu instructions; pc %#x (thumb %u) word %#x fault_addr %#x\n", a32_exit_name(e), (unsigned long long)c->insns, pc, c->thumb, c->fault_word, c->fault_addr);
    fprintf(stderr, "[boot] words at pc: %08x %08x\n", *(uint32_t *)(W.host + (pc & ~1u)), *(uint32_t *)(W.host + (pc & ~1u) + 4));
    for (int i = 0; i < 16; i++) fprintf(stderr, "r%d=%#x ", i, c->r[i]);
    fprintf(stderr, "\n");
    for (unsigned m = 0; m < W.nmods; m++) if (pc >= W.mods[m].base && pc < W.mods[m].base + W.mods[m].size) fprintf(stderr, "[boot] pc is %s+%#x\n", W.mods[m].name, pc - W.mods[m].base);
    return 1;
}
static void run_inits(void) {
    for (unsigned m = 0; m < W.nmods; m++) {
        const A32Module *mo = &W.mods[m];
        if (mo->dt_init) { uint32_t r; fprintf(stderr, "[boot] %s DT_INIT\n", mo->name); A32Exit e = call_guest(mo->base + mo->dt_init, "DT_INIT", 0, 0, &r); if (e != A32_EXIT_STOP) { fail_report(e); exit(1); } }
        for (uint32_t k = 0; k < mo->init_array_sz / 4; k++) {
            uint32_t fn = *(uint32_t *)(W.host + mo->base + mo->init_array + 4 * k);
            if (fn && fn != ~0u) { A32Exit e = call_guest(fn, "init_array entry", 0, 0, NULL); if (e != A32_EXIT_STOP) { fprintf(stderr, "[boot] %s init_array[%u]\n", mo->name, k); fail_report(e); exit(1); } }
        }
        fprintf(stderr, "[boot] %s initializers done (%u entries)\n", mo->name, mo->init_array_sz / 4);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <data dir> [max million instructions]\n", argv[0]); return 2; }
    g_budget_left = (argc > 2 ? strtoull(argv[2], NULL, 10) : 200) * 1000000ull;
    char dir[1024], err[256];
    snprintf(dir, sizeof dir, "%s/modules", argv[1]);
    if (a32_window_init(&W)) { perror("window"); return 1; }
    static const char *order[] = { "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "sdk" };
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++) if (a32_module_map(&W, dir, order[i], err, sizeof err)) { fprintf(stderr, "map: %s\n", err); return 1; }
    A32RelStats st;
    if (a32_relocate(&W, &st, err, sizeof err)) { fprintf(stderr, "relocate: %s\n", err); return 1; }
    uint32_t stack = 0x30000000u, stack_size = 0x400000u, tlr = 0x2FFF0000u;
    map_rw(stack, stack_size); map_rw(tlr, 0x1000);
    W.nhle = 1; W.caphle = 4; W.hle_names = calloc(4, sizeof(char *)); W.hle_names[0] = strdup("<exit>");
    stub_exit = A32_HLE_BASE;
    static const A32Hooks hooks = { svc, hle, counter };
    A32Cpu *c = calloc(1, sizeof *c); g_cpu = c;
    a32_reset(c);
    c->mem = W.host; c->hooks = &hooks; c->hle_lo = A32_HLE_BASE; c->hle_hi = A32_HLE_BASE + 0x1000;
    c->tpidruro = tlr;
    c->r[13] = stack + stack_size - 16;
    /* rtld's startup sequence for the main thread (read from rtld's code; this SDK has no nn::init::Start) */
    struct { const char *name; uint32_t a0, a1; int optional; } seq[] = {
        { "__nnDetailInitLibc0", 0, 0, 0 },
        { "nnosInitialize", 0xD000, 0, 0 },
        { "__nnDetailInitLibc1", 0, 0, 0 },
        { "nndiagStartup", 0, 0, 0 },
        { "nninitInitializeAbortObserver", 0, 0, 1 },
        { "nninitInitializeSdkModule", 0, 0, 1 },
        { "nninitStartup", 0, 0, 0 },
        { "__nnDetailInitLibc2", 0, 0, 0 },
        { "<module initializers>", 0, 0, 0 },
        { "nnMain", 0, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
        if (!strcmp(seq[i].name, "<module initializers>")) { run_inits(); continue; }
        uint32_t fn = a32_find_symbol(&W, seq[i].name);
        if (!fn) { if (seq[i].optional) { fprintf(stderr, "[boot] %s: not present, skipped\n", seq[i].name); continue; } fprintf(stderr, "[boot] missing %s\n", seq[i].name); return 1; }
        uint32_t r;
        A32Exit e = call_guest(fn, seq[i].name, seq[i].a0, seq[i].a1, &r);
        if (e != A32_EXIT_STOP) return fail_report(e);
        fprintf(stderr, "[boot] %s returned %#x\n", seq[i].name, r);
    }
    fprintf(stderr, "[boot] nnMain returned\n");
    return 0;
}
