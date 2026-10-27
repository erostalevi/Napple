/* ngc:u (nn::ngc): the profanity filter. Animal Crossing asks for it right after the timezone service during boot. The word lists are not shipped with the
 * runtime, so by default nothing is flagged: Check reports "clean" and Mask returns the text unchanged; a user-supplied list (NAPPLE_NGC_WORDLIST, ngc_core.h) makes both real. Command ids and layouts are from the author's memory of
 * the public documentation (unverified). */
#include "svc_common.h"
#include "ngc_core.h"

static NgcWords words;
static int words_loaded;
static const NgcWords *wl(void) {
    if (!words_loaded) {
        ngc_words_load_env(&words);
        words_loaded = 1;
    }
    return &words;
}
/* 0 GetContentVersion -> u32 (changes whenever the list is reloaded). */
static void ngc_content_version(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = wl()->version;
    IPC_OUT(c, v);
}
/* 1 Check(u32 flags; in: UTF-16 text) -> u32 flags: bit 0 set when any listed word occurs. */
static void ngc_check_call(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t flags = 0;
    const IpcBuf *in = ipc_in_buf(c, 0);
    const uint16_t *t = in && in->size ? ipc_buf_ptr(in) : NULL;
    if (t && ngc_check(wl(), t, (size_t)(in->size / 2))) flags = 1;
    IPC_OUT(c, flags);
}
/* 2 Mask(in: text, out: text) -> u32 number of masked units; without a list the text is returned unchanged. */
static void ngc_mask_call(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    uint32_t masked = 0;
    if (in && out && in->size && ipc_buf_ptr(in)) {
        uint64_t n = in->size < out->size ? in->size : out->size;
        uint16_t tmp[0x1000];
        if (n > sizeof tmp) n = sizeof tmp;
        memcpy(tmp, ipc_buf_ptr(in), (size_t)n);
        masked = (uint32_t)ngc_mask(wl(), tmp, (size_t)(n / 2));
        ipc_write_buf(c, out, tmp, n);
    }
    IPC_OUT(c, masked);
}
/* 3 Reload: re-reads the user's word list. */
static void ngc_reload(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    ngc_words_load_env(&words);
    words_loaded = 1;
}
static const IpcCmd ngc_cmds[] = {
    {0, "GetContentVersion", ngc_content_version, IPC_IMPL},
    {1, "Check", ngc_check_call, IPC_IMPL},
    {2, "Mask", ngc_mask_call, IPC_IMPL},
    {3, "Reload", ngc_reload, IPC_IMPL},
};
static IpcObject *ngc_open(void) { return IPC_OBJECT("ngc:u", ngc_cmds, NULL, NULL); }
void service_register_ngc(void) { ipc_register_service("ngc:u", ngc_open); }
