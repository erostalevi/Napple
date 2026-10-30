/* mtl_stages.inc - replay of transform feedback, geometry shaders and tessellation (included by mtl_replay.m; contract: docs/architecture/gfx.md section 13).
 *
 * Metal has no geometry stage and no transform feedback, and its tessellator takes factors from a buffer. A draw that needs any of them runs as passes:
 *  1. vertex capture: the vertex program with ShaderKey.stage_out (and / or the xfb entries) over the draw's vertex range, rasterization off, drawn as points
 *     in its own attachment-less render pass. Record ordinal xo = range * instance + (vertex id - lo) (xfb_ctl[0] = range, xfb_ctl[1] = lo + range * base
 *     instance, as [[instance_id]] counts from the base instance).
 *  2. assembly: the CPU expands the draw (indices, base vertex, primitive restart, strips / fans / loops / adjacency / patches) into a list of record ordinals;
 *     a gather kernel copies those records into the list layout the next stage reads (stage_in, or tess_vtx without a tess control program).
 *  3. geometry: the GS kernel (one thread per primitive) writes gs_vtx / gs_idx; the draw renders gs_idx as restart-separated strips (points: pulled through
 *     gs_idx, unused slots land outside the clip volume) with a pass-through vertex function.
 *     tessellation: the TCS kernel (one threadgroup per patch) writes tess_vtx / tess_patch / tess_fac (pre-filled with the NVN default levels); without a TCS
 *     the replay fills them. drawPatches runs the tess eval program as a post-tessellation vertex function; isolines, a capture of tess eval output and a
 *     geometry program after tessellation go through the software tessellator instead (see draw_tessellation).
 *  4. transform feedback without a geometry / tessellation stage: a plain list draw (points, lines, triangles; non-indexed; whole primitives that fit) captures
 *     straight from the vertex program into the xfb buffers; any other draw captures records and a copy kernel writes the decomposed primitives in order.
 *     The written byte counts are kept here per buffer (CPU side: every count is known when the draw is encoded) and stored to the game's control memory
 *     at End / Pause, read back at Resume and by DrawTransformFeedback.
 * Indirect draws read their arguments at encode time (mtl_replay.m). Not covered: MultiDraw*IndirectCount with these stages (drawn without them by
 * mtl_replay.m), point-mode tessellation, the hardware's ring triangulation of unequal levels in the software tessellator. The primitive id runs on
 * across instances. */

/* Transform feedback control memory, Napple's own layout (the guest only hands it back to us): u32 bytes written per buffer [0..3], u32 vertex stride per
 * buffer [4..7] of the program that wrote them (DrawTransformFeedback draws bytes[0] / stride[0] vertices). */
/* Two kinds of session: CPU-counted (every draw's vertex count is known at encode time: the vertex stage captures) and GPU-counted, entered by the first
 * capture of geometry shader output (how many vertices a geometry program emits is only known on the GPU): from then on the counts live in a small
 * counter buffer (cnt: u32 bytes[4], stride[4], the control memory layout) that the capture kernels advance, End / Pause blit it into the control memory,
 * Resume of a control memory last written by the GPU blits it back, and DrawTransformFeedback of such a control memory draws indirectly. */
static struct {
    int active, paused;
    uint32_t bytes[MAX_XFB_BUFFERS], stride[MAX_XFB_BUFFERS];
    int gpu;                               /* GPU-counted session: `cnt` holds the counts, bytes[] / stride[] are stale */
    __unsafe_unretained id<MTLBuffer> cnt; /* 32 bytes, shared storage (xfb_cnt_new) */
} g_xfb;
static uint64_t g_xfb_gpu_ctl; /* control memory address whose contents the GPU writes (End / Pause of a GPU-counted session); 0 = none */

static uint32_t *xfb_ctl_mem(uint64_t a) { return (a >= 0x100000 && !(a & 3)) ? (uint32_t *)(uintptr_t)a : NULL; }
static id<MTLBuffer> xfb_cnt_new(void) {
    static id<MTLBuffer> pool[64];
    static unsigned i;
    unsigned k = i++ & 63;
    if (!pool[k]) pool[k] = [g_mtl.dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    return pool[k];
}
/* 32-byte copy between the counter buffer and the control memory (which must sit in a memory pool) on the GPU timeline. */
static void xfb_cnt_blit(id<MTLBuffer> cnt, uint64_t control, int to_control) {
    id<MTLBuffer> b;
    uint64_t o;
    if (!cnt || !mtl_addr(control, 32, &b, &o)) {
        LOG_ONCE(NAPPLE_LOG_WARN, "transform feedback control memory %#llx is not inside a memory pool: GPU-side counts cannot reach it", (unsigned long long)control);
        return;
    }
    id<MTLBlitCommandEncoder> be = blit();
    if (to_control) [be copyFromBuffer:cnt sourceOffset:0 toBuffer:b destinationOffset:o size:32];
    else [be copyFromBuffer:b sourceOffset:o toBuffer:cnt destinationOffset:0 size:32];
}
/* Switches the session to GPU counting (the first geometry capture): the CPU counts so far seed the counter buffer. */
static void xfb_go_gpu(void) {
    if (g_xfb.gpu) return;
    g_xfb.cnt = xfb_cnt_new();
    uint32_t *w = (uint32_t *)g_xfb.cnt.contents;
    memcpy(w, g_xfb.bytes, 16);
    memcpy(w + 4, g_xfb.stride, 16);
    g_xfb.gpu = 1;
    LOG_ONCE(NAPPLE_LOG_INFO, "transform feedback of geometry shader output: the session's counts move to the GPU (control memory written on the GPU timeline)");
}
static void do_xfb(const p_xfb *x) {
    uint32_t *c = xfb_ctl_mem(x->control);
    switch (x->op) {
        case XFB_BEGIN:
            memset(&g_xfb, 0, sizeof g_xfb);
            g_xfb.active = 1;
            if (c) memset(c, 0, 32);
            if (g_xfb_gpu_ctl == x->control) g_xfb_gpu_ctl = 0;
            break;
        case XFB_RESUME:
            if (x->control && g_xfb_gpu_ctl == x->control) { /* the counts are on the GPU: fetch them there */
                g_xfb.cnt = xfb_cnt_new();
                g_xfb.gpu = 1;
                xfb_cnt_blit(g_xfb.cnt, x->control, 0);
            } else if (c) {
                memcpy(g_xfb.bytes, c, sizeof g_xfb.bytes);
                memcpy(g_xfb.stride, c + 4, sizeof g_xfb.stride);
                g_xfb.gpu = 0;
            }
            g_xfb.active = 1;
            g_xfb.paused = 0;
            break;
        case XFB_PAUSE:
        case XFB_END:
            if (g_xfb.gpu) {
                xfb_cnt_blit(g_xfb.cnt, x->control, 1);
                g_xfb_gpu_ctl = x->control;
            } else if (c) {
                memcpy(c, g_xfb.bytes, sizeof g_xfb.bytes);
                memcpy(c + 4, g_xfb.stride, sizeof g_xfb.stride);
                if (g_xfb_gpu_ctl == x->control) g_xfb_gpu_ctl = 0;
            }
            g_xfb.paused = x->op == XFB_PAUSE;
            if (x->op == XFB_END) g_xfb.active = 0;
            break;
    }
}
static int xfb_capturing(void) { return g_xfb.active && !g_xfb.paused; }
static int xfb_gpu_control(uint64_t control) { return control && g_xfb_gpu_ctl == control; }

/* SPH words (after the 0x30-byte NVN header) of a graphics program */
static const uint32_t *sph_of(uint64_t hash) {
    const shader_blob *b = hash ? shader_blob_find(hash) : NULL;
    return b && b->code_size >= 0x80 ? (const uint32_t *)(b->code + 0x30) : NULL;
}

typedef struct {
    uint32_t mode, first, count, base_instance, instances, idx_type;
    int indexed;
    int32_t base_vertex;
    uint64_t idx_addr;
} sdraw;

static int g_no_stages; /* NAPPLE_GFX_NO_STAGES=1: draws ignore geometry / tessellation programs (the behaviour before this file; debugging) */
/* Whether a draw needs the passes of this file. */
static int stage_draw_needed(uint32_t mode) {
    if (g_no_stages) return 0; return S.prog[2] || S.prog[3] || S.prog[4] || mode == NVN_DRAW_PRIMITIVE_PATCHES; }

#define REC_RESTART 0xFFFFFFFFu
/* The draw's vertex ids in element order (primitive restart -> REC_RESTART); lo / hi = their range. Returns the element count, 0 on failure. */
static uint32_t draw_elements(const sdraw *d, uint32_t **out, uint32_t *lo, uint32_t *hi) {
    uint32_t n = d->count, *e = n ? malloc((size_t)n * 4) : NULL;
    *out = e;
    if (!e) return 0;
    uint32_t mn = ~0u, mx = 0;
    const uint8_t *src = (const uint8_t *)(uintptr_t)d->idx_addr;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v;
        if (d->indexed) {
            uint32_t raw = d->idx_type == 0 ? src[i] : d->idx_type == 1 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
            if (S.prim_restart_en && raw == S.prim_restart_idx) {
                e[i] = REC_RESTART;
                continue;
            }
            v = (uint32_t)((int64_t)raw + d->base_vertex);
        } else v = d->first + i;
        e[i] = v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mn > mx) { /* nothing but restarts */
        free(e);
        *out = NULL;
        return 0;
    }
    *lo = mn;
    *hi = mx;
    return n;
}

