/* t_vp_swizzle.c - nvnCommandBufferSetViewportSwizzles: the clip position the vertex program writes is swizzled per output component before the
 * viewport transform (ShaderKey.vp_swizzle, applied in the vertex function). A rectangle covering the left half of clip space (x in [-1, 0]) is drawn
 * with a constant colour into a 16x16 R32F target:
 *  1. identity: the left half is covered;
 *  2. x = -x: the right half;
 *  3. x and y exchanged: one horizontal half (all 16 columns of 8 rows);
 *  4. back to identity with SetViewportSwizzles: the left half again.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_RT = 0x100000, OFF_RB = H_READBACK };

static H h;
static NVNprogram *prog;
static NVNtexture *rt;

/* draws with swizzles `sw` (x, y, z, w outputs) on viewport 0, returns the covered pixels as a bit per pixel in `cov` */
static int draw(const NVNviewportSwizzle sw[4], uint8_t *cov) {
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
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 6 * 16);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferSetViewportSwizzles)(cb, 0, 1, sw);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    const float *px = (const float *)(h.mem + OFF_RB);
    int n = 0;
    for (int i = 0; i < W * W; i++) n += cov[i] = px[i] == 1.0f;
    return n;
}
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
    return h_finish("t_vp_swizzle", &h);
}
