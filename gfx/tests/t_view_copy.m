/* t_view_copy.m - copy-backed texture views (mtl_tex.m GfxAlias): sampled views Metal cannot make over the storage are a copy that mtl_tex_view keeps current.
 *  1. Depth32F viewed as R32F / R32UI stays the depth texture (the float's bits are the integer view's value); as RGBA8 it is a copy of the bytes (a float
 *     colour slot); Depth16 as R16UI is a copy bound like a stencil view (uint slot); Depth32F_Stencil8 as RGBA8 copies the depth plane; DEPTH24_STENCIL8
 *     has no packed bits to copy (sampled as depth, as before).
 *  2. BC1 viewed as RG32F (one texel per block) and RG32UI viewed as BC1 (four texels per block): the blocks, byte for byte; BC1 as RG32UI (a 2-channel
 *     integer texture the translator cannot declare) stays BC1.
 *  3. A 4x Depth32F texture viewed as 4x RGBA8: every sample's bytes, by a per-sample pass; as R32UI it is sampled as depth.
 *  4. 2D slices of a 3D texture (2D array view of slices 1..2, 2D view of slice 3) and a 3D view of a 2D array.
 *  5. Refresh: nothing is queued while the source is unchanged; a write (mtl_tex_written) queues the copy again; a view of another texel size falls back.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/napple_gfx.h"
#import "mtl_priv.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)
static const uint8_t SWZ[4] = {2, 3, 4, 5};
static uint8_t *g_mem;

static GfxTex *tex(int fmt, int target, uint32_t w, uint32_t h, uint32_t d, uint32_t samples, uint32_t off) {
    texsnap s;
    memset(&s, 0, sizeof s);
    s.d.width = w; s.d.height = h; s.d.depth = d; s.d.levels = 1; s.d.target = target; s.d.format = fmt; s.d.samples = (int)samples;
    s.mem = g_mem + off; s.size = 1 << 20; s.valid = 1;
    return mtl_tex_get(&s, 1);
}
static view_t vfmt(int fmt) { view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = (uint16_t)fmt; return v; }
static void clear_depth(GfxTex *t, float d) {
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = t->tex; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = d; rp.depthAttachment.storeAction = MTLStoreActionStore;
    if (t->has_stencil) { rp.stencilAttachment.texture = t->tex; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    mtl_tex_written(t);
}
static void flush(void) {
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    mtl_tex_alias_flush(cb);
    [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.error == nil, "copy command buffer: %s", cb.error.localizedDescription.UTF8String);
}
/* bytes of (level 0, slice) of a single-sampled texture, through a shared buffer */
static NSData *read_tex(id<MTLTexture> t, NSUInteger slice, NSUInteger row) {
    NSUInteger h = t.height;
    if (t.pixelFormat >= MTLPixelFormatBC1_RGBA && t.pixelFormat <= MTLPixelFormatBC7_RGBAUnorm_sRGB) h = (h + 3) / 4;
    id<MTLBuffer> b = [g_mtl.dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    int is3d = t.textureType == MTLTextureType3D;
    [be copyFromTexture:t sourceSlice:is3d ? 0 : slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, is3d ? slice : 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return [NSData dataWithBytes:b.contents length:row * h];
}
/* sample s of texel (x, y) of a multisampled colour texture, as 4 uints (integer formats) or 4 floats' bits */
static void read_ms(id<MTLTexture> t, uint32_t x, uint32_t y, uint32_t s, uint32_t out[4]) {
    static id<MTLComputePipelineState> pu, pf;
    if (!pu) {
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         "kernel void ku(texture2d_ms<uint> t [[texture(0)]], device uint4 *o [[buffer(0)]], constant uint3 &p [[buffer(1)]]) { o[0] = t.read(p.xy, p.z); }\n"
                         "kernel void kf(texture2d_ms<float> t [[texture(0)]], device float4 *o [[buffer(0)]], constant uint3 &p [[buffer(1)]]) { o[0] = t.read(p.xy, p.z); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:src options:nil error:&e];
        pu = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ku"] error:&e];
        pf = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kf"] error:&e];
    }
    id<MTLBuffer> b = [g_mtl.dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    uint32_t p[4] = {x, y, s, 0};
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    int isint = t.pixelFormat == MTLPixelFormatR32Uint || t.pixelFormat == MTLPixelFormatRGBA8Uint;
    [ce setComputePipelineState:isint ? pu : pf];
    [ce setTexture:t atIndex:0]; [ce setBuffer:b offset:0 atIndex:0]; [ce setBytes:p length:sizeof p atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(out, b.contents, 16);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 64; cfg.headless_height = 64;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    g_mem = calloc(1, 16 << 20);
    const float qf = 0.25f; uint32_t q; memcpy(&q, &qf, 4);

    /* 1. depth views */
    GfxTex *d32 = tex(0x34 /* DEPTH32F */, 1, 16, 16, 1, 1, 0);
    CHECK(d32 && d32->is_depth, "Depth32F texture");
    clear_depth(d32, 0.25f);
    view_t r32ui = vfmt(0x0B), r32f = vfmt(0x0A), rgba8 = vfmt(0x25), r16ui = vfmt(0x08);
    CHECK(mtl_tex_samples_as_depth(&d32->key, &r32ui, 1) && mtl_tex_samples_as_depth(&d32->key, &r32f, 1), "R32UI / R32F views of 32-bit depth sample as depth");
    id<MTLTexture> a = mtl_tex_view(d32, SWZ, &r32ui, 1, TEXTYPE_2D, 0, 0);
    CHECK(a == d32->tex && mtl_tex_alias_pending() == 0, "R32UI view of Depth32F is the depth texture (the float's bits are the view's value): format %lu", (unsigned long)a.pixelFormat);
    id<MTLTexture> f = mtl_tex_view(d32, SWZ, &r32f, 1, TEXTYPE_2D, 0, 0);
    CHECK(f.pixelFormat == MTLPixelFormatDepth32Float && mtl_tex_alias_pending() == 0, "R32F view of Depth32F is the depth texture (format %lu)", (unsigned long)f.pixelFormat);
    CHECK(!mtl_tex_samples_as_depth(&d32->key, &rgba8, 1) && !mtl_tex_samples_as_uint(&d32->key, &rgba8, 1), "RGBA8 view of depth: a float colour slot");
    a = mtl_tex_view(d32, SWZ, &rgba8, 1, TEXTYPE_2D, 0, 0);
    CHECK(a != d32->tex && a.pixelFormat == MTLPixelFormatRGBA8Unorm && a.width == 16, "RGBA8 copy of Depth32F: format %lu width %lu", (unsigned long)a.pixelFormat, (unsigned long)a.width);
    CHECK(mtl_tex_alias_pending() == 1, "copy queued: %u", mtl_tex_alias_pending());
    flush();
    NSData *px = read_tex(a, 0, 16 * 4);
    CHECK(((const uint32_t *)px.bytes)[0] == q && ((const uint32_t *)px.bytes)[255] == q, "depth bytes copied: %08x %08x (%08x)", ((const uint32_t *)px.bytes)[0], ((const uint32_t *)px.bytes)[255], q);
    GfxTex *d16 = tex(0x32 /* DEPTH16 */, 1, 16, 16, 1, 1, 0x90000);
    clear_depth(d16, 0.5f);
    CHECK(mtl_tex_samples_as_uint(&d16->key, &r16ui, 1) && !mtl_tex_samples_as_depth(&d16->key, &r16ui, 1), "R16UI view of Depth16: a uint slot (bound like a stencil view)");
    a = mtl_tex_view(d16, SWZ, &r16ui, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == MTLPixelFormatR16Uint, "R16UI copy of Depth16 (format %lu)", (unsigned long)a.pixelFormat);
    flush();
    px = read_tex(a, 0, 16 * 2);
    uint16_t c16 = ((const uint16_t *)px.bytes)[40];
    CHECK(c16 == 0x8000 || c16 == 0x7fff, "Depth16 code of 0.5 copied: %04x", c16);
    /* 5. refresh */
    a = mtl_tex_view(d16, SWZ, &r16ui, 1, TEXTYPE_2D, 0, 0);
    CHECK(mtl_tex_alias_pending() == 0, "unchanged source: nothing queued (%u)", mtl_tex_alias_pending());
    clear_depth(d16, 1.0f);
    a = mtl_tex_view(d16, SWZ, &r16ui, 1, TEXTYPE_2D, 0, 0);
    CHECK(mtl_tex_alias_pending() == 1, "written source: copy queued again (%u)", mtl_tex_alias_pending());
    flush();
    px = read_tex(a, 0, 16 * 2);
    CHECK(((const uint16_t *)px.bytes)[17] == 0xffff, "refreshed copy holds 1.0: %04x", ((const uint16_t *)px.bytes)[17]);
    GfxTex *d32s8 = tex(0x36 /* DEPTH32F_STENCIL8 */, 1, 16, 16, 1, 1, 0x10000);
    clear_depth(d32s8, 0.25f);
    a = mtl_tex_view(d32s8, SWZ, &rgba8, 1, TEXTYPE_2D, 0, 0);
    flush();
    px = read_tex(a, 0, 16 * 4);
    CHECK(a.pixelFormat == MTLPixelFormatRGBA8Unorm && ((const uint32_t *)px.bytes)[5] == q, "Depth32F_Stencil8 as RGBA8: depth plane bytes %08x", ((const uint32_t *)px.bytes)[5]);
    GfxTex *d24s8 = tex(0x35 /* DEPTH24_STENCIL8 */, 1, 16, 16, 1, 1, 0x20000);
    a = mtl_tex_view(d24s8, SWZ, &rgba8, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == d24s8->tex.pixelFormat && mtl_tex_alias_pending() == 0 && mtl_tex_samples_as_depth(&d24s8->key, &rgba8, 1),
          "DEPTH24_STENCIL8 as RGBA8: no packed bits here, sampled as depth (format %lu)", (unsigned long)a.pixelFormat);
    CHECK(mtl_tex_samples_as_depth(&d24s8->key, &r32ui, 1) && mtl_tex_view(d24s8, SWZ, &r32ui, 1, TEXTYPE_2D, 0, 0) == d24s8->tex, "DEPTH24_STENCIL8 as R32UI: sampled as depth");

    /* 2. BC1 <-> 8-byte colour */
    GfxTex *bc = tex(0x42 /* RGBA_DXT1 */, 1, 16, 16, 1, 1, 0x30000);
    uint8_t blocks[4 * 4 * 8];
    for (unsigned i = 0; i < sizeof blocks; i++) blocks[i] = (uint8_t)(i * 7 + 3);
    [bc->tex replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:blocks bytesPerRow:4 * 8];
    mtl_tex_written(bc);
    view_t rg32f = vfmt(0x16), rg32ui = vfmt(0x17);
    a = mtl_tex_view(bc, SWZ, &rg32ui, 1, TEXTYPE_2D, 0, 0);
    CHECK(a == bc->tex && mtl_tex_alias_pending() == 0, "BC1 as RG32UI: the translator cannot sample a 2-channel integer texture, kept as BC1");
    a = mtl_tex_view(bc, SWZ, &rg32f, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == MTLPixelFormatRG32Float && a.width == 4 && a.height == 4, "BC1 as RG32F: one texel per block (%lux%lu fmt %lu)", (unsigned long)a.width, (unsigned long)a.height, (unsigned long)a.pixelFormat);
    flush();
    px = read_tex(a, 0, 4 * 8);
    CHECK(!memcmp(px.bytes, blocks, sizeof blocks), "BC1 blocks copied byte for byte");
    GfxTex *rg = tex(0x17, 1, 4, 4, 1, 1, 0x40000);
    [rg->tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:blocks bytesPerRow:4 * 8];
    mtl_tex_written(rg);
    view_t bc1 = vfmt(0x42);
    a = mtl_tex_view(rg, SWZ, &bc1, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == MTLPixelFormatBC1_RGBA && a.width == 16, "RG32UI as BC1: four texels per block (width %lu fmt %lu)", (unsigned long)a.width, (unsigned long)a.pixelFormat);
    flush();
    px = read_tex(a, 0, 4 * 8);
    CHECK(!memcmp(px.bytes, blocks, sizeof blocks), "blocks land in the BC1 copy");

    /* 3. multisampled depth as multisampled colour */
    GfxTex *msd = tex(0x34, 5 /* 2D_MULTISAMPLE */, 8, 8, 1, 4, 0x50000);
    CHECK(msd && msd->tex.sampleCount == 4, "4x Depth32F");
    clear_depth(msd, 0.25f);
    CHECK(mtl_tex_samples_as_depth(&msd->key, &r32ui, 1) && mtl_tex_view(msd, SWZ, &r32ui, 1, TEXTYPE_2D_MS, 0, 0) == msd->tex, "4x depth as R32UI: sampled as depth");
    uint32_t o[4];
    a = mtl_tex_view(msd, SWZ, &rgba8, 1, TEXTYPE_2D_MS, 0, 0);
    flush();
    read_ms(a, 1, 1, 3, o);
    float fo[4]; memcpy(fo, o, 16);
    CHECK(a.pixelFormat == MTLPixelFormatRGBA8Unorm && fo[0] < 0.01f && fo[1] < 0.01f && fo[2] > 0.49f && fo[2] < 0.51f && fo[3] > 0.24f && fo[3] < 0.25f,
          "4x depth as RGBA8: the bytes of 0.25f (00 00 80 3e) -> %g %g %g %g", fo[0], fo[1], fo[2], fo[3]);
    a = mtl_tex_view(msd, SWZ, &rgba8, 1, TEXTYPE_2D, 0, 0);
    CHECK(mtl_tex_alias_pending() == 0, "4x depth sampled as a single-sampled 2D texture: no per-sample copy");

    /* 4. 3D <-> 2D slices */
    GfxTex *t3 = tex(0x25 /* RGBA8 */, 2 /* 3D */, 4, 4, 4, 1, 0x60000);
    CHECK(t3 && t3->tex.textureType == MTLTextureType3D, "3D texture");
    uint32_t vox[4 * 4 * 4];
    for (int z = 0; z < 4; z++) for (int i = 0; i < 16; i++) vox[z * 16 + i] = 0x01010101u * (uint32_t)(z * 10 + 1);
    [t3->tex replaceRegion:MTLRegionMake3D(0, 0, 0, 4, 4, 4) mipmapLevel:0 slice:0 withBytes:vox bytesPerRow:16 bytesPerImage:64];
    mtl_tex_written(t3);
    view_t lay; memset(&lay, 0, sizeof lay); lay.set_layers = 1; lay.min_layer = 1; lay.nlayers = 2;
    a = mtl_tex_view(t3, SWZ, &lay, 1, TEXTYPE_2D_ARRAY, 0, 0);
    CHECK(a.textureType == MTLTextureType2DArray && a.arrayLength == 2, "3D slices 1..2 as a 2D array (type %lu, %lu layers)", (unsigned long)a.textureType, (unsigned long)a.arrayLength);
    lay.min_layer = 3; lay.nlayers = 1;
    id<MTLTexture> a2 = mtl_tex_view(t3, SWZ, &lay, 1, TEXTYPE_2D, 0, 0);
    CHECK(a2.textureType == MTLTextureType2D, "3D slice 3 as 2D (type %lu)", (unsigned long)a2.textureType);
    flush();
    px = read_tex(a, 1, 16);
    NSData *px2 = read_tex(a2, 0, 16);
    CHECK(((const uint32_t *)px.bytes)[3] == 0x15151515u && ((const uint32_t *)px2.bytes)[0] == 0x1f1f1f1fu, "slice contents: layer 1 of the array %08x (z 2), 2D %08x (z 3)",
          ((const uint32_t *)px.bytes)[3], ((const uint32_t *)px2.bytes)[0]);
    GfxTex *arr = tex(0x25, 4 /* 2D array */, 4, 4, 3, 1, 0x70000);
    for (int z = 0; z < 3; z++) {
        uint32_t c[16];
        for (int i = 0; i < 16; i++) c[i] = 0x02020202u * (uint32_t)(z + 1);
        [arr->tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 slice:(NSUInteger)z withBytes:c bytesPerRow:16 bytesPerImage:64];
    }
    mtl_tex_written(arr);
    a = mtl_tex_view(arr, SWZ, &(view_t){0}, 0, TEXTYPE_3D, 0, 0);
    CHECK(a.textureType == MTLTextureType3D && a.depth == 3, "2D array as 3D (type %lu depth %lu)", (unsigned long)a.textureType, (unsigned long)a.depth);
    flush();
    px = read_tex(a, 2, 16);
    CHECK(((const uint32_t *)px.bytes)[7] == 0x06060606u, "3D copy depth slice 2 = layer 2: %08x", ((const uint32_t *)px.bytes)[7]);

    /* 5. another texel size: the texture's own format, nothing queued */
    GfxTex *c8 = tex(0x25, 1, 8, 8, 1, 1, 0x80000);
    a = mtl_tex_view(c8, SWZ, &rg32f, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == MTLPixelFormatRGBA8Unorm && mtl_tex_alias_pending() == 0, "RG32F view of RGBA8 (8 vs 4 bytes): texture format %lu", (unsigned long)a.pixelFormat);
    view_t r32fv = vfmt(0x0A);
    a = mtl_tex_view(c8, SWZ, &r32fv, 1, TEXTYPE_2D, 0, 0);
    CHECK(a.pixelFormat == MTLPixelFormatR32Float && a.parentTexture == c8->tex, "R32F view of RGBA8 stays a Metal view (format %lu)", (unsigned long)a.pixelFormat);

    napple_gfx_shutdown();
    printf(g_fail ? "t_view_copy: %d FAILED\n" : "t_view_copy: ok\n", g_fail);
    return g_fail != 0;
}
