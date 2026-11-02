/* t_provoking.c - nvnCommandBufferSetProvokingVertexMode: which vertex a flat-shaded output comes from. Each vertex carries its own value (1, 2, 3, ...)
 * in a varying the fragment program reads with IPA.CONSTANT ([[flat]]) and writes to an R32F target; the target's values tell the provoking vertex.
 * Mode 0 (the default, Metal's own convention) takes the first vertex of each primitive; mode 1 the last (the replay re-emits each primitive with its last
 * vertex first, pv_last_indices in mtl_replay.m). Cases: a triangle list, a 4-vertex triangle strip (two triangles), the same strip indexed with a primitive
 * restart in the middle, a line list. Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_IB = 0x800, OFF_SH = 0x1000, OFF_RT = 0x100000, OFF_RB = H_READBACK };
/* IPA.CONSTANT Rd = attribute `addr` (Ra = RZ, no multiplier) */
#define IPA_FLAT(d, addr) (0xe000000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | ((uint64_t)(addr) << 28) | (255ull << 39) | (2ull << 54) | PT_BITS)

static H h;
static NVNprogram *prog;
static NVNtexture *rt;

/* one vertex: clip position (x, y, 0, 1) and the value */
static void put(int i, float x, float y, float v) {
    float *f = (float *)(h.mem + OFF_VB) + i * 8;
    f[0] = x, f[1] = y, f[2] = 0, f[3] = 1, f[4] = v, f[5] = f[6] = 0, f[7] = 1;
}

/* draws with `mode` (0 first, 1 last) and returns the distinct non-zero values of the target, ascending, in `out` */
static int draw(int pv, NVNdrawPrimitive prim, int count, const uint16_t *idx, float *out) {
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    const NVNtexture *rts[1] = {rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    float zero[4] = {0, 0, 0, 0};
    L(nvnCommandBufferClearColor)(cb, 0, zero, NVN_CLEAR_COLOR_MASK_RGBA);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va[2];
    for (int i = 0; i < 2; i++) {
        L(nvnVertexAttribStateSetDefaults)(&va[i]);
        L(nvnVertexAttribStateSetFormat)(&va[i], NVN_FORMAT_RGBA32F, 16 * i);
    }
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 32);
    L(nvnCommandBufferBindVertexAttribState)(cb, 2, va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 32 * 16);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferSetProvokingVertexMode)(cb, pv);
    if (idx) {
        L(nvnCommandBufferSetPrimitiveRestart)(cb, 1, 0xffff);
        L(nvnCommandBufferDrawElements)(cb, prim, NVN_INDEX_TYPE_UNSIGNED_SHORT, count, (NVNbufferAddress)(uintptr_t)idx);
        L(nvnCommandBufferSetPrimitiveRestart)(cb, 0, 0);
    } else L(nvnCommandBufferDrawArrays)(cb, prim, 0, count);
    L(nvnCommandBufferSetProvokingVertexMode)(cb, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    const float *px = (const float *)(h.mem + OFF_RB);
    int n = 0;
    for (int i = 0; i < W * W; i++) {
        if (px[i] == 0) continue;
        int k = 0;
        while (k < n && out[k] < px[i]) k++;
        if (k < n && out[k] == px[i]) continue;
        if (n == 8) break;
        memmove(out + k + 1, out + k, (size_t)(n - k) * sizeof *out);
        out[k] = px[i];
        n++;
    }
    return n;
}

static void expect(const char *what, int pv, NVNdrawPrimitive prim, int count, const uint16_t *idx, int nwant, const float *want) {
    float got[8];
    int n = draw(pv, prim, count, idx, got);
    printf("%s, mode %s: values", what, pv ? "LAST" : "FIRST");
    for (int i = 0; i < n; i++) printf(" %g", got[i]);
    printf(" (want");
    for (int i = 0; i < nwant; i++) printf(" %g", want[i]);
    printf(")\n");
    int ok = n == nwant;
    for (int i = 0; ok && i < n; i++) ok = got[i] == want[i];
    CHECK(ok, "%s, provoking vertex %s: the flat output comes from the expected vertices", what, pv ? "LAST" : "FIRST");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    /* vertex: position, attribute 1 -> generic output 0; fragment: IPA.CONSTANT of input 0 -> colour 0 x */
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_ald(4, 0x90, 4), ins_ast(4, 0x80, 4), ins_exit()};
    uint64_t fs_i[] = {IPA_FLAT(0, 0x80), ins_mov32i(8, 0), ins_exit()}; /* three instructions: a zero word inside the first bundle would end the program */
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 5, 0);
    build_shader(h.mem + OFF_SH + 0x2000, fctl, 1, fs_i, 3, 0x1);
    prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");
    rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32F, W, W, 1, OFF_RT, 0);

    /* vertices 0-2: one full-screen triangle (1, 2, 3); 3-6: a strip quad (4, 5, 6, 7); 8-11: two horizontal lines (8 -> 9, 10 -> 11) */
    put(0, -1, -1, 1), put(1, 3, -1, 2), put(2, -1, 3, 3);
    put(3, -1, -1, 4), put(4, 1, -1, 5), put(5, -1, 1, 6), put(6, 1, 1, 7);
    put(8, -1, -0.4375f, 8), put(9, 1, -0.4375f, 9), put(10, -1, 0.5625f, 10), put(11, 1, 0.5625f, 11);
    /* indexed strip with a restart: (3, 4, 5) | restart | (4, 6, 5) -> triangles (3, 4, 5) and (4, 6, 5) */
    uint16_t *ib = (uint16_t *)(h.mem + OFF_IB);
    const uint16_t strip_r[7] = {3, 4, 5, 0xffff, 4, 6, 5};
    memcpy(ib, strip_r, sizeof strip_r);

    const float tri_f[] = {1}, tri_l[] = {3};
    expect("triangle list", 0, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, NULL, 1, tri_f);
    expect("triangle list", 1, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, NULL, 1, tri_l);
    /* strip triangles (3, 4, 5) and (4, 5, 6): first vertices 4, 5; last vertices 6, 7 */
    uint16_t *ib2 = ib + 8;
    const uint16_t strip[4] = {3, 4, 5, 6};
    memcpy(ib2, strip, sizeof strip);
    const float st_f[] = {4, 5}, st_l[] = {6, 7};
    expect("triangle strip", 0, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 4, ib2, 2, st_f);
    expect("triangle strip", 1, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 4, ib2, 2, st_l);
    /* restart: triangle (3, 4, 5) -> first 4, last 6; triangle (4, 6, 5) -> first 5, last 6 */
    const float rs_f[] = {4, 5}, rs_l[] = {6};
    expect("indexed strip with restart", 0, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 7, ib, 2, rs_f);
    expect("indexed strip with restart", 1, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 7, ib, 1, rs_l);
    /* lines (8 -> 9), (10 -> 11) */
    uint16_t *ib3 = ib + 16;
    const uint16_t lines[4] = {8, 9, 10, 11};
    memcpy(ib3, lines, sizeof lines);
    const float ln_f[] = {8, 10}, ln_l[] = {9, 11};
    expect("line list", 0, NVN_DRAW_PRIMITIVE_LINES, 4, ib3, 2, ln_f);
    expect("line list", 1, NVN_DRAW_PRIMITIVE_LINES, 4, ib3, 2, ln_l);
    return h_finish("t_provoking", &h);
}
