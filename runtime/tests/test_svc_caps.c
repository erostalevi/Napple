/* Unit tests: caps core and the caps:su / caps:u tables. */
#include "../src/services/caps.c"
#include "svc_test.h"
#ifdef __linux__
static int permitted = 1;
int applet_screenshot_permitted(void) { return permitted; }   /* applet.c is not linked on Linux */
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
    svc_call(u, 142, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "album list is empty");
    svc_call(u, 104, NULL, 0, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "file size of a missing file: not found");
    svc_call(u, 130, NULL, 0, &c); CHECK(c.result == 0, "PrecheckToCreateContents succeeds");
    svc_call(u, 60002, NULL, 0, &c); CHECK(c.nout_objects == 1, "OpenAccessorSessionForApplication returns a session");
    IpcObject *sess = c.out_objects[0];
    svc_call(sess, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, int64_t, 0) == 0, "session: empty list");
    svc_call(sess, 2, NULL, 0, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "session: load of a missing file");
    printf("{\"test\":\"svc_caps\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
