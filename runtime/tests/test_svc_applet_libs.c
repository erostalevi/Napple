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

int main(void) {
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    svc_call(proxy, 11, NULL, 0, &c); IpcObject *lac = c.out_objects[0];
    uint8_t out[0x100]; uint64_t n;

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

    la = create_applet(lac, 0x13);  /* web: still not implemented */
    CHECK(run(la, out, &n) == 0x1480 && n == 0, "unimplemented library applets still report cancelled");

    printf("{\"test\":\"svc_applet_libs\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
