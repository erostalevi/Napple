/* Unit tests: caps core and the caps:su / caps:u tables. */
#include "../src/services/caps.c"
#include "svc_test.h"
#ifdef __linux__
static int permitted = 1;
int applet_screenshot_permitted(void) { return permitted; }   /* applet.c is not linked on Linux */
#endif

#ifdef __linux__
#define LINUX_EXTRA_SAVE 1u
#else
#define LINUX_EXTRA_SAVE 0u
#endif

int main(void) {
    CapsState st = {0}; uint8_t e[CAPS_ENTRY_SIZE], f[CAPS_ENTRY_SIZE];
    caps_make_entry(&st, 2026, 10, 24, 13, 5, 9, e);
    uint16_t year; memcpy(&year, e + 0x10, 2);
    CHECK(year == 2026 && e[0x12] == 10 && e[0x13] == 24 && e[0x14] == 13 && e[0x15] == 5 && e[0x16] == 9, "entry carries the save time");
    uint64_t size, hash; memcpy(&size, e, 8); memcpy(&hash, e + 8, 8); CHECK(size == 0 && hash == 0, "no file: size and hash are 0");
    caps_make_entry(&st, 2026, 10, 24, 13, 5, 9, f); CHECK(st.saved == 2 && f[0x17] == 1 && e[0x17] == 0, "unique id counts up");

    IpcObject *su = su_open(), *u = u_open(); IpcCall c;
    for (unsigned i = 0; i < su->ncmds; i++) {
        svc_call(su, su->cmds[i].cmd, NULL, 0, &c);
        CHECK(c.result == 0 && c.out_size == CAPS_ENTRY_SIZE, "caps:su cmd %u succeeds with an album entry and no file", su->cmds[i].cmd);
    }
#ifdef __linux__
    permitted = 0; svc_call(su, 32, NULL, 0, &c); CHECK(c.result == HZ_R_CAPTURE_SCREENSHOT_NOT_PERMITTED && c.out_size == 0, "screenshots disabled by the title: refused");
    permitted = 1; svc_call(su, 32, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == CAPS_ENTRY_SIZE, "re-enabled: succeeds again");
#endif
    uint64_t ver = 5; svc_call(u, 32, &ver, 8, &c); CHECK(c.result == 0 && caps.shim_version == 5, "SetShimLibraryVersion");
    CHECK(caps_album_count(&caps) == su->ncmds + LINUX_EXTRA_SAVE, "every successful save landed (a refused one did not) in the album");
    unsigned saved = caps_album_count(&caps);
    uint8_t buf[CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE];
    static const unsigned list_cmds[] = {102, 140, 141, 142, 143};
    for (unsigned i = 0; i < 5; i++) {
        c = (IpcCall){0}; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(u, list_cmds[i], &c);
        CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == (int64_t)saved, "album list cmd %u returns the saved entries", list_cmds[i]);
    }
    c = (IpcCall){0}; svc_set_out_buf(&c, buf, 2 * CAPS_ENTRY_SIZE); svc_call_c(u, 142, &c); CHECK(SVC_OUT(&c, int64_t, 0) == 2, "list into a small buffer is capped");
    c = (IpcCall){0}; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(u, 142, &c);
    uint8_t first[CAPS_ENTRY_SIZE], last[CAPS_ENTRY_SIZE]; memcpy(first, buf, sizeof first); memcpy(last, buf + (saved - 1) * CAPS_ENTRY_SIZE, sizeof last);
    CHECK(first[0x17] == 0 && last[0x17] == saved - 1, "entries keep their unique ids, oldest first");

    svc_call(u, 104, first, sizeof first, &c); CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "file size of a saved entry (no file behind it)");
    svc_call(u, 110, first, sizeof first, &c); CHECK(c.result == 0, "load image of a saved entry");
    svc_call(u, 120, first, sizeof first, &c); CHECK(c.result == 0, "load thumbnail of a saved entry");
    uint8_t missing[CAPS_ENTRY_SIZE] = {0}; missing[0x17] = 0xee;
    svc_call(u, 104, missing, sizeof missing, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "file size of a missing file: not found");
    svc_call(u, 104, NULL, 0, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "file size without a valid entry: not found");
    svc_call(u, 110, missing, sizeof missing, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "load of a missing file: not found");
    svc_call(u, 103, missing, sizeof missing, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "delete of a missing file: not found");
    svc_call(u, 103, first, sizeof first, &c); CHECK(c.result == 0 && caps_album_count(&caps) == saved - 1, "delete removes the entry");
    svc_call(u, 104, first, sizeof first, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "deleted entry is gone");
    svc_call(u, 105, last, sizeof last, &c); CHECK(c.result == 0 && caps_album_count(&caps) == saved - 2, "DeleteAlbumFileByAruidForDebug");
    svc_call(u, 130, NULL, 0, &c); CHECK(c.result == 0, "PrecheckToCreateContents succeeds");

    svc_call(u, 60002, NULL, 0, &c); CHECK(c.nout_objects == 1, "OpenAccessorSessionForApplication returns a session");
    IpcObject *sess = c.out_objects[0];
    svc_call(su, 32, NULL, 0, &c); uint8_t fresh[CAPS_ENTRY_SIZE]; memcpy(fresh, c.out, sizeof fresh);
    c = (IpcCall){0}; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(sess, 1, &c); CHECK(SVC_OUT(&c, int64_t, 0) == (int64_t)caps_album_count(&caps), "session: list matches the album");
    svc_call(sess, 3, fresh, sizeof fresh, &c); CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "session: size of a saved entry");
    svc_call(sess, 2, fresh, sizeof fresh, &c); CHECK(c.result == 0, "session: load of a saved entry");
    svc_call(sess, 5, fresh, sizeof fresh, &c); CHECK(c.result == 0, "session: thumbnail of a saved entry");
    svc_call(sess, 2, missing, sizeof missing, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "session: load of a missing file");
    svc_call(sess, 4, fresh, sizeof fresh, &c); CHECK(c.result == 0, "session: delete");
    svc_call(sess, 4, fresh, sizeof fresh, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "session: second delete is not found");

    for (unsigned i = 0; i < CAPS_ALBUM_MAX + 3; i++) svc_call(su, 32, NULL, 0, &c);
    CHECK(caps_album_count(&caps) == CAPS_ALBUM_MAX, "album is capped, oldest dropped");
    CHECK(caps_album_list(&caps, CAPS_CONTENT_MOVIE, 0, 8, buf) == 0, "no movies in the album");
    { /* ipc-local-stubs: album list variants and movie read streams */
        IpcObject *u2 = u_open(); IpcCall k; uint8_t shot[CAPS_ENTRY_SIZE], mv[CAPS_ENTRY_SIZE];
        caps_make_entry(&caps, 2026, 11, 1, 9, 0, 0, shot);
        unsigned listed = caps_album_count(&caps);
        const unsigned lists[] = {144, 145, 146, 147};
        for (unsigned i = 0; i < 4; i++) {
            static uint8_t out[CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE]; uint8_t in[0x30] = {0};
            memset(&k, 0, sizeof k); k.in = in; k.in_size = sizeof in; svc_set_out_buf(&k, out, sizeof out); svc_call_c(u2, lists[i], &k);
            CHECK(k.result == 0 && SVC_OUT(&k, int64_t, 0) == (int64_t)listed, "caps:u %u lists the album (%u entries)", lists[i], listed);
            CHECK(svc_find(u2, lists[i])->status == IPC_IMPL, "caps:u %u is real now", lists[i]);
        }
        caps_make_entry(&caps, 2026, 11, 1, 9, 30, 0, mv);
        int mi = caps_album_find(&caps, mv); CHECK(mi >= 0, "the movie entry is in the album");
        static const char movie[] = "0123456789abcdef";
        caps.album[mi][0x19] = CAPS_CONTENT_MOVIE; caps.jpeg[mi] = malloc(sizeof movie - 1); memcpy(caps.jpeg[mi], movie, sizeof movie - 1); caps.jpeg_size[mi] = sizeof movie - 1;
        memcpy(mv, caps.album[mi], CAPS_ENTRY_SIZE);
        svc_call(u2, 60002, NULL, 0, &k); IpcObject *sess2 = k.out_objects[0];
        svc_call(sess2, 2001, shot, sizeof shot, &k); CHECK(k.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "2001 OpenAlbumMovieReadStream of a screenshot: invalid argument");
        uint8_t gone[CAPS_ENTRY_SIZE]; memcpy(gone, shot, sizeof gone); gone[0x10] ^= 0xff; gone[0x11] ^= 0xff;
        svc_call(sess2, 2001, gone, sizeof gone, &k); CHECK(k.result == HZ_R_CAPTURE_NOT_FOUND, "...of an entry that does not exist: not found");
        svc_call(sess2, 2001, mv, sizeof mv, &k); CHECK(k.result == 0 && k.out_size == 8, "...of a movie: a stream handle");
        uint64_t h = SVC_OUT(&k, uint64_t, 0); CHECK(h != 0, "the handle is not 0");
        svc_call(sess2, 2003, &h, 8, &k); CHECK(k.result == 0 && SVC_OUT(&k, uint64_t, 0) == 16, "2003 GetAlbumMovieReadStreamMovieDataSize: 16 bytes");
        uint8_t rin[16]; memcpy(rin, &h, 8); int64_t off = 3; memcpy(rin + 8, &off, 8); char got[8] = {0};
        memset(&k, 0, sizeof k); k.in = rin; k.in_size = sizeof rin; svc_set_out_buf(&k, got, 4); svc_call_c(sess2, 2004, &k);
        CHECK(k.result == 0 && SVC_OUT(&k, uint64_t, 0) == 4 && !memcmp(got, "3456", 4), "2004 ReadMovieData reads 4 bytes at offset 3");
        off = 12; memcpy(rin + 8, &off, 8); char got2[16] = {0};
        memset(&k, 0, sizeof k); k.in = rin; k.in_size = sizeof rin; svc_set_out_buf(&k, got2, sizeof got2); svc_call_c(sess2, 2004, &k);
        CHECK(k.result == 0 && SVC_OUT(&k, uint64_t, 0) == 4 && !memcmp(got2, "cdef", 4), "...a read near the end is cut");
        off = 16; memcpy(rin + 8, &off, 8);
        memset(&k, 0, sizeof k); k.in = rin; k.in_size = sizeof rin; svc_set_out_buf(&k, got2, sizeof got2); svc_call_c(sess2, 2004, &k);
        CHECK(k.result == 0 && SVC_OUT(&k, uint64_t, 0) == 0, "...a read at the end gives 0 bytes");
        svc_call(sess2, 2005, &h, 8, &k); CHECK(k.result == 0 && SVC_OUT(&k, uint32_t, 0) == 0, "2005 GetAlbumMovieReadStreamBrokenReason: not broken");
        svc_call(sess2, 2002, &h, 8, &k); CHECK(k.result == 0, "2002 CloseAlbumMovieReadStream");
        svc_call(sess2, 2003, &h, 8, &k); CHECK(k.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "...the handle is dead afterwards");
        svc_call(sess2, 2002, &h, 8, &k); CHECK(k.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "...closing twice is an error");
        uint64_t bogus = 99; svc_call(sess2, 2005, &bogus, 8, &k); CHECK(k.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "a handle that was never handed out is invalid");
        svc_call(sess2, 2001, mv, sizeof mv, &k); uint64_t h2 = SVC_OUT(&k, uint64_t, 0);
        caps_album_delete(&caps, mv);
        svc_call(sess2, 2003, &h2, 8, &k); CHECK(k.result == HZ_R_CAPTURE_NOT_FOUND, "a movie deleted while its stream is open: not found");
        for (unsigned i = 2001; i <= 2005; i++) CHECK(svc_find(sess2, i)->status == IPC_IMPL, "session %u is real now", i);
    }
    printf("{\"test\":\"svc_caps\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
