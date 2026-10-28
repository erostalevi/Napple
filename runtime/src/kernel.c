/* Horizon kernel surface: handle table, svc dispatch and the process/info svcs. Threads and synchronization live in
 * threads.c, memory in memory.c, sessions and IPC in ipc.c. Every svc the guest issues that no handler claims is
 * recorded (summary + trace) and stops the runtime with its number and name. */
#include "napple.h"
#include "kthread.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int threads_close_object(ObjType type, void *obj);
int ipc_close_object(ObjType type, void *obj);
int ksession_close_object(ObjType type, void *obj);
int kcode_close_object(ObjType type, void *obj);
void threads_svc_enter(unsigned num, uint64_t *regs, const char *module, uint64_t offset);
void threads_svc_exit(void);
void threads_checkpoint(void);
int threads_live_count(void);

/* ---- handle table ---------------------------------------------------------------------------------------------
 * Handles encode the slot in bits 0-14 and a linear id in bits 15-29 that changes on every allocation, as the kernel's
 * handle table does, so a closed handle stays invalid even when its slot is reused (guests do use stale handles).
 * Bit 30 stays clear: user-mode mutex tags use it as the "has waiters" bit. */
#define MAX_H 4096
static struct {
    ObjType type;
    void *obj;
    uint16_t lin;
} slots[MAX_H];
static uint16_t next_lin = 1;
static pthread_mutex_t h_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(ObjType type, void *obj) {
    pthread_mutex_lock(&h_lock);
    for (int i = 1; i < MAX_H; i++)
        if (!slots[i].type) {
            uint16_t lin = next_lin++;
            if (next_lin > 0x7fff) next_lin = 1;
            slots[i].type = type;
            slots[i].obj = obj;
            slots[i].lin = lin;
            pthread_mutex_unlock(&h_lock);
            return ((uint32_t)lin << 15) | (uint32_t)i;
        }
    pthread_mutex_unlock(&h_lock);
    LOG("kernel", "handle table full");
    return 0;
}
static int slot_of(uint32_t h) {
    unsigned i = h & 0x7fff, lin = (h >> 15) & 0x7fff;
    if ((h >> 30) || i == 0 || i >= MAX_H || !slots[i].type || slots[i].lin != lin) return 0;
    return (int)i;
}
void *handle_get(uint32_t h, ObjType type) {
    pthread_mutex_lock(&h_lock);
    int i = slot_of(h);
    void *o = i && slots[i].type == type ? slots[i].obj : NULL;
    pthread_mutex_unlock(&h_lock);
    return o;
}
ObjType handle_type(uint32_t h) {
    pthread_mutex_lock(&h_lock);
    int i = slot_of(h);
    ObjType t = i ? slots[i].type : OBJ_FREE;
    pthread_mutex_unlock(&h_lock);
    return t;
}
void *handle_close(uint32_t h, ObjType *type) {
    pthread_mutex_lock(&h_lock);
    int i = slot_of(h);
    void *o = NULL;
    if (i) {
        o = slots[i].obj;
        if (type) *type = slots[i].type;
        slots[i].type = OBJ_FREE;
        slots[i].obj = NULL;
    }
    pthread_mutex_unlock(&h_lock);
    return i ? (o ? o : (void *)1) : NULL;
}

/* ---- svc table ------------------------------------------------------------------------------------------------ */
static SvcHandler handlers[256];
static const char *names[256];
int svc_trace_all;

void svc_register(unsigned num, const char *name, SvcHandler fn) {
    if (num > 255) runtime_fail("svc number %u", num);
    handlers[num] = fn;
    names[num] = name;
}

