/* hid: controller, keyboard, mouse and touch state through the input library (input/include/napple_input.h). The guest
 * reads a 0x40000-byte shared memory block that the input backend keeps current at 120 Hz; the IPC surface here only
 * configures it (supported styles, player count, vibration, six-axis) and hands out the shared memory handle. */
#include "../ipc.h"
#include "../frontend.h"
#include "hid_core.h"
#include "napple_input.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include "napple.h"
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#endif
#include "hz_result.h"

static napple_input *g_input;
#ifdef __APPLE__
static int trace_on;
static void *trace_rearm(void *arg);
#endif
static IpcSharedMemory *g_shmem;
static uint32_t npad_events;
static HidCore hidc;
static int hidc_ready;
static HidCore *HC(void) {
    if (!hidc_ready) {
        hidc_init(&hidc);
        hidc_ready = 1;
    }
    return &hidc;
}
#define IN_OK(c, off, T) ((c)->in_size >= (uint32_t)((off) + sizeof(T))) /* argument layouts below are from memory: a short request is ignored, not fatal */

static void ensure_input(void) {
    if (g_input) return;
    g_shmem = ipc_shared_memory_new(0x40000);
    napple_input_config cfg = {0};
    static char cdir[1200];
    snprintf(cdir, sizeof cdir, "%s/%s", g_cfg.save_root, g_cfg.title_id); /* --save-root moves input.json too */
    cfg.config_dir = cdir;
    cfg.title_id = g_cfg.title_id;
    cfg.shm = g_shmem->host;
    cfg.layout_profile = NAPPLE_HID_LAYOUT_LATEST;
    cfg.use_game_controller = !g_cfg.headless;
    cfg.screen_w = 1280;
    cfg.screen_h = 720;
    if (napple_input_create(&cfg, &g_input)) runtime_fail("input backend failed to start");
    napple_input_start(g_input);
#ifdef NAPPLE_LIB
    lib_mirror_host_input(g_input);
#endif
    if (getenv("NAPPLE_HID_DUMP")) { /* debug aid: once a second, slot 0's style set and the newest sample of the FullKey and SystemExt lifos */
        extern void *hid_dump_thread(void *);
        pthread_t t;
        pthread_create(&t, NULL, hid_dump_thread, NULL);
        pthread_detach(t);
    }
#ifdef __APPLE__
    if (getenv("NAPPLE_HID_TRACE")) {
        trace_on = 1;
        pthread_t t;
        pthread_create(&t, NULL, trace_rearm, NULL);
        pthread_detach(t);
    }
#endif
    {
        extern void input_script_start(napple_input *);
        input_script_start(g_input);
    }
    LOG("hid", "input backend started (%s)", g_cfg.headless ? "no GameController discovery" : "keyboard + GameController");
}

#ifdef __APPLE__
/* NAPPLE_HID_TRACE=1: debugging aid that logs which offsets of the HID shared memory the guest reads (and from where). The guest alias is
 * protected PROT_NONE and re-armed every 20 ms; each fault unprotects its page, is logged once per distinct offset (first 600) and
 * the faulting load then runs normally. Writes by the input thread go through the host alias and are unaffected. */
