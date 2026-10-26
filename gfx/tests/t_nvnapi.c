/* t_nvnapi.c - NVN entry points filled in on 2026-10-24 (docs/archive/changes/2026-10-24-nvn-pipeline.md), checked through the public API with pixel readback:
 * DrawTexture in both window origin modes, object getters, advanced (Porter-Duff) blend modes, multi-draw indirect with a GPU count, MSAA resolve copies. */
#include <signal.h>
#include "nvn_test.h"

static void on_fault(int sig, siginfo_t *si, void *uc) {
    if (napple_gfx_handle_fault(si->si_addr, 1, uc)) return;
    signal(sig, SIG_DFL);
}

#define W 16
static NVNdevice *dev;
static NVNqueue *q;
static NVNmemoryPool *pool;
static uint8_t *mem;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;
static NVNprogram *prog_solid;           /* fullscreen-ish geometry from the vertex buffer, constant colour from c3 (UBO 0) */
static uint64_t solid_vtx = 0x0;         /* vertex data offset in mem */
#define READBACK 0x10000

static NVNcommandBuffer *begin(NVNtexture *rt) {
    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    L(nvnCommandBufferInitialize)(cb, dev);
    L(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *rts[1] = { rt };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
    return cb;
}
static void finish(NVNcommandBuffer *cb, NVNtexture *rt) {
    NVNcopyRegion region = { 0, 0, 0, W, W, 1 };
    if (rt) L(nvnCommandBufferCopyTextureToBuffer)(cb, rt, NULL, &region, (NVNbufferAddress)(uintptr_t)(mem + READBACK), 0);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);
    L(nvnCommandBufferFinalize)(cb);
    free(cb);
}
static uint32_t px(int x, int y) { return ((const uint32_t *)(mem + READBACK))[y * W + x]; }
static NVNtexture *make_tex(int w, int h, NVNformat f, int64_t off, int samples) {
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, samples > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D);
    L(nvnTextureBuilderSetSize2D)(&tb, w, h); L(nvnTextureBuilderSetFormat)(&tb, f); L(nvnTextureBuilderSetStorage)(&tb, pool, off);
    if (samples > 1) L(nvnTextureBuilderSetSamples)(&tb, samples);
    NVNtexture *t = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t, &tb), "texture init %dx%d", w, h);
    return t;
}

