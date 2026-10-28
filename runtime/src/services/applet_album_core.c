#include "applet_album_core.h"
#include "applet_ui.h"
#include <stdlib.h>
#include <string.h>

static AppletUiShowImage g_show;
void applet_ui_set_show_image(AppletUiShowImage fn) { g_show = fn; }
int applet_ui_show_image(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count) { return g_show ? g_show(rgba, w, h, index, count) : 0; }

int applet_album_script(const char *spec, unsigned index, unsigned count) {
    (void)count;
    if (!spec || !*spec) return -3;
    if (!strcmp(spec, "none")) return -1;
    if (!strcmp(spec, "next")) return 0;
    if (!strcmp(spec, "first")) return index == 0;
    if (spec[0] >= '0' && spec[0] <= '9') return (unsigned)atoi(spec) == index;
    return -3;
}

static int g_selecting;
int applet_ui_album_selecting(void) { return g_selecting; }

static const CapsState *g_src;
void applet_album_set_source(const CapsState *s) { g_src = s; }

void album_args_parse(const uint8_t *data, size_t size, AlbumArgs *a) {
    a->mode = ALBUM_MODE_VIEW;
    a->content = -1;
    if (data && size >= 1) a->mode = data[0] == ALBUM_MODE_SELECT ? ALBUM_MODE_SELECT : ALBUM_MODE_VIEW;
    if (data && size >= 2 && data[1] <= CAPS_CONTENT_MOVIE) a->content = data[1];
}

unsigned album_list(const CapsState *s, const AlbumArgs *a, uint8_t (*out)[CAPS_ENTRY_SIZE], unsigned max) {
    if (!s || !max) return 0;
    return caps_album_list(s, a->content, 0, max, (uint8_t *)out);
}

void album_result_encode(uint32_t result, const uint8_t *entry, uint8_t out[ALBUM_RESULT_SIZE]) {
    memset(out, 0, ALBUM_RESULT_SIZE);
    memcpy(out, &result, 4);
    uint32_t sel = entry != NULL;
    memcpy(out + 4, &sel, 4);
    if (entry) memcpy(out + 8, entry, CAPS_ENTRY_SIZE);
}

uint32_t album_run(const CapsState *s, const AlbumArgs *a, uint8_t picked[CAPS_ENTRY_SIZE], int *has_pick) {
    uint8_t(*list)[CAPS_ENTRY_SIZE] = malloc((size_t)CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE);
    uint8_t *img = NULL;
    *has_pick = 0;
    g_selecting = a->mode == ALBUM_MODE_SELECT;
    if (!list) return ALBUM_RESULT_CANCELLED;
    unsigned n = album_list(s, a, list, CAPS_ALBUM_MAX);
    for (unsigned i = 0; i < n && !*has_pick; i++) {
        if (!img) img = malloc(CAPS_IMAGE_BYTES);
        if (!img) break;
        if (caps_album_load(s, list[i], 1, img, CAPS_IMAGE_BYTES) <= 0) continue; /* no image stored / undecodable: nothing to show */
        int ans = applet_album_script(getenv("NAPPLE_ALBUM_PICK"), i, n);
        if (ans == -3) ans = applet_ui_show_image(img, CAPS_IMAGE_W, CAPS_IMAGE_H, i, n);
        if (ans < 0) break; /* the user closed the album */
        if (ans && a->mode == ALBUM_MODE_SELECT) {
            memcpy(picked, list[i], CAPS_ENTRY_SIZE);
            *has_pick = 1;
        }
    }
    free(img);
    free(list);
    if (a->mode == ALBUM_MODE_SELECT) return *has_pick ? ALBUM_RESULT_OK : ALBUM_RESULT_CANCELLED;
    return ALBUM_RESULT_OK;
}

int applet_album_handler(LibAppletCall *call) {
    const LibAppletIn *in = NULL;
    for (unsigned i = 1; i < call->nin; i++) /* in[0] is CommonArguments */
        if (call->in[i].data) in = &call->in[i];
    AlbumArgs a;
    album_args_parse(in ? in->data : NULL, in ? in->size : 0, &a);
    uint8_t picked[CAPS_ENTRY_SIZE], out[ALBUM_RESULT_SIZE];
    int has;
    uint32_t r = album_run(g_src, &a, picked, &has);
    album_result_encode(r, has ? picked : NULL, out);
    libapplet_call_set_out(call, out, sizeof out);
    call->result = 0;
    return 1;
}

int applet_album_register(void) { return libapplet_register_handler(LIBAPPLET_ID_PHOTO_VIEWER, applet_album_handler); }