static int trace_n;
static uint64_t trace_seen[1024];
static void *trace_rearm(void *arg) {
    (void)arg;
    for (;;) {
        usleep(20000);
        if (g_shmem)
            for (unsigned i = 0; i < g_shmem->nmaps; i++)
                if (g_shmem->maps[i].remapped) mprotect((void *)g_shmem->maps[i].addr, g_shmem->maps[i].size, PROT_NONE);
    }
    return NULL;
}
/* Emulates the faulting load (the page stays protected, so every read is seen). Returns 0 if the instruction form is not handled. */
static int emulate_load(ucontext_t *u, uint64_t fault, const uint8_t *host_alias_base, uint64_t map_addr, uint64_t *out_size) {
    uint64_t *x = u->uc_mcontext->__ss.__x;
    uint32_t in = *(uint32_t *)(uintptr_t)u->uc_mcontext->__ss.__pc;
    unsigned rt = in & 31, rn = (in >> 5) & 31;
    uint64_t *basep = rn == 31 ? &u->uc_mcontext->__ss.__sp : &x[rn];
    uint64_t base = *basep, addr = 0;
    unsigned size = 0, nreg = 1;
    int sign = 0, wb = 0;
    int64_t wbval = 0;
    unsigned rt2 = 0, sbits = 0;
    if ((in & 0xbffffc00u) == 0x88dffc00u || (in & 0xfffffc00u) == 0x08dffc00u || (in & 0xfffffc00u) == 0x48dffc00u) { /* ldar / ldarb / ldarh */
        size = (in & 0x80000000u) ? ((in >> 30) & 1 ? 8 : 4) : ((in >> 30) & 1 ? 2 : 1);
        addr = base;
        if ((in & 0xbffffc00u) == 0x88dffc00u) size = (in >> 30) & 1 ? 8 : 4;
        else size = (in & 0x40000000u) ? 2 : 1;
    } else if ((in & 0x3f000000) == 0x39000000 && !((in >> 26) & 1) && ((in >> 22) & 3) != 0) { /* ldr/ldrs [xn, #uimm] */
        size = 1u << (in >> 30);
        addr = base + (uint64_t)((in >> 10) & 0xfff) * size;
        unsigned opc = (in >> 22) & 3;
        if (opc != 1) {
            sign = 1;
            sbits = (opc == 2) ? 64 : 32;
        }
    } else if ((in & 0x3f200c00) == 0x38000000 && !((in >> 26) & 1) && ((in >> 22) & 3) != 0) { /* ldur / ldurs */
        size = 1u << (in >> 30);
        addr = base + (int64_t)((int32_t)(in << 11) >> 23);
        unsigned opc = (in >> 22) & 3;
        if (opc != 1) {
            sign = 1;
            sbits = (opc == 2) ? 64 : 32;
        }
    } else if ((in & 0x3f200000) == 0x38000000 && !((in >> 26) & 1) && ((in >> 22) & 3) != 0 &&
               (((in >> 10) & 3) == 1 || ((in >> 10) & 3) == 3)) { /* ldr post/pre-index */
        size = 1u << (in >> 30);
        int64_t imm = (int64_t)((int32_t)(in << 11) >> 23);
        unsigned mode = (in >> 10) & 3;
        wb = 1;
        if (mode == 1) {
            addr = base;
            wbval = base + imm;
        } else {
            addr = base + imm;
            wbval = addr;
        }
    } else if ((in & 0x3fe00c00) == 0x38600800 && !((in >> 26) & 1)) { /* ldr register offset */
        size = 1u << (in >> 30);
        unsigned rm = (in >> 16) & 31, opt = (in >> 13) & 7, sb = (in >> 12) & 1;
        uint64_t off = rm == 31 ? 0 : x[rm];
        if (opt == 2) off = (uint32_t)off;
        else if (opt == 6) off = (uint64_t)(int64_t)(int32_t)off;
        else if (opt != 3 && opt != 7) return 0;
        if (sb) off <<= (size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0);
        addr = base + off;
    } else if ((in & 0x7fc00000) == 0x29400000 || (in & 0x7fc00000) == 0x29c00000 || (in & 0x7fc00000) == 0x28c00000) { /* ldp w/x */
        rt2 = (in >> 10) & 31;
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
    } else return 0;
    if (addr + size * nreg - 1 < map_addr || addr < map_addr) return 0;
    uint64_t val[2] = {0, 0};
    for (unsigned i = 0; i < nreg; i++) memcpy(&val[i], host_alias_base + (addr + i * size - map_addr), size);
    if (sign)
        for (unsigned i = 0; i < nreg; i++) {
            int sh = 64 - 8 * (int)size;
            val[i] = (uint64_t)(((int64_t)(val[i] << sh)) >> sh);
            if (sbits == 32) val[i] &= 0xffffffffu;
        }
    if (rt != 31) x[rt] = val[0];
    if (nreg == 2 && rt2 != 31) x[rt2] = val[1];
    if (wb) *basep = (uint64_t)wbval;
    u->uc_mcontext->__ss.__pc += 4;
    (void)fault;
    *out_size = size * nreg;
    return 1;
}
int hid_trace_fault(const void *addr, void *context) {
    if (!trace_on || !g_shmem) return 0;
    ucontext_t *u = context;
    uint64_t pc = u->uc_mcontext->__ss.__pc;
    for (unsigned i = 0; i < g_shmem->nmaps; i++) {
        const IpcShmemMap *m = &g_shmem->maps[i];
        uint64_t a = (uint64_t)(uintptr_t)addr;
        if (!m->remapped || a < m->addr || a >= m->addr + m->size) continue;
        uint64_t off = a - m->addr, sz = 0;
        if (!emulate_load(u, a, (const uint8_t *)g_shmem->host, m->addr, &sz)) { /* unknown instruction: let it run, page open until re-armed */
            char w[96];
            module_describe(pc, w, sizeof w);
            LOG("hid", "unhandled access form %#x at %s (shm+%#llx)", *(uint32_t *)(uintptr_t)pc, w, (unsigned long long)off);
            mprotect((void *)(a & ~(uint64_t)(PAGE_16K - 1)), PAGE_16K, PROT_READ);
            return 1;
        }
        unsigned n;
        uint64_t key = off & ~7ull;
        for (n = 0; n < (unsigned)trace_n; n++)
            if (trace_seen[n] == key) break;
        if (n == (unsigned)trace_n && trace_n < 1024) {
            trace_seen[trace_n++] = key;
            char w[96];
            module_describe(pc, w, sizeof w);
            LOG("hid", "guest read at shm+%#llx (%llu bytes, value %#llx) from %s", (unsigned long long)off, (unsigned long long)sz,
                (unsigned long long)*(volatile uint64_t *)((uint8_t *)g_shmem->host + (off & ~7ull)), w);
            runtime_trace_guest_n(u->uc_mcontext->__ss.__fp, u->uc_mcontext->__ss.__lr, 8);
            if (getenv("NAPPLE_HID_TRACE_STACK")) {
                uint64_t sp = u->uc_mcontext->__ss.__sp;
                for (int k = 0; k < 24; k++) {
                    uint64_t cand = 0, v[8];
                    mach_vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), sp + 8 * k, 8, (mach_vm_address_t)(uintptr_t)&cand, &got) != KERN_SUCCESS || cand < 0x1000)
                        continue;
                    static const unsigned offs[8] = {0x4e0, 0x4e4, 0x4e8, 0x548, 0xa4, 0x54c, 0x56c, 0x350};
                    int ok = 1;
                    for (int j = 0; j < 8; j++)
                        if (mach_vm_read_overwrite(mach_task_self(), cand + offs[j], 8, (mach_vm_address_t)(uintptr_t)&v[j], &got) != KERN_SUCCESS) ok = 0;
                    if (ok)
                        LOG("hid", "stack[%d]=%#llx: +4e0=%#llx +4e4=%#llx +4e8=%#llx +548=%#llx +a4=%#llx +54c=%#llx +56c=%#llx +350=%#llx", k,
                            (unsigned long long)cand, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
                            (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7]);
                }
            }
        }
        return 1;
    }
    return 0;
}

