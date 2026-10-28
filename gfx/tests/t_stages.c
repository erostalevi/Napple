/* t_stages.c - the replay of the stages Metal lacks (docs/architecture/gfx.md section 13, src/mtl_stages.inc), end to end through the NVN entry points:
 *  1. transform feedback: a plain triangle list captured straight from the vertex program, then (after Pause / Resume) an indexed, instanced triangle strip
 *     captured through stage-out records and the copy kernel; the captured words are compared exactly, the control memory holds the byte counts, and
 *     DrawTransformFeedback draws the captured vertices.
 *  2. geometry shader: points expanded to quads (4 emitted vertices, triangle strip) by compute emulation; pixels checked.
 *  3. tessellation: a quad patch with the NVN default levels (no tess control program) and a triangle patch whose levels the tess control program writes;
 *     the tess eval program places the vertices at control point 0 + tess coordinate; pixels checked.
 * Hand-assembled Maxwell code, headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
#include "stages.h"

#define W 64
#define ALDV(d, addr, n, vreg) (0xefd8000000000000ull | (uint64_t)(d) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(vreg) << 39) | PT_BITS)
#define ASTP(s, addr, n, patch) (0xeff0000000000000ull | (uint64_t)(s) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(patch) << 31) | PT_BITS)
#define S2R(d, sr) (0xf0c8000000000000ull | (uint64_t)(d) | ((uint64_t)(sr) << 20) | PT_BITS)
#define OUT_EMIT(d) (0xfbe0000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | (1ull << 39) | PT_BITS)
static uint64_t fadd32i(int d, int a, float f) { uint32_t u; memcpy(&u, &f, 4); return 0x0800000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)u << 20) | PT_BITS; }
static uint64_t fadd(int d, int a, int b) { return 0x5c58000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | PT_BITS; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

enum { OFF_SH = 0x1000, OFF_VB = 0x20000, OFF_IB = 0x21000, OFF_X0 = 0x22000, OFF_X1 = 0x26000, OFF_CTL = 0x28000, OFF_RT = 0x100000, OFF_RB = H_READBACK };
#define ALL_GFX_BITS (NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT | NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT)

static H h;
static NVNtexture *g_rt;
static int g_nsh;
static uint8_t g_ctl[8][0x880];

/* builds one stage at its own 0x1000-byte slot of the pool; returns its shader data. sph: SPH words 2, 3, 4 to set (0 = leave) */
static NVNshaderData stage(int st, const uint64_t *ins, int n, uint32_t omap, uint32_t w2, uint32_t w3, uint32_t w4) {
    uint8_t *m = h.mem + OFF_SH + 0x1000 * (size_t)g_nsh;
    uint8_t *ctl = g_ctl[g_nsh++];
    build_shader(m, ctl, st, ins, n, omap);
    uint32_t *sph = (uint32_t *)(m + 0x30);
    if (w2) sph[2] = w2;
    if (w3) sph[3] = w3;
    if (w4) sph[4] = w4;
    return (NVNshaderData){(NVNbufferAddress)(uintptr_t)m, ctl};
}
static NVNprogram *program(int n, const NVNshaderData *sd) {
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(p, h.dev), "program init");
    CHECK(L(nvnProgramSetShaders)(p, n, sd), "set shaders");
    return p;
}
static NVNshaderData vs_pos(void) { /* position from attribute 0 */
    uint64_t i[] = {ALDV(0, 0x80, 4, 0), ASTP(0, 0x70, 4, 0), ins_exit()};
    return stage(0, i, 3, 0, 0, 0, 0);
}
static NVNshaderData fs_red(void) {
    uint64_t i[] = {ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    return stage(1, i, 5, 0xf, 0, 0, 0);
}

static void common_state(int raster_discard) {
    NVNcommandBuffer *cb = h.cb;
    const NVNtexture *rts[1] = {g_rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vst; L(nvnVertexStreamStateSetDefaults)(&vst); L(nvnVertexStreamStateSetStride)(&vst, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vst);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 0x1000);
    L(nvnCommandBufferSetRasterizerDiscard)(cb, raster_discard);
}
static void clear_rt(void) {
    float z[4] = {0, 0, 0, 0};
    L(nvnCommandBufferClearColor)(h.cb, 0, z, NVN_CLEAR_COLOR_MASK_RGBA);
}
static uint32_t px(int x, int y) { return ((const uint32_t *)(h.mem + OFF_RB))[y * W + x]; }
static void readback(void) {
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, g_rt, 0, 0, W, W, OFF_RB);
}
#define RED 0xff0000ffu
/* NDC (x, y) -> pixel (upper-left origin: +y is the top row) */
static int pxx(float x) { return (int)((x + 1) * 0.5f * W); }
static int pxy(float y) { return (int)((1 - y) * 0.5f * W); }

