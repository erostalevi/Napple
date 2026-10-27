/* mii:u and mii:e (nn::mii): the Mii database. mii:u is what titles use (read, build, convert); mii:e adds the editor's commands (add/replace, delete, move, format,
 * import/export). Both hand out the same IDatabaseService over the one console database: the player's Mii plus whatever the Mii editor saved, kept in
 * <save root>/system/mii/MiiDatabase.dat, and the six default Miis the firmware's mii service builds (mii_core.c). Command ids follow the public documentation of
 * nn::mii::IDatabaseService; the layouts of CharInfo and StoreData are checked here only for size, value ranges and round trips (mii_core.c). */
#include "svc_common.h"
#include "mii_core.h"
#include "host_info.h"
#include "../ios_platform.h"
#include "hz_result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define R_MII_NOT_FOUND HZ_RESULT(HZ_MOD_MII, 4)
#define R_MII_INVALID HZ_RESULT(HZ_MOD_MII, 1)
#define R_MII_FULL HZ_RESULT(HZ_MOD_MII, 6)

typedef struct {
    uint32_t seen_generation; /* IsUpdated compares against what this session last listed */
    uint32_t interface_version;
} DbSession;

static uint32_t rc_to_result(int rc) { return rc == MII_OK ? 0 : rc == MII_ERR_NOT_FOUND ? R_MII_NOT_FOUND : rc == MII_ERR_FULL ? R_MII_FULL : R_MII_INVALID; }

/* The database file lives with the other system data under the save root. */
static void db_open_path(void) {
    static int done;
    if (done) return;
    done = 1;
    mii_set_user_name(host_user_name());
    /* One database for every title: NAPPLE_MII_DIR (any folder: an iCloud Drive one included) or <app support>/system/mii, never a per-title save root. */
    char dir[1300], path[1400];
    const char *env = getenv("NAPPLE_MII_DIR");
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
#if NAPPLE_IOS
    else if (ios_shared_mii_dir()) snprintf(dir, sizeof dir, "%s", ios_shared_mii_dir()); /* App Group: the same database in every Napple game app */
#endif
    else if (napple_support_dir() && *napple_support_dir()) {
        snprintf(dir, sizeof dir, "%s/system", napple_support_dir());
        mkdir(dir, 0755);
        snprintf(dir, sizeof dir, "%s/system/mii", napple_support_dir());
    } else return; /* tests and tools without a save root keep it in memory */
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/MiiDatabase.dat", dir);
    mii_db_set_path(path);
}

