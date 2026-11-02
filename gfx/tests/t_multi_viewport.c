/* t_multi_viewport.c - viewport arrays: nvnCommandBufferSetViewports / SetScissors / SetDepthRanges for viewports 0-15 and the viewport index (attribute
 * 0x68) and layer (0x64) a program writes ([[viewport_array_index]] / [[render_target_array_index]]). One draw of three full-screen triangles, each with
 * its own viewport index and layer from a vertex attribute, into a 32x32 R32F target; the fragment program writes its window depth (attribute 0x78), so
 * each pixel tells the depth range that applied:
 *   viewport 0 (0, 0, 16, 16) depth [0, 1]     -> 0.5, scissor 0 the whole target;
 *   viewport 1 (16, 0, 16, 16) depth [0.2, 0.4] -> 0.3, scissor 1 its left half (8 x 16);
 *   viewport 2 (0, 16, 16, 16) depth [0.6, 1]   -> 0.8, scissor 2 the whole target.
 * Cases: 1. a 2D target, viewport indices 0 / 1 / 2;  2. a program that writes no index: viewport 0 only;  3. a 2-layer array target, (viewport, layer)
 * (0, 0) (1, 1) (2, 1): layer 0 has viewport 0's square, layer 1 the other two;  4. the same into a one-layer view of layer 1: the layer is ignored, all
 * three land in layer 1;  5. a geometry program that forwards the triangle with viewport index 2 and layer 1 (the pass-through routes them);
 * 6. a quad patch whose tess eval program places its vertices at control point 0 + (u, v) (the lower-left quarter of clip space) with viewport index 1
 * and layer 1: the quarter of viewport 1 inside scissor 1 (8 x 8), in layer 1.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
#include <math.h>
#include "stages.h"

#define W 32
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_RT = 0x100000, OFF_ARR = 0x110000, OFF_RB = H_READBACK };
/* IPA Rd = attribute `addr` (fragment: 0x78 is the window depth) */
#define IPA_ATTR(d, addr) (0xe000000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | ((uint64_t)(addr) << 28) | (255ull << 39) | (2ull << 54) | PT_BITS)
#define ALDV(d, addr, n, vreg) (0xefd8000000000000ull | (uint64_t)(d) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(vreg) << 39) | PT_BITS)
static uint64_t fadd(int d, int a, int b) { return 0x5c58000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | PT_BITS; }
#define OUT_EMIT(d) (0xfbe0000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | (1ull << 39) | PT_BITS)

static H h;
static NVNprogram *prog_route, *prog_plain, *prog_gs, *prog_tes;
static int g_patch; /* draw one 4-vertex patch (tess levels 4) instead of triangles */
static NVNtexture *rt2d, *arr;

/* one vertex: clip position (x, y, 0.5, 1), then viewport index and layer as an RGBA32UI attribute */
static void put(int i, float x, float y, uint32_t vpi, uint32_t layer) {
    float *f = (float *)(h.mem + OFF_VB) + i * 8;
    f[0] = x, f[1] = y, f[2] = 0.5f, f[3] = 1; /* window depth = near + (far - near) / 2 (depth mode [0, 1], the harness device's) */
    uint32_t *u = (uint32_t *)(f + 4);
    u[0] = vpi, u[1] = layer, u[2] = 0, u[3] = 0;
}
static void set_tri(int t, uint32_t vpi, uint32_t layer) {
    put(3 * t, -1, -1, vpi, layer), put(3 * t + 1, 3, -1, vpi, layer), put(3 * t + 2, -1, 3, vpi, layer);
}

