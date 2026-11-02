/* Unit tests (ipc-depth-95 r10, applet part): the am rows made real in r10, driven through applet.c's own IPC objects: the user channel
 * (UnpopToUserChannel / ClearUserChannel / PopLaunchParameter(1)), the restarts of ExecuteProgram and CreateApplication(AndPush)AndRequestToStart (with a test
 * hook in place of the host's execv, then a "new process" that reads what the restart left), HDCP activation and its state / event, the system button
 * presses of PerformSystemButtonPressingIfInFocus with SetHandlingHomeButtonShortPressedEnabled, the GPU-error abort delay (forked children), PrepareForJit
 * with and without a jit:u service, and ILibraryAppletAccessor::RequestForLibraryAppletToGetForeground. applet.c is #included. */
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
static IpcObject *mk_storage(const char *text) {
    IpcObject *st = storage_new(strlen(text)); memcpy(((Storage *)st->state)->data, text, strlen(text)); return st;
}
static void call_obj(IpcObject *o, unsigned cmd, const void *in, uint32_t n, IpcObject *arg, IpcCall *c) {
    memset(c, 0, sizeof *c); c->in = in; c->in_size = n;
    if (arg) { c->in_objects[0] = arg; c->nin_objects = 1; }
    svc_call_c(o, cmd, c);
}
/* PopLaunchParameter(UserChannel): the storage's text, "" when the channel answers NoDataInChannel. */
static const char *pop_user(IpcObject *af, char *buf, size_t cap) {
    uint32_t kind = 1; IpcCall c; svc_call(af, 1, &kind, 4, &c);
    buf[0] = 0;
    if (c.result == APPLET_R_NO_DATA_IN_CHANNEL || !c.nout_objects) return buf;
    Storage *st = c.out_objects[0]->state; size_t n = st->size < cap - 1 ? (size_t)st->size : cap - 1;
    memcpy(buf, st->data, n); buf[n] = 0; return buf;
}
/* What a new process sees: the user channel is read again from NAPPLE_USER_CHANNEL. */
static void as_new_process(void) {
    pthread_mutex_lock(&lock); uc_clear_locked(); user_channel_loaded = 0; pthread_mutex_unlock(&lock);
}
static unsigned g_relaunches; static int g_relaunch_answer;
static char g_seen_channel[1100], g_seen_prev[16];
static int test_relaunch(void) {
    g_relaunches++;
    const char *e = getenv(ENV_USER_CHANNEL), *p = getenv(ENV_PREVIOUS_PROGRAM_INDEX);
    snprintf(g_seen_channel, sizeof g_seen_channel, "%s", e ? e : "");
    snprintf(g_seen_prev, sizeof g_seen_prev, "%s", p ? p : "");
    return g_relaunch_answer;
}
static uint32_t next_message(IpcObject *csg) { IpcCall c; svc_call(csg, 1, NULL, 0, &c); return c.result ? 0 : SVC_OUT(&c, uint32_t, 0); }
static int test_frame(uint8_t *rgba, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { uint8_t *p = rgba + ((size_t)y * w + x) * 4; p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 60; p[3] = 255; }
    return 1;
}
static IpcObject *fake_jit_open(void) { static const IpcCmd none[] = {{0, "(none)", csg_noop, IPC_STUB}}; return IPC_OBJECT("jit:u", none, NULL, NULL); }