/* Vertices of one primitive of a draw mode as the next stage sees it (geometry input, tessellation patch, transform feedback output); 0 = not assemblable. */
static uint32_t prim_vertices(uint32_t mode, int for_gs) {
    switch (mode) {
        case NVN_DRAW_PRIMITIVE_POINTS: return 1;
        case NVN_DRAW_PRIMITIVE_LINES:
        case NVN_DRAW_PRIMITIVE_LINE_LOOP:
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return 2;
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return for_gs ? 4 : 0;
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return for_gs ? 6 : 0;
        case NVN_DRAW_PRIMITIVE_PATCHES: return S.patch_size ? S.patch_size : 3;
        default: return 3;
    }
}

/* Expands elements e[0..n) into primitives of `nv` vertex ids each (strips, fans and loops restart at REC_RESTART). Returns the ids written (a multiple of nv). */
static uint32_t assemble(uint32_t mode, const uint32_t *e, uint32_t n, uint32_t nv, uint32_t *out) {
    uint32_t w = 0;
    for (uint32_t s = 0; s < n;) {
        uint32_t t = s;
        while (t < n && e[t] != REC_RESTART) t++;
        const uint32_t *v = e + s;
        uint32_t c = t - s;
#define P1(a) out[w++] = v[a]
        switch (mode) {
            case NVN_DRAW_PRIMITIVE_POINTS:
                for (uint32_t i = 0; i < c; i++) P1(i);
                break;
            case NVN_DRAW_PRIMITIVE_LINES:
                for (uint32_t i = 0; i + 1 < c; i += 2) { P1(i); P1(i + 1); }
                break;
            case NVN_DRAW_PRIMITIVE_LINE_STRIP:
            case NVN_DRAW_PRIMITIVE_LINE_LOOP:
                for (uint32_t i = 0; i + 1 < c; i++) { P1(i); P1(i + 1); }
                if (mode == NVN_DRAW_PRIMITIVE_LINE_LOOP && c > 2) { P1(c - 1); P1(0); }
                break;
            case NVN_DRAW_PRIMITIVE_TRIANGLES:
                for (uint32_t i = 0; i + 2 < c; i += 3) { P1(i); P1(i + 1); P1(i + 2); }
                break;
            case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: /* odd triangles swap their first two vertices (winding kept) */
                for (uint32_t i = 0; i + 2 < c; i++) {
                    if (i & 1) { P1(i + 1); P1(i); }
                    else { P1(i); P1(i + 1); }
                    P1(i + 2);
                }
                break;
            case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
            case NVN_DRAW_PRIMITIVE_POLYGON:
                for (uint32_t i = 1; i + 1 < c; i++) { P1(0); P1(i); P1(i + 1); }
                break;
            case NVN_DRAW_PRIMITIVE_QUADS:
                for (uint32_t i = 0; i + 3 < c; i += 4) { P1(i); P1(i + 1); P1(i + 2); P1(i); P1(i + 2); P1(i + 3); }
                break;
            case NVN_DRAW_PRIMITIVE_QUAD_STRIP:
                for (uint32_t i = 0; i + 3 < c; i += 2) { P1(i); P1(i + 1); P1(i + 3); P1(i); P1(i + 3); P1(i + 2); }
                break;
            case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY:
                for (uint32_t i = 0; i + 3 < c; i += 4) for (uint32_t k = 0; k < 4; k++) P1(i + k);
                break;
            case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY:
                for (uint32_t i = 0; i + 3 < c; i++) for (uint32_t k = 0; k < 4; k++) P1(i + k);
                break;
            case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY:
                for (uint32_t i = 0; i + 5 < c; i += 6) for (uint32_t k = 0; k < 6; k++) P1(i + k);
                break;
            case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: { /* GL 4.6 table 10.1 (1-based there): triangle i of n, as (v0, adj01, v1, adj12, v2, adj20) */
                uint32_t np = c >= 6 ? (c - 4) / 2 : 0;
                for (uint32_t i = 0; i < np; i++) {
                    int first = i == 0, last = i + 1 == np, odd = i & 1;
                    uint32_t a, b, d, e, f, g; /* 1-based vertex numbers */
                    if (first) { a = 1; b = 2; d = 3; e = last ? 6 : 7; f = 5; g = 4; }
                    else if (odd) { a = 2 * i + 3; b = 2 * i - 1; d = 2 * i + 1; e = 2 * i + 4; f = 2 * i + 5; g = last ? 2 * i + 6 : 2 * i + 7; }
                    else { a = 2 * i + 1; b = 2 * i - 1; d = 2 * i + 3; e = last ? 2 * i + 6 : 2 * i + 7; f = 2 * i + 5; g = 2 * i + 4; }
                    P1(a - 1); P1(b - 1); P1(d - 1); P1(e - 1); P1(f - 1); P1(g - 1);
                }
                break;
            }
            case NVN_DRAW_PRIMITIVE_PATCHES:
                for (uint32_t i = 0; i + nv <= c; i += nv) for (uint32_t k = 0; k < nv; k++) P1(i + k);
                break;
            default: break;
        }
#undef P1
        s = t + 1;
    }
    return w - w % (nv ? nv : 1);
}
static uint32_t assemble_max(uint32_t mode, uint32_t n, uint32_t nv) {
    switch (mode) {
        case NVN_DRAW_PRIMITIVE_LINE_STRIP:
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return 2 * n + 2;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
        case NVN_DRAW_PRIMITIVE_POLYGON:
        case NVN_DRAW_PRIMITIVE_QUADS:
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return 3 * n + 6;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return 4 * n;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return 3 * n + 6;
        default: return n + nv;
    }
}

