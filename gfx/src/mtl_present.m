/* mtl_present.m - the output stage: the game's final image -> window.
 *
 * Moved out of mtl_core.m (present, screenshots) and extended by the display pipeline (docs/graphics-and-speed-settings.md):
 *   - aspect handling (letterbox / stretch / fill), any window size,
 *   - output filters: nearest, bilinear, box (area), bicubic (Catmull-Rom), Lanczos3, sharpened bicubic. Box / bicubic / Lanczos are separable two-pass
 *     resamplers with a kernel stretched by the down-scale ratio (so supersampled renders are filtered properly, not point sampled),
 *   - MetalFX spatial upscaling when the render is smaller than the output,
 *   - game speed: frames beyond the display rate are dropped, presents are paced by the virtual vsync (timescale),
 *   - frame generation (mtl_interp.m) takes the composed frame instead of the drawable,
 *   - statistics for the overlay.
 * Runs on the replay thread (one job at a time). */
#import <ImageIO/ImageIO.h>
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define NAPPLE_HAVE_METALFX 1 /* absent from the Xcode 27 iOS Simulator SDK: the upscaler is then unavailable and the plain scaler is used */
#endif
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <math.h>
#include <pthread.h>

#import "mtl_display.h"
#import "mtl_interp.h"

/* ---------------------------------------------------------------------------------------------------------------
 * Shaders
 * --------------------------------------------------------------------------------------------------------------- */