#else
int hid_trace_fault(const void *addr, void *context) {
    (void)addr;
    (void)context;
    return 0;
} /* the shared-memory read trace needs Mach */
#endif

napple_input *hid_input_get(void) {
    ensure_input();
    return g_input;
}

static void noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void event_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
    (void)npad_events;
}

/* AcquireNpadStyleSetUpdateEventHandle(npad id, aruid): the event the SDK polls each frame to learn that a controller was
 * connected, disconnected or changed style (nn::hid::BindNpadStyleSetUpdateEvent). The game's pad manager (Link's Awakening) only
 * reads the controller state after this event signalled once, so it is signalled when the handle is created and whenever the input
 * backend's style for that npad changes (a watcher thread compares at ~60 Hz). */
static struct {
    uint32_t writable;
    int slot;
    uint32_t last;
} npad_ev[16];
static unsigned n_npad_ev;
static pthread_mutex_t npad_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static int npad_ev_thread;
static void *npad_ev_watch(void *arg) {
    (void)arg;
    for (;;) {
        usleep(16000);
        pthread_mutex_lock(&npad_ev_mu);
        for (unsigned i = 0; i < n_npad_ev; i++) {
            uint32_t st = napple_input_active_style(g_input, npad_ev[i].slot);
            if (st != npad_ev[i].last) {
                npad_ev[i].last = st;
                event_signal_handle(npad_ev[i].writable);
            }
        }
        pthread_mutex_unlock(&npad_ev_mu);
    }
    return NULL;
}
static void hid_style_update_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    uint32_t id = IPC_IN(c, uint32_t, 0), r = 0, w = event_create(&r);
    ipc_out_copy_handle(c, r);
    int slot = napple_npad_slot_from_id(id);
    pthread_mutex_lock(&npad_ev_mu);
    if (slot >= 0 && n_npad_ev < 16) {
        npad_ev[n_npad_ev].writable = w;
        npad_ev[n_npad_ev].slot = slot;
        npad_ev[n_npad_ev].last = napple_input_active_style(g_input, slot);
        n_npad_ev++;
    }
    if (!npad_ev_thread) {
        npad_ev_thread = 1;
        pthread_t t;
        pthread_create(&t, NULL, npad_ev_watch, NULL);
        pthread_detach(t);
    }
    pthread_mutex_unlock(&npad_ev_mu);
    event_signal_handle(w);
}

static void resource_get_shmem(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    ipc_out_copy_handle(c, ipc_shared_memory_handle(g_shmem));
}
static const IpcCmd resource_cmds[] = {{0, "GetSharedMemoryHandle", resource_get_shmem, IPC_IMPL}};
static void hid_create_applet_resource(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    ipc_out_object(c, IPC_OBJECT("IAppletResource", resource_cmds, NULL, NULL));
}