static const char *known_names[256] = {
    [0x01] = "SetHeapSize",
    [0x02] = "SetMemoryPermission",
    [0x03] = "SetMemoryAttribute",
    [0x04] = "MapMemory",
    [0x05] = "UnmapMemory",
    [0x06] = "QueryMemory",
    [0x07] = "ExitProcess",
    [0x08] = "CreateThread",
    [0x09] = "StartThread",
    [0x0a] = "ExitThread",
    [0x0b] = "SleepThread",
    [0x0c] = "GetThreadPriority",
    [0x0d] = "SetThreadPriority",
    [0x0e] = "GetThreadCoreMask",
    [0x0f] = "SetThreadCoreMask",
    [0x10] = "GetCurrentProcessorNumber",
    [0x11] = "SignalEvent",
    [0x12] = "ClearEvent",
    [0x13] = "MapSharedMemory",
    [0x14] = "UnmapSharedMemory",
    [0x15] = "CreateTransferMemory",
    [0x16] = "CloseHandle",
    [0x17] = "ResetSignal",
    [0x18] = "WaitSynchronization",
    [0x19] = "CancelSynchronization",
    [0x1a] = "ArbitrateLock",
    [0x1b] = "ArbitrateUnlock",
    [0x1c] = "WaitProcessWideKeyAtomic",
    [0x1d] = "SignalProcessWideKey",
    [0x1e] = "GetSystemTick",
    [0x1f] = "ConnectToNamedPort",
    [0x20] = "SendSyncRequestLight",
    [0x21] = "SendSyncRequest",
    [0x22] = "SendSyncRequestWithUserBuffer",
    [0x23] = "SendAsyncRequestWithUserBuffer",
    [0x24] = "GetProcessId",
    [0x25] = "GetThreadId",
    [0x26] = "Break",
    [0x27] = "OutputDebugString",
    [0x28] = "ReturnFromException",
    [0x29] = "GetInfo",
    [0x2a] = "FlushEntireDataCache",
    [0x2b] = "FlushDataCache",
    [0x2c] = "MapPhysicalMemory",
    [0x2d] = "UnmapPhysicalMemory",
    [0x2e] = "GetDebugFutureThreadInfo",
    [0x2f] = "GetLastThreadInfo",
    [0x30] = "GetResourceLimitLimitValue",
    [0x31] = "GetResourceLimitCurrentValue",
    [0x32] = "SetThreadActivity",
    [0x33] = "GetThreadContext3",
    [0x34] = "WaitForAddress",
    [0x35] = "SignalToAddress",
    [0x36] = "SynchronizePreemptionState",
    [0x37] = "GetResourceLimitPeakValue",
    [0x3c] = "KernelDebug",
    [0x3d] = "ChangeKernelTraceState",
    [0x40] = "CreateSession",
    [0x41] = "AcceptSession",
    [0x42] = "ReplyAndReceiveLight",
    [0x43] = "ReplyAndReceive",
    [0x44] = "ReplyAndReceiveWithUserBuffer",
    [0x45] = "CreateEvent",
    [0x46] = "MapIoRegion",
    [0x47] = "UnmapIoRegion",
    [0x48] = "MapPhysicalMemoryUnsafe",
    [0x49] = "UnmapPhysicalMemoryUnsafe",
    [0x4a] = "SetUnsafeLimit",
    [0x4b] = "CreateCodeMemory",
    [0x4c] = "ControlCodeMemory",
    [0x4d] = "SleepSystem",
    [0x4e] = "ReadWriteRegister",
    [0x4f] = "SetProcessActivity",
    [0x50] = "CreateSharedMemory",
    [0x51] = "MapTransferMemory",
    [0x52] = "UnmapTransferMemory",
    [0x53] = "CreateInterruptEvent",
    [0x54] = "QueryPhysicalAddress",
    [0x55] = "QueryIoMapping",
    [0x56] = "CreateDeviceAddressSpace",
    [0x57] = "AttachDeviceAddressSpace",
    [0x58] = "DetachDeviceAddressSpace",
    [0x59] = "MapDeviceAddressSpaceByForce",
    [0x5a] = "MapDeviceAddressSpaceAligned",
    [0x5c] = "UnmapDeviceAddressSpace",
    [0x70] = "CreatePort",
    [0x71] = "ManageNamedPort",
    [0x72] = "ConnectToPort",
    [0x73] = "SetProcessMemoryPermission",
    [0x74] = "MapProcessMemory",
    [0x75] = "UnmapProcessMemory",
    [0x76] = "QueryProcessMemory",
    [0x77] = "MapProcessCodeMemory",
    [0x78] = "UnmapProcessCodeMemory",
    [0x79] = "CreateProcess",
    [0x7a] = "StartProcess",
    [0x7b] = "TerminateProcess",
    [0x7c] = "GetProcessInfo",
};
const char *svc_name(unsigned num) { return num < 256 ? (names[num] ? names[num] : known_names[num]) : NULL; }

