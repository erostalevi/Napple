/* vi:u: display service (nn::vi). One built-in display and one layer. The native window the guest passes to NVN is opaque to
 * the graphics back end (nvnBootstrapLoader is overridden), so OpenLayer returns a well-formed zero parcel. Vsync events are
 * signalled by a host thread at the back end's vsync period. */
#include "../ipc.h"
#include "../gfx.h"
#include "../timescale.h"
#include "napple_display.h"
#include "vi_core.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t vsync_events[8];
static unsigned nvsync;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static int vthread_started;
static ViState vi;
static pthread_once_t vi_once = PTHREAD_ONCE_INIT;
static void vi_once_init(void) { vi_init(&vi); }
/* one lock for the layer table and binder counts (vi_core is not thread-safe by itself) */
static pthread_mutex_t vi_lock = PTHREAD_MUTEX_INITIALIZER;
static ViState *vi_state(void) {
    pthread_once(&vi_once, vi_once_init);
    return &vi;
}

/* The display's vsync as the guest sees it. Deadlines are absolute (next += period), so scheduler latency never accumulates: a sleep-per-tick loop
 * would run a 60 Hz game at ~59.5 Hz and slow its logic by the same fraction. After a long stall (suspend, debugger) the clock resynchronises
 * instead of firing a burst of catch-up vsyncs. */
static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t next = runtime_now_ns();
    for (;;) {
        /* The guest's cadence is the title's 60 Hz scaled by the Speed control only: the host present rate (fps_mode) never reaches the guest. Re-read every
         * tick: the player can change the game speed while the game runs. */
        uint64_t period = timescale_vsync_period_ns(TS_GAME_VSYNC_NS);
        uint64_t wait = timescale_deadline_step(&next, period, runtime_now_ns());
        if (wait) {
            struct timespec ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        timescale_count_vsync();
        pthread_mutex_lock(&vlock);
        for (unsigned i = 0; i < nvsync; i++) event_signal_handle(vsync_events[i]);
        pthread_mutex_unlock(&vlock);
    }
    return NULL;
}
static void noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}

