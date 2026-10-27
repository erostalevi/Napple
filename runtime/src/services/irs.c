/* irs (nn::irsensor): the Joy-Con IR camera, simulated over an IrFrameSource (irs_core.h). With no frame source installed every processor reports the IR
 * sensor as not connected (the offline behaviour). With one, the moment, clustering and image-transfer processors run on its frames: the states are written
 * into the irsensor shared memory and the transfer memory by a pump thread (about 60 Hz) that runs only while a processor is running; camera access is
 * requested from the source when the first processor starts. Command ids, argument offsets and struct layouts are from the author's memory of the public
 * documentation (unverified on a console or a title). */
#include "svc_common.h"
#include "irs_core.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

static IpcSharedMemory *shm;
static IrsCore core;
static int core_ready, env_checked, thread_run, thread_live;
static pthread_t pump_thread;
static pthread_mutex_t irs_lock = PTHREAD_MUTEX_INITIALIZER;
static int irs_active;              /* ActivateIrsensor / ActivateIrsensorWithFunctionLevel seen and not deactivated */
static uint32_t irs_function_level; /* SetFunctionLevel: the sensor's function level (0 = default); processors read it from the config, so it is only kept */

static uint64_t now_ns(void) { return runtime_guest_ticks() * 625ull / 12ull; } /* 19.2 MHz ticks to ns */
static void ensure(void) {
    if (!core_ready) {
        irs_core_init(&core);
        core_ready = 1;
    }
}
static void ensure_source(void) { /* caller holds irs_lock */
    ensure();
    if (!env_checked) {
        env_checked = 1;
        IrFrameSource s;
        const char *spec = getenv("NAPPLE_IR_SOURCE");
        if (!irs_core_has_source(&core) && spec && irs_source_from_spec(spec, &s) == 0) irs_core_set_source(&core, &s);
    }
}
void irs_service_set_source(const IrFrameSource *src) {
    pthread_mutex_lock(&irs_lock);
    ensure();
    env_checked = 1; /* an explicit source wins over the environment */
    irs_core_set_source(&core, src);
    pthread_mutex_unlock(&irs_lock);
}
static void *pump_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&irs_lock);
        int run = thread_run;
        if (run && shm) {
            irs_core_pump(&core, shm->host, now_ns());
            ipc_shared_memory_flush(shm);
        }
        pthread_mutex_unlock(&irs_lock);
        if (!run) break;
        struct timespec ts = {0, 16 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
static void pump_start(void) { /* caller holds irs_lock */
    if (thread_live) return;
    thread_run = 1;
    thread_live = pthread_create(&pump_thread, NULL, pump_main, NULL) == 0;
}
static void pump_stop_if_idle(void) {
    pthread_mutex_lock(&irs_lock);
    int idle = thread_live && !irs_core_running(&core);
    if (idle) thread_run = 0;
    pthread_mutex_unlock(&irs_lock);
    if (idle) {
        pthread_join(pump_thread, NULL);
        pthread_mutex_lock(&irs_lock);
        thread_live = 0;
        pthread_mutex_unlock(&irs_lock);
    }
}

static void irs_get_shmem(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!shm) shm = ipc_shared_memory_new(IR_SHM_SIZE);
    ipc_out_copy_handle(c, ipc_shared_memory_handle(shm));
}
static void irs_get_camera_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t h;
    if (irs_camera_handle(IPC_IN(c, uint32_t, 0), &h)) {
        c->result = HZ_RESULT(HZ_MOD_IRSENSOR, 1);
        return;
    } /* [unverified] invalid npad */
    IPC_OUT(c, h);
}
/* ActivateIrsensor takes the aruid (u64); the WithFunctionLevel form takes {u32 level, pad, u64 aruid}. The state is kept so that a title that activates
 * twice or sets the level before running a processor sees the same success as on a console; processors do not require activation here. */
static void irs_activate(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&irs_lock);
    irs_active = 1;
    pthread_mutex_unlock(&irs_lock);
}
static void irs_set_level(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t level = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    pthread_mutex_lock(&irs_lock);
    irs_function_level = level;
    pthread_mutex_unlock(&irs_lock);
}
static void irs_activate_level(IpcObject *s, IpcCall *c) {
    irs_set_level(s, c);
    irs_activate(s, c);
}
static void irs_deactivate(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&irs_lock);
    irs_active = 0;
    ensure();
    for (unsigned i = 0; i < IR_NCAM; i++) irs_core_stop(&core, i);
    pthread_mutex_unlock(&irs_lock);
    pump_stop_if_idle();
}
static unsigned cam_of(const IpcCall *c) { return c->in_size >= 1 ? c->in[0] : 0xff; } /* IrCameraHandle byte 0 = controller id */