/* ---- 1. transform feedback ---- */
static void test_xfb(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    /* draw 1 (list): two triangles covering the target; draw 2 (indexed strip, vertices 6..9): a quad drawn again */
    const float v[10][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1},
                            {-1, -1, 0.25f, 1}, {1, -1, 0.25f, 1}, {-1, 1, 0.25f, 1}, {1, 1, 0.25f, 1}};
    memcpy(vb, v, sizeof v);
    uint16_t *ib = (uint16_t *)(h.mem + OFF_IB);
    ib[0] = 0; ib[1] = 1; ib[2] = 2; ib[3] = 3;
    /* vertex program: position = attribute 0, generic 0 = (7, 8, 9, 10) */
    uint64_t vi[] = {ALDV(0, 0x80, 4, 0), ASTP(0, 0x70, 4, 0), ins_mov32i(4, fbits(7)), ins_mov32i(5, fbits(8)), ins_mov32i(6, fbits(9)), ins_mov32i(7, fbits(10)),
                     ASTP(4, 0x80, 4, 0), ins_exit()};
    NVNshaderData sd[2] = {stage(0, vi, 8, 0, 0, 0, 0), fs_red()};
    NVNprogram *prog = program(2, sd);
    /* buffer 0: position + generic 0 (32 bytes per vertex); buffer 1: generic 0 .y .z at offset 4 of an 12-byte vertex */
    gfx_xfb_layout xl;
    memset(&xl, 0, sizeof xl);
    xl.count = 3;
    xl.stride[0] = 32;
    xl.stride[1] = 12;
    xl.e[0] = (ShaderXfbEntry){0x70, 0, 4, 0};
    xl.e[1] = (ShaderXfbEntry){0x80, 0, 4, 16};
    xl.e[2] = (ShaderXfbEntry){0x84, 1, 2, 4};
    CHECK(gfx_program_set_xfb_layout(prog, &xl), "xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_X1, 0xee, 0x2000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);

    h_begin(&h);
    common_state(1);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    NVNbufferRange xr[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), 0x4000}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_X1), 0x2000}};
    L(nvnCommandBufferBindTransformFeedbackBuffers)(h.cb, 0, 2, xr);
    L(nvnCommandBufferBeginTransformFeedback)(h.cb, ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
    L(nvnCommandBufferPauseTransformFeedback)(h.cb, ctl);
    L(nvnCommandBufferSetRasterizerDiscard)(h.cb, 0);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6); /* paused: rendered, not captured */
    L(nvnCommandBufferSetRasterizerDiscard)(h.cb, 1);
    L(nvnCommandBufferResumeTransformFeedback)(h.cb, ctl);
    L(nvnCommandBufferDrawElementsInstanced)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, NVN_INDEX_TYPE_UNSIGNED_SHORT, 4, (NVNbufferAddress)(uintptr_t)ib, 6, 0, 2);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    h_submit(&h);

    /* expected capture order: 6 list vertices, then per instance the strip's triangles (6,7,8) and (8,7,9) (odd triangle: first two swapped) */
    int order[18] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 7, 9, 6, 7, 8, 8, 7, 9};
    const uint32_t *x0 = (const uint32_t *)(h.mem + OFF_X0), *x1 = (const uint32_t *)(h.mem + OFF_X1);
    int bad0 = 0, bad1 = 0;
    for (int i = 0; i < 18; i++) {
        for (int c = 0; c < 4; c++) {
            if (x0[i * 8 + c] != fbits(v[order[i]][c])) bad0++;
            if (x0[i * 8 + 4 + c] != fbits(7.0f + c)) bad0++;
        }
        if (x1[i * 3] != 0xeeeeeeeeu || x1[i * 3 + 1] != fbits(8) || x1[i * 3 + 2] != fbits(9)) bad1++;
    }
    if (bad0) printf("  xfb0 vertex 6: %08x %08x %08x %08x | %08x\n", x0[48], x0[49], x0[50], x0[51], x0[52]);
    CHECK(bad0 == 0, "xfb buffer 0: %d words differ from the expected capture", bad0);
    CHECK(bad1 == 0, "xfb buffer 1: %d vertices differ (unwritten word 0 must keep its value)", bad1);
    CHECK(x0[18 * 8] == 0xeeeeeeeeu && x1[18 * 3 + 1] == 0xeeeeeeeeu, "nothing written past the captured vertices");
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 18 * 32 && cw[1] == 18 * 12 && cw[4] == 32 && cw[5] == 12, "control memory: bytes %u %u strides %u %u", cw[0], cw[1], cw[4], cw[5]);

    /* DrawTransformFeedback: buffer 0 as the vertex buffer (stride 32): 18 vertices = 6 triangles covering the target */
    h_begin(&h);
    common_state(0);
    NVNvertexStreamState vst; L(nvnVertexStreamStateSetDefaults)(&vst); L(nvnVertexStreamStateSetStride)(&vst, 32);
    L(nvnCommandBufferBindVertexStreamState)(h.cb, 1, &vst);
    L(nvnCommandBufferBindVertexBuffer)(h.cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), 0x4000);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawTransformFeedback)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, ctl);
    readback();
    h_submit(&h);
    CHECK(px(5, 5) == RED && px(58, 58) == RED && px(32, 32) == RED, "DrawTransformFeedback covers the target: %08x %08x %08x", px(5, 5), px(58, 58), px(32, 32));
}

