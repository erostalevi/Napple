/* Unit tests: the generic library applet host (applets-host, 2026-10-27): the four built-in applets on their success and cancel paths, the inline keyboard, the
 * interactive storage channel, the firmware applet table driven by a stand-in program (NAPPLE_APPLET_EXE) for every id, and the rows that were stubs (sleep lock,
 * fatal section, capture images, illuminance, GPU time slice, events). applet.c is #included; on Linux the frontend/gfx functions it calls are stubbed here. */
/* The three frontend calls the tests steer are renamed for this file so the same test links against the real frontend on macOS and the stubs on Linux. */
#define frontend_text_input t_frontend_text_input
#define frontend_alert t_frontend_alert
#define frontend_connected_pads t_frontend_connected_pads
#include "../src/services/applet.c"
#include "svc_test.h"
#ifdef __linux__
const uint8_t g_user_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
void host_display_version(char out[16]) { snprintf(out, 16, "1.0.0"); }
uint64_t host_desired_language(void) { return 0x53552d6e65ULL; }
void ipc_trace_guest(unsigned n) { (void)n; }
void napple_display_game_size(uint32_t w, uint32_t h, uint32_t *gw, uint32_t *gh) { *gw = w; *gh = h; }
size_t napple_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap) { size_t k = 0; for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)u[i]; if (cap) out[k] = 0; return k; }
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) { size_t k = 0; for (; s[k] && k < cap; k++) out[k] = (uint8_t)s[k]; return k; }
#endif
static const char *g_typed; /* what the user types into the text bar (NULL = cancel) */
int t_frontend_text_input(const char *a, const char *b, const char *c, const char *d, unsigned e, unsigned f, int g, int h, char *out, unsigned cap) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    if (!g_typed) { if (cap) out[0] = 0; return 0; }
    snprintf(out, cap, "%s", g_typed); return 1;
}
static int g_alerts;
void t_frontend_alert(const char *t, const char *m) { (void)t; (void)m; g_alerts++; }
static int g_pads = 1;
int t_frontend_connected_pads(void) { return g_pads; }

static IpcObject *create_applet(IpcObject *lac, uint32_t id, uint32_t mode) {
    uint32_t in[2] = {id, mode}; IpcCall c; svc_call(lac, 0, in, 8, &c); return c.nout_objects ? c.out_objects[0] : NULL;
}
static void push_cmd(IpcObject *la, unsigned cmd, const uint8_t *data, uint64_t size) {
    IpcObject *st = storage_new(size); if (data) memcpy(((Storage *)st->state)->data, data, size);
    IpcCall c; memset(&c, 0, sizeof c); c.in_objects[0] = st; c.nin_objects = 1; svc_call_c(la, cmd, &c); CHECK(c.result == 0, "push cmd %u", cmd);
}
static void push(IpcObject *la, const uint8_t *data, uint64_t size) { push_cmd(la, 100, data, size); }
static void push_inter(IpcObject *la, const uint8_t *data, uint64_t size) { push_cmd(la, 103, data, size); }
static uint32_t start_result(IpcObject *la) {
    IpcCall c; svc_call(la, 10, NULL, 0, &c); CHECK(c.result == 0, "Start"); svc_call(la, 30, NULL, 0, &c); return c.result;
}
/* pop the next (interactive) output storage; returns its size or -1 when there is none */
static long pop_cmd(IpcObject *la, unsigned cmd, uint8_t *out) {
    IpcCall c; svc_call(la, cmd, NULL, 0, &c);
    if (c.result != 0 || !c.nout_objects) return -1;
    Storage *s = c.out_objects[0]->state; memcpy(out, s->data, s->size); return (long)s->size;
}
static long pop_out(IpcObject *la, uint8_t *out) { return pop_cmd(la, 101, out); }
static long pop_inter(IpcObject *la, uint8_t *out) { return pop_cmd(la, 104, out); }
static int completed(IpcObject *la) { IpcCall c; svc_call(la, 1, NULL, 0, &c); return c.out[0]; }