/* ---- DrawTexture ---- */
static void test_draw_texture(void) {
    NVNtexture *rt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x20000, 1);
    NVNtexture *src = make_tex(2, 2, NVN_FORMAT_RGBA8, 0x40000, 1);
    uint32_t texels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };   /* (0,0) red (1,0) green (0,1) blue (1,1) white */
    NVNcopyRegion all = { 0, 0, 0, 2, 2, 1 };
    L(nvnTextureWriteTexels)(src, NULL, &all, texels);
    L(nvnTexturePoolRegisterTexture)(tp, 300, src, NULL);
    uint64_t handle = L(nvnDeviceGetTextureHandle)(dev, 300, 5);

    /* upper-left origin: window y = memory row */
    L(nvnDeviceSetWindowOriginMode)(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    NVNcommandBuffer *cb = begin(rt);
    float black[4] = { 0, 0, 0, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    NVNdrawTextureRegion dst = { 4, 4, 12, 12 }, sr = { 0, 0, 1, 1 };
    L(nvnCommandBufferDrawTexture)(cb, handle, &dst, &sr);
    finish(cb, rt);
    printf("DrawTexture (upper-left): %08x %08x / %08x %08x, outside %08x %08x\n", px(5, 5), px(10, 5), px(5, 10), px(10, 10), px(1, 1), px(14, 14));
    CHECK(px(5, 5) == 0xff0000ffu && px(10, 5) == 0xff00ff00u && px(5, 10) == 0xffff0000u && px(10, 10) == 0xffffffffu, "DrawTexture texels (upper-left)");
    CHECK(px(1, 1) == 0xff000000u && px(14, 14) == 0xff000000u && px(3, 8) == 0xff000000u && px(12, 8) == 0xff000000u, "DrawTexture drew outside its rectangle");

    /* lower-left origin: window y counts from the bottom, texture t = 0 is at the rectangle's bottom edge; half of the source region */
    L(nvnDeviceSetWindowOriginMode)(dev, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    cb = begin(rt);
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    NVNdrawTextureRegion dst2 = { 2, 2, 6, 6 }, sr2 = { 0.5f, 0, 1, 1 };          /* right column of the texture: green (t=0) and white (t=1) */
    L(nvnCommandBufferDrawTexture)(cb, handle, &dst2, &sr2);
    finish(cb, rt);
    if (getenv("T_DUMP")) for (int y = 0; y < W; y++) { for (int x = 0; x < W; x++) printf("%c", px(x, y) == 0xff000000u ? '.' : px(x, y) == 0xff00ff00u ? 'g' : px(x, y) == 0xffffffffu ? 'w' : '?'); printf("\n"); }
    /* lower-left rows are stored bottom-up (window y = memory row, the present flips them: core.c origin_flip), so window rows 2..5 are memory rows 2..5 */
    printf("DrawTexture (lower-left): row2 %08x row5 %08x, row1 %08x row6 %08x\n", px(3, 2), px(3, 5), px(3, 1), px(3, 6));
    CHECK(px(3, 2) == 0xff00ff00u && px(5, 3) == 0xff00ff00u, "DrawTexture lower-left: bottom half (t = 0) should be green, got %08x", px(3, 2));
    CHECK(px(3, 5) == 0xffffffffu && px(2, 4) == 0xffffffffu, "DrawTexture lower-left: top half should be white, got %08x", px(3, 5));
    CHECK(px(3, 1) == 0xff000000u && px(3, 6) == 0xff000000u && px(1, 3) == 0xff000000u && px(6, 3) == 0xff000000u, "DrawTexture lower-left: drew outside");
    L(nvnDeviceSetWindowOriginMode)(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
}

/* ---- MultiDraw*IndirectCount: three triangles in three quadrants, the count (2) is written by a GPU copy earlier in the same command buffer ---- */
static NVNprogram *colour_program(int slot, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {     /* constant colour (float bit patterns) */
    static uint8_t vctl[0x880], fctl[4][0x880];
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(0, r), ins_mov32i(1, g), ins_mov32i(2, b), ins_mov32i(3, a), ins_exit() };
    build_shader(mem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(mem + 0x3000 + (size_t)slot * 0x400, fctl[slot], 1, fs_i, 5, 0xf);
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(p, dev);
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(mem + 0x3000 + (size_t)slot * 0x400), fctl[slot] } };
    CHECK(L(nvnProgramSetShaders)(p, 2, sd), "colour program");
    return p;
}
static NVNprogram *solid_program(void) { return colour_program(0, 0x3f800000, 0x3f800000, 0, 0x3f800000); }   /* yellow */
static void bind_geometry(NVNcommandBuffer *cb, NVNprogram *p, uint64_t vtx) {
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(mem + vtx), 0x1000);
    L(nvnCommandBufferBindProgram)(cb, p, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
}
static void test_indirect_count(void) {
    NVNtexture *rt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x20000, 1);
    NVNprogram *p = solid_program();
    /* triangles covering the top-left, top-right and bottom-left quadrant centres (NDC +y is the top row) */
    float v[9 * 4] = { -1, 1, 0, 1, 0, 1, 0, 1, -1, 0, 0, 1,     0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1,     -1, 0, 0, 1, 0, 0, 0, 1, -1, -1, 0, 1 };
    memcpy(mem + 0x5000, v, sizeof v);
    uint32_t *args = (uint32_t *)(mem + 0x6000);          /* stride 32: { count, instances, first, base instance } + padding */
    for (int i = 0; i < 3; i++) { uint32_t *a = args + i * 8; a[0] = 3; a[1] = 1; a[2] = 3 * i; a[3] = 0; }
    uint32_t *count = (uint32_t *)(mem + 0x6100), *two = (uint32_t *)(mem + 0x6104);
    *count = 3; *two = 2;
    NVNcommandBuffer *cb = begin(rt);
    float black[4] = { 0, 0, 0, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    L(nvnCommandBufferCopyBufferToBuffer)(cb, (NVNbufferAddress)(uintptr_t)two, (NVNbufferAddress)(uintptr_t)count, 4, 0);
    bind_geometry(cb, p, 0x5000);
    L(nvnCommandBufferMultiDrawArraysIndirectCount)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, (NVNbufferAddress)(uintptr_t)args, (NVNbufferAddress)(uintptr_t)count, 3, 32);
    finish(cb, rt);
    printf("MultiDrawArraysIndirectCount: top-left %08x top-right %08x bottom-left %08x (count %u)\n", px(2, 2), px(13, 2), px(1, 10), *count);
    CHECK(*count == 2, "GPU copy of the count did not run");
    CHECK(px(2, 2) == 0xff00ffffu && px(13, 2) == 0xff00ffffu, "draws 0 and 1 should be drawn");
    CHECK(px(1, 10) == 0xff000000u, "draw 2 is beyond the GPU-written count and must not be drawn, got %08x", px(1, 10));

    /* indexed: the same three triangles through an index buffer, count 1, default stride */
    uint16_t idx[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    memcpy(mem + 0x6200, idx, sizeof idx);
    uint32_t *eargs = (uint32_t *)(mem + 0x6300);         /* { count, instances, first index, base vertex, base instance } */
    for (int i = 0; i < 3; i++) { uint32_t *a = eargs + i * 5; a[0] = 3; a[1] = 1; a[2] = 3 * (2 - i); a[3] = 0; a[4] = 0; }
    *count = getenv("T_COUNT") ? (uint32_t)atoi(getenv("T_COUNT")) : 1;
    cb = begin(rt);
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    bind_geometry(cb, p, 0x5000);
    L(nvnCommandBufferMultiDrawElementsIndirectCount)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, (NVNbufferAddress)(uintptr_t)(mem + 0x6200),
                                                      (NVNbufferAddress)(uintptr_t)eargs, (NVNbufferAddress)(uintptr_t)count, 3, 0);
    finish(cb, rt);
    if (getenv("T_DUMP")) for (int y = 0; y < W; y++) { for (int x = 0; x < W; x++) printf("%c", px(x, y) == 0xff000000u ? '.' : px(x, y) == 0xff00ffffu ? 'y' : '?'); printf("\n"); }
    printf("MultiDrawElementsIndirectCount: top-left %08x top-right %08x bottom-left %08x\n", px(2, 2), px(13, 2), px(1, 10));
    CHECK(px(1, 10) == 0xff00ffffu && px(2, 2) == 0xff000000u && px(13, 2) == 0xff000000u, "indexed: only draw 0 (the bottom-left triangle) should be drawn");
}

/* ---- advanced blend: premultiplied (0.5, 0.5, 0, 0.5) over premultiplied (0, 0, 0.5, 0.5) ---- */
static void test_advanced_blend(void) {
    NVNtexture *rt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x20000, 1);
    NVNprogram *p = colour_program(1, 0x3f000000, 0x3f000000, 0, 0x3f000000);
    float full[12] = { -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1 };
    memcpy(mem + 0x7000, full, sizeof full);
    static const struct { int mode, overlap; float e[4]; const char *name; } cases[] = {
        { NVN_BLEND_ADVANCED_MODE_SRC_OVER, 0, { 0.5f, 0.5f, 0.25f, 0.75f }, "SRC_OVER" },
        { NVN_BLEND_ADVANCED_MODE_DST_OVER, 0, { 0.25f, 0.25f, 0.5f, 0.75f }, "DST_OVER" },
        { NVN_BLEND_ADVANCED_MODE_SRC_IN, 0, { 0.25f, 0.25f, 0, 0.25f }, "SRC_IN" },
        { NVN_BLEND_ADVANCED_MODE_DST_OUT, 0, { 0, 0, 0.25f, 0.25f }, "DST_OUT" },
        { NVN_BLEND_ADVANCED_MODE_SRC_ATOP, 0, { 0.25f, 0.25f, 0.25f, 0.5f }, "SRC_ATOP" },
        { NVN_BLEND_ADVANCED_MODE_XOR, 0, { 0.25f, 0.25f, 0.25f, 0.5f }, "XOR" },
        { NVN_BLEND_ADVANCED_MODE_SCREEN, 0, { 0.5f, 0.5f, 0.5f, 0.75f }, "SCREEN" },
        { NVN_BLEND_ADVANCED_MODE_MINUS, 0, { 0, 0, 0.5f, 0.75f }, "MINUS" },
        { NVN_BLEND_ADVANCED_MODE_SRC, NVN_BLEND_ADVANCED_OVERLAP_CONJOINT, { 0.5f, 0.5f, 0, 0.5f }, "SRC (conjoint)" },
        { NVN_BLEND_ADVANCED_MODE_MULTIPLY, 0, { 0.5f, 0.5f, 0.25f, 0.75f }, "MULTIPLY (SRC_OVER fallback)" },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        NVNcommandBuffer *cb = begin(rt);
        float dst[4] = { 0, 0, 0.5f, 0.5f };
        L(nvnCommandBufferClearColor)(cb, 0, dst, 0xf);
        NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnColorStateSetBlendEnable)(&cs, 0, 1); L(nvnCommandBufferBindColorState)(cb, &cs);
        NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs);
        L(nvnBlendStateSetAdvancedMode)(&bs, cases[c].mode);
        L(nvnBlendStateSetAdvancedOverlap)(&bs, cases[c].overlap);
        CHECK((int)L(nvnBlendStateGetAdvancedMode)(&bs) == cases[c].mode && (int)L(nvnBlendStateGetAdvancedOverlap)(&bs) == cases[c].overlap, "advanced blend getters");
        CHECK(L(nvnBlendStateGetAdvancedPremultipliedSrc)(&bs) == 1 && L(nvnBlendStateGetAdvancedNormalizedDst)(&bs) == 1, "advanced blend defaults");
        L(nvnCommandBufferBindBlendState)(cb, &bs);
        bind_geometry(cb, p, 0x7000);
        L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        finish(cb, rt);
        uint32_t v = px(8, 8); int ok = 1;
        for (int k = 0; k < 4; k++) { int got = (int)((v >> (8 * k)) & 0xff), want = (int)(cases[c].e[k] * 255.0f + 0.5f); if (abs(got - want) > 2) ok = 0; }
        printf("advanced blend %-28s %08x\n", cases[c].name, v);
        CHECK(ok, "advanced blend %s: got %08x, expected %.2f %.2f %.2f %.2f", cases[c].name, v, cases[c].e[0], cases[c].e[1], cases[c].e[2], cases[c].e[3]);
    }
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs);
    L(nvnBlendStateSetAdvancedPremultipliedSrc)(&bs, 0); L(nvnBlendStateSetAdvancedNormalizedDst)(&bs, 0);
    CHECK(L(nvnBlendStateGetAdvancedPremultipliedSrc)(&bs) == 0 && L(nvnBlendStateGetAdvancedNormalizedDst)(&bs) == 0, "advanced blend flag getters");
}

