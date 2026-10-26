/* a32_proc.c - an AArch32 (32-bit ARM) Horizon process on the runtime's kernel.
 *
 * The guest's 32-bit address space is a 4 GiB window at host base B (a32_module.c / memory_init_a32). Everything the runtime already has for 64-bit
 * guests (handle table, threads and scheduling, futexes, sessions and IPC, services, gfx) keeps working on host addresses; this file converts at the
 * boundary:
 *   - svc: the A32 register layout (derived from the C prototype of each svc with the algorithm of Atmosphere's svc_codegen) is mapped to the A64
 *     register view the handlers take; pointer arguments get +B, address results -B, 32-bit structs are converted (QueryMemory).
 *   - threads: a guest thread is a pthread running the interpreter; svc handlers (including every wait) run on that pthread as before.
 *   - TLR: each thread's 0x200-byte thread local region lives in the window (tpidruro = guest address); the kernel's TLR key points at it, so the
 *     IPC code reads and writes the guest's message in place.
 *   - boot: the SDK of this era has no nn::init::Start; rtld's startup sequence (read from its code) is replayed on the main thread.
 * Imports no module defines (the graphics API handed out by nvnBootstrapLoader, ...) are host functions entered through the interpreter's hle hook. */
#include "napple.h"
#include "a32.h"
#include "a32_module.h"
#include "../a32jit/a32jit.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

extern void memory_init_a32(uint8_t *window);
extern void memory_a32_reserve(uint64_t host_lo, uint64_t size, unsigned state, unsigned perm);
extern uint8_t *memory_a32_base(void);
void threads_set_tlr_current(void *tlr);
int a32_hle_call(A32Cpu *c, const char *name, uint32_t pc);
int a32_is_host_override(const char *name);       /* a32_hle.c: runs the host function behind an import; non-zero = unknown */

A32Window g_a32w;
static uint8_t *B;

#define TLR_POOL 0x0E000000u        /* 4096 thread local regions of 0x200 bytes */
#define TLR_SLOTS 4096u
#define MAIN_STACK 0x20000000u      /* the gap below the heap region */

/* ---- svc register layouts ------------------------------------------------------------------------------------------- */
typedef struct { uint8_t num; const char *name; const char *sig; } SvcSig;
/* sig, one char per C parameter in order: I 32-bit value, P 32-bit pointer/address (host handlers get +B), L 64-bit value,
 * o 32-bit out value, O 64-bit out value, a out address (-B), R returns a 32-bit value instead of a result, S returns a 64-bit value. */
static const SvcSig svc_sigs[] = {
    {0x01, "SetHeapSize", "aI"}, {0x02, "SetMemoryPermission", "PII"}, {0x03, "SetMemoryAttribute", "PIII"}, {0x04, "MapMemory", "PPI"}, {0x05, "UnmapMemory", "PPI"},
    {0x06, "QueryMemory", "PoP"}, {0x07, "ExitProcess", ""}, {0x08, "CreateThread", "oIIIII"}, {0x09, "StartThread", "I"}, {0x0A, "ExitThread", ""},
    {0x0B, "SleepThread", "L"}, {0x0C, "GetThreadPriority", "oI"}, {0x0D, "SetThreadPriority", "II"}, {0x0E, "GetThreadCoreMask", "oOI"},
    {0x0F, "SetThreadCoreMask", "IIL"}, {0x10, "GetCurrentProcessorNumber", "R"}, {0x11, "SignalEvent", "I"}, {0x12, "ClearEvent", "I"},
    {0x13, "MapSharedMemory", "IPII"}, {0x14, "UnmapSharedMemory", "IPI"}, {0x15, "CreateTransferMemory", "oPII"}, {0x16, "CloseHandle", "I"},
    {0x17, "ResetSignal", "I"}, {0x18, "WaitSynchronization", "oPIL"}, {0x19, "CancelSynchronization", "I"}, {0x1A, "ArbitrateLock", "IPI"},
    {0x1B, "ArbitrateUnlock", "P"}, {0x1C, "WaitProcessWideKeyAtomic", "PPIL"}, {0x1D, "SignalProcessWideKey", "PI"}, {0x1E, "GetSystemTick", "S"},
    {0x1F, "ConnectToNamedPort", "oP"}, {0x20, "SendSyncRequestLight", "I"}, {0x21, "SendSyncRequest", "I"}, {0x22, "SendSyncRequestWithUserBuffer", "PII"},
    {0x23, "SendAsyncRequestWithUserBuffer", "oPII"}, {0x24, "GetProcessId", "OI"}, {0x25, "GetThreadId", "OI"}, {0x26, "Break", "III"},
    {0x27, "OutputDebugString", "PI"}, {0x28, "ReturnFromException", "I"}, {0x29, "GetInfo", "OIIL"}, {0x2A, "FlushEntireDataCache", ""},
    {0x2B, "FlushDataCache", "PI"}, {0x2C, "MapPhysicalMemory", "PI"}, {0x2D, "UnmapPhysicalMemory", "PI"}, {0x34, "WaitForAddress", "PILL"},
    {0x35, "SignalToAddress", "PIII"}, {0x5F, "FlushProcessDataCache", "IWL"}, {0x36, "SynchronizePreemptionState", ""}, {0x40, "CreateSession", "ooII"}, {0x41, "AcceptSession", "oI"},
    {0x42, "ReplyAndReceiveLight", "I"}, {0x43, "ReplyAndReceive", "oPIIL"}, {0x44, "ReplyAndReceiveWithUserBuffer", "oPPIIIL"}, {0x45, "CreateEvent", "oo"},
};

