/* bcat:u (nn::bcat): background content delivery. The delivery cache comes from a provider (bcat_core.h: the user's folder via NAPPLE_BCAT_DIR, or a
 * hook a private build installs). Without one there is never a cache: sync requests complete at once with the offline result, directory and file
 * opens report "not found", enumerations are empty. With one, sync requests succeed at once (the data is already local) and directories and files
 * read from it. Name checks and the progress record are real.
 * Command ids and result descriptions are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "bcat_core.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "sys_default.h"

static void ret_s32_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
}
static uint32_t bad_result(void) { return HZ_RESULT(HZ_MOD_BCAT, 2); } /* [unverified] invalid argument */
static void bad_name(IpcCall *c) { c->result = bad_result(); }

/* IDeliveryCacheProgressService: the sync "ran" and failed because there is no network. */
static void prog_get_event(IpcObject *s, IpcCall *c) { svc_ret_event_signaled(s, c); }
static void prog_get_impl(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t rec[BCAT_PROGRESS_SIZE];
    bcat_make_progress(BCAT_STATUS_DONE, bcat_provider() ? 0 : HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, rec);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, rec, sizeof rec);
}
/* GetEvent is the real behaviour: the sync finished (failed offline) before the guest could wait, so the event is signalled at once. */
static const IpcCmd progress_cmds[] = {{0, "GetEvent", prog_get_event, IPC_IMPL}, {1, "GetImpl", prog_get_impl, IPC_IMPL}};
static void make_progress(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IDeliveryCacheProgressService", progress_cmds, NULL, NULL));
}

/* directory and file services over the provider. Open of a name the provider does not hold is "not found" (the only answer without a provider), and a
 * directory or file object that was never opened reports zero entries / "not found", as for an empty cache. */
typedef struct {
    int open, cursor, n;
    char name[BCAT_NAME_SIZE];
    BcatFile files[BCAT_MAX_FILES];
} CacheDir;
typedef struct {
    int open;
    char dir[BCAT_NAME_SIZE], file[BCAT_NAME_SIZE];
    BcatFile info;
} CacheFile;
static void dir_open(IpcObject *s, IpcCall *c) {
    CacheDir *d = s->state;
    const char *name = ipc_in(c, 0, BCAT_NAME_SIZE);
    if (!bcat_valid_directory_name(name)) {
        bad_name(c);
        return;
    }
    const BcatProvider *p = bcat_provider();
    int n = p && p->list_files ? p->list_files(p->ctx, name, d->files, BCAT_MAX_FILES) : -1;
    if (n < 0) {
        c->result = HZ_R_BCAT_NOT_FOUND;
        return;
    }
    d->open = 1;
    d->n = n;
    d->cursor = 0;
    memcpy(d->name, name, BCAT_NAME_SIZE);
}
static void dir_read(IpcObject *s, IpcCall *c) { /* out array of {char name[0x20]; u64 size; u8 digest[16]} */
    CacheDir *d = s->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    int32_t got = 0;
    if (d->open && b) {
        uint8_t ents[BCAT_MAX_FILES * BCAT_ENTRY_SIZE];
        while (d->cursor < d->n && (uint64_t)(got + 1) * BCAT_ENTRY_SIZE <= b->size) {
            const BcatFile *f = &d->files[d->cursor++];
            uint8_t *ent = ents + (size_t)got * BCAT_ENTRY_SIZE;
            bcat_pack_entry(f, ent);
            got++;
        }
        if (got) ipc_write_buf(c, b, ents, (uint64_t)got * BCAT_ENTRY_SIZE);
    }
    IPC_OUT(c, got);
}
static void dir_count(IpcObject *s, IpcCall *c) {
    CacheDir *d = s->state;
    int32_t v = d->open ? d->n : 0;
    IPC_OUT(c, v);
}
static void dir_close(IpcObject *s, IpcCall *c) {
    (void)c;
    ((CacheDir *)s->state)->open = 0;
}
static const IpcCmd dir_cmds[] = {
    {0, "Open", dir_open, IPC_IMPL}, {1, "Read", dir_read, IPC_IMPL}, {2, "GetCount", dir_count, IPC_IMPL}, {3, "Close", dir_close, IPC_IMPL}};
