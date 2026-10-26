/* mtl_tex.m - Metal textures from guest texture descriptors: creation, upload (block-linear -> Metal), views, samplers, vertex formats. */
#include <TargetConditionals.h>
#include <pthread.h>

#import "mtl_priv.h"
#import "mtl_display.h"
#include "tex/napple_bcdec.h"
#include "tex/napple_deswizzle.h"

@implementation GfxTex
@end

/* ---------------------------------------------------------------------------------------------------------------
 * Texture cache keyed by the hardware-visible identity of the storage
 * --------------------------------------------------------------------------------------------------------------- */
#define TT_BITS 14
#define TT_SIZE (1u << TT_BITS)
static __unsafe_unretained GfxTex *g_tt[TT_SIZE]; /* open addressing; entries stay alive through g_all */
static NSMutableSet<GfxTex *> *g_all;
static uint64_t g_tt_hash[TT_SIZE];
static pthread_mutex_t g_tt_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t tex_hash(const texsnap *s) {
    const napple_texdesc *d = &s->d;
    uint64_t h = fnv1a(&s->mem, sizeof s->mem, FNV_INIT);
    uint32_t v[12] = {(uint32_t)d->format, (uint32_t)d->target,      d->width, d->height, d->depth, d->levels, (uint32_t)d->samples, (uint32_t)d->flags & ~0x2u,
                      d->stride,           (uint32_t)d->pkg_block_h, 0,        0};
    h = fnv1a(v, sizeof v, h);
    return h ? h : 1;
}
static int same_identity(const texsnap *a, const texsnap *b) {
    return a->mem == b->mem && a->d.format == b->d.format && a->d.target == b->d.target && a->d.width == b->d.width && a->d.height == b->d.height &&
           a->d.depth == b->d.depth && a->d.levels == b->d.levels && a->d.samples == b->d.samples && a->d.stride == b->d.stride &&
           (a->d.flags & ~0x2) == (b->d.flags & ~0x2) && a->d.pkg_block_h == b->d.pkg_block_h;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Formats
 * --------------------------------------------------------------------------------------------------------------- */
/* NVN formats with three components have no Metal pixel format: they are expanded to four on upload. */
static MTLPixelFormat expand_format(int f) {
    switch (f) {
        case 0x19: return MTLPixelFormatRGBA8Unorm;
        case 0x1A: return MTLPixelFormatRGBA8Snorm;
        case 0x1B: return MTLPixelFormatRGBA8Uint;
        case 0x1C: return MTLPixelFormatRGBA8Sint;
        case 0x1D: return MTLPixelFormatRGBA16Float;
        case 0x1E: return MTLPixelFormatRGBA16Unorm;
        case 0x1F: return MTLPixelFormatRGBA16Snorm;
        case 0x20: return MTLPixelFormatRGBA16Uint;
        case 0x21: return MTLPixelFormatRGBA16Sint;
        case 0x22: return MTLPixelFormatRGBA32Float;
        case 0x23: return MTLPixelFormatRGBA32Uint;
        case 0x24: return MTLPixelFormatRGBA32Sint;
        default: return MTLPixelFormatInvalid;
    }
}

int mtl_tex_format_info(const texsnap *s, MTLPixelFormat *pf, int *is_depth, int *has_stencil) {
    uint32_t m = 0;
    int kind = napple_fmt_to_mtl(s->d.format, &m, NULL);
    MTLPixelFormat f = (MTLPixelFormat)m;
    int expand = 0;
    if (!kind) {
        f = expand_format(s->d.format);
        expand = f != MTLPixelFormatInvalid;
    }
    uint32_t bcpf;
    int bpp;
    if (!g_mtl.bc_ok && napple_bc_unpacked((uint32_t)s->d.format, &bcpf, &bpp)) f = (MTLPixelFormat)bcpf;
    *pf = f;
    *is_depth = f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8;
    *has_stencil = f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8;
    return expand ? 2 : (kind ? 1 : 0);
}

static MTLTextureType type_for(const napple_texdesc *d, uint32_t *layers_out) {
    uint32_t layers = 1;
    MTLTextureType t;
    switch (d->target) {
        case 0: t = MTLTextureType1D; break;
        case 1:
        case 7: t = d->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; break;
        case 2: t = MTLTextureType3D; break;
        case 3:
            t = MTLTextureType1DArray;
            layers = d->height ? d->height : 1;
            break;
        case 4:
            t = MTLTextureType2DArray;
            layers = d->depth ? d->depth : 1;
            break;
        case 5: t = MTLTextureType2DMultisample; break;
        case 6:
            t = MTLTextureType2DMultisampleArray;
            layers = d->depth ? d->depth : 1;
            break;
        case 8:
            t = MTLTextureTypeCube;
            layers = 6;
            break;
        case 9:
            t = MTLTextureTypeCubeArray;
            layers = d->depth ? d->depth : 6;
            break; /* depth counts layer-faces (see napple_layout.c) */
        default: t = MTLTextureType2D; break;
    }
    *layers_out = layers;
    return t;
}

static GfxTex *tex_create(const texsnap *s, int for_gpu_write) {
    GfxTex *t = [GfxTex new];
    t->key = *s;
    int kind = mtl_tex_format_info(s, &t->pf, &t->is_depth, &t->has_stencil);
    t->expand_rgb = kind == 2;
    const napple_texdesc *d = &s->d;
    uint32_t layers;
    MTLTextureType type = type_for(d, &layers);
    if (t->pf == MTLPixelFormatInvalid || d->target == 10) {
        LOG_ONCE(NAPPLE_LOG_WARN, "texture format %#x / target %d has no Metal mapping yet: using a placeholder", d->format, d->target);
        t->pf = MTLPixelFormatRGBA8Unorm;
        t->is_depth = 0;
        t->has_stencil = 0;
        type = MTLTextureType2D;
        layers = 1;
    }
    t->w = d->width ? d->width : 1;
    t->h = d->height ? d->height : 1;
    t->depth = d->depth ? d->depth : 1;
    t->levels = d->levels ? d->levels : 1;
    t->layers = layers;
    t->samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = t->pf;
    mtl_scale_pick pk;
    mtl_scale_pick_size(s, t->is_depth, for_gpu_write, &pk); /* screen-sized render targets are allocated larger (mtl_scale.m) */
    t->rsx = pk.rx;
    t->rsy = pk.ry;
    t->scale_gen = mtl_scale_generation();
    td.width = pk.w;
    td.height = (type == MTLTextureType1D || type == MTLTextureType1DArray) ? 1 : pk.h;
    td.depth = type == MTLTextureType3D ? t->depth : 1;
    td.mipmapLevelCount = t->samples > 1 ? 1 : t->levels;
    td.arrayLength = (type == MTLTextureTypeCubeArray)                                                                              ? layers / 6
                     : (type == MTLTextureType2DArray || type == MTLTextureType1DArray || type == MTLTextureType2DMultisampleArray) ? layers
                                                                                                                                    : 1;
    td.sampleCount = t->samples;
    td.storageMode = MTLStorageModeShared;
#if TARGET_OS_IOS
    if (t->is_depth) td.storageMode = MTLStorageModePrivate; /* iOS and the iOS Simulator reject depth/stencil textures with Shared storage */
#endif
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.usage |= MTLTextureUsageRenderTarget;
    if (d->flags & 0x4) td.usage |= MTLTextureUsageShaderWrite; /* NVN_TEXTURE_FLAGS_IMAGE */
    if (t->samples == 1 && !t->is_depth) td.usage |= MTLTextureUsageShaderWrite;
    t->tex = [g_mtl.dev newTextureWithDescriptor:td];
    if (!t->tex) {
        LOGE("texture creation failed (%ux%ux%u fmt %#x)", t->w, t->h, t->depth, d->format);
        return nil;
    }
    t->views = [NSMutableDictionary dictionary];
    mtl_scale_count(pk.rx != 1 || pk.ry != 1, pk.rx != 1 || pk.ry != 1 ? (uint64_t)pk.w * pk.h * 4 * t->samples : 0);
    return t;
}

/* The scale factors changed (window resize, settings) or a texture turned out to be a render target: re-decide its allocated size. The old Metal texture stays alive
 * for the command buffers that still use it; a texture the CPU filled is left alone (its contents live in guest memory only). g_tt_mu held. */
static void mtl_tex_rescale(GfxTex *t, int for_gpu_write) {
    t->scale_gen = mtl_scale_generation();
    if (t->stamp && t->rsx == 1 && t->rsy == 1) return; /* uploaded from guest memory: never scaled */
    mtl_scale_pick pk;
    mtl_scale_pick_size(&t->key, t->is_depth, for_gpu_write || t->gpu_owned, &pk);
    if (pk.rx == t->rsx && pk.ry == t->rsy) return;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = t->tex.textureType;
    td.pixelFormat = t->tex.pixelFormat;
    td.width = pk.w;
    td.height = pk.h;
    td.depth = 1;
    td.mipmapLevelCount = 1;
    td.arrayLength = t->tex.arrayLength;
    td.sampleCount = t->tex.sampleCount;
    td.storageMode = MTLStorageModeShared;
    td.usage = t->tex.usage;
    id<MTLTexture> n = [g_mtl.dev newTextureWithDescriptor:td];
    if (!n) {
        LOGE("render target rebuild failed (%ux%u)", pk.w, pk.h);
        return;
    }
    t->tex = n;
    t->rsx = pk.rx;
    t->rsy = pk.ry;
    t->views = [NSMutableDictionary dictionary];
    mtl_scale_count(pk.rx != 1 || pk.ry != 1, pk.rx != 1 || pk.ry != 1 ? (uint64_t)pk.w * pk.h * 4 * t->samples : 0);
}

/* A render pass whose attachments disagree (some scaled, some not) would render only their overlap (the top-left corner) and leave the rest garbage: SMO's
 * G-buffer pass has a colour target that was once filled from guest memory (never scaled by mtl_tex_rescale) next to scaled siblings. The target is a render
 * target now, so it is re-allocated at the siblings' factor; what it held is lost (the pass renders it, a load would read zeros). */
void mtl_tex_force_scale(GfxTex *t, float rx, float ry) {
    if (!t || (t->rsx == rx && t->rsy == ry) || (t->tex.textureType != MTLTextureType2D && t->tex.textureType != MTLTextureType2DArray) ||
        t->tex.mipmapLevelCount > 1)
        return;
    uint32_t nw = (uint32_t)lroundf((float)t->w * rx), nh = (uint32_t)lroundf((float)t->h * ry);
    if (nw < 1 || nh < 1 || nw > 16384 || nh > 16384) return;
    pthread_mutex_lock(&g_tt_mu);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = t->tex.textureType;
    td.pixelFormat = t->tex.pixelFormat;
    td.width = nw;
    td.height = nh;
    td.depth = 1;
    td.mipmapLevelCount = 1;
    td.arrayLength = t->tex.arrayLength;
    td.sampleCount = t->tex.sampleCount;
    td.storageMode = MTLStorageModeShared;
    td.usage = t->tex.usage;
    id<MTLTexture> n = [g_mtl.dev newTextureWithDescriptor:td];
    if (n) {
        t->tex = n;
        t->rsx = (float)nw / (float)t->w;
        t->rsy = (float)nh / (float)t->h;
        t->views = [NSMutableDictionary dictionary];
        t->gpu_owned = 1;
    }
    pthread_mutex_unlock(&g_tt_mu);
}

GfxTex *mtl_tex_get(const texsnap *s, int for_gpu_write) {
    if (!s || !s->valid) return nil;
    uint64_t h = tex_hash(s);
    GfxTex *t = nil;
    pthread_mutex_lock(&g_tt_mu);
    uint32_t i = (uint32_t)h & (TT_SIZE - 1);
    for (uint32_t n = 0; n < TT_SIZE && g_tt[i]; n++, i = (i + 1) & (TT_SIZE - 1))
        if (g_tt_hash[i] == h && same_identity(&g_tt[i]->key, s)) {
            t = g_tt[i];
            break;
        }
    if (!t) {
        t = tex_create(s, for_gpu_write);
        if (t) {
            if (!g_all) g_all = [NSMutableSet set];
            [g_all addObject:t];
            uint32_t j = (uint32_t)h & (TT_SIZE - 1);
            for (uint32_t n = 0; n < TT_SIZE && g_tt[j]; n++, j = (j + 1) & (TT_SIZE - 1)) {}
            g_tt[j] = t;
            g_tt_hash[j] = h;
        }
    }
    if (t && (t->scale_gen != mtl_scale_generation() || (for_gpu_write && !t->gpu_owned && t->rsx == 1 && t->rsy == 1))) mtl_tex_rescale(t, for_gpu_write);
    pthread_mutex_unlock(&g_tt_mu);
    if (t) {
        t->last_use = atomic_load(&g_gfx.frames);
        if (for_gpu_write) t->gpu_owned = 1;
    }
    return t;
}

void mtl_tex_forget_range(const void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_tt_mu);
    NSMutableArray *dead = [NSMutableArray array];
    for (GfxTex *t in g_all) {
        uintptr_t m = (uintptr_t)t->key.mem;
        if (m >= lo && m < hi) [dead addObject:t];
    }
    if (dead.count) {
        for (GfxTex *t in dead) [g_all removeObject:t];
        /* rebuild the table without the dead entries */
        memset(g_tt, 0, sizeof g_tt);
        for (GfxTex *t in g_all) {
            uint64_t h = tex_hash(&t->key);
            uint32_t j = (uint32_t)h & (TT_SIZE - 1);
            while (g_tt[j]) j = (j + 1) & (TT_SIZE - 1);
            g_tt[j] = t;
            g_tt_hash[j] = h;
        }
        gfx_retire((void (*)(void *))CFRelease, (void *)CFBridgingRetain(dead)); /* keep Metal objects alive until the GPU is done */
    }
    pthread_mutex_unlock(&g_tt_mu);
}

