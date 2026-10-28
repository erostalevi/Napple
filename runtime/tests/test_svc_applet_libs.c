/* Unit tests: the error, player select and controller support library applets (applet_lib_core.c) driven through the real ILibraryAppletAccessor
 * handlers of applet.c, plus the unknown-applet path. applet.c is #included; on Linux the frontend/gfx functions it calls are stubbed here. */
#include "../src/services/applet.c"
#include "svc_test.h"
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

static int g_ps_ret = -2, g_ps_calls, g_ctl_ret, g_ctl_calls, g_err_calls;
static uint8_t g_ps_uids[8][16];
static unsigned g_ps_count;
static int ps_ui(const uint8_t (*u)[16], unsigned count) { g_ps_calls++; g_ps_count = count; memcpy(g_ps_uids, u, count * 16); return g_ps_ret; }
static int ctl_ui(AppletControllerAnswer *a) { g_ctl_calls++; if (g_ctl_ret) a->players = 3; return g_ctl_ret; }
static char g_err_code[32], g_err_desc[200];
static void err_ui(const AppletErrorInfo *e, const char *code, const char *desc) { (void)e; g_err_calls++; snprintf(g_err_code, sizeof g_err_code, "%s", code); snprintf(g_err_desc, sizeof g_err_desc, "%s", desc); }
static unsigned lister2(uint8_t (*u)[16], unsigned max) { (void)max; memset(u, 0xA1, 32); u[1][0] = 0xB2; return 2; }

/* CreateLibraryApplet(id, mode) on ILibraryAppletCreator */
static IpcObject *create_applet(IpcObject *lac, uint32_t id) {
    uint32_t in[2] = {id, 0}; IpcCall c; svc_call(lac, 0, in, 8, &c); return c.nout_objects ? c.out_objects[0] : NULL;
}
/* PushInData of an IStorage holding `size` bytes (data may be NULL for zeros) */
static void push(IpcObject *la, const uint8_t *data, uint64_t size) {
    IpcObject *st = storage_new(size); if (data) memcpy(((Storage *)st->state)->data, data, size);
    IpcCall c; memset(&c, 0, sizeof c); c.in_objects[0] = st; c.nin_objects = 1; svc_call_c(la, 100, &c); CHECK(c.result == 0, "PushInData");
}
/* Start, then GetResult; returns the result and, when the applet produced output, its bytes in out (size returned via *n) */
static uint32_t run(IpcObject *la, uint8_t *out, uint64_t *n) {
    IpcCall c; svc_call(la, 10, NULL, 0, &c); CHECK(c.result == 0, "Start");
    svc_call(la, 1, NULL, 0, &c); CHECK(c.out[0] == 1, "IsCompleted after Start");
    svc_call(la, 30, NULL, 0, &c); uint32_t r = c.result;
    svc_call(la, 101, NULL, 0, &c);
    if (c.result == 0 && c.nout_objects) { Storage *s = c.out_objects[0]->state; *n = s->size; memcpy(out, s->data, s->size); } else *n = 0;
    return r;
}

static int h_calls;
static int h_echo(LibAppletCall *call) {
    h_calls++;
    if (call->mode == 9) return 0; /* falls through to the built-in answer */
    uint8_t o[4] = {0xAB, (uint8_t)call->nin, call->nin ? call->in[0].data[0] : 0, 0};
    libapplet_call_set_out(call, o, sizeof o);
    return 1;
}
static int h_fail(LibAppletCall *call) { call->unsupported = 1; call->result = 0x1234; return 1; }

