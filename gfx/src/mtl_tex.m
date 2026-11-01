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
    if ((d->flags & 0x4) && (t->pf == MTLPixelFormatR32Uint || t->pf == MTLPixelFormatR32Sint)) {
        if (@available(macOS 14.0, iOS 17.0, *)) td.usage |= MTLTextureUsageShaderAtomic; /* SUATOM / SURED on an image (surface atomics) */
    }
    if (t->samples == 1 && !t->is_depth) td.usage |= MTLTextureUsageShaderWrite;
    t->tex = [g_mtl.dev newTextureWithDescriptor:td];
    if (!t->tex) {
        LOGE("texture creation failed (%ux%ux%u fmt %#x)", t->w, t->h, t->depth, d->format);
        return nil;
    }
    t->views = [NSMutableDictionary dictionary];
    mtl_scale_count(pk.rx != 1 || pk.ry != 1, pk.rx != 1 || pk.ry != 1 ? (uint64_t)pk.w * pk.h * 4 * t->samples : 0);
    if (t->samples > 1)
        LOG_ONCE(NAPPLE_LOG_INFO, "multisampled textures in use (first: %ux%u, %u samples, format %#x, target %d)", t->w, t->h, t->samples, d->format, d->target);
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
    /* A sparse texture (NVN_TEXTURE_FLAGS_SPARSE) never becomes GPU-owned: the GPU writes some tiles (copies into newly resident tiles), the guest memory and its
     * residency still decide the others, and its re-uploads only touch the tiles whose pages changed (upload_texture_part). Not render-scaled either. */
    int sparse = t && (t->key.d.flags & 0x20);
    if (t && (t->scale_gen != mtl_scale_generation() || (for_gpu_write && !sparse && !t->gpu_owned && t->rsx == 1 && t->rsy == 1))) mtl_tex_rescale(t, for_gpu_write);
    pthread_mutex_unlock(&g_tt_mu);
    if (t) {
        t->last_use = atomic_load(&g_gfx.frames);
        if (for_gpu_write && !sparse) t->gpu_owned = 1;
    }
    return t;
}

/* nvnMemoryPoolMapVirtual (api_memory.c) replaces the pool records of the remapped range of a virtual pool: the textures there are not gone (their
 * storage changed residency, the dirty tracking sees it and a sparse texture re-uploads the changed tiles), so they are kept, with what the GPU wrote
 * into them. Thread-local: the remap runs on the calling thread. */
