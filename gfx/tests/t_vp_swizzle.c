/* t_vp_swizzle.c - nvnCommandBufferSetViewportSwizzles: the clip position the vertex program writes is swizzled per output component before the
 * viewport transform (ShaderKey.vp_swizzle, applied in the vertex function). A rectangle covering the left half of clip space (x in [-1, 0]) is drawn
 * with a constant colour into a 16x16 R32F target:
 *  1. identity: the left half is covered;
 *  2. x = -x: the right half;
 *  3. x and y exchanged: one horizontal half (all 16 columns of 8 rows);
 *  4. back to identity with SetViewportSwizzles: the left half again.
 * Per viewport (ShaderKey.vp_swizzle_vp): viewport 0 identity, viewport 1 x = -x, with a vertex program that writes the viewport index (attribute 0x68):
 *  5. index 1: viewport 1's swizzle, the right half;   6. a program that writes no index: viewport 0's, the left half;
 *  7. index 1 with viewport 1 = x <-> y: a horizontal half;   8. index 0 with viewport 0 = x = -x and viewport 1 identity: the right half.
 *  9. the same through a geometry program that forwards the triangle and writes viewport index 1 (the GS pass-through applies it): the right half.
 * Only viewport 0's rectangle reaches Metal (the replay sets one viewport), so viewports 0 and 1 share the rectangle here.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_RT = 0x100000, OFF_RB = H_READBACK };

static H h;
#define ALDV(d, addr, n, vreg) (0xefd8000000000000ull | (uint64_t)(d) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(vreg) << 39) | PT_BITS)
#define OUT_EMIT(d) (0xfbe0000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | (1ull << 39) | PT_BITS)
static NVNprogram *prog, *prog_gs, *prog_vi[2]; /* prog_vi[i]: the vertex program also writes viewport index i */
static NVNtexture *rt;

