#include "jpeg_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                   41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                   30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* ---- shared 8x8 transforms (float, separable) ---- */
static float dct_c[8][8]; /* dct_c[u][x] = a(u) cos((2x+1) u pi / 16) / 2 */
static void dct_init(void) {
    static int done;
    if (done) return;
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++) dct_c[u][x] = (float)((u ? 1.0 : 0.70710678118654752) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0) * 0.5);
    done = 1;
}
static void idct8x8(const float in[64], float out[64]) {
    float t[64];
    for (int y = 0; y < 8; y++) /* rows: over u */
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += dct_c[u][x] * in[y * 8 + u];
            t[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += dct_c[v][y] * t[v * 8 + x];
            out[y * 8 + x] = s;
        }
}
static void fdct8x8(const float in[64], float out[64]) {
    float t[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) s += dct_c[u][x] * in[y * 8 + x];
            t[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) s += dct_c[v][y] * t[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

/* ---- decoder ---- */
typedef struct {
    uint8_t bits[17], vals[256];
    int present;
    int mincode[17], maxcode[18], valptr[17];
    int16_t look[512]; /* 9-bit lookahead: (len << 8) | value, 0 = longer code */
} Huff;
typedef struct {
    int id, h, v, tq, dc_t, ac_t, pred;
    int bw, bh; /* blocks per line / column covering the whole MCU grid */
    uint8_t *plane;
    int pw; /* plane width in samples */
} Comp;
typedef struct {
    const uint8_t *d;
    size_t n, pos;
    uint32_t acc;
    int nbits, hit_marker, truncated;
} Bits;

static int rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static void huff_build(Huff *h) {
    int code = 0, k = 0;
    memset(h->look, 0, sizeof h->look);
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        for (int i = 0; i < h->bits[l]; i++, k++, code++) {
            if (l <= 9) {
                int base = code << (9 - l);
                for (int j = 0; j < (1 << (9 - l)); j++) h->look[base + j] = (int16_t)((l << 8) | h->vals[k]);
            }
        }
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
}
static void bits_fill(Bits *b) {
    while (b->nbits <= 24) {
        int byte = 0;
        if (!b->hit_marker && b->pos < b->n) {
            byte = b->d[b->pos];
            if (byte == 0xff) {
                int nx = b->pos + 1 < b->n ? b->d[b->pos + 1] : 0xd9;
                if (nx == 0) b->pos += 2;
                else {
                    b->hit_marker = 1;
                    byte = 0;
                }
            } else b->pos++;
        } else {
            if (!b->hit_marker && b->pos >= b->n) b->truncated = 1;
            b->hit_marker = 1;
        }
        b->acc |= (uint32_t)byte << (24 - b->nbits);
        b->nbits += 8;
    }
}
static int bits_get(Bits *b, int n) {
    if (!n) return 0;
    bits_fill(b);
    int v = (int)(b->acc >> (32 - n));
    b->acc <<= n;
    b->nbits -= n;
    return v;
}
static int huff_decode(Bits *b, const Huff *h) {
    bits_fill(b);
    int e = h->look[b->acc >> 23];
    if (e) {
        int l = e >> 8;
        b->acc <<= l;
        b->nbits -= l;
        return e & 0xff;
    }
    int code = (int)(b->acc >> 23), l = 9; /* longer than 9 bits: walk the canonical ranges */
    uint32_t a = b->acc << 9;
    for (;;) {
        l++;
        if (l > 16) return -1;
        code = (code << 1) | (int)(a >> 31);
        a <<= 1;
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
            b->acc <<= l;
            b->nbits -= l;
            return h->vals[h->valptr[l] + code - h->mincode[l]];
        }
    }
}
static int extend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

typedef struct {
    int width, height, ncomp, progressive, unsupported;
    int h[3], v[3], id[3], tq[3];
    size_t sof_pos;
} Frame;

/* Walks the markers up to the first SOS (or EOI). Fills quant/huffman tables when the pointers are given. */
static int parse_headers(const uint8_t *d, size_t n, Frame *f, uint16_t q[4][64], Huff hdc[4], Huff hac[4], int *ri, size_t *scan_pos, Comp *sc, int *nsc) {
    memset(f, 0, sizeof *f);
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return JPEGCORE_BAD;
    size_t p = 2;
    int have_sof = 0;
    for (;;) {
        while (p < n && d[p] != 0xff) p++;
        while (p < n && d[p] == 0xff) p++;
        if (p >= n) return JPEGCORE_TRUNCATED;
        int m = d[p++];
        if (m == 0xd9) return have_sof ? JPEGCORE_TRUNCATED : JPEGCORE_BAD;
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7) || m == 0) continue;
        if (p + 2 > n) return JPEGCORE_TRUNCATED;
        int len = rd16(d + p);
        if (len < 2 || p + (size_t)len > n) return JPEGCORE_TRUNCATED;
        const uint8_t *s = d + p + 2;
        int sl = len - 2;
        if (m == 0xc0 || m == 0xc1 || m == 0xc2) {
            if (have_sof) return JPEGCORE_BAD;
            if (sl < 6 || s[0] != 8) return s[0] != 8 && sl >= 1 ? JPEGCORE_UNSUPPORTED : JPEGCORE_BAD;
            f->height = rd16(s + 1);
            f->width = rd16(s + 3);
            f->ncomp = s[5];
            f->progressive = m == 0xc2;
            if (!f->width || !f->height || f->width > JPEGCORE_MAX_DIM || f->height > JPEGCORE_MAX_DIM) return JPEGCORE_BAD;
            if ((f->ncomp != 1 && f->ncomp != 3) || sl < 6 + 3 * f->ncomp) return f->ncomp == 4 ? JPEGCORE_UNSUPPORTED : JPEGCORE_BAD;
            for (int i = 0; i < f->ncomp; i++) {
                f->id[i] = s[6 + 3 * i];
                f->h[i] = s[7 + 3 * i] >> 4;
                f->v[i] = s[7 + 3 * i] & 15;
                f->tq[i] = s[8 + 3 * i];
                if (f->h[i] < 1 || f->h[i] > 2 || f->v[i] < 1 || f->v[i] > 2 || f->tq[i] > 3) return JPEGCORE_UNSUPPORTED;
            }
            have_sof = 1;
            f->sof_pos = p;
            if (f->progressive) return JPEGCORE_UNSUPPORTED;
        } else if ((m >= 0xc3 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc)) {
            return JPEGCORE_UNSUPPORTED; /* lossless, differential, arithmetic */
        } else if (m == 0xdb && q) {
            while (sl >= 1) {
                int pq = s[0] >> 4, tq = s[0] & 15, need = pq ? 129 : 65;
                if (tq > 3 || sl < need) return JPEGCORE_BAD;
                for (int i = 0; i < 64; i++) q[tq][zigzag[i]] = pq ? (uint16_t)rd16(s + 1 + 2 * i) : s[1 + i];
                s += need;
                sl -= need;
            }
        } else if (m == 0xc4 && hdc) {
            while (sl >= 17) {
                int tc = s[0] >> 4, th = s[0] & 15, total = 0;
                if (tc > 1 || th > 3) return JPEGCORE_BAD;
                Huff *h = tc ? &hac[th] : &hdc[th];
                memset(h, 0, sizeof *h);
                for (int i = 1; i <= 16; i++) total += h->bits[i] = s[i];
                if (total > 256 || sl < 17 + total) return JPEGCORE_BAD;
                memcpy(h->vals, s + 17, (size_t)total);
                h->present = 1;
                huff_build(h);
                s += 17 + total;
                sl -= 17 + total;
            }
        } else if (m == 0xdd && ri) {
            if (sl < 2) return JPEGCORE_BAD;
            *ri = rd16(s);
        } else if (m == 0xda) {
            if (!have_sof) return JPEGCORE_BAD;
            if (sl < 1 || s[0] != f->ncomp || sl < 1 + 2 * s[0] + 3) return JPEGCORE_UNSUPPORTED; /* non-interleaved scans */
            if (sc) {
                for (int i = 0; i < f->ncomp; i++) {
                    int id = s[1 + 2 * i], k;
                    for (k = 0; k < f->ncomp && f->id[k] != id; k++) {}
                    if (k == f->ncomp) return JPEGCORE_BAD;
                    sc[i].dc_t = s[2 + 2 * i] >> 4;
                    sc[i].ac_t = s[2 + 2 * i] & 15;
                    if (sc[i].dc_t > 3 || sc[i].ac_t > 3) return JPEGCORE_BAD;
                    sc[i].id = id;
                }
                *nsc = f->ncomp;
            }
            if (scan_pos) *scan_pos = p + (size_t)len;
            return JPEGCORE_OK;
        }
        p += (size_t)len;
    }
}
int napjpeg_info(const uint8_t *data, size_t size, JpegInfo *info) {
    Frame f;
    int r = parse_headers(data, size, &f, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (f.width && info) {
        info->width = f.width;
        info->height = f.height;
        info->ncomp = f.ncomp;
        info->progressive = f.progressive;
        for (int i = 0; i < f.ncomp; i++) {
            info->h[i] = f.h[i];
            info->v[i] = f.v[i];
        }
    }
    return r == JPEGCORE_TRUNCATED && f.width ? JPEGCORE_OK : r; /* the header alone was asked for */
}
static uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

uint8_t *napjpeg_decode_rgba(const uint8_t *d, size_t n, int *width, int *height, int *err) {
    int dummy;
    if (!err) err = &dummy;
    *err = JPEGCORE_OK;
    dct_init();
    uint16_t q[4][64];
    memset(q, 0, sizeof q);
    Huff *hdc = calloc(8, sizeof(Huff)), *hac = hdc + 4;
    Frame f;
    Comp c[3], fc[3];
    memset(c, 0, sizeof c);
    memset(fc, 0, sizeof fc);
    int ri = 0, nsc = 0;
    size_t scan = 0;
    uint8_t *rgba = NULL;
    if (!hdc) {
        *err = JPEGCORE_NOMEM;
        return NULL;
    }
    int r = parse_headers(d, n, &f, q, hdc, hac, &ri, &scan, c, &nsc);
    if (r != JPEGCORE_OK) {
        *err = r;
        goto out;
    }
    int hmax = 1, vmax = 1;
    for (int i = 0; i < f.ncomp; i++) {
        if (f.h[i] > hmax) hmax = f.h[i];
        if (f.v[i] > vmax) vmax = f.v[i];
    }
    int mcux = (f.width + 8 * hmax - 1) / (8 * hmax), mcuy = (f.height + 8 * vmax - 1) / (8 * vmax);
    /* map scan components onto frame components (they come in the same order for interleaved scans in practice, but resolve by id) */
    for (int i = 0; i < f.ncomp; i++) {
        for (int k = 0; k < nsc; k++)
            if (c[k].id == f.id[i]) {
                fc[i].dc_t = c[k].dc_t;
                fc[i].ac_t = c[k].ac_t;
            }
        fc[i].id = f.id[i];
        fc[i].h = f.h[i];
        fc[i].v = f.v[i];
        fc[i].tq = f.tq[i];
        fc[i].bw = mcux * f.h[i];
        fc[i].bh = mcuy * f.v[i];
        fc[i].pw = fc[i].bw * 8;
        fc[i].plane = malloc((size_t)fc[i].pw * fc[i].bh * 8);
        if (!fc[i].plane) {
            *err = JPEGCORE_NOMEM;
            goto out;
        }
        memset(fc[i].plane, 128, (size_t)fc[i].pw * fc[i].bh * 8);
    }
    Bits b = {.d = d, .n = n, .pos = scan};
    int restart_left = ri, truncated = 0;
    for (int my = 0; my < mcuy && !truncated; my++)
        for (int mx = 0; mx < mcux; mx++) {
            if (ri && restart_left == 0) { /* RSTn: realign on the marker, reset predictors */
                b.acc = 0;
                b.nbits = 0;
                size_t p = b.pos;
                while (p + 1 < n && !(d[p] == 0xff && d[p + 1] >= 0xd0 && d[p + 1] <= 0xd7)) p++;
                if (p + 1 >= n) {
                    truncated = 1;
                    break;
                }
                b.pos = p + 2;
                b.hit_marker = 0;
                for (int i = 0; i < f.ncomp; i++) fc[i].pred = 0;
                restart_left = ri;
            }
            for (int i = 0; i < f.ncomp; i++) {
                const Huff *dc = &hdc[fc[i].dc_t], *ac = &hac[fc[i].ac_t];
                if (!dc->present || !ac->present) {
                    *err = JPEGCORE_BAD;
                    goto out;
                }
                for (int by = 0; by < fc[i].v; by++)
                    for (int bx = 0; bx < fc[i].h; bx++) {
                        float coef[64] = {0}, px[64];
                        int s = huff_decode(&b, dc);
                        if (s < 0 || s > 15) {
                            *err = JPEGCORE_BAD;
                            goto out;
                        }
                        fc[i].pred += extend(bits_get(&b, s), s);
                        coef[0] = (float)(fc[i].pred * q[fc[i].tq][0]);
                        for (int k = 1; k < 64;) {
                            int rs = huff_decode(&b, ac);
                            if (rs < 0) {
                                *err = JPEGCORE_BAD;
                                goto out;
                            }
                            int run = rs >> 4, sz = rs & 15;
                            if (!sz) {
                                if (run != 15) break;
                                k += 16;
                                continue;
                            }
                            k += run;
                            if (k > 63) {
                                *err = JPEGCORE_BAD;
                                goto out;
                            }
                            coef[zigzag[k]] = (float)(extend(bits_get(&b, sz), sz) * q[fc[i].tq][zigzag[k]]);
                            k++;
                        }
                        idct8x8(coef, px);
                        int ox = (mx * fc[i].h + bx) * 8, oy = (my * fc[i].v + by) * 8;
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++) fc[i].plane[(size_t)(oy + y) * fc[i].pw + ox + x] = clamp8(px[y * 8 + x] + 128.0f);
                    }
            }
            if (b.truncated) truncated = 1;
            restart_left--;
        }
    if (b.truncated && !*err) *err = JPEGCORE_TRUNCATED; /* what decoded is returned, the rest stays grey */
    rgba = malloc((size_t)f.width * f.height * 4);
    if (!rgba) {
        *err = JPEGCORE_NOMEM;
        goto out;
    }
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++) {
            uint8_t *o = rgba + ((size_t)y * f.width + x) * 4;
            int sx0 = x * fc[0].h / hmax, sy0 = y * fc[0].v / vmax;
            float Y = fc[0].plane[(size_t)sy0 * fc[0].pw + sx0];
            if (f.ncomp == 1) {
                o[0] = o[1] = o[2] = (uint8_t)Y;
            } else {
                float cb = fc[1].plane[(size_t)(y * fc[1].v / vmax) * fc[1].pw + x * fc[1].h / hmax] - 128.0f;
                float cr = fc[2].plane[(size_t)(y * fc[2].v / vmax) * fc[2].pw + x * fc[2].h / hmax] - 128.0f;
                o[0] = clamp8(Y + 1.402f * cr);
                o[1] = clamp8(Y - 0.344136f * cb - 0.714136f * cr);
                o[2] = clamp8(Y + 1.772f * cb);
            }
            o[3] = 255;
        }
    *width = f.width;
    *height = f.height;
