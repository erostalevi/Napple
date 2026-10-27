/* miiimg (nn::mii image database): the stored face images of the Mii applets. IStaticService command 0 opens the IImageDatabaseService; the database starts empty and
 * keeps nothing (a Mii face image is generated again when asked for). Command ids follow the public documentation; every command is logged so a firmware applet that
 * needs more shows what. */
#include "../ipc.h"
#include <stdlib.h>
#include <string.h>

static void img_log_ok(IpcObject *s, IpcCall *c) {
    (void)s;
    LOGV("mii-img", "command %u (in %u bytes) answered with success", c->cmd, c->in_size);
}
static void img_get_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t n = 0;
    IPC_OUT(c, n);
}
static void img_bool_true(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
}
static void img_bool_false(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void img_not_found(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = 148u | (4u << 9); /* mii module, not found */
}
#define OK(n, name) {n, name, img_log_ok, IPC_STUB}
static const IpcCmd img_cmds[] = {
    OK(0, "Initialize"), OK(1, "Reload"), {2, "GetCount", img_get_count, IPC_IMPL}, {3, "IsEmpty", img_bool_true, IPC_IMPL}, {4, "IsFull", img_bool_false, IPC_IMPL},
    {5, "GetAttribute", img_not_found, IPC_STUB}, {6, "LoadImage", img_not_found, IPC_STUB}, OK(7, "AddOrUpdateImage"), OK(8, "DeleteImages"), OK(9, "DeleteFile"),
    OK(10, "DestroFile"), {11, "GetFreeSlotCount", img_get_count, IPC_STUB}, OK(12, "ExportFile"), OK(13, "ForceInitialize"), OK(14, "Reserved14"), OK(15, "Reserved15"),
};
__attribute__((unused)) static void img_root_get(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IImageDatabaseService", img_cmds, NULL, NULL));
}
static const IpcCmd root_cmds[] = {{0, "GetImageDatabaseService", img_root_get, IPC_IMPL}};
static IpcObject *img_open(void) { (void)root_cmds; return IPC_OBJECT("miiimg", img_cmds, NULL, NULL); } /* the service itself is the image database (Initialize arrives on the root session) */
void service_register_mii_img(void) { ipc_register_service("miiimg", img_open); }