static void hid_set_styles(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    HC()->styles = IPC_IN(c, uint32_t, 0);
    HC()->styles_set = 1;
    LOG("hid", "SetSupportedNpadStyleSet %#x", IPC_IN(c, uint32_t, 0));
    napple_input_set_supported_styles(g_input, IPC_IN(c, uint32_t, 0));
}
/* hid:sys ApplyNpadSystemCommonPolicy (firmware applets): the system's common npad policy supports every controller style. */
void hid_apply_system_common_policy(void) {
    ensure_input();
    HC()->styles = 0x1f;
    HC()->styles_set = 1;
    LOG("hid", "ApplyNpadSystemCommonPolicy: supported styles %#x", 0x1fu);
    napple_input_set_supported_styles(g_input, 0x1f);
}
void *hid_dump_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        const uint8_t *b = (const uint8_t *)g_shmem->host + 0x9A00;
        for (int k = 0; k < 2; k++) {
            uint32_t off = k ? 0x1408 : 0x28;
            uint64_t tail, count;
            memcpy(&tail, b + off + 0x10, 8);
            memcpy(&count, b + off + 0x18, 8);
            uint64_t btn = 0, sn = 0;
            if (tail < 17) {
                memcpy(&sn, b + off + 0x20 + tail * 0x30, 8);
                memcpy(&btn, b + off + 0x20 + tail * 0x30 + 0x10, 8);
            }
            LOG("hid", "dump slot0 %s lifo: style_set %#x tail %llu count %llu newest sample %llu buttons %#llx", k ? "SystemExt" : "FullKey", *(const uint32_t *)b, (unsigned long long)tail,
                (unsigned long long)count, (unsigned long long)sn, (unsigned long long)btn);
        }
    }
    return NULL;
}
static void hid_get_styles(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = HC()->styles_set ? HC()->styles : 0x1f;
    IPC_OUT(c, v);
}
static void push_assignment(void);
static void hid_set_id_types(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (b) {
        const uint32_t *ids = (const uint32_t *)(uintptr_t)b->addr;
        LOG("hid", "SetSupportedNpadIdType: %llu ids, first %#x", (unsigned long long)(b->size / 4), b->size >= 4 ? ids[0] : 0);
    }
    napple_input_set_player_count(g_input, b ? (int)(b->size / 4 > 8 ? 8 : (b->size / 4 ? b->size / 4 : 1)) : 1);
    hidc_reconnect_all(HC()); /* the guest declared its pads again: disconnected ones come back */
    push_assignment();
}
static void hid_assign_single(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    napple_input_set_assignment_mode(g_input, 1);
    (void)c;
}
static void hid_assign_dual(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    napple_input_set_assignment_mode(g_input, 0);
    (void)c;
}
static void hid_six_axis(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    napple_input_set_sixaxis_enabled(g_input, 1);
    (void)c;
}
static void hid_six_axis_off(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    napple_input_set_sixaxis_enabled(g_input, 0);
    (void)c;
}
/* Vibration. HidVibrationDeviceHandle (u32) = bytes {type, npad style index, player number (npad id), device index 0 left / 1 right}; the value is
 * {amp_low, freq_low, amp_high, freq_high} floats. Rumble reaches the physical pad through the input backend (CoreHaptics, input/src/gamepad.m). */
static void vib_apply(uint32_t handle, const napple_vibration *v) {
    if (!HC()->vibration_permitted) return; /* PermitVibration(false): the title muted rumble */
    uint32_t npad_id = (handle >> 16) & 0xff, side = (handle >> 24) & 0xff, style = (handle >> 8) & 0xff;
    if (style == 1 || npad_id == 0x20) npad_id = 0x20; /* handheld */
    if (side > 1) return;
    napple_input_set_vibration(g_input, npad_id, (int)side, v);
}
static void hid_send_vibration(IpcObject *s, IpcCall *c) { /* 201: {handle u32, value 16 bytes, aruid u64} */
    (void)s;
    ensure_input();
    uint32_t h = IPC_IN(c, uint32_t, 0);
    napple_vibration v;
    memcpy(&v, ipc_in(c, 4, sizeof v), sizeof v);
    vib_apply(h, &v);
}
static void hid_send_vibrations(IpcObject *s, IpcCall *c) { /* 206: in buffers {handles[], values[]}, aruid in raw */
    (void)s;
    ensure_input();
    const IpcBuf *hb = ipc_in_buf(c, 0), *vb = ipc_in_buf(c, 1);
    if (!hb || !vb) return;
    uint64_t n = hb->size / 4;
    if (vb->size / sizeof(napple_vibration) < n) n = vb->size / sizeof(napple_vibration);
    if (n > 16) n = 16;
    const uint32_t *hs = ipc_buf_ptr(hb);
    const napple_vibration *vs = ipc_buf_ptr(vb);
    for (uint64_t i = 0; i < n; i++) {
        napple_vibration v;
        memcpy(&v, &vs[i], sizeof v);
        vib_apply(hs[i], &v);
    }
}
static void hid_get_actual_vibration(IpcObject *s, IpcCall *c) { /* 202 GetActualVibrationValue(handle, aruid) -> value */
    (void)s;
    ensure_input();
    uint32_t h = IPC_IN(c, uint32_t, 0);
    int slot = napple_npad_slot_from_id(((h >> 8) & 0xff) == 1 ? 0x20 : (h >> 16) & 0xff);
    napple_vibration v = {0, 160.f, 0, 320.f};
    if (slot >= 0) napple_input_get_vibration(g_input, slot, (int)((h >> 24) & 1), &v);
    IPC_OUT(c, v);
}
/* IActiveVibrationDeviceList::ActivateVibrationDevice(handle): the device joins the active set (hid_core keeps it). SendVibrationValue reaches the
 * pad whether or not it was activated: titles that skip the activation (or activate late) still rumble, which is the more useful host behaviour. */