/* Get / Get1 / Get2 / Get3: writes as many elements as fit in the output buffer, returns the count written. */
enum { LIST_ELEMENT, LIST_CHAR_INFO, LIST_STORE_ELEMENT, LIST_STORE };
static void list_into(IpcObject *s, IpcCall *c, int kind) {
    DbSession *ds = s->state;
    uint32_t flags = IPC_IN(c, uint32_t, 0);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t total = mii_count(flags), written = 0;
    size_t esz = kind == LIST_ELEMENT ? sizeof(MiiElement) : kind == LIST_CHAR_INFO ? sizeof(MiiCharInfo) : kind == LIST_STORE_ELEMENT ? sizeof(MiiStoreElement) : sizeof(MiiStoreData);
    if (b && total) {
        uint32_t cap = (uint32_t)(b->size / esz);
        MiiElement *tmp = calloc(total, sizeof *tmp);
        if (tmp) {
            written = mii_list(flags, tmp, cap < total ? cap : total);
            uint8_t *flat = calloc(written ? written : 1, esz);
            if (flat) {
                for (uint32_t i = 0; i < written; i++) {
                    uint8_t *dst = flat + i * esz;
                    if (kind == LIST_ELEMENT) memcpy(dst, &tmp[i], esz);
                    else if (kind == LIST_CHAR_INFO) memcpy(dst, &tmp[i].info, esz);
                    else {
                        MiiStoreData st;
                        mii_store_from_char_info(&tmp[i].info, &st);
                        if (kind == LIST_STORE) memcpy(dst, &st, esz);
                        else {
                            MiiStoreElement se = {st, tmp[i].source};
                            memcpy(dst, &se, esz);
                        }
                    }
                }
                ipc_write_buf(c, b, flat, (uint64_t)written * esz);
                free(flat);
            }
            free(tmp);
        }
    }
    if (ds) ds->seen_generation = mii_db_generation();
    LOG("mii", "list kind %d flags %#x: output buffer %llu bytes (%llu elements of %zu), wrote %u, interface version %u", kind, flags, b ? (unsigned long long)b->size : 0ull,
        b ? (unsigned long long)(b->size / esz) : 0ull, esz, written, ds ? ds->interface_version : 0);
    int32_t n = (int32_t)written;
    IPC_OUT(c, n);
}
static void db_get(IpcObject *s, IpcCall *c) { list_into(s, c, LIST_ELEMENT); }
static void db_get1(IpcObject *s, IpcCall *c) { list_into(s, c, LIST_CHAR_INFO); }
static void db_get2(IpcObject *s, IpcCall *c) { list_into(s, c, LIST_STORE_ELEMENT); }
static void db_get3(IpcObject *s, IpcCall *c) { list_into(s, c, LIST_STORE); }
static void db_is_updated(IpcObject *s, IpcCall *c) {
    DbSession *ds = s->state;
    uint8_t v = ds && ds->seen_generation != mii_db_generation();
    IPC_OUT(c, v);
}
static void db_is_full(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = mii_db_count() >= MII_DB_MAX;
    IPC_OUT(c, v);
}
static void db_get_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t n = (int32_t)mii_count(IPC_IN(c, uint32_t, 0));
    IPC_OUT(c, n);
}
static void db_build_default(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m;
    mii_build_default(IPC_IN(c, uint32_t, 0), &m);
    IPC_OUT(c, m);
}
static void db_build_random(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m;
    mii_build_random(IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4), IPC_IN(c, uint32_t, 8), &m);
    IPC_OUT(c, m);
}
/* UpdateLatest(CharInfo, SourceFlag): the newest version of that Mii in the requested sources. A Mii that is in none of them comes back unchanged (the caller keeps its copy). */
static int latest_of(const uint8_t id[16], uint32_t flags, MiiCharInfo *out) {
    MiiStoreData st;
    int i;
    if ((flags & MII_FLAG_DATABASE) && (i = mii_db_find(id)) >= 0 && mii_db_get((uint32_t)i, &st) == MII_OK) {
        mii_char_info_from_store(&st, out);
        return 1;
    }
    if (flags & MII_FLAG_DEFAULT)
        for (uint32_t d = 0; d < MII_DEFAULT_COUNT; d++) {
            mii_build_default(d, out);
            if (!memcmp(out->create_id, id, 16)) return 1;
        }
    return 0;
}
static void db_update_latest(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0), latest;
    if (latest_of(m.create_id, c->in_size >= sizeof m + 4 ? IPC_IN(c, uint32_t, sizeof m) : 3, &latest)) m = latest;
    IPC_OUT(c, m);
}
static void db_update_latest1(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiStoreData st = IPC_IN(c, MiiStoreData, 0);
    MiiCharInfo latest;
    if (latest_of(st.create_id, c->in_size >= sizeof st + 4 ? IPC_IN(c, uint32_t, sizeof st) : 3, &latest)) mii_store_from_char_info(&latest, &st);
    IPC_OUT(c, st);
}
static void db_find_index(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t i = mii_find_index(ipc_in(c, 0, 16));
    if (i < 0) {
        c->result = R_MII_NOT_FOUND;
        return;
    }
    IPC_OUT(c, i);
}
static void db_get_index(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0);
    int32_t i = mii_find_index(m.create_id);
    if (i < 0) {
        c->result = R_MII_NOT_FOUND;
        return;
    }
    IPC_OUT(c, i);
}
static void db_set_interface_version(IpcObject *s, IpcCall *c) {
    DbSession *ds = s->state;
    if (ds && c->in_size >= 4) ds->interface_version = IPC_IN(c, uint32_t, 0);
}
static void db_broken(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)mii_db_take_broken();
    IPC_OUT(c, v);
}
/* conversions */
static void db_convert_core_to_char_info(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCoreData core = IPC_IN(c, MiiCoreData, 0);
    MiiCharInfo m;
    mii_char_info_from_core(&core, NULL, &m);
    IPC_OUT(c, m);
}
static void db_convert_char_info_to_core(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0);
    MiiCoreData core;
    mii_core_from_char_info(&m, &core);
    IPC_OUT(c, core);
}
/* editor commands (mii:e) */
static void db_add_or_replace(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiStoreData st = IPC_IN(c, MiiStoreData, 0);
    c->result = rc_to_result(mii_db_add_or_replace(&st));
}
static void db_append(IpcObject *s, IpcCall *c) { /* Append(CharInfo): the editor adds a Mii as it stands */
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0);
    MiiStoreData st;
    mii_store_from_char_info(&m, &st);
    c->result = rc_to_result(mii_db_add_or_replace(&st));
}
static void db_delete(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = rc_to_result(mii_db_delete(ipc_in(c, 0, 16)));
}
static void db_move(IpcObject *s, IpcCall *c) { /* Move(s32 index, CreateId) */
    (void)s;
    int32_t index = IPC_IN(c, int32_t, 0);
    c->result = index < 0 ? R_MII_INVALID : rc_to_result(mii_db_move(ipc_in(c, 4, 16), (uint32_t)index));
}
static void db_format(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    mii_db_format();
}
static void db_destroy_file(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)mii_db_destroy_file();
    (void)c;
}
static void db_export(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t tmp[16 + MII_DB_MAX * sizeof(MiiStoreData)];
    size_t n = 0;
    if (!b || mii_db_export(tmp, sizeof tmp, &n) != MII_OK || b->size < n) {
        c->result = R_MII_INVALID;
        return;
    }
    ipc_write_buf(c, b, tmp, n);
}
static void db_import(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    c->result = p ? rc_to_result(mii_db_import(p, (size_t)b->size)) : R_MII_INVALID;
}

