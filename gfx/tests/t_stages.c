/* t_stages.c - the replay of the stages Metal lacks (docs/architecture/gfx.md section 13, src/mtl_stages.inc), end to end through the NVN entry points:
 *  1. transform feedback: a plain triangle list captured straight from the vertex program, then (after Pause / Resume) an indexed, instanced triangle strip
 *     captured through stage-out records and the copy kernel; the captured words are compared exactly, the control memory holds the byte counts, and
 *     DrawTransformFeedback draws the captured vertices.
 *  2. geometry shader: points expanded to quads (4 emitted vertices, triangle strip) by compute emulation; pixels checked.
 *  3. tessellation: a quad patch with the NVN default levels (no tess control program) and a triangle patch whose levels the tess control program writes;
 *     the tess eval program places the vertices at control point 0 + tess coordinate; pixels checked.
 *  4. software tessellator: tess eval output captured (quad, both windings; indirect triangle patches with a tess control program), isolines drawn and
 *     captured, a geometry program after tessellation drawn and captured; exact words.
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
#define MAX_STAGES 30 /* 0x1000-byte shader slots between OFF_SH and OFF_VB */
static uint8_t g_ctl[MAX_STAGES][0x880];

/* builds one stage at its own 0x1000-byte slot of the pool; returns its shader data. sph: SPH words 2, 3, 4 to set (0 = leave) */
static NVNshaderData stage(int st, const uint64_t *ins, int n, uint32_t omap, uint32_t w2, uint32_t w3, uint32_t w4) {
    if (g_nsh >= MAX_STAGES) {
        CHECK(0, "more than %d shader stages built", MAX_STAGES);
        exit(1);
    }
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
static NVNshaderData vs_pos(void) { /* position from attribute 0 (built once, shared by the programs) */
    static NVNshaderData sd;
    uint64_t i[] = {ALDV(0, 0x80, 4, 0), ASTP(0, 0x70, 4, 0), ins_exit()};
    if (!sd.data) sd = stage(0, i, 3, 0, 0, 0, 0);
    return sd;
}
static NVNshaderData fs_red(void) {
    static NVNshaderData sd;
    uint64_t i[] = {ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    if (!sd.data) sd = stage(1, i, 5, 0xf, 0, 0, 0);
    return sd;
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

/* ---- 2b. geometry shader instancing: 2 invocations, invocation i shifts its quad by +i in x ---- */
#define I2F_U32(d, b) (0x5cb8000000000000ull | (uint64_t)(d) | (2ull << 8) | (2ull << 10) | ((uint64_t)(b) << 20) | PT_BITS)
static int gs_quad(uint64_t *gi, int n, int shift_reg) { /* emits a quad of half-size 0.25 around vertex 0 (+ shift_reg in x when >= 0) */
    const float s = 0.25f, cx[4] = {-s, s, -s, s}, cy[4] = {-s, -s, s, s};
    for (int c = 0; c < 4; c++) {
        gi[n++] = ALDV(0, 0x70, 4, 255);
        gi[n++] = fadd32i(0, 0, cx[c]);
        gi[n++] = fadd32i(1, 1, cy[c]);
        if (shift_reg >= 0) gi[n++] = fadd(0, 0, shift_reg);
        gi[n++] = ASTP(0, 0x70, 4, 0);
        gi[n++] = OUT_EMIT(10);
    }
    return n;
}
static void test_gs_instanced(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[2][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t gi[40];
    int n = 0;
    gi[n++] = S2R(2, 0x11); /* SR_INVOCATION_ID */
    gi[n++] = I2F_U32(8, 2); /* r8: the position load below fills r0..r3 */
    n = gs_quad(gi, n, 8);
    gi[n++] = ins_exit();
    /* SPH word 2 bits 24..31: 2 invocations per primitive */
    NVNshaderData sd[3] = {vs_pos(), stage(2, gi, n, 0, 2u << 24, 7u << 24, 4), fs_red()};
    NVNprogram *prog = program(3, sd);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_POINTS, 0, 2);
    readback();
    h_submit(&h);
    uint32_t a = px(pxx(-0.5f), pxy(-0.5f)), b = px(pxx(0.5f), pxy(0.5f)), c = px(pxx(0.5f), pxy(-0.5f)), e = px(pxx(-0.5f), pxy(0.5f));
    CHECK(a == RED && b == RED, "invocation 0 quads at both points: %08x %08x", a, b);
    CHECK(c == RED, "invocation 1 quad of point 0 shifted by +1 in x: %08x", c);
    CHECK(e == 0, "nothing where no invocation emits: %08x", e);
}

/* ---- 2c. triangle strip with adjacency: the GS forwards the triangle's own vertices (input vertices 0, 2, 4) ---- */
static void test_gs_strip_adjacency(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    /* 8 vertices = 2 triangles: triangle vertices (even) form the quad [-0.5, 0.5]^2, adjacency vertices (odd) sit far away */
    const float v[8][4] = {{-0.5f, -0.5f, 0, 1}, {2, 2, 0, 1}, {0.5f, -0.5f, 0, 1}, {2, 2, 0, 1}, {-0.5f, 0.5f, 0, 1}, {2, 2, 0, 1}, {0.5f, 0.5f, 0, 1}, {2, 2, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t gi[24];
    int n = 0;
    gi[n++] = ins_mov32i(10, 2);
    gi[n++] = ins_mov32i(11, 4);
    gi[n++] = ALDV(0, 0x70, 4, 255); gi[n++] = ASTP(0, 0x70, 4, 0); gi[n++] = OUT_EMIT(12);
    gi[n++] = ALDV(0, 0x70, 4, 10); gi[n++] = ASTP(0, 0x70, 4, 0); gi[n++] = OUT_EMIT(12);
    gi[n++] = ALDV(0, 0x70, 4, 11); gi[n++] = ASTP(0, 0x70, 4, 0); gi[n++] = OUT_EMIT(12);
    gi[n++] = ins_exit();
    NVNshaderData sd[3] = {vs_pos(), stage(2, gi, n, 0, 0, 7u << 24, 3), fs_red()};
    NVNprogram *prog = program(3, sd);
    NVNpolygonState ps;
    h_begin(&h);
    common_state(0);
    L(nvnPolygonStateSetDefaults)(&ps); L(nvnPolygonStateSetCullFace)(&ps, NVN_FACE_NONE); L(nvnCommandBufferBindPolygonState)(h.cb, &ps);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY, 0, 8);
    readback();
    h_submit(&h);
    uint32_t c0 = px(32, 32), c1 = px(pxx(-0.4f), pxy(-0.4f)), c2 = px(pxx(0.4f), pxy(0.4f)), o = px(pxx(0.8f), pxy(0.8f)), o2 = px(2, 2);
    CHECK(c0 == RED && c1 == RED && c2 == RED, "both strip triangles drawn from their own vertices: %08x %08x %08x", c0, c1, c2);
    CHECK(o == 0 && o2 == 0, "adjacency vertices are not drawn: %08x %08x", o, o2);
}

/* ---- 2d. indirect draw with a geometry program (arguments read at encode time) ---- */
static void test_gs_indirect(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[3][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t gi[32];
    int n = gs_quad(gi, 0, -1);
    gi[n++] = ins_exit();
    NVNshaderData sd[3] = {vs_pos(), stage(2, gi, n, 0, 0, 7u << 24, 4), fs_red()};
    NVNprogram *prog = program(3, sd);
    uint32_t *args = (uint32_t *)(h.mem + OFF_CTL + 0x100); /* count, instances, first, base instance: the two points from vertex 1 */
    args[0] = 2; args[1] = 1; args[2] = 1; args[3] = 0;
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawArraysIndirect)(h.cb, NVN_DRAW_PRIMITIVE_POINTS, (NVNbufferAddress)(uintptr_t)args);
    readback();
    h_submit(&h);
    uint32_t a = px(pxx(-0.5f), pxy(-0.5f)), b = px(pxx(0.5f), pxy(0.5f)), c = px(pxx(0.5f), pxy(-0.5f));
    CHECK(b == RED && c == RED, "indirect GS draw: quads at vertices 1 and 2: %08x %08x", b, c);
    CHECK(a == 0, "vertex 0 (before `first`) not drawn: %08x", a);
}

/* ---- 2e. transform feedback of geometry shader output: the quads' triangles, decomposed from the strips, counted on the GPU ---- */
static void test_gs_xfb(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[2][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t gi[32];
    int n = gs_quad(gi, 0, -1);
    gi[n++] = ins_exit();
    NVNshaderData sd[3] = {vs_pos(), stage(2, gi, n, 0, 0, 7u << 24, 4), fs_red()};
    NVNprogram *prog = program(3, sd);
    gfx_xfb_layout xl;
    memset(&xl, 0, sizeof xl);
    xl.count = 1;
    xl.stride[0] = 16;
    xl.e[0] = (ShaderXfbEntry){0x70, 0, 4, 0};
    CHECK(gfx_program_set_xfb_layout(prog, &xl), "xfb layout registered for the geometry program");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    h_begin(&h);
    common_state(1);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    NVNbufferRange xr[1] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), 0x4000}};
    L(nvnCommandBufferBindTransformFeedbackBuffers)(h.cb, 0, 1, xr);
    L(nvnCommandBufferBeginTransformFeedback)(h.cb, ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_POINTS, 0, 2);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    h_submit(&h);
    /* per point: strip c0 c1 c2 c3 -> triangles (c0, c1, c2), (c2, c1, c3) */
    const float s = 0.25f, cx[4] = {-s, s, -s, s}, cy[4] = {-s, -s, s, s};
    const int tri[6] = {0, 1, 2, 2, 1, 3};
    const uint32_t *x0 = (const uint32_t *)(h.mem + OFF_X0);
    int bad = 0;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 6; i++) {
            const uint32_t *w = x0 + (p * 6 + i) * 4;
            if (w[0] != fbits(v[p][0] + cx[tri[i]]) || w[1] != fbits(v[p][1] + cy[tri[i]]) || w[2] != fbits(0) || w[3] != fbits(1)) bad++;
        }
    if (bad) printf("  gs xfb vertex 0: %08x %08x %08x %08x (expected %08x %08x)\n", x0[0], x0[1], x0[2], x0[3], fbits(-0.75f), fbits(-0.75f));
    CHECK(bad == 0, "GS output captured as 12 decomposed triangle vertices (%d differ)", bad);
    CHECK(x0[12 * 4] == 0xeeeeeeeeu, "nothing written past the captured vertices");
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 12 * 16 && cw[4] == 16, "control memory written by the GPU: bytes %u stride %u", cw[0], cw[4]);
    /* DrawTransformFeedback from that control memory (GPU-side count): the two quads appear */
    NVNshaderData sd2[2] = {vs_pos(), fs_red()};
    NVNprogram *plain = program(2, sd2);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindVertexBuffer)(h.cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), 0x4000);
    L(nvnCommandBufferBindProgram)(h.cb, plain, ALL_GFX_BITS);
    clear_rt();
    L(nvnCommandBufferDrawTransformFeedback)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, ctl);
    readback();
    h_submit(&h);
    uint32_t a = px(pxx(-0.5f), pxy(-0.5f)), b = px(pxx(0.5f), pxy(0.5f)), e = px(32, 32);
    CHECK(a == RED && b == RED && e == 0, "DrawTransformFeedback of the GPU-counted capture draws the quads: %08x %08x %08x", a, b, e);
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

