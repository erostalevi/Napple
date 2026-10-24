/* api_texture.c - NVN texture builders, textures, views, samplers and descriptor pools. */
#include <stdlib.h>

#include "gfx_mtl.h"
#include "tex/napple_deswizzle.h"
#include "hostobj.h"
#include "timeline.h"

void gfx_api_link_texture(void) {}

#define TEX_MAGIC MAGIC('T', 'E', 'X', '0')
#define SAMP_MAGIC MAGIC('S', 'M', 'P', '0')
#define DPOOL_MAGIC MAGIC('D', 'P', 'L', '0')
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')

/* ---------------------------------------------------------------------------------------------------------------
 * Texture builder
 * --------------------------------------------------------------------------------------------------------------- */
#define TB(b) ((tex_builder_t *)(b))
#define CTB(b) ((const tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev;
    memset(b, 0, sizeof(tex_builder_t));
    TB(b)->dev = dev;
    TB(b)->d.swizzle_r = NVN_TEXTURE_SWIZZLE_R; TB(b)->d.swizzle_g = NVN_TEXTURE_SWIZZLE_G;
    TB(b)->d.swizzle_b = NVN_TEXTURE_SWIZZLE_B; TB(b)->d.swizzle_a = NVN_TEXTURE_SWIZZLE_A;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D;
    TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = (uint32_t)v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = (uint32_t)v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = (uint32_t)v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = (uint32_t)w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = (uint32_t)w; TB(b)->d.height = (uint32_t)h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = (uint32_t)w; TB(b)->d.height = (uint32_t)h; TB(b)->d.depth = (uint32_t)d; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = (uint32_t)v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = (int)f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    TB(b)->d.swizzle_r = r & 7; TB(b)->d.swizzle_g = g & 7; TB(b)->d.swizzle_b = bl & 7; TB(b)->d.swizzle_a = a & 7;
}
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { TB(b)->ds_mode = (uint8_t)m; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL;
    if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;       /* byte 0 of a packaged layout is log2 of the block height */
}
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureBuilderSetGLTextureName, void, NVNtextureBuilder *b, uint32_t n) { (void)b; (void)n; }
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)CTB(b)->d.flags; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)CTB(b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)CTB(b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return CTB(b)->d.samples; }
NVN(nvnTextureBuilderGetSwizzle, void, const NVNtextureBuilder *b, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *bl, NVNtextureSwizzle *a) {
    *r = (NVNtextureSwizzle)CTB(b)->d.swizzle_r; *g = (NVNtextureSwizzle)CTB(b)->d.swizzle_g;
    *bl = (NVNtextureSwizzle)CTB(b)->d.swizzle_b; *a = (NVNtextureSwizzle)CTB(b)->d.swizzle_a;
}
NVN(nvnTextureBuilderGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtextureBuilder *b) { return (NVNtextureDepthStencilMode)CTB(b)->ds_mode; }
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *b) { return CTB(b)->pkg_data; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)CTB(b)->d.stride; }
NVN(nvnTextureBuilderGetGLTextureName, uint32_t, const NVNtextureBuilder *b) { (void)b; return 0; }
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { (void)b; return 0; }
NVN(nvnTextureBuilderGetSparseTileLayout, void, const NVNtextureBuilder *b, NVNtextureSparseTileLayout *l) { (void)b; (void)l; }
NVN_NOPROTO(nvnTextureBuilderGetDevice, const NVNdevice *, const NVNtextureBuilder *b) { return CTB(b)->dev; }
NVN_NOPROTO(nvnTextureBuilderGetMemoryPool, const NVNmemoryPool *, const NVNtextureBuilder *b) { return (const NVNmemoryPool *)CTB(b)->pool; }
NVN_NOPROTO(nvnTextureBuilderGetMemoryOffset, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)CTB(b)->off; }
NVN_NOPROTO(nvnTextureBuilderGetPackagedTextureLayout, const NVNpackagedTextureLayout *, const NVNtextureBuilder *b) { return CTB(b)->has_layout ? (const NVNpackagedTextureLayout *)CTB(b)->layout : NULL; }