/* ---- helper kernels: record gather and the transform feedback copy -------------------------------------------- */
static NSString *const kStageMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void gather(device const float4 *src [[buffer(0)]], device float4 *dst [[buffer(1)]], device const uint *seq [[buffer(2)]],\n"
     "                   constant uint &n [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
     "  if (i >= n) return; device const float4 *s = src + seq[i] * 34u; device float4 *d = dst + i * 34u;\n"
     "  for (uint k = 0u; k < 34u; k++) d[k] = s[k]; }\n"
     /* t: [0] vertices, [1] entries, [2..5] start of each buffer in words, [6..9] stride in words, [10..11] stored slot mask (lo, hi),
      * then 4 words per entry: attribute address, components, buffer, word offset */
     "kernel void xfbcopy(device const float4 *rec [[buffer(0)]], device const uint *seq [[buffer(1)]], constant uint *t [[buffer(2)]],\n"
     "                    device uint *x0 [[buffer(3)]], device uint *x1 [[buffer(4)]], device uint *x2 [[buffer(5)]], device uint *x3 [[buffer(6)]],\n"
     "                    uint i [[thread_position_in_grid]]) {\n"
     "  if (i >= t[0]) return; device const float4 *r = rec + seq[i] * 34u; ulong mask = ulong(t[10]) | (ulong(t[11]) << 32);\n"
     "  for (uint e = 0u; e < t[1]; e++) { constant uint *x = t + 12u + 4u * e; uint b = x[2];\n"
     "    for (uint j = 0u; j < x[1]; j++) { uint a = x[0] + 4u * j; uint sl = a < 0x70u ? 0u : a < 0x80u ? 1u : 2u + (a - 0x80u) / 16u;\n"
     "      uint v = (a >= 0x60u && a < 0x280u && ((mask >> sl) & 1ul) != 0ul) ? as_type<uint>(r[sl][(a >> 2) & 3u]) : 0u;\n"
     "      uint w = t[2u + b] + i * t[6u + b] + x[3] + j;\n"
     "      if (b == 0u) x0[w] = v; else if (b == 1u) x1[w] = v; else if (b == 2u) x2[w] = v; else x3[w] = v; } } }\n"
     /* GPU-counted capture (one thread, in order): records + a sequence (mode 0: record ordinals, nv per primitive; mode 1: gs_idx, restart-separated
      * strips / points of topology t[13]) -> the whole primitives that fit, counts advanced in cnt. t: [0] sequence length, [1] entries, [2..5] buffer
      * capacity in words, [6..9] stride in words, [10..11] stored slot mask, [12] mode, [13] nv (mode 0) / output topology (mode 1), then 4 words per entry */
     "kernel void xfbgpu(device const float4 *rec [[buffer(0)]], device const uint *seq [[buffer(1)]], constant uint *t [[buffer(2)]],\n"
     "                   device uint *x0 [[buffer(3)]], device uint *x1 [[buffer(4)]], device uint *x2 [[buffer(5)]], device uint *x3 [[buffer(6)]],\n"
     "                   device uint *cnt [[buffer(7)]], uint tid [[thread_position_in_grid]]) {\n"
     "  if (tid != 0u) return; ulong mask = ulong(t[10]) | (ulong(t[11]) << 32); uint start[4]; uint room = 0xFFFFFFFFu, used = 0u;\n"
     "  for (uint e = 0u; e < t[1]; e++) used |= 1u << t[16u + 4u * e];\n"
     "  for (uint k = 0u; k < 4u; k++) { start[k] = cnt[k] / 4u; if (((used >> k) & 1u) != 0u) { uint left = t[2u + k] > start[k] ? t[2u + k] - start[k] : 0u;\n"
     "    room = min(room, t[6u + k] != 0u ? left / t[6u + k] : 0u); } }\n"
     "  uint mode = t[12], nv = mode == 0u ? t[13] : (t[13] == 1u ? 1u : t[13] == 2u ? 2u : 3u); uint out = 0u, c = 0u, p1 = 0u, p2 = 0u, prim[3];\n"
     "  for (uint i = 0u; i < t[0]; i++) { uint r = seq[i]; uint have = 0u;\n"
     "    if (mode == 1u && r == 0xFFFFFFFFu) { c = 0u; continue; }\n"
     "    if (mode == 0u) { prim[c] = r; c++; if (c == nv) { have = nv; c = 0u; } }\n"
     "    else if (nv == 1u) { prim[0] = r; have = 1u; }\n"
     "    else if (nv == 2u) { c++; if (c >= 2u) { prim[0] = p1; prim[1] = r; have = 2u; } p1 = r; }\n"
     "    else { c++; if (c >= 3u) { bool odd = ((c - 3u) & 1u) != 0u; prim[0] = odd ? p1 : p2; prim[1] = odd ? p2 : p1; prim[2] = r; have = 3u; } p2 = p1; p1 = r; }\n"
     "    if (have == 0u) continue; if (out + have > room) break;\n"
     "    for (uint v = 0u; v < have; v++) { device const float4 *rr = rec + prim[v] * 34u;\n"
     "      for (uint e = 0u; e < t[1]; e++) { constant uint *x = t + 14u + 4u * e; uint b = x[2];\n"
     "        for (uint j = 0u; j < x[1]; j++) { uint a = x[0] + 4u * j; uint sl = a < 0x70u ? 0u : a < 0x80u ? 1u : 2u + (a - 0x80u) / 16u;\n"
     "          uint val = (a >= 0x60u && a < 0x280u && ((mask >> sl) & 1ul) != 0ul) ? as_type<uint>(rr[sl][(a >> 2) & 3u]) : 0u;\n"
     "          uint w = start[b] + out * t[6u + b] + x[3] + j;\n"
     "          if (b == 0u) x0[w] = val; else if (b == 1u) x1[w] = val; else if (b == 2u) x2[w] = val; else x3[w] = val; } }\n"
     "      out++; } }\n"
     "  for (uint k = 0u; k < 4u; k++) if (((used >> k) & 1u) != 0u) { cnt[k] += out * t[6u + k] * 4u; cnt[4u + k] = t[6u + k] * 4u; } }\n"
     /* DrawTransformFeedback of a GPU-written control memory: indirect arguments {bytes[0] / stride[0], 1, 0, 0} */
     "kernel void xfbargs(device const uint *ctl [[buffer(0)]], device uint *args [[buffer(1)]], uint tid [[thread_position_in_grid]]) {\n"
     "  if (tid != 0u) return; args[0] = ctl[4] != 0u ? ctl[0] / ctl[4] : 0u; args[1] = 1u; args[2] = 0u; args[3] = 0u; }\n";
#define SK_GATHER 0
#define SK_XFBCOPY 1
#define SK_XFBGPU 2
#define SK_XFBARGS 3
static id<MTLComputePipelineState> stage_kernel(int which) {
    static id<MTLComputePipelineState> ps[4];
    static int failed;
    if (ps[which] || failed) return ps[which];
    NSError *e = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:kStageMSL options:nil error:&e];
    static NSString *const names[4] = {@"gather", @"xfbcopy", @"xfbgpu", @"xfbargs"};
    for (int i = 0; lib && i < 4; i++) ps[i] = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:names[i]] error:&e];
    if (!ps[0] || !ps[1] || !ps[2] || !ps[3]) {
        LOGE("stage helper kernels: %s", e.localizedDescription.UTF8String);
        failed = 1;
    }
    return ps[which];
}
static void dispatch_1d(id<MTLComputeCommandEncoder> ce, id<MTLComputePipelineState> ps, uint32_t n) {
    NSUInteger tg = ps.threadExecutionWidth;
    [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg < n ? tg : n, 1, 1)];
}

/* Scratch for GPU-written stage data: the submission's ring (shared storage, recycled when the command buffer completes). Large draws are refused. */
#define STAGE_SCRATCH_MAX (256u << 20)
static void *stage_alloc(uint64_t size, id<MTLBuffer> __autoreleasing *b, uint32_t *off) {
    if (!size || size > STAGE_SCRATCH_MAX) return NULL;
    return ring_alloc(&g_ring, (uint32_t)size, 256, b, off);
}

static void stage_skip(const char *why) {
    static _Atomic uint64_t n;
    uint64_t k = atomic_fetch_add(&n, 1);
    if (k < 4 || (k & (k - 1)) == 0) LOGW("stage draw skipped (#%llu): %s (vs %016llx gs %016llx tcs %016llx tes %016llx)", (unsigned long long)k + 1, why,
                                         (unsigned long long)S.prog[0], (unsigned long long)S.prog[2], (unsigned long long)S.prog[3], (unsigned long long)S.prog[4]);
    atomic_fetch_add(&g_gfx.draws_skipped, 1);
}

/* Pass 1: run the vertex program over vertex ids [lo, lo + range) x instances in an attachment-less pass. stage_out: write the records (returned in *rb / *ro);
 * xl: also write these transform feedback entries straight to the bound buffers (xfb_dw = start of each buffer in words). Returns 0 on failure. */
static int capture_pass(const sdraw *d, uint32_t lo, uint32_t range, int stage_out, const gfx_xfb_layout *xl, const uint32_t xfb_dw[4], id<MTLBuffer> __autoreleasing *rb,
                        uint32_t *ro, uint64_t *slot_mask) {
    pipe_key pk;
    fill_pipe_key(NVN_DRAW_PRIMITIVE_POINTS, &pk);
    stage_pipe_key sk;
    memset(&sk, 0, sizeof sk);
    sk.k = pk;
    sk.mode = SPK_CAPTURE;
    sk.stage_out = (uint8_t)stage_out;
    if (xl) sk.xfb_id = (uint32_t)fnv1a(xl, sizeof *xl, FNV_INIT) | 1u;
    GfxPipe *p = mtl_stage_pipe_get(&sk, xl);
    if (!p) return 0;
    if (slot_mask) *slot_mask = p->vs->info.stage_slot_mask;
    id<MTLBuffer> sb = nil;
    uint32_t so = 0;
    if (stage_out && !stage_alloc((uint64_t)range * d->instances * STAGE_SLOTS * 16, &sb, &so)) return 0;
    flush_clears();
    end_encoders();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 1;
    rp.renderTargetHeight = 1;
    rp.defaultRasterSampleCount = 1;
    g_re = [g_cb renderCommandEncoderWithDescriptor:rp];
    g_enc_serial++;
    [g_re setRenderPipelineState:p->ps];
    bind_vertex_streams();
    bind_stage(ST_VS, p->vs, g_re, 0, NULL);
    uint32_t ctl[2 + MAX_XFB_BUFFERS] = {range, lo + range * d->base_instance, 0, 0, 0, 0};
    if (xl)
        for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
            ctl[2 + k] = xfb_dw[k];
            if (!(p->vs->info.xfb_buffer_mask & (1u << k))) continue;
            id<MTLBuffer> xb;
            uint64_t xo;
            mtl_addr(S.xfb[k].addr, S.xfb[k].size, &xb, &xo); /* checked by the caller */
            [g_re setVertexBuffer:xb offset:xo atIndex:XFB_BUFFER_BASE + k];
        }
    [g_re setVertexBytes:ctl length:sizeof ctl atIndex:XFB_CTL_BUFFER_INDEX];
    if (stage_out) [g_re setVertexBuffer:sb offset:so atIndex:STAGE_OUT_BUFFER_INDEX];
    [g_re drawPrimitives:MTLPrimitiveTypePoint vertexStart:lo vertexCount:range instanceCount:d->instances baseInstance:d->base_instance];
    [g_re endEncoding];
    g_re = nil;
    P.open = 0;
    if (rb) *rb = sb;
    if (ro) *ro = so;
    return 1;
}

/* Record ordinals (into the capture of capture_pass) of the assembled primitives of every instance, instance-major. */
static uint32_t *record_list(const uint32_t *ids, uint32_t n, uint32_t lo, uint32_t range, uint32_t instances, id<MTLBuffer> __autoreleasing *b, uint32_t *o) {
    uint32_t *r = stage_alloc((uint64_t)n * instances * 4, b, o);
    if (!r) return NULL;
    for (uint32_t i = 0; i < instances; i++)
        for (uint32_t k = 0; k < n; k++) r[i * n + k] = i * range + (ids[k] - lo);
    return r;
}