static NSString *const kPresentMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V pv(uint vid [[vertex_id]], constant float4 &crop [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); float2 t = float2(p.x, 1.0 - p.y);\n"
     "  o.uv = crop.xy + t * crop.zw; return o; }\n"
     "fragment float4 pf(V in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) { return float4(tex.sample(s, in.uv).rgb, 1.0); }\n"
     /* Separable resampler. P: org / span = source rectangle in source pixels (span > 0 here: flips are applied by the caller through `flip`),
        ratio = source pixels per output pixel along the pass axis, kernel: 1 box (area), 2 triangle, 3 Catmull-Rom, 4 Lanczos3. */
     "struct RS { float2 org; float2 span; float2 out_dim; float2 vp; float ratio; float radius; int kern_id; int axis; int flip; int n; };\n"
     "float kern(int k, float d, float a) {\n"
     "  if (k == 1) return clamp(0.5 * a + 0.5 - fabs(d), 0.0, 1.0);\n"
     "  float t = fabs(d) / a;\n"
     "  if (k == 2) return max(0.0, 1.0 - t);\n"
     "  if (k == 3) { if (t < 1.0) return 1.5 * t * t * t - 2.5 * t * t + 1.0; if (t < 2.0) return -0.5 * t * t * t + 2.5 * t * t - 4.0 * t + 2.0; return 0.0; "
     "}\n"
     "  if (t < 1e-4) return 1.0; if (t >= 3.0) return 0.0; float pt = M_PI_F * t; return 3.0 * sin(pt) * sin(pt / 3.0) / (pt * pt);\n"
     "}\n"
     "fragment float4 rs(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant RS &p [[buffer(0)]]) {\n"
     "  float2 px = in.pos.xy - p.vp;                /* pixel position inside the pass viewport */\n"
     "  float a = max(1.0, p.ratio);\n"
     "  float o = p.axis == 0 ? px.x : px.y;\n"
     "  if (p.axis == 1 && p.flip != 0) o = p.out_dim.y - 1.0 - (px.y);\n"
     "  float c = (o + 0.5) * p.ratio - 0.5 + (p.axis == 0 ? p.org.x : p.org.y);\n"
     "  float lo0 = p.axis == 0 ? p.org.x : p.org.y, hi0 = lo0 + (p.axis == 0 ? p.span.x : p.span.y) - 1.0;\n"
     "  int i0 = int(ceil(c - p.radius * a)), i1 = int(floor(c + p.radius * a));\n"
     "  float4 acc = 0; float ws = 0;\n"
     "  for (int i = i0; i <= i1; i++) {\n"
     "    float w = kern(p.kern_id, float(i) - c, a); if (w == 0.0) continue;\n"
     "    float ii = clamp(float(i), lo0, hi0);\n"
     "    uint2 q = p.axis == 0 ? uint2(uint(ii), uint(px.y + p.org.y)) : uint2(uint(px.x), uint(ii));\n"
     "    acc += tex.read(q) * w; ws += w;\n"
     "  }\n"
     "  return acc / max(ws, 1e-5);\n"
     "}\n"
     /* contrast adaptive sharpening (after AMD FidelityFX CAS, simplified): reads the pass input at the output pixel */
     "struct SH { float amount; packed_float2 origin; float pad; };   /* 16 bytes, same layout as the host struct */\n"
     "fragment float4 sharpen(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant SH &p [[buffer(0)]]) {\n"
     "  int2 q = int2(in.pos.xy - float2(p.origin)); int2 mx = int2(tex.get_width() - 1, tex.get_height() - 1);\n"
     "  float3 c = tex.read(uint2(clamp(q, int2(0), mx))).rgb, n = tex.read(uint2(clamp(q + int2(0, -1), int2(0), mx))).rgb, s = tex.read(uint2(clamp(q + "
     "int2(0, 1), int2(0), mx))).rgb,\n"
     "         e = tex.read(uint2(clamp(q + int2(1, 0), int2(0), mx))).rgb, w = tex.read(uint2(clamp(q + int2(-1, 0), int2(0), mx))).rgb;\n"
     "  float3 mn = min(min(min(n, s), min(e, w)), c), mxx = max(max(max(n, s), max(e, w)), c);\n"
     "  float3 amp = sqrt(clamp(min(mn, 1.0 - mxx) / max(mxx, 1e-3), 0.0, 1.0));\n"
     "  float peak = -1.0 / mix(8.0, 4.0, p.amount);\n"
     "  float3 wgt = amp * peak; float3 r = 1.0 / (1.0 + 4.0 * wgt);\n"
     "  return float4(clamp((wgt * (n + s + e + w) + c) * r, mn, mxx), 1.0);   /* no halos: the result stays inside the local range */\n"
     "}\n"
     "fragment float4 copy1(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant float2 &origin [[buffer(0)]]) {\n"
     "  return float4(tex.read(uint2(in.pos.xy - origin)).rgb, 1.0); }\n";

enum { KERN_BOX = 1, KERN_TRI = 2, KERN_CATMULL = 3, KERN_LANCZOS = 4 };

static id<MTLLibrary> g_lib;
static id<MTLRenderPipelineState> g_pso_pf, g_pso_rs[2], g_pso_sharp, g_pso_copy1; /* targets: BGRA8 (final), RGBA16F (intermediate) via separate arrays */
static id<MTLRenderPipelineState> g_pso_rs_f16, g_pso_copy1_f16, g_pso_pf_f16;
static id<MTLSamplerState> g_samp[2];

static id<MTLRenderPipelineState> make_pso(NSString *vs, NSString *fs, MTLPixelFormat pf) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_lib newFunctionWithName:vs];
    d.fragmentFunction = [g_lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = pf;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGE("present pipeline %s: %s", fs.UTF8String, err.localizedDescription.UTF8String);
    return p;
}

int mtl_present_init(void) {
    NSError *err = nil;
    g_lib = [g_mtl.dev newLibraryWithSource:kPresentMSL options:nil error:&err];
    if (!g_lib) {
        LOGE("present shader: %s", err.localizedDescription.UTF8String);
        return -1;
    }
    g_pso_pf = make_pso(@"pv", @"pf", MTLPixelFormatBGRA8Unorm);
    g_pso_rs[0] = make_pso(@"pv", @"rs", MTLPixelFormatRGBA16Float); /* horizontal pass -> intermediate */
    g_pso_rs[1] = make_pso(@"pv", @"rs", MTLPixelFormatBGRA8Unorm); /* vertical pass -> final */
    g_pso_rs_f16 = make_pso(@"pv", @"rs", MTLPixelFormatRGBA16Float); /* vertical pass -> intermediate (followed by sharpening) */
    g_pso_sharp = make_pso(@"pv", @"sharpen", MTLPixelFormatBGRA8Unorm);
    g_pso_copy1 = make_pso(@"pv", @"copy1", MTLPixelFormatBGRA8Unorm);
    g_pso_copy1_f16 = make_pso(@"pv", @"copy1", MTLPixelFormatRGBA16Float);
    g_pso_pf_f16 = make_pso(@"pv", @"pf", MTLPixelFormatRGBA16Float);
    if (!g_pso_pf || !g_pso_rs[0] || !g_pso_rs[1] || !g_pso_sharp || !g_pso_copy1) return -1;
    for (int i = 0; i < 2; i++) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_samp[i] = [g_mtl.dev newSamplerStateWithDescriptor:sd];
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Intermediate textures (per size / format, reused)
 * --------------------------------------------------------------------------------------------------------------- */
static id<MTLTexture> tmp_tex(int slot, uint32_t w, uint32_t h, MTLPixelFormat pf) {
    static id<MTLTexture> t[4];
    if (!t[slot] || t[slot].width != w || t[slot].height != h || t[slot].pixelFormat != pf) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        d.storageMode = MTLStorageModePrivate;
        t[slot] = [g_mtl.dev newTextureWithDescriptor:d];
    }
    return t[slot];
}