typedef struct {
    int valid; const char *name; char kind[12]; int n;
    int slot[12];      /* A64 argument register of an input */
    int reg32[12];     /* A32 register of an input (low word for 64-bit) */
    int out32[12];     /* A32 output register of an out value */
    int outs;          /* number of out values */
    char ret;          /* 0 result code, 'R' 32-bit value, 'S' 64-bit value */
} Layout;
static Layout layouts[256];
static uint32_t main_stack_lo, main_stack_hi;

static void build_layouts(void) {
    for (unsigned k = 0; k < sizeof svc_sigs / sizeof *svc_sigs; k++) {
        const SvcSig *sg = &svc_sigs[k];
        Layout *L = &layouts[sg->num];
        L->valid = 1; L->name = sg->name;
        if (sg->sig[0] == 'R' || sg->sig[0] == 'S') { L->ret = sg->sig[0]; continue; }
        L->n = (int)strlen(sg->sig);
        memcpy(L->kind, sg->sig, (size_t)L->n);
        unsigned used = 0; int ncrn = 0, stack_in[12], nstack = 0;
        int in_reg[12]; memset(in_reg, -1, sizeof in_reg);
        for (int i = 0; i < L->n; i++) {         /* stage 1: AAPCS over the C prototype (out values are pointer parameters) */
            char c = L->kind[i]; int is_out = c == 'o' || c == 'O' || c == 'a'; int w64 = c == 'L' || c == 'W';
            L->slot[i] = i; L->reg32[i] = -1;
            if (is_out) L->outs++;
            if (w64) { if (ncrn & 1) ncrn++; }
            if (ncrn + (w64 ? 2 : 1) <= 4) { in_reg[i] = ncrn; ncrn += w64 ? 2 : 1; if (!is_out) { used |= 1u << in_reg[i]; if (w64) used |= 1u << (in_reg[i] + 1); } }
            else { ncrn = 4; if (!is_out) stack_in[nstack++] = i; }
        }
        for (int i = 0; i < L->n; i++) { char c = L->kind[i]; if (c != 'o' && c != 'O' && c != 'a' && in_reg[i] >= 0) L->reg32[i] = in_reg[i]; }
        for (int s = 0; s < nstack; s++) {       /* stage 2: stack inputs take the lowest free registers */
            int i = stack_in[s], need = (L->kind[i] == 'L' || L->kind[i] == 'W') ? 2 : 1;
            int r = 0;
            if (need == 1) { while (used & (1u << r)) r++; used |= 1u << r; L->reg32[i] = r; }
            else { while (used & (1u << r)) r++; int r2 = r + 1; while (used & (1u << r2)) r2++; used |= 1u << r; used |= 1u << r2; L->reg32[i] = r; L->reg32[i] = r | (r2 << 8); }
        }
        int next = 1;
        for (int i = 0; i < L->n; i++) { char c = L->kind[i]; if (c == 'o' || c == 'a') L->out32[i] = next++; else if (c == 'O') { L->out32[i] = next; next += 2; } }
    }
}

