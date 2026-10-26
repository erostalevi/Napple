/* set:fd (nn::settings firmware-debug settings): items the guest sets are kept in memory for the life of the process and can be reset or
 * enumerated through the key iterator; nothing is persisted and set:sys does not read them back. Command ids are from the author's
 * memory of the public documentation (unverified). */
#include "svc_common.h"
#include "setfd_core.h"
#include "hz_result.h"
#include <stdlib.h>

static SetfdStore store;
#define R_BAD_ARG HZ_RESULT(HZ_MOD_SETTINGS, 102) /* [unverified] invalid argument */

static int names(IpcCall *c, char name[SETFD_NAME_MAX], char key[SETFD_NAME_MAX]) {
    const IpcBuf *n = ipc_in_buf(c, 0), *k = ipc_in_buf(c, 1);
    const void *np = n ? ipc_buf_ptr(n) : NULL, *kp = k ? ipc_buf_ptr(k) : NULL;
    return np && kp && !setfd_copy_string(name, SETFD_NAME_MAX, np, (size_t)n->size) && !setfd_copy_string(key, SETFD_NAME_MAX, kp, (size_t)k->size);
}
static void fd_set_item(IpcObject *s, IpcCall *c) {
    (void)s;
    char name[SETFD_NAME_MAX], key[SETFD_NAME_MAX];
    const IpcBuf *v = ipc_in_buf(c, 2);
    const void *vp = v ? ipc_buf_ptr(v) : NULL;
    if (!names(c, name, key) || !vp) {
        c->result = R_BAD_ARG;
        return;
    }
    if (setfd_set(&store, name, key, vp, (size_t)v->size)) c->result = R_BAD_ARG;
}
static void fd_reset_item(IpcObject *s, IpcCall *c) {
    (void)s;
    char name[SETFD_NAME_MAX], key[SETFD_NAME_MAX];
    if (!names(c, name, key)) {
        c->result = R_BAD_ARG;
        return;
    }
    setfd_reset(&store, name, key);
}

typedef struct {
    char name[SETFD_NAME_MAX];
    unsigned index;
} Iter;
static void free_state(IpcObject *o) { free(o->state); }
static void it_go_next(IpcObject *s, IpcCall *c) {
    (void)c;
    Iter *it = s->state;
    it->index++;
}
static void it_key_size(IpcObject *s, IpcCall *c) {
    Iter *it = s->state;
    const char *k = setfd_nth_key(&store, it->name, it->index);
    if (!k) {
        c->result = HZ_RESULT(HZ_MOD_SETTINGS, 101);
        return;
    } /* [unverified] end of iteration */
    uint64_t n = strlen(k) + 1;
    IPC_OUT(c, n);
}
static void it_get_key(IpcObject *s, IpcCall *c) {
    Iter *it = s->state;
    const char *k = setfd_nth_key(&store, it->name, it->index);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!k) {
        c->result = HZ_RESULT(HZ_MOD_SETTINGS, 101);
        return;
    }
    if (b) ipc_write_buf(c, b, k, strlen(k) + 1);
}
static const IpcCmd iter_cmds[] = {{0, "GoNext", it_go_next, IPC_IMPL}, {1, "GetKeySize", it_key_size, IPC_IMPL}, {2, "GetKey", it_get_key, IPC_IMPL}};
static void fd_create_iterator(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *n = ipc_in_buf(c, 0);
    const void *np = n ? ipc_buf_ptr(n) : NULL;
    Iter *it = calloc(1, sizeof *it);
    if (!np || setfd_copy_string(it->name, sizeof it->name, np, (size_t)n->size)) {
        free(it);
        c->result = R_BAD_ARG;
        return;
    }
    ipc_out_object(c, IPC_OBJECT("IFirmwareDebugSettingsItemKeyIterator", iter_cmds, it, free_state));
}
static const IpcCmd fd_cmds[] = {
    {2, "SetSettingsItemValue", fd_set_item, IPC_IMPL},
    {3, "ResetSettingsItemValue", fd_reset_item, IPC_IMPL},
    {4, "CreateSettingsItemKeyIterator", fd_create_iterator, IPC_IMPL},
};
static IpcObject *fd_open(void) { return IPC_OBJECT("set:fd", fd_cmds, NULL, NULL); }
void service_register_setfd(void) { ipc_register_service("set:fd", fd_open); }