/* Storage classes are opaque compatibility identifiers: encoded storage always lives in guest memory and is decoded per descriptor. */
static NVNstorageClass storage_class(const napple_texdesc *d) { return d->target == NVN_TEXTURE_TARGET_BUFFER ? 0 : 1; }
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { return storage_class(&CTB(b)->d); }
/* The raw class is the hardware memory kind behind the storage class; this driver has one kind per storage class (no compression), so they agree. */
NVN_NOPROTO(nvnTextureBuilderGetRawStorageClass, int, const NVNtextureBuilder *b) { return (int)storage_class(&CTB(b)->d); }

NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    napple_texlayout L;
    napple_tex_layout(&CTB(b)->d, &L);
    if (!L.bpb) LOG_ONCE(NAPPLE_LOG_WARN, "TextureBuilderGetStorageSize: unknown format %#x", CTB(b)->d.format);
    return (size_t)L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    napple_texlayout L;
    napple_tex_layout(&CTB(b)->d, &L);
    return L.align;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Texture objects
 * --------------------------------------------------------------------------------------------------------------- */
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))

NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = CTB(b);
    tex_t *o = TX(t);
    memset(o, 0, sizeof *o);
    napple_texlayout L;
    napple_tex_layout(&s->d, &L);
    if (!L.bpb) { LOGE("TextureInitialize: unsupported format %#x", s->d.format); return 0; }
    if (!s->pool || s->pool->magic != POOL_MAGIC || s->off < 0 || (uint64_t)s->off + L.size > s->pool->size) {
        LOGE("TextureInitialize: storage out of range (pool %p off %lld size %llu)", (void *)s->pool, (long long)s->off, (unsigned long long)L.size);
        return 0;
    }
    o->magic = TEX_MAGIC;
    o->s.d = s->d;
    o->s.mem = s->pool->mem + s->off;
    o->s.size = L.size;
    o->s.swz[0] = (uint8_t)s->d.swizzle_r; o->s.swz[1] = (uint8_t)s->d.swizzle_g; o->s.swz[2] = (uint8_t)s->d.swizzle_b; o->s.swz[3] = (uint8_t)s->d.swizzle_a;
    o->s.ds_mode = s->ds_mode;
    o->s.valid = 1;
    o->pool = s->pool; o->off = s->off;
    if (s->pkg_data) {
        napple_gfx_guest_write_prepare(o->s.mem, (size_t)L.size);
        memcpy(o->s.mem, s->pkg_data, (size_t)L.size);
    }
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { memset(t, 0, sizeof(tex_t)); }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *l) { (void)t; (void)l; }
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { return storage_class(&CTX(t)->s.d); }
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { return (int)storage_class(&CTX(t)->s.d); }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->s.d.flags; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->s.d.target; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->s.d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->s.d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->s.d.depth; }
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->s.d.levels; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->s.d.format; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->s.d.samples; }
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const uint8_t *z = CTX(t)->s.swz;
    *r = (NVNtextureSwizzle)z[0]; *g = (NVNtextureSwizzle)z[1]; *b = (NVNtextureSwizzle)z[2]; *a = (NVNtextureSwizzle)z[3];
}
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return (NVNtextureDepthStencilMode)CTX(t)->s.ds_mode; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)napple_tex_linear_stride(&CTX(t)->s.d); }
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) { return (NVNtextureAddress)(uintptr_t)CTX(t)->s.mem; }
NVN(nvnTextureGetSparseTileLayout, void, const NVNtexture *t, NVNtextureSparseTileLayout *l) { (void)t; (void)l; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->s.size; }
NVN(nvnTextureGetDebugID, uint64_t, const NVNtexture *t) { return (uint64_t)(uintptr_t)CTX(t)->s.mem; }
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return x->s.mem == y->s.mem && !memcmp(&x->s.d, &y->s.d, sizeof x->s.d);
}
NVN_NOPROTO(nvnTextureGetMemoryPool, const NVNmemoryPool *, const NVNtexture *t) { return (const NVNmemoryPool *)CTX(t)->pool; }
NVN_NOPROTO(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }

