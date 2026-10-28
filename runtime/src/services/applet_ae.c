/* appletAE: the system-applet side of the applet manager (nn::ae), what a firmware applet process (MiiEdit, swkbd, controller, ... run from the user's firmware
 * as a program of its own) connects to. The process is the applet; the application that started it is another process of the host (the "firmware applet
 * host", docs/changes/2026-10-26-mii-firmware.md): it exchanges the library applet storages with this one through files.
 *   NAPPLE_APPLET_IN   <dir>: in0.bin, in1.bin ... the storages the caller pushed (common arguments first), popped in order by PopInData
 *   NAPPLE_APPLET_OUT  <dir>: out0.bin ... the storages the applet pushed, and result.txt (the exit result) written when the process ends
 *   NAPPLE_APPLET_ID / NAPPLE_APPLET_MODE: GetLibraryAppletInfo (default 0x12 MiiEdit, 0 AllForeground)
 * The application's own storages are those of applet.c (shared objects below). */
#include "../ipc.h"
#include "../frontend.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern uint32_t g_applet_kind;
IpcObject *applet_shared_object(const char *which);
IpcObject *applet_storage_make(const void *data, uint64_t size);
int applet_storage_peek(const IpcObject *o, const uint8_t **data, uint64_t *size);

extern void applet_push_message(uint32_t message);
extern void applet_push_message(uint32_t message);
static pthread_mutex_t ae_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned in_next, out_count, iin_next, iout_count;
static uint32_t terminate_result; /* IAppletCommonFunctions::SetTerminateResult: what ExitProcessAndReturn reports to the caller */

static const char *dir_env(const char *name) { const char *d = getenv(name); return d && *d ? d : NULL; }
static uint8_t *read_file(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1); if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)) { fclose(f); free(b); return NULL; }
    fclose(f); *size = (uint64_t)n; return b;
}
static unsigned in_count(void) {
    const char *d = dir_env("NAPPLE_APPLET_IN"); unsigned n = 0; char p[1300];
    while (d) { snprintf(p, sizeof p, "%s/in%u.bin", d, n); struct stat st; if (stat(p, &st)) break; n++; }
    return n;
}
static unsigned iin_count(void) {
    const char *d = dir_env("NAPPLE_APPLET_IN"); unsigned n = 0; char p[1300];
    while (d) { snprintf(p, sizeof p, "%s/iin%u.bin", d, n); struct stat st; if (stat(p, &st)) break; n++; }
    return n;
}
static uint32_t plain_ev(int signaled) { uint32_t r = 0, w = event_create(&r); if (signaled) event_signal_handle(w); return r; }

