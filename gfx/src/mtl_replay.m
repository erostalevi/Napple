/* mtl_replay.m - executes recorded NVN command lists on Metal.
 *
 * One replay thread runs jobs in submission order. NVN state persists across command lists (the GPU channel keeps it), so the state
 * machine below is global. Render passes are opened lazily by the first draw/clear after SetRenderTargets and closed by anything that
 * needs another encoder type. Uniform buffers are snapshotted per draw into a scratch ring, so a CPU write after the draw was
 * encoded can never leak into it. */
#include <pthread.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IOS /* unused here; the iOS SDKs reject the header */
#include <mach/mach_vm.h>
#endif

#import "mtl_priv.h"
#import "mtl_display.h"

/* ---------------------------------------------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------------------------------------------- */
enum { ST_VS = 0, ST_FS = 1, ST_CS = 5 };

typedef struct {
    uint64_t prog[6];
    blend_st blend[8];
    color_st color;
    chmask_st chmask;
    ms_st ms;
    poly_st poly;
    ds_st ds;
    va_st vattr[32];
    uint32_t nattr;
    vs_st vstream[32];
    uint32_t nstream;
    bufrange vbuf[16];
    bufrange ubo[6][14], ssbo[6][16];
    uint64_t tex[6][128], img[6][16];
    uint64_t texpool, samppool;
    p_set_rt rt;
    int rt_valid;
    float vp[16][4];
    int32_t sc[16][4];
    int sc_set;
    float dr[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_wmask[2], stencil_rmask[2]; /* [0] front, [1] back */
    float poly_factor, poly_units, poly_clamp;
    int prim_restart_en;
    uint32_t prim_restart_idx;
    int depth_clamp, raster_discard, render_enable;
    float alpha_ref;
    int64_t copy_row_stride, copy_img_stride;
    uint32_t patch_size;              /* tessellation (mtl_stages.inc): control points per input patch, NVN default levels */
    uint8_t sample_shading;           /* the bound program asks for sample shading (p_bind_program.sample_shading) */
    float tess_outer[4], tess_inner[2];
    bufrange xfb[MAX_XFB_BUFFERS];    /* transform feedback buffer bindings */
} nstate;

static nstate S;
static int pf_is_int(uint32_t pf);
static int pf_is_sint(MTLPixelFormat pf);
static int g_predict; /* the look-ahead pass of replay_run_submission: state ops run, draws only predict their pipeline, everything else is skipped */

/* per-submission context */
static id<MTLCommandBuffer> g_cb;
static id<MTLRenderCommandEncoder> g_re;
static id<MTLBlitCommandEncoder> g_be;
static id<MTLComputeCommandEncoder> g_ce;
static uint64_t g_enc_serial; /* identity of the current encoder (an address can be reused by the next one) */
static ring g_ring;
static uint64_t g_seq;

typedef struct post {
    int kind;
    uint64_t a;
    uint32_t b, c;
    struct post *next;
} post; /* actions after GPU completion */
static post *g_post, *g_post_tail;

/* Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED). NVN keeps one running sample counter that ResetCounter zeroes and ReportCounter writes out.
 * Metal counts samples per visibility-result slot, so draws are counted in "segments": a slot opens at the first draw after a pass begins or a
 * report / reset, and closes at the encoder end or the next report / reset. Each closed segment is added to the running counter when the command
 * buffer completes, in recording order with the resets and reports. Counting starts once the game first resets or reports SAMPLES_PASSED (before
 * that every report said 0 samples, and games that cull with occlusion queries, SMO, drew no world at all). */
enum { VIS_SLOTS = 16384 };
static int g_occl; /* the game uses SAMPLES_PASSED */
static id<MTLBuffer> g_vis; /* visibility results of the current command buffer */
static uint32_t g_vis_n; /* slots used in g_vis */
static int64_t g_vis_cur = -1; /* open slot, -1 = none */
static int g_pass_vis; /* the open render encoder has g_vis attached */
static uint64_t g_samples; /* running SAMPLES_PASSED counter (completion handlers only) */
static NSMutableArray<id<MTLBuffer>> *g_vis_pool;
static pthread_mutex_t g_vis_mu = PTHREAD_MUTEX_INITIALIZER;

/* pass description */
typedef struct {
    int open;
    uint32_t w, h, samples; /* guest pixel units */
    float sx, sy; /* allocation / guest size of the targets (render target scaling, mtl_scale.m) */
    uint32_t color_fmt[8];
    uint32_t depth_fmt, stencil_fmt;
    int ncolor;
    int clear_color[8];
    float clear_val[8][4];
    int clear_kind[8];
    int clear_depth, clear_stencil;
    float depth_val;
    uint32_t stencil_val;
    int discard_color[8], discard_ds;
    int draws;
    uint8_t sl_n, sl_pos[8]; /* programmable sample positions the pass was begun with (sl_n 0 = the default positions) */
} passinfo;
static passinfo P;

static id<MTLTexture> g_dummy_tex[8];
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *g_dss;
static NSMutableDictionary<NSNumber *, id> *g_clear_pipes;
static id<MTLDepthStencilState> g_dss_default;
static int g_pk_dirty = 1;
static GfxPipe *g_cur_pipe;
_Atomic uint64_t g_frame_draws;
/* NAPPLE_DRAWLOG=<first>[-<last>]: per-draw diagnostics (programs, render target, blend, bound textures) for presented frames in that range. */
static int g_drawlog_on, g_drawlog_verbose; /* verbose = per-draw text; NAPPLE_DRAWLOG_QUIET=1 keeps only the pass dumps / grids */
static int g_drawlog_ubo_words = 24; /* NAPPLE_DRAWLOG_UBO=<words>: how many float words of each bound uniform buffer to print */
static long g_drawlog_lo = -1, g_drawlog_hi = -1;
/* Diagnostic "event" clock: NAPPLE_GFX_EVENT_FS=<fragment program hash hex> starts a relative frame counter at the first draw that uses that program, so a
 * scripted run can capture the same scene regardless of machine load. With it set, NAPPLE_DRAWLOG frame numbers, the NAPPLE_SHADER_PROBE cycle and
 * NAPPLE_GFX_EVENT_SHOTS=<frame,frame,...> (relative frames; PNGs named <NAPPLE_GFX_EVENT_PREFIX>-<frame>.png) count from that event. */
static _Atomic int g_capture_req, g_capture_shots; /* live capture state (SIGUSR2), see gfx_capture_request */
static _Atomic unsigned g_capture_serial;
static uint64_t g_event_fs, g_event_frame; /* g_event_frame = absolute frame + 1 of the first draw (0 = not yet) */
static int g_event_init;
static uint64_t g_event_shots[64];
static int g_event_nshots;
static void event_init(void) {
    g_event_init = 1;
    const char *e = getenv("NAPPLE_GFX_EVENT_FS");
    if (e) g_event_fs = strtoull(e, NULL, 16);
    const char *l = getenv("NAPPLE_GFX_EVENT_SHOTS");
    if (l)
        for (char *p = (char *)l; *p && g_event_nshots < 64;) {
            g_event_shots[g_event_nshots++] = strtoull(p, &p, 10);
            if (*p == ',') p++;
            else break;
        }
}
static uint64_t rel_frame(void) {
    uint64_t f = atomic_load(&g_gfx.frames);
    if (!g_event_init) event_init();
    if (!g_event_fs) return f;
    return g_event_frame ? f - (g_event_frame - 1) : 0;
}
int gfx_event_shot_due(char *path, size_t n) { /* called by the present path for every frame about to be presented */
    if (!g_event_init) event_init();
    if (atomic_load(&g_capture_shots) > 0) {
        atomic_fetch_sub(&g_capture_shots, 1);
        const char *dir = getenv("NAPPLE_CAPTURE_DIR");
        snprintf(path, n, "%s/cap%03u-%llu.png", dir ? dir : ".", atomic_load(&g_capture_serial), (unsigned long long)atomic_load(&g_gfx.frames));
        return 1;
    }
    if (!g_event_frame || !g_event_nshots) return 0;
    uint64_t r = atomic_load(&g_gfx.frames) - (g_event_frame - 1);
    for (int i = 0; i < g_event_nshots; i++)
        if (g_event_shots[i] == r) {
            const char *pre = getenv("NAPPLE_GFX_EVENT_PREFIX");
            snprintf(path, n, "%s-%llu.png", pre ? pre : "event", (unsigned long long)r);
            return 1;
        }
    return 0;
}
/* Live capture (SIGUSR2 in the runtime): the next N presented frames get per-draw diagnostics and a screenshot each. */
void gfx_capture_request(int frames) {
    if (frames < 1) frames = 1;
    atomic_store(&g_capture_shots, frames);
    atomic_store(&g_capture_req, frames);
    atomic_fetch_add(&g_capture_serial, 1);
}
static void drawlog_update(void) {
    if (g_drawlog_lo == -1) {
        const char *e = getenv("NAPPLE_DRAWLOG");
        if (getenv("NAPPLE_DRAWLOG_UBO")) g_drawlog_ubo_words = atoi(getenv("NAPPLE_DRAWLOG_UBO"));
        g_drawlog_lo = 0;
        g_drawlog_hi = -2;
        if (e) {
            char *end;
            g_drawlog_lo = strtol(e, &end, 10);
            g_drawlog_hi = *end == '-' ? strtol(end + 1, NULL, 10) : g_drawlog_lo;
        }
    }
    long f = (long)rel_frame();
    int req = atomic_exchange(&g_capture_req, 0);
    if (req > 0) {
        g_drawlog_lo = f;
        g_drawlog_hi = f + req - 1;
    }
    g_drawlog_on = g_drawlog_hi >= 0 && f >= g_drawlog_lo && f <= g_drawlog_hi;
    g_drawlog_verbose = g_drawlog_on && !getenv("NAPPLE_DRAWLOG_QUIET");
}

static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) {
        S.blend[i] =
            (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    }
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    S.chmask.mask = 0xffffffffu;
    S.ms.enable = 1;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.stencil_wmask[0] = S.stencil_wmask[1] = S.stencil_rmask[0] = S.stencil_rmask[1] = 0xff;
    for (int i = 0; i < 16; i++) { S.dr[i][1] = 1.0f; }
    S.render_enable = 1;
    S.patch_size = 3;
    for (int i = 0; i < 4; i++) S.tess_outer[i] = 1.0f;
    S.tess_inner[0] = S.tess_inner[1] = 1.0f;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Encoders
 * --------------------------------------------------------------------------------------------------------------- */
static void dbg_dump_pass(void);
static void vis_close(void);
static void end_encoders(void) {
    if (g_re) {
        vis_close();
        g_pass_vis = 0;
        [g_re endEncoding];
        g_re = nil;
        P.open = 0;
        if (P.draws && g_drawlog_on) dbg_dump_pass();
        if (mtl_bcrt_pending()) mtl_bcrt_flush(g_cb);
    }
    if (g_be) {
        [g_be endEncoding];
        g_be = nil;
    }
    if (g_ce) {
        [g_ce endEncoding];
        g_ce = nil;
    }
}
static void flush_clears(void);
static id<MTLBlitCommandEncoder> blit(void) {
    if (!g_be) {
        flush_clears();
        end_encoders();
        g_be = [g_cb blitCommandEncoder];
    }
    return g_be;
}

static void post_add(int kind, uint64_t a, uint32_t b, uint32_t c) {
    post *p = malloc(sizeof *p);
    *p = (post){kind, a, b, c, NULL};
    if (g_post_tail) g_post_tail->next = p;
    else g_post = p;
    g_post_tail = p;
}

static void vis_close(void) {
    if (g_vis_cur < 0) return;
    post_add(4, (uint64_t)(uintptr_t)((uint64_t *)g_vis.contents + g_vis_cur), 0, 0);
    g_vis_cur = -1;
}
/* Called for every draw in an encoder from pass_begin: opens a counting segment when none is open. */
static void vis_draw(void) {
    if (!g_pass_vis || g_vis_cur >= 0) return;
    if (g_vis_n >= VIS_SLOTS) {
        [g_re setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        return;
    }
    g_vis_cur = g_vis_n++;
    ((uint64_t *)g_vis.contents)[g_vis_cur] = 0;
    [g_re setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)g_vis_cur * 8];
}

static void cb_begin(void) {
    if (g_cb) return;
    g_cb = [g_mtl.queue commandBuffer];
    ring_begin(&g_ring);
    g_post = g_post_tail = NULL;
}

static void cb_commit(int complete_seq) {
    if (!g_cb) {
        if (complete_seq) gfx_timeline_complete(g_seq);
        return;
    }
    end_encoders();
    post *list = g_post;
    g_post = g_post_tail = NULL;
    ring rg = g_ring;
    uint64_t seq = g_seq;
    uint64_t t0 = gfx_now_ns();
    id<MTLBuffer> vis = g_vis; /* kept alive by the block until the counts are read */
    g_vis = nil;
    g_vis_cur = -1;
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.error) LOGW("command buffer error: %s", cb.error.localizedDescription.UTF8String);
        for (post *p = list; p;) {
            post *n = p->next;
            if (p->kind == 1) { /* event write: a = address, b = mode, c = value */
                volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)p->a;
                if (p->b) __atomic_fetch_add(v, p->c, __ATOMIC_SEQ_CST);
                else __atomic_store_n(v, p->c, __ATOMIC_SEQ_CST);
            } else if (p->kind == 2) { /* counter report: a = address, b = type */
                NVNcounterData *cd = (NVNcounterData *)(uintptr_t)p->a;
                cd->counter = p->b == NVN_COUNTER_TYPE_SAMPLES_PASSED ? g_samples : 0;
                cd->timestamp = (uint64_t)((double)gfx_now_ns() * 0.0192);
            } else if (p->kind == 4) { /* closed occlusion segment: a = its visibility result */
                g_samples += *(volatile uint64_t *)(uintptr_t)p->a;
            } else if (p->kind == 5) { /* SAMPLES_PASSED reset */
                g_samples = 0;
            } else if (p->kind == 6) { /* ReportValue: a = address, b = 32-bit value */
                __atomic_store_n((uint32_t *)(uintptr_t)p->a, p->b, __ATOMIC_SEQ_CST);
            } else if (p->kind == 3) { /* sync signal: a = sync_t* */
                sync_t *s = (sync_t *)(uintptr_t)p->a;
                s->seq = seq;
                s->kind = 0;
            }
            free(p);
            p = n;
        }
        ring r2 = rg;
        ring_end(&r2);
        atomic_store(&g_frame_gpu_us, (gfx_now_ns() - t0) / 1000);
        if (cb.GPUEndTime > cb.GPUStartTime) mtl_stats_gpu_add((cb.GPUEndTime - cb.GPUStartTime) * 1000.0);
        if (g_prof_on) atomic_fetch_add(&g_prof[2], gfx_now_ns() - t0);
        if (vis) {
            pthread_mutex_lock(&g_vis_mu);
            if (g_vis_pool.count < 8) [g_vis_pool addObject:vis];
            pthread_mutex_unlock(&g_vis_mu);
        }
        if (complete_seq) gfx_timeline_complete(seq);
        else gfx_wake();
    }];
    [g_cb commit];
    g_cb = nil;
    memset(&g_ring, 0, sizeof g_ring);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Render pass
 * --------------------------------------------------------------------------------------------------------------- */
static void pass_unify_scale(
    void) { /* all attachments of a pass share one allocation size: lift the unscaled ones to the largest factor present (see mtl_tex_force_scale); factors below 1 count too */
    GfxTex *att[9];
    int n = 0;
    float mx = 1, my = 1;
    for (uint32_t i = 0; i < S.rt.n && i < 8; i++) {
        GfxTex *t = mtl_tex_get(&S.rt.color[i].s, 1);
        if (t) att[n++] = t;
    }
    GfxTex *d = S.rt.depth.s.valid ? mtl_tex_get(&S.rt.depth.s, 1) : nil;
    if (d) att[n++] = d;
    for (int i = 0; i < n; i++) { /* the factor farthest from 1 wins: below 1 (render scale under 100 %, a narrow window) as well as above it */
        if (fabsf(att[i]->rsx - 1) > fabsf(mx - 1)) mx = att[i]->rsx;
        if (fabsf(att[i]->rsy - 1) > fabsf(my - 1)) my = att[i]->rsy;
    }
    if (mx == 1 && my == 1) return;
    {
        int mixed = 0;
        for (int i = 0; i < n; i++)
            if (att[i]->rsx != mx || att[i]->rsy != my) mixed = 1;
        if (mixed && getenv("NAPPLE_SCALE_DIAG")) {
            static int once;
            if (once++ < 6)
                for (int i = 0; i < n; i++)
                    fprintf(stderr, "[scale-diag] unify att %d: guest %ux%u rsx %.3f tex %lux%lu type %d samples %lu stamp %d gpu_owned %d\n", i, att[i]->w,
                            att[i]->h, att[i]->rsx, (unsigned long)att[i]->tex.width, (unsigned long)att[i]->tex.height, (int)att[i]->tex.textureType,
                            (unsigned long)att[i]->tex.sampleCount, (int)att[i]->stamp, (int)att[i]->gpu_owned);
        }
    }
    for (int i = 0; i < n; i++)
        if (att[i]->rsx != mx || att[i]->rsy != my) {
            if (att[i]->w == att[0]->w && att[i]->h == att[0]->h) mtl_tex_force_scale(att[i], mx, my); /* same guest size as the rest: it is screen sized */
        }
    /* an attachment that cannot be lifted (a mipped texture is never re-allocated) leaves the pass with attachments of different sizes: Metal then renders only
     * their overlap, or aborts on the scissor of a pass smaller than the guest size (ACNH and MK8D at 0.375x). The scaled siblings go back to their guest size. */
    for (int i = 0; i < n; i++)
        if (att[i]->w == att[0]->w && att[i]->h == att[0]->h && (att[i]->rsx != mx || att[i]->rsy != my)) {
            for (int k = 0; k < n; k++)
                if (att[k]->w == att[0]->w && att[k]->h == att[0]->h) mtl_tex_force_scale(att[k], 1, 1);
            break;
        }
}
static void pass_info_from_rt(void) {
    pass_unify_scale();
    P.ncolor = 0;
    P.w = P.h = 0;
    P.samples = 1;
    P.depth_fmt = P.stencil_fmt = 0;
    P.sx = P.sy = 1;
    memset(P.color_fmt, 0, sizeof P.color_fmt);
    for (uint32_t i = 0; i < S.rt.n && i < 8; i++) {
        GfxTex *t = mtl_tex_get(&S.rt.color[i].s, 1);
        if (!t) continue;
        uint32_t lv, sl;
        P.color_fmt[i] =
            (uint32_t)mtl_tex_attach_view(t, &S.rt.color[i].view, S.rt.color[i].has_view, &lv, &sl).pixelFormat; /* the view's format, if it has one */
        if (!P.w) {
            P.w = t->w;
            P.h = t->h;
            P.samples = t->samples;
            P.sx = t->rsx > 0 ? t->rsx : 1;
            P.sy = t->rsy > 0 ? t->rsy : 1;
        }
        {
            id<MTLTexture> av = mtl_tex_attach_view(t, &S.rt.color[i].view, S.rt.color[i].has_view, &lv, &sl);
            if (av != t->tex && (NSUInteger)lroundf((float)t->w * (t->rsx > 0 ? t->rsx : 1)) != av.width && i == 0 && t->tex.width != av.width) {
                P.w = (uint32_t)av.width;
                P.h = (uint32_t)av.height;
                P.sx = P.sy = 1;
            }
        } /* block-grid render of a compressed texture */
        P.ncolor = (int)i + 1;
    }
    GfxTex *d = S.rt.depth.s.valid ? mtl_tex_get(&S.rt.depth.s, 1) : nil;
    if (d) {
        P.depth_fmt = (uint32_t)d->pf;
        P.stencil_fmt = d->has_stencil ? (uint32_t)d->pf : 0;
        if (d->pf == MTLPixelFormatStencil8) P.depth_fmt = 0;
        if (!P.w) {
            P.w = d->w;
            P.h = d->h;
            P.samples = d->samples;
            P.sx = d->rsx > 0 ? d->rsx : 1;
            P.sy = d->rsy > 0 ? d->rsy : 1;
        }
    }
}

