/* miiimg (nn::mii image database): the stored face images of the Mii applets. IStaticService command 0 opens the IImageDatabaseService; the database lives in memory (mii_img_core.c): a title stores JPEGs, and an image it did not
 * supply is generated from the Mii's id. Command ids follow the public documentation; every command is logged so a firmware applet that
 * needs more shows what. */
#include "../ipc.h"
#include "mii_img_core.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static MiiImgDb db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
#define MIIIMG_R_NOT_FOUND (148u | (4u << 9)) /* mii module, not found */
#define MIIIMG_R_FULL (148u | (6u << 9))      /* [unverified] database full */
/* The database starts empty on the first call of any command (the title may skip Initialize). Caller holds db_lock. */
static MiiImgDb *img_db(void) {
    if (!db.initialized) miiimg_init(&db);
    return &db;
}
static uint32_t arg_u32(const IpcCall *c) { return c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0xffffffffu; }
static void img_init(IpcObject *s, IpcCall *c) { /* 0 Initialize, 1 Reload: nothing is stored on disk, so there is nothing to (re)load */
    (void)s;
    (void)c;
    pthread_mutex_lock(&db_lock);
    img_db();
    pthread_mutex_unlock(&db_lock);
}
static void img_reset(IpcObject *s, IpcCall *c) { /* 9 DeleteFile, 10 DestroFile, 13 ForceInitialize: the file is gone and the database empty again */
    (void)s;
    (void)c;
    pthread_mutex_lock(&db_lock);
    miiimg_init(&db);
    pthread_mutex_unlock(&db_lock);
}
static void img_ok(IpcObject *s, IpcCall *c) { /* 12 ExportFile and the reserved rows: there is no file to export (the database is memory only) */
    (void)s;
    LOGV("mii-img", "command %u (in %u bytes) answered with success", c->cmd, c->in_size);
}
static void img_get_count(IpcObject *s, IpcCall *c) { /* 2 GetCount -> s32 */
    (void)s;
    pthread_mutex_lock(&db_lock);
    int32_t n = (int32_t)miiimg_count(img_db());
    pthread_mutex_unlock(&db_lock);
    IPC_OUT(c, n);
}
static void img_free_slots(IpcObject *s, IpcCall *c) { /* 11 GetFreeSlotCount -> s32 */
    (void)s;
    pthread_mutex_lock(&db_lock);
    int32_t n = (int32_t)miiimg_free_slots(img_db());
    pthread_mutex_unlock(&db_lock);
    IPC_OUT(c, n);
}
static void img_is_empty(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&db_lock);
    uint8_t v = miiimg_count(img_db()) == 0;
    pthread_mutex_unlock(&db_lock);
    IPC_OUT(c, v);
}
static void img_is_full(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&db_lock);
    uint8_t v = miiimg_free_slots(img_db()) == 0;
    pthread_mutex_unlock(&db_lock);
    IPC_OUT(c, v);
}
static void img_get_attribute(IpcObject *s, IpcCall *c) { /* 5 GetAttribute(u32 index) -> attribute record (0x20 bytes) */
    (void)s;
    pthread_mutex_lock(&db_lock);
    MiiImgDb *d = img_db();
    uint32_t i = arg_u32(c);
    uint8_t a[MIIIMG_ATTR_SIZE];
    int ok = i < d->count;
    if (ok) miiimg_attribute(&d->e[i], a);
    pthread_mutex_unlock(&db_lock);
    if (!ok) {
        c->result = MIIIMG_R_NOT_FOUND;
        return;
    }
    ipc_out(c, a, sizeof a);
}
static void img_load_image(IpcObject *s, IpcCall *c) { /* 6 LoadImage(u32 index) -> u32 size, JPEG in buffer 0 (a short buffer gets a prefix) */
    (void)s;
    pthread_mutex_lock(&db_lock);
    MiiImgDb *d = img_db();
    uint32_t i = arg_u32(c), n = 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    int ok = i < d->count;
    if (ok && b) n = (uint32_t)ipc_write_buf(c, b, d->e[i].data, d->e[i].size);
    else if (ok) n = d->e[i].size;
    pthread_mutex_unlock(&db_lock);
    if (!ok) {
        c->result = MIIIMG_R_NOT_FOUND;
        return;
    }
    IPC_OUT(c, n);
}
/* 7 AddOrUpdateImage(u8 unique id[16]) with the JPEG in input buffer 0; without an image buffer the host generates a face image from the id (miiimg_generate). */
static void img_add(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16) {
        c->result = MIIIMG_R_NOT_FOUND;
        return;
    }
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    pthread_mutex_lock(&db_lock);
    int idx = miiimg_add(img_db(), ipc_in(c, 0, 16), p, p ? (size_t)b->size : 0);
    pthread_mutex_unlock(&db_lock);
    if (idx < 0) c->result = MIIIMG_R_FULL;
}
static void img_delete(IpcObject *s, IpcCall *c) { /* 8 DeleteImages(u32 index) */
    (void)s;
    pthread_mutex_lock(&db_lock);
    int r = miiimg_delete_index(img_db(), arg_u32(c));
    pthread_mutex_unlock(&db_lock);
    if (r) c->result = MIIIMG_R_NOT_FOUND;
}
static const IpcCmd img_cmds[] = {
    {0, "Initialize", img_init, IPC_IMPL},      {1, "Reload", img_init, IPC_IMPL},
    {2, "GetCount", img_get_count, IPC_IMPL},   {3, "IsEmpty", img_is_empty, IPC_IMPL},
    {4, "IsFull", img_is_full, IPC_IMPL},       {5, "GetAttribute", img_get_attribute, IPC_IMPL},
    {6, "LoadImage", img_load_image, IPC_IMPL}, {7, "AddOrUpdateImage", img_add, IPC_IMPL},
    {8, "DeleteImages", img_delete, IPC_IMPL},  {9, "DeleteFile", img_reset, IPC_IMPL},
    {10, "DestroFile", img_reset, IPC_IMPL},    {11, "GetFreeSlotCount", img_free_slots, IPC_IMPL},
    {12, "ExportFile", img_ok, IPC_IMPL},       {13, "ForceInitialize", img_reset, IPC_IMPL},
    {14, "Reserved14", img_ok, IPC_IMPL},       {15, "Reserved15", img_ok, IPC_IMPL},
};
__attribute__((unused)) static void img_root_get(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IImageDatabaseService", img_cmds, NULL, NULL));
}
static const IpcCmd root_cmds[] = {{0, "GetImageDatabaseService", img_root_get, IPC_IMPL}};
static IpcObject *img_open(void) {
    (void)root_cmds;
    return IPC_OBJECT("miiimg", img_cmds, NULL, NULL);
} /* the service itself is the image database (Initialize arrives on the root session) */
void service_register_mii_img(void) { ipc_register_service("miiimg", img_open); }
