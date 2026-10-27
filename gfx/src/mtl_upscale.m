/* mtl_upscale.m - Napple's own spatial upscaler (used when MetalFX is absent or not preferred): edge adaptive upsampling plus a limited-lobe sharpening pass.
 *
 * Original implementation (no code from other upscalers); the ideas are the usual ones for a single image, single pass upscaler:
 *   pass 1 (edge adaptive resample): for every output pixel the 4x4 source texels around it are read, the local luma structure tensor gives the edge
 *     direction and how coherent it is, and the texels are weighted with a Lanczos-2 kernel evaluated in a frame aligned with the edge (narrowed across
 *     the edge, widened along it). Stair steps of slanted edges are smoothed instead of being enlarged, and the result is clamped to the range of the four
 *     nearest texels so that no ringing appears.
 *   pass 2 (sharpen): a negative lobe filter over the 4 neighbours whose strength shrinks where the neighbourhood is already near black or white, and
 *     whose result is clamped to the neighbourhood range (no halos). `sharpness` 0..1 scales the lobe; 0 skips the pass.
 * Used for upscaling only (source smaller than the destination on both axes). mtl_present.m falls back to the separable resamplers otherwise. */
#import "mtl_display.h"

static NSString *const kUpscaleMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V uv_vs(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
     /* org / span: source rectangle in source pixels; ratio: source pixels per output pixel; vp: viewport origin; out_h for the vertical flip */
     "struct UP { float2 org; float2 span; float2 ratio; float2 vp; float out_h; int flip; float sharp; float pad; };\n"
     "inline float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
     "inline float sinc1(float x) { x = fabs(x); if (x < 1e-4) return 1.0; float t = M_PI_F * x; return sin(t) / t; }\n"
     "inline float lanczos2(float r) { return r >= 2.0 ? 0.0 : sinc1(r) * sinc1(r * 0.5); }\n"
     "fragment float4 easu(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant UP &p [[buffer(0)]]) {\n"
     "  float2 px = in.pos.xy - p.vp;\n"
     "  if (p.flip != 0) px.y = p.out_h - 1.0 - px.y;\n"
     "  float2 sp = (px + 0.5) * p.ratio - 0.5;\n"
     "  float2 b = floor(sp), f = sp - b;\n"
     "  float3 c[4][4]; float l[4][4];\n"
     "  float2 hi = p.span - 1.0;\n"
     "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
     "    float2 q = clamp(b + float2(i - 1, j - 1), float2(0.0), hi) + p.org;\n"
     "    c[j][i] = tex.read(uint2(q)).rgb; l[j][i] = luma(c[j][i]); }\n"
     /* structure tensor over the four centre texels, weighted by the bilinear weights of the sample position */
     "  float jxx = 0, jyy = 0, jxy = 0;\n"
     "  for (int j = 1; j <= 2; j++) for (int i = 1; i <= 2; i++) {\n"
     "    float w = (i == 1 ? 1.0 - f.x : f.x) * (j == 1 ? 1.0 - f.y : f.y);\n"
     "    float gx = 0.5 * (l[j][i + 1] - l[j][i - 1]), gy = 0.5 * (l[j + 1][i] - l[j - 1][i]);\n"
     "    jxx += w * gx * gx; jyy += w * gy * gy; jxy += w * gx * gy; }\n"
     "  float tr = jxx + jyy; float2 n = float2(1.0, 0.0); float coh = 0.0;\n"
     "  if (tr > 2e-4) {\n"
     "    float th = 0.5 * atan2(2.0 * jxy, jxx - jyy); n = float2(cos(th), sin(th));\n"
     "    coh = sqrt((jxx - jyy) * (jxx - jyy) + 4.0 * jxy * jxy) / tr; coh *= smoothstep(2e-4, 4e-3, tr); }\n"
     "  float2 t = float2(-n.y, n.x);\n"
     "  float kn = 1.0 + 0.45 * coh, kt = 1.0 - 0.4 * coh;\n"
     "  float3 acc = 0; float ws = 0;\n"
     "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
     "    float2 d = float2(float(i - 1) - f.x, float(j - 1) - f.y);\n"
     "    float r = length(float2(dot(d, n) * kn, dot(d, t) * kt));\n"
     "    float w = lanczos2(r); acc += w * c[j][i]; ws += w; }\n"
     "  float3 o = acc / max(ws, 1e-4);\n"
     "  float3 mn = min(min(c[1][1], c[1][2]), min(c[2][1], c[2][2])), mx = max(max(c[1][1], c[1][2]), max(c[2][1], c[2][2]));\n"
     "  return float4(clamp(o, mn, mx), 1.0);\n"
     "}\n"
     "struct SP { float amount; float ox; float oy; float pad; };\n"
     "fragment float4 lobe(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant SP &p [[buffer(0)]]) {\n"
     "  int2 q = int2(in.pos.xy - float2(p.ox, p.oy)); int2 m = int2(tex.get_width() - 1, tex.get_height() - 1);\n"
     "  float3 c = tex.read(uint2(clamp(q, int2(0), m))).rgb, n = tex.read(uint2(clamp(q + int2(0, -1), int2(0), m))).rgb,\n"
     "         s = tex.read(uint2(clamp(q + int2(0, 1), int2(0), m))).rgb, e = tex.read(uint2(clamp(q + int2(1, 0), int2(0), m))).rgb,\n"
     "         w = tex.read(uint2(clamp(q + int2(-1, 0), int2(0), m))).rgb;\n"
     "  float3 mn = min(min(min(n, s), min(e, w)), c), mx = max(max(max(n, s), max(e, w)), c);\n"
     /* headroom to black and to white relative to the local peak: the lobe may not push the result outside [0, 1] */
     "  float3 room = min(mn, 1.0 - mx) / max(mx, 1e-3);\n"
     "  float3 lobe_w = -clamp(room, 0.0, 0.375) * (0.5 * clamp(p.amount, 0.0, 1.0));   /* at most -0.1875 per neighbour */\n"
     "  float3 r = (lobe_w * (n + s + e + w) + c) / (1.0 + 4.0 * lobe_w);\n"
     "  return float4(clamp(r, mn, mx), 1.0);\n"
     "}\n"
     "fragment float4 plain(V in [[stage_in]], texture2d<float> tex [[texture(0)]], constant float2 &origin [[buffer(0)]]) {\n"
     "  return float4(tex.read(uint2(in.pos.xy - origin)).rgb, 1.0); }\n";

static id<MTLRenderPipelineState> g_easu_f16, g_lobe_bgra, g_plain_bgra;
static int g_ready;

static id<MTLRenderPipelineState> mk(id<MTLLibrary> lib, NSString *fs, MTLPixelFormat pf) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"uv_vs"];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = pf;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGE("upscaler pipeline %s: %s", fs.UTF8String, err.localizedDescription.UTF8String);
    return p;
}