/* clears `t` (view `v` or the whole texture), sets the viewport arrays, draws `ntri` triangles with `p`, reads `nlayers` layers back */
static void draw(NVNprogram *p, NVNtexture *t, const NVNtextureView *v, int ntri, int nlayers) {
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    float zero[4] = {0, 0, 0, 0};
    for (int l = 0; l < (t == arr ? 2 : 1); l++) { /* each layer through a one-layer view (a clear covers the attachment's own image) */
        NVNtextureView lv; L(nvnTextureViewSetDefaults)(&lv);
        if (t == arr) L(nvnTextureViewSetLayers)(&lv, l, 1);
        const NVNtexture *rts[1] = {t};
        const NVNtextureView *lvs[1] = {&lv};
        L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, t == arr ? lvs : NULL, NULL, NULL);
        L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
        L(nvnCommandBufferClearColor)(cb, 0, zero, NVN_CLEAR_COLOR_MASK_RGBA);
    }
    const NVNtexture *rts[1] = {t};
    const NVNtextureView *vs[1] = {v};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, v ? vs : NULL, NULL, NULL);
    const float vps[12] = {0, 0, 16, 16, 16, 0, 16, 16, 0, 16, 16, 16};
    const int scs[12] = {0, 0, W, W, 16, 0, 8, 16, 0, 0, W, W};
    const float drs[6] = {0, 1, 0.2f, 0.4f, 0.6f, 1};
    L(nvnCommandBufferSetViewports)(cb, 0, 3, vps);
    L(nvnCommandBufferSetScissors)(cb, 0, 3, scs);
    L(nvnCommandBufferSetDepthRanges)(cb, 0, 3, drs);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va[2];
    L(nvnVertexAttribStateSetDefaults)(&va[0]); L(nvnVertexAttribStateSetFormat)(&va[0], NVN_FORMAT_RGBA32F, 0);
    L(nvnVertexAttribStateSetDefaults)(&va[1]); L(nvnVertexAttribStateSetFormat)(&va[1], NVN_FORMAT_RGBA32UI, 16);
    NVNvertexStreamState st; L(nvnVertexStreamStateSetDefaults)(&st); L(nvnVertexStreamStateSetStride)(&st, 32);
    L(nvnCommandBufferBindVertexAttribState)(cb, 2, va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &st);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 9 * 32);
    L(nvnCommandBufferBindProgram)(cb, p, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT |
                                              NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
    if (g_patch) {
        float outer[4] = {4, 4, 4, 4}, inner[2] = {4, 4};
        L(nvnCommandBufferSetOuterTessellationLevels)(cb, outer);
        L(nvnCommandBufferSetInnerTessellationLevels)(cb, inner);
        L(nvnCommandBufferSetPatchSize)(cb, 4);
        L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
    } else L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3 * ntri);
    memset(h.mem + OFF_RB, 0xcd, (size_t)W * W * 4 * nlayers);
    for (int l = 0; l < nlayers; l++) {
        NVNcopyRegion r = {0, 0, l, W, W, 1};
        L(nvnCommandBufferCopyTextureToBuffer)(cb, t, NULL, &r, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_RB + (size_t)W * W * 4 * l), 0);
    }
    h_submit(&h);
}

