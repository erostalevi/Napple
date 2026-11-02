/* t_stages_dumped.c - real geometry programs of a title through the geometry replay (src/mtl_stages.inc), with synthetic input.
 * NAPPLE_GFX_STAGE_DUMP=<dir>: a NAPPLE_GFX_CTL_DUMP directory of a title run (s2-<hash>.ctl + .code per geometry program). Without it the test
 * skips (game code is never committed). Each program runs once per input primitive kind (points, lines, triangles: the draw decides it, the SPH does
 * not say) over two primitives of a unit-square vertex set, with its output captured (transform feedback layout: position + generic 0); the vertex
 * program writes position = attribute 0 and generic 0..3 = (0.25, 0.5, 0.75, 1). Checked: the draw goes through the geometry stage (the capture's
 * byte count is written by the GPU-counted session, see gfx.md section 13), every captured word is finite, nothing is written past the count, and
 * captured positions lie on the input vertices or inside their bounding box scaled by 64 (a GS that reads its vertices out of range produced garbage
 * there; with line input, a program that copies its input positions must not collapse every line to a point). The emitted counts are printed: every
 * constant buffer of the geometry stage reads 1.0f, so what a program computes from its uniforms is arbitrary but finite.
 * Run: NAPPLE_GFX_STAGE_DUMP=<dir> MTL_DEBUG_LAYER=1 build/tests/t_stages_dumped (docs/guides/gpu-title-scan.md). */
#include <dirent.h>
#include <math.h>
#include "nvn_harness.h"
#include "stages.h"

#define W 64
#define ALDV(d, addr, n, vreg) (0xefd8000000000000ull | (uint64_t)(d) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(vreg) << 39) | PT_BITS)
#define ASTP(s, addr, n) (0xeff0000000000000ull | (uint64_t)(s) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | PT_BITS)
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

enum { OFF_SH = 0x1000, OFF_GS = 0x8000, OFF_VB = 0x20000, OFF_X0 = 0x22000, X0_SIZE = 0x80000, OFF_CTL = 0xa8000, OFF_RT = 0x100000, OFF_UBO = 0x180000, UBO_SIZE = 0x10000 };
#define ALL_GFX_BITS (NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT)

static H h;
static NVNtexture *g_rt;
static uint8_t g_ctl_vs[0x880], g_ctl_fs[0x880];

static size_t read_file(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void state(void) {
    NVNcommandBuffer *cb = h.cb;
    const NVNtexture *rts[1] = {g_rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnPolygonStateSetCullFace)(&ps, NVN_FACE_NONE); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vst; L(nvnVertexStreamStateSetDefaults)(&vst); L(nvnVertexStreamStateSetStride)(&vst, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vst);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 0x1000);
    L(nvnCommandBufferSetRasterizerDiscard)(cb, 1);
    for (int i = 0; i < 14; i++) L(nvnCommandBufferBindUniformBuffer)(cb, NVN_SHADER_STAGE_GEOMETRY, i, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_UBO), UBO_SIZE);
}

