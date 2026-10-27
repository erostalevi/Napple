/* Fault handling for guest code running natively: emulates the two cache-maintenance forms Darwin traps but Horizon
 * allows at EL0, hands graphics write-faults to gfx, and otherwise reports the crash (guest module+offset, registers,
 * frame chain) into the log and the compatibility summary. */
#include "napple.h"
#include <execinfo.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <setjmp.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* A host read of guest memory that may fault sets this; the handler then longjmps instead of reporting a crash. */
__thread sigjmp_buf *runtime_fault_guard;

/* CTR_EL0 for 128-byte host cache lines: DminLine = IminLine = 5 (32 words), ERG = CWG = 5, PIPT I-cache, IDC/DIC clear. */
#define HOST_CTR_EL0 0x8555c005ULL

static void out(const char *s) {
    size_t n = strlen(s);
    if (write(2, s, n) < 0) {}
}
static void out_hex(const char *label, uint64_t v) {
    char b[96];
    snprintf(b, sizeof b, "[napple] %s0x%016llx\n", label, (unsigned long long)v);
    out(b);
}

void runtime_peek(const char *what) {
    const char *e = getenv("NAPPLE_PEEK");
    if (!e) return;
    char mod[16];
    unsigned long long off;
    if (sscanf(e, "%15[^+]+%llx", mod, &off) != 2) return;
    const Module *m = module_by_name(mod);
    if (!m) return;
    uint64_t v;
    memcpy(&v, m->base + off, 8);
    LOG("peek", "%s: %s+%#llx = %#llx", what, mod, off, (unsigned long long)v);
}
void runtime_trace_guest(uint64_t fp, uint64_t lr) { runtime_trace_guest_n(fp, lr, 24); }
void runtime_trace_guest_n(uint64_t fp, uint64_t lr, unsigned max) {
    for (unsigned n = 0; n < max; n++) {
        char where[96];
        const Module *m = module_for_address(lr);
        if (m) {
            module_describe(lr, where, sizeof where);
            LOG("guest", "caller %s", where);
        }
        if (!fp || (fp & 7)) break;
        uint64_t f[2];
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (mach_vm_address_t)(uintptr_t)f, &got) != KERN_SUCCESS || got != 16) break;
        lr = f[1];
        if (f[0] <= fp || f[0] - fp > (64u << 20)) break;
        fp = f[0];
    }
}

/* ---- debug aid: NAPPLE_WATCH="main+0x24741f8" logs every write of the guest to that 8-byte location (pc, caller, value). The page is made read-only;
 * the faulting store is decoded (str/strb/strh/stur/stp with an immediate offset) and emulated, so the program runs on at near normal speed. ---- */