static int upscale_init(void) {
    if (g_ready) return g_ready > 0 ? 0 : -1;
    NSError *err = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:kUpscaleMSL options:nil error:&err];
    if (!lib) {
        LOGE("upscaler shader: %s", err.localizedDescription.UTF8String);
        g_ready = -1;
        return -1;
    }
    g_easu_f16 = mk(lib, @"easu", MTLPixelFormatRGBA16Float);
    g_lobe_bgra = mk(lib, @"lobe", MTLPixelFormatBGRA8Unorm);
    g_plain_bgra = mk(lib, @"plain", MTLPixelFormatBGRA8Unorm);
    g_ready = g_easu_f16 && g_lobe_bgra && g_plain_bgra ? 1 : -1;
    return g_ready > 0 ? 0 : -1;
}

static id<MTLTexture> mid_tex(uint32_t w, uint32_t h) {
    static id<MTLTexture> t;
    if (!t || t.width != w || t.height != h) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        t = [g_mtl.dev newTextureWithDescriptor:d];
    }
    return t;
}

int mtl_upscale_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, const float rect[4], id<MTLTexture> target, const uint32_t dst[4], int vflip, float sharpness) {
    if (upscale_init() != 0) return 0;
    const uint32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
    if (!dw || !dh || rect[2] < 1 || rect[3] < 1) return 0;
    id<MTLTexture> mid = mid_tex(dw, dh);
    if (!mid) return 0;
    struct {
        float org[2], span[2], ratio[2], vp[2], out_h;
        int flip;
        float sharp, pad;
    } p = {{rect[0], rect[1]}, {rect[2], rect[3]}, {rect[2] / (float)dw, rect[3] / (float)dh}, {0, 0}, (float)dh, vflip, sharpness, 0};
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = mid;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g_easu_f16];
        [e setViewport:(MTLViewport){0, 0, (double)dw, (double)dh, 0, 1}];
        [e setFragmentBytes:&p length:sizeof p atIndex:0];
        [e setFragmentTexture:src atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setViewport:(MTLViewport){(double)dx, (double)dy, (double)dw, (double)dh, 0, 1}];
        if (sharpness > 0.01f) {
            struct {
                float amount, ox, oy, pad;
            } sp = {sharpness, (float)dx, (float)dy, 0};
            [e setRenderPipelineState:g_lobe_bgra];
            [e setFragmentBytes:&sp length:sizeof sp atIndex:0];
        } else {
            float org[2] = {(float)dx, (float)dy};
            [e setRenderPipelineState:g_plain_bgra];
            [e setFragmentBytes:org length:8 atIndex:0];
        }
        [e setFragmentTexture:mid atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    return 1;
}
