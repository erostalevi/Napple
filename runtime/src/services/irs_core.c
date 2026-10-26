#include "irs_core.h"
#include "hz_result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int irs_camera_handle(uint32_t id, uint32_t *out) {
    uint32_t controller;
    if (id <= 7) controller = id;
    else if (id == 0x20) controller = 8;
    else if (id == 0x10) controller = 9;
    else return -1;
    *out = controller; /* npad type byte 0 */
    return 0;
}

/* ---- processors ------------------------------------------------------------------------------------------------------------------ */
static IrRect clamp_window(IrRect r) {
    if (r.w <= 0 || r.h <= 0) return (IrRect){0, 0, IR_W, IR_H};
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    if (x1 > IR_W) x1 = IR_W;
    if (y1 > IR_H) y1 = IR_H;
    if (x1 <= x0 || y1 <= y0) return (IrRect){0, 0, IR_W, IR_H};
    return (IrRect){(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}
static inline uint8_t pre(uint8_t v, int negative, uint32_t mode, uint32_t thr) {
    if (negative) v = (uint8_t)(255 - v);
    if (mode == IR_PRE_BINARIZE) return v >= thr ? 255 : 0;
    if (mode == IR_PRE_CUTOFF) return v < thr ? 0 : v;
    return v;
}
void ir_moment_compute(const uint8_t *f, const IrMomentConfig *cfg, IrMomentData out[IR_MOMENT_BLOCKS]) {
    IrRect w = clamp_window(cfg->window);
    for (int by = 0; by < IR_MOMENT_ROWS; by++)
        for (int bx = 0; bx < IR_MOMENT_COLS; bx++) {
            int x0 = bx * w.w / IR_MOMENT_COLS, x1 = (bx + 1) * w.w / IR_MOMENT_COLS, y0 = by * w.h / IR_MOMENT_ROWS, y1 = (by + 1) * w.h / IR_MOMENT_ROWS;
            double sum = 0, sx = 0, sy = 0;
            long n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    double v = pre(f[(w.y + y) * IR_W + w.x + x], cfg->cam.negative, cfg->preprocess, cfg->threshold);
                    sum += v;
                    sx += v * x;
                    sy += v * y;
                    n++;
                }
            IrMomentData *d = &out[by * IR_MOMENT_COLS + bx];
            d->avg_intensity = n ? (float)(sum / (double)n) : 0.0f;
            d->cx = sum > 0 ? (float)(sx / sum) : (float)(x0 + x1 - 1) / 2.0f;
            d->cy = sum > 0 ? (float)(sy / sum) : (float)(y0 + y1 - 1) / 2.0f;
        }
}

int ir_cluster_compute(const uint8_t *f, const IrClusteringConfig *cfg, IrClusterData out[IR_MAX_CLUSTERS]) {
    IrRect w = clamp_window(cfg->window);
    uint8_t *seen = calloc(IR_W * IR_H, 1);
    int *stack = malloc(sizeof(int) * IR_W * IR_H);
    IrClusterData *all = malloc(sizeof(IrClusterData) * 512);
    int nall = 0;
    if (!seen || !stack || !all) {
        free(seen);
        free(stack);
        free(all);
        return 0;
    }
    uint32_t pmax = cfg->pixel_max ? cfg->pixel_max : 0xffffffffu;
    for (int y0 = w.y; y0 < w.y + w.h; y0++)
        for (int x0 = w.x; x0 < w.x + w.w; x0++) {
            int i0 = y0 * IR_W + x0;
            if (seen[i0] || pre(f[i0], cfg->cam.negative, 0, 0) < cfg->intensity_min) continue;
            /* flood fill, 8-connected, inside the window */
            int sp = 0;
            stack[sp++] = i0;
            seen[i0] = 1;
            double sx = 0, sy = 0, sv = 0;
            uint32_t cnt = 0;
            int minx = x0, maxx = x0, miny = y0, maxy = y0;
            while (sp) {
                int i = stack[--sp], x = i % IR_W, y = i / IR_W;
                cnt++;
                sx += x;
                sy += y;
                sv += pre(f[i], cfg->cam.negative, 0, 0);
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < w.x || nx >= w.x + w.w || ny < w.y || ny >= w.y + w.h) continue;
                        int ni = ny * IR_W + nx;
                        if (seen[ni] || pre(f[ni], cfg->cam.negative, 0, 0) < cfg->intensity_min) continue;
                        seen[ni] = 1;
                        stack[sp++] = ni;
                    }
            }
            if (cnt < cfg->pixel_min || cnt > pmax || nall >= 512) continue;
            all[nall++] = (IrClusterData){.avg_intensity = (uint8_t)(sv / cnt + 0.5),
                                          .cx = (float)(sx / cnt),
                                          .cy = (float)(sy / cnt),
                                          .pixel_count = cnt,
                                          .bx = (int16_t)minx,
                                          .by = (int16_t)miny,
                                          .bw = (int16_t)(maxx - minx + 1),
                                          .bh = (int16_t)(maxy - miny + 1)};
        }
    /* largest first; discovery (scan) order breaks ties */
    int n = nall < IR_MAX_CLUSTERS ? nall : IR_MAX_CLUSTERS;
    for (int k = 0; k < n; k++) {
        int best = k;
        for (int j = k + 1; j < nall; j++)
            if (all[j].pixel_count > all[best].pixel_count) best = j;
        IrClusterData t = all[best];
        for (int j = best; j > k; j--) all[j] = all[j - 1];
        all[k] = t;
        out[k] = t;
    }
    free(seen);
    free(stack);
    free(all);
    return n;
}