/* draws `p` with swizzles `sw` (x, y, z, w outputs per viewport) on viewports 0..nvp-1, returns the covered pixels as a bit per pixel in `cov` */
static int draw_vp(NVNprogram *p, const NVNviewportSwizzle *sw, int nvp, uint8_t *cov) {
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    const NVNtexture *rts[1] = {rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    const float vps[8] = {0, 0, W, W, 0, 0, W, W};
    L(nvnCommandBufferSetViewports)(cb, 0, 2, vps);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    float zero[4] = {0, 0, 0, 0};
    L(nvnCommandBufferClearColor)(cb, 0, zero, NVN_CLEAR_COLOR_MASK_RGBA);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 6 * 16);
    L(nvnCommandBufferBindProgram)(cb, p, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT);
    L(nvnCommandBufferSetViewportSwizzles)(cb, 0, nvp, sw);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    const float *px = (const float *)(h.mem + OFF_RB);
    int n = 0;
    for (int i = 0; i < W * W; i++) n += cov[i] = px[i] == 1.0f;
    return n;
}
static int draw(const NVNviewportSwizzle sw[4], uint8_t *cov) { return draw_vp(prog, sw, 1, cov); }
/* covered pixels form exactly the half: 0 left, 1 right, 2 top rows, 3 bottom rows */
static int is_half(const uint8_t *cov, int which) {
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            int in = which == 0 ? x < W / 2 : which == 1 ? x >= W / 2 : which == 2 ? y < W / 2 : y >= W / 2;
            if (cov[y * W + x] != in) return 0;
        }
    return 1;
}
static void show(const char *what, const uint8_t *cov, int n) {
    printf("%s: %d pixels covered; row 0 ", what, n);
    for (int x = 0; x < W; x++) putchar(cov[x] ? '#' : '.');
    printf(" row 15 ");
    for (int x = 0; x < W; x++) putchar(cov[(W - 1) * W + x] ? '#' : '.');
    printf(" column 0 ");
    for (int y = 0; y < W; y++) putchar(cov[y * W] ? '#' : '.');
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    /* the left half of clip space as two triangles */
    float rect[24] = {-1, -1, 0, 1, 0, -1, 0, 1, -1, 1, 0, 1, 0, -1, 0, 1, 0, 1, 0, 1, -1, 1, 0, 1};
    memcpy(h.mem + OFF_VB, rect, sizeof rect);
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x2000, fctl, 1, fs_i, 3, 0x1);
    prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");
    for (int i = 0; i < 2; i++) {
        uint64_t vi_i[] = {ins_ald(0, 0x80, 4), ins_mov32i(4, (uint32_t)i), ins_ast(4, 0x68, 1), ins_ast(0, 0x70, 4), ins_exit()};
        static uint8_t victl[2][0x880];
        build_shader(h.mem + OFF_SH + 0x4000 + 0x2000 * i, victl[i], 0, vi_i, 5, 0);
        prog_vi[i] = calloc(1, sizeof(NVNprogram));
        L(nvnProgramInitialize)(prog_vi[i], h.dev);
        NVNshaderData vd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x4000 + 0x2000 * i), victl[i]}, sd[1]};
        CHECK(L(nvnProgramSetShaders)(prog_vi[i], 2, vd), "shaders with viewport index %d", i);
    }
    { /* geometry program: forwards input vertices 0, 1, 2 and stores viewport index 1 with each */
        uint64_t gi[16];
        int n = 0;
        gi[n++] = ins_mov32i(10, 1);
        gi[n++] = ins_mov32i(11, 2);
        gi[n++] = ins_mov32i(12, 1);
        for (int k = 0; k < 3; k++) {
            gi[n++] = ALDV(0, 0x70, 4, k == 0 ? 255 : 9 + k);
            gi[n++] = ins_ast(12, 0x68, 1);
            gi[n++] = ins_ast(0, 0x70, 4);
            gi[n++] = OUT_EMIT(13);
        }
        gi[n++] = ins_exit();
        static uint8_t gctl[0x880];
        uint8_t *m = h.mem + OFF_SH + 0x8000;
        build_shader(m, gctl, 2, gi, n, 0);
        uint32_t *sph = (uint32_t *)(m + 0x30);
        sph[3] = 7u << 24; /* output topology: triangle strip */
        sph[4] = 3;        /* maximum output vertices */
        prog_gs = calloc(1, sizeof(NVNprogram));
        L(nvnProgramInitialize)(prog_gs, h.dev);
        NVNshaderData gd[3] = {sd[0], {(NVNbufferAddress)(uintptr_t)m, gctl}, sd[1]};
        CHECK(L(nvnProgramSetShaders)(prog_gs, 3, gd), "shaders with a geometry program");
    }
    rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32F, W, W, 1, OFF_RT, 0);

    static const NVNviewportSwizzle id[4] = {NVN_VIEWPORT_SWIZZLE_POSITIVE_X, NVN_VIEWPORT_SWIZZLE_POSITIVE_Y, NVN_VIEWPORT_SWIZZLE_POSITIVE_Z,
                                             NVN_VIEWPORT_SWIZZLE_POSITIVE_W};
    static const NVNviewportSwizzle negx[4] = {NVN_VIEWPORT_SWIZZLE_NEGATIVE_X, NVN_VIEWPORT_SWIZZLE_POSITIVE_Y, NVN_VIEWPORT_SWIZZLE_POSITIVE_Z,
                                               NVN_VIEWPORT_SWIZZLE_POSITIVE_W};
    static const NVNviewportSwizzle xy[4] = {NVN_VIEWPORT_SWIZZLE_POSITIVE_Y, NVN_VIEWPORT_SWIZZLE_POSITIVE_X, NVN_VIEWPORT_SWIZZLE_POSITIVE_Z,
                                             NVN_VIEWPORT_SWIZZLE_POSITIVE_W};
    uint8_t cov[W * W];
    int n = draw(id, cov);
    show("identity", cov, n);
    CHECK(is_half(cov, 0), "identity swizzle: the left half is covered");
    n = draw(negx, cov);
    show("x = -x", cov, n);
    CHECK(is_half(cov, 1), "NEGATIVE_X swizzle: the right half is covered");
    n = draw(xy, cov);
    show("x <-> y", cov, n);
    CHECK(is_half(cov, 2) || is_half(cov, 3), "x / y exchanged: one horizontal half is covered");
    n = draw(id, cov);
    show("identity again", cov, n);
    CHECK(is_half(cov, 0), "swizzle reset to identity: the left half again");

    /* per viewport */
    const NVNviewportSwizzle v01[8] = {id[0], id[1], id[2], id[3], negx[0], negx[1], negx[2], negx[3]};
    n = draw_vp(prog_vi[1], v01, 2, cov);
    show("index 1, vp1 x = -x", cov, n);
    CHECK(is_half(cov, 1), "viewport index 1: viewport 1's swizzle (x = -x), not viewport 0's: the right half");
    n = draw_vp(prog, v01, 2, cov);
    show("no index, vp1 x = -x", cov, n);
    CHECK(is_half(cov, 0), "a program that writes no viewport index gets viewport 0's swizzle (identity): the left half");
    const NVNviewportSwizzle v0xy[8] = {id[0], id[1], id[2], id[3], xy[0], xy[1], xy[2], xy[3]};
    n = draw_vp(prog_vi[1], v0xy, 2, cov);
    show("index 1, vp1 x <-> y", cov, n);
    CHECK(is_half(cov, 2) || is_half(cov, 3), "viewport index 1 with viewport 1 = x <-> y: a horizontal half");
    const NVNviewportSwizzle vnx0[8] = {negx[0], negx[1], negx[2], negx[3], id[0], id[1], id[2], id[3]};
    n = draw_vp(prog_vi[0], vnx0, 2, cov);
    show("index 0, vp0 x = -x", cov, n);
    CHECK(is_half(cov, 1), "viewport index 0 written explicitly: viewport 0's swizzle (x = -x): the right half");
    n = draw_vp(prog_gs, v01, 2, cov);
    show("GS index 1, vp1 x = -x", cov, n);
    CHECK(is_half(cov, 1), "geometry program writing viewport index 1: viewport 1's swizzle in the pass-through: the right half");
    n = draw_vp(prog_gs, v0xy, 2, cov);
    show("GS index 1, vp1 x <-> y", cov, n);
    CHECK(is_half(cov, 2) || is_half(cov, 3), "geometry program, viewport 1 = x <-> y: a horizontal half");
    return h_finish("t_vp_swizzle", &h);
}