/* ---------------------------------------------------------------------------------------------------------------
 * Texture views
 * --------------------------------------------------------------------------------------------------------------- */
#define VW(v) ((view_t *)(v))
#define CVW(v) ((const view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = (uint8_t)base; VW(v)->nlevels = (uint8_t)n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = (uint16_t)min; VW(v)->nlayers = (uint16_t)n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = (uint16_t)f; }
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle b, NVNtextureSwizzle a) {
    VW(v)->set_swizzle = 1; VW(v)->swz[0] = (uint8_t)r; VW(v)->swz[1] = (uint8_t)g; VW(v)->swz[2] = (uint8_t)b; VW(v)->swz[3] = (uint8_t)a;
}
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_ds = 1; VW(v)->ds_mode = (uint8_t)m; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = (uint8_t)t; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { *base = CVW(v)->base_level; *n = CVW(v)->nlevels; return CVW(v)->set_levels; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) { *min = CVW(v)->min_layer; *n = CVW(v)->nlayers; return CVW(v)->set_layers; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { *f = (NVNformat)CVW(v)->format; return CVW(v)->set_format; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    *r = (NVNtextureSwizzle)CVW(v)->swz[0]; *g = (NVNtextureSwizzle)CVW(v)->swz[1]; *b = (NVNtextureSwizzle)CVW(v)->swz[2]; *a = (NVNtextureSwizzle)CVW(v)->swz[3];
    return CVW(v)->set_swizzle;
}
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) { *m = (NVNtextureDepthStencilMode)CVW(v)->ds_mode; return CVW(v)->set_ds; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { *t = (NVNtextureTarget)CVW(v)->target; return CVW(v)->set_target; }
NVN(nvnTextureViewCompare, NVNboolean, const NVNtextureView *a, const NVNtextureView *b) { return !memcmp(a, b, sizeof(view_t)); }
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t);
    const view_t *w = CVW(v);
    if (!w) return 0;
    napple_texlayout L;
    napple_tex_layout(&o->s.d, &L);
    uint64_t off = w->set_levels ? napple_tex_level_offset(&o->s.d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    return (ptrdiff_t)off;
}

/* ---------------------------------------------------------------------------------------------------------------
 * CPU texel access (storage is block-linear: convert)
 * --------------------------------------------------------------------------------------------------------------- */