#define READ_CMDS                                                                                                                                                  \
    {0, "IsUpdated", db_is_updated, IPC_IMPL}, {1, "IsFullDatabase", db_is_full, IPC_IMPL}, {2, "GetCount", db_get_count, IPC_IMPL}, {3, "Get", db_get, IPC_IMPL},    \
        {4, "Get1", db_get1, IPC_IMPL}, {5, "UpdateLatest", db_update_latest, IPC_IMPL}, {6, "BuildRandom", db_build_random, IPC_IMPL},                                \
        {7, "BuildDefault", db_build_default, IPC_IMPL}, {8, "Get2", db_get2, IPC_IMPL}, {9, "Get3", db_get3, IPC_IMPL}, {10, "UpdateLatest1", db_update_latest1, IPC_IMPL}, \
        {11, "FindIndex", db_find_index, IPC_IMPL}, {21, "GetIndex", db_get_index, IPC_IMPL}, {22, "SetInterfaceVersion", db_set_interface_version, IPC_IMPL},        \
        {24, "ConvertCoreDataToCharInfo", db_convert_core_to_char_info, IPC_IMPL}, {25, "ConvertCharInfoToCoreData", db_convert_char_info_to_core, IPC_IMPL},
static const IpcCmd user_cmds[] = {
    READ_CMDS{20, "IsBrokenDatabaseWithClearFlag", db_broken, IPC_IMPL},
};
static const IpcCmd editor_cmds[] = {
    READ_CMDS{12, "Move", db_move, IPC_IMPL},
    {13, "AddOrReplace", db_add_or_replace, IPC_IMPL},
    {14, "Delete", db_delete, IPC_IMPL},
    {15, "DestroyFile", db_destroy_file, IPC_IMPL},
    {16, "DeleteFile", db_destroy_file, IPC_IMPL},
    {17, "Format", db_format, IPC_IMPL},
    {18, "Import", db_import, IPC_IMPL},
    {19, "Export", db_export, IPC_IMPL},
    {20, "IsBrokenDatabaseWithClearFlag", db_broken, IPC_IMPL},
    {26, "Append", db_append, IPC_IMPL},
};
static IpcObject *new_session(const IpcCmd *cmds, size_t n, const char *iface) {
    DbSession *ds = calloc(1, sizeof *ds);
    ds->seen_generation = mii_db_generation();
    return ipc_object_new(iface, cmds, n, ds, NULL);
}
static void mii_get_database_service(IpcObject *s, IpcCall *c) {
    db_open_path();
    int editor = s->state != NULL;
    ipc_out_object(c, editor ? new_session(editor_cmds, sizeof editor_cmds / sizeof *editor_cmds, "IDatabaseService(mii:e)") : new_session(user_cmds, sizeof user_cmds / sizeof *user_cmds, "IDatabaseService"));
}
static const IpcCmd mii_cmds[] = {{0, "GetDatabaseService", mii_get_database_service, IPC_IMPL}};
static IpcObject *mii_open(void) { return IPC_OBJECT("mii:u", mii_cmds, NULL, NULL); }
static IpcObject *mii_e_open(void) { return ipc_object_new("mii:e", mii_cmds, sizeof mii_cmds / sizeof *mii_cmds, (void *)1, NULL); }
void service_register_mii(void) {
    ipc_register_service("mii:u", mii_open);
    ipc_register_service("mii:e", mii_e_open);
}
