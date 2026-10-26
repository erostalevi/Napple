/* mtl_interp.m - frame generation: shows N images per game frame by synthesising the ones in between.
 *
 * Design (docs/graphics-and-speed-settings.md has the long version and the measurements):
 *   - The game keeps running at its own rate; only the *presentation* is multiplied. The replay thread composes every game frame into a window-sized
 *     "scene" texture (aspect fit, filter, upscaler: mtl_present.m) and hands it to this module. A dedicated thread owns a queue and the display:
 *     it shows the generated frames between the previous and the new game frame, then the new frame itself, spaced evenly over the measured game
 *     frame interval and aligned to the display with presentDrawable:atTime:. Added latency = (n - 1) / n of a game frame (n = images per game frame).
 *   - Method "motion-compensated blend" (default, Metal compute): luma pyramid (1/4 .. 1/32), coarse-to-fine block-matching optical flow in both
 *     directions (A -> B and B -> A, SAD 5x5 windows, +-2..3 px refinement per level, parabolic sub-pixel), full-resolution bidirectional warp of the two
 *     frames to the requested phase, occlusion fallback to the temporally nearest frame, static-pixel preservation (pixels identical in both frames
 *     stay put: HUD / text over moving backgrounds do not smear). Scene cuts (best-match difference of the 1/32 level) skip interpolation.
 *   - Method "plain blend": cross fade (cheap, ghosting on motion). Apple's VideoToolbox VTFrameProcessor was measured and is NOT used: frame rate
 *     conversion needs 64-bit float RGBA frames and costs 23 ms at 1080p and 83 ms at 1440p (low latency variant: 1080p maximum, 10 ms, 4:2:0 only),
 *     far above the 8 ms a 120 Hz output allows.
 * Runs on its own queue so game work never blocks generated frames and vice versa. */
#include <math.h>
#include <pthread.h>
#include <time.h>
#import <QuartzCore/QuartzCore.h>
#import "mtl_interp.h"

/* ---------------------------------------------------------------------------------------------------------------
 * Shaders
 * --------------------------------------------------------------------------------------------------------------- */