static void fmt_size(uint32_t f, int *w, int *h) {
    static const int W[] = {320, 160, 80, 40, 20}, H[] = {240, 120, 60, 30, 15};
    if (f > IR_FMT_20x15) f = IR_FMT_320x240;
    *w = W[f];
    *h = H[f];
}
size_t ir_image_make(const uint8_t *f, const IrTransferConfig *cfg, uint8_t *out, int *wp, int *hp) {
    int ow, oh, tw, th;
    fmt_size(cfg->origin_format, &ow, &oh);
    fmt_size(cfg->trimming_format, &tw, &th);
    if (tw > ow) tw = ow;
    if (th > oh) th = oh;
    int sx = cfg->trim_x, sy = cfg->trim_y;
    if (sx + tw > ow) sx = ow - tw;
    if (sy + th > oh) sy = oh - th;
    int k = IR_W / ow; /* 1, 2, 4, 8, 16 */
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            unsigned acc = 0;
            for (int j = 0; j < k; j++)
                for (int i = 0; i < k; i++) acc += pre(f[((sy + y) * k + j) * IR_W + (sx + x) * k + i], cfg->cam.negative, 0, 0);
            out[y * tw + x] = (uint8_t)((acc + (unsigned)(k * k) / 2) / (unsigned)(k * k));
        }
    *wp = tw;
    *hp = th;
    return (size_t)tw * (size_t)th;
}
uint32_t ir_ambient_noise(const uint8_t *f) {
    uint64_t s = 0;
    for (int i = 0; i < IR_W * IR_H; i++) s += f[i];
    uint32_t mean = (uint32_t)(s / (IR_W * IR_H));
    return mean < 40 ? 0 : mean < 100 ? 1 : 2;
}

/* ---- the simulator ------------------------------------------------------------------------------------------------------------- */
void irs_core_init(IrsCore *c) { memset(c, 0, sizeof *c); }
void irs_core_free(IrsCore *c) {
    for (int i = 0; i < IR_NCAM; i++) free(c->cam[i].image);
    if (c->started && c->have_src && c->src.stop) c->src.stop(c->src.ctx);
    memset(c, 0, sizeof *c);
}
void irs_core_set_source(IrsCore *c, const IrFrameSource *src) {
    if (c->started && c->have_src && c->src.stop) c->src.stop(c->src.ctx);
    c->started = 0;
    c->have_frame = 0;
    c->access = IR_ACCESS_PENDING;
    c->asked = 0;
    c->have_src = src != NULL;
    if (src) c->src = *src;
}
int irs_core_has_source(const IrsCore *c) { return c->have_src; }
void irs_core_access_done(void *core, int granted) { ((IrsCore *)core)->access = granted ? IR_ACCESS_GRANTED : IR_ACCESS_DENIED; }

uint32_t irs_core_run(IrsCore *c, unsigned cam, IrMode mode, const void *cfg, uint8_t *xfer, size_t xfer_size) {
    if (cam >= IR_NCAM || !c->have_src) return HZ_R_IRSENSOR_NOT_CONNECTED;
    if (c->access != IR_ACCESS_GRANTED) { /* ask on the first processor start, once per source */
        if (!c->asked) {
            c->asked = 1;
            int a = c->src.request_access ? c->src.request_access(c->src.ctx, irs_core_access_done, c) : IR_ACCESS_GRANTED;
            if (a != IR_ACCESS_PENDING) c->access = a; /* a synchronous answer through the callback is kept */
        }
        if (c->access == IR_ACCESS_DENIED) return HZ_R_IRSENSOR_NOT_CONNECTED;
    }
    if (!c->started) {
        if (c->src.start && c->src.start(c->src.ctx) != 0) return HZ_R_IRSENSOR_NOT_CONNECTED;
        c->started = 1;
    }
    IrsCam *k = &c->cam[cam];
    k->mode = mode;
    memset(&k->cfg, 0, sizeof k->cfg);
    if (mode == IR_MODE_MOMENT && cfg) k->cfg.moment = *(const IrMomentConfig *)cfg;
    if (mode == IR_MODE_CLUSTERING && cfg) k->cfg.cluster = *(const IrClusteringConfig *)cfg;
    if (mode == IR_MODE_TRANSFER && cfg) k->cfg.transfer = *(const IrTransferConfig *)cfg;
    k->xfer_dst = xfer;
    k->xfer_size = xfer_size;
    k->sampling = 0;
    return 0;
}
void irs_core_stop(IrsCore *c, unsigned cam) {
    if (cam >= IR_NCAM) return;
    c->cam[cam].mode = IR_MODE_NONE;
    c->cam[cam].xfer_dst = NULL;
    if (!irs_core_running(c) && c->started && c->have_src && c->src.stop) {
        c->src.stop(c->src.ctx);
        c->started = 0;
    }
}
int irs_core_running(const IrsCore *c) {
    int n = 0;
    for (int i = 0; i < IR_NCAM; i++) n += c->cam[i].mode != IR_MODE_NONE;
    return n;
}