void mtl_tex_gc(uint64_t frame) {
    if (frame & 255) return;
    pthread_mutex_lock(&g_tt_mu);
    NSMutableArray *dead = nil;
    for (GfxTex *t in g_all) {
        uint64_t idle = frame - t->last_use;
        if (idle > (t->gpu_owned ? 3600u : 900u)) {
            if (!dead) dead = [NSMutableArray array];
            [dead addObject:t];
        }
    }
    if (dead) {
        for (GfxTex *t in dead) [g_all removeObject:t];
        memset(g_tt, 0, sizeof g_tt);
        for (GfxTex *t in g_all) {
            uint64_t h = tex_hash(&t->key);
            uint32_t j = (uint32_t)h & (TT_SIZE - 1);
            while (g_tt[j]) j = (j + 1) & (TT_SIZE - 1);
            g_tt[j] = t;
            g_tt_hash[j] = h;
        }
        LOGD("texture gc released %lu textures", (unsigned long)dead.count);
        gfx_retire((void (*)(void *))CFRelease, (void *)CFBridgingRetain(dead));
    }
    pthread_mutex_unlock(&g_tt_mu);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Upload
 * --------------------------------------------------------------------------------------------------------------- */
static void expand3_to_4(const uint8_t *src, uint8_t *dst, uint32_t npix, int comp_bytes, int one_bits) {
    for (uint32_t i = 0; i < npix; i++) {
        memcpy(dst + (size_t)i * comp_bytes * 4, src + (size_t)i * comp_bytes * 3, (size_t)comp_bytes * 3);
        memcpy(dst + (size_t)i * comp_bytes * 4 + (size_t)comp_bytes * 3, &one_bits, (size_t)comp_bytes);
    }
}

static void upload_texture(GfxTex *t) {
    const napple_texdesc *d = &t->key.d;
    if (t->samples > 1 || t->is_depth || d->target == 10) return;
    napple_texlayout L;
    napple_tex_layout(d, &L);
    int bw, bh;
    int bpb = napple_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return;
    uint32_t bcpf;
    int bcpp;
    int bc_decode = !g_mtl.bc_ok && napple_bc_unpacked((uint32_t)d->format, &bcpf, &bcpp);
    int layered = d->target == 3 || d->target == 4 || d->target == 8 || d->target == 9;
    uint32_t nlay = layered ? L.layers : 1;
    int linear = napple_tex_is_linear(d);
    for (uint32_t lv = 0; lv < t->levels; lv++) {
        for (uint32_t ly = 0; ly < nlay; ly++) {
            uint32_t row_bytes, rows, slices, bpb2;
            uint64_t base;
            unsigned th, td;
            uint32_t w = t->w >> lv ? t->w >> lv : 1, h = (d->target == 0 || d->target == 3) ? 1 : (t->h >> lv ? t->h >> lv : 1);
            if (linear) {
                row_bytes = (uint32_t)(((w + bw - 1) / bw) * bpb);
                rows = (h + bh - 1) / bh;
                slices = 1;
            } else if (napple_deswizzle_geom(d, lv, ly, &row_bytes, &rows, &slices, &base, &th, &td, &bpb2)) continue;
            size_t tmp_size = (size_t)row_bytes * rows * slices;
            uint8_t *tmp = malloc(tmp_size);
            if (!tmp) continue;
            if (napple_deswizzle(d, lv, ly, t->key.mem, tmp, row_bytes)) {
                free(tmp);
                continue;
            }
            MTLRegion region = MTLRegionMake3D(0, 0, 0, w, h, d->target == 2 ? slices : 1);
            NSUInteger slice = layered ? ly : 0;
            if (bc_decode) {
                uint8_t *px = malloc((size_t)w * h * bcpp);
                if (px && napple_bc_decode((uint32_t)d->format, tmp, row_bytes, w, h, px, (size_t)w * bcpp) == 0)
                    [t->tex replaceRegion:region mipmapLevel:lv slice:slice withBytes:px bytesPerRow:(size_t)w * bcpp bytesPerImage:(size_t)w * h * bcpp];
                free(px);
            } else if (t->expand_rgb) {
                int comp = bpb / 3;
                uint32_t npix = w * h * (d->target == 2 ? slices : 1);
                uint8_t *px = malloc((size_t)npix * comp * 4);
                int one = (comp == 4 && d->format == 0x22) ? 0x3f800000 : (comp == 2 && d->format == 0x1D) ? 0x3c00 : 1;
                if (px) {
                    expand3_to_4(tmp, px, npix, comp, one);
                    [t->tex replaceRegion:region
                              mipmapLevel:lv
                                    slice:slice
                                withBytes:px
                              bytesPerRow:(size_t)w * comp * 4
                            bytesPerImage:(size_t)w * h * comp * 4];
                    free(px);
                }
            } else {
                [t->tex replaceRegion:region mipmapLevel:lv slice:slice withBytes:tmp bytesPerRow:row_bytes bytesPerImage:(size_t)row_bytes * rows];
            }
            free(tmp);
        }
    }
}

/* Debug aid (NAPPLE_DRAWLOG_TEX=<dir>): write level 0 / layer `layer` of a CPU-written texture as a PGM (BC4, R8) or PPM (RGBA8, other BC) from the guest storage. */
void mtl_tex_debug_dump(GfxTex *t, const char *dir, unsigned layer) {
    const napple_texdesc *d = &t->key.d;
    napple_texlayout L;
    napple_tex_layout(d, &L);
    int bw, bh;
    int bpb = napple_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || t->samples > 1 || t->is_depth || !t->key.mem) return;
    uint32_t row_bytes, rows, slices, bpb2;
    uint64_t base;
    unsigned th, td;
    if (napple_deswizzle_geom(d, 0, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb2)) return;
    uint8_t *tmp = malloc((size_t)row_bytes * rows * slices);
    if (!tmp || napple_deswizzle(d, 0, layer, t->key.mem, tmp, row_bytes)) {
        free(tmp);
        return;
    }
    uint32_t w = t->w, h = t->h;
    uint8_t *px = NULL;
    int ch = 4;
    size_t pitch = 0;
    uint32_t bcpf;
    int bcpp;
    if (napple_bc_unpacked((uint32_t)d->format, &bcpf, &bcpp)) {
        px = malloc((size_t)w * h * bcpp);
        ch = bcpp;
        pitch = (size_t)w * bcpp;
        if (!px || napple_bc_decode((uint32_t)d->format, tmp, row_bytes, w, h, px, pitch)) {
            free(px);
            free(tmp);
            return;
        }
    } else if (d->format == 0x25 || d->format == 0x26) {
        px = tmp;
        tmp = NULL;
        pitch = row_bytes;
        ch = 4;
    } else {
        free(tmp);
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/tex-%llx-f%x-%ux%u-l%u.%s", dir, (unsigned long long)(uintptr_t)t->key.mem, d->format, w, h, layer,
             ch == 1 ? "pgm" : "ppm");
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, ch == 1 ? "P5\n%u %u\n255\n" : "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; y++) {
            if (ch == 1) fwrite(px + y * pitch, 1, w, f);
            else
                for (uint32_t x = 0; x < w; x++) fwrite(px + y * pitch + (size_t)x * ch, 1, 3, f);
        }
        fclose(f);
    }
    free(px);
    free(tmp);
}