/* Post-mortem ring of the last svcs of every thread (NAPPLE_SVC_RING=1; costs one atomic add and a 72-byte store per svc). Dumped by guest Break and by the crash handler (svc_ring_dump), so an intermittent abort shows the calls that led to it
 * without the timing change of a full svc trace. */
typedef struct {
    uint64_t t_ns, in[3], out[2];
    uint32_t num, tid, cmd, ires; /* IPC (0x21/0x22): command id of the request and result word of the reply (~0 when none) */
    uint64_t lr;
} SvcRec;
#define SVC_RING_N 8192
static SvcRec g_ring[SVC_RING_N];
static _Atomic uint64_t g_ring_next;
static int g_ring_on = -1;
static int svc_ring_enabled(void) {
    if (g_ring_on < 0) g_ring_on = getenv("NAPPLE_SVC_RING") ? 1 : 0;
    return g_ring_on;
}
/* The word after `magic` ("SFCI" request: command id at +8, "SFCO" reply: result at +8 of the magic) in the first 0x40 bytes of the message buffer. */
static uint32_t ring_ipc_word(Thread *t, uint32_t magic, unsigned at) {
    const uint32_t *w = t ? t->tlr : NULL;
    if (!w) return ~0u;
    for (unsigned i = 2; i < 12; i++)
        if (w[i] == magic) return w[i + at];
    return ~0u;
}
void svc_ring_dump(unsigned per_thread, unsigned total) {
    if (!svc_ring_enabled()) return;
    uint64_t end = atomic_load(&g_ring_next), self = threads_current_id();
    uint64_t begin = end > SVC_RING_N ? end - SVC_RING_N : 0;
    unsigned shown = 0;
    LOG("svcring", "last svcs of thread %#llx (oldest first), then the last %u of all threads", (unsigned long long)self, total);
    uint64_t from = end;
    for (uint64_t i = end; i > begin && shown < per_thread; i--)
        if (g_ring[(i - 1) % SVC_RING_N].tid == self) {
            from = i - 1;
            shown++;
        }
    for (uint64_t i = from; i < end; i++) {
        const SvcRec *r = &g_ring[i % SVC_RING_N];
        if (r->tid != self) continue;
        char w[96];
        module_describe(r->lr, w, sizeof w);
        LOG("svcring", "  t=%.6f %#04x %s from %s in %#llx %#llx %#llx -> %#llx %#llx ipc cmd %d result %#x", r->t_ns / 1e9, r->num, svc_name(r->num) ? svc_name(r->num) : "?", w,
            (unsigned long long)r->in[0], (unsigned long long)r->in[1], (unsigned long long)r->in[2], (unsigned long long)r->out[0], (unsigned long long)r->out[1],
            r->cmd == ~0u ? -1 : (int)r->cmd, r->ires);
    }
    uint64_t start = end > total && end - total > begin ? end - total : begin;
    for (uint64_t i = start; i < end; i++) {
        const SvcRec *r = &g_ring[i % SVC_RING_N];
        LOG("svcring", "  all t=%.6f tid %#llx %#04x %s in %#llx %#llx -> %#llx ipc %d/%#x", r->t_ns / 1e9, (unsigned long long)r->tid, r->num, svc_name(r->num) ? svc_name(r->num) : "?",
            (unsigned long long)r->in[0], (unsigned long long)r->in[1], (unsigned long long)r->out[0], r->cmd == ~0u ? -1 : (int)r->cmd, r->ires);
    }
}
void svc_dispatch(unsigned num, uint64_t *regs, const char *module, uint64_t offset) {
    threads_svc_enter(num, regs, module, offset);
    threads_checkpoint();
    if (svc_trace_all)
        LOG("svc", "0x%02x %s from %s+0x%llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx", num, svc_name(num) ? svc_name(num) : "?", module, (unsigned long long)offset,
            (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], (unsigned long long)regs[3]);
    if (num < 256 && handlers[num]) {
        SvcRec rec;
        int ring = svc_ring_enabled();
        if (ring) {
            rec.in[0] = regs[0];
            rec.in[1] = regs[1];
            rec.in[2] = regs[2];
            rec.lr = regs[30];
            rec.cmd = num == 0x21 || num == 0x22 ? ring_ipc_word(kthread_self_or_null(), 0x49434653, 2) : ~0u;
        }
        handlers[num](regs);
        if (ring) {
            rec.num = num;
            rec.tid = (uint32_t)threads_current_id();
            rec.out[0] = regs[0];
            rec.out[1] = regs[1];
            rec.ires = num == 0x21 || num == 0x22 ? ring_ipc_word(kthread_self_or_null(), 0x4f434653, 2) : ~0u;
            rec.t_ns = runtime_now_ns();
            g_ring[atomic_fetch_add(&g_ring_next, 1) % SVC_RING_N] = rec;
        }
        threads_svc_exit();
        if (svc_trace_all) LOG("svc", "   -> 0x%02x result=%#llx x1=%#llx", num, (unsigned long long)regs[0], (unsigned long long)regs[1]);
        return;
    }
    summary_svc_unsupported(num, svc_name(num));
    trace_event("svc_unsupported", "\"svc\":%u,\"name\":\"%s\",\"module\":\"%s\",\"offset\":\"0x%llx\"", num, svc_name(num) ? svc_name(num) : "?", module,
                (unsigned long long)offset);
    runtime_trace_guest(regs[29], regs[30]);
    if (getenv("NAPPLE_SVC_LENIENT")) {
        regs[0] = R_NOT_IMPLEMENTED;
        return;
    } /* enumerate more unsupported svcs in one run */
    runtime_fail("unimplemented svc 0x%02x (%s) at %s+0x%llx", num, svc_name(num) ? svc_name(num) : "?", module, (unsigned long long)offset);
}

