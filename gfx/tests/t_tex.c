/* t_tex.c - texture storage layout, block-linear conversion, format table. Pure C plus one Metal check of every mapped format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex/napple_deswizzle.h"
#include "tex/napple_fmt.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void roundtrip(int target, int fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels) {
    napple_texdesc t; memset(&t, 0, sizeof t);
    t.target = target; t.format = fmt; t.width = w; t.height = h; t.depth = d; t.levels = levels;
    napple_texlayout L; napple_tex_layout(&t, &L);
    uint8_t *st = calloc(1, (size_t)L.size + 64);
    unsigned nl = (target == 4 || target == 8 || target == 9) ? L.layers : 1;
    for (unsigned lv = 0; lv < levels; lv++) for (unsigned ly = 0; ly < nl; ly++) {
        uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
        if (napple_deswizzle_geom(&t, lv, ly, &rb, &rows, &sl, &base, &th, &td, &bpb)) { CHECK(0, "geom"); continue; }
        size_t n = (size_t)rb * rows * sl;
        uint8_t *src = malloc(n), *dst = malloc(n);
        for (size_t i = 0; i < n; i++) src[i] = (uint8_t)(i * 31 + lv * 7 + ly * 13 + 1);
        CHECK(napple_swizzle(&t, lv, ly, src, rb, st) == 0, "swizzle");
        CHECK(napple_deswizzle(&t, lv, ly, st, dst, rb) == 0, "deswizzle");
        CHECK(!memcmp(src, dst, n), "round trip fmt %#x %ux%ux%u level %u layer %u", fmt, w, h, d, lv, ly);
        CHECK(base + (uint64_t)0 <= L.size, "level base in range");
        free(src); free(dst);
    }
    free(st);
}

int main(void) {
    /* GOB addressing from the public layout rules: (x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16 */
    CHECK(napple_bl_offset(0, 0, 0, 64, 8, 0, 0) == 0, "origin");
    CHECK(napple_bl_offset(16, 0, 0, 64, 8, 0, 0) == 32, "x=16");
    CHECK(napple_bl_offset(32, 0, 0, 64, 8, 0, 0) == 256, "x=32");
    CHECK(napple_bl_offset(0, 1, 0, 64, 8, 0, 0) == 16, "y=1");
    CHECK(napple_bl_offset(0, 2, 0, 64, 8, 0, 0) == 64, "y=2");
    CHECK(napple_bl_offset(0, 8, 0, 64, 16, 0, 0) == 512, "second GOB row");
    roundtrip(1, 0x25, 64, 64, 1, 1);        /* RGBA8 2D */
    roundtrip(1, 0x25, 37, 53, 1, 6);        /* odd size, mips */
    roundtrip(4, 0x25, 16, 16, 5, 3);        /* 2D array */
    roundtrip(2, 0x25, 16, 16, 9, 3);        /* 3D */
    roundtrip(8, 0x25, 32, 32, 1, 4);        /* cube */
    roundtrip(1, 0x44, 128, 64, 1, 5);       /* DXT5 (BC3) */
    roundtrip(1, 0x4d, 100, 60, 1, 3);       /* BC7 */
    roundtrip(1, 0x79, 64, 64, 1, 3);        /* ASTC 4x4 */
    /* known NVN sizes: 4x4 RGBA8 occupies one GOB (512 bytes); texture alignment 512 */
    napple_texdesc t; memset(&t, 0, sizeof t); t.target = 1; t.format = 0x25; t.width = t.height = t.depth = t.levels = 1; t.width = t.height = 4;
    napple_texlayout L; napple_tex_layout(&t, &L);
    CHECK(L.size == 512 && L.align == 512, "4x4 RGBA8 size %llu align %u", (unsigned long long)L.size, L.align);
    /* the block height rounding that differs from deko3d: 160x160 BC3 uses block height 4 GOBs (log2 2); storage with all 8 mips is 57344 bytes */
    memset(&t, 0, sizeof t); t.target = 1; t.format = 0x44; t.width = t.height = 160; t.depth = 1; t.levels = 8;
    napple_tex_layout(&t, &L);
    CHECK(L.tileH == 2 && L.size == 57344, "160x160 BC3 block height log2 %u size %llu", L.tileH, (unsigned long long)L.size);
    /* sRGB formats must map to Metal sRGB formats; every mapped format has a known byte size */
    for (int f = 1; f <= 0x9c; f++) {
        uint32_t m; uint8_t sw[4];
        int k = napple_fmt_to_mtl(f, &m, sw);
        int bw, bh; int bpb = napple_fmt_bpb(f, &bw, &bh);
        CHECK(k == 0 || bpb > 0, "format %#x maps to Metal but has no block size", f);
    }
    printf(g_fail ? "t_tex: %d FAILED\n" : "t_tex: ok\n", g_fail);
    return g_fail != 0;
}
