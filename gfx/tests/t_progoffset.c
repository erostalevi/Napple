/* t_progoffset.c - nvnProgramSetShaders with a code address that is a bare offset into a shader-code pool (Animal Crossing's window pass sets its program this way:
 * the base it adds the offset to is 0). The call must succeed (the game skips the program for good when it fails) and the program must work: the code is found at the
 * first draw, in the shader-code pool that has a shader header at the offsets of all stages. No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    /* a shader-code pool (CPU uncached, GPU cached, shader code) with the triangle's vertex and fragment shaders at 0x1000 and 0x3000 */
    size_t psz = 0x10000;
    uint8_t *cmem = mmap(NULL, psz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, h.dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, cmem, psz);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE);
    NVNmemoryPool *cp = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(cp, &pb), "code pool init");
    float tri[12] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    memcpy(h.mem, tri, sizeof tri);
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(cmem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(cmem + 0x3000, fctl, 1, fs_i, 5, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(prog, h.dev), "program init");
    NVNshaderData sd[2] = { { 0x1000, vctl }, { 0x3000, fctl } };      /* bare offsets, no pool base */
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "ProgramSetShaders with pool-offset code addresses must succeed");

    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    const NVNtexture *rts[1] = { t };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 64, 64); L(nvnCommandBufferSetScissor)(cb, 0, 0, 64, 64);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vstream; L(nvnVertexStreamStateSetDefaults)(&vstream); L(nvnVertexStreamStateSetStride)(&vstream, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vstream);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)h.mem, sizeof tri);
    float black[4] = { 0, 0, 0, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK);
    h_submit(&h);
    uint32_t c = ((const uint32_t *)(h.mem + H_READBACK))[32 * 64 + 32];
    CHECK(c == 0xff0000ffu, "the triangle drawn with the offset-addressed program is red: center %08x", c);
    return h_finish("t_progoffset", &h);
}