/* ---- generic svcs ---------------------------------------------------------------------------------------------- */
static void svc_close_handle(uint64_t *r) {
    uint32_t h = (uint32_t)r[0];
    ObjType type = OBJ_FREE;
    void *o = handle_close(h, &type);
    if (!o) {
        r[0] = R_INVALID_HANDLE;
        return;
    }
    if (o != (void *)1 && !threads_close_object(type, o) && !ksession_close_object(type, o) && !kcode_close_object(type, o)) ipc_close_object(type, o);
    r[0] = 0;
}
/* SendAsyncRequestWithUserBuffer(x1 message buffer, x2 size, w3 session) -> w1 readable event. There is no second kernel thread to run the
 * request in the background, so it is delivered like SendSyncRequestWithUserBuffer (0x22) and the returned event is already signalled:
 * a guest that waits on it, or reads the reply after it, sees the same result as on hardware once the server has answered. A request to a
 * guest server thread therefore blocks here until that server replies. */
static void svc_send_async_user(uint64_t *r) {
    SvcHandler sync = handlers[0x22];
    if (!sync) {
        r[0] = R_NOT_IMPLEMENTED;
        return;
    }
    uint64_t s[9] = {r[1], r[2], r[3]};
    sync(s); /* x0 addr, x1 size, x2 session handle */
    if (s[0]) {
        r[0] = s[0];
        return;
    }
    uint32_t rd = 0, w = event_create(&rd);
    event_signal_handle(w);
    uint64_t c[9] = {w};
    svc_close_handle(c); /* the guest only gets the readable end */
    r[0] = 0;
    r[1] = rd;
}
static void svc_get_system_tick(uint64_t *r) { r[0] = runtime_guest_ticks(); }
static void svc_get_process_id(uint64_t *r) {
    r[0] = 0;
    r[1] = 0x80;
}
static void svc_flush_cache(uint64_t *r) { r[0] = 0; }
static void svc_exit_process(uint64_t *r) {
    (void)r;
    LOG("kernel", "guest called ExitProcess");
    summary_note("guest called ExitProcess");
    summary_write("fail", "guest exited the process (ExitProcess)");
    GUEST_EXIT(0);
}
static void svc_output_debug_string(uint64_t *r) {
    uint64_t n = r[1];
    if (n > 4096) n = 4096;
    const char *p = guest_ptr(r[0], n);
    if (p) {
        char buf[4200];
        memcpy(buf, p, n);
        buf[n] = 0;
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        LOG("guest-debug", "%s", buf);
    }
    r[0] = 0;
}
static void svc_break(uint64_t *r) {
    uint32_t result = 0;
    const void *info = r[1] && r[2] >= 4 ? guest_ptr(r[1], 4) : NULL;
    if (info) memcpy(&result, info, 4);
    char where[96];
    module_describe(r[30], where, sizeof where);
    LOG("kernel", "guest Break reason=%#llx result=%#x (module %u desc %u) from %s", (unsigned long long)r[0], result, result & 0x1ff, (result >> 9) & 0x1fff,
        where);
    runtime_trace_guest(r[29], r[30]);
    svc_ring_dump(40, 120);
    thread_dump_all();
    if (r[0] & 0x80000000ULL) {
        r[0] = 0;
        return;
    } /* notification only: the program continues */
    summary_note("guest Break reason=%#llx result=%#x at %s", (unsigned long long)r[0], result, where);
    runtime_fail("guest Break (reason %#llx, result %#x) from %s", (unsigned long long)r[0], result, where);
}
/* Debug svcs. An application has no debug objects, so GetDebugFutureThreadInfo always answers InvalidHandle; KernelDebug and
 * ChangeKernelTraceState return nothing and do nothing on a retail kernel either. */