/* Pass 2: copy records seq[0..n) of src into a list (dst). */
static int gather_records(id<MTLBuffer> src, uint32_t so, id<MTLBuffer> seq, uint32_t qo, uint32_t n, id<MTLBuffer> __autoreleasing *db, uint32_t *dof) {
    id<MTLComputePipelineState> ps = stage_kernel(SK_GATHER);
    if (!ps || !stage_alloc((uint64_t)n * STAGE_SLOTS * 16, db, dof)) return 0;
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:ps];
    [ce setBuffer:src offset:so atIndex:0];
    [ce setBuffer:*db offset:*dof atIndex:1];
    [ce setBuffer:seq offset:qo atIndex:2];
    [ce setBytes:&n length:4 atIndex:3];
    dispatch_1d(ce, ps, n);
    return 1;
}

/* A render pass of the game's targets for a stage draw (state as prepare_draw sets it, without vertex streams). Returns 0 when the draw is skipped. */
static int stage_draw_begin(pipe_key *k, uint32_t prim, int triangles) {
    if (!S.render_enable || S.raster_discard) return 0;
    if (triangles && S.poly.cull == NVN_FACE_FRONT_AND_BACK) return 0; /* as prepare_draw: every triangle is culled (Metal has no such mode) */
    if (!pass_begin()) return 0;
    vis_draw();
    fill_pipe_key(prim, k);
    return 1;
}
static void stage_draw_state(GfxPipe *p) {
    [g_re setRenderPipelineState:p->ps];
    [g_re setDepthStencilState:depth_stencil_state()];
    apply_raster_state();
    if (p->fs) bind_stage(ST_FS, p->fs, g_re, 0, NULL);
    P.draws++;
    atomic_fetch_add(&g_gfx.draws, 1);
}
static void gs_fallback(const char *why) {
    static uint64_t seen[32];
    static int ns;
    for (int i = 0; i < ns; i++)
        if (seen[i] == S.prog[2]) return;
    if (ns < 32) seen[ns++] = S.prog[2];
    LOGW("geometry program %016llx: %s: drawn without the geometry stage", (unsigned long long)S.prog[2], why);
}
static void stage_key_masks(int stage, ShaderKey *sk) {
    ds_slot_masks(stage, &sk->depth_slots, &sk->stencil_slots);
    image_masks(stage, &sk->image_int_mask, &sk->image_uint_mask);
}

/* The capture layout of the program that feeds the rasterizer: `h` (geometry, else vertex). 0 with one warning per program when none is registered. */
static int xfb_layout_for(uint64_t h, gfx_xfb_layout *xl) {
    if ((gfx_xfb_get_layout(h, xl) || (h != S.prog[ST_VS] && gfx_xfb_get_layout(S.prog[ST_VS], xl))) && xl->count) return 1;
    static uint64_t warned[16];
    int seen = 0;
    for (int i = 0; i < 16; i++) seen |= warned[i] == h;
    if (!seen) {
        static int wi;
        warned[wi++ & 15] = h;
        LOGW("transform feedback draw with program %016llx: no capture layout registered for it (stages.h / control section): nothing captured", (unsigned long long)h);
    }
    return 0;
}
/* The bound buffers every entry of the layout needs: 1 and their mask, 0 (one warning) when one is unusable. */
static int xfb_buffers_ok(const gfx_xfb_layout *xl, uint32_t *used) {
    *used = 0;
    for (int i = 0; i < xl->count; i++) *used |= 1u << xl->e[i].buffer;
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        if (!(*used & (1u << k))) continue;
        id<MTLBuffer> xb;
        uint64_t xo;
        if (!xl->stride[k] || (xl->stride[k] & 3) || !S.xfb[k].addr || (S.xfb[k].addr & 3) || !mtl_addr(S.xfb[k].addr, S.xfb[k].size, &xb, &xo)) {
            LOG_ONCE(NAPPLE_LOG_WARN, "transform feedback buffer %d is not bound inside a memory pool (or its stride %u is not a multiple of 4): nothing captured", k,
                     xl->stride[k]);
            return 0;
        }
    }
    return 1;
}
/* GPU-counted capture (xfbgpu kernel, see kStageMSL): `n` sequence entries (mode 0: record ordinals in primitive order, nv per primitive; mode 1: gs_idx of
 * output topology `topo`) of the records at rb / ro. Moves the session to GPU counting. */
static void xfb_gpu_capture(const gfx_xfb_layout *xl, uint32_t used, id<MTLBuffer> rb, uint32_t ro, id<MTLBuffer> qb, uint32_t qo, uint32_t n, int mode,
                            uint32_t nv_or_topo, uint64_t mask) {
    xfb_go_gpu();
    id<MTLBuffer> tb;
    uint32_t to;
    uint32_t *t = n ? stage_alloc(4 * (14 + 4 * (uint64_t)xl->count), &tb, &to) : NULL;
    id<MTLComputePipelineState> ps = stage_kernel(SK_XFBGPU);
    if (!t || !ps) return;
    t[0] = n;
    t[1] = xl->count;
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        t[2 + k] = (used & (1u << k)) ? (uint32_t)(S.xfb[k].size / 4) : 0;
        t[6 + k] = xl->stride[k] / 4;
    }
    t[10] = (uint32_t)mask;
    t[11] = (uint32_t)(mask >> 32);
    t[12] = (uint32_t)mode;
    t[13] = nv_or_topo;
    for (int i = 0; i < xl->count; i++) {
        t[14 + 4 * i] = xl->e[i].addr;
        t[15 + 4 * i] = xl->e[i].comps;
        t[16 + 4 * i] = xl->e[i].buffer & 3;
        t[17 + 4 * i] = xl->e[i].offset / 4;
    }
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:ps];
    [ce setBuffer:rb offset:ro atIndex:0];
    [ce setBuffer:qb offset:qo atIndex:1];
    [ce setBuffer:tb offset:to atIndex:2];
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        id<MTLBuffer> xb = nil;
        uint64_t xo = 0;
        if (used & (1u << k)) mtl_addr(S.xfb[k].addr, S.xfb[k].size, &xb, &xo);
        if (!xb) { xb = rb; xo = ro; }
        [ce setBuffer:xb offset:xo atIndex:3 + k];
    }
    [ce setBuffer:g_xfb.cnt offset:0 atIndex:7];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    end_encoders();
}

/* Pass 4 (transform feedback without a geometry / tessellation stage; also with a geometry program the replay could not run, whose input then is what the
 * hardware would capture). Called before the draw's own rendering (which still happens unless the rasterizer discards). Writes at most the whole
 * primitives that fit in every bound buffer, then advances the counters. */
