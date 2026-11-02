/* t_control.c - the GLSLC control section reader (src/shader/control.c): header fields of a real vertex output (Link's Awakening's StartUpRegion.arc,
 * reproduced word for word), the "not present" answers on it, the varying-table conversion, a synthetic section carrying a transform feedback block
 * and a tessellation word at the candidate offsets, and two real geometry sections (SMO, ACNH; 2026-11-01). No Metal. */
#include <stdio.h>
#include <string.h>

#include "shader/control.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void w32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }

/* The non-zero words of LAS's vertex control section 02e03a17 (length 0x850) */
static void las_vertex(uint8_t *c) {
    memset(c, 0, 0x880);
    w32(c, 0x00, 0x98761234); w32(c, 0x04, 1); w32(c, 0x08, 0xf); w32(c, 0x0c, 0x120); w32(c, 0x10, 0xb); w32(c, 0x14, 0x848);
    w32(c, 0x20, 0x850);
    w32(c, 0x6f0, 0x850); w32(c, 0x6f8, 0x5d0); w32(c, 0x700, 0x600); w32(c, 0x704, 0x600); w32(c, 0x708, 0x30); w32(c, 0x70c, 0x1f);
    w32(c, 0x788, 1); w32(c, 0x790, 0x00220014); w32(c, 0x794, 8);
    w32(c, 0x7c8, 0x850); w32(c, 0x7e8, 0x850); w32(c, 0x7f4, 0xc); w32(c, 0x830, 0x00010000);
}

/* The non-zero words of two real geometry sections (NAPPLE_GFX_CTL_DUMP, 2026-11-01): Super Mario Odyssey's alThunderShader line program fccb3f42c7f70682
 * (GLSLC version 9, length 0x7d1) and Animal Crossing's keyboard ocean program 8c8302af6b2a4b88 (version 0xf, 0x850; +0x7d0..+0x7e4 its digest) */
static void smo_geometry(uint8_t *c) {
    memset(c, 0, 0x880);
    w32(c, 0x00, 0x98761234); w32(c, 0x04, 1); w32(c, 0x08, 9); w32(c, 0x0c, 0x120); w32(c, 0x10, 0xb); w32(c, 0x14, 0x7d0);
    w32(c, 0x20, 0x7d1);
    w32(c, 0x6f0, 0x7d1); w32(c, 0x6f8, 0x250); w32(c, 0x700, 0x300); w32(c, 0x704, 0x300); w32(c, 0x708, 0x30); w32(c, 0x70c, 0xc); w32(c, 0x714, 2);
    w32(c, 0x790, 0x0022001c); w32(c, 0x7c8, 0x7d1);
}
static void acnh_geometry(uint8_t *c) {
    memset(c, 0, 0x880);
    w32(c, 0x00, 0x98761234); w32(c, 0x04, 1); w32(c, 0x08, 0xf); w32(c, 0x0c, 0x120); w32(c, 0x10, 0xb); w32(c, 0x14, 0x848);
    w32(c, 0x20, 0x850);
    w32(c, 0x6f0, 0x850); w32(c, 0x6f8, 0x890); w32(c, 0x6fc, 8); w32(c, 0x700, 0x900); w32(c, 0x704, 0xa00); w32(c, 0x708, 0x30); w32(c, 0x70c, 0x20);
    w32(c, 0x714, 2); w32(c, 0x788, 1); w32(c, 0x790, 0x00220014); w32(c, 0x794, 8); w32(c, 0x7c8, 0x850);
    w32(c, 0x7d0, 0x453a0109); w32(c, 0x7d4, 0x03f8a400); w32(c, 0x7d8, 0xef942178); w32(c, 0x7dc, 0xc506e696); w32(c, 0x7e0, 0x51c22772); w32(c, 0x7e4, 0x840ba5d8);
    w32(c, 0x7e8, 0x850); w32(c, 0x7f4, 0xc); w32(c, 0x828, 4);
}