/* Programmable sample positions the bound multisample state asks for in this pass (n = 0: the default positions). Metal sets them per pass. */
static void sample_positions(uint8_t *n, uint8_t pos[8]) {
    *n = 0;
    memset(pos, 0, 8);
    if (!S.ms.sl_en || !S.ms.sl_n || P.samples < 2 || !g_mtl.dev.programmableSamplePositionsSupported) return;
    *n = S.ms.sl_n < 8 ? S.ms.sl_n : 8;
    memcpy(pos, S.ms.sl_pos, *n);
}
id<MTLTexture> g_dbg_composite;
id<MTLTexture> mtl_dbg_composite(void) { return g_dbg_composite; }
static int pass_begin(void) {
    if (P.open) return 1;
    if (!S.rt_valid) return 0;
    end_encoders();
    pass_info_from_rt();
    if (!P.w) return 0;
    if (getenv("NAPPLE_SCALE_DIAG")) { /* every distinct pass whose attachments do not all match guest size x scale (render target scaling bugs) */
        static uint64_t seen[256];
        static int nseen;
        for (uint32_t i = 0; i <= S.rt.n && i < 9; i++) {
            GfxTex *t = i < S.rt.n ? mtl_tex_get(&S.rt.color[i].s, 1) : (S.rt.depth.s.valid ? mtl_tex_get(&S.rt.depth.s, 1) : nil);
            if (!t) continue;
            uint32_t ew = (uint32_t)lroundf((float)P.w * P.sx), eh = (uint32_t)lroundf((float)P.h * P.sy);
            if (t->tex.width == ew && t->tex.height == eh && t->w == P.w) continue;
            uint64_t key = ((uint64_t)t->tex.width << 40) ^ ((uint64_t)t->tex.height << 24) ^ ((uint64_t)P.w << 8) ^ i ^ ((uint64_t)(P.sx * 1000) << 52);
            int dup = 0;
            for (int k = 0; k < nseen; k++)
                if (seen[k] == key) dup = 1;
            if (dup || nseen >= 256) continue;
            seen[nseen++] = key;
            fprintf(stderr, "[scale-diag] pass att %u (%s): tex %lux%lu guest %ux%u rsx %.3f | pass guest %ux%u scale %.3f -> expected %ux%u\n", i,
                    i < S.rt.n ? "color" : "depth", (unsigned long)t->tex.width, (unsigned long)t->tex.height, t->w, t->h, t->rsx, P.w, P.h, P.sx, ew, eh);
        }
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (uint32_t i = 0; i < S.rt.n && i < 8; i++) {
        GfxTex *t = mtl_tex_get(&S.rt.color[i].s, 1);
        if (!t) continue;
        uint32_t lv, sl;
        id<MTLTexture> mt = mtl_tex_attach_view(t, &S.rt.color[i].view, S.rt.color[i].has_view, &lv, &sl);
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
        a.texture = mt;
        a.level = lv;
        if (i == 0 && t->pf == MTLPixelFormatRGB10A2Unorm && t->w >= 1280)
            g_dbg_composite = mt; /* NAPPLE_SHOT_COMPOSITE: the last 3D-scene composite, shown instead of the window texture in screenshots */
        if (getenv("NAPPLE_CUBELOG") && (t->tex.textureType == MTLTextureTypeCube || t->tex.textureType == MTLTextureTypeCubeArray)) {
            static int n;
            if (n++ < 100000) fprintf(stderr, "[cubelog] frame %llu pass into %lux%lu cube type %lu fmt %lu slice %u level %u load %d clear %d\n", (unsigned long long)atomic_load(&g_gfx.frames), (unsigned long)t->tex.width,
                    (unsigned long)t->tex.height, (unsigned long)t->tex.textureType, (unsigned long)t->tex.pixelFormat, sl, lv, P.discard_color[i] ? 0 : 1, P.clear_color[i]);
        }
        if (t->tex.textureType == MTLTextureType3D) a.depthPlane = sl;
        else a.slice = sl;
        if (P.clear_color[i]) {
            a.loadAction = MTLLoadActionClear;
            a.clearColor = MTLClearColorMake(P.clear_val[i][0], P.clear_val[i][1], P.clear_val[i][2], P.clear_val[i][3]);
        } else a.loadAction = P.discard_color[i] ? MTLLoadActionDontCare : MTLLoadActionLoad;
        a.storeAction = MTLStoreActionStore;
    }
    if (S.rt.depth.s.valid) {
        GfxTex *d = mtl_tex_get(&S.rt.depth.s, 1);
        if (d) {
            uint32_t lv, sl;
            id<MTLTexture> mt = mtl_tex_attach_view(d, &S.rt.depth.view, S.rt.depth.has_view, &lv, &sl);
            if (d->pf != MTLPixelFormatStencil8) {
                rp.depthAttachment.texture = mt;
                rp.depthAttachment.level = lv;
                rp.depthAttachment.slice = sl;
                rp.depthAttachment.loadAction = P.clear_depth ? MTLLoadActionClear : (P.discard_ds ? MTLLoadActionDontCare : MTLLoadActionLoad);
                rp.depthAttachment.clearDepth = P.depth_val;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
            }
            if (d->has_stencil) {
                rp.stencilAttachment.texture = mt;
                rp.stencilAttachment.level = lv;
                rp.stencilAttachment.slice = sl;
                rp.stencilAttachment.loadAction = P.clear_stencil ? MTLLoadActionClear : (P.discard_ds ? MTLLoadActionDontCare : MTLLoadActionLoad);
                rp.stencilAttachment.clearStencil = P.stencil_val;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }
    if (g_occl) {
        if (!g_vis) {
            static dispatch_once_t once;
            dispatch_once(&once, ^{ g_vis_pool = [NSMutableArray array]; });
            pthread_mutex_lock(&g_vis_mu);
            g_vis = g_vis_pool.lastObject;
            if (g_vis) [g_vis_pool removeLastObject];
            pthread_mutex_unlock(&g_vis_mu);
            if (!g_vis) g_vis = [g_mtl.dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared];
            g_vis_n = 0;
        }
        rp.visibilityResultBuffer = g_vis;
    }
    sample_positions(&P.sl_n, P.sl_pos);
    if (P.sl_n) {
        MTLSamplePosition sp[16];
        for (uint32_t i = 0; i < P.samples && i < 16; i++) {
            uint8_t v = i < P.sl_n ? P.sl_pos[i] : 0x88;
            float x = (float)(v & 15) / 16.0f, y = (float)(v >> 4) / 16.0f;
            if (g_gfx.device_origin == 0 && !g_gfx.origin_flip) y = 15.0f / 16.0f - y; /* lower-left window coordinates with top-down rows */
            sp[i] = MTLSamplePositionMake(x, y);
        }
        [rp setSamplePositions:sp count:P.samples];
    }
    /* Two colour attachments that are the same image (texture, level, slice): Metal's API validation aborts ("Attachment 3 overlaps with attachment 4", Mario Kart 8);
     * the later one is dropped (its writes would land on the same pixels). */
    for (uint32_t i = 1; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
        if (!a.texture) continue;
        for (uint32_t j = 0; j < i; j++) {
            MTLRenderPassColorAttachmentDescriptor *b = rp.colorAttachments[j];
            if (b.texture == a.texture && b.level == a.level && b.slice == a.slice && b.depthPlane == a.depthPlane) {
                LOG_ONCE(NAPPLE_LOG_INFO, "colour attachments %u and %u are the same image: the later one is not attached", j, i);
                a.texture = nil;
                P.color_fmt[i] = 0;   /* the pipeline then has no attachment there (its writes are dropped), as the attachment check below expects */
                break;
            }
        }
    }
    /* The pipeline formats (P) must be the formats actually attached: Metal's API validation aborts on a mismatch (seen on Animal Crossing: a pipeline built with no
     * colour attachment 0 drawn into a pass whose attachment 0 is RGBA16Float). P was derived from the same render targets, so a difference means the texture was created
     * or replaced in between: follow the encoder. */
    for (uint32_t i = 0; i < 8; i++) {
        id<MTLTexture> at = rp.colorAttachments[i].texture;
        uint32_t actual = at ? (uint32_t)at.pixelFormat : 0;
        if (actual != P.color_fmt[i]) {
            LOG_ONCE(NAPPLE_LOG_WARN, "render pass colour attachment %u is format %u but the pipeline state said %u: following the attachment", i, actual,
                     P.color_fmt[i]);
            P.color_fmt[i] = actual;
            if ((int)i >= P.ncolor && actual) P.ncolor = (int)i + 1;
        }
    }
    g_re = [g_cb renderCommandEncoderWithDescriptor:rp];
    if (!g_re) return 0;
    g_pass_vis = g_occl && g_vis;
    g_enc_serial++;
    P.open = 1;
    P.draws = 0;
    memset(P.clear_color, 0, sizeof P.clear_color);
    P.clear_depth = P.clear_stencil = 0;
    memset(P.discard_color, 0, sizeof P.discard_color);
    P.discard_ds = 0;
    g_pk_dirty = 1;
    return 1;
}

static void cb_commit(int complete_seq);
static void cb_begin(void);
static int g_dbg_pass;
static uint64_t g_last_draw_fs; /* fragment program of the most recent draw (S.prog may already name the next one when a pass ends) */
/* NAPPLE_DRAWLOG_RT=<dir>: after each pass with draws in a NAPPLE_DRAWLOG frame, wait for the GPU and write color attachment 0 as a PPM. */
static void dbg_dump_att(const char *dir, uint32_t a, int npass);
static GfxTex *g_dbg_3d; /* a small 3D texture bound by a logged draw: its slices are printed at the next pass dump (GPU idle there) */
/* (NAPPLE_DRAWLOG_RT_ALL=1: every color attachment, files pass<N>-rt<i>-...) */
static void dbg_dump_pass(void) {
    const char *dir = getenv("NAPPLE_DRAWLOG_RT");
    if (!dir || !g_drawlog_on || !S.rt.n) return;
    id<MTLCommandBuffer> cb = g_cb;
    cb_commit(0);
    [cb waitUntilCompleted];
    cb_begin();
    int npass = g_dbg_pass++;
    if (g_dbg_3d && g_dbg_3d->pf == MTLPixelFormatRG11B10Float) {
        GfxTex *t3 = g_dbg_3d;
        g_dbg_3d = NULL;
        NSUInteger W = t3->tex.width, H = t3->tex.height, D = t3->tex.depth;
        uint32_t *px = calloc(W * H * D, 4);
        [t3->tex getBytes:px bytesPerRow:W * 4 bytesPerImage:W * H * 4 fromRegion:MTLRegionMake3D(0, 0, 0, W, H, D) mipmapLevel:0 slice:0];
        fprintf(stderr, "[drawlog] 3d tex %#llx %lux%lux%lu slice means (R G B):", (unsigned long long)(uintptr_t)t3->key.mem, (unsigned long)W,
                (unsigned long)H, (unsigned long)D);
        for (NSUInteger z = 0; z < D; z++) {
            double m[3] = {0, 0, 0};
            for (NSUInteger q = 0; q < W * H; q++) {
                uint32_t v = px[z * W * H + q];
                uint32_t c3[3] = {v & 0x7ff, (v >> 11) & 0x7ff, (v >> 22) & 0x3ff};
                int mb[3] = {6, 6, 5};
                for (int k = 0; k < 3; k++) {
                    int ex = (int)(c3[k] >> mb[k]), mt = (int)(c3[k] & ((1u << mb[k]) - 1));
                    m[k] += ex ? (1 + mt / (double)(1 << mb[k])) * exp2(ex - 15) : mt / (double)(1 << mb[k]) / 16384.0;
                }
            }
            fprintf(stderr, " [%.2f %.2f %.2f]", m[0] / (W * H), m[1] / (W * H), m[2] / (W * H));
        }
        fprintf(stderr, "\n");
        free(px);
    }
    uint32_t n = getenv("NAPPLE_DRAWLOG_RT_ALL") ? S.rt.n : 1;
    for (uint32_t a = 0; a < n && a < 8; a++) dbg_dump_att(dir, a, npass);
}
static void dbg_dump_att(const char *dir, uint32_t a, int npass) {
    GfxTex *t = mtl_tex_get(&S.rt.color[a].s, 1);
    if (!t || t->samples > 1) return;
    uint32_t w = t->w, h = t->h;
    MTLPixelFormat pf = t->pf;
    uint32_t lv = S.rt.color[a].has_view && S.rt.color[a].view.set_levels ? S.rt.color[a].view.base_level : 0;
    if (lv) return;
    size_t bpp = (pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRG32Float) ? 8
                 : (pf == MTLPixelFormatR8Unorm)                                    ? 1
                 : (pf == MTLPixelFormatR16Float)                                   ? 2
                                                                                    : 4;
    uint8_t *buf = malloc((size_t)w * h * bpp);
    if (!buf) return;
    NSUInteger slice = S.rt.color[a].has_view && S.rt.color[a].view.set_layers ? S.rt.color[a].view.min_layer : 0;
    if (t->tex.textureType == MTLTextureType2DArray)
        [t->tex getBytes:buf bytesPerRow:w * bpp bytesPerImage:(size_t)w * h * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 slice:slice];
    else [t->tex getBytes:buf bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    char path[1024];
    if (a) snprintf(path, sizeof path, "%s/pass%03d-rt%u-%ux%u-pf%d.ppm", dir, npass, a, w, h, (int)pf);
    else snprintf(path, sizeof path, "%s/pass%03d-%ux%u-pf%d.ppm", dir, npass, w, h, (int)pf);
    int grid = getenv("NAPPLE_DRAWLOG_GRID") != NULL; /* NAPPLE_DRAWLOG_GRID=1: print exact values at a 3x3 grid of pixels instead of writing images */
    FILE *f = grid ? fopen("/dev/null", "wb") : fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        float cmax = 0, csum = 0;
        int cnan = 0;
        for (uint32_t i = 0; i < w * h; i++) {
            float c[3] = {0, 0, 0};
            const uint8_t *p = buf + (size_t)i * bpp;
            switch ((int)pf) {
                case MTLPixelFormatRGBA8Unorm:
                case MTLPixelFormatRGBA8Unorm_sRGB:
                    c[0] = p[0] / 255.f;
                    c[1] = p[1] / 255.f;
                    c[2] = p[2] / 255.f;
                    break;
                case MTLPixelFormatBGRA8Unorm:
                case MTLPixelFormatBGRA8Unorm_sRGB:
                    c[0] = p[2] / 255.f;
                    c[1] = p[1] / 255.f;
                    c[2] = p[0] / 255.f;
                    break;
                case MTLPixelFormatRGB10A2Unorm: {
                    uint32_t v = *(const uint32_t *)p;
                    c[0] = (v & 1023) / 1023.f;
                    c[1] = ((v >> 10) & 1023) / 1023.f;
                    c[2] = ((v >> 20) & 1023) / 1023.f;
                    break;
                }
                case MTLPixelFormatRG11B10Float: {
                    uint32_t v = *(const uint32_t *)p;
                    uint32_t m[3] = {v & 0x7ff, (v >> 11) & 0x7ff, (v >> 22) & 0x3ff};
                    int mb[3] = {6, 6, 5};
                    for (int k = 0; k < 3; k++) {
                        int e = m[k] >> mb[k], mant = m[k] & ((1 << mb[k]) - 1);
                        c[k] = e == 0 ? mant / (float)(1 << mb[k]) / 16384.f : (1.f + mant / (float)(1 << mb[k])) * exp2f((float)e - 15);
                    }
                    break;
                }
                case MTLPixelFormatRGBA16Float: {
                    const __fp16 *hp = (const __fp16 *)p;
                    c[0] = hp[0];
                    c[1] = hp[1];
                    c[2] = hp[2];
                    break;
                }
                case MTLPixelFormatR16Float: {
                    c[0] = c[1] = c[2] = *(const __fp16 *)p;
                    break;
                }
                case MTLPixelFormatR8Unorm: c[0] = c[1] = c[2] = p[0] / 255.f; break;
                case MTLPixelFormatRG16Snorm: {
                    const int16_t *sp = (const int16_t *)p;
                    c[0] = sp[0] / 32767.f * 0.5f + 0.5f;
                    c[1] = sp[1] / 32767.f * 0.5f + 0.5f;
                    break;
                } /* shown as x*0.5+0.5 */
                case MTLPixelFormatRG16Float: {
                    const __fp16 *hp = (const __fp16 *)p;
                    c[0] = hp[0];
                    c[1] = hp[1];
                    break;
                }
                default:
                    c[0] = p[0] / 255.f;
                    c[1] = p[1] / 255.f;
                    c[2] = p[2] / 255.f;
                    break;
            }
            if (grid) { /* 24 x 12 sample grid; print the samples this pass changed (with NAPPLE_DRAWLOG_EACH: what one draw wrote) */
                uint32_t gx = i % w, gy = i / w;
                if (gx % (w / 24) == w / 48 && gy % (h / 12) == h / 24 && gx / (w / 24) < 24 && gy / (h / 12) < 12) {
                    static float prev[24 * 12 * 3];
                    static uint32_t pw, ph;
                    static int nshown;
                    int cell = (int)(gy / (h / 12)) * 24 + (int)(gx / (w / 24));
                    if (i == 0 || pw != w || ph != h) {
                        if (pw != w || ph != h) memset(prev, 0, sizeof prev);
                        pw = w;
                        ph = h;
                    }
                    if (memcmp(prev + cell * 3, c, 12) && nshown < 12) {
                        nshown++;
                        fprintf(stderr, "[grid] f%llu pass %d fs %016llx %ux%u (%u,%u): %g %g %g -> %g %g %g\n", (unsigned long long)rel_frame(), npass,
                                (unsigned long long)g_last_draw_fs, w, h, gx, gy, prev[cell * 3], prev[cell * 3 + 1], prev[cell * 3 + 2], c[0], c[1], c[2]);
                    }
                    memcpy(prev + cell * 3, c, 12);
                    if (cell == 24 * 12 - 1) nshown = 0;
                }
            } else {
                static int shown;
                if (shown < 6 && (c[0] > 0 || c[1] > 0 || c[2] > 0) && i > (uint32_t)(w * h / 3)) {
                    shown++;
                    fprintf(stderr, "[drawlog]   px %u,%u = %g %g %g\n", i % w, i / w, c[0], c[1], c[2]);
                }
                if (i == 0) shown = 0;
            }
            for (int k = 0; k < 3; k++) {
                if (c[k] != c[k]) cnan++;
                else {
                    if (c[k] > cmax) cmax = c[k];
                    csum += c[k];
                }
            }
            uint8_t o[3];
            for (int k = 0; k < 3; k++) {
                float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                o[k] = (uint8_t)(v * 255.f + 0.5f);
            }
            fwrite(o, 1, 3, f);
        }
        fclose(f);
        fprintf(stderr, "[drawlog] dumped %s max %g mean %g nan %d mem %#llx\n", path, cmax, csum / (3.0 * w * h), cnan,
                (unsigned long long)(uintptr_t)t->key.mem);
    }
    free(buf);
}
static void pass_end(void) {
    if (g_re) {
        vis_close();
        g_pass_vis = 0;
        [g_re endEncoding];
        g_re = nil;
        if (P.draws && g_drawlog_on) {
            P.open = 0;
            dbg_dump_pass();
        }
        if (mtl_bcrt_pending()) mtl_bcrt_flush(g_cb);
    }
    P.open = 0;
}

/* Clears folded into the next pass must still happen when no draw follows. */
/* The active scissor in Metal's top-left convention (clamped to the pass); NVN counts y from the bottom in the default lower-left window origin mode. */
static MTLScissorRect scissor_rect(void) {
    MTLScissorRect sr = {0, 0, (NSUInteger)lroundf((float)P.w * P.sx), (NSUInteger)lroundf((float)P.h * P.sy)};
    if (!S.sc_set) return sr;
    int32_t x = S.sc[0][0] < 0 ? 0 : S.sc[0][0], y = S.sc[0][1], w = S.sc[0][2], h = S.sc[0][3];
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (g_gfx.device_origin == 0 && !g_gfx.origin_flip) y = (int32_t)P.h - y - h;
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x > (int32_t)P.w) x = (int32_t)P.w;
    if (y > (int32_t)P.h) y = (int32_t)P.h;
    if (w > (int32_t)P.w - x) w = (int32_t)P.w - x;
    if (h > (int32_t)P.h - y) h = (int32_t)P.h - y;
    if (h < 0) h = 0;
    if (P.sx != 1 || P.sy != 1) { /* guest pixels -> the scaled allocation (edges rounded outwards so a full-screen scissor stays full) */
        int32_t x1 = (int32_t)lroundf((float)(x + w) * P.sx), y1 = (int32_t)lroundf((float)(y + h) * P.sy);
        x = (int32_t)lroundf((float)x * P.sx);
        y = (int32_t)lroundf((float)y * P.sy);
        w = x1 - x;
        h = y1 - y;
        int32_t aw = (int32_t)lroundf((float)P.w * P.sx), ah = (int32_t)lroundf((float)P.h * P.sy);
        if (x + w > aw) w = aw - x;
        if (y + h > ah) h = ah - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
    }
    return (MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h};
}
static void flush_clears(void) {
    int pending = P.clear_depth || P.clear_stencil;
    for (int i = 0; i < 8; i++) pending |= P.clear_color[i];
    if (!pending || P.open) return;
    if (pass_begin()) pass_end();
}

/* A clear folded into the load action of the next pass is only valid while nothing was drawn and the area is the whole target. */
static int clear_covers_target(void) {
    if (!S.sc_set) return 1;
    return S.sc[0][0] <= 0 && S.sc[0][1] <= 0 && (uint32_t)(S.sc[0][0] + S.sc[0][2]) >= P.w && (uint32_t)(S.sc[0][1] + S.sc[0][3]) >= P.h;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Clear by drawing (partial clears)
 * --------------------------------------------------------------------------------------------------------------- */
static NSString *const kClearMSL = @"#include <metal_stdlib>\nusing namespace metal;\n"
                                    "struct V { float4 pos [[position]]; };\n"
                                    "vertex V cv(uint vid [[vertex_id]], constant float &z [[buffer(0)]]) { float2 p = float2((vid << 1) & 2, vid & 2); V o; "
                                    "o.pos = float4(p * 2.0 - 1.0, z, 1); return o; }\n"
                                    "fragment float4 cf(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n"
                                    "fragment int4 cfi(V in [[stage_in]], constant int4 &c [[buffer(0)]]) { return c; }\n"
                                    "fragment uint4 cfu(V in [[stage_in]], constant uint4 &c [[buffer(0)]]) { return c; }\n"
                                    "fragment void cfd(V in [[stage_in]]) { }\n";

static id<MTLRenderPipelineState> clear_pipe(MTLPixelFormat color_pf, int kind, uint32_t mask, MTLPixelFormat depth_pf, MTLPixelFormat stencil_pf,
                                             uint32_t samples) {
    static id<MTLLibrary> lib;
    if (!lib) {
        NSError *e = nil;
        lib = [g_mtl.dev newLibraryWithSource:kClearMSL options:nil error:&e];
        if (!lib) gfx_die("clear shader: %s", e.localizedDescription.UTF8String);
    }
    if (!g_clear_pipes) g_clear_pipes = [NSMutableDictionary dictionary];
    uint64_t kh =
        (uint64_t)color_pf | (uint64_t)kind << 12 | (uint64_t)mask << 16 | (uint64_t)depth_pf << 24 | (uint64_t)stencil_pf << 36 | (uint64_t)samples << 48;
    kh = fnv1a(&P.color_fmt[1], sizeof P.color_fmt - sizeof P.color_fmt[0],
               kh); /* the pass's other attachments are part of the pipeline's framebuffer description */
    NSNumber *k = @(kh);
    id p = g_clear_pipes[k];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"cv"];
    d.fragmentFunction = [lib newFunctionWithName:color_pf ? (kind == 1 ? @"cfi" : kind == 2 ? @"cfu" : @"cf") : @"cfd"];
    d.rasterSampleCount = samples ? samples : 1;
    if (color_pf) {
        d.colorAttachments[0].pixelFormat = color_pf;
        d.colorAttachments[0].writeMask = (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
                                          (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    }
    for (int i = 1; i < 8; i++) { /* Metal's API validation: a pipeline must declare every colour attachment of the pass; the clear writes only attachment 0 */
        if (!P.color_fmt[i]) continue;
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)P.color_fmt[i];
        d.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
    }
    d.depthAttachmentPixelFormat = depth_pf;
    d.stencilAttachmentPixelFormat = stencil_pf;
    NSError *e = nil;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) {
        LOGW("clear pipeline: %s", e.localizedDescription.UTF8String);
        return nil;
    }
    g_clear_pipes[k] = p;
    return p;
}

/* The depth / stencil clear quad writes no colour, but its pipeline must still declare the pass's colour attachments (Metal's API validation aborts when a pipeline's colour
 * formats differ from the framebuffer's, Animal Crossing's partial depth clears): the same formats, write mask none. */
static id<MTLRenderPipelineState> clear_pipe_depth(MTLPixelFormat depth_pf, MTLPixelFormat stencil_pf, uint32_t samples) {
    static NSMutableDictionary<NSNumber *, id> *cache;
    if (!cache) cache = [NSMutableDictionary dictionary];
    uint64_t h = fnv1a(P.color_fmt, sizeof P.color_fmt, FNV_INIT);
    h = fnv1a(&depth_pf, sizeof depth_pf, h);
    h = fnv1a(&stencil_pf, sizeof stencil_pf, h);
    h = fnv1a(&samples, sizeof samples, h);
    NSNumber *k = @(h);
    id p = cache[k];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    static id<MTLLibrary> lib;
    if (!lib) lib = [g_mtl.dev newLibraryWithSource:kClearMSL options:nil error:nil];
    d.vertexFunction = [lib newFunctionWithName:@"cv"];
    d.fragmentFunction = [lib newFunctionWithName:@"cfd"];
    d.rasterSampleCount = samples ? samples : 1;
    for (int i = 0; i < 8; i++) {
        if (!P.color_fmt[i]) continue;
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)P.color_fmt[i];
        d.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
    }
    d.depthAttachmentPixelFormat = depth_pf;
    d.stencilAttachmentPixelFormat = stencil_pf;
    NSError *e = nil;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) {
        LOGW("depth clear pipeline: %s", e.localizedDescription.UTF8String);
        return nil;
    }
    cache[k] = p;
    return p;
}

