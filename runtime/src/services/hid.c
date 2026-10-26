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
#include <sys/ucontext.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"

static napple_input *g_input;
static int trace_on;
static void *trace_rearm(void *arg);
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
    if (getenv("NAPPLE_HID_TRACE")) {
        trace_on = 1;
        pthread_t t;
        pthread_create(&t, NULL, trace_rearm, NULL);
        pthread_detach(t);
    }
    {
        extern void input_script_start(napple_input *);
        input_script_start(g_input);
    }
    LOG("hid", "input backend started (%s)", g_cfg.headless ? "no GameController discovery" : "keyboard + GameController");
}

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

napple_input *hid_input_get(void) {
    ensure_input();
    return g_input;
}

static void noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void ret_i64_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t v = 0;
    IPC_OUT(c, v);
}
static void ret_u8_1(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
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
static void hid_get_styles(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = HC()->styles_set ? HC()->styles : 0x1f;
    IPC_OUT(c, v);
}
static void hid_set_id_types(IpcObject *s, IpcCall *c) {
    (void)s;
    ensure_input();
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (b) {
        const uint32_t *ids = (const uint32_t *)(uintptr_t)b->addr;
        LOG("hid", "SetSupportedNpadIdType: %llu ids, first %#x", (unsigned long long)(b->size / 4), b->size >= 4 ? ids[0] : 0);
    }
    napple_input_set_player_count(g_input, b ? (int)(b->size / 4 > 8 ? 8 : (b->size / 4 ? b->size / 4 : 1)) : 1);
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
static void hid_active_vibration_list(IpcObject *s, IpcCall *c) {
    (void)s;
    static const IpcCmd cmds[] = {{0, "ActivateVibrationDevice", noop, IPC_STUB}};
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
static void hid_device_mounted(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
} /* a vibration device handle the SDK produced is always attached (one virtual pad per slot) */

static void hid_fw_update_needed(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static const IpcCmd hid_cmds[] = {
    {0, "CreateAppletResource", hid_create_applet_resource, IPC_IMPL},
    {1, "ActivateDebugPad", noop, IPC_IMPL},
    {11, "ActivateTouchScreen", noop, IPC_IMPL},
    {21, "ActivateMouse", noop, IPC_IMPL},
    {31, "ActivateKeyboard", noop, IPC_IMPL},
    {32, "SendKeyboardLockKeyEvent", noop, IPC_STUB},
    {40, "AcquireXpadIdEventHandle", event_handle, IPC_STUB},
    {41, "ReleaseXpadIdEventHandle", noop, IPC_STUB},
    {51, "ActivateXpad", noop, IPC_STUB},
    {55, "GetXpadIds", ret_i64_0, IPC_STUB},
    {66, "StartSixAxisSensor", hid_six_axis, IPC_IMPL},
    {67, "StopSixAxisSensor", hid_six_axis_off, IPC_IMPL},
    {68, "IsSixAxisSensorFusionEnabled", hid_fusion_enabled, IPC_IMPL},
    {69, "EnableSixAxisSensorFusion", hid_enable_fusion, IPC_IMPL},
    {70, "SetSixAxisSensorFusionParameters", hid_set_fusion_params, IPC_IMPL},
    {72, "ResetSixAxisSensorFusionParameters", hid_reset_fusion_params, IPC_IMPL},
    {79, "SetGyroscopeZeroDriftMode", hid_set_zero_drift, IPC_IMPL},
    {80, "GetGyroscopeZeroDriftMode", hid_get_zero_drift, IPC_IMPL},
    {81, "ResetGyroscopeZeroDriftMode", hid_reset_zero_drift, IPC_IMPL},
    {82, "IsSixAxisSensorAtRest", ret_u8_1, IPC_STUB},
    {91, "ActivateGesture", noop, IPC_STUB},
    {100, "SetSupportedNpadStyleSet", hid_set_styles, IPC_IMPL},
    {101, "GetSupportedNpadStyleSet", hid_get_styles, IPC_IMPL},
    {102, "SetSupportedNpadIdType", hid_set_id_types, IPC_IMPL},
    {103, "ActivateNpad", noop, IPC_IMPL},
    {104, "DeactivateNpad", noop, IPC_STUB},
    {106, "AcquireNpadStyleSetUpdateEventHandle", hid_style_update_event, IPC_IMPL},
    {107, "DisconnectNpad", noop, IPC_STUB},
    {108, "GetPlayerLedPattern", hid_get_player_led_pattern, IPC_IMPL},
    {109, "ActivateNpadWithRevision", noop, IPC_IMPL},
    {120, "SetNpadJoyHoldType", hid_set_joy_hold, IPC_IMPL},
    {121, "GetNpadJoyHoldType", hid_get_joy_hold, IPC_IMPL},
    {122, "SetNpadJoyAssignmentModeSingleByDefault", hid_assign_single, IPC_IMPL},
    {123, "SetNpadJoyAssignmentModeSingle", hid_assign_single, IPC_IMPL},
    {124, "SetNpadJoyAssignmentModeDual", hid_assign_dual, IPC_IMPL},
    {125, "MergeSingleJoyAsDualJoy", noop, IPC_STUB},
    {126, "StartLrAssignmentMode", noop, IPC_STUB},
    {127, "StopLrAssignmentMode", noop, IPC_STUB},
    {128, "SetNpadHandheldActivationMode", hid_set_handheld_mode, IPC_IMPL},
    {129, "GetNpadHandheldActivationMode", hid_get_handheld_mode, IPC_IMPL},
    {130, "SwapNpadAssignment", noop, IPC_STUB},
    {131, "IsUnintendedHomeButtonInputProtectionEnabled", hid_home_protect_get, IPC_IMPL},
    {132, "EnableUnintendedHomeButtonInputProtection", hid_home_protect_set, IPC_IMPL},
    {133, "SetNpadAnalogStickUseCenterClamp", noop, IPC_STUB},
    {134, "SetNpadCaptureButtonAssignment", noop, IPC_STUB},
    {135, "ClearNpadCaptureButtonAssignment", noop, IPC_STUB},
    {200, "GetVibrationDeviceInfo", hid_vibration_device_info, IPC_IMPL},
    {201, "SendVibrationValue", hid_send_vibration, IPC_IMPL},
    {202, "GetActualVibrationValue", hid_get_actual_vibration, IPC_IMPL},
    {203, "CreateActiveVibrationDeviceList", hid_active_vibration_list, IPC_IMPL},
    {204, "PermitVibration", hid_permit_vibration, IPC_IMPL},
    {205, "IsVibrationPermitted", hid_is_vibration_permitted, IPC_IMPL},
    {206, "SendVibrationValues", hid_send_vibrations, IPC_IMPL},
    {211, "IsVibrationDeviceMounted", hid_device_mounted, IPC_STUB},
    {300, "ActivateConsoleSixAxisSensor", noop, IPC_STUB},
    {301, "StartConsoleSixAxisSensor", noop, IPC_STUB},
    {302, "StopConsoleSixAxisSensor", noop, IPC_STUB},
    {1000, "SetNpadCommunicationMode", hid_set_comm_mode, IPC_IMPL},
    {1001, "GetNpadCommunicationMode", hid_get_comm_mode, IPC_IMPL},
    {1002, "SetTouchScreenConfiguration", noop, IPC_STUB},
    {1003, "IsFirmwareUpdateNeededForNotification", hid_fw_update_needed, IPC_STUB},
    {1004, "SetTouchScreenResolution", noop, IPC_STUB},
};
static IpcObject *hid_open(void) { return IPC_OBJECT("hid", hid_cmds, NULL, NULL); }
void service_register_hid(void) { ipc_register_service("hid", hid_open); }