/* ---- programmable sample locations + MSAA resolve; a second resolve after more drawing must still see the first pass's samples ---- */
static void ms_state(NVNcommandBuffer *cb, float sx) {
    NVNmultisampleState ms; L(nvnMultisampleStateSetDefaults)(&ms); L(nvnMultisampleStateSetSamples)(&ms, 4);
    if (sx >= 0) {
        float loc[8]; for (int i = 0; i < 4; i++) { loc[2 * i] = sx; loc[2 * i + 1] = 0.5f; }
        L(nvnMultisampleStateSetSampleLocationsEnable)(&ms, 1);
        L(nvnMultisampleStateSetSampleLocations)(&ms, 0, 4, loc);
    }
    L(nvnCommandBufferBindMultisampleState)(cb, &ms);
}
static void test_msaa(void) {
    NVNtexture *msrt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x20000, 4);
    NVNtexture *rt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x48000, 1);
    NVNprogram *yellow = solid_program(), *green = colour_program(2, 0, 0x3f800000, 0, 0x3f800000);
    /* left quad: x in [-1, -1/16] = pixels [0, 7.5); right quad: x in [0.25, 1] = pixels [10, 16) */
    float q[24] = { -1, -1, 0, 1, -0.0625f, -1, 0, 1, -1, 1, 0, 1,   -0.0625f, -1, 0, 1, -0.0625f, 1, 0, 1, -1, 1, 0, 1 };
    float r[24] = { 0.25f, -1, 0, 1, 1, -1, 0, 1, 0.25f, 1, 0, 1,   1, -1, 0, 1, 1, 1, 0, 1, 0.25f, 1, 0, 1 };
    memcpy(mem + 0x7100, q, sizeof q); memcpy(mem + 0x7200, r, sizeof r);
    NVNmultisampleState chk; L(nvnMultisampleStateSetDefaults)(&chk); L(nvnMultisampleStateSetSamples)(&chk, 4);
    int gw = 0, gh = 0; L(nvnMultisampleStateGetSampleLocationsGrid)(&chk, &gw, &gh);
    CHECK(gw == 2 && gh == 2, "4-sample location grid should be 2x2, got %dx%d", gw, gh);
    float black[4] = { 0, 0, 0, 1 };
    for (int pass = 0; pass < 2; pass++) {
        float sx = pass ? 0.75f : 0.25f;
        NVNcommandBuffer *cb = begin(msrt);
        L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
        ms_state(cb, sx);
        bind_geometry(cb, yellow, 0x7100);
        L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
        L(nvnCommandBufferDownsample)(cb, msrt, rt);
        finish(cb, rt);
        printf("sample locations x=%.2f: column 6 %08x column 7 %08x column 8 %08x\n", sx, px(6, 8), px(7, 8), px(8, 8));
        CHECK(px(6, 8) == 0xff00ffffu && px(8, 8) == 0xff000000u, "resolve: columns 6 / 8 should be covered / empty");
        if (!pass) CHECK(px(7, 8) == 0xff00ffffu, "samples at x=0.25 are left of the edge (7.5): column 7 should be covered, got %08x", px(7, 8));
        else CHECK(px(7, 8) == 0xff000000u, "samples at x=0.75 are right of the edge (7.5): column 7 should be empty, got %08x", px(7, 8));
    }
    /* a later pass draws only the right quad and resolves again: the left one must still be in the multisampled target */
    NVNcommandBuffer *cb = begin(msrt);
    ms_state(cb, -1);
    bind_geometry(cb, green, 0x7200);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
    L(nvnCommandBufferDownsample)(cb, msrt, rt);
    finish(cb, rt);
    printf("second resolve: left %08x right %08x\n", px(3, 8), px(13, 8));
    CHECK(px(3, 8) == 0xff00ffffu, "the first resolve dropped the multisampled contents (left quad lost: %08x)", px(3, 8));
    CHECK(px(13, 8) == 0xff00ff00u, "right quad missing after the second resolve (%08x)", px(13, 8));
}