/* ---------------------------------------------------------------------------------------------------------------
 * Composition
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    float org[2], span[2], out_dim[2], vp[2], ratio, radius;
    int kern_id, axis, flip, n;
} RSParams;

static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb, id<MTLTexture> target, int clear) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [cb renderCommandEncoderWithDescriptor:rp];
}

#ifdef NAPPLE_HAVE_METALFX
/* MetalFX spatial scaler cache (one per input/output size and format). */
static id<MTLFXSpatialScaler> get_fx(MTLPixelFormat in_pf, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
    static id<MTLFXSpatialScaler> sc;
    static uint64_t key;
    uint64_t k = (uint64_t)iw | (uint64_t)ih << 16 | (uint64_t)ow << 32 | (uint64_t)oh << 48 | (uint64_t)in_pf << 8;
    if (sc && key == k) return sc;
    MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
    d.colorTextureFormat = in_pf;
    d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
    d.inputWidth = iw;
    d.inputHeight = ih;
    d.outputWidth = ow;
    d.outputHeight = oh;
    d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    sc = [d newSpatialScalerWithDevice:g_mtl.dev];
    key = k;
    if (!sc) LOGW("MetalFX spatial scaler %ux%u -> %ux%u is not available (format %lu)", iw, ih, ow, oh, (unsigned long)in_pf);
    return sc;
}
#endif

