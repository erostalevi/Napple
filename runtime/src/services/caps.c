/* caps:u and caps:su (nn::capsrv): screenshots and the album. The album is an in-memory list (caps_core): saving a screenshot adds an entry,
 * listing returns the saved entries, deleting removes it, unknown entries are "not found". A save that comes with a 1280x720 RGBA input buffer
 * is stored as a JPEG (jpeg_core, the codec nvjpg uses): sizing it gives the file size, LoadAlbumFile the JPEG, the image loads the decoded RGBA
 * and the thumbnail loads the 320x180 RGBA. A save without pixels (or entries from one) keeps size 0 and loads of 0 bytes. Command ids are from
 * the author's memory of the public documentation and are unverified. Unlisted commands fail loudly (generic unknown-command policy). */
#include "svc_common.h"
#include "caps_core.h"
#include "applet_album_core.h"
#include "hz_result.h"
#include "host_info.h"
#include <stdlib.h>
#include <time.h>

static CapsState caps;

/* Entry of the screenshot just saved; the pixels, when the call carries a capture-sized image buffer, are stored with it. */
static void entry_out(IpcCall *c) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t e[CAPS_ENTRY_SIZE];
    caps_make_entry(&caps, (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min,
                    (uint8_t)tm.tm_sec, e);
    const IpcBuf *img = ipc_in_buf(c, 0);
    if (img && img->size >= CAPS_IMAGE_BYTES) {
        const uint8_t *px = ipc_buf_ptr(img);
        if (px && !caps_album_set_image(&caps, e, px, CAPS_IMAGE_W, CAPS_IMAGE_H, CAPS_IMAGE_W * 4)) caps_album_entry(&caps, e, e);
    }
    ipc_out(c, e, sizeof e);
}
/* A title that disabled screenshots (ISelfController::SetScreenShotPermission) gets the refusal a console gives. */
static void su_save_screenshot(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!applet_screenshot_permitted()) {
        c->result = HZ_R_CAPTURE_SCREENSHOT_NOT_PERMITTED;
        return;
    }
    entry_out(c);
}
/* The file argument is an ApplicationAlbumFileEntry (0x20 bytes, as caps_make_entry builds); a short or unknown one is "not found". */
static int find_arg(IpcCall *c, uint32_t off) {
    if (c->in_size < off + CAPS_ENTRY_SIZE) return -1;
    return caps_album_find(&caps, ipc_in(c, off, CAPS_ENTRY_SIZE));
}
/* Lists the album into output buffer 0 and answers the number of entries written (s64). */
static void list_album(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t out[CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE];
    unsigned cap = b ? (unsigned)(b->size / CAPS_ENTRY_SIZE) : 0;
    if (cap > CAPS_ALBUM_MAX) cap = CAPS_ALBUM_MAX;
    unsigned n = caps_album_list(&caps, -1, 0, cap, out);
    if (n) ipc_write_buf(c, b, out, (uint64_t)n * CAPS_ENTRY_SIZE);
    int64_t cnt = n;
    IPC_OUT(c, cnt);
}
static void file_size(IpcObject *s, IpcCall *c) {
    (void)s;
    if (find_arg(c, 0) < 0) {
        c->result = HZ_R_CAPTURE_NOT_FOUND;
        return;
    }
    int64_t n = caps_album_file_size(&caps, ipc_in(c, 0, CAPS_ENTRY_SIZE));
    IPC_OUT(c, n);
}
static void delete_file(IpcObject *s, IpcCall *c) {
    (void)s;
    if (find_arg(c, 0) < 0) {
        c->result = HZ_R_CAPTURE_NOT_FOUND;
        return;
    }
    caps_album_delete(&caps, ipc_in(c, 0, CAPS_ENTRY_SIZE));
}
/* Load*: writes what the entry holds into output buffer 0 (kind: 0 JPEG file, 1 decoded image, 2 decoded thumbnail) and answers the byte count.
 * An entry without an image writes nothing and answers 0; a buffer that is too small or an undecodable file is an invalid-argument style error. */
