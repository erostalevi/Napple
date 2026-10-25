/* mtl_priv.h - internals of the Metal layer (Objective-C, ARC). */
#ifndef NAPPLE_GFX_MTL_PRIV_H
#define NAPPLE_GFX_MTL_PRIV_H

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <TargetConditionals.h>
#if TARGET_OS_SIMULATOR
/* The iOS Simulator SDK headers (Xcode 27) omit MTLDrawable's addPresentedHandler: and the command buffer's presentDrawable:afterMinimumDuration: that the device and macOS SDKs declare (ios 10.3+). */
@interface NSObject (NappleSimDrawable)
- (void)addPresentedHandler:(void (^)(id<MTLDrawable>))block;
- (void)presentDrawable:(id<MTLDrawable>)d afterMinimumDuration:(CFTimeInterval)t;
@end
#endif
static inline void napple_present_after(id<MTLCommandBuffer> cb, id<MTLDrawable> d, CFTimeInterval t) {
    if ([(id)cb respondsToSelector:@selector(presentDrawable:afterMinimumDuration:)]) [(id)cb presentDrawable:d afterMinimumDuration:t]; else [cb presentDrawable:d];
}
/* Runs `block` once the drawable has been presented. The iOS 26 Simulator's CAMetalDrawable does not implement addPresentedHandler: at all
   (not only the SDK header): there the command buffer's completion stands in (call before `cb` is committed). */
static inline void napple_drawable_presented(id<MTLCommandBuffer> cb, id<MTLDrawable> d, void (^block)(id<MTLDrawable>)) {
    if ([(id)d respondsToSelector:@selector(addPresentedHandler:)]) [(id)d addPresentedHandler:block];
    else [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; block(d); }];
}
#include "gfx_mtl.h"
#include "timeline.h"
#include "shader/blob.h"
#include "shader/maxwell.h"

/* Buffer table layout shared with the translated shaders: cbuf k -> buffer(k) (0..17), gmem -> 17, rscale -> 18; vertex streams start at VB_BASE. */
#define VB_BASE 19
#define MAX_STREAMS 10
#define CBUF_USER_BASE 3          /* NVN uniform buffer binding i is hardware constant buffer i + 3 */
#define TEX_COMBINED_BASE 0x20    /* c2: combined texture handles, 8 bytes each */

typedef struct gfx_mtl {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    CAMetalLayer *layer;
    id<MTLTexture> headless;          /* offscreen present target */
    uint32_t out_w, out_h;
    int bc_ok;                        /* device supports BC textures */
} gfx_mtl;
extern gfx_mtl g_mtl;

/* ---- pools / address resolution (replay thread) ---------------------------------------------------------------- */
/* Resolve [addr, addr+len) to a buffer and offset; 0 on failure. */
int mtl_addr(uint64_t addr, uint64_t len, id<MTLBuffer> __autoreleasing *buf, uint64_t *off);
uint64_t mtl_pool_epoch(void);                                  /* changes whenever a pool is added or removed */
void mtl_pool_each(void (^fn)(id<MTLBuffer> buf));            /* every registered pool buffer */

/* ---- per-submission scratch ring (shared storage) --------------------------------------------------------------- */
@interface GfxRingChunk : NSObject { @public id<MTLBuffer> buf; uint8_t *cpu; uint32_t size, used; } @end
typedef struct ring { __unsafe_unretained NSMutableArray<GfxRingChunk *> *chunks; uint32_t cur; } ring;
void ring_begin(ring *r);
void *ring_alloc(ring *r, uint32_t size, uint32_t align, id<MTLBuffer> __autoreleasing *buf, uint32_t *off);
void ring_end(ring *r);                                  /* returns chunks to the pool */