static int g_hook_calls; static char g_hook_code[40];
static void error_hook(const AppletErrorInfo *e, const char *code, const char *desc) { (void)e; (void)desc; g_hook_calls++; snprintf(g_hook_code, sizeof g_hook_code, "%s", code); }
static unsigned lister3(uint8_t (*u)[16], unsigned max) { for (unsigned i = 0; i < 3 && i < max; i++) { memset(u[i], 0, 16); u[i][0] = (uint8_t)(0x10 + i); } return 3; }
static int frame_source(uint8_t *rgba, uint32_t w, uint32_t h) { for (size_t i = 0; i < (size_t)w * h * 4; i += 4) { rgba[i] = 10; rgba[i + 1] = 20; rgba[i + 2] = 30; rgba[i + 3] = 255; } return 1; }
/* a registered handler sees the interactive input and answers on both channels */
static int h_inter(LibAppletCall *call) {
    uint8_t first = call->ninteractive_in ? call->interactive_in[0].data[0] : 0;
    uint8_t o[2] = {0x77, first};
    libapplet_call_set_out(call, o, 2);
    libapplet_call_push_interactive_out(call, o, 2);
    uint8_t o2[3] = {0x78, first, 1};
    libapplet_call_push_interactive_out(call, o2, 3);
    return 1;
}
static IpcObject *get(IpcObject *proxy, unsigned cmd) { IpcCall c; svc_call(proxy, cmd, NULL, 0, &c); return c.nout_objects ? c.out_objects[0] : NULL; }