static void hid_vib_list_activate(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    int r = hidc_vib_activate(HC(), IPC_IN(c, uint32_t, 0));
    if (r) c->result = HZ_RESULT(HZ_MOD_HID, 301); /* [unverified] invalid vibration device handle / list full */
}
static void hid_active_vibration_list(IpcObject *s, IpcCall *c) {
    (void)s;
    static const IpcCmd cmds[] = {{0, "ActivateVibrationDevice", hid_vib_list_activate, IPC_IMPL}};
    ipc_out_object(c, IPC_OBJECT("IActiveVibrationDeviceList", cmds, NULL, NULL));
}
/* GetVibrationDeviceInfo(handle) -> {device type (1 left LRA, 2 right LRA), position (0 left, 1 right)} */
static void hid_vibration_device_info(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t h = IPC_IN(c, uint32_t, 0), side = (h >> 24) & 1;
    uint32_t v[2] = {side ? 2u : 1u, side};
    ipc_out(c, v, 8);
}

/* ---- state-backed commands (hid_core.h) ------------------------------------------------------------------------------------- */
static void hid_fusion_enabled(IpcObject *s, IpcCall *c) { /* 68(handle u32, aruid u64) -> bool */
    (void)s;
    HidcSensor *x = IN_OK(c, 0, uint32_t) ? hidc_sensor(HC(), IPC_IN(c, uint32_t, 0)) : NULL;
    uint8_t v = x ? (uint8_t)x->fusion_enabled : 1;
    IPC_OUT(c, v);
}
static void hid_enable_fusion(IpcObject *s, IpcCall *c) { /* 69(bool, handle u32 at 4, aruid u64 at 8) */
    (void)s;
    if (!IN_OK(c, 4, uint32_t)) return;
    HidcSensor *x = hidc_sensor(HC(), IPC_IN(c, uint32_t, 4));
    if (x) x->fusion_enabled = IPC_IN(c, uint8_t, 0) != 0;
}
static void hid_set_fusion_params(IpcObject *s, IpcCall *c) { /* 70(handle u32, f32 gain at 4, f32 reduction at 8, aruid) */
    (void)s;
    if (!IN_OK(c, 8, float)) return;
    HidcSensor *x = hidc_sensor(HC(), IPC_IN(c, uint32_t, 0));
    if (x) {
        x->gain = IPC_IN(c, float, 4);
        x->reduction = IPC_IN(c, float, 8);
    }
}
static void hid_reset_fusion_params(IpcObject *s, IpcCall *c) { /* 72(handle, aruid) */
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    HidcSensor *x = hidc_sensor(HC(), IPC_IN(c, uint32_t, 0));
    if (x) hidc_reset_fusion(x);
}
static void hid_set_zero_drift(IpcObject *s, IpcCall *c) { /* 79(handle u32, mode u32 at 4, aruid u64 at 8) */
    (void)s;
    if (!IN_OK(c, 4, uint32_t)) return;
    HidcSensor *x = hidc_sensor(HC(), IPC_IN(c, uint32_t, 0));
    if (x && hidc_set_zero_drift(x, IPC_IN(c, uint32_t, 4))) c->result = 0x1d80;
}
static void hid_get_zero_drift(IpcObject *s, IpcCall *c) { /* 80(handle, aruid) -> u32 */
    (void)s;
    HidcSensor *x = IN_OK(c, 0, uint32_t) ? hidc_sensor(HC(), IPC_IN(c, uint32_t, 0)) : NULL;
    uint32_t v = x ? x->zero_drift : HIDC_ZERO_DRIFT_STANDARD;
    IPC_OUT(c, v);
}
static void hid_reset_zero_drift(IpcObject *s, IpcCall *c) { /* 81(handle, aruid) */
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    HidcSensor *x = hidc_sensor(HC(), IPC_IN(c, uint32_t, 0));
    if (x) x->zero_drift = HIDC_ZERO_DRIFT_STANDARD;
}
static void hid_get_player_led_pattern(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = IN_OK(c, 0, uint32_t) ? hidc_led_pattern(IPC_IN(c, uint32_t, 0)) : 0;
    IPC_OUT(c, v);
}
/* The 8-byte fields of 120/128 are {aruid, value} or {value, aruid} depending on the SDK; our aruid (applet.c IWindowController) is 0x1000, so the field
 * that is not it is the value. */