static inline uint64_t get_in(const A32Cpu *c, const Layout *L, int i) {
    int rr = L->reg32[i];
    if (L->kind[i] == 'L' || L->kind[i] == 'W') { int lo = rr & 0xff, hi = rr >> 8; if (!hi) hi = lo + 1; return (uint64_t)c->r[lo] | (uint64_t)c->r[hi] << 32; }
    return c->r[rr & 0xff];
}

/* NAPPLE_A32_TRACE=1: run one instruction at a time and keep the last 4096 pcs; dumped (symbolized) when the guest calls Break. Slow: for finding why a title aborts. */
static int trace_on;
static __thread uint32_t trace_ring[4096]; static __thread unsigned trace_n;
static const char *symbolize(uint32_t pc, char *buf, size_t n) {
    for (unsigned m = 0; m < g_a32w.nmods; m++) {
        const A32Module *mo = &g_a32w.mods[m];
        if (pc < mo->base || pc >= mo->base + mo->size) continue;
        uint32_t off = pc - mo->base, best = 0; const char *bn = NULL;
        for (uint32_t i = 1; i < mo->nsym; i++) { const A32Sym *sy = &mo->syms[i]; if (sy->defined && sy->type == 2 && sy->value <= off && sy->value >= best && off - sy->value < 0x10000) { best = (uint32_t)sy->value; bn = mo->strtab + sy->name; } }
        if (bn) snprintf(buf, n, "%s+%#x (%s+%#x)", bn, off - best, mo->name, off); else snprintf(buf, n, "%s+%#x", mo->name, off);
        return buf;
    }
    snprintf(buf, n, "%#x", pc); return buf;
}
static void trace_dump(void) {
    if (!trace_on) return;
    unsigned n = trace_n < 4096 ? trace_n : 4096; char b[300];
    unsigned from = n > 160 ? n - 160 : 0;
    LOG("a32", "last %u of %u instructions before Break (pc, nearest export):", n - from, n);
    for (unsigned i = from; i < n; i++) { uint32_t pc = trace_ring[(trace_n - n + i) & 4095]; symbolize(pc, b, sizeof b); LOG("a32", "  %#x %s", pc, b); }
}
/* Heuristic backtrace: words on the guest stack that point into module code just after a call instruction (bl / blx reg). */
static void stack_backtrace(const A32Cpu *c) {
    char sb[300]; int shown = 0;
    for (uint32_t a = c->r[13]; a < c->r[13] + 0x2000 && shown < 28; a += 4) {
        uint32_t v; memcpy(&v, B + a, 4);
        int in_text = 0; for (unsigned m = 0; m < g_a32w.nmods; m++) if (v >= g_a32w.mods[m].base + 4 && v < g_a32w.mods[m].base + g_a32w.mods[m].text_end) in_text = 1;
        if (!in_text) continue;
        uint32_t prev; memcpy(&prev, B + (v & ~1u) - 4, 4);
        if ((prev & 0x0f000000) != 0x0b000000 && (prev & 0xfffffff0) != 0xe12fff30) continue;
        fprintf(stderr, "  [sp+%#x] %s\n", a - c->r[13], symbolize(v & ~1u, sb, sizeof sb)); shown++;
    }
}
static int a32_svc_impl(A32Cpu *c, uint32_t imm);
static int svc_trace = -1;
static int a32_svc(A32Cpu *c, uint32_t imm) {
    if (svc_trace < 0) svc_trace = getenv("NAPPLE_A32_SVCTRACE") != NULL;
    if (!svc_trace) return a32_svc_impl(c, imm);
    uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3], lr = c->r[14];
    int rc = a32_svc_impl(c, imm);
    fprintf(stderr, "[svc] tlr %#x svc %#04x(%s) r0-r3 %#x %#x %#x %#x lr %#x -> r0 %#x r1 %#x\n", c->tpidruro, imm, imm < 256 && layouts[imm].valid ? "" : "?", a0, a1, a2, a3, lr, c->r[0], c->r[1]);
    return rc;
}
static int a32_svc_impl(A32Cpu *c, uint32_t imm) {
    if (imm == 0x26) {
        trace_dump();
        char sb[300]; LOG("a32", "guest Break: r0 %#x r1 %#x r2 %#x lr %s sp %#x", c->r[0], c->r[1], c->r[2], symbolize(c->r[14] & ~1u, sb, sizeof sb), c->r[13]);
        stack_backtrace(c);
    }
    if (imm > 255 || !layouts[imm].valid) runtime_fail("AArch32 svc %#x has no register layout yet (pc %#x r0-r3 %#x %#x %#x %#x)", imm, c->r[15], c->r[0], c->r[1], c->r[2], c->r[3]);
    const Layout *L = &layouts[imm];
    uint64_t r[8] = {0};
    uint32_t pc = c->r[15];
    if (imm == 0x06) {            /* QueryMemory(MemoryInfo *, PageInfo *, address): the 32-bit SDK uses the 40-byte MemoryInfo with 64-bit base and size too; only the address needs converting */
        uint32_t info_guest = c->r[0], addr = c->r[2];
        if (info_guest < 0x10000 || info_guest > 0xFFFFFFF0u - 40) { c->r[0] = 0xe601; return 0; }
        uint8_t *info = B + info_guest;
        r[0] = (uintptr_t)info; r[2] = (uint64_t)addr + (uintptr_t)B;
        svc_dispatch(imm, r, "a32", pc);
        c->r[0] = (uint32_t)r[0]; c->r[1] = (uint32_t)r[1];
        if ((uint32_t)r[0] == 0) {
            uint64_t base, size; memcpy(&base, info, 8); memcpy(&size, info + 8, 8);
            base = base >= (uintptr_t)B ? base - (uintptr_t)B : 0;
            if (addr >= main_stack_lo && addr < main_stack_hi) {   /* the main thread's stack */
                base = main_stack_lo; size = main_stack_hi - main_stack_lo;
                uint32_t st = 0xb, perm = 3; memcpy(info + 16, &st, 4); memcpy(info + 24, &perm, 4);
            }
            memcpy(info, &base, 8); memcpy(info + 8, &size, 8);
        }
        return 0;
    }
    for (int i = 0; i < L->n; i++) {
        char k = L->kind[i];
        if (k == 'o' || k == 'O' || k == 'a') continue;
        uint64_t v = get_in(c, L, i);
        if ((k == 'P' || k == 'W') && v) v += (uintptr_t)B;
        r[L->slot[i]] = v;
    }
    svc_dispatch(imm, r, "a32", pc);
    if (L->ret == 'R') { c->r[0] = (uint32_t)r[0]; return 0; }
    if (L->ret == 'S') { c->r[0] = (uint32_t)r[0]; c->r[1] = (uint32_t)(r[0] >> 32); return 0; }
    c->r[0] = (uint32_t)r[0];
    int oi = 0;
    for (int i = 0; i < L->n; i++) {
        char k = L->kind[i];
        if (k != 'o' && k != 'O' && k != 'a') continue;
        uint64_t v = r[1 + oi++];
        if (k == 'a' && v) v -= (uintptr_t)B;
        if (k == 'O') {
            if (imm == 0x29) {             /* GetInfo: addresses are guest addresses here; the ASLR region is the low gap below the stack region */
                uint32_t id = c->r[1];
                if (id == 2 || id == 4 || id == 14 || id == 20) v -= (uintptr_t)B;
                else if (id == 12) v = 0x200000;
                else if (id == 13) v = 0x0FE00000;
            }
            c->r[L->out32[i]] = (uint32_t)v; c->r[L->out32[i] + 1] = (uint32_t)(v >> 32);
        } else c->r[L->out32[i]] = (uint32_t)v;
    }
    return 0;
}