static NSString *const kInterpMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void luma0(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant uint &F [[buffer(0)]], "
     "uint2 gid [[thread_position_in_grid]]) {\n"
     "  uint w = src.get_width(), h = src.get_height(); if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;\n"
     "  float s = 0; for (uint dy = 0; dy < F; dy++) for (uint dx = 0; dx < F; dx++) { uint2 p = min(gid * F + uint2(dx, dy), uint2(w - 1, h - 1)); s += "
     "dot(src.read(p).rgb, float3(0.2126, 0.7152, 0.0722)); }\n"
     "  dst.write(float4(s / float(F * F)), gid); }\n"
     "kernel void lumaN(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 gid "
     "[[thread_position_in_grid]]) {\n"
     "  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return; uint2 m = uint2(src.get_width() - 1, src.get_height() - 1); uint2 b = gid * 2;\n"
     "  float s = src.read(min(b, m)).r + src.read(min(b + uint2(1, 0), m)).r + src.read(min(b + uint2(0, 1), m)).r + src.read(min(b + uint2(1, 1), m)).r; "
     "dst.write(float4(s * 0.25), gid); }\n"
     /* flow of one pyramid level: v minimises SAD(A(x + o), B(x + o + v)) over a 5x5 window; initial guess = the coarser level's flow * 2 */
     "struct FP { int radius; int has_prev; float lambda; int pad; };\n"
     "float sad(thread const float *aw, texture2d<float, access::read> b, int2 x, int2 v, int2 mx) {\n"
     "  float s = 0; for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++) {\n"
     "    int2 pb = clamp(x + int2(ox, oy) + v, int2(0), mx); s += fabs(aw[(oy + 2) * 5 + ox + 2] - b.read(uint2(pb)).r); } return s; }\n"
     "kernel void flow_level(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::read> b [[texture(1)]], texture2d<float, access::read> "
     "prev [[texture(2)]],\n"
     "                       texture2d<float, access::write> out [[texture(3)]], constant FP &p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
     "  if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;\n"
     "  int2 mx = int2(a.get_width() - 1, a.get_height() - 1); int2 x = int2(gid);\n"
     "  float2 init = p.has_prev != 0 ? prev.read(uint2(min(x / 2, int2(prev.get_width() - 1, prev.get_height() - 1)))).xy * 2.0 : float2(0);\n"
     "  int2 base = int2(round(init)); float best = 1e9; int2 bv = base; float aw[25]; for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++) "
     "aw[(oy + 2) * 5 + ox + 2] = a.read(uint2(clamp(x + int2(ox, oy), int2(0), mx))).r;\n  float c0 = sad(aw, b, x, base, mx);\n"
     "  best = c0;\n"
     "  for (int dy = -p.radius; dy <= p.radius; dy++) for (int dx = -p.radius; dx <= p.radius; dx++) {\n"
     "    if (dx == 0 && dy == 0) continue; int2 v = base + int2(dx, dy); float c = sad(aw, b, x, v, mx) + p.lambda * float(abs(dx) + abs(dy));\n"
     "    if (c < best) { best = c; bv = v; } }\n"
     "  float2 fv = float2(bv);\n"
     "  /* parabolic sub-pixel refinement around the winner */\n"
     "  float cx0 = sad(aw, b, x, bv + int2(-1, 0), mx), cx1 = best, cx2 = sad(aw, b, x, bv + int2(1, 0), mx);\n"
     "  float cy0 = sad(aw, b, x, bv + int2(0, -1), mx), cy2 = sad(aw, b, x, bv + int2(0, 1), mx);\n"
     "  float dxs = cx0 - 2.0 * cx1 + cx2, dys = cy0 - 2.0 * cx1 + cy2;\n"
     "  if (dxs > 1e-4) fv.x += clamp(0.5 * (cx0 - cx2) / dxs, -0.5, 0.5);\n"
     "  if (dys > 1e-4) fv.y += clamp(0.5 * (cy0 - cy2) / dys, -0.5, 0.5);\n"
     "  out.write(float4(fv, best / 25.0, 0), gid); }\n"
     /* warp: output at phase t between A (t = 0) and B (t = 1) */
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V wv(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - "
     "p.y); return o; }\n"
     "struct WP { float t; int mode; float2 inv_flow_dim; float2 inv_dim; float eps; float pad; };\n"
     "fragment float4 wf(V in [[stage_in]], texture2d<float> A [[texture(0)]], texture2d<float> B [[texture(1)]], texture2d<float> fab [[texture(2)]], "
     "texture2d<float> fba [[texture(3)]],\n"
     "                   sampler s [[sampler(0)]], constant WP &p [[buffer(0)]]) {\n"
     "  float2 uv = in.uv; float3 a0 = A.sample(s, uv).rgb, b0 = B.sample(s, uv).rgb;\n"
     "  if (p.mode == 1) return float4(mix(a0, b0, p.t), 1.0);\n"
     /* static pixels: identical in both frames over a 3x3 neighbourhood -> keep (HUD, bars, text) */
     "  float md = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { float2 q = uv + float2(dx, dy) * p.inv_dim; md = max(md, "
     "max3_(fabs(A.sample(s, q).rgb - B.sample(s, q).rgb))); }\n"
     "  if (md <= p.eps) return float4(b0, 1.0);\n"
     "  float2 vab = fab.sample(s, uv).xy * p.inv_flow_dim, vba = fba.sample(s, uv).xy * p.inv_flow_dim;     /* flows in normalised units */\n"
     "  float3 ca = A.sample(s, uv - p.t * vab).rgb, cb = B.sample(s, uv - (1.0 - p.t) * vba).rgb;\n"
     "  float3 w = mix(ca, cb, p.t);\n"
     "  float d = max3_(fabs(ca - cb)); float occl = smoothstep(0.12, 0.30, d);\n"
     "  float3 fallback = p.t < 0.5 ? a0 : b0;\n"
     "  return float4(mix(w, fallback, occl), 1.0); }\n";

/* helper used in the shader above (declared before use by prefixing) */
static NSString *interp_source(void) {
    return [@"#include <metal_stdlib>\nusing namespace metal;\nstatic inline float max3_(float3 v) { return max(v.x, max(v.y, v.z)); }\n"
        stringByAppendingString:[kInterpMSL substringFromIndex:[kInterpMSL rangeOfString:@"kernel void luma0"].location]];
}

/* ---------------------------------------------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------------------------------------------- */
#define NLEVELS 4
#define CUTLV 2 /* pyramid level used for scene cut detection (shared storage, copied to the CPU) */
#define POOL 8

typedef struct FrameRec {
    id<MTLTexture> scene; /* BGRA8 composed frame (window sized) */
    id<MTLTexture> pyr[NLEVELS]; /* luma pyramid, R8Unorm, 1/4 .. 1/32 */
    uint8_t *l3;
    uint32_t l3w, l3h; /* copy of the coarsest level for scene cut detection */
    double arrival; /* CACurrentMediaTime at submission */
    win_t *win;
    int idx; /* guest swapchain image to release when shown */
    _Atomic int released;
    _Atomic int refs; /* owner + in-flight command buffers reading this frame; the pool slot is free at 0 */
    struct FrameRec *next;
} FrameRec;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    FrameRec *q_head, *q_tail; /* completed frames waiting for the presenter */
    FrameRec *pool[POOL];
    int pool_used[POOL];
    uint32_t w, h; /* size of the pool textures */
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> luma0, lumaN, flow;
    id<MTLRenderPipelineState> warp;
    id<MTLSamplerState> samp;
    id<MTLTexture> flow_ab[NLEVELS], flow_ba[NLEVELS];
    pthread_t thread;
    int started, stop;
    int fail; /* pipelines could not be built: frame generation is unavailable */
    id<MTLTexture> out_tex; /* headless output */
} G = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void rec_release(FrameRec *r) {
    if (!r || atomic_exchange(&r->released, 1)) return;
    if (r->win) {
        atomic_fetch_add(&r->win->displayed[r->idx], 1);
        atomic_fetch_add(&g_gfx.frames, 1);
        gfx_wake();
    }
}

static int build(void) {
    if (G.queue) return 0;
    if (G.fail) return -1;
    NSError *err = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:interp_source() options:nil error:&err];
    if (!lib) {
        LOGE("frame generation shaders: %s", err.localizedDescription.UTF8String);
        G.fail = 1;
        return -1;
    }
    G.luma0 = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"luma0"] error:&err];
    G.lumaN = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lumaN"] error:&err];
    G.flow = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"flow_level"] error:&err];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"wv"];
    d.fragmentFunction = [lib newFunctionWithName:@"wf"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    G.warp = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!G.luma0 || !G.lumaN || !G.flow || !G.warp) {
        LOGE("frame generation pipelines: %s", err.localizedDescription.UTF8String);
        G.fail = 1;
        return -1;
    }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    G.samp = [g_mtl.dev newSamplerStateWithDescriptor:sd];
    G.queue = [g_mtl.dev newCommandQueue];
    return 0;
}