/* One geometry program (code at OFF_GS, control `ctl`) with input primitive `mode`: returns the captured vertex count, -1 on a failed check. */
static int run(const char *name, const uint8_t *ctl, NVNshaderData vs, NVNshaderData fs, int mode, const char *mname, int nv) {
    NVNshaderData sd[3] = {vs, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_GS), ctl}, fs};
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(p, h.dev), "program init");
    if (!L(nvnProgramSetShaders)(p, 3, sd)) {
        CHECK(0, "%s: nvnProgramSetShaders refused the program", name);
        return -1;
    }
    gfx_xfb_layout xl;
    memset(&xl, 0, sizeof xl);
    xl.count = 2;
    xl.stride[0] = 32;
    xl.e[0] = (ShaderXfbEntry){0x70, 0, 4, 0};
    xl.e[1] = (ShaderXfbEntry){0x80, 0, 4, 16};
    CHECK(gfx_program_set_xfb_layout(p, &xl), "xfb layout registered");
    memset(h.mem + OFF_X0, 0xee, X0_SIZE);
    memset(h.mem + OFF_CTL, 0xee, 64);
    NVNbufferAddress ctlm = (NVNbufferAddress)(uintptr_t)(h.mem + OFF_CTL);
    h_begin(&h);
    state();
    L(nvnCommandBufferBindProgram)(h.cb, p, ALL_GFX_BITS);
    NVNbufferRange xr[1] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_X0), X0_SIZE}};
    L(nvnCommandBufferBindTransformFeedbackBuffers)(h.cb, 0, 1, xr);
    L(nvnCommandBufferBeginTransformFeedback)(h.cb, ctlm);
    L(nvnCommandBufferDrawArrays)(h.cb, mode, 0, 2 * nv);
    L(nvnCommandBufferEndTransformFeedback)(h.cb, ctlm);
    h_submit(&h);
    const uint32_t *cw = (const uint32_t *)(h.mem + OFF_CTL);
    const uint32_t *x = (const uint32_t *)(h.mem + OFF_X0);
    uint32_t bytes = cw[0], nvtx = bytes / 32;
    int ok = 1;
    if (bytes == 0xeeeeeeeeu || bytes > X0_SIZE || bytes % 32) {
        CHECK(0, "%s %s: control memory not written as a capture (bytes %#x)", name, mname, bytes);
        return -1;
    }
    int bad_val = 0, far = 0;
    for (uint32_t i = 0; i < nvtx * 8; i++) {
        float f;
        memcpy(&f, &x[i], 4);
        if (!isfinite(f)) bad_val++;
        if (i % 8 < 2 && fabsf(f) > 64.0f) far++; /* x, y of the position: inputs are within [-1, 1] */
    }
    if (nvtx * 8 < X0_SIZE / 4 && x[nvtx * 8] != 0xeeeeeeeeu) ok = 0;
    CHECK(bad_val == 0, "%s %s: %d non-finite captured words", name, mname, bad_val);
    CHECK(far == 0, "%s %s: %d captured coordinates far outside the input (out-of-range vertex reads)", name, mname, far);
    CHECK(ok, "%s %s: words written past the captured count", name, mname);
    /* A program that only copies input positions (every captured position is an input vertex) must not collapse every captured line to a point: that was
     * the signature of GLSLC vertex handles read as attribute addresses (SMO's alThunderShader drew zero-length lines before TRANSLATOR_VERSION 15). */
    if (mode == NVN_DRAW_PRIMITIVE_LINES && nvtx >= 2) {
        const float *vb = (const float *)(h.mem + OFF_VB);
        int copies = 1, nondegenerate = 0;
        for (uint32_t i = 0; i < nvtx && copies; i++) {
            int hit = 0;
            for (int k = 0; k < 2 * nv; k++) hit |= !memcmp(&x[8 * i], &vb[4 * k], 16);
            copies = hit;
        }
        for (uint32_t i = 0; i + 1 < nvtx; i += 2) nondegenerate += memcmp(&x[8 * i], &x[8 * (i + 1)], 16) != 0;
        CHECK(!copies || nondegenerate, "%s lines: every captured line collapses to one input vertex (vertex handles misread)", name);
        if (copies && !nondegenerate) ok = 0;
    }
    printf("  %s %-9s: %u vertices captured", name, mname, nvtx);
    if (nvtx) printf(" (first %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f)", *(const float *)&x[0], *(const float *)&x[1], *(const float *)&x[2], *(const float *)&x[3],
                     *(const float *)&x[4], *(const float *)&x[5], *(const float *)&x[6], *(const float *)&x[7]);
    printf("\n");
    if (getenv("T_VERBOSE"))
        for (uint32_t i = 0; i < nvtx && i < 64; i++)
            printf("    %2u: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", i, *(const float *)&x[8 * i], *(const float *)&x[8 * i + 1], *(const float *)&x[8 * i + 2],
                   *(const float *)&x[8 * i + 3], *(const float *)&x[8 * i + 4], *(const float *)&x[8 * i + 5], *(const float *)&x[8 * i + 6], *(const float *)&x[8 * i + 7]);
    return (bad_val || far || !ok) ? -1 : (int)nvtx;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = getenv("NAPPLE_GFX_STAGE_DUMP");
    if (!dir || !dir[0]) {
        printf("t_stages_dumped: ok (1 skipped: set NAPPLE_GFX_STAGE_DUMP to a NAPPLE_GFX_CTL_DUMP directory)\n");
        return 0;
    }
    if (h_init(&h, W, W, NULL)) return 1;
    g_rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, NVN_TEXTURE_FLAGS_DISPLAY);
    /* two of each primitive over a unit square: vertex i at the corners in turn */
    float *vb = (float *)(h.mem + OFF_VB);
    const float sq[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int i = 0; i < 8; i++) {
        vb[4 * i] = sq[i % 4][0];
        vb[4 * i + 1] = sq[i % 4][1];
        vb[4 * i + 2] = 0;
        vb[4 * i + 3] = 1;
    }
    float *ubo = (float *)(h.mem + OFF_UBO); /* every constant buffer of the geometry stage reads 1.0 (zeros give 0 / 0 in programs that normalise) */
    for (uint32_t i = 0; i < UBO_SIZE / 4; i++) ubo[i] = 1.0f;
    uint64_t vi[] = {ALDV(0, 0x80, 4, 0), ASTP(0, 0x70, 4), ins_mov32i(4, fbits(0.25f)), ins_mov32i(5, fbits(0.5f)), ins_mov32i(6, fbits(0.75f)), ins_mov32i(7, fbits(1)),
                     ASTP(4, 0x80, 4), ins_exit()};
    build_shader(h.mem + OFF_SH, g_ctl_vs, 0, vi, 8, 0);
    uint64_t fi[] = {ins_mov32i(0, fbits(1)), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, fbits(1)), ins_exit()};
    build_shader(h.mem + OFF_SH + 0x1000, g_ctl_fs, 1, fi, 5, 0xf);
    NVNshaderData vs = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), g_ctl_vs}, fs = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x1000), g_ctl_fs};
    DIR *d = opendir(dir);
    if (!d) {
        CHECK(0, "cannot open %s", dir);
        return h_finish("t_stages_dumped", &h);
    }
    static uint8_t ctl[16][0x2000];
    int nprog = 0, emitted = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nprog < 16) {
        size_t ln = strlen(e->d_name);
        if (strncmp(e->d_name, "s2-", 3) || ln < 5 || strcmp(e->d_name + ln - 4, ".ctl")) continue;
        char path[1024], name[32];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t cl = read_file(path, ctl[nprog], sizeof ctl[nprog]);
        snprintf(path, sizeof path, "%s/%.*s.code", dir, (int)(ln - 4), e->d_name);
        memset(h.mem + OFF_GS, 0, OFF_VB - OFF_GS);
        size_t n = read_file(path, h.mem + OFF_GS, OFF_VB - OFF_GS);
        snprintf(name, sizeof name, "%.16s", e->d_name + 3);
        if (cl < 0x760 || n < 0x80) {
            CHECK(0, "%s: control (%zu bytes) or code (%zu bytes) missing", name, cl, n);
            continue;
        }
        const uint32_t *sph = (const uint32_t *)(h.mem + OFF_GS + 0x30);
        printf("%s: SPH max vertices %u, output topology %u, invocations %u, code %zu bytes\n", name, sph[4] & 0xfff, (sph[3] >> 24) & 0xf, sph[2] >> 24, n);
        static const struct { int mode, nv; const char *name; } m[3] = {{NVN_DRAW_PRIMITIVE_POINTS, 1, "points"}, {NVN_DRAW_PRIMITIVE_LINES, 2, "lines"},
                                                                         {NVN_DRAW_PRIMITIVE_TRIANGLES, 3, "triangles"}};
        for (int k = 0; k < 3; k++) {
            int r = run(name, ctl[nprog], vs, fs, m[k].mode, m[k].name, m[k].nv);
            if (r > 0) emitted++;
        }
        nprog++;
    }
    closedir(d);
    CHECK(nprog > 0, "no geometry program (s2-*.ctl) in %s", dir);
    printf("%d geometry programs run, %d (program, primitive) runs emitted vertices\n", nprog, emitted);
    return h_finish("t_stages_dumped", &h);
}