/* the pixels of layer `l` equal to `v`: count and bounding box */
typedef struct { int n, x0, y0, x1, y1; } box;
static box find(int l, float v) {
    const float *px = (const float *)(h.mem + OFF_RB) + (size_t)W * W * l;
    box b = {0, W, W, -1, -1};
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++)
            if (fabsf(px[y * W + x] - v) < 1e-4f) {
                b.n++;
                if (x < b.x0) b.x0 = x;
                if (y < b.y0) b.y0 = y;
                if (x > b.x1) b.x1 = x;
                if (y > b.y1) b.y1 = y;
            }
    return b;
}
static int nonzero(int l) {
    const float *px = (const float *)(h.mem + OFF_RB) + (size_t)W * W * l;
    int n = 0;
    for (int i = 0; i < W * W; i++) n += px[i] != 0;
    return n;
}
/* a filled w x h rectangle at column x0 */
static int is_rect(box b, int x0, int w, int hh) { return b.n == w * hh && b.x0 == x0 && b.x1 - b.x0 + 1 == w && b.y1 - b.y0 + 1 == hh; }
static void show(const char *what, int l) {
    box a = find(l, 0.5f), b = find(l, 0.3f), c = find(l, 0.8f);
    printf("%s, layer %d: %d non-zero; 0.5: %d px (%d,%d)-(%d,%d); 0.3: %d px (%d,%d)-(%d,%d); 0.8: %d px (%d,%d)-(%d,%d)\n", what, l, nonzero(l), a.n, a.x0,
           a.y0, a.x1, a.y1, b.n, b.x0, b.y0, b.x1, b.y1, c.n, c.x0, c.y0, c.x1, c.y1);
}
/* viewport 0's square, viewport 1's scissored half next to it (same rows), viewport 2's square above / below viewport 0's (the other rows) */
static int vp0_ok(int l) { return is_rect(find(l, 0.5f), 0, 16, 16); }
static int vp1_ok(int l, int rows_of) {
    box b = find(l, 0.3f), r = find(rows_of, 0.5f);
    return is_rect(b, 16, 8, 16) && b.y0 == r.y0;
}
static int vp2_ok(int l, int rows_of) {
    box c = find(l, 0.8f), r = find(rows_of, 0.5f);
    return is_rect(c, 0, 16, 16) && c.y0 == (r.y0 == 0 ? 16 : 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    uint64_t fs_i[] = {IPA_ATTR(0, 0x78), ins_mov32i(8, 0), ins_exit()};
    uint64_t vp_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t vr_i[] = {ins_ald(0, 0x80, 4), ins_ald(4, 0x90, 2), ins_ast(4, 0x68, 1), ins_ast(5, 0x64, 1), ins_ast(0, 0x70, 4), ins_exit()};
    static uint8_t fctl[0x880], vpctl[0x880], vrctl[0x880], gctl[0x880];
    build_shader(h.mem + OFF_SH, fctl, 1, fs_i, 3, 0x1);
    build_shader(h.mem + OFF_SH + 0x2000, vpctl, 0, vp_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x4000, vrctl, 0, vr_i, 6, 0);
    NVNshaderData fsd = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), fctl};
    NVNshaderData vpd = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), vpctl};
    NVNshaderData vrd = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x4000), vrctl};
    prog_route = calloc(1, sizeof(NVNprogram)), prog_plain = calloc(1, sizeof(NVNprogram)), prog_gs = calloc(1, sizeof(NVNprogram)),
    prog_tes = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog_tes, h.dev);
    L(nvnProgramInitialize)(prog_route, h.dev);
    L(nvnProgramInitialize)(prog_plain, h.dev);
    L(nvnProgramInitialize)(prog_gs, h.dev);
    NVNshaderData rsd[2] = {vrd, fsd}, psd[2] = {vpd, fsd};
    CHECK(L(nvnProgramSetShaders)(prog_route, 2, rsd), "shaders: vertex program writing the viewport index and layer");
    CHECK(L(nvnProgramSetShaders)(prog_plain, 2, psd), "shaders: plain vertex program");
    { /* geometry program: forwards input vertices 0, 1, 2 with viewport index 2 and layer 1 */
        uint64_t gi[20];
        int n = 0;
        gi[n++] = ins_mov32i(10, 1);
        gi[n++] = ins_mov32i(11, 2);
        gi[n++] = ins_mov32i(12, 2);
        gi[n++] = ins_mov32i(14, 1);
        for (int k = 0; k < 3; k++) {
            gi[n++] = ALDV(0, 0x70, 4, k == 0 ? 255 : 9 + k);
            gi[n++] = ins_ast(12, 0x68, 1);
            gi[n++] = ins_ast(14, 0x64, 1);
            gi[n++] = ins_ast(0, 0x70, 4);
            gi[n++] = OUT_EMIT(13);
        }
        gi[n++] = ins_exit();
        uint8_t *m = h.mem + OFF_SH + 0x8000;
        build_shader(m, gctl, 2, gi, n, 0);
        uint32_t *sph = (uint32_t *)(m + 0x30);
        sph[3] = 7u << 24; /* output topology: triangle strip */
        sph[4] = 3;        /* maximum output vertices */
        NVNshaderData gsd[3] = {vpd, {(NVNbufferAddress)(uintptr_t)m, gctl}, fsd};
        CHECK(L(nvnProgramSetShaders)(prog_gs, 3, gsd), "shaders with a geometry program");
    }
    { /* tess eval: position = control point 0 + (u, v, 0, 0), viewport index 1, layer 1 */
        uint64_t ti[] = {ALDV(0, 0x70, 4, 255), ALDV(8, 0x2f0, 2, 255), fadd(0, 0, 8), fadd(1, 1, 9), ins_mov32i(12, 1), ins_ast(12, 0x68, 1),
                         ins_ast(12, 0x64, 1), ins_ast(0, 0x70, 4), ins_exit()};
        static uint8_t tctl[0x880];
        uint8_t *m = h.mem + OFF_SH + 0xa000;
        build_shader(m, tctl, 4, ti, 9, 0);
        NVNshaderData tsd[3] = {vpd, {(NVNbufferAddress)(uintptr_t)m, tctl}, fsd};
        CHECK(L(nvnProgramSetShaders)(prog_tes, 3, tsd), "shaders with a tess eval program");
        gfx_tess_params tp = {TESS_DOMAIN_QUAD, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
        CHECK(gfx_program_set_tess_params(prog_tes, &tp), "tess params registered");
    }
    rt2d = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32F, W, W, 1, OFF_RT, 0);
    arr = h_tex(&h, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_R32F, W, W, 2, OFF_ARR, 0);

    /* 1. 2D target, viewports 0 / 1 / 2 */
    set_tri(0, 0, 0), set_tri(1, 1, 0), set_tri(2, 2, 0);
    draw(prog_route, rt2d, NULL, 3, 1);
    show("2D, indices 0 1 2", 0);
    CHECK(vp0_ok(0), "viewport 0: its square with depth range [0, 1] (0.5)");
    CHECK(vp1_ok(0, 0), "viewport 1: its square, scissor 1 (left half), depth range [0.2, 0.4] (0.3)");
    CHECK(vp2_ok(0, 0), "viewport 2: the square in the other rows, depth range [0.6, 1] (0.8)");
    CHECK(nonzero(0) == 16 * 16 + 8 * 16 + 16 * 16, "nothing else is drawn");
    /* 2. no viewport index: viewport 0 */
    draw(prog_plain, rt2d, NULL, 3, 1);
    show("2D, no index", 0);
    CHECK(vp0_ok(0) && nonzero(0) == 16 * 16, "a program that writes no viewport index: viewport 0 only");
    /* 3. layered: (0, 0) (1, 1) (2, 1) */
    set_tri(0, 0, 0), set_tri(1, 1, 1), set_tri(2, 2, 1);
    draw(prog_route, arr, NULL, 3, 2);
    show("array, layers 0 1 1", 0);
    show("array, layers 0 1 1", 1);
    CHECK(vp0_ok(0) && nonzero(0) == 16 * 16, "layer 0: viewport 0's square only");
    CHECK(vp1_ok(1, 0) && vp2_ok(1, 0) && nonzero(1) == 8 * 16 + 16 * 16, "layer 1: viewports 1 and 2");
    /* 4. a one-layer view of layer 1: not layered, the layer output is ignored */
    NVNtextureView v1; L(nvnTextureViewSetDefaults)(&v1); L(nvnTextureViewSetLayers)(&v1, 1, 1);
    draw(prog_route, arr, &v1, 3, 2);
    show("view of layer 1", 0);
    show("view of layer 1", 1);
    CHECK(nonzero(0) == 0, "one-layer view: layer 0 untouched");
    CHECK(vp0_ok(1) && vp1_ok(1, 1) && vp2_ok(1, 1), "one-layer view: all three viewports in layer 1");
    /* 5. geometry program: viewport index 2, layer 1 */
    set_tri(0, 0, 0);
    draw(prog_gs, arr, NULL, 1, 2);
    show("geometry, index 2 layer 1", 0);
    show("geometry, index 2 layer 1", 1);
    CHECK(nonzero(0) == 0, "geometry program: layer 0 untouched");
    CHECK(is_rect(find(1, 0.8f), 0, 16, 16) && nonzero(1) == 16 * 16, "geometry program: viewport 2's square in layer 1");
    /* 6. tess eval: viewport index 1, layer 1, the lower-left quarter of clip space */
    g_patch = 1;
    draw(prog_tes, arr, NULL, 1, 2);
    g_patch = 0;
    show("tess eval, index 1 layer 1", 0);
    show("tess eval, index 1 layer 1", 1);
    box q = find(1, 0.3f);
    CHECK(nonzero(0) == 0, "tess eval: layer 0 untouched");
    CHECK(is_rect(q, 16, 8, 8) && nonzero(1) == 8 * 8, "tess eval: a quarter of viewport 1 (8 x 8 inside scissor 1) in layer 1");
    return h_finish("t_multi_viewport", &h);
}
