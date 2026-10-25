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
    printf("{\"test\":\"svc_caps\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