static void seq_begin(uint64_t *seq) { __atomic_store_n(seq, *seq | 1, __ATOMIC_RELEASE); }
static void seq_end(uint64_t *seq) { __atomic_store_n(seq, (*seq | 1) + 1, __ATOMIC_RELEASE); }

int irs_core_pump(IrsCore *c, uint8_t *shm, uint64_t now_ns) {
    if (!c->have_src || !irs_core_running(c)) return 0;
    uint64_t ts = now_ns;
    int r = c->access == IR_ACCESS_GRANTED ? c->src.get_frame(c->src.ctx, c->frame, &ts) : 0;
    if (r < 0) return 0;
    if (r == 0) {
        if (c->access == IR_ACCESS_GRANTED || c->have_frame) return 0;
        memset(c->frame, 0, sizeof c->frame); /* permission pending: processors run on dark frames until access is decided */
    } else {
        c->have_frame = 1;
        c->frame_ts = ts;
    }
    int updated = 0;
    uint32_t noise = ir_ambient_noise(c->frame);
    for (unsigned i = 0; i < IR_NCAM; i++) {
        IrsCam *k = &c->cam[i];
        if (k->mode == IR_MODE_NONE) continue;
        k->sampling++;
        k->frames++;
        k->noise = noise;
        uint8_t *slot = shm ? shm + (size_t)i * IR_SLOT_SIZE : NULL;
        if (slot) {
            uint32_t mode = (uint32_t)k->mode;
            memcpy(slot, &mode, 4);
            memcpy(slot + 8, &k->frames, 8);
        }
        if (k->mode == IR_MODE_MOMENT) {
            IrMomentState st;
            memset(&st, 0, sizeof st);
            ir_moment_compute(c->frame, &k->cfg.moment, st.data);
            if (slot) {
                IrMomentState *d = (IrMomentState *)(slot + IR_SLOT_MOMENT);
                seq_begin(&d->seq);
                d->timestamp_ns = ts;
                d->sampling_number = k->sampling;
                d->ambient_noise = noise;
                memcpy(d->data, st.data, sizeof st.data);
                seq_end(&d->seq);
            }
        } else if (k->mode == IR_MODE_CLUSTERING) {
            IrClusterData cd[IR_MAX_CLUSTERS];
            int n = ir_cluster_compute(c->frame, &k->cfg.cluster, cd);
            if (slot) {
                IrClusterState *d = (IrClusterState *)(slot + IR_SLOT_CLUSTER);
                seq_begin(&d->seq);
                d->timestamp_ns = ts;
                d->sampling_number = k->sampling;
                d->count = (uint8_t)n;
                memset(d->data, 0, sizeof d->data);
                memcpy(d->data, cd, (size_t)n * sizeof cd[0]);
                seq_end(&d->seq);
            }
        } else if (k->mode == IR_MODE_TRANSFER) {
            if (!k->image) k->image = malloc(IR_W * IR_H);
            if (k->image) {
                k->image_size = ir_image_make(c->frame, &k->cfg.transfer, k->image, &k->image_w, &k->image_h);
                if (k->xfer_dst && k->xfer_size) memcpy(k->xfer_dst, k->image, k->image_size < k->xfer_size ? k->image_size : k->xfer_size);
            }
        }
        updated++;
    }
    return updated;
}
long irs_core_image(IrsCore *c, unsigned cam, uint8_t *dst, size_t cap, IrTransferState *st) {
    if (cam >= IR_NCAM || c->cam[cam].mode != IR_MODE_TRANSFER) return -1;
    IrsCam *k = &c->cam[cam];
    memset(st, 0, sizeof *st);
    st->sampling_number = k->sampling;
    st->ambient_noise = k->noise;
    if (!k->image || !k->image_size) return 0;
    size_t n = k->image_size < cap ? k->image_size : cap;
    if (dst) memcpy(dst, k->image, n);
    return (long)n;
}