out:
    for (int i = 0; i < 3; i++) free(fc[i].plane);
    free(hdc);
    return rgba;
}

uint8_t *napjpeg_scale_rgba(const uint8_t *rgba, int w, int h, int dw, int dh) {
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return NULL;
    uint8_t *o = malloc((size_t)dw * dh * 4);
    if (!o) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * h / dh), y1 = (int)(((int64_t)(y + 1) * h + dh - 1) / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * w / dw), x1 = (int)(((int64_t)(x + 1) * w + dw - 1) / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned s[4] = {0, 0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, cnt++)
                    for (int k = 0; k < 4; k++) s[k] += rgba[((size_t)yy * w + xx) * 4 + k];
            for (int k = 0; k < 4; k++) o[((size_t)y * dw + x) * 4 + k] = (uint8_t)((s[k] + cnt / 2) / cnt);
        }
    }
    return o;
}

/* ---- encoder ---- */
static const uint8_t qlum[64] = {16, 11,  10,  16, 24, 40, 51, 61, 12,  12,  14,  19,  26, 58, 60, 55,  14,  13,  16,  24, 40, 57,
                                 69, 56,  14,  17, 22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103, 77,  24,  35, 55, 64,
                                 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98,  112, 100, 103, 99};
static const uint8_t qchr[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
static const uint8_t dc_l_bits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_c_bits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_l_bits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_l_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_c_bits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_c_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} HuffEnc;
static void henc_build(HuffEnc *e, const uint8_t bits[17], const uint8_t *vals) {
    int code = 0, k = 0;
    memset(e, 0, sizeof *e);
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++, k++, code++) {
            e->code[vals[k]] = (uint16_t)code;
            e->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}
