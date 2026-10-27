/* bcat:u (nn::bcat): background content delivery. The delivery cache comes from a provider (bcat_core.h: the user's folder via NAPPLE_BCAT_DIR, or a
 * hook a private build installs). Without one there is never a cache: sync requests complete at once with the offline result, directory and file
 * opens report "not found", enumerations are empty. With one, sync requests succeed at once (the data is already local) and directories and files
 * read from it. Name checks and the progress record are real.
 * Command ids and result descriptions are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "bcat_core.h"
#include "hz_result.h"
#include <stdlib.h>
#include <string.h>

static void ret_s32_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
}
static void not_found(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_BCAT_NOT_FOUND;
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
            memset(ent, 0, BCAT_ENTRY_SIZE);
            memcpy(ent, f->name, BCAT_NAME_SIZE);
            memcpy(ent + 0x20, &f->size, 8);
            memcpy(ent + 0x28, f->digest, 16);
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
    {10100, "RequestSyncDeliveryCache", make_progress, IPC_IMPL},
    {10101, "RequestSyncDeliveryCacheWithDirectoryName", make_progress, IPC_IMPL},
    {10200, "CancelSyncDeliveryCacheRequest", svc_noop, IPC_IMPL}, /* the sync already finished: nothing to cancel */
    {20100, "RequestSyncDeliveryCacheWithApplicationId", make_progress, IPC_IMPL},
    {30100, "SetPassphrase", bcat_set_passphrase, IPC_IMPL},
    {30200, "RegisterBackgroundDeliveryTask", svc_noop, IPC_IMPL},
    {30201, "UnregisterBackgroundDeliveryTask", svc_noop, IPC_IMPL},
    {30202, "BlockDeliveryTask", svc_noop, IPC_IMPL},
    {30203, "UnblockDeliveryTask", svc_noop, IPC_IMPL},
    {90100, "EnumerateBackgroundDeliveryTask", ret_s32_0, IPC_IMPL},
    {90201, "ClearDeliveryCacheStorage", svc_noop, IPC_IMPL}, /* nothing of ours to clear: a provider folder is the user's, never deleted */
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
