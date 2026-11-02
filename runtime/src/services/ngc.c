/* ngc:u (nn::ngc): the profanity filter. Animal Crossing asks for it right after the timezone service during boot. The word lists are the firmware's own
 * NgWord system data archive (0100000000000806, written by `napple build --firmware` to <title dir>/sysdata/, see ngc_core.h for the list format): every
 * list in it is loaded, so Check flags and Mask replaces the words the console would. A user-supplied list (NAPPLE_NGC_WORDLIST) wins over the firmware's;
 * with neither nothing is flagged. Command ids and layouts are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "ngc_core.h"
#include "romfs_read.h"
#include "sys_default.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static NgcWords words;
static int words_loaded;

static ssize_t fd_read(void *ctx, void *dst, size_t len, uint64_t off) { return pread((int)(intptr_t)ctx, dst, len, (off_t)off); }
/* Loads every list of the NgWord archive image at `path` (common.txt and 0.txt..15.txt) into `w` (cleared first); the number of words, 0 when the archive is absent. */
static unsigned ngc_words_load_firmware(NgcWords *w, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    off_t size = lseek(fd, 0, SEEK_END);
    RomfsImg *img = size > 0 ? romfs_img_open(fd_read, (void *)(intptr_t)fd, (uint64_t)size) : NULL;
    unsigned total = 0;
    if (img) {
        ngc_words_clear(w);
        char name[32];
        for (int i = -1; i < 16; i++) {
            if (i < 0) snprintf(name, sizeof name, "/common.txt");
            else snprintf(name, sizeof name, "/%d.txt", i);
            uint64_t off, fsz;
            if (romfs_img_lookup(img, name, &off, &fsz) != ROMFS_FILE || fsz > (1u << 20)) continue;
            uint8_t *buf = malloc((size_t)fsz);
            if (buf && romfs_img_read(img, off, fsz, 0, buf, (size_t)fsz) == (ssize_t)fsz) total += ngc_words_add_ngword(w, buf, (size_t)fsz);
            free(buf);
        }
        romfs_img_close(img);
    }
    close(fd);
    return total;
}
static void load_words(NgcWords *w) {
    const char *env = getenv("NAPPLE_NGC_WORDLIST");
    if (env && *env) {
        ngc_words_load_env(w);
        return;
    }
    char path[1400];
    const char *dd = g_cfg.data_dir;
    unsigned n = 0;
    if (dd && *dd) {
        snprintf(path, sizeof path, "%s/sysdata/0100000000000806.bin", dd);
        n = ngc_words_load_firmware(w, path);
        if (!n) {
            snprintf(path, sizeof path, "%s/../sysdata/0100000000000806.bin", dd);
            n = ngc_words_load_firmware(w, path);
        }
    }
    if (n) {
        ngc_words_commit(w);
        LOG("ngc", "%u NG words loaded from the firmware NgWord archive", n);
    } else ngc_words_clear(w);
}
static const NgcWords *wl(void) {
    if (!words_loaded) {
        load_words(&words);
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
    load_words(&words);
    words_loaded = 1;
}
static const IpcCmd ngc_cmds[] = {
    {0, "GetContentVersion", ngc_content_version, IPC_IMPL},
    {1, "Check", ngc_check_call, IPC_IMPL},
    {2, "Mask", ngc_mask_call, IPC_IMPL},
    {3, "Reload", ngc_reload, IPC_IMPL},
    /* 21.0.0+: the second-generation entry points take the same text; the word data behind them is the same archive, so they answer like 1 and 2. */
    {4, "Check2", ngc_check_call, IPC_IMPL},
    {5, "Mask2", ngc_mask_call, IPC_IMPL},
};
static IpcObject *ngc_open(void) { return IPC_OBJECT("ngc:u", ngc_cmds, NULL, NULL); }
void service_register_ngc(void) { ipc_register_service("ngc:u", ngc_open); }
