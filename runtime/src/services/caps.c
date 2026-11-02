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
#include "sys_default.h"

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

/* Movie read streams (2001..2005): the album entries of content type movie (entry byte 0x19) can be read in pieces through a stream handle; the data is the
 * entry's stored file. Nothing on the host records movies, so a screenshot entry is "not a movie" and a stream only opens for an entry that was stored as one. */
#define MOVIE_STREAMS 8
typedef struct {
    uint8_t used[MOVIE_STREAMS];
    uint8_t entry[MOVIE_STREAMS][CAPS_ENTRY_SIZE];
} MovieSession;
static int stream_entry(IpcObject *s, IpcCall *c, uint64_t handle, int *idx) {
    MovieSession *m = s->state;
    if (!m || handle == 0 || handle > MOVIE_STREAMS || !m->used[handle - 1]) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return 0;
    }
    *idx = caps_album_find(&caps, m->entry[handle - 1]);
    if (*idx < 0) { /* the movie was deleted while the stream was open */
        c->result = HZ_R_CAPTURE_NOT_FOUND;
        return 0;
    }
    return 1;
}
static void movie_open(IpcObject *s, IpcCall *c) {
    MovieSession *m = s->state;
    int idx = find_arg(c, 0);
    if (idx < 0) {
        c->result = HZ_R_CAPTURE_NOT_FOUND;
        return;
    }
    if (caps.album[idx][0x19] != CAPS_CONTENT_MOVIE) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return;
    }
    for (unsigned i = 0; i < MOVIE_STREAMS; i++)
        if (!m->used[i]) {
            m->used[i] = 1;
            memcpy(m->entry[i], ipc_in(c, 0, CAPS_ENTRY_SIZE), CAPS_ENTRY_SIZE);
            uint64_t handle = i + 1;
            IPC_OUT(c, handle);
            return;
        }
    c->result = HZ_R_CAPTURE_INVALID_ARGUMENT; /* all stream slots in use */
}
static void movie_close(IpcObject *s, IpcCall *c) {
    MovieSession *m = s->state;
    uint64_t handle = IPC_IN(c, uint64_t, 0);
    if (!m || handle == 0 || handle > MOVIE_STREAMS || !m->used[handle - 1]) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return;
    }
    m->used[handle - 1] = 0;
}
static void movie_size(IpcObject *s, IpcCall *c) {
    MovieSession *m = s->state;
    int idx;
    uint64_t handle = IPC_IN(c, uint64_t, 0);
    if (!stream_entry(s, c, handle, &idx)) return;
    int64_t n = caps_album_file_size(&caps, m->entry[handle - 1]);
    uint64_t v = n > 0 ? (uint64_t)n : 0;
    IPC_OUT(c, v);
}
/* 2004 ReadMovieDataFromAlbumMovieReadStream(u64 handle, s64 offset; out: buffer) -> u64 bytes read (0 at or past the end). */
static void movie_read(IpcObject *s, IpcCall *c) {
    MovieSession *m = s->state;
    int idx;
    uint64_t handle = IPC_IN(c, uint64_t, 0);
    int64_t off = IPC_IN(c, int64_t, 8);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!stream_entry(s, c, handle, &idx)) return;
    if (!b || off < 0) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return;
    }
    int64_t total = caps_album_file_size(&caps, m->entry[handle - 1]);
    uint64_t got = 0;
    if (total > 0 && off < total) {
        uint8_t *all = malloc((size_t)total);
        if (!all || caps_album_load(&caps, m->entry[handle - 1], 0, all, (size_t)total) != total) {
            free(all);
            c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
            return;
        }
        got = (uint64_t)(total - off) < b->size ? (uint64_t)(total - off) : b->size;
        ipc_write_buf(c, b, all + off, got);
        free(all);
    }
    IPC_OUT(c, got);
}
/* 2005 GetAlbumMovieReadStreamBrokenReason(u64 handle) -> result: 0 while the stream's file reads cleanly (the album keeps whole files in memory). */
static void movie_broken_reason(IpcObject *s, IpcCall *c) {
    int idx;
    if (!stream_entry(s, c, IPC_IN(c, uint64_t, 0), &idx)) return;
    uint32_t r = 0;
    IPC_OUT(c, r);
}
static const IpcCmd session_cmds[] = {
    {1, "GetAlbumFileList", list_album, IPC_IMPL}, {2, "LoadAlbumFile", load_file, IPC_IMPL},           {3, "GetAlbumFileSize", file_size, IPC_IMPL},
    {4, "DeleteAlbumFile", delete_file, IPC_IMPL}, {5, "LoadAlbumFileThumbnail", load_thumb, IPC_IMPL},
    {2001, "OpenAlbumMovieReadStream", movie_open, IPC_IMPL},
    {2002, "CloseAlbumMovieReadStream", movie_close, IPC_IMPL},
    {2003, "GetAlbumMovieReadStreamMovieDataSize", movie_size, IPC_IMPL},
    {2004, "ReadMovieDataFromAlbumMovieReadStream", movie_read, IPC_IMPL},
    {2005, "GetAlbumMovieReadStreamBrokenReason", movie_broken_reason, IPC_IMPL},
};
static void session_destroy(IpcObject *o) { free(o->state); }
static void caps_open_session(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IAlbumAccessorApplicationSession", session_cmds, calloc(1, sizeof(MovieSession)), session_destroy));
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
    {142, "GetAlbumFileList3AaeAruidDeprecated", list_album, IPC_IMPL},
    {143, "GetAlbumFileList4AaeUidAruidDeprecated", list_album, IPC_IMPL},
    {60002, "OpenAccessorSessionForApplication", caps_open_session, IPC_IMPL},
    {144, "GetAllAlbumFileList3AaeAruidDeprecated", list_album, IPC_IMPL},
    {145, "GetAlbumFileList5AaeAruid", list_album, IPC_IMPL},
    {146, "GetAlbumFileList6AaeUidAruid", list_album, IPC_IMPL},
    {147, "GetAllAlbumFileList5AaeAruid", list_album, IPC_IMPL},
};
static IpcObject *u_open(void) { return IPC_OBJECT("caps:u", u_cmds, NULL, NULL); }
void service_register_caps(void) {
    ipc_register_service("caps:su", su_open);
    ipc_register_service("caps:u", u_open);
    applet_album_set_source(&caps); /* the album applet (0x15) browses the screenshots saved here */
}
