/* Unit tests: vi core (layers, binder reference counts) and the vi:u display service rows built on it. */
#include "../src/services/vi.c"
#include "svc_test.h"
#ifdef __linux__
/* the graphics library is macOS-only: the display size the service asks for */
void napple_display_game_size(uint32_t bw, uint32_t bh, uint32_t *w, uint32_t *h) { *w = bw; *h = bh; }
#endif

static IpcObject *sub(IpcObject *o, unsigned cmd) { IpcCall k; svc_call(o, cmd, NULL, 0, &k); return k.out_objects[0]; }

int main(void) {
    ViState v; vi_init(&v);
    CHECK(v.display_enabled == 1, "the display starts on");
    ViLayer *a = vi_layer_create(&v, 0), *b = vi_layer_create(&v, 1);
    CHECK(a && b && a->id == 2 && b->id == 3 && !a->managed && b->managed && a->visible && a->scaling == VI_SCALE_TO_WINDOW, "layer ids count up from 2");
    CHECK(vi_layer(&v, 1) && v.nlayers == 3 && vi_layer(&v, 1) == vi_layer(&v, 1), "an unnamed layer registers once on first use");
    CHECK(vi_set_scaling(&v, 2, VI_SCALE_NONE) == 0 && vi_layer(&v, 2)->scaling == VI_SCALE_NONE, "scaling mode stored");
    CHECK(vi_set_scaling(&v, 2, 5) == VI_R_INVALID_RANGE && vi_layer(&v, 2)->scaling == VI_SCALE_NONE, "an unknown mode is rejected, the old one kept");
    CHECK(vi_layer_destroy(&v, 2) == 0 && vi_layer_destroy(&v, 2) == -1 && v.nlayers == 2, "destroy once");
    for (int i = 0; i < 40; i++) vi_layer_create(&v, 0);
    CHECK(v.nlayers == VI_MAX_LAYERS && vi_layer_create(&v, 0) == NULL && vi_set_scaling(&v, 9999, 0) == VI_R_NOT_FOUND, "a full table refuses new layers");
    CHECK(vi_binder_adjust(&v, 1, 1, 1) == 1 && vi_binder_adjust(&v, 1, 2, 1) == 3 && vi_binder_adjust(&v, 1, 1, 0) == 1 && vi_binder_adjust(&v, 1, -9, 1) == 0, "binder counts, floored at zero, strong and weak apart");
    CHECK(vi_binder_adjust(&v, -1, 1, 1) == -1 && vi_binder_adjust(&v, VI_MAX_BINDERS, 1, 1) == -1, "binder id out of range");

    IpcObject *root = vi_open(); IpcCall c;
    IpcObject *ds = sub(root, 0);
    pthread_once(&vi_once, vi_once_init); vi_init(&vi);
    uint8_t en = 0; uint64_t disp = 1; uint8_t in1101[16] = {0}; in1101[0] = en; memcpy(in1101 + 8, &disp, 8);
    svc_call(ds, 1101, in1101, 16, &c); CHECK(c.result == 0 && vi.display_enabled == 0, "SetDisplayEnabled(false)");
    in1101[0] = 1; svc_call(ds, 1101, in1101, 16, &c); CHECK(vi.display_enabled == 1, "SetDisplayEnabled(true)");
    uint8_t in2101[16] = {0}; uint32_t mode = VI_SCALE_PRESERVE_ASPECT; uint64_t layer = 7; memcpy(in2101, &mode, 4); memcpy(in2101 + 8, &layer, 8);
    svc_call(ds, 2101, in2101, 16, &c); CHECK(c.result == 0 && vi_layer(&vi, 7)->scaling == VI_SCALE_PRESERVE_ASPECT, "SetLayerScalingMode stores the mode for the layer");
    mode = 9; memcpy(in2101, &mode, 4); svc_call(ds, 2101, in2101, 16, &c); CHECK(c.result == VI_R_INVALID_RANGE, "SetLayerScalingMode with a bad mode fails");
    svc_call(ds, 2030, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 2, "CreateStrayLayer: first id is 2");
    svc_call(ds, 2030, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 3, "CreateStrayLayer: next id is 3");
    uint64_t gone = 2; svc_call(ds, 2021, &gone, 8, &c); CHECK(c.result == 0 && vi_layer_destroy(&vi, 2) == -1, "CloseLayer removes the layer");

    IpcObject *sd = sub(ds, 101), *md = sub(ds, 102);
    CHECK(sd->ncmds > 0 && md->ncmds > 0, "system and manager display services have commands");
    uint8_t p[24] = {0}; float fx = 12.5f, fy = -3.f; uint64_t L = 40; memcpy(p, &fx, 4); memcpy(p + 4, &fy, 4); memcpy(p + 8, &L, 8);
    svc_call(sd, 2201, p, 16, &c); CHECK(c.result == 0 && vi_layer(&vi, 40)->x == 12.5f && vi_layer(&vi, 40)->y == -3.f, "SetLayerPosition");
    uint64_t sz[3] = {40, 1280, 720}; svc_call(sd, 2203, sz, 24, &c); CHECK(vi_layer(&vi, 40)->w == 1280 && vi_layer(&vi, 40)->h == 720, "SetLayerSize");
    uint64_t z[2] = {40, 5}; svc_call(sd, 2205, z, 16, &c); svc_call(sd, 2204, &L, 8, &c); CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 5, "SetLayerZ / GetLayerZ");
    uint8_t vis[16] = {0}; memcpy(vis + 8, &L, 8); svc_call(sd, 2207, vis, 16, &c); CHECK(vi_layer(&vi, 40)->visible == 0, "SetLayerVisibility(false)");
    svc_call(sd, 1200, &disp, 8, &c); int64_t zmin = SVC_OUT(&c, int64_t, 0); svc_call(sd, 1202, &disp, 8, &c); CHECK(zmin == 0 && SVC_OUT(&c, int64_t, 0) > zmin, "z-order range");
    svc_call(md, 2010, NULL, 0, &c); uint64_t ml = SVC_OUT(&c, uint64_t, 0); CHECK(c.result == 0 && ml >= 4 && vi_layer(&vi, ml)->managed, "CreateManagedLayer");
    uint8_t st[16] = {0}; uint32_t stk = 1; memcpy(st, &stk, 4); memcpy(st + 8, &ml, 8);
    svc_call(md, 6000, st, 16, &c); CHECK(vi_layer(&vi, ml)->stack == 2, "AddToLayerStack"); svc_call(md, 6001, st, 16, &c); CHECK(vi_layer(&vi, ml)->stack == 0, "RemoveFromLayerStack");
    svc_call(md, 2011, &ml, 8, &c); CHECK(c.result == 0, "DestroyManagedLayer"); svc_call(md, 2011, &ml, 8, &c); CHECK(c.result == VI_R_NOT_FOUND, "DestroyManagedLayer again: not found");

    IpcObject *bd = sub(ds, 100);
    int32_t adj[3] = {1, 2, 1}; int32_t before = vi.binder_strong[1];
    svc_call(bd, 1, adj, 12, &c); CHECK(c.result == 0 && vi.binder_strong[1] == before + 2, "AdjustRefcount adds strong references");
    adj[1] = -100; svc_call(bd, 1, adj, 12, &c); CHECK(vi.binder_strong[1] == 0, "AdjustRefcount never goes below zero");
    svc_call(bd, 2, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1, "GetNativeHandle returns an event");
    printf("{\"test\":\"svc_vi\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