static void xfb_capture(const sdraw *d) {
    gfx_xfb_layout xl;
    if (!xfb_layout_for(S.prog[ST_VS], &xl)) return;
    if (d->mode == NVN_DRAW_PRIMITIVE_PATCHES || S.prog[3] || S.prog[4]) {
        LOG_ONCE(NAPPLE_LOG_WARN, "transform feedback of tessellation output is not captured (needs a software tessellator: Metal gives no access to the tessellated primitives)");
        return;
    }
    uint32_t nv = prim_vertices(d->mode, 0);
    if (!nv) {
        LOG_ONCE(NAPPLE_LOG_WARN, "transform feedback with primitive %u is not captured", d->mode);
        return;
    }
    uint32_t used = 0;
    if (!xfb_buffers_ok(&xl, &used)) return;
    uint64_t room = ~0ull; /* output vertices every used buffer still holds */
    uint32_t dw[MAX_XFB_BUFFERS] = {0};
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        if (!(used & (1u << k))) continue;
        uint64_t left = S.xfb[k].size > g_xfb.bytes[k] ? S.xfb[k].size - g_xfb.bytes[k] : 0;
        if (left / xl.stride[k] < room) room = left / xl.stride[k];
        dw[k] = g_xfb.bytes[k] / 4;
    }
    uint32_t *e = NULL, lo = 0, hi = 0;
    uint32_t n = draw_elements(d, &e, &lo, &hi);
    if (!n) return;
    uint32_t range = hi - lo + 1;
    uint32_t cap = assemble_max(d->mode, n, nv);
    uint32_t *ids = malloc((size_t)cap * 4);
    uint32_t nids = ids ? assemble(d->mode, e, n, nv, ids) : 0;
    free(e);
    if (g_xfb.gpu) { /* GPU-counted session: records in primitive order through the serial kernel, which knows the counts */
        id<MTLBuffer> rb, qb;
        uint32_t ro, qo;
        uint64_t mask = 0;
        if (nids && capture_pass(d, lo, range, 1, NULL, NULL, &rb, &ro, &mask) && record_list(ids, nids, lo, range, d->instances, &qb, &qo))
            xfb_gpu_capture(&xl, used, rb, ro, qb, qo, nids * d->instances, 0, nv, mask);
        free(ids);
        return;
    }
    uint64_t total = (uint64_t)nids * d->instances;
    uint64_t out = total < room ? total : room - room % nv;
    int direct = !d->indexed && (d->mode == NVN_DRAW_PRIMITIVE_POINTS || d->mode == NVN_DRAW_PRIMITIVE_LINES || d->mode == NVN_DRAW_PRIMITIVE_TRIANGLES) &&
                 nids == d->count && out == total;
    if (out) {
        if (direct) { /* the vertex program writes the buffers itself: output vertex = element order */
            if (!capture_pass(d, lo, range, 0, &xl, dw, NULL, NULL, NULL)) out = 0;
        } else {
            id<MTLBuffer> rb, qb, tb;
            uint32_t ro, qo, to;
            uint64_t mask = 0;
            uint32_t *seq = NULL, *t = NULL;
            if (!capture_pass(d, lo, range, 1, NULL, NULL, &rb, &ro, &mask) || !(seq = record_list(ids, nids, lo, range, d->instances, &qb, &qo)) ||
                !(t = stage_alloc(4 * (12 + 4 * (uint64_t)xl.count), &tb, &to)) || !stage_kernel(SK_XFBCOPY))
                out = 0;
            else {
                t[0] = (uint32_t)out;
                t[1] = xl.count;
                for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
                    t[2 + k] = dw[k];
                    t[6 + k] = xl.stride[k] / 4;
                }
                t[10] = (uint32_t)mask;
                t[11] = (uint32_t)(mask >> 32);
                for (int i = 0; i < xl.count; i++) {
                    t[12 + 4 * i] = xl.e[i].addr;
                    t[13 + 4 * i] = xl.e[i].comps;
                    t[14 + 4 * i] = xl.e[i].buffer & 3;
                    t[15 + 4 * i] = xl.e[i].offset / 4;
                }
                id<MTLComputeCommandEncoder> ce = compute_encoder();
                id<MTLComputePipelineState> ps = stage_kernel(SK_XFBCOPY);
                [ce setComputePipelineState:ps];
                [ce setBuffer:rb offset:ro atIndex:0];
                [ce setBuffer:qb offset:qo atIndex:1];
                [ce setBuffer:tb offset:to atIndex:2];
                for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
                    id<MTLBuffer> xb = nil;
                    uint64_t xo = 0;
                    if (used & (1u << k)) mtl_addr(S.xfb[k].addr, S.xfb[k].size, &xb, &xo);
                    if (!xb) { /* unused slot: any valid buffer */
                        xb = rb;
                        xo = ro;
                    }
                    [ce setBuffer:xb offset:xo atIndex:3 + k];
                }
                dispatch_1d(ce, ps, (uint32_t)out);
                end_encoders();
            }
        }
    }
    free(ids);
    if (out < total) LOG_ONCE(NAPPLE_LOG_INFO, "transform feedback buffer full: %llu of %llu vertices captured", (unsigned long long)out, (unsigned long long)total);
    for (int k = 0; k < MAX_XFB_BUFFERS; k++)
        if (used & (1u << k)) {
            g_xfb.bytes[k] += (uint32_t)out * xl.stride[k];
            g_xfb.stride[k] = xl.stride[k];
        }
}

/* Whether a geometry program emits vertices (OUT in its register, immediate or constant buffer form). One that does not is a pass-through geometry
 * program (NV_geometry_shader_passthrough): the draw then goes on without the geometry stage, as before this file existed. */
static int gs_emits(uint64_t hash) {
    const shader_blob *b = shader_blob_find(hash);
    if (!b || b->code_size <= 0x80) return 0;
    const uint64_t *w = (const uint64_t *)(b->code + 0x80);
    size_t n = (b->code_size - 0x80) / 8;
    for (size_t i = 0; i < n; i++) {
        if (i % 4 == 0) continue; /* scheduling word */
        uint64_t top = w[i] >> 52;
        if (top == 0xfbe || top == 0xf6e || top == 0xebe) return 1;
    }
    return 0;
}
/* Declared output of a geometry program (its SPH). 0 (logged once per program) when the draw should go on without the geometry stage. */
typedef struct {
    uint32_t maxv, inv;
    uint8_t topo;
} gs_decl;
static int gs_decl_of(uint64_t gsh, gs_decl *g) {
    const uint32_t *w = sph_of(gsh);
    if (!w) { gs_fallback("no shader header"); return 0; }
    uint32_t topo_hw = (w[3] >> 24) & 0xf;
    g->maxv = w[4] & 0xfff;
    g->inv = (w[2] >> 24) & 0xff; /* SPH word 2 bits 24..31: invocations per primitive (GS instancing); the kernel runs nprim * inv threads (sp[3] = inv) */
    g->topo = topo_hw == 1 ? GS_OUT_POINTS : topo_hw == 6 ? GS_OUT_LINE_STRIP : GS_OUT_TRIANGLE_STRIP;
    if (topo_hw != 1 && topo_hw != 6 && topo_hw != 7) LOG_ONCE(NAPPLE_LOG_WARN, "geometry output topology %u read as a triangle strip", topo_hw);
    if (g->inv < 1) g->inv = 1;
    if (g->inv > 32) { gs_fallback("more than 32 invocations declared"); return 0; }
    if (!g->maxv || !gs_emits(gsh)) {
        gs_fallback("pass-through geometry program (no output vertices declared or no OUT)");
        return 0;
    }
    return 1;
}

/* Passes 2-3 of a geometry draw: the GS kernel over the input records rb / ro, primitive i being records seq[nv * i ..] (qb / qo); then the capture of
 * its output (xl != NULL: the layout, xused its buffers) and the draw of it (want_draw; `prim` = the primitive the pipeline key is filled for). nprim =
 * primitives; with spb, the capacity, while the GPU-side count sits in spb[0] (sp as the kernel reads it: input from the software tessellator), and the
 * output slots of the threads past it are pre-filled as restarts. Returns 0 when the program did not compile (nothing encoded by this function). */
static int gs_run(uint64_t gsh, const gs_decl *g, uint32_t prim, uint32_t nv, id<MTLBuffer> rb, uint32_t ro, id<MTLBuffer> qb, uint32_t qo, uint32_t nprim,
                  id<MTLBuffer> spb, uint32_t spo, int want_draw, const gfx_xfb_layout *xl, uint32_t xused) {
    pipe_key k;
    memset(&k, 0, sizeof k);
    uint32_t nthr = nprim * g->inv, maxv = g->maxv; /* one thread per (primitive, invocation): output slots are per thread */
    ShaderKey gk = {0}; /* no clip-space conversions in the kernel (gs_vtx is also the transform feedback source): the pass-through function applies them */
    gk.gs_in_vertices = (uint8_t)nv;
    gk.gs_out_topology = g->topo;
    gk.gs_max_vertices = (uint16_t)maxv;
    stage_key_masks(2, &gk);
    GfxShader *gs = nil;
    id<MTLComputePipelineState> gps = mtl_compute_pipe_key(gsh, &gk, &gs);
    if (!gps) return 0;
    id<MTLBuffer> ib, vb, xb;
    uint32_t io, vo, xo;
    if (!nthr || !gather_records(rb, ro, qb, qo, nprim * nv, &ib, &io) || !stage_alloc((uint64_t)nthr * maxv * STAGE_SLOTS * 16, &vb, &vo) ||
        !stage_alloc((uint64_t)nthr * maxv * 8, &xb, &xo)) {
        stage_skip("geometry passes could not be set up (scratch or capture)");
        return 1;
    }
    if (spb) [blit() fillBuffer:xb range:NSMakeRange(xo, (NSUInteger)nthr * maxv * 8) value:0xff];
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:gps];
    bind_stage(2, gs, ce, 1, NULL);
    if (spb) [ce setBuffer:spb offset:spo atIndex:STAGE_PARAM_BUFFER_INDEX];
    else {
        uint32_t sp[4] = {nprim, 0, 0, g->inv};
        [ce setBytes:sp length:sizeof sp atIndex:STAGE_PARAM_BUFFER_INDEX];
    }
    [ce setBuffer:ib offset:io atIndex:STAGE_IN_BUFFER_INDEX];
    [ce setBuffer:vb offset:vo atIndex:GS_VERTEX_BUFFER_INDEX];
    [ce setBuffer:xb offset:xo atIndex:GS_INDEX_BUFFER_INDEX];
    dispatch_1d(ce, gps, nthr);
    end_encoders();
    if (xl) xfb_gpu_capture(xl, xused, vb, vo, xb, xo, nthr * 2 * maxv, 1, g->topo, gs->info.stage_slot_mask);
    if (!want_draw) return 1;
    if (!stage_draw_begin(&k, prim, g->topo == GS_OUT_TRIANGLE_STRIP)) return 1;
    stage_pipe_key sk;
    memset(&sk, 0, sizeof sk);
    sk.k = k;
    sk.mode = SPK_GS_DRAW;
    sk.gs_topo = g->topo;
    sk.prog = gsh;
    sk.gs_slots = gs->info.stage_slot_mask;
    sk.gs_clip = (uint8_t)((k.flip_y ? 1 : 0) | (k.depth_mode == 0 ? 2 : 0));
    GfxPipe *p = mtl_stage_pipe_get(&sk, NULL);
    if (!p) { stage_skip("geometry output pipeline failed"); return 1; }
    stage_draw_state(p);
    [g_re setVertexBuffer:vb offset:vo atIndex:GS_VERTEX_BUFFER_INDEX];
    [g_re setVertexBuffer:xb offset:xo atIndex:GS_INDEX_BUFFER_INDEX];
    uint32_t nidx = nthr * 2 * maxv;
    if (g->topo == GS_OUT_POINTS) [g_re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:nidx];
    else
        [g_re drawIndexedPrimitives:g->topo == GS_OUT_LINE_STRIP ? MTLPrimitiveTypeLineStrip : MTLPrimitiveTypeTriangleStrip
                         indexCount:nidx
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:xb
                  indexBufferOffset:xo];
    return 1;
}