/* ---- 2. geometry shader: point -> quad ---- */
static void test_gs(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[2][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    const float s = 0.25f, cx[4] = {-s, s, -s, s}, cy[4] = {-s, -s, s, s};
    uint64_t gi[32];
    int n = 0;
    for (int c = 0; c < 4; c++) {
        gi[n++] = ALDV(0, 0x70, 4, 255); /* vertex 0 of the input primitive */
        gi[n++] = fadd32i(0, 0, cx[c]);
        gi[n++] = fadd32i(1, 1, cy[c]);
        gi[n++] = ASTP(0, 0x70, 4, 0);
        gi[n++] = OUT_EMIT(10);
    }
    gi[n++] = ins_exit();
    /* SPH: word 3 bits 24..27 output topology (7 = triangle strip), word 4 bits 0..11 maximum output vertices */
    NVNshaderData sd[3] = {vs_pos(), stage(2, gi, n, 0, 0, 7u << 24, 4), fs_red()};
    NVNprogram *prog = program(3, sd);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_POINTS, 0, 2);
    readback();
    h_submit(&h);
    uint32_t a = px(pxx(-0.5f), pxy(-0.5f)), b = px(pxx(0.5f), pxy(0.5f)), e1 = px(pxx(-0.5f), pxy(0.5f)), e2 = px(32, 32), e3 = px(pxx(-0.5f) + 10, pxy(-0.5f));
    CHECK(a == RED && b == RED, "GS quads drawn at both points: %08x %08x", a, b);
    CHECK(e1 == 0 && e2 == 0 && e3 == 0, "nothing outside the quads: %08x %08x %08x", e1, e2, e3);
    /* the edges of a quad: 0.25 NDC = 8 pixels each way */
    CHECK(px(pxx(-0.5f) - 7, pxy(-0.5f) + 7) == RED && px(pxx(-0.5f) - 9, pxy(-0.5f)) == 0, "quad extent is the emitted size");
}