/* ---- webcam adapter --------------------------------------------------------------------------------------------------------------- */
int irs_adapter_rgb_to_ir(const uint8_t *src, int w, int h, int stride, int channels, int bgr, const IrAdapterOpts *opts, uint8_t *out) {
    if (!src || !out || w < 2 || h < 2 || (channels != 3 && channels != 4) || stride < w * channels) return -1;
    IrAdapterOpts o = opts ? *opts : (IrAdapterOpts){0, 1, 0, 0};
    /* centred 4:3 crop of the source */
    int cw = w, ch = h;
    if (cw * 3 > ch * 4) cw = ch * 4 / 3;
    else ch = cw * 3 / 4;
    int ox = (w - cw) / 2, oy = (h - ch) / 2;
    int ri = bgr ? 2 : 0, bi = bgr ? 0 : 2;
    for (int y = 0; y < IR_H; y++) {
        int y0 = oy + y * ch / IR_H, y1 = oy + (y + 1) * ch / IR_H;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < IR_W; x++) {
            int x0 = ox + x * cw / IR_W, x1 = ox + (x + 1) * cw / IR_W;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long acc = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const uint8_t *p = src + (size_t)yy * stride + (size_t)xx * channels;
                    acc += (299u * p[ri] + 587u * p[1] + 114u * p[bi]) / 1000u;
                    n++;
                }
            out[y * IR_W + (o.mirror ? IR_W - 1 - x : x)] = (uint8_t)(n ? acc / n : 0);
        }
    }
    if (o.stretch) {
        unsigned hist[256] = {0};
        for (int i = 0; i < IR_W * IR_H; i++) hist[out[i]]++;
        unsigned total = IR_W * IR_H, lo_t = total / 100, hi_t = total - total / 100, acc = 0;
        int lo = 0, hi = 255;
        for (int v = 0; v < 256; v++) {
            acc += hist[v];
            if (acc > lo_t) {
                lo = v;
                break;
            }
        }
        acc = 0;
        for (int v = 0; v < 256; v++) {
            acc += hist[v];
            if (acc >= hi_t) {
                hi = v;
                break;
            }
        }
        if (hi - lo >= 8)
            for (int i = 0; i < IR_W * IR_H; i++) {
                int v = ((int)out[i] - lo) * 255 / (hi - lo);
                out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    }
    if (o.threshold > 0)
        for (int i = 0; i < IR_W * IR_H; i++) out[i] = out[i] >= o.threshold ? (o.binarize ? 255 : out[i]) : 0;
    return 0;
}

/* ---- scripted sources --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    int kind; /* 0 dark, 1 flat, 2 blob */
    int v, x, y, r, dx, dy;
    uint64_t frames;
} SynthState;
static SynthState synth;
static int synth_frame(void *ctx, uint8_t *out, uint64_t *ts) {
    SynthState *s = ctx;
    memset(out, s->kind == 1 ? s->v : 0, IR_W * IR_H);
    if (s->kind == 2) {
        for (int y = s->y - s->r; y <= s->y + s->r; y++)
            for (int x = s->x - s->r; x <= s->x + s->r; x++)
                if (x >= 0 && x < IR_W && y >= 0 && y < IR_H && (x - s->x) * (x - s->x) + (y - s->y) * (y - s->y) <= s->r * s->r) out[y * IR_W + x] = 255;
        s->x += s->dx;
        s->y += s->dy;
        if (s->x < 0 || s->x >= IR_W) s->dx = -s->dx, s->x += 2 * s->dx;
        if (s->y < 0 || s->y >= IR_H) s->dy = -s->dy, s->y += 2 * s->dy;
    }
    s->frames++;
    *ts = s->frames * 16666667ull; /* 60 Hz */
    return 1;
}
int irs_source_from_spec(const char *spec, IrFrameSource *out) {
    if (!spec || !*spec) return -1;
    memset(&synth, 0, sizeof synth);
    int a, b, r, dx = 0, dy = 0;
    if (!strcmp(spec, "dark")) synth.kind = 0;
    else if (sscanf(spec, "flat:%d", &a) == 1) synth.kind = 1, synth.v = a < 0 ? 0 : a > 255 ? 255 : a;
    else if (sscanf(spec, "blob:%d,%d,%d,%d,%d", &a, &b, &r, &dx, &dy) >= 3) synth = (SynthState){.kind = 2, .x = a, .y = b, .r = r, .dx = dx, .dy = dy};
    else return -1;
    *out = (IrFrameSource){.ctx = &synth, .name = "scripted", .get_frame = synth_frame};
    return 0;
}
