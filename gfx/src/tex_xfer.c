/* tex_xfer.c - CPU access to texture storage: copy between linear memory and the Tegra block-linear layout. */
#include <string.h>

#include "gfx_mtl.h"
#include "tex/napple_deswizzle.h"

static int is_layered(int target) { return target == 3 || target == 4 || target == 6 || target == 8 || target == 9; }

static int xfer(const texsnap *t, unsigned level, unsigned layer, const int32_t reg[6], void *lin, size_t row_stride, size_t img_stride, int to_linear) {
    if (!t || !t->valid || !t->mem) return -1;
    const napple_texdesc *d = &t->d;
    int bw, bh;
    int bpb = napple_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return -1;
    int32_t x0 = reg[0], y0 = reg[1], z0 = reg[2], w = reg[3], h = reg[4], dp = reg[5];
    if (w <= 0 || h <= 0 || dp <= 0 || x0 < 0 || y0 < 0 || z0 < 0) return -1;
    uint32_t bx0 = (uint32_t)x0 / (uint32_t)bw, by0 = (uint32_t)y0 / (uint32_t)bh;
    uint32_t nbx = ((uint32_t)w + (uint32_t)bw - 1) / (uint32_t)bw, nby = ((uint32_t)h + (uint32_t)bh - 1) / (uint32_t)bh;
    size_t row = (size_t)nbx * (size_t)bpb;
    if (!row_stride) row_stride = row;
    if (!img_stride) img_stride = row_stride * nby;
    uint8_t *lp = lin;
    int layered = is_layered(d->target);

    if (napple_tex_is_linear(d)) {
        if (level || layered || d->target == 2) return -1;
        size_t stride = napple_tex_linear_stride(d);
        for (uint32_t y = 0; y < nby; y++) {
            uint8_t *s = t->mem + (size_t)(by0 + y) * stride + (size_t)bx0 * bpb;
            if (to_linear) memcpy(lp + y * row_stride, s, row); else memcpy(s, lp + y * row_stride, row);
        }
        return 0;
    }
    for (int k = 0; k < dp; k++) {
        unsigned lay = layered ? layer + (unsigned)z0 + (unsigned)k : layer;
        uint32_t row_bytes, rows, slices, bpb2; uint64_t base; unsigned th, td;
        if (napple_deswizzle_geom(d, level, lay, &row_bytes, &rows, &slices, &base, &th, &td, &bpb2)) return -1;
        uint32_t z = layered ? 0 : (uint32_t)z0 + (uint32_t)k;
        if (!layered && d->target == 2 && z >= slices) return -1;
        if (by0 + nby > rows || (bx0 + nbx) * (uint32_t)bpb > row_bytes) return -1;
        for (uint32_t y = 0; y < nby; y++) {
            uint8_t *ls = lp + (size_t)k * img_stride + y * row_stride;
            for (uint32_t x = 0; x < nbx; x++) {
                uint8_t *p = t->mem + base + napple_bl_offset((bx0 + x) * (uint32_t)bpb, by0 + y, z, row_bytes, rows, th, td);
                if (to_linear) memcpy(ls + (size_t)x * bpb, p, (size_t)bpb); else memcpy(p, ls + (size_t)x * bpb, (size_t)bpb);
            }
        }
    }
    return 0;
}

int tex_write_texels(const texsnap *t, unsigned level, unsigned layer, const int32_t region[6], const void *src, size_t row_stride, size_t img_stride) {
    if (!t || !t->valid) return -1;
    napple_gfx_guest_write_prepare(t->mem, (size_t)t->size);
    return xfer(t, level, layer, region, (void *)src, row_stride, img_stride, 0);
}
int tex_read_texels(const texsnap *t, unsigned level, unsigned layer, const int32_t region[6], void *dst, size_t row_stride, size_t img_stride) {
    return xfer(t, level, layer, region, dst, row_stride, img_stride, 1);
}