/* ---- thread local regions ------------------------------------------------------------------------------------------- */
static _Atomic uint32_t tlr_next;
static uint32_t tlr_alloc(void) {
    uint32_t i = atomic_fetch_add(&tlr_next, 1);
    if (i >= TLR_SLOTS) runtime_fail("out of AArch32 thread local regions");
    return TLR_POOL + i * 0x200u;
}
static uint64_t hook_counter(A32Cpu *c, int which) {
    (void)c;
    if (which) return 19200000;
    return runtime_guest_ticks();
}
static uint32_t stub_return;
static int hook_hle(A32Cpu *c, uint32_t pc) {
    if (pc == stub_return) return 1;                              /* a call from the host returned */
    const char *name = a32_hle_name(&g_a32w, pc);
    if (!name) runtime_fail("AArch32 guest jumped to %#x (no import stub there; lr %#x)", pc, c->r[14]);
    if (a32_hle_call(c, name, pc)) runtime_fail("AArch32 import %s is not implemented (r0 %#x r1 %#x lr %#x)", name, c->r[0], c->r[1], c->r[14]);
    return 0;
}
static const A32Hooks hooks = { a32_svc, hook_hle, hook_counter };

static __thread A32Cpu *cur_cpu;
void a32_crash_report(void) {
    A32Cpu *c = cur_cpu; if (!c) return;
    char b[300]; symbolize(c->r[15], b, sizeof b);
    fprintf(stderr, "[napple] AArch32 guest at the fault: pc %#x %s (thumb %u) lr %#x sp %#x\n", c->r[15], b, c->thumb, c->r[14], c->r[13]);
    for (int i = 0; i < 13; i++) fprintf(stderr, "r%d=%#x ", i, c->r[i]);
    fprintf(stderr, "\n[napple] words at pc: %08x %08x\n", *(uint32_t *)(B + (c->r[15] & ~1u)), *(uint32_t *)(B + (c->r[15] & ~1u) + 4));
    if (c->call_ring) { unsigned n = c->call_n < 1024 ? c->call_n : 1024; fprintf(stderr, "[napple] last %u of %u calls (oldest first):\n", n > 60 ? 60 : n, c->call_n); for (unsigned i = n > 60 ? n - 60 : 0; i < n; i++) { uint32_t t = c->call_ring[(c->call_n - n + i) & 1023]; fprintf(stderr, "  call %s\n", symbolize(t & ~1u, b, sizeof b)); } }
    stack_backtrace(c);
    unsigned n = trace_n < 4096 ? trace_n : 4096; if (trace_on) for (unsigned i = n > 40 ? n - 40 : 0; i < n; i++) { uint32_t pc = trace_ring[(trace_n - n + i) & 4095]; symbolize(pc, b, sizeof b); fprintf(stderr, "  trace %#x %s\n", pc, b); }
}
static A32Cpu *cpu_new(uint32_t tlr) {
    A32Cpu *c = calloc(1, sizeof *c);
    if (getenv("NAPPLE_A32_CALLS")) c->call_ring = calloc(1024, 4);
    if (!c) runtime_fail("out of memory");
    a32_reset(c);
    cur_cpu = c; c->mem = B; c->hooks = &hooks; c->hle_lo = A32_HLE_BASE; c->hle_hi = A32_HLE_BASE + 0x100000; c->tpidruro = tlr;
    return c;
}
static A32Exit run_slice(A32Cpu *c) {
    if (!trace_on) return a32jit_active() ? a32jit_run(c, 1000000) : a32_run(c, 1000000);
    static uint32_t abort_fn;
    if (!abort_fn) abort_fn = a32_find_symbol(&g_a32w, "_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_St9__va_list") ?: 1;
    for (unsigned i = 0; i < 1000000; i++) {
        trace_ring[trace_n++ & 4095] = c->r[15];
        if (c->r[15] == (abort_fn & ~1u)) {   /* VAbortImpl(expr, func, file, line, result*, ...): what the game asserted */
            const char *a[3]; for (int k = 0; k < 3; k++) a[k] = c->r[k] > 0x10000 ? (const char *)(B + c->r[k]) : "(null)";
            { uint32_t st[4]; memcpy(st, B + c->r[13], 16); uint32_t res = st[0] > 0x10000 ? *(uint32_t *)(B + st[0]) : 0xffffffffu; const char *fmt = st[2] > 0x10000 ? (const char *)(B + st[2]) : "(null)";
              LOG("a32", "GUEST ABORT args: result %#x (module %u desc %u) fmt \"%.200s\" va %#x", res, res & 0x1ff, res >> 9, fmt, st[3]); }
            { uint32_t fp = c->r[11], lr = c->r[14]; char sb[300]; LOG("a32", "GUEST ABORT call chain (frame pointers): lr %s", symbolize(lr & ~1u, sb, sizeof sb));
              for (int d = 0; d < 14 && fp > 0x10000 && fp < 0xfffff000u && !(fp & 3); d++) { uint32_t nfp, nlr; memcpy(&nfp, B + fp, 4); memcpy(&nlr, B + fp + 4, 4); LOG("a32", "    from %s", symbolize(nlr & ~1u, sb, sizeof sb)); if (nfp <= fp) break; fp = nfp; } }
            { uint32_t th; memcpy(&th, B + c->tpidruro + 0x1fc, 4); uint32_t f[20] = {0}; if (th > 0x10000 && th < 0xffffff00u) memcpy(f, B + th, 80);
              LOG("a32", "GUEST ABORT thread object %#x (tls %#x sp %#x): +0x20 %#x +0x24 %#x +0x28 %#x +0x2c %#x +0x30 %#x +0x34 %#x +0x38 %#x +0x3c %#x", th, c->tpidruro, c->r[13], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]); }
            LOG("a32", "GUEST ABORT: condition \"%.200s\" in %.120s at %.160s:%u (lr %#x)", a[0], a[1], a[2], c->r[3], c->r[14]);
        }
        A32Exit e = a32_run(c, 1); if (e != A32_EXIT_BUDGET) return e;
    }
    return A32_EXIT_BUDGET;
}
static void run_forever(A32Cpu *c, const char *what) {
    unsigned slices = 0;
    for (;;) {
        A32Exit e = run_slice(c);
        if (e == A32_EXIT_BUDGET) {
            if (getenv("NAPPLE_A32_WATCH") && ++slices % 256 == 0) {   /* a thread that keeps running for 256M instructions: where is it? */
                char b[300]; symbolize(c->r[15], b, sizeof b);
                LOG("a32", "thread (tlr %#x) busy after %u M instructions: pc %s lr %#x sp %#x r0-r3 %#x %#x %#x %#x", c->tpidruro, slices, b, c->r[14], c->r[13], c->r[0], c->r[1], c->r[2], c->r[3]);
            }
            continue;
        }
        runtime_fail("AArch32 %s stopped: %s at pc %#x (thumb %u) word %#x, lr %#x sp %#x", what, a32_exit_name(e), c->fault_pc ? c->fault_pc : c->r[15], c->thumb, c->fault_word, c->r[14], c->r[13]);
    }
}