static void svc_get_debug_future_thread_info(uint64_t *r) { r[0] = R_INVALID_HANDLE; }
static void svc_kernel_debug(uint64_t *r) {
    LOGV("kernel", "KernelDebug(%llu, %#llx, %#llx, %#llx) ignored", (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2],
         (unsigned long long)r[3]);
}
static void svc_change_kernel_trace_state(uint64_t *r) { (void)r; }
static void svc_return_from_exception(uint64_t *r) {
    (void)r;
    runtime_fail("ReturnFromException without an exception");
}

/* GetInfo(out x1, id x1, handle x2, subid x3). Process-scoped ids answer for this single process. */
static void svc_get_info(uint64_t *r) {
    uint64_t value = 0;
    unsigned id = (unsigned)r[1];
    uint64_t sub = r[3];
    memory_init();
    switch (id) {
        case 0: value = 7; break; /* CoreMask */
        case 1: { /* PriorityMask */ value = 0x0ffffffff0000000ULL; break;
        }
        case 2: value = memory_alias_base(); break;
        case 3: value = memory_alias_size(); break;
        case 4: value = memory_heap_base(); break;
        case 5:
            value = 0;
            {
                extern uint64_t memory_heap_cap(void);
                value = memory_heap_cap();
            }
            break;
        case 6:
        case 21: value = g_application_pool_bytes; break; /* TotalMemorySize / TotalNonSystemMemorySize */
        case 7:
        case 22: {
            extern uint64_t memory_used_bytes(void);
            value = memory_used_bytes();
        } break;
        case 8: value = 0; break; /* DebuggerAttached */
        case 9:
            r[0] = 0;
            r[1] = handle_new(OBJ_MISC, (void *)(uintptr_t)1);
            return;                /* ResourceLimit */
        case 10: value = 0; break; /* IdleTickCount */
        case 11: {
            arc4random_buf(&value, 8);
            break;
        } /* RandomEntropy (subid 0..3) */
        case 12: value = memory_aslr_base(); break;
        case 13: value = memory_aslr_size(); break;
        case 14: value = memory_stack_region_base(); break;
        case 15: value = memory_stack_region_size(); break;
        case 16: value = g_npdm.valid ? g_npdm.system_resource_size : (32ULL << 20); break;
        case 17: value = 0x100000; break; /* SystemResourceSizeUsed */
        case 18: value = g_program_id; break;
        case 20: {
            extern uint64_t memory_exception_context(void);
            value = memory_exception_context();
        } break;
        case 23: value = 1; break; /* IsApplication */
        case 24: {
            int live = threads_live_count();
            value = live < 96 ? 96 - live : 0;
            break;
        }                          /* FreeThreadCount: pm's 96 application threads minus the live ones */
        case 25: value = 0; break; /* ThreadTickCount */
        case 26: { /* IsSvcPermitted */ value = !g_npdm.valid || (sub < 192 && (g_npdm.svc_mask[sub / 32] >> (sub % 32) & 1)); break;
        }
        case 28: value = 0; break; /* AliasRegionExtraSize */
        default:
            LOG("kernel", "GetInfo id %u (handle %#llx, subid %llu) not implemented", id, (unsigned long long)r[2], (unsigned long long)sub);
            summary_note("GetInfo id %u", id);
            r[0] = R_INVALID_ENUM;
            return;
    }
    r[0] = 0;
    r[1] = value;
}
/* Resource limits: generous plausible numbers; the SDK only checks that creating objects will not run out. */
static uint64_t resource_limit(unsigned which, int current) {
    static const uint64_t limit[] = {0, 0x200, 0x2000, 0x80, 0x80}; /* memory (set at runtime), threads, events, transfer memories, sessions */
    if (which == 0) return g_application_pool_bytes;
    if (which > 4) return 0;
    return current ? 1 : limit[which];
}
static void svc_get_resource_limit_value(uint64_t *r) {
    unsigned which = (unsigned)r[2];
    r[0] = which <= 4 ? 0 : R_INVALID_ENUM;
    r[1] = resource_limit(which, 0);
}
static void svc_get_resource_limit_current(uint64_t *r) {
    unsigned which = (unsigned)r[2];
    r[0] = which <= 4 ? 0 : R_INVALID_ENUM;
    r[1] = resource_limit(which, 1);
}