/* Common start path. Arguments: IrCameraHandle (4) + pad (4) + aruid (8), then the processor config at offset 16 [unverified]. */
static void run_processor(IpcCall *c, IrMode mode, size_t cfg_size, int with_tmem) {
    unsigned cam = cam_of(c);
    union {
        IrMomentConfig m;
        IrClusteringConfig k;
        IrTransferConfig t;
        IrDpdConfig d;
    } cfg;
    memset(&cfg, 0, sizeof cfg);
    int have = cfg_size && c->in_size >= 16 + cfg_size;
    if (have) memcpy(&cfg, c->in + 16, cfg_size);
    uint8_t *xfer = NULL;
    size_t xsize = 0;
    if (with_tmem) {
        IpcTransferMemory *t = c->ncopy ? handle_get(c->copy[0], OBJ_TMEM) : NULL;
        uint64_t want = c->in_size >= 16 + cfg_size + 8 ? IPC_IN(c, uint64_t, (uint32_t)(16 + cfg_size)) : 0;
        if (t) {
            xfer = guest_ptr(t->addr, t->size);
            xsize = (size_t)(want && want < t->size ? want : t->size);
        }
    }
    pthread_mutex_lock(&irs_lock);
    ensure_source();
    uint32_t r = irs_core_run(&core, cam, mode, have ? &cfg : NULL, xfer, xsize);
    if (r == 0) {
        if (!shm) shm = ipc_shared_memory_new(IR_SHM_SIZE);
        pump_start();
    }
    pthread_mutex_unlock(&irs_lock);
    c->result = r;
}
static void irs_run_moment(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_MOMENT, sizeof(IrMomentConfig), 0);
}
static void irs_run_clustering(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_CLUSTERING, sizeof(IrClusteringConfig), 0);
}
static void irs_run_transfer(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_TRANSFER, sizeof(IrTransferConfig), 1);
}
/* Processors the simulator does not compute (IR LED): accepted while a source exists, producing nothing. */
/* RunImageTransferExProcessor: the same transfer as the plain processor (IrTransferConfig already carries the origin and trimming formats the Ex form adds). */
static void irs_run_transfer_ex(IpcObject *s, IpcCall *c) { irs_run_transfer(s, c); }
static void irs_run_tera(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_TERA, sizeof(IrTeraConfig), 0);
}
static void irs_run_dpd(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_DPD, sizeof(IrDpdConfig), 0);
}
static void irs_run_other(IpcObject *s, IpcCall *c) {
    (void)s;
    run_processor(c, IR_MODE_OTHER, 0, 0);
}
static void irs_stop(IpcObject *s, IpcCall *c) {
    (void)s;
    unsigned cam = cam_of(c);
    pthread_mutex_lock(&irs_lock);
    ensure();
    irs_core_stop(&core, cam);
    pthread_mutex_unlock(&irs_lock);
    pump_stop_if_idle();
}
static void irs_check_firmware(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&irs_lock);
    ensure_source();
    int have = irs_core_has_source(&core);
    pthread_mutex_unlock(&irs_lock);
    if (!have) c->result = HZ_R_IRSENSOR_NOT_CONNECTED;
}
/* GetImageTransferProcessorState: the newest image into the output buffer, {u64 sampling number, u32 ambient noise level, u32 pad} as the result. */
static void irs_get_transfer_state(IpcObject *s, IpcCall *c) {
    (void)s;
    unsigned cam = cam_of(c);
    pthread_mutex_lock(&irs_lock);
    ensure_source();
    if (!irs_core_has_source(&core)) {
        pthread_mutex_unlock(&irs_lock);
        c->result = HZ_R_IRSENSOR_NOT_CONNECTED;
        return;
    }
    IrTransferState st;
    static uint8_t img[IR_W * IR_H];
    const IpcBuf *b = ipc_out_buf(c, 0);
    irs_core_pump(&core, shm ? shm->host : NULL, now_ns());
    long n = irs_core_image(&core, cam, img, sizeof img, &st);
    pthread_mutex_unlock(&irs_lock);
    if (n < 0) {
        c->result = HZ_R_IRSENSOR_NOT_CONNECTED;
        return;
    }
    if (b && n > 0) ipc_write_buf(c, b, img, (uint64_t)n);
    IPC_OUT(c, st);
}
static const IpcCmd irs_cmds[] = {
    {302, "ActivateIrsensor", irs_activate, IPC_IMPL},
    {303, "DeactivateIrsensor", irs_deactivate, IPC_IMPL},
    {304, "GetIrsensorSharedMemoryHandle", irs_get_shmem, IPC_IMPL},
    {305, "StopImageProcessor", irs_stop, IPC_IMPL},
    {306, "RunMomentProcessor", irs_run_moment, IPC_IMPL},
    {307, "RunClusteringProcessor", irs_run_clustering, IPC_IMPL},
    {308, "RunImageTransferProcessor", irs_run_transfer, IPC_IMPL},
    {309, "GetImageTransferProcessorState", irs_get_transfer_state, IPC_IMPL},
    {310, "RunTeraPluginProcessor", irs_run_tera, IPC_IMPL},
    {311, "GetNpadIrCameraHandle", irs_get_camera_handle, IPC_IMPL},
    {312, "RunDpdProcessor", irs_run_dpd, IPC_IMPL},
    {313, "SuspendImageProcessor", irs_stop, IPC_IMPL},
    {314, "CheckFirmwareVersion", irs_check_firmware, IPC_IMPL},
    {315, "SetFunctionLevel", irs_set_level, IPC_IMPL},
    {316, "RunImageTransferExProcessor", irs_run_transfer_ex, IPC_IMPL},
    {317, "RunIrLedProcessor", irs_run_other, IPC_IMPL}, /* LED on: no host-visible effect, accepted while a camera source exists */
    {318, "StopImageProcessorAsync", irs_stop, IPC_IMPL},
    {319, "ActivateIrsensorWithFunctionLevel", irs_activate_level, IPC_IMPL},
};
static IpcObject *irs_open(void) { return IPC_OBJECT("irs", irs_cmds, NULL, NULL); }
void service_register_irs(void) { ipc_register_service("irs", irs_open); }
