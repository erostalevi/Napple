/* vi:u: display service (nn::vi). One built-in display and one layer. The native window the guest passes to NVN is opaque to
 * the graphics back end (nvnBootstrapLoader is overridden), so OpenLayer returns a well-formed zero parcel. Vsync events are
 * signalled by a host thread at the back end's vsync period. */
#include "../ipc.h"
#include "../gfx.h"
#include "../timescale.h"
#include "napple_display.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t vsync_events[8];
static unsigned nvsync;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static int vthread_started;

/* The display's vsync as the guest sees it. Deadlines are absolute (next += period), so scheduler latency never accumulates: a sleep-per-tick loop
 * would run a 60 Hz game at ~59.5 Hz and slow its logic by the same fraction. After a long stall (suspend, debugger) the clock resynchronises
 * instead of firing a burst of catch-up vsyncs. */
static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t next = runtime_now_ns();
    for (;;) {
        uint64_t period =
            timescale_vsync_period_ns(gfx_vsync_period_ns()); /* re-read every tick: the player can change the frame rate or game speed while the game runs */
        next += period;
        uint64_t now = runtime_now_ns();
        if (next > now) {
            uint64_t d = next - now;
            struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
            nanosleep(&ts, NULL);
        } else if (now - next > 8 * period) next = now;
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
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t size = sizeof parcel;
    LOG("vi", "OpenLayer: out buffer %#llx bytes, input bytes %u", b ? (unsigned long long)b->size : 0ull, c->in_size);
    if (b) ipc_write_buf(c, b, parcel, sizeof parcel);
    IPC_OUT(c, size);
}
static void ds_create_stray_layer(IpcObject *s, IpcCall *c) {
    uint64_t id = 2;
    IPC_OUT(c, id);
    ds_open_layer(s, c);
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
/* The SDK's android::Surface talks to an IGraphicBufferProducer while a layer is created, even though the graphics back end
 * (which overrides nvnBootstrapLoader) never uses that producer. Every transaction is answered with a well-formed success reply:
 * a zero-filled payload of the size the call returns, ending with a zero android status. Codes are logged. */
static unsigned reply_words(uint32_t code) {
    switch (code) {
        case 9: return 1;  /* query: value */
        case 10: return 4; /* connect: QueueBufferOutput {width, height, transform hint, pending buffers} */
        case 7: return 4;  /* queueBuffer: QueueBufferOutput */
        case 3: return 2;  /* dequeueBuffer: slot, fence flag (+ fence/graphic buffer objects are not produced) */
        default: return 0;
    }
}
static void binder_transact(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t id = IPC_IN(c, uint32_t, 0), code = IPC_IN(c, uint32_t, 4), flags = IPC_IN(c, uint32_t, 8);
    LOG("vi", "IGraphicBufferProducer transaction binder %u code %u flags %#x (answered with a dummy success)", id, code, flags);
    const IpcBuf *out = ipc_out_buf(c, 0);
    if (!out) return;
    uint32_t words = reply_words(code), size = (words + 1) * 4;
    uint8_t parcel[0x80] = {0};
    uint32_t hdr[4] = {size, 0x10, 0, 0x10 + size};
    memcpy(parcel, hdr, 16);
    if (code == 10) {
        uint32_t w = 1280, h = 720;
        memcpy(parcel + 0x10, &w, 4);
        memcpy(parcel + 0x14, &h, 4);
    }
    ipc_write_buf(c, out, parcel, 0x10 + size);
}
static void binder_adjust(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void binder_native_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
}
static const IpcCmd binder_cmds[] = {{0, "TransactParcel", binder_transact, IPC_STUB},
                                     {1, "AdjustRefcount", binder_adjust, IPC_STUB},
                                     {2, "GetNativeHandle", binder_native_handle, IPC_STUB},
                                     {3, "TransactParcelAuto", binder_transact, IPC_STUB}};
static void ds_get_relay(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IHOSBinderDriver", binder_cmds, NULL, NULL));
}
/* ISystemDisplayService / IManagerDisplayService: firmware applets (MiiEdit) open them to place their layer; catch-all stubs until a command matters. */
static void ds_get_system(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, ipc_object_new("ISystemDisplayService", NULL, 0, NULL, NULL));
}
static void ds_get_manager(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, ipc_object_new("IManagerDisplayService", NULL, 0, NULL, NULL));
}
static const IpcCmd display_cmds[] = {
    {100, "GetRelayService", ds_get_relay, IPC_IMPL},
    {101, "GetSystemDisplayService", ds_get_system, IPC_STUB},
    {102, "GetManagerDisplayService", ds_get_manager, IPC_STUB},
    {1000, "ListDisplays", ds_list_displays, IPC_IMPL},
    {1010, "OpenDisplay", ds_open_display, IPC_IMPL},
    {1011, "OpenDefaultDisplay", ds_open_display, IPC_IMPL},
    {1020, "CloseDisplay", noop, IPC_IMPL},
    {1101, "SetDisplayEnabled", noop, IPC_STUB},
    {1102, "GetDisplayResolution", ds_get_resolution, IPC_IMPL},
    {2020, "OpenLayer", ds_open_layer, IPC_IMPL},
    {2021, "CloseLayer", noop, IPC_IMPL},
    {2030, "CreateStrayLayer", ds_create_stray_layer, IPC_IMPL},
    {2031, "DestroyStrayLayer", noop, IPC_IMPL},
    {2101, "SetLayerScalingMode", noop, IPC_STUB},
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