/* ---- 3. tessellation ---- */
static void test_tess(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[4][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}, {-0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    /* tess eval: position = control point 0 + (u, v, 0, 0) */
    uint64_t ti[] = {ALDV(0, 0x70, 4, 255), ALDV(8, 0x2f0, 2, 255), fadd(0, 0, 8), fadd(1, 1, 9), ASTP(0, 0x70, 4, 0), ins_exit()};
    /* (a) quad patch of 4 control points, no tess control program, levels from the NVN state */
    {
        NVNshaderData sd[3] = {vs_pos(), stage(4, ti, 6, 0, 0, 0, 0), fs_red()};
        NVNprogram *prog = program(3, sd);
        gfx_tess_params tp = {TESS_DOMAIN_QUAD, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
        CHECK(gfx_program_set_tess_params(prog, &tp), "tess params registered");
        h_begin(&h);
        common_state(0);
        L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
        float outer[4] = {4, 4, 4, 4}, inner[2] = {4, 4};
        L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, outer);
        L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, inner);
        L(nvnCommandBufferSetPatchSize)(h.cb, 4);
        clear_rt();
        L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
        readback();
        h_submit(&h);
        CHECK(px(32, 32) == RED && px(pxx(-0.45f), pxy(-0.45f)) == RED && px(pxx(0.45f), pxy(0.45f)) == RED, "quad patch covers [-0.5, 0.5]^2: %08x %08x %08x",
              px(32, 32), px(pxx(-0.45f), pxy(-0.45f)), px(pxx(0.45f), pxy(0.45f)));
        CHECK(px(4, 4) == 0 && px(60, 60) == 0, "outside the patch: %08x %08x", px(4, 4), px(60, 60));
        /* zero outer levels cull the patch (proves the factors reach the tessellator) */
        h_begin(&h);
        common_state(0);
        L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
        float zero[4] = {0, 0, 0, 0};
        L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, zero);
        L(nvnCommandBufferSetPatchSize)(h.cb, 4);
        clear_rt();
        L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
        readback();
        h_submit(&h);
        CHECK(px(32, 32) == 0, "zero outer levels cull the patch: %08x", px(32, 32));
    }
    /* (b) triangle patch of 3 control points; the tess control program copies the control points and writes the levels (outer 3 x 4, inner 4) */
    {
        uint64_t ci[] = {S2R(0, 0x11), ALDV(4, 0x70, 4, 0), ASTP(4, 0x70, 4, 0), ins_mov32i(8, fbits(4)), ins_mov32i(9, fbits(4)), ins_mov32i(10, fbits(4)),
                         ins_mov32i(11, fbits(4)), ASTP(8, 0x00, 4, 1), ASTP(8, 0x10, 2, 1), ins_exit()};
        /* SPH word 2 bits 24..31: output control points (= invocations per patch) */
        NVNshaderData sd[4] = {vs_pos(), stage(3, ci, 10, 0, 3u << 24, 0, 0), stage(4, ti, 6, 0, 0, 0, 0), fs_red()};
        NVNprogram *prog = program(4, sd);
        gfx_tess_params tp = {TESS_DOMAIN_TRIANGLE, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
        CHECK(gfx_program_set_tess_params(prog, &tp), "tess params registered");
        h_begin(&h);
        common_state(0);
        L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
        float zero[4] = {0, 0, 0, 0};
        L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, zero); /* the program's levels must win over these */
        L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, zero);
        L(nvnCommandBufferSetPatchSize)(h.cb, 3);
        clear_rt();
        L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
        readback();
        h_submit(&h);
        uint32_t in = px(pxx(-0.4f), pxy(-0.4f)), in2 = px(pxx(0.4f), pxy(-0.45f)), out = px(pxx(0.4f), pxy(0.4f)), out2 = px(4, 4);
        CHECK(in == RED && in2 == RED, "triangle patch near its legs: %08x %08x", in, in2);
        CHECK(out == 0 && out2 == 0, "beyond the hypotenuse / outside: %08x %08x", out, out2);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    g_rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, NVN_TEXTURE_FLAGS_DISPLAY);
    test_xfb();
    test_gs();
    test_tess();
    return h_finish("t_stages", &h);
}