/* Entry of a guest thread created by CreateThread (called by threads.c instead of runtime_thread_enter). Never returns: ExitThread ends the pthread. */
void a32_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    uint32_t tlr = tlr_alloc();
    pthread_setspecific(runtime_keys[2], B + tlr);
    threads_set_tlr_current(B + tlr);
    A32Cpu *c = cpu_new(tlr);
    c->r[0] = (uint32_t)arg; c->r[13] = (uint32_t)sp; c->r[15] = (uint32_t)entry & ~1u; c->thumb = (uint32_t)entry & 1;
    run_forever(c, "thread");
}

/* ---- process: modules and boot ---------------------------------------------------------------------------------------- */
static const char *const module_order[] = { "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk" };

static void svc_flush_process_data_cache(uint64_t *r) { r[0] = 0; }   /* guest data cache maintenance: nothing to do for the interpreter */
void a32_process_init(const char *data_dir) {
    build_layouts();
    svc_register(0x5F, "FlushProcessDataCache", svc_flush_process_data_cache);
    trace_on = getenv("NAPPLE_A32_TRACE") != NULL;
    if (a32_window_init(&g_a32w)) runtime_fail("cannot reserve the 4 GiB AArch32 guest window: %s", strerror(errno));
    B = g_a32w.host;
    g_a32w.is_override = a32_is_host_override;
    memory_init_a32(B);
    if (mprotect(B + TLR_POOL, TLR_SLOTS * 0x200u, PROT_READ | PROT_WRITE)) runtime_fail("cannot map the thread local regions: %s", strerror(errno));
    memory_a32_reserve((uint64_t)(uintptr_t)B + TLR_POOL, TLR_SLOTS * 0x200u, 0xc, 3);   /* ThreadLocal, read + write */
    char dir[4096], err[256];
    snprintf(dir, sizeof dir, "%s/modules", data_dir);
    for (unsigned i = 0; i < sizeof module_order / sizeof *module_order; i++) {
        char p[4200]; snprintf(p, sizeof p, "%s/%s.tables", dir, module_order[i]);
        if (access(p, R_OK)) continue;
        if (a32_module_map(&g_a32w, dir, module_order[i], err, sizeof err)) runtime_fail("AArch32 module %s: %s", module_order[i], err);
        const A32Module *m = &g_a32w.mods[g_a32w.nmods - 1];
        memory_a32_reserve((uint64_t)(uintptr_t)B + m->base, (m->size + 0xFFFu) & ~0xFFFu, 3, 5);   /* Code, read + execute */
        LOG("a32", "mapped %s at %#x (%#x bytes, %u relocations)", m->name, m->base, m->size, m->nrel);
    }
    if (!g_a32w.nmods) runtime_fail("no AArch32 modules in %s", dir);
    stub_return = a32_hle_stub(&g_a32w, "<return to host>");
    A32RelStats st;
    if (a32_relocate(&g_a32w, &st, err, sizeof err)) runtime_fail("AArch32 relocation: %s", err);
    a32jit_init(dir, &g_a32w);
    LOG("a32", "relocations applied: %llu (%llu imports resolved across modules, %u host imports)", (unsigned long long)g_a32w.applied, (unsigned long long)st.to_module, g_a32w.nhle);
}

