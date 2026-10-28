/* Unit tests: the photo viewer / album library applet (0x15, applet_album_core.c) over the caps album: argument parse, list, view and select
 * through the show-image hook, result encoding, dispatch through libapplet_register_handler via the real accessor of applet.c. */
#include "../src/services/applet.c"
#include "svc_test.h"
#include "../src/services/caps_core.c"
#include "../src/services/jpeg_core.c"
#include "../src/services/applet_album_core.c"
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


static int g_shown, g_pick_at, g_last_idx, g_last_count;
static unsigned g_last_w, g_last_h;
static uint8_t g_first_px;
static int show_hook(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count) {
    g_shown++; g_last_w = w; g_last_h = h; g_last_idx = (int)index; g_last_count = (int)count; g_first_px = rgba[0];
    return (int)index == g_pick_at;
}
static int leave_hook(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count) { (void)rgba; (void)w; (void)h; (void)index; (void)count; g_shown++; return -1; }
/* adds an entry with a flat-colour image */
static void add(CapsState *s, unsigned sec, uint8_t shade, uint8_t entry[CAPS_ENTRY_SIZE], int image) {
    caps_make_entry(s, 2026, 10, 27, 12, 0, (uint8_t)sec, entry);
    if (!image) return;
    uint8_t *px = malloc(CAPS_IMAGE_BYTES);
    for (unsigned i = 0; i < CAPS_IMAGE_BYTES; i += 4) { px[i] = shade; px[i + 1] = shade; px[i + 2] = shade; px[i + 3] = 255; }
    CHECK(caps_album_set_image(s, entry, px, CAPS_IMAGE_W, CAPS_IMAGE_H, CAPS_IMAGE_W * 4) == 0, "set image");
    caps_album_entry(s, entry, entry);
    free(px);
}