static uint64_t watch_addr, watch_page;
void watch_install(void) {
    const char *e = getenv("NAPPLE_WATCH");
    char mod[16];
    unsigned long long off;
    if (!e || sscanf(e, "%15[^+]+%llx", mod, &off) != 2) return;
    const Module *m = module_by_name(mod);
    if (!m) return;
    watch_addr = (uintptr_t)m->base + off;
    watch_page = watch_addr & ~(PAGE_16K - 1);
    mprotect((void *)watch_page, PAGE_16K, PROT_READ);
    LOG("watch", "watching %s+%#llx (%#llx), page %#llx now read-only", mod, off, (unsigned long long)watch_addr, (unsigned long long)watch_page);
}
static int watch_emulate(ucontext_t *u, uint64_t fault) {
    if (!watch_page || (fault & ~(PAGE_16K - 1)) != watch_page) return 0;
    uint64_t pc = u->uc_mcontext->__ss.__pc;
    uint32_t in = *(uint32_t *)(uintptr_t)pc;
    uint64_t *x = u->uc_mcontext->__ss.__x;
    unsigned rt = in & 31, rn = (in >> 5) & 31;
    uint64_t *basep = rn == 31 ? &u->uc_mcontext->__ss.__sp : &x[rn];
    uint64_t base = *basep;
    uint64_t val[2] = {rt == 31 ? 0 : x[rt], 0};
    uint64_t addr = 0;
    unsigned size = 0, nreg = 1;
    int wb = 0;
    int64_t wbval = 0;
    if ((in & 0x3f000000) == 0x39000000 && !((in >> 23) & 1) && !((in >> 22) & 1) && !((in >> 26) & 1)) { /* strb/strh/str w/x, [xn, #uimm] */
        size = 1u << (in >> 30);
        addr = base + (uint64_t)((in >> 10) & 0xfff) * size;
    } else if ((in & 0x3f200000) == 0x38000000 && !((in >> 22) & 3) && !((in >> 26) & 1)) { /* stur / str with post- or pre-index */
        size = 1u << (in >> 30);
        int64_t imm = (int64_t)((int32_t)(in << 11) >> 23);
        unsigned mode = (in >> 10) & 3;
        if (mode == 0) addr = base + imm;
        else if (mode == 1) {
            addr = base;
            wb = 1;
            wbval = base + imm;
        } else if (mode == 3) {
            addr = base + imm;
            wb = 1;
            wbval = addr;
        } else goto unsupported;
    } else if ((in & 0x3fe00c00) == 0x38200800 && !((in >> 26) & 1)) { /* str with a register offset */
        size = 1u << (in >> 30);
        unsigned rm = (in >> 16) & 31, opt = (in >> 13) & 7, sbit = (in >> 12) & 1;
        uint64_t off = rm == 31 ? 0 : x[rm];
        if (opt == 2) off = (uint32_t)off;
        else if (opt == 6) off = (uint64_t)(int64_t)(int32_t)off;
        else if (opt != 3 && opt != 7) goto unsupported;
        if (sbit) off <<= (size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0);
        addr = base + off;
    } else if ((in & 0x7fc00000) == 0x29000000 || (in & 0x7fc00000) == 0x29800000 || (in & 0x7fc00000) == 0x28800000) { /* stp w/x pairs: offset, pre, post */
        unsigned rt2 = (in >> 10) & 31;
        val[1] = rt2 == 31 ? 0 : x[rt2];
        nreg = 2;
        size = (in >> 31) ? 8 : 4;
        int64_t imm = (int64_t)((int32_t)(in << 10) >> 25) * size;
        unsigned kind = (in >> 23) & 3;
        if (kind == 2) addr = base + imm;
        else if (kind == 3) {
            addr = base + imm;
            wb = 1;
            wbval = addr;
        } else {
            addr = base;
            wb = 1;
            wbval = base + imm;
        }
    } else {
    unsupported:
        LOG("watch", "unsupported store %#x at pc %#llx: watching stops", in, (unsigned long long)pc);
        watch_page = 0;
        mprotect((void *)(fault & ~(PAGE_16K - 1)), PAGE_16K, PROT_READ | PROT_WRITE);
        return 1;
    }
    if (size < 8) {
        val[0] &= (1ull << (size * 8)) - 1;
        val[1] &= (1ull << (size * 8)) - 1;
    }
    mprotect((void *)watch_page, PAGE_16K, PROT_READ | PROT_WRITE);
    for (unsigned i = 0; i < nreg; i++) {
        uint64_t a = addr + i * size;
        if (a < watch_addr + 8 && a + size > watch_addr) {
            char where[96], lw[96];
            module_describe(pc, where, sizeof where);
            module_describe(u->uc_mcontext->__ss.__lr, lw, sizeof lw);
            LOG("watch", "write of %u bytes (%#llx) at %#llx by %s, lr %s", size, (unsigned long long)val[i], (unsigned long long)a, where, lw);
            runtime_trace_guest_n(u->uc_mcontext->__ss.__fp, u->uc_mcontext->__ss.__lr, 6);
        }
    }
    memcpy((void *)(uintptr_t)addr, &val[0], size);
    if (nreg == 2) memcpy((void *)(uintptr_t)(addr + size), &val[1], size);
    if (wb) *basep = (uint64_t)wbval;
    mprotect((void *)watch_page, PAGE_16K, PROT_READ);
    u->uc_mcontext->__ss.__pc += 4;
    return 1;
}