static void texel_args(const tex_t *o, const view_t *v, unsigned *level, unsigned *layer) {
    (void)o;
    *level = v && v->set_levels ? v->base_level : 0;
    *layer = v && v->set_layers ? v->min_layer : 0;
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) {
    unsigned lv, ly; texel_args(CTX(t), CVW(v), &lv, &ly);
    tex_write_texels(&CTX(t)->s, lv, ly, (const int32_t *)r, src, 0, 0);
}
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    unsigned lv, ly; texel_args(CTX(t), CVW(v), &lv, &ly);
    tex_write_texels(&CTX(t)->s, lv, ly, (const int32_t *)r, src, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    unsigned lv, ly; texel_args(CTX(t), CVW(v), &lv, &ly);
    gfx_mtl_finish();       /* the GPU may still be rendering into this texture */
    tex_read_texels(&CTX(t)->s, lv, ly, (const int32_t *)r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    unsigned lv, ly; texel_args(CTX(t), CVW(v), &lv, &ly);
    gfx_mtl_finish();
    tex_read_texels(&CTX(t)->s, lv, ly, (const int32_t *)r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

/* ---------------------------------------------------------------------------------------------------------------
 * Samplers
 * --------------------------------------------------------------------------------------------------------------- */
#define SB(b) ((samp_builder_t *)(b))
#define CSB(b) ((const samp_builder_t *)(b))
#define SP(s) ((samp_t *)(s))
#define CSP(s) ((const samp_t *)(s))

NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { SB(b)->dev = d; }
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *d = SB(b)->dev;
    memset(b, 0, sizeof(samp_builder_t));
    SB(b)->dev = d;
    samp_par *p = &SB(b)->p;
    p->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; p->magf = NVN_MAG_FILTER_LINEAR;
    p->ws = p->wt = p->wr = NVN_WRAP_MODE_REPEAT;
    p->cmp_mode = NVN_COMPARE_MODE_NONE; p->cmp_func = NVN_COMPARE_FUNC_LESS;
    p->reduction = NVN_SAMPLER_REDUCTION_AVERAGE;
    p->lod_min = 0.0f; p->lod_max = 1000.0f; p->aniso = 1.0f;
}
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mn, NVNmagFilter mg) { SB(b)->p.minf = (uint8_t)mn; SB(b)->p.magf = (uint8_t)mg; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->p.ws = (uint8_t)s; SB(b)->p.wt = (uint8_t)t; SB(b)->p.wr = (uint8_t)r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float mn, float mx) { SB(b)->p.lod_min = mn; SB(b)->p.lod_max = mx; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->p.lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->p.cmp_mode = (uint8_t)m; SB(b)->p.cmp_func = (uint8_t)f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->p.border, c, 16); }
NVN(nvnSamplerBuilderSetBorderColori, void, NVNsamplerBuilder *b, const int *c) { for (int i = 0; i < 4; i++) SB(b)->p.border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetBorderColorui, void, NVNsamplerBuilder *b, const uint32_t *c) { for (int i = 0; i < 4; i++) SB(b)->p.border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->p.aniso = v; }
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { SB(b)->p.reduction = (uint8_t)r; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { SB(b)->p.lod_snap = v; }
NVN_NOPROTO(nvnSamplerBuilderGetDevice, const NVNdevice *, const NVNsamplerBuilder *b) { return CSB(b)->dev; }
#define SAMPLER_GETTERS(PFX, T, P)                                                                                        \
    NVN(nvn##PFX##GetMinMagFilter, void, const T *b, NVNminFilter *mn, NVNmagFilter *mg) { *mn = (NVNminFilter)P(b)->p.minf; *mg = (NVNmagFilter)P(b)->p.magf; } \
    NVN(nvn##PFX##GetWrapMode, void, const T *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) { *s = (NVNwrapMode)P(b)->p.ws; *t = (NVNwrapMode)P(b)->p.wt; *r = (NVNwrapMode)P(b)->p.wr; } \
    NVN(nvn##PFX##GetLodClamp, void, const T *b, float *mn, float *mx) { *mn = P(b)->p.lod_min; *mx = P(b)->p.lod_max; }  \
    NVN(nvn##PFX##GetLodBias, float, const T *b) { return P(b)->p.lod_bias; }                                              \
    NVN(nvn##PFX##GetCompare, void, const T *b, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)P(b)->p.cmp_mode; *f = (NVNcompareFunc)P(b)->p.cmp_func; } \
    NVN(nvn##PFX##GetBorderColor, void, const T *b, float *c) { memcpy(c, P(b)->p.border, 16); }                           \
    NVN(nvn##PFX##GetBorderColori, void, const T *b, int *c) { for (int i = 0; i < 4; i++) c[i] = (int)P(b)->p.border[i]; } \
    NVN(nvn##PFX##GetBorderColorui, void, const T *b, uint32_t *c) { for (int i = 0; i < 4; i++) c[i] = (uint32_t)P(b)->p.border[i]; } \
    NVN(nvn##PFX##GetMaxAnisotropy, float, const T *b) { return P(b)->p.aniso; }                                           \
    NVN(nvn##PFX##GetReductionFilter, NVNsamplerReduction, const T *b) { return (NVNsamplerReduction)P(b)->p.reduction; }
SAMPLER_GETTERS(SamplerBuilder, NVNsamplerBuilder, CSB)
SAMPLER_GETTERS(Sampler, NVNsampler, CSP)
NVN(nvnSamplerBuilderGetLodSnap, float, const NVNsamplerBuilder *b) { return CSB(b)->p.lod_snap; }
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) { SP(s)->magic = SAMP_MAGIC; SP(s)->p = CSB(b)->p; return 1; }
NVN(nvnSamplerFinalize, void, NVNsampler *s) { SP(s)->magic = 0; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) { return !memcmp(&CSP(a)->p, &CSP(b)->p, sizeof(samp_par)); }
NVN(nvnSamplerGetDebugID, uint64_t, const NVNsampler *s) { return (uint64_t)(uintptr_t)s; }

/* ---------------------------------------------------------------------------------------------------------------
 * Descriptor pools: host tables of texture snapshots / sampler parameters indexed by descriptor id
 * --------------------------------------------------------------------------------------------------------------- */
#define DP(p) ((dpool_t *)(p))
#define CDP(p) ((const dpool_t *)(p))

static gdpool *dpool_host(const dpool_t *p) { return p->magic == DPOOL_MAGIC ? (gdpool *)htab_get(p->handle) : NULL; }
static void dpool_free(void *g) {
    gdpool *h = g;
    free(h->tex); free(h->samp); free(h->samp_used); free(h);
}
static int dpool_init(dpool_t *o, const NVNmemoryPool *mp, ptrdiff_t off, int n, int is_sampler) {
    const pool_t *pool = (const pool_t *)mp;
    memset(o, 0, sizeof *o);
    if (n <= 0 || n > (1 << 20)) { LOGE("descriptor pool with %d entries rejected", n); return 0; }
    gdpool *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->magic = DPOOL_MAGIC; h->is_sampler = is_sampler; h->n = n;
    if (is_sampler) { h->samp = calloc((size_t)n, sizeof(samp_par)); h->samp_used = calloc((size_t)n, 1); if (!h->samp || !h->samp_used) { dpool_free(h); return 0; } }
    else { h->tex = calloc((size_t)n, sizeof(dtex)); if (!h->tex) { dpool_free(h); return 0; } }
    o->magic = DPOOL_MAGIC; o->n = n; o->pool = pool; o->off = off;
    o->handle = htab_alloc(h);
    return 1;
}
static void dpool_fini(dpool_t *o) {
    gdpool *h = dpool_host(o);
    if (h) { htab_free(o->handle); gfx_retire(dpool_free, h); }
    memset(o, 0, sizeof *o);
}

NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *p, const NVNmemoryPool *mp, ptrdiff_t off, int n) { return dpool_init(DP(p), mp, off, n, 0); }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *l) { (void)p; (void)l; }
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *p) { dpool_fini(DP(p)); }
static void register_tex(const dpool_t *p, int id, const NVNtexture *t, const NVNtextureView *v) {
    gdpool *h = dpool_host(p);
    if (!h || id < 0 || id >= h->n || !t || CTX(t)->magic != TEX_MAGIC) return;
    dtex *e = &h->tex[id];
    e->s = CTX(t)->s;
    if (v) e->view = *CVW(v); else memset(&e->view, 0, sizeof e->view);
}
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *p, int id, const NVNtexture *t, const NVNtextureView *v) { register_tex(CDP(p), id, t, v); }
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *p, int id, const NVNtexture *t, const NVNtextureView *v) { register_tex(CDP(p), id, t, v); }
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool *, const NVNtexturePool *p) { return (const NVNmemoryPool *)CDP(p)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool *p) { return (ptrdiff_t)CDP(p)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool *p) { return CDP(p)->n; }

NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *p, const NVNmemoryPool *mp, ptrdiff_t off, int n) { return dpool_init(DP(p), mp, off, n, 1); }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *l) { (void)p; (void)l; }
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *p) { dpool_fini(DP(p)); }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *p, int id, const NVNsampler *s) {
    gdpool *h = dpool_host(CDP(p));
    if (!h || id < 0 || id >= h->n || CSP(s)->magic != SAMP_MAGIC) return;
    h->samp[id] = CSP(s)->p; h->samp_used[id] = 1;
}
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *p, int id, const NVNsamplerBuilder *b) {
    gdpool *h = dpool_host(CDP(p));
    if (!h || id < 0 || id >= h->n) return;
    h->samp[id] = CSB(b)->p; h->samp_used[id] = 1;
}
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool *, const NVNsamplerPool *p) { return (const NVNmemoryPool *)CDP(p)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool *p) { return (ptrdiff_t)CDP(p)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool *p) { return CDP(p)->n; }