int main(void) {
    IpcObject *oe = oe_open(); IpcCall c; char buf[64];
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    IpcObject *csg = get(proxy, 0), *sc = get(proxy, 1), *lac = get(proxy, 11), *af = get(proxy, 20);
    CHECK(csg && sc && lac && af, "proxy hands out the sub-interfaces");
    while (next_message(csg)) {} /* drop the initial focus message */
    applet_relaunch = test_relaunch;
    unsetenv(ENV_USER_CHANNEL); unsetenv(ENV_PREVIOUS_PROGRAM_INDEX);
    g_program_id = 0x0100AAAA00010000ull;

    /* ---- user channel: Unpop puts at the front, Pop(1) takes from the front, Clear empties ---- */
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), ""), "the user channel starts empty (NoDataInChannel)");
    call_obj(af, 122, NULL, 0, mk_storage("first"), &c); CHECK(c.result == 0, "UnpopToUserChannel(first)");
    call_obj(af, 122, NULL, 0, mk_storage("second"), &c); CHECK(c.result == 0, "UnpopToUserChannel(second)");
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), "second"), "PopLaunchParameter(UserChannel) takes the last unpopped storage first (%s)", buf);
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), "first"), "then the earlier one (%s)", buf);
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), ""), "then the channel is empty");
    call_obj(af, 122, NULL, 0, mk_storage("x"), &c); svc_call(af, 121, NULL, 0, &c);
    CHECK(c.result == 0 && !strcmp(pop_user(af, buf, sizeof buf), ""), "ClearUserChannel empties it");
    call_obj(af, 122, NULL, 0, NULL, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS, "UnpopToUserChannel without a storage: invalid parameters");
    { IpcObject *big = storage_new(0x1001); call_obj(af, 122, NULL, 0, big, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS, "a storage above 0x1000 bytes is refused"); }
    { uint32_t kind = 2; svc_call(af, 1, &kind, 4, &c); CHECK(c.result == 0 && c.nout_objects == 1, "PopLaunchParameter(PreselectedUser) still answers the account"); }

    /* ---- ExecuteProgram: refusals, then RestartProgram with the user channel carried over ---- */
    struct { uint32_t kind, pad; uint64_t value; } ep = {1, 0, 0};
    svc_call(af, 120, &ep, sizeof ep, &c); CHECK(c.result == HZ_R_AM_DEBUG_MODE_NOT_ENABLED && c.result == 0x79c80, "JumpToSubApplicationProgramForDevelopment: DebugMode not enabled");
    ep.kind = 3; svc_call(af, 120, &ep, sizeof ep, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS && c.result == 0x3f480, "unknown ProgramSpecifyKind: invalid parameters");
    ep.kind = 0; ep.value = 1; svc_call(af, 120, &ep, sizeof ep, &c); CHECK(c.result == HZ_R_AM_TITLE_NOT_FOUND && c.result == 0x4a80, "another program index of the title: not on this host");
    CHECK(g_relaunches == 0, "no restart for a refused request");
    call_obj(af, 121, NULL, 0, NULL, &c); call_obj(af, 122, NULL, 0, mk_storage("hello"), &c);
    ep.kind = 2; ep.value = 0; svc_call(af, 120, &ep, sizeof ep, &c);
    CHECK(c.result == 0 && g_relaunches == 1 && g_seen_channel[0] && access(g_seen_channel, R_OK) == 0 && !strcmp(g_seen_prev, "0"),
          "RestartProgram: the application is started again with the user channel in a file and previous program index 0 (%s, %s)", g_seen_channel, g_seen_prev);
    as_new_process();
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), "hello"), "the new process pops the storage it was given (%s)", buf);
    CHECK(access(g_seen_channel, F_OK) != 0 && !getenv(ENV_USER_CHANNEL), "the hand-over file is removed once read");
    svc_call(af, 123, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "GetPreviousProgramIndex after ExecuteProgram: 0 (was %d)", SVC_OUT(&c, int32_t, 0));
    ep.kind = 0; ep.value = 0; svc_call(af, 120, &ep, sizeof ep, &c); CHECK(c.result == 0 && g_relaunches == 2 && !g_seen_channel[0], "ExecuteProgram(own index 0): restarted, empty user channel");
    g_relaunch_answer = -1; ep.kind = 2; svc_call(af, 120, &ep, sizeof ep, &c);
    CHECK(c.result == 0x1480 && !getenv(ENV_PREVIOUS_PROGRAM_INDEX), "a host that cannot restart answers unavailable and leaves nothing behind");
    g_relaunch_answer = 0;
    svc_call(af, 120, NULL, 0, &c); CHECK(c.result == 0x1480, "a request without arguments: unavailable");

    /* ---- CreateApplication(AndPush)AndRequestToStart: only the running application (id 0 or its own id) ---- */
    struct { uint64_t id; } ca = {0x0100BBBB00020000ull};
    call_obj(af, 10, &ca, 8, mk_storage("arg"), &c); CHECK(c.result == HZ_R_AM_TITLE_NOT_FOUND, "another application: not on this host");
    svc_call(af, 12, &ca, 8, &c); CHECK(c.result == HZ_R_AM_TITLE_NOT_FOUND, "the same for 12");
    ca.id = 0; call_obj(af, 10, &ca, 8, NULL, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS, "10 without its storage: invalid parameters");
    unsigned before = g_relaunches;
    call_obj(af, 10, &ca, 8, mk_storage("launch-arg"), &c);
    CHECK(c.result == 0 && g_relaunches == before + 1 && !g_seen_prev[0], "id 0: the current application is relaunched (no previous program index)");
    as_new_process(); unsetenv(ENV_PREVIOUS_PROGRAM_INDEX);
    CHECK(!strcmp(pop_user(af, buf, sizeof buf), "launch-arg"), "the relaunched application pops the pushed storage (%s)", buf);
    svc_call(af, 123, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == -1, "GetPreviousProgramIndex: -1 after an application relaunch");
    call_obj(af, 122, NULL, 0, mk_storage("left"), &c);
    ca.id = g_program_id; svc_call(af, 12, &ca, 8, &c);
    CHECK(c.result == 0 && g_relaunches == before + 2 && !g_seen_channel[0], "12 with its own id: relaunched with an empty user channel (the unpopped storage is not given)");

    /* ---- HDCP: activation drives the state, each change signals the event ---- */
    svc_call(csg, 63, NULL, 0, &c); uint32_t hev = c.nout_copy ? c.out_copy[0] : 0;
    CHECK(hev && !event_is_signaled(hev), "GetHdcpAuthenticationStateChangeEvent: quiet before any change");
    svc_call(csg, 62, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "unauthenticated before activation");
    uint8_t on = 1, off = 0; svc_call(af, 170, &on, 1, &c);
    CHECK(c.result == 0 && event_is_signaled(hev), "SetHdcpAuthenticationActivated(true) signals the change event");
    svc_call(csg, 62, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 2, "authenticated once activated");
    svc_call(af, 170, &off, 1, &c); svc_call(csg, 62, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "deactivated: unauthenticated again");

    /* ---- PerformSystemButtonPressingIfInFocus ---- */
    uint32_t b = 3; svc_call(csg, 80, &b, 4, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS, "power button types are only for PerformSystemButtonPressing");
    b = 0; svc_call(csg, 80, &b, 4, &c); CHECK(c.result == HZ_R_AM_INVALID_PARAMETERS, "type 0 is not a button");
    b = 1; svc_call(csg, 80, &b, 4, &c); CHECK(c.result == 0 && next_message(csg) == 0, "Home short press, not handled by the application: no message (no Home menu here)");
    svc_call(csg, 100, &on, 1, &c); CHECK(AS()->handling_home_short_press == 1, "SetHandlingHomeButtonShortPressedEnabled kept");
    svc_call(csg, 80, &b, 4, &c); CHECK(next_message(csg) == MSG_DETECT_SHORT_HOME, "handled: DetectShortPressingHomeButton (20)");
    svc_call(csg, 100, &off, 1, &c); svc_call(csg, 80, &b, 4, &c); CHECK(next_message(csg) == 0, "handling turned off: no message again");
    b = 2; svc_call(af, 30, NULL, 0, &c); svc_call(csg, 80, &b, 4, &c);
    CHECK(next_message(csg) == MSG_DETECT_LONG_HOME, "Home long press while blocked (BeginBlockingHomeButtonShortAndLongPressed): message 21");
    b = 1; svc_call(csg, 80, &b, 4, &c); CHECK(next_message(csg) == MSG_DETECT_SHORT_HOME, "and the short press too (message 20)");
    svc_call(af, 31, NULL, 0, &c);
    b = 6; svc_call(sc, 18, &on, 1, &c); svc_call(csg, 80, &b, 4, &c); CHECK(next_message(csg) == MSG_DETECT_SHORT_CAPTURE, "capture short press, message requested: 90");
    svc_call(sc, 18, &off, 1, &c);
    applet_capture_set_source(test_frame); static CapsState caps; applet_album_set_source(&caps);
    unsigned saved = AS()->screenshots_saved; svc_call(csg, 80, &b, 4, &c);
    CHECK(c.result == 0 && AS()->screenshots_saved == saved + 1 && next_message(csg) == 0, "capture short press otherwise: a screenshot into the album");
    b = 7; svc_call(csg, 80, &b, 4, &c); CHECK(c.result == 0 && next_message(csg) == 0 && AS()->screenshots_saved == saved + 1, "capture long press: nothing (no recording)");
    applet_set_focus(0); while (next_message(csg)) {}
    b = 6; svc_call(csg, 80, &b, 4, &c); CHECK(c.result == 0 && AS()->screenshots_saved == saved + 1, "out of focus: success, nothing happens");
    applet_set_focus(1); while (next_message(csg)) {}

    /* ---- PrepareForJit: 0x408 without a jit sysmodule, success with one ---- */
    svc_call(af, 1001, NULL, 0, &c); CHECK(c.result == HZ_R_LR_PROGRAM_NOT_FOUND && c.result == 0x408 && !AS()->jit_prepared, "PrepareForJit without jit:u: 0x408");
    ipc_register_service("jit:u", fake_jit_open);
    svc_call(af, 1001, NULL, 0, &c); CHECK(c.result == 0 && AS()->jit_prepared, "PrepareForJit with a jit:u service: prepared");

    /* ---- ILibraryAppletAccessor 80 ---- */
    { uint32_t in[2] = {0x10, 0}; svc_call(lac, 0, in, 8, &c); IpcObject *la = c.nout_objects ? c.out_objects[0] : NULL;
      CHECK(la != NULL, "CreateLibraryApplet");
      if (la) { svc_call(la, 80, NULL, 0, &c); CHECK(c.result == 0 && ((LibApplet *)la->state)->foreground_requested, "RequestForLibraryAppletToGetForeground recorded"); } }

    /* ---- SetDelayTimeToAbortOnGpuError: the application is aborted that long after a GPU error (forked children) ---- */
    for (int with_delay = 1; with_delay >= 0; with_delay--) {
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        pid_t pid = fork();
        if (pid == 0) {
            if (with_delay) { int64_t ns = 200000000; IpcCall cc; svc_call(af, 131, &ns, 8, &cc); }
            applet_signal_gpu_error();
            usleep(1500000);
            _exit(0); /* reached only without an abort */
        }
        int ws = 0; waitpid(pid, &ws, 0); clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (with_delay) CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 1 && secs >= 0.19 && secs < 1.4, "GPU error with a 200 ms delay: aborted after it (status %#x, %.2f s)", ws, secs);
        else CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0 && secs >= 1.4, "GPU error without a delay: only the event fires (status %#x, %.2f s)", ws, secs);
    }
    { int64_t neg = -5; svc_call(af, 131, &neg, 8, &c); CHECK(c.result == 0 && gpu_abort_delay_ns == 0, "a negative delay counts as 0"); }

    printf("{\"test\":\"svc_applet_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