int main(void) {
    static CapsState st; AlbumArgs a;
    /* ---- args ---- */
    album_args_parse(NULL, 0, &a); CHECK(a.mode == ALBUM_MODE_VIEW && a.content == -1, "defaults");
    uint8_t args[4] = {1, 0, 0, 0}; album_args_parse(args, 4, &a); CHECK(a.mode == ALBUM_MODE_SELECT && a.content == CAPS_CONTENT_SCREENSHOT, "select, screenshots");
    args[0] = 9; args[1] = 0xff; album_args_parse(args, 4, &a); CHECK(a.mode == ALBUM_MODE_VIEW && a.content == -1, "unknown mode -> view, 0xff -> any");
    album_args_parse(args, 1, &a); CHECK(a.content == -1, "one byte only");
    uint8_t res[ALBUM_RESULT_SIZE], e0[CAPS_ENTRY_SIZE], e1[CAPS_ENTRY_SIZE], e2[CAPS_ENTRY_SIZE];
    album_result_encode(ALBUM_RESULT_CANCELLED, NULL, res); uint32_t w0, w1; memcpy(&w0, res, 4); memcpy(&w1, res + 4, 4);
    CHECK(w0 == 2 && w1 == 0, "cancel encoding");

    /* ---- list ---- */
    uint8_t (*list)[CAPS_ENTRY_SIZE] = malloc(CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE);
    album_args_parse(NULL, 0, &a);
    CHECK(album_list(&st, &a, list, CAPS_ALBUM_MAX) == 0 && album_list(NULL, &a, list, 8) == 0, "empty album");
    add(&st, 1, 40, e0, 1); add(&st, 2, 200, e1, 1); add(&st, 3, 0, e2, 0); /* the third has no image */
    CHECK(album_list(&st, &a, list, CAPS_ALBUM_MAX) == 3, "three entries");
    CHECK(!memcmp(list[1] + 0x10, e1 + 0x10, 8), "oldest first");
    CHECK(album_list(&st, &a, list, 2) == 2, "max respected");
    album_args_parse((uint8_t[]){0, CAPS_CONTENT_MOVIE}, 2, &a); CHECK(album_list(&st, &a, list, 8) == 0, "movie filter: none");
    album_args_parse(NULL, 0, &a);

    /* ---- run: view shows every image, returns success with no pick ---- */
    applet_ui_set_show_image(show_hook); g_pick_at = 1;
    uint8_t picked[CAPS_ENTRY_SIZE]; int has;
    uint32_t r = album_run(&st, &a, picked, &has);
    CHECK(r == ALBUM_RESULT_OK && !has && g_shown == 2 && g_last_w == CAPS_IMAGE_W && g_last_h == CAPS_IMAGE_H, "view: shown %d result %u has %d", g_shown, r, has);
    CHECK(g_last_count == 3, "count passed");
    /* ---- select picks the chosen one and stops ---- */
    g_shown = 0; album_args_parse((uint8_t[]){1, 0xff}, 2, &a); g_pick_at = 1;
    r = album_run(&st, &a, picked, &has);
    CHECK(r == ALBUM_RESULT_OK && has && !memcmp(picked + 0x10, e1 + 0x10, 8) && g_shown == 2, "select: picked the second (shown %d)", g_shown);
    CHECK(g_first_px > 150, "decoded pixels of the bright entry (%u)", g_first_px);
    /* select with nobody picking -> cancelled; empty album -> cancelled */
    g_pick_at = -1; r = album_run(&st, &a, picked, &has); CHECK(r == ALBUM_RESULT_CANCELLED && !has, "select, no pick: cancelled");
    static CapsState empty; r = album_run(&empty, &a, picked, &has); CHECK(r == ALBUM_RESULT_CANCELLED && !has, "select, empty album: cancelled");
    applet_ui_set_show_image(NULL); g_pick_at = 1;
    r = album_run(&st, &a, picked, &has); CHECK(r == ALBUM_RESULT_CANCELLED && !has, "no frontend: nothing is picked");

    /* ---- scripted answers (NAPPLE_ALBUM_PICK) and the leave answer ---- */
    applet_ui_set_show_image(show_hook); g_pick_at = 0; g_shown = 0;
    CHECK(applet_album_script(NULL, 0, 3) == -3 && applet_album_script("bogus", 0, 3) == -3, "script: unset / unknown asks the hook");
    CHECK(applet_album_script("none", 1, 3) == -1 && applet_album_script("next", 1, 3) == 0 && applet_album_script("first", 0, 3) == 1 && applet_album_script("first", 1, 3) == 0 && applet_album_script("2", 2, 3) == 1 && applet_album_script("2", 1, 3) == 0, "script values");
    setenv("NAPPLE_ALBUM_PICK", "1", 1); r = album_run(&st, &a, picked, &has);
    CHECK(r == ALBUM_RESULT_OK && has && !g_shown && applet_ui_album_selecting(), "scripted pick: the hook is not asked (shown %d)", g_shown);
    setenv("NAPPLE_ALBUM_PICK", "none", 1); r = album_run(&st, &a, picked, &has); CHECK(r == ALBUM_RESULT_CANCELLED && !has, "scripted none: select mode is cancelled");
    unsetenv("NAPPLE_ALBUM_PICK");
    album_args_parse(NULL, 0, &a); r = album_run(&st, &a, picked, &has); CHECK(!applet_ui_album_selecting() && r == ALBUM_RESULT_OK, "view mode is not selecting");
    album_args_parse((uint8_t[]){1, 0xff}, 2, &a); g_pick_at = -1;
    r = album_run(&st, &a, picked, &has);
    /* a hook that answers -1 closes the album at once */
    applet_ui_set_show_image(leave_hook); g_shown = 0; r = album_run(&st, &a, picked, &has);
    CHECK(r == ALBUM_RESULT_CANCELLED && !has && g_shown == 1, "hook leave: the walk stops after one image (shown %d)", g_shown);
    applet_ui_set_show_image(show_hook); g_pick_at = 1;

    /* ---- dispatch through the hook, via the accessor ---- */
    libapplet_clear_handlers();
    CHECK(applet_album_register() == 0 && libapplet_find_handler(0x15) == applet_album_handler, "registered for 0x15");
    applet_album_set_source(&st); applet_ui_set_show_image(show_hook); g_pick_at = 2 - 1; g_shown = 0;
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    svc_call(proxy, 11, NULL, 0, &c); IpcObject *lac = c.out_objects[0];
    uint8_t out[0x100]; uint64_t n; uint8_t common[0x10] = {1, 0, 0, 0, 0x10, 0, 0, 0};
    IpcObject *la = create_applet(lac, 0x15); CHECK(la != NULL, "CreateLibraryApplet 0x15");
    uint8_t sel[4] = {1, 0xff, 0, 0};
    push(la, common, sizeof common); push(la, sel, sizeof sel);
    r = run(la, out, &n);
    memcpy(&w0, out, 4); memcpy(&w1, out + 4, 4);
    CHECK(r == 0 && n == ALBUM_RESULT_SIZE && w0 == 0 && w1 == 1 && !memcmp(out + 8 + 0x10, e1 + 0x10, 8), "accessor: select returns the entry (n=%llu)", (unsigned long long)n);
    /* view, no args */
    g_shown = 0; la = create_applet(lac, 0x15); push(la, common, sizeof common); r = run(la, out, &n);
    memcpy(&w0, out, 4); memcpy(&w1, out + 4, 4);
    CHECK(r == 0 && n == ALBUM_RESULT_SIZE && w0 == 0 && w1 == 0 && g_shown == 2, "accessor: view with no args (shown %d)", g_shown);
    /* no source: an empty album, select cancels */
    applet_album_set_source(NULL); la = create_applet(lac, 0x15); push(la, common, sizeof common); push(la, sel, sizeof sel); run(la, out, &n);
    memcpy(&w0, out, 4); CHECK(w0 == ALBUM_RESULT_CANCELLED, "accessor: no source, cancelled");
    caps_album_clear(&st); free(list);
    printf("{\"test\":\"svc_applet_album\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