static void file_open(IpcObject *s, IpcCall *c) {
    CacheFile *f = s->state;
    const char *dir = ipc_in(c, 0, BCAT_NAME_SIZE), *file = ipc_in(c, BCAT_NAME_SIZE, BCAT_NAME_SIZE);
    if (!bcat_valid_directory_name(dir) || !bcat_valid_file_name(file)) {
        bad_name(c);
        return;
    }
    const BcatProvider *p = bcat_provider();
    static BcatFile list[BCAT_MAX_FILES]; /* serialized by the single service thread */
    int n = p && p->list_files ? p->list_files(p->ctx, dir, list, BCAT_MAX_FILES) : -1;
    for (int i = 0; i < n; i++)
        if (!strncmp(list[i].name, file, BCAT_NAME_SIZE)) {
            f->open = 1;
            f->info = list[i];
            memcpy(f->dir, dir, BCAT_NAME_SIZE);
            memcpy(f->file, file, BCAT_NAME_SIZE);
            return;
        }
    c->result = HZ_R_BCAT_NOT_FOUND;
}
static void file_read(IpcObject *s, IpcCall *c) { /* in: s64 offset; out buffer; result s64 bytes read */
    CacheFile *f = s->state;
    const BcatProvider *p = bcat_provider();
    const IpcBuf *b = ipc_out_buf(c, 0);
    int64_t off = IPC_IN(c, int64_t, 0);
    if (!f->open || !p || !p->read || off < 0) {
        c->result = f->open ? bad_result() : HZ_R_BCAT_NOT_FOUND;
        return;
    }
    int64_t got = 0;
    if (b && b->size && (uint64_t)off < f->info.size) {
        uint64_t want = b->size < f->info.size - (uint64_t)off ? b->size : f->info.size - (uint64_t)off;
        uint8_t *tmp = malloc((size_t)want);
        got = tmp ? p->read(p->ctx, f->dir, f->file, (uint64_t)off, tmp, (size_t)want) : -1;
        if (got > 0) ipc_write_buf(c, b, tmp, (uint64_t)got);
        free(tmp);
        if (got < 0) {
            c->result = HZ_R_BCAT_NOT_FOUND;
            return;
        }
    }
    IPC_OUT(c, got);
}
static void file_size(IpcObject *s, IpcCall *c) {
    CacheFile *f = s->state;
    if (!f->open) {
        c->result = HZ_R_BCAT_NOT_FOUND;
        return;
    }
    int64_t v = (int64_t)f->info.size;
    IPC_OUT(c, v);
}
static void file_digest(IpcObject *s, IpcCall *c) {
    CacheFile *f = s->state;
    if (!f->open) {
        c->result = HZ_R_BCAT_NOT_FOUND;
        return;
    }
    uint8_t d[16];
    memcpy(d, f->info.digest, 16);
    IPC_OUT(c, d);
}
static const IpcCmd file_cmds[] = {
    {0, "Open", file_open, IPC_IMPL}, {1, "Read", file_read, IPC_IMPL}, {2, "GetSize", file_size, IPC_IMPL}, {3, "GetDigest", file_digest, IPC_IMPL}};
static void free_state(IpcObject *o) { free(o->state); }
static void make_dir_service(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IDeliveryCacheDirectoryService", dir_cmds, calloc(1, sizeof(CacheDir)), free_state));
}
static void make_file_service(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IDeliveryCacheFileService", file_cmds, calloc(1, sizeof(CacheFile)), free_state));
}
static void enumerate_dirs(IpcObject *s, IpcCall *c) { /* out array of 0x20-byte names; result s32 count */
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    const BcatProvider *p = bcat_provider();
    int32_t n = 0;
    if (b && p && p->list_dirs) {
        char names[BCAT_MAX_DIRS][BCAT_NAME_SIZE];
        int max = (int)(b->size / BCAT_NAME_SIZE);
        if (max > BCAT_MAX_DIRS) max = BCAT_MAX_DIRS;
        n = max > 0 ? p->list_dirs(p->ctx, names, max) : 0;
        if (n < 0) n = 0;
        if (n) ipc_write_buf(c, b, names, (uint64_t)n * BCAT_NAME_SIZE);
    }
    IPC_OUT(c, n);
}
static const IpcCmd storage_cmds[] = {
    {0, "CreateFileService", make_file_service, IPC_IMPL},
    {1, "CreateDirectoryService", make_dir_service, IPC_IMPL},
    {10, "EnumerateDeliveryCacheDirectory", enumerate_dirs, IPC_IMPL},
};
static void make_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IDeliveryCacheStorageService", storage_cmds, NULL, NULL));
}