void mtl_present_compose(id<MTLCommandBuffer> cb, id<MTLTexture> src, const float crop_in[4], float gw, float gh, id<MTLTexture> target,
                         const napple_display_settings *ds, int allow_upscaler) {
    const float sw = (float)src.width, sh = (float)src.height;
    const uint32_t tw = (uint32_t)target.width, th = (uint32_t)target.height;
    if (!tw || !th) return;
    /* source rectangle in source pixels; vflip when the crop height is negative (bottom-up storage) */
    float cx = crop_in[0], cy = crop_in[1], cw = crop_in[2], ch = crop_in[3];
    int vflip = 0;
    if (ch < 0) {
        cy += ch;
        ch = -ch;
        vflip = 1;
    }
    if (cw <= 0 || ch <= 0) return;
    /* aspect of the image in guest units (the actual allocation may be scaled per axis) */
    float img_asp = (gw * cw) / (gh * ch);
    uint32_t dx = 0, dy = 0, dw = tw, dh = th;
    float tgt_asp = (float)tw / (float)th;
    if (ds->fit == NAPPLE_FIT_LETTERBOX) {
        if (fabsf(img_asp - tgt_asp) > 0.001f) {
            if (img_asp > tgt_asp) {
                dw = tw;
                dh = (uint32_t)lroundf((float)tw / img_asp);
            } else {
                dh = th;
                dw = (uint32_t)lroundf((float)th * img_asp);
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            if (dw > tw) dw = tw;
            if (dh > th) dh = th;
            dx = (tw - dw) / 2;
            dy = (th - dh) / 2;
        }
    } else if (ds->fit == NAPPLE_FIT_FILL && fabsf(img_asp - tgt_asp) > 0.001f) {
        if (img_asp > tgt_asp) {
            float keep = tgt_asp / img_asp;
            cx += cw * (1 - keep) / 2;
            cw *= keep;
        } else {
            float keep = img_asp / tgt_asp;
            cy += ch * (1 - keep) / 2;
            ch *= keep;
        }
    }
    const float ox = cx * sw, oy = cy * sh, spx = cw * sw, spy = ch * sh; /* source pixels */
    const float rx = spx / (float)dw, ry = spy / (float)dh;
    int filter = (int)ds->filter;
    id<MTLTexture> in = src;
    uint32_t in_h = (uint32_t)lroundf(spy);
    if (in_h < 1) in_h = 1;

    /* MetalFX spatial upscaling: only when the source is smaller than the destination on both axes and covers the whole texture */
#ifdef NAPPLE_HAVE_METALFX
    if (allow_upscaler && ds->upscaler == 1 && rx < 0.999f && ry < 0.999f && fabsf(cw - 1) < 1e-4f && fabsf(ch - 1) < 1e-4f && !vflip) {
        id<MTLFXSpatialScaler> fx = get_fx(src.pixelFormat, (uint32_t)src.width, (uint32_t)src.height, dw, dh);
        if (fx) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:dw height:dh mipmapped:NO];
            d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            static id<MTLTexture> out;
            if (!out || out.width != dw || out.height != dh) out = [g_mtl.dev newTextureWithDescriptor:d];
            fx.colorTexture = src;
            fx.outputTexture = out;
            [fx encodeToCommandBuffer:cb];
            id<MTLRenderCommandEncoder> e = begin_pass(cb, target, 1);
            [e setRenderPipelineState:g_pso_copy1];
            [e setViewport:(MTLViewport){(double)dx, (double)dy, (double)dw, (double)dh, 0, 1}];
            float org[2] = {(float)dx, (float)dy};
            float full[4] = {0, 0, 1, 1};
            [e setVertexBytes:full length:16 atIndex:0];
            [e setFragmentBytes:org length:8 atIndex:0];
            [e setFragmentTexture:out atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            return;
        }
    }
#endif

    /* 1:1: exact copy */
    int one_to_one = fabsf(rx - 1) < 0.001f && fabsf(ry - 1) < 0.001f;
    if (filter == NAPPLE_FILTER_NEAREST || filter == NAPPLE_FILTER_BILINEAR || (one_to_one && filter != NAPPLE_FILTER_SHARP)) {
        float c[4] = {cx, cy, cw, ch};
        if (vflip) {
            c[1] += c[3];
            c[3] = -c[3];
        }
        id<MTLRenderCommandEncoder> e = begin_pass(cb, target, 1);
        [e setRenderPipelineState:g_pso_pf];
        [e setViewport:(MTLViewport){(double)dx, (double)dy, (double)dw, (double)dh, 0, 1}];
        [e setVertexBytes:c length:16 atIndex:0];
        [e setFragmentTexture:in atIndex:0];
        [e setFragmentSamplerState:g_samp[(filter == NAPPLE_FILTER_NEAREST || one_to_one) ? 1 : 0] atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        return;
    }
    int kernel = filter == NAPPLE_FILTER_BOX ? KERN_BOX : filter == NAPPLE_FILTER_LANCZOS ? KERN_LANCZOS : KERN_CATMULL;
    float radius = kernel == KERN_BOX ? 0.5f : kernel == KERN_LANCZOS ? 3.0f : 2.0f;
    int sharp = filter == NAPPLE_FILTER_SHARP && ds->sharpness > 0.01f;
    /* pass 1 (horizontal): source rows [oy, oy + spy) -> tmp(dw x in_h) ; pass 2 (vertical): tmp -> target (or tmp2 when sharpening follows) */
    id<MTLTexture> t1 = tmp_tex(0, dw, in_h, MTLPixelFormatRGBA16Float);
    {
        RSParams p = {{ox, (float)lroundf(oy)}, {spx, (float)in_h}, {(float)dw, (float)dh}, {0, 0}, rx, radius, kernel, 0, 0, 0};
        id<MTLRenderCommandEncoder> e = begin_pass(cb, t1, 0);
        float full[4] = {0, 0, 1, 1};
        [e setRenderPipelineState:g_pso_rs_f16];
        [e setViewport:(MTLViewport){0, 0, (double)dw, (double)in_h, 0, 1}];
        [e setVertexBytes:full length:16 atIndex:0];
        [e setFragmentBytes:&p length:sizeof p atIndex:0];
        [e setFragmentTexture:in atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    /* the vertical pass reads t1 rows [0, in_h) ; its origin is 0 in t1 coordinates and its x positions are the same as the output x (relative to dest) */
    id<MTLTexture> dst = target;
    uint32_t vx = dx, vy = dy;
    int clear = 1;
    id<MTLTexture> t2 = nil;
    if (sharp) {
        t2 = tmp_tex(1, dw, dh, MTLPixelFormatRGBA16Float);
        dst = t2;
        vx = vy = 0;
        clear = 0;
    }
    {
        RSParams p = {{0, 0}, {(float)dw, (float)in_h}, {(float)dw, (float)dh}, {(float)vx, (float)vy}, ry, radius, kernel, 1, vflip, 0};
        id<MTLRenderCommandEncoder> e = begin_pass(cb, dst, clear);
        float full[4] = {0, 0, 1, 1};
        [e setRenderPipelineState:sharp ? g_pso_rs_f16 : g_pso_rs[1]];
        [e setViewport:(MTLViewport){(double)vx, (double)vy, (double)dw, (double)dh, 0, 1}];
        [e setVertexBytes:full length:16 atIndex:0];
        [e setFragmentBytes:&p length:sizeof p atIndex:0];
        [e setFragmentTexture:t1 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    if (sharp) {
        struct {
            float amount, ox, oy, pad;
        } sp = {ds->sharpness, (float)dx, (float)dy, 0};
        id<MTLRenderCommandEncoder> e = begin_pass(cb, target, 1);
        float full[4] = {0, 0, 1, 1};
        [e setRenderPipelineState:g_pso_sharp];
        [e setViewport:(MTLViewport){(double)dx, (double)dy, (double)dw, (double)dh, 0, 1}];
        [e setVertexBytes:full length:16 atIndex:0];
        [e setFragmentBytes:&sp length:sizeof sp atIndex:0];
        [e setFragmentTexture:t2 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Statistics
 * --------------------------------------------------------------------------------------------------------------- */
static _Atomic uint64_t g_gpu_ns_acc;
void mtl_stats_gpu_add(double ms) { atomic_fetch_add(&g_gpu_ns_acc, (uint64_t)(ms * 1e6)); }

/* ---------------------------------------------------------------------------------------------------------------
 * Present
 * --------------------------------------------------------------------------------------------------------------- */
static char g_shot_path[1024];
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_shot_pending;
static _Atomic int g_hidden; /* napple_gfx_set_hidden */
void napple_gfx_set_hidden(int hidden) { atomic_store(&g_hidden, hidden != 0); }
static id<MTLTexture> g_last_present; /* last presented NVN texture (retained) */
static float g_last_crop[4];
static float g_last_gw, g_last_gh;

static MTLPixelFormat nonsrgb(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
        case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
        default: return f;
    }
}

static void write_png(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, size_t stride) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate((void *)bgra, w, h, 8, stride, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSURL *url = [NSURL fileURLWithPath:@(path)];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        if (!CGImageDestinationFinalize(dst)) LOGE("screenshot: cannot write %s", path);
        CFRelease(dst);
    } else LOGE("screenshot: cannot create %s", path);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

static void compute_crop(const win_t *w, const texsnap *t, float crop[4]) {
    float tw = (float)t->d.width, th = (float)t->d.height;
    const int *c = w->crop;
    if (c[2] > 0 && c[3] > 0) {
        crop[0] = c[0] / tw;
        crop[1] = c[1] / th;
        crop[2] = c[2] / tw;
        crop[3] = c[3] / th;
    } else {
        crop[0] = 0;
        crop[1] = 0;
        crop[2] = 1;
        crop[3] = 1;
    }
}

int gfx_event_shot_due(char *path, size_t n);

static id<MTLTexture> shot_texture(uint32_t w, uint32_t h) {
    MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    sd.storageMode = MTLStorageModeShared;
    sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [g_mtl.dev newTextureWithDescriptor:sd];
}
static void shot_write(id<MTLTexture> shot, NSString *path) {
    uint32_t sw = (uint32_t)shot.width, sh = (uint32_t)shot.height;
    size_t stride = (size_t)sw * 4;
    uint8_t *px = malloc(stride * sh);
    [shot getBytes:px bytesPerRow:stride fromRegion:MTLRegionMake2D(0, 0, sw, sh) mipmapLevel:0];
    write_png(path.UTF8String, px, sw, sh, stride);
    free(px);
}

/* Drop-in replacement of the former mtl_core.m implementation, extended. */
void replay_run_present(win_t *w, int idx, const texsnap *snap, uint32_t interval, uint64_t seq) {
    napple_gfx_settings st;
    gfx_settings_get(&st);
    napple_display_settings ds;
    napple_display_get(&ds);
    uint32_t win_w, win_h;
    double hz;
    display_window_get(&win_w, &win_h, &hz);
    mtl_scale_note_window(snap->d.width, snap->d.height);
    mtl_scale_tick();
    GfxTex *gt = mtl_tex_get(snap, 1);
    id<MTLTexture> src = nil;
    if (gt) {
        MTLPixelFormat pf = nonsrgb(gt->pf);
        src = pf == gt->pf ? gt->tex : [gt->tex newTextureViewWithPixelFormat:pf];
    }
    {
        extern id<MTLTexture> mtl_dbg_composite(void);
        if (getenv("NAPPLE_SHOT_COMPOSITE") && mtl_dbg_composite()) src = mtl_dbg_composite();
    }
    {
        extern id<MTLTexture> mtl_frame_present_pick(uint32_t, uint32_t);
        id<MTLTexture> alt = mtl_frame_present_pick((uint32_t)snap->d.width, (uint32_t)snap->d.height);
        if (alt) {
            src = alt;
            static int once;
            if (!once++)
                LOGW("presenting the orphan render target (%lux%lu) instead of the window texture: the frame's picture was never copied to it",
                     (unsigned long)alt.width, (unsigned long)alt.height);
        }
    }
    uint32_t interval_eff = st.present_interval_override ? st.present_interval_override : (interval ? interval : 1);
    double speed = ds.speed > 0.05f ? ds.speed : 1.0;
    double min_dur = 0;
    if (st.pacing == NAPPLE_PACING_GAME) min_dur = interval_eff / 60.0 / speed;
    else if (st.pacing == NAPPLE_PACING_FIXED && st.target_fps > 0) min_dur = 1.0 / st.target_fps / speed;
    _Atomic uint64_t *cell = &w->displayed[idx];
    uint64_t t_enter = gfx_now_ns();

    /* statistics: game rate */
    static uint64_t last_present_ns;
    static double ema_dt;
    if (last_present_ns) {
        double dt = (double)(t_enter - last_present_ns) / 1e9;
        ema_dt = ema_dt == 0 ? dt : ema_dt * 0.9 + dt * 0.1;
    }
    last_present_ns = t_enter;
    g_display_stats.game_fps = ema_dt > 0 ? 1.0 / ema_dt : 0;
    g_display_stats.game_frames++;
    { /* NAPPLE_PERF_LOG=1: one stats line every 5 s (fps over the window, worst frame gap, GPU/output ms) for long-run slowdown hunts */
        static int perf_on = -1;
        static uint64_t w_start, w_frames, w_max_gap, w_last;
        static double w_gpu_max;
        static uint64_t w_pc, w_st, w_pw, w_ds, w_pb;
        if (perf_on < 0) perf_on = getenv("NAPPLE_PERF_LOG") != NULL;
        if (perf_on) {
            if (!w_start) w_start = t_enter;
            if (w_last && t_enter - w_last > w_max_gap) w_max_gap = t_enter - w_last;
            w_last = t_enter;
            w_frames++;
            if (g_display_stats.gpu_ms > w_gpu_max) w_gpu_max = g_display_stats.gpu_ms;
            if (t_enter - w_start >= 5000000000ull) {
                double secs = (double)(t_enter - w_start) / 1e9;
                uint64_t pc = atomic_load(&g_gfx.pipelines_compiled), st = atomic_load(&g_gfx.shaders_translated), pw = atomic_load(&g_gfx.pipeline_wait_us),
                         ds = atomic_load(&g_gfx.draws_skipped), pb = atomic_load(&g_gfx.pipe_build_us), pbm = atomic_exchange(&g_gfx.pipe_build_max_us, 0);
                fprintf(stderr,
                        "[perf] fps %.1f maxgap_ms %.1f gpu_ms_max %.2f out_ms %.2f shown_fps %.1f frames %llu | pipes+%llu shaders+%llu pipewait_ms+%llu "
                        "skipped+%llu pending %llu | blockbuild_ms+%llu max %llu | cmdrecs live %llu\n",
                        (double)w_frames / secs, (double)w_max_gap / 1e6, w_gpu_max, g_display_stats.output_ms, g_display_stats.out_fps,
                        (unsigned long long)g_display_stats.game_frames, (unsigned long long)(pc - w_pc), (unsigned long long)(st - w_st),
                        (unsigned long long)((pw - w_pw) / 1000), (unsigned long long)(ds - w_ds), (unsigned long long)atomic_load(&g_gfx.shaders_pending),
                        (unsigned long long)((pb - w_pb) / 1000), (unsigned long long)(pbm / 1000), (unsigned long long)gfx_cmdrec_live());
                w_pb = pb;
                w_pc = pc;
                w_st = st;
                w_pw = pw;
                w_ds = ds;
                w_start = t_enter;
                w_frames = 0;
                w_max_gap = 0;
                w_gpu_max = 0;
            }
        }
    }
    g_display_stats.gpu_ms = (double)atomic_exchange(&g_gpu_ns_acc, 0) / 1e6;
    g_display_stats.base_w = snap->d.width;
    g_display_stats.base_h = snap->d.height;
    if (gt) {
        g_display_stats.render_w = (uint32_t)gt->tex.width;
        g_display_stats.render_h = (uint32_t)gt->tex.height;
        g_display_stats.scale_x = gt->rsx;
        g_display_stats.scale_y = gt->rsy;
    }

    float crop[4];
    compute_crop(w, snap, crop);
    float gw = (float)snap->d.width, gh = (float)snap->d.height;
    if (g_gfx.origin_flip && g_gfx.device_origin == 0) {
        crop[1] += crop[3];
        crop[3] = -crop[3];
    } /* rows are stored bottom-up: flip for the display */
    if (src) {
        g_last_present = src;
        memcpy(g_last_crop, crop, sizeof crop);
        g_last_gw = gw;
        g_last_gh = gh;
    }

    /* Frames beyond the display rate (game speed above 1x, uncapped fixed rates) are not shown: they would only queue up behind the display. */
    const double disp_period = 1.0 / (hz > 1 ? hz : 60.0);
    static uint64_t last_shown_ns;
    int drop = atomic_load(&g_hidden) != 0; /* napple_gfx_set_hidden: a library applet owns the screen */
    if (g_mtl.layer && st.pacing != NAPPLE_PACING_UNCAPPED && min_dur > 0 && min_dur < disp_period * 0.92 && last_shown_ns &&
        (double)(t_enter - last_shown_ns) / 1e9 < disp_period * 0.92)
        drop = 1;
    if (!drop && !atomic_load(&g_shot_pending) && mtl_interp_active(&ds)) { /* frame generation consumes the frame: it shows it and the generated ones */
        if (src && mtl_interp_submit(&ds, src, crop, gw, gh, win_w, win_h, hz, interval_eff, speed, w, idx, seq, t_enter)) return;
    }

    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    id<CAMetalDrawable> dr = nil;
    id<MTLTexture> target = g_mtl.headless;
    if (g_mtl.layer && !drop) {
        if (st.output_width && st.output_height) g_mtl.layer.drawableSize = CGSizeMake(st.output_width, st.output_height);
        dr = [g_mtl.layer nextDrawable];
        target = dr.texture;
    }
    if (src && target && !drop) mtl_present_compose(cb, src, crop, gw, gh, target, &ds, 1);
    if (drop) { g_display_stats.dropped_frames++; }

    {
        char ep[1024];
        if (gfx_event_shot_due(ep, sizeof ep)) napple_gfx_screenshot(ep);
    } /* NAPPLE_GFX_EVENT_SHOTS diagnostics */
    /* screenshot of this frame */
    id<MTLTexture> shot = nil;
    char path[1024] = "";
    if (src && atomic_exchange(&g_shot_pending, 0)) {
        pthread_mutex_lock(&g_shot_mu);
        snprintf(path, sizeof path, "%s", g_shot_path);
        pthread_mutex_unlock(&g_shot_mu);
        uint32_t sw = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.width : (uint32_t)(target ? target.width : g_mtl.out_w),
                 sh2 = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.height : (uint32_t)(target ? target.height : g_mtl.out_h);
        shot = shot_texture(sw, sh2);
        mtl_present_compose(cb, src, crop, gw, gh, shot, &ds, 1);
    }
    NSString *shot_path = path[0] ? @(path) : nil;
    uint64_t t0 = gfx_now_ns();
    __block BOOL shown = NO;
    void (^finish_display)(void) = ^{
        if (shown) return;
        shown = YES;
        atomic_fetch_add(cell, 1);
        atomic_fetch_add(&g_gfx.frames, 1);
        gfx_wake();
    };
    if (dr) {
        last_shown_ns = gfx_now_ns();
        if (min_dur > 0 && min_dur >= disp_period * 0.92) napple_present_after(cb, dr, min_dur);
        else [cb presentDrawable:dr];
        napple_drawable_presented(cb, dr, ^(id<MTLDrawable> d) {
            (void)d;
            finish_display();
        });
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        atomic_store(&g_frame_gpu_us, (gfx_now_ns() - t0) / 1000);
        g_display_stats.output_ms = (c.GPUEndTime - c.GPUStartTime) * 1000.0;
        if (shot) shot_write(shot, shot_path);
        if (!dr) { /* headless (or dropped): "shown" at the pacing rate, so the guest runs at its real speed without a display */
            /* The wait is a timer, not a sleep in this handler: Metal runs the completion handlers of every command buffer on one thread, and a sleeping handler held
             * back the completion (and so the fences) of the next frame's drawing work, adding the whole sleep to every frame (MK8D: 46 fps instead of 60). */
            static uint64_t last_due;
            static dispatch_queue_t pq;
            static dispatch_once_t once;
            dispatch_once(&once, ^{
                pq = dispatch_queue_create("napple.pacing", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
            });
            uint64_t now = gfx_now_ns(), due = now;
            if (min_dur > 0 && !drop) {
                due = last_due + (uint64_t)(min_dur * 1e9);
                if (due < now) due = now;
            } /* a late frame is not followed by a burst */
            last_due = due;
            if (g_prof_on && due > now) atomic_fetch_add(&g_prof[3], due - now);
            if (due - now < 200000) {
                finish_display();
                gfx_timeline_complete(seq);
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(due - now)), pq, ^{
                finish_display();
                gfx_timeline_complete(seq);
            });
            return;
        }
        gfx_timeline_complete(seq);
    }];
    [cb commit];
    /* shown-rate statistic */
    {
        static uint64_t last_out_ns;
        static double ema_o;
        if (!drop) {
            uint64_t n = gfx_now_ns();
            if (last_out_ns) {
                double dt = (double)(n - last_out_ns) / 1e9;
                ema_o = ema_o == 0 ? dt : ema_o * 0.9 + dt * 0.1;
            }
            last_out_ns = n;
            g_display_stats.out_fps = ema_o > 0 ? 1.0 / ema_o : 0;
            g_display_stats.shown_frames++;
        }
    }
    mtl_tex_gc(atomic_load(&g_gfx.frames));
}

int napple_gfx_screenshot(const char *png_path) {
    if (!png_path) return -1;
    pthread_mutex_lock(&g_shot_mu);
    snprintf(g_shot_path, sizeof g_shot_path, "%s", png_path);
    pthread_mutex_unlock(&g_shot_mu);
    atomic_store(&g_shot_pending, 1);
    return 0;
}
int napple_gfx_screenshot_pending(void) { return atomic_load(&g_shot_pending); }
int napple_gfx_screenshot_last(const char *png_path) {
    @autoreleasepool {
        gfx_mtl_finish();
        id<MTLTexture> src = g_last_present;
        if (!src || !png_path) return -1;
        uint32_t w = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.width : g_mtl.out_w,
                 h = g_mtl.layer ? (uint32_t)g_mtl.layer.drawableSize.height : g_mtl.out_h;
        napple_display_settings ds;
        napple_display_get(&ds);
        id<MTLTexture> shot = shot_texture(w, h);
        id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
        mtl_present_compose(cb, src, g_last_crop, g_last_gw, g_last_gh, shot, &ds, 1);
        [cb commit];
        [cb waitUntilCompleted];
        shot_write(shot, @(png_path));
        return 0;
    }
}