static void clear_color_quad(uint32_t index, uint32_t kind, uint32_t mask, const uint32_t v[4]) {
    if (!pass_begin()) return;
    /* a clear quad writes only attachment `index`: use a pass-wide pipeline that targets color(0), so bind through a dedicated mapping */
    if (index != 0) { LOG_ONCE(NAPPLE_LOG_WARN, "partial clear of color target %u falls back to a pass restart", index); }
    id<MTLRenderPipelineState> ps =
        clear_pipe((MTLPixelFormat)P.color_fmt[index], (int)kind, mask, (MTLPixelFormat)P.depth_fmt, (MTLPixelFormat)P.stencil_fmt, P.samples);
    if (!ps || index != 0) return;
    id<MTLDepthStencilState> ds;
    {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        ds = [g_mtl.dev newDepthStencilStateWithDescriptor:dd];
    }
    float z = 0;
    [g_re setRenderPipelineState:ps];
    [g_re setDepthStencilState:ds];
    [g_re setCullMode:MTLCullModeNone];
    [g_re setVertexBytes:&z length:4 atIndex:0];
    [g_re setFragmentBytes:v length:16 atIndex:0];
    MTLViewport vp = {0, 0, (double)lroundf((float)P.w * P.sx), (double)lroundf((float)P.h * P.sy), 0, 1};
    [g_re setViewport:vp];
    [g_re setScissorRect:scissor_rect()];
    [g_re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    g_pk_dirty = 1;
    g_cur_pipe = nil;
}

static void do_clear_color(const p_clear_color *c) {
    if (c->index >= 8 || !S.render_enable) return;
    GfxTex *t = (c->index < S.rt.n) ? mtl_tex_get(&S.rt.color[c->index].s, 1) : nil;
    if (!t) return;
    if (!P.open) pass_info_from_rt();
    int full_mask = c->mask == 0xf;
    if ((!P.open || P.draws == 0) && full_mask && clear_covers_target() && c->kind == 0) {
        float f[4];
        memcpy(f, c->v, 16);
        if (P.open) { /* nothing drawn yet: restart the pass with the clear folded in */
            pass_end();
        }
        P.clear_color[c->index] = 1;
        memcpy(P.clear_val[c->index], f, 16);
        return;
    }
    clear_color_quad(c->index, c->kind, c->mask, c->v);
}

static void do_clear_ds(const p_clear_ds *c) {
    if (!S.rt.depth.s.valid || !S.render_enable) return;
    GfxTex *d = mtl_tex_get(&S.rt.depth.s, 1);
    if (!d) return;
    if (!P.open) pass_info_from_rt();
    int fold = (!P.open || P.draws == 0) && clear_covers_target();
    if (fold) {
        if (P.open) pass_end();
        if (c->depth_mask) {
            P.clear_depth = 1;
            P.depth_val = c->depth;
        }
        if (c->stencil_mask) {
            P.clear_stencil = 1;
            P.stencil_val = c->stencil & c->stencil_mask;
        }
        return;
    }
    if (!pass_begin()) return;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = c->depth_mask ? YES : NO;
    if (d->has_stencil && c->stencil_mask) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.stencilFailureOperation = sd.depthFailureOperation = sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = c->stencil_mask & 0xff;
        sd.readMask = 0xff;
        dd.frontFaceStencil = dd.backFaceStencil = sd;
    }
    id<MTLDepthStencilState> ds = [g_mtl.dev newDepthStencilStateWithDescriptor:dd];
    id<MTLRenderPipelineState> ps = clear_pipe_depth((MTLPixelFormat)P.depth_fmt, (MTLPixelFormat)P.stencil_fmt, P.samples);
    if (!ps) return;
    float z = c->depth;
    [g_re setRenderPipelineState:ps];
    [g_re setDepthStencilState:ds];
    [g_re setCullMode:MTLCullModeNone];
    [g_re setStencilReferenceValue:c->stencil & 0xff];
    [g_re setVertexBytes:&z length:4 atIndex:0];
    MTLViewport vp = {0, 0, (double)lroundf((float)P.w * P.sx), (double)lroundf((float)P.h * P.sy), 0, 1};
    [g_re setViewport:vp];
    [g_re setScissorRect:scissor_rect()];
    [g_re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    g_pk_dirty = 1;
    g_cur_pipe = nil;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Draw state
 * --------------------------------------------------------------------------------------------------------------- */
static MTLCompareFunction cmpfn(unsigned f) {
    static const MTLCompareFunction t[] = {MTLCompareFunctionAlways,   MTLCompareFunctionNever,        MTLCompareFunctionLess,
                                           MTLCompareFunctionEqual,    MTLCompareFunctionLessEqual,    MTLCompareFunctionGreater,
                                           MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return t[f > 8 ? 0 : f];
}
static MTLStencilOperation stencilop(unsigned o) {
    static const MTLStencilOperation t[] = {MTLStencilOperationKeep,    MTLStencilOperationKeep,           MTLStencilOperationZero,
                                            MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp,
                                            MTLStencilOperationInvert,  MTLStencilOperationIncrementWrap,  MTLStencilOperationDecrementWrap};
    return t[o > 8 ? 0 : o];
}

static id<MTLDepthStencilState> depth_stencil_state(void) {
    int has_depth = P.depth_fmt != 0, has_stencil = P.stencil_fmt != 0;
    uint64_t k = fnv1a(&S.ds, sizeof S.ds, FNV_INIT);
    uint32_t m[4] = {S.stencil_rmask[0], S.stencil_wmask[0], S.stencil_rmask[1], S.stencil_wmask[1]};
    k = fnv1a(m, sizeof m, k) ^ ((uint64_t)has_depth << 1 | (uint64_t)has_stencil);
    NSNumber *key = @(k);
    if (!g_dss) g_dss = [NSMutableDictionary dictionary];
    id<MTLDepthStencilState> s = g_dss[key];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth) {
        d.depthCompareFunction = S.ds.dt ? cmpfn((unsigned)S.ds.df) : MTLCompareFunctionAlways;
        {
            static int eq = -1;
            if (eq < 0) eq = getenv("NAPPLE_DEBUG_EQUAL_LEQUAL") != NULL; /* bisect: depth EQUAL (pre-pass + colour) -> LEQUAL */
            if (eq && d.depthCompareFunction == MTLCompareFunctionEqual) d.depthCompareFunction = MTLCompareFunctionLessEqual;
        }
        d.depthWriteEnabled = S.ds.dw ? YES : NO;
    }
    if (has_stencil && S.ds.st) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = cmpfn((unsigned)S.ds.sf_f);
        f.stencilFailureOperation = stencilop((unsigned)S.ds.fail_f);
        f.depthFailureOperation = stencilop((unsigned)S.ds.zfail_f);
        f.depthStencilPassOperation = stencilop((unsigned)S.ds.zpass_f);
        f.readMask = S.stencil_rmask[0] & 0xff;
        f.writeMask = S.stencil_wmask[0] & 0xff;
        b.stencilCompareFunction = cmpfn((unsigned)S.ds.sf_b);
        b.stencilFailureOperation = stencilop((unsigned)S.ds.fail_b);
        b.depthFailureOperation = stencilop((unsigned)S.ds.zfail_b);
        b.depthStencilPassOperation = stencilop((unsigned)S.ds.zpass_b);
        b.readMask = S.stencil_rmask[1] & 0xff;
        b.writeMask = S.stencil_wmask[1] & 0xff;
        d.frontFaceStencil = f;
        d.backFaceStencil = b;
    }
    s = [g_mtl.dev newDepthStencilStateWithDescriptor:d];
    g_dss[key] = s;
    return s;
}

static MTLPrimitiveType prim_native(uint32_t m, int *native) {
    *native = 1;
    switch (m) {
        case 0: return MTLPrimitiveTypePoint;
        case 1: return MTLPrimitiveTypeLine;
        case 3: return MTLPrimitiveTypeLineStrip;
        case 4: return MTLPrimitiveTypeTriangle;
        case 5: return MTLPrimitiveTypeTriangleStrip;
        default: *native = 0; return MTLPrimitiveTypeTriangle;
    }
}
static MTLPrimitiveTopologyClass topo_class(uint32_t m) {
    switch (m) {
        case 0: return MTLPrimitiveTopologyClassPoint;
        case 1:
        case 2:
        case 3: return MTLPrimitiveTopologyClassLine;
        default: return MTLPrimitiveTopologyClassTriangle;
    }
}

/* Binding slots (of one stage) whose bound texture is a depth format: those textures must be declared depth2d in the shader. */
/* Image bindings of a stage whose texture is a signed / unsigned integer format (ShaderKey.image_int_mask / image_uint_mask, bit n = image binding n). */
static void image_masks(int stage, uint32_t *imask, uint32_t *umask) {
    *imask = *umask = 0;
    gdpool *tp = (gdpool *)htab_get(S.texpool);
    if (!tp) return;
    for (int slot = 0; slot < 16; slot++) {
        uint64_t h = S.img[stage][slot];
        uint32_t tic = (uint32_t)(h & 0xfffff);
        if (!h || tic >= (uint32_t)tp->n || !tp->tex[tic].s.valid) continue;
        MTLPixelFormat pf;
        int dep, st;
        mtl_tex_format_info(&tp->tex[tic].s, &pf, &dep, &st);
        if (pf_is_sint(pf)) *imask |= 1u << slot;
        else if (pf_is_int((uint32_t)pf)) *umask |= 1u << slot;
    }
}
/* Binding slots of a stage whose texture samples as depth (depth bit) or as a stencil view (stencil bit; the view's depth-stencil mode overrides the
 * texture's, as in mtl_tex_view). A depth-stencil texture bound in stencil mode is a stencil slot, not a depth one: the shader reads a uint. */
static void ds_slot_masks(int stage, uint32_t *depth, uint32_t *stencil) {
    *depth = *stencil = 0;
    gdpool *tp = (gdpool *)htab_get(S.texpool);
    if (!tp) return;
    for (int slot = 0; slot < 32; slot++) {
        uint64_t h = S.tex[stage][slot];
        uint32_t tic = (uint32_t)(h & 0xfffff);
        if (!h || tic >= (uint32_t)tp->n || !tp->tex[tic].s.valid) continue;
        const dtex *e = &tp->tex[tic];
        MTLPixelFormat pf;
        int dep, st;
        mtl_tex_format_info(&e->s, &pf, &dep, &st);
        int stencil_view = st && (e->view.set_ds ? e->view.ds_mode : e->s.ds_mode) == 1;
        if (pf == MTLPixelFormatStencil8 || stencil_view) *stencil |= 1u << slot;
        else if (dep) *depth |= 1u << slot;
    }
}

static void fill_pipe_key(uint32_t prim, pipe_key *k) {
    memset(k, 0, sizeof *k);
    ds_slot_masks(ST_VS, &k->depth_slots[0], &k->stencil_slots[0]);
    ds_slot_masks(ST_FS, &k->depth_slots[1], &k->stencil_slots[1]);
    image_masks(ST_VS, &k->img_int[0], &k->img_uint[0]);
    image_masks(ST_FS, &k->img_int[1], &k->img_uint[1]);
    k->vs = S.prog[ST_VS];
    k->fs = S.prog[ST_FS];
    memcpy(k->color_fmt, P.color_fmt, sizeof k->color_fmt);
    k->depth_fmt = P.depth_fmt;
    k->stencil_fmt = P.stencil_fmt;
    k->samples = P.samples;
    k->sample_shading = S.sample_shading && P.samples > 1; /* the fragment function then declares [[sample_id]] and runs per sample */
    k->topo = (uint32_t)topo_class(prim);
    for (int i = 0; i < 8; i++) {
        k->blend[i] = mtl_blend_resolve(S.blend[i]);
        k->blend_en[i] = (S.color.blend_en >> i) & 1;
        k->wmask[i] = (S.chmask.mask >> (4 * i)) & 0xf;
        GfxTex *t = nil;
        (void)t;
        k->rt_kind[i] = 0;
    }
    for (uint32_t i = 0; i < S.rt.n && i < 8; i++) {
        uint32_t pf = P.color_fmt[i];
        /* integer render targets: the shader must write int / uint */
        if (pf == MTLPixelFormatR8Sint || pf == MTLPixelFormatRG8Sint || pf == MTLPixelFormatRGBA8Sint || pf == MTLPixelFormatR16Sint ||
            pf == MTLPixelFormatRG16Sint || pf == MTLPixelFormatRGBA16Sint || pf == MTLPixelFormatR32Sint || pf == MTLPixelFormatRG32Sint ||
            pf == MTLPixelFormatRGBA32Sint)
            k->rt_kind[i] = 1;
        else if (pf == MTLPixelFormatR8Uint || pf == MTLPixelFormatRG8Uint || pf == MTLPixelFormatRGBA8Uint || pf == MTLPixelFormatR16Uint ||
                 pf == MTLPixelFormatRG16Uint || pf == MTLPixelFormatRGBA16Uint || pf == MTLPixelFormatR32Uint || pf == MTLPixelFormatRG32Uint ||
                 pf == MTLPixelFormatRGBA32Uint || pf == MTLPixelFormatRGB10A2Uint)
            k->rt_kind[i] = 2;
    }
    k->a2c = S.ms.a2c;
    {
        static const char *pr;
        static int n = -1; /* NAPPLE_SHADER_PROBE: see mtl_shader.m */
        if (n < 0) {
            pr = getenv("NAPPLE_SHADER_PROBE");
            n = 0;
            for (const char *e = pr; e && *e; e++) n += *e == ';';
            n = pr ? n + 1 : 0;
        }
        if (n) {
            static uint64_t per;
            if (!per) {
                const char *e = getenv("NAPPLE_SHADER_PROBE_PERIOD");
                per = e ? strtoull(e, NULL, 10) : 30;
                if (!per) per = 30;
            }
            k->probe = (uint8_t)(1 + (rel_frame() / per) % (uint64_t)n);
        }
    }
    k->nattr = (uint8_t)S.nattr;
    for (uint32_t i = 0; i < S.nattr; i++) {
        k->attr[i] = S.vattr[i];
        int kind;
        (void)mtl_vertex_format((int)S.vattr[i].fmt, &kind);
        k->attr_kind[i] = (uint8_t)kind;
    }
    k->nstream = (uint8_t)(S.nstream > MAX_STREAMS ? MAX_STREAMS : S.nstream);
    for (uint32_t i = 0; i < k->nstream; i++) {
        k->stream[i] = S.vstream[i];
        if (k->stream[i].stride & 3)
            k->stream[i].stride =
                (k->stream[i].stride + 3) & ~3u; /* Metal vertex strides are multiples of 4: such streams are repacked per draw (bind_vertex_streams) */
    }
    k->flip_y =
        g_gfx.origin_flip &&
        g_gfx.device_origin ==
            0; /* the window origin mode only changes the window-coordinate convention (viewport, scissor, fragment coordinate), see apply_raster_state */
    k->depth_mode = (uint8_t)g_gfx.device_depth_mode;
}

static id<MTLTexture> dummy_texture(int type) {
    if (g_dummy_tex[type & 7]) return g_dummy_tex[type & 7];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    static const MTLTextureType tt[] = {MTLTextureType1D,      MTLTextureType2D,        MTLTextureType3D,      MTLTextureTypeCube,
                                        MTLTextureType2DArray, MTLTextureTypeCubeArray, MTLTextureType1DArray, MTLTextureType2D};
    d.textureType = tt[type & 7];
    if (d.textureType == MTLTextureType3D) d.depth = 1;
    if (d.textureType == MTLTextureTypeCube || d.textureType == MTLTextureTypeCubeArray) { d.width = d.height = 1; }
    if (d.textureType == MTLTextureType1D || d.textureType == MTLTextureType1DArray) d.height = 1;
    if (d.textureType == MTLTextureType2DArray || d.textureType == MTLTextureType1DArray) d.arrayLength = 1;
    if (d.textureType == MTLTextureTypeCubeArray) d.arrayLength = 1;
    d.usage = MTLTextureUsageShaderRead;
    g_dummy_tex[type & 7] = [g_mtl.dev newTextureWithDescriptor:d];
    return g_dummy_tex[type & 7];
}

/* A 1x1 read-write image of the declared type and component kind, bound where the game's image handle resolves to nothing. */
static id<MTLTexture> dummy_image(int type, int kind) {
    static id<MTLTexture> cache[8][3];
    int ti = type & 7, ki = kind > 2 ? 0 : kind;
    if (cache[ti][ki]) return cache[ti][ki];
    MTLPixelFormat pf = kind == IMGKIND_INT ? MTLPixelFormatR32Sint : kind == IMGKIND_UINT ? MTLPixelFormatR32Uint : MTLPixelFormatRGBA8Unorm;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:1 height:1 mipmapped:NO];
    static const MTLTextureType tt[] = {MTLTextureType1D,      MTLTextureType2D,        MTLTextureType3D,      MTLTextureTypeCube,
                                        MTLTextureType2DArray, MTLTextureTypeCubeArray, MTLTextureType1DArray, MTLTextureType1D};
    d.textureType = tt[ti];
    if (d.textureType == MTLTextureType3D) d.depth = 1;
    if (d.textureType == MTLTextureType1D || d.textureType == MTLTextureType1DArray) d.height = 1;
    if (d.textureType == MTLTextureType2DArray || d.textureType == MTLTextureType1DArray || d.textureType == MTLTextureTypeCubeArray) d.arrayLength = 1;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (pf != MTLPixelFormatRGBA8Unorm) {
        if (@available(macOS 14.0, iOS 17.0, *)) d.usage |= MTLTextureUsageShaderAtomic;
    }
    d.storageMode = MTLStorageModePrivate;
    cache[ti][ki] = [g_mtl.dev newTextureWithDescriptor:d];
    return cache[ti][ki];
}