/* ---- textures ------------------------------------------------------------------------------------------------- */
@interface GfxTex : NSObject {
@public
    texsnap key;
    id<MTLTexture> tex;
    MTLPixelFormat pf;
    int is_depth, has_stencil, gpu_owned, expand_rgb;
    uint64_t stamp;                   /* dirty-tracking stamp of the last upload (0 = never uploaded) */
    uint64_t checked;                 /* dirty epoch at the last check */
    uint64_t last_use;
    uint32_t w, h, depth, levels, layers, samples;            /* guest sizes (what the game asked for) */
    float rsx, rsy;                   /* render target scaling (mtl_scale.m): the Metal texture is w * rsx by h * rsy; 1 when not scaled */
    uint32_t scale_gen;               /* mtl_scale_generation() when rsx / rsy were decided */
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *views;
}
@end
GfxTex *mtl_tex_get(const texsnap *s, int for_gpu_write);                 /* find or create; replay thread */
void    mtl_tex_force_scale(GfxTex *t, float rx, float ry);                 /* re-allocate at guest size x (rx, ry): a pass mixed scaled and unscaled attachments; replay thread */
void mtl_tex_debug_dump(GfxTex *t, const char *dir, unsigned layer);       /* NAPPLE_DRAWLOG_TEX debugging aid */
void mtl_tex_prepare_sample(GfxTex *t);                                    /* upload if the CPU wrote the storage */
id<MTLTexture> mtl_tex_view(GfxTex *t, const uint8_t tswz[4], const view_t *v, int has_view, int shader_type, int shadow);   /* sampled view */
id<MTLTexture> mtl_tex_attach_view(GfxTex *t, const view_t *v, int has_view, uint32_t *level, uint32_t *slice);
int  mtl_tex_format_info(const texsnap *s, MTLPixelFormat *pf, int *is_depth, int *has_stencil);
id<MTLSamplerState> mtl_sampler_get(const samp_par *p);
void mtl_tex_forget_range(const void *mem, uint64_t size);
void mtl_tex_gc(uint64_t frame);
uint32_t mtl_vertex_format(int nvn_format, int *kind);                   /* MTLVertexFormat, attribute kind 0 float / 1 sint / 2 uint */

/* ---- shaders / pipelines ---------------------------------------------------------------------------------------- */
@interface GfxShader : NSObject { @public id<MTLFunction> fn; ShaderInfo info; int state; /* 0 pending 1 ready 2 failed */ int claimed; /* build started (atomic 0 -> 1) */ uint32_t local[3]; const shader_blob *blob; id<MTLBuffer> c1; } @end
typedef struct pipe_key {
    uint64_t vs, fs;
    uint32_t color_fmt[8], depth_fmt, stencil_fmt;
    uint32_t samples, topo;
    blend_st blend[8]; uint8_t blend_en[8]; uint8_t wmask[8];
    uint8_t a2c, flip_y, depth_mode, nattr, nstream, probe;
    va_st attr[32];
    vs_st stream[MAX_STREAMS];
    uint8_t attr_kind[32], rt_kind[8];
    uint32_t depth_slots[2];          /* per stage (vs, fs): binding slots holding depth textures */
} pipe_key;
@interface GfxPipe : NSObject { @public id<MTLRenderPipelineState> ps; GfxShader *vs, *fs; int state; int claimed; /* build started (atomic 0 -> 1) */ } @end
blend_st mtl_blend_resolve(blend_st b);         /* advanced blend modes -> plain factors (logs the ones it approximates) */
MTLBlendFactor mtl_blend_factor(unsigned nvn_blend_func);
MTLBlendOperation mtl_blend_op(unsigned nvn_blend_equation);
GfxPipe *mtl_pipe_get(const pipe_key *k);                  /* nil while compiling asynchronously */
GfxShader *mtl_shader_for(uint64_t hash, const ShaderKey *key);   /* translated + compiled (async); never nil for a known blob */
id<MTLComputePipelineState> mtl_compute_pipe(uint64_t hash, GfxShader *__autoreleasing *sh);
void mtl_shader_init(void);
void mtl_shader_shutdown(void);                             /* writes the binary archive */

/* ---- replay entry points --------------------------------------------------------------------------------------- */
void replay_run_submission(cmdrec *const *recs, uint32_t n, uint64_t seq);
void replay_run_present(win_t *w, int idx, const texsnap *t, uint32_t interval, uint64_t seq);
void replay_finish(id<MTLCommandBuffer> cb);
extern _Atomic uint64_t g_frame_gpu_us;

#endif