/* ---- 4. software tessellator (src/mtl_stages.inc): capture of tess eval output, geometry after tessellation, isolines, indirect patches ---- */
static NVNshaderData tes_cp0_uv(void) { /* tess eval: position = control point 0 + (u, v, 0, 0) */
    static NVNshaderData sd;
    uint64_t ti[] = {ALDV(0, 0x70, 4, 255), ALDV(8, 0x2f0, 2, 255), fadd(0, 0, 8), fadd(1, 1, 9), ASTP(0, 0x70, 4, 0), ins_exit()};
    if (!sd.data) sd = stage(4, ti, 6, 0, 0, 0, 0);
    return sd;
}
static gfx_xfb_layout xfb_pos16(void) { /* buffer 0: the position, 16 bytes per vertex */
    gfx_xfb_layout xl;
    memset(&xl, 0, sizeof xl);
    xl.count = 1;
    xl.stride[0] = 16;
    xl.e[0] = (ShaderXfbEntry){0x70, 0, 4, 0};
    return xl;
}
static void xfb_begin(NVNbufferAddress ctl) {
    NVNbufferRange xr[1] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), 0x4000}};
    L(nvnCommandBufferBindTransformFeedbackBuffers)(h.cb, 0, 1, xr);
    L(nvnCommandBufferBeginTransformFeedback)(h.cb, ctl);
}
/* compares the captured positions of buffer 0 with exp[n] (z = 0, w = 1); returns the number that differ */
static int xfb_cmp(const float (*exp)[2], int n, const char *what) {
    const uint32_t *x0 = (const uint32_t *)(h.mem + OFF_X0);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t *w = x0 + i * 4;
        if (w[0] != fbits(exp[i][0]) || w[1] != fbits(exp[i][1]) || w[2] != fbits(0) || w[3] != fbits(1)) {
            if (!bad) printf("  %s: vertex %d = %08x %08x %08x %08x, expected %08x %08x 0 3f800000\n", what, i, w[0], w[1], w[2], w[3], fbits(exp[i][0]), fbits(exp[i][1]));
            bad++;
        }
    }
    CHECK(x0[n * 4] == 0xeeeeeeeeu, "%s: nothing written past the %d captured vertices", what, n);
    return bad;
}
/* the software tessellator's quad grid of n x n cells over [x0, x0 + 1] x [y0, y0 + 1]: triangles (a, b, c), (a, c, d) per cell, row-major */
static int quad_grid(int n, float x0, float y0, float (*o)[2]) {
    int w = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float u0 = (float)i / n, u1 = (float)(i + 1) / n, v0 = (float)j / n, v1 = (float)(j + 1) / n;
            const float t[6][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v0}, {u1, v1}, {u0, v1}};
            for (int k = 0; k < 6; k++) { o[w][0] = x0 + t[k][0]; o[w][1] = y0 + t[k][1]; w++; }
        }
    return w;
}