int main(void) {
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    svc_call(proxy, 11, NULL, 0, &c); IpcObject *lac = c.out_objects[0];
    uint8_t out[0x200]; uint64_t n;

    /* ---- pure cores ---- */
    AppletControllerAnswer ca;
    applet_controller_answer(NULL, 0, 1, &ca);
    CHECK(ca.players == 1 && ca.min == 1 && ca.max == 4 && !ca.single_mode && ca.selected_id == 0xffffffffu, "controller defaults without an argument");
    uint8_t arg[0x120] = {0}; arg[0] = 2; arg[1] = 4;
    applet_controller_answer(arg, sizeof arg, 1, &ca); CHECK(ca.players == 2 && ca.min == 2 && ca.max == 4, "controller: min players requested");
    arg[0] = 6; arg[1] = 3; applet_controller_answer(arg, sizeof arg, 1, &ca); CHECK(ca.players == 3, "controller: clamped to the game's maximum");
    arg[0] = (uint8_t)-1; arg[1] = 0; arg[5] = 1; applet_controller_answer(arg, sizeof arg, 1, &ca);
    CHECK(ca.players == 1 && ca.max == 4 && ca.single_mode && ca.selected_id == 0, "controller: unset counts, single mode selects Player 1");
    arg[0] = 1; arg[1] = 4; arg[5] = 0; applet_controller_answer(arg, sizeof arg, 3, &ca);
    CHECK(ca.players == 3 && ca.connected == 3 && !ca.short_of_min, "controller: reports the connected pads");
    applet_controller_answer(arg, sizeof arg, 8, &ca); CHECK(ca.players == 4, "controller: capped at the game's maximum");
    arg[0] = 2; applet_controller_answer(arg, sizeof arg, 1, &ca); CHECK(ca.players == 2 && ca.short_of_min, "controller: fewer pads than the minimum is flagged");
    arg[0] = 1; applet_controller_answer(arg, sizeof arg, 0, &ca); CHECK(ca.connected == 1, "controller: 0 pads reads as the keyboard");
    arg[0] = (uint8_t)-1; arg[1] = 0; arg[5] = 1; applet_controller_answer(arg, sizeof arg, 1, &ca);
    uint8_t enc[APPLET_CONTROLLER_OUT_SIZE]; applet_controller_encode(&ca, enc);
    CHECK(*(int32_t *)enc == 1 && *(uint32_t *)(enc + 4) == 0 && *(uint32_t *)(enc + 8) == 0, "controller result encoding");

    uint8_t ps[APPLET_PLAYER_SELECT_OUT_SIZE]; uint8_t uid[16] = {9, 8, 7};
    applet_player_select_answer(uid, 0, ps); CHECK(*(uint64_t *)ps == 0 && !memcmp(ps + 8, uid, 16), "player select: success carries the uid");
    applet_player_select_answer(uid, 1, ps); CHECK(*(uint64_t *)ps == APPLET_PLAYER_SELECT_CANCELLED && !*(uint64_t *)(ps + 8), "player select: cancelled carries no uid");

    { uint8_t uids[2][16] = {{1}, {2}};
      CHECK(applet_player_select_pick(uids, 2, 1, 0) == uids[1], "player select: picks the requested profile");
      CHECK(applet_player_select_pick(uids, 2, 7, 0) == uids[0], "player select: out of range index falls back to the first");
      CHECK(applet_player_select_pick(uids, 2, 0, 1) == NULL && applet_player_select_pick(uids, 0, 0, 0) == NULL, "player select: cancel / no profiles"); }
    { AppletSwkbdConfig sc; char fit[64]; uint8_t k[0x4C8] = {0}; uint32_t v;
      applet_swkbd_parse(NULL, 0, &sc); CHECK(sc.max_len == 0 && sc.min_len == 0 && !sc.numeric, "swkbd: blank config");
      v = 1; memcpy(k, &v, 4); v = 5; memcpy(k + 0x3AC, &v, 4); v = 2; memcpy(k + 0x3B0, &v, 4); v = 1; memcpy(k + 0x3B4, &v, 4);
      applet_swkbd_parse(k, sizeof k, &sc); CHECK(sc.numeric && sc.max_len == 5 && sc.min_len == 2 && sc.password, "swkbd: config parsed");
      CHECK(applet_swkbd_fit(&sc, "abcdefgh", fit, sizeof fit) && !strcmp(fit, "abcde"), "swkbd: cut to the max length");
      CHECK(!applet_swkbd_fit(&sc, "a", fit, sizeof fit), "swkbd: below the minimum cannot be confirmed");
      CHECK(applet_swkbd_fit(&sc, "h\xc3\xa9llo!!", fit, sizeof fit) && !strcmp(fit, "h\xc3\xa9llo"), "swkbd: limit counts code points, not bytes");
      v = 600; memcpy(k + 0x3AC, &v, 4); applet_swkbd_parse(k, sizeof k, &sc); CHECK(sc.max_len == APPLET_SWKBD_MAX_LEN, "swkbd: max length clamped"); }
    AppletErrorInfo e; char d[200]; uint8_t hdr[8] = {1, 1}, ea[8] = {0x78, 0x56, 0x34, 0x12};
    applet_error_parse(hdr, 8, ea, 8, &e); applet_error_describe(&e, d, sizeof d);
    CHECK(e.valid && e.type == 1 && e.jump_flag && e.has_word && e.first_word == 0x12345678u && strstr(d, "0x12345678") && strstr(d, "not done"), "error info: %s", d);
    { char code[32]; uint8_t a2[8] = {0x7b, 1, 0, 0, 0x2a, 0, 0, 0};
      applet_error_parse(hdr, 8, a2, 8, &e); applet_error_code_text(&e, code, sizeof code); CHECK(!strcmp(code, "0379-0042"), "error code text: %s", code);
      hdr[0] = 2; uint8_t r[4] = {0x02, 0x04, 0, 0}; /* module 2, description 2 */ applet_error_parse(hdr, 8, r, 4, &e); applet_error_code_text(&e, code, sizeof code); CHECK(!strcmp(code, "2002-0002"), "error result text: %s", code); }
    applet_error_parse(NULL, 0, NULL, 0, &e); applet_error_describe(&e, d, sizeof d); CHECK(!e.valid && !strcmp(d, "no header"), "error info without header");

    /* ---- through the accessor ---- */
    IpcObject *la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); CHECK(la != NULL, "create player select");
    uint8_t common[0x20] = {0}, selarg[0xa0] = {0}; push(la, common, 0x20); push(la, selarg, 0xa0);
    CHECK(run(la, out, &n) == 0 && n == APPLET_PLAYER_SELECT_OUT_SIZE && *(uint64_t *)out == 0 && !memcmp(out + 8, g_user_id, 16), "player select applet confirms with the user profile");

    la = create_applet(lac, LIBAPPLET_ID_ERROR);
    uint8_t ehdr[0x10] = {0, 0}; uint8_t earg[0x1000] = {0x7b, 0x01}; push(la, common, 0x20); push(la, ehdr, sizeof ehdr); push(la, earg, sizeof earg);
    CHECK(run(la, out, &n) == 0 && n == 0, "error applet is dismissed with no output (not the 0x1480 unsupported result)");

    la = create_applet(lac, LIBAPPLET_ID_CONTROLLER);
    uint8_t carg[0x21c] = {0}; carg[0] = 1; carg[1] = 2; carg[5] = 1; push(la, common, 0x20); push(la, carg, sizeof carg);
    CHECK(run(la, out, &n) == 0 && n == APPLET_CONTROLLER_OUT_SIZE && *(int32_t *)out == 1 && *(uint32_t *)(out + 4) == 0, "controller applet: 1 player, single mode selects Player 1");
    la = create_applet(lac, LIBAPPLET_ID_CONTROLLER); carg[0] = 2; carg[5] = 0; push(la, common, 0x20); push(la, carg, sizeof carg);
    CHECK(run(la, out, &n) == 0 && *(int32_t *)out == 2 && *(uint32_t *)(out + 4) == 0xffffffffu, "controller applet: 2 players requested, no selected id");

    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0);
    setenv("NAPPLE_PLAYER_SELECT", "cancel", 1);
    CHECK(run(la, out, &n) == 0 && n == APPLET_PLAYER_SELECT_OUT_SIZE && *(uint64_t *)out == APPLET_PLAYER_SELECT_CANCELLED && !*(uint64_t *)(out + 8), "player select applet: NAPPLE_PLAYER_SELECT=cancel");
    unsetenv("NAPPLE_PLAYER_SELECT");

    /* ---- the frontend sheets: player select (with a profile list, pick and cancel), controller (confirm adjusts players, cancel), error hook ---- */
    applet_set_profile_lister(lister2); applet_set_player_select_ui(ps_ui);
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0); g_ps_ret = 1;
    CHECK(run(la, out, &n) == 0 && g_ps_calls == 1 && g_ps_count == 2 && *(uint64_t *)out == 0 && out[8] == 0xB2, "player select sheet: the second profile of two is returned");
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0); g_ps_ret = -1;
    CHECK(run(la, out, &n) == 0 && *(uint64_t *)out == APPLET_PLAYER_SELECT_CANCELLED && !*(uint64_t *)(out + 8), "player select sheet: cancel returns the cancelled result");
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0); g_ps_ret = 9;
    CHECK(run(la, out, &n) == 0 && out[8] == 0xA1, "player select sheet: a bad index is the first profile");
    setenv("NAPPLE_PLAYER_SELECT", "1", 1); g_ps_calls = 0; g_ps_ret = -1;
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0);
    CHECK(run(la, out, &n) == 0 && !g_ps_calls && out[8] == 0xB2, "player select: the scripted answer wins over the sheet");
    unsetenv("NAPPLE_PLAYER_SELECT"); applet_set_player_select_ui(NULL); applet_set_profile_lister(NULL);

    applet_set_controller_ui(ctl_ui); g_ctl_ret = 1; carg[0] = 1; carg[1] = 4; carg[5] = 0;
    la = create_applet(lac, LIBAPPLET_ID_CONTROLLER); push(la, common, 0x20); push(la, carg, sizeof carg);
    CHECK(run(la, out, &n) == 0 && g_ctl_calls == 1 && *(int32_t *)out == 3 && *(uint32_t *)(out + 8) == 0, "controller sheet: confirmed with the players the sheet chose");
    g_ctl_ret = 0;
    la = create_applet(lac, LIBAPPLET_ID_CONTROLLER); push(la, common, 0x20); push(la, carg, sizeof carg);
    CHECK(run(la, out, &n) == 0 && *(int32_t *)out == 0 && *(uint32_t *)(out + 4) == 0xffffffffu && *(uint32_t *)(out + 8) == APPLET_CONTROLLER_CANCELLED, "controller sheet: cancel reports 0 players and the cancelled result");
    setenv("NAPPLE_CONTROLLER_SUPPORT", "strict", 1); g_ctl_calls = 0;
    la = create_applet(lac, LIBAPPLET_ID_CONTROLLER); push(la, common, 0x20); push(la, carg, sizeof carg);
    CHECK(run(la, out, &n) == 0 && !g_ctl_calls && *(uint32_t *)(out + 8) == 0, "controller: a scripted policy skips the sheet");
    unsetenv("NAPPLE_CONTROLLER_SUPPORT"); applet_set_controller_ui(NULL);

    applet_set_error_hook(err_ui);
    la = create_applet(lac, LIBAPPLET_ID_ERROR); push(la, common, 0x20); push(la, ehdr, sizeof ehdr); push(la, earg, sizeof earg);
    CHECK(run(la, out, &n) == 0 && g_err_calls == 1 && !strcmp(g_err_code, "2379-0000") && g_err_desc[0], "error hook: code %s, text '%s'", g_err_code, g_err_desc);
    applet_set_error_hook(NULL);

    /* registered handlers answer before the built-in applets */
    CHECK(libapplet_find_handler(0x13) == NULL, "no handler registered at start");
    CHECK(libapplet_register_handler(0x13, h_echo) == 0 && libapplet_find_handler(0x13) == h_echo, "register a handler");
    la = create_applet(lac, 0x13); { uint8_t d[8] = {5}; push(la, d, 8); }
    CHECK(run(la, out, &n) == 0 && n == 4 && out[0] == 0xAB && out[1] == 1 && out[2] == 5 && h_calls == 1, "handler answers a library applet id and its output is popped");
    CHECK(libapplet_register_handler(LIBAPPLET_ID_PLAYER_SELECT, h_fail) == 0, "register over a built-in id");
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT);
    CHECK(run(la, out, &n) == 0x1234 && n == 0, "handler overrides a built-in applet and may report a failure result");
    libapplet_register_handler(LIBAPPLET_ID_PLAYER_SELECT, NULL);
    la = create_applet(lac, LIBAPPLET_ID_PLAYER_SELECT); push(la, common, 0x20); push(la, selarg, 0xa0);
    CHECK(run(la, out, &n) == 0 && n == APPLET_PLAYER_SELECT_OUT_SIZE, "removing the handler restores the built-in answer");
    libapplet_clear_handlers();
    CHECK(libapplet_find_handler(0x13) == NULL, "clear_handlers");
    { LibAppletCall cc; memset(&cc, 0, sizeof cc); CHECK(libapplet_dispatch(&cc) == 0, "dispatch without a handler"); }

    la = create_applet(lac, 0x13);  /* web: still not implemented */
    CHECK(run(la, out, &n) == 0x1480 && n == 0, "unimplemented library applets still report cancelled");


    /* the firmware applet host (parent side): a stand-in program plays the Mii Maker: it reads in0.bin and writes out0.bin = "OK" + the input */
    {
        char root[96], cmd[600];
        snprintf(root, sizeof root, "/tmp/napple-applet-host-%d", (int)getpid());
        snprintf(cmd, sizeof cmd, "mkdir -p %s/game/b1 %s/game/applets/0100000000001009/b2 && echo '{}' > %s/game/applets/0100000000001009/b2/manifest.json && "
                 "printf '#!/bin/sh\\nprintf OK > \"$NAPPLE_APPLET_OUT/out0.bin\"; cat \"$NAPPLE_APPLET_IN/in0.bin\" >> \"$NAPPLE_APPLET_OUT/out0.bin\"\\n' > %s/fake.sh && chmod +x %s/fake.sh", root, root, root, root, root);
        CHECK(system(cmd) == 0, "host test scaffolding");
        static char dd[160], fk[160]; snprintf(dd, sizeof dd, "%s/game/b1", root); snprintf(fk, sizeof fk, "%s/fake.sh", root);
        g_cfg.data_dir = dd; g_cfg.save_root = root;
        IpcObject *la = create_applet(lac, 0x12); uint8_t in[0x100] = {3, 0, 0, 0, 7}; push(la, in, sizeof in);
        setenv("NAPPLE_FIRMWARE_APPLETS", "0", 1);
        CHECK(run(la, out, &n) == 0x1480, "Mii Maker with NAPPLE_FIRMWARE_APPLETS=0: cancelled as before");
        setenv("NAPPLE_FIRMWARE_APPLETS", "1", 1); setenv("NAPPLE_APPLET_EXE", fk, 1);
        la = create_applet(lac, 0x12); push(la, in, sizeof in);
        uint32_t r = run(la, out, &n);
        CHECK(r == 0 && n == 2 + sizeof in && out[0] == 'O' && out[1] == 'K' && out[2] == 3 && out[6] == 7, "firmware applet host: storages go to the child as files and its output comes back (result %#x, %llu bytes)", r, (unsigned long long)n);
        setenv("NAPPLE_APPLET_EXE", "/bin/false", 1);
        la = create_applet(lac, 0x12); push(la, in, sizeof in);
        CHECK(run(la, out, &n) == 0x1480 && n == 0, "a child that produces nothing: cancelled");
        unsetenv("NAPPLE_FIRMWARE_APPLETS"); unsetenv("NAPPLE_APPLET_EXE");
        snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)!system(cmd);
    }
    printf("{\"test\":\"svc_applet_libs\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