static int64_t pick_value(IpcCall *c) {
    if (!IN_OK(c, 8, int64_t)) return IN_OK(c, 0, int64_t) ? IPC_IN(c, int64_t, 0) : 0;
    int64_t a = IPC_IN(c, int64_t, 0), b = IPC_IN(c, int64_t, 8);
    return a == 0x1000 ? b : a;
}
static void hid_set_joy_hold(IpcObject *s, IpcCall *c) {
    (void)s;
    HC()->joy_hold_type = pick_value(c);
}
static void hid_get_joy_hold(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t v = HC()->joy_hold_type;
    IPC_OUT(c, v);
}
static void hid_set_handheld_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    HC()->handheld_mode = pick_value(c);
}
static void hid_get_handheld_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t v = HC()->handheld_mode;
    IPC_OUT(c, v);
}
static void hid_home_protect_get(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = IN_OK(c, 0, uint32_t) ? (uint8_t)hidc_home_protection_get(HC(), IPC_IN(c, uint32_t, 0)) : 0;
    IPC_OUT(c, v);
}
static void hid_home_protect_set(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 4, uint32_t)) hidc_home_protection_set(HC(), IPC_IN(c, uint32_t, 4), IPC_IN(c, uint8_t, 0) != 0);
}
static void hid_permit_vibration(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) HC()->vibration_permitted = IPC_IN(c, uint8_t, 0) != 0;
}
static void hid_is_vibration_permitted(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)HC()->vibration_permitted;
    IPC_OUT(c, v);
}
static void hid_set_comm_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint64_t)) HC()->comm_mode = IPC_IN(c, uint64_t, 0);
    else if (IN_OK(c, 0, uint32_t)) HC()->comm_mode = IPC_IN(c, uint32_t, 0);
}
static void hid_get_comm_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = HC()->comm_mode;
    IPC_OUT(c, v);
}
/* IsVibrationDeviceMounted(handle, aruid): a valid handle whose npad is connected (the backend reports a style for the slot). */
static void hid_device_mounted(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t id = 0, side = 0;
    uint8_t v = 0;
    if (IN_OK(c, 0, uint32_t) && !hidc_vib_handle_decode(IPC_IN(c, uint32_t, 0), &id, &side)) {
        ensure_input();
        int slot = hidc_slot_from_id(id);
        v = slot >= 0 && napple_input_active_style(g_input, slot) != 0;
    }
    IPC_OUT(c, v);
}

/* A host pad never needs a firmware update (the Mac's GameController stack owns the device): the console's answer for an up-to-date controller. */
static void hid_fw_update_needed(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}

/* ---- npad assignment, optional sections and the rarely used commands (hid_core.h keeps the state, input/ applies it) ------------------ */
#define R_HID_INVALID_NPAD HZ_RESULT(HZ_MOD_HID, 710) /* [unverified] NpadIdType not valid for this command */
#define R_HID_SAME_NPAD HZ_RESULT(HZ_MOD_HID, 711)    /* [unverified] both ids name the same pad */
#define R_HID_INVALID_ARG HZ_RESULT(HZ_MOD_HID, 100)  /* [unverified] */
static void push_assignment(void) {
    ensure_input();
    HidCore *h = HC();
    napple_npad_assign a = {.active = h->npad_active, .disconnected = h->disconnected, .keyboard_locks = h->kb_locks};
    memcpy(a.perm, h->perm, sizeof a.perm);
    memcpy(a.dual_of, h->dual_of, sizeof a.dual_of);
    napple_input_set_assignment(g_input, &a);
}
static void hid_activate_debug_pad(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    ensure_input();
    HC()->debug_pad_active = 1;
    napple_input_set_debug_pad_enabled(g_input, 1); /* the debug pad is player 1's pad */
}
static void hid_activate_gesture(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    ensure_input();
    HC()->gesture_active = 1;
    napple_input_set_gesture_enabled(g_input, 1);
}
static void hid_activate_npad(IpcObject *s, IpcCall *c) { /* 103, 109: arms the pads again */
    (void)s;
    (void)c;
    HC()->npad_active = 1;
    hidc_reconnect_all(HC());
    push_assignment();
}
static void hid_deactivate_npad(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->npad_active = 0;
    push_assignment();
}
static void hid_disconnect_npad(IpcObject *s, IpcCall *c) { /* 107(npad id u32, aruid u64) */
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    if (hidc_disconnect(HC(), IPC_IN(c, uint32_t, 0))) {
        c->result = R_HID_INVALID_NPAD;
        return;
    }
    push_assignment();
}
static void hid_swap_assignment(IpcObject *s, IpcCall *c) { /* 130(npad id a, npad id b, aruid) */
    (void)s;
    if (!IN_OK(c, 4, uint32_t)) return;
    int r = hidc_swap(HC(), IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4));
    if (r) {
        c->result = r == -2 ? R_HID_SAME_NPAD : R_HID_INVALID_NPAD;
        return;
    }
    push_assignment();
}
static void hid_merge_joy(IpcObject *s, IpcCall *c) { /* 125(npad id a, npad id b, aruid) */
    (void)s;
    if (!IN_OK(c, 4, uint32_t)) return;
    int r = hidc_merge(HC(), IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4));
    if (r) {
        c->result = r == -2 ? R_HID_SAME_NPAD : R_HID_INVALID_NPAD;
        return;
    }
    push_assignment();
}
/* LR assignment mode (the screen where a player presses SL/SR on a lone Joy-Con to choose its side): the host pads have no detachable halves, so
 * the mode is only a flag the guest can bracket its screen with. */