static void apply_raster_state(void) {
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("NAPPLE_DEBUG_NOCULL") != NULL;
    if (nocull || S.poly.cull == NVN_FACE_FRONT_AND_BACK) {
        [g_re setCullMode:MTLCullModeNone];
    } else [g_re setCullMode:S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone];
    /* NVN renders NDC +y to the top row of the target in both window origin modes and the display scans rows top-down; "lower-left" only means window
     * coordinates (viewport, scissor, fragment coordinate) count from the bottom. Metal has the same NDC mapping, so no vertex or winding change. */
    {
        int ccw = S.poly.front == NVN_FRONT_FACE_CCW;
        if (g_gfx.origin_flip && g_gfx.device_origin == 0) ccw = !ccw; /* y is negated in the vertex stage */
        [g_re setFrontFacingWinding:ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    }
    int lower_left = g_gfx.device_origin == 0;
    [g_re setTriangleFillMode:S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    [g_re setDepthClipMode:S.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    if (S.poly.offset_en) [g_re setDepthBias:S.poly_units slopeScale:S.poly_factor clamp:S.poly_clamp];
    else [g_re setDepthBias:0 slopeScale:0 clamp:0];
    [g_re setBlendColorRed:S.blend_color[0] green:S.blend_color[1] blue:S.blend_color[2] alpha:S.blend_color[3]];
    [g_re setStencilFrontReferenceValue:S.stencil_ref[0] & 0xff backReferenceValue:S.stencil_ref[1] & 0xff];
    MTLViewport vp = {S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0][0], S.dr[0][1]};
    if (vp.height < 0 || vp.width < 0) LOG_ONCE(NAPPLE_LOG_WARN, "negative viewport extent %g x %g (origin mode %u)", vp.width, vp.height, g_gfx.device_origin);
    if (vp.width <= 0 || vp.height <= 0) {
        vp.originX = 0;
        vp.originY = 0;
        vp.width = P.w;
        vp.height = P.h;
    } else if (lower_left && !g_gfx.origin_flip) vp.originY = (double)P.h - vp.originY - vp.height;
    vp.originX *= P.sx;
    vp.originY *= P.sy;
    vp.width *= P.sx;
    vp.height *= P.sy; /* guest pixels -> scaled render target */
    [g_re setViewport:vp];
    {
        MTLScissorRect sr = scissor_rect();
        static int wl = -1;
        if (wl < 0) wl = getenv("NAPPLE_WINDRAWLOG") != NULL;
        if (wl && P.w == 1920 && P.color_fmt[0] == 71)
            fprintf(stderr, "[windraw] fmt %d vp %g %g %g %g (guest %g %g %g %g) sc_set %d guest sc %d %d %d %d -> %lu %lu %lu %lu P %ux%u scale %g %g\n",
                    (int)P.color_fmt[0], vp.originX, vp.originY, vp.width, vp.height, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.sc_set, S.sc[0][0],
                    S.sc[0][1], S.sc[0][2], S.sc[0][3], (unsigned long)sr.x, (unsigned long)sr.y, (unsigned long)sr.width, (unsigned long)sr.height, P.w, P.h,
                    P.sx, P.sy);
        [g_re setScissorRect:sr];
    }
}

/* GPU virtual address of a guest address range inside a registered pool (0 when it is not backed by one: shader accesses then read zero / are dropped). */
static uint64_t gpu_va(uint64_t addr, uint64_t size) {
    if (!addr) return 0;
    id<MTLBuffer> b;
    uint64_t o;
    if (!mtl_addr(addr, size ? size : 1, &b, &o)) {
        /* The driver-constant table carries all 16 slots of a stage, used or not, and NVN lets a binding dangle once its pool is finalized (SMO leaves one behind
         * between scenes): an unbacked slot reads as zero, which is what the hardware gives a shader that never touches it. Not a warning. */
        LOG_ONCE(NAPPLE_LOG_INFO, "shader storage buffer %#llx+%#llx is not inside a memory pool (stale binding, reads as zero)", (unsigned long long)addr, (unsigned long long)size);
        return 0;
    }
    return b.gpuAddress + o;
}
/* Buffers reached through raw GPU addresses are not tracked by Metal: declare every pool buffer resident once per encoder. */
static void make_pools_resident(id enc, int compute) {
    static uint64_t last_serial, last_epoch;
    uint64_t epoch = mtl_pool_epoch();
    if (g_enc_serial == last_serial && epoch == last_epoch) return;
    last_serial = g_enc_serial;
    last_epoch = epoch;
    mtl_pool_each(^(id<MTLBuffer> b) {
        if (compute) [(id<MTLComputeCommandEncoder>)enc useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        else
            [(id<MTLRenderCommandEncoder>)enc useResource:b
                                                    usage:MTLResourceUsageRead | MTLResourceUsageWrite
                                                   stages:MTLRenderStageVertex | MTLRenderStageFragment];
    });
}

/* Bind constant buffers, textures and samplers of one stage. `enc` is the render or compute encoder. */
static void bind_stage(int stage, GfxShader *sh, id enc, int compute, const uint32_t cs_groups[3]) {
    const ShaderInfo *si = &sh->info;
    /* geometry and tess control kernels run on a compute encoder (compute = 1) but keep the graphics driver-constant layout of their stage */
    int cs_layout = compute && stage == ST_CS;
    uint32_t cbsz[18];
    for (int i = 0; i < 18; i++) cbsz[i] = 0x10000; /* bound size per constant buffer (the shader reads 0 past it: CBSZ_BUFFER_INDEX) */
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        id<MTLBuffer> buf = nil;
        uint32_t off = 0;
        if (k == 0) { /* driver constants */
            uint32_t sz = si->cbuf_size[0] ? si->cbuf_size[0] : 256;
            if (sz < 0x800) sz = 0x800;
            uint8_t *p = ring_alloc(&g_ring, sz, 256, &buf, &off);
            memset(p, 0, sz);
            /* storage buffer table of the stage: 0x110 + 0x100 * hardware stage (VS 0, TCS 1, TES 2, GS 3, FS 4); the VS and FS offsets are the measured ones,
             * the three between are inferred from that spacing */
            static const uint32_t hw_stage[6] = {0, 4, 3, 1, 2, 0};
            uint32_t base = 0x110 + 0x100 * hw_stage[stage < 6 ? stage : 0];
            for (int i = 0; i < 16 && !cs_layout; i++) {
                uint32_t *e = (uint32_t *)(p + base + 16 * i);
                uint64_t va = gpu_va(S.ssbo[stage][i].addr, S.ssbo[stage][i].size);
                e[0] = (uint32_t)va;
                e[1] = (uint32_t)(va >> 32);
                e[2] = (uint32_t)S.ssbo[stage][i].size;
            }
            /* c0 + 0x610: the largest point size (Maxwell's limit, 2047); GLSLC clamps a shader-written gl_PointSize to it (SMO's point sprites, nxnative G6) */
            if (!cs_layout) {
                const float max_point = 2047.0f;
                memcpy(p + 0x610, &max_point, 4);
            }
            if (cs_layout) {
                uint32_t *g = (uint32_t *)(p + 0x200);
                g[0] = cs_groups[0];
                g[1] = cs_groups[1];
                g[2] = cs_groups[2];
                for (int i = 0; i < 14; i++) { /* uniform buffers read through global memory (compute shaders): GPU virtual addresses, like the SSBOs */
                    uint32_t *e = (uint32_t *)(p + 0x210 + 16 * i);
                    uint64_t va = gpu_va(S.ubo[stage][i].addr, S.ubo[stage][i].size);
                    e[0] = (uint32_t)va;
                    e[1] = (uint32_t)(va >> 32);
                    e[2] = (uint32_t)S.ubo[stage][i].size;
                }
                for (int i = 0; i < 16; i++) {
                    uint32_t *e = (uint32_t *)(p + 0x310 + 16 * i);
                    uint64_t va = gpu_va(S.ssbo[stage][i].addr, S.ssbo[stage][i].size);
                    e[0] = (uint32_t)va;
                    e[1] = (uint32_t)(va >> 32);
                    e[2] = (uint32_t)S.ssbo[stage][i].size;
                }
            }
        } else if (k == 1) { /* compiler constants */
            if (!sh->c1) {
                const shader_blob *b = sh->blob;
                sh->c1 = [g_mtl.dev newBufferWithBytes:b->consts length:b->const_size > 16 ? b->const_size : 16 options:MTLResourceStorageModeShared];
            }
            buf = sh->c1;
            off = 0;
        } else if (k == 2) {
            continue;
        } else {
            int idx = k - CBUF_USER_BASE;
            uint32_t sz = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
            bufrange r = (idx >= 0 && idx < 14) ? S.ubo[stage][idx] : (bufrange){0, 0};
            if ((si->cbuf_dynamic & (1u << k)) && r.size) sz = r.size > 0x10000 ? 0x10000 : (uint32_t)r.size;
            if (r.size && sz > r.size) sz = (uint32_t)r.size;
            if (sz < 16) sz = 16;
            uint8_t *p = ring_alloc(&g_ring, sz, 256, &buf, &off);
            if (r.addr) memcpy(p, (const void *)(uintptr_t)r.addr, sz);
            else memset(p, 0, sz);
            cbsz[k] = r.addr ? (r.size && r.size < sz ? (uint32_t)r.size : sz) : 0;
        }
        if (compute) [(id<MTLComputeCommandEncoder>)enc setBuffer:buf offset:off atIndex:k];
        else if (stage != ST_FS) [(id<MTLRenderCommandEncoder>)enc setVertexBuffer:buf offset:off atIndex:k];
        else [(id<MTLRenderCommandEncoder>)enc setFragmentBuffer:buf offset:off atIndex:k];
    }
    if (si->cbuf_dynamic) {
        if (compute) [(id<MTLComputeCommandEncoder>)enc setBytes:cbsz length:sizeof cbsz atIndex:CBSZ_BUFFER_INDEX];
        else if (stage != ST_FS) [(id<MTLRenderCommandEncoder>)enc setVertexBytes:cbsz length:sizeof cbsz atIndex:CBSZ_BUFFER_INDEX];
        else [(id<MTLRenderCommandEncoder>)enc setFragmentBytes:cbsz length:sizeof cbsz atIndex:CBSZ_BUFFER_INDEX];
    }
    float *rscale_ptr = NULL; /* the rscale table of this stage (filled per texture below) */
    if (si->uses_gmem) make_pools_resident(enc, compute);
    if (si->uses_rscale) { /* resolution scale table: identity */
        id<MTLBuffer> buf;
        uint32_t off;
        float *p = ring_alloc(&g_ring, 66 * 16, 256, &buf, &off);
        for (int i = 0; i < 66; i++) {
            p[i * 4] = 1;
            p[i * 4 + 1] = 1;
            p[i * 4 + 2] = 0;
            p[i * 4 + 3] = 0;
        }
        rscale_ptr = p;
        if (!compute) {
            p[0] = P.sx;
            p[1] = P.sy;
        } /* [0] the render target of the pass: fragment position / scale = guest pixels */
        if (g_gfx.device_origin == 0 && !g_gfx.origin_flip) {
            p[1] = -(compute ? 1.0f : P.sy);
            p[3] = (float)P.h * (compute ? 1.0f : P.sy);
        } /* fragment coordinate y of the lower-left origin: H - y_top */
        if (compute) [(id<MTLComputeCommandEncoder>)enc setBuffer:buf offset:off atIndex:18];
        else if (stage != ST_FS) [(id<MTLRenderCommandEncoder>)enc setVertexBuffer:buf offset:off atIndex:18];
        else [(id<MTLRenderCommandEncoder>)enc setFragmentBuffer:buf offset:off atIndex:18];
    }
    /* textures and samplers */
    gdpool *tp = (gdpool *)htab_get(S.texpool), *sp = (gdpool *)htab_get(S.samppool);
    for (int i = 0; i < si->ntex; i++) {
        const ShaderTexture *st = &si->tex[i];
        int slot = ((int)st->handle_offset - TEX_COMBINED_BASE) / 8;
        uint64_t h = (slot >= 0 && slot < 128) ? S.tex[stage][slot] : 0;
        if (st->bindless) { /* handle stored by the game in a uniform buffer: read the bound buffer at the static offset */
            int ui = (int)st->bindless_cbuf - CBUF_USER_BASE;
            h = 0;
            slot = -1;
            if (ui >= 0 && ui < 14 && S.ubo[stage][ui].addr && (uint64_t)st->bindless_offset + 4 <= S.ubo[stage][ui].size)
                h = *(const uint32_t *)(uintptr_t)(S.ubo[stage][ui].addr + st->bindless_offset);
        }
        uint32_t tic = (uint32_t)(h & 0xfffff), tsc = (uint32_t)(h >> 20) & 0xfff;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        if (g_drawlog_verbose && !(h && tp && tic < (uint32_t)tp->n && tp->tex[tic].s.valid))
            fprintf(stderr, "    tex [%d] MISSING: handle offset %#x slot %d bindless %d handle %#llx tic %u pool %d entries valid %d\n", i, st->handle_offset,
                    slot, st->bindless, (unsigned long long)h, tic, tp ? tp->n : -1, h && tp && tic < (uint32_t)tp->n ? tp->tex[tic].s.valid : -1);
        if (h && tp && tic < (uint32_t)tp->n && tp->tex[tic].s.valid) {
            dtex *e = &tp->tex[tic];
            GfxTex *gt = mtl_tex_get(&e->s, 0);
            if (gt && g_drawlog_verbose)
                fprintf(
                    stderr,
                    "    tex [%d] %ux%u fmt %#x mem %#llx gpu_owned %d | guest target %u depth %u | metal type %lu %lux%lux%lu layers %lu | shader type %d\n",
                    i, e->s.d.width, e->s.d.height, e->s.d.format, (unsigned long long)(uintptr_t)gt->key.mem, gt->gpu_owned, (unsigned)e->s.d.target,
                    (unsigned)e->s.d.depth, (unsigned long)gt->tex.textureType, (unsigned long)gt->tex.width, (unsigned long)gt->tex.height,
                    (unsigned long)gt->tex.depth, (unsigned long)gt->tex.arrayLength, st->type);
            if (gt && g_drawlog_verbose && gt->tex.textureType == MTLTextureType3D && gt->tex.width <= 16) g_dbg_3d = gt;
            if (gt && g_drawlog_on && getenv("NAPPLE_DRAWLOG_TEX")) {
                static uintptr_t seen[256];
                static int ns;
                int dup = 0;
                for (int q = 0; q < ns; q++) dup |= seen[q] == (uintptr_t)gt->key.mem;
                if (!dup && ns < 256) {
                    seen[ns++] = (uintptr_t)gt->key.mem;
                    mtl_tex_prepare_sample(gt);
                    mtl_tex_debug_dump(gt, getenv("NAPPLE_DRAWLOG_TEX"), 0);
                }
            }
            if (gt) {
                mtl_tex_prepare_sample(gt);
                tex = mtl_tex_view(gt, e->s.swz, &e->view, 1, st->type, st->shadow, e->s.ds_mode);
                if (rscale_ptr && i < 64) {
                    rscale_ptr[(i + 1) * 4] = gt->rsx > 0 ? gt->rsx : 1;
                    rscale_ptr[(i + 1) * 4 + 1] = gt->rsy > 0 ? gt->rsy : 1;
                }
            }
            if (gt && g_drawlog_verbose &&
                e->s.d.width * e->s.d.height <= 16) { /* tiny data textures (animation / wind parameters): print guest bytes and what the GPU texture holds */
                const uint32_t *raw = (const uint32_t *)(uintptr_t)gt->key.mem;
                uint8_t back[128] = {0};
                if (!gt->gpu_owned && gt->tex && gt->tex.width * gt->tex.height <= 16 && gt->pf == MTLPixelFormatRG11B10Float)
                    [gt->tex getBytes:back bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, gt->tex.width, gt->tex.height) mipmapLevel:0];
                fprintf(stderr, "    tiny tex %ux%u fmt %#x gpu_owned %d guest %08x %08x %08x %08x | metal %08x %08x\n", e->s.d.width, e->s.d.height,
                        e->s.d.format, gt->gpu_owned, raw[0], raw[1], raw[2], raw[3], *(uint32_t *)back, *(uint32_t *)(back + 4));
            }
        }
        if (h && sp && tsc < (uint32_t)sp->n && sp->samp_used[tsc]) smp = mtl_sampler_get(&sp->samp[tsc]);
        if (tex) { /* Metal's API validation aborts when the bound texture type is not the one the shader declares (the game bound a 2D texture where the shader samples a cube) */
            static const MTLTextureType tt[] = {MTLTextureType1D,      MTLTextureType2D,        MTLTextureType3D,      MTLTextureTypeCube,
                                                MTLTextureType2DArray, MTLTextureTypeCubeArray, MTLTextureType1DArray, MTLTextureType2D};
            MTLTextureType want = tt[st->type & 7], have = tex.textureType;
            int ms = have == MTLTextureType2DMultisample || have == MTLTextureType2DMultisampleArray;
            if (have != want && !ms) {
                LOG_ONCE(NAPPLE_LOG_WARN, "texture slot %d: the shader samples texture type %lu but type %lu is bound: using a dummy texture", i,
                         (unsigned long)want, (unsigned long)have);
                tex = nil;
            }
        }
        if (!tex) tex = dummy_texture(st->type);
        if (!smp) {
            static samp_par def;
            static int init;
            if (!init) {
                def.minf = 1;
                def.magf = 1;
                def.lod_max = 1000;
                def.aniso = 1;
                init = 1;
            }
            smp = mtl_sampler_get(&def);
        }
        if (g_drawlog_verbose && !compute) {
            dtex *e = (h && tp && tic < (uint32_t)tp->n) ? &tp->tex[tic] : NULL;
            fprintf(stderr, "    tex[%d] stage %d off %#x slot %d handle %#llx tic %u tsc %u fmt %#x %ux%u lv %u tgt %d swz %u%u%u%u %s\n", i, stage,
                    st->handle_offset, slot, (unsigned long long)h, tic, tsc, e && e->s.valid ? e->s.d.format : 0, e ? e->s.d.width : 0, e ? e->s.d.height : 0,
                    e ? e->s.d.levels : 0, e ? e->s.d.target : -1, e ? e->s.swz[0] : 0, e ? e->s.swz[1] : 0, e ? e->s.swz[2] : 0, e ? e->s.swz[3] : 0, "");
            if (e)
                fprintf(stderr,
                        "      view: set_swizzle %d swz %u%u%u%u set_levels %d set_format %d set_target %d target %u set_layers %d layers %u+%u dsmode %u\n",
                        e->view.set_swizzle, e->view.swz[0], e->view.swz[1], e->view.swz[2], e->view.swz[3], e->view.set_levels, e->view.set_format,
                        e->view.set_target, e->view.target, e->view.set_layers, e->view.min_layer, e->view.nlayers, e->s.ds_mode);
        }
        if (compute) {
            [(id<MTLComputeCommandEncoder>)enc setTexture:tex atIndex:i];
            if (i < 16) [(id<MTLComputeCommandEncoder>)enc setSamplerState:smp atIndex:i];
        } else if (stage != ST_FS) {
            [(id<MTLRenderCommandEncoder>)enc setVertexTexture:tex atIndex:i];
            if (i < 16) [(id<MTLRenderCommandEncoder>)enc setVertexSamplerState:smp atIndex:i];
        } else {
            [(id<MTLRenderCommandEncoder>)enc setFragmentTexture:tex atIndex:i];
            if (i < 16) [(id<MTLRenderCommandEncoder>)enc setFragmentSamplerState:smp atIndex:i];
        }
    }
    /* surface images (SULD / SUST / SUATOM / SURED): image slot i of the program at texture index IMAGE_TEXTURE_BASE + i, the texture from the image handle the game
     * bound (BindImage binding = c2 handle index) or a bindless handle in a uniform buffer; a written image makes the GPU copy the owner of the texture */
    for (int i = 0; i < si->nimg && i < MAX_IMAGES; i++) {
        const ShaderImage *im = &si->img[i];
        int slot = (int)(im->handle_offset / 4);
        uint64_t h = (slot >= 0 && slot < 16) ? S.img[stage][slot] : 0;
        if (im->bindless) {
            int ui = (int)im->bindless_cbuf - CBUF_USER_BASE;
            h = 0;
            if (ui >= 0 && ui < 14 && S.ubo[stage][ui].addr && (uint64_t)im->bindless_offset + 4 <= S.ubo[stage][ui].size)
                h = *(const uint32_t *)(uintptr_t)(S.ubo[stage][ui].addr + im->bindless_offset);
        }
        uint32_t tic = (uint32_t)(h & 0xfffff);
        id<MTLTexture> tex = nil;
        if (h && tp && tic < (uint32_t)tp->n && tp->tex[tic].s.valid) {
            GfxTex *gt = mtl_tex_get(&tp->tex[tic].s, (im->access & IMGACC_WRITE) ? 1 : 0);
            if (gt) {
                mtl_tex_prepare_sample(gt);
                tex = mtl_tex_image_view(gt, im->kind, im->type, im->atomic);
                if (tex && (im->access & IMGACC_WRITE)) gt->gpu_owned = 1; /* the shader writes it: the Metal texture is the data from now on */
            }
        }
        if (!tex) {
            LOG_ONCE(NAPPLE_LOG_WARN, "surface image %d (handle %#llx, c2 offset %#x, kind %u type %u) has no usable texture: a dummy is bound", i,
                     (unsigned long long)h, im->handle_offset, im->kind, im->type);
            tex = dummy_image(im->type, im->kind);
        }
        if (compute) [(id<MTLComputeCommandEncoder>)enc setTexture:tex atIndex:IMAGE_TEXTURE_BASE + i];
        else if (stage != ST_FS) [(id<MTLRenderCommandEncoder>)enc setVertexTexture:tex atIndex:IMAGE_TEXTURE_BASE + i];
        else [(id<MTLRenderCommandEncoder>)enc setFragmentTexture:tex atIndex:IMAGE_TEXTURE_BASE + i];
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * DrawTexture: a screen-aligned textured rectangle drawn in the current pass (GL NV_draw_texture). The texel goes to every float colour target
 * through the bound blend state and channel masks; integer targets are left alone, depth and stencil are not tested or written.
 * --------------------------------------------------------------------------------------------------------------- */
static int pf_is_int(uint32_t pf) {
    switch (pf) {
        case MTLPixelFormatR8Sint:
        case MTLPixelFormatRG8Sint:
        case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatR16Sint:
        case MTLPixelFormatRG16Sint:
        case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA32Sint:
        case MTLPixelFormatR8Uint:
        case MTLPixelFormatRG8Uint:
        case MTLPixelFormatRGBA8Uint:
        case MTLPixelFormatR16Uint:
        case MTLPixelFormatRG16Uint:
        case MTLPixelFormatRGBA16Uint:
        case MTLPixelFormatR32Uint:
        case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA32Uint:
        case MTLPixelFormatRGB10A2Uint: return 1;
        default: return 0;
    }
}
static id<MTLRenderPipelineState> draw_tex_pipe(void) {
    static NSMutableDictionary<NSNumber *, id> *libs, *pipes;
    if (!pipes) {
        libs = [NSMutableDictionary dictionary];
        pipes = [NSMutableDictionary dictionary];
    }
    uint32_t fmask = 0;
    for (int i = 0; i < 8; i++)
        if (P.color_fmt[i] && !pf_is_int(P.color_fmt[i])) fmask |= 1u << i;
    struct {
        uint32_t color_fmt[8], depth_fmt, stencil_fmt, samples, fmask;
        blend_st blend[8];
        uint32_t blend_en, wmask;
    } key;
    memset(&key, 0, sizeof key);
    memcpy(key.color_fmt, P.color_fmt, sizeof key.color_fmt);
    key.depth_fmt = P.depth_fmt;
    key.stencil_fmt = P.stencil_fmt;
    key.samples = P.samples;
    key.fmask = fmask;
    for (int i = 0; i < 8; i++)
        if (fmask & (1u << i)) {
            key.blend[i] = mtl_blend_resolve(S.blend[i]);
            key.blend[i].target = 0;
        }
    key.blend_en = S.color.blend_en & fmask;
    key.wmask = S.chmask.mask;
    NSNumber *k = @(fnv1a(&key, sizeof key, FNV_INIT));
    id p = pipes[k];
    if (p) return p == (id)NSNull.null ? nil : p;
    id<MTLLibrary> lib = libs[@(fmask)];
    NSError *err = nil;
    if (!lib) {
        NSMutableString *src = [NSMutableString
            stringWithString:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "struct V { float4 pos [[position]]; float2 uv; };\n"
                 "vertex V dtv(uint vid [[vertex_id]], constant float4 *r [[buffer(0)]]) {\n" /* r[0] = NDC x0 y0 x1 y1, r[1] = s0 t0 s1 t1 */
                 "  float2 f = float2(vid & 1, vid >> 1); V o; o.pos = float4(mix(r[0].xy, r[0].zw, f), 0, 1); o.uv = mix(r[1].xy, r[1].zw, f); return o; }\n"
                 "struct O {"];
        for (int i = 0; i < 8; i++)
            if (fmask & (1u << i)) [src appendFormat:@" float4 c%d [[color(%d)]];", i, i];
        [src appendString:@" };\nfragment O dtf(V in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = tex.sample(s, "
                          @"in.uv); O o;"];
        for (int i = 0; i < 8; i++)
            if (fmask & (1u << i)) [src appendFormat:@" o.c%d = c;", i];
        [src appendString:@" return o; }\n"];
        if (!fmask)
            src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 pos [[position]]; float2 uv; };\n"
                                                     "vertex V dtv(uint vid [[vertex_id]], constant float4 *r [[buffer(0)]]) { float2 f = float2(vid & 1, vid "
                                                     ">> 1); V o; o.pos = float4(mix(r[0].xy, r[0].zw, f), 0, 1); o.uv = f; return o; }\n"
                                                     "fragment void dtf(V in [[stage_in]]) { }\n"];
        lib = [g_mtl.dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            LOGE("DrawTexture shader: %s", err.localizedDescription.UTF8String);
            pipes[k] = NSNull.null;
            return nil;
        }
        libs[@(fmask)] = lib;
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"dtv"];
    d.fragmentFunction = [lib newFunctionWithName:@"dtf"];
    d.rasterSampleCount = P.samples ? P.samples : 1;
    for (int i = 0; i < 8; i++) {
        if (!P.color_fmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)P.color_fmt[i];
        uint32_t m = (fmask & (1u << i)) ? (S.chmask.mask >> (4 * i)) & 0xf : 0;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) |
                      (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if (key.blend_en & (1u << i)) {
            const blend_st *b = &key.blend[i];
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = mtl_blend_factor((unsigned)b->src_rgb);
            c.destinationRGBBlendFactor = mtl_blend_factor((unsigned)b->dst_rgb);
            c.sourceAlphaBlendFactor = mtl_blend_factor((unsigned)b->src_a);
            c.destinationAlphaBlendFactor = mtl_blend_factor((unsigned)b->dst_a);
            c.rgbBlendOperation = mtl_blend_op((unsigned)b->eq_rgb);
            c.alphaBlendOperation = mtl_blend_op((unsigned)b->eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)P.depth_fmt;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)P.stencil_fmt;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGW("DrawTexture pipeline: %s", err.localizedDescription.UTF8String);
    pipes[k] = p ?: (id)NSNull.null;
    return p;
}
static void do_draw_texture(const p_draw_tex *dt) {
    if (!S.render_enable || S.raster_discard) return;
    if (!pass_begin()) return;
    gdpool *tp = (gdpool *)htab_get(S.texpool), *sp = (gdpool *)htab_get(S.samppool);
    uint32_t tic = (uint32_t)(dt->handle & 0xfffff), tsc = (uint32_t)(dt->handle >> 20) & 0xfff;
    if (!tp || tic >= (uint32_t)tp->n || !tp->tex[tic].s.valid) {
        LOG_ONCE(NAPPLE_LOG_WARN, "DrawTexture with an unregistered texture handle %#llx ignored", (unsigned long long)dt->handle);
        return;
    }
    dtex *e = &tp->tex[tic];
    GfxTex *gt = mtl_tex_get(&e->s, 0);
    if (!gt) return;
    mtl_tex_prepare_sample(gt);
    id<MTLTexture> tex = mtl_tex_view(gt, e->s.swz, &e->view, 1, TEXTYPE_2D, 0, e->s.ds_mode);
    id<MTLSamplerState> smp = (sp && tsc < (uint32_t)sp->n && sp->samp_used[tsc]) ? mtl_sampler_get(&sp->samp[tsc]) : nil;
    if (!smp) {
        static samp_par def;
        static int init;
        if (!init) {
            def.minf = 1;
            def.magf = 1;
            def.lod_max = 1000;
            def.aniso = 1;
            init = 1;
        }
        smp = mtl_sampler_get(&def);
    }
    id<MTLRenderPipelineState> ps = draw_tex_pipe();
    if (!tex || !ps) return;
    static id<MTLDepthStencilState> ds;
    if (!ds) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        ds = [g_mtl.dev newDepthStencilStateWithDescriptor:dd];
    }
    /* window coordinates (y from the bottom in the lower-left origin mode) -> NDC (+y up, the top row is NDC +1) */
    float W = (float)P.w, H = (float)P.h, r[8];
    int lower_left = g_gfx.device_origin == 0 && !g_gfx.origin_flip;
    for (int i = 0; i < 2; i++) {
        float x = dt->dst[i * 2], y = dt->dst[i * 2 + 1];
        r[i * 2] = 2.0f * x / W - 1.0f;
        r[i * 2 + 1] = lower_left ? 2.0f * y / H - 1.0f : 1.0f - 2.0f * y / H;
    }
    memcpy(r + 4, dt->src, sizeof dt->src);
    [g_re setRenderPipelineState:ps];
    [g_re setDepthStencilState:ds];
    [g_re setCullMode:MTLCullModeNone];
    [g_re setTriangleFillMode:MTLTriangleFillModeFill];
    [g_re setVertexBytes:r length:sizeof r atIndex:0];
    [g_re setFragmentTexture:tex atIndex:0];
    [g_re setFragmentSamplerState:smp atIndex:0];
    MTLViewport vp = {0, 0, (double)lroundf(W * P.sx), (double)lroundf(H * P.sy), 0, 1};
    [g_re setViewport:vp];
    [g_re setScissorRect:scissor_rect()];
    [g_re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    P.draws++;
    atomic_fetch_add(&g_gfx.draws, 1);
    g_pk_dirty = 1;
    g_cur_pipe = nil;
}

/* Vertex rows the current draw reads (set by the draw before prepare_draw; used to repack streams whose stride is not a multiple of 4). */
static int g_vr_valid;
static int64_t g_vr_lo, g_vr_hi;
static int streams_misaligned(void) {
    for (uint32_t s = 0; s < S.nstream && s < MAX_STREAMS; s++)
        if (S.vstream[s].stride & 3) return 1;
    return 0;
}

/* Whether any bound vertex attribute reads from stream `st` (a bound stream nothing reads is harmless whatever it points at). */
static int stream_referenced(uint32_t st) {
    for (uint32_t i = 0; i < S.nattr && i < 32; i++)
        if (S.vattr[i].fmt && (uint32_t)(S.vattr[i].stream % MAX_STREAMS) == st) return 1;
    return 0;
}
/* Vertex streams of the current state on g_re (VB_BASE + stream; the constant zero stream at VB_BASE + MAX_STREAMS). */
static void bind_vertex_streams(void) {
    for (uint32_t s = 0; s < S.nstream && s < MAX_STREAMS; s++) {
        bufrange r = S.vbuf[s];
        id<MTLBuffer> b;
        uint64_t o;
        uint32_t stride = S.vstream[s].stride;
        if (stride && (stride & 3) && r.addr) {
            /* Streams with a stride that is not a multiple of 4 (2-bone skin weights/indices, 16-bit normals...) are copied into rows padded to 4. Only the rows this draw
             * reads are copied (index range recorded by the draw); the rest of the staging buffer is left untouched. */
            uint32_t ns = (stride + 3) & ~3u;
            uint64_t avail = r.size ? r.size / stride : 0;
            int64_t lo = g_vr_valid ? g_vr_lo : 0, hi = g_vr_valid ? g_vr_hi : (int64_t)avail - 1;
            if (S.vstream[s].divisor) {
                lo = 0;
                hi = (int64_t)avail - 1;
            } /* per-instance data: copy it all */
            if (lo < 0) lo = 0;
            if (hi >= (int64_t)avail && avail) hi = (int64_t)avail - 1;
            if (!avail || hi < lo) continue;
            if (hi > (1 << 20)) hi = 1 << 20;
            uint32_t ro;
            uint8_t *dst = ring_alloc(&g_ring, (size_t)(hi + 1) * ns, 4, &b, &ro);
            const uint8_t *src = (const uint8_t *)(uintptr_t)r.addr;
            for (int64_t v = lo; v <= hi; v++) {
                memcpy(dst + (size_t)v * ns, src + (size_t)v * stride, stride);
                memset(dst + (size_t)v * ns + stride, 0, ns - stride);
            }
            [g_re setVertexBuffer:b offset:ro atIndex:VB_BASE + s];
            continue;
        }
        if (r.addr && mtl_addr(r.addr, r.size ? r.size : 1, &b, &o)) [g_re setVertexBuffer:b offset:o atIndex:VB_BASE + s];
        else if (r.addr && mtl_addr(r.addr, 1, &b, &o)) {
            /* The stream starts inside a pool but its size runs past the pool's end (a stream spanning two mapped ranges, or a size larger than what the draw reads):
             * bind from its start; reads past the end return zero on this GPU. */
            LOG_ONCE(NAPPLE_LOG_WARN, "vertex stream %u at %#llx+%#llx runs past the end of its memory pool: bound from its start", s,
                     (unsigned long long)r.addr, (unsigned long long)r.size);
            [g_re setVertexBuffer:b offset:o atIndex:VB_BASE + s];
        } else if (r.addr && !stream_referenced(s)) {
            /* a stream no vertex attribute reads (stale binding of a freed pool): nothing to bind */
        } else if (r.addr) {
            /* Not inside any pool (memory the GPU could not reach on the console either): a zero-filled stream, never the previous draw's binding. */
            LOG_ONCE(NAPPLE_LOG_WARN, "vertex stream %u at %#llx+%#llx is not inside a memory pool: bound as zeros", s, (unsigned long long)r.addr,
                     (unsigned long long)r.size);
            uint32_t zo;
            uint32_t zn = (uint32_t)MIN(MAX(r.size, 256), 1u << 20);
            void *z = ring_alloc(&g_ring, zn, 256, &b, &zo);
            memset(z, 0, zn);
            [g_re setVertexBuffer:b offset:zo atIndex:VB_BASE + s];
        }
    }
    static const float zeros[4] = {0, 0, 0, 0};
    [g_re setVertexBytes:zeros length:16 atIndex:VB_BASE + MAX_STREAMS];
}
static int stage_draw_needed(uint32_t mode); /* mtl_stages.inc */
static int xfb_capturing(void);
/* Prepare pipeline + state for a draw. Returns 0 when the draw must be skipped. */
static int prepare_draw(uint32_t prim) {
    if (!S.render_enable && getenv("NAPPLE_RCLOG")) {
        static uint64_t n;
        if (n++ % 1000 == 0)
            fprintf(stderr, "[rclog] draw skipped by render enable #%llu: rt0 %ux%u fmt %d vs %016llx fs %016llx\n", (unsigned long long)n,
                    S.rt.color[0].s.d.width, S.rt.color[0].s.d.height, S.rt.color[0].s.d.format, (unsigned long long)S.prog[ST_VS],
                    (unsigned long long)S.prog[ST_FS]);
    }
    if (g_predict && stage_draw_needed(prim)) return 0; /* stage pipelines are built at the draw */
    if (S.raster_discard && S.render_enable && !g_predict && !xfb_capturing()) {
        static _Atomic uint64_t nrd;
        uint64_t k = atomic_fetch_add(&nrd, 1);
        if (k < 3 || (k & (k - 1)) == 0)
            LOGW("draw with rasterizer discard skipped (vs %016llx, #%llu): it may write storage buffers", (unsigned long long)S.prog[ST_VS],
                 (unsigned long long)k + 1);
    }
    if (!g_predict && getenv("NAPPLE_PREPLOG")) {
        static _Atomic uint64_t calls, norender, novs;
        uint64_t n = atomic_fetch_add(&calls, 1) + 1;
        if (!S.render_enable) atomic_fetch_add(&norender, 1);
        if (!S.prog[ST_VS]) atomic_fetch_add(&novs, 1);
        if (n % 20000 == 0) fprintf(stderr, "[preplog] draws %llu, render-disabled %llu, no vertex program %llu\n", (unsigned long long)n, (unsigned long long)atomic_load(&norender), (unsigned long long)atomic_load(&novs));
    }
    if (!S.render_enable || S.raster_discard) return 0;
    if (!S.prog[ST_VS]) return 0;
    /* CullFace FRONT_AND_BACK discards every triangle (Metal has no such cull mode): Mario Kart 8's title draws depth-only passes this way to draw nothing; drawing them
     * wrote the floor's depth and a glossy blue floor covered the title artwork. Lines and points are not culled. */
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && prim >= 4 && prim <= 9) return 0;
    if (g_predict) { /* look-ahead pass: only the pipeline key, built in the background; the draw itself comes with the real pass */
        if (!S.rt_valid) return 0;
        pass_info_from_rt();
        if (!P.w) return 0;
        pipe_key k;
        fill_pipe_key(prim, &k);
        static pipe_key last;
        if (memcmp(&k, &last, sizeof k)) {
            last = k;
            mtl_pipe_prefetch(&k);
        }
        return 0;
    }
    drawlog_update();
    { /* NAPPLE_DEBUG_SKIP_VS=<first frame>:<hash>,<hash>,...: from that (relative) frame on, frame first+i skips the draws of the i-th vertex program (bisecting which program draws a wrong shape) */
        static int init;
        static uint64_t first;
        static uint64_t list[64];
        static int n;
        if (!init) {
            init = 1;
            const char *e = getenv("NAPPLE_DEBUG_SKIP_VS");
            if (e) {
                first = strtoull(e, (char **)&e, 10);
                while (*e && n < 64) {
                    e++;
                    list[n++] = strtoull(e, (char **)&e, 16);
                }
            }
        }
        if (n) {
            uint64_t f = rel_frame();
            if (f >= first && f < first + (uint64_t)n && S.prog[ST_VS] == list[f - first]) return 0;
        }
    }
    { /* NAPPLE_DEBUG_SKIP_DRAWN=<first frame>:<first draw>: frame first+i skips the draw with ordinal (first draw + i) of that frame (counting draws that reach this point) */
        static int init2;
        static uint64_t f0, d0;
        static uint64_t lastf = ~0ull;
        static uint64_t ord;
        if (!init2) {
            init2 = 1;
            const char *e = getenv("NAPPLE_DEBUG_SKIP_DRAWN");
            if (e) {
                f0 = strtoull(e, (char **)&e, 10);
                d0 = *e ? strtoull(e + 1, NULL, 10) : 0;
            } else f0 = ~0ull;
        }
        if (f0 != ~0ull) {
            uint64_t f = rel_frame();
            if (f != lastf) {
                lastf = f;
                ord = 0;
            }
            if (f >= f0 && ord++ == d0 + (f - f0)) return 0;
        }
    }
    if (!g_event_init) event_init();
    if (g_event_fs && !g_event_frame && S.prog[ST_FS] == g_event_fs) {
        g_event_frame = atomic_load(&g_gfx.frames) + 1;
        LOGI("event: first draw with fragment program %016llx at frame %llu", (unsigned long long)g_event_fs, (unsigned long long)(g_event_frame - 1));
    }
    /* NAPPLE_DRAWLOG_EACH=1 (with NAPPLE_DRAWLOG_RT): split the pass after every draw so each draw's result is dumped */
    if (g_drawlog_on && P.open && P.draws && getenv("NAPPLE_DRAWLOG_EACH")) pass_end();
    if (!pass_begin()) return 0;
    vis_draw();
    pipe_key k;
    fill_pipe_key(prim, &k);
    if (g_drawlog_verbose) {
        fprintf(
            stderr,
            "[drawlog] frame %llu draw vs %016llx fs %016llx prim %u rt %ux%u n%d fmt0 %u depth %u/%u f%u blend0 %d (%u %u %u %u eq %u %u) wmask %x cull %d\n",
            (unsigned long long)atomic_load(&g_gfx.frames), (unsigned long long)S.prog[ST_VS], (unsigned long long)S.prog[ST_FS], prim, P.w, P.h, P.ncolor,
            P.color_fmt[0], S.ds.dt, S.ds.dw, (unsigned)S.ds.df, (S.color.blend_en & 1), (unsigned)S.blend[0].src_rgb, (unsigned)S.blend[0].dst_rgb,
            (unsigned)S.blend[0].src_a, (unsigned)S.blend[0].dst_a, (unsigned)S.blend[0].eq_rgb, (unsigned)S.blend[0].eq_a, S.chmask.mask & 0xf, S.poly.cull);
    }
    g_last_draw_fs = S.prog[ST_FS];
    GfxPipe *p = mtl_pipe_get(&k);
    {
        static int wl = -1;
        if (wl < 0) wl = getenv("NAPPLE_WINDRAWLOG") != NULL;
        if (wl && P.w == 1920 && P.color_fmt[0] == 71)
            fprintf(stderr, "[windraw] pipe %s vs %016llx fs %016llx\n", p ? "ok" : "NULL", (unsigned long long)S.prog[ST_VS],
                    (unsigned long long)S.prog[ST_FS]);
    }
    if (!p) {
        atomic_fetch_add(&g_gfx.draws_skipped, 1);
        return 0;
    }
    [g_re setRenderPipelineState:p->ps];
    [g_re setDepthStencilState:depth_stencil_state()];
    apply_raster_state();
    bind_vertex_streams();
    if (g_drawlog_verbose) {
        for (int st = 0; st < 2; st++)
            for (int i = 0; i < 4; i++)
                if (S.ssbo[st][i].addr) {
                    fprintf(stderr, "    ssbo stage %d [%d] addr %#llx size %#llx gpu %#llx:", st, i, (unsigned long long)S.ssbo[st][i].addr,
                            (unsigned long long)S.ssbo[st][i].size, (unsigned long long)gpu_va(S.ssbo[st][i].addr, S.ssbo[st][i].size));
                    const uint32_t *w = (const uint32_t *)(uintptr_t)S.ssbo[st][i].addr;
                    for (int k = 0; k < 24 && (uint64_t)k * 4 < S.ssbo[st][i].size; k++) fprintf(stderr, " %08x", w[k]);
                    fprintf(stderr, "\n");
                }
        for (int st = 0; st < 2; st++)
            for (int i = 0; i < 14; i++)
                if (S.ubo[st][i].addr) {
                    fprintf(stderr, "    ubo stage %d [%d] addr %#llx size %#llx:", st, i, (unsigned long long)S.ubo[st][i].addr,
                            (unsigned long long)S.ubo[st][i].size);
                    const float *w = (const float *)(uintptr_t)S.ubo[st][i].addr;
                    for (int k = 0; k < g_drawlog_ubo_words && (uint64_t)k * 4 < S.ubo[st][i].size; k++) fprintf(stderr, " %g", w[k]);
                    fprintf(stderr, "\n");
                    if (getenv("NAPPLE_DRAWLOG_UBOHEX")) {
                        fprintf(stderr, "    ubo hex stage %d [%d]:", st, i);
                        const uint32_t *hw = (const uint32_t *)(uintptr_t)S.ubo[st][i].addr;
                        for (int k = 0; k < g_drawlog_ubo_words && (uint64_t)k * 4 < S.ubo[st][i].size; k++) fprintf(stderr, " %08x", hw[k]);
                        fprintf(stderr, "\n");
                    }
                }
        for (uint32_t i = 0; i < S.nattr; i++)
            fprintf(stderr, "    attr[%u] stream %u offset %u format %#x\n", i, (unsigned)S.vattr[i].stream, (unsigned)S.vattr[i].offset,
                    (unsigned)S.vattr[i].fmt);
        for (uint32_t i = 0; i < S.nstream && i < 8; i++)
            fprintf(stderr, "    stream[%u] stride %u divisor %u\n", i, S.vstream[i].stride, S.vstream[i].divisor);
        for (uint32_t i = 0; i < S.nstream && i < 8; i++)
            if (S.vbuf[i].addr) {
                fprintf(stderr, "    vbuf [%u] addr %#llx size %#llx stride %u:", i, (unsigned long long)S.vbuf[i].addr, (unsigned long long)S.vbuf[i].size,
                        S.vstream[i].stride);
                const float *w = (const float *)(uintptr_t)S.vbuf[i].addr;
                for (int k = 0; k < 16 && (uint64_t)k * 4 < S.vbuf[i].size; k++) fprintf(stderr, " %g", w[k]);
                fprintf(stderr, "\n    vbuf [%u] hex:", i);
                const uint32_t *hx = (const uint32_t *)(uintptr_t)S.vbuf[i].addr;
                for (int k = 0; k < (getenv("NAPPLE_DRAWLOG_VBFULL") ? 4096 : 24) && (uint64_t)k * 4 < S.vbuf[i].size; k++) fprintf(stderr, " %08x", hx[k]);
                fprintf(stderr, "\n");
            }
        fprintf(stderr, "    vs uses_gmem %d fs uses_gmem %d gmem base %#llx size %#llx\n", p->vs->info.uses_gmem, p->fs ? p->fs->info.uses_gmem : 0,
                (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
    }
    bind_stage(ST_VS, p->vs, g_re, 0, NULL);
    if (p->fs) bind_stage(ST_FS, p->fs, g_re, 0, NULL);
    P.draws++;
    atomic_fetch_add(&g_gfx.draws, 1);
    return 1;
}

/* Index list for primitive types Metal lacks. Returns the number of indices (0 = unsupported). */
static uint32_t expand_indices(uint32_t mode, uint32_t first, uint32_t count, uint32_t *out) {
    uint32_t n = 0;
    switch (mode) {
        case 2:
            for (uint32_t i = 0; i < count; i++) { out[n++] = first + i; }
            out[n++] = first;
            break; /* line loop -> strip */
        case 6:
            for (uint32_t i = 1; i + 1 < count; i++) {
                out[n++] = first;
                out[n++] = first + i;
                out[n++] = first + i + 1;
            }
            break; /* fan */
        case 7:
            for (uint32_t i = 0; i + 3 < count; i += 4) {
                out[n++] = first + i;
                out[n++] = first + i + 1;
                out[n++] = first + i + 2;
                out[n++] = first + i;
                out[n++] = first + i + 2;
                out[n++] = first + i + 3;
            }
            break;
        case 8:
            for (uint32_t i = 0; i + 3 < count; i += 2) {
                out[n++] = first + i;
                out[n++] = first + i + 1;
                out[n++] = first + i + 3;
                out[n++] = first + i;
                out[n++] = first + i + 3;
                out[n++] = first + i + 2;
            }
            break;
        case 9:
            for (uint32_t i = 1; i + 1 < count; i++) {
                out[n++] = first;
                out[n++] = first + i;
                out[n++] = first + i + 1;
            }
            break;
        default: return 0;
    }
    return n;
}
static uint32_t expanded_max(uint32_t mode, uint32_t count) {
    switch (mode) {
        case 2: return count + 1;
        case 6:
        case 9: return (count ? count - 1 : 0) * 3;
        case 7: return count / 4 * 6;
        case 8: return count / 2 * 6;
        default: return 0;
    }
}
static MTLPrimitiveType expanded_type(uint32_t mode) { return mode == 2 ? MTLPrimitiveTypeLineStrip : MTLPrimitiveTypeTriangle; }

static id<MTLComputeCommandEncoder> compute_encoder(void);
#include "mtl_stages.inc"

static void do_draw_arrays(const p_draw_arrays *d) {
    int native;
    MTLPrimitiveType pt = prim_native(d->mode, &native);
    g_vr_valid = 1;
    g_vr_lo = d->first;
    g_vr_hi = (int64_t)d->first + d->count - 1;
    {
        sdraw sd = {d->mode, d->first, d->count, d->base_instance, d->instances, 0, 0, 0, 0};
        if (stage_draw(&sd)) return;
    }
    if (d->mode > 9) {
        LOG_ONCE(NAPPLE_LOG_WARN, "draw with unsupported primitive %u skipped", d->mode);
        return;
    }
    if (!prepare_draw(d->mode)) return;
    if (g_drawlog_verbose)
        fprintf(stderr, "    drawArrays mode %u first %u count %u instances %u base %u\n", d->mode, d->first, d->count, d->instances, d->base_instance);
    if (native) {
        [g_re drawPrimitives:pt vertexStart:d->first vertexCount:d->count instanceCount:d->instances baseInstance:d->base_instance];
        return;
    }
    uint32_t cap = expanded_max(d->mode, d->count);
    if (!cap) return;
    id<MTLBuffer> b;
    uint32_t o;
    uint32_t *idx = ring_alloc(&g_ring, cap * 4, 4, &b, &o);
    uint32_t n = expand_indices(d->mode, d->first, d->count, idx);
    if (n)
        [g_re drawIndexedPrimitives:expanded_type(d->mode)
                         indexCount:n
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:b
                  indexBufferOffset:o
                      instanceCount:d->instances
                         baseVertex:0
                       baseInstance:d->base_instance];
}

static void do_draw_elements(const p_draw_elems *d) {
    int native;
    MTLPrimitiveType pt = prim_native(d->mode, &native);
    g_vr_valid = 0;
    if (streams_misaligned() && d->count) { /* need the vertex range the indices reach */
        uint32_t isz0 = d->idx_type == 0 ? 1 : d->idx_type == 1 ? 2 : 4;
        const uint8_t *src = (const uint8_t *)(uintptr_t)d->idx_addr;
        uint32_t mn = ~0u, mx = 0;
        for (uint32_t i = 0; i < d->count; i++) {
            uint32_t v = isz0 == 1 ? src[i] : isz0 == 2 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        g_vr_valid = 1;
        g_vr_lo = (int64_t)mn + d->base_vertex;
        g_vr_hi = (int64_t)mx + d->base_vertex;
    }
    {
        sdraw sd = {d->mode, 0, d->count, d->base_instance, d->instances, d->idx_type, 1, (int32_t)d->base_vertex, d->idx_addr};
        if (stage_draw(&sd)) return;
    }
    if (d->mode > 9) {
        LOG_ONCE(NAPPLE_LOG_WARN, "draw with unsupported primitive %u skipped", d->mode);
        return;
    }
    if (!prepare_draw(d->mode)) return;
    if (g_drawlog_verbose) {
        fprintf(stderr, "    drawElements mode %u count %u instances %u base_vertex %d idx_type %u idx_addr %#llx\n", d->mode, d->count, d->instances,
                d->base_vertex, d->idx_type, (unsigned long long)d->idx_addr);
        uint32_t isz0 = d->idx_type == 0 ? 1 : d->idx_type == 1 ? 2 : 4, mx = 0;
        const uint8_t *src = (const uint8_t *)(uintptr_t)d->idx_addr;
        for (uint32_t i = 0; i < d->count; i++) {
            uint32_t v = isz0 == 1 ? src[i] : isz0 == 2 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
            if (v > mx) mx = v;
        }
        fprintf(stderr, "    indices max %u first:", mx);
        for (uint32_t i = 0; i < d->count && i < 24; i++)
            fprintf(stderr, " %u", isz0 == 1 ? src[i] : isz0 == 2 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i]);
        fprintf(stderr, "\n");
    }
    uint32_t isz = d->idx_type == 0 ? 1 : d->idx_type == 1 ? 2 : 4;
    id<MTLBuffer> b;
    uint64_t o;
    id<MTLBuffer> rb;
    uint32_t ro;
    MTLIndexType it = isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    if (!native) {
        /* read the indices, expand per primitive type through a scratch index list */
        uint32_t cap = expanded_max(d->mode, d->count);
        if (!cap) return;
        uint32_t *tmp = malloc((size_t)cap * 4), *seq = malloc((size_t)d->count * 4);
        if (!tmp || !seq) {
            free(tmp);
            free(seq);
            return;
        }
        const uint8_t *src = (const uint8_t *)(uintptr_t)d->idx_addr;
        for (uint32_t i = 0; i < d->count; i++) seq[i] = isz == 1 ? src[i] : isz == 2 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
        uint32_t n = expand_indices(d->mode, 0, d->count, tmp);
        uint32_t *out = ring_alloc(&g_ring, n * 4, 4, &rb, &ro);
        for (uint32_t i = 0; i < n; i++) out[i] = seq[tmp[i]];
        free(tmp);
        free(seq);
        if (!n) return;   /* a zero index count draws nothing; Metal validation rejects it */
        [g_re drawIndexedPrimitives:expanded_type(d->mode)
                         indexCount:n
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:rb
                  indexBufferOffset:ro
                      instanceCount:d->instances
                         baseVertex:d->base_vertex
                       baseInstance:d->base_instance];
        return;
    }
    if (isz == 1 || !mtl_addr(d->idx_addr, (uint64_t)d->count * isz, &b, &o) || (o % isz)) {
        void *p = ring_alloc(&g_ring, d->count * (isz == 1 ? 2 : isz), 4, &rb, &ro);
        const uint8_t *src = (const uint8_t *)(uintptr_t)d->idx_addr;
        if (isz == 1) {
            uint16_t *w = p;
            for (uint32_t i = 0; i < d->count; i++) w[i] = src[i];
            it = MTLIndexTypeUInt16;
        } else memcpy(p, src, (size_t)d->count * isz);
        b = rb;
        o = ro;
    }
    if (!d->count) return;   /* a zero index count draws nothing; Metal validation rejects it */
    [g_re drawIndexedPrimitives:pt
                     indexCount:d->count
                      indexType:it
                    indexBuffer:b
              indexBufferOffset:o
                  instanceCount:d->instances
                     baseVertex:d->base_vertex
                   baseInstance:d->base_instance];
}

static void do_draw_indirect(const p_draw_ind *d, int indexed) {
    int native;
    MTLPrimitiveType pt = prim_native(d->mode, &native);
    if (xfb_capturing() || stage_draw_needed(d->mode)) {
        /* The stage passes assemble primitives on the CPU, so the arguments are read at encode time (as do_dispatch reads indirect dispatch counts):
         * right when the game wrote them before the submit, wrong for arguments another GPU pass of this submission produces (logged once). */
        id<MTLBuffer> ab;
        uint64_t ao;
        if (!mtl_addr(d->indirect, indexed ? 20 : 16, &ab, &ao)) return;
        uint32_t a[5] = {0};
        memcpy(a, (const uint8_t *)ab.contents + ao, indexed ? 20 : 16);
        LOG_ONCE(NAPPLE_LOG_INFO, "indirect draw with transform feedback / geometry / tessellation: arguments read at encode time (%u elements, %u instances)", a[0], a[1]);
        sdraw sd = indexed ? (sdraw){d->mode, 0, a[0], a[4], a[1], d->idx_type, 1, (int32_t)a[3], d->idx_addr + (uint64_t)a[2] * (d->idx_type == 0 ? 1 : d->idx_type == 1 ? 2 : 4)}
                           : (sdraw){d->mode, a[2], a[0], a[3], a[1], 0, 0, 0, 0};
        if (!g_predict && stage_draw(&sd)) return;
        if (g_predict && stage_draw_needed(d->mode)) return;
    }
    if (!native) {
        LOG_ONCE(NAPPLE_LOG_WARN, "indirect draw with primitive %u skipped", d->mode);
        return;
    }
    g_vr_valid = 0;
    if (!prepare_draw(d->mode)) return;
    id<MTLBuffer> ib;
    uint64_t io;
    if (!mtl_addr(d->indirect, indexed ? 20 : 16, &ib, &io)) return;
    if (!indexed) {
        [g_re drawPrimitives:pt indirectBuffer:ib indirectBufferOffset:io];
        return;
    }
    uint32_t isz = d->idx_type == 1 ? 2 : 4;
    id<MTLBuffer> b;
    uint64_t o;
    if (!mtl_addr(d->idx_addr, 4, &b, &o)) return;
    [g_re drawIndexedPrimitives:pt
                      indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                    indexBuffer:b
              indexBufferOffset:o
                 indirectBuffer:ib
           indirectBufferOffset:io];
}

/* ---------------------------------------------------------------------------------------------------------------
 * Compute
 * --------------------------------------------------------------------------------------------------------------- */
static id<MTLComputeCommandEncoder> compute_encoder(void) {
    if (!g_ce) {
        flush_clears();
        end_encoders();
        g_ce = [g_cb computeCommandEncoder];
        g_enc_serial++;
    }
    return g_ce;
}
/* MultiDraw*IndirectCount. Metal has no draw count read from a buffer: a compute pass copies the argument records of draws i < min(*count, max)
 * into scratch memory, packed, and zeroes the rest (a zero vertex / index count draws nothing), then max indirect draws read the scratch copy.
 * The count is written by the GPU in the usual use (culling compute), so it cannot be read at encode time. The compute pass ends the open
 * render pass; the next one loads the attachments. */
static NSString *const kIndCountMSL = @"#include <metal_stdlib>\nusing namespace metal;\n"
                                       "kernel void ic(device const uint *count [[buffer(0)]], device const uchar *src [[buffer(1)]], device uint *dst "
                                       "[[buffer(2)]], constant uint4 &p [[buffer(3)]],\n"
                                       "               uint i [[thread_position_in_grid]]) {\n" /* p = (max draws, stride, words per record, 0) */
                                       "  if (i >= p.x) return; uint n = min(count[0], p.x); device const uint *s = (device const uint *)(src + i * p.y);\n"
                                       "  for (uint w = 0; w < p.z; w++) dst[i * p.z + w] = i < n ? s[w] : 0; }\n";
static void do_draw_indirect_count(const p_draw_ind_count *d) {
    int native;
    MTLPrimitiveType pt = prim_native(d->mode, &native);
    if (!native) {
        LOG_ONCE(NAPPLE_LOG_WARN, "indirect draw with primitive %u skipped", d->mode);
        return;
    }
    static id<MTLComputePipelineState> cps;
    if (!cps) {
        NSError *e = nil;
        id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:kIndCountMSL options:nil error:&e];
        cps = lib ? [g_mtl.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ic"] error:&e] : nil;
        if (!cps) {
            LOGE("indirect count kernel: %s", e.localizedDescription.UTF8String);
            return;
        }
    }
    uint32_t words = d->indexed ? 5 : 4, max = d->max_draws;
    if ((d->stride & 3) || (d->indirect & 3) || (d->count_addr & 3)) {
        LOG_ONCE(NAPPLE_LOG_WARN, "MultiDraw*IndirectCount with unaligned arguments skipped");
        return;
    }
    id<MTLBuffer> cb, ab, sb;
    uint64_t co, ao;
    uint32_t so;
    if (!mtl_addr(d->count_addr, 4, &cb, &co) || !mtl_addr(d->indirect, (uint64_t)(max - 1) * d->stride + words * 4, &ab, &ao)) return;
    ring_alloc(&g_ring, max * words * 4, 256, &sb, &so);
    pass_end();
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:cps];
    [ce setBuffer:cb offset:co atIndex:0];
    [ce setBuffer:ab offset:ao atIndex:1];
    [ce setBuffer:sb offset:so atIndex:2];
    uint32_t prm[4] = {max, (uint32_t)d->stride, words, 0};
    [ce setBytes:prm length:sizeof prm atIndex:3];
    NSUInteger tg = cps.threadExecutionWidth;
    [ce dispatchThreads:MTLSizeMake(max, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg < max ? tg : max, 1, 1)];
    end_encoders();
    g_vr_valid = 0;
    if (!prepare_draw(d->mode)) return;
    id<MTLBuffer> ib = nil;
    uint64_t io = 0;
    if (d->indexed && !mtl_addr(d->idx_addr, 4, &ib, &io)) return;
    MTLIndexType it = d->idx_type == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    for (uint32_t i = 0; i < max; i++) {
        if (d->indexed) [g_re drawIndexedPrimitives:pt indexType:it indexBuffer:ib indexBufferOffset:io indirectBuffer:sb indirectBufferOffset:so + i * 20];
        else [g_re drawPrimitives:pt indirectBuffer:sb indirectBufferOffset:so + i * 16];
    }
}
static void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    if (!S.prog[ST_CS]) return;
    GfxShader *sh;
    uint32_t cimask, cumask;
    image_masks(ST_CS, &cimask, &cumask);
    id<MTLComputePipelineState> ps = mtl_compute_pipe(S.prog[ST_CS], cimask, cumask, &sh);
    if (!ps) return;
    uint32_t cnt[3] = {x, y, z};
    if (indirect) {
        id<MTLBuffer> b;
        uint64_t o;
        if (mtl_addr(indirect, 12, &b, &o)) memcpy(cnt, (uint8_t *)b.contents + o, 12);
    }
    id<MTLComputeCommandEncoder> ce = compute_encoder();
    [ce setComputePipelineState:ps];
    bind_stage(ST_CS, sh, ce, 1, cnt);
    MTLSize local = MTLSizeMake(sh->local[0] ? sh->local[0] : 1, sh->local[1] ? sh->local[1] : 1, sh->local[2] ? sh->local[2] : 1);
    [ce dispatchThreadgroups:MTLSizeMake(cnt[0], cnt[1], cnt[2]) threadsPerThreadgroup:local];
    atomic_fetch_add(&g_gfx.dispatches, 1);
    drawlog_update();
    if (g_drawlog_verbose) {
        fprintf(stderr, "[drawlog] dispatch cs %016llx groups %u %u %u local %lu %lu %lu\n", (unsigned long long)S.prog[ST_CS], cnt[0], cnt[1], cnt[2],
                (unsigned long)local.width, (unsigned long)local.height, (unsigned long)local.depth);
        end_encoders();
        id<MTLCommandBuffer> cb = g_cb;
        cb_commit(0);
        [cb waitUntilCompleted];
        cb_begin();
        for (int i = 0; i < 14; i++)
            if (S.ubo[ST_CS][i].addr) {
                fprintf(stderr, "    cs ubo [%d] addr %#llx size %#llx:", i, (unsigned long long)S.ubo[ST_CS][i].addr,
                        (unsigned long long)S.ubo[ST_CS][i].size);
                const float *w = (const float *)(uintptr_t)S.ubo[ST_CS][i].addr;
                for (int k = 0; k < g_drawlog_ubo_words && (uint64_t)k * 4 < S.ubo[ST_CS][i].size; k++) fprintf(stderr, " %g", w[k]);
                fprintf(stderr, "\n");
            }
        for (int i = 0; i < 6; i++)
            if (S.ssbo[ST_CS][i].addr) {
                fprintf(stderr, "    cs ssbo [%d] addr %#llx size %#llx after:", i, (unsigned long long)S.ssbo[ST_CS][i].addr,
                        (unsigned long long)S.ssbo[ST_CS][i].size);
                const uint32_t *w = (const uint32_t *)(uintptr_t)S.ssbo[ST_CS][i].addr;
                for (int k = 0; k < 16 && (uint64_t)k * 4 < S.ssbo[ST_CS][i].size; k++) fprintf(stderr, " %08x", w[k]);
                fprintf(stderr, "\n");
            }
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Copies
 * --------------------------------------------------------------------------------------------------------------- */
static void do_copy_b2b(const p_copy_b2b *c) {
    id<MTLBuffer> sb, db;
    uint64_t so, dof;
    if (!mtl_addr(c->src, c->size, &sb, &so) || !mtl_addr(c->dst, c->size, &db, &dof)) return;
    [blit() copyFromBuffer:sb sourceOffset:so toBuffer:db destinationOffset:dof size:c->size];
}
static void do_clear_buffer(uint64_t addr, uint64_t size, uint32_t v) {
    id<MTLBuffer> b;
    uint64_t o;
    if (!mtl_addr(addr, size, &b, &o)) return;
    uint8_t byte = v & 0xff;
    if (v == (uint32_t)byte * 0x01010101u) {
        [blit() fillBuffer:b range:NSMakeRange(o, size) value:byte];
        return;
    }
    id<MTLBuffer> rb;
    uint32_t ro;
    uint32_t *p = ring_alloc(&g_ring, (uint32_t)size, 4, &rb, &ro);
    for (uint64_t i = 0; i < size / 4; i++) p[i] = v;
    [blit() copyFromBuffer:rb sourceOffset:ro toBuffer:b destinationOffset:o size:size];
}
static void copy_region_dims(const GfxTex *t, const int32_t r[6], uint32_t lv, uint32_t *x, uint32_t *y, uint32_t *z, uint32_t *w, uint32_t *h, uint32_t *d) {
    uint32_t mw = t->w >> lv ? t->w >> lv : 1, mh = t->h >> lv ? t->h >> lv : 1;
    *x = (uint32_t)r[0];
    *y = (uint32_t)r[1];
    *z = (uint32_t)r[2];
    *w = r[3] > 0 ? (uint32_t)r[3] : mw;
    *h = r[4] > 0 ? (uint32_t)r[4] : mh;
    *d = r[5] > 0 ? (uint32_t)r[5] : 1;
}
/* Upload of 24-bit depth data into the 32-bit float depth texture that stands in for DEPTH24 (0x33, depth in the low 24 bits of a 32-bit word) and DEPTH24_STENCIL8
 * (0x35, depth in the high 24 bits, stencil in the low 8: the Maxwell Z24S8 layout): the words are converted on the CPU and blitted into the depth (and
 * stencil) aspect. Metal has no 24-bit depth format on Apple GPUs. */
static void copy_d24_to_tex(GfxTex *t, const p_copy_xfer *c, const uint8_t *src, NSUInteger row, NSUInteger img, uint32_t lv, uint32_t sl, uint32_t x,
                            uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d) {
    (void)c;
    int stencil = t->key.d.format == 0x35;
    int is3d = t->tex.textureType == MTLTextureType3D;
    uint32_t slice = is3d ? 0 : sl + z, ns = is3d ? 1 : d;
    for (uint32_t k = 0; k < ns; k++) {
        id<MTLBuffer> bd, bs = nil;
        uint32_t od, os = 0;
        float *df = ring_alloc(&g_ring, w * h * 4, 256, &bd, &od);
        uint8_t *sb = stencil ? ring_alloc(&g_ring, w * h, 256, &bs, &os) : NULL;
        for (uint32_t j = 0; j < h; j++) {
            const uint32_t *rp = (const uint32_t *)(src + k * img + (size_t)j * row);
            for (uint32_t i = 0; i < w; i++) {
                uint32_t v = rp[i];
                if (stencil) {
                    df[j * w + i] = (float)(v >> 8) / 16777215.0f;
                    sb[j * w + i] = (uint8_t)v;
                } else df[j * w + i] = (float)(v & 0xffffff) / 16777215.0f;
            }
        }
        id<MTLBlitCommandEncoder> be = blit();
        [be copyFromBuffer:bd
                   sourceOffset:od
              sourceBytesPerRow:(NSUInteger)w * 4
            sourceBytesPerImage:(NSUInteger)w * h * 4
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:t->tex
               destinationSlice:slice + k
               destinationLevel:lv
              destinationOrigin:MTLOriginMake(x, y, 0)
                        options:stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        if (stencil)
            [be copyFromBuffer:bs
                       sourceOffset:os
                  sourceBytesPerRow:(NSUInteger)w
                sourceBytesPerImage:(NSUInteger)w * h
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:t->tex
                   destinationSlice:slice + k
                   destinationLevel:lv
                  destinationOrigin:MTLOriginMake(x, y, 0)
                            options:MTLBlitOptionStencilFromDepthStencil];
    }
}
static void do_copy_xfer(const p_copy_xfer *c, int to_tex) {
    GfxTex *t = mtl_tex_get(&c->tex.s, to_tex);
    if (!t) return;
    int bw, bh;
    int bpb = napple_fmt_bpb(t->key.d.format, &bw, &bh);
    if (!bpb || t->expand_rgb) {
        LOG_ONCE(NAPPLE_LOG_WARN, "texture copy for format %#x not supported", t->key.d.format);
        return;
    }
    if (t->rsx != 1 || t->rsy != 1) {
        LOG_ONCE(NAPPLE_LOG_WARN, "buffer <-> texture copy of a scaled render target (x%.2f) is skipped; set rscale=0 for this game if it needs the pixels",
                 t->rsx);
        return;
    }
    uint32_t lv = c->tex.has_view && c->tex.view.set_levels ? c->tex.view.base_level : 0;
    uint32_t sl = c->tex.has_view && c->tex.view.set_layers ? c->tex.view.min_layer : 0;
    uint32_t x, y, z, w, h, d;
    copy_region_dims(t, c->region, lv, &x, &y, &z, &w, &h, &d);
    NSUInteger row = c->row_stride ? (NSUInteger)c->row_stride : (NSUInteger)((w + bw - 1) / bw) * bpb;
    NSUInteger img = c->img_stride ? (NSUInteger)c->img_stride : row * ((h + bh - 1) / bh);
    id<MTLBuffer> b;
    uint64_t o;
    if (!mtl_addr(c->buf, img * d, &b, &o)) return;
    if (t->is_depth && (t->key.d.format == 0x33 || t->key.d.format == 0x35)) { /* 24-bit depth: the Metal texture is 32-bit float depth */
        if (!to_tex) {
            LOG_ONCE(NAPPLE_LOG_WARN, "reading back a 24-bit depth texture (format %#x) into memory is not implemented", t->key.d.format);
            return;
        }
        copy_d24_to_tex(t, c, (const uint8_t *)(uintptr_t)c->buf, row, img, lv, sl, x, y, z, w, h, d);
        return;
    }
    int is3d = t->tex.textureType == MTLTextureType3D;
    uint32_t slice = is3d ? 0 : (sl + z), nslices = is3d ? 1 : d;
    for (uint32_t k = 0; k < nslices; k++) {
        if (to_tex)
            [blit() copyFromBuffer:b
                       sourceOffset:o + k * img
                  sourceBytesPerRow:row
                sourceBytesPerImage:img
                         sourceSize:MTLSizeMake(w, h, is3d ? d : 1)
                          toTexture:t->tex
                   destinationSlice:slice + k
                   destinationLevel:lv
                  destinationOrigin:MTLOriginMake(x, y, is3d ? z : 0)];
        else
            [blit() copyFromTexture:t->tex
                             sourceSlice:slice + k
                             sourceLevel:lv
                            sourceOrigin:MTLOriginMake(x, y, is3d ? z : 0)
                              sourceSize:MTLSizeMake(w, h, is3d ? d : 1)
                                toBuffer:b
                       destinationOffset:o + k * img
                  destinationBytesPerRow:row
                destinationBytesPerImage:img];
    }
}
/* Scaled and/or format-converting texture-to-texture copy (the console's 2D engine does both): a draw sampling the source region into the destination
 * region. Depth/stencil, multisampled and 3D textures are not handled (reported once). */
static NSString *const kXferMSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V xv(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = r.xy + float2(p.x, 1.0 - p.y) * r.zw; return o; }\n"
     "fragment float4 xf(V in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) { return tex.sample(s, in.uv); }\n"
     "struct DOut { float d [[depth(any)]]; };\n"
     "fragment DOut xfd(V in [[stage_in]], depth2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) { DOut o; o.d = tex.sample(s, in.uv); return o; }\n";
static id<MTLLibrary> g_xfer_lib;
static NSMutableDictionary<NSNumber *, id> *g_xfer_pso;
static id<MTLSamplerState> g_xfer_samp[2];
static id<MTLRenderPipelineState> xfer_pso(MTLPixelFormat pf) {
    if (!g_xfer_pso) g_xfer_pso = [NSMutableDictionary new];
    id p = g_xfer_pso[@(pf)];
    if (p) return p;
    NSError *err = nil;
    if (!g_xfer_lib) {
        g_xfer_lib = [g_mtl.dev newLibraryWithSource:kXferMSL options:nil error:&err];
        if (!g_xfer_lib) {
            LOGE("texture copy shader: %s", err.localizedDescription.UTF8String);
            return nil;
        }
        for (int i = 0; i < 2; i++) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            g_xfer_samp[i] = [g_mtl.dev newSamplerStateWithDescriptor:sd];
        }
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g_xfer_lib newFunctionWithName:@"xv"];
    pd.fragmentFunction = [g_xfer_lib newFunctionWithName:@"xf"];
    pd.colorAttachments[0].pixelFormat = pf;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) {
        LOGE("texture copy pipeline for format %lu: %s", (unsigned long)pf, err.localizedDescription.UTF8String);
        return nil;
    }
    g_xfer_pso[@(pf)] = p;
    return p;
}
/* Depth -> depth copy with a size change (Link's Awakening scales its depth buffer down): a draw that writes the sampled depth through [[depth(any)]] into the
 * destination's depth attachment (nearest sampling: no blending of depths; the stencil aspect, when there is one, is not copied). */
static id<MTLRenderPipelineState> xfer_depth_pso(MTLPixelFormat pf) {
    static NSMutableDictionary<NSNumber *, id> *pso;
    if (!pso) pso = [NSMutableDictionary new];
    id p = pso[@(pf)];
    if (p) return p;
    if (!xfer_pso(MTLPixelFormatRGBA8Unorm)) return nil; /* creates the library and samplers */
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g_xfer_lib newFunctionWithName:@"xv"];
    pd.fragmentFunction = [g_xfer_lib newFunctionWithName:@"xfd"];
    pd.depthAttachmentPixelFormat = pf;
    NSError *err = nil;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) {
        LOGE("depth copy pipeline for format %lu: %s", (unsigned long)pf, err.localizedDescription.UTF8String);
        return nil;
    }
    pso[@(pf)] = p;
    return p;
}
static void copy_t2t_draw(GfxTex *s, GfxTex *d, uint32_t slv, uint32_t dlv, uint32_t ssl, uint32_t dsl, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                          uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    int depth_copy = s->is_depth && d->is_depth && s->pf != MTLPixelFormatStencil8 && d->pf != MTLPixelFormatStencil8;
    if ((s->is_depth != d->is_depth) || (s->is_depth && !depth_copy) || s->tex.sampleCount > 1 || d->tex.sampleCount > 1 ||
        s->tex.textureType == MTLTextureType3D || d->tex.textureType == MTLTextureType3D) {
        LOG_ONCE(NAPPLE_LOG_WARN, "texture copy with a depth/multisampled/3D texture and a different size or format is not implemented (format %lu -> %lu)",
                 (unsigned long)s->pf, (unsigned long)d->pf);
        return;
    }
    { /* Metal aborts on a view outside the texture: refuse such a copy instead */
        NSUInteger sn = s->tex.textureType == MTLTextureTypeCube        ? 6
                        : s->tex.textureType == MTLTextureTypeCubeArray ? s->tex.arrayLength * 6
                                                                        : (s->tex.arrayLength ? s->tex.arrayLength : 1);
        NSUInteger dn = d->tex.textureType == MTLTextureTypeCube        ? 6
                        : d->tex.textureType == MTLTextureTypeCubeArray ? d->tex.arrayLength * 6
                                                                        : (d->tex.arrayLength ? d->tex.arrayLength : 1);
        if (slv >= s->tex.mipmapLevelCount || dlv >= d->tex.mipmapLevelCount || ssl >= sn || dsl >= dn) {
            LOG_ONCE(NAPPLE_LOG_WARN, "texture copy outside the texture ignored (level %u/%u slice %u/%u)", slv, dlv, ssl, dsl);
            return;
        }
    }
    id<MTLRenderPipelineState> pso = depth_copy ? xfer_depth_pso(d->pf) : xfer_pso(d->pf);
    if (!pso) return;
    LOG_ONCE(NAPPLE_LOG_INFO, "texture copy by draw: %ux%u format %lu -> %ux%u format %lu", sw, sh, (unsigned long)s->pf, dw, dh, (unsigned long)d->pf);
    id<MTLTexture> src = s->tex;
    if (src.textureType != MTLTextureType2D || slv || ssl)
        src = [s->tex newTextureViewWithPixelFormat:s->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(slv, 1) slices:NSMakeRange(ssl, 1)];
    id<MTLTexture> dst = d->tex;
    if (dst.textureType != MTLTextureType2D || dlv || dsl)
        dst = [d->tex newTextureViewWithPixelFormat:d->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(dlv, 1) slices:NSMakeRange(dsl, 1)];
    float tw = (float)(src.width), th = (float)(src.height);
    float r[4] = {sx / tw, sy / th, sw / tw, sh / th};
    flush_clears();
    end_encoders();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth_copy) {
        rp.depthAttachment.texture = dst;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
    } else {
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pso];
    if (depth_copy) {
        static id<MTLDepthStencilState> dsw;
        if (!dsw) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            dsw = [g_mtl.dev newDepthStencilStateWithDescriptor:dd];
        }
        [e setDepthStencilState:dsw];
    }
    [e setViewport:(MTLViewport){(double)dx, (double)dy, (double)dw, (double)dh, 0, 1}];
    [e setScissorRect:(MTLScissorRect){dx, dy, dw, dh}];
    [e setVertexBytes:r length:sizeof r atIndex:0];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:g_xfer_samp[(sw != dw || sh != dh) && !depth_copy ? 1 : 0] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}