/* Geometry draw (passes 1-3). Returns 0 when the draw should go on without the geometry stage (pass-through or unusable program: logged once). */
static int draw_geometry(const sdraw *d) {
    uint64_t gsh = S.prog[2];
    gs_decl g;
    if (!gs_decl_of(gsh, &g)) return 0;
    uint32_t nv = prim_vertices(d->mode, 1);
    if (!nv || d->mode == NVN_DRAW_PRIMITIVE_PATCHES) { gs_fallback("input primitive not supported"); return 0; }
    int want_draw = S.render_enable && !S.raster_discard && S.rt_valid;
    gfx_xfb_layout xl;
    uint32_t xused = 0;
    int want_xfb = xfb_capturing() && xfb_layout_for(gsh, &xl) && xfb_buffers_ok(&xl, &xused);
    if (!want_draw && !want_xfb) return 1;
    uint32_t *e = NULL, lo = 0, hi = 0;
    uint32_t n = draw_elements(d, &e, &lo, &hi);
    if (!n) return 1;
    uint32_t range = hi - lo + 1, cap = assemble_max(d->mode, n, nv);
    uint32_t *ids = malloc((size_t)cap * 4);
    uint32_t nids = ids ? assemble(d->mode, e, n, nv, ids) : 0;
    free(e);
    uint32_t nprim = nids / nv * d->instances;
    id<MTLBuffer> rb, qb;
    uint32_t ro, qo;
    int ok = nprim && capture_pass(d, lo, range, 1, NULL, NULL, &rb, &ro, NULL) && record_list(ids, nids, lo, range, d->instances, &qb, &qo);
    free(ids);
    if (!ok) {
        if (nprim) stage_skip("geometry passes could not be set up (scratch or capture)");
        return 1;
    }
    if (!gs_run(gsh, &g, d->mode, nv, rb, ro, qb, qo, nprim, nil, 0, want_draw, want_xfb ? &xl : NULL, xused)) {
        gs_fallback("the geometry program did not compile");
        return 0;
    }
    return 1;
}

/* ---- software tessellator (isolines, capture of tess eval output, geometry after tessellation) ------------------------------------------------
 * Metal's tessellator feeds the rasterizer only: its vertices cannot be captured in order, it has no isoline domain and nothing can follow it. For
 * those draws the replay tessellates itself: the tessgen kernel (one thread per patch) reads the same factor record Metal would (quad layout for
 * isolines), and writes a fixed number of coordinate slots per patch (u, v, w, patch id) plus restart-separated index strips (3 + restart per
 * triangle, 2 + restart per line segment, unused slots all restarts). The tess eval program then runs as a plain vertex function over those slots
 * (ShaderKey.tess_soft): drawn as indexed strips, or into stage-out records (capture / geometry input).
 * Topology: isolines follow GL exactly (outer[0] lines at v = j / n0, j < n0, each of outer[1] segments; spacing applies to outer[1] only). Quads and
 * triangles are a uniform grid (quads nu x nv cells, nu from max(inner[0], outer[1], outer[3]), nv from max(inner[1], outer[0], outer[2]); triangles
 * a barycentric grid of n = the largest level): with all levels equal and integer spacing it has the vertices of the hardware tessellation, but not its
 * ring triangulation, and fractional spacing rounds the segment count up to the spacing's parity with evenly spaced vertices. A patch with an outer level
 * <= 0 (or NaN) is culled. Levels are capped by t[3]: the CPU knows them without a tess control program; with one, NAPPLE_GFX_SOFT_TESS_MAX (16). */
static NSString *const kTessMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "static uint tseg(float l, uint sp, uint L) { l = clamp(l, 1.0f, 64.0f); uint n = uint(ceil(l));\n"
     "  if (sp == 1u) { if ((n & 1u) == 0u) n++; } else if (sp == 2u) { if (n < 2u) n = 2u; if ((n & 1u) != 0u) n++; } return max(min(n, L), 1u); }\n"
     /* t: [0] patches, [1] domain (TESS_DOMAIN_*: 0 quad 1 triangle 2 isoline), [2] spacing, [3] level cap, [4] coordinate slots per patch,
      * [5] index slots per patch, [6] clockwise, [7] factor halves per patch */
     "kernel void tessgen(device const half *fac [[buffer(0)]], device float4 *uv [[buffer(1)]], device uint *idx [[buffer(2)]], constant uint *t [[buffer(3)]],\n"
     "                    uint p [[thread_position_in_grid]]) {\n"
     "  if (p >= t[0]) return; uint vc = t[4], ic = t[5], vb = p * vc, dom = t[1], L = t[3], w = 0u; bool cw = t[6] != 0u;\n"
     "  device float4 *o = uv + vb; device uint *x = idx + p * ic; device const half *f = fac + p * t[7]; float pid = as_type<float>(p);\n"
     "  for (uint k = 0u; k < vc; k++) o[k] = float4(0.0f, 0.0f, 0.0f, pid);\n"
     "  for (uint k = 0u; k < ic; k++) x[k] = 0xFFFFFFFFu;\n"
     "  uint nout = dom == 1u ? 3u : dom == 2u ? 2u : 4u;\n"
     "  for (uint k = 0u; k < nout; k++) { float l = float(f[k]); if (!(l > 0.0f)) return; }\n"
     "  if (dom == 2u) {\n"
     "    uint n0 = min(uint(ceil(clamp(float(f[0]), 1.0f, 64.0f))), L), n1 = tseg(float(f[1]), t[2], L);\n"
     "    for (uint j = 0u; j < n0; j++) for (uint i = 0u; i <= n1; i++) o[j * (n1 + 1u) + i] = float4(float(i) / float(n1), float(j) / float(n0), 0.0f, pid);\n"
     "    for (uint j = 0u; j < n0; j++) for (uint i = 0u; i < n1; i++) { uint a = j * (n1 + 1u) + i; x[w] = vb + a; x[w + 1u] = vb + a + 1u; w += 3u; }\n"
     "  } else if (dom == 0u) {\n"
     "    uint nu = tseg(max(float(f[4]), max(float(f[1]), float(f[3]))), t[2], L), nv = tseg(max(float(f[5]), max(float(f[0]), float(f[2]))), t[2], L);\n"
     "    for (uint j = 0u; j <= nv; j++) for (uint i = 0u; i <= nu; i++) o[j * (nu + 1u) + i] = float4(float(i) / float(nu), float(j) / float(nv), 0.0f, pid);\n"
     "    for (uint j = 0u; j < nv; j++) for (uint i = 0u; i < nu; i++) { uint a = j * (nu + 1u) + i, b = a + 1u, c = a + nu + 2u, d = a + nu + 1u;\n"
     "      x[w] = vb + a; x[w + 1u] = vb + (cw ? c : b); x[w + 2u] = vb + (cw ? b : c); w += 4u;\n"
     "      x[w] = vb + a; x[w + 1u] = vb + (cw ? d : c); x[w + 2u] = vb + (cw ? c : d); w += 4u; }\n"
     "  } else {\n"
     "    uint n = tseg(max(max(float(f[0]), float(f[1])), max(float(f[2]), float(f[3]))), t[2], L);\n"
     "    for (uint j = 0u; j <= n; j++) for (uint i = 0u; i + j <= n; i++) { float u = float(i) / float(n), v = float(j) / float(n);\n"
     "      o[j * (n + 1u) - j * (j - 1u) / 2u + i] = float4(u, v, max(1.0f - u - v, 0.0f), pid); }\n"
     "    for (uint j = 0u; j < n; j++) for (uint i = 0u; i + j < n; i++) {\n"
     "      uint a = j * (n + 1u) - j * (j - 1u) / 2u + i, b = a + 1u, c = (j + 1u) * (n + 1u) - (j + 1u) * j / 2u + i, d = c + 1u;\n"
     "      x[w] = vb + a; x[w + 1u] = vb + (cw ? c : b); x[w + 2u] = vb + (cw ? b : c); w += 4u;\n"
     "      if (i + j + 1u < n) { x[w] = vb + b; x[w + 1u] = vb + (cw ? c : d); x[w + 2u] = vb + (cw ? d : c); w += 4u; } }\n"
     "  } }\n"
     /* the tessellated primitives as geometry input: index strips -> packed record ordinals (nv per primitive, tail zeroed) and the GS kernel's sp
      * {count, 0, 0, invocations}. t: [0] index slots, [1] vertices per primitive (2 / 3), [2] invocations */
     "kernel void tesslist(device const uint *idx [[buffer(0)]], device uint *seq [[buffer(1)]], device uint *sp [[buffer(2)]], constant uint *t [[buffer(3)]],\n"
     "                     uint tid [[thread_position_in_grid]]) {\n"
     "  if (tid != 0u) return; uint g = t[1] + 1u, n = 0u;\n"
     "  for (uint i = 0u; i + g <= t[0]; i += g) { if (idx[i] == 0xFFFFFFFFu) continue; for (uint k = 0u; k < t[1]; k++) seq[n * t[1] + k] = idx[i + k]; n++; }\n"
     "  for (uint k = n * t[1]; k < (t[0] / g) * t[1]; k++) seq[k] = 0u;\n"
     "  sp[0] = n; sp[1] = 0u; sp[2] = 0u; sp[3] = t[2]; }\n";