/* ---- delivery task bookkeeping and storage notifications (ipc-depth-95 r10) ----
 * Layouts from switchbrew "BCAT services": GetDeliveryCacheStorageUpdateNotifier(u64 ApplicationId) -> INotifierService { 0 GetEvent },
 * RequestSuspendDeliveryTask(u64 ApplicationId) -> IDeliveryTaskSuspensionService { 0 GetEvent }, Register/UnregisterSystemApplicationDeliveryTask
 * (u64 ApplicationId, no output), GetDeliveryList(u64 [3.0.0+], type-0x6 buffer) -> 8 bytes. The process has one delivery cache (the provider), so the
 * application ids select nothing beyond it. */
static pthread_mutex_t g_bcat_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t in_app_id(IpcCall *c) { return c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0; }

/* INotifierService: its event fires when a sync finds the delivery cache changed since the notifier last looked (bcat_storage_fingerprint before and after;
 * the provider's data is already local, so a sync "downloads" exactly the difference). Without a provider nothing ever changes and the event never fires.
 * Each GetEvent hands out a new handle on the notifier's event; a change seen while none is out is kept until the next GetEvent. */
#define BCAT_MAX_NOTIFIERS 16
#define BCAT_NOTIFIER_EVENTS 4
typedef struct {
    uint64_t app, seen;
    uint32_t ev[BCAT_NOTIFIER_EVENTS];
    unsigned nev;
    int pending;
} BcatNotifier;
static BcatNotifier *g_notifiers[BCAT_MAX_NOTIFIERS];
static void storage_synced(void) { /* every sync request: a changed storage wakes the notifiers */
    uint64_t fp = bcat_storage_fingerprint(bcat_provider());
    pthread_mutex_lock(&g_bcat_mu);
    for (int i = 0; i < BCAT_MAX_NOTIFIERS; i++) {
        BcatNotifier *n = g_notifiers[i];
        if (!n || n->seen == fp) continue;
        n->seen = fp;
        if (!n->nev) n->pending = 1;
        for (unsigned k = 0; k < n->nev; k++) event_signal_handle(n->ev[k]);
    }
    pthread_mutex_unlock(&g_bcat_mu);
}
static void notifier_get_event(IpcObject *s, IpcCall *c) {
    BcatNotifier *n = s->state;
    uint32_t r = 0, w = event_create(&r);
    pthread_mutex_lock(&g_bcat_mu);
    if (n->nev < BCAT_NOTIFIER_EVENTS) n->ev[n->nev++] = w;
    if (n->pending) {
        n->pending = 0;
        event_signal_handle(w);
    }
    pthread_mutex_unlock(&g_bcat_mu);
    ipc_out_copy_handle(c, r);
}
static const IpcCmd notifier_cmds[] = {{0, "GetEvent", notifier_get_event, IPC_IMPL}};
static void notifier_destroy(IpcObject *o) {
    pthread_mutex_lock(&g_bcat_mu);
    for (int i = 0; i < BCAT_MAX_NOTIFIERS; i++)
        if (g_notifiers[i] == o->state) g_notifiers[i] = NULL;
    pthread_mutex_unlock(&g_bcat_mu);
    free(o->state);
}
static void bcat_get_update_notifier(IpcObject *s, IpcCall *c) {
    (void)s;
    BcatNotifier *n = calloc(1, sizeof *n);
    if (!n) {
        c->result = bad_result();
        return;
    }
    n->app = in_app_id(c);
    n->seen = bcat_storage_fingerprint(bcat_provider());
    pthread_mutex_lock(&g_bcat_mu);
    int slot = -1;
    for (int i = 0; i < BCAT_MAX_NOTIFIERS && slot < 0; i++)
        if (!g_notifiers[i]) slot = i;
    if (slot >= 0) g_notifiers[slot] = n;
    pthread_mutex_unlock(&g_bcat_mu);
    if (slot < 0) { /* [unverified] napple's bound: more live notifiers than a title has use for */
        free(n);
        c->result = bad_result();
        return;
    }
    ipc_out_object(c, IPC_OBJECT("INotifierService", notifier_cmds, n, notifier_destroy));
}

/* IDeliveryTaskSuspensionService: while it is open the application's background delivery task is suspended (a count per application id, as several can be
 * open). GetEvent is taken to report that the suspension is in effect [unverified meaning]: no background transfer ever runs here (offline nothing is
 * fetched, a provider's data is local), so there is nothing to wait for and it is signalled at once. */