static _Thread_local int g_remapping;
void mtl_tex_remap_scope(int on) { g_remapping = on; }
void mtl_tex_forget_range(const void *mem, uint64_t size) {
    if (g_remapping) return;
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

/* Sparse textures (NVN_TEXTURE_FLAGS_SPARSE): a residency change (nvnMemoryPoolMapVirtual remapping or unmapping pages) after an upload makes the pages
 * dirty, and a full re-upload from guest memory would wipe what the GPU wrote into the other tiles (GPU writes never reach guest memory). So a 2D sparse
 * texture re-uploads only the tiles whose storage changed: the tile shape of api_texture.c (64 KiB of level-0 storage per tile, tiles in row-major
 * order), the levels after level 0 as a whole when their pages changed. */
typedef struct {
    const uint8_t *dirty; /* per level-0 tile: re-upload it */
    uint32_t tw, th, ntx; /* tile size in texels, tiles per row */
    int rest;             /* levels after level 0 changed */
} part_upload;
static int sparse_tiles(const napple_texdesc *d, uint32_t *tw, uint32_t *th) {
    if (!(d->flags & 0x20) || d->target != 1 || d->samples > 1 || napple_tex_is_linear(d)) return 0; /* NVN_TEXTURE_FLAGS_SPARSE, 2D */
    int bw = 1, bh = 1, bpb = napple_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return 0;
    uint32_t w = bpb == 1 ? 256 : bpb == 2 ? 256 : bpb == 4 ? 128 : bpb == 8 ? 128 : 64, h = bpb == 1 ? 256 : bpb == 2 ? 128 : bpb == 4 ? 128 : bpb == 8 ? 64 : 64;
    *tw = w * (uint32_t)bw;
    *th = h * (uint32_t)bh;
    return 1;
}

static void upload_texture_part(GfxTex *t, const part_upload *part);
static void upload_texture_part(GfxTex *t, const part_upload *part) {
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
        if (part && lv > 0 && !part->rest) break;
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
            if (part && lv == 0 && !bc_decode && !t->expand_rgb) { /* the changed tiles only (sparse, see part_upload) */
                uint32_t nty = (h + part->th - 1) / part->th;
                for (uint32_t ty = 0; ty < nty; ty++)
                    for (uint32_t tx = 0; tx < part->ntx; tx++) {
                        if (!part->dirty[ty * part->ntx + tx]) continue;
                        uint32_t x0 = tx * part->tw, y0 = ty * part->th, rw = MIN(part->tw, w - x0), rh = MIN(part->th, h - y0);
                        const uint8_t *src = tmp + (size_t)(y0 / bh) * row_bytes + (size_t)(x0 / bw) * bpb;
                        [t->tex replaceRegion:MTLRegionMake2D(x0, y0, rw, rh) mipmapLevel:0 slice:slice withBytes:src bytesPerRow:row_bytes bytesPerImage:0];
                    }
            } else if (bc_decode) {
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
    /* a sparse texture uploaded before: which tiles changed (checked against the old stamp, before the pages are protected again) */
    uint32_t tw, th;
    uint8_t *dirty = NULL;
    part_upload part = {0};
    if (t->stamp && sparse_tiles(&t->key.d, &tw, &th)) {
        uint64_t l0 = t->levels > 1 ? napple_tex_level_offset(&t->key.d, 1) : t->key.size;
        uint32_t ntx = (t->w + tw - 1) / tw, nty = (t->h + th - 1) / th;
            if (l0 <= t->key.size && (uint64_t)ntx * nty * 0x10000 <= l0 + 0xffff && (dirty = calloc((size_t)ntx * nty, 1))) {
            for (uint32_t i = 0; i < ntx * nty; i++) {
                uint64_t o = (uint64_t)i * 0x10000;
                if (o < l0) dirty[i] = (uint8_t)dirty_since(t->key.mem + o, (size_t)MIN(0x10000, l0 - o), t->stamp);
            }
            part = (part_upload){dirty, tw, th, ntx, t->levels > 1 && dirty_since(t->key.mem + l0, (size_t)(t->key.size - l0), t->stamp)};
        }
    }
    dirty_protect(t->key.mem, (size_t)t->key.size); /* protect, stamp, then read: no write can be lost */
    t->stamp = dirty_now();
    upload_texture_part(t, dirty ? &part : NULL);
    free(dirty);
    mtl_tex_written(t);
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

/* ---------------------------------------------------------------------------------------------------------------
 * Copy-backed views (mtl_priv.h): what newTextureView cannot do over the same storage
 * --------------------------------------------------------------------------------------------------------------- */
/* Bytes per texel (per 4x4 block for BC); *cls: 0 colour, 1 depth / stencil, 2 block-compressed (BC, 4x4). 0 = not handled (ASTC, YUV, ...). The depth plane
 * of Depth32Float_Stencil8 counts as 4 bytes: that is what a blit with MTLBlitOptionDepthFromDepthStencil writes. */
static uint32_t pf_bytes(MTLPixelFormat f, int *cls) {
    *cls = 0;
    switch ((NSUInteger)f) {
        case 10: case 11: case 12: case 13: case 14: return 1;
        case 20: case 22: case 23: case 24: case 25: case 30: case 32: case 33: case 34: case 40: case 41: case 42: case 43: return 2;
        case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65: case 70: case 71: case 72: case 73: case 74: case 80: case 81:
        case 90: case 91: case 92: case 93: return 4;
        case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;
        case 123: case 124: case 125: return 16;
        case 130: case 131: case 140: case 141: *cls = 2; return 8;
        case 132: case 133: case 134: case 135: case 142: case 143: case 150: case 151: case 152: case 153: *cls = 2; return 16;
        case 253: *cls = 1; return 1;          /* Stencil8 */
        case 250: *cls = 1; return 2;          /* Depth16Unorm */
        case 252: case 260: *cls = 1; return 4; /* Depth32Float, Depth32Float_Stencil8 (depth plane) */
        default: return 0;
    }
}
static int pf_int_kind(MTLPixelFormat f) { /* 0 float / normalized, 1 sint, 2 uint */
    switch ((NSUInteger)f) {
        case 14: case 24: case 34: case 54: case 64: case 74: case 104: case 114: case 124: return 1;
        case 13: case 23: case 33: case 53: case 63: case 73: case 91: case 103: case 113: case 123: return 2;
        default: return 0;
    }
}
/* A multisampled depth texture viewed as one of these colour formats is converted by a per-sample pass (alias_ms_pso). Only float-sampled formats: the
 * translator declares multisampled textures as texture2d_ms<float>. */
static int ms_alias_dst_ok(MTLPixelFormat src, MTLPixelFormat dst) {
    return (src == MTLPixelFormatDepth32Float || src == MTLPixelFormatDepth32Float_Stencil8) && dst == MTLPixelFormatRGBA8Unorm;
}
/* Single-channel unsigned integer formats: a copy in one of them is bound like a stencil view (ShaderKey.stencil_slots: texture2d<uint>, (value, 0, 0, 1) as
 * integer bits), the only integer sampling the translator declares. Other integer copies cannot be bound (see view_fmt_class). */
static int pf_uint1(MTLPixelFormat f) { return f == MTLPixelFormatR8Uint || f == MTLPixelFormatR16Uint || f == MTLPixelFormatR32Uint; }
/* How a sampled view's format is honoured: */
enum {
    VF_SAME,  /* the texture's own format */
    VF_VIEW,  /* a Metal format view (same size class, both plain colour, or an sRGB pair) */
    VF_DEPTH, /* a depth texture viewed as a single-channel format whose texel reads back as the same register bits: R32F / R32UI / R32I of 32-bit float depth (the
               * shader moves texel results into registers bit for bit, so the float depth IS the integer view's value), R16 unorm of Depth16. Sampled as depth. */
    VF_ALIAS, /* same bytes per texel / block, another class: a copy (GfxAlias) */
    VF_NONE,  /* not expressible (another texel size, ASTC, a 24-bit depth format read as bits, an integer format the translator cannot declare): the texture's own
               * format, reported once */
};
static int g_int_slots; /* the replay marks integer sampled bindings (it calls mtl_tex_int_kind): integer views can then be copies the shader reads as integers */
static int view_fmt_class(const texsnap *s, MTLPixelFormat have, MTLPixelFormat want, uint32_t samples) {
    if (want == have || want == MTLPixelFormatInvalid) return VF_SAME;
    if (same_class_srgb(want, have)) return VF_VIEW;
    int ch, cw;
    uint32_t bh = pf_bytes(have, &ch), bw = pf_bytes(want, &cw);
    if (!bh || !bw) return VF_NONE;
    int d24 = ch == 1 && (s->d.format == 0x33 || s->d.format == 0x35); /* NVN DEPTH24 / DEPTH24_STENCIL8 are stored as 32-bit float depth: their packed bits do not exist here */
    if (ch == 1 && cw == 0 &&
        ((have == MTLPixelFormatDepth16Unorm && want == MTLPixelFormatR16Unorm) ||
         ((have == MTLPixelFormatDepth32Float || have == MTLPixelFormatDepth32Float_Stencil8) &&
          (want == MTLPixelFormatR32Float || (!d24 && (want == MTLPixelFormatR32Uint || want == MTLPixelFormatR32Sint))))))
        return VF_DEPTH;
    if (ch == 0 && cw == 0) return bh == bw ? VF_VIEW : VF_NONE;
    if (bh != bw || have == MTLPixelFormatStencil8 || want == MTLPixelFormatStencil8 || want == MTLPixelFormatDepth32Float_Stencil8 || d24) return VF_NONE;
    /* an integer view is a copy when the shader can declare it as an integer texture: single-channel uint as a stencil slot, any integer format once the replay
     * fills ShaderKey.tex_uint_slots / tex_sint_slots (g_int_slots); otherwise the translator samples it as float (texture2d<float>) */
    if (cw == 0 && pf_int_kind(want) && !(pf_uint1(want) && samples == 1) && !(g_int_slots && samples == 1)) return VF_NONE;
    if (samples > 1) return ch == 1 && cw == 0 && ms_alias_dst_ok(have, want) ? VF_ALIAS : VF_NONE;
    return VF_ALIAS;
}
/* The Metal format a view's NVN format names (BC decoded on GPUs without BC, as in mtl_tex_format_info); Invalid when the view keeps the texture's format. */
static MTLPixelFormat view_pf(const view_t *v, int has_view) {
    if (!has_view || !v->set_format) return MTLPixelFormatInvalid;
    uint32_t m = 0, bcpf;
    int bpp;
    MTLPixelFormat pf = napple_fmt_to_mtl(v->format, &m, NULL) && m ? (MTLPixelFormat)m : MTLPixelFormatInvalid;
    if (!g_mtl.bc_ok && napple_bc_unpacked((uint32_t)v->format, &bcpf, &bpp)) pf = (MTLPixelFormat)bcpf;
    return pf;
}
int mtl_tex_samples_as_uint(const texsnap *s, const view_t *v, int has_view) {
    MTLPixelFormat pf;
    int dep, st;
    mtl_tex_format_info(s, &pf, &dep, &st);
    MTLPixelFormat want = view_pf(v, has_view);
    return view_fmt_class(s, pf, want, s->d.samples > 1 ? (uint32_t)s->d.samples : 1) == VF_ALIAS && pf_uint1(want);
}
/* The component type of the Metal texture a sampled binding gets (its own format, a format view or a copy): IMGKIND_UINT / IMGKIND_INT for integer colour
 * formats (ShaderKey.tex_uint_slots / tex_sint_slots), IMGKIND_FLOAT otherwise. Single-channel uint copies stay on the stencil-slot path
 * (mtl_tex_samples_as_uint) and depth reads on the depth path. Calling it tells view_fmt_class that integer views may be copies. */
int mtl_tex_int_kind(const texsnap *s, const view_t *v, int has_view) {
    g_int_slots = 1;
    MTLPixelFormat pf;
    int dep, st;
    mtl_tex_format_info(s, &pf, &dep, &st);
    uint32_t samples = s->d.samples > 1 ? (uint32_t)s->d.samples : 1;
    MTLPixelFormat want = view_pf(v, has_view);
    int c = view_fmt_class(s, pf, want, samples);
    if (c == VF_DEPTH || (c == VF_ALIAS && pf_uint1(want) && samples == 1)) return IMGKIND_FLOAT;
    int k = pf_int_kind(c == VF_VIEW || c == VF_ALIAS ? want : pf);
    return k == 1 ? IMGKIND_INT : k == 2 ? IMGKIND_UINT : IMGKIND_FLOAT;
}
int mtl_tex_samples_as_depth(const texsnap *s, const view_t *v, int has_view) {
    MTLPixelFormat pf;
    int dep, st;
    mtl_tex_format_info(s, &pf, &dep, &st);
    MTLPixelFormat want = view_pf(v, has_view);
    int c = view_fmt_class(s, pf, want, s->d.samples > 1 ? (uint32_t)s->d.samples : 1);
    if (c == VF_ALIAS) { int cw; pf_bytes(want, &cw); return cw == 1; }
    return dep;
}

enum { AK_BYTES = 1, AK_MS_DEPTH, AK_TYPE };
@interface GfxAlias : NSObject {
@public
    id<MTLTexture> src, tex; /* the source storage it copies from (a re-allocated source makes a new alias), the copy */
    __weak GfxTex *owner;    /* the texture it is a view of (its wgen when the copy is encoded is what the copy holds) */
    id<MTLBuffer> scratch;   /* AK_BYTES: staging bytes */
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *views; /* swizzle / format views of tex */
    int kind;
    uint32_t l0, nl, s0, ns;
    uint64_t gen;            /* source wgen the queued / last copy reads */
    int queued, filled;
}
@end
@implementation GfxAlias
@end
static NSMutableArray<GfxAlias *> *g_alias_q;
static pthread_mutex_t g_alias_mu = PTHREAD_MUTEX_INITIALIZER;
uint32_t mtl_tex_alias_pending(void) { return (uint32_t)g_alias_q.count; }

static int pf_is_bc(MTLPixelFormat f) { return f >= MTLPixelFormatBC1_RGBA && f <= MTLPixelFormatBC7_RGBAUnorm_sRGB; }
/* Size of the copy of source level `lv` in the alias' texels: one alias texel per source block (BC -> plain), four per block the other way. */
static void alias_level_size(id<MTLTexture> s, MTLPixelFormat dst_pf, uint32_t lv, NSUInteger *w, NSUInteger *h) {
    NSUInteger sw = MAX(1, s.width >> lv), sh = MAX(1, s.height >> lv);
    int sbc = pf_is_bc(s.pixelFormat), dbc = pf_is_bc(dst_pf);
    if (sbc && !dbc) { sw = (sw + 3) / 4; sh = (sh + 3) / 4; }
    else if (!sbc && dbc) { sw *= 4; sh *= 4; }
    *w = sw;
    *h = sh;
}
static GfxAlias *alias_create(GfxTex *t, int kind, MTLPixelFormat pf, MTLTextureType type, uint32_t l0, uint32_t nl, uint32_t s0, uint32_t ns) {
    id<MTLTexture> s = t->tex;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = pf;
    NSUInteger w, h;
    alias_level_size(s, pf, l0, &w, &h);
    td.width = w;
    td.height = (type == MTLTextureType1D || type == MTLTextureType1DArray) ? 1 : h;
    td.depth = type == MTLTextureType3D ? (s.textureType == MTLTextureType3D ? MAX(1, s.depth >> l0) : ns) : 1;
    td.mipmapLevelCount = s.sampleCount > 1 ? 1 : nl;
    td.arrayLength = type == MTLTextureTypeCube || type == MTLTextureType3D || type == MTLTextureType2D || type == MTLTextureType1D || type == MTLTextureType2DMultisample
                         ? 1
                         : type == MTLTextureTypeCubeArray ? MAX(1, ns / 6) : ns;
    td.sampleCount = s.sampleCount;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | (kind == AK_MS_DEPTH ? MTLTextureUsageRenderTarget : 0);
    id<MTLTexture> a = [g_mtl.dev newTextureWithDescriptor:td];
    if (!a) {
        LOG_ONCE(NAPPLE_LOG_WARN, "copy-backed texture view (format %lu type %lu %lux%lu) could not be allocated", (unsigned long)pf, (unsigned long)type,
                 (unsigned long)w, (unsigned long)h);
        return nil;
    }
    GfxAlias *al = [GfxAlias new];
    al->src = s;
    al->owner = t;
    al->tex = a;
    al->kind = kind;
    al->l0 = l0;
    al->nl = (uint32_t)td.mipmapLevelCount;
    al->s0 = s0;
    al->ns = ns;
    al->views = [NSMutableDictionary dictionary];
    al->gen = ~0ull;
    return al;
}
/* The alias for (key), refreshed if the source moved on; the copy is encoded later (mtl_tex_alias_flush). */
static GfxAlias *alias_get(GfxTex *t, uint64_t key, int kind, MTLPixelFormat pf, MTLTextureType type, uint32_t l0, uint32_t nl, uint32_t s0, uint32_t ns) {
    if (!t->aliases) t->aliases = [NSMutableDictionary dictionary];
    NSNumber *k = @(key);
    GfxAlias *al = t->aliases[k];
    if (al && al->src != t->tex) al = nil; /* the source was re-allocated (render scale): copy from the new storage */
    if (!al) {
        al = alias_create(t, kind, pf, type, l0, nl, s0, ns);
        if (!al) return nil;
        t->aliases[k] = al;
        LOG_ONCE(NAPPLE_LOG_INFO, "copy-backed texture views in use (first: kind %d, format %lu -> %lu, type %lu -> %lu, levels %u+%u slices %u+%u)", kind,
                 (unsigned long)t->tex.pixelFormat, (unsigned long)pf, (unsigned long)t->tex.textureType, (unsigned long)type, l0, nl, s0, ns);
    }
    if (al->gen != t->wgen) {
        al->gen = t->wgen;
        if (!al->queued) {
            al->queued = 1;
            pthread_mutex_lock(&g_alias_mu);
            if (!g_alias_q) g_alias_q = [NSMutableArray array];
            [g_alias_q addObject:al];
            pthread_mutex_unlock(&g_alias_mu);
        }
    }
    return al;
}

/* Multisampled depth -> multisampled colour, one fragment per sample: the value (float formats), its bits (32-bit integer formats), the 16-bit unorm code
 * (16-bit integer formats of Depth16) or the bytes of the float (RGBA8). */
static NSString *const kAliasMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct VO { float4 pos [[position]]; };\n"
     "vertex VO al_vs(uint vid [[vertex_id]]) { VO o; float2 p = float2((vid << 1) & 2, vid & 2); o.pos = float4(p * 2.0 - 1.0, 0, 1); return o; }\n"
     "struct AP { uint layer, mode; };\n" /* mode 0 bits, 1 unorm16 code, 2 unpack RGBA8 unorm, 3 RGBA8 bytes as uint */
     "static uint al_bits(float v, uint mode) { return mode == 1 ? uint(rint(saturate(v) * 65535.0)) : as_type<uint>(v); }\n"
     "fragment float4 al_f(VO i [[stage_in]], depth2d_ms<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  float v = d.read(uint2(i.pos.xy), s); return p.mode == 2 ? unpack_unorm4x8_to_float(as_type<uint>(v)) : float4(v, 0, 0, 1); }\n"
     "fragment uint4 al_u(VO i [[stage_in]], depth2d_ms<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  uint b = al_bits(d.read(uint2(i.pos.xy), s), p.mode); return p.mode == 3 ? uint4(b & 255u, (b >> 8) & 255u, (b >> 16) & 255u, b >> 24) : uint4(b, 0, 0, 1); }\n"
     "fragment int4 al_i(VO i [[stage_in]], depth2d_ms<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  return int4(as_type<int>(al_bits(d.read(uint2(i.pos.xy), s), p.mode)), 0, 0, 1); }\n"
     "fragment float4 al_fa(VO i [[stage_in]], depth2d_ms_array<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  float v = d.read(uint2(i.pos.xy), p.layer, s); return p.mode == 2 ? unpack_unorm4x8_to_float(as_type<uint>(v)) : float4(v, 0, 0, 1); }\n"
     "fragment uint4 al_ua(VO i [[stage_in]], depth2d_ms_array<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  uint b = al_bits(d.read(uint2(i.pos.xy), p.layer, s), p.mode); return p.mode == 3 ? uint4(b & 255u, (b >> 8) & 255u, (b >> 16) & 255u, b >> 24) : uint4(b, 0, 0, 1); }\n"
     "fragment int4 al_ia(VO i [[stage_in]], depth2d_ms_array<float> d [[texture(0)]], constant AP &p [[buffer(0)]], uint s [[sample_id]]) {\n"
     "  return int4(as_type<int>(al_bits(d.read(uint2(i.pos.xy), p.layer, s), p.mode)), 0, 0, 1); }\n";
static id<MTLRenderPipelineState> alias_ms_pso(MTLPixelFormat dst, int array, uint32_t samples) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *cache;
    static id<MTLLibrary> lib;
    NSNumber *k = @((uint64_t)dst | (uint64_t)array << 16 | (uint64_t)samples << 20);
    id<MTLRenderPipelineState> ps = cache[k];
    if (ps) return ps;
    NSError *err = nil;
    if (!lib) lib = [g_mtl.dev newLibraryWithSource:kAliasMSL options:nil error:&err];
    if (!lib) {
        LOGE("copy-backed view shader: %s", err.localizedDescription.UTF8String);
        return nil;
    }
    int ik = pf_int_kind(dst);
    NSString *fn = [NSString stringWithFormat:@"al_%s%s", ik == 1 ? "i" : ik == 2 ? "u" : "f", array ? "a" : ""];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"al_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:fn];
    pd.colorAttachments[0].pixelFormat = dst;
    pd.rasterSampleCount = samples;
    ps = [g_mtl.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) {
        LOGE("copy-backed view pipeline (format %lu): %s", (unsigned long)dst, err.localizedDescription.UTF8String);
        return nil;
    }
    if (!cache) cache = [NSMutableDictionary dictionary];
    cache[k] = ps;
    return ps;
}
static void alias_copy_ms(id<MTLCommandBuffer> cb, GfxAlias *al) {
    MTLPixelFormat dst = al->tex.pixelFormat;
    int array = al->src.textureType == MTLTextureType2DMultisampleArray;
    id<MTLRenderPipelineState> ps = alias_ms_pso(dst, array, (uint32_t)al->src.sampleCount);
    if (!ps) return;
    uint32_t mode = al->src.pixelFormat == MTLPixelFormatDepth16Unorm && pf_int_kind(dst) ? 1
                    : dst == MTLPixelFormatRGBA8Unorm                                    ? 2
                    : dst == MTLPixelFormatRGBA8Uint                                     ? 3
                                                                                         : 0;
    for (uint32_t j = 0; j < al->ns; j++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = al->tex;
        rp.colorAttachments[0].slice = al->tex.textureType == MTLTextureType2DMultisampleArray ? j : 0;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        uint32_t p[2] = {al->s0 + j, mode};
        [e setRenderPipelineState:ps];
        [e setFragmentTexture:al->src atIndex:0];
        [e setFragmentBytes:p length:sizeof p atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
}
/* AK_BYTES: source -> staging buffer -> alias, level by level and slice by slice (the bytes are reinterpreted, nothing converted).
 * AK_TYPE: same format, 3D depth slices <-> 2D (array) slices, texture to texture. */
static void alias_copy_blit(id<MTLCommandBuffer> cb, GfxAlias *al) {
    id<MTLTexture> s = al->src, d = al->tex;
    int cls;
    uint32_t bpb = pf_bytes(s.pixelFormat, &cls);
    MTLBlitOption opt = s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
    int s3d = s.textureType == MTLTextureType3D, d3d = d.textureType == MTLTextureType3D;
    int sbc = pf_is_bc(s.pixelFormat), dbc = pf_is_bc(d.pixelFormat);
    /* staging layout: per level, per slice, rows of source blocks; offsets 256-aligned */
    uint64_t need = 0;
    if (al->kind == AK_BYTES)
        for (uint32_t i = 0; i < al->nl; i++) {
            NSUInteger w = MAX(1, s.width >> (al->l0 + i)), h = MAX(1, s.height >> (al->l0 + i));
            if (sbc) { w = (w + 3) / 4; h = (h + 3) / 4; }
            need += (((uint64_t)w * h * bpb + 255) & ~255ull) * al->ns;
        }
    if (al->kind == AK_BYTES && (!al->scratch || al->scratch.length < need)) {
        al->scratch = [g_mtl.dev newBufferWithLength:need ? need : 256 options:MTLResourceStorageModePrivate];
        if (!al->scratch) return;
    }
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    uint64_t off = 0;
    for (uint32_t i = 0; i < al->nl; i++) {
        uint32_t slv = al->l0 + i;
        NSUInteger sw = MAX(1, s.width >> slv), sh = MAX(1, s.height >> slv), sdep = s3d ? MAX(1, s.depth >> slv) : 1;
        NSUInteger dw = MAX(1, d.width >> i), dh = MAX(1, d.height >> i);
        for (uint32_t j = 0; j < al->ns; j++) {
            uint32_t ss = al->s0 + j;
            if (s3d && ss >= sdep) break;
            NSUInteger sslice = s3d ? 0 : ss, sz = s3d ? ss : 0, dslice = d3d ? 0 : j, dz = d3d ? j : 0;
            if (d3d && j >= MAX(1, d.depth >> i)) break;
            if (al->kind == AK_TYPE) {
                NSUInteger cw = MIN(sw, dw), chh = MIN(sh, dh);
                [be copyFromTexture:s sourceSlice:sslice sourceLevel:slv sourceOrigin:MTLOriginMake(0, 0, sz) sourceSize:MTLSizeMake(cw, chh, 1)
                          toTexture:d destinationSlice:dslice destinationLevel:i destinationOrigin:MTLOriginMake(0, 0, dz)];
                continue;
            }
            NSUInteger bw = sbc ? (sw + 3) / 4 : sw, bh = sbc ? (sh + 3) / 4 : sh, row = bw * bpb, img = row * bh;
            [be copyFromTexture:s sourceSlice:sslice sourceLevel:slv sourceOrigin:MTLOriginMake(0, 0, sz) sourceSize:MTLSizeMake(sw, sh, 1)
                         toBuffer:al->scratch destinationOffset:off destinationBytesPerRow:row destinationBytesPerImage:img options:opt];
            /* the same blocks in the alias' texels: one texel per block (BC source), 4x4 texels per block (BC alias), else 1:1 */
            NSUInteger cw = sbc && !dbc ? bw : (!sbc && dbc ? MIN(bw * 4, dw) : MIN(sw, dw)), chh = sbc && !dbc ? bh : (!sbc && dbc ? MIN(bh * 4, dh) : MIN(sh, dh));
            cw = MIN(cw, dw);
            chh = MIN(chh, dh);
            [be copyFromBuffer:al->scratch sourceOffset:off sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:MTLSizeMake(cw, chh, 1)
                     toTexture:d destinationSlice:dslice destinationLevel:i destinationOrigin:MTLOriginMake(0, 0, dz)];
            off += (img + 255) & ~255ull;
        }
    }
    [be endEncoding];
}
void mtl_tex_alias_flush(id<MTLCommandBuffer> cb) {
    pthread_mutex_lock(&g_alias_mu);
    NSArray<GfxAlias *> *list = [g_alias_q copy];
    [g_alias_q removeAllObjects];
    pthread_mutex_unlock(&g_alias_mu);
    static uint64_t n;
    uint64_t prev = n;
    n += list.count;
    if (list.count && (!prev || __builtin_clzll(prev) != __builtin_clzll(n))) LOGI("copy-backed texture views: %llu refreshes", (unsigned long long)n); /* each power of two */
    for (GfxAlias *al in list) {
        al->queued = 0;
        al->filled = 1;
        GfxTex *o = al->owner;
        if (o) al->gen = o->wgen; /* written again since it was queued (the pass split to make the copy closed it): the copy is current now */
        if (al->kind == AK_MS_DEPTH) alias_copy_ms(cb, al);
        else alias_copy_blit(cb, al);
    }
}
/* The alias texture seen through the requested format / type / swizzle (a view of the copy when they differ from how it was allocated). */
static id<MTLTexture> alias_view(GfxAlias *al, MTLPixelFormat pf, MTLTextureType type, MTLTextureSwizzleChannels sw, uint64_t key) {
    int identity = sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
    if (identity && pf == al->tex.pixelFormat && type == al->tex.textureType) return al->tex;
    NSNumber *k = @(key);
    id<MTLTexture> v = al->views[k];
    if (v) return v;
    NSUInteger slices = al->tex.textureType == MTLTextureTypeCube ? 6 : al->tex.textureType == MTLTextureTypeCubeArray ? al->tex.arrayLength * 6 : al->tex.arrayLength;
    v = [al->tex newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(0, al->tex.mipmapLevelCount) slices:NSMakeRange(0, MAX(1, slices)) swizzle:sw];
    if (!v) v = al->tex;
    al->views[k] = v;
    return v;
}

id<MTLTexture> mtl_tex_view(GfxTex *t, const uint8_t tswz[4], const view_t *v, int has_view, int stype, int shadow, int ds_mode) {
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
    }
    /* the view's format: a Metal format view, the depth texture itself (R32F of Depth32F reads the same values), a copy (VF_ALIAS) or, when nothing fits, the
     * texture's own format */
    MTLPixelFormat vpf = view_pf(v, has_view);
    int fc = view_fmt_class(&t->key, t->pf, vpf, t->samples);
    if (fc == VF_VIEW || fc == VF_ALIAS) pf = vpf;
    else if (fc == VF_NONE)
        LOG_ONCE(NAPPLE_LOG_INFO, "texture view format %#x (Metal %lu) of a format %#x texture (Metal %lu) is not expressible: sampled in the texture's format",
                 v->format, (unsigned long)vpf, t->key.d.format, (unsigned long)t->pf);
    int dst_cls = 0;
    if (fc == VF_ALIAS) pf_bytes(vpf, &dst_cls);
    int samples_depth = fc == VF_ALIAS ? dst_cls == 1 : t->is_depth;
    /* a shader that samples a single 2D image of an array / cube texture (the game bound the whole texture, no layer range): its first layer */
    if ((want == MTLTextureType2D || want == MTLTextureType1D) && ns > 1) ns = 1;
    if (l0 + nl > t->tex.mipmapLevelCount) { nl = l0 < t->tex.mipmapLevelCount ? t->tex.mipmapLevelCount - l0 : 1; }
    /* swizzle: view swizzle composed with the format component map and the texture-level swizzle */
    uint32_t m = 0;
    uint8_t fswz[4];
    napple_fmt_to_mtl(fc == VF_VIEW || fc == VF_ALIAS ? v->format : t->key.d.format, &m, fswz); /* the component map of the format the texels are read as */
    uint8_t user[4] = {tswz[0], tswz[1], tswz[2], tswz[3]};
    if (has_view && v->set_swizzle) memcpy(user, v->swz, 4);
    uint8_t out[4];
    for (int i = 0; i < 4; i++) out[i] = user[i] < 2 ? user[i] : fswz[user[i] - 2];
    if (samples_depth) {
        out[0] = 2;
        out[1] = 3;
        out[2] = 4;
        out[3] = 5;
    } /* depth views are sampled as one channel */
    /* Stencil mode of a depth-stencil texture (the view's mode overrides the texture's): the stencil plane is read through a stencil-only format view (X32_Stencil8 of a
     * Depth32Float_Stencil8 texture; a Stencil8 texture is its own stencil view). The depth plane is what the combined format already samples as. */
    int stencil_view = t->has_stencil && (has_view && v->set_ds ? v->ds_mode : ds_mode) == 1;
    if (stencil_view) pf = t->pf == MTLPixelFormatStencil8 ? MTLPixelFormatStencil8 : MTLPixelFormatX32_Stencil8;
    /* copy-backed views: a format the storage cannot be viewed as (same bytes per texel, another class), or 2D slices of a 3D texture / a 3D view of a 2D array */
    {
        MTLTextureType have = t->tex.textureType;
        int akind = 0;
        MTLTextureType atype = want;
        MTLPixelFormat apf = pf;
        uint32_t anl = nl ? nl : 1, ans = ns ? ns : 1;
        if (fc == VF_ALIAS && !stencil_view) {
            akind = t->samples > 1 ? AK_MS_DEPTH : AK_BYTES;
            MTLPixelFormat same = t->tex.pixelFormat;
            if (akind == AK_MS_DEPTH) atype = ans > 1 || have == MTLTextureType2DMultisampleArray ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
            else if (!view_valid(t->tex, &same, want, l0, anl, s0, ans) && have != MTLTextureType3D)
                atype = ans == 1 ? MTLTextureType2D : MTLTextureType2DArray;
            if (akind == AK_MS_DEPTH && (want != MTLTextureType2DMultisample && want != MTLTextureType2DMultisampleArray)) akind = 0; /* not sampled as multisampled */
        } else if (t->samples == 1 && !stencil_view && ((have == MTLTextureType3D && (want == MTLTextureType2D || want == MTLTextureType2DArray)) ||
                                                        (have == MTLTextureType2DArray && want == MTLTextureType3D))) {
            akind = AK_TYPE;
            apf = t->tex.pixelFormat;
            if (have == MTLTextureType3D) {
                uint32_t dep = (uint32_t)MAX(1, t->tex.depth >> l0);
                if (s0 >= dep) s0 = dep - 1;
                if (s0 + ans > dep) ans = dep - s0;
                if (want == MTLTextureType2D) ans = 1;
            }
        }
        if (akind) {
            uint64_t akey = (uint64_t)apf | (uint64_t)atype << 12 | (uint64_t)l0 << 16 | (uint64_t)anl << 20 | (uint64_t)s0 << 24 | (uint64_t)ans << 36 |
                            (uint64_t)akind << 60;
            GfxAlias *al = alias_get(t, akey, akind, apf, atype, l0, anl, s0, ans);
            if (al) {
                MTLTextureSwizzleChannels asw =
                    MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)out[0], (MTLTextureSwizzle)out[1], (MTLTextureSwizzle)out[2], (MTLTextureSwizzle)out[3]);
                uint64_t vkey = (uint64_t)pf | (uint64_t)atype << 12 | (uint64_t)(out[0] & 7) << 48 | (uint64_t)(out[1] & 7) << 51 | (uint64_t)(out[2] & 7) << 54 |
                                (uint64_t)(out[3] & 7) << 57;
                return alias_view(al, pf, atype, asw, vkey);
            }
        }
        if (fc == VF_ALIAS) pf = t->pf; /* no copy: the texture's own format */
    }
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
        /* The asked type cannot be viewed (a cube of a non-square or short array, an array of a 3D texture, ...): keep the level and layer RANGE the game chose and view
         * it as the texture's own kind (a 2D array for cube / array sources, 2D for a single layer) instead of throwing the selection away with the whole texture. */
        MTLTextureType have = t->tex.textureType;
        MTLTextureType alt = (have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray) ? (ns == 1 ? MTLTextureType2D : MTLTextureType2DArray) : have;
        if (alt != want && have != MTLTextureType3D && view_valid(t->tex, &pf, alt, l0, nl ? nl : 1, s0, ns ? ns : 1)) {
            LOG_ONCE(NAPPLE_LOG_INFO, "texture view type %lu of a type %lu texture is not expressible: viewed as type %lu over levels %u+%u slices %u+%u",
                     (unsigned long)want, (unsigned long)have, (unsigned long)alt, l0, nl, s0, ns);
            want = alt;
            key = (key & ~(0xfull << 12)) | ((uint64_t)want << 12);
            k = @(key);
            cached = t->views[k];
            if (cached) return cached;
        }
    }
    if (!view_valid(t->tex, &pf, want, l0, nl ? nl : 1, s0, ns ? ns : 1)) {
        LOG_ONCE(NAPPLE_LOG_INFO,
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

/* Surface image view (SULD / SUST / SUATOM, see shader/maxwell.h ShaderImage): the texture as the shader declares it, `kind` IMGKIND_FLOAT / INT / UINT and
 * `type` TEXTYPE_*. An integer kind over a float / normalized texture is a view in the integer format of the same texel size (what the hardware does with a
 * raw .U32 access); an sRGB texture is viewed linear (sRGB views cannot be written). nil when the Metal texture type cannot be viewed as the declared one. */
static MTLPixelFormat image_int_format(MTLPixelFormat pf, int sint) {
    switch (pf) {
        case MTLPixelFormatR8Unorm:
        case MTLPixelFormatR8Snorm:
        case MTLPixelFormatR8Uint:
        case MTLPixelFormatR8Sint: return sint ? MTLPixelFormatR8Sint : MTLPixelFormatR8Uint;
        case MTLPixelFormatRG8Unorm:
        case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatRG8Uint:
        case MTLPixelFormatRG8Sint: return sint ? MTLPixelFormatRG8Sint : MTLPixelFormatRG8Uint;
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Snorm:
        case MTLPixelFormatRGBA8Uint:
        case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB: return sint ? MTLPixelFormatRGBA8Sint : MTLPixelFormatRGBA8Uint;
        case MTLPixelFormatR16Unorm:
        case MTLPixelFormatR16Snorm:
        case MTLPixelFormatR16Float:
        case MTLPixelFormatR16Uint:
        case MTLPixelFormatR16Sint: return sint ? MTLPixelFormatR16Sint : MTLPixelFormatR16Uint;
        case MTLPixelFormatRG16Unorm:
        case MTLPixelFormatRG16Snorm:
        case MTLPixelFormatRG16Float:
        case MTLPixelFormatRG16Uint:
        case MTLPixelFormatRG16Sint: return sint ? MTLPixelFormatRG16Sint : MTLPixelFormatRG16Uint;
        case MTLPixelFormatRGBA16Unorm:
        case MTLPixelFormatRGBA16Snorm:
        case MTLPixelFormatRGBA16Float:
        case MTLPixelFormatRGBA16Uint:
        case MTLPixelFormatRGBA16Sint: return sint ? MTLPixelFormatRGBA16Sint : MTLPixelFormatRGBA16Uint;
        case MTLPixelFormatR32Float:
        case MTLPixelFormatR32Uint:
        case MTLPixelFormatR32Sint: return sint ? MTLPixelFormatR32Sint : MTLPixelFormatR32Uint;
        case MTLPixelFormatRG32Float:
        case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRG32Sint: return sint ? MTLPixelFormatRG32Sint : MTLPixelFormatRG32Uint;
        case MTLPixelFormatRGBA32Float:
        case MTLPixelFormatRGBA32Uint:
        case MTLPixelFormatRGBA32Sint: return sint ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Uint;
        case MTLPixelFormatRGB10A2Unorm:
        case MTLPixelFormatRGB10A2Uint: return MTLPixelFormatRGB10A2Uint;
        default: return MTLPixelFormatInvalid;
    }
}
id<MTLTexture> mtl_tex_image_view(GfxTex *t, int kind, int type, int atomic) {
    (void)atomic;
    static const MTLTextureType tt[] = {MTLTextureType1D,           MTLTextureType2D,        MTLTextureType3D,      MTLTextureTypeCube,
                                        MTLTextureType2DArray,      MTLTextureTypeCubeArray, MTLTextureType1DArray, MTLTextureTypeTextureBuffer,
                                        MTLTextureType2DMultisample};
    MTLTextureType want = tt[type & 7];
    MTLPixelFormat pf = t->tex.pixelFormat;
    if (kind == IMGKIND_INT || kind == IMGKIND_UINT) {
        MTLPixelFormat ip = image_int_format(pf, kind == IMGKIND_INT);
        if (ip == MTLPixelFormatInvalid) {
            LOG_ONCE(NAPPLE_LOG_WARN, "surface image: no integer view of texture format %lu", (unsigned long)pf);
            return nil;
        }
        pf = ip;
    } else if (pf == MTLPixelFormatRGBA8Unorm_sRGB) pf = MTLPixelFormatRGBA8Unorm;
    else if (pf == MTLPixelFormatBGRA8Unorm_sRGB) pf = MTLPixelFormatBGRA8Unorm;
    if (t->is_depth || t->tex.sampleCount > 1) return nil;
    MTLTextureType have = t->tex.textureType;
    if (want == MTLTextureTypeTextureBuffer) {
        if (have != MTLTextureTypeTextureBuffer && have != MTLTextureType1D) return nil;
        want = have;
    }
    int ok = have == want || (want == MTLTextureType2D && (have == MTLTextureType2DArray || have == MTLTextureTypeCube)) ||
             (want == MTLTextureType2DArray && (have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray)) ||
             (want == MTLTextureType1D && have == MTLTextureType1DArray);
    if (!ok) return nil;
    if (pf == t->tex.pixelFormat && want == have) return t->tex;
    uint64_t key = (uint64_t)pf | (uint64_t)want << 12 | 1ull << 62; /* image views: bit 62 (sampled views keep it clear, attachment views use bit 63) */
    NSNumber *k = @(key);
    id<MTLTexture> v = t->views[k];
    if (v) return v;
    if (pf != t->tex.pixelFormat && !(t->tex.usage & MTLTextureUsagePixelFormatView)) return nil;
    v = [t->tex newTextureViewWithPixelFormat:pf
                                  textureType:want
                                       levels:NSMakeRange(0, 1)
                                       slices:NSMakeRange(0, want == have ? (t->tex.arrayLength ? t->tex.arrayLength : 1) : 1)];
    if (!v) {
        LOG_ONCE(NAPPLE_LOG_WARN, "surface image view (format %lu type %lu) could not be created", (unsigned long)pf, (unsigned long)want);
        return nil;
    }
    t->views[k] = v;
    return v;
}

/* Render target attachment: the base texture with level / slice chosen by the view. A view with another format (an sRGB view of a UNORM target,
 * a UINT view of a float one of the same size) renders through a format view of the whole texture; the attachment level / slice still pick the
 * image. The format must be view-compatible (same size per pixel); otherwise the base format is used and that is reported once. */
/* Rendering into a block-compressed texture through an uncompressed view of one block's size (one texel per 4x4 block, e.g. runtime texture compression:
 * Mario Kart 8 renders BC4/BC7 blocks into the shadow-map and cube-map textures): Metal cannot view BC as an integer format and a BC texture is not
 * renderable, so the pass draws into a shadow texture of the block grid (one per BC texture level / slice / view format). When the pass ends the
 * shadow is copied back, block for block, into the compressed texture (mtl_bcrt_flush, called by the replay after the encoder closes). The copy-in
 * (a pass that blends over the old blocks) is not done: compression passes overwrite every block. Written after nxnative's bcrt_attach / bcrt_flush
 * (MIT, copyright seedds, NX:dm/gfx/nvn/nvn_mtl.m; see THIRD_PARTY_NOTICES.md). */
@interface BcRt : NSObject {
  @public
    id<MTLTexture> base, shadow;
    uint32_t lv, sl;
}
@end
@implementation BcRt
@end
static NSMutableArray<BcRt *> *g_bcrt;
static pthread_mutex_t g_bcrt_mu = PTHREAD_MUTEX_INITIALIZER;
static void bcrt_note(id<MTLTexture> base, id<MTLTexture> shadow, uint32_t lv, uint32_t sl) {
    pthread_mutex_lock(&g_bcrt_mu);
    if (!g_bcrt) g_bcrt = [NSMutableArray array];
    for (BcRt *r in g_bcrt)
        if (r->shadow == shadow) {
            pthread_mutex_unlock(&g_bcrt_mu);
            return;
        }
    BcRt *r = [BcRt new];
    r->base = base;
    r->shadow = shadow;
    r->lv = lv;
    r->sl = sl;
    [g_bcrt addObject:r];
    pthread_mutex_unlock(&g_bcrt_mu);
}
uint32_t mtl_bcrt_pending(void) { return g_bcrt.count; }
static NSUInteger bc_block_bytes(MTLPixelFormat f) {
    if (f < MTLPixelFormatBC1_RGBA || f > MTLPixelFormatBC7_RGBAUnorm_sRGB) return 0;
    return f <= MTLPixelFormatBC1_RGBA_sRGB || f == MTLPixelFormatBC4_RUnorm || f == MTLPixelFormatBC4_RSnorm ? 8 : 16;
}
/* Blit every pending shadow back into its compressed texture. No encoder may be open on cb. */
void mtl_bcrt_flush(id<MTLCommandBuffer> cb) {
    pthread_mutex_lock(&g_bcrt_mu);
    NSArray<BcRt *> *list = [g_bcrt copy];
    [g_bcrt removeAllObjects];
    pthread_mutex_unlock(&g_bcrt_mu);
    if (!list.count) return;
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    for (BcRt *r in list) {
        NSUInteger bb = bc_block_bytes(r->base.pixelFormat), bw = r->shadow.width, bh = r->shadow.height;
        if (!bb) continue;
        id<MTLBuffer> buf = [g_mtl.dev newBufferWithLength:bw * bh * bb options:MTLResourceStorageModePrivate];
        [be copyFromTexture:r->shadow
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(bw, bh, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:bw * bb
            destinationBytesPerImage:bw * bh * bb];
        NSUInteger lw = MAX(1, r->base.width >> r->lv), lh = MAX(1, r->base.height >> r->lv);
        NSUInteger cw = MIN(bw * 4, lw),
                   ch = MIN(bh * 4, lh); /* a level smaller than a block: the copy reaches the level's edge (Metal rejects a size beyond the level) */
        int is3d = r->base.textureType == MTLTextureType3D;
        [be copyFromBuffer:buf
                   sourceOffset:0
              sourceBytesPerRow:bw * bb
            sourceBytesPerImage:bw * bh * bb
                     sourceSize:MTLSizeMake(cw, ch, 1)
                      toTexture:r->base
               destinationSlice:is3d ? 0 : r->sl
               destinationLevel:r->lv
              destinationOrigin:MTLOriginMake(0, 0, is3d ? r->sl : 0)];
    }
    [be endEncoding];
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
    MTLPixelFormat sp = t->tex.pixelFormat;
    int src_bc = sp >= MTLPixelFormatBC1_RGBA && sp <= MTLPixelFormatBC7_RGBAUnorm_sRGB,
        dst_bc = pf >= MTLPixelFormatBC1_RGBA && pf <= MTLPixelFormatBC7_RGBAUnorm_sRGB;
    uint32_t lv = *level, sl = *slice;
    NSNumber *k =
        @((uint64_t)pf | 1ull << 63 |
          (src_bc && !dst_bc ? ((uint64_t)(lv & 0xff) << 32 | (uint64_t)(sl & 0xfff) << 40) : 0)); /* not a sampled-view key (those keep bit 63 clear) */
    id<MTLTexture> cached = t->views[k];
    if (cached) {
        if (src_bc && !dst_bc) {
            bcrt_note(t->tex, cached, lv, sl);
            *level = 0;
            *slice = 0;
        }
        return cached;
    }
    int bw, bh, bv = napple_fmt_bpb(v->format, &bw, &bh), bt = napple_fmt_bpb(t->key.d.format, &bw, &bh);
    id<MTLTexture> nv = nil;
    if (src_bc && !dst_bc) {
        NSUInteger lw = MAX(1, t->tex.width >> lv), lh = MAX(1, t->tex.height >> lv);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                      width:MAX(1, (lw + 3) / 4)
                                                                                     height:MAX(1, (lh + 3) / 4)
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        nv = [g_mtl.dev newTextureWithDescriptor:td];
        LOG_ONCE(NAPPLE_LOG_INFO, "rendering into block-compressed textures (view %#x of format %#x) through block-grid shadows copied back after each pass",
                 v->format, t->key.d.format);
        t->views[k] = nv;
        if (nv) {
            bcrt_note(t->tex, nv, lv, sl);
            *level = 0;
            *slice = 0;
        }
        return nv ? nv : t->tex;
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