static id<MTLTexture> mk(uint32_t w, uint32_t h, MTLPixelFormat pf, MTLTextureUsage u, MTLStorageMode sm) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w ? w : 1 height:h ? h : 1 mipmapped:NO];
    d.usage = u;
    d.storageMode = sm;
    return [g_mtl.dev newTextureWithDescriptor:d];
}
static uint32_t g_F = 4; /* luma downscale of level 0: keeps the finest flow level near 480 pixels wide whatever the window size */
static void level_dims(uint32_t w, uint32_t h, int lv, uint32_t *lw, uint32_t *lh) {
    uint32_t sw = w, sh = h;
    sw = (sw + g_F - 1) / g_F;
    sh = (sh + g_F - 1) / g_F;
    for (int i = 0; i < lv; i++) {
        sw = (sw + 1) / 2;
        sh = (sh + 1) / 2;
    }
    *lw = sw ? sw : 1;
    *lh = sh ? sh : 1;
}

/* (Re)build the pool for a window size. Called with G.mu held, only when nothing is in flight (size changes drop the queue). */
static void pool_prepare(uint32_t w, uint32_t h) {
    if (G.w == w && G.h == h && G.pool[0]) return;
    g_F = (w + 479) / 480;
    if (g_F < 2) g_F = 2;
    if (g_F > 8) g_F = 8;
    for (int i = 0; i < POOL; i++) {
        if (G.pool[i]) { free(G.pool[i]->l3); }
        G.pool[i] = NULL;
        G.pool_used[i] = 0;
    }
    G.q_head = G.q_tail = NULL;
    G.w = w;
    G.h = h;
    for (int i = 0; i < POOL; i++) {
        FrameRec *r = calloc(1, sizeof *r);
        r->scene = mk(w, h, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
        for (int l = 0; l < NLEVELS; l++) {
            uint32_t lw, lh;
            level_dims(w, h, l, &lw, &lh);
            r->pyr[l] = mk(lw, lh, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
                           l == CUTLV ? MTLStorageModeShared : MTLStorageModePrivate);
        }
        level_dims(w, h, CUTLV, &r->l3w, &r->l3h);
        r->l3 = calloc((size_t)r->l3w * r->l3h, 1);
        G.pool[i] = r;
    }
    for (int l = 0; l < NLEVELS; l++) {
        uint32_t lw, lh;
        level_dims(w, h, l, &lw, &lh);
        G.flow_ab[l] = mk(lw, lh, MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, MTLStorageModePrivate);
        G.flow_ba[l] = mk(lw, lh, MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, MTLStorageModePrivate);
    }
    LOGI("frame generation: %ux%u frames, %d buffers", w, h, POOL);
}

static FrameRec *pool_take(void) {
    for (int i = 0; i < POOL; i++)
        if (G.pool[i] && !G.pool_used[i]) {
            G.pool_used[i] = 1;
            atomic_store(&G.pool[i]->released, 0);
            atomic_store(&G.pool[i]->refs, 1);
            G.pool[i]->win = NULL;
            return G.pool[i];
        }
    return NULL;
}
static void pool_give(FrameRec *r) {
    for (int i = 0; i < POOL; i++)
        if (G.pool[i] == r) G.pool_used[i] = 0;
}
static void rec_ref(FrameRec *r) { atomic_fetch_add(&r->refs, 1); }
static void rec_unref(FrameRec *r) {
    if (r && atomic_fetch_sub(&r->refs, 1) == 1) {
        pthread_mutex_lock(&G.mu);
        pool_give(r);
        pthread_mutex_unlock(&G.mu);
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Pyramid and flow
 * --------------------------------------------------------------------------------------------------------------- */
static void encode_pyramid(id<MTLCommandBuffer> cb, FrameRec *r) {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    uint32_t F = g_F;
    [e setComputePipelineState:G.luma0];
    [e setTexture:r->scene atIndex:0];
    [e setTexture:r->pyr[0] atIndex:1];
    [e setBytes:&F length:4 atIndex:0];
    [e dispatchThreads:MTLSizeMake(r->pyr[0].width, r->pyr[0].height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e setComputePipelineState:G.lumaN];
    for (int l = 1; l < NLEVELS; l++) {
        [e setTexture:r->pyr[l - 1] atIndex:0];
        [e setTexture:r->pyr[l] atIndex:1];
        [e dispatchThreads:MTLSizeMake(r->pyr[l].width, r->pyr[l].height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    }
    [e endEncoding];
}

static void encode_flow(id<MTLCommandBuffer> cb, FrameRec *a, FrameRec *b) {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:G.flow];
    for (int dir = 0; dir < 2; dir++) {
        FrameRec *from = dir ? b : a, *to = dir ? a : b;
        id<MTLTexture> __strong *fl = dir ? G.flow_ba : G.flow_ab;
        for (int l = NLEVELS - 1; l >= 0; l--) {
            struct {
                int radius, has_prev;
                float lambda;
                int pad;
            } p = {l == NLEVELS - 1 ? 6 : l == 0 ? 1 : 2, l != NLEVELS - 1, 0.04f, 0};
            [e setTexture:from->pyr[l] atIndex:0];
            [e setTexture:to->pyr[l] atIndex:1];
            [e setTexture:l == NLEVELS - 1 ? from->pyr[l] : fl[l + 1] atIndex:2];
            [e setTexture:fl[l] atIndex:3];
            [e setBytes:&p length:sizeof p atIndex:0];
            [e dispatchThreads:MTLSizeMake(fl[l].width, fl[l].height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
    }
    [e endEncoding];
}

static void encode_warp(id<MTLCommandBuffer> cb, FrameRec *a, FrameRec *b, float t, int mode, id<MTLTexture> target) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    struct {
        float t;
        int mode;
        float ifd[2];
        float id[2];
        float eps, pad;
    } p = {t, mode, {1.0f / (float)G.flow_ab[0].width, 1.0f / (float)G.flow_ab[0].height}, {1.0f / (float)G.w, 1.0f / (float)G.h}, 3.0f / 255.0f, 0};
    [e setRenderPipelineState:G.warp];
    [e setFragmentTexture:a->scene atIndex:0];
    [e setFragmentTexture:b->scene atIndex:1];
    [e setFragmentTexture:G.flow_ab[0] atIndex:2];
    [e setFragmentTexture:G.flow_ba[0] atIndex:3];
    [e setFragmentSamplerState:G.samp atIndex:0];
    [e setFragmentBytes:&p length:sizeof p atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

/* Scene cut detection on the coarsest luma level: the best match over +-3 level pixels (a pan or a fast camera move still matches) of the mean absolute
 * difference. A cut (or a fade through black) makes every shift mismatch. Exposed for tests through mtl_interp_cut_score. */
double mtl_interp_cut_score(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h) {
    double best = 1e9;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            double s = 0;
            long n = 0;
            for (uint32_t y = 3; y + 3 < h; y++)
                for (uint32_t x = 3; x + 3 < w; x++) {
                    s += abs((int)a[y * w + x] - (int)b[(y + (uint32_t)dy) * w + x + (uint32_t)dx]);
                    n++;
                }
            if (n) {
                s /= (double)n * 255.0;
                if (s < best) best = s;
            }
        }
    return best < 1e8 ? best : 0;
}
#define CUT_THRESHOLD 0.075

/* ---------------------------------------------------------------------------------------------------------------
 * Presenter thread
 * --------------------------------------------------------------------------------------------------------------- */
static double ema_src = 1.0 / 60.0; /* measured game frame interval (s) */
static double g_last_present_media; /* CACurrentMediaTime of the last scheduled present */

static void sleep_until(double media_t) {
    for (;;) {
        double d = media_t - CACurrentMediaTime();
        if (d <= 0) return;
        if (d > 0.002) {
            struct timespec ts = {0, (long)((d - 0.0015) * 1e9)};
            nanosleep(&ts, NULL);
        } else {
            sched_yield();
        }
    }
}

/* One output image: generated (a, b, t) or the real frame (a == NULL, b real). */
static void show_image(FrameRec *a, FrameRec *b, float t, int mode, double at, int release_after, double disp_period) {
    id<CAMetalDrawable> dr = nil;
    id<MTLTexture> target = G.out_tex;
    if (g_mtl.layer) {
        dr = [g_mtl.layer nextDrawable];
        target = dr.texture;
    }
    if (!target) {
        if (release_after) rec_release(b);
        return;
    }
    id<MTLCommandBuffer> cb = [G.queue commandBuffer];
    rec_ref(b);
    if (a) rec_ref(a);
    if (a) encode_warp(cb, a, b, t, mode, target);
    else { /* the real frame: a plain copy through the warp shader in blend mode with t = 1 (identity) */ encode_warp(cb, b, b, 1.0f, 1, target);
    }
    if (dr) {
        if (at > CACurrentMediaTime() + 0.0005) [cb presentDrawable:dr atTime:at];
        else [cb presentDrawable:dr];
        g_last_present_media = at > CACurrentMediaTime() ? at : CACurrentMediaTime();
        if (release_after)
            napple_drawable_presented(cb, dr, ^(id<MTLDrawable> d) {
                (void)d;
                rec_release(b);
            });
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        if (a) g_display_stats.fg_ms = (c.GPUEndTime - c.GPUStartTime) * 1000.0;
        if (!dr && release_after) rec_release(b);
        rec_unref(b);
        if (a) rec_unref(a);
    }];
    [cb commit];
    g_display_stats.shown_frames++;
    if (a) g_display_stats.generated_frames++;
    (void)disp_period;
}

static void *presenter(void *arg) {
    (void)arg;
    pthread_setname_np("napple-gfx-framegen");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    FrameRec *prev = NULL;
    for (;;) {
        pthread_mutex_lock(&G.mu);
        while (!G.q_head && !G.stop) pthread_cond_wait(&G.cv, &G.mu);
        if (G.stop) {
            pthread_mutex_unlock(&G.mu);
            return NULL;
        }
        FrameRec *cur = G.q_head;
        G.q_head = cur->next;
        if (!G.q_head) G.q_tail = NULL;
        /* backlog: only the newest frame matters (older ones are released without being shown) */
        while (G.q_head) {
            FrameRec *skip = cur;
            cur = G.q_head;
            G.q_head = cur->next;
            if (!G.q_head) G.q_tail = NULL;
            rec_release(skip);
            rec_unref(skip);
            g_display_stats.dropped_frames++;
        }
        pthread_mutex_unlock(&G.mu);

        napple_display_settings ds;
        napple_display_get(&ds);
        uint32_t ww, wh;
        double hz;
        display_window_get(&ww, &wh, &hz);
        if (!g_mtl.layer) hz = 120;
        const double Pd = 1.0 / (hz > 1 ? hz : 60.0);
        /* interval estimate */
        double Ps = ema_src;
        if (prev && cur->arrival > prev->arrival) {
            double dt = cur->arrival - prev->arrival;
            if (dt > 0.002 && dt < 0.25) {
                ema_src = ema_src * 0.8 + dt * 0.2;
                Ps = ema_src;
            }
        }
        int n = ds.frame_gen_mult ? (int)ds.frame_gen_mult : (int)floor(Ps * hz + 0.5);
        if (n < 1) n = 1;
        if (n > 8) n = 8;
        int mode = ds.frame_gen_method == NAPPLE_FG_BLEND ? 1 : 0;
        int generate = prev != NULL && n > 1;
        if (generate) {
            double score = mtl_interp_cut_score(prev->l3, cur->l3, cur->l3w, cur->l3h);
            if (score > CUT_THRESHOLD) {
                generate = 0;
                g_display_stats.scene_cuts++;
            }
        }
        double spacing = Ps / n;
        if (spacing < Pd) spacing = Pd;
        double start = CACurrentMediaTime() + 0.0015;
        if (g_last_present_media + Pd * 0.5 > start) start = g_last_present_media + Pd;
        if (generate && mode == 0) {
            id<MTLCommandBuffer> cb = [G.queue commandBuffer];
            encode_flow(cb, prev, cur);
            FrameRec *fa = prev, *fb = cur;
            rec_ref(fa);
            rec_ref(fb);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                g_display_stats.fg_ms = (c.GPUEndTime - c.GPUStartTime) * 1000.0;
                rec_unref(fa);
                rec_unref(fb);
            }];
            [cb commit];
        }
        if (generate) {
            for (int k = 1; k < n; k++) {
                double at = start + (k - 1) * spacing;
                if (g_mtl.layer) sleep_until(at - 1.5 * Pd);
                show_image(prev, cur, (float)k / (float)n, mode, at, k == 1, Pd);
            }
            double at = start + (n - 1) * spacing;
            if (g_mtl.layer) sleep_until(at - 1.5 * Pd);
            show_image(NULL, cur, 1.0f, 1, at, 0, Pd);
            if (!g_mtl.layer) {
                struct timespec ts = {0, (long)(Ps * 1e9)};
                nanosleep(&ts, NULL);
                rec_release(cur);
            }
            g_display_stats.latency_ms = ((double)(n - 1) / n * Ps + 0.0015) * 1000.0;
        } else {
            show_image(NULL, cur, 1.0f, 1, start, 1, Pd);
            if (!g_mtl.layer) {
                struct timespec ts = {0, (long)(Ps * 1e9)};
                nanosleep(&ts, NULL);
            }
            g_display_stats.latency_ms = 1.5;
        }
        g_display_stats.fg_active = 1;
        g_display_stats.fg_method = mode ? NAPPLE_FG_BLEND : NAPPLE_FG_METAL_FLOW;
        { /* output rate statistic */
            static double last;
            double now = CACurrentMediaTime();
            static double ema_o;
            if (last) {
                double dt = (now - last) / (generate ? n : 1);
                ema_o = ema_o == 0 ? dt : ema_o * 0.9 + dt * 0.1;
            }
            last = now;
            g_display_stats.out_fps = ema_o > 0 ? 1.0 / ema_o : 0;
        }
        /* the older of the two frames is free once the command buffers that read it finish (it was the owner's reference) */
        if (prev) rec_unref(prev);
        prev = cur;
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Replay thread side
 * --------------------------------------------------------------------------------------------------------------- */
int mtl_interp_active(const napple_display_settings *ds) {
    if (!ds->frame_gen) {
        g_display_stats.fg_active = 0;
        return 0;
    }
    if (!g_mtl.layer && !getenv("NAPPLE_FG_HEADLESS")) return 0;
    if (build()) {
        g_display_stats.fg_active = 0;
        return 0;
    }
    return 1;
}

int mtl_interp_submit(const napple_display_settings *ds, id<MTLTexture> src, const float crop[4], float gw, float gh, uint32_t win_w, uint32_t win_h, double hz,
                      uint32_t interval, double speed, win_t *w, int idx, uint64_t seq, uint64_t arrival_ns) {
    (void)hz;
    (void)interval;
    (void)speed;
    (void)arrival_ns;
    if (build()) return 0;
    uint32_t tw = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.width : (uint32_t)(g_mtl.headless ? g_mtl.headless.width : win_w);
    uint32_t th = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.height : (uint32_t)(g_mtl.headless ? g_mtl.headless.height : win_h);
    if (tw < 64 || th < 64) return 0;
    FrameRec *r;
    pthread_mutex_lock(&G.mu);
    if (G.w != tw || G.h != th) {
        /* size change: wait for the presenter to be idle with the old pool (the queue is dropped) */
        pool_prepare(tw, th);
        if (!g_mtl.layer) G.out_tex = mk(tw, th, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    }
    r = pool_take();
    pthread_mutex_unlock(&G.mu);
    if (!r) return 0; /* presenter far behind: present normally this time */
    r->win = w;
    r->idx = idx;
    r->arrival = CACurrentMediaTime();
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    mtl_present_compose(cb, src, crop, gw, gh, r->scene, ds, 1);
    encode_pyramid(cb, r);
    __block FrameRec *rr = r;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        [rr->pyr[CUTLV] getBytes:rr->l3 bytesPerRow:rr->l3w fromRegion:MTLRegionMake2D(0, 0, rr->l3w, rr->l3h) mipmapLevel:0];
        gfx_timeline_complete(seq);
        pthread_mutex_lock(&G.mu);
        rr->next = NULL;
        if (G.q_tail) G.q_tail->next = rr;
        else G.q_head = rr;
        G.q_tail = rr;
        pthread_cond_signal(&G.cv);
        pthread_mutex_unlock(&G.mu);
    }];
    [cb commit];
    if (!G.started) {
        G.started = 1;
        pthread_create(&G.thread, NULL, presenter, NULL);
    }
    return 1;
}

void mtl_interp_shutdown(void) {
    if (!G.started) return;
    pthread_mutex_lock(&G.mu);
    G.stop = 1;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.thread, NULL);
    G.started = 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Test hook (gfx/tests/t_interp.m): interpolate between two ready-made frames of equal size, synchronously. Returns 0 on success.
 * --------------------------------------------------------------------------------------------------------------- */
int mtl_interp_test_pair(id<MTLTexture> a, id<MTLTexture> b, float t, int mode, id<MTLTexture> out, double *cut_score,
                         double *gpu_ms /* [3]: total, flow, warp */) {
    if (build()) return -1;
    uint32_t w = (uint32_t)a.width, h = (uint32_t)a.height;
    pthread_mutex_lock(&G.mu);
    pool_prepare(w, h);
    FrameRec *ra = pool_take(), *rb = pool_take();
    pthread_mutex_unlock(&G.mu);
    if (!ra || !rb) return -2;
    id<MTLCommandBuffer> cb = [G.queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:a toTexture:ra->scene];
    [bl copyFromTexture:b toTexture:rb->scene];
    [bl endEncoding];
    encode_pyramid(cb, ra);
    encode_pyramid(cb, rb);
    [cb commit];
    [cb waitUntilCompleted];
    FrameRec *both[2] = {ra, rb};
    for (int i = 0; i < 2; i++)
        [both[i]->pyr[CUTLV] getBytes:both[i]->l3 bytesPerRow:both[i]->l3w fromRegion:MTLRegionMake2D(0, 0, both[i]->l3w, both[i]->l3h) mipmapLevel:0];
    if (cut_score) *cut_score = mtl_interp_cut_score(ra->l3, rb->l3, ra->l3w, ra->l3h);
    double fms = 0, wms = 0;
    if (mode == 0) {
        cb = [G.queue commandBuffer];
        encode_flow(cb, ra, rb);
        [cb commit];
        [cb waitUntilCompleted];
        fms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    }
    cb = [G.queue commandBuffer];
    encode_warp(cb, ra, rb, t, mode, out);
    [cb commit];
    [cb waitUntilCompleted];
    wms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    if (gpu_ms) {
        gpu_ms[0] = fms + wms;
        gpu_ms[1] = fms;
        gpu_ms[2] = wms;
    }
    pthread_mutex_lock(&G.mu);
    pool_give(ra);
    pool_give(rb);
    pthread_mutex_unlock(&G.mu);
    return 0;
}