#ifdef NAPPLE_LIB
/* Library mode: the host process (a runtime of its own, the game) installed these handlers first. A fault that this image's handlers did not claim and that is not in its
 * guest code belongs to the host: pass it on instead of reporting it as ours. */
static struct sigaction g_prev[32];
static int chain_to_host(int num, siginfo_t *info, void *context) {
    if (module_for_address(((ucontext_t *)context)->uc_mcontext->__ss.__pc)) return 0; /* guest code of this image faulted: ours */
    struct sigaction *p = &g_prev[num];
    if (p->sa_flags & SA_SIGINFO) {
        if (!p->sa_sigaction) return 0;
        p->sa_sigaction(num, info, context);
        return 1;
    }
    if (p->sa_handler == SIG_DFL || p->sa_handler == SIG_IGN) return 0;
    p->sa_handler(num);
    return 1;
}
#endif
static void crash(int num, siginfo_t *info, void *context) {
    ucontext_t *u = context;
    uint64_t pc = u->uc_mcontext->__ss.__pc;
    /* EL0 cache maintenance the host kernel traps. */
    if (num == SIGILL) {
        uint32_t insn = *(const uint32_t *)(uintptr_t)pc;
        unsigned rt = insn & 31;
        if ((insn & ~31u) == 0xd53b0020u) { /* mrs Xt, CTR_EL0 */
            if (rt != 31) u->uc_mcontext->__ss.__x[rt] = HOST_CTR_EL0;
            u->uc_mcontext->__ss.__pc += 4;
            return;
        }
        if ((insn & ~31u) == 0xd50b7e20u) { /* dc civac, Xt */
            uintptr_t a = rt == 31 ? 0 : (uintptr_t)u->uc_mcontext->__ss.__x[rt];
            sys_dcache_flush((void *)(a & ~(uintptr_t)127), 128);
            u->uc_mcontext->__ss.__pc += 4;
            return;
        }
    }
    if (num == SIGTRAP) {
        extern int module_debug_log_hit(uint64_t pc, const uint64_t *x, uint64_t lr, uint64_t tid);
        if (module_debug_log_hit(pc, (const uint64_t *)u->uc_mcontext->__ss.__x, u->uc_mcontext->__ss.__lr, (uint64_t)(uintptr_t)pthread_self())) return;
    }
    if ((num == SIGBUS || num == SIGSEGV) && watch_page && watch_emulate(u, (uint64_t)(uintptr_t)info->si_addr)) return;
    if ((num == SIGBUS || num == SIGSEGV) && runtime_fault_guard) {
        sigjmp_buf *g = runtime_fault_guard;
        runtime_fault_guard = NULL;
        siglongjmp(*g, 1);
    }
    {
        extern int hid_trace_fault(const void *, void *);
        if ((num == SIGBUS || num == SIGSEGV) && hid_trace_fault(info->si_addr, context)) return;
    }
    if ((num == SIGBUS || num == SIGSEGV) && guest_fault(info->si_addr, (int)((u->uc_mcontext->__es.__esr >> 6) & 1), context)) return;

#ifdef NAPPLE_LIB
    if (chain_to_host(num, info, context)) return;
#endif
    runtime_peek("at crash");
    {
        extern void a32_crash_report(void) __attribute__((weak));
        if (a32_crash_report) a32_crash_report();
    } /* AArch32 guest: the interpreter's cpu state at the fault */
    uint64_t fp = u->uc_mcontext->__ss.__fp, lr = u->uc_mcontext->__ss.__lr;
    char where[96], text[256];
    module_describe(pc, where, sizeof where);
    snprintf(text, sizeof text, "[napple] CRASH signal=%d fault=0x%llx pc=%s\n", num, (unsigned long long)(uintptr_t)info->si_addr, where);
    out(text);
    out_hex("esr=", u->uc_mcontext->__es.__esr);
    {
        mach_vm_address_t ra = (uintptr_t)info->si_addr;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t ro = 0;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &rc, &ro) == KERN_SUCCESS) {
            out_hex("nearest region start=", ra);
            out_hex("region size=", rs);
            out_hex("region prot=", (uint64_t)ri.protection | ((uint64_t)ri.max_protection << 8));
        }
    }
    out_hex("pc=", pc);
    out_hex("lr=", lr);
    out_hex("fp=", fp);
    out_hex("sp=", u->uc_mcontext->__ss.__sp);
    module_describe(lr, where, sizeof where);
    snprintf(text, sizeof text, "[napple] lr in %s\n", where);
    out(text);
    for (unsigned i = 0; i < 29; i++) {
        char l[16];
        snprintf(l, sizeof l, "x%u=", i);
        out_hex(l, u->uc_mcontext->__ss.__x[i]);
    }
    if (getenv("NAPPLE_CRASH_DUMP")) { /* debug aid: the 16 words each register points at, for the registers that hold readable memory */
        for (unsigned i = 0; i < 29; i++) {
            uint64_t v = u->uc_mcontext->__ss.__x[i], w[16];
            mach_vm_size_t got = 0;
            if (v < 0x10000 || (v & 7) || mach_vm_read_overwrite(mach_task_self(), v, sizeof w, (mach_vm_address_t)(uintptr_t)w, &got) != KERN_SUCCESS || got != sizeof w) continue;
            snprintf(text, sizeof text, "[napple] *x%u:", i);
            out(text);
            for (unsigned k = 0; k < 16; k++) {
                snprintf(text, sizeof text, " %llx", (unsigned long long)w[k]);
                out(text);
            }
            out("\n");
        }
    }
    for (unsigned depth = 0; depth < 24 && fp && !(fp & 15); depth++) {
        uint64_t frame[2];
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (mach_vm_address_t)(uintptr_t)frame, &got) != KERN_SUCCESS || got != 16) break;
        module_describe(frame[1], where, sizeof where);
        snprintf(text, sizeof text, "[napple] frame lr=0x%llx %s\n", (unsigned long long)frame[1], where);
        out(text);
        if (frame[0] == fp) break;
        fp = frame[0];
    }
    module_describe(pc, where, sizeof where);
    char note[200];
    snprintf(note, sizeof note, "fault at %s, address 0x%llx", where, (unsigned long long)(uintptr_t)info->si_addr);
    summary_crash(num, pc, note);
    summary_write("fail", "guest crashed (see crashes)");
    filelog_drain();
    GUEST_EXIT(128 + num);
}