#define BCAT_MAX_SUSPENDED 16
static struct {
    uint64_t app;
    unsigned count;
} g_suspended[BCAT_MAX_SUSPENDED];
__attribute__((unused)) static unsigned bcat_suspend_count(uint64_t app) { /* the open suspensions of an application (tests, diagnostics) */
    unsigned n = 0;
    pthread_mutex_lock(&g_bcat_mu);
    for (int i = 0; i < BCAT_MAX_SUSPENDED; i++)
        if (g_suspended[i].count && g_suspended[i].app == app) n = g_suspended[i].count;
    pthread_mutex_unlock(&g_bcat_mu);
    return n;
}
static int suspend_change(uint64_t app, int delta) {
    int ok = 0;
    pthread_mutex_lock(&g_bcat_mu);
    int slot = -1;
    for (int i = 0; i < BCAT_MAX_SUSPENDED && slot < 0; i++)
        if (g_suspended[i].count && g_suspended[i].app == app) slot = i;
    if (slot < 0 && delta > 0)
        for (int i = 0; i < BCAT_MAX_SUSPENDED && slot < 0; i++)
            if (!g_suspended[i].count) slot = i, g_suspended[i].app = app;
    if (slot >= 0) {
        g_suspended[slot].count = (unsigned)((int)g_suspended[slot].count + delta);
        ok = 1;
    }
    pthread_mutex_unlock(&g_bcat_mu);
    return ok;
}
static void suspension_get_event(IpcObject *s, IpcCall *c) { svc_ret_event_signaled(s, c); }
static const IpcCmd suspension_cmds[] = {{0, "GetEvent", suspension_get_event, IPC_IMPL}};
static void suspension_destroy(IpcObject *o) {
    suspend_change(*(uint64_t *)o->state, -1);
    free(o->state);
}
static void bcat_request_suspend(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t *app = malloc(sizeof *app);
    if (!app || !suspend_change(*app = in_app_id(c), 1)) {
        free(app);
        c->result = bad_result(); /* [unverified] napple's bound on applications suspended at once */
        return;
    }
    ipc_out_object(c, IPC_OBJECT("IDeliveryTaskSuspensionService", suspension_cmds, app, suspension_destroy));
}

/* System-application delivery tasks: the registered application ids (Register is idempotent, Unregister of an id that is not registered succeeds). Nothing
 * is ever scheduled for them (no network; the provider is not a server), and the task list's entry layout is not public, so they are only kept. */
#define BCAT_MAX_SYSAPP_TASKS 32
static uint64_t g_sysapp_tasks[BCAT_MAX_SYSAPP_TASKS];
static unsigned g_nsysapp_tasks;
static int bcat_sysapp_registered(uint64_t app) {
    int found = 0;
    pthread_mutex_lock(&g_bcat_mu);
    for (unsigned i = 0; i < g_nsysapp_tasks; i++)
        if (g_sysapp_tasks[i] == app) found = 1;
    pthread_mutex_unlock(&g_bcat_mu);
    return found;
}
static void bcat_register_sysapp_task(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t app = in_app_id(c);
    if (bcat_sysapp_registered(app)) return;
    pthread_mutex_lock(&g_bcat_mu);
    if (g_nsysapp_tasks < BCAT_MAX_SYSAPP_TASKS) g_sysapp_tasks[g_nsysapp_tasks++] = app;
    else c->result = bad_result(); /* [unverified] napple's bound */
    pthread_mutex_unlock(&g_bcat_mu);
}
static void bcat_unregister_sysapp_task(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t app = in_app_id(c);
    pthread_mutex_lock(&g_bcat_mu);
    for (unsigned i = 0; i < g_nsysapp_tasks; i++)
        if (g_sysapp_tasks[i] == app) g_sysapp_tasks[i--] = g_sysapp_tasks[--g_nsysapp_tasks];
    pthread_mutex_unlock(&g_bcat_mu);
}

/* GetDeliveryList(u64 ApplicationId, type-0x6 buffer) -> u64: the delivery list (msgpack) the last sync downloaded, from the provider (list.msgpack). The
 * output is taken to be the s64 size written [unverified: the page gives "4 bytes, 8 on 3.0.0+"]. A console that never synced (no provider, or a provider
 * without a list) has no list: not found. */
static void bcat_get_delivery_list(IpcObject *s, IpcCall *c) {
    (void)s;
    const BcatProvider *p = bcat_provider();
    const IpcBuf *b = ipc_out_buf(c, 0);
    size_t cap = b ? (size_t)b->size : 0;
    uint8_t *tmp = cap ? malloc(cap) : NULL;
    int64_t total = p && p->read_list ? p->read_list(p->ctx, tmp, tmp ? cap : 0) : -1;
    if (total < 0) {
        free(tmp);
        c->result = HZ_R_BCAT_NOT_FOUND;
        return;
    }
    uint64_t written = 0;
    if (tmp && total) written = ipc_write_buf(c, b, tmp, (uint64_t)total < cap ? (uint64_t)total : cap);
    free(tmp);
    IPC_OUT(c, written);
}

