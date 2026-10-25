/* Unit tests: applet settings core and the appletOE tables that store/return them. applet.c is #included; on Linux the few frontend/gfx
 * functions it calls are provided here (macOS links the real ones). */
#include "../src/services/applet.c"
#include "svc_test.h"
#ifdef __linux__
const uint8_t g_user_id[16] = {1};
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

int main(void) {
    AppletState a; applet_state_init(&a);
    CHECK(a.main_volume == 1.0f && a.library_volume == 1.0f && !a.vr_mode && !a.auto_sleep_disabled && a.restart_message && applet_screenshot_allowed(&a), "defaults");
    CHECK(applet_clamp_volume(2.0f, 0.5f) == 1.0f && applet_clamp_volume(-1.0f, 0.5f) == 0.0f && applet_clamp_volume(0.0f / 0.0f, 0.5f) == 0.5f && applet_clamp_volume(0.25f, 0) == 0.25f, "volume clamp");
    CHECK(applet_valid_screenshot_permission(0) && applet_valid_screenshot_permission(2) && !applet_valid_screenshot_permission(3), "screenshot permission values");
    unsigned d = 0; applet_block_begin(&d); applet_block_begin(&d);
    CHECK(applet_block_end(&d) == 1 && applet_block_end(&d) == 0 && applet_block_end(&d) == 0 && d == 0, "blocking nests and never underflows");
    a.screenshot_permission = APPLET_SCREENSHOT_DISABLE; CHECK(!applet_screenshot_allowed(&a), "disabled screenshots");

    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    IpcObject *csg = get(proxy, 0), *sc = get(proxy, 1), *ac = get(proxy, 3), *af = get(proxy, 20);
    CHECK(csg && sc && ac && af, "proxy hands out the sub-interfaces");

    /* message queue */
    svc_call(csg, 1, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == MSG_FOCUS_CHANGED, "first message: focus changed");
    svc_call(csg, 1, NULL, 0, &c); CHECK(c.result == R_NO_MESSAGES, "queue empty");

    /* ICommonStateGetter */
    svc_call(csg, 900, NULL, 0, &c); CHECK(c.result == 0, "cmd 900 (request-exit-to-library-applet flag) is accepted");
    svc_call(csg, 50, NULL, 0, &c); CHECK(c.out[0] == 0, "VR mode off");
    uint8_t on = 1; svc_call(csg, 51, &on, 1, &c); svc_call(csg, 50, NULL, 0, &c); CHECK(c.out[0] == 1, "SetVrModeEnabled is read back");
    svc_call(csg, 51, NULL, 0, &c); CHECK(c.result == 0 && AS()->vr_mode == 1, "short request is ignored, not fatal");
    uint32_t boost = 2; svc_call(csg, 66, &boost, 4, &c); CHECK(AS()->cpu_boost_mode == 2 && AS()->cpu_boost_requests == 1, "SetCpuBoostMode");
    svc_call(csg, 67, NULL, 0, &c); CHECK(AS()->cpu_boost_mode == 0, "CancelCpuBoostMode");

    /* ISelfController */
    svc_call(sc, 69, NULL, 0, &c); CHECK(c.out[0] == 0, "auto sleep enabled by default");
    svc_call(sc, 68, &on, 1, &c); svc_call(sc, 69, NULL, 0, &c); CHECK(c.out[0] == 1, "SetAutoSleepDisabled is read back");
    uint32_t ext = 5; svc_call(sc, 62, &ext, 4, &c); svc_call(sc, 63, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 5, "idle detection extension round trip");
    uint32_t perm = APPLET_SCREENSHOT_DISABLE; svc_call(sc, 10, &perm, 4, &c); CHECK(c.result == 0 && !applet_screenshot_permitted(), "SetScreenShotPermission(Disable)");
    perm = APPLET_SCREENSHOT_ENABLE; svc_call(sc, 10, &perm, 4, &c); CHECK(applet_screenshot_permitted(), "SetScreenShotPermission(Enable)");
    perm = 9; svc_call(sc, 10, &perm, 4, &c); CHECK(c.result != 0 && applet_screenshot_permitted(), "invalid permission rejected, state unchanged");
    svc_call(sc, 65, NULL, 0, &c); svc_call(sc, 65, NULL, 0, &c); CHECK(AS()->user_active_reports == 2, "ReportUserIsActive counted");
    uint8_t off = 0; svc_call(sc, 14, &off, 1, &c); CHECK(AS()->restart_message == 0, "SetRestartMessageEnabled");
    uint32_t orient = 3; svc_call(sc, 19, &orient, 4, &c); CHECK(AS()->screenshot_orientation == 3, "SetScreenShotImageOrientation");

    /* IAudioController */
    float vols[2] = {0.5f, 2.0f}; svc_call(ac, 0, vols, 8, &c);
    svc_call(ac, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.5f, "main applet volume");
    svc_call(ac, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 1.0f, "library volume clamped to 1");
    uint8_t cv[12] = {0}; float nv = 0.25f; memcpy(cv, &nv, 4); svc_call(ac, 3, cv, sizeof cv, &c); svc_call(ac, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.25f, "ChangeMainAppletMasterVolume");

    /* IApplicationFunctions */
    svc_call(af, 32, NULL, 0, &c); svc_call(af, 32, NULL, 0, &c); svc_call(af, 33, NULL, 0, &c);
    CHECK(AS()->home_button_block == 1, "home button block nests");
    uint32_t rec = 2; svc_call(af, 67, &rec, 4, &c); CHECK(AS()->game_play_recording_state == 2, "SetGamePlayRecordingState");
    svc_call(af, 90, &on, 1, &c); CHECK(AS()->crash_report_enabled == 1, "EnableApplicationCrashReport");
    /* cmd 210 hands the guest a copy handle (an unsignalled event): a reply without one left Animal Crossing spinning on WaitSynchronization(handle 0) */
    svc_call(af, 210, NULL, 0, &c); CHECK(c.nout_copy == 1 && c.out_copy[0] != 0 && c.nout_objects == 0 && !event_is_signaled(c.out_copy[0]), "cmd 210 returns an event copy handle");
    printf("{\"test\":\"svc_applet\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