static void hid_start_lr(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->lr_assign_mode = 1;
}
static void hid_stop_lr(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->lr_assign_mode = 0;
}
/* Stick centre clamp: the backend already limits every stick to the unit circle (input test "diagonal magnitude"), which is what the clamp asks for. */
static void hid_center_clamp(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) HC()->center_clamp = IPC_IN(c, uint8_t, 0) != 0;
}
/* Capture button assignment: Set(style u32 at 0, aruid at 8, button index u64 at 16), Clear(style). The Capture press itself reaches the host's
 * screenshot hotkey; the table decides which npad button bit would mirror it, which the host pads never set. */
static void hid_set_capture_button(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    int button = IN_OK(c, 16, uint64_t) ? (int)IPC_IN(c, uint64_t, 16) : -1;
    if (hidc_capture_assign(HC(), IPC_IN(c, uint32_t, 0), button)) c->result = R_HID_INVALID_ARG;
}
static void hid_clear_capture_button(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    if (hidc_capture_clear(HC(), IPC_IN(c, uint32_t, 0))) c->result = R_HID_INVALID_ARG;
}
static void hid_keyboard_lock(IpcObject *s, IpcCall *c) { /* 32(HidKeyboardLockKeyEvent bits u32) */
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    hidc_keyboard_lock_event(HC(), IPC_IN(c, uint32_t, 0));
    push_assignment();
}
/* Xpad (the pre-Npad pad interface, removed from current firmware): this host has no xpad devices, so the id list is empty and the id event never
 * fires, which is what a console without one answers. */