void svc_kernel_register(void) {
    svc_register(0x07, "ExitProcess", svc_exit_process);
    svc_register(0x16, "CloseHandle", svc_close_handle);
    svc_register(0x1E, "GetSystemTick", svc_get_system_tick);
    svc_register(0x23, "SendAsyncRequestWithUserBuffer", svc_send_async_user);
    svc_register(0x24, "GetProcessId", svc_get_process_id);
    svc_register(0x26, "Break", svc_break);
    svc_register(0x27, "OutputDebugString", svc_output_debug_string);
    svc_register(0x28, "ReturnFromException", svc_return_from_exception);
    svc_register(0x29, "GetInfo", svc_get_info);
    svc_register(0x2A, "FlushEntireDataCache", svc_flush_cache);
    svc_register(0x2B, "FlushDataCache", svc_flush_cache);
    svc_register(0x30, "GetResourceLimitLimitValue", svc_get_resource_limit_value);
    svc_register(0x31, "GetResourceLimitCurrentValue", svc_get_resource_limit_current);
    svc_register(0x37, "GetResourceLimitPeakValue", svc_get_resource_limit_current);
    svc_register(0x2E, "GetDebugFutureThreadInfo", svc_get_debug_future_thread_info);
    svc_register(0x3C, "KernelDebug", svc_kernel_debug);
    svc_register(0x3D, "ChangeKernelTraceState", svc_change_kernel_trace_state);
}