/* ---- render target texture view with another format: 0.5 grey through an sRGB view of an RGBA8 texture is stored sRGB-encoded (0xbc) ---- */
static void test_rt_view(void) {
    NVNtexture *rt = make_tex(W, W, NVN_FORMAT_RGBA8, 0x20000, 1);
    NVNprogram *grey = colour_program(3, 0x3f000000, 0x3f000000, 0x3f000000, 0x3f800000);
    float full[12] = { -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1 };
    memcpy(mem + 0x7000, full, sizeof full);
    for (int srgb = 0; srgb < 2; srgb++) {
        NVNcommandBuffer *cb = begin(rt);
        NVNtextureView v; L(nvnTextureViewSetDefaults)(&v); L(nvnTextureViewSetFormat)(&v, NVN_FORMAT_RGBA8_SRGB);
        const NVNtexture *rts[1] = { rt }; const NVNtextureView *views[1] = { &v };
        L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, srgb ? views : NULL, NULL, NULL);
        bind_geometry(cb, grey, 0x7000);
        L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        finish(cb, rt);
        uint32_t c = px(8, 8) & 0xff;
        printf("render target %s: 0.5 stored as %#x\n", srgb ? "sRGB view" : "base format", c);
        if (srgb) CHECK(c >= 0xba && c <= 0xbe, "sRGB render target view: expected about 0xbc, got %#x", c);
        else CHECK(c >= 0x7f && c <= 0x81, "UNORM render target: expected 0x80, got %#x", c);
    }
}