static void load_kind(IpcCall *c, int kind) {
    if (find_arg(c, 0) < 0) {
        c->result = HZ_R_CAPTURE_NOT_FOUND;
        return;
    }
    const uint8_t *entry = ipc_in(c, 0, CAPS_ENTRY_SIZE);
    const IpcBuf *b = ipc_out_buf(c, 0);
    int64_t n = 0;
    if (caps_album_file_size(&caps, entry) > 0) {
        size_t need = kind == 0 ? (size_t)caps_album_file_size(&caps, entry) : kind == 1 ? CAPS_IMAGE_BYTES : CAPS_THUMB_BYTES;
        uint8_t *tmp = b && b->size >= need ? malloc(need) : NULL;
        if (!tmp) {
            c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
            return;
        }
        n = caps_album_load(&caps, entry, kind, tmp, need);
        if (n > 0) ipc_write_buf(c, b, tmp, (uint64_t)n);
        free(tmp);
        if (n < 0) {
            c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
            return;
        }
    }
    IPC_OUT(c, n);
}
static void load_file(IpcObject *s, IpcCall *c) {
    (void)s;
    load_kind(c, 0);
}
static void load_image(IpcObject *s, IpcCall *c) {
    (void)s;
    load_kind(c, 1);
}
static void load_thumb(IpcObject *s, IpcCall *c) {
    (void)s;
    load_kind(c, 2);
}
static void set_shim_version(IpcObject *s, IpcCall *c) {
    (void)s;
    caps.shim_version = IPC_IN(c, uint64_t, 0);
}
static void precheck(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}

static const IpcCmd su_cmds[] = {
    {32, "SaveScreenShot", su_save_screenshot, IPC_IMPL},     {33, "SaveScreenShotEx0", su_save_screenshot, IPC_IMPL},
    {201, "SaveScreenShot", su_save_screenshot, IPC_IMPL},    {202, "SaveEditedScreenShot", su_save_screenshot, IPC_IMPL},
    {203, "SaveScreenShotEx0", su_save_screenshot, IPC_IMPL}, {204, "SaveEditedScreenShotEx0", su_save_screenshot, IPC_IMPL},
    {205, "SaveScreenShotEx1", su_save_screenshot, IPC_IMPL}, {206, "SaveScreenShotEx2", su_save_screenshot, IPC_IMPL},
    {210, "SaveScreenShotEx2", su_save_screenshot, IPC_IMPL},
};
static IpcObject *su_open(void) { return IPC_OBJECT("caps:su", su_cmds, NULL, NULL); }

static const IpcCmd session_cmds[] = {
    {1, "GetAlbumFileList", list_album, IPC_IMPL}, {2, "LoadAlbumFile", load_file, IPC_IMPL},           {3, "GetAlbumFileSize", file_size, IPC_IMPL},
    {4, "DeleteAlbumFile", delete_file, IPC_IMPL}, {5, "LoadAlbumFileThumbnail", load_thumb, IPC_IMPL},
};
static void caps_open_session(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IAlbumAccessorApplicationSession", session_cmds, NULL, NULL));
}
static const IpcCmd u_cmds[] = {
    {32, "SetShimLibraryVersion", set_shim_version, IPC_IMPL},
    {102, "GetAlbumFileList0AafeAruidDeprecated", list_album, IPC_IMPL},
    {103, "DeleteAlbumFileByAruid", delete_file, IPC_IMPL},
    {104, "GetAlbumFileSizeByAruid", file_size, IPC_IMPL},
    {105, "DeleteAlbumFileByAruidForDebug", delete_file, IPC_IMPL},
    {110, "LoadAlbumScreenShotImageByAruid", load_image, IPC_IMPL},
    {120, "LoadAlbumScreenShotThumbnailImageByAruid", load_thumb, IPC_IMPL},
    {130, "PrecheckToCreateContentsByAruid", precheck, IPC_IMPL},
    {140, "GetAlbumFileList1AafeAruidDeprecated", list_album, IPC_IMPL},
    {141, "GetAlbumFileList2AafeUidAruidDeprecated", list_album, IPC_IMPL},
    {142, "GetAlbumFileList3AaeAruid", list_album, IPC_IMPL},
    {143, "GetAlbumFileList4AaeUidAruid", list_album, IPC_IMPL},
    {60002, "OpenAccessorSessionForApplication", caps_open_session, IPC_IMPL},
};
static IpcObject *u_open(void) { return IPC_OBJECT("caps:u", u_cmds, NULL, NULL); }
void service_register_caps(void) {
    ipc_register_service("caps:su", su_open);
    ipc_register_service("caps:u", u_open);
    applet_album_set_source(&caps); /* the album applet (0x15) browses the screenshots saved here */
}
