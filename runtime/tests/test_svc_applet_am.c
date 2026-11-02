/* Unit tests (am-stubs-legal, 2026-11-02): the am commands made real in r4, driven through applet.c's own IPC objects: the exit requests
 * (RequestToShutdown / RequestToReboot / RequestExitApplicationAndTryPopMessage: ExitRequested message, then a clean stop; run in a forked child),
 * the launch-required version (persisted, only raised), application play statistics, SaveCurrentScreenshot into the caps album (with a test frame
 * source; refused when screenshots are disabled, not-found without a frame), TakeScreenShotOfOwnLayerEx, the panel values and license answers, and the
 * network-connection library applet (0x0f) answering "not connected". applet.c is #included. */
#include "../src/services/applet.c"
#include "svc_test.h"
#include <sys/wait.h>
#include <time.h>
#ifdef __linux__
const uint8_t g_user_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
void host_display_version(char out[16]) { snprintf(out, 16, "1.0.0"); }
uint64_t host_desired_language(void) { return 0x53552d6e65ULL; }
void ipc_trace_guest(unsigned n) { (void)n; }
void napple_display_game_size(uint32_t w, uint32_t h, uint32_t *gw, uint32_t *gh) { *gw = w; *gh = h; }
int frontend_text_input(const char *a, const char *b, const char *c, const char *d, unsigned e, unsigned f, int g, int h, char *out, unsigned cap) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; if (cap) out[0] = 0; return 0; }
size_t napple_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap) { size_t k = 0; for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)u[i]; if (cap) out[k] = 0; return k; }
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) { size_t k = 0; for (; s[k] && k < cap; k++) out[k] = (uint8_t)s[k]; return k; }
void frontend_alert(const char *t, const char *m) { (void)t; (void)m; }
int frontend_connected_pads(void) { return 1; }
#endif

static IpcObject *get(IpcObject *proxy, unsigned cmd) { IpcCall c; svc_call(proxy, cmd, NULL, 0, &c); return c.nout_objects ? c.out_objects[0] : NULL; }
static IpcObject *create_applet(IpcObject *lac, uint32_t id) {
    uint32_t in[2] = {id, 0}; IpcCall c; svc_call(lac, 0, in, 8, &c); return c.nout_objects ? c.out_objects[0] : NULL;
}
static void push(IpcObject *la, const uint8_t *data, uint64_t size) {
    IpcObject *st = storage_new(size); if (data) memcpy(((Storage *)st->state)->data, data, size);
    IpcCall c; memset(&c, 0, sizeof c); c.in_objects[0] = st; c.nin_objects = 1; svc_call_c(la, 100, &c); CHECK(c.result == 0, "PushInData");
}
static int g_frame_calls, g_frame_ok = 1;
static int test_frame(uint8_t *rgba, uint32_t w, uint32_t h) { /* a gradient, so the album JPEG is a real image */
    g_frame_calls++;
    if (!g_frame_ok) return 0;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { uint8_t *p = rgba + ((size_t)y * w + x) * 4; p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = 90; p[3] = 255; }
    return 1;
}