int main(void) {
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    IpcObject *csg = get(proxy, 0), *sc = get(proxy, 1), *wc = get(proxy, 2), *dcn = get(proxy, 4), *lac = get(proxy, 11), *af = get(proxy, 20);
    uint8_t out[0x800]; long n;
    uint8_t common[0x20] = {0};

    /* ---- built-in applets: success and cancel paths ---- */
    { /* swkbd (foreground): typed text is returned; no text = cancel */
        uint8_t cfg[0x4C8] = {0}; uint32_t v = 8; memcpy(cfg + 0x3AC, &v, 4);
        g_typed = "hello world";
        IpcObject *la = create_applet(lac, LIBAPPLET_ID_SWKBD, 0); push(la, common, 0x20); push(la, cfg, sizeof cfg);
        CHECK(start_result(la) == 0 && completed(la), "swkbd: Start completes");
        n = pop_out(la, out); CHECK(n == 0x800 && *(uint32_t *)out == 0 && !strncmp((char *)out + 4, "hello wo", 9), "swkbd success: text cut to the max length (%s)", (char *)out + 4);
        g_typed = NULL; la = create_applet(lac, LIBAPPLET_ID_SWKBD, 0); push(la, common, 0x20); push(la, cfg, sizeof cfg);
        start_result(la); n = pop_out(la, out); CHECK(n == 0x800 && *(uint32_t *)out == 1, "swkbd cancel: CloseResult 1");
    }
    { /* controller support */
        uint8_t arg[0x21c] = {0}; arg[0] = 2; arg[1] = 4; g_pads = 3;
        IpcObject *la = create_applet(lac, LIBAPPLET_ID_CONTROLLER, 0); push(la, common, 0x20); push(la, arg, sizeof arg);
        CHECK(start_result(la) == 0, "controller: Start"); n = pop_out(la, out);
        CHECK(n == 12 && *(int32_t *)out == 3 && *(uint32_t *)(out + 8) == APPLET_CONTROLLER_OK, "controller success: the 3 connected pads, ok");
        g_pads = 1; la = create_applet(lac, LIBAPPLET_ID_CONTROLLER, 0); push(la, common, 0x20); push(la, arg, sizeof arg);
        start_result(la); n = pop_out(la, out); CHECK(*(int32_t *)out == 2 && *(uint32_t *)(out + 8) == APPLET_CONTROLLER_OK, "controller: fewer pads than the minimum still reports the minimum");
        setenv("NAPPLE_CONTROLLER_SUPPORT", "strict", 1);
        la = create_applet(lac, LIBAPPLET_ID_CONTROLLER, 0); push(la, common, 0x20); push(la, arg, sizeof arg);
        start_result(la); n = pop_out(la, out); CHECK(*(int32_t *)out == 0 && *(uint32_t *)(out + 8) == APPLET_CONTROLLER_CANCELLED, "controller cancel: strict mode with too few pads");
        setenv("NAPPLE_CONTROLLER_SUPPORT", "cancel", 1); g_pads = 4;
        la = create_applet(lac, LIBAPPLET_ID_CONTROLLER, 0); push(la, common, 0x20); push(la, arg, sizeof arg);
        start_result(la); n = pop_out(la, out); CHECK(*(uint32_t *)(out + 8) == APPLET_CONTROLLER_CANCELLED, "controller cancel: NAPPLE_CONTROLLER_SUPPORT=cancel");
        unsetenv("NAPPLE_CONTROLLER_SUPPORT"); g_pads = 1;
    }
    { /* player select with several profiles */
        applet_set_profile_lister(lister3);
        uint8_t sel[0xa0] = {0};
        setenv("NAPPLE_PLAYER_SELECT", "2", 1);
        IpcObject *la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT, 0); push(la, common, 0x20); push(la, sel, sizeof sel);
        start_result(la); n = pop_out(la, out); CHECK(n == 0x18 && *(uint64_t *)out == 0 && out[8] == 0x12, "player select: profile 2 of 3");
        setenv("NAPPLE_PLAYER_SELECT", "9", 1);
        la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT, 0); push(la, common, 0x20); push(la, sel, sizeof sel);
        start_result(la); n = pop_out(la, out); CHECK(out[8] == 0x10, "player select: an index out of range falls back to the first profile");
        setenv("NAPPLE_PLAYER_SELECT", "cancel", 1);
        la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT, 0); push(la, common, 0x20); push(la, sel, sizeof sel);
        start_result(la); n = pop_out(la, out); CHECK(*(uint64_t *)out == APPLET_PLAYER_SELECT_CANCELLED && !*(uint64_t *)(out + 8), "player select cancel");
        unsetenv("NAPPLE_PLAYER_SELECT"); applet_set_profile_lister(NULL);
        la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT, 0); push(la, common, 0x20); push(la, sel, sizeof sel);
        start_result(la); n = pop_out(la, out); CHECK(out[8] == g_user_id[0], "player select without an account lister: the local user");
    }
    { /* error applet: the frontend hook, else the alert */
        uint8_t hdr[0x10] = {1, 1}, arg[0x1000] = {0x7b, 0x01, 0, 0, 0x2a};
        int a0 = g_alerts;
        IpcObject *la = create_applet(lac, LIBAPPLET_ID_ERROR, 0); push(la, common, 0x20); push(la, hdr, sizeof hdr); push(la, arg, sizeof arg);
        CHECK(start_result(la) == 0 && pop_out(la, out) < 0 && g_alerts == a0 + 1, "error applet without a hook: dismissed, the generic alert is raised");
        applet_set_error_hook(error_hook);
        la = create_applet(lac, LIBAPPLET_ID_ERROR, 0); push(la, common, 0x20); push(la, hdr, sizeof hdr); push(la, arg, sizeof arg);
        CHECK(start_result(la) == 0 && g_hook_calls == 1 && !strcmp(g_hook_code, "0379-0042") && g_alerts == a0 + 1, "error applet with a hook: the hook gets the code text, no alert (%s)", g_hook_code);
        applet_set_error_hook(NULL);
    }

    /* ---- inline keyboard over the interactive channel ---- */
    {
        uint8_t cfg[0x4C8] = {0}; uint32_t v = 6; memcpy(cfg + 0x3AC, &v, 4); uint8_t fin[8] = {APPLET_SWKBD_CMD_FINALIZE}, calc[16] = {APPLET_SWKBD_CMD_CALC};
        IpcObject *la = create_applet(lac, LIBAPPLET_ID_SWKBD, 1); push(la, common, 0x20); push(la, cfg, sizeof cfg);
        IpcCall c2; svc_call(la, 10, NULL, 0, &c2);
        CHECK(!completed(la), "swkbd inline: stays running after Start");
        svc_call(lac, 2, NULL, 0, &c2); CHECK(c2.out[0] == 1, "AreAnyLibraryAppletsLeft sees the running keyboard");
        svc_call(la, 106, NULL, 0, &c2); CHECK(c2.nout_copy == 1 && event_is_signaled(c2.out_copy[0]), "interactive out event is signalled once a reply is queued");
        n = pop_inter(la, out); CHECK(n > 8 && *(uint32_t *)out == APPLET_SWKBD_REPLY_FINISHED_INITIALIZE, "swkbd inline: FinishedInitialize");
        CHECK(pop_inter(la, out) < 0, "interactive queue empty");
        g_typed = "abcdefghij"; push_inter(la, calc, sizeof calc);
        n = pop_inter(la, out); CHECK(*(uint32_t *)out == APPLET_SWKBD_REPLY_CHANGED_STRING_UTF8 && !strcmp((char *)out + 8, "abcdef"), "swkbd inline: ChangedStringUtf8 with the cut text (%s)", (char *)out + 8);
        n = pop_inter(la, out); CHECK(*(uint32_t *)out == APPLET_SWKBD_REPLY_DECIDED_ENTER_UTF8 && !strcmp((char *)out + 8, "abcdef"), "swkbd inline: DecidedEnterUtf8");
        g_typed = NULL; push_inter(la, calc, sizeof calc);
        n = pop_inter(la, out); CHECK(*(uint32_t *)out == APPLET_SWKBD_REPLY_DECIDED_CANCEL, "swkbd inline cancel: DecidedCancel");
        push_inter(la, fin, sizeof fin); CHECK(completed(la), "swkbd inline: Finalize completes the applet");
        svc_call(lac, 2, NULL, 0, &c2); CHECK(c2.out[0] == 0, "no library applet left after Finalize");
        la = create_applet(lac, LIBAPPLET_ID_SWKBD, 1); push(la, common, 0x20); push(la, cfg, sizeof cfg); svc_call(la, 10, NULL, 0, &c2);
        svc_call(la, 20, NULL, 0, &c2); CHECK(completed(la), "swkbd inline: RequestExit ends it"); svc_call(la, 110, NULL, 0, &c2); CHECK(c2.out[0] == 1, "NeedsToExitProcess after RequestExit");
        la = create_applet(lac, LIBAPPLET_ID_SWKBD, 2); push(la, cfg, sizeof cfg); svc_call(la, 10, NULL, 0, &c2); svc_call(lac, 1, NULL, 0, &c2);
        CHECK(completed(la), "TerminateAllLibraryApplets ends a running keyboard");
    }

    /* ---- the firmware applet table: a stand-in program per id, with the interactive channel ---- */
    {
        char root[96], cmd[1500];
        snprintf(root, sizeof root, "/tmp/napple-applet-hosttbl-%d", (int)getpid());
        snprintf(cmd, sizeof cmd, "mkdir -p %s/game/b1 && printf '#!/bin/sh\\nprintf \"OK$NAPPLE_APPLET_ID\" > \"$NAPPLE_APPLET_OUT/out0.bin\"; cat \"$NAPPLE_APPLET_IN/in0.bin\" >> \"$NAPPLE_APPLET_OUT/out0.bin\"; "
                 "printf IO > \"$NAPPLE_APPLET_OUT/iout0.bin\"; cat \"$NAPPLE_APPLET_IN/iin0.bin\" >> \"$NAPPLE_APPLET_OUT/iout0.bin\"; echo 0 > \"$NAPPLE_APPLET_OUT/result.txt\"\\n' > %s/fake.sh && chmod +x %s/fake.sh", root, root, root);
        CHECK(system(cmd) == 0, "host table scaffolding");
        static char dd[160], fk[160]; snprintf(dd, sizeof dd, "%s/game/b1", root); snprintf(fk, sizeof fk, "%s/fake.sh", root);
        g_cfg.data_dir = dd; g_cfg.save_root = root;
        setenv("NAPPLE_APPLET_EXE", fk, 1);
        const uint32_t ids[] = {LIBAPPLET_ID_SWKBD, LIBAPPLET_ID_CONTROLLER, LIBAPPLET_ID_ERROR, LIBAPPLET_ID_PLAYER_SELECT, 0x12};
        for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
            const LibAppletProgram *p = libapplet_program_for(ids[i]);
            CHECK(p != NULL, "program table has %#x", ids[i]);
            if (!p) continue;
            snprintf(cmd, sizeof cmd, "mkdir -p %s/game/applets/%s/b2 && echo '{}' > %s/game/applets/%s/b2/manifest.json", root, p->program_id, root, p->program_id);
            CHECK(system(cmd) == 0, "applet dir for %s", p->name);
            uint8_t in[8] = {(uint8_t)ids[i], 7}, iin[4] = {0xA5, 0x5A};
            /* not enabled (only the Mii Maker is by default): the built-in answer, the stand-in is not run */
            unsetenv("NAPPLE_FIRMWARE_APPLET_IDS"); unsetenv("NAPPLE_FIRMWARE_APPLETS");
            IpcObject *la = create_applet(lac, ids[i], 0); push(la, in, sizeof in); push_inter(la, iin, sizeof iin);
            uint32_t r = start_result(la); n = pop_out(la, out);
            if (ids[i] == 0x12) CHECK(r == 0 && n == 2 + 4 + (long)sizeof in && !memcmp(out, "OK0x12", 6) || r == 0 && n >= 8 && out[0] == 'O', "%s: firmware by default (%ld bytes)", p->name, n);
            else CHECK(!(n >= 2 && out[0] == 'O' && out[1] == 'K'), "%s: HLE answers when the firmware applet is not enabled", p->name);
            /* enabled by id: the stand-in runs, its output and its interactive output come back */
            char idl[32]; snprintf(idl, sizeof idl, "%#x", ids[i]); setenv("NAPPLE_FIRMWARE_APPLET_IDS", idl, 1);
            la = create_applet(lac, ids[i], 0); push(la, in, sizeof in); push_inter(la, iin, sizeof iin);
            r = start_result(la); n = pop_out(la, out);
            CHECK(r == 0 && n > 8 && out[0] == 'O' && out[1] == 'K' && !memcmp(out + n - 8, in, 8), "%s: firmware applet answers (out %ld bytes)", p->name, n);
            IpcCall ev; svc_call(la, 106, NULL, 0, &ev);
            CHECK(ev.nout_copy == 1 && event_is_signaled(ev.out_copy[0]), "%s: interactive out event signalled", p->name);
            long ni = pop_inter(la, out); CHECK(ni == 2 + 4 && out[0] == 'I' && out[1] == 'O' && out[2] == 0xA5 && out[3] == 0x5A, "%s: interactive in/out round trip through the firmware applet (%ld bytes)", p->name, ni);
            CHECK(pop_inter(la, out) < 0, "%s: no second interactive storage", p->name);
            /* a name works too */
            setenv("NAPPLE_FIRMWARE_APPLET_IDS", p->name, 1);
            la = create_applet(lac, ids[i], 0); push(la, in, sizeof in);
            start_result(la); n = pop_out(la, out); CHECK(n > 8 && out[0] == 'O', "%s: enabled by name", p->name);
            unsetenv("NAPPLE_FIRMWARE_APPLET_IDS");
        }
        setenv("NAPPLE_FIRMWARE_APPLETS", "all", 1);
        { uint8_t in[8] = {1}; IpcObject *la = create_applet(lac, LIBAPPLET_ID_ERROR, 0); push(la, in, sizeof in); start_result(la); n = pop_out(la, out);
          CHECK(n > 8 && out[0] == 'O', "NAPPLE_FIRMWARE_APPLETS=all runs every table entry"); }
        setenv("NAPPLE_FIRMWARE_APPLETS", "0", 1);
        { uint8_t in[8] = {1}; IpcObject *la = create_applet(lac, 0x12, 0); push(la, in, sizeof in); CHECK(start_result(la) == 0x1480, "NAPPLE_FIRMWARE_APPLETS=0 turns the Mii Maker off"); }
        unsetenv("NAPPLE_FIRMWARE_APPLETS");
        /* the applet leaves a failure result: GetResult reports it */
        snprintf(cmd, sizeof cmd, "printf '#!/bin/sh\\necho 2005 > \"$NAPPLE_APPLET_OUT/result.txt\"\\n' > %s/fail.sh && chmod +x %s/fail.sh", root, root);
        CHECK(system(cmd) == 0, "fail.sh"); snprintf(fk, sizeof fk, "%s/fail.sh", root); setenv("NAPPLE_APPLET_EXE", fk, 1);
        { uint8_t in[8] = {1}; IpcObject *la = create_applet(lac, 0x12, 0); push(la, in, sizeof in); CHECK(start_result(la) == 2005, "an applet that exits with a result reports it from GetResult"); }
        unsetenv("NAPPLE_APPLET_EXE");
        snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)!system(cmd);
    }
    { const LibAppletProgram *p = libapplet_program_for(0x11); CHECK(p && !strcmp(p->program_id, "0100000000001008") && !p->default_firmware, "swkbd entry");
      CHECK(libapplet_program_for(0x99) == NULL && libapplet_program_for(0x12)->default_firmware, "table lookups");
      CHECK(libapplet_firmware_enabled(libapplet_program_for(0x12), NULL, NULL) && !libapplet_firmware_enabled(p, NULL, NULL) && libapplet_firmware_enabled(p, NULL, "0x0c, swkbd")
            && !libapplet_firmware_enabled(p, NULL, "0x0c,error") && libapplet_firmware_enabled(p, "all", NULL) && !libapplet_firmware_enabled(libapplet_program_for(0x12), "0", NULL), "firmware policy"); }

    /* ---- the former stub rows ---- */
    {
        uint8_t one = 1, zero = 0;
        svc_call(csg, 4, NULL, 0, &c); CHECK(applet_sleep_disallowed(), "DisallowToEnterSleep"); svc_call(csg, 3, NULL, 0, &c); CHECK(!applet_sleep_disallowed(), "AllowToEnterSleep");
        svc_call(csg, 13, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "sleep lock event is clear while the lock is not held");
        svc_call(csg, 10, NULL, 0, &c); CHECK(AS()->sleep_lock_held && applet_sleep_disallowed(), "RequestToAcquireSleepLock");
        svc_call(csg, 13, NULL, 0, &c); CHECK(event_is_signaled(c.out_copy[0]), "sleep lock event signalled while held");
        svc_call(csg, 12, NULL, 0, &c); CHECK(AS()->sleep_lock_transient, "ReleaseSleepLockTransiently");
        svc_call(csg, 11, NULL, 0, &c); CHECK(!AS()->sleep_lock_held && !applet_sleep_disallowed(), "ReleaseSleepLock");
        svc_call(csg, 900, &one, 1, &c); CHECK(AS()->request_exit_to_library_applet == 1, "SetRequestExitToLibraryApplet...");
        svc_call(csg, 300, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "platform region defaults to Global");
        svc_call(csg, 61, NULL, 0, &c); uint32_t rev = c.out_copy[0]; CHECK(!event_is_signaled(rev), "resolution change event clear");
        applet_push_message(MSG_OPERATION_MODE_CHANGED); CHECK(event_is_signaled(rev), "an operation mode message wakes the resolution event");

        svc_call(sc, 3, NULL, 0, &c); svc_call(sc, 3, NULL, 0, &c); svc_call(sc, 4, NULL, 0, &c); CHECK(AS()->fatal_section == 1, "fatal section nests");
        svc_call(sc, 4, NULL, 0, &c); svc_call(sc, 4, NULL, 0, &c); CHECK(AS()->fatal_section == 0, "fatal section never underflows");
        uint8_t ident[0x18]; memset(ident, 0x5a, sizeof ident); svc_call(sc, 15, ident, sizeof ident, &c); CHECK(AS()->screenshot_identity_set && AS()->screenshot_identity[5] == 0x5a, "SetScreenShotAppletIdentityInfo");
        svc_call(sc, 50, &one, 1, &c); svc_call(sc, 51, NULL, 0, &c); CHECK(AS()->handles_request_to_display && AS()->approve_to_display_count == 1, "SetHandlesRequestToDisplay / ApproveToDisplay");
        int32_t ov[4 + 1] = {10, 20, 30, 40, 1}; svc_call(sc, 60, ov, 17, &c); CHECK(AS()->auto_sleep_overridden && AS()->auto_sleep_override[2] == 30, "OverrideAutoSleepTimeAndDimmingTime");
        svc_call(sc, 67, NULL, 0, &c); CHECK(c.out[0] == 0, "illuminance sensor absent by default");
        applet_illuminance_set(AS(), 1, 250000.0f); svc_call(sc, 67, NULL, 0, &c); CHECK(c.out[0] == 1, "illuminance present once supplied");
        svc_call(sc, 66, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 100000.0f, "GetCurrentIlluminance clamps to the sensor limit");
        svc_call(sc, 71, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 100000.0f && c.out[4] == 1, "GetCurrentIlluminanceEx flags the over-limit value");
        applet_illuminance_set(AS(), 1, 42.0f); svc_call(sc, 71, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 42.0f && c.out[4] == 0, "illuminance in range");
        uint32_t mmerr = 0x1234; svc_call(sc, 70, &mmerr, 4, &c); CHECK(AS()->last_multimedia_error == 0x1234 && AS()->multimedia_error_reports == 1, "ReportMultimediaError");
        svc_call(sc, 90, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0, "no suspended ticks while in focus");
        svc_call(sc, 91, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "suspended tick event is not signalled by default");
        svc_call(sc, 9, NULL, 0, &c); CHECK(event_is_signaled(c.out_copy[0]), "launchable event is signalled");
        { uint8_t blob[16]; memset(blob, 3, sizeof blob); IpcCall c3; memset(&c3, 0, sizeof c3); svc_set_in_buf(&c3, blob, sizeof blob); svc_call_c(sc, 110, &c3);
          CHECK(AS()->album_user_data_size == 16 && AS()->album_user_data_hash != 0, "SetApplicationAlbumUserData"); }

        svc_call(wc, 11, NULL, 0, &c); CHECK(!AS()->foreground_rights, "ReleaseForegroundRights"); svc_call(wc, 10, NULL, 0, &c); CHECK(AS()->foreground_rights, "AcquireForegroundRights");
        svc_call(wc, 12, &one, 1, &c); CHECK(AS()->reject_background, "RejectToChangeIntoBackground");
        svc_call(wc, 20, &zero, 1, &c); CHECK(!AS()->window_visible, "SetAppletWindowVisibility(false)"); svc_call(sc, 51, NULL, 0, &c); CHECK(AS()->window_visible, "ApproveToDisplay shows the window");
        uint64_t slice = 16000000; svc_call(wc, 21, &slice, 8, &c); CHECK(AS()->gpu_time_slice == 16000000, "SetAppletGpuTimeSlice");

        /* capture images */
        applet_capture_reset();
        static uint8_t img[APPLET_CAP_BYTES];
        IpcCall cc; memset(&cc, 0, sizeof cc); svc_set_out_buf(&cc, img, sizeof img); svc_call_c(dcn, 0, &cc);
        CHECK(cc.result == 0 && cc.out[0] == 0 && img[3] == 255 && img[0] == 0, "no frame source: GetLastForegroundCaptureImage is opaque black, has_frame false");
        applet_capture_set_source(frame_source);
        svc_call(dcn, 1, NULL, 0, &c); memset(&cc, 0, sizeof cc); svc_set_out_buf(&cc, img, sizeof img); svc_call_c(dcn, 5, &cc);
        CHECK(cc.out[0] == 1 && img[0] == 10 && img[1] == 20 && img[2] == 30, "UpdateLastForegroundCaptureImage + GetLastForegroundCaptureImageEx return the frame");
        memset(&cc, 0, sizeof cc); svc_set_out_buf(&cc, img, sizeof img); svc_call_c(dcn, 2, &cc); CHECK(cc.out[0] == 0 && img[0] == 0, "the application slot is still empty");
        uint32_t cp[2] = {APPLET_CAP_LAST_APPLICATION, APPLET_CAP_LAST_FOREGROUND}; svc_call(dcn, 9, cp, 8, &c); CHECK(c.result == 0, "CopyBetweenCaptureBuffers");
        memset(&cc, 0, sizeof cc); svc_set_out_buf(&cc, img, sizeof img); svc_call_c(dcn, 6, &cc); CHECK(cc.out[0] == 1 && img[2] == 30, "the copy reached the application slot");
        cp[0] = 9; svc_call(dcn, 9, cp, 8, &c); CHECK(c.result != 0, "CopyBetweenCaptureBuffers rejects a bad slot");
        svc_call(dcn, 4, NULL, 0, &c); memset(&cc, 0, sizeof cc); svc_set_out_buf(&cc, img, sizeof img); svc_call_c(dcn, 7, &cc); CHECK(cc.out[0] == 1, "caller applet slot");
        { uint32_t perm = APPLET_SCREENSHOT_DISABLE; svc_call(sc, 10, &perm, 4, &c); applet_capture_reset(); applet_capture_set_source(frame_source);
          svc_call(dcn, 8, NULL, 0, &c); CHECK(!applet_capture_has_frame(APPLET_CAP_LAST_APPLICATION), "TakeScreenShotOfOwnLayer honours the disabled permission");
          perm = APPLET_SCREENSHOT_ENABLE; svc_call(sc, 10, &perm, 4, &c); svc_call(dcn, 8, NULL, 0, &c); CHECK(applet_capture_has_frame(APPLET_CAP_LAST_APPLICATION), "TakeScreenShotOfOwnLayer captures"); }
        svc_call(dcn, 10, NULL, 0, &c); CHECK(c.result == 0, "AcquireLastApplicationCaptureBuffer"); svc_call(dcn, 10, NULL, 0, &c); CHECK(c.result != 0, "a second acquire fails while held");
        svc_call(dcn, 11, NULL, 0, &c); svc_call(dcn, 10, NULL, 0, &c); CHECK(c.result == 0, "ReleaseLastApplicationCaptureBuffer lets it be acquired again");
        applet_capture_reset();

        svc_call(af, 66, NULL, 0, &c); svc_call(af, 68, NULL, 0, &c); svc_call(af, 68, NULL, 0, &c);
        CHECK(AS()->game_play_recording_initialized && AS()->game_play_recording_flushes == 2, "game play recording calls are counted");
        svc_call(af, 65, NULL, 0, &c); CHECK(c.out[0] == 0, "game play recording is not supported");
        svc_call(af, 60, &one, 1, &c); CHECK(AS()->media_playback_state == 1, "SetMediaPlaybackStateForApplication");
        svc_call(af, 91, NULL, 0, &c); CHECK(AS()->copyright_frame_buffer_initialized, "InitializeApplicationCopyrightFrameBuffer");
        { uint8_t blob[64] = {0}; IpcCall c3; memset(&c3, 0, sizeof c3); svc_set_in_buf(&c3, blob, sizeof blob); svc_call_c(af, 92, &c3); CHECK(AS()->copyright_image_size == 64, "SetApplicationCopyrightImage"); }
        svc_call(af, 120, NULL, 0, &c); uint32_t gpu = c.out_copy[0]; svc_call(af, 130, NULL, 0, &c); uint32_t fr = c.out_copy[0]; svc_call(af, 210, NULL, 0, &c); uint32_t e210 = c.out_copy[0];
        CHECK(!event_is_signaled(gpu) && !event_is_signaled(fr) && !event_is_signaled(e210), "system events start clear");
        applet_signal_gpu_error(); CHECK(event_is_signaled(gpu) && !event_is_signaled(fr), "applet_signal_gpu_error wakes the GPU error event only");
        applet_signal_friend_invitation(); CHECK(event_is_signaled(fr), "applet_signal_friend_invitation wakes the invitation event");
    }
    {
        libapplet_register_handler(0x13, h_inter);
        IpcObject *la = create_applet(lac, 0x13, 0); uint8_t iin[2] = {0x42, 0};
        push_inter(la, iin, sizeof iin); start_result(la);
        n = pop_out(la, out); CHECK(n == 2 && out[0] == 0x77 && out[1] == 0x42, "handler output");
        n = pop_inter(la, out); CHECK(n == 2 && out[0] == 0x77 && out[1] == 0x42, "handler interactive output 0 (saw the interactive input)");
        n = pop_inter(la, out); CHECK(n == 3 && out[0] == 0x78, "handler interactive output 1");
        CHECK(pop_inter(la, out) < 0, "interactive queue drained");
        libapplet_clear_handlers();
    }
    printf("{\"test\":\"svc_applet_host\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
