/* mii:u (nn::mii): Mii database. The database holds one Mii for the local player; the six default Miis are available through the
 * Default source. The database is read-only (editing commands are not provided and fail loudly through the generic unknown-command
 * policy). Command ids and the CharInfo layout are from the author's memory of the public documentation; unverified on a console. */
#include "svc_common.h"
#include "mii_core.h"
#include "host_info.h"
#include "hz_result.h"
#include <stdlib.h>

/* Get(source) / Get1(source): writes as many elements as fit in the output buffer, returns the count written. `with_source`: elements carry the source word. */
static void list_into(IpcCall *c, int with_source) {
    uint32_t flags = IPC_IN(c, uint32_t, 0);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t total = mii_count(flags), written = 0;
    if (b && total) {
        size_t esz = with_source ? sizeof(MiiElement) : sizeof(MiiCharInfo);
        uint32_t cap = (uint32_t)(b->size / esz);
        MiiElement *tmp = calloc(total, sizeof *tmp);
        if (tmp) {
            written = mii_list(flags, tmp, cap < total ? cap : total);
            uint8_t *flat = calloc(written ? written : 1, esz);
            if (flat) {
                for (uint32_t i = 0; i < written; i++) memcpy(flat + i * esz, with_source ? (const void *)&tmp[i] : (const void *)&tmp[i].info, esz);
                ipc_write_buf(c, b, flat, (uint64_t)written * esz);
                free(flat);
            }
            free(tmp);
        }
    }
    int32_t n = (int32_t)written;
    IPC_OUT(c, n);
}
static void db_get(IpcObject *s, IpcCall *c) {
    (void)s;
    list_into(c, 1);
}
static void db_get1(IpcObject *s, IpcCall *c) {
    (void)s;
    list_into(c, 0);
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
static void db_update_latest(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0);
    IPC_OUT(c, m);
} /* nothing is ever newer than the caller's copy */
static void db_find_index(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t i = mii_find_index(ipc_in(c, 0, 16));
    if (i < 0) {
        c->result = HZ_RESULT(HZ_MOD_MII, 4);
        return;
    } /* [unverified] "not found" */
    IPC_OUT(c, i);
}
static void db_get_index(IpcObject *s, IpcCall *c) {
    (void)s;
    MiiCharInfo m = IPC_IN(c, MiiCharInfo, 0);
    int32_t i = mii_find_index(m.create_id);
    if (i < 0) {
        c->result = HZ_RESULT(HZ_MOD_MII, 4);
        return;
    }
    IPC_OUT(c, i);
}
static const IpcCmd db_cmds[] = {
    {0, "IsUpdated", svc_ret_u8_0, IPC_IMPL},
    {1, "IsFullDatabase", svc_ret_u8_0, IPC_IMPL},
    {2, "GetCount", db_get_count, IPC_IMPL},
    {3, "Get", db_get, IPC_IMPL},
    {4, "Get1", db_get1, IPC_IMPL},
    {5, "UpdateLatest", db_update_latest, IPC_IMPL},
    {6, "BuildRandom", db_build_random, IPC_IMPL},
    {7, "BuildDefault", db_build_default, IPC_IMPL},
    {11, "FindIndex", db_find_index, IPC_IMPL},
    {15, "DestroyFile", svc_noop, IPC_STUB},
    {16, "DeleteFile", svc_noop, IPC_STUB},
    {17, "Format", svc_noop, IPC_STUB},
    {20, "IsBrokenDatabaseWithClearFlag", svc_ret_u8_0, IPC_STUB},
    {21, "GetIndex", db_get_index, IPC_IMPL},
    {22, "SetInterfaceVersion", svc_noop, IPC_STUB},
};
static void mii_get_database_service(IpcObject *s, IpcCall *c) {
    (void)s;
    mii_set_user_name(host_user_name());
    ipc_out_object(c, IPC_OBJECT("IDatabaseService", db_cmds, NULL, NULL));
}
static const IpcCmd mii_cmds[] = {{0, "GetDatabaseService", mii_get_database_service, IPC_IMPL}};
static IpcObject *mii_open(void) { return IPC_OBJECT("mii:u", mii_cmds, NULL, NULL); }
void service_register_mii(void) { ipc_register_service("mii:u", mii_open); }