/* Calls a guest function on the cpu and returns its r0. */
static uint32_t call_guest(A32Cpu *c, uint32_t fn, const char *what, uint32_t a0, uint32_t a1, uint32_t ret_stub) {
    uint32_t sp = c->r[13];
    LOG("a32", "calling %s at %#x", what, fn);
    c->r[0] = a0; c->r[1] = a1; c->r[14] = ret_stub; c->r[15] = fn & ~1u; c->thumb = fn & 1; c->itstate = 0;
    unsigned slices = 0;
    for (;;) {
        A32Exit e = run_slice(c);
        if (e == A32_EXIT_BUDGET) {
            if (++slices > 100 && (slices & 63) == 0) {   /* a call that runs this long is stuck or waiting on a host-side effect: say where (every 64M instructions) */
                const A32Module *mo = NULL; for (unsigned m = 0; m < g_a32w.nmods; m++) if (c->r[15] >= g_a32w.mods[m].base && c->r[15] < g_a32w.mods[m].base + g_a32w.mods[m].size) mo = &g_a32w.mods[m];
                if (getenv("NAPPLE_A32_DUMP") && slices == 128) for (int k = 0; k < 4; k++) { uint32_t a = c->r[k]; if (a > 0x10000 && a < 0xfffff000u) { uint32_t w[6]; memcpy(w, B + a, 24); LOG("a32", "  r%d=%#x -> %08x %08x %08x %08x %08x %08x", k, a, w[0], w[1], w[2], w[3], w[4], w[5]); } }
                LOG("a32", "%s still running after %u M instructions: pc %s+%#x (thumb %u) lr %#x sp %#x r0-r3 %#x %#x %#x %#x", what, slices, mo ? mo->name : "?", mo ? c->r[15] - mo->base : c->r[15], c->thumb, c->r[14], c->r[13], c->r[0], c->r[1], c->r[2], c->r[3]);
            }
            continue;
        }
        if (e == A32_EXIT_STOP) break;
        runtime_fail("AArch32 %s stopped: %s at pc %#x (thumb %u) word %#x, lr %#x sp %#x", what, a32_exit_name(e), c->fault_pc ? c->fault_pc : c->r[15], c->thumb, c->fault_word, c->r[14], c->r[13]);
    }
    c->r[13] = sp;
    return c->r[0];
}