static id<MTLComputePipelineState> tess_kernel(int which) { /* 0 tessgen, 1 tesslist */
    static id<MTLComputePipelineState> ps[2];
    static int failed;
    if (ps[which] || failed) return ps[which];
    NSError *e = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:kTessMSL options:nil error:&e];
    for (int i = 0; lib && i < 2; i++) ps[i] = [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:i ? @"tesslist" : @"tessgen"] error:&e];
    if (!ps[0] || !ps[1]) {
        LOGE("software tessellator kernels: %s", e.localizedDescription.UTF8String);
        failed = 1;
    }
    return ps[which];
}
/* Coordinate / index slots per patch of the software tessellator at level cap L. */
static void tess_soft_slots(uint8_t domain, uint32_t L, uint32_t *vc, uint32_t *ic) {
    if (domain == TESS_DOMAIN_ISOLINE) { *vc = L * (L + 1); *ic = L * L * 3; }
    else if (domain == TESS_DOMAIN_QUAD) { *vc = (L + 1) * (L + 1); *ic = L * L * 8; }
    else { *vc = (L + 1) * (L + 2) / 2; *ic = L * L * 4; }
}
static uint32_t tess_soft_cap(void) {
    static uint32_t cap;
    if (!cap) {
        const char *s = getenv("NAPPLE_GFX_SOFT_TESS_MAX");
        int v = s ? atoi(s) : 16;
        cap = v < 1 ? 1 : v > 64 ? 64 : (uint32_t)v;
    }
    return cap;
}
/* The software tessellated tess eval program into stage-out records (one per coordinate slot, rb / ro) in an attachment-less pass. */
static int tes_soft_capture(const stage_pipe_key *tsk, id<MTLBuffer> vb, uint32_t vo, id<MTLBuffer> pb, uint32_t po, id<MTLBuffer> ub, uint32_t uo, uint32_t nvert,
                            uint32_t npatch, id<MTLBuffer> __autoreleasing *rb, uint32_t *ro, uint64_t *slot_mask) {
    stage_pipe_key sk = *tsk;
    fill_pipe_key(NVN_DRAW_PRIMITIVE_POINTS, &sk.k);
    sk.mode = SPK_TESS_SOFT_CAPTURE;
    GfxPipe *p = mtl_stage_pipe_get(&sk, NULL);
    if (!p || !stage_alloc((uint64_t)nvert * STAGE_SLOTS * 16, rb, ro)) return 0;
    *slot_mask = p->vs->info.stage_slot_mask;
    flush_clears();
    end_encoders();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 1;
    rp.renderTargetHeight = 1;
    rp.defaultRasterSampleCount = 1;
    g_re = [g_cb renderCommandEncoderWithDescriptor:rp];
    g_enc_serial++;
    [g_re setRenderPipelineState:p->ps];
    bind_stage(4, p->vs, g_re, 0, NULL);
    uint32_t sp[4] = {npatch, 0, 0, 0}, ctl[2 + MAX_XFB_BUFFERS] = {0};
    [g_re setVertexBytes:sp length:sizeof sp atIndex:STAGE_PARAM_BUFFER_INDEX];
    [g_re setVertexBytes:ctl length:sizeof ctl atIndex:XFB_CTL_BUFFER_INDEX];
    [g_re setVertexBuffer:vb offset:vo atIndex:TESS_VERTEX_BUFFER_INDEX];
    [g_re setVertexBuffer:pb offset:po atIndex:TESS_PATCH_BUFFER_INDEX];
    [g_re setVertexBuffer:ub offset:uo atIndex:TESS_UV_BUFFER_INDEX];
    [g_re setVertexBuffer:*rb offset:*ro atIndex:STAGE_OUT_BUFFER_INDEX];
    [g_re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:nvert];
    [g_re endEncoding];
    g_re = nil;
    P.open = 0;
    return 1;
}

/* Tessellated draw (passes 1-3): patches of S.patch_size control points. Metal's tessellator draws triangle and quad patches; isolines, a capture of the
 * tess eval output and a geometry program after tessellation go through the software tessellator above. Captures its own transform feedback. */
