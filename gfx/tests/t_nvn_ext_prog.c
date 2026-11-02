/* t_nvn_ext_prog.c - program objects smaller than nvn.h's 0xC0 (Animal Crossing's "program hash 0x3"): its NVNprogram is followed by an NVNbuffer at +0x80,
 * whose writes landed in the old 152-byte prog_t's hash[4] and reached BindProgram as a stage hash.
 *  1. Initialize / SetShaders / Finalize write nothing at or past +0x80 (NVN_MIN_NVNprogram) of the program object (a sentinel there survives);
 *  2. a guest write over a stage hash in the object after SetShaders (the fragment stage's, prog_t.hash[1]) does not change what BindProgram binds: green;
 *  3. a program object copied by value to another address still binds (stages 0..3 are mirrored in the object): green.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
#include "obj.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_RT = 0x100000, OFF_RB = H_READBACK };
static H h;

static uint32_t draw_with(NVNprogram *prog, NVNtexture *rt) {
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    const NVNtexture *rts[1] = {rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    L(nvnCommandBufferClearColor)(cb, 0, (const float[4]){0, 0, 1, 1}, NVN_CLEAR_COLOR_MASK_RGBA);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    return ((const uint32_t *)(h.mem + OFF_RB))[3 * W + 3];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(0, 0), ins_mov32i(1, 0x3f800000), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x2000, fctl, 1, fs_i, 5, 0xf);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), fctl}};
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, 0);

    /* 1: a program object of the smallest size followed by a neighbour */
    uint8_t *mem = calloc(1, 0x200);
    uint8_t *obj = mem + 0x40, *neighbour = obj + NVN_MIN_NVNprogram;
    memset(neighbour, 0x5a, 0x40);
    NVNprogram *prog = (NVNprogram *)obj;
    CHECK(L(nvnProgramInitialize)(prog, h.dev), "program init");
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "set shaders");
    int intact = 1;
    for (int i = 0; i < 0x40; i++) intact &= neighbour[i] == 0x5a;
    CHECK(intact, "Initialize / SetShaders write nothing at or past +%#x", NVN_MIN_NVNprogram);
    uint32_t px = draw_with(prog, rt);
    CHECK(px == 0xff00ff00u, "plain draw: green %08x", px);

    /* 2: the guest scribbles over the fragment stage's hash */
    const size_t fh = offsetof(prog_t, hash) + 8;
    uint64_t keep;
    memcpy(&keep, obj + fh, sizeof keep);
    *(uint64_t *)(obj + fh) = 3;
    px = draw_with(prog, rt);
    CHECK(px == 0xff00ff00u, "after a guest write over the fragment hash the program still draws green: %08x", px);
    memcpy(obj + fh, &keep, sizeof keep);

    /* 3: copied by value */
    uint8_t *copy = calloc(1, 0x100);
    memcpy(copy, obj, NVN_MIN_NVNprogram);
    px = draw_with((NVNprogram *)copy, rt);
    CHECK(px == 0xff00ff00u, "a program object copied by value binds its vertex / fragment stages: %08x", px);

    L(nvnProgramFinalize)(prog);
    intact = 1;
    for (int i = 0; i < 0x40; i++) intact &= neighbour[i] == 0x5a;
    CHECK(intact, "Finalize writes nothing at or past +%#x", NVN_MIN_NVNprogram);
    return h_finish("t_nvn_ext_prog", &h);
}