static void do_copy_t2t(const p_copy_t2t *c) {
    GfxTex *s = mtl_tex_get(&c->src.s, 0), *d = mtl_tex_get(&c->dst.s, 1);
    if (!s || !d) return;
    if (getenv("NAPPLE_T2TLOG")) {
        static int n;
        if (n++ < 400) fprintf(stderr, "[t2t] %ux%u type %lu fmt %lu (own %d) -> %ux%u type %lu fmt %lu (own %d) dview layers %d+%d dsl %d\n", s->w, s->h, (unsigned long)s->tex.textureType, (unsigned long)s->pf, s->gpu_owned, d->w, d->h,
                (unsigned long)d->tex.textureType, (unsigned long)d->pf, d->gpu_owned, c->dst.view.set_layers, c->dst.view.min_layer, c->dst.has_view);
    }
    mtl_tex_prepare_sample(s);
    uint32_t slv = c->src.has_view && c->src.view.set_levels ? c->src.view.base_level : 0,
             dlv = c->dst.has_view && c->dst.view.set_levels ? c->dst.view.base_level : 0;
    uint32_t ssl = c->src.has_view && c->src.view.set_layers ? c->src.view.min_layer : 0,
             dsl = c->dst.has_view && c->dst.view.set_layers ? c->dst.view.min_layer : 0;
    uint32_t sx, sy, sz, sw, sh, sd, dx, dy, dz, dw, dh, dd;
    copy_region_dims(s, c->sregion, slv, &sx, &sy, &sz, &sw, &sh, &sd);
    copy_region_dims(d, c->dregion, dlv, &dx, &dy, &dz, &dw, &dh, &dd);
    { /* render target scaling: the regions are in guest pixels, the allocations may be larger (mtl_scale.m) */
        float srx = s->rsx > 0 ? s->rsx : 1, sry = s->rsy > 0 ? s->rsy : 1, drx = d->rsx > 0 ? d->rsx : 1, dry = d->rsy > 0 ? d->rsy : 1;
        if (srx != 1 || sry != 1 || drx != 1 || dry != 1) {
            uint32_t asx = (uint32_t)lroundf(sx * srx), asy = (uint32_t)lroundf(sy * sry), asw = (uint32_t)lroundf((sx + sw) * srx) - asx,
                     ash = (uint32_t)lroundf((sy + sh) * sry) - asy;
            uint32_t adx = (uint32_t)lroundf(dx * drx), ady = (uint32_t)lroundf(dy * dry), adw = (uint32_t)lroundf((dx + dw) * drx) - adx,
                     adh = (uint32_t)lroundf((dy + dh) * dry) - ady;
            if (srx == drx && sry == dry && sw == dw && sh == dh) {
                sx = asx;
                sy = asy;
                sw = asw;
                sh = ash;
                dx = adx;
                dy = ady;
                dw = asw;
                dh = ash;
            } else {
                copy_t2t_draw(s, d, slv, dlv, ssl + sz, dsl + dz, asx, asy, asw, ash, adx, ady, adw, adh);
                return;
            }
        }
    }
    if (sw != dw || sh != dh || s->pf != d->pf) {
        copy_t2t_draw(s, d, slv, dlv, ssl + sz, dsl + dz, sx, sy, sw, sh, dx, dy, dw, dh);
        return;
    }
    int s3 = s->tex.textureType == MTLTextureType3D, d3 = d->tex.textureType == MTLTextureType3D;
    [blit() copyFromTexture:s->tex
                sourceSlice:s3 ? 0 : ssl + sz
                sourceLevel:slv
               sourceOrigin:MTLOriginMake(sx, sy, s3 ? sz : 0)
                 sourceSize:MTLSizeMake(sw, sh, s3 ? sd : 1)
                  toTexture:d->tex
           destinationSlice:d3 ? 0 : dsl + dz
           destinationLevel:dlv
          destinationOrigin:MTLOriginMake(dx, dy, d3 ? dz : 0)];
}
/* Downsample (MSAA resolve): a pass that loads the multisampled source and resolves it into the destination's level / layer (texture views).
 * The source keeps its samples (StoreAndMultisampleResolve): games resolve mid-frame and go on drawing into the multisampled target. Depth
 * resolves take sample 0, as the console's 2D engine does; integer formats cannot be resolved by Metal and are skipped (reported once). */