static void draw_tessellation(const sdraw *d) {
    uint64_t tcs = S.prog[3], tes = S.prog[4];
    if (!tes) { stage_skip(d->mode == NVN_DRAW_PRIMITIVE_PATCHES ? "patches without a tess evaluation program" : "tess control program without patches"); return; }
    if (d->mode != NVN_DRAW_PRIMITIVE_PATCHES) { stage_skip("tessellation programs bound for a draw that is not PATCHES"); return; }
    gfx_tess_params tp;
    gfx_tess_get_params(tes, &tp);
    int iso = tp.domain == TESS_DOMAIN_ISOLINE;
    uint32_t nv = prim_vertices(d->mode, 0), tov = nv;
    if (tcs) {
        const uint32_t *w = sph_of(tcs);
        uint32_t t = w ? (w[2] >> 24) & 0xff : 0;
        if (t) tov = t;
    }
    if (tov < 1 || tov > 32) { stage_skip("tess control output patch size out of 1..32"); return; }
    gs_decl g;
    int use_gs = S.prog[2] && gs_decl_of(S.prog[2], &g); /* a pass-through geometry program: drawn without it (as the plain geometry path does) */
    int want_draw = S.render_enable && !S.raster_discard && S.rt_valid;
    gfx_xfb_layout xl;
    uint32_t xused = 0;
    int want_xfb = xfb_capturing() && xfb_layout_for(use_gs ? S.prog[2] : tes, &xl) && xfb_buffers_ok(&xl, &xused);
    if (!want_draw && !want_xfb) return;
    int soft = iso || use_gs || want_xfb;
    uint32_t *e = NULL, lo = 0, hi = 0;
    uint32_t n = draw_elements(d, &e, &lo, &hi);
    if (!n) return;
    uint32_t range = hi - lo + 1;
    uint32_t *ids = malloc(((size_t)n + nv) * 4);
    uint32_t nids = ids ? assemble(d->mode, e, n, nv, ids) : 0;
    free(e);
    uint32_t npatch = nids / nv * d->instances;
    int quad = tp.domain != TESS_DOMAIN_TRIANGLE; /* isolines use the quad factor record (outer[0] lines, outer[1] segments) */
    uint32_t nfac = quad ? 6 : 4;
    id<MTLBuffer> rb, qb, vb, pb, fb;
    uint32_t ro, qo, vo, po, fo;
    __fp16 *fac = NULL;
    float *patch = NULL;
    int ok = npatch && capture_pass(d, lo, range, 1, NULL, NULL, &rb, &ro, NULL) && record_list(ids, nids, lo, range, d->instances, &qb, &qo) &&
             (fac = stage_alloc((uint64_t)npatch * nfac * 2, &fb, &fo)) && (patch = stage_alloc((uint64_t)npatch * STAGE_SLOTS * 16, &pb, &po));
    free(ids);
    if (!ok) {
        if (npatch) stage_skip("tessellation passes could not be set up (scratch or capture)");
        return;
    }
    for (uint32_t i = 0; i < npatch; i++) { /* NVN default levels: the factors, and the level slots of the patch record */
        __fp16 *f = fac + i * nfac;
        if (quad) {
            for (int j = 0; j < 4; j++) f[j] = (__fp16)S.tess_outer[j];
            f[4] = (__fp16)S.tess_inner[0];
            f[5] = (__fp16)S.tess_inner[1];
        } else {
            for (int j = 0; j < 3; j++) f[j] = (__fp16)S.tess_outer[j];
            f[3] = (__fp16)S.tess_inner[0];
        }
        float *r = patch + (size_t)i * STAGE_SLOTS * 4;
        memset(r, 0, STAGE_SLOTS * 16);
        memcpy(r, S.tess_outer, 16);
        memcpy(r + 4, S.tess_inner, 8);
    }
    if (!gather_records(rb, ro, qb, qo, npatch * nv, &vb, &vo)) { stage_skip("tessellation gather failed"); return; }
    if (tcs) {
        ShaderKey ck = {0};
        ck.tess_in_vertices = (uint8_t)nv;
        ck.tess_out_vertices = (uint8_t)tov;
        ck.tess_domain = tp.domain;
        stage_key_masks(3, &ck);
        GfxShader *cs = nil;
        id<MTLComputePipelineState> cps = mtl_compute_pipe_key(tcs, &ck, &cs);
        id<MTLBuffer> ob;
        uint32_t oo;
        if (!cps || !stage_alloc((uint64_t)npatch * tov * STAGE_SLOTS * 16, &ob, &oo)) { stage_skip("tess control program did not compile (or scratch)"); return; }
        id<MTLComputeCommandEncoder> ce = compute_encoder();
        [ce setComputePipelineState:cps];
        bind_stage(3, cs, ce, 1, NULL);
        uint32_t sp[4] = {npatch, 0, 0, 0};
        [ce setBytes:sp length:sizeof sp atIndex:STAGE_PARAM_BUFFER_INDEX];
        [ce setBuffer:vb offset:vo atIndex:STAGE_IN_BUFFER_INDEX];
        [ce setBuffer:ob offset:oo atIndex:TESS_VERTEX_BUFFER_INDEX];
        [ce setBuffer:pb offset:po atIndex:TESS_PATCH_BUFFER_INDEX];
        [ce setBuffer:fb offset:fo atIndex:TESS_FACTOR_BUFFER_INDEX];
        [ce dispatchThreadgroups:MTLSizeMake(npatch, 1, 1) threadsPerThreadgroup:MTLSizeMake(tov, 1, 1)];
        vb = ob;
        vo = oo;
    }
    stage_pipe_key sk;
    memset(&sk, 0, sizeof sk);
    sk.prog = tes;
    sk.tess_domain = tp.domain;
    sk.tess_spacing = tp.spacing;
    sk.tess_winding = tp.winding;
    sk.tess_cp = (uint8_t)tov;
    ds_slot_masks(4, &sk.te_depth, &sk.te_stencil);
    image_masks(4, &sk.te_img_int, &sk.te_img_uint);
    pipe_key k;
    if (!soft) {
        end_encoders();
        if (!stage_draw_begin(&k, d->mode, 1)) return;
        sk.k = k;
        sk.mode = SPK_TESS;
        GfxPipe *p = mtl_stage_pipe_get(&sk, NULL);
        if (!p) { stage_skip("tessellation pipeline failed"); return; }
        stage_draw_state(p);
        bind_stage(4, p->vs, g_re, 0, NULL);
        uint32_t sp[4] = {npatch, 0, 0, 0};
        [g_re setVertexBytes:sp length:sizeof sp atIndex:STAGE_PARAM_BUFFER_INDEX];
        [g_re setVertexBuffer:vb offset:vo atIndex:TESS_VERTEX_BUFFER_INDEX];
        [g_re setVertexBuffer:pb offset:po atIndex:TESS_PATCH_BUFFER_INDEX];
        [g_re setTessellationFactorBuffer:fb offset:fo instanceStride:0];
        [g_re drawPatches:tov patchStart:0 patchCount:npatch patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        return;
    }
    /* software tessellation: level cap, coordinates + index strips */
    uint32_t L = tess_soft_cap();
    if (!tcs) {
        float m = 1;
        for (int j = 0; j < 4; j++) m = S.tess_outer[j] > m ? S.tess_outer[j] : m;
        for (int j = 0; j < 2; j++) m = S.tess_inner[j] > m ? S.tess_inner[j] : m;
        L = m >= 63 ? 64 : (uint32_t)ceilf(m) + 1; /* + 1: room for the parity round-up of fractional spacing */
    } else LOG_ONCE(NAPPLE_LOG_INFO, "software tessellation after a tess control program: levels capped at %u (NAPPLE_GFX_SOFT_TESS_MAX)", L);
    uint32_t vc, ic;
    tess_soft_slots(tp.domain, L, &vc, &ic);
    id<MTLBuffer> ub, xb, tb;
    uint32_t uo, xo, to;
    uint32_t *t = stage_alloc(32, &tb, &to);
    id<MTLComputePipelineState> gen = tess_kernel(0);
    if (!t || !gen || (uint64_t)npatch * vc > 0xffffffffu / STAGE_SLOTS || !stage_alloc((uint64_t)npatch * vc * 16, &ub, &uo) || !stage_alloc((uint64_t)npatch * ic * 4, &xb, &xo)) {
        stage_skip("software tessellation could not be set up (kernels or scratch)");
        return;
    }
    uint32_t tv[8] = {npatch, tp.domain, tp.spacing, L, vc, ic, tp.winding == TESS_WINDING_CW, nfac};
    memcpy(t, tv, sizeof tv);
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:gen];
    [ce setBuffer:fb offset:fo atIndex:0];
    [ce setBuffer:ub offset:uo atIndex:1];
    [ce setBuffer:xb offset:xo atIndex:2];
    [ce setBuffer:tb offset:to atIndex:3];
    dispatch_1d(ce, gen, npatch);
    end_encoders();
    uint32_t nvert = npatch * vc, nidx = npatch * ic, pv = iso ? 2 : 3;
    if (use_gs || want_xfb) {
        id<MTLBuffer> sb;
        uint32_t so;
        uint64_t mask = 0;
        if (!tes_soft_capture(&sk, vb, vo, pb, po, ub, uo, nvert, npatch, &sb, &so, &mask)) {
            stage_skip("software tessellation capture failed (tess eval program or scratch)");
            return;
        }
        if (!use_gs) xfb_gpu_capture(&xl, xused, sb, so, xb, xo, nidx, 1, iso ? GS_OUT_LINE_STRIP : GS_OUT_TRIANGLE_STRIP, mask);
        else {
            id<MTLBuffer> qb2, spb, lb;
            uint32_t qo2, spo, lo2, cap = nidx / (pv + 1);
            uint32_t *lt = stage_alloc(16, &lb, &lo2);
            id<MTLComputePipelineState> lst = tess_kernel(1);
            if (!lt || !lst || !stage_alloc((uint64_t)cap * pv * 4, &qb2, &qo2) || !stage_alloc(16, &spb, &spo)) {
                stage_skip("tessellation -> geometry list could not be set up");
                return;
            }
            lt[0] = nidx;
            lt[1] = pv;
            lt[2] = g.inv;
            ce = compute_encoder();
            [ce setComputePipelineState:lst];
            [ce setBuffer:xb offset:xo atIndex:0];
            [ce setBuffer:qb2 offset:qo2 atIndex:1];
            [ce setBuffer:spb offset:spo atIndex:2];
            [ce setBuffer:lb offset:lo2 atIndex:3];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            end_encoders();
            if (!gs_run(S.prog[2], &g, iso ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES, pv, sb, so, qb2, qo2, cap, spb, spo, want_draw,
                        want_xfb ? &xl : NULL, xused))
                stage_skip("the geometry program after tessellation did not compile");
            return;
        }
    }
    if (!want_draw || !stage_draw_begin(&k, iso ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES, !iso)) return;
    sk.k = k;
    sk.mode = SPK_TESS_SOFT;
    GfxPipe *p = mtl_stage_pipe_get(&sk, NULL);
    if (!p) { stage_skip("software tessellation pipeline failed"); return; }
    stage_draw_state(p);
    bind_stage(4, p->vs, g_re, 0, NULL);
    uint32_t sp[4] = {npatch, 0, 0, 0};
    [g_re setVertexBytes:sp length:sizeof sp atIndex:STAGE_PARAM_BUFFER_INDEX];
    [g_re setVertexBuffer:vb offset:vo atIndex:TESS_VERTEX_BUFFER_INDEX];
    [g_re setVertexBuffer:pb offset:po atIndex:TESS_PATCH_BUFFER_INDEX];
    [g_re setVertexBuffer:ub offset:uo atIndex:TESS_UV_BUFFER_INDEX];
    [g_re drawIndexedPrimitives:iso ? MTLPrimitiveTypeLineStrip : MTLPrimitiveTypeTriangleStrip
                     indexCount:nidx
                      indexType:MTLIndexTypeUInt32
                    indexBuffer:xb
              indexBufferOffset:xo];
}

/* Entry from do_draw_arrays / do_draw_elements: transform feedback capture first, then the draw's stages. Returns 1 when the draw was handled here. */
static int stage_draw(const sdraw *d) {
    if (g_predict) return stage_draw_needed(d->mode);
    int tess = S.prog[3] || S.prog[4] || d->mode == NVN_DRAW_PRIMITIVE_PATCHES;
    int own = !g_no_stages && (tess || S.prog[2]); /* draw_geometry / draw_tessellation capture their own output */
    if (xfb_capturing() && !own) xfb_capture(d);
    if (!stage_draw_needed(d->mode)) return 0;
    if (!S.prog[ST_VS]) return 1;
    if (tess) {
        draw_tessellation(d);
        return 1;
    }
    int r = draw_geometry(d);
    if (!r && xfb_capturing()) xfb_capture(d); /* pass-through / unusable geometry program: its input is what the hardware would capture */
    return r;
}