typedef struct {
    uint8_t *buf;
    size_t n, cap;
    uint32_t acc;
    int nb, fail;
} Out;
static void out_byte(Out *o, int v) {
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 4096;
        uint8_t *nb = realloc(o->buf, nc);
        if (!nb) {
            o->fail = 1;
            return;
        }
        o->buf = nb;
        o->cap = nc;
    }
    o->buf[o->n++] = (uint8_t)v;
}
static void out_bits(Out *o, unsigned code, int len) {
    o->acc = (o->acc << len) | (code & ((1u << len) - 1));
    o->nb += len;
    while (o->nb >= 8) {
        int v = (o->acc >> (o->nb - 8)) & 0xff;
        out_byte(o, v);
        if (v == 0xff) out_byte(o, 0);
        o->nb -= 8;
    }
}
static void out_marker(Out *o, int m, const uint8_t *payload, int len) {
    out_byte(o, 0xff);
    out_byte(o, m);
    if (len >= 0) {
        out_byte(o, (len + 2) >> 8);
        out_byte(o, (len + 2) & 255);
        for (int i = 0; i < len; i++) out_byte(o, payload[i]);
    }
}
static int bitsize(int v) {
    int a = v < 0 ? -v : v, n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    return n;
}
static void encode_block(Out *o, const float px[64], const uint16_t *qt, int *pred, const HuffEnc *dc, const HuffEnc *ac) {
    float co[64];
    int qv[64];
    fdct8x8(px, co);
    for (int i = 0; i < 64; i++) {
        float v = co[i] / (float)qt[i];
        qv[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    int diff = qv[0] - *pred;
    *pred = qv[0];
    int s = bitsize(diff);
    out_bits(o, dc->code[s], dc->len[s]);
    if (s) out_bits(o, (unsigned)(diff < 0 ? diff - 1 : diff), s);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = qv[zigzag[k]];
        if (!v) {
            run++;
            continue;
        }
        while (run > 15) {
            out_bits(o, ac->code[0xf0], ac->len[0xf0]);
            run -= 16;
        }
        int sz = bitsize(v), rs = (run << 4) | sz;
        out_bits(o, ac->code[rs], ac->len[rs]);
        out_bits(o, (unsigned)(v < 0 ? v - 1 : v), sz);
        run = 0;
    }
    if (run) out_bits(o, ac->code[0], ac->len[0]);
}
uint8_t *napjpeg_encode_rgba(const uint8_t *rgba, int w, int h, int stride, int quality, int sub, size_t *out_size) {
    if (!rgba || w <= 0 || h <= 0 || w > JPEGCORE_MAX_DIM || h > JPEGCORE_MAX_DIM || stride < w * 4) return NULL;
    dct_init();
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint16_t ql[64], qc[64];
    uint8_t zl[64], zc[64];
    for (int i = 0; i < 64; i++) {
        int a = (qlum[i] * scale + 50) / 100, b = (qchr[i] * scale + 50) / 100;
        ql[i] = (uint16_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[i] = (uint16_t)(b < 1 ? 1 : b > 255 ? 255 : b);
    }
    for (int i = 0; i < 64; i++) {
        zl[i] = (uint8_t)ql[zigzag[i]];
        zc[i] = (uint8_t)qc[zigzag[i]];
    }
    HuffEnc dcl, dcc, acl, acc;
    henc_build(&dcl, dc_l_bits, dc_vals);
    henc_build(&dcc, dc_c_bits, dc_vals);
    henc_build(&acl, ac_l_bits, ac_l_vals);
    henc_build(&acc, ac_c_bits, ac_c_vals);
    Out o = {0};
    out_marker(&o, 0xd8, NULL, -1);
    static const uint8_t jfif[14] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    out_marker(&o, 0xe0, jfif, 14);
    uint8_t t[65];
    t[0] = 0;
    memcpy(t + 1, zl, 64);
    out_marker(&o, 0xdb, t, 65);
    t[0] = 1;
    memcpy(t + 1, zc, 64);
    out_marker(&o, 0xdb, t, 65);
    uint8_t sof[15] = {8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3, 1, (uint8_t)(sub ? 0x22 : 0x11), 0, 2, 0x11, 1, 3, 0x11, 1};
    out_marker(&o, 0xc0, sof, 15);
    uint8_t dht[17 + 162];
    const uint8_t *tb[4] = {dc_l_bits, ac_l_bits, dc_c_bits, ac_c_bits};
    const uint8_t *tv[4] = {dc_vals, ac_l_vals, dc_vals, ac_c_vals};
    int tn[4] = {12, 162, 12, 162}, tid[4] = {0x00, 0x10, 0x01, 0x11};
    for (int i = 0; i < 4; i++) {
        dht[0] = (uint8_t)tid[i];
        for (int l = 1; l <= 16; l++) dht[l] = tb[i][l];
        memcpy(dht + 17, tv[i], (size_t)tn[i]);
        out_marker(&o, 0xc4, dht, 17 + tn[i]);
    }
    static const uint8_t sos[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    out_marker(&o, 0xda, sos, 10);
    int mw = sub ? 16 : 8, mh = sub ? 16 : 8;
    int pred[3] = {0, 0, 0};
    for (int my = 0; my < (h + mh - 1) / mh; my++)
        for (int mx = 0; mx < (w + mw - 1) / mw; mx++) {
            float Y[4][64], Cb[64], Cr[64], cbf[256], crf[256];
            int nb = sub ? 4 : 1;
            for (int py = 0; py < mh; py++)
                for (int pxl = 0; pxl < mw; pxl++) {
                    int x = mx * mw + pxl, y = my * mh + py;
                    if (x >= w) x = w - 1;
                    if (y >= h) y = h - 1;
                    const uint8_t *p = rgba + (size_t)y * stride + (size_t)x * 4;
                    float yy = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                    cbf[py * mw + pxl] = -0.168736f * p[0] - 0.331264f * p[1] + 0.5f * p[2];
                    crf[py * mw + pxl] = 0.5f * p[0] - 0.418688f * p[1] - 0.081312f * p[2];
                    int blk = sub ? (py / 8) * 2 + pxl / 8 : 0;
                    Y[blk][(py % 8) * 8 + pxl % 8] = yy - 128.0f;
                }
            for (int by = 0; by < 8; by++)
                for (int bx = 0; bx < 8; bx++) {
                    if (sub) {
                        int i0 = by * 2 * mw + bx * 2;
                        Cb[by * 8 + bx] = (cbf[i0] + cbf[i0 + 1] + cbf[i0 + mw] + cbf[i0 + mw + 1]) * 0.25f;
                        Cr[by * 8 + bx] = (crf[i0] + crf[i0 + 1] + crf[i0 + mw] + crf[i0 + mw + 1]) * 0.25f;
                    } else {
                        Cb[by * 8 + bx] = cbf[by * 8 + bx];
                        Cr[by * 8 + bx] = crf[by * 8 + bx];
                    }
                }
            for (int i = 0; i < nb; i++) encode_block(&o, Y[i], ql, &pred[0], &dcl, &acl);
            encode_block(&o, Cb, qc, &pred[1], &dcc, &acc);
            encode_block(&o, Cr, qc, &pred[2], &dcc, &acc);
        }
    if (o.nb) out_bits(&o, 0x7f, 8 - o.nb);
    out_marker(&o, 0xd9, NULL, -1);
    if (o.fail) {
        free(o.buf);
        return NULL;
    }
    *out_size = o.n;
    return o.buf;
}