/* Resolve a pool handle for the replay layer (replay thread). */
gdpool *gfx_dpool_from_handle(uint64_t h) { return (gdpool *)htab_get(h); }
uint64_t gfx_dpool_handle(const void *guest_pool) { return CDP(guest_pool)->magic == DPOOL_MAGIC ? CDP(guest_pool)->handle : 0; }

/* ---------------------------------------------------------------------------------------------------------------
 * Host-side texture writes (video decoders): the host is the "GPU engine" that fills a texture the game created.
 * --------------------------------------------------------------------------------------------------------------- */
int napple_gfx_texture_info(void *nvn_texture, int *format, uint32_t *width, uint32_t *height) {
    const tex_t *t = (const tex_t *)nvn_texture;
    if (!t || t->magic != TEX_MAGIC || !t->s.valid) return -1;
    if (format) *format = t->s.d.format;
    if (width) *width = t->s.d.width;
    if (height) *height = t->s.d.height;
    return 0;
}
/* RGBA8 pixels (R,G,B,A byte order, tightly packed with `src_pitch`) -> level 0 of the texture, converted to its 8-bit-per-channel format
 * (RGBA8 / RGBX8 / BGRA8 / BGRX8, with or without sRGB tag: the bytes are written as given). The image is clipped to the texture size. */