static void hid_get_xpad_ids(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t n = 0;
    IPC_OUT(c, n);
}
static int six_slot(IpcCall *c) {
    uint32_t h = IN_OK(c, 0, uint32_t) ? IPC_IN(c, uint32_t, 0) : 0;
    return hidc_slot_from_id(((h >> 8) & 0xff) == 1 ? 0x20 : (h >> 16) & 0xff);
}
/* IsSixAxisSensorAtRest(handle, aruid): judged from the sensor sample the backend last wrote for that pad. */
static void hid_six_at_rest(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    napple_sixaxis_state st;
    int slot = six_slot(c);
    uint8_t v = slot >= 0 && napple_input_get_sixaxis(g_input, slot, &st) ? (uint8_t)napple_sixaxis_at_rest(&st) : 1;
    IPC_OUT(c, v);
}
static void console_six_apply(void) {
    ensure_input();
    napple_input_set_console_sixaxis_enabled(g_input, HC()->console_six_active && HC()->console_six_started);
}
static void hid_console_six_activate(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->console_six_active = 1;
    console_six_apply();
}
static void hid_console_six_start(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->console_six_started = 1;
    console_six_apply();
}
static void hid_console_six_stop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    HC()->console_six_started = 0;
    console_six_apply();
}
static void hid_set_touch_config(IpcObject *s, IpcCall *c) { /* 1002(config: first byte is the mode) */
    (void)s;
    if (IN_OK(c, 0, uint8_t)) HC()->touch_mode = IPC_IN(c, uint8_t, 0);
}
static void hid_set_touch_resolution(IpcObject *s, IpcCall *c) { /* 1004(u32 width, u32 height, aruid) */
    (void)s;
    if (!IN_OK(c, 4, uint32_t)) return;
    uint32_t w = IPC_IN(c, uint32_t, 0), h = IPC_IN(c, uint32_t, 4);
    if (hidc_set_touch_resolution(HC(), w, h)) {
        c->result = R_HID_INVALID_ARG;
        return;
    }
    ensure_input();
    napple_input_set_screen_size(g_input, (int)w, (int)h);
}
static const IpcCmd hid_cmds[] = {
    {0, "CreateAppletResource", hid_create_applet_resource, IPC_IMPL},
    {1, "ActivateDebugPad", hid_activate_debug_pad, IPC_IMPL},
    {11, "ActivateTouchScreen", noop, IPC_IMPL},
    {21, "ActivateMouse", noop, IPC_IMPL},
    {31, "ActivateKeyboard", noop, IPC_IMPL},
    {32, "SendKeyboardLockKeyEvent", hid_keyboard_lock, IPC_IMPL},
    {40, "AcquireXpadIdEventHandle", event_handle, IPC_IMPL},
    {41, "ReleaseXpadIdEventHandle", noop, IPC_IMPL},
    {51, "ActivateXpad", noop, IPC_IMPL},
    {55, "GetXpadIds", hid_get_xpad_ids, IPC_IMPL},
    {66, "StartSixAxisSensor", hid_six_axis, IPC_IMPL},
    {67, "StopSixAxisSensor", hid_six_axis_off, IPC_IMPL},
    {68, "IsSixAxisSensorFusionEnabled", hid_fusion_enabled, IPC_IMPL},
    {69, "EnableSixAxisSensorFusion", hid_enable_fusion, IPC_IMPL},
    {70, "SetSixAxisSensorFusionParameters", hid_set_fusion_params, IPC_IMPL},
    {72, "ResetSixAxisSensorFusionParameters", hid_reset_fusion_params, IPC_IMPL},
    {79, "SetGyroscopeZeroDriftMode", hid_set_zero_drift, IPC_IMPL},
    {80, "GetGyroscopeZeroDriftMode", hid_get_zero_drift, IPC_IMPL},
    {81, "ResetGyroscopeZeroDriftMode", hid_reset_zero_drift, IPC_IMPL},
    {82, "IsSixAxisSensorAtRest", hid_six_at_rest, IPC_IMPL},
    {91, "ActivateGesture", hid_activate_gesture, IPC_IMPL},
    {100, "SetSupportedNpadStyleSet", hid_set_styles, IPC_IMPL},
    {101, "GetSupportedNpadStyleSet", hid_get_styles, IPC_IMPL},
    {102, "SetSupportedNpadIdType", hid_set_id_types, IPC_IMPL},
    {103, "ActivateNpad", hid_activate_npad, IPC_IMPL},
    {104, "DeactivateNpad", hid_deactivate_npad, IPC_IMPL},
    {106, "AcquireNpadStyleSetUpdateEventHandle", hid_style_update_event, IPC_IMPL},
    {107, "DisconnectNpad", hid_disconnect_npad, IPC_IMPL},
    {108, "GetPlayerLedPattern", hid_get_player_led_pattern, IPC_IMPL},
    {109, "ActivateNpadWithRevision", hid_activate_npad, IPC_IMPL},
    {120, "SetNpadJoyHoldType", hid_set_joy_hold, IPC_IMPL},
    {121, "GetNpadJoyHoldType", hid_get_joy_hold, IPC_IMPL},
    {122, "SetNpadJoyAssignmentModeSingleByDefault", hid_assign_single, IPC_IMPL},
    {123, "SetNpadJoyAssignmentModeSingle", hid_assign_single, IPC_IMPL},
    {124, "SetNpadJoyAssignmentModeDual", hid_assign_dual, IPC_IMPL},
    {125, "MergeSingleJoyAsDualJoy", hid_merge_joy, IPC_IMPL},
    {126, "StartLrAssignmentMode", hid_start_lr, IPC_IMPL},
    {127, "StopLrAssignmentMode", hid_stop_lr, IPC_IMPL},
    {128, "SetNpadHandheldActivationMode", hid_set_handheld_mode, IPC_IMPL},
    {129, "GetNpadHandheldActivationMode", hid_get_handheld_mode, IPC_IMPL},
    {130, "SwapNpadAssignment", hid_swap_assignment, IPC_IMPL},
    {131, "IsUnintendedHomeButtonInputProtectionEnabled", hid_home_protect_get, IPC_IMPL},
    {132, "EnableUnintendedHomeButtonInputProtection", hid_home_protect_set, IPC_IMPL},
    {133, "SetNpadAnalogStickUseCenterClamp", hid_center_clamp, IPC_IMPL},
    {134, "SetNpadCaptureButtonAssignment", hid_set_capture_button, IPC_IMPL},
    {135, "ClearNpadCaptureButtonAssignment", hid_clear_capture_button, IPC_IMPL},
    {200, "GetVibrationDeviceInfo", hid_vibration_device_info, IPC_IMPL},
    {201, "SendVibrationValue", hid_send_vibration, IPC_IMPL},
    {202, "GetActualVibrationValue", hid_get_actual_vibration, IPC_IMPL},
    {203, "CreateActiveVibrationDeviceList", hid_active_vibration_list, IPC_IMPL},
    {204, "PermitVibration", hid_permit_vibration, IPC_IMPL},
    {205, "IsVibrationPermitted", hid_is_vibration_permitted, IPC_IMPL},
    {206, "SendVibrationValues", hid_send_vibrations, IPC_IMPL},
    {211, "IsVibrationDeviceMounted", hid_device_mounted, IPC_IMPL},
    {300, "ActivateConsoleSixAxisSensor", hid_console_six_activate, IPC_IMPL},
    {301, "StartConsoleSixAxisSensor", hid_console_six_start, IPC_IMPL},
    {302, "StopConsoleSixAxisSensor", hid_console_six_stop, IPC_IMPL},
    {1000, "SetNpadCommunicationMode", hid_set_comm_mode, IPC_IMPL},
    {1001, "GetNpadCommunicationMode", hid_get_comm_mode, IPC_IMPL},
    {1002, "SetTouchScreenConfiguration", hid_set_touch_config, IPC_IMPL},
    {1003, "IsFirmwareUpdateNeededForNotification", hid_fw_update_needed, IPC_IMPL},
    {1004, "SetTouchScreenResolution", hid_set_touch_resolution, IPC_IMPL},
};
static IpcObject *hid_open(void) { return IPC_OBJECT("hid", hid_cmds, NULL, NULL); }
void service_register_hid(void) { ipc_register_service("hid", hid_open); }