static void abort_report(int num) {
    (void)num;
    void *frames[64];
    int n = backtrace(frames, 64);
    out("[napple] host abort, call stack:\n");
    backtrace_symbols_fd(frames, n, 2);
    summary_crash(SIGABRT, 0, "host abort");
    summary_write("fail", "host abort");
    filelog_drain();
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void signals_install(void) {
    struct sigaction a = {0};
    a.sa_sigaction = crash;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&a.sa_mask);
#ifdef NAPPLE_LIB
    sigaction(SIGSEGV, &a, &g_prev[SIGSEGV]);
#else
    sigaction(SIGSEGV, &a, NULL);
#endif
#ifdef NAPPLE_LIB
    sigaction(SIGBUS, &a, &g_prev[SIGBUS]);
#else
    sigaction(SIGBUS, &a, NULL);
#endif
#ifdef NAPPLE_LIB
    sigaction(SIGILL, &a, &g_prev[SIGILL]);
#else
    sigaction(SIGILL, &a, NULL);
#endif
#ifdef NAPPLE_LIB
    sigaction(SIGTRAP, &a, &g_prev[SIGTRAP]);
#else
    sigaction(SIGTRAP, &a, NULL);
#endif
    struct sigaction ab = {0};
    ab.sa_handler = abort_report;
    ab.sa_flags = SA_ONSTACK;
    sigemptyset(&ab.sa_mask);
    sigaction(SIGABRT, &ab, NULL);
}