int napple_gfx_texture_write_rgba(void *nvn_texture, const void *rgba, size_t src_pitch, uint32_t width, uint32_t height) {
    tex_t *t = (tex_t *)nvn_texture;
    if (!t || t->magic != TEX_MAGIC || !t->s.valid || !rgba) return -1;
    const napple_texdesc *d = &t->s.d;
    int bgr;
    switch (d->format) {
    case NVN_FORMAT_RGBA8: case NVN_FORMAT_RGBA8_SRGB: case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8_SRGB: bgr = 0; break;
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRA8_SRGB: case NVN_FORMAT_BGRX8_SRGB: bgr = 1; break;
    default: LOG_ONCE(NAPPLE_LOG_WARN, "texture write: format %#x is not an 8-bit RGBA layout", d->format); return -2;
    }
    uint32_t w = width < d->width ? width : d->width, h = height < d->height ? height : d->height;
    napple_texlayout L; napple_tex_layout(d, &L);
    if (L.bpb != 4 || !w || !h) return -2;
    uint32_t dst_pitch = w * 4;
    uint8_t *lin = malloc((size_t)d->width * d->height * 4);
    if (!lin) return -3;
    memset(lin, 0, (size_t)d->width * d->height * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = (const uint8_t *)rgba + (size_t)y * src_pitch; uint8_t *o = lin + (size_t)y * d->width * 4;
        if (!bgr) memcpy(o, s, dst_pitch);
        else for (uint32_t x = 0; x < w; x++) { o[4 * x] = s[4 * x + 2]; o[4 * x + 1] = s[4 * x + 1]; o[4 * x + 2] = s[4 * x]; o[4 * x + 3] = s[4 * x + 3]; }
    }
    napple_gfx_guest_write_prepare(t->s.mem, (size_t)t->s.size);
    int rc = 0;
    if (napple_tex_is_linear(d)) {
        uint32_t stride = napple_tex_linear_stride(d);
        for (uint32_t y = 0; y < d->height; y++) memcpy(t->s.mem + (size_t)y * stride, lin + (size_t)y * d->width * 4, (size_t)d->width * 4);
    } else rc = napple_swizzle(d, 0, 0, lin, (size_t)d->width * 4, t->s.mem);
    free(lin);
    return rc ? -4 : 0;
}