int main(void) {
    uint8_t c[0x880];
    gfx_control_info ci;
    gfx_xfb_layout xl;
    gfx_tess_params tp;
    /* 1. real vertex section */
    las_vertex(c);
    CHECK(gfx_control_parse(c, sizeof c, &ci), "LAS vertex control parses");
    CHECK(ci.length == 0x850 && ci.code_len == 0x600 && ci.const_off == 0x600 && ci.const_size == 0 && ci.stage == 0, "fields: len %#x code %#x const %#x/%#x stage %u",
          ci.length, ci.code_len, ci.const_off, ci.const_size, ci.stage);
    CHECK(!gfx_control_xfb(c, sizeof c, &xl), "a plain vertex section carries no transform feedback layout");
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "a vertex section carries no tessellation parameters");
    /* 2. rejects */
    w32(c, 0x00, 0);
    CHECK(!gfx_control_parse(c, sizeof c, &ci), "bad magic rejected");
    las_vertex(c);
    w32(c, 0x714, 7);
    CHECK(!gfx_control_parse(c, sizeof c, &ci), "stage 7 rejected");
    las_vertex(c);
    w32(c, 0x20, 0x123);
    CHECK(gfx_control_length(c) == 0x880, "implausible length word falls back to 0x880 (got %#x)", gfx_control_length(c));
    CHECK(!gfx_control_parse(c, 0x700, &ci), "a buffer shorter than the section is refused");
    las_vertex(c);
    w32(c, 0x20, 0x7d1); /* SMO / MK8D sections are 0x7d1 bytes: not a multiple of 4, still the real length */
    CHECK(gfx_control_length(c) == 0x7d1 && gfx_control_parse(c, 0x7d1, &ci) && ci.length == 0x7d1, "odd section length 0x7d1 kept (got %#x)", gfx_control_length(c));
    w32(c, 0x20, 0x11d0); /* the longest seen (LAS, a section with embedded assembly text) */
    CHECK(gfx_control_length(c) == 0x11d0, "section length 0x11d0 kept (got %#x)", gfx_control_length(c));
    /* 3. varying table conversion: buffer 0 = position xyzw + generic 0 xy, buffer 1 = skip, generic 1 z */
    uint16_t stride[4] = {24, 8, 0, 0};
    uint8_t count[4] = {6, 2, 0, 0}, locs[4][128];
    memset(locs, 0, sizeof locs);
    locs[0][0] = 0x1c; locs[0][1] = 0x1d; locs[0][2] = 0x1e; locs[0][3] = 0x1f; locs[0][4] = 0x20; locs[0][5] = 0x21;
    locs[1][0] = 0xff; locs[1][1] = 0x26;
    CHECK(gfx_xfb_layout_from_varyings(stride, count, locs, &xl), "varyings convert");
    CHECK(xl.count == 3 && xl.stride[0] == 24 && xl.stride[1] == 8, "3 entries, strides %u %u (count %u)", xl.stride[0], xl.stride[1], xl.count);
    CHECK(xl.e[0].addr == 0x70 && xl.e[0].comps == 4 && xl.e[0].buffer == 0 && xl.e[0].offset == 0, "entry 0 = position xyzw at 0 (addr %#x comps %u off %u)", xl.e[0].addr, xl.e[0].comps, xl.e[0].offset);
    CHECK(xl.e[1].addr == 0x80 && xl.e[1].comps == 2 && xl.e[1].buffer == 0 && xl.e[1].offset == 16, "entry 1 = generic 0 xy at 16 (addr %#x comps %u off %u)", xl.e[1].addr, xl.e[1].comps, xl.e[1].offset);
    CHECK(xl.e[2].addr == 0x98 && xl.e[2].comps == 1 && xl.e[2].buffer == 1 && xl.e[2].offset == 4, "entry 2 = generic 1 z in buffer 1 at 4 (addr %#x comps %u buf %u off %u)", xl.e[2].addr, xl.e[2].comps, xl.e[2].buffer, xl.e[2].offset);
    /* a run never crosses an attribute boundary: position w then generic 0 x are two entries */
    uint8_t count2[4] = {2, 0, 0, 0};
    uint8_t locs2[4][128]; memset(locs2, 0, sizeof locs2); locs2[0][0] = 0x1f; locs2[0][1] = 0x20;
    CHECK(gfx_xfb_layout_from_varyings(stride, count2, locs2, &xl) && xl.count == 2, "runs stop at attribute boundaries (count %u)", xl.count);
    uint16_t bad[4] = {6, 0, 0, 0};
    CHECK(!gfx_xfb_layout_from_varyings(bad, count2, locs2, &xl), "stride not a multiple of 4 refused");
    /* 4. synthetic section with the candidate transform feedback block */
    las_vertex(c);
    for (int k = 0; k < 4; k++) { w32(c, GLSLC_XFB_STRIDE_OFF + 4 * k, stride[k]); w32(c, GLSLC_XFB_COUNT_OFF + 4 * k, count[k]); }
    memcpy(c + GLSLC_XFB_LOCS_OFF, locs, sizeof locs);
    CHECK(gfx_control_xfb(c, sizeof c, &xl) && xl.count == 3 && xl.e[2].buffer == 1, "transform feedback block read from the section (count %u)", xl.count);
    c[GLSLC_XFB_LOCS_OFF + 100] = 0x33; /* garbage past the count: not that block */
    CHECK(!gfx_control_xfb(c, sizeof c, &xl), "an inconsistent block is not taken");
    las_vertex(c);
    w32(c, 0x714, 1);
    CHECK(!gfx_control_xfb(c, sizeof c, &xl), "a fragment section never carries a layout");
    /* 4b. the words that sections with embedded NV assembly text (!!NVvp5.0 / !!NVfp5.0; 18 of 20624 sections dumped from LAS, ACNH, SMO, b8) carry
     * inside the candidate block: not a transform feedback layout (those programs capture nothing), and the consistency checks must refuse it */
    las_vertex(c);
    w32(c, 0x34, 0x340); w32(c, 0x38, 0xffffff01);
    for (uint32_t o = 0x3c; o <= 0x54; o += 4) w32(c, o, 0xffffffff);
    c[0x58] = 0xff;
    CHECK(!gfx_control_xfb(c, sizeof c, &xl), "the assembly-text words at +0x034..+0x058 are not taken as a layout");
    /* 4c. +0x718 in fragment sections holds fragment state (1 or 0x10000 in every fragment dumped): the tess reader never looks at a fragment section */
    las_vertex(c);
    w32(c, 0x714, 1);
    w32(c, GLSLC_TESS_MODE_OFF, 1);
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "fragment state at +0x718 is not read as tessellation parameters");
    /* 5. synthetic tess eval section */
    las_vertex(c);
    w32(c, 0x714, 4);
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "zero mode word = not present");
    w32(c, GLSLC_TESS_MODE_OFF, 0x2 | (2 << 4) | (2 << 8)); /* NVB197_SET_TESSELLATION_PARAMETERS: OUTPUT_PRIMITIVES 9:8 = TRIANGLES_CW (2) */
    CHECK(gfx_control_tess(c, sizeof c, &tp) && tp.domain == TESS_DOMAIN_QUAD && tp.spacing == TESS_SPACING_FRACTIONAL_EVEN && tp.winding == TESS_WINDING_CW,
          "quad / fractional even / cw read (%u %u %u)", tp.domain, tp.spacing, tp.winding);
    w32(c, GLSLC_TESS_MODE_OFF, 0x1 | (1 << 4) | (3 << 8)); /* TRIANGLES_CCW (3) */
    CHECK(gfx_control_tess(c, sizeof c, &tp) && tp.domain == TESS_DOMAIN_TRIANGLE && tp.spacing == TESS_SPACING_FRACTIONAL_ODD && tp.winding == TESS_WINDING_CCW, "triangle / fractional odd / ccw");
    /* Source for the tess word (layout facts only): NVIDIA open-gpu-doc classes/3d/clb197.h, NVB197_SET_TESSELLATION_PARAMETERS (0x0320): DOMAIN_TYPE 1:0
     * ISOLINE 0 / TRIANGLE 1 / QUAD 2; SPACING 5:4 INTEGER 0 / FRACTIONAL_ODD 1 / FRACTIONAL_EVEN 2; OUTPUT_PRIMITIVES 9:8 POINTS 0 / LINES 1 / TRIANGLES_CW 2 / TRIANGLES_CCW 3. */
    w32(c, GLSLC_TESS_MODE_OFF, 0 | (0 << 4) | (1 << 8));
    CHECK(gfx_control_tess(c, sizeof c, &tp) && tp.domain == TESS_DOMAIN_ISOLINE && tp.spacing == TESS_SPACING_EQUAL, "isoline / integer / lines output");
    w32(c, GLSLC_TESS_MODE_OFF, 0x1 | (1 << 8));
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "triangle domain with LINES output is inconsistent: not taken");
    w32(c, GLSLC_TESS_MODE_OFF, 0x2 | (3 << 8));
    CHECK(gfx_control_tess(c, sizeof c, &tp) && tp.winding == TESS_WINDING_CCW, "quad / ccw");
    w32(c, GLSLC_TESS_MODE_OFF, 0x2 | (0 << 8) | (1 << 4));
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "point-mode output (POINTS) is not represented: not taken, the caller keeps its defaults");
    w32(c, GLSLC_TESS_MODE_OFF, 0x12345678);
    CHECK(!gfx_control_tess(c, sizeof c, &tp), "a word with bits outside the mode form is not taken");
    /* 6. real geometry sections: header fields read, no transform feedback layout, no tessellation word (geometry parameters come from the SPH) */
    smo_geometry(c);
    CHECK(gfx_control_parse(c, 0x7d1, &ci) && ci.length == 0x7d1 && ci.stage == 2 && ci.code_len == 0x300 && ci.const_off == 0x300 && ci.const_size == 0,
          "SMO geometry section: len %#x stage %u code %#x const %#x/%#x", ci.length, ci.stage, ci.code_len, ci.const_off, ci.const_size);
    CHECK(!gfx_control_xfb(c, 0x7d1, &xl) && !gfx_control_tess(c, 0x7d1, &tp), "SMO geometry section: no layout, no tessellation word");
    acnh_geometry(c);
    CHECK(gfx_control_parse(c, sizeof c, &ci) && ci.length == 0x850 && ci.stage == 2 && ci.code_len == 0xa00 && ci.const_off == 0x900 && ci.const_size == 8,
          "ACNH geometry section: len %#x stage %u code %#x const %#x/%#x", ci.length, ci.stage, ci.code_len, ci.const_off, ci.const_size);
    CHECK(!gfx_control_xfb(c, sizeof c, &xl) && !gfx_control_tess(c, sizeof c, &tp), "ACNH geometry section: no layout, no tessellation word");
    printf("t_control: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