/* Integer formats cannot be resolved by Metal's fixed-function resolve (averaging has no meaning). The hardware's downsample of an integer target copies sample 0:
 * a draw reads that sample of every pixel and writes it to the single-sampled destination. */
static int pf_is_sint(MTLPixelFormat pf) {
    switch (pf) {
        case MTLPixelFormatR8Sint:
        case MTLPixelFormatRG8Sint:
        case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatR16Sint:
        case MTLPixelFormatRG16Sint:
        case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA32Sint: return 1;
        default: return 0;
    }
}
static id<MTLRenderPipelineState> resolve_int_pso(MTLPixelFormat pf) {
    static NSMutableDictionary<NSNumber *, id> *pso;
    if (!pso) pso = [NSMutableDictionary new];
    id p = pso[@(pf)];
    if (p) return p;
    int sg = pf_is_sint(pf);
    NSString *src = [NSString
        stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                          "vertex float4 rv(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); return float4(p * 2.0 - 1.0, 0, 1); }\n"
                          "fragment %s rf(float4 pos [[position]], texture2d_ms<%s> t [[texture(0)]]) { return t.read(uint2(pos.xy), 0); }\n",
                         sg ? "int4" : "uint4", sg ? "int" : "uint"];
    NSError *err = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        LOGE("integer resolve shader: %s", err.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"rv"];
    pd.fragmentFunction = [lib newFunctionWithName:@"rf"];
    pd.colorAttachments[0].pixelFormat = pf;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) {
        LOGE("integer resolve pipeline for format %lu: %s", (unsigned long)pf, err.localizedDescription.UTF8String);
        return nil;
    }
    pso[@(pf)] = p;
    return p;
}
static void resolve_int(GfxTex *s, GfxTex *d, uint32_t ssl, uint32_t dlv, uint32_t dsl) {
    if (s->pf != d->pf || s->tex.textureType != MTLTextureType2DMultisample || ssl) {
        LOG_ONCE(NAPPLE_LOG_WARN, "Downsample of an integer format (%lu) between different formats or of an array texture is not implemented",
                 (unsigned long)s->pf);
        return;
    }
    id<MTLRenderPipelineState> ps = resolve_int_pso(s->pf);
    if (!ps) return;
    flush_clears();
    end_encoders();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d->tex;
    rp.colorAttachments[0].level = dlv;
    rp.colorAttachments[0].slice = dsl;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setFragmentTexture:s->tex atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}