_Noreturn void a32_boot(uint32_t main_handle) {
    uint32_t tlr = tlr_alloc();
    pthread_setspecific(runtime_keys[2], B + tlr);
    threads_set_tlr_current(B + tlr);
    uint32_t stack_size = g_npdm.valid && g_npdm.main_stack_size ? g_npdm.main_stack_size : 0x100000;
    stack_size = (stack_size + 0xFFF) & ~0xFFFu;
    if (mprotect(B + MAIN_STACK, stack_size, PROT_READ | PROT_WRITE)) runtime_fail("cannot map the main thread stack: %s", strerror(errno));
    main_stack_lo = MAIN_STACK; main_stack_hi = MAIN_STACK + stack_size;
    memory_a32_reserve((uint64_t)(uintptr_t)B + MAIN_STACK, stack_size, 0xb, 3);   /* Stack: else QueryMemory reports it inside a Free gap and the SDK maps shared memory (pl:u fonts) over it */
    A32Cpu *c = cpu_new(tlr);
    c->r[13] = MAIN_STACK + stack_size - 16;
    LOG("a32", "main thread: handle %#x stack %#x+%#x tlr %#x", main_handle, MAIN_STACK, stack_size, tlr);
    /* rtld's startup sequence for the main thread (from its code and PLT): InitLibc0, nnosInitialize(handle), InitLibc1, nndiagStartup, [abort observer],
     * [SDK module init], nninitStartup, InitLibc2, module constructors, nnMain, [SDK module finalize], nnosQuickExit */
    struct { const char *name; uint32_t a0; int optional; } seq[] = {
        { "__nnDetailInitLibc0", 0, 0 }, { "nnosInitialize", main_handle, 0 }, { "__nnDetailInitLibc1", 0, 0 }, { "nndiagStartup", 0, 0 },
        { "nninitInitializeAbortObserver", 0, 1 }, { "nninitInitializeSdkModule", 0, 1 }, { "nninitStartup", 0, 0 }, { "__nnDetailInitLibc2", 0, 0 },
        { "<module initializers>", 0, 0 }, { "nnMain", 0, 0 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
        if (!strcmp(seq[i].name, "<module initializers>")) {
            for (unsigned m = 0; m < g_a32w.nmods; m++) {
                const A32Module *mo = &g_a32w.mods[m];
                if (mo->dt_init) { call_guest(c, mo->base + mo->dt_init, "DT_INIT", 0, 0, stub_return); LOG("a32", "%s initializers done (DT_INIT)", mo->name); continue; }   /* DT_INIT runs the init array itself */
                for (uint32_t k = 0; k < mo->init_array_sz / 4; k++) {
                    uint32_t fn; memcpy(&fn, B + mo->base + mo->init_array + 4 * k, 4);
                    if (fn && fn != ~0u) call_guest(c, fn, "constructor", 0, 0, stub_return);
                }
                LOG("a32", "%s initializers done (%u)", mo->name, mo->init_array_sz / 4);
            }
            continue;
        }
        uint32_t fn = a32_find_symbol(&g_a32w, seq[i].name);
        if (!fn) { if (seq[i].optional) continue; runtime_fail("AArch32 boot: %s not found", seq[i].name); }
        uint32_t r = call_guest(c, fn, seq[i].name, seq[i].a0, 0, stub_return);
        LOG("a32", "%s returned %#x", seq[i].name, r);
    }
    runtime_fail("nnMain returned");
}

/* The NVN API version the title was built against: it compares nvnDeviceGetInteger(API_MAJOR/MINOR) with build-time constants and skips graphics start-up on a
 * mismatch (as for 64-bit titles, see main.c). ARM form: `cmp r0,#major; bne fail; ldr r0,[sp,#x]; cmp r0,#minor; blt fail`. Returns 1 and sets major/minor when found. */
int a32_detect_nvn_version(unsigned *major, unsigned *minor) {
    const A32Module *m = NULL;
    for (unsigned i = 0; i < g_a32w.nmods; i++) if (!strcmp(g_a32w.mods[i].name, "main")) m = &g_a32w.mods[i];
    if (!m) return 0;
    const uint32_t *w = (const uint32_t *)(B + m->base); size_t n = m->text_end / 4;
    for (size_t i = 0; i + 5 < n; i++) {
        if ((w[i] & 0xfffff000) != 0xe3500000 || (w[i + 1] & 0xff000000) != 0x1a000000) continue;      /* cmp r0,#imm; bne */
        unsigned maj = w[i] & 0xff;
        if (maj < 40 || maj > 70) continue;
        for (int k = 2; k <= 3; k++)
            if ((w[i + k] & 0xfffff000) == 0xe59d0000 && (w[i + k + 1] & 0xfffff000) == 0xe3500000 && (w[i + k + 2] & 0xff000000) == 0xba000000) {   /* ldr r0,[sp,#x]; cmp r0,#minor; blt */
                unsigned mnr = w[i + k + 1] & 0xff;
                if (mnr < 5) continue;
                *major = maj; *minor = mnr;
                return 1;
            }
    }
    return 0;
}