/* 4a. transform feedback of tess eval output: quad patch at level 2 (no tess control program), drawn as well */
static void test_tes_xfb(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[4][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}, {-0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    NVNshaderData sd[3] = {vs_pos(), tes_cp0_uv(), fs_red()};
    NVNprogram *prog = program(3, sd);
    gfx_tess_params tp = {TESS_DOMAIN_QUAD, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
    gfx_xfb_layout xl = xfb_pos16();
    CHECK(gfx_program_set_tess_params(prog, &tp) && gfx_program_set_xfb_layout(prog, &xl), "tess params and xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    float outer[4] = {2, 2, 2, 2}, inner[2] = {2, 2};
    L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, outer);
    L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, inner);
    L(nvnCommandBufferSetPatchSize)(h.cb, 4);
    clear_rt();
    xfb_begin(ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    readback();
    h_submit(&h);
    float exp[24][2];
    int n = quad_grid(2, -0.5f, -0.5f, exp);
    int bad = xfb_cmp(exp, n, "tes xfb");
    CHECK(n == 24 && bad == 0, "quad patch at level 2 captured as 8 triangles in grid order (%d of %d differ)", bad, n);
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 24 * 16 && cw[4] == 16, "control memory: bytes %u stride %u", cw[0], cw[4]);
    CHECK(px(32, 32) == RED && px(pxx(-0.45f), pxy(-0.45f)) == RED && px(4, 4) == 0, "the patch is also drawn (software path): %08x %08x %08x", px(32, 32),
          px(pxx(-0.45f), pxy(-0.45f)), px(4, 4));
    /* clockwise winding: every triangle's second and third vertices swap */
    tp.winding = TESS_WINDING_CW;
    CHECK(gfx_program_set_tess_params(prog, &tp), "cw params registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    h_begin(&h);
    common_state(1);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, outer);
    L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, inner);
    L(nvnCommandBufferSetPatchSize)(h.cb, 4);
    xfb_begin(ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    h_submit(&h);
    for (int t = 0; t < n; t += 3) {
        float tmp[2] = {exp[t + 1][0], exp[t + 1][1]};
        exp[t + 1][0] = exp[t + 2][0]; exp[t + 1][1] = exp[t + 2][1];
        exp[t + 2][0] = tmp[0]; exp[t + 2][1] = tmp[1];
    }
    bad = xfb_cmp(exp, n, "tes xfb cw");
    CHECK(bad == 0, "clockwise winding swaps each triangle's last two vertices (%d of %d differ)", bad, n);
}

/* 4b. indirect patches with a tess control program (levels 4, triangle domain) captured, rasterizer off */
static void test_tes_xfb_indirect(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[3][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}, {-0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t ci[] = {S2R(0, 0x11), ALDV(4, 0x70, 4, 0), ASTP(4, 0x70, 4, 0), ins_mov32i(8, fbits(4)), ins_mov32i(9, fbits(4)), ins_mov32i(10, fbits(4)),
                     ins_mov32i(11, fbits(4)), ASTP(8, 0x00, 4, 1), ASTP(8, 0x10, 2, 1), ins_exit()};
    NVNshaderData sd[4] = {vs_pos(), stage(3, ci, 10, 0, 3u << 24, 0, 0), tes_cp0_uv(), fs_red()};
    NVNprogram *prog = program(4, sd);
    gfx_tess_params tp = {TESS_DOMAIN_TRIANGLE, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
    gfx_xfb_layout xl = xfb_pos16();
    CHECK(gfx_program_set_tess_params(prog, &tp) && gfx_program_set_xfb_layout(prog, &xl), "tess params and xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    uint32_t *args = (uint32_t *)(h.mem + OFF_CTL + 0x100); /* count, instances, first, base instance */
    args[0] = 3; args[1] = 1; args[2] = 0; args[3] = 0;
    h_begin(&h);
    common_state(1);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    L(nvnCommandBufferSetPatchSize)(h.cb, 3);
    xfb_begin(ctl);
    L(nvnCommandBufferDrawArraysIndirect)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, (NVNbufferAddress)(uintptr_t)args);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    h_submit(&h);
    /* barycentric grid n = 4: rows j, triangles (i, j) (i + 1, j) (i, j + 1) and, inside, (i + 1, j) (i + 1, j + 1) (i, j + 1) */
    float exp[48][2];
    int w = 0;
    const int nn = 4;
    for (int j = 0; j < nn; j++)
        for (int i = 0; i + j < nn; i++) {
            const int t[2][3][2] = {{{i, j}, {i + 1, j}, {i, j + 1}}, {{i + 1, j}, {i + 1, j + 1}, {i, j + 1}}};
            for (int q = 0; q < (i + j + 1 < nn ? 2 : 1); q++)
                for (int k = 0; k < 3; k++) { exp[w][0] = -0.5f + (float)t[q][k][0] / nn; exp[w][1] = -0.5f + (float)t[q][k][1] / nn; w++; }
        }
    int bad = xfb_cmp(exp, w, "tes xfb indirect");
    CHECK(w == 48 && bad == 0, "indirect triangle patch at the program's level 4: 16 triangles captured (%d of %d differ)", bad, w);
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 48 * 16 && cw[4] == 16, "control memory: bytes %u stride %u", cw[0], cw[4]);
}

/* 4c. isolines (Metal has none): outer {2, 4} = 2 lines of 4 segments, drawn and captured */
static void test_isolines(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float y0 = -0.515625f; /* line rows land on pixel centres: rows 48 and 32 */
    const float v[4][4] = {{-0.5f, y0, 0, 1}, {0.5f, y0, 0, 1}, {-0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    NVNshaderData sd[3] = {vs_pos(), tes_cp0_uv(), fs_red()};
    NVNprogram *prog = program(3, sd);
    gfx_tess_params tp = {TESS_DOMAIN_ISOLINE, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
    gfx_xfb_layout xl = xfb_pos16();
    CHECK(gfx_program_set_tess_params(prog, &tp) && gfx_program_set_xfb_layout(prog, &xl), "isoline params and xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    float outer[4] = {2, 4, 0, 0}, inner[2] = {0, 0};
    L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, outer);
    L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, inner);
    L(nvnCommandBufferSetPatchSize)(h.cb, 4);
    clear_rt();
    xfb_begin(ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    readback();
    h_submit(&h);
    float exp[16][2];
    int w = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 2; k++) { exp[w][0] = -0.5f + (float)(i + k) / 4; exp[w][1] = y0 + (float)j / 2; w++; }
    int bad = xfb_cmp(exp, w, "isolines");
    CHECK(bad == 0, "isolines: 2 lines x 4 segments captured as 16 line vertices (%d differ)", bad);
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 16 * 16 && cw[4] == 16, "control memory: bytes %u stride %u", cw[0], cw[4]);
    uint32_t l0 = px(32, 48), l1 = px(24, 32), gap = px(32, 40), above = px(32, 20);
    CHECK(l0 == RED && l1 == RED, "both lines drawn (v = 0 at row 48, v = 0.5 at row 32): %08x %08x", l0, l1);
    CHECK(gap == 0 && above == 0, "no line at v = 1 (isolines stop before it) or between: %08x %08x", gap, above);
}

/* 4d. geometry and tessellation together: a quad patch at level 2 (8 triangles) -> the GS emits a quad around each triangle's vertex 0; its output is
 * drawn and captured (8 x 2 triangles, GPU-counted) */
static void test_tess_gs(void) {
    float *vb = (float *)(h.mem + OFF_VB);
    const float v[4][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}, {-0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    memcpy(vb, v, sizeof v);
    uint64_t gi[32];
    int n = gs_quad(gi, 0, -1);
    gi[n++] = ins_exit();
    NVNshaderData sd[4] = {vs_pos(), stage(2, gi, n, 0, 0, 7u << 24, 4), tes_cp0_uv(), fs_red()};
    NVNprogram *prog = program(4, sd);
    gfx_tess_params tp = {TESS_DOMAIN_QUAD, TESS_SPACING_EQUAL, TESS_WINDING_CCW};
    gfx_xfb_layout xl = xfb_pos16();
    CHECK(gfx_program_set_tess_params(prog, &tp) && gfx_program_set_xfb_layout(prog, &xl), "tess params and xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, 0x4000);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctl = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    h_begin(&h);
    common_state(0);
    L(nvnCommandBufferBindProgram)(h.cb, prog, ALL_GFX_BITS);
    float outer[4] = {2, 2, 2, 2}, inner[2] = {2, 2};
    L(nvnCommandBufferSetOuterTessellationLevels)(h.cb, outer);
    L(nvnCommandBufferSetInnerTessellationLevels)(h.cb, inner);
    L(nvnCommandBufferSetPatchSize)(h.cb, 4);
    clear_rt();
    xfb_begin(ctl);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 4);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctl);
    readback();
    h_submit(&h);
    /* triangle t of cell (i, j) has vertex 0 at (-0.5 + i / 2, -0.5 + j / 2); its quad strip c0 c1 c2 c3 -> triangles (c0, c1, c2), (c2, c1, c3) */
    const float s = 0.25f, cx[4] = {-s, s, -s, s}, cy[4] = {-s, -s, s, s};
    const int tri[6] = {0, 1, 2, 2, 1, 3};
    float exp[48][2];
    int w = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < 6; k++) { exp[w][0] = -0.5f + (float)i / 2 + cx[tri[k]]; exp[w][1] = -0.5f + (float)j / 2 + cy[tri[k]]; w++; }
    int bad = xfb_cmp(exp, w, "tess + gs xfb");
    CHECK(bad == 0, "geometry output after tessellation captured in primitive order: 48 vertices (%d differ)", bad);
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    CHECK(cw[0] == 48 * 16 && cw[4] == 16, "control memory written by the GPU: bytes %u stride %u", cw[0], cw[4]);
    uint32_t a = px(pxx(-0.6f), pxy(-0.6f)), b = px(pxx(0.1f), pxy(0.1f)), c = px(pxx(0.5f), pxy(0.5f)), d = px(pxx(0.4f), pxy(-0.6f));
    CHECK(a == RED && b == RED, "GS quads drawn around the tessellated vertices: %08x %08x", a, b);
    CHECK(c == 0 && d == 0, "nothing beyond [-0.75, 0.25]^2 (the tessellated triangles themselves are not drawn): %08x %08x", c, d);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    g_rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, NVN_TEXTURE_FLAGS_DISPLAY);
    test_xfb();
    test_gs();
    test_gs_instanced();
    test_gs_strip_adjacency();
    test_gs_indirect();
    test_gs_xfb();
    test_tess();
    test_tes_xfb();
    test_tes_xfb_indirect();
    test_isolines();
    test_tess_gs();
    return h_finish("t_stages", &h);
}