/* A sync request: the progress object, then the storage-update check. */
static void request_sync(IpcObject *s, IpcCall *c) {
    make_progress(s, c);
    storage_synced();
}

/* SetPassphrase(u64 application id; in buffer: passphrase, at most 0x40 characters plus the NUL): the passphrase only matters to a
 * delivery against the server, so it is validated and dropped. */
static void bcat_set_passphrase(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (!b || !bcat_valid_passphrase_size(b->size)) bad_name(c);
}
/* Background delivery tasks: nothing is ever scheduled (no network), so register/unregister/block/unblock have no effect to record, and the
 * task list is empty. Documented no-ops: the console with delivery disabled answers success to the same calls. */
static const IpcCmd bcat_service_cmds[] = {
    {10100, "RequestSyncDeliveryCache", request_sync, IPC_IMPL},
    {10101, "RequestSyncDeliveryCacheWithDirectoryName", request_sync, IPC_IMPL},
    {10200, "CancelSyncDeliveryCacheRequest", svc_noop, IPC_IMPL}, /* the sync already finished: nothing to cancel */
    {20100, "RequestSyncDeliveryCacheWithApplicationId", request_sync, IPC_IMPL},
    {30100, "SetPassphrase", bcat_set_passphrase, IPC_IMPL},
    {30200, "RegisterDeliveryTask", svc_noop, IPC_IMPL},
    {30201, "UnregisterDeliveryTask", svc_noop, IPC_IMPL},
    {30202, "BlockDeliveryTask", svc_noop, IPC_IMPL},
    {30203, "UnblockDeliveryTask", svc_noop, IPC_IMPL},
    {90100, "GetDeliveryTaskList", ret_s32_0, IPC_IMPL},
    {90201, "ClearDeliveryCacheStorage", svc_noop, IPC_IMPL}, /* nothing of ours to clear: a provider folder is the user's, never deleted */
    /* 20101 (u64 application id, DirectoryName): the same sync as 10101, from the provider; the console's system variant names the application explicitly */
    {20101, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName", request_sync, IPC_IMPL},
    {20300, "GetDeliveryCacheStorageUpdateNotifier", bcat_get_update_notifier, IPC_IMPL},
    {20301, "RequestSuspendDeliveryTask", bcat_request_suspend, IPC_IMPL},
    {20400, "RegisterSystemApplicationDeliveryTask", bcat_register_sysapp_task, IPC_IMPL},
    {20401, "UnregisterSystemApplicationDeliveryTask", bcat_unregister_sysapp_task, IPC_IMPL},
    /* No public layout (switchbrew lists names only) for 20410, 20500, 30210, 30300, 90202, 90300 and 90301: documented defaults. */
    {20410, "SetSystemApplicationDeliveryTaskTimer", sysd_ok, IPC_STUB},
    {20500, "StopBackgroundService", sysd_ok, IPC_STUB},
    {30210, "SetDeliveryTaskTimer", sysd_ok, IPC_STUB},
    {30300, "RegisterSystemApplicationDeliveryTasks", sysd_ok, IPC_STUB},
    {90200, "GetDeliveryList", bcat_get_delivery_list, IPC_IMPL},
    {90202, "ClearDeliveryTaskSubscriptionStatus", sysd_ok, IPC_STUB},
    {90300, "GetPushNotificationLog", sysd_err_net, IPC_STUB},
    {90301, "GetDeliveryCacheStorageUsage", sysd_err_net, IPC_STUB},
};
static void make_bcat_service(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IBcatService", bcat_service_cmds, NULL, NULL));
}
static const IpcCmd bcat_cmds[] = {
    {0, "CreateBcatService", make_bcat_service, IPC_IMPL},
    {1, "CreateDeliveryCacheStorageService", make_storage, IPC_IMPL},
    {2, "CreateDeliveryCacheStorageServiceWithApplicationId", make_storage, IPC_IMPL},
    {3, "CreateDeliveryCacheProgressService", make_progress, IPC_IMPL},
    {4, "CreateDeliveryCacheProgressServiceWithApplicationId", make_progress, IPC_IMPL},
};
static IpcObject *bcat_open(void) { return IPC_OBJECT("bcat:u", bcat_cmds, NULL, NULL); }
void service_register_bcat(void) { ipc_register_service("bcat:u", bcat_open); }