static void ds_list_displays(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t n = 1;
    if (b) {
        uint8_t info[0x60] = {0};
        snprintf((char *)info, 0x40, "Default");
        uint32_t gw, gh;
        napple_display_game_size(g_cfg.op_mode_handheld ? 1280 : 1920, g_cfg.op_mode_handheld ? 720 : 1080, &gw, &gh);
        info[0x40] = 1;
        uint64_t v[3] = {1, gw, gh};
        memcpy(info + 0x48, v, 24);
        ipc_write_buf(c, b, info, sizeof info);
    }
    IPC_OUT(c, n);
}
static void ds_open_display(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    uint64_t id = 1;
    IPC_OUT(c, id);
}
static void ds_get_resolution(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t gw, gh;
    napple_display_game_size(g_cfg.op_mode_handheld ? 1280 : 1920, g_cfg.op_mode_handheld ? 720 : 1080, &gw, &gh);
    int64_t w = gw, h = gh;
    IPC_OUT(c, w);
    IPC_OUT(c, h);
}
static void ds_open_layer(IpcObject *s, IpcCall *c) {
    (void)s;
    /* The native window is an android::Parcel (nn::vi::native::ParcelIo::OpenParcel): header {data size, data offset, objects
     * size, objects offset}, data = one flat binder object (s32 type 2, s32 flags 1, u64 binder id, u64 cookie) followed by the
     * "dispdrv" token, then the array of u32 object offsets (the binder object sits at data offset 0). */
    uint8_t parcel[0x3c] = {0};
    uint32_t hdr[4] = {0x28, 0x10, 4, 0x38};
    memcpy(parcel, hdr, 16);
    uint32_t type = 2, flags = 1;
    memcpy(parcel + 0x10, &type, 4);
    memcpy(parcel + 0x14, &flags, 4);
    uint64_t binder = 1;
    memcpy(parcel + 0x18, &binder, 8);
    memcpy(parcel + 0x28, "dispdrv", 8);
    pthread_mutex_lock(&vi_lock);
    if (c->in_size >= 0x48) vi_layer(vi_state(), IPC_IN(c, uint64_t, 0x40)); /* OpenLayer(char name[0x40], u64 layer, u64 aruid) */
    vi_binder_adjust(vi_state(), 1, 1, 1);
    pthread_mutex_unlock(&vi_lock);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t size = sizeof parcel;
    LOG("vi", "OpenLayer: out buffer %#llx bytes, input bytes %u", b ? (unsigned long long)b->size : 0ull, c->in_size);
    if (b) ipc_write_buf(c, b, parcel, sizeof parcel);
    IPC_OUT(c, size);
}
static void ds_create_stray_layer(IpcObject *s, IpcCall *c) {
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = vi_layer_create(vi_state(), 0);
    uint64_t id = l ? l->id : 2;
    pthread_mutex_unlock(&vi_lock);
    IPC_OUT(c, id);
    ds_open_layer(s, c);
}
/* Layer and display state (vi_core.c). Layer ids a title names without creating (the display's own layer 1) register on first use. */
static uint64_t in_u64(const IpcCall *c, uint32_t off) { return c->in_size >= off + 8 ? IPC_IN(c, uint64_t, off) : 0; }
static void ds_set_display_enabled(IpcObject *s, IpcCall *c) { /* 1101 SetDisplayEnabled(u8 enable, u64 display) */
    (void)s;
    uint8_t on = c->in_size >= 1 ? IPC_IN(c, uint8_t, 0) : 1;
    pthread_mutex_lock(&vi_lock);
    vi_state()->display_enabled = on != 0;
    pthread_mutex_unlock(&vi_lock);
}
static void ds_set_scaling(IpcObject *s, IpcCall *c) { /* 2101 SetLayerScalingMode(u32 mode, u64 layer) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    uint32_t r = vi_set_scaling(vi_state(), in_u64(c, 8), c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0);
    pthread_mutex_unlock(&vi_lock);
    if (r) c->result = r;
}
static void ds_close_layer(IpcObject *s, IpcCall *c) { /* 2021 CloseLayer(u64 layer) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    vi_layer_destroy(vi_state(), in_u64(c, 0)); /* closing a layer that was never opened is harmless */
    pthread_mutex_unlock(&vi_lock);
}
static void ds_vsync_event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0, w = event_create(&r);
    pthread_mutex_lock(&vlock);
    if (nvsync < 8) vsync_events[nvsync++] = w;
    if (!vthread_started) {
        vthread_started = 1;
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
        pthread_detach(t);
    }
    pthread_mutex_unlock(&vlock);
    ipc_out_copy_handle(c, r);
}
/* IHOSBinderDriver (relay service): the NVN driver that would use it is not executed. */
/* The SDK's android::Surface talks to an IGraphicBufferProducer while a layer is created, even though the graphics back end (which overrides nvnBootstrapLoader) never
 * uses that producer to move pixels. vi_producer_transact (vi_core.c) keeps a consistent per-binder answer instead of a dummy: connect / disconnect, dequeue / request /
 * queue / cancel with slot bookkeeping, query of the window size and format. The parcel is {data size, data offset, objects size, objects offset} + data; the data starts
 * with the strict-mode word and the interface token (UTF-16 string), the arguments follow. Codes are logged. */