/* ---- ILibraryAppletSelfAccessor ---------------------------------------------------------------------------------------------------------- */
static void sa_pop_in(IpcObject *s, IpcCall *c) {
    (void)s; const char *d = dir_env("NAPPLE_APPLET_IN"); char p[1300]; uint64_t n = 0;
    pthread_mutex_lock(&ae_lock); unsigned i = in_next; pthread_mutex_unlock(&ae_lock);
    unsigned first = i == 0;
    snprintf(p, sizeof p, "%s/in%u.bin", d ? d : ".", i);
    uint8_t *b = d ? read_file(p, &n) : NULL;
    if (b && i == 0 && n >= 8 && n < 0x100 && ((uint32_t *)b)[0] == 1 && ((uint32_t *)b)[1] == n) { /* the caller's LibAppletArgs (common arguments): the Mii Maker's SDK
                                                                                                        reads only its own 0x100-byte argument, the first storage it pops */
        LOG("applet-ae", "PopInData: skipping the caller's common arguments (%llu bytes)", (unsigned long long)n);
        free(b); b = NULL; i = 1;
        snprintf(p, sizeof p, "%s/in%u.bin", d, i);
        b = read_file(p, &n);
    }
    if (!d && i == 0) { /* started on its own (no caller): MiiEdit's input storage with version 3 and mode 0 (ShowMiiEdit) */
        b = calloc(1, 0x100);
        b[0] = 3;
        n = 0x100;
    }
    if (!b) { LOG("applet-ae", "PopInData: no input storage %u", i); c->result = 0x680; return; }   /* applet 128 desc 3: no data */
    pthread_mutex_lock(&ae_lock); in_next = i + 1; pthread_mutex_unlock(&ae_lock);
    if (first) applet_push_message(1); /* what the system tells a library applet once it was started: ChangeIntoForeground (the SDK brings its UI up on it) */
    LOG("applet-ae", "PopInData: storage %u, %llu bytes (first words %08x %08x %08x %08x)", i, (unsigned long long)n,
        n >= 4 ? *(uint32_t *)b : 0, n >= 8 ? *(uint32_t *)(b + 4) : 0, n >= 12 ? *(uint32_t *)(b + 8) : 0, n >= 16 ? *(uint32_t *)(b + 12) : 0);
    ipc_out_object(c, applet_storage_make(b, n)); free(b);
}
static void sa_push_out(IpcObject *s, IpcCall *c) {
    (void)s; const uint8_t *data; uint64_t n;
    if (!c->nin_objects || !applet_storage_peek(c->in_objects[0], &data, &n)) { c->result = 0x1480; return; }
    const char *d = dir_env("NAPPLE_APPLET_OUT"); char p[1300];
    pthread_mutex_lock(&ae_lock); unsigned i = out_count++; pthread_mutex_unlock(&ae_lock);
    LOG("applet-ae", "PushOutData: storage %u, %llu bytes", i, (unsigned long long)n);
    if (!d) return;
    mkdir(d, 0755); snprintf(p, sizeof p, "%s/out%u.bin", d, i);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(data, 1, (size_t)n, f); fclose(f); } else LOG("applet-ae", "cannot write %s", p);
}
/* The interactive channel: the caller's PushInteractiveInData storages are iin<N>.bin, the applet's PushInteractiveOutData ones become iout<N>.bin. */
static void sa_pop_interactive_in(IpcObject *s, IpcCall *c) {
    (void)s; const char *d = dir_env("NAPPLE_APPLET_IN"); char p[1300]; uint64_t n = 0;
    pthread_mutex_lock(&ae_lock); unsigned i = iin_next; pthread_mutex_unlock(&ae_lock);
    snprintf(p, sizeof p, "%s/iin%u.bin", d ? d : ".", i);
    uint8_t *b = d ? read_file(p, &n) : NULL;
    if (!b) { c->result = 0x680; return; }
    pthread_mutex_lock(&ae_lock); iin_next = i + 1; pthread_mutex_unlock(&ae_lock);
    LOG("applet-ae", "PopInteractiveInData: storage %u, %llu bytes", i, (unsigned long long)n);
    ipc_out_object(c, applet_storage_make(b, n)); free(b);
}
static void sa_push_interactive_out(IpcObject *s, IpcCall *c) {
    (void)s; const uint8_t *data; uint64_t n;
    if (!c->nin_objects || !applet_storage_peek(c->in_objects[0], &data, &n)) { c->result = 0x1480; return; }
    const char *d = dir_env("NAPPLE_APPLET_OUT"); char p[1300];
    pthread_mutex_lock(&ae_lock); unsigned i = iout_count++; pthread_mutex_unlock(&ae_lock);
    LOG("applet-ae", "PushInteractiveOutData: storage %u, %llu bytes", i, (unsigned long long)n);
    if (!d) return;
    mkdir(d, 0755); snprintf(p, sizeof p, "%s/iout%u.bin", d, i);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(data, 1, (size_t)n, f); fclose(f); } else LOG("applet-ae", "cannot write %s", p);
}
static void sa_pop_in_event(IpcObject *s, IpcCall *c) { (void)s; ipc_out_copy_handle(c, plain_ev(in_next < in_count())); }
static void sa_pop_inter_event(IpcObject *s, IpcCall *c) { (void)s; ipc_out_copy_handle(c, plain_ev(iin_next < iin_count())); }
static void sa_exit_and_return(IpcObject *s, IpcCall *c) {
    (void)s; (void)c; LOG("applet-ae", "ExitProcessAndReturn: the applet is done (%u output storage(s))", out_count);
    const char *d = dir_env("NAPPLE_APPLET_OUT");
    if (d) { char p[1300]; mkdir(d, 0755); snprintf(p, sizeof p, "%s/result.txt", d); FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%u\n", terminate_result); fclose(f); } }
    summary_note("applet exited with %u output storage(s)", out_count);
    GUEST_EXIT(0); /* the system ends the applet's process when it returns (here: the child process, or this image's guest in library mode) */
}
static void sa_info(IpcObject *s, IpcCall *c) {
    (void)s; const char *id = getenv("NAPPLE_APPLET_ID"), *mode = getenv("NAPPLE_APPLET_MODE");
    uint32_t v[2] = {id ? (uint32_t)strtoul(id, NULL, 0) : 0x12, mode ? (uint32_t)strtoul(mode, NULL, 0) : 0}; ipc_out(c, v, 8);
}
static void sa_can_use_app_core(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static void sa_caller_identity(IpcObject *s, IpcCall *c) { (void)s; uint8_t v[0x18] = {0}; uint64_t app = strtoull(getenv("NAPPLE_APPLET_CALLER") ? getenv("NAPPLE_APPLET_CALLER") : "0", NULL, 16); memcpy(v, &app, 8); ipc_out(c, v, sizeof v); }
static void sa_storage_id(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 4; IPC_OUT(c, v); }   /* NCM storage id: NAND user */
static const IpcCmd sa_cmds[] = {
    {0, "PopInData", sa_pop_in, IPC_IMPL}, {1, "PushOutData", sa_push_out, IPC_IMPL}, {2, "PopInteractiveInData", sa_pop_interactive_in, IPC_IMPL},
    {3, "PushInteractiveOutData", sa_push_interactive_out, IPC_IMPL}, {5, "GetPopInDataEvent", sa_pop_in_event, IPC_IMPL}, {6, "GetPopInteractiveInDataEvent", sa_pop_inter_event, IPC_IMPL},
    {10, "ExitProcessAndReturn", sa_exit_and_return, IPC_IMPL}, {11, "GetLibraryAppletInfo", sa_info, IPC_IMPL}, {12, "GetMainAppletIdentityInfo", sa_caller_identity, IPC_IMPL},
    {13, "CanUseApplicationCore", sa_can_use_app_core, IPC_IMPL}, {14, "GetCallerAppletIdentityInfo", sa_caller_identity, IPC_IMPL},
    {16, "GetMainAppletStorageId", sa_storage_id, IPC_IMPL},
};

/* ---- IProcessWindingController (library applet launch reason) ---------------------------------------------------------------------------- */
static void pw_launch_reason(IpcObject *s, IpcCall *c) { (void)s; uint8_t v[4] = {0}; ipc_out(c, v, 4); }
static const IpcCmd pw_cmds[] = {{0, "GetLaunchReason", pw_launch_reason, IPC_IMPL}};

/* ---- IAppletCommonFunctions ---------------------------------------------------------------------------------------------------------------- */
/* Accepted silently: with no Home menu process the debug message has no receiver (see csg_noop in applet.c). */
static void acf_noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void acf_no_data(IpcObject *s, IpcCall *c) { (void)s; c->result = 0x680; }
static void acf_set_terminate_result(IpcObject *s, IpcCall *c) {
    (void)s; if (c->in_size >= 4) { terminate_result = IPC_IN(c, uint32_t, 0); LOG("applet-ae", "SetTerminateResult %#x", terminate_result); }
}
/* The applet theme storage (the home menu's persisted theme blob): kept in memory for the life of the process. */
static uint8_t theme_blob[0x1000]; static size_t theme_len;
static void acf_read_theme(IpcObject *s, IpcCall *c) {
    (void)s; const IpcBuf *b = ipc_out_buf(c, 0);
    if (!theme_len || !b) { c->result = 0x680; return; }
    size_t n = b->size < theme_len ? b->size : theme_len; ipc_write_buf(c, b, theme_blob, n);
}
static void acf_write_theme(IpcObject *s, IpcCall *c) {
    (void)s; const IpcBuf *b = ipc_in_buf(c, 0); const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    if (!p) { c->result = 0x1d80; return; }
    theme_len = b->size < sizeof theme_blob ? b->size : sizeof theme_blob; memcpy(theme_blob, p, theme_len);
}
/* TryPopFromAppletBoundChannel: no channel data is ever queued for the applet, so the queue reads empty (applet module 128 desc 3). */
static const IpcCmd acf_cmds[] = {{0, "SetTerminateResult", acf_set_terminate_result, IPC_IMPL}, {10, "ReadThemeStorage", acf_read_theme, IPC_IMPL}, {11, "WriteThemeStorage", acf_write_theme, IPC_IMPL}, {21, "TryPopFromAppletBoundChannel", acf_no_data, IPC_IMPL}};

/* ---- ILibraryAppletProxy ------------------------------------------------------------------------------------------------------------------- */
#define SHARED(fn, key) static void fn(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, applet_shared_object(key)); }
SHARED(lp_common, "common") SHARED(lp_self, "self") SHARED(lp_window, "window") SHARED(lp_audio, "audio") SHARED(lp_display, "display") SHARED(lp_creator, "creator")
static void lp_winding(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IProcessWindingController", pw_cmds, NULL, NULL)); }
static void lp_self_accessor(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ILibraryAppletSelfAccessor", sa_cmds, NULL, NULL)); }
static void lp_common_functions(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IAppletCommonFunctions", acf_cmds, NULL, NULL)); }
/* Kept stub: there is no home menu to notify. */
static const IpcCmd lp_debug_cmds[] = {{0, "NotifyMessageToHomeMenuForDebug", acf_noop, IPC_IMPL}};
static void lp_debug(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IDebugFunctions", lp_debug_cmds, NULL, NULL)); }
static const IpcCmd lp_cmds[] = {
    {0, "GetCommonStateGetter", lp_common, IPC_IMPL}, {1, "GetSelfController", lp_self, IPC_IMPL}, {2, "GetWindowController", lp_window, IPC_IMPL},
    {3, "GetAudioController", lp_audio, IPC_IMPL}, {4, "GetDisplayController", lp_display, IPC_IMPL}, {10, "GetProcessWindingController", lp_winding, IPC_IMPL},
    {11, "GetLibraryAppletCreator", lp_creator, IPC_IMPL}, {20, "OpenLibraryAppletSelfAccessor", lp_self_accessor, IPC_IMPL},
    {21, "GetAppletCommonFunctions", lp_common_functions, IPC_IMPL}, {1000, "GetDebugFunctions", lp_debug, IPC_IMPL},
};

/* ---- appletAE (IAllSystemAppletProxiesService) ------------------------------------------------------------------------------------------- */
/* Experiment aid: NAPPLE_APPLET_MSGS="16,15" pushes those applet messages half a second after the proxy opened (what a home menu would send to bring the applet up). */
extern void applet_push_message(uint32_t message);
static void *delayed_messages(void *arg) {
    (void)arg;
    usleep(500000);
    for (const char *t = getenv("NAPPLE_APPLET_MSGS"); t && *t;) {
        applet_push_message((uint32_t)strtoul(t, (char **)&t, 0));
        LOG("applet-ae", "pushed an applet message");
        if (*t == ',') t++;
    }
    return NULL;
}
static void ae_open_library_proxy(IpcObject *s, IpcCall *c) {
    (void)s; g_applet_kind = 3;
    if (getenv("NAPPLE_APPLET_MSGS")) {
        pthread_t th;
        pthread_create(&th, NULL, delayed_messages, NULL);
        pthread_detach(th);
    }                                             /* LibraryApplet */
    LOG("applet-ae", "OpenLibraryAppletProxy (applet id %s)", getenv("NAPPLE_APPLET_ID") ? getenv("NAPPLE_APPLET_ID") : "0x12");
    ipc_out_object(c, IPC_OBJECT("ILibraryAppletProxy", lp_cmds, NULL, NULL));
}
static const IpcCmd ae_cmds[] = {{200, "OpenLibraryAppletProxyOld", ae_open_library_proxy, IPC_IMPL}, {201, "OpenLibraryAppletProxy", ae_open_library_proxy, IPC_IMPL}};
static IpcObject *ae_open(void) { return IPC_OBJECT("appletAE", ae_cmds, NULL, NULL); }
void service_register_applet_ae(void) { ipc_register_service("appletAE", ae_open); }

/* fatal:u (nn::fatal): a process throws a fatal error. The firmware applets call it when their own checks fail; the host stops the run and names the result code. */
static void fatal_throw(IpcObject *s, IpcCall *c) {
    (void)s; uint32_t r = IPC_IN(c, uint32_t, 0);
    LOG("fatal", "ThrowFatal(cmd %u): result %#x (2%03u-%04u): the guest process gave up", c->cmd, r, r & 0x1ff, (r >> 9) & 0x1fff);
    { extern void ipc_trace_guest(unsigned); ipc_trace_guest(24); }
    runtime_fail("fatal error thrown by the guest: result %#x (module %u description %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
}
static const IpcCmd fatal_cmds[] = {{0, "ThrowFatal", fatal_throw, IPC_IMPL}, {1, "ThrowFatalWithPolicy", fatal_throw, IPC_IMPL}, {2, "ThrowFatalWithCpuContext", fatal_throw, IPC_IMPL}};
static IpcObject *fatal_open(void) { return IPC_OBJECT("fatal:u", fatal_cmds, NULL, NULL); }
void service_register_fatal(void) { ipc_register_service("fatal:u", fatal_open); }

/* hid:sys and other system-only services a firmware applet opens but only pokes at: catch-all stubs (ipc.c: an object without commands) until a command matters. */
static IpcObject *stub_object(const char *name) { return ipc_object_new(name, NULL, 0, NULL, NULL); }
extern void hid_apply_system_common_policy(void);
static void hid_sys_policy(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    hid_apply_system_common_policy();
}
static const IpcCmd hid_sys_cmds[] = {{303, "ApplyNpadSystemCommonPolicy", hid_sys_policy, IPC_IMPL}, {306, "ApplyNpadSystemCommonPolicyFull", hid_sys_policy, IPC_IMPL}};
static IpcObject *hid_sys_open(void) { return IPC_OBJECT("hid:sys", hid_sys_cmds, NULL, NULL); }
/* Kept as catch-all objects until a firmware applet calls into them: ldn:m, ldn:s, lp2p:m, ngct:u. (nfc:sys / nfp:sys are real: offline.c answers CreateSystemInterface with the
 * same simulated NFC objects as the user services; miiimg is registered by mii_img.c.) */
#define STUB_FACTORY(n, name) static IpcObject *n(void) { return stub_object(name); }
STUB_FACTORY(sf2, "ldn:m") STUB_FACTORY(sf3, "ldn:s") STUB_FACTORY(sf4, "lp2p:m") STUB_FACTORY(sf5, "ngct:u")
static IpcObject *(*const stub_factory[])(void) = {sf2, sf3, sf4, sf5};
void service_register_firmware_stubs(void) {
    ipc_register_service("hid:sys", hid_sys_open);
    static const char *const names[] = {"ldn:m", "ldn:s", "lp2p:m", "ngct:u"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) ipc_register_service(names[i], stub_factory[i]);
}

/* The NVN driver a firmware applet carries is replaced by the host's (module.c hooks: its stripped nvnBootstrapLoader jumps here). The applet's own, newer NVN
 * loader also asks for extension entry points (nvnSyncInitializeFromNvRmSyncNVX, ...) the host's table does not have: those get a stub that returns 0, logged,
 * so the applet's init goes on instead of reading a null function pointer. */
static void *(*real_getproc)(void *, const char *);
static uintptr_t nvn_ext_stub(void) {
    LOG("applet-ae", "an NVN extension entry point the host does not implement was called (returned 0)");
    return 0;
}
/* nvnSyncInitializeFromNvRmSyncNVX(sync, ...): a sync made from the system compositor's fence. There is no compositor: such a sync counts as signalled, so the applet's
 * waits on it (nvnSyncWait, nvnQueueWaitSync, nvnCommandBufferWaitSync) return at once. */
static void *nvrm_syncs[64];
static unsigned nnvrm_syncs;
static int is_nvrm_sync(void *p) {
    for (unsigned i = 0; i < nnvrm_syncs; i++)
        if (nvrm_syncs[i] == p) return 1;
    return 0;
}
static uint8_t nvx_sync_init(void *sync) {
    if (nnvrm_syncs < 64) nvrm_syncs[nnvrm_syncs++] = sync;
    LOG("applet-ae", "nvnSyncInitializeFromNvRmSyncNVX(%p): treated as signalled", sync);
    return 1;
}
static void *(*real_sync_wait)(void *, uint64_t);
static int wrap_sync_wait(void *sync, uint64_t timeout) {
    LOGV("nvn", "nvnSyncWait(%p, %llu)%s", sync, (unsigned long long)timeout, is_nvrm_sync(sync) ? " [nvrm: signalled]" : "");
    if (is_nvrm_sync(sync)) return 0; /* AlreadySignaled */
    /* A fence the host cannot complete within the applet's own timeout (the first Metal pipeline compiles are slow) is waited for to the end instead: the applet has no
     * path for "the GPU did not answer". NAPPLE_NVN_SYNC_TIMEOUT_KEEP=1 keeps the applet's timeout. */
    if (!getenv("NAPPLE_NVN_SYNC_TIMEOUT_KEEP")) timeout = UINT64_MAX;
    return ((int (*)(void *, uint64_t))real_sync_wait)(sync, timeout);
}
static void (*real_queue_wait_sync)(void *, void *);
static void wrap_queue_wait_sync(void *queue, void *sync) {
    if (!is_nvrm_sync(sync)) real_queue_wait_sync(queue, sync);
}
static void (*real_cb_wait_sync)(void *, void *);
static void wrap_cb_wait_sync(void *cb, void *sync) {
    if (!is_nvrm_sync(sync)) real_cb_wait_sync(cb, sync);
}
/* NAPPLE_NVN_TRACE=1: log the queue/sync calls the applet makes (a probe for waits that never end). */
static void (*real_fence)(void *, void *, int, int);
static void wrap_fence(void *q, void *sync, int cond, int flags) {
    LOG("nvn", "nvnQueueFenceSync(queue %p, sync %p, cond %d, flags %#x)", q, sync, cond, flags);
    real_fence(q, sync, cond, flags);
}
static void (*real_flush)(void *);
static void wrap_flush(void *q) {
    LOG("nvn", "nvnQueueFlush(queue %p)", q);
    real_flush(q);
}
static int (*real_sync_init)(void *, void *);
static int wrap_sync_init(void *sync, void *dev) {
    LOG("nvn", "nvnSyncInitialize(sync %p, device %p)", sync, dev);
    return real_sync_init(sync, dev);
}
static void (*real_submit)(void *, int, void *const *);
static void wrap_submit(void *q, int n, void *const *handles) {
    LOG("nvn", "nvnQueueSubmitCommands(queue %p, %d lists)", q, n);
    real_submit(q, n, handles);
}
static int (*real_qws)(void *, void *);
static int wrap_qws(void *q, void *sync) {
    LOG("nvn", "nvnQueueWaitSync(queue %p, sync %p)%s", q, sync, is_nvrm_sync(sync) ? " [nvrm: skipped]" : "");
    return is_nvrm_sync(sync) ? 1 : real_qws(q, sync);
}
static int (*real_acq)(void *, void *, int *);
static int wrap_acq(void *win, void *sync, int *idx) {
    int r = real_acq(win, sync, idx);
    LOG("nvn", "nvnWindowAcquireTexture(window %p, sync %p) -> %d index %d", win, sync, r, idx ? *idx : -1);
    return r;
}
static void (*real_present)(void *, void *, int);
static void wrap_present(void *q, void *win, int idx) {
    LOG("nvn", "nvnQueuePresentTexture(queue %p, window %p, index %d)", q, win, idx);
    real_present(q, win, idx);
}
/* NAPPLE_NVN_FAILTRACE=1: every "...Initialize" / "...SetShaders" / "...Allocate" NVN call of the applet is wrapped (32 trampolines) and one that answers false is logged with its name:
 * a probe for a graphics object the host refused to create. Arguments pass through in x0..x5. */
typedef uintptr_t (*nvn_fn6)(void *, void *, void *, void *, void *, void *);
static struct {
    nvn_fn6 real;
    char name[48];
} fail_slots[32];
static unsigned nfail_slots;
#define FAILTRAMP(i)                                                                                                                                                  \
    static uintptr_t ft##i(void *a, void *b, void *c, void *d, void *e, void *f) {                                                                                  \
        uintptr_t r = fail_slots[i].real(a, b, c, d, e, f);                                                                                                         \
        if (!(r & 0xff)) LOG("nvn", "%s(%p, %p, %p, %p) returned false", fail_slots[i].name, a, b, c, d);                                                           \
        return r;                                                                                                                                                   \
    }
FAILTRAMP(0) FAILTRAMP(1) FAILTRAMP(2) FAILTRAMP(3) FAILTRAMP(4) FAILTRAMP(5) FAILTRAMP(6) FAILTRAMP(7) FAILTRAMP(8) FAILTRAMP(9) FAILTRAMP(10) FAILTRAMP(11) FAILTRAMP(12) FAILTRAMP(13) FAILTRAMP(14) FAILTRAMP(15) FAILTRAMP(16) FAILTRAMP(17) FAILTRAMP(18) FAILTRAMP(19) FAILTRAMP(20) FAILTRAMP(21) FAILTRAMP(22) FAILTRAMP(23) FAILTRAMP(24) FAILTRAMP(25) FAILTRAMP(26) FAILTRAMP(27) FAILTRAMP(28) FAILTRAMP(29) FAILTRAMP(30) FAILTRAMP(31)
static nvn_fn6 const fail_tramps[32] = {ft0, ft1, ft2, ft3, ft4, ft5, ft6, ft7, ft8, ft9, ft10, ft11, ft12, ft13, ft14, ft15, ft16, ft17, ft18, ft19, ft20, ft21, ft22, ft23, ft24, ft25, ft26, ft27, ft28, ft29, ft30, ft31};
static void *fail_wrap(void *real, const char *name) {
    if (!real || nfail_slots >= 32) return real;
    unsigned i = nfail_slots++;
    fail_slots[i].real = real;
    snprintf(fail_slots[i].name, sizeof fail_slots[i].name, "%s", name);
    return (void *)fail_tramps[i];
}
static void *wrap_getproc(void *device, const char *name) {
    if (name && getenv("NAPPLE_NVN_FAILTRACE") && (strstr(name, "Initialize") || strstr(name, "SetShaders") || strstr(name, "Allocate")) && strcmp(name, "nvnDeviceInitialize")) {
        void *r = real_getproc(device, name);
        if (r) return fail_wrap(r, name);
    }
    if (getenv("NAPPLE_NVN_TRACE") && name) {
        if (!strcmp(name, "nvnQueueWaitSync") && (real_qws = real_getproc(device, name))) return (void *)wrap_qws;
        if (!strcmp(name, "nvnWindowAcquireTexture") && (real_acq = real_getproc(device, name))) return (void *)wrap_acq;
        if (!strcmp(name, "nvnQueuePresentTexture") && (real_present = real_getproc(device, name))) return (void *)wrap_present;
        if (!strcmp(name, "nvnQueueFenceSync") && (real_fence = real_getproc(device, name))) return (void *)wrap_fence;
        if (!strcmp(name, "nvnQueueFlush") && (real_flush = real_getproc(device, name))) return (void *)wrap_flush;
        if (!strcmp(name, "nvnSyncInitialize") && (real_sync_init = real_getproc(device, name))) return (void *)wrap_sync_init;
        if (!strcmp(name, "nvnQueueSubmitCommands") && (real_submit = real_getproc(device, name))) return (void *)wrap_submit;
    }
    LOGV("applet-ae", "getproc(%s)", name ? name : "(null)");
    if (name && !strcmp(name, "nvnDeviceGetProcAddress")) return (void *)wrap_getproc; /* later lookups through the table entry stay wrapped */
    if (name && !strcmp(name, "nvnSyncInitializeFromNvRmSyncNVX")) return (void *)nvx_sync_init;
    if (name && !strcmp(name, "nvnSyncWait")) {
        real_sync_wait = real_getproc(device, name);
        return real_sync_wait ? (void *)wrap_sync_wait : NULL;
    }
    if (name && !strcmp(name, "nvnQueueWaitSync")) {
        real_queue_wait_sync = real_getproc(device, name);
        return real_queue_wait_sync ? (void *)wrap_queue_wait_sync : NULL;
    }
    if (name && !strcmp(name, "nvnCommandBufferWaitSync")) {
        real_cb_wait_sync = real_getproc(device, name);
        return real_cb_wait_sync ? (void *)wrap_cb_wait_sync : NULL;
    }
    if (name && strstr(name, "NVX")) { /* vendor extensions: the host has none */
        LOG("applet-ae", "NVN extension %s is not implemented by the host: stub", name);
        return (void *)nvn_ext_stub;
    }
    void *p = real_getproc(device, name);
    if (p) return p;
    LOG("applet-ae", "NVN extension %s is not implemented by the host: stub", name ? name : "(null)");
    return (void *)nvn_ext_stub;
}
static void *applet_nvn_bootstrap(const char *name) {
    void *(*real)(const char *) = host_symbol_lookup("nvnBootstrapLoader");
    void *p = real ? real(name) : NULL;
    LOGV("applet-ae", "nvnBootstrapLoader(\"%s\") -> %p", name ? name : "(null)", p);
    if (p && name && !strcmp(name, "nvnDeviceGetProcAddress")) {
        real_getproc = p;
        return (void *)wrap_getproc;
    }
    if (!p && name && !strncmp(name, "nvn", 3)) {
        LOG("applet-ae", "NVN extension %s is not implemented by the host: stub", name);
        return (void *)nvn_ext_stub;
    }
    return p;
}
static const HostSymbol applet_syms[] = {{"napple_applet_nvn_bootstrap", (void *)applet_nvn_bootstrap}};
__attribute__((constructor)) static void applet_register_hook_symbols(void) { host_symbols_register(applet_syms, 1); }