/* ---- getters that returned constants ---- */
static void test_getters(void) {
    NVNtexture *t = make_tex(8, 8, NVN_FORMAT_RGBA8, 0x60000, 1);
    CHECK(L(nvnTextureGetMemoryPool)(t) == pool, "TextureGetMemoryPool");
    CHECK(L(nvnTextureGetMemoryOffset)(t) == 0x60000, "TextureGetMemoryOffset %lld", (long long)L(nvnTextureGetMemoryOffset)(t));
    int (*raw_class)(const NVNtexture *) = (int (*)(const NVNtexture *))napple_gfx_nvn_bootstrap_loader("nvnTextureGetRawStorageClass");
    CHECK(raw_class(t) == (int)L(nvnTextureGetStorageClass)(t), "raw storage class agrees with the storage class");
    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    L(nvnCommandBufferInitialize)(cb, dev);
    int tag;
    L(nvnCommandBufferSetMemoryCallbackData)(cb, &tag);
    CHECK(L(nvnCommandBufferGetMemoryCallbackData)(cb) == &tag, "CommandBufferGetMemoryCallbackData returned %p", L(nvnCommandBufferGetMemoryCallbackData)(cb));
    L(nvnCommandBufferFinalize)(cb);
}

int main(void) {
    struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = W; cfg.headless_height = W; cfg.settings.async_shader_compile = 0;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db); L(nvnDeviceInitialize)(dev, &db);
    L(nvnDeviceSetDepthMode)(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    size_t PS = 4 << 20;
    mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(pool, &pb), "pool");
    tp = calloc(1, sizeof(NVNtexturePool)); sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, pool, 0x80000, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, pool, 0x90000, 64), "sampler pool");
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    L(nvnSamplerBuilderSetWrapMode)(&smb, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, dev); L(nvnQueueBuilderSetDefaults)(&qb);
    q = calloc(1, sizeof(NVNqueue)); L(nvnQueueInitialize)(q, &qb);
    (void)prog_solid; (void)solid_vtx;

    test_draw_texture();
    test_indirect_count();
    test_advanced_blend();
    test_msaa();
    test_rt_view();
    test_getters();

    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls: %llu", (unsigned long long)st.unimplemented_calls_total);
    printf(g_fail ? "t_nvnapi: %d FAILED\n" : "t_nvnapi: ok\n", g_fail);
    napple_gfx_diag_dump();
    return g_fail != 0;
}