int main(void) {
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    IpcObject *csg = get(proxy, 0), *sc = get(proxy, 1), *dc = get(proxy, 4), *lac = get(proxy, 11), *af = get(proxy, 20);
    CHECK(csg && sc && dc && lac && af, "proxy hands out the sub-interfaces");
    svc_call(csg, 1, NULL, 0, &c); /* drop the initial focus message */

    /* ---- license and panel answers ---- */
    svc_call(af, 34, NULL, 0, &c); CHECK(c.result == 0, "SelectApplicationLicense accepted (full license)");
    svc_call(af, 36, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0, "GetLimitedApplicationLicense: no limited license");
    svc_call(csg, 1004, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 137 && SVC_OUT(&c, uint32_t, 4) == 77, "panel physical size 137 x 77 mm");
    svc_call(csg, 1005, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1280 && SVC_OUT(&c, uint32_t, 4) == 720, "panel resolution 1280 x 720");

    /* ---- launch-required version: persisted per title next to the saves, only raised ---- */
    char root[] = "/tmp/napple-am-XXXXXX";
    CHECK(mkdtemp(root) != NULL, "temp save root");
    g_cfg.save_root = root;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "01006BB00C6F0000");
    char tdir[300]; snprintf(tdir, sizeof tdir, "%s/%s", root, g_cfg.title_id); mkdir(tdir, 0755);
    svc_call(af, 180, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetLaunchRequiredVersion starts at 0");
    uint32_t v = 0x50000; svc_call(af, 181, &v, 4, &c);
    svc_call(af, 180, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x50000, "UpgradeLaunchRequiredVersion raises it");
    v = 0x10000; svc_call(af, 181, &v, 4, &c);
    svc_call(af, 180, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x50000, "a lower version never lowers it");
    launch_required_version = 0xffffffffu; /* as a new process: reload from the file */
    svc_call(af, 180, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x50000, "persisted across runs (%#x)", SVC_OUT(&c, uint32_t, 0));

    /* ---- play statistics: only the running title, one launch, play time since start ---- */
    g_program_id = 0x01006BB00C6F0000ull;
    uint64_t ids[3] = {0x0100000000010000ull, g_program_id, g_program_id};
    uint64_t st[4][3]; memset(st, 0xee, sizeof st);
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, ids, sizeof ids); svc_set_out_buf(&c, st, sizeof st); svc_call_c(af, 110, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 1 && st[0][0] == g_program_id && st[0][2] == 1 && st[0][1] < 3600ull * 1000000000ull && st[1][0] == 0xeeeeeeeeeeeeeeeeull,
          "QueryApplicationPlayStatistics: one entry for the running title (launches %llu, %llu ns), other titles unplayed", (unsigned long long)st[0][2], (unsigned long long)st[0][1]);
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, ids, 8); svc_set_out_buf(&c, st, sizeof st); svc_call_c(af, 111, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0, "ByUid: a title never played here gives 0 entries");

    /* ---- screenshots: TakeScreenShotOfOwnLayerEx and SaveCurrentScreenshot into the album ---- */
    applet_capture_set_source(test_frame);
    static CapsState caps; applet_album_set_source(&caps);
    uint8_t ex[16] = {0}; svc_call(dc, 28, ex, sizeof ex, &c);
    CHECK(c.result == 0 && applet_capture_has_frame(APPLET_CAP_LAST_APPLICATION), "TakeScreenShotOfOwnLayerEx fills the application capture slot");
    uint32_t opt = 1; svc_call(sc, 120, &opt, 4, &c);
    uint64_t jsize = 0; if (caps_album_count(&caps)) memcpy(&jsize, caps.album[0], 8);
    CHECK(c.result == 0 && caps_album_count(&caps) == 1 && jsize > 1000 && AS()->screenshots_saved == 1, "SaveCurrentScreenshot stores the frame in the album (%llu-byte JPEG)", (unsigned long long)jsize);
    uint8_t *img = malloc(CAPS_IMAGE_BYTES);
    int64_t got = caps_album_load(&caps, caps.album[0], 1, img, CAPS_IMAGE_BYTES);
    CHECK(got == (int64_t)CAPS_IMAGE_BYTES && img[0] < 10 && img[(CAPS_IMAGE_W - 1) * 4] > 245, "the saved screenshot decodes to the frame (R %u .. %u)", img[0], img[(CAPS_IMAGE_W - 1) * 4]);
    free(img);
    uint32_t perm = APPLET_SCREENSHOT_DISABLE; svc_call(sc, 10, &perm, 4, &c);
    svc_call(sc, 120, &opt, 4, &c); CHECK(c.result == HZ_R_CAPTURE_SCREENSHOT_NOT_PERMITTED && caps_album_count(&caps) == 1, "SaveCurrentScreenshot refused when screenshots are disabled");
    perm = APPLET_SCREENSHOT_ENABLE; svc_call(sc, 10, &perm, 4, &c);
    g_frame_ok = 0; applet_capture_reset();
    svc_call(sc, 120, &opt, 4, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND && caps_album_count(&caps) == 1, "no frame yet: nothing saved, not-found");
    g_frame_ok = 1;
    uint8_t mute = 1; svc_call(sc, 130, &mute, 1, &c); CHECK(AS()->record_volume_muted == 1, "SetRecordVolumeMuted kept");

    /* ---- network-connection applet (0x0f): finishes normally, output = the not-connected result ---- */
    applet_netconnect_register();
    IpcObject *la = create_applet(lac, LIBAPPLET_ID_NET_CONNECT);
    CHECK(la != NULL, "CreateLibraryApplet(NetConnect)");
    uint8_t common[0x20] = {1, 0, 0x20, 0}; push(la, common, sizeof common);
    uint8_t narg[0x80] = {0}; push(la, narg, sizeof narg);
    svc_call(la, 10, NULL, 0, &c); CHECK(c.result == 0, "Start");
    svc_call(la, 1, NULL, 0, &c); CHECK(c.out[0] == 1, "completed at once");
    svc_call(la, 30, NULL, 0, &c); CHECK(c.result == 0, "GetResult: the applet closed normally");
    svc_call(la, 101, NULL, 0, &c);
    uint32_t r = 0; uint64_t osz = 0;
    if (c.result == 0 && c.nout_objects) { Storage *s = c.out_objects[0]->state; osz = s->size; memcpy(&r, s->data, 4); }
    CHECK(osz == APPLET_NETCONNECT_OUT_SIZE && r == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "output: not connected (%#x, %llu bytes; %#x expected)", r, (unsigned long long)osz, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED);

    /* ---- exit requests: ExitRequested message, then the process stops cleanly after the grace period (forked child per command) ---- */
    const unsigned exit_cmds[3] = {70, 71, 310};
    for (int k = 0; k < 3; k++) {
        int p[2]; CHECK(pipe(p) == 0, "pipe");
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            setenv("NAPPLE_EXIT_GRACE_MS", "300", 1);
            IpcCall cc; svc_call(af, exit_cmds[k], NULL, 0, &cc);
            svc_call(af, exit_cmds[k], NULL, 0, &cc); /* a second request changes nothing */
            uint32_t m = 0; svc_call(csg, 1, NULL, 0, &cc); if (cc.result == 0) memcpy(&m, cc.out, 4);
            (void)!write(p[1], &m, 4);
            svc_call(csg, 1, NULL, 0, &cc); uint32_t again = cc.result == 0; (void)!write(p[1], &again, 4);
            sleep(5);
            _exit(77); /* never reached: the grace thread stops the process */
        }
        close(p[1]);
        uint32_t m = 0, again = 1;
        (void)!read(p[0], &m, 4); (void)!read(p[0], &again, 4); close(p[0]);
        int ws = 0; waitpid(pid, &ws, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        CHECK(m == MSG_EXIT_REQUESTED && !again, "cmd %u: the application is told ExitRequested (message %u), once", exit_cmds[k], m);
        CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0 && secs < 3.0, "cmd %u: the process stops cleanly after the grace period (status %#x, %.2f s)", exit_cmds[k], ws, secs);
    }

    char f[400]; snprintf(f, sizeof f, "%s/launch_required_version", tdir); unlink(f); rmdir(tdir); rmdir(root);
    printf("{\"test\":\"svc_applet_am\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