void mtl_tex_prepare_sample(GfxTex *t) {
    if (t->gpu_owned || !t->key.mem || t->rsx != 1 || t->rsy != 1) return;
    uint64_t now = dirty_now();
    if (t->stamp && t->checked == now) return;
    t->checked = now;
    if (t->stamp && !dirty_since(t->key.mem, (size_t)t->key.size, t->stamp)) return;
    dirty_protect(t->key.mem, (size_t)t->key.size); /* protect, stamp, then read: no write can be lost */
    t->stamp = dirty_now();
    upload_texture(t);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Views
 * --------------------------------------------------------------------------------------------------------------- */
static MTLTextureType shader_type(int st, int array_hint, MTLTextureType base) {
    switch (st) {
        case TEXTYPE_1D: return MTLTextureType1D;
        case TEXTYPE_2D: return base == MTLTextureType2DMultisample ? base : MTLTextureType2D;
        case TEXTYPE_3D: return MTLTextureType3D;
        case TEXTYPE_CUBE: return MTLTextureTypeCube;
        case TEXTYPE_2D_ARRAY: return MTLTextureType2DArray;
        case TEXTYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
        case TEXTYPE_1D_ARRAY: return MTLTextureType1DArray;
        default: (void)array_hint; return base;
    }
}

/* Metal aborts the process (not a nil return) on a view it cannot make. Checks its rules; a format change it does not allow falls back to the
 * texture's own format (*pf is updated), anything else returns 0. */
static int same_class_srgb(MTLPixelFormat a, MTLPixelFormat b) {
    static const MTLPixelFormat pairs[][2] = {
        {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB}, {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
        {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB},     {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
        {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB},     {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB},
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if ((a == pairs[i][0] && b == pairs[i][1]) || (a == pairs[i][1] && b == pairs[i][0])) return 1;
    return 0;
}
static int view_valid(id<MTLTexture> tex, MTLPixelFormat *pf, MTLTextureType want, uint32_t l0, uint32_t nl, uint32_t s0, uint32_t ns) {
    if (*pf != tex.pixelFormat && !(tex.usage & MTLTextureUsagePixelFormatView) && !same_class_srgb(*pf, tex.pixelFormat)) *pf = tex.pixelFormat;
    if (l0 + nl > tex.mipmapLevelCount || !nl) return 0;
    MTLTextureType have = tex.textureType;
    NSUInteger slices = tex.arrayLength ? tex.arrayLength : 1;
    if (have == MTLTextureTypeCube) slices = 6;
    else if (have == MTLTextureTypeCubeArray) slices *= 6;
    if (have == MTLTextureType3D) return want == MTLTextureType3D && s0 == 0 && ns == 1;
    if (want == MTLTextureType3D || s0 + ns > slices || !ns) return 0;
    switch (want) {
        case MTLTextureType2D:
        case MTLTextureType1D:
            return ns == 1 && (have == MTLTextureType2D || have == MTLTextureType2DArray || have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray ||
                               have == MTLTextureType1D || have == MTLTextureType1DArray);
        case MTLTextureType2DArray:
        case MTLTextureType1DArray: return have != MTLTextureType2DMultisample && have != MTLTextureType2DMultisampleArray;
        case MTLTextureTypeCube:
            return ns == 6 && tex.width == tex.height && (have == MTLTextureType2DArray || have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray);
        case MTLTextureTypeCubeArray:
            return ns % 6 == 0 && tex.width == tex.height && (have == MTLTextureType2DArray || have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray);
        case MTLTextureType2DMultisample: return ns == 1 && (have == MTLTextureType2DMultisample || have == MTLTextureType2DMultisampleArray);
        default: return have == want;
    }
}

id<MTLTexture> mtl_tex_view(GfxTex *t, const uint8_t tswz[4], const view_t *v, int has_view, int stype, int shadow) {
    (void)shadow;
    MTLTextureType want = shader_type(stype, 0, t->tex.textureType);
    MTLPixelFormat pf = t->pf;
    uint32_t l0 = 0, nl = t->tex.mipmapLevelCount, s0 = 0, ns = t->tex.arrayLength ? t->tex.arrayLength : 1;
    if (t->tex.textureType == MTLTextureTypeCube) ns = 6;
    else if (t->tex.textureType == MTLTextureTypeCubeArray) ns = t->tex.arrayLength * 6;
    if (has_view) {
        if (v->set_levels) {
            l0 = v->base_level;
            nl = v->nlevels;
        }
        if (v->set_layers) {
            s0 = v->min_layer;
            ns = v->nlayers;
        }
        if (v->set_format) {
            uint32_t m = 0, bcpf;
            int bpp;
            if (napple_fmt_to_mtl(v->format, &m, NULL) && m) pf = (MTLPixelFormat)m;
            if (!g_mtl.bc_ok && napple_bc_unpacked((uint32_t)v->format, &bcpf, &bpp))
                pf = (MTLPixelFormat)bcpf; /* BC textures are stored decoded on GPUs without BC (same mapping as mtl_tex_format_info) */
        }
    }
    if (l0 + nl > t->tex.mipmapLevelCount) { nl = l0 < t->tex.mipmapLevelCount ? t->tex.mipmapLevelCount - l0 : 1; }
    /* swizzle: view swizzle composed with the format component map and the texture-level swizzle */
    uint32_t m = 0;
    uint8_t fswz[4];
    napple_fmt_to_mtl(t->key.d.format, &m, fswz);
    uint8_t user[4] = {tswz[0], tswz[1], tswz[2], tswz[3]};
    if (has_view && v->set_swizzle) memcpy(user, v->swz, 4);
    uint8_t out[4];
    for (int i = 0; i < 4; i++) out[i] = user[i] < 2 ? user[i] : fswz[user[i] - 2];
    if (t->is_depth) {
        out[0] = 2;
        out[1] = 3;
        out[2] = 4;
        out[3] = 5;
    } /* depth views are sampled as one channel */
    int identity = out[0] == 2 && out[1] == 3 && out[2] == 4 && out[3] == 5;
    int full = l0 == 0 && nl == t->tex.mipmapLevelCount && s0 == 0;
    if (identity && pf == t->pf && want == t->tex.textureType && full && (ns == (t->tex.arrayLength ? t->tex.arrayLength : 1) || want == MTLTextureTypeCube))
        return t->tex;
    uint64_t key = (uint64_t)pf | (uint64_t)want << 12 | (uint64_t)l0 << 16 | (uint64_t)nl << 20 | (uint64_t)s0 << 24 | (uint64_t)ns << 36 |
                   (uint64_t)(out[0] & 7) << 48 | (uint64_t)(out[1] & 7) << 51 | (uint64_t)(out[2] & 7) << 54 | (uint64_t)(out[3] & 7) << 57;
    NSNumber *k = @(key);
    id<MTLTexture> cached = t->views[k];
    if (cached) return cached;
    MTLTextureSwizzleChannels sw =
        MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)out[0], (MTLTextureSwizzle)out[1], (MTLTextureSwizzle)out[2], (MTLTextureSwizzle)out[3]);
    if (!view_valid(t->tex, &pf, want, l0, nl ? nl : 1, s0, ns ? ns : 1)) {
        LOG_ONCE(NAPPLE_LOG_WARN,
                 "texture view not expressible in Metal (fmt %lu->%lu type %lu->%lu levels %u+%u/%lu slices %u+%u/%lu): using the whole texture",
                 (unsigned long)t->pf, (unsigned long)pf, (unsigned long)t->tex.textureType, (unsigned long)want, l0, nl,
                 (unsigned long)t->tex.mipmapLevelCount, s0, ns, (unsigned long)t->tex.arrayLength);
        t->views[k] = t->tex;
        return t->tex;
    }
    id<MTLTexture> nv = [t->tex newTextureViewWithPixelFormat:pf
                                                  textureType:want
                                                       levels:NSMakeRange(l0, nl ? nl : 1)
                                                       slices:NSMakeRange(s0, ns ? ns : 1)
                                                      swizzle:sw];
    if (!nv) {
        LOG_ONCE(NAPPLE_LOG_WARN, "texture view creation failed (fmt %lu type %lu)", (unsigned long)pf, (unsigned long)want);
        return t->tex;
    }
    t->views[k] = nv;
    return nv;
}

/* Render target attachment: the base texture with level / slice chosen by the view. A view with another format (an sRGB view of a UNORM target,
 * a UINT view of a float one of the same size) renders through a format view of the whole texture; the attachment level / slice still pick the
 * image. The format must be view-compatible (same size per pixel); otherwise the base format is used and that is reported once. */
id<MTLTexture> mtl_tex_attach_view(GfxTex *t, const view_t *v, int has_view, uint32_t *level, uint32_t *slice) {
    *level = 0;
    *slice = 0;
    if (!has_view) return t->tex;
    if (v->set_levels) *level = v->base_level;
    if (v->set_layers) *slice = v->min_layer;
    uint32_t m = 0;
    if (!v->set_format || t->is_depth || !napple_fmt_to_mtl(v->format, &m, NULL) || !m || (MTLPixelFormat)m == t->pf) return t->tex;
    MTLPixelFormat pf = (MTLPixelFormat)m;
    NSNumber *k = @((uint64_t)pf | 1ull << 63); /* not a sampled-view key (those keep bit 63 clear) */
    id<MTLTexture> cached = t->views[k];
    if (cached) return cached;
    int bw, bh, bv = napple_fmt_bpb(v->format, &bw, &bh), bt = napple_fmt_bpb(t->key.d.format, &bw, &bh);
    id<MTLTexture> nv = nil;
    MTLPixelFormat sp = t->tex.pixelFormat;
    int src_bc = sp >= MTLPixelFormatBC1_RGBA && sp <= MTLPixelFormatBC7_RGBAUnorm_sRGB,
        dst_bc = pf >= MTLPixelFormatBC1_RGBA && pf <= MTLPixelFormatBC7_RGBAUnorm_sRGB;
    if (src_bc && !dst_bc) {
        /* Rendering into a block-compressed texture through an integer view (one texel per 4x4 block, e.g. runtime texture compression): Metal cannot view BC as uint and a BC
         * texture is not renderable, so the pass draws into an auxiliary texture of the block grid. TODO: copy it back into the BC texture before that is sampled. */
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                      width:MAX(1, (t->tex.width + 3) / 4)
                                                                                     height:MAX(1, (t->tex.height + 3) / 4)
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        nv = [g_mtl.dev newTextureWithDescriptor:td];
        LOG_ONCE(NAPPLE_LOG_WARN,
                 "render target view %#x of a block-compressed texture (format %#x): rendering into a block-grid texture; the result is not copied back into "
                 "the compressed texture yet",
                 v->format, t->key.d.format);
    } else if (bv && bv == bt && (t->tex.usage & MTLTextureUsagePixelFormatView)) nv = [t->tex newTextureViewWithPixelFormat:pf];
    if (!nv) {
        LOG_ONCE(NAPPLE_LOG_WARN, "render target view format %#x is not view-compatible with the texture (format %#x): drawn in the texture's format",
                 v->format, t->key.d.format);
        nv = t->tex;
    }
    t->views[k] = nv;
    return nv;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Samplers
 * --------------------------------------------------------------------------------------------------------------- */
static NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *g_samplers;
static MTLSamplerAddressMode wrap_mode(int m) {
    switch (m) {
        case 1: return MTLSamplerAddressModeRepeat;
        case 2:
        case 3: return MTLSamplerAddressModeMirrorClampToEdge;
        case 4:
        case 5: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat;
        default: return MTLSamplerAddressModeClampToEdge; /* CLAMP, CLAMP_TO_EDGE */
    }
}
id<MTLSamplerState> mtl_sampler_get(const samp_par *p) {
    napple_display_settings dset;
    napple_display_get(&dset); /* player's minimum anisotropy (Video settings), part of the cache key */
    uint64_t h = fnv1a(&dset.anisotropy, sizeof dset.anisotropy, fnv1a(p, sizeof *p, FNV_INIT));
    NSNumber *k = @(h);
    if (!g_samplers) g_samplers = [NSMutableDictionary dictionary];
    id<MTLSamplerState> s = g_samplers[k];
    if (s) return s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = (p->minf & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = p->magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p->minf < 2 ? MTLSamplerMipFilterNotMipmapped : (p->minf >= 4 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest);
    d.sAddressMode = wrap_mode(p->ws);
    d.tAddressMode = wrap_mode(p->wt);
    d.rAddressMode = wrap_mode(p->wr);
    float mx = p->aniso < 1.0f ? 1.0f : (p->aniso > 16.0f ? 16.0f : p->aniso);
    napple_gfx_settings st;
    gfx_settings_get(&st);
    if (st.anisotropy) mx = (float)st.anisotropy > 16.0f ? 16.0f : (float)st.anisotropy;
    if (dset.anisotropy > 1 && mx < (float)dset.anisotropy && p->minf != 0 && p->magf != 0)
        mx = (float)dset.anisotropy; /* only for filtered samplers: nearest stays nearest */
    d.maxAnisotropy = (NSUInteger)mx;
    d.lodMinClamp = p->lod_min;
    d.lodMaxClamp = p->lod_max < p->lod_min ? p->lod_min : p->lod_max;
    if (p->cmp_mode == 1) {
        static const MTLCompareFunction cf[] = {MTLCompareFunctionNever,    MTLCompareFunctionNever,        MTLCompareFunctionLess,
                                                MTLCompareFunctionEqual,    MTLCompareFunctionLessEqual,    MTLCompareFunctionGreater,
                                                MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
        d.compareFunction = cf[p->cmp_func > 8 ? 8 : p->cmp_func];
    }
    float b = p->border[0] + p->border[1] + p->border[2];
    d.borderColor =
        p->border[3] == 0 && b == 0 ? MTLSamplerBorderColorTransparentBlack : (b == 0 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite);
    d.supportArgumentBuffers = NO;
    s = [g_mtl.dev newSamplerStateWithDescriptor:d];
    g_samplers[k] = s;
    return s;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Vertex formats
 * --------------------------------------------------------------------------------------------------------------- */
uint32_t mtl_vertex_format(int f, int *kind) {
    *kind = 0;
    /* component-count groups in NVN enum order: [first format][type order] */
    /* component-count groups in NVN enum order; kind: unorm/snorm/float 0, uint 2, sint 1 */
    static const uint8_t k8[] = {0, 0, 2, 1}, k16[] = {0, 0, 0, 2, 1}, k32[] = {0, 2, 1};
    static const MTLVertexFormat r8[] = {MTLVertexFormatUCharNormalized, MTLVertexFormatCharNormalized, MTLVertexFormatUChar, MTLVertexFormatChar};
    static const MTLVertexFormat r16[] = {MTLVertexFormatHalf, MTLVertexFormatUShortNormalized, MTLVertexFormatShortNormalized, MTLVertexFormatUShort,
                                          MTLVertexFormatShort};
    static const MTLVertexFormat r32[] = {MTLVertexFormatFloat, MTLVertexFormatUInt, MTLVertexFormatInt};
    static const MTLVertexFormat rg8[] = {MTLVertexFormatUChar2Normalized, MTLVertexFormatChar2Normalized, MTLVertexFormatUChar2, MTLVertexFormatChar2};
    static const MTLVertexFormat rg16[] = {MTLVertexFormatHalf2, MTLVertexFormatUShort2Normalized, MTLVertexFormatShort2Normalized, MTLVertexFormatUShort2,
                                           MTLVertexFormatShort2};
    static const MTLVertexFormat rg32[] = {MTLVertexFormatFloat2, MTLVertexFormatUInt2, MTLVertexFormatInt2};
    static const MTLVertexFormat rgb8[] = {MTLVertexFormatUChar3Normalized, MTLVertexFormatChar3Normalized, MTLVertexFormatUChar3, MTLVertexFormatChar3};
    static const MTLVertexFormat rgb16[] = {MTLVertexFormatHalf3, MTLVertexFormatUShort3Normalized, MTLVertexFormatShort3Normalized, MTLVertexFormatUShort3,
                                            MTLVertexFormatShort3};
    static const MTLVertexFormat rgb32[] = {MTLVertexFormatFloat3, MTLVertexFormatUInt3, MTLVertexFormatInt3};
    static const MTLVertexFormat rgba8[] = {MTLVertexFormatUChar4Normalized, MTLVertexFormatChar4Normalized, MTLVertexFormatUChar4, MTLVertexFormatChar4};
    static const MTLVertexFormat rgba16[] = {MTLVertexFormatHalf4, MTLVertexFormatUShort4Normalized, MTLVertexFormatShort4Normalized, MTLVertexFormatUShort4,
                                             MTLVertexFormatShort4};
    static const MTLVertexFormat rgba32[] = {MTLVertexFormatFloat4, MTLVertexFormatUInt4, MTLVertexFormatInt4};
    static const struct {
        int first, n;
        const MTLVertexFormat *v;
        const uint8_t *k;
    } g[] = {
        {0x01, 4, r8, k8},   {0x05, 5, r16, k16},   {0x0A, 3, r32, k32},   {0x0D, 4, rg8, k8},   {0x11, 5, rg16, k16},   {0x16, 3, rg32, k32},
        {0x19, 4, rgb8, k8}, {0x1D, 5, rgb16, k16}, {0x22, 3, rgb32, k32}, {0x25, 4, rgba8, k8}, {0x29, 5, rgba16, k16}, {0x2E, 3, rgba32, k32},
    };
    for (unsigned i = 0; i < sizeof g / sizeof *g; i++)
        if (f >= g[i].first && f < g[i].first + g[i].n) {
            *kind = g[i].k[f - g[i].first];
            return (uint32_t)g[i].v[f - g[i].first];
        }
    if (f == 0x3D) return (uint32_t)MTLVertexFormatUInt1010102Normalized;
    if (f == 0x3E) {
        *kind = 2;
        return (uint32_t)MTLVertexFormatUInt1010102Normalized;
    }
    if (f == 0x69) return (uint32_t)MTLVertexFormatInt1010102Normalized; /* RGB10A2SN: packed normals / tangents of Link's Awakening meshes */
    /* *_UI2F / *_I2F: integer storage read as a float with no normalisation = Metal's plain integer vertex formats feeding a float attribute (kind 0) */
    if (f >= 0x51 && f <= 0x68) {
        static const MTLVertexFormat ui2f[] = {
            MTLVertexFormatUChar,  MTLVertexFormatChar,  MTLVertexFormatUShort,  MTLVertexFormatShort,  MTLVertexFormatUInt,  MTLVertexFormatInt,
            MTLVertexFormatUChar2, MTLVertexFormatChar2, MTLVertexFormatUShort2, MTLVertexFormatShort2, MTLVertexFormatUInt2, MTLVertexFormatInt2,
            MTLVertexFormatUChar3, MTLVertexFormatChar3, MTLVertexFormatUShort3, MTLVertexFormatShort3, MTLVertexFormatUInt3, MTLVertexFormatInt3,
            MTLVertexFormatUChar4, MTLVertexFormatChar4, MTLVertexFormatUShort4, MTLVertexFormatShort4, MTLVertexFormatUInt4, MTLVertexFormatInt4};
        return (uint32_t)ui2f[f - 0x51];
    }
    if (f == 0x99) return (uint32_t)MTLVertexFormatUChar4Normalized_BGRA;
    if (f == 0x9A) return (uint32_t)MTLVertexFormatUChar4Normalized_BGRA;
    return (uint32_t)MTLVertexFormatInvalid;
}