/* Generic colour resolve (what Metal's fixed-function resolve refuses: another destination format, another size, an array slice of an integer source): a
 * pass into the destination level / slice whose fragment function reads the multisampled source (texture2d_ms, or texture2d_ms_array at slice ssl) at the
 * scaled pixel and averages the samples (float formats) or takes sample 0 (integer formats, as the 2D engine does). Format conversion happens on the write. */
static id<MTLRenderPipelineState> resolve_generic_pso(MTLPixelFormat dst, int kind /* 0 float, 1 sint, 2 uint */, int array) {
    static NSMutableDictionary<NSNumber *, id> *pso;
    if (!pso) pso = [NSMutableDictionary new];
    NSNumber *key = @((uint64_t)dst | (uint64_t)kind << 32 | (uint64_t)array << 40);
    id p = pso[key];
    if (p) return p;
    const char *t = kind == 1 ? "int" : kind == 2 ? "uint" : "float";
    NSString *src = [NSString
        stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nstruct RP { float2 scale; uint slice, ns; };\n"
                          "vertex float4 rv(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); return float4(p * 2.0 - 1.0, 0, 1); }\n"
                          "fragment %s4 rf(float4 pos [[position]], texture2d_ms%s<%s> t [[texture(0)]], constant RP &rp [[buffer(0)]]) {\n"
                          "  uint2 c = uint2(float2(pos.xy) * rp.scale);\n%s}\n",
                         t, array ? "_array" : "", t,
                         kind ? (array ? "  return t.read(c, rp.slice, 0);\n" : "  return t.read(c, 0);\n")
                              : (array ? "  float4 a = 0; for (uint i = 0; i < rp.ns; i++) a += t.read(c, rp.slice, i); return a / float(rp.ns);\n"
                                       : "  float4 a = 0; for (uint i = 0; i < rp.ns; i++) a += t.read(c, i); return a / float(rp.ns);\n")];
    NSError *err = nil;
    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        LOGE("generic resolve shader: %s", err.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"rv"];
    pd.fragmentFunction = [lib newFunctionWithName:@"rf"];
    pd.colorAttachments[0].pixelFormat = dst;
    p = [g_mtl.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) {
        LOGE("generic resolve pipeline for format %lu: %s", (unsigned long)dst, err.localizedDescription.UTF8String);
        return nil;
    }
    pso[key] = p;
    return p;
}
static void resolve_generic(GfxTex *s, GfxTex *d, uint32_t ssl, uint32_t dlv, uint32_t dsl) {
    int kind = pf_is_int((uint32_t)s->pf) ? (pf_is_sint(s->pf) ? 1 : 2) : 0;
    int array = s->tex.textureType == MTLTextureType2DMultisampleArray;
    if (s->is_depth || d->is_depth || (kind != 0) != (pf_is_int((uint32_t)d->pf) != 0)) {
        LOG_ONCE(NAPPLE_LOG_WARN, "Downsample between depth or integer / float formats (%lu -> %lu) is not implemented", (unsigned long)s->pf, (unsigned long)d->pf);
        return;
    }
    id<MTLRenderPipelineState> ps = resolve_generic_pso(d->pf, kind, array);
    if (!ps) return;
    LOG_ONCE(NAPPLE_LOG_INFO, "Downsample through the generic resolve pass (source format %lu %lux%lu%s -> destination %lu %lux%lu)", (unsigned long)s->pf,
             (unsigned long)s->tex.width, (unsigned long)s->tex.height, array ? " array" : "", (unsigned long)d->pf, (unsigned long)(d->tex.width >> dlv),
             (unsigned long)(d->tex.height >> dlv));
    flush_clears();
    end_encoders();
    NSUInteger dw = MAX(1, d->tex.width >> dlv), dh = MAX(1, d->tex.height >> dlv);
    struct { float scale[2]; uint32_t slice, ns; } rp = {{(float)s->tex.width / dw, (float)s->tex.height / dh}, ssl, (uint32_t)s->tex.sampleCount};
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = d->tex;
    rpd.colorAttachments[0].level = dlv;
    rpd.colorAttachments[0].slice = dsl;
    rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rpd];
    [e setRenderPipelineState:ps];
    [e setFragmentTexture:s->tex atIndex:0];
    [e setFragmentBytes:&rp length:sizeof rp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}
static void do_downsample(const p_copy_t2t *c) {
    GfxTex *s = mtl_tex_get(&c->src.s, 1), *d = mtl_tex_get(&c->dst.s, 1);
    if (!s || !d) return;
    if (s->tex.sampleCount < 2) {
        LOG_ONCE(NAPPLE_LOG_WARN, "Downsample from a single-sampled texture: copied instead");
        p_copy_t2t cc = *c;
        memset(cc.sregion, 0, sizeof cc.sregion);
        memset(cc.dregion, 0, sizeof cc.dregion);
        end_encoders();
        do_copy_t2t(&cc);
        return;
    }
    uint32_t ssl = c->src.has_view && c->src.view.set_layers ? c->src.view.min_layer : 0;
    uint32_t dlv = c->dst.has_view && c->dst.view.set_levels ? c->dst.view.base_level : 0,
             dsl = c->dst.has_view && c->dst.view.set_layers ? c->dst.view.min_layer : 0;
    if (pf_is_int((uint32_t)s->pf)) {
        if (s->pf == d->pf && s->tex.textureType == MTLTextureType2DMultisample && !ssl) resolve_int(s, d, ssl, dlv, dsl);
        else resolve_generic(s, d, ssl, dlv, dsl);
        return;
    }
    if (s->pf != d->pf || (d->tex.width >> dlv) != s->tex.width || (d->tex.height >> dlv) != s->tex.height) {
        resolve_generic(s, d, ssl, dlv, dsl);
        return;
    }
    flush_clears();
    end_encoders();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (s->is_depth) {
        if (s->pf != MTLPixelFormatStencil8) {
            rp.depthAttachment.texture = s->tex;
            rp.depthAttachment.slice = ssl;
            rp.depthAttachment.resolveTexture = d->tex;
            rp.depthAttachment.resolveLevel = dlv;
            rp.depthAttachment.resolveSlice = dsl;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        }
        if (s->has_stencil) {
            rp.stencilAttachment.texture = s->tex;
            rp.stencilAttachment.slice = ssl;
            rp.stencilAttachment.resolveTexture = d->tex;
            rp.stencilAttachment.resolveLevel = dlv;
            rp.stencilAttachment.resolveSlice = dsl;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        }
    } else {
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[0];
        a.texture = s->tex;
        a.slice = ssl;
        a.resolveTexture = d->tex;
        a.resolveLevel = dlv;
        a.resolveSlice = dsl;
        a.loadAction = MTLLoadActionLoad;
        a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    }
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
}
static void do_clear_tex(const p_copy_xfer *c) {
    GfxTex *t = mtl_tex_get(&c->tex.s, 1);
    if (!t) return;
    end_encoders();
    uint32_t kind = c->flags & 0xff, mask = (c->flags >> 8) & 0xf;
    uint32_t lv = c->tex.has_view && c->tex.view.set_levels ? c->tex.view.base_level : 0;
    uint32_t sl = c->tex.has_view && c->tex.view.set_layers ? c->tex.view.min_layer : 0;
    uint32_t v[4];
    memcpy(v, &c->buf, 8);
    memcpy(v + 2, &c->row_stride, 8);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t->is_depth) {
        rp.depthAttachment.texture = t->tex;
        rp.depthAttachment.level = lv;
        rp.depthAttachment.slice = sl;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = *(float *)&v[0];
        rp.depthAttachment.storeAction = MTLStoreActionStore;
    } else {
        rp.colorAttachments[0].texture = t->tex;
        rp.colorAttachments[0].level = lv;
        rp.colorAttachments[0].slice = sl;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = kind == 0 ? MTLClearColorMake(*(float *)&v[0], *(float *)&v[1], *(float *)&v[2], *(float *)&v[3])
                                                      : MTLClearColorMake((int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        (void)mask;
    }
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
}

/* ---------------------------------------------------------------------------------------------------------------
 * Op interpreter
 * --------------------------------------------------------------------------------------------------------------- */
static void exec_rec(const cmdrec *r, int depth);

static void wait_event(const p_event *e) {
    cb_commit(0);
    volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)e->addr;
    for (int spins = 0;; spins++) {
        uint32_t cur = *v;
        if (e->mode == 0 ? cur == e->value : (int32_t)(cur - e->value) >= 0) break;
        if (spins > 2000000) {
            LOG_ONCE(NAPPLE_LOG_WARN, "WaitEvent timed out");
            break;
        }
        usleep(50);
    }
    cb_begin();
}

/* Conditional rendering (NVN SetRenderEnableConditional): the condition compares the 64-bit counters at addr and addr + 16, evaluated here at replay from
 * memory. A timestamp report replayed earlier in the same submission has not reached memory yet and counts as 0 (the time goes in the second field of the
 * record); a samples-passed result (an occlusion query) is only known when the GPU completes, so a condition on it keeps drawing. (Animal Crossing and Zelda TOTK
 * wrap their clears in these conditions: ignoring them wipes targets that were just drawn, a black frame; the idea comes from nxnative's render_cond.) */