static unsigned parcel_args(const uint8_t *p, uint64_t n, const uint8_t **args) {
    *args = NULL;
    uint32_t dsize, doff;
    if (!p || n < 16) return 0;
    memcpy(&dsize, p, 4);
    memcpy(&doff, p + 4, 4);
    if (doff > n || dsize > n - doff || dsize < 8) return 0;
    const uint8_t *d = p + doff;
    int32_t tlen;
    memcpy(&tlen, d + 4, 4); /* d[0] = strict mode policy */
    if (tlen < 0 || tlen > 256) return 0;
    uint32_t tok = 8 + (((uint32_t)tlen + 1) * 2 + 3) / 4 * 4;
    if (tok > dsize) return 0;
    *args = d + tok;
    return dsize - tok;
}
static void binder_transact(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t id = IPC_IN(c, uint32_t, 0), code = IPC_IN(c, uint32_t, 4), flags = IPC_IN(c, uint32_t, 8);
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint8_t *args;
    unsigned alen = parcel_args(in ? ipc_buf_ptr(in) : NULL, in ? in->size : 0, &args);
    uint8_t reply[0x40] = {0};
    pthread_mutex_lock(&vi_lock);
    unsigned rn = vi_producer_transact(vi_state(), id, code, args, alen, reply, sizeof reply);
    pthread_mutex_unlock(&vi_lock);
    LOG("vi", "IGraphicBufferProducer transaction binder %u code %u flags %#x: %u reply bytes", id, code, flags, rn);
    if (!out) return;
    if (!rn) { /* an unknown code: a bad-value status */
        int32_t st = VI_ST_BAD_VALUE;
        memcpy(reply, &st, 4);
        rn = 4;
    }
    uint8_t parcel[0x80] = {0};
    uint32_t hdr[4] = {rn, 0x10, 0, 0x10 + rn};
    memcpy(parcel, hdr, 16);
    memcpy(parcel + 16, reply, rn);
    ipc_write_buf(c, out, parcel, 0x10 + rn);
}
/* 1 AdjustRefcount(s32 binder id, s32 addend, s32 type: 0 weak, 1 strong): counted per binder (vi_core.c), never below zero. */
static void binder_adjust(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t id = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : 0, add = c->in_size >= 8 ? IPC_IN(c, int32_t, 4) : 0,
            type = c->in_size >= 12 ? IPC_IN(c, int32_t, 8) : 1;
    pthread_mutex_lock(&vi_lock);
    vi_binder_adjust(vi_state(), id, add, type);
    pthread_mutex_unlock(&vi_lock);
}
static void binder_native_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
}
/* TransactParcel / TransactParcelAuto answer from the producer bookkeeping above; no title-visible behaviour depends on more (the back end never produces buffers). */
static const IpcCmd binder_cmds[] = {{0, "TransactParcel", binder_transact, IPC_IMPL},
                                     {1, "AdjustRefcount", binder_adjust, IPC_IMPL},
                                     {2, "GetNativeHandle", binder_native_handle, IPC_IMPL},
                                     {3, "TransactParcelAuto", binder_transact, IPC_IMPL}};
static void ds_get_relay(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IHOSBinderDriver", binder_cmds, NULL, NULL));
}
/* ISystemDisplayService / IManagerDisplayService: firmware applets (MiiEdit) open them to place their layer. The placement calls keep what they are given in the layer
 * table (vi_core.c); the host window composes the one game layer itself, so none of it moves pixels. Command ids and argument layouts are the author's memory
 * of the public description (unverified). */
