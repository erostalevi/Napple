/* t_tess_xfb_layout.c - pins the hardware encodings the control-section readers rely on, against NVIDIA's public Maxwell-B class header
 * (open-gpu-doc, classes/3d/clb197.h; layout facts only). No Metal, no game data.
 *   NVB197_SET_TESSELLATION_PARAMETERS (0x0320): DOMAIN_TYPE 1:0 ISOLINE 0 / TRIANGLE 1 / QUAD 2; SPACING 5:4 INTEGER 0 / FRACTIONAL_ODD 1 / FRACTIONAL_EVEN 2;
 *     OUTPUT_PRIMITIVES 9:8 POINTS 0 / LINES 1 / TRIANGLES_CW 2 / TRIANGLES_CCW 3.
 *   NVB197_SET_STREAM_OUT_CONTROL_STRIDE(j) 0x0708 + 16 j (BYTES 31:0), _COMPONENT_COUNT(j) 0x0704 + 16 j (MAX 7:0),
 *   NVB197_SET_STREAM_OUT_LAYOUT_SELECT(i, j) 0x2800 + 128 i + 4 j: ATTRIBUTE_NUMBER00..03 = bits 7:0, 15:8, 23:16, 31:24 (four 8-bit attribute numbers a word). */
#include <stdio.h>
#include <string.h>

#include "shader/control.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void w32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static uint32_t r32(const uint8_t *p, size_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; }

static void tess_eval(uint8_t *c) {
    memset(c, 0, 0x880);
    w32(c, 0x00, 0x98761234); w32(c, 0x04, 1); w32(c, 0x08, 0xf); w32(c, 0x0c, 0x120); w32(c, 0x10, 0xb); w32(c, 0x14, 0x848);
    w32(c, 0x20, 0x850); w32(c, 0x6f0, 0x850); w32(c, 0x6f8, 0x5d0); w32(c, 0x700, 0x600); w32(c, 0x704, 0x600); w32(c, 0x708, 0x30); w32(c, 0x70c, 0x1f);
    w32(c, 0x714, 4);
}

int main(void) {
    uint8_t c[0x880];
    gfx_tess_params tp;
    /* 1. every (domain, spacing, output primitives) combination of the class header: the three fields decode independently */
    static const int dom_hw[3] = {0, 1, 2}, sp_hw[3] = {0, 1, 2};
    static const int dom_want[3] = {TESS_DOMAIN_ISOLINE, TESS_DOMAIN_TRIANGLE, TESS_DOMAIN_QUAD};
    static const int sp_want[3] = {TESS_SPACING_EQUAL, TESS_SPACING_FRACTIONAL_ODD, TESS_SPACING_FRACTIONAL_EVEN};
    for (int d = 0; d < 3; d++)
        for (int s = 0; s < 3; s++)
            for (int prim = 0; prim < 4; prim++) {
                tess_eval(c);
                w32(c, GLSLC_TESS_MODE_OFF, (uint32_t)dom_hw[d] | (uint32_t)sp_hw[s] << 4 | (uint32_t)prim << 8);
                int got = gfx_control_tess(c, sizeof c, &tp);
                /* POINTS has no equivalent (refused); LINES only with ISOLINE; triangles only with TRIANGLE / QUAD */
                int ok = prim != 0 && ((d == 0) == (prim == 1));
                CHECK(got == ok, "domain %d spacing %d prim %d: accepted=%d, expected %d", d, s, prim, got, ok);
                if (got && ok) {
                    CHECK((int)tp.domain == dom_want[d] && (int)tp.spacing == sp_want[s], "domain %d spacing %d decoded as %u %u", d, s, tp.domain, tp.spacing);
                    if (prim >= 2) CHECK(tp.winding == (prim == 2 ? TESS_WINDING_CW : TESS_WINDING_CCW), "prim %d winding %u", prim, tp.winding);
                }
            }
    /* reserved values of the 2-bit fields are not that word */
    tess_eval(c); w32(c, GLSLC_TESS_MODE_OFF, 3 | (3 << 8)); CHECK(!gfx_control_tess(c, sizeof c, &tp), "domain 3 refused");
    tess_eval(c); w32(c, GLSLC_TESS_MODE_OFF, 1 | (3 << 4) | (3 << 8)); CHECK(!gfx_control_tess(c, sizeof c, &tp), "spacing 3 refused");

    /* 2. layout select image: the control section's u8 locs[4][128] is the little-endian image of the 32 method words per buffer (word j at 0x2800 + 128 i + 4 j) */
    uint8_t locs[4][128]; memset(locs, 0, sizeof locs);
    static const uint8_t pos[6] = {0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21};
    memcpy(locs[0], pos, 6);
    for (int j = 0; j < 2; j++) {
        uint32_t word = (uint32_t)locs[0][4 * j] | (uint32_t)locs[0][4 * j + 1] << 8 | (uint32_t)locs[0][4 * j + 2] << 16 | (uint32_t)locs[0][4 * j + 3] << 24;
        CHECK(r32(&locs[0][0], 4 * j) == word, "word %d: attribute numbers pack low byte first (%08x vs %08x)", j, r32(&locs[0][0], 4 * j), word);
    }
    CHECK(r32(&locs[0][0], 0) == 0x1f1e1d1cu, "word 0 = ATTRIBUTE_NUMBER00..03 = 1c 1d 1e 1f (%08x)", r32(&locs[0][0], 0));
    CHECK(sizeof locs[0] == 128 && (size_t)(&locs[1][0] - &locs[0][0]) == 128, "buffer stride of the table is 128 bytes (0x2800 + 128 i)");

    /* 3. stride / component count map to the layout; the count is the number of 32-bit components, the stride is in bytes (a multiple of 4) */
    uint16_t stride[4] = {24, 0, 0, 0}; uint8_t count[4] = {6, 0, 0, 0};
    gfx_xfb_layout xl;
    CHECK(gfx_xfb_layout_from_varyings(stride, count, locs, &xl) && xl.stride[0] == 24 && xl.count == 2, "position xyzw + generic 0 xy: 2 entries (%u)", xl.count);
    CHECK(xl.e[0].addr == 0x70 && xl.e[0].comps == 4 && xl.e[1].addr == 0x80 && xl.e[1].comps == 2, "attribute number n = address / 4: 0x1c -> 0x70, 0x20 -> 0x80");
    /* the section block: strides at +0x030, counts at +0x040, locs at +0x050 as one image */
    tess_eval(c); w32(c, 0x714, 0); /* vertex */
    w32(c, GLSLC_XFB_STRIDE_OFF, 24); w32(c, GLSLC_XFB_COUNT_OFF, 6); memcpy(c + GLSLC_XFB_LOCS_OFF, locs, sizeof locs);
    CHECK(gfx_control_xfb(c, sizeof c, &xl) && xl.count == 2, "block read from a vertex section");
    CHECK(GLSLC_XFB_LOCS_OFF == GLSLC_XFB_COUNT_OFF + 0x10 && GLSLC_XFB_COUNT_OFF == GLSLC_XFB_STRIDE_OFF + 0x10, "block order stride[4] count[4] locs");

    printf(fails ? "t_tess_xfb_layout: %d FAILED\n" : "t_tess_xfb_layout: ok\n", fails);
    return fails != 0;
}