#define RC_PEND 256
static struct {
    uint64_t addr;
    int ts;
} g_rc_pend[RC_PEND];
static int g_rc_npend;
static void rc_note_report(uint64_t addr, uint32_t type, int is_value) {
    if (g_rc_npend >= RC_PEND) g_rc_npend = 0;
    g_rc_pend[g_rc_npend].addr = addr;
    g_rc_pend[g_rc_npend].ts = !is_value && type != NVN_COUNTER_TYPE_SAMPLES_PASSED && type < 0x20;
    g_rc_npend++;
}
static int rc_value(uint64_t a, uint64_t *v) { /* 0 = not known at replay time */
    for (int i = g_rc_npend - 1; i >= 0; i--)
        if (g_rc_pend[i].addr == a) {
            if (!g_rc_pend[i].ts) return 0;
            *v = 0;
            return 1;
        }
    if (a < 0x100000 || (a & 7)) return 0;
    *v = *(const volatile uint64_t *)(uintptr_t)a;
    return 1;
}
static int predict_skips(uint16_t op) {
    switch (op) {
        case OP_CLEAR_COLOR:
        case OP_CLEAR_DS:
        case OP_DISCARD_COLOR:
        case OP_DISCARD_DS:
        case OP_DISPATCH:
        case OP_DISPATCH_IND:
        case OP_COPY_B2B:
        case OP_COPY_B2T:
        case OP_COPY_T2B:
        case OP_COPY_T2T:
        case OP_DOWNSAMPLE:
        case OP_CLEAR_TEX:
        case OP_CLEAR_BUF:
        case OP_UPDATE_UBO:
        case OP_FENCE_SYNC:
        case OP_WAIT_SYNC:
        case OP_SIGNAL_EVENT:
        case OP_WAIT_EVENT:
        case OP_REPORT_COUNTER:
        case OP_RESET_COUNTER:
        case OP_DRAW_TEXTURE:
        case OP_BARRIER:
        case OP_SCREEN_MARK:
        case OP_XFB:
        case OP_DRAW_XFB: return 1;
        default: return 0;
    }
}
static void exec_op(const op_hdr *h, const uint8_t *pl, int depth) {
    if (g_predict && predict_skips(h->op)) return;
    if (g_predict &&
        (h->op == OP_DRAW_ARRAYS || h->op == OP_DRAW_ELEMENTS || h->op == OP_DRAW_ARRAYS_IND || h->op == OP_DRAW_ELEMENTS_IND || h->op == OP_DRAW_IND_COUNT)) {
        prepare_draw(*(
            const uint32_t *)pl); /* every draw payload starts with the primitive mode; no index conversion or scratch allocation (the ring is not open yet) */
        return;
    }
    if (!g_predict) {
        static long fr = -2;
        if (fr == -2) {
            const char *e = getenv("NAPPLE_OPLOG_FRAME");
            fr = e ? atol(e) : -1;
        }
        if (fr >= 0 && (long)atomic_load(&g_gfx.frames) == fr) {
            fprintf(stderr, "[oplog] d%d op %u", depth, h->op);
            if (h->op == OP_SET_RT) {
                const p_set_rt *r = (const p_set_rt *)pl;
                fprintf(stderr, " n %u c0 mem %#llx %ux%u fmt %d", r->n, (unsigned long long)(uintptr_t)r->color[0].s.mem, r->color[0].s.d.width,
                        r->color[0].s.d.height, r->color[0].s.d.format);
            } else if (h->op == OP_BIND_TEX) {
                const uint32_t *u = (const uint32_t *)pl;
                fprintf(stderr, " stage %u first %u n %u:", u[0], u[1], u[2]);
                for (uint32_t k = 0; k < u[2] && k < 16; k++) fprintf(stderr, " %llu", (unsigned long long)(((const uint64_t *)(u + 4))[k] & 0xfffff));
            } else if (h->op == OP_BIND_PROGRAM) {
                const p_bind_program *b = (const p_bind_program *)pl;
                fprintf(stderr, " mask %x vs %016llx fs %016llx", b->stage_mask, (unsigned long long)b->hash[0], (unsigned long long)b->hash[4]);
            } else if (h->op == OP_CALL) {
                const p_call *c = (const p_call *)pl;
                const uint64_t *l = (const uint64_t *)(c + 1);
                fprintf(stderr, " count %u", c->count);
                for (uint32_t i = 0; i < c->count && i < 6; i++) {
                    const cmdrec *r = (const cmdrec *)(uintptr_t)l[i];
                    fprintf(stderr, " [len %u nchild %u magic %#x]", r->len, r->nchildren, r->magic);
                }
            } else if (h->op == OP_BIND_UBO || h->op == OP_BIND_SSBO || h->op == OP_BIND_VBUF || h->op == OP_DRAW_ELEMENTS) {
                const uint64_t *q = (const uint64_t *)pl;
                fprintf(stderr, " w %016llx %016llx %016llx %016llx", (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                        (unsigned long long)q[3]);
            } else if (h->op == OP_RENDER_ENABLE) {
                const uint32_t *u = (const uint32_t *)pl;
                fprintf(stderr, " en %u mode %u addr %#llx", u[0], u[1], (unsigned long long)*(const uint64_t *)(u + 2));
            }
            fputc('\n', stderr);
        }
    }
    switch (h->op) {
        case OP_CALL: {
            const p_call *c = (const p_call *)pl;
            const uint64_t *l = (const uint64_t *)(c + 1);
            if (depth < 8)
                for (uint32_t i = 0; i < c->count; i++) exec_rec((const cmdrec *)(uintptr_t)l[i], depth + 1);
            break;
        }
        case OP_BIND_PROGRAM: {
            const p_bind_program *p = (const p_bind_program *)pl;
            for (int s = 0; s < 6; s++)
                if (p->stage_mask & (1u << s)) { S.prog[s] = gfx_prog_resolve_hash(p->hash[s]); }
            if (p->stage_mask & NVN_SHADER_STAGE_FRAGMENT_BIT) S.sample_shading = (uint8_t)p->sample_shading;
            g_pk_dirty = 1;
            break;
        }
        case OP_BIND_BLEND: {
            blend_st b;
            memcpy(&b, pl, sizeof b);
            S.blend[b.target & 7] = b;
            g_pk_dirty = 1;
            break;
        }
        case OP_BIND_COLOR:
            memcpy(&S.color, pl, sizeof S.color);
            g_pk_dirty = 1;
            break;
        case OP_BIND_CHMASK:
            memcpy(&S.chmask, pl, sizeof S.chmask);
            g_pk_dirty = 1;
            break;
        case OP_BIND_MS: {
            memcpy(&S.ms, pl, sizeof S.ms);
            g_pk_dirty = 1;
            if (P.open) {
                uint8_t n, pos[8];
                sample_positions(&n, pos);
                if (n != P.sl_n || memcmp(pos, P.sl_pos, 8)) pass_end();
            } /* new sample positions: new pass */
            break;
        }
        case OP_BIND_POLY: memcpy(&S.poly, pl, sizeof S.poly); break;
        case OP_BIND_DS: memcpy(&S.ds, pl, sizeof S.ds); break;
        case OP_BIND_VATTR: {
            const p_range *r = (const p_range *)pl;
            S.nattr = r->n;
            memcpy(S.vattr, r + 1, r->n * sizeof(va_st));
            g_pk_dirty = 1;
            break;
        }
        case OP_BIND_VSTREAM: {
            const p_range *r = (const p_range *)pl;
            S.nstream = r->n;
            memcpy(S.vstream, r + 1, r->n * sizeof(vs_st));
            g_pk_dirty = 1;
            break;
        }
        case OP_BIND_VBUF: {
            const p_range *r = (const p_range *)pl;
            const bufrange *b = (const bufrange *)(r + 1);
            for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) S.vbuf[r->first + i] = b[i];
            break;
        }
        case OP_BIND_UBO:
        case OP_BIND_SSBO: {
            const p_stage_range *r = (const p_stage_range *)pl;
            const bufrange *b = (const bufrange *)(r + 1);
            if (r->stage > 5) break;
            for (uint32_t i = 0; i < r->n; i++) {
                if (h->op == OP_BIND_UBO) {
                    if (r->first + i < 14) S.ubo[r->stage][r->first + i] = b[i];
                } else if (r->first + i < 16) S.ssbo[r->stage][r->first + i] = b[i];
            }
            break;
        }
        case OP_BIND_TEX:
        case OP_BIND_IMG:
        case OP_BIND_SEPTEX:
        case OP_BIND_SEPSAMP: {
            const p_stage_range *r = (const p_stage_range *)pl;
            const uint64_t *hs = (const uint64_t *)(r + 1);
            if (r->stage > 5) break;
            if (h->op == OP_BIND_TEX)
                for (uint32_t i = 0; i < r->n && r->first + i < 128; i++) S.tex[r->stage][r->first + i] = hs[i];
            else if (h->op == OP_BIND_IMG)
                for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) S.img[r->stage][r->first + i] = hs[i];
            else LOG_ONCE(NAPPLE_LOG_WARN, "separate texture/sampler binding is not supported yet");
            break;
        }
        case OP_SET_TEXPOOL: S.texpool = *(const uint64_t *)pl; break;
        case OP_SET_SAMPPOOL: S.samppool = *(const uint64_t *)pl; break;
        case OP_SET_RT:
            if (!g_predict) {
                flush_clears();
                pass_end();
            }
            memcpy(&S.rt, pl, sizeof S.rt);
            S.rt_valid = S.rt.n > 0 || S.rt.depth.s.valid;
            g_pk_dirty = 1;
            break;
        case OP_VIEWPORTS: {
            const p_range *r = (const p_range *)pl;
            const float *f = (const float *)(r + 1);
            {
                static int n;
                if (n < 12 && g_gfx.log_level >= NAPPLE_LOG_DEBUG) {
                    n++;
                    LOGD("viewport[%u..] x %g y %g w %g h %g (origin mode %u)", r->first, f[0], f[1], f[2], f[3], g_gfx.device_origin);
                }
            }
            for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) memcpy(S.vp[r->first + i], f + 4 * i, 16);
            break;
        }
        case OP_SCISSORS: {
            const p_range *r = (const p_range *)pl;
            const int32_t *f = (const int32_t *)(r + 1);
            for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) memcpy(S.sc[r->first + i], f + 4 * i, 16);
            S.sc_set = 1;
            break;
        }
        case OP_DEPTH_RANGES: {
            const p_range *r = (const p_range *)pl;
            const float *f = (const float *)(r + 1);
            for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) memcpy(S.dr[r->first + i], f + 2 * i, 8);
            break;
        }
        case OP_BLEND_COLOR: memcpy(S.blend_color, pl, 16); break;
        case OP_STENCIL: {
            const uint32_t *p = (const uint32_t *)pl;
            for (int f = 0; f < 2; f++)
                if (p[1] & (1u << f)) {
                    if (p[0] == 0) S.stencil_ref[f] = p[2];
                    else if (p[0] == 1) S.stencil_wmask[f] = p[2];
                    else S.stencil_rmask[f] = p[2];
                }
            break;
        }
        case OP_POLY_OFFSET: {
            const float *f = (const float *)pl;
            S.poly_factor = f[0];
            S.poly_units = f[1];
            S.poly_clamp = f[2];
            break;
        }
        case OP_PRIM_RESTART: {
            const uint32_t *u = (const uint32_t *)pl;
            S.prim_restart_en = (int)u[0];
            S.prim_restart_idx = u[1];
            break;
        }
        case OP_ALPHA_REF: S.alpha_ref = *(const float *)pl; break;
        case OP_DEPTH_CLAMP: S.depth_clamp = (int)*(const uint32_t *)pl; break;
        case OP_RASTER_DISCARD: S.raster_discard = (int)*(const uint32_t *)pl; break;
        case OP_RENDER_ENABLE: {
            const uint32_t *u = (const uint32_t *)pl;
            if (!u[1]) {
                S.render_enable = (int)u[0];
                break;
            }
            uint64_t addr = *(const uint64_t *)(u + 2), x = 0, y = 0;
            int known = rc_value(addr, &x) && rc_value(addr + 16, &y), if_not_equal = (int)u[1] - 1;
            int skip = known && (if_not_equal ? x == y : x != y);
            S.render_enable = !skip;
            break;
        }
        case OP_LINE_WIDTH:
        case OP_POINT_SIZE:
        case OP_SAMPLE_MASK:
        case OP_DEPTH_BOUNDS: break;
        case OP_CLEAR_COLOR: do_clear_color((const p_clear_color *)pl); break;
        case OP_CLEAR_DS: do_clear_ds((const p_clear_ds *)pl); break;
        case OP_DISCARD_COLOR: {
            uint32_t i = *(const uint32_t *)pl;
            if (i < 8 && !P.open) P.discard_color[i] = 1;
            break;
        }
        case OP_DISCARD_DS:
            if (!P.open) P.discard_ds = 1;
            break;
        case OP_DRAW_ARRAYS: do_draw_arrays((const p_draw_arrays *)pl); break;
        case OP_DRAW_ELEMENTS: do_draw_elements((const p_draw_elems *)pl); break;
        case OP_DRAW_ARRAYS_IND: do_draw_indirect((const p_draw_ind *)pl, 0); break;
        case OP_DRAW_ELEMENTS_IND: do_draw_indirect((const p_draw_ind *)pl, 1); break;
        case OP_DRAW_IND_COUNT: do_draw_indirect_count((const p_draw_ind_count *)pl); break;
        case OP_DISPATCH: {
            const p_dispatch *d = (const p_dispatch *)pl;
            {
                static _Atomic uint64_t nd;
                uint64_t k = atomic_fetch_add(&nd, 1);
                if (getenv("NAPPLE_DISPLOG") && (k < 5 || (k & (k - 1)) == 0))
                    fprintf(stderr, "[displog] dispatch #%llu %u,%u,%u cs %016llx\n", (unsigned long long)k + 1, d->x, d->y, d->z,
                            (unsigned long long)S.prog[5]);
            }
            do_dispatch(d->x, d->y, d->z, 0);
            break;
        }
        case OP_DISPATCH_IND: do_dispatch(0, 0, 0, *(const uint64_t *)pl); break;
        case OP_COPY_B2B:
            pass_end();
            do_copy_b2b((const p_copy_b2b *)pl);
            break;
        case OP_COPY_B2T:
            pass_end();
            do_copy_xfer((const p_copy_xfer *)pl, 1);
            break;
        case OP_COPY_T2B:
            pass_end();
            do_copy_xfer((const p_copy_xfer *)pl, 0);
            break;
        case OP_COPY_T2T:
            pass_end();
            do_copy_t2t((const p_copy_t2t *)pl);
            break;
        case OP_DOWNSAMPLE: do_downsample((const p_copy_t2t *)pl); break;
        case OP_CLEAR_TEX: do_clear_tex((const p_copy_xfer *)pl); break;
        case OP_CLEAR_BUF: {
            const uint64_t *u = (const uint64_t *)pl;
            pass_end();
            do_clear_buffer(u[0], u[1], (uint32_t)u[2]);
            break;
        }
        case OP_UPDATE_UBO: {
            const p_update_ubo *u = (const p_update_ubo *)pl;
            memcpy((void *)(uintptr_t)(u->addr + u->offset), u + 1, u->size);
            break;
        }
        case OP_COPY_STRIDES: {
            const int64_t *s = (const int64_t *)pl;
            S.copy_row_stride = s[0];
            S.copy_img_stride = s[1];
            break;
        }
        case OP_FENCE_SYNC: {
            const p_sync *s = (const p_sync *)pl;
            post_add(3, s->sync, 0, 0);
            break;
        }
        case OP_WAIT_SYNC: break;
        case OP_SIGNAL_EVENT: {
            const p_event *e = (const p_event *)pl;
            post_add(1, e->addr, e->mode, e->value);
            break;
        }
        case OP_WAIT_EVENT: wait_event((const p_event *)pl); break;
        case OP_REPORT_COUNTER: {
            const p_counter *c = (const p_counter *)pl;
            rc_note_report(c->addr, c->type, c->type == 0x100);
            /* In-order GPU semantics for the CPU's view: a timestamp report zeroes the counter field now, not when the command buffer completes. The game's condition
         * for its next clear of this target reads the same record (it sets the counter to 1 when the target needs a clear, the report resets it): evaluated against the
         * old value it would clear a target nothing drew into since, or skip one that was drawn. The time field is still written at completion. */
            if (c->type != 0x100 && c->type != NVN_COUNTER_TYPE_SAMPLES_PASSED && c->type < 0x20 && c->addr >= 0x100000 && !(c->addr & 7))
                __atomic_store_n((uint64_t *)(uintptr_t)c->addr, 0, __ATOMIC_SEQ_CST);
            if (c->type == 0x100) {
                post_add(6, c->addr, c->pad, 0);
                break;
            } /* ReportValue */
            if (c->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                if (!g_occl) {
                    g_occl = 1;
                    LOGI("occlusion queries in use: counting samples");
                }
                vis_close();
            }
            post_add(2, c->addr, c->type, 0);
            break;
        }
        case OP_RESET_COUNTER: {
            uint32_t t = *(const uint32_t *)pl;
            if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                if (!g_occl) {
                    g_occl = 1;
                    LOGI("occlusion queries in use: counting samples");
                }
                vis_close();
                post_add(5, 0, 0, 0);
            }
            break;
        }
        case OP_DRAW_TEXTURE: do_draw_texture((const p_draw_tex *)pl); break;
        case OP_PATCH_SIZE: S.patch_size = *(const uint32_t *)pl; break;
        case OP_TESS_LEVELS: {
            const p_tess_levels *t = (const p_tess_levels *)pl;
            if (t->outer) memcpy(S.tess_outer, t->v, sizeof S.tess_outer);
            else memcpy(S.tess_inner, t->v, sizeof S.tess_inner);
            break;
        }
        case OP_BIND_XFB: {
            const p_range *r = (const p_range *)pl;
            const bufrange *b = (const bufrange *)(r + 1);
            for (uint32_t i = 0; i < r->n && r->first + i < MAX_XFB_BUFFERS; i++) S.xfb[r->first + i] = b[i];
            break;
        }
        case OP_XFB: do_xfb((const p_xfb *)pl); break;
        case OP_DRAW_XFB: { /* vertex count = bytes written to buffer 0 / its stride, from the control memory End / Pause stored */
            const p_draw_xfb *x = (const p_draw_xfb *)pl;
            if (xfb_gpu_control(x->control)) { /* written on the GPU timeline: an indirect draw whose arguments a kernel derives from it */
                int native;
                MTLPrimitiveType pt = prim_native(x->mode, &native);
                id<MTLBuffer> cb, ab;
                uint64_t co;
                uint32_t ao;
                id<MTLComputePipelineState> ps = stage_kernel(SK_XFBARGS);
                if (!native || !ps || !mtl_addr(x->control, 32, &cb, &co) || !ring_alloc(&g_ring, 16, 256, &ab, &ao)) {
                    LOG_ONCE(NAPPLE_LOG_WARN, "DrawTransformFeedback of GPU-counted output with primitive %u skipped", x->mode);
                    break;
                }
                id<MTLComputeCommandEncoder> ce = compute_encoder();
                [ce setComputePipelineState:ps];
                [ce setBuffer:cb offset:co atIndex:0];
                [ce setBuffer:ab offset:ao atIndex:1];
                [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                end_encoders();
                g_vr_valid = 0;
                if (prepare_draw(x->mode)) [g_re drawPrimitives:pt indirectBuffer:ab indirectBufferOffset:ao];
                break;
            }
            const uint32_t *c = xfb_ctl_mem(x->control);
            uint32_t n = c && c[4] ? c[0] / c[4] : 0;
            if (n) {
                p_draw_arrays da = {x->mode, 0, n, 0, 1, 0};
                do_draw_arrays(&da);
            }
            break;
        }
        case OP_BARRIER:
            if (*(const uint32_t *)pl & BARRIER_RESOLVE_DEPTH) {
                flush_clears();
                end_encoders();
            }
            break;
        default: LOG_ONCE(NAPPLE_LOG_WARN, "unknown recorded op %u", h->op); break;
    }
}

static void exec_rec(const cmdrec *r, int depth) {
    const uint8_t *p = cmdrec_data(r), *end = p + r->len;
    while (p + sizeof(op_hdr) <= end) {
        const op_hdr *h = (const op_hdr *)p;
        if (h->op == OP_END || h->size < sizeof(op_hdr)) break;
        exec_op(h, p + sizeof(op_hdr), depth);
        p += h->size;
    }
}

void replay_run_submission(cmdrec *const *recs, uint32_t n, uint64_t seq) {
    static int init;
    if (!init) {
        state_reset();
        g_no_stages = getenv("NAPPLE_GFX_NO_STAGES") != NULL;
        init = 1;
    }
    {
        const char *e = getenv("NAPPLE_OPLOG_FRAME");
        if (e && (long)atomic_load(&g_gfx.frames) == atol(e)) fprintf(stderr, "[oplog] ==== submission seq %llu recs %u\n", (unsigned long long)seq, n);
    }
    g_seq = seq;
    g_rc_npend = 0;
    { /* look-ahead: walk the submission once for its pipeline keys and start the unknown ones building, then replay it for real (NAPPLE_GFX_NO_LOOKAHEAD=1 disables) */
        static int on = -1;
        if (on < 0) on = getenv("NAPPLE_GFX_NO_LOOKAHEAD") == NULL;
        if (on) {
            static nstate s0;
            static uint8_t p0[sizeof P];
            s0 = S;
            memcpy(p0, &P, sizeof P);
            g_predict = 1;
            for (uint32_t i = 0; i < n; i++) exec_rec(recs[i], 0);
            g_predict = 0;
            S = s0;
            memcpy(&P, p0, sizeof P);
        }
    }
    cb_begin();
    for (uint32_t i = 0; i < n; i++) exec_rec(recs[i], 0);
    flush_clears();
    pass_end();
    memset(&P, 0, sizeof P);
    cb_commit(1);
}

/* Global memory (LDG/STG, SSBO access) is addressed by Metal GPU virtual address: the driver-constant entry of each SSBO binding holds the GPU address of the
 * pool buffer that contains it (see gpu_va), and the translated shader dereferences it directly (`maxwell_gmem_size == ~0` selects that code shape).
 * Every pool buffer must be resident in the encoder (useResource), done lazily once per encoder. The guest-heap window of the first design cannot work:
 * the guest heap contains PROT_NONE holes (thread stacks moved by MapMemory) and Metal refuses to wrap such a range. */
void gfx_mtl_set_gmem(uint64_t base, uint64_t size) {
    (void)base;
    (void)size;
    maxwell_gmem_base = 0;
    maxwell_gmem_size = ~0ull;
}