static ViLayer *layer_at(IpcCall *c, uint32_t off) { return vi_layer(vi_state(), in_u64(c, off)); }
static void sd_set_position(IpcObject *s, IpcCall *c) { /* 2201 SetLayerPosition(f32 x, f32 y, u64 layer) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 8);
    if (l && c->in_size >= 8) memcpy(&l->x, c->in, 4), memcpy(&l->y, c->in + 4, 4);
    pthread_mutex_unlock(&vi_lock);
}
static void sd_set_size(IpcObject *s, IpcCall *c) { /* 2203 SetLayerSize(u64 layer, u64 width, u64 height) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 0);
    if (l) l->w = in_u64(c, 8), l->h = in_u64(c, 16);
    pthread_mutex_unlock(&vi_lock);
}
static void sd_get_z(IpcObject *s, IpcCall *c) { /* 2204 GetLayerZ(u64 layer) -> s64 */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 0);
    int64_t z = l ? l->z : 0;
    pthread_mutex_unlock(&vi_lock);
    IPC_OUT(c, z);
}
static void sd_set_z(IpcObject *s, IpcCall *c) { /* 2205 SetLayerZ(u64 layer, s64 z) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 0);
    if (l) l->z = (int64_t)in_u64(c, 8);
    pthread_mutex_unlock(&vi_lock);
}
static void sd_set_visible(IpcObject *s, IpcCall *c) { /* 2207 SetLayerVisibility(u8 visible, u64 layer) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 8);
    if (l && c->in_size >= 1) l->visible = IPC_IN(c, uint8_t, 0) != 0;
    pthread_mutex_unlock(&vi_lock);
}
static void sd_z_min(IpcObject *s, IpcCall *c) { /* 1200 GetZOrderCountMin(u64 display) -> s64 */
    (void)s;
    int64_t v = 0;
    IPC_OUT(c, v);
}
static void sd_z_max(IpcObject *s, IpcCall *c) { /* 1202 GetZOrderCountMax(u64 display) -> s64: the application layer plus the system overlays */
    (void)s;
    int64_t v = 255;
    IPC_OUT(c, v);
}
static const IpcCmd system_display_cmds[] = {
    {1200, "GetZOrderCountMin", sd_z_min, IPC_IMPL},
    {1202, "GetZOrderCountMax", sd_z_max, IPC_IMPL},
    {2201, "SetLayerPosition", sd_set_position, IPC_IMPL},
    {2203, "SetLayerSize", sd_set_size, IPC_IMPL},
    {2204, "GetLayerZ", sd_get_z, IPC_IMPL},
    {2205, "SetLayerZ", sd_set_z, IPC_IMPL},
    {2207, "SetLayerVisibility", sd_set_visible, IPC_IMPL},
};
static void md_create_layer(IpcObject *s, IpcCall *c) { /* 2010 CreateManagedLayer(u32 flags, u64 display, u64 aruid) -> u64 layer */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = vi_layer_create(vi_state(), 1);
    uint64_t id = l ? l->id : 0;
    pthread_mutex_unlock(&vi_lock);
    if (!l) {
        c->result = VI_R_NOT_FOUND;
        return;
    }
    IPC_OUT(c, id);
}
static void md_destroy_layer(IpcObject *s, IpcCall *c) { /* 2011 DestroyManagedLayer(u64 layer) */
    (void)s;
    pthread_mutex_lock(&vi_lock);
    int r = vi_layer_destroy(vi_state(), in_u64(c, 0));
    pthread_mutex_unlock(&vi_lock);
    if (r) c->result = VI_R_NOT_FOUND;
}
static void md_stack(IpcObject *s, IpcCall *c) { /* 6000 AddToLayerStack / 6001 RemoveFromLayerStack (u32 stack, u64 layer) */
    pthread_mutex_lock(&vi_lock);
    ViLayer *l = layer_at(c, 8);
    if (l && c->in_size >= 4) l->stack = c->cmd == 6000 ? (uint8_t)(IPC_IN(c, uint32_t, 0) + 1) : 0;
    pthread_mutex_unlock(&vi_lock);
    (void)s;
}
static const IpcCmd manager_display_cmds[] = {
    {2010, "CreateManagedLayer", md_create_layer, IPC_IMPL},
    {2011, "DestroyManagedLayer", md_destroy_layer, IPC_IMPL},
    {6000, "AddToLayerStack", md_stack, IPC_IMPL},
    {6001, "RemoveFromLayerStack", md_stack, IPC_IMPL},
};
static void ds_get_system(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ISystemDisplayService", system_display_cmds, NULL, NULL));
}
static void ds_get_manager(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IManagerDisplayService", manager_display_cmds, NULL, NULL));
}
static const IpcCmd display_cmds[] = {
    {100, "GetRelayService", ds_get_relay, IPC_IMPL},
    {101, "GetSystemDisplayService", ds_get_system, IPC_IMPL},
    {102, "GetManagerDisplayService", ds_get_manager, IPC_IMPL},
    {1000, "ListDisplays", ds_list_displays, IPC_IMPL},
    {1010, "OpenDisplay", ds_open_display, IPC_IMPL},
    {1011, "OpenDefaultDisplay", ds_open_display, IPC_IMPL},
    {1020, "CloseDisplay", noop, IPC_IMPL},
    {1101, "SetDisplayEnabled", ds_set_display_enabled, IPC_IMPL},
    {1102, "GetDisplayResolution", ds_get_resolution, IPC_IMPL},
    {2020, "OpenLayer", ds_open_layer, IPC_IMPL},
    {2021, "CloseLayer", ds_close_layer, IPC_IMPL},
    {2030, "CreateStrayLayer", ds_create_stray_layer, IPC_IMPL},
    {2031, "DestroyStrayLayer", noop, IPC_IMPL},
    {2101, "SetLayerScalingMode", ds_set_scaling, IPC_IMPL},
    {5202, "GetDisplayVsyncEvent", ds_vsync_event, IPC_IMPL},
};
static void vi_get_display_service(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IApplicationDisplayService", display_cmds, NULL, NULL));
}
static const IpcCmd vi_cmds[] = {{0, "GetDisplayService", vi_get_display_service, IPC_IMPL},
                                 {1, "GetDisplayServiceWithProxyNameExchange", vi_get_display_service, IPC_IMPL}};
static IpcObject *vi_open(void) { return IPC_OBJECT("vi:u", vi_cmds, NULL, NULL); }
void service_register_vi(void) {
    ipc_register_service("vi:u", vi_open);
    ipc_register_service("vi:m", vi_open);
    ipc_register_service("vi:s", vi_open);
}
